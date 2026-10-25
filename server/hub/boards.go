package hub

import "speedbraten/server/board"

// BoardRows is how many entries of a board the hub sends a client.
const BoardRows = 10

// SetBoards lets the hub answer BOARD requests from the leaderboards.
func (h *Hub) SetBoards(s *board.Store) {
	h.mu.Lock()
	h.boards = s
	h.mu.Unlock()
}

// boardRequest answers off the hub's lock, since building the boards reads the witness's
// database; one request per player at a time. Called with h.mu held.
func (h *Hub) boardRequest(p *player, r *reader) error {
	k := board.Key{Map: r.str(64)}
	mode, kind := r.s(), r.str(16)
	if err := r.done(); err != nil {
		return err
	}
	k.Mode, k.Kind = int(mode), kind
	if p.boardBusy {
		return nil
	}
	store := h.boards
	if store == nil {
		h.send(p, MsgBoardRows, boardMsg(k, nil, 0, 0))
		return nil
	}
	p.boardBusy = true
	key := p.key
	go func() {
		var rows []board.Entry
		var rank int
		var ms int64
		if b, err := store.Board(k); err != nil {
			h.cfg.Logf("board %v: %v", k, err)
		} else if b != nil {
			rows = b.Entries
			mine := store.InstallOfPub(key)
			for _, e := range b.Entries {
				if mine != "" && e.Install == mine {
					rank, ms = e.Rank, e.MS
					break
				}
			}
		}
		if len(rows) > BoardRows {
			rows = rows[:BoardRows]
		}
		msg := boardMsg(k, rows, rank, ms)
		h.mu.Lock()
		p.boardBusy = false
		if !p.closed {
			h.send(p, MsgBoardRows, msg)
		}
		h.mu.Unlock()
	}()
	return nil
}

func boardMsg(k board.Key, rows []board.Entry, rank int, ms int64) []byte {
	var w writer
	w.str(k.Map)
	w.s(int64(k.Mode))
	w.str(k.Kind)
	w.u(uint64(len(rows)))
	for _, e := range rows {
		w.u(uint64(e.Rank))
		w.str(e.Name)
		w.u(uint64(e.MS))
	}
	w.u(uint64(rank))
	w.u(uint64(ms))
	return w.b
}
