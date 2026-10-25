// Package board makes the leaderboards from the witness's database: one board per map, run kind
// and mode, holding each install's fastest valid finished run. Names come from the hub's database,
// and the site's own database holds moderation: runs removed from the boards and installs banned
// from them. The witness's and hub's databases are only read.
package board

import (
	"errors"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"speedbraten/server/sqlite"
)

// Key names a board.
type Key struct {
	Map  string
	Kind string // flagrun, race, campaign, dmsp
	Mode int
}

// Entry is one install's place on a board.
type Entry struct {
	Rank    int
	Run     int64
	Install string // the witness's install key, hex
	Name    string
	// Registered: the name is the player's hub name or one picked on the dashboard, not just
	// the name in their replays, which anyone can set
	Registered bool
	MS         int64
	When       time.Time // when the run was judged
}

type Board struct {
	Key     Key
	Entries []Entry
	Runs    int // the runs that count on it, every player's
}

// Record is a time that was the best on its board when it was set.
type Record struct {
	Key        Key
	Run        int64
	Install    string
	Name       string
	Registered bool
	MS         int64
	Prev       int64 // the time it beat, -1 for the first time on the board
	When       time.Time
}

// Place is an install's entry on one board, for its player page.
type Place struct {
	Key   Key
	Entry Entry
	Of    int // entries on the board
}

type Store struct {
	witness, hub, site *sqlite.DB
	ttl                time.Duration

	mu      sync.Mutex
	built   time.Time
	boards  map[Key]*Board
	names   map[string]string
	named   map[string]bool // players whose name is registered (see Entry)
	listed  map[int64]bool
	merged  map[string]string // install -> the install it was merged into
	records []Record
	runs    []validRun // the runs that count, in the order they were judged
}

// Open reads witness.db and hub.db (hubDB may be "") and keeps moderation in site.db, making it if
// needed. Boards are rebuilt at most every ttl, and at once after a moderation change.
func Open(witnessDB, hubDB, siteDB string, ttl time.Duration) (*Store, error) {
	s := &Store{ttl: ttl}
	var err error
	if s.witness, err = sqlite.OpenReadOnly(witnessDB); err != nil {
		return nil, err
	}
	if hubDB != "" {
		if s.hub, err = sqlite.OpenReadOnly(hubDB); err != nil {
			return nil, err
		}
	}
	if s.site, err = sqlite.Create(siteDB); err != nil {
		return nil, err
	}
	for _, q := range []string{
		"CREATE TABLE IF NOT EXISTS removed(run INTEGER PRIMARY KEY, note TEXT, at INTEGER)",
		"CREATE TABLE IF NOT EXISTS banned(install TEXT PRIMARY KEY, note TEXT, at INTEGER)",
		"CREATE TABLE IF NOT EXISTS flagreviews(flag INTEGER PRIMARY KEY, note TEXT, at INTEGER)",
		"CREATE TABLE IF NOT EXISTS merged(install TEXT PRIMARY KEY, player TEXT NOT NULL, at INTEGER)",
		"CREATE TABLE IF NOT EXISTS names(install TEXT PRIMARY KEY, name TEXT NOT NULL)",
	} {
		if _, err := s.site.Exec(q); err != nil {
			return nil, err
		}
	}
	return s, nil
}

func (s *Store) Close() {
	s.witness.Close()
	if s.hub != nil {
		s.hub.Close()
	}
	s.site.Close()
}

// Witness is the witness's database, for pages that list runs.
func (s *Store) Witness() *sqlite.DB { return s.witness }

func atoi(v string) int64 { n, _ := strconv.ParseInt(v, 10, 64); return n }

type validRun struct {
	key           Key
	run, ms, when int64
	install       string
}

