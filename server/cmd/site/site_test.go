package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"speedbraten/server/board"
	"speedbraten/server/sqlite"
)

// fixture: forge flagrun (effic ctf) with alice (hub name) 9.000 and a script-named player 9.500,
// a rejected faster run, and a campaign run.
func fixture(t *testing.T) (*board.Store, string) {
	dir := t.TempDir()
	wpath, hpath := filepath.Join(dir, "witness.db"), filepath.Join(dir, "hub.db")
	w, err := sqlite.Create(wpath)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	file := func(name string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte("SBR2 "+name), 0600); err != nil {
			t.Fatal(err)
		}
		return p
	}
	for _, q := range []struct {
		sql  string
		args []any
	}{
		{"CREATE TABLE sessions(id INTEGER PRIMARY KEY, installkey TEXT, installpub TEXT)", nil},
		{"CREATE TABLE runs(id INTEGER PRIMARY KEY, session INTEGER, installkey TEXT, map TEXT, player TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, verdict TEXT, judged INTEGER, file TEXT, started INTEGER, reason TEXT, stop TEXT, refused TEXT, violations INTEGER, maxlagms INTEGER, finished INTEGER DEFAULT 1)", nil},
		{"CREATE TABLE flags(id INTEGER PRIMARY KEY, run INTEGER, kind TEXT, other INTEGER, matched INTEGER, total INTEGER, created INTEGER, reviewed INTEGER DEFAULT 0, note TEXT)", nil},
		{"INSERT INTO flags(run, kind, other, matched, total, created) VALUES(2, 'copy', 1, 58, 156, 1700000200000)", nil},
		{"INSERT INTO sessions(installkey, installpub) VALUES('ka', 'pubA')", nil},
		{"INSERT INTO runs(id, session, installkey, map, player, runkind, mode, runtimems, verdict, judged, file) VALUES(1, 1, 'ka', 'forge', 'x', 'flagrun', 17, 9000, 'valid', 1792900000000, ?)", []any{file("a.sbr")}},
		{"INSERT INTO runs(id, session, installkey, map, player, runkind, mode, runtimems, verdict, judged, file) VALUES(2, 2, 'kb', 'forge', '<script>bob</script>', 'flagrun', 17, 9500, 'valid', 1792900001000, ?)", []any{file("b.sbr")}},
		{"INSERT INTO runs(id, session, installkey, map, player, runkind, mode, runtimems, verdict, judged, file, reason) VALUES(3, 2, 'kb', 'forge', 'bob', 'flagrun', 17, 8000, 'rejected', 1792900002000, ?, 'the replay desynced at tick 50')", []any{file("c.sbr")}},
		{"INSERT INTO runs(id, session, installkey, map, player, runkind, mode, runtimems, verdict, judged, file) VALUES(4, 2, 'kb', 'mpsp6a', '<script>bob</script>', 'campaign', -3, 369260, 'valid', 1792900003000, ?)", []any{file("d.sbr")}},
	} {
		if _, err := w.Exec(q.sql, q.args...); err != nil {
			t.Fatal(err)
		}
	}
	h, err := sqlite.Create(hpath)
	if err != nil {
		t.Fatal(err)
	}
	h.Exec("CREATE TABLE installs(key TEXT PRIMARY KEY, name TEXT)")
	h.Exec("INSERT INTO installs VALUES('pubA', 'alice')")
	h.Close()
	store, err := board.Open(wpath, hpath, filepath.Join(dir, "site.db"), time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(store.Close)
	return store, dir
}

func get(t *testing.T, h http.Handler, path string) (int, string) {
	t.Helper()
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest("GET", path, nil))
	b, _ := io.ReadAll(rec.Body)
	return rec.Code, string(b)
}

