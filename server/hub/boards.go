package hub

import (
	"os"
	"time"

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
		h.send(p, MsgBoardRows, boardMsg(k, nil, 0, 0, nil, 0))
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
		var files uint64
		if legacy != nil {
			files = h.legacyFiles(k)
		}
		msg := boardMsg(k, rows, rank, ms, legacy, files)
		h.mu.Lock()
		p.boardBusy = false
		if !p.closed {
			h.send(p, MsgBoardRows, msg)
		}
		h.mu.Unlock()
	}()
	return nil
}

// files: the legacy record's files (legacyFiles)
// boardSummaryRequest sends a line per board for the map browser's filters: its record, the asking
// player's rank and its legacy record, boards with only a legacy record included. Read off the
// hub's lock, sharing the BOARD requests' one-at-a-time limit. Called with h.mu held.
func (h *Hub) boardSummaryRequest(p *player, r *reader) error {
	if err := r.done(); err != nil {
		return err
	}
	store := h.boards
	if store == nil || p.boardBusy {
		return nil
	}
	p.boardBusy = true
	key := p.key
	go func() {
		msg := boardSummaryMsg(store, store.InstallOfPub(key), h.cfg.Logf)
		h.mu.Lock()
		defer h.mu.Unlock()
		p.boardBusy = false
		if !p.closed {
			h.send(p, MsgBoardSummaryData, msg)
		}
	}()
	return nil
}

type boardSummary struct {
	recordMS, rank, legacyMS int64
}

func boardSummaryMsg(store *board.Store, mine string, logf func(string, ...any)) []byte {
	sums := map[board.Key]*boardSummary{}
	boards, err := store.Boards()
	if err != nil {
		logf("board summary: %v", err)
	}
	for _, b := range boards {
		s := &boardSummary{}
		if len(b.Entries) > 0 {
			s.recordMS = b.Entries[0].MS
		}
		for _, e := range b.Entries {
			if mine != "" && e.Install == mine {
				s.rank = int64(e.Rank)
				break
			}
		}
		sums[b.Key] = s
	}
	legacy, err := store.LegacyRecords()
	if err != nil {
		logf("board summary: %v", err)
	}
	for _, l := range legacy {
		s := sums[l.Key]
		if s == nil {
			s = &boardSummary{}
			sums[l.Key] = s
		}
		s.legacyMS = l.MS
	}
	var w writer
	w.u(uint64(len(sums)))
	for k, s := range sums {
		w.str(k.Map)
		w.str(k.Kind)
		w.s(int64(k.Mode))
		w.u(uint64(s.recordMS))
		w.u(uint64(s.rank))
		w.u(uint64(s.legacyMS))
	}
	return w.b
}

func boardMsg(k board.Key, rows []board.Entry, rank int, ms int64, legacy *board.Legacy, files uint64) []byte {
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
		w.u(files)
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
	key := p.key
	go func() {
		data, why := boardReplay(store, run, store.InstallOfPub(key))
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
// viewer: the asking player's install.
func boardReplay(store *board.Store, run int64, viewer string) ([]byte, string) {
	until, ok, err := store.ReplayLock(run, viewer)
	if err != nil || !ok {
		return nil, "that run is not on a board"
	}
	if !until.IsZero() {
		return nil, "that run's replay is public in " + board.Wait(time.Until(until)) + " (" + until.UTC().Format("2006-01-02 15:04") + " UTC)"
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
