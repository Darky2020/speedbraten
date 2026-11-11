package hub

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"speedbraten/server/board"
)

// Shared replays: a player uploads a replay file (SHARE, then SHAREDATA pieces), the hub checks
// it, keeps the newest ShareKeep in Config.Shared and tells everyone else online who isn't on do
// not disturb. SHAREDLIST lists them, SHAREDGET sends one.

// Limits; variables so the hub's flags and the tests can change them.
var (
	MaxShare      = 10 << 20
	ShareKeep     = 10
	ShareEvery    = 20 * time.Second
	SharesPerHour = 30
	MaxUploads    = 4 // at once, hub-wide
	ShareStall    = time.Minute
	shareChunk    = 48 << 10
)

// SHARESTATE's states
const (
	ShareRefused = 0 // a NOTICE says why
	ShareSend    = 1 // send the SHAREDATA pieces
	ShareDone    = 2 // shared, with its id
)

// SHARED rows' flags
const (
	ShareFlagrun  = 1
	ShareRace     = 2
	ShareFinished = 4
)

type upload struct {
	size int
	data []byte
	last time.Time
}

type sharedRow struct {
	id              int64
	sharer          string
	at              int64
	mapname, runner string
	mode            int64
	flags           uint64
	runtimems, size int64
}

func (r sharedRow) put(w *writer) {
	w.u(uint64(r.id))
	w.str(r.sharer)
	w.u(uint64(r.at))
	w.str(r.mapname)
	w.s(r.mode)
	w.u(r.flags)
	w.str(r.runner)
	w.u(uint64(r.runtimems))
	w.u(uint64(r.size))
}

func shareState(state, id int64) []byte {
	var w writer
	w.u(uint64(state))
	w.u(uint64(id))
	return w.b
}

func (h *Hub) refuseShare(p *player, why string) {
	h.notice(p, why)
	h.send(p, MsgShareState, shareState(ShareRefused, 0))
}

// shareStart: SHARE size. Called with h.mu held.
func (h *Hub) shareStart(p *player, r *reader) error {
	size := r.u()
	if err := r.done(); err != nil {
		return err
	}
	now := h.cfg.Now()
	switch {
	case h.cfg.Shared == "":
		h.refuseShare(p, "this hub doesn't keep shared replays")
		return nil
	case p.name == "":
		h.refuseShare(p, "pick a name to share replays")
		return nil
	case p.mutedUntil > now.Unix():
		h.refuseShare(p, "you are muted")
		return nil
	case p.upload != nil:
		h.refuseShare(p, "a replay of yours is still uploading")
		return nil
	case size == 0 || size > uint64(MaxShare):
		h.refuseShare(p, fmt.Sprintf("replays over %d MB can't be shared", MaxShare>>20))
		return nil
	}
	recent := p.shares[:0]
	for _, t := range p.shares {
		if now.Sub(t) < time.Hour {
			recent = append(recent, t)
		}
	}
	p.shares = recent
	if len(recent) >= SharesPerHour {
		h.refuseShare(p, fmt.Sprintf("you shared %d replays in the last hour, wait a bit", len(recent)))
		return nil
	}
	if n := len(recent); n > 0 && now.Sub(recent[n-1]) < ShareEvery {
		wait := (ShareEvery - now.Sub(recent[n-1]) + time.Second - 1) / time.Second
		h.refuseShare(p, fmt.Sprintf("wait %ds before sharing again", wait))
		return nil
	}
	uploads := 0
	for _, q := range h.players {
		if q.upload != nil && now.Sub(q.upload.last) > ShareStall {
			q.upload = nil
		}
		if q.upload != nil {
			uploads++
		}
	}
	if uploads >= MaxUploads {
		h.refuseShare(p, "the hub is busy with other uploads, try again in a moment")
		return nil
	}
	p.upload = &upload{size: int(size), last: now}
	h.send(p, MsgShareState, shareState(ShareSend, 0))
	return nil
}

