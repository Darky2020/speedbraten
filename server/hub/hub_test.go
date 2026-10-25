package hub

import (
	"bytes"
	"encoding/hex"
	"net"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/flynn/noise"

	"fmt"
	"speedbraten/server/board"
	"speedbraten/server/noisechan"
	"speedbraten/server/sqlite"
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
	w.str("flagrun")
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
	kind     string
	specs    []uint64
	watched  []uint64
}

func (c *client) expectLobby() lobbyinfo {
	c.t.Helper()
	m := c.expect(MsgLobby)
	l := lobbyinfo{id: m.r.u(), host: m.r.u(), mapname: m.r.str(64), mode: m.r.s()}
	for n := m.r.u(); n > 0; n-- {
		l.members = append(l.members, m.r.u())
	}
	l.kind = m.r.str(16)
	for n := m.r.u(); n > 0; n-- {
		l.specs = append(l.specs, m.r.u())
		l.watched = append(l.watched, m.r.u())
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
	w.str("flagrun")
	b.send(MsgStatus, w.b)
	m = a.expectPlayer(b.id)
	if name, state, mapname, mode, lobby, kind := m.r.str(64), m.r.u(), m.r.str(64), m.r.s(), m.r.u(), m.r.str(16); name != "bob" || state != StatePlaying || mapname != "forge" || mode != 17 || lobby != 0 || kind != "flagrun" {
		t.Fatalf("status: %q %d %q %d %d %q", name, state, mapname, mode, lobby, kind)
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
	for _, n := range []string{"has space", "sixteen_letters_", "\fcolour", "bad\xc5utf8", ""} {
		c := f.connect(newKey(), "1.1.1.1", n)
		if c.name != "" {
			t.Errorf("%q accepted", n)
		}
		c.conn.Close()
	}
}

func TestUnicodeNames(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "*Ur|Ňєkø")
	if a.name != "*Ur|Ňєkø" {
		t.Fatalf("got %q", a.name)
	}
	b := f.connect(newKey(), "2.2.2.2", "*UR|ŇЄKØ")
	if b.name != "" {
		t.Fatalf("a name differing only in case was taken: %q", b.name)
	}
	c := f.connect(newKey(), "3.3.3.3", "ПятнадцатьБукв!")
	if c.name != "ПятнадцатьБукв!" {
		t.Fatalf("15 Cyrillic characters refused: %q", c.name)
	}
	a.chat(ChatGlobal, "привіт \x01")
	if got := a.expectChat(); got.text != "привіт" {
		t.Fatalf("chat %q", got.text)
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
	if l.host != a.id || l.mapname != "forge" || l.mode != 17 || len(l.members) != 1 || l.kind != "flagrun" {
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

func TestFinish(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	c := f.connect(newKey(), "3.3.3.3", "carol")
	a.mapmsg(MsgLobbyCreate, "forge", 17)
	l := b.expectLobby()
	var w writer
	w.u(l.id)
	b.send(MsgLobbyJoin, w.b)
	a.expectLobby()
	a.expectLobby()
	w = writer{}
	w.str("forge")
	w.s(17)
	w.str("flagrun")
	w.u(12345)
	a.send(MsgFinish, w.b)
	m := b.expect(MsgFinished)
	if id, name, mapname, mode, kind, ms := m.r.u(), m.r.str(64), m.r.str(64), m.r.s(), m.r.str(16), m.r.u(); id != a.id || name != "alice" || mapname != "forge" || mode != 17 || kind != "flagrun" || ms != 12345 {
		t.Fatalf("finished: %d %q %q %d %q %d", id, name, mapname, mode, kind, ms)
	}
	if _, ok := a.next(MsgFinished, 200*time.Millisecond); ok {
		t.Fatal("the finisher heard their own finish")
	}
	if _, ok := c.next(MsgFinished, 200*time.Millisecond); ok {
		t.Fatal("a player outside the lobby heard the finish")
	}
}

func (c *client) status(mapname string, mode int64) {
	var w writer
	w.u(StatePlaying)
	w.str(mapname)
	w.s(mode)
	w.str("flagrun")
	c.send(MsgStatus, w.b)
}

func TestPos(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	c := f.connect(newKey(), "3.3.3.3", "carol")
	a.status("forge", 17)
	b.status("forge", 17)
	c.status("akroseum", 17)
	a.mapmsg(MsgLobbyCreate, "forge", 17)
	l := b.expectLobby()
	for _, x := range []*client{b, c} {
		var w writer
		w.u(l.id)
		x.send(MsgLobbyJoin, w.b)
	}
	for {
		if got := a.expectLobby(); len(got.members) == 3 {
			break
		}
	}
	var w writer
	for _, v := range []uint64{100, 200, 300, 4000, 5000, 9} {
		w.u(v)
	}
	sample := w.b
	a.send(MsgPos, sample)
	m := b.expect(MsgPosFrom)
	if id := m.r.u(); id != a.id || string(m.r.b) != string(sample) {
		t.Fatalf("posfrom %d %x", id, m.r.b)
	}
	if _, ok := c.next(MsgPosFrom, 200*time.Millisecond); ok {
		t.Fatal("a member on another map got the sample")
	}
	if _, ok := a.next(MsgPosFrom, 100*time.Millisecond); ok {
		t.Fatal("the sender got its own sample")
	}

	// a flood is cut to the rate, and the sender stays connected
	for i := 0; i < 200; i++ {
		a.send(MsgPos, sample)
	}
	a.chat(ChatGlobal, "still here")
	if got := a.expectChat(); got.text != "still here" {
		t.Fatalf("after the flood: %+v", got)
	}
	n := 0
	for {
		if _, ok := b.next(MsgPosFrom, 200*time.Millisecond); !ok {
			break
		}
		n++
	}
	if n < 40 || n > int(PosBurst)+5 {
		t.Fatalf("%d of 200 flooded samples passed", n)
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
	c.send(99, []byte("from a newer client"))
	c.chat(ChatGlobal, "still here")
	if got := c.expectChat(); got.text != "still here" {
		t.Fatalf("after an unknown message: %+v", got)
	}
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

func TestResults(t *testing.T) {
	f := newFixture(t)
	wdb := filepath.Join(t.TempDir(), "witness.db")
	w, err := sqlite.Create(wdb)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	if _, err := w.Exec("CREATE TABLE events(id INTEGER PRIMARY KEY, run INTEGER UNIQUE, installpub TEXT, player TEXT, map TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, record INTEGER, previous INTEGER, created INTEGER)"); err != nil {
		t.Fatal(err)
	}
	add := func(run int, pub, player string, ms int, record int, previous int) {
		if _, err := w.Exec("INSERT INTO events(run, installpub, player, map, runkind, mode, runtimems, record, previous, created) VALUES(?, ?, ?, 'forge', 'flagrun', 17, ?, ?, ?, 0)", run, pub, player, ms, record, previous); err != nil {
			t.Fatal(err)
		}
	}
	add(1, "old", "before", 20000, 1, -1)

	ka := newKey()
	a := f.connect(ka, "1.1.1.1", "alice")
	res, err := f.h.OpenResults(wdb)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Close()
	if n, err := res.Poll(); n != 0 || err != nil {
		t.Fatalf("first poll announced history: %d %v", n, err)
	}
	add(2, hex.EncodeToString(ka.Public), "replayname", 12345, 1, 20000)
	add(3, "unknown", "bob", 13001, 0, -1)
	if n, err := res.Poll(); n != 2 || err != nil {
		t.Fatalf("poll: %d %v", n, err)
	}
	read := func() (string, string, int64, string, uint64, uint64, uint64) {
		m := a.expect(MsgResult)
		return m.r.str(64), m.r.str(64), m.r.s(), m.r.str(16), m.r.u(), m.r.u(), m.r.u()
	}
	if name, mapname, mode, kind, ms, record, prev := read(); name != "alice" || mapname != "forge" || mode != 17 || kind != "flagrun" || ms != 12345 || record != 1 || prev != 20001 {
		t.Fatalf("record: %q %q %d %q %d %d %d", name, mapname, mode, kind, ms, record, prev)
	}
	if name, _, _, _, ms, record, prev := read(); name != "bob" || ms != 13001 || record != 0 || prev != 0 {
		t.Fatalf("pb: %q %d %d %d", name, ms, record, prev)
	}

	// a restarted hub goes on after the last event it announced
	res2, err := f.h.OpenResults(wdb)
	if err != nil {
		t.Fatal(err)
	}
	defer res2.Close()
	add(4, "unknown", "bob", 12000, 1, 12345)
	if n, err := res2.Poll(); n != 1 || err != nil {
		t.Fatalf("after a restart: %d %v", n, err)
	}
}

func (c *client) view(id uint64, mapname string) {
	var w writer
	w.u(id)
	w.str(mapname)
	w.s(17)
	w.str("flagrun")
	c.send(MsgView, w.b)
}

func (c *client) viewData(id uint64, off int, b []byte) {
	var w writer
	w.u(id)
	w.u(uint64(off))
	w.bytes(b)
	c.send(MsgViewData, w.b)
}

func TestSpectate(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	part1, part2 := bytes.Repeat([]byte{1}, 40000), []byte("next segment")
	a.view(1, "forge")
	a.viewData(1, 0, part1)
	a.chat(ChatGlobal, "sync")
	a.expectChat()

	// a spectator arriving mid-view gets the view so far, then the rest as it comes
	var w writer
	w.u(a.id)
	b.send(MsgSpectate, w.b)
	m := b.expect(MsgSpecRun)
	if id, view, mapname, mode, kind, chained := m.r.u(), m.r.u(), m.r.str(64), m.r.s(), m.r.str(16), m.r.u(); id != a.id || view != 1 || mapname != "forge" || mode != 17 || kind != "flagrun" || chained != 0 {
		t.Fatalf("specrun %d %d %q %d %q %d", id, view, mapname, mode, kind, chained)
	}
	var got []byte
	collect := func(view uint64, upto int) {
		t.Helper()
		for len(got) < upto {
			m := b.expect(MsgSpecData)
			v, off := m.r.u(), m.r.u()
			if v != view || int(off) != len(got) {
				t.Fatalf("specdata view %d offset %d, have %d", v, off, len(got))
			}
			got = append(got, m.r.b...)
		}
	}
	collect(1, len(part1))
	a.viewData(1, len(part1), part2)
	collect(1, len(part1)+len(part2))
	if !bytes.Equal(got, append(append([]byte(nil), part1...), part2...)) {
		t.Fatal("view bytes differ")
	}
	// a piece at the wrong offset is dropped
	a.viewData(1, 5, []byte("stray"))
	if _, ok := b.next(MsgSpecData, 200*time.Millisecond); ok {
		t.Fatal("a piece at the wrong offset was passed on")
	}

	// a new view (a load, a new map) starts over
	a.view(2, "forge")
	m = b.expect(MsgSpecRun)
	if _, view := m.r.u(), m.r.u(); view != 2 {
		t.Fatalf("new view %d", view)
	}
	got = nil
	a.viewData(2, 0, []byte("from a state"))
	collect(2, len("from a state"))

	a.conn.Close()
	m = b.expect(MsgSpecEnd)
	if id, why := m.r.u(), m.r.str(100); id != a.id || why != "alice left" {
		t.Fatalf("specend %d %q", id, why)
	}
}

func TestViewLimit(t *testing.T) {
	f := newFixture(t)
	f.h.maxView = 64
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	var w writer
	w.u(a.id)
	b.send(MsgSpectate, w.b)
	a.view(1, "forge")
	b.expect(MsgSpecRun)
	a.viewData(1, 0, bytes.Repeat([]byte{2}, 50))
	b.expect(MsgSpecData)
	a.viewData(1, 50, bytes.Repeat([]byte{2}, 50))
	if _, ok := b.next(MsgSpecData, 200*time.Millisecond); ok {
		t.Fatal("a view over the limit was passed on")
	}
}

// waitLobby reads LOBBY messages until one matches.
func (c *client) waitLobby(ok func(lobbyinfo) bool) lobbyinfo {
	c.t.Helper()
	for {
		if l := c.expectLobby(); ok(l) {
			return l
		}
	}
}

func TestLobbySpectators(t *testing.T) {
	f := newFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	a.mapmsg(MsgLobbyCreate, "forge", 17)
	la := b.expectLobby()
	b.mapmsg(MsgLobbyCreate, "nadefly", 4)
	lb := a.waitLobby(func(l lobbyinfo) bool { return l.id != la.id })

	// spectating alice takes bob out of his lobby and into hers as a spectator
	var w writer
	w.u(a.id)
	b.send(MsgSpectate, w.b)
	if m := a.expect(MsgLobbyGone); m.r.u() != lb.id {
		t.Fatal("bob's own lobby wasn't closed")
	}
	l := a.waitLobby(func(l lobbyinfo) bool { return l.id == la.id })
	if len(l.members) != 1 || len(l.specs) != 1 || l.specs[0] != b.id || l.watched[0] != a.id {
		t.Fatalf("watched lobby %+v", l)
	}

	// spectators hear and talk in the lobby's chat, and see its finishes
	b.chat(ChatLobby, "nice run")
	if got := a.expectChat(); got.scope != ChatLobby || got.text != "nice run" {
		t.Fatalf("alice got %+v", got)
	}
	b.expectChat()
	w = writer{}
	w.str("forge")
	w.s(17)
	w.str("flagrun")
	w.u(12345)
	a.send(MsgFinish, w.b)
	b.expect(MsgFinished)

	// the spectators move along when alice opens another lobby
	a.mapmsg(MsgLobbyCreate, "ot", 17)
	l = b.waitLobby(func(l lobbyinfo) bool { return l.mapname == "ot" && len(l.specs) == 1 })
	if l.specs[0] != b.id || l.host != a.id {
		t.Fatalf("moved lobby %+v", l)
	}

	// leaving the lobby while spectating stops spectating too
	b.send(MsgLobbyLeave, nil)
	if m := b.expect(MsgSpecEnd); m.r.u() != a.id {
		t.Fatal("specend for another player")
	}
	a.waitLobby(func(l lobbyinfo) bool { return len(l.specs) == 0 })
	f.h.mu.Lock()
	still := f.h.players[b.id].spec != nil
	f.h.mu.Unlock()
	if still {
		t.Fatal("still spectating after leaving the lobby")
	}

	// stopping leaves the lobby
	w = writer{}
	w.u(a.id)
	b.send(MsgSpectate, w.b)
	a.waitLobby(func(l lobbyinfo) bool { return len(l.specs) == 1 })
	w = writer{}
	w.u(0)
	b.send(MsgSpectate, w.b)
	l = a.waitLobby(func(l lobbyinfo) bool { return len(l.specs) == 0 })
	if len(l.members) != 1 {
		t.Fatalf("after stopping %+v", l)
	}
	b.chat(ChatLobby, "anyone?")
	b.expectNotice("aren't in a lobby")
}

type statinfo struct {
	id, running, ms, best, pb uint64
}

// expectStat reads LOBBYSTAT messages until one for that player.
func (c *client) expectStat(id uint64) statinfo {
	c.t.Helper()
	for {
		m := c.expect(MsgLobbyStat)
		s := statinfo{m.r.u(), m.r.u(), m.r.u(), m.r.u(), m.r.u()}
		if err := m.r.done(); err != nil {
			c.t.Fatal(err)
		}
		if s.id == id {
			return s
		}
	}
}

func TestLobbyStats(t *testing.T) {
	f := newFixture(t)
	wdb := filepath.Join(t.TempDir(), "witness.db")
	w, err := sqlite.Create(wdb)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	for _, q := range []string{
		"CREATE TABLE events(id INTEGER PRIMARY KEY, run INTEGER UNIQUE, installpub TEXT, player TEXT, map TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, record INTEGER, previous INTEGER, created INTEGER)",
		"CREATE TABLE sessions(id INTEGER PRIMARY KEY, installkey TEXT, installpub TEXT)",
		"CREATE TABLE bests(map TEXT, runkind TEXT, mode INTEGER, installkey TEXT, runtimems INTEGER, run INTEGER, achieved INTEGER)",
	} {
		if _, err := w.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	ka := newKey()
	pub := hex.EncodeToString(ka.Public)
	if _, err := w.Exec("INSERT INTO sessions(installkey, installpub) VALUES('ik', ?)", pub); err != nil {
		t.Fatal(err)
	}
	if _, err := w.Exec("INSERT INTO bests VALUES('forge', 'flagrun', 17, 'ik', 15000, 1, 0)"); err != nil {
		t.Fatal(err)
	}
	res, err := f.h.OpenResults(wdb)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Close()
	res.Poll()

	a := f.connect(ka, "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	a.mapmsg(MsgLobbyCreate, "forge", 17)
	if s := a.expectStat(a.id); s.pb != 15001 || s.best != 0 {
		t.Fatalf("on create %+v", s)
	}
	var rt writer
	rt.u(1)
	rt.u(4200)
	a.send(MsgRunTime, rt.b)
	if s := a.expectStat(a.id); s.running != 1 || s.ms != 4200 {
		t.Fatalf("run time %+v", s)
	}

	// a joiner gets everyone's numbers
	l := b.expectLobby()
	var jw writer
	jw.u(l.id)
	b.send(MsgLobbyJoin, jw.b)
	if s := b.expectStat(a.id); s.running != 1 || s.ms != 4200 || s.pb != 15001 {
		t.Fatalf("joiner saw %+v", s)
	}
	if s := a.expectStat(b.id); s.pb != 0 {
		t.Fatalf("bob's %+v", s)
	}

	finish := func(ms uint64) {
		var fw writer
		fw.str("forge")
		fw.s(17)
		fw.str("flagrun")
		fw.u(ms)
		a.send(MsgFinish, fw.b)
	}
	finish(14000)
	if s := b.expectStat(a.id); s.best != 14001 {
		t.Fatalf("lobby best %+v", s)
	}
	finish(14500)
	finish(13000)
	if s := b.expectStat(a.id); s.best != 13001 {
		t.Fatalf("a slower finish changed the best, or a faster one didn't: %+v", s)
	}

	// a witnessed PB on the lobby's map
	if _, err := w.Exec("INSERT INTO events(run, installpub, player, map, runkind, mode, runtimems, record, previous, created) VALUES(9, ?, 'alice', 'forge', 'flagrun', 17, 13000, 0, 15000, 0)", pub); err != nil {
		t.Fatal(err)
	}
	if n, err := res.Poll(); n != 1 || err != nil {
		t.Fatalf("poll %d %v", n, err)
	}
	if s := b.expectStat(a.id); s.pb != 13001 {
		t.Fatalf("pb after the result %+v", s)
	}

	// a new map starts the lobby bests over
	a.mapmsg(MsgLobbyMap, "nadefly", 17)
	if s := b.expectStat(a.id); s.best != 0 || s.pb != 0 {
		t.Fatalf("after the map change %+v", s)
	}
}

func TestBoardRequest(t *testing.T) {
	f := newFixture(t)
	dir := t.TempDir()
	wpath := filepath.Join(dir, "witness.db")
	w, err := sqlite.Create(wpath)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	ka := newKey()
	for _, q := range []string{
		"CREATE TABLE sessions(id INTEGER PRIMARY KEY, installkey TEXT, installpub TEXT)",
		"CREATE TABLE runs(id INTEGER PRIMARY KEY, installkey TEXT, map TEXT, player TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, verdict TEXT, judged INTEGER)",
	} {
		if _, err := w.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	w.Exec("INSERT INTO sessions(installkey, installpub) VALUES('wa', ?)", hex.EncodeToString(ka.Public))
	for i := 0; i < 12; i++ {
		w.Exec("INSERT INTO runs(installkey, map, player, runkind, mode, runtimems, verdict, judged) VALUES(?, 'forge', ?, 'flagrun', 17, ?, 'valid', 1)", fmt.Sprintf("k%d", i), fmt.Sprintf("p%d", i), 9000+i*100)
	}
	w.Exec("INSERT INTO runs(installkey, map, player, runkind, mode, runtimems, verdict, judged) VALUES('wa', 'forge', 'alice', 'flagrun', 17, 9950, 'valid', 1)")
	store, err := board.Open(wpath, "", filepath.Join(dir, "site.db"), time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	a := f.connect(ka, "1.1.1.1", "alice")
	ask := func(mapname string) msg {
		var q writer
		q.str(mapname)
		q.s(17)
		q.str("flagrun")
		a.send(MsgBoard, q.b)
		return a.expect(MsgBoardRows)
	}
	// a hub without the boards answers with an empty board
	if m := ask("forge"); m.r.str(64) != "forge" || m.r.s() != 17 || m.r.str(16) != "flagrun" || m.r.u() != 0 {
		t.Fatal("no boards: not an empty reply")
	}
	f.h.SetBoards(store)
	m := ask("forge")
	m.r.str(64)
	m.r.s()
	m.r.str(16)
	n := m.r.u()
	var first string
	var firstms uint64
	for i := uint64(0); i < n; i++ {
		rank, name, ms := m.r.u(), m.r.str(64), m.r.u()
		if i == 0 {
			first, firstms = name, ms
		}
		if rank != i+1 {
			t.Fatalf("row %d has rank %d", i, rank)
		}
	}
	rank, ms := m.r.u(), m.r.u()
	if err := m.r.done(); err != nil {
		t.Fatal(err)
	}
	if n != BoardRows || first != "p0" || firstms != 9000 || rank != 11 || ms != 9950 {
		t.Fatalf("board: %d rows, #1 %s %d, mine %d %d", n, first, firstms, rank, ms)
	}
}
