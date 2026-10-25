package main

import (
	"embed"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"strings"
	"time"

	"speedbraten/server/board"
)

//go:embed templates
var templateFS embed.FS

var funcs = template.FuncMap{
	"runtime": board.Time,
	"gap":     func(ms, best int64) string { return board.Time(ms - best) },
	"date":    func(t time.Time) string { return t.UTC().Format("2006-01-02") },
	"boardURL": func(k board.Key) string {
		return "/board?" + url.Values{"map": {k.Map}, "kind": {k.Kind}, "mode": {strconv.Itoa(k.Mode)}}.Encode()
	},
	"playerURL": func(install string) string { return "/player?id=" + url.QueryEscape(install) },
}

func page(files ...string) *template.Template {
	paths := []string{"templates/layout.html", "templates/records.html"}
	for _, f := range files {
		paths = append(paths, "templates/"+f)
	}
	return template.Must(template.New(files[0]).Funcs(funcs).ParseFS(templateFS, paths...))
}

var (
	homePage    = page("home.html")
	boardPage   = page("board.html")
	playerPage  = page("player.html")
	recordsPage = page("records.html")
)

type site struct {
	store *board.Store
	mux   *http.ServeMux
}

func newSite(store *board.Store) *site {
	s := &site{store: store, mux: http.NewServeMux()}
	s.mux.HandleFunc("/", s.home)
	s.mux.HandleFunc("/board", s.board)
	s.mux.HandleFunc("/player", s.player)
	s.mux.HandleFunc("/records", s.records)
	s.mux.HandleFunc("/replay", s.replay)
	return s
}

func (s *site) ServeHTTP(w http.ResponseWriter, r *http.Request) { s.mux.ServeHTTP(w, r) }

func render(w http.ResponseWriter, t *template.Template, name string, data any) {
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
	boards, err := s.store.Boards()
	if err != nil {
		fail(w, err)
		return
	}
	records, err := s.store.Records(15)
	if err != nil {
		fail(w, err)
		return
	}
	render(w, homePage, "home.html", map[string]any{"Title": "Leaderboards", "Boards": boards, "Records": records})
}

func (s *site) records(w http.ResponseWriter, r *http.Request) {
	records, err := s.store.Records(200)
	if err != nil {
		fail(w, err)
		return
	}
	render(w, recordsPage, "recordspage", map[string]any{"Title": "Records", "Records": records})
}

func (s *site) board(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	mode, err := strconv.Atoi(q.Get("mode"))
	if err != nil {
		http.NotFound(w, r)
		return
	}
	b, err := s.store.Board(board.Key{Map: q.Get("map"), Kind: q.Get("kind"), Mode: mode})
	if err != nil {
		fail(w, err)
		return
	}
	if b == nil {
		http.NotFound(w, r)
		return
	}
	render(w, boardPage, "board.html", map[string]any{"Title": b.Key.MapLabel() + " (" + b.Key.KindLabel() + ")", "Board": b})
}

func (s *site) player(w http.ResponseWriter, r *http.Request) {
	id := r.URL.Query().Get("id")
	places, name, err := s.store.Player(id)
	if err != nil {
		fail(w, err)
		return
	}
	if len(places) == 0 {
		http.NotFound(w, r)
		return
	}
	render(w, playerPage, "player.html", map[string]any{"Title": name, "Name": name, "Places": places})
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
