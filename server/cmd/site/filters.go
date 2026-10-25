package main

import (
	"net/http"
	"sort"
	"strings"

	"speedbraten/server/board"
)

type option struct{ Value, Label string }

// listFilter is a list page's search box, mode filter and sort order, from the query string.
type listFilter struct {
	Q, Mode, Sort string
	Modes, Sorts  []option
}

func newFilter(r *http.Request, sorts []option, modes []option) listFilter {
	q := r.URL.Query()
	f := listFilter{Q: strings.TrimSpace(q.Get("q")), Mode: q.Get("mode"), Sort: q.Get("sort"), Sorts: sorts, Modes: modes}
	if !hasOption(sorts, f.Sort) {
		f.Sort = sorts[0].Value
	}
	if !hasOption(modes, f.Mode) {
		f.Mode = ""
	}
	return f
}

func hasOption(opts []option, v string) bool {
	for _, o := range opts {
		if o.Value == v {
			return true
		}
	}
	return false
}

// matches: the search text is in the map's name, its campaign's name or one of the names.
func (f listFilter) matches(k board.Key, names ...string) bool {
	if f.Q == "" {
		return true
	}
	q := strings.ToLower(f.Q)
	if strings.Contains(strings.ToLower(k.Map), q) || strings.Contains(strings.ToLower(k.MapLabel()), q) {
		return true
	}
	for _, n := range names {
		if strings.Contains(strings.ToLower(n), q) {
			return true
		}
	}
	return false
}

// modeOf is the mode filter's value for a board: its badge.
func modeOf(k board.Key) string { return strings.ToLower(badgeOf(k).Text) }

var boardSorts = []option{{"map", "map A–Z"}, {"newest", "newest record"}, {"oldest", "oldest record"}, {"players", "most players"}, {"time", "fastest record"}}

// sortBoards orders boards by the filter's sort.
func sortBoards(boards []*board.Board, by string) {
	sort.SliceStable(boards, func(i, j int) bool {
		a, b := boards[i], boards[j]
		ea, eb := a.Entries[0], b.Entries[0]
		switch by {
		case "newest":
			return ea.When.After(eb.When)
		case "oldest":
			return ea.When.Before(eb.When)
		case "players":
			return len(a.Entries) > len(b.Entries)
		case "time":
			return ea.MS < eb.MS
		}
		return a.Key.MapLabel() < b.Key.MapLabel()
	})
}

func filterBoards(boards []*board.Board, f listFilter) []*board.Board {
	var out []*board.Board
	for _, b := range boards {
		names := make([]string, 0, len(b.Entries))
		for _, e := range b.Entries {
			names = append(names, e.Name)
		}
		if f.matches(b.Key, names...) && (f.Mode == "" || modeOf(b.Key) == f.Mode) {
			out = append(out, b)
		}
	}
	sortBoards(out, f.Sort)
	return out
}
