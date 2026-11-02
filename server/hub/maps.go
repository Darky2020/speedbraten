package hub

import (
	"hash/crc32"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
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
		if dir := h.mapDir(name); dir != "" {
			if ogz, ok = readMapFile(filepath.Join(dir, name+".ogz")); ok {
				cfg, _ = readMapFile(filepath.Join(dir, name+".cfg"))
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

// mapDir is the packages/base folder a map is served from: the game data's before the race maps'.
func (h *Hub) mapDir(name string) string {
	for _, root := range []string{h.cfg.Maps, h.cfg.RaceMaps} {
		if root == "" {
			continue
		}
		dir := filepath.Join(root, "packages", "base")
		if st, err := os.Stat(filepath.Join(dir, name+".ogz")); err == nil && st.Mode().IsRegular() {
			return dir
		}
	}
	return ""
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

// mapCRCs are the CRC-32s of the race maps' files as served, which clients compare with the copies
// they downloaded before; kept up to date by RefreshMapCRCs.
type mapCRCs struct {
	mu   sync.Mutex
	maps map[string]mapCRC
}

type mapCRC struct {
	ogz, cfg fileStamp
	ogzCRC   uint32
	cfgCRC   uint32 // 0: no .cfg
}

type fileStamp struct {
	size int64
	mod  time.Time
}

func stampOf(path string) fileStamp {
	st, err := os.Stat(path)
	if err != nil || !st.Mode().IsRegular() {
		return fileStamp{}
	}
	return fileStamp{st.Size(), st.ModTime()}
}

func fileCRC(path string) uint32 {
	data, ok := readMapFile(path)
	if !ok {
		return 0
	}
	return crc32.ChecksumIEEE(data)
}

// RefreshMapCRCs computes the CRCs of the race maps whose files are new or changed since the last
// call and forgets maps that are gone; it returns how many maps it read.
func (h *Hub) RefreshMapCRCs() int {
	maps := h.raceMaps()
	h.crcs.mu.Lock()
	known := make(map[string]mapCRC, len(h.crcs.maps))
	for k, v := range h.crcs.maps {
		known[k] = v
	}
	h.crcs.mu.Unlock()
	next := make(map[string]mapCRC, len(maps))
	read := 0
	for _, m := range maps {
		dir := h.mapDir(m)
		if dir == "" {
			continue
		}
		ogz, cfg := filepath.Join(dir, m+".ogz"), filepath.Join(dir, m+".cfg")
		c := mapCRC{ogz: stampOf(ogz), cfg: stampOf(cfg)}
		if old, ok := known[m]; ok && old.ogz == c.ogz && old.cfg == c.cfg {
			next[m] = old
			continue
		}
		c.ogzCRC = fileCRC(ogz)
		if c.cfg.size > 0 {
			c.cfgCRC = fileCRC(cfg)
		}
		next[m] = c
		read++
	}
	h.crcs.mu.Lock()
	h.crcs.maps = next
	h.crcs.mu.Unlock()
	return read
}

// raceMapsMsg lists the race maps, then for each the CRC-32s of its .ogz and .cfg (0: not known yet,
// or no .cfg).
func (h *Hub) raceMapsMsg() []byte {
	maps := h.raceMaps()
	var w writer
	w.u(uint64(len(maps)))
	for _, m := range maps {
		w.str(m)
	}
	h.crcs.mu.Lock()
	defer h.crcs.mu.Unlock()
	for _, m := range maps {
		c := h.crcs.maps[m]
		w.u(uint64(c.ogzCRC))
		w.u(uint64(c.cfgCRC))
	}
	return w.b
}