// shareData: SHAREDATA offset bytes, the next piece of p's upload. It doesn't count against the
// message rate; the declared size bounds it. Called with h.mu held.
func (h *Hub) shareData(p *player, msg []byte) {
	up := p.upload
	if up == nil {
		return
	}
	r := reader{b: msg}
	off := r.u()
	if r.err != nil || off != uint64(len(up.data)) || len(up.data)+len(r.b) > up.size {
		p.upload = nil
		h.refuseShare(p, "the upload went wrong, try again")
		return
	}
	up.data = append(up.data, r.b...)
	up.last = h.cfg.Now()
	if len(up.data) < up.size {
		return
	}
	p.upload = nil
	p.shares = append(p.shares, h.cfg.Now())
	go h.shareFinish(p, up)
}

// shareFinish checks a complete upload and keeps it, off the hub's lock.
func (h *Hub) shareFinish(p *player, up *upload) {
	row, content, err := checkReplay(up.data)
	why := ""
	if err != nil {
		why = "that file isn't a replay the hub can share (" + err.Error() + ")"
	} else if h.boards != nil {
		why = lockedShare(h.boards, content, p.key)
	}
	tmp := ""
	if why == "" {
		tmp = filepath.Join(h.cfg.Shared, hex.EncodeToString(content)+".tmp")
		if err := os.MkdirAll(h.cfg.Shared, 0o755); err != nil {
			why = "the hub couldn't keep that replay"
		} else if err := os.WriteFile(tmp, up.data, 0o644); err != nil {
			why = "the hub couldn't keep that replay"
		}
	}
	defer os.Remove(tmp)
	h.mu.Lock()
	defer h.mu.Unlock()
	if p.closed {
		return
	}
	if why != "" {
		h.refuseShare(p, why)
		return
	}
	row.sharer = p.name
	row.at = h.cfg.Now().Unix()
	row.size = int64(len(up.data))
	id, err := h.keepShare(row, content, tmp)
	if err != nil {
		h.cfg.Logf("share from %s: %v", p.name, err)
		h.refuseShare(p, "the hub couldn't keep that replay")
		return
	}
	row.id = id
	h.send(p, MsgShareState, shareState(ShareDone, id))
	var w writer
	row.put(&w)
	for _, q := range h.players {
		if q != p && q.name != "" && q.away&AwayDND == 0 {
			h.send(q, MsgShared, w.b)
		}
	}
}

// lockedShare: why a replay can't be shared yet, when it is a board entry whose replay isn't
// public and the sharer isn't its runner.
func lockedShare(store *board.Store, content []byte, sharerKey string) string {
	run, ok := store.RunOfContent(hex.EncodeToString(content))
	if !ok {
		return ""
	}
	until, listed, err := store.ReplayLock(run, store.InstallOfPub(sharerKey))
	if err != nil || !listed || until.IsZero() {
		return ""
	}
	return "that run's replay is public in " + board.Wait(time.Until(until)) + "; only its runner can share it before then"
}

// keepShare stores a checked replay written to tmp: a file already in the buffer moves to the top
// under its new sharer, otherwise it gets a new row; the oldest beyond ShareKeep go. Called with
// h.mu held.
func (h *Hub) keepShare(row sharedRow, content []byte, tmp string) (int64, error) {
	hexc := hex.EncodeToString(content)
	rows, err := h.st.db.Query("SELECT id FROM shared WHERE content = ?", hexc)
	if err != nil {
		return 0, err
	}
	var id int64
	if len(rows) > 0 {
		id, _ = strconv.ParseInt(rows[0][0], 10, 64)
		if _, err := h.st.db.Exec("UPDATE shared SET sharer = ?, time = ? WHERE id = ?", row.sharer, row.at, id); err != nil {
			return 0, err
		}
	} else {
		if _, err := h.st.db.Exec("INSERT INTO shared (time, sharer, content, map, mode, flags, runner, runtimems, size) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
			row.at, row.sharer, hexc, row.mapname, row.mode, int64(row.flags), row.runner, row.runtimems, row.size); err != nil {
			return 0, err
		}
		got, err := h.st.db.Query("SELECT last_insert_rowid()")
		if err != nil || len(got) != 1 {
			return 0, errors.New("no id for the new share")
		}
		id, _ = strconv.ParseInt(got[0][0], 10, 64)
		if err := os.Rename(tmp, h.sharePath(id)); err != nil {
			h.st.db.Exec("DELETE FROM shared WHERE id = ?", id)
			return 0, err
		}
	}
	old, err := h.st.db.Query("SELECT id FROM shared ORDER BY time DESC, id DESC LIMIT -1 OFFSET ?", ShareKeep)
	if err == nil {
		for _, o := range old {
			oid, _ := strconv.ParseInt(o[0], 10, 64)
			os.Remove(h.sharePath(oid))
			h.st.db.Exec("DELETE FROM shared WHERE id = ?", oid)
		}
	}
	return id, nil
}

