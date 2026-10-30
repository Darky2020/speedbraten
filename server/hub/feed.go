package hub

import (
	"fmt"
	"sort"
	"strings"
	"time"

	"speedbraten/server/board"
)

// Feed takes what the hub tells the outside world, such as a Discord channel: lines about players
// and records, and a status of the lobbies that is kept up to date. Its methods are called in order,
// sometimes with the hub's lock held, so they must not block.
type Feed interface {
	Post(text string)
	// Status is the lobbies now, "" when there is no lobby
	Status(text string)
}

var (
	// a player who comes back within FeedRejoin of leaving was never gone, as far as the feed says
	FeedRejoin = time.Minute
	// how often the lobby status is sent while there is a lobby
	FeedStatusEvery = 5 * time.Minute
)

type feedState struct {
	gone       map[string]feedGone // install key -> a named player who left and may be back soon
	nextStatus time.Time
	statusOn   bool
}

type feedGone struct {
	name string
	at   time.Time
}

// feedJoined: p has a name for the first time this connection. Runs with h.mu held.
func (h *Hub) feedJoined(p *player) {
	if h.cfg.Feed == nil || p.name == "" || p.fed {
		return
	}
	p.fed = true
	if _, back := h.feed.gone[p.key]; back {
		delete(h.feed.gone, p.key)
		return
	}
	h.feedPost(feedName(p.name) + " joined the hub")
}

// feedLeft: a named player left. Runs with h.mu held.
func (h *Hub) feedLeft(p *player) {
	if h.cfg.Feed == nil || !p.fed {
		return
	}
	if h.feed.gone == nil {
		h.feed.gone = map[string]feedGone{}
	}
	h.feed.gone[p.key] = feedGone{p.name, h.cfg.Now()}
}

// FeedTick posts the leaves that weren't followed by a return and, every FeedStatusEvery while there
// is a lobby, the lobby status. Called every few seconds.
func (h *Hub) FeedTick() {
	if h.cfg.Feed == nil {
		return
	}
	h.mu.Lock()
	now := h.cfg.Now()
	var left []string
	for key, g := range h.feed.gone {
		if now.Sub(g.at) >= FeedRejoin {
			left = append(left, g.name)
			delete(h.feed.gone, key)
		}
	}
	status, send := "", false
	if len(h.lobbies) > 0 {
		if !h.feed.statusOn || !now.Before(h.feed.nextStatus) {
			status, send = h.lobbyStatus(), true
			h.feed.statusOn = true
			h.feed.nextStatus = now.Add(FeedStatusEvery)
		}
	} else if h.feed.statusOn {
		send = true
		h.feed.statusOn = false
	}
	sort.Strings(left)
	for _, name := range left {
		h.cfg.Feed.Post(feedName(name) + " left the hub")
	}
	if send {
		h.cfg.Feed.Status(status)
	}
	h.mu.Unlock()
}

func (h *Hub) feedPost(text string) { h.cfg.Feed.Post(text) }

// lobbyStatus is one line per lobby: its map and category, its members (the host first) and who
// watches. Runs with h.mu held.
func (h *Hub) lobbyStatus() string {
	var ls []*lobby
	for _, l := range h.lobbies {
		ls = append(ls, l)
	}
	sort.Slice(ls, func(i, j int) bool { return ls[i].id < ls[j].id })
	var b strings.Builder
	players := 0
	for _, l := range ls {
		players += len(l.members)
	}
	fmt.Fprintf(&b, "**%d %s in %d %s**", players, plural(players, "player", "players"), len(ls), plural(len(ls), "lobby", "lobbies"))
	for _, l := range ls {
		var names []string
		for _, m := range l.members {
			n := feedName(m.name)
			if m == l.host {
				n += " (host)"
			}
			names = append(names, n)
		}
		k := board.Key{Map: l.mapname, Kind: l.kind, Mode: int(l.mode)}
		fmt.Fprintf(&b, "\n%s (%s): %s", feedName(k.MapLabel()), k.KindLabel(), strings.Join(names, ", "))
		if len(l.specs) > 0 {
			var specs []string
			for _, s := range l.specs {
				specs = append(specs, feedName(s.name))
			}
			fmt.Fprintf(&b, "; watching: %s", strings.Join(specs, ", "))
		}
	}
	return b.String()
}

// feedRecord: a new record. prev is whose record it took ("" for the board's first time, the
// player's own name when they improved it), legacy the older leaderboard's record it went under.
func (h *Hub) feedRecord(name string, k board.Key, ms uint64, prev string, previous int64, legacy *board.Legacy) {
	if h.cfg.Feed == nil {
		return
	}
	text := fmt.Sprintf("%s set a new record on %s (%s): **%s**", feedName(name), feedName(k.MapLabel()), k.KindLabel(), board.Time(int64(ms)))
	switch {
	case previous < 0:
	case prev == name:
		text += fmt.Sprintf(", improving their own %s", board.Time(previous))
	case prev != "":
		text += fmt.Sprintf(", beating %s's %s", feedName(prev), board.Time(previous))
	}
	if legacy != nil {
		text += fmt.Sprintf(", and beat the legacy record of %s (%s)", feedName(legacy.Player), board.Time(legacy.MS))
	}
	h.feedPost(text)
}

func plural(n int, one, many string) string {
	if n == 1 {
		return one
	}
	return many
}

// feedName keeps a player's name from being read as Discord markdown or a mention.
func feedName(s string) string {
	var b strings.Builder
	for _, r := range s {
		if strings.ContainsRune("\\*_~`|>#[]()-@:<", r) {
			b.WriteByte('\\')
		}
		b.WriteRune(r)
	}
	return b.String()
}
