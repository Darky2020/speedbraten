package hub

import (
	"os"
	"path/filepath"
	"testing"
)

// The servers file's servers go to every player who joins, read at the join, so edits apply
// without a restart; malformed lines are left out.
func TestServers(t *testing.T) {
	f := newFixture(t)
	path := filepath.Join(t.TempDir(), "servers.txt")
	os.WriteFile(path, []byte("# speedyruns\n51.15.126.195 28785 speedyruns race 1\n\nbad line\n51.15.126.195 99999 bad port\n51.15.126.195 28795 speedyruns race 2\n"), 0o644)
	f.h.cfg.Servers = path

	a := f.connect(newKey(), "1.1.1.1", "alice")
	m := a.expect(MsgServers)
	if n := m.r.u(); n != 2 {
		t.Fatalf("%d servers, want 2", n)
	}
	for _, want := range []Server{{"51.15.126.195", 28785, "speedyruns race 1"}, {"51.15.126.195", 28795, "speedyruns race 2"}} {
		if got := (Server{m.r.str(255), int(m.r.u()), m.r.str(100)}); got != want {
			t.Fatalf("server %+v, want %+v", got, want)
		}
	}
	if err := m.r.done(); err != nil {
		t.Fatal(err)
	}

	os.WriteFile(path, []byte("1.2.3.4 28785 other\n"), 0o644)
	b := f.connect(newKey(), "2.2.2.2", "bob")
	m = b.expect(MsgServers)
	if n, host := m.r.u(), m.r.str(255); n != 1 || host != "1.2.3.4" {
		t.Fatalf("after the edit: %d servers, first %q", n, host)
	}
}

// Without a servers file the list is empty: online life is played nowhere.
func TestNoServers(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	if m := a.expect(MsgServers); m.r.u() != 0 {
		t.Fatal("servers listed without a servers file")
	}
}
