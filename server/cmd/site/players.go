package main

import (
	"net/http"
	"sort"
	"strings"

	"speedbraten/server/board"
)

var playersPage = page("players.html")

// playerCategory is one completion category's numbers for a player: records held and boards with a time
type playerCategory struct {
	Records, Done, Of int
}

type playerRow struct {
	Install    string
	Name       string
	SortName   string // the name lowercased, what the player column sorts by
	Registered bool
	Records    int
	Runs       int
	Legacy     int // legacy records this player was the first to beat
	Done, Of   int // boards with a time, of all the categories' boards
	Percent    int
	Categories []playerCategory // in completionGroups order
}

// playerRows ranks every player with a counted run: most records first, then most boards, then by name
func (s *site) playerRows() ([]playerRow, error) {
	stats, err := s.store.PlayerStats()
	if err != nil {
		return nil, err
	}
	all, err := s.store.Boards()
	if err != nil {
		return nil, err
	}
	totals := s.completionTotals()
	type counts struct{ records, done map[string]int }
	byPlayer := map[string]*counts{}
	get := func(install string) *counts {
		c := byPlayer[install]
		if c == nil {
			c = &counts{records: map[string]int{}, done: map[string]int{}}
			byPlayer[install] = c
		}
		return c
	}
	for _, b := range all {
		g := completionGroup(b.Key)
		for _, e := range b.Entries {
			c := get(e.Install)
			c.done[g]++
			if e.Rank == 1 {
				c.records[g]++
			}
		}
	}
	legacy, err := s.legacyFirstBeaten()
	if err != nil {
		return nil, err
	}
	var rows []playerRow
	for _, p := range stats {
		c := get(p.Player)
		row := playerRow{Install: p.Player, Name: p.Name, SortName: strings.ToLower(p.Name), Registered: p.Registered, Records: p.Medals[1], Runs: p.Runs, Legacy: legacy[p.Player]}
		for _, g := range completionGroups {
			row.Categories = append(row.Categories, playerCategory{Records: c.records[g.Key], Done: c.done[g.Key], Of: totals[g.Key]})
			row.Done += c.done[g.Key]
			row.Of += totals[g.Key]
		}
		if row.Of > 0 {
			row.Percent = 100 * row.Done / row.Of
		}
		rows = append(rows, row)
	}
	sort.Slice(rows, func(i, j int) bool {
		a, b := rows[i], rows[j]
		if a.Records != b.Records {
			return a.Records > b.Records
		}
		if a.Done != b.Done {
			return a.Done > b.Done
		}
		if a.SortName != b.SortName {
			return a.SortName < b.SortName
		}
		return a.Install < b.Install
	})
	return rows, nil
}

// legacyFirstBeaten counts, per player, the legacy records they set the first witnessed time under
func (s *site) legacyFirstBeaten() (map[string]int, error) {
	out := map[string]int{}
	legacy, err := s.store.LegacyRecords()
	if err != nil {
		return nil, err
	}
	for _, l := range legacy {
		k := l.Key
		if k.Kind == "race" {
			k.Mode = 0
		}
		history, err := s.store.History(k)
		if err != nil {
			return nil, err
		}
		if first := firstUnder(history, l.MS); first != nil {
			out[first.Install]++
		}
	}
	return out, nil
}

// firstUnder is the earliest record faster than ms, nil if none is
func firstUnder(history []board.Record, ms int64) *board.Record {
	var first *board.Record
	for i := range history {
		r := &history[i]
		if r.MS < ms && (first == nil || r.When.Before(first.When)) {
			first = r
		}
	}
	return first
}

func (s *site) players(w http.ResponseWriter, _ *http.Request) {
	rows, err := s.playerRows()
	if err != nil {
		fail(w, err)
		return
	}
	totals := s.completionTotals()
	type groupHead struct {
		Label string
		Badge board.Key
		Of    int
	}
	var groups []groupHead
	for _, g := range completionGroups {
		groups = append(groups, groupHead{g.Label, g.Badge, totals[g.Key]})
	}
	render(w, playersPage, "players.html", map[string]any{"Title": "Players", "Tab": "players", "Players": rows, "Groups": groups})
}
