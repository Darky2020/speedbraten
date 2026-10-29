package hub

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	// MaxMapFile is the largest map file (.ogz or .cfg) the hub sends.
	MaxMapFile = 32 << 20
	mapChunk   = 32 << 10
)

// Map file roles in MAPDATA.
const (
	MapOgz = 0
	MapCfg = 1
)

// validMapName: the characters map names have, no folders.
func validMapName(name string) bool {
	if name == "" || len(name) > 64 {
		return false
	}
	for _, c := range name {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}

func mapDataMsg(name string, role, size, offset int, chunk []byte) []byte {
	var w writer
	w.str(name)
	w.u(uint64(role))
	w.u(uint64(size))
	w.u(uint64(offset))
	w.bytes(chunk)
	return w.b
}

// mapRequest sends a map's .cfg (if it has one) and then its .ogz; the .ogz arriving whole ends
// the transfer, and an empty .ogz piece says there is no such map. One request per player at a
// time. Called with h.mu held.
func (h *Hub) mapRequest(p *player, r *reader) error {
	name := r.str(64)
	if err := r.done(); err != nil {
		return err
	}
	if p.mapBusy {
		return nil
	}
	refuse := func(why string) error {
		h.notice(p, why)
		h.send(p, MsgMapData, mapDataMsg(name, MapOgz, 0, 0, nil))
		return nil
	}
	if h.cfg.Maps == "" && h.cfg.RaceMaps == "" {
		return refuse("this hub has no maps")
	}
	if !validMapName(name) {
		return refuse("no map is named that")
	}
	p.mapBusy = true
	go func() {
		var ogz, cfg []byte
		ok := false
		for _, root := range []string{h.cfg.Maps, h.cfg.RaceMaps} {
			if root == "" {
				continue
			}
			dir := filepath.Join(root, "packages", "base")
			if ogz, ok = readMapFile(filepath.Join(dir, name+".ogz")); ok {
				cfg, _ = readMapFile(filepath.Join(dir, name+".cfg"))
				break
			}
		}
		h.mu.Lock()
		if !ok {
			p.mapBusy = false
			if !p.closed {
				refuse("the hub doesn't have " + name)
			}
			h.mu.Unlock()
			return
		}
		h.mu.Unlock()
		if cfg != nil && !h.sendMapFile(p, name, MapCfg, cfg) {
			return
		}
		h.sendMapFile(p, name, MapOgz, ogz)
		h.mu.Lock()
		p.mapBusy = false
		h.mu.Unlock()
	}()
	return nil
}

func readMapFile(path string) ([]byte, bool) {
	st, err := os.Stat(path)
	if err != nil || !st.Mode().IsRegular() || st.Size() == 0 || st.Size() > MaxMapFile {
		return nil, false
	}
	data, err := os.ReadFile(path)
	return data, err == nil
}

// sendMapFile queues a file's pieces, waiting while the player's queue is half full so a big map
// doesn't overflow it; false when the player left.
func (h *Hub) sendMapFile(p *player, name string, role int, data []byte) bool {
	for off := 0; off < len(data); {
		h.mu.Lock()
		if p.closed {
			h.mu.Unlock()
			return false
		}
		if len(p.out) > OutQueue/2 {
			h.mu.Unlock()
			time.Sleep(10 * time.Millisecond)
			continue
		}
		end := min(off+mapChunk, len(data))
		h.send(p, MsgMapData, mapDataMsg(name, role, len(data), off, data[off:end]))
		h.mu.Unlock()
		off = end
	}
	return true
}

// raceMaps are the maps in the race map dir, sorted; read on each call so that maps added while
// the hub runs are listed.
func (h *Hub) raceMaps() []string {
	if h.cfg.RaceMaps == "" {
		return nil
	}
	entries, err := os.ReadDir(filepath.Join(h.cfg.RaceMaps, "packages", "base"))
	if err != nil {
		return nil
	}
	var maps []string
	for _, e := range entries {
		if name, ok := strings.CutSuffix(e.Name(), ".ogz"); ok && e.Type().IsRegular() && validMapName(name) {
			maps = append(maps, name)
		}
	}
	sort.Strings(maps)
	return maps
}

func (h *Hub) raceMapsMsg() []byte {
	maps := h.raceMaps()
	var w writer
	w.u(uint64(len(maps)))
	for _, m := range maps {
		w.str(m)
	}
	return w.b
}
