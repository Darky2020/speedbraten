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
	Runs, Players, Boards int
	Last                  time.Time
}

func (s *Store) Stats() (Stats, error) {
	var st Stats
	err := s.locked(func() {
		st.Runs = len(s.runs)
		st.Boards = len(s.boards)
		players := map[string]bool{}
		for _, b := range s.boards {
			for _, e := range b.Entries {
				players[e.Install] = true
			}
		}
		st.Players = len(players)
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
