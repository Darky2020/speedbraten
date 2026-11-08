package main

// The public JSON API under /api/v1/: read-only views of the hub's players, the boards and the
// records. Answers are cached for a few seconds per URL, and each client address has a request
// budget, so a busy or hostile client costs little more than a cache lookup.

import (
	"bytes"
	"encoding/json"
	"html/template"
	"net/http"
	"regexp"
	"sort"
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
	mux.Handle("/api/v1/players", a.handle(apiCacheTTL, a.players))
	mux.Handle("/api/v1/runs", a.handle(apiCacheTTL, a.runs))
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
	name := board.ModeName(k.Mode)
	if k.Kind == "race" {
		name = "ffa or coop edit"
	}
	return apiKey{Map: k.Map, Kind: k.Kind, Mode: k.Mode, ModeName: name}
}

// replays gives the download URLs of the runs whose replays are public: board entries past their
// kind's publish delay; the others are left out
func (a *api) replays(r *http.Request, runs []int64) map[int64]string {
	locks, err := a.s.store.ReplayLocks(runs, "")
	if err != nil {
		return nil
	}
	scheme := "http"
	if r.TLS != nil || r.Header.Get("X-Forwarded-Proto") == "https" {
		scheme = "https"
	}
	urls := map[int64]string{}
	for _, run := range runs {
		if listed, _ := a.s.store.Listed(run); !listed {
			continue
		}
		if _, locked := locks[run]; !locked {
			urls[run] = scheme + "://" + r.Host + "/replay?run=" + strconv.FormatInt(run, 10)
		}
	}
	return urls
}

type apiEntry struct {
	Rank   int    `json:"rank"`
	Player string `json:"player"` // the player's id, as in /player?id=
	Name   string `json:"name"`
	MS     int64  `json:"ms"`
	Time   string `json:"time"`
	When   string `json:"when"` // RFC 3339, when the run was judged
	Run    int64  `json:"run"`
	Replay string `json:"replay,omitempty"` // download URL; absent while the replay isn't public
}

