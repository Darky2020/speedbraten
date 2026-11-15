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

func (c *client) server(host string, port uint64, cn int64) {
	var w writer
	w.str(host)
	w.u(port)
	w.s(cn)
	c.send(MsgServer, w.b)
}

// playerServer reads a PLAYER's game server fields: label, host, port, client number.
func playerServer(m msg) (string, string, uint64, int64) {
	m.r.str(64)
	m.r.u()
	m.r.str(64)
	m.r.s()
	m.r.u()
	m.r.str(16)
	m.r.u()
	return m.r.str(100), m.r.str(255), m.r.u(), m.r.s()
}

// A player on a listed game server is shown on it to everyone, with its client number there; an
// unlisted server counts as none. Its hub spectators lose the view, and new ones are told to join.
func TestServerStatus(t *testing.T) {
	f := newFixture(t)
	path := filepath.Join(t.TempDir(), "servers.txt")
	os.WriteFile(path, []byte("51.15.126.195 28785 speedyruns race 1\n"), 0o644)
	f.h.cfg.Servers = path
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	a.view(1, "forge")
	b.spectate(a.id)
	b.expect(MsgSpecRun)

	a.server("51.15.126.195", 28785, 3)
	if m := b.expect(MsgSpecEnd); m.r.u() != a.id || m.r.str(100) != "alice joined speedyruns race 1" {
		t.Fatal("spectator not ended")
	}
	if label, host, port, cn := playerServer(b.expectPlayer(a.id)); label != "speedyruns race 1" || host != "51.15.126.195" || port != 28785 || cn != 3 {
		t.Fatalf("on a server: %q %q %d %d", label, host, port, cn)
	}
	b.spectate(a.id)
	b.expectNotice("alice is on speedyruns race 1")
	f.h.mu.Lock()
	if f.h.players[b.id].spec != nil {
		t.Error("spectating a player on a server through the hub")
	}
	f.h.mu.Unlock()

	a.server("10.0.0.1", 28785, 5)
	if label, host, port, cn := playerServer(b.expectPlayer(a.id)); label != "" || host != "" || port != 0 || cn != 0 {
		t.Fatalf("on an unlisted server: %q %q %d %d", label, host, port, cn)
	}
	a.server("51.15.126.195", 28785, 4)
	b.expectPlayer(a.id)
	a.server("", 0, 0)
	if label, _, _, _ := playerServer(b.expectPlayer(a.id)); label != "" {
		t.Fatalf("off the server: %q", label)
	}
}
