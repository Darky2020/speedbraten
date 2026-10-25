package main

import (
	"embed"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"

	"speedbraten/server/board"
)

//go:embed templates
var templateFS embed.FS

type badgeInfo struct{ Class, Text string }

func badgeOf(k board.Key) badgeInfo {
	switch {
	case k.Kind == "race":
		return badgeInfo{"b-race", "RACE"}
	case k.Kind == "campaign" || k.Kind == "dmsp":
		return badgeInfo{"b-sp", "SP"}
	case k.Mode == 11:
		return badgeInfo{"b-ctf", "CTF"}
	case k.Mode == 12:
		return badgeInfo{"b-ictf", "ICTF"}
	case k.Mode == 17:
		return badgeInfo{"b-ectf", "ECTF"}
	}
	return badgeInfo{"b-other", strings.ToUpper(board.ModeName(k.Mode))}
}

func ago(t time.Time) string {
	d := time.Since(t)
	switch {
	case d < time.Minute:
		return "just now"
	case d < time.Hour:
		return fmt.Sprintf("%d min ago", int(d.Minutes()))
	case d < 48*time.Hour:
		return fmt.Sprintf("%d h ago", int(d.Hours()))
	case d < 60*24*time.Hour:
		return fmt.Sprintf("%d days ago", int(d.Hours()/24))
	}
	return t.UTC().Format("2 Jan 2006")
}

func age(t time.Time) string {
	d := time.Since(t)
	switch {
	case d < 48*time.Hour:
		return fmt.Sprintf("%d h", int(d.Hours()))
	case d < 365*24*time.Hour:
		return fmt.Sprintf("%d days", int(d.Hours()/24))
	}
	return fmt.Sprintf("%.1f years", d.Hours()/24/365)
}

func mapURL(k board.Key) string {
	return "/map?" + url.Values{"map": {k.Map}, "kind": {k.Kind}, "mode": {strconv.Itoa(k.Mode)}}.Encode()
}

var funcs = template.FuncMap{
	"runtime":   board.Time,
	"gap":       func(a, b int64) string { return board.Time(a - b) },
	"date":      func(t time.Time) string { return t.UTC().Format("2006-01-02") },
	"ago":       ago,
	"age":       age,
	"inc":       func(i int) int { return i + 1 },
	"badge":     badgeOf,
	"modeName":  func(k board.Key) string { return board.ModeName(k.Mode) },
	"mapURL":    mapURL,
	"boardURL":  mapURL,
	"mapImage":  func(m string) string { return "/mapimg/" + url.PathEscape(m) },
	"playerURL": func(install string) string { return "/player?id=" + url.QueryEscape(install) },
	"short":     shortKey,
	"join":      func(s []string) string { return strings.Join(s, ", ") },
}

func page(files ...string) *template.Template {
	paths := []string{"templates/layout.html", "templates/records.html", "templates/panels.html"}
	for _, f := range files {
		paths = append(paths, "templates/"+f)
	}
	return template.Must(template.New(files[0]).Funcs(funcs).ParseFS(templateFS, paths...))
}

var (
	homePage     = page("home.html")
	racesPage    = page("races.html")
	flagrunsPage = page("flagruns.html")
	spPage       = page("singleplayer.html")
	mapPage      = page("map.html")
	playerPage   = page("player.html")
	infoPage     = page("info.html")
	recordsPage  = page("records.html")
)

type site struct {
	store    *board.Store
	mux      *http.ServeMux
	shots    string // dir of chosen map pictures, <map>.png or .jpg
	previews string // the game's packages/base, for <map>.jpg previews
	hubAddr  string
}

