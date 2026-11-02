package hub

import (
	"bytes"
	"crypto/rand"
	"hash/crc32"
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
	if n := f.h.RefreshMapCRCs(); n != 1 {
		t.Fatalf("checksums of %d maps, want 1", n)
	}

	a := f.connect(newKey(), "1.1.1.1", "alice")
	// the race map list: the race dir's maps with valid names, then the CRCs of their files
	m := a.expect(MsgRaceMaps)
	if n := m.r.u(); n != 1 || m.r.str(64) != "speed1" {
		t.Fatalf("race maps: %d", n)
	}
	if ogzCRC, cfgCRC := m.r.u(), m.r.u(); ogzCRC != uint64(crc32.ChecksumIEEE(ogz[:200])) || cfgCRC != 0 {
		t.Fatalf("race map checksums %x %x", ogzCRC, cfgCRC)
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

func TestMapCRCRefresh(t *testing.T) {
	f := newFixture(t)
	dir := t.TempDir()
	base := filepath.Join(dir, "packages", "base")
	os.MkdirAll(base, 0o755)
	write := func(name, data string) { os.WriteFile(filepath.Join(base, name), []byte(data), 0o644) }
	write("speed1.ogz", "first version")
	write("speed2.ogz", "other map")
	write("speed2.cfg", "mapmodel carrot")
	f.h.cfg.RaceMaps = dir
	crcs := func() []uint64 {
		var r reader
		r.b = f.h.raceMapsMsg()
		n := r.u()
		for i := uint64(0); i < n; i++ {
			r.str(64)
		}
		var out []uint64
		for i := uint64(0); i < 2*n; i++ {
			out = append(out, r.u())
		}
		return out
	}
	if got := crcs(); got[0] != 0 || got[2] != 0 {
		t.Fatalf("checksums before the first refresh: %v", got)
	}
	if n := f.h.RefreshMapCRCs(); n != 2 {
		t.Fatalf("first refresh read %d maps", n)
	}
	if n := f.h.RefreshMapCRCs(); n != 0 {
		t.Fatalf("unchanged maps were read again: %d", n)
	}
	before := crcs()
	if before[3] != uint64(crc32.ChecksumIEEE([]byte("mapmodel carrot"))) {
		t.Fatalf("cfg checksum %x", before[3])
	}
	write("speed1.ogz", "second version, longer")
	os.Remove(filepath.Join(base, "speed2.cfg"))
	if n := f.h.RefreshMapCRCs(); n != 2 {
		t.Fatalf("changed maps read: %d, want 2", n)
	}
	after := crcs()
	if after[0] != uint64(crc32.ChecksumIEEE([]byte("second version, longer"))) || after[0] == before[0] || after[3] != 0 {
		t.Fatalf("after the change: %v, before %v", after, before)
	}
}
