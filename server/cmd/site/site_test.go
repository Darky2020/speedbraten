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
	for _, want := range []string{"forge", "flagrun, effic ctf", "An Army Of One", "alice", "0:09.000", "6:09.260"} {
		if code != 200 || !strings.Contains(body, want) {
			t.Fatalf("home %d lacks %q", code, want)
		}
	}
	if strings.Contains(body, "<script>bob") {
		t.Fatal("a player name isn't escaped")
	}

	code, body = get(t, s, "/board?map=forge&kind=flagrun&mode=17")
	if code != 200 || !strings.Contains(body, "+0:00.500") || strings.Contains(body, "0:08.000") {
		t.Fatalf("board %d: gap missing or the rejected run shown", code)
	}
	if code, _ = get(t, s, "/board?map=forge&kind=flagrun&mode=11"); code != 404 {
		t.Fatalf("an empty board: %d", code)
	}

	code, body = get(t, s, "/player?id=kb")
	if code != 200 || !strings.Contains(body, "2 boards") || !strings.Contains(body, "&lt;script&gt;bob") {
		t.Fatalf("player %d", code)
	}

	code, body = get(t, s, "/records")
	if code != 200 || !strings.Contains(body, "first") {
		t.Fatalf("records %d", code)
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
