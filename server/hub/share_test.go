package hub

import (
	"bytes"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"speedbraten/server/board"
	"speedbraten/server/sqlite"
)

func goldReplay(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "tests", "gold", name))
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func newShareFixture(t *testing.T) *fixture {
	f := newFixture(t)
	f.h.cfg.Shared = t.TempDir()
	return f
}

// shareState waits for the next SHARESTATE, keeping the NOTICE sent before it in c.notice.
func (c *client) shareState() (state, id uint64) {
	c.t.Helper()
	c.notice = ""
	deadline := time.After(2 * time.Second)
	for {
		select {
		case m, ok := <-c.in:
			if !ok {
				c.t.Fatal("closed while waiting for SHARESTATE")
			}
			if m.typ == MsgNotice {
				c.notice = m.r.str(1000)
			} else if m.typ == MsgShareState {
				return m.r.u(), m.r.u()
			}
		case <-deadline:
			c.t.Fatal("no SHARESTATE")
		}
	}
}

func (c *client) expectShareNotice(sub string) {
	c.t.Helper()
	if !strings.Contains(c.notice, sub) {
		c.t.Fatalf("notice %q, want %q", c.notice, sub)
	}
}

// share uploads data in pieces of the given size and returns the SHARESTATE that ends it.
func (c *client) share(data []byte, piece int) (state, id uint64) {
	c.t.Helper()
	var w writer
	w.u(uint64(len(data)))
	c.send(MsgShare, w.b)
	if state, id = c.shareState(); state != ShareSend {
		return state, id
	}
	for off := 0; off < len(data); off += piece {
		var p writer
		p.u(uint64(off))
		p.bytes(data[off:min(off+piece, len(data))])
		c.send(MsgShareData, p.b)
	}
	return c.shareState()
}

func readRow(t *testing.T, r *reader) sharedRow {
	t.Helper()
	row := sharedRow{id: int64(r.u()), sharer: r.str(64), at: int64(r.u()), mapname: r.str(64), mode: r.s(), flags: r.u(), runner: r.str(64), runtimems: int64(r.u()), size: int64(r.u())}
	if r.err != nil {
		t.Fatal(r.err)
	}
	return row
}

func (c *client) sharedList() []sharedRow {
	c.t.Helper()
	c.send(MsgSharedList, nil)
	m := c.expect(MsgSharedRows)
	n := m.r.u()
	var rows []sharedRow
	for i := uint64(0); i < n; i++ {
		rows = append(rows, readRow(c.t, &m.r))
	}
	if err := m.r.done(); err != nil {
		c.t.Fatal(err)
	}
	return rows
}

func (c *client) sharedGet(id uint64) []byte {
	c.t.Helper()
	var w writer
	w.u(id)
	c.send(MsgSharedGet, w.b)
	var got []byte
	for {
		m := c.expect(MsgSharedData)
		gid, size, off := m.r.u(), m.r.u(), m.r.u()
		if gid != id || off != uint64(len(got)) {
			c.t.Fatalf("piece: id %d offset %d after %d bytes", gid, off, len(got))
		}
		if size == 0 {
			return nil
		}
		got = append(got, m.r.b...)
		if len(got) >= int(size) {
			return got
		}
	}
}

// A shared replay is announced to everyone named except its sharer and players on do not
// disturb, listed, and downloads unchanged; its pieces don't count against the message rate.
func TestShare(t *testing.T) {
	f := newShareFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	c := f.connect(newKey(), "3.3.3.3", "carol")
	var w writer
	w.u(AwayDND)
	c.send(MsgAway, w.b)
	c.expect(MsgPlayer)

	data := goldReplay(t, "forge_flagrun.sbr")
	state, id := a.share(data, 200) // ~80 pieces: over MsgBurst
	if state != ShareDone || id == 0 {
		t.Fatalf("share: state %d id %d", state, id)
	}
	m := b.expect(MsgShared)
	row := readRow(t, &m.r)
	if row.id != int64(id) || row.sharer != "alice" || row.mapname != "forge" || row.flags&ShareFlagrun == 0 || row.size != int64(len(data)) || row.runner == "" {
		t.Fatalf("announcement %+v", row)
	}
	if _, ok := c.next(MsgShared, 300*time.Millisecond); ok {
		t.Fatal("a player on do not disturb heard of the share")
	}
	if _, ok := a.next(MsgShared, 100*time.Millisecond); ok {
		t.Fatal("the sharer heard of its own share")
	}
	if rows := c.sharedList(); len(rows) != 1 || rows[0].id != int64(id) {
		t.Fatalf("list %+v", rows)
	}
	if got := c.sharedGet(id); !bytes.Equal(got, data) {
		t.Fatalf("download: %d bytes, want %d unchanged", len(got), len(data))
	}
	var g writer
	g.u(id + 100)
	c.send(MsgSharedGet, g.b)
	c.expectNotice("gone")
}

