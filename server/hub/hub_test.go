package hub

import (
	"encoding/hex"
	"net"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/flynn/noise"

	"speedbraten/server/noisechan"
)

type clock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *clock) now() time.Time { c.mu.Lock(); defer c.mu.Unlock(); return c.t }
func (c *clock) add(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

type fixture struct {
	t     *testing.T
	h     *Hub
	clock *clock
}

func newFixture(t *testing.T, admins ...noise.DHKey) *fixture {
	key, _ := noisechan.GenerateKey()
	c := &clock{t: time.Unix(1_800_000_000, 0)}
	cfg := Config{Key: key, DB: filepath.Join(t.TempDir(), "hub.db"), Admins: map[string]bool{}, Now: c.now, Logf: t.Logf}
	for _, a := range admins {
		cfg.Admins[hex.EncodeToString(a.Public)] = true
	}
	h, err := New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(h.Close)
	return &fixture{t, h, c}
}

type msg struct {
	typ byte
	r   reader
}

type client struct {
	t    *testing.T
	key  noise.DHKey
	ch   *noisechan.Conn
	conn net.Conn
	in   chan msg
	id   uint64
	name string
}

func (f *fixture) connect(key noise.DHKey, addr, name string) *client {
	a, b := net.Pipe()
	go f.h.Serve(b, addr)
	ch, hello, err := noisechan.Client(a, key, f.h.cfg.Key.Public, []byte(Prologue), []byte("test build"))
	if err != nil || len(hello) != 1 || hello[0] != Proto {
		f.t.Fatalf("handshake: %v %v", hello, err)
	}
	c := &client{t: f.t, key: key, ch: ch, conn: a, in: make(chan msg, 1000)}
	go func() {
		defer close(c.in)
		for {
			typ, p, err := ch.Read()
			if err != nil {
				return
			}
			c.in <- msg{typ, reader{b: append([]byte(nil), p...)}}
		}
	}()
	var w writer
	w.str(name)
	c.send(MsgHello, w.b)
	m := c.expect(MsgWelcome)
	c.id = m.r.u()
	c.name = m.r.str(64)
	return c
}

func (c *client) send(typ byte, payload []byte) {
	if err := c.ch.Write(typ, payload); err != nil {
		c.t.Fatalf("send %d: %v", typ, err)
	}
}

// next returns the next message of type typ, skipping others; ok is false on timeout or close.
func (c *client) next(typ byte, wait time.Duration) (msg, bool) {
	deadline := time.After(wait)
	for {
		select {
		case m, ok := <-c.in:
			if !ok {
				return msg{}, false
			}
			if m.typ == typ {
				return m, true
			}
		case <-deadline:
			return msg{}, false
		}
	}
}

func (c *client) expect(typ byte) msg {
	c.t.Helper()
	m, ok := c.next(typ, 2*time.Second)
	if !ok {
		c.t.Fatalf("no message %d", typ)
	}
	return m
}

func (c *client) expectNotice(sub string) {
	c.t.Helper()
	for {
		m := c.expect(MsgNotice)
		if text := m.r.str(1000); strings.Contains(text, sub) {
			return
		} else {
			c.t.Logf("notice %q", text)
		}
	}
}

// expectPlayer returns the next PLAYER record of player id, read past the id.
func (c *client) expectPlayer(id uint64) msg {
	c.t.Helper()
	for {
		m := c.expect(MsgPlayer)
		if m.r.u() == id {
			return m
		}
	}
}

func (c *client) expectClosed() {
	c.t.Helper()
	deadline := time.After(2 * time.Second)
	for {
		select {
		case _, open := <-c.in:
			if !open {
				return
			}
		case <-deadline:
			c.t.Fatal("still open")
		}
	}
}

func (c *client) chat(scope int, text string) {
	var w writer
	w.u(uint64(scope))
	w.str(text)
	c.send(MsgChat, w.b)
}

func (c *client) mapmsg(typ byte, mapname string, mode int64) {
	var w writer
	w.str(mapname)
	w.s(mode)
	c.send(typ, w.b)
}

type chatline struct {
	scope int
	from  uint64
	name  string
	text  string
}

func (c *client) expectChat() chatline {
	c.t.Helper()
	m := c.expect(MsgChatFrom)
	return chatline{int(m.r.u()), m.r.u(), m.r.str(64), m.r.str(1000)}
}

type lobbyinfo struct {
	id, host uint64
	mapname  string
	mode     int64
	members  []uint64
}

func (c *client) expectLobby() lobbyinfo {
	c.t.Helper()
	m := c.expect(MsgLobby)
	l := lobbyinfo{id: m.r.u(), host: m.r.u(), mapname: m.r.str(64), mode: m.r.s()}
	for n := m.r.u(); n > 0; n-- {
		l.members = append(l.members, m.r.u())
	}
	if err := m.r.done(); err != nil {
		c.t.Fatal(err)
	}
	return l
}

func newKey() noise.DHKey { k, _ := noisechan.GenerateKey(); return k }

func TestNamesAndPresence(t *testing.T) {
	f := newFixture(t)
	ka, kb := newKey(), newKey()
	a := f.connect(ka, "1.1.1.1", "alice")
	if a.name != "alice" {
		t.Fatalf("a got name %q", a.name)
	}
	b := f.connect(kb, "2.2.2.2", "ALICE")
	if b.name != "" {
		t.Fatalf("b took a's name: %q", b.name)
	}
	var w writer
	w.str("bob")
	b.send(MsgSetName, w.b)
	m := b.expect(MsgWelcome)
	if id, name, why := m.r.u(), m.r.str(64), m.r.str(200); id != b.id || name != "bob" || why != "" {
		t.Fatalf("rename: %d %q %q", id, name, why)
	}
	m = a.expectPlayer(b.id)
	if name := m.r.str(64); name != "bob" {
		t.Fatalf("a saw %q", name)
	}

	w = writer{}
	w.u(StatePlaying)
	w.str("forge")
	w.s(17)
	b.send(MsgStatus, w.b)
	m = a.expectPlayer(b.id)
	if id, name, state, mapname, mode := b.id, m.r.str(64), m.r.u(), m.r.str(64), m.r.s(); id != b.id || name != "bob" || state != StatePlaying || mapname != "forge" || mode != 17 {
		t.Fatalf("status: %d %q %d %q %d", id, name, state, mapname, mode)
	}

	b.conn.Close()
	m = a.expect(MsgGone)
	if id := m.r.u(); id != b.id {
		t.Fatalf("gone %d", id)
	}

	// names stay with the install while it is away
	c := f.connect(newKey(), "3.3.3.3", "bob")
	if c.name != "" {
		t.Fatalf("c took bob while b was away")
	}
	b = f.connect(kb, "2.2.2.2", "")
	if b.name != "bob" {
		t.Fatalf("b came back as %q", b.name)
	}

	// a second connection of the same install replaces the first
	a2 := f.connect(ka, "1.1.1.1", "alice")
	a.expectNotice("somewhere else")
	if a2.name != "alice" {
		t.Fatalf("a2 is %q", a2.name)
	}
	m = b.expect(MsgGone)
	if id := m.r.u(); id != a.id {
		t.Fatalf("gone %d, want %d", id, a.id)
	}
}

func TestBadNames(t *testing.T) {
	f := newFixture(t)
	for _, n := range []string{"has space", "sixteen_letters_", "\fcolour"} {
		c := f.connect(newKey(), "1.1.1.1", n)
		if c.name != "" {
			t.Errorf("%q accepted", n)
		}
		c.conn.Close()
	}
}

func TestLobbies(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	nobody := f.connect(newKey(), "3.3.3.3", "")

	nobody.mapmsg(MsgLobbyCreate, "forge", 17)
	nobody.expectNotice("pick a name")

	a.mapmsg(MsgLobbyCreate, "forge", 17)
	l := b.expectLobby()
	if l.host != a.id || l.mapname != "forge" || l.mode != 17 || len(l.members) != 1 {
		t.Fatalf("created %+v", l)
	}
	var w writer
	w.u(l.id)
	b.send(MsgLobbyJoin, w.b)
	l = a.expectLobby()
	l = a.expectLobby()
	if len(l.members) != 2 || l.members[1] != b.id {
		t.Fatalf("joined %+v", l)
	}

	b.mapmsg(MsgLobbyMap, "nadefly", 4)
	b.expectNotice("only the lobby's host")
	a.mapmsg(MsgLobbyMap, "nadefly", 4)
	for {
		l = b.expectLobby()
		if l.mapname == "nadefly" {
			break
		}
	}
	if l.mode != 4 || l.host != a.id {
		t.Fatalf("map change %+v", l)
	}

	// a late joiner sees the lobby
	c := f.connect(newKey(), "4.4.4.4", "carol")
	if l2 := c.expectLobby(); l2.id != l.id || l2.mapname != "nadefly" || len(l2.members) != 2 {
		t.Fatalf("late joiner saw %+v", l2)
	}

	a.send(MsgLobbyLeave, nil)
	for {
		l = c.expectLobby()
		if len(l.members) == 1 {
			break
		}
	}
	if l.host != b.id {
		t.Fatalf("host after leave %d, want %d", l.host, b.id)
	}
	b.conn.Close()
	m := c.expect(MsgLobbyGone)
	if id := m.r.u(); id != l.id {
		t.Fatalf("lobby gone %d", id)
	}
}

func TestChat(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	c := f.connect(newKey(), "3.3.3.3", "carol")
	nobody := f.connect(newKey(), "4.4.4.4", "")

	a.chat(ChatGlobal, "  hello \f3there  ")
	for _, x := range []*client{a, b, c} {
		if got := x.expectChat(); got != (chatline{ChatGlobal, a.id, "alice", "hello 3there"}) {
			t.Fatalf("%s got %+v", x.name, got)
		}
	}

	a.chat(ChatLobby, "nobody hears this")
	a.expectNotice("aren't in a lobby")
	a.mapmsg(MsgLobbyCreate, "forge", 17)
	l := b.expectLobby()
	var w writer
	w.u(l.id)
	b.send(MsgLobbyJoin, w.b)
	b.expectLobby()
	b.chat(ChatLobby, "just us")
	if got := a.expectChat(); got.scope != ChatLobby || got.text != "just us" {
		t.Fatalf("a got %+v", got)
	}
	b.expectChat()
	a.chat(ChatGlobal, "everyone")
	if got := c.expectChat(); got.text != "everyone" {
		t.Fatalf("carol heard %+v before the global line", got)
	}

	nobody.chat(ChatGlobal, "hi")
	nobody.expectNotice("pick a name")
	if _, ok := nobody.next(MsgChatFrom, 100*time.Millisecond); ok {
		t.Fatal("an unnamed player hears no chat")
	}

	rows, err := f.h.st.db.Query("SELECT name, scope, lobby, text FROM chat ORDER BY rowid")
	if err != nil || len(rows) != 3 || rows[1][0] != "bob" || rows[1][1] != "1" || rows[1][3] != "just us" {
		t.Fatalf("chat log %v %v", rows, err)
	}

	f.clock.add(31 * 24 * time.Hour)
	a.chat(ChatGlobal, "a month later")
	if n, err := f.h.Prune(); err != nil || n != 3 {
		t.Fatalf("pruned %d, %v", n, err)
	}
}

func TestTalkRate(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	for i := 0; i < int(TalkBurst); i++ {
		a.chat(ChatGlobal, "spam")
		a.expectChat()
	}
	a.chat(ChatGlobal, "spam")
	a.expectNotice("slow down")
	f.clock.add(2 * time.Second)
	a.chat(ChatGlobal, "later")
	if got := a.expectChat(); got.text != "later" {
		t.Fatalf("got %+v", got)
	}
}

func TestAdmin(t *testing.T) {
	admin := newKey()
	f := newFixture(t, admin)
	a := f.connect(admin, "1.1.1.1", "boss")
	b := f.connect(newKey(), "2.2.2.2", "bob")

	b.chat(ChatGlobal, "#mute boss")
	if got := a.expectChat(); got.text != "#mute boss" {
		t.Fatalf("a player's # line is plain chat: %+v", got)
	}
	a.chat(ChatGlobal, "#mute bob 5")
	b.expectNotice("you are muted")
	a.expectNotice("muted bob")
	b.chat(ChatGlobal, "hello?")
	b.expectNotice("you are muted")

	// the mute is kept with the install
	b.conn.Close()
	b = f.connect(b.key, "2.2.2.2", "")
	b.chat(ChatGlobal, "hello?")
	b.expectNotice("you are muted")
	f.clock.add(6 * time.Minute)
	b.chat(ChatGlobal, "free")
	if got := a.expectChat(); got.text != "free" {
		t.Fatalf("after the mute: %+v", got)
	}

	var w writer
	w.u(a.id)
	w.str("bossy")
	b.send(MsgReport, w.b)
	b.expectNotice("reported boss")
	rows, _ := f.h.st.db.Query("SELECT name, reason FROM reports")
	if len(rows) != 1 || rows[0][0] != "boss" || rows[0][1] != "bossy" {
		t.Fatalf("reports %v", rows)
	}

	a.chat(ChatGlobal, "#kick bob")
	b.expectNotice("kicked")
	b.expectClosed()
	m := a.expect(MsgGone)
	if id := m.r.u(); id != b.id {
		t.Fatalf("gone %d", id)
	}
}

func TestLimits(t *testing.T) {
	f := newFixture(t)
	old := MaxNewInstallsPerDay
	MaxNewInstallsPerDay = 2
	defer func() { MaxNewInstallsPerDay = old }()
	f.connect(newKey(), "9.9.9.9", "")
	f.connect(newKey(), "9.9.9.9", "")
	a, b := net.Pipe()
	go f.h.Serve(b, "9.9.9.9")
	var w writer
	w.str("test")
	ch, _, err := noisechan.Client(a, newKey(), f.h.cfg.Key.Public, []byte(Prologue), w.b)
	if err != nil {
		t.Fatal(err)
	}
	w = writer{}
	w.str("third")
	ch.Write(MsgHello, w.b)
	typ, p, err := ch.Read()
	r := reader{b: p}
	if err != nil || typ != MsgNotice || !strings.Contains(r.str(200), "too many new installs") {
		t.Fatalf("third install: %d %v", typ, err)
	}
	f.clock.add(25 * time.Hour)
	f.connect(newKey(), "9.9.9.9", "")

	// key request
	a, b = net.Pipe()
	go f.h.Serve(b, "8.8.8.8")
	reply, err := noisechan.RequestKeys(a)
	if err != nil || string(reply[:5]) != "SBRH\x01" || string(reply[5:]) != string(f.h.cfg.Key.Public) {
		t.Fatalf("key reply %x %v", reply, err)
	}

	// a malformed message ends the connection
	c := f.connect(newKey(), "7.7.7.7", "eve")
	c.send(MsgStatus, []byte{9})
	c.expectClosed()
}

func TestWitness(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	ch, sign := make([]byte, 32), make([]byte, 32)
	ch[0], sign[0] = 1, 2
	f.h.SetWitness("w.example", 28790, ch, sign)
	check := func(c *client) {
		t.Helper()
		m := c.expect(MsgWitness)
		host, port := m.r.str(256), m.r.u()
		if host != "w.example" || port != 28790 || len(m.r.b) != 64 || m.r.b[0] != 1 || m.r.b[32] != 2 {
			t.Fatalf("witness %q %d %x", host, port, m.r.b)
		}
	}
	check(a)
	check(f.connect(newKey(), "2.2.2.2", "bob"))
}
