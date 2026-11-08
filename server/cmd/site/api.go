package main

// The public JSON API under /api/v1/: read-only views of the hub's players, the boards and the
// records. Answers are cached for a few seconds per URL, and each client address has a request
// budget, so a busy or hostile client costs little more than a cache lookup.

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"speedbraten/server/board"
)

const (
	apiRate       = 5.0 // requests a second per client address, on average
	apiBurst      = 20.0
	apiCacheTTL   = 10 * time.Second
	apiOnlineTTL  = 2 * time.Second
	apiCacheMax   = 2000
	apiClientsMax = 20000
	apiRecordsMax = 500
)

type apiBucket struct {
	tokens float64
	at     time.Time
}

type apiCached struct {
	body []byte
	code int
	at   time.Time
}

type api struct {
	s       *site
	mu      sync.Mutex
	clients map[string]*apiBucket
	cache   map[string]apiCached
	now     func() time.Time
}

func newAPI(s *site) *api {
	return &api{s: s, clients: map[string]*apiBucket{}, cache: map[string]apiCached{}, now: time.Now}
}

func (a *api) register(mux *http.ServeMux) {
	mux.Handle("/api/v1/online", a.handle(apiOnlineTTL, a.online))
	mux.Handle("/api/v1/boards", a.handle(apiCacheTTL, a.boards))
	mux.Handle("/api/v1/board", a.handle(apiCacheTTL, a.board))
	mux.Handle("/api/v1/records", a.handle(apiCacheTTL, a.records))
	mux.Handle("/api/v1/player", a.handle(apiCacheTTL, a.player))
}

// allow takes one request from the client's budget
func (a *api) allow(client string, now time.Time) bool {
	b := a.clients[client]
	if b == nil {
		if len(a.clients) >= apiClientsMax {
			for k, o := range a.clients {
				if now.Sub(o.at) > time.Minute {
					delete(a.clients, k)
				}
			}
		}
		b = &apiBucket{tokens: apiBurst, at: now}
		a.clients[client] = b
	}
	b.tokens = min(apiBurst, b.tokens+now.Sub(b.at).Seconds()*apiRate)
	b.at = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// handle answers GETs from the cache while it is fresh, else from fn (status, value as JSON)
func (a *api) handle(ttl time.Duration, fn func(r *http.Request) (int, any)) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Content-Type", "application/json")
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			w.WriteHeader(http.StatusMethodNotAllowed)
			w.Write([]byte(`{"error":"GET only"}` + "\n"))
			return
		}
		now := a.now()
		key := r.URL.Path + "?" + r.URL.RawQuery
		a.mu.Lock()
		if !a.allow(clientIP(r), now) {
			a.mu.Unlock()
			w.Header().Set("Retry-After", "1")
			w.WriteHeader(http.StatusTooManyRequests)
			w.Write([]byte(`{"error":"too many requests"}` + "\n"))
			return
		}
		c, ok := a.cache[key]
		a.mu.Unlock()
		if !ok || now.Sub(c.at) > ttl {
			code, v := fn(r)
			body, err := json.Marshal(v)
			if err != nil {
				code, body = http.StatusInternalServerError, []byte(`{"error":"something went wrong"}`)
			}
			c = apiCached{body: append(body, '\n'), code: code, at: now}
			a.mu.Lock()
			if len(a.cache) >= apiCacheMax {
				clear(a.cache)
			}
			a.cache[key] = c
			a.mu.Unlock()
		}
		w.Header().Set("Cache-Control", "public, max-age="+strconv.Itoa(int(ttl.Seconds())))
		w.WriteHeader(c.code)
		w.Write(c.body)
	})
}

type apiError struct {
	Error string `json:"error"`
}

type apiKey struct {
	Map      string `json:"map"`
	Kind     string `json:"kind"`
	Mode     int    `json:"mode"`
	ModeName string `json:"mode_name"`
}

func keyOf(k board.Key) apiKey {
	return apiKey{Map: k.Map, Kind: k.Kind, Mode: k.Mode, ModeName: board.ModeName(k.Mode)}
}

type apiEntry struct {
	Rank   int    `json:"rank"`
	Player string `json:"player"` // the player's id, as in /player?id=
	Name   string `json:"name"`
	MS     int64  `json:"ms"`
	Time   string `json:"time"`
	When   string `json:"when"` // RFC 3339, when the run was judged
	Run    int64  `json:"run"`
	Replay string `json:"replay,omitempty"` // a path on the site; absent while the replay isn't public
}

func entryOf(e board.Entry, locked bool) apiEntry {
	out := apiEntry{Rank: e.Rank, Player: e.Install, Name: e.Name, MS: e.MS, Time: board.Time(e.MS), When: e.When.UTC().Format(time.RFC3339), Run: e.Run}
	if !locked {
		out.Replay = "/replay?run=" + strconv.FormatInt(e.Run, 10)
	}
	return out
}

func (a *api) online(r *http.Request) (int, any) {
	type out struct {
		Online  bool              `json:"online"` // false while the hub hasn't been heard from lately
		Count   int               `json:"count"`
		Players []json.RawMessage `json:"players"`
	}
	n, names, ok := a.s.store.Online(onlineStale)
	if !ok {
		return 200, out{Players: []json.RawMessage{}}
	}
	res := out{Online: true, Count: n, Players: []json.RawMessage{}}
	if detail, ok := a.s.store.OnlineDetail(onlineStale); ok && json.Unmarshal([]byte(detail), &res.Players) == nil {
		return 200, res
	}
	// a hub that writes only the names
	for _, name := range strings.Split(names, ", ") {
		if name != "" {
			b, _ := json.Marshal(map[string]string{"name": name})
			res.Players = append(res.Players, b)
		}
	}
	return 200, res
}