func (h *Hub) sharePath(id int64) string {
	return filepath.Join(h.cfg.Shared, strconv.FormatInt(id, 10)+".sbr")
}

func (h *Hub) sharedRows() ([]sharedRow, error) {
	rows, err := h.st.db.Query("SELECT id, sharer, time, map, mode, flags, runner, runtimems, size FROM shared ORDER BY time DESC, id DESC LIMIT ?", ShareKeep)
	if err != nil {
		return nil, err
	}
	out := make([]sharedRow, 0, len(rows))
	for _, r := range rows {
		n := func(i int) int64 { v, _ := strconv.ParseInt(r[i], 10, 64); return v }
		out = append(out, sharedRow{id: n(0), sharer: r[1], at: n(2), mapname: r[3], mode: n(4), flags: uint64(n(5)), runner: r[6], runtimems: n(7), size: n(8)})
	}
	return out, nil
}

// sharedList: SHAREDLIST, answered with SHAREDROWS, newest first. Called with h.mu held.
func (h *Hub) sharedList(p *player, r *reader) error {
	if err := r.done(); err != nil {
		return err
	}
	rows, err := h.sharedRows()
	if err != nil {
		h.cfg.Logf("shared list: %v", err)
	}
	var w writer
	w.u(uint64(len(rows)))
	for _, row := range rows {
		row.put(&w)
	}
	h.send(p, MsgSharedRows, w.b)
	return nil
}

// sharedGet: SHAREDGET id, answered with SHAREDDATA pieces, sent only while the player's queue
// has room so that a large file doesn't overflow it; one at a time per player. Called with
// h.mu held.
func (h *Hub) sharedGet(p *player, r *reader) error {
	id := int64(r.u())
	if err := r.done(); err != nil {
		return err
	}
	if p.sharedBusy {
		return nil
	}
	fail := func() { h.send(p, MsgSharedData, sharedMsg(id, 0, 0, nil)) }
	if h.cfg.Shared == "" {
		h.notice(p, "this hub doesn't keep shared replays")
		fail()
		return nil
	}
	rows, err := h.st.db.Query("SELECT size FROM shared WHERE id = ?", id)
	if err != nil || len(rows) != 1 {
		h.notice(p, "that shared replay is gone")
		fail()
		return nil
	}
	p.sharedBusy = true
	path := h.sharePath(id)
	go func() {
		data, err := os.ReadFile(path)
		h.mu.Lock()
		defer h.mu.Unlock()
		defer func() { p.sharedBusy = false }()
		if err != nil || len(data) == 0 {
			h.notice(p, "that shared replay is gone")
			fail()
			return
		}
		for off := 0; off < len(data) && !p.closed; off += shareChunk {
			for len(p.out) > OutQueue/4 && !p.closed {
				h.mu.Unlock()
				time.Sleep(10 * time.Millisecond)
				h.mu.Lock()
			}
			h.send(p, MsgSharedData, sharedMsg(id, len(data), off, data[off:min(off+shareChunk, len(data))]))
		}
	}()
	return nil
}

func sharedMsg(id int64, size, offset int, chunk []byte) []byte {
	var w writer
	w.u(uint64(id))
	w.u(uint64(size))
	w.u(uint64(offset))
	w.bytes(chunk)
	return w.b
}

