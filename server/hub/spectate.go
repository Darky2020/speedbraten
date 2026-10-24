package hub

// Spectating: a player's client streams the game it plays to the hub as views (VIEW, then
// VIEWDATA): replay files without a trailer, each starting at a map start or from a saved
// state. The hub keeps each player's current view in memory and passes it on: all of it to a
// new spectator, then every new piece as it arrives.

const (
	MaxSpectators = 16
	MaxView       = 4 << 20 // bytes of one view the hub keeps
	specChunk     = 16 << 10
)

type liveView struct {
	id      uint64
	mapname string
	mode    int64
	kind    string
	data    []byte
}

type spectate struct {
	target *player
	view   uint64 // the view being sent, 0 before the first
}

// Everything below runs with h.mu held.

func (h *Hub) specRun(p, q *player) {
	v := q.view
	p.spec.view = v.id
	var w writer
	w.u(q.id)
	w.u(v.id)
	w.str(v.mapname)
	w.s(v.mode)
	w.str(v.kind)
	w.u(0)
	h.send(p, MsgSpecRun, w.b)
}

func (h *Hub) specData(p *player, id uint64, offset int, b []byte) {
	for off := 0; off < len(b); off += specChunk {
		end := min(off+specChunk, len(b))
		var w writer
		w.u(id)
		w.u(uint64(offset + off))
		w.bytes(b[off:end])
		h.send(p, MsgSpecData, w.b)
	}
}

func (h *Hub) spectatorsOf(q *player) []*player {
	var ps []*player
	for _, p := range h.players {
		if p.spec != nil && p.spec.target == q {
			ps = append(ps, p)
		}
	}
	return ps
}

func (h *Hub) startSpectating(p *player, id uint64) {
	h.stopWatching(p)
	if id == 0 {
		return
	}
	q := h.players[id]
	switch {
	case q == nil || q.name == "":
		h.notice(p, "that player is gone")
	case q == p:
		h.notice(p, "you can't spectate yourself")
	case len(h.spectatorsOf(q)) >= MaxSpectators:
		h.notice(p, q.name+" has too many spectators")
	default:
		p.spec = &spectate{target: q}
		h.watch(p)
		if q.view != nil {
			h.specRun(p, q)
			h.specData(p, q.view.id, 0, q.view.data)
		}
	}
}

// endSpectators tells everyone watching q that it is over.
func (h *Hub) endSpectators(q *player, why string) {
	for _, p := range h.spectatorsOf(q) {
		var w writer
		w.u(q.id)
		w.str(why)
		h.send(p, MsgSpecEnd, w.b)
		h.stopWatching(p)
	}
}

// watch puts a named spectator in its target's lobby, or in none while the target has none.
func (h *Hub) watch(p *player) {
	h.leaveLobby(p)
	if l := p.spec.target.lobby; l != nil && p.name != "" {
		l.specs = append(l.specs, p)
		p.lobby, p.watching = l, true
		h.broadcast(MsgLobby, lobbyMsg(l))
		h.statsTo(p, l)
	}
	h.broadcast(MsgPlayer, h.playerMsg(p))
}

// stopWatching ends p's spectating and takes it out of the lobby it watched from.
func (h *Hub) stopWatching(p *player) {
	p.spec = nil
	if p.watching {
		h.leaveLobby(p)
		h.broadcast(MsgPlayer, h.playerMsg(p))
	}
}

// bringSpectators moves q's spectators along to q's lobby after q changed lobbies.
func (h *Hub) bringSpectators(q *player) {
	for _, s := range h.spectatorsOf(q) {
		h.watch(s)
	}
}

func (h *Hub) viewStart(p *player, r *reader) error {
	v := &liveView{id: r.u(), mapname: r.str(64), mode: r.s(), kind: r.str(16)}
	if err := r.done(); err != nil {
		return err
	}
	p.view = v
	for _, s := range h.spectatorsOf(p) {
		h.specRun(s, p)
	}
	return nil
}

func (h *Hub) viewData(p *player, r *reader) error {
	id, off := r.u(), r.u()
	if r.err != nil {
		return r.err
	}
	v := p.view
	if v == nil || id != v.id || off != uint64(len(v.data)) {
		return nil
	}
	if len(v.data)+len(r.b) > h.maxView {
		p.view = nil
		return nil
	}
	v.data = append(v.data, r.b...)
	for _, s := range h.spectatorsOf(p) {
		if s.spec.view == id {
			h.specData(s, id, int(off), r.b)
		}
	}
	return nil
}
