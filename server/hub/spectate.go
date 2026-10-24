package hub

import (
	"encoding/binary"
	"io"
	"os"
	"strconv"
	"time"
)

// Spectating: the hub follows the replay file the witness writes for the spectated player's
// newest run and passes on its whole segments (fpsgame/replayio.h): everything up to the end
// of the last SEGE chunk. The trailer after the last segment, with the client's tells, is
// never sent.

const (
	MaxSpectators = 16
	specChunk     = 16 << 10
)

// segmentsEnd returns how many leading bytes of an SBR2 file are whole segments; 0 if none.
func segmentsEnd(b []byte) int {
	if len(b) < 5 || string(b[:4]) != "SBR2" {
		return 0
	}
	_, n := binary.Uvarint(b[4:])
	if n <= 0 {
		return 0
	}
	pos, cut := 4+n, 0
	for pos+4 <= len(b) {
		typ := string(b[pos : pos+4])
		_, n1 := binary.Uvarint(b[pos+4:])
		if n1 <= 0 {
			break
		}
		size, n2 := binary.Uvarint(b[pos+4+n1:])
		if n2 <= 0 {
			break
		}
		end := pos + 4 + n1 + n2 + int(size) + 4
		if size > uint64(len(b)) || end > len(b) {
			break
		}
		if typ == "END " {
			break
		}
		pos = end
		if typ == "SEGE" {
			cut = pos
		}
	}
	return cut
}

// specRun is the run a spectated player is on and how much of it can be passed on.
type specRun struct {
	row     int64
	file    string
	mapname string
	mode    int64
	kind    string
	chained bool
	cut     int
}

// spectating state of one spectator
type spectate struct {
	target *player
	row    int64 // the run being sent, 0 before the first
	sent   int
}

func (h *Hub) startSpectating(p *player, id uint64) {
	p.spec = nil
	if id == 0 {
		return
	}
	q := h.players[id]
	switch {
	case h.wdb == nil:
		h.notice(p, "spectating needs the witness")
	case q == nil || q.name == "":
		h.notice(p, "that player is gone")
	case q == p:
		h.notice(p, "you can't spectate yourself")
	case h.spectatorsOf(q) >= MaxSpectators:
		h.notice(p, q.name+" has too many spectators")
	default:
		p.spec = &spectate{target: q}
	}
}

func (h *Hub) spectatorsOf(q *player) int {
	n := 0
	for _, p := range h.players {
		if p.spec != nil && p.spec.target == q {
			n++
		}
	}
	return n
}

// liveRun reads the newest witnessed run of an install from witness.db and how much of its
// file is whole segments.
func (h *Hub) liveRun(installpub string) (*specRun, error) {
	rows, err := h.wdb.Query("SELECT r.id, r.file, IFNULL(r.map, ''), IFNULL(r.mode, 0), IFNULL(r.runkind, ''), r.kind FROM runs r JOIN sessions s ON s.id = r.session WHERE s.installpub = ? ORDER BY r.id DESC LIMIT 1", installpub)
	if err != nil || len(rows) == 0 {
		return nil, err
	}
	row, _ := strconv.ParseInt(rows[0][0], 10, 64)
	mode, _ := strconv.ParseInt(rows[0][3], 10, 64)
	r := &specRun{row: row, file: rows[0][1], mapname: rows[0][2], mode: mode, kind: rows[0][4], chained: rows[0][5] == "2"}
	b, err := os.ReadFile(r.file)
	if err != nil {
		return nil, err
	}
	r.cut = segmentsEnd(b)
	return r, nil
}

// SpectateSlice sends every spectator the segments of their player's run they don't have yet.
func (h *Hub) SpectateSlice() {
	h.mu.Lock()
	targets := map[*player]string{}
	for _, p := range h.players {
		if p.spec != nil {
			if h.players[p.spec.target.id] != p.spec.target {
				var w writer
				w.u(p.spec.target.id)
				w.str(p.spec.target.name + " left")
				h.send(p, MsgSpecEnd, w.b)
				p.spec = nil
				continue
			}
			targets[p.spec.target] = p.spec.target.key
		}
	}
	h.mu.Unlock()
	runs := map[*player]*specRun{}
	for q, key := range targets {
		if r, err := h.liveRun(key); err == nil && r != nil {
			runs[q] = r
		}
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, p := range h.players {
		if p.spec == nil {
			continue
		}
		r := runs[p.spec.target]
		if r == nil || r.cut == 0 {
			continue
		}
		s := p.spec
		if s.row != r.row {
			s.row, s.sent = r.row, 0
			var w writer
			w.u(s.target.id)
			w.u(uint64(r.row))
			w.str(r.mapname)
			w.s(r.mode)
			w.str(r.kind)
			if r.chained {
				w.u(1)
			} else {
				w.u(0)
			}
			h.send(p, MsgSpecRun, w.b)
		}
		if s.sent >= r.cut {
			continue
		}
		f, err := os.Open(r.file)
		if err != nil {
			continue
		}
		buf := make([]byte, r.cut-s.sent)
		_, err = f.ReadAt(buf, int64(s.sent))
		f.Close()
		if err != nil && err != io.EOF {
			continue
		}
		for off := 0; off < len(buf); off += specChunk {
			end := min(off+specChunk, len(buf))
			var w writer
			w.u(uint64(r.row))
			w.u(uint64(s.sent + off))
			w.bytes(buf[off:end])
			h.send(p, MsgSpecData, w.b)
		}
		s.sent = r.cut
	}
}

// SpectateLoop runs SpectateSlice every interval.
func (h *Hub) SpectateLoop(interval time.Duration) {
	for {
		h.SpectateSlice()
		time.Sleep(interval)
	}
}
