package board

import (
	"sort"
	"time"
)

// Run is a valid finished run, for the list of recent runs.
type Run struct {
	Key     Key
	Run     int64
	Name    string
	Install string
	MS      int64
	When    time.Time
	Record  bool // it was its board's best when it was judged
}

// Stats are the site's totals.
type Stats struct {
	Runs, Players, Boards, Maps int
	Last                        time.Time
}

func (s *Store) Stats() (Stats, error) {
	var st Stats
	err := s.locked(func() {
		st.Runs = len(s.runs)
		st.Boards = len(s.boards)
		players, maps := map[string]bool{}, map[string]bool{}
		for _, b := range s.boards {
			maps[b.Key.Map] = true
			for _, e := range b.Entries {
				players[e.Install] = true
			}
		}
		st.Players = len(players)
		st.Maps = len(maps)
		if n := len(s.runs); n > 0 {
			st.Last = time.UnixMilli(s.runs[n-1].when)
		}
	})
	return st, err
}

// RecentRuns lists the newest n runs that count, newest first.
func (s *Store) RecentRuns(n int) ([]Run, error) {
	var out []Run
	err := s.locked(func() {
		records := map[int64]bool{}
		for _, r := range s.records {
			records[r.Run] = true
		}
		for i := len(s.runs) - 1; i >= 0 && len(out) < n; i-- {
			v := s.runs[i]
			out = append(out, Run{Key: v.key, Run: v.run, Name: s.names[v.install], Install: v.install, MS: v.ms, When: time.UnixMilli(v.when), Record: records[v.run]})
		}
	})
	return out, err
}

// Standing is a board's current best.
type Standing struct {
	Key   Key
	Entry Entry
}

// OldestRecords lists the current bests that have stood the longest, oldest first.
func (s *Store) OldestRecords(n int) ([]Standing, error) {
	var out []Standing
	err := s.locked(func() {
		for _, b := range s.boards {
			out = append(out, Standing{Key: b.Key, Entry: b.Entries[0]})
		}
	})
	sort.Slice(out, func(i, j int) bool {
		a, b := out[i].Entry, out[j].Entry
		return a.When.Before(b.When) || a.When.Equal(b.When) && a.Run < b.Run
	})
	if len(out) > n {
		out = out[:n]
	}
	return out, err
}

// History lists a board's records in the order they were set, for its graph.
func (s *Store) History(k Key) ([]Record, error) {
	var out []Record
	err := s.locked(func() {
		for _, r := range s.records {
			if r.Key == k {
				out = append(out, r)
			}
		}
	})
	return out, err
}

// Holder is an install and how many boards it holds the best time on.
type Holder struct {
	Install, Name string
	Records       int
}

// TopHolders lists the installs holding the most bests.
func (s *Store) TopHolders(n int) ([]Holder, error) {
	counts := map[string]int{}
	names := map[string]string{}
	err := s.locked(func() {
		for _, b := range s.boards {
			e := b.Entries[0]
			counts[e.Install]++
			names[e.Install] = e.Name
		}
	})
	var out []Holder
	for k, c := range counts {
		out = append(out, Holder{Install: k, Name: names[k], Records: c})
	}
	sort.Slice(out, func(i, j int) bool {
		return out[i].Records > out[j].Records || out[i].Records == out[j].Records && out[i].Name < out[j].Name
	})
	if len(out) > n {
		out = out[:n]
	}
	return out, err
}

// PlayerRuns are an install's runs that count, newest first (at most n), with its total and when
// its first one was judged.
func (s *Store) PlayerRuns(install string, n int) (recent []Run, total int, first time.Time, err error) {
	err = s.locked(func() {
		records := map[int64]bool{}
		for _, r := range s.records {
			records[r.Run] = true
		}
		for i := len(s.runs) - 1; i >= 0; i-- {
			v := s.runs[i]
			if v.install != install {
				continue
			}
			total++
			first = time.UnixMilli(v.when)
			if len(recent) < n {
				recent = append(recent, Run{Key: v.key, Run: v.run, Name: s.names[v.install], Install: v.install, MS: v.ms, When: time.UnixMilli(v.when), Record: records[v.run]})
			}
		}
	})
	return
}

// PlayerSummary is a player for the dashboard: every install merged into it, every name it was seen
// under, and its runs (all of them, finished or not).
type PlayerSummary struct {
	Player   string // the player's own install key
	Name     string
	Keys     []string
	Aliases  []string
	Runs     int
	LastSeen time.Time
	Banned   bool
}

// Players lists every install that has sent a run, grouped into players, newest first.
func (s *Store) Players() ([]PlayerSummary, error) {
	rows, err := s.witness.Query("SELECT installkey, COUNT(*), IFNULL(MAX(started), 0) FROM runs GROUP BY installkey")
	if err != nil {
		return nil, err
	}
	aliases, err := s.aliases()
	if err != nil {
		return nil, err
	}
	banned, _ := s.BannedInstalls()
	isBanned := map[string]bool{}
	for _, b := range banned {
		isBanned[s.Canonical(b.ID)] = true
	}
	byPlayer := map[string]*PlayerSummary{}
	for _, r := range rows {
		key := r[0]
		p := s.Canonical(key)
		ps := byPlayer[p]
		if ps == nil {
			ps = &PlayerSummary{Player: p, Name: s.Name(p), Keys: s.Keys(p), Banned: isBanned[p]}
			byPlayer[p] = ps
		}
		ps.Runs += int(atoi(r[1]))
		if t := time.UnixMilli(atoi(r[2])); t.After(ps.LastSeen) {
			ps.LastSeen = t
		}
		ps.Aliases = mergeNames(ps.Aliases, aliases[key])
	}
	var out []PlayerSummary
	for _, ps := range byPlayer {
		out = append(out, *ps)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].LastSeen.After(out[j].LastSeen) })
	return out, nil
}

// aliases: the names each install was seen under, from its runs and from the hub.
func (s *Store) aliases() (map[string][]string, error) {
	out := map[string][]string{}
	rows, err := s.witness.Query("SELECT DISTINCT installkey, player FROM runs WHERE IFNULL(player, '') != ''")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		out[r[0]] = mergeNames(out[r[0]], []string{r[1]})
	}
	if s.hub == nil {
		return out, nil
	}
	hubnames := map[string]string{}
	if rows, err = s.hub.Query("SELECT key, name FROM installs WHERE IFNULL(name, '') != ''"); err != nil {
		return nil, err
	}
	for _, r := range rows {
		hubnames[r[0]] = r[1]
	}
	if rows, err = s.witness.Query("SELECT DISTINCT installkey, installpub FROM sessions WHERE IFNULL(installpub, '') != ''"); err != nil {
		return nil, err
	}
	for _, r := range rows {
		if n := hubnames[r[1]]; n != "" {
			out[r[0]] = mergeNames(out[r[0]], []string{n})
		}
	}
	return out, nil
}

func mergeNames(a, b []string) []string {
	for _, n := range b {
		found := false
		for _, m := range a {
			found = found || m == n
		}
		if !found {
			a = append(a, n)
		}
	}
	return a
}

// PickedName is the name picked for a player on the dashboard, "" if none.
func (s *Store) PickedName(player string) string {
	rows, err := s.site.Query("SELECT name FROM names WHERE install = ?", player)
	if err != nil || len(rows) == 0 {
		return ""
	}
	return rows[0][0]
}

// DefaultName is the name a player has without a picked one.
func (s *Store) DefaultName(player string) string {
	names, err := s.loadNames()
	if err != nil {
		return ""
	}
	return names[player]
}