func entryOf(e board.Entry, replay string) apiEntry {
	return apiEntry{Rank: e.Rank, Player: e.Install, Name: e.Name, MS: e.MS, Time: board.Time(e.MS), When: e.When.UTC().Format(time.RFC3339), Run: e.Run, Replay: replay}
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
		Run    int64  `json:"run"`
		Replay string `json:"replay,omitempty"`
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
	var runs []int64
	for _, b := range all {
		if q.Get("kind") != "" && b.Key.Kind != q.Get("kind") || q.Get("mode") != "" && strconv.Itoa(b.Key.Mode) != q.Get("mode") {
			continue
		}
		o := out{apiKey: keyOf(b.Key), Entries: len(b.Entries)}
		if len(b.Entries) > 0 {
			e := b.Entries[0]
			o.Record = &rec{Player: e.Install, Name: e.Name, MS: e.MS, Time: board.Time(e.MS), When: e.When.UTC().Format(time.RFC3339), Run: e.Run}
			runs = append(runs, e.Run)
		}
		list = append(list, o)
	}
	urls := a.replays(r, runs)
	for i := range list {
		if list[i].Record != nil {
			list[i].Record.Replay = urls[list[i].Record.Run]
		}
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
	if b != nil {
		k = b.Key
	}
	res := out{apiKey: keyOf(k), Entries: []apiEntry{}}
	if b != nil {
		runs := make([]int64, len(b.Entries))
		for i, e := range b.Entries {
			runs[i] = e.Run
		}
		urls := a.replays(r, runs)
		for _, e := range b.Entries {
			res.Entries = append(res.Entries, entryOf(e, urls[e.Run]))
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
		Replay string `json:"replay,omitempty"`
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
	runs := make([]int64, len(list))
	for i, o := range list {
		runs[i] = o.Run
	}
	urls := a.replays(r, runs)
	for i := range list {
		list[i].Replay = urls[list[i].Run]
	}
	return 200, list
}

func (a *api) player(r *http.Request) (int, any) {
	type place struct {
		apiKey
		Rank   int    `json:"rank"`
		Of     int    `json:"of"`
		MS     int64  `json:"ms"`
		Time   string `json:"time"`
		When   string `json:"when"`
		Run    int64  `json:"run"`
		Replay string `json:"replay,omitempty"`
	}
	type out struct {
		ID     string   `json:"id"`
		Name   string   `json:"name"`
		Stats  apiStats `json:"stats"`
		Places []place  `json:"places"`
		Recent []apiRun `json:"recent_runs"`
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
	if all, err := a.s.store.PlayerStats(); err == nil {
		for _, p := range all {
			if p.Player == id {
				res.Stats = statsOf(p, a.completionByPlayer()[id], a.s.completionTotals())
			}
		}
	}
	if res.Recent, err = a.latestRuns(r, id, 20); err != nil {
		return 500, apiError{"something went wrong"}
	}
	runs := make([]int64, len(places))
	for i, p := range places {
		runs[i] = p.Entry.Run
	}
	urls := a.replays(r, runs)
	for _, p := range places {
		res.Places = append(res.Places, place{apiKey: keyOf(p.Key), Rank: p.Entry.Rank, Of: p.Of, MS: p.Entry.MS, Time: board.Time(p.Entry.MS),
			When: p.Entry.When.UTC().Format(time.RFC3339), Run: p.Entry.Run, Replay: urls[p.Entry.Run]})
	}
	return 200, res
}

var jsonToken = regexp.MustCompile(`"(?:[^"\\]|\\.)*"(\s*:)?|-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?|\btrue\b|\bfalse\b|\bnull\b`)

// jsonHighlight indents a JSON example for the API page and wraps its keys, strings, numbers and
// literals in spans for colour; text that isn't JSON is shown as it is.
func jsonHighlight(src string) template.HTML {
	var b bytes.Buffer
	if json.Indent(&b, []byte(src), "", "  ") != nil {
		return template.HTML(template.HTMLEscapeString(src))
	}
	text := b.String()
	var out strings.Builder
	last := 0
	for _, m := range jsonToken.FindAllStringSubmatchIndex(text, -1) {
		out.WriteString(template.HTMLEscapeString(text[last:m[0]]))
		tok := text[m[0]:m[1]]
		class := "jn"
		switch {
		case m[2] >= 0:
			class, tok = "jk", text[m[0]:m[2]]
		case tok[0] == '"':
			class = "js"
		case tok == "true" || tok == "false" || tok == "null":
			class = "jl"
		}
		out.WriteString(`<span class="` + class + `">` + template.HTMLEscapeString(tok) + `</span>`)
		if m[2] >= 0 {
			out.WriteString(template.HTMLEscapeString(text[m[2]:m[1]]))
		}
		last = m[1]
	}
	out.WriteString(template.HTMLEscapeString(text[last:]))
	return template.HTML(out.String())
}

type apiCompletion struct {
	Category string `json:"category"` // race, ctf, ictf, ectf or sp
	Label    string `json:"label"`
	Done     int    `json:"done"` // boards they have a time on
	Of       int    `json:"of"`
	Percent  int    `json:"percent"`
}

func completionOf(mine, totals map[string]int) []apiCompletion {
	out := []apiCompletion{}
	for _, c := range completionRows(mine, totals) {
		out = append(out, apiCompletion{Category: c.Key, Label: c.Label, Done: c.Done, Of: c.Of, Percent: c.Percent})
	}
	return out
}

// completionByPlayer counts each player's boards per completion category
func (a *api) completionByPlayer() map[string]map[string]int {
	out := map[string]map[string]int{}
	all, _ := a.s.store.Boards()
	for _, b := range all {
		g := completionGroup(b.Key)
		for _, e := range b.Entries {
			if out[e.Install] == nil {
				out[e.Install] = map[string]int{}
			}
			out[e.Install][g]++
		}
	}
	return out
}

type apiStats struct {
	Runs    int    `json:"runs"`
	Boards  int    `json:"boards"`
	Records int    `json:"records"` // boards they hold the record on
	Podiums int    `json:"podiums"`
	Gold    int    `json:"gold"`
	Silver  int    `json:"silver"`
	Bronze  int    `json:"bronze"`
	First   string `json:"first_run"`
	Last    string `json:"last_run"`
	// of each category's boards, how many they have a time on
	Completion []apiCompletion `json:"completion"`
}

func statsOf(p board.PlayerStats, mine, totals map[string]int) apiStats {
	return apiStats{Runs: p.Runs, Boards: p.Boards, Records: p.Medals[1], Podiums: p.Medals[1] + p.Medals[2] + p.Medals[3],
		Gold: p.Medals[1], Silver: p.Medals[2], Bronze: p.Medals[3],
		First: p.First.UTC().Format(time.RFC3339), Last: p.Last.UTC().Format(time.RFC3339), Completion: completionOf(mine, totals)}
}

func (a *api) players(r *http.Request) (int, any) {
	type out struct {
		ID         string   `json:"id"`
		Name       string   `json:"name"`
		Registered bool     `json:"registered"`
		Stats      apiStats `json:"stats"`
	}
	all, err := a.s.store.PlayerStats()
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	q := r.URL.Query()
	by := q.Get("sort")
	sort.Slice(all, func(i, j int) bool {
		x, y := all[i], all[j]
		switch by {
		case "runs":
			if x.Runs != y.Runs {
				return x.Runs > y.Runs
			}
		case "recent":
			if !x.Last.Equal(y.Last) {
				return x.Last.After(y.Last)
			}
		case "name":
			if xl, yl := strings.ToLower(x.Name), strings.ToLower(y.Name); xl != yl {
				return xl < yl
			}
		default:
			if x.Medals != y.Medals {
				for k := 1; k <= 3; k++ {
					if x.Medals[k] != y.Medals[k] {
						return x.Medals[k] > y.Medals[k]
					}
				}
			}
			if x.Boards != y.Boards {
				return x.Boards > y.Boards
			}
		}
		return x.Player < y.Player
	})
	limit := 100
	if n, err := strconv.Atoi(q.Get("limit")); err == nil && n > 0 {
		limit = min(n, 1000)
	}
	list := []out{}
	totals, mine := a.s.completionTotals(), a.completionByPlayer()
	for _, p := range all {
		if len(list) >= limit {
			break
		}
		list = append(list, out{ID: p.Player, Name: p.Name, Registered: p.Registered, Stats: statsOf(p, mine[p.Player], totals)})
	}
	return 200, list
}

type apiRun struct {
	apiKey
	Player string `json:"player"`
	Name   string `json:"name"`
	MS     int64  `json:"ms"`
	Time   string `json:"time"`
	When   string `json:"when"`
	Run    int64  `json:"run"`
	Record bool   `json:"record"` // the board's best when it was judged
	Replay string `json:"replay,omitempty"`
}

// latestRuns lists the newest runs that count, newest first, filtered as the request asks
func (a *api) latestRuns(r *http.Request, player string, limit int) ([]apiRun, error) {
	q := r.URL.Query()
	all, err := a.s.store.RecentRuns(1 << 30)
	if err != nil {
		return nil, err
	}
	list := []apiRun{}
	for _, run := range all {
		if len(list) >= limit {
			break
		}
		if q.Get("kind") != "" && run.Key.Kind != q.Get("kind") || q.Get("mode") != "" && strconv.Itoa(run.Key.Mode) != q.Get("mode") || player != "" && run.Install != player {
			continue
		}
		list = append(list, apiRun{apiKey: keyOf(run.Key), Player: run.Install, Name: run.Name, MS: run.MS, Time: board.Time(run.MS),
			When: run.When.UTC().Format(time.RFC3339), Run: run.Run, Record: run.Record})
	}
	runs := make([]int64, len(list))
	for i, o := range list {
		runs[i] = o.Run
	}
	urls := a.replays(r, runs)
	for i := range list {
		list[i].Replay = urls[list[i].Run]
	}
	return list, nil
}

func (a *api) runs(r *http.Request) (int, any) {
	q := r.URL.Query()
	limit := 50
	if n, err := strconv.Atoi(q.Get("limit")); err == nil && n > 0 {
		limit = min(n, apiRecordsMax)
	}
	player := ""
	if id := q.Get("player"); id != "" {
		player = a.s.store.Canonical(id)
	}
	list, err := a.latestRuns(r, player, limit)
	if err != nil {
		return 500, apiError{"something went wrong"}
	}
	return 200, list
}
