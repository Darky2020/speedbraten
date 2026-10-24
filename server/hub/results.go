package hub

import (
	"strconv"

	"speedbraten/server/sqlite"
)

// Results reads the PBs and records the witness writes to the events table of its database
// and announces each one to every player. It starts after the newest event already
// announced (kept in hub.db), or after the newest one there is when first used.
type Results struct {
	h    *Hub
	db   *sqlite.DB
	last int64
}

func (h *Hub) OpenResults(witnessDB string) (*Results, error) {
	db, err := sqlite.Open(witnessDB)
	if err != nil {
		return nil, err
	}
	r := &Results{h: h, db: db, last: -1}
	h.mu.Lock()
	h.wdb = db
	h.mu.Unlock()
	if _, err := h.st.db.Exec("CREATE TABLE IF NOT EXISTS announced (id INTEGER)"); err != nil {
		return nil, err
	}
	rows, err := h.st.db.Query("SELECT MAX(id) FROM announced")
	if err == nil && len(rows) == 1 && rows[0][0] != "" {
		r.last, _ = strconv.ParseInt(rows[0][0], 10, 64)
	}
	return r, nil
}

func (r *Results) Close() { r.db.Close() }

// Poll announces the events added since the last poll and reports how many.
func (r *Results) Poll() (int, error) {
	if r.last < 0 {
		rows, err := r.db.Query("SELECT IFNULL(MAX(id), 0) FROM events")
		if err != nil {
			return 0, err
		}
		r.last, _ = strconv.ParseInt(rows[0][0], 10, 64)
		return 0, r.save()
	}
	rows, err := r.db.Query("SELECT id, installpub, IFNULL(player, ''), map, runkind, mode, runtimems, record, IFNULL(previous, -1) FROM events WHERE id > ? ORDER BY id", r.last)
	if err != nil {
		return 0, err
	}
	for _, row := range rows {
		id, _ := strconv.ParseInt(row[0], 10, 64)
		mode, _ := strconv.ParseInt(row[5], 10, 64)
		ms, _ := strconv.ParseUint(row[6], 10, 64)
		previous, _ := strconv.ParseInt(row[8], 10, 64)
		r.h.announce(row[1], row[2], row[3], row[4], mode, ms, row[7] == "1", previous)
		r.last = id
	}
	if len(rows) > 0 {
		return len(rows), r.save()
	}
	return 0, nil
}

func (r *Results) save() error {
	if _, err := r.h.st.db.Exec("DELETE FROM announced"); err != nil {
		return err
	}
	_, err := r.h.st.db.Exec("INSERT INTO announced (id) VALUES (?)", r.last)
	return err
}

// announce tells every player of a PB or record; the name is the one registered for the
// install on the hub, else the one in the replay.
func (h *Hub) announce(installpub, player, mapname, kind string, mode int64, ms uint64, record bool, previous int64) {
	name := player
	if in, err := h.st.install(installpub); err == nil && in.name != "" {
		name = in.name
	}
	var w writer
	w.str(name)
	w.str(mapname)
	w.s(mode)
	w.str(kind)
	w.u(ms)
	if record {
		w.u(1)
	} else {
		w.u(0)
	}
	if previous < 0 {
		w.u(0)
	} else {
		w.u(uint64(previous) + 1)
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, q := range h.players {
		if q.name != "" {
			h.send(q, MsgResult, w.b)
		}
	}
}
