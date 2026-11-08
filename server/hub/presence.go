package hub

import (
	"encoding/json"
	"sort"
	"strings"
	"time"
)

// PresenceRefresh is how often the hub's presence is written while it hasn't changed, so the
// site can tell a stopped hub from an empty one.
var PresenceRefresh = time.Minute

// PresenceDetail is one named player on the hub as the leaderboard site's API gives it.
type PresenceDetail struct {
	Name       string `json:"name"`
	State      string `json:"state"` // menu, playing, run
	Map        string `json:"map"`
	Mode       int64  `json:"mode"`
	Kind       string `json:"kind"`
	Lobby      uint64 `json:"lobby"` // 0: in none
	Host       string `json:"host"`  // the lobby's host
	Spectating string `json:"spectating"`
	Away       string `json:"away"` // "", dnd or afk
}

var stateNames = map[int]string{StateMenu: "menu", StatePlaying: "playing", StateRun: "run"}

// PresenceTick writes the named players on the hub and what they are doing to hub.db when that
// changes, and at least every PresenceRefresh; the leaderboard site shows how many are online and
// serves the detail through its API.
func (h *Hub) PresenceTick() {
	h.mu.Lock()
	var players []PresenceDetail
	for _, p := range h.players {
		if p.name == "" {
			continue
		}
		d := PresenceDetail{Name: p.name, State: stateNames[p.state], Map: p.mapname, Mode: p.mode, Kind: p.kind}
		if p.lobby != nil {
			d.Lobby, d.Host = p.lobby.id, p.lobby.host.name
		}
		if p.spec != nil {
			d.Spectating = p.spec.target.name
		}
		switch {
		case p.away&AwayDND != 0:
			d.Away = "dnd"
		case p.away&AwayAFK != 0:
			d.Away = "afk"
		}
		players = append(players, d)
	}
	sort.Slice(players, func(i, j int) bool { return players[i].Name < players[j].Name })
	names := make([]string, len(players))
	for i, d := range players {
		names[i] = d.Name
	}
	b, _ := json.Marshal(players)
	if players == nil {
		b = []byte("[]")
	}
	detail := string(b)
	now := h.cfg.Now()
	if detail == h.presence && now.Sub(h.presenceAt) < PresenceRefresh {
		h.mu.Unlock()
		return
	}
	h.presence, h.presenceAt = detail, now
	h.mu.Unlock()
	if err := h.st.setPresence(len(names), strings.Join(names, ", "), detail, now.Unix()); err != nil {
		h.cfg.Logf("presence: %v", err)
	}
}
