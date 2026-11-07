package hub

import (
	"sort"
	"strings"
	"time"
)

// PresenceRefresh is how often the hub's presence is written while it hasn't changed, so the
// site can tell a stopped hub from an empty one.
var PresenceRefresh = time.Minute

// PresenceTick writes the named players on the hub to hub.db when they change, and at least every
// PresenceRefresh; the leaderboard site shows how many are online from it.
func (h *Hub) PresenceTick() {
	h.mu.Lock()
	var names []string
	for _, p := range h.players {
		if p.name != "" {
			names = append(names, p.name)
		}
	}
	now := h.cfg.Now()
	sort.Strings(names)
	list := strings.Join(names, ", ")
	if list == h.presence && now.Sub(h.presenceAt) < PresenceRefresh {
		h.mu.Unlock()
		return
	}
	h.presence, h.presenceAt = list, now
	h.mu.Unlock()
	if err := h.st.setPresence(len(names), list, now.Unix()); err != nil {
		h.cfg.Logf("presence: %v", err)
	}
}