func TestPublicPages(t *testing.T) {
	store, _ := fixture(t)
	s := newSite(store)

	code, body := get(t, s, "/")
	for _, want := range []string{"Recently submitted runs", "Oldest standing records", "Top record holders", "forge", "ECTF", "An Army Of One", "alice", "0:09.000", "6:09.260"} {
		if code != 200 || !strings.Contains(body, want) {
			t.Fatalf("home %d lacks %q", code, want)
		}
	}
	if strings.Contains(body, "<script>bob") {
		t.Fatal("a player name isn't escaped")
	}

	// the tabs: flagruns has the forge row with the effic ctf record, singleplayer the campaign
	if code, body = get(t, s, "/flagruns"); code != 200 || !strings.Contains(body, "0:09.000") || !strings.Contains(body, "ICTF") {
		t.Fatalf("flagruns %d", code)
	}
	if code, body = get(t, s, "/singleplayer"); code != 200 || !strings.Contains(body, "An Army Of One") || strings.Contains(body, "0:09.000") {
		t.Fatalf("singleplayer %d", code)
	}
	if code, body = get(t, s, "/races"); code != 200 || !strings.Contains(body, "No races yet") {
		t.Fatalf("races %d", code)
	}

	// each category page has its own newest records, recent runs, holders and oldest records
	_, body = get(t, s, "/flagruns")
	for _, want := range []string{"Recently submitted runs", "Top ctf runners", "Top insta ctf runners", "Top effic ctf runners", "Oldest standing records", `href="/records?mode=flagrun"`} {
		if !strings.Contains(body, want) {
			t.Fatalf("flagruns page lacks %q", want)
		}
	}
	if strings.Index(body, "Recently submitted runs") > strings.Index(body, `<h2 class="section">Maps`) {
		t.Fatal("the category lists aren't above the maps")
	}
	if _, body = get(t, s, "/races"); !strings.Contains(body, "Top record holders") {
		t.Fatal("races page holders")
	}
	if strings.Contains(body, "6:09.260") || strings.Contains(body, "Newest records") {
		t.Fatal("a campaign run, or the newest records, on the flagruns page")
	}
	if _, body = get(t, s, "/records?mode=flagrun"); !strings.Contains(body, "0:09.000") || strings.Contains(body, "6:09.260") {
		t.Fatal("the all-flagruns records filter")
	}

	// search, filter and sort
	if _, body = get(t, s, "/flagruns?q=ALI"); !strings.Contains(body, "0:09.000") {
		t.Fatal("search by a holder's name, any case, doesn't find forge")
	}
	if _, body = get(t, s, "/flagruns?q=nothing"); !strings.Contains(body, "No flagruns match") {
		t.Fatal("a search with no match")
	}
	if _, body = get(t, s, "/singleplayer?q=army"); !strings.Contains(body, "An Army Of One") {
		t.Fatal("search by a campaign's name")
	}
	if _, body = get(t, s, "/records?mode=sp"); !strings.Contains(body, "An Army Of One") || strings.Contains(body, "0:09.000") {
		t.Fatal("the records mode filter")
	}
	_, body = get(t, s, "/records?sort=oldest")
	if i, j := strings.Index(body, "0:09.000"), strings.Index(body, "6:09.260"); i < 0 || j < 0 || i > j {
		t.Fatal("records sorted oldest first")
	}
	if _, body = get(t, s, "/records?sort=bogus&mode=bogus"); !strings.Contains(body, "6:09.260") {
		t.Fatal("unknown sort and mode values fall back to the defaults")
	}

	code, body = get(t, s, "/map?map=forge&kind=flagrun&mode=17")
	if code != 200 || !strings.Contains(body, `slower">+0:00.500`) || strings.Contains(body, "0:08.000") || !strings.Contains(body, "<svg class=\"graph\"") {
		t.Fatalf("map page %d: gap or graph missing, or the rejected run shown", code)
	}
	// a flagrun map has a page for each ctf mode, runs or not; other boards without runs don't exist
	if code, body = get(t, s, "/map?map=forge&kind=flagrun&mode=11"); code != 200 || !strings.Contains(body, "No runs in this mode yet") || !strings.Contains(body, "insta ctf") {
		t.Fatalf("a flagrun mode without runs: %d", code)
	}
	if code, _ = get(t, s, "/map?map=forge&kind=flagrun&mode=3"); code != 404 {
		t.Fatalf("a mode that isn't a ctf mode: %d", code)
	}
	if code, _ = get(t, s, "/map?map=nosuchmap&kind=flagrun&mode=11"); code != 404 {
		t.Fatalf("a map with no runs at all: %d", code)
	}
	if code, _ = get(t, s, "/map?map=forge&kind=race&mode=0"); code != 404 {
		t.Fatalf("an empty race board: %d", code)
	}
	rec := httptest.NewRecorder()
	s.ServeHTTP(rec, httptest.NewRequest("GET", "/board?map=forge&kind=flagrun&mode=17", nil))
	if rec.Code != http.StatusMovedPermanently || !strings.HasPrefix(rec.Header().Get("Location"), "/map?") {
		t.Fatalf("old board link: %d %q", rec.Code, rec.Header().Get("Location"))
	}

	code, body = get(t, s, "/player?id=kb")
	if code != 200 || !strings.Contains(body, "on 2 maps") || !strings.Contains(body, "&lt;script&gt;bob") {
		t.Fatalf("player %d", code)
	}
	if !strings.Contains(body, `<nav class="tabs"><a href="/">Home</a>`) || strings.Contains(body, `class="bar"><div class="fill`) {
		t.Fatal("the player page's menu, or a progress bar sharing the menu's class")
	}
		// kb: second on forge (effic ctf), first on the campaign; 3 runs, the rejected one not counted
	for _, want := range []string{"Map completion", `<div class="meter-num">1 / 1</div>`, "1 gold</span> · <span class=\"silver\">1 silver", `<div class="k">runs</div><div class="v">2</div>`} {
		if !strings.Contains(body, want) {
			t.Fatalf("player stats lack %q", want)
		}
	}
	if code, body = get(t, s, "/records"); code != 200 || !strings.Contains(body, "first") {
		t.Fatalf("records %d", code)
	}
	if code, body = get(t, s, "/info"); code != 200 || !strings.Contains(body, "141.95.67.191") {
		t.Fatalf("info %d", code)
	}

	// pictures: a placeholder when there is none, the game's preview when there is
	if code, body = get(t, s, "/mapimg/forge"); code != 200 || !strings.HasPrefix(body, "<svg") {
		t.Fatalf("placeholder picture %d", code)
	}
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "forge.jpg"), []byte("JPEG"), 0644)
	s.previews = dir
	if code, body = get(t, s, "/mapimg/forge"); code != 200 || body != "JPEG" {
		t.Fatalf("preview picture %d %q", code, body)
	}
	if code, _ = get(t, s, "/mapimg/..%2Fsecret"); code != 404 {
		t.Fatalf("a path outside the picture folders: %d", code)
	}
}

func TestReplayDownload(t *testing.T) {
	store, _ := fixture(t)
	s := newSite(store)
	rec := httptest.NewRecorder()
	s.ServeHTTP(rec, httptest.NewRequest("GET", "/replay?run=1", nil))
	if rec.Code != 200 || rec.Body.String() != "SBR2 a.sbr" || !strings.Contains(rec.Header().Get("Content-Disposition"), "forge_0-09-000_run1.sbr") {
		t.Fatalf("download %d %q %q", rec.Code, rec.Body.String(), rec.Header().Get("Content-Disposition"))
	}
	// rejected runs and runs that aren't an entry aren't public
	if code, _ := get(t, s, "/replay?run=3"); code != 404 {
		t.Fatalf("rejected run download: %d", code)
	}
	store.Remove(1, "test")
	if code, _ := get(t, s, "/replay?run=1"); code != 404 {
		t.Fatalf("removed run download: %d", code)
	}
}