// load rebuilds the boards when they are older than ttl. Called with s.mu held.
func (s *Store) load() error {
	if s.boards != nil && time.Since(s.built) < s.ttl {
		return nil
	}
	removed, banned := map[int64]bool{}, map[string]bool{}
	rows, err := s.site.Query("SELECT run FROM removed")
	if err != nil {
		return err
	}
	for _, r := range rows {
		removed[atoi(r[0])] = true
	}
	if rows, err = s.site.Query("SELECT install FROM banned"); err != nil {
		return err
	}
	for _, r := range rows {
		banned[r[0]] = true
	}
	merged := map[string]string{}
	if rows, err = s.site.Query("SELECT install, player FROM merged"); err != nil {
		return err
	}
	for _, r := range rows {
		merged[r[0]] = r[1]
	}
	canon := func(k string) string {
		if into, ok := merged[k]; ok {
			return into
		}
		return k
	}
	// a ban on any of a player's installs bans the player
	for k := range banned {
		banned[canon(k)] = true
	}
	names, named, err := s.loadNames()
	if err != nil {
		return err
	}
	// a merged player takes a hub name from any of its installs when its own has none
	for k, into := range merged {
		if named[k] && !named[into] {
			names[into], named[into] = names[k], true
		}
	}
	if rows, err = s.site.Query("SELECT install, name FROM names"); err != nil {
		return err
	}
	for _, r := range rows {
		names[r[0]], named[r[0]] = r[1], true
	}

	// fastest first, so an install's first run on a board is its best; ties go to the earlier run
	if rows, err = s.witness.Query(`SELECT id, installkey, map, runkind, mode, runtimems, IFNULL(judged, 0)
		FROM runs WHERE verdict = 'valid' AND runtimems > 0 AND IFNULL(map, '') != '' AND IFNULL(runkind, '') != ''
		ORDER BY runtimems, id`); err != nil {
		return err
	}
	boards := map[Key]*Board{}
	listed := map[int64]bool{}
	onboard := map[Key]map[string]bool{}
	var runs []validRun
	for _, r := range rows {
		v := validRun{key: Key{Map: r[2], Kind: r[3], Mode: int(atoi(r[4]))}, run: atoi(r[0]), install: canon(r[1]), ms: atoi(r[5]), when: atoi(r[6])}
		if removed[v.run] || banned[v.install] || v.key.Kind == "flagrun" && !IsFlagrunMap(v.key.Map) {
			continue
		}
		runs = append(runs, v)
		if onboard[v.key] == nil {
			onboard[v.key] = map[string]bool{}
			boards[v.key] = &Board{Key: v.key}
		}
		boards[v.key].Runs++
		if onboard[v.key][v.install] {
			continue
		}
		onboard[v.key][v.install] = true
		b := boards[v.key]
		b.Entries = append(b.Entries, Entry{Rank: len(b.Entries) + 1, Run: v.run, Install: v.install, Name: names[v.install], Registered: named[v.install], MS: v.ms, When: time.UnixMilli(v.when)})
		listed[v.run] = true
	}

	// records in the order they were set: each run faster than every earlier one on its board
	sort.Slice(runs, func(i, j int) bool {
		return runs[i].when < runs[j].when || runs[i].when == runs[j].when && runs[i].run < runs[j].run
	})
	best := map[Key]int64{}
	var records []Record
	for _, v := range runs {
		prev, ok := best[v.key]
		if ok && v.ms >= prev {
			continue
		}
		if !ok {
			prev = -1
		}
		best[v.key] = v.ms
		records = append(records, Record{Key: v.key, Run: v.run, Install: v.install, Name: names[v.install], Registered: named[v.install], MS: v.ms, Prev: prev, When: time.UnixMilli(v.when)})
	}
	s.boards, s.names, s.named, s.listed, s.records, s.runs, s.merged, s.built = boards, names, named, listed, records, runs, merged, time.Now()
	return nil
}

// loadNames: an install's hub name (named), else the player name in its newest run.
func (s *Store) loadNames() (names map[string]string, named map[string]bool, err error) {
	names, named = map[string]string{}, map[string]bool{}
	rows, err := s.witness.Query("SELECT installkey, player FROM runs WHERE IFNULL(player, '') != '' ORDER BY id")
	if err != nil {
		return nil, nil, err
	}
	for _, r := range rows {
		names[r[0]] = r[1]
	}
	if s.hub == nil {
		return names, named, nil
	}
	hubnames := map[string]string{}
	if rows, err = s.hub.Query("SELECT key, name FROM installs WHERE IFNULL(name, '') != ''"); err != nil {
		return nil, nil, err
	}
	for _, r := range rows {
		hubnames[r[0]] = r[1]
	}
	// the hub knows an install by its Noise key, which the witness's sessions record as installpub
	if rows, err = s.witness.Query("SELECT DISTINCT installkey, installpub FROM sessions WHERE IFNULL(installpub, '') != ''"); err != nil {
		return nil, nil, err
	}
	for _, r := range rows {
		if n := hubnames[r[1]]; n != "" {
			names[r[0]], named[r[0]] = n, true
		}
	}
	return names, named, nil
}

