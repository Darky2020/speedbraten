package board

import (
	"path/filepath"
	"testing"
	"time"

	"speedbraten/server/sqlite"
)

type fixture struct {
	t                  *testing.T
	witness, hub, site string
	w                  *sqlite.DB
}

func newFixture(t *testing.T) *fixture {
	dir := t.TempDir()
	f := &fixture{t: t, witness: filepath.Join(dir, "witness.db"), hub: filepath.Join(dir, "hub.db"), site: filepath.Join(dir, "site.db")}
	w, err := sqlite.Create(f.witness)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(w.Close)
	f.w = w
	for _, q := range []string{
		"CREATE TABLE sessions(id INTEGER PRIMARY KEY, installkey TEXT, installpub TEXT)",
		"CREATE TABLE runs(id INTEGER PRIMARY KEY, session INTEGER, installkey TEXT, map TEXT, player TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, verdict TEXT, judged INTEGER, file TEXT)",
		"INSERT INTO sessions(installkey, installpub) VALUES('ka', 'pubA'), ('kb', NULL), ('kc', 'pubC')",
	} {
		f.exec(q)
	}
	h, err := sqlite.Create(f.hub)
	if err != nil {
		t.Fatal(err)
	}
	defer h.Close()
	for _, q := range []string{
		"CREATE TABLE installs(key TEXT PRIMARY KEY, name TEXT)",
		"INSERT INTO installs VALUES('pubA', 'alice'), ('pubC', '')",
	} {
		if _, err := h.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	return f
}

func (f *fixture) exec(q string, args ...any) {
	f.t.Helper()
	if _, err := f.w.Exec(q, args...); err != nil {
		f.t.Fatal(err)
	}
}

// run adds a run: judged is its id in seconds, so later runs are judged later.
func (f *fixture) run(id int, install, player, mapname, kind string, mode, ms int, verdict string) {
	f.exec("INSERT INTO runs(id, installkey, map, player, runkind, mode, runtimems, verdict, judged, file) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
		id, install, mapname, player, kind, mode, ms, verdict, id*1000, "runs/"+install+".sbr")
}

func (f *fixture) open() *Store {
	s, err := Open(f.witness, f.hub, f.site, time.Hour)
	if err != nil {
		f.t.Fatal(err)
	}
	f.t.Cleanup(s.Close)
	return s
}

func entries(t *testing.T, s *Store, k Key) []Entry {
	t.Helper()
	b, err := s.Board(k)
	if err != nil {
		t.Fatal(err)
	}
	if b == nil {
		return nil
	}
	return b.Entries
}

func TestBoards(t *testing.T) {
	f := newFixture(t)
	forge := Key{"forge", "flagrun", 17}
	f.run(1, "ka", "a-replay", "forge", "flagrun", 17, 10000, "valid")
	f.run(2, "kb", "bob1", "forge", "flagrun", 17, 9500, "valid")
	f.run(3, "ka", "a-replay", "forge", "flagrun", 17, 9000, "valid")
	f.run(4, "kb", "bob2", "forge", "flagrun", 17, 8000, "rejected")
	f.run(5, "kc", "carol", "forge", "flagrun", 17, 9500, "valid")
	f.run(6, "kb", "bob3", "forge", "flagrun", 17, 0, "valid")
	f.run(7, "kb", "bob4", "forge", "flagrun", 11, 7000, "valid")
	s := f.open()

	got := entries(t, s, forge)
	want := []struct {
		run  int64
		name string
		ms   int64
	}{{3, "alice", 9000}, {2, "bob4", 9500}, {5, "carol", 9500}}
	if len(got) != len(want) {
		t.Fatalf("board %+v", got)
	}
	for i, w := range want {
		if got[i].Rank != i+1 || got[i].Run != w.run || got[i].Name != w.name || got[i].MS != w.ms {
			t.Fatalf("entry %d: %+v, want %+v", i, got[i], w)
		}
	}
	if e := entries(t, s, Key{"forge", "flagrun", 11}); len(e) != 1 || e[0].Run != 7 {
		t.Fatalf("ctf board %+v", e)
	}
	if ok, _ := s.Listed(1); ok {
		t.Fatal("a slower run of an install on the board is listed")
	}

	// a removed run gives way to the install's next best; a banned install leaves every board
	if err := s.Remove(3, "copied"); err != nil {
		t.Fatal(err)
	}
	if got = entries(t, s, forge); len(got) != 3 || got[2].Run != 1 || got[2].Rank != 3 {
		t.Fatalf("after removing run 3: %+v", got)
	}
	if ok, _ := s.Listed(1); !ok {
		t.Fatal("run 1 isn't listed after run 3 was removed")
	}
	if err := s.Ban("kb", "cheating"); err != nil {
		t.Fatal(err)
	}
	if got = entries(t, s, forge); len(got) != 2 || got[0].Install != "kc" {
		t.Fatalf("after banning kb: %+v", got)
	}
	if e := entries(t, s, Key{"forge", "flagrun", 11}); e != nil {
		t.Fatalf("a banned install's only board is still there: %+v", e)
	}
	s.Restore(3)
	s.Unban("kb")
	if got = entries(t, s, forge); len(got) != 3 || got[0].Run != 3 {
		t.Fatalf("after restoring: %+v", got)
	}
	if notes, _ := s.RemovedRuns(); len(notes) != 0 {
		t.Fatalf("removed after restore: %+v", notes)
	}

	places, name, err := s.Player("kb")
	if err != nil || name != "bob4" || len(places) != 2 || places[0].Key.Mode != 11 || places[0].Entry.Rank != 1 || places[1].Of != 3 {
		t.Fatalf("player kb: %q %+v %v", name, places, err)
	}

	// records in the order they were judged: 10000 (first), 9500 beat it, 9000 beat that
	recs, err := s.Records(10)
	if err != nil {
		t.Fatal(err)
	}
	var forgeRecs []Record
	for _, r := range recs {
		if r.Key == forge {
			forgeRecs = append(forgeRecs, r)
		}
	}
	if len(forgeRecs) != 3 || forgeRecs[0].Run != 3 || forgeRecs[0].Prev != 9500 || forgeRecs[2].Run != 1 || forgeRecs[2].Prev != -1 {
		t.Fatalf("records %+v", forgeRecs)
	}
}

func TestLabels(t *testing.T) {
	if l := (Key{"mpsp6a", "campaign", -3}).MapLabel(); l != "An Army Of One" {
		t.Fatal(l)
	}
	if l := (Key{"forge", "flagrun", 17}).KindLabel(); l != "flagrun, effic ctf" {
		t.Fatal(l)
	}
	if l := Time(369260); l != "6:09.260" {
		t.Fatal(l)
	}
}

func TestMerge(t *testing.T) {
	f := newFixture(t)
	forge := Key{"forge", "flagrun", 17}
	f.run(1, "ka", "a-replay", "forge", "flagrun", 17, 10000, "valid")
	f.run(2, "kb", "b-replay", "forge", "flagrun", 17, 9000, "valid")
	f.run(3, "kc", "carol", "forge", "flagrun", 17, 9500, "valid")
	s := f.open()

	// kb is alice's second install: one entry with the better time, under alice's name
	if err := s.Merge("kb", "ka"); err != nil {
		t.Fatal(err)
	}
	got := entries(t, s, forge)
	if len(got) != 2 || got[0].Install != "ka" || got[0].MS != 9000 || got[0].Name != "alice" || got[1].Install != "kc" {
		t.Fatalf("after merging: %+v", got)
	}
	if k := s.Canonical("kb"); k != "ka" {
		t.Fatalf("canonical of kb: %q", k)
	}
	if keys := s.Keys("ka"); len(keys) != 2 || keys[1] != "kb" {
		t.Fatalf("keys %v", keys)
	}
	// merging ka (with kb) into kc moves both
	if err := s.Merge("ka", "kc"); err != nil {
		t.Fatal(err)
	}
	if got = entries(t, s, forge); len(got) != 1 || got[0].Install != "kc" || got[0].MS != 9000 {
		t.Fatalf("after merging into kc: %+v", got)
	}
	if err := s.Merge("kb", "kc"); err == nil {
		t.Fatal("merging a player into itself")
	}
	s.SetName("kc", "Carol")
	if got = entries(t, s, forge); got[0].Name != "Carol" {
		t.Fatalf("picked name: %+v", got)
	}
	// a ban on any key covers the player
	s.Ban("kb", "test")
	if got = entries(t, s, forge); len(got) != 0 {
		t.Fatalf("ban on a merged key: %+v", got)
	}
	s.Unban("kb")
	s.Split("ka")
	s.Split("kb")
	if got = entries(t, s, forge); len(got) != 3 {
		t.Fatalf("after splitting: %+v", got)
	}
}