// checkReplay walks an SBR2 file's container — magic, version, every chunk's CRC, META first, an
// END with the content hash, only signatures after it — and reads the row's fields from its META
// and RSLT. It doesn't decompress or re-hash: players' clients check that before playing.
func checkReplay(b []byte) (sharedRow, []byte, error) {
	var row sharedRow
	if len(b) < 4 || string(b[:4]) != "SBR2" {
		return row, nil, errors.New("not an SBR2 file")
	}
	r := reader{b: b[4:]}
	version := r.u()
	if r.err != nil || version < 1 || version > 2 {
		return row, nil, errors.New("unknown version")
	}
	var content []byte
	meta, result := false, false
	for i := 0; len(r.b) > 0; i++ {
		start := len(b) - len(r.b)
		if len(r.b) < 4 {
			return row, nil, errors.New("cut off")
		}
		typ := string(r.b[:4])
		r.b = r.b[4:]
		flags := r.u()
		stored := r.u()
		if r.err != nil || stored > uint64(len(r.b)) || len(r.b)-int(stored) < 4 {
			return row, nil, errors.New("cut off")
		}
		if flags&^1 != 0 {
			return row, nil, errors.New("unknown chunk flags")
		}
		payload := r.b[:stored]
		end := len(b) - len(r.b) + int(stored)
		if binary.LittleEndian.Uint32(b[end:end+4]) != crc32.ChecksumIEEE(b[start:end]) {
			return row, nil, fmt.Errorf("chunk %d: CRC mismatch", i)
		}
		r.b = r.b[stored+4:]
		if content != nil && typ != "sIGN" {
			return row, nil, errors.New("chunk after END")
		}
		compressed := flags&1 != 0
		switch {
		case i == 0 && typ != "META":
			return row, nil, errors.New("no META first")
		case typ == "META" && !compressed:
			if meta {
				return row, nil, errors.New("two META chunks")
			}
			meta = true
			readShareMeta(payload, &row)
		case typ == "RSLT" && !compressed:
			pr := reader{b: payload}
			finished := pr.u()
			pr.u()
			pr.u()
			runtime := pr.u()
			if pr.err != nil {
				return row, nil, errors.New("bad RSLT")
			}
			result = true
			if finished != 0 {
				row.flags |= ShareFinished
			}
			row.runtimems = int64(runtime)
		case typ == "END ":
			pr := reader{b: payload}
			pr.u()
			if pr.err != nil || len(pr.b) != 32 {
				return row, nil, errors.New("bad END")
			}
			content = bytes.Clone(pr.b)
		}
	}
	if !meta || !result || content == nil {
		return row, nil, errors.New("incomplete")
	}
	return row, content, nil
}

// readShareMeta reads the row's fields from a META chunk's key-value list; the rest is skipped.
func readShareMeta(b []byte, row *sharedRow) {
	r := reader{b: b}
	n := r.u()
	for i := uint64(0); i < n && r.err == nil; i++ {
		key := r.str(64)
		if r.err != nil || len(r.b) == 0 {
			return
		}
		typ := r.b[0]
		r.b = r.b[1:]
		switch typ {
		case 0: // signed varint
			v := r.s()
			if key == "mode" {
				row.mode = v
			}
		case 1: // varint
			v := r.u()
			if key == "flagrun" && v != 0 {
				row.flags |= ShareFlagrun
			} else if key == "race" && v != 0 {
				row.flags |= ShareRace
			}
		case 2, 3: // string, bytes
			s := r.str(1 << 20)
			if typ == 2 && key == "map" {
				row.mapname = clip(s, 64)
			} else if typ == 2 && key == "player" {
				row.runner = clip(s, 64)
			}
		case 4: // f32
			if len(r.b) < 4 {
				return
			}
			r.b = r.b[4:]
		case 5: // bool
			if len(r.b) < 1 {
				return
			}
			r.b = r.b[1:]
		default:
			return
		}
	}
}

func clip(s string, n int) string {
	if len(s) > n {
		s = strings.ToValidUTF8(s[:n], "")
	}
	return s
}
