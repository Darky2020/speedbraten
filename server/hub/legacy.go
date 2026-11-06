package hub

import (
	"fmt"
	"os"
	"path/filepath"

	"speedbraten/server/board"
)

// A flagrun board's legacy record can come with files made from the record's demo by
// tools/legacyflagruns.py --out, kept in Config.Legacy: <map>-<mode>.sbg, the record holder's
// path as a ghost, and <map>-<mode>.dmo, the demo cut down to the run.
const (
	LegacyGhost = 0
	LegacyDemo  = 1
)

var legacyExt = [...]string{".sbg", ".dmo"}

// MaxLegacyFile is the largest legacy file the hub sends.
const MaxLegacyFile = 1 << 20

func (h *Hub) legacyPath(k board.Key, what int) string {
	if h.cfg.Legacy == "" || k.Kind != "flagrun" || what < 0 || what >= len(legacyExt) || !validMapName(k.Map) {
		return ""
	}
	return filepath.Join(h.cfg.Legacy, fmt.Sprintf("%s-%d%s", k.Map, k.Mode, legacyExt[what]))
}

// legacyFiles is a bit per legacy file the board has, 1<<LegacyGhost and 1<<LegacyDemo.
func (h *Hub) legacyFiles(k board.Key) uint64 {
	var bits uint64
	for what := range legacyExt {
		if p := h.legacyPath(k, what); p != "" {
			if fi, err := os.Stat(p); err == nil && fi.Mode().IsRegular() {
				bits |= 1 << what
			}
		}
	}
	return bits
}

// legacyRequest sends one of a board's legacy files, read off the hub's lock; one download per
// player at a time, shared with board replays. Called with h.mu held.
func (h *Hub) legacyRequest(p *player, r *reader) error {
	k := board.Key{Map: r.str(64)}
	mode, kind, what := r.s(), r.str(16), int(r.u())
	if err := r.done(); err != nil {
		return err
	}
	k.Mode, k.Kind = int(mode), kind
	if p.replayBusy {
		return nil
	}
	path := h.legacyPath(k, what)
	if path == "" {
		h.notice(p, "this board has no legacy files")
		h.send(p, MsgLegacyData, legacyMsg(k, what, 0, 0, nil))
		return nil
	}
	p.replayBusy = true
	go func() {
		data, err := os.ReadFile(path)
		h.mu.Lock()
		defer h.mu.Unlock()
		p.replayBusy = false
		if p.closed {
			return
		}
		if err != nil || len(data) == 0 || len(data) > MaxLegacyFile {
			if err != nil && !os.IsNotExist(err) {
				h.cfg.Logf("legacy file %s: %v", path, err)
			}
			h.notice(p, "this board has no legacy files")
			h.send(p, MsgLegacyData, legacyMsg(k, what, 0, 0, nil))
			return
		}
		for off := 0; off < len(data); off += replayChunk {
			h.send(p, MsgLegacyData, legacyMsg(k, what, len(data), off, data[off:min(off+replayChunk, len(data))]))
		}
	}()
	return nil
}

func legacyMsg(k board.Key, what, size, offset int, chunk []byte) []byte {
	var w writer
	w.str(k.Map)
	w.s(int64(k.Mode))
	w.str(k.Kind)
	w.u(uint64(what))
	w.u(uint64(size))
	w.u(uint64(offset))
	w.bytes(chunk)
	return w.b
}
