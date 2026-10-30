package hub

import (
	"strings"
	"sync"
	"testing"
	"time"

	"speedbraten/server/board"
)

type fakeFeed struct {
	mu     sync.Mutex
	posts  []string
	status []string
}

func (f *fakeFeed) Post(text string)   { f.mu.Lock(); f.posts = append(f.posts, text); f.mu.Unlock() }
func (f *fakeFeed) Status(text string) { f.mu.Lock(); f.status = append(f.status, text); f.mu.Unlock() }
func (f *fakeFeed) take() ([]string, []string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	p, s := f.posts, f.status
	f.posts, f.status = nil, nil
	return p, s
}

func newFeedFixture(t *testing.T) (*fixture, *fakeFeed) {
	f := newFixture(t)
	feed := &fakeFeed{}
	f.h.cfg.Feed = feed
	return f, feed
}

// waitGone waits until the hub has dropped the player with that name
func (f *fixture) waitGone(name string) {
	f.t.Helper()
	for i := 0; i < 200; i++ {
		f.h.mu.Lock()
		online := false
		for _, p := range f.h.players {
			if p.name == name {
				online = true
			}
		}
		f.h.mu.Unlock()
		if !online {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	f.t.Fatalf("%s is still online", name)
}

func TestFeedJoinLeave(t *testing.T) {
	f, feed := newFeedFixture(t)
	alice := newKey()
	a := f.connect(alice, "1.1.1.1", "alice")
	if posts, _ := feed.take(); len(posts) != 1 || posts[0] != "👋 alice joined the hub" {
		t.Fatalf("join: %q", posts)
	}

	// a reconnect within FeedRejoin is neither a leave nor a join
	a.conn.Close()
	f.waitGone("alice")
	f.clock.add(30 * time.Second)
	f.h.FeedTick()
	a = f.connect(alice, "1.1.1.1", "alice")
	f.clock.add(2 * time.Minute)
	f.h.FeedTick()
	if posts, _ := feed.take(); len(posts) != 0 {
		t.Fatalf("reconnect posted %q", posts)
	}

	// gone for longer is a leave, posted once
	a.conn.Close()
	f.waitGone("alice")
	f.clock.add(30 * time.Second)
	f.h.FeedTick()
	if posts, _ := feed.take(); len(posts) != 0 {
		t.Fatalf("leave posted before FeedRejoin: %q", posts)
	}
	f.clock.add(31 * time.Second)
	f.h.FeedTick()
	f.h.FeedTick()
	if posts, _ := feed.take(); len(posts) != 1 || posts[0] != "🚪 alice left the hub" {
		t.Fatalf("leave: %q", posts)
	}

	// a player who never got a name is never announced
	f.connect(newKey(), "2.2.2.2", "")
	if posts, _ := feed.take(); len(posts) != 0 {
		t.Fatalf("nameless player posted %q", posts)
	}
}

func TestFeedStatus(t *testing.T) {
	f, feed := newFeedFixture(t)
	a := f.connect(newKey(), "1.1.1.1", "alice")
	b := f.connect(newKey(), "2.2.2.2", "bob")
	feed.take()
	f.h.FeedTick()
	if _, status := feed.take(); len(status) != 0 {
		t.Fatalf("status without a lobby: %q", status)
	}

	a.mapmsg(MsgLobbyCreate, "forge", 11)
	l := b.expectLobby()
	var w writer
	w.u(l.id)
	b.send(MsgLobbyJoin, w.b)
	b.waitLobby(func(l lobbyinfo) bool { return len(l.members) == 2 })
	f.h.FeedTick()
	_, status := feed.take()
	if len(status) != 1 || !strings.HasPrefix(status[0], "🎮 **2 players in 1 lobby**") ||
		!strings.Contains(status[0], "\n🚩 forge (flagrun, ctf): 👑 alice, bob") {
		t.Fatalf("status: %q", status)
	}

	// kept up to date every FeedStatusEvery, not more often
	f.clock.add(time.Minute)
	f.h.FeedTick()
	if _, status := feed.take(); len(status) != 0 {
		t.Fatalf("status again after a minute: %q", status)
	}
	f.clock.add(FeedStatusEvery)
	f.h.FeedTick()
	if _, status := feed.take(); len(status) != 1 {
		t.Fatalf("status after FeedStatusEvery: %q", status)
	}

	// once the last lobby is gone the status ends, once
	a.send(MsgLobbyLeave, nil)
	b.send(MsgLobbyLeave, nil)
	for i := 0; i < 200; i++ {
		f.h.mu.Lock()
		n := len(f.h.lobbies)
		f.h.mu.Unlock()
		if n == 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	f.h.FeedTick()
	f.h.FeedTick()
	if _, status := feed.take(); len(status) != 1 || status[0] != "" {
		t.Fatalf("status after the lobbies: %q", status)
	}
}

func TestFeedRecords(t *testing.T) {
	f, feed := newFeedFixture(t)
	k := board.Key{Map: "dust2", Kind: "flagrun", Mode: 11}
	f.h.feedRecord("alice", k, 6130, "", -1, nil)
	f.h.feedRecord("bob", k, 6100, "alice", 6130, nil)
	f.h.feedRecord("bob", k, 6050, "bob", 6100, &board.Legacy{Player: "Buck", MS: 6070})
	f.h.feedRecord("m_r*x", board.Key{Map: "mpsp6a", Kind: "campaign", Mode: -3}, 133090, "", -1, nil)
	posts, _ := feed.take()
	want := []string{
		"🏆 alice set a new record on dust2 (flagrun, ctf): **0:06.130**",
		"🏆 bob set a new record on dust2 (flagrun, ctf): **0:06.100**, beating alice's 0:06.130",
		"🏆📜 bob set a new record on dust2 (flagrun, ctf): **0:06.050**, improving their own 0:06.100, and beat the legacy record of Buck (0:06.070)",
		`🏆 m\_r\*x set a new record on An Army Of One (campaign): **2:13.090**`,
	}
	if len(posts) != len(want) {
		t.Fatalf("posts %q", posts)
	}
	for i := range want {
		if posts[i] != want[i] {
			t.Errorf("post %d:\n got %q\nwant %q", i, posts[i], want[i])
		}
	}

	// a PB that isn't a record isn't posted
	f.h.announce("", "carol", "dust2", "flagrun", 11, 7000, false, -1)
	if posts, _ := feed.take(); len(posts) != 0 {
		t.Fatalf("a PB was posted: %q", posts)
	}
	f.h.announce("", "carol", "dust2", "flagrun", 11, 6000, true, 6050)
	if posts, _ := feed.take(); len(posts) != 1 || !strings.HasPrefix(posts[0], "🏆 carol set a new record on dust2") {
		t.Fatalf("record from the witness: %q", posts)
	}
}
