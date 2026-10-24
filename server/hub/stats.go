package hub

import "strconv"

// Lobby scoreboard: each member's run timer as its client reports it (RUNTIME), its best finish
// in the lobby so far and its witnessed PB on the lobby's map, sent to the lobby as LOBBYSTAT.
// A lobby best counts finishes since the member joined or the map last changed.

type runStat struct {
	running bool
	ms      uint64
	best    uint64 // lobby best ms + 1, 0: none
	pb      uint64 // witnessed PB ms + 1, 0: none or unknown
}

func statMsg(p *player) []byte {
	var w writer
	w.u(p.id)
	if p.stat.running {
		w.u(1)
	} else {
		w.u(0)
	}
	w.u(p.stat.ms)
	w.u(p.stat.best)
	w.u(p.stat.pb)
	return w.b
}

// Everything below runs with h.mu held.

func (h *Hub) sendStat(p *player) {
	if p.lobby == nil || p.watching {
		return
	}
	b := statMsg(p)
	for _, q := range p.lobby.everyone() {
		h.send(q, MsgLobbyStat, b)
	}
}

// statsTo sends every member's numbers to one player who just came into the lobby.
func (h *Hub) statsTo(p *player, l *lobby) {
	for _, m := range l.members {
		h.send(p, MsgLobbyStat, statMsg(m))
	}
}

// joinedStats starts p's lobby numbers over after it joined l, or after l's map changed.
func (h *Hub) joinedStats(p *player, l *lobby) {
	p.stat.best = 0
	p.stat.pb = h.lookupPB(p.key, l)
	h.sendStat(p)
}

func (h *Hub) lookupPB(key string, l *lobby) uint64 {
	if h.wdb == nil || l.mapname == "" {
		return 0
	}
	rows, err := h.wdb.Query("SELECT IFNULL(MIN(runtimems), -1) FROM bests WHERE map = ? AND runkind = ? AND mode = ? AND installkey IN (SELECT installkey FROM sessions WHERE installpub = ?)", l.mapname, l.kind, l.mode, key)
	if err != nil {
		h.cfg.Logf("pb lookup: %v", err)
		return 0
	}
	if len(rows) != 1 {
		return 0
	}
	ms, err := strconv.ParseInt(rows[0][0], 10, 64)
	if err != nil || ms < 0 {
		return 0
	}
	return uint64(ms) + 1
}

func (h *Hub) runTime(p *player, r *reader) error {
	running, ms := r.u(), r.u()
	if err := r.done(); err != nil {
		return err
	}
	if running > 1 {
		return errShort
	}
	p.stat.running, p.stat.ms = running == 1, ms
	h.sendStat(p)
	return nil
}

// finished keeps a finish on the lobby's map as the member's lobby best when it beats it.
func (h *Hub) finished(p *player, mapname string, mode int64, kind string, ms uint64) {
	l := p.lobby
	if l == nil || p.watching || mapname != l.mapname || mode != l.mode || kind != l.kind {
		return
	}
	if p.stat.best == 0 || ms+1 < p.stat.best {
		p.stat.best = ms + 1
		h.sendStat(p)
	}
}

// newPB updates the PB of the player with that install key when it is for its lobby's map.
func (h *Hub) newPB(key, mapname, kind string, mode int64, ms uint64) {
	p := h.byKey[key]
	if p == nil || p.lobby == nil || p.watching {
		return
	}
	l := p.lobby
	if mapname != l.mapname || mode != l.mode || kind != l.kind {
		return
	}
	if p.stat.pb == 0 || ms+1 < p.stat.pb {
		p.stat.pb = ms + 1
		h.sendStat(p)
	}
}
