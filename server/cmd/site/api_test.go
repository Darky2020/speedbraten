package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"speedbraten/server/sqlite"
)

func getJSON(t *testing.T, h http.Handler, path string, v any) int {
	t.Helper()
	code, body := get(t, h, path)
	if err := json.Unmarshal([]byte(body), v); err != nil {
		t.Fatalf("%s: %v: %s", path, err, body)
	}
	return code
}

func TestAPI(t *testing.T) {
	store, dir := fixture(t)
	s := newSite(store)

	var boards []struct {
		Map, Kind string
		Mode      int
		Entries   int
		Record    *struct{ Name, Time string }
	}
	if code := getJSON(t, s, "/api/v1/boards?kind=flagrun", &boards); code != 200 || len(boards) != 1 || boards[0].Map != "forge" || boards[0].Entries != 2 ||
		boards[0].Record == nil || boards[0].Record.Time != "0:09.000" {
		t.Fatalf("boards %d %+v", code, boards)
	}

	var b struct {
		Map     string
		Entries []struct {
			Rank           int
			Player, Replay string
			MS             int64
		}
		Legacy *struct{ Player string }
	}
	if code := getJSON(t, s, "/api/v1/board?map=forge&kind=flagrun&mode=17", &b); code != 200 || len(b.Entries) != 2 || b.Entries[0].Player != "ka" ||
		b.Entries[1].MS != 9500 || b.Entries[0].Replay != "http://example.com/replay?run=1" || b.Legacy != nil {
		t.Fatalf("board %d %+v", code, b)
	}
	var e struct{ Error string }
	if code := getJSON(t, s, "/api/v1/board?map=nowhere&kind=flagrun&mode=17", &e); code != 404 || e.Error == "" {
		t.Fatalf("missing board %d %+v", code, e)
	}
	if code := getJSON(t, s, "/api/v1/board?map=forge", &e); code != 400 {
		t.Fatalf("board without mode %d %+v", code, e)
	}

	var recs []struct {
		Map    string
		MS     int64
		PrevMS int64 `json:"prev_ms"`
	}
	if code := getJSON(t, s, "/api/v1/records?kind=flagrun", &recs); code != 200 || len(recs) != 1 || recs[0].MS != 9000 {
		t.Fatalf("records %d %+v", code, recs)
	}
	if getJSON(t, s, "/api/v1/records?limit=1", &recs); len(recs) != 1 {
		t.Fatalf("records limit %+v", recs)
	}

	var p struct {
		ID, Name string
		Places   []struct {
			Map  string
			Rank int
			Of   int
		}
	}
	if code := getJSON(t, s, "/api/v1/player?id=kb", &p); code != 200 || p.ID != "kb" || len(p.Places) != 2 || p.Places[0].Rank != 1 {
		t.Fatalf("player %d %+v", code, p)
	}
	if code := getJSON(t, s, "/api/v1/player?id=nobody", &e); code != 404 {
		t.Fatalf("missing player %d", code)
	}

	// online: nothing while the hub hasn't written its presence, then each player's detail
	var on struct {
		Online  bool
		Count   int
		Players []map[string]any
	}
	if getJSON(t, s, "/api/v1/online", &on); on.Online || len(on.Players) != 0 {
		t.Fatalf("online without presence %+v", on)
	}
	h, err := sqlite.Open(filepath.Join(dir, "hub.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer h.Close()
	h.Exec("CREATE TABLE presence(id INTEGER PRIMARY KEY, players INTEGER, names TEXT, updated INTEGER, detail TEXT NOT NULL DEFAULT '[]')")
	h.Exec(`INSERT INTO presence VALUES(1, 1, 'alice', ?, '[{"name":"alice","map":"forge","mode":17,"kind":"flagrun","away":"dnd"}]')`, time.Now().Unix())
	time.Sleep(apiOnlineTTL + 100*time.Millisecond)
	if getJSON(t, s, "/api/v1/online", &on); !on.Online || on.Count != 1 || len(on.Players) != 1 || on.Players[0]["map"] != "forge" || on.Players[0]["away"] != "dnd" {
		t.Fatalf("online %+v", on)
	}

	rec := httptest.NewRecorder()
	s.ServeHTTP(rec, httptest.NewRequest("POST", "/api/v1/boards", nil))
	if rec.Code != http.StatusMethodNotAllowed || rec.Header().Get("Access-Control-Allow-Origin") != "*" {
		t.Fatalf("post %d %v", rec.Code, rec.Header())
	}
}

// A client gets apiBurst requests at once, then apiRate a second; others aren't affected. Answers
// come from the cache while it is fresh, without asking the store.
func TestAPILimits(t *testing.T) {
	store, _ := fixture(t)
	s := newSite(store)
	a := newAPI(s)
	now := time.Unix(1800000000, 0)
	a.now = func() time.Time { return now }
	mux := http.NewServeMux()
	a.register(mux)
	call := func(ip string) int {
		r := httptest.NewRequest("GET", "/api/v1/boards", nil)
		r.RemoteAddr = ip + ":1234"
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, r)
		return rec.Code
	}
	for i := 0; i < int(apiBurst); i++ {
		if code := call("10.0.0.1"); code != 200 {
			t.Fatalf("request %d: %d", i, code)
		}
	}
	if code := call("10.0.0.1"); code != http.StatusTooManyRequests {
		t.Fatalf("over the burst: %d", code)
	}
	if code := call("10.0.0.2"); code != 200 {
		t.Fatalf("another client: %d", code)
	}
	now = now.Add(time.Second)
	for i := 0; i < int(apiRate); i++ {
		if code := call("10.0.0.1"); code != 200 {
			t.Fatalf("after a second, request %d: %d", i, code)
		}
	}
	if code := call("10.0.0.1"); code != http.StatusTooManyRequests {
		t.Fatalf("over the rate: %d", code)
	}

	calls := 0
	cached := a.handle(apiCacheTTL, func(*http.Request) (int, any) { calls++; return 200, calls })
	ask := func() string {
		r := httptest.NewRequest("GET", "/api/v1/x", nil)
		r.RemoteAddr = "10.0.0.3:1"
		rec := httptest.NewRecorder()
		cached.ServeHTTP(rec, r)
		return strings.TrimSpace(rec.Body.String())
	}
	if ask() != "1" || ask() != "1" {
		t.Fatal("not cached")
	}
	now = now.Add(apiCacheTTL + time.Second)
	if got := ask(); got != "2" {
		t.Fatalf("stale cache served: %s", got)
	}
}

func TestJSONHighlight(t *testing.T) {
	got := string(jsonHighlight(`{"a":"<b>","n":-1.5,"ok":true,"x":null,"l":[1]}`))
	want := "{\n  <span class=\"jk\">&#34;a&#34;</span>: <span class=\"js\">&#34;&lt;b&gt;&#34;</span>,\n  <span class=\"jk\">&#34;n&#34;</span>: <span class=\"jn\">-1.5</span>," +
		"\n  <span class=\"jk\">&#34;ok&#34;</span>: <span class=\"jl\">true</span>,\n  <span class=\"jk\">&#34;x&#34;</span>: <span class=\"jl\">null</span>," +
		"\n  <span class=\"jk\">&#34;l&#34;</span>: [\n    <span class=\"jn\">1</span>\n  ]\n}"
	if got != want {
		t.Fatalf("got\n%s\nwant\n%s", got, want)
	}
	if got := string(jsonHighlight("not json <x>")); got != "not json &lt;x&gt;" {
		t.Fatalf("not json: %s", got)
	}
}