func newSite(store *board.Store) *site {
	s := &site{store: store, mux: http.NewServeMux(), hubAddr: "141.95.67.191"}
	s.mux.HandleFunc("/", s.home)
	s.mux.HandleFunc("/races", s.races)
	s.mux.HandleFunc("/flagruns", s.flagruns)
	s.mux.HandleFunc("/singleplayer", s.singleplayer)
	s.mux.HandleFunc("/map", s.mapBoard)
	s.mux.HandleFunc("/board", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/map?"+r.URL.RawQuery, http.StatusMovedPermanently)
	})
	s.mux.HandleFunc("/player", s.player)
	s.mux.HandleFunc("/records", s.records)
	s.mux.HandleFunc("/info", s.info)
	s.mux.HandleFunc("/replay", s.replay)
	s.mux.HandleFunc("/mapimg/", s.mapImage)
	return s
}

func (s *site) ServeHTTP(w http.ResponseWriter, r *http.Request) { s.mux.ServeHTTP(w, r) }

func render(w http.ResponseWriter, t *template.Template, name string, data map[string]any) {
	if _, ok := data["Admin"]; !ok {
		data["Admin"] = false
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := t.ExecuteTemplate(w, name, data); err != nil {
		log.Printf("%s: %v", name, err)
	}
}

func fail(w http.ResponseWriter, err error) {
	log.Print(err)
	http.Error(w, "something went wrong", http.StatusInternalServerError)
}

func (s *site) home(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	st, err := s.store.Stats()
	if err != nil {
		fail(w, err)
		return
	}
	recent, _ := s.store.RecentRuns(homeRuns)
	holders, _ := s.store.TopHolders(8)
	oldest, _ := s.store.OldestRecords(homeRuns)
	render(w, homePage, "home.html", map[string]any{"Title": "Leaderboards", "Tab": "home", "Stats": st, "Recent": recent,
		"Holders": holders, "Oldest": oldest, "More": "/records"})
}

var recordModes = []option{{"", "all modes"}, {"race", "race"}, {"flagrun", "all flagruns"}, {"ctf", "ctf"}, {"ictf", "insta ctf"}, {"ectf", "effic ctf"}, {"sp", "singleplayer"}}
var recordSorts = []option{{"newest", "newest first"}, {"oldest", "oldest first"}, {"improved", "biggest improvement"}, {"map", "map A–Z"}}

func (s *site) records(w http.ResponseWriter, r *http.Request) {
	all, err := s.store.Records(100000)
	if err != nil {
		fail(w, err)
		return
	}
	f := newFilter(r, recordSorts, recordModes)
	var records []board.Record
	for _, rec := range all {
		if f.matches(rec.Key, rec.Name) && (f.Mode == "" || modeOf(rec.Key) == f.Mode || f.Mode == "flagrun" && rec.Key.Kind == "flagrun") {
			records = append(records, rec)
		}
	}
	sort.SliceStable(records, func(i, j int) bool {
		a, b := records[i], records[j]
		switch f.Sort {
		case "oldest":
			return a.When.Before(b.When)
		case "improved":
			return improvement(a) > improvement(b)
		case "map":
			return a.Key.MapLabel() < b.Key.MapLabel()
		}
		return a.When.After(b.When)
	})
	if len(records) > 500 {
		records = records[:500]
	}
	render(w, recordsPage, "recordspage", map[string]any{"Title": "Records", "Tab": "home", "Records": records, "F": f, "Path": "/records"})
}

// improvement is how much a record beat the one before it; a board's first record counts as none.
func improvement(r board.Record) int64 {
	if r.Prev < 0 {
		return -1
	}
	return r.Prev - r.MS
}

func (s *site) boardsOf(kinds ...string) ([]*board.Board, error) {
	all, err := s.store.Boards()
	var out []*board.Board
	for _, b := range all {
		for _, k := range kinds {
			if b.Key.Kind == k {
				out = append(out, b)
			}
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Key.Map != out[j].Key.Map {
			return out[i].Key.Map < out[j].Key.Map
		}
		return out[i].Key.Mode < out[j].Key.Mode
	})
	return out, err
}

func (s *site) races(w http.ResponseWriter, r *http.Request) {
	boards, err := s.boardsOf("race")
	if err != nil {
		fail(w, err)
		return
	}
	f := newFilter(r, boardSorts, nil)
	data := map[string]any{"Title": "Races", "Tab": "races", "Boards": filterBoards(boards, f), "F": f, "Path": "/races"}
	s.addCategory(data, boards, "race")
	render(w, racesPage, "races.html", data)
}

type flagrunCell struct {
	Key   board.Key
	Entry *board.Entry
}

type flagrunRow struct {
	Key   board.Key // the row's map, for its picture and first board
	Cells []flagrunCell
	Runs  int // on all its modes
}

// flagrunModes are the columns of the flagruns table: ctf, insta ctf, effic ctf.
var flagrunModes = []int{11, 12, 17}

func (s *site) flagruns(w http.ResponseWriter, r *http.Request) {
	boards, err := s.boardsOf("flagrun")
	if err != nil {
		fail(w, err)
		return
	}
	modes := append([]int(nil), flagrunModes...)
	for _, b := range boards {
		known := false
		for _, m := range modes {
			known = known || m == b.Key.Mode
		}
		if !known {
			modes = append(modes, b.Key.Mode)
		}
	}
	// every map flagruns count on gets a row, runs or not
	byMap := map[string]map[int]*board.Board{}
	maps := append([]string(nil), board.FlagrunMaps...)
	for _, b := range boards {
		if byMap[b.Key.Map] == nil {
			byMap[b.Key.Map] = map[int]*board.Board{}
		}
		byMap[b.Key.Map][b.Key.Mode] = b
	}
	var rows []flagrunRow
	for _, m := range maps {
		row := flagrunRow{Key: board.Key{Map: m, Kind: "flagrun", Mode: flagrunModes[0]}}
		hasRuns := false
		for _, mode := range modes {
			c := flagrunCell{Key: board.Key{Map: m, Kind: "flagrun", Mode: mode}}
			if b := byMap[m][mode]; b != nil {
				c.Entry = &b.Entries[0]
				row.Runs += b.Runs
				if !hasRuns {
					row.Key, hasRuns = b.Key, true
				}
			}
			row.Cells = append(row.Cells, c)
		}
		rows = append(rows, row)
	}
	var cols []board.Key
	for _, m := range modes {
		cols = append(cols, board.Key{Kind: "flagrun", Mode: m})
	}
	f := newFilter(r, flagrunSorts, nil)
	var shown []flagrunRow
	for _, row := range rows {
		var names []string
		for _, c := range row.Cells {
			if c.Entry != nil {
				names = append(names, c.Entry.Name)
			}
		}
		if f.matches(row.Key, names...) {
			shown = append(shown, row)
		}
	}
	sort.SliceStable(shown, func(i, j int) bool {
		a, b := shown[i], shown[j]
		switch f.Sort {
		case "newest":
			return a.newest().After(b.newest())
		case "oldest":
			return a.newest().Before(b.newest())
		case "records":
			return a.filled() > b.filled()
		case "runs":
			return a.Runs > b.Runs
		}
		return a.Key.Map < b.Key.Map
	})
	data := map[string]any{"Title": "Flagruns", "Tab": "flagruns", "Rows": shown, "Modes": cols, "F": f, "Path": "/flagruns"}
	s.addCategory(data, boards, "flagrun")
	render(w, flagrunsPage, "flagruns.html", data)
}

var flagrunSorts = []option{{"map", "map A–Z"}, {"runs", "most runs"}, {"newest", "newest record"}, {"oldest", "oldest record"}, {"records", "most modes set"}}

// newest is when the row's latest record was set.
func (r flagrunRow) newest() time.Time {
	var t time.Time
	for _, c := range r.Cells {
		if c.Entry != nil && c.Entry.When.After(t) {
			t = c.Entry.When
		}
	}
	return t
}

func (r flagrunRow) filled() int {
	n := 0
	for _, c := range r.Cells {
		if c.Entry != nil {
			n++
		}
	}
	return n
}

func (s *site) singleplayer(w http.ResponseWriter, r *http.Request) {
	boards, err := s.boardsOf("campaign")
	if err != nil {
		fail(w, err)
		return
	}
	f := newFilter(r, boardSorts, nil)
	data := map[string]any{"Title": "Singleplayer", "Tab": "sp", "Campaigns": filterBoards(boards, f), "F": f, "Path": "/singleplayer"}
	s.addCategory(data, boards, "campaign")
	render(w, spPage, "singleplayer.html", data)
}

func tabOf(k board.Key) string {
	switch k.Kind {
	case "race":
		return "races"
	case "flagrun":
		return "flagruns"
	}
	return "sp"
}

func (s *site) mapBoard(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	mode, err := strconv.Atoi(q.Get("mode"))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	k := board.Key{Map: q.Get("map"), Kind: q.Get("kind"), Mode: mode}
	b, err := s.store.Board(k)
	if err != nil {
		fail(w, err)
		return
	}
	// the same map's other boards of this kind, for the mode tabs; a flagrun map has a tab (and
	// a page) for each of the three ctf modes, runs or not
	siblings, _ := s.boardsOf(k.Kind)
	var others []board.Key
	for _, o := range siblings {
		if o.Key.Map == k.Map {
			others = append(others, o.Key)
		}
	}
	if k.Kind == "flagrun" && board.IsFlagrunMap(k.Map) {
		others = others[:0]
		for _, m := range flagrunModes {
			others = append(others, board.Key{Map: k.Map, Kind: k.Kind, Mode: m})
		}
		if b == nil && slices.Contains(flagrunModes, k.Mode) {
			b = &board.Board{Key: k}
		}
	}
	if b == nil {
		http.NotFound(w, r)
		return
	}
	history, _ := s.store.History(k)
	render(w, mapPage, "map.html", map[string]any{"Title": k.MapLabel() + " · " + k.KindLabel(), "Tab": tabOf(k), "Board": b, "Others": others,
		"History": history, "Graph": recordGraph(history)})
}

type placeRow struct {
	board.Place
	Best int64
}

func (s *site) player(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	if c := s.store.Canonical(id); c != id {
		http.Redirect(w, r, "/player?id="+url.QueryEscape(c), http.StatusMovedPermanently)
		return
	}
	places, name, err := s.store.Player(id)
	if err != nil {
		fail(w, err)
		return
	}
	if len(places) == 0 {
		http.NotFound(w, r)
		return
	}
	var rows []placeRow
	medals := [4]int{}
	maps := map[string]bool{}
	mine := map[string]int{}
	for _, p := range places {
		maps[p.Key.Map] = true
		best := p.Entry.MS
		if b, _ := s.store.Board(p.Key); b != nil {
			best = b.Entries[0].MS
		}
		if p.Entry.Rank <= 3 {
			medals[p.Entry.Rank]++
		}
		mine[completionGroup(p.Key)]++
		rows = append(rows, placeRow{Place: p, Best: best})
	}
	// completion: of each category's boards, how many the player has a time on
	all, _ := s.store.Boards()
	totals := map[string]int{}
	for _, b := range all {
		totals[completionGroup(b.Key)]++
	}
	var completion []completionRow
	for _, g := range completionGroups {
		if totals[g.Key] == 0 {
			continue
		}
		completion = append(completion, completionRow{Label: g.Label, Badge: g.Badge, Done: mine[g.Key], Of: totals[g.Key], Percent: 100 * mine[g.Key] / totals[g.Key]})
	}
	// places by category, like the site's tabs: flagruns as one row per map with a cell per mode
	sort.Slice(rows, func(i, j int) bool { return rows[i].Key.MapLabel() < rows[j].Key.MapLabel() })
	var flagruns []playerFlagRow
	flagIndex := map[string]int{}
	races := placeSection{Title: "Races"}
	campaigns := placeSection{Title: "Campaigns"}
	for _, p := range rows {
		switch p.Key.Kind {
		case "flagrun":
			col := slices.Index(flagrunModes, p.Key.Mode)
			if col < 0 {
				continue
			}
			i, ok := flagIndex[p.Key.Map]
			if !ok {
				i = len(flagruns)
				flagIndex[p.Key.Map] = i
				flagruns = append(flagruns, playerFlagRow{Key: p.Key, Cells: make([]playerFlagCell, len(flagrunModes))})
			}
			pp := p
			flagruns[i].Cells[col].Place = &pp
		case "race":
			races.Places = append(races.Places, p)
		default:
			campaigns.Places = append(campaigns.Places, p)
		}
	}
	var sections []placeSection
	for _, sec := range []placeSection{races, campaigns} {
		if len(sec.Places) > 0 {
			sections = append(sections, sec)
		}
	}
	var flagModes []board.Key
	for _, m := range flagrunModes {
		flagModes = append(flagModes, board.Key{Kind: "flagrun", Mode: m})
	}
	recent, total, first, _ := s.store.PlayerRuns(id, recentRuns)
	render(w, playerPage, "player.html", map[string]any{"Title": name, "Tab": "", "Name": name, "Registered": s.store.Registered(id), "Places": rows, "Records": medals[1],
		"Podiums": medals[1] + medals[2] + medals[3], "Medals": medals, "Maps": len(maps), "Runs": total, "Since": first,
		"Completion": completion, "Recent": recent, "Flagruns": flagruns, "FlagModes": flagModes, "Sections": sections})
}

func (s *site) info(w http.ResponseWriter, r *http.Request) {
	render(w, infoPage, "info.html", map[string]any{"Title": "About", "Tab": "info", "Hub": s.hubAddr})
}

// replay sends a board entry's signed replay; runs that aren't on a board aren't public.
func (s *site) replay(w http.ResponseWriter, r *http.Request) {
	run, err := strconv.ParseInt(r.URL.Query().Get("run"), 10, 64)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	if ok, err := s.store.Listed(run); err != nil || !ok {
		http.NotFound(w, r)
		return
	}
	s.sendReplay(w, r, run)
}

func (s *site) sendReplay(w http.ResponseWriter, r *http.Request, run int64) {
	path, err := s.store.File(run)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(path)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	name := fmt.Sprintf("run-%d.sbr", run)
	if rows, err := s.store.Witness().Query("SELECT IFNULL(map, ''), IFNULL(runtimems, 0) FROM runs WHERE id = ?", run); err == nil && len(rows) == 1 && rows[0][0] != "" {
		ms, _ := strconv.ParseInt(rows[0][1], 10, 64)
		name = fmt.Sprintf("%s_%s_run%d.sbr", safeName(rows[0][0]), strings.NewReplacer(":", "-", ".", "-").Replace(board.Time(ms)), run)
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
	http.ServeContent(w, r, name, time.Time{}, f)
}

func safeName(s string) string {
	return strings.Map(func(c rune) rune {
		if c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || c == '-' {
			return c
		}
		return '_'
	}, s)
}

const placeholderSVG = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 320 180"><rect width="320" height="180" fill="#d1d5db"/><path d="M0 180 L110 90 L170 140 L230 70 L320 150 L320 180 Z" fill="#9ca3af"/></svg>`

// mapImage serves a map's picture: a chosen view if there is one, else the game's preview, else a
// placeholder.
func (s *site) mapImage(w http.ResponseWriter, r *http.Request) {
	name := strings.TrimPrefix(r.URL.Path, "/mapimg/")
	if name == "" || safeName(name) != name {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "public, max-age=3600")
	var candidates []string
	if s.shots != "" {
		candidates = append(candidates, filepath.Join(s.shots, name+".jpg"), filepath.Join(s.shots, name+".png"))
	}
	if s.previews != "" {
		candidates = append(candidates, filepath.Join(s.previews, name+".jpg"))
	}
	for _, p := range candidates {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			http.ServeFile(w, r, p)
			return
		}
	}
	w.Header().Set("Content-Type", "image/svg+xml")
	fmt.Fprint(w, placeholderSVG)
}

// how many rows the lists of the home and category pages have
const (
	categoryPanel = 8
	recentRuns    = 5
	homeRuns      = 10
)

// addCategory adds a category page's newest records, recent runs, top record holders and oldest
// standing records, all of the boards of one kind.
func (s *site) addCategory(data map[string]any, boards []*board.Board, kind string) {
	runs, _ := s.store.RecentRuns(100000)
	var recent []board.Run
	for _, r := range runs {
		if r.Key.Kind == kind && len(recent) < recentRuns {
			recent = append(recent, r)
		}
	}
	var oldest []board.Standing
	for _, b := range boards {
		oldest = append(oldest, board.Standing{Key: b.Key, Entry: b.Entries[0]})
	}
	holders := topHolders(boards, categoryPanel)
	sort.Slice(oldest, func(i, j int) bool { return oldest[i].Entry.When.Before(oldest[j].Entry.When) })
	if len(oldest) > recentRuns {
		oldest = oldest[:recentRuns]
	}
	if kind == "flagrun" {
		type modeHolders struct {
			Key     board.Key
			Holders []board.Holder
		}
		var split []modeHolders
		for _, m := range flagrunModes {
			var of []*board.Board
			for _, b := range boards {
				if b.Key.Mode == m {
					of = append(of, b)
				}
			}
			split = append(split, modeHolders{Key: board.Key{Kind: kind, Mode: m}, Holders: topHolders(of, 5)})
		}
		data["ModeHolders"] = split
	}
	more := map[string]string{"race": "race", "flagrun": "flagrun", "campaign": "sp"}[kind]
	data["Recent"], data["Holders"], data["Oldest"] = recent, holders, oldest
	data["More"] = "/records"
	if more != "" {
		data["More"] = "/records?mode=" + more
	}
}

// topHolders lists who holds the most of these boards' bests.
func topHolders(boards []*board.Board, n int) []board.Holder {
	counts, firsts := map[string]int{}, map[string]board.Entry{}
	for _, b := range boards {
		e := b.Entries[0]
		counts[e.Install]++
		firsts[e.Install] = e
	}
	var out []board.Holder
	for k, c := range counts {
		out = append(out, board.Holder{Install: k, Name: firsts[k].Name, Registered: firsts[k].Registered, Records: c})
	}
	sort.Slice(out, func(i, j int) bool {
		return out[i].Records > out[j].Records || out[i].Records == out[j].Records && out[i].Name < out[j].Name
	})
	if len(out) > n {
		out = out[:n]
	}
	return out
}

type completionRow struct {
	Label   string
	Badge   board.Key
	Done    int
	Of      int
	Percent int
}

// the categories a player's completion is shown for, each its own count of boards
var completionGroups = []struct {
	Key, Label string
	Badge      board.Key
}{
	{"race", "Races", board.Key{Kind: "race"}},
	{"ctf", "ctf", board.Key{Kind: "flagrun", Mode: 11}},
	{"ictf", "insta ctf", board.Key{Kind: "flagrun", Mode: 12}},
	{"ectf", "effic ctf", board.Key{Kind: "flagrun", Mode: 17}},
	{"sp", "Campaigns", board.Key{Kind: "campaign"}},
}

func completionGroup(k board.Key) string { return modeOf(k) }

type playerFlagCell struct{ Place *placeRow }

type playerFlagRow struct {
	Key   board.Key
	Cells []playerFlagCell
}

type placeSection struct {
	Title  string
	Places []placeRow
}
