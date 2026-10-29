package hub

import (
	"bytes"
	"crypto/rand"
	"os"
	"path/filepath"
	"testing"
)

func TestGetMap(t *testing.T) {
	f := newFixture(t)
	dir := t.TempDir()
	base := filepath.Join(dir, "packages", "base")
	os.MkdirAll(base, 0o755)
	ogz := make([]byte, 1500<<10) // many pieces
	rand.Read(ogz)
	cfg := []byte("mapmodel carrot\n")
	os.WriteFile(filepath.Join(base, "jump.ogz"), ogz, 0o644)
	os.WriteFile(filepath.Join(base, "jump.cfg"), cfg, 0o644)
	os.WriteFile(filepath.Join(base, "nocfg.ogz"), ogz[:100], 0o644)
	f.h.cfg.Maps = dir
	race := filepath.Join(t.TempDir(), "packages", "base")
	os.MkdirAll(race, 0o755)
	os.WriteFile(filepath.Join(race, "speed1.ogz"), ogz[:200], 0o644)
	os.WriteFile(filepath.Join(race, "Bad Name.ogz"), ogz[:10], 0o644)
	os.WriteFile(filepath.Join(race, "notes.txt"), []byte("x"), 0o644)
	f.h.cfg.RaceMaps = filepath.Dir(filepath.Dir(race))

	a := f.connect(newKey(), "1.1.1.1", "alice")
	// the race map list: the race dir's maps with valid names
	m := a.expect(MsgRaceMaps)
	if n := m.r.u(); n != 1 || m.r.str(64) != "speed1" {
		t.Fatalf("race maps: %d", n)
	}
	ask := func(name string) {
		var q writer
		q.str(name)
		a.send(MsgGetMap, q.b)
	}
	// receive: the files of one map in the order they come, keyed by role
	receive := func(name string) map[uint64][]byte {
		t.Helper()
		got := map[uint64][]byte{}
		for {
			m := a.expect(MsgMapData)
			n, role, size, off := m.r.str(64), m.r.u(), m.r.u(), m.r.u()
			if n != name || off != uint64(len(got[role])) {
				t.Fatalf("piece of %q role %d at %d after %d bytes", n, role, off, len(got[role]))
			}
			got[role] = append(got[role], m.r.b...)
			if role == MapCfg && got[MapOgz] != nil {
				t.Fatal("the cfg came after the ogz")
			}
			if role == MapOgz && uint64(len(got[MapOgz])) == size {
				return got
			}
		}
	}
	ask("jump")
	if got := receive("jump"); !bytes.Equal(got[MapOgz], ogz) || !bytes.Equal(got[MapCfg], cfg) {
		t.Fatal("the map arrived changed")
	}
	ask("speed1")
	if got := receive("speed1"); !bytes.Equal(got[MapOgz], ogz[:200]) {
		t.Fatal("a race map from the race dir")
	}
	ask("nocfg")
	if got := receive("nocfg"); !bytes.Equal(got[MapOgz], ogz[:100]) || got[MapCfg] != nil {
		t.Fatal("a map without a cfg")
	}
	for name, why := range map[string]string{"missing": "doesn't have missing", "../hub": "no map is named that", "a/b": "no map is named that"} {
		ask(name)
		a.expectNotice(why)
		if m := a.expect(MsgMapData); m.r.str(64) != name || m.r.u() != MapOgz || m.r.u() != 0 {
			t.Fatalf("%q: a refusal is not an empty ogz piece", name)
		}
	}
}