func (s *Store) locked(f func()) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.load(); err != nil {
		return err
	}
	f()
	return nil
}

// Boards lists every board, most entries first, then by map, kind and mode.
func (s *Store) Boards() ([]*Board, error) {
	var out []*Board
	err := s.locked(func() {
		for _, b := range s.boards {
			out = append(out, b)
		}
	})
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i], out[j]
		if len(a.Entries) != len(b.Entries) {
			return len(a.Entries) > len(b.Entries)
		}
		if a.Key.Map != b.Key.Map {
			return a.Key.Map < b.Key.Map
		}
		if a.Key.Kind != b.Key.Kind {
			return a.Key.Kind < b.Key.Kind
		}
		return a.Key.Mode < b.Key.Mode
	})
	return out, err
}

// Board is one board, or nil when it has no entries.
func (s *Store) Board(k Key) (*Board, error) {
	var b *Board
	err := s.locked(func() { b = s.boards[k] })
	return b, err
}

// Player lists an install's places, best rank first, and its name ("" when unknown).
func (s *Store) Player(install string) ([]Place, string, error) {
	var places []Place
	var name string
	err := s.locked(func() {
		name = s.names[install]
		for _, b := range s.boards {
			for _, e := range b.Entries {
				if e.Install == install {
					places = append(places, Place{Key: b.Key, Entry: e, Of: len(b.Entries)})
					break
				}
			}
		}
	})
	sort.Slice(places, func(i, j int) bool {
		if places[i].Entry.Rank != places[j].Entry.Rank {
			return places[i].Entry.Rank < places[j].Entry.Rank
		}
		return places[i].Key.Map < places[j].Key.Map
	})
	return places, name, err
}

// Records lists the newest n records, newest first.
func (s *Store) Records(n int) ([]Record, error) {
	var out []Record
	err := s.locked(func() {
		for i := len(s.records) - 1; i >= 0 && len(out) < n; i-- {
			out = append(out, s.records[i])
		}
	})
	return out, err
}

// Listed tells whether a run is an entry on a board.
func (s *Store) Listed(run int64) (bool, error) {
	var ok bool
	err := s.locked(func() { ok = s.listed[run] })
	return ok, err
}

// Installs lists the installs whose name contains the text, ignoring case.
func (s *Store) Installs(text string) []string {
	var out []string
	text = strings.ToLower(text)
	s.locked(func() {
		for k, n := range s.names {
			if strings.Contains(strings.ToLower(n), text) {
				out = append(out, k)
			}
		}
	})
	sort.Strings(out)
	return out
}

// InstallOfPub is the witness's install key of the install with that Noise key (the hub's name
// for it), "" when the witness hasn't seen it.
func (s *Store) InstallOfPub(pub string) string {
	rows, err := s.witness.Query("SELECT installkey FROM sessions WHERE installpub = ? ORDER BY id DESC LIMIT 1", pub)
	if err != nil || len(rows) == 0 {
		return ""
	}
	return s.Canonical(rows[0][0])
}

// Canonical is the player an install belongs to: the install it was merged into, or itself.
func (s *Store) Canonical(install string) string {
	k := install
	s.locked(func() {
		if into, ok := s.merged[install]; ok {
			k = into
		}
	})
	return k
}

// Keys lists a player's installs: its own key first, then those merged into it.
func (s *Store) Keys(player string) []string {
	keys := []string{player}
	s.locked(func() {
		var more []string
		for k, into := range s.merged {
			if into == player {
				more = append(more, k)
			}
		}
		sort.Strings(more)
		keys = append(keys, more...)
	})
	return keys
}

// Merge makes install (and every install merged into it) part of the player into.
func (s *Store) Merge(install, into string) error {
	into = s.Canonical(into)
	install = s.Canonical(install)
	if install == into || install == "" || into == "" {
		return errors.New("that's the same player")
	}
	now := time.Now().Unix()
	if _, err := s.site.Exec("UPDATE merged SET player = ? WHERE player = ?", into, install); err != nil {
		return err
	}
	_, err := s.site.Exec("INSERT OR REPLACE INTO merged(install, player, at) VALUES(?, ?, ?)", install, into, now)
	s.changed()
	return err
}