func (a *api) boards(r *http.Request) (int, any) {
	type rec struct {
		Player string `json:"player"`
		Name   string `json:"name"`
		MS     int64  `json:"ms"`
		Time   string `json:"time"`
		When   string `json:"when"`
	}
	type out struct {
		apiKey
		Entries int  `json:"entries"`
		Record  *rec `json:"record"`
	}
	q := r.URL.Query()
	all, err := a.s.store.Boards()
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	list := []out{}
	for _, b := range all {
		if q.Get("kind") != "" && b.Key.Kind != q.Get("kind") || q.Get("mode") != "" && strconv.Itoa(b.Key.Mode) != q.Get("mode") {
			continue
		}
		o := out{apiKey: keyOf(b.Key), Entries: len(b.Entries)}
		if len(b.Entries) > 0 {
			e := b.Entries[0]
			o.Record = &rec{Player: e.Install, Name: e.Name, MS: e.MS, Time: board.Time(e.MS), When: e.When.UTC().Format(time.RFC3339)}
		}
		list = append(list, o)
	}
	return 200, list
}

func (a *api) board(r *http.Request) (int, any) {
	type legacy struct {
		Player string `json:"player"`
		MS     int64  `json:"ms"`
		Time   string `json:"time"`
		Date   string `json:"date"`
		Source string `json:"source"`
		Link   string `json:"link"`
	}
	type out struct {
		apiKey
		Entries []apiEntry `json:"entries"`
		Legacy  *legacy    `json:"legacy"` // the best known time from before the witness, not witnessed
	}
	q := r.URL.Query()
	mode, err := strconv.Atoi(q.Get("mode"))
	if err != nil || q.Get("map") == "" || q.Get("kind") == "" {
		return 400, apiError{"map, kind and mode are needed"}
	}
	k := board.Key{Map: q.Get("map"), Kind: q.Get("kind"), Mode: mode}
	b, err := a.s.store.Board(k)
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	l, _ := a.s.store.LegacyRecord(k)
	if b == nil && l == nil {
		return 404, apiError{"no such board"}
	}
	res := out{apiKey: keyOf(k), Entries: []apiEntry{}}
	if b != nil {
		runs := make([]int64, len(b.Entries))
		for i, e := range b.Entries {
			runs[i] = e.Run
		}
		locks, _ := a.s.store.ReplayLocks(runs, "")
		for _, e := range b.Entries {
			_, locked := locks[e.Run]
			res.Entries = append(res.Entries, entryOf(e, locked))
		}
	}
	if l != nil {
		res.Legacy = &legacy{Player: l.Player, MS: l.MS, Time: board.Time(l.MS), Date: l.Date, Source: l.Source, Link: l.Link}
	}
	return 200, res
}

func (a *api) records(r *http.Request) (int, any) {
	type out struct {
		apiKey
		Player string `json:"player"`
		Name   string `json:"name"`
		MS     int64  `json:"ms"`
		Time   string `json:"time"`
		Prev   int64  `json:"prev_ms"` // the record it beat, -1 for a board's first
		When   string `json:"when"`
		Run    int64  `json:"run"`
	}
	q := r.URL.Query()
	limit := 50
	if n, err := strconv.Atoi(q.Get("limit")); err == nil && n > 0 {
		limit = min(n, apiRecordsMax)
	}
	get := a.s.store.Records
	if q.Get("latest") == "1" {
		get = a.s.store.LatestRecords
	}
	all, err := get(100000)
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	list := []out{}
	for _, rec := range all {
		if len(list) >= limit {
			break
		}
		if q.Get("kind") != "" && rec.Key.Kind != q.Get("kind") || q.Get("mode") != "" && strconv.Itoa(rec.Key.Mode) != q.Get("mode") {
			continue
		}
		list = append(list, out{apiKey: keyOf(rec.Key), Player: rec.Install, Name: rec.Name, MS: rec.MS, Time: board.Time(rec.MS), Prev: rec.Prev,
			When: rec.When.UTC().Format(time.RFC3339), Run: rec.Run})
	}
	return 200, list
}

func (a *api) player(r *http.Request) (int, any) {
	type place struct {
		apiKey
		Rank int    `json:"rank"`
		Of   int    `json:"of"`
		MS   int64  `json:"ms"`
		Time string `json:"time"`
		When string `json:"when"`
		Run  int64  `json:"run"`
	}
	type out struct {
		ID     string  `json:"id"`
		Name   string  `json:"name"`
		Places []place `json:"places"`
	}
	id := r.URL.Query().Get("id")
	if id == "" {
		return 400, apiError{"id is needed"}
	}
	id = a.s.store.Canonical(id)
	places, name, err := a.s.store.Player(id)
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	if len(places) == 0 && name == "" {
		return 404, apiError{"no such player"}
	}
	res := out{ID: id, Name: name, Places: []place{}}
	for _, p := range places {
		res.Places = append(res.Places, place{apiKey: keyOf(p.Key), Rank: p.Entry.Rank, Of: p.Of, MS: p.Entry.MS, Time: board.Time(p.Entry.MS),
			When: p.Entry.When.UTC().Format(time.RFC3339), Run: p.Entry.Run})
	}
	return 200, res
}