// Sharing is limited per player in time and size; broken files and broken uploads are refused;
// the same file shared again moves to the top; the buffer keeps the newest ShareKeep.
func TestShareLimits(t *testing.T) {
	f := newShareFixture(t)
	defer func(keep, max int) { ShareKeep, MaxShare = keep, max }(ShareKeep, MaxShare)
	ShareKeep = 2
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")

	forge := goldReplay(t, "forge_flagrun.sbr")
	if state, _ := a.share(forge, shareChunk); state != ShareDone {
		t.Fatalf("first share: state %d", state)
	}
	if state, _ := a.share(forge, shareChunk); state != ShareRefused {
		t.Fatal("a second share within ShareEvery was taken")
	}
	a.expectShareNotice("wait")

	MaxShare = 1000
	if state, _ := b.share(forge, shareChunk); state != ShareRefused {
		t.Fatal("an oversized share was taken")
	}
	b.expectShareNotice("can't be shared")
	MaxShare = 10 << 20

	broken := bytes.Clone(forge)
	broken[len(broken)/2] ^= 0xff
	if state, _ := b.share(broken, shareChunk); state != ShareRefused {
		t.Fatal("a corrupted replay was taken")
	}
	b.expectShareNotice("CRC mismatch")
	f.clock.add(ShareEvery)
	if state, _ := b.share([]byte("SBR2 but not really a replay"), shareChunk); state != ShareRefused {
		t.Fatal("garbage was taken")
	}
	b.expectShareNotice("isn't a replay")

	// a piece at the wrong offset ends the upload
	f.clock.add(ShareEvery)
	var w writer
	w.u(uint64(len(forge)))
	b.send(MsgShare, w.b)
	if state, _ := b.shareState(); state != ShareSend {
		t.Fatal("upload not started")
	}
	var p writer
	p.u(5)
	p.bytes(forge[:10])
	b.send(MsgShareData, p.b)
	if state, _ := b.shareState(); state != ShareRefused {
		t.Fatal("a piece at the wrong offset was taken")
	}

	f.clock.add(ShareEvery)
	akro := goldReplay(t, "akroseum_flagrun.sbr")
	_, akroID := b.share(akro, shareChunk)
	f.clock.add(ShareEvery)
	_, again := a.share(forge, shareChunk)
	rows := b.sharedList()
	if len(rows) != 2 || rows[0].id != int64(again) || rows[0].mapname != "forge" || rows[1].id != int64(akroID) {
		t.Fatalf("after sharing forge again: %+v", rows)
	}
	f.clock.add(ShareEvery)
	_, anubis := b.share(goldReplay(t, "anubis_dmsp.sbr"), shareChunk)
	rows = b.sharedList()
	if len(rows) != 2 || rows[0].id != int64(anubis) || rows[1].id != int64(again) {
		t.Fatalf("buffer of 2 after a third replay: %+v", rows)
	}
	if _, err := os.Stat(f.h.sharePath(int64(akroID))); !os.IsNotExist(err) {
		t.Fatal("the evicted replay's file is still there")
	}

	defer func(n int) { SharesPerHour = n }(SharesPerHour)
	SharesPerHour = 3
	f.clock.add(ShareEvery)
	if state, _ := b.share(forge, shareChunk); state != ShareRefused {
		t.Fatal("a fourth share in an hour was taken")
	}
	b.expectShareNotice("in the last hour")
}

// While a board entry's replay isn't public, only its runner may share it.
func TestShareLockedRun(t *testing.T) {
	f := newShareFixture(t)
	data := goldReplay(t, "forge_flagrun.sbr")
	_, content, err := checkReplay(data)
	if err != nil {
		t.Fatal(err)
	}
	runnerKey, otherKey := newKey(), newKey()
	dir := t.TempDir()
	wpath := filepath.Join(dir, "witness.db")
	w, err := sqlite.Create(wpath)
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	for _, q := range []string{
		"CREATE TABLE sessions(id INTEGER PRIMARY KEY, installkey TEXT, installpub TEXT)",
		"CREATE TABLE runs(id INTEGER PRIMARY KEY, installkey TEXT, map TEXT, player TEXT, runkind TEXT, mode INTEGER, runtimems INTEGER, verdict TEXT, judged INTEGER, file TEXT, content TEXT)",
	} {
		if _, err := w.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	w.Exec("INSERT INTO sessions(installkey, installpub) VALUES('k0', ?)", hex.EncodeToString(runnerKey.Public))
	w.Exec("INSERT INTO runs(installkey, map, player, runkind, mode, runtimems, verdict, judged, content) VALUES('k0', 'forge', 'alice', 'flagrun', 17, 9000, 'valid', ?, ?)", time.Now().UnixMilli(), hex.EncodeToString(content))
	store, err := board.Open(wpath, "", filepath.Join(dir, "site.db"), time.Minute)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if err := store.SetReplayDelay("flagrun", 48); err != nil {
		t.Fatal(err)
	}
	f.h.SetBoards(store)

	other := f.connect(otherKey, "1.1.1.1", "bob")
	if state, _ := other.share(data, shareChunk); state != ShareRefused {
		t.Fatal("someone else shared a locked board replay")
	}
	other.expectShareNotice("only its runner")
	runner := f.connect(runnerKey, "2.2.2.2", "alice")
	if state, _ := runner.share(data, shareChunk); state != ShareDone {
		t.Fatal("the runner couldn't share their own locked replay")
	}
}

func TestCheckReplay(t *testing.T) {
	for _, name := range []string{"forge_flagrun.sbr", "anubis_dmsp.sbr", "skrsp1_segmented.sbr", "crnsp1_sp.sbr"} {
		row, content, err := checkReplay(goldReplay(t, name))
		if err != nil || len(content) != 32 || row.mapname == "" || row.runner == "" {
			t.Errorf("%s: %+v %x %v", name, row, content, err)
		}
	}
	data := goldReplay(t, "forge_flagrun.sbr")
	for _, cut := range []int{3, 10, len(data) / 2, len(data) - 1} {
		if _, _, err := checkReplay(data[:cut]); err == nil {
			t.Errorf("cut at %d of %d: no error", cut, len(data))
		}
	}
}