// Split takes an install back out of the player it was merged into.
func (s *Store) Split(install string) error {
	_, err := s.site.Exec("DELETE FROM merged WHERE install = ?", install)
	s.changed()
	return err
}

// SetName sets a player's name on the boards; "" goes back to its hub or replay name.
func (s *Store) SetName(player, name string) error {
	var err error
	if name == "" {
		_, err = s.site.Exec("DELETE FROM names WHERE install = ?", player)
	} else {
		_, err = s.site.Exec("INSERT OR REPLACE INTO names(install, name) VALUES(?, ?)", player, name)
	}
	s.changed()
	return err
}

// Registered tells whether a player's name is registered (see Entry).
func (s *Store) Registered(player string) bool {
	var ok bool
	s.locked(func() { ok = s.named[player] })
	return ok
}

// Name is an install's name on the boards, "" when unknown.
func (s *Store) Name(install string) string {
	var n string
	s.locked(func() { n = s.names[install] })
	return n
}

// File is the path of a run's replay as the witness stored it, signatures included.
func (s *Store) File(run int64) (string, error) {
	rows, err := s.witness.Query("SELECT IFNULL(file, '') FROM runs WHERE id = ?", run)
	if err != nil {
		return "", err
	}
	if len(rows) != 1 || rows[0][0] == "" {
		return "", errors.New("no file for that run")
	}
	return rows[0][0], nil
}

// Moderation. A change rebuilds the boards on the next read.

func (s *Store) changed() { s.mu.Lock(); s.boards = nil; s.mu.Unlock() }

func (s *Store) Remove(run int64, note string) error {
	_, err := s.site.Exec("INSERT OR REPLACE INTO removed(run, note, at) VALUES(?, ?, ?)", run, note, time.Now().Unix())
	s.changed()
	return err
}

func (s *Store) Restore(run int64) error {
	_, err := s.site.Exec("DELETE FROM removed WHERE run = ?", run)
	s.changed()
	return err
}

func (s *Store) Ban(install, note string) error {
	_, err := s.site.Exec("INSERT OR REPLACE INTO banned(install, note, at) VALUES(?, ?, ?)", install, note, time.Now().Unix())
	s.changed()
	return err
}

func (s *Store) Unban(install string) error {
	_, err := s.site.Exec("DELETE FROM banned WHERE install = ?", install)
	s.changed()
	return err
}

// Note is a moderation entry: a removed run (ID is the run) or a banned install (ID is its key).
type Note struct {
	ID   string
	Note string
	At   time.Time
}

func (s *Store) notes(q string) ([]Note, error) {
	rows, err := s.site.Query(q)
	if err != nil {
		return nil, err
	}
	var out []Note
	for _, r := range rows {
		out = append(out, Note{ID: r[0], Note: r[1], At: time.Unix(atoi(r[2]), 0)})
	}
	return out, nil
}

func (s *Store) RemovedRuns() ([]Note, error) {
	return s.notes("SELECT run, IFNULL(note, ''), at FROM removed ORDER BY at DESC")
}

func (s *Store) BannedInstalls() ([]Note, error) {
	return s.notes("SELECT install, IFNULL(note, ''), at FROM banned ORDER BY at DESC")
}

// ReviewFlag marks a copy flag of the witness's as reviewed, with a note.
func (s *Store) ReviewFlag(flag int64, note string) error {
	_, err := s.site.Exec("INSERT OR REPLACE INTO flagreviews(flag, note, at) VALUES(?, ?, ?)", flag, note, time.Now().Unix())
	return err
}

// FlagReviews are the reviews made here, by flag.
func (s *Store) FlagReviews() (map[string]Note, error) {
	notes, err := s.notes("SELECT flag, IFNULL(note, ''), at FROM flagreviews")
	out := map[string]Note{}
	for _, n := range notes {
		out[n.ID] = n
	}
	return out, err
}

// IsRemoved and IsBanned tell the dashboard which actions to offer.
func (s *Store) IsRemoved(run int64) bool {
	rows, err := s.site.Query("SELECT 1 FROM removed WHERE run = ?", run)
	return err == nil && len(rows) > 0
}

func (s *Store) IsBanned(install string) bool {
	rows, err := s.site.Query("SELECT 1 FROM banned WHERE install = ?", install)
	return err == nil && len(rows) > 0
}
