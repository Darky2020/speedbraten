package hub

import (
	"os"

	"speedbraten/server/board"
)

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
		h.send(p, MsgBoardRows, boardMsg(k, nil, 0, 0, nil))
		return nil
	}
	p.boardBusy = true
	key := p.key
	go func() {
		var rows []board.Entry
		var rank int
		var ms int64
		legacy, err := store.LegacyRecord(k)
		if err != nil {
			h.cfg.Logf("legacy record %v: %v", k, err)
		}
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
		msg := boardMsg(k, rows, rank, ms, legacy)
		h.mu.Lock()
		p.boardBusy = false
		if !p.closed {
			h.send(p, MsgBoardRows, msg)
		}
		h.mu.Unlock()
	}()
	return nil
}

func boardMsg(k board.Key, rows []board.Entry, rank int, ms int64, legacy *board.Legacy) []byte {
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
	for _, e := range rows {
		w.u(uint64(e.Run))
	}
	if legacy == nil {
		w.u(0)
	} else {
		w.u(1)
		w.str(legacy.Player)
		w.u(uint64(legacy.MS))
		w.str(legacy.Source)
	}
	return w.b
}

const (
	// MaxReplay is the largest replay file the hub sends.
	MaxReplay   = 4 << 20
	replayChunk = 32 << 10
)

// replayRequest sends the replay of a run on a board, read off the hub's lock; one request per
// player at a time. Called with h.mu held.
func (h *Hub) replayRequest(p *player, r *reader) error {
	run := int64(r.u())
	if err := r.done(); err != nil {
		return err
	}
	if p.replayBusy {
		return nil
	}
	store := h.boards
	if store == nil {
		h.notice(p, "this hub has no boards")
		h.send(p, MsgReplayData, replayMsg(run, 0, 0, nil))
		return nil
	}
	p.replayBusy = true
	go func() {
		data, why := boardReplay(store, run)
		h.mu.Lock()
		defer h.mu.Unlock()
		p.replayBusy = false
		if p.closed {
			return
		}
		if why != "" {
			h.notice(p, why)
			h.send(p, MsgReplayData, replayMsg(run, 0, 0, nil))
			return
		}
		for off := 0; off < len(data); off += replayChunk {
			h.send(p, MsgReplayData, replayMsg(run, len(data), off, data[off:min(off+replayChunk, len(data))]))
		}
	}()
	return nil
}

// boardReplay reads the replay of a run that is an entry on a board, or says why it can't.
func boardReplay(store *board.Store, run int64) ([]byte, string) {
	if ok, err := store.Listed(run); err != nil || !ok {
		return nil, "that run is not on a board"
	}
	path, err := store.File(run)
	if err != nil {
		return nil, "that run's replay is not kept"
	}
	if st, err := os.Stat(path); err != nil || st.Size() == 0 || st.Size() > MaxReplay {
		return nil, "that run's replay can't be sent"
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, "that run's replay can't be sent"
	}
	return data, ""
}

func replayMsg(run int64, size, offset int, chunk []byte) []byte {
	var w writer
	w.u(uint64(run))
	w.u(uint64(size))
	w.u(uint64(offset))
	w.bytes(chunk)
	return w.b
}
