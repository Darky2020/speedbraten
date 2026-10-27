package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/bcrypt"

	"speedbraten/server/board"
)

const (
	maxFailures = 5
	lockout     = 15 * time.Minute
)

var runsPerPage = 100

var (
	runsAdmin       = page("admin_runs.html")
	flagsAdmin      = page("admin_flags.html")
	moderationAdmin = page("admin_moderation.html")
	legacyAdmin     = page("admin_legacy.html")
	playersAdmin    = page("admin_players.html")
	playerAdmin     = page("admin_player.html")
	serverAdmin     = page("admin_server.html")
)

// admin is the dashboard under /admin: HTTP basic auth against a bcrypt hash (any user name), with
// a lockout per address after repeated failures, and a per-process token on every form.
type admin struct {
	store      *board.Store
	hash       []byte
	rejudgeDir string // the witness's <data dir>/rejudge
	disk       *diskWatch
	token      string
	now        func() time.Time

	mu    sync.Mutex
	fails map[string]*failure
}

type failure struct {
	count int
	until time.Time
}

func newAdmin(store *board.Store, hash []byte, rejudgeDir string) *admin {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return &admin{store: store, hash: hash, rejudgeDir: rejudgeDir, token: hex.EncodeToString(b), now: time.Now, fails: map[string]*failure{}}
}

func (a *admin) register(mux *http.ServeMux) {
	mux.Handle("/admin", a.auth(http.RedirectHandler("/admin/runs", http.StatusSeeOther)))
	mux.Handle("/admin/runs", a.auth(http.HandlerFunc(a.runs)))
	mux.Handle("/admin/flags", a.auth(http.HandlerFunc(a.flags)))
	mux.Handle("/admin/moderation", a.auth(http.HandlerFunc(a.moderation)))
	mux.Handle("/admin/legacy", a.auth(http.HandlerFunc(a.legacy)))
	mux.Handle("/admin/players", a.auth(http.HandlerFunc(a.players)))
	mux.Handle("/admin/player", a.auth(http.HandlerFunc(a.player)))
	mux.Handle("/admin/server", a.auth(http.HandlerFunc(a.server)))
	mux.Handle("/admin/file", a.auth(http.HandlerFunc(a.file)))
	mux.Handle("/admin/act", a.auth(http.HandlerFunc(a.act)))
}

// clientIP is the connecting address, or the one nginx passes in X-Real-IP when the connection
// comes from this machine.
func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
		if real := r.Header.Get("X-Real-IP"); real != "" {
			return real
		}
	}
	return host
}

func (a *admin) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Cache-Control", "no-store")
		ip := clientIP(r)
		a.mu.Lock()
		f := a.fails[ip]
		locked := f != nil && f.count >= maxFailures && a.now().Before(f.until)
		a.mu.Unlock()
		if locked {
			http.Error(w, "too many failed logins, try again later", http.StatusTooManyRequests)
			return
		}
		_, pass, ok := r.BasicAuth()
		if !ok || bcrypt.CompareHashAndPassword(a.hash, []byte(pass)) != nil {
			if ok {
				a.mu.Lock()
				if f = a.fails[ip]; f == nil || a.now().After(f.until) {
					f = &failure{}
					a.fails[ip] = f
				}
				f.count++
				f.until = a.now().Add(lockout)
				a.mu.Unlock()
			}
			w.Header().Set("WWW-Authenticate", `Basic realm="speedbraten admin"`)
			http.Error(w, "login required", http.StatusUnauthorized)
			return
		}
		a.mu.Lock()
		delete(a.fails, ip)
		a.mu.Unlock()
		next.ServeHTTP(w, r)
	})
}

func when(ms string) string {
	v, err := strconv.ParseInt(ms, 10, 64)
	if err != nil || v == 0 {
		return ""
	}
	return time.UnixMilli(v).UTC().Format("2006-01-02 15:04")
}

type runRow struct {
	ID, Started, Install, Player, Name, Map, Kind, Length, Verdict, Reason, Stop, Lag string
	Violations                                                                        int64
	Finished, Deleted, Banned                                                         bool
}

type runFilter struct {
	Player, Map, Verdict, Kind, Mode, From, To, Problems, Lag, Show, Sort string
	Page                                                                  int
}

var (
	runVerdicts = []option{{"", "any verdict"}, {"valid", "valid"}, {"rejected", "rejected"}, {"unverified", "unverified"}, {"none", "no verdict"}, {"unfinished", "unfinished"}}
	runKinds    = []option{{"", "any kind"}, {"flagrun", "flagrun"}, {"race", "race"}, {"campaign", "campaign"}, {"dmsp", "dmsp"}}
	runModes    = []option{{"", "any mode"}, {"11", "ctf"}, {"12", "insta ctf"}, {"17", "effic ctf"}, {"0", "ffa"}, {"1", "coop edit"}, {"-3", "SP"}, {"-2", "DMSP"}}
	runProblems = []option{{"", "any problems"}, {"violations", "with violations"}, {"lag", "lag over"}}
	runShows    = []option{{"", "deleted + banned: show"}, {"hide", "deleted + banned: hide"}, {"deleted", "only deleted"}, {"banned", "only banned"}}
	runSorts    = []option{{"", "newest"}, {"old", "oldest"}, {"fast", "fastest"}, {"slow", "slowest"}, {"lag", "most lag"}, {"viol", "most violations"}}
	runOrders   = map[string]string{
		"":     "id DESC",
		"old":  "id",
		"fast": "IFNULL(finished, 0) = 0, runtimems IS NULL, runtimems, id DESC",
		"slow": "IFNULL(finished, 0) = 0, runtimems IS NULL, runtimems DESC, id DESC",
		"lag":  "IFNULL(maxlagms, 0) DESC, id DESC",
		"viol": "IFNULL(violations, 0) DESC, id DESC",
	}
)

func pick(opts []option, v string) string {
	if hasOption(opts, v) {
		return v
	}
	return ""
}

// inList is "column IN (?,?..)" over the values, or a false condition for none.
func inList(column string, values []string, args *[]any) string {
	if len(values) == 0 {
		return "0"
	}
	for _, v := range values {
		*args = append(*args, v)
	}
	return column + " IN (" + strings.TrimSuffix(strings.Repeat("?,", len(values)), ",") + ")"
}

func (f runFilter) query(page int) string {
	v := url.Values{}
	for k, s := range map[string]string{"player": f.Player, "map": f.Map, "verdict": f.Verdict, "kind": f.Kind, "mode": f.Mode, "from": f.From, "to": f.To,
		"problems": f.Problems, "lag": f.Lag, "show": f.Show, "sort": f.Sort} {
		if s != "" {
			v.Set(k, s)
		}
	}
	if page > 0 {
		v.Set("page", strconv.Itoa(page))
	}
	return "/admin/runs?" + v.Encode()
}

func (a *admin) runs(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	f := runFilter{Player: strings.TrimSpace(q.Get("player")), Map: strings.TrimSpace(q.Get("map")), Verdict: pick(runVerdicts, q.Get("verdict")),
		Kind: pick(runKinds, q.Get("kind")), Mode: pick(runModes, q.Get("mode")), From: q.Get("from"), To: q.Get("to"),
		Problems: pick(runProblems, q.Get("problems")), Lag: strings.TrimSpace(q.Get("lag")), Show: pick(runShows, q.Get("show")), Sort: pick(runSorts, q.Get("sort"))}
	f.Page, _ = strconv.Atoi(q.Get("page"))
	f.Page = max(f.Page, 0)
	where, args := []string{"1"}, []any{}
	if f.Player != "" {
		// the player's own keys, or every install whose name contains the text
		var keys []string
		for _, k := range append([]string{f.Player}, a.store.Installs(f.Player)...) {
			keys = append(keys, a.store.Keys(a.store.Canonical(k))...)
		}
		where = append(where, inList("installkey", keys, &args))
	}
	if f.Map != "" {
		where = append(where, "map = ?")
		args = append(args, f.Map)
	}
	switch f.Verdict {
	case "valid", "rejected", "unverified":
		where = append(where, "verdict = ?")
		args = append(args, f.Verdict)
	case "none":
		where = append(where, "verdict IS NULL AND IFNULL(finished, 0) = 1")
	case "unfinished":
		where = append(where, "IFNULL(finished, 0) = 0")
	}
	if f.Kind != "" {
		where = append(where, "runkind = ?")
		args = append(args, f.Kind)
	}
	if f.Mode != "" {
		mode, _ := strconv.Atoi(f.Mode)
		where = append(where, "mode = ?")
		args = append(args, mode)
	}
	// dates are UTC days, like the started column shown
	if t, err := time.Parse("2006-01-02", f.From); err == nil {
		where = append(where, "IFNULL(started, 0) >= ?")
		args = append(args, t.UnixMilli())
	} else {
		f.From = ""
	}
	if t, err := time.Parse("2006-01-02", f.To); err == nil {
		where = append(where, "IFNULL(started, 0) < ?")
		args = append(args, t.AddDate(0, 0, 1).UnixMilli())
	} else {
		f.To = ""
	}
	switch f.Problems {
	case "violations":
		where = append(where, "IFNULL(violations, 0) > 0")
	case "lag":
		lag, err := strconv.Atoi(f.Lag)
		if err != nil || lag < 0 {
			lag, f.Lag = 500, "500"
		}
		where = append(where, "IFNULL(maxlagms, 0) > ?")
		args = append(args, lag)
	}
	if f.Show != "" {
		var removed, banned []string
		if notes, err := a.store.RemovedRuns(); err == nil {
			for _, n := range notes {
				removed = append(removed, n.ID)
			}
		}
		if notes, err := a.store.BannedInstalls(); err == nil {
			for _, n := range notes {
				banned = append(banned, a.store.Keys(a.store.Canonical(n.ID))...)
			}
		}
		switch f.Show {
		case "deleted":
			where = append(where, inList("id", removed, &args))
		case "banned":
			where = append(where, inList("installkey", banned, &args))
		case "hide":
			where = append(where, "NOT "+inList("id", removed, &args), "NOT "+inList("installkey", banned, &args))
		}
	}
	rows, err := a.store.Witness().Query(`SELECT id, IFNULL(started, 0), IFNULL(installkey, ''), IFNULL(map, ''), IFNULL(runkind, ''), IFNULL(mode, 0),
		IFNULL(runtimems, 0), IFNULL(verdict, ''), IFNULL(reason, ''), IFNULL(stop, IFNULL(refused, '')), IFNULL(violations, 0), IFNULL(maxlagms, 0),
		IFNULL(finished, 0), IFNULL(ended, 0)
		FROM runs WHERE `+strings.Join(where, " AND ")+` ORDER BY `+runOrders[f.Sort]+` LIMIT `+strconv.Itoa(runsPerPage+1)+` OFFSET `+strconv.Itoa(f.Page*runsPerPage), args...)
	if err != nil {
		fail(w, err)
		return
	}
	more := len(rows) > runsPerPage
	if more {
		rows = rows[:runsPerPage]
	}
	banned := map[string]bool{}
	if notes, err := a.store.BannedInstalls(); err == nil {
		for _, n := range notes {
			banned[a.store.Canonical(n.ID)] = true
		}
	}
	var list []runRow
	for _, v := range rows {
		id, _ := strconv.ParseInt(v[0], 10, 64)
		mode, _ := strconv.Atoi(v[5])
		ms, _ := strconv.ParseInt(v[6], 10, 64)
		viol, _ := strconv.ParseInt(v[10], 10, 64)
		started, _ := strconv.ParseInt(v[1], 10, 64)
		ended, _ := strconv.ParseInt(v[13], 10, 64)
		player := a.store.Canonical(v[2])
		k := board.Key{Map: v[3], Kind: v[4], Mode: mode}
		rr := runRow{ID: v[0], Started: when(v[1]), Install: v[2], Player: player, Name: a.store.Name(player), Map: k.MapLabel(), Kind: k.KindLabel(),
			Verdict: v[7], Reason: v[8], Stop: v[9], Violations: viol, Lag: v[11], Finished: v[12] == "1",
			Deleted: a.store.IsRemoved(id), Banned: banned[player]}
		if rr.Name == "" {
			rr.Name = shortKey(player)
		}
		switch {
		case ms > 0:
			rr.Length = board.Time(ms)
		case ended > started:
			rr.Length = board.Time(ended - started)
		default:
			rr.Length = "—"
		}
		list = append(list, rr)
	}
	prev, next := "", ""
	if f.Page > 0 {
		prev = f.query(f.Page - 1)
	}
	if more {
		next = f.query(f.Page + 1)
	}
	filtered := f != runFilter{Page: f.Page}
	render(w, runsAdmin, "admin_runs.html", map[string]any{"Title": "Runs", "Tab": "runs", "Admin": true, "Token": a.token, "F": f, "Runs": list,
		"Prev": prev, "Next": next, "PageNo": f.Page + 1, "Filtered": filtered,
		"Verdicts": runVerdicts, "Kinds": runKinds, "Modes": runModes, "Problems": runProblems, "Shows": runShows, "Sorts": runSorts})
}

func (a *admin) players(w http.ResponseWriter, r *http.Request) {
	all, err := a.store.Players()
	if err != nil {
		fail(w, err)
		return
	}
	q := strings.ToLower(strings.TrimSpace(r.URL.Query().Get("q")))
	var shown []board.PlayerSummary
	for _, p := range all {
		if q == "" || strings.Contains(strings.ToLower(p.Name), q) || strings.Contains(strings.ToLower(strings.Join(p.Aliases, " ")), q) || strings.Contains(strings.ToLower(strings.Join(p.Keys, " ")), q) {
			shown = append(shown, p)
		}
	}
	render(w, playersAdmin, "admin_players.html", map[string]any{"Title": "Players", "Tab": "players", "Admin": true, "Token": a.token, "Players": shown, "Q": r.URL.Query().Get("q")})
}

func (a *admin) player(w http.ResponseWriter, r *http.Request) {
	id := a.store.Canonical(r.URL.Query().Get("id"))
	all, err := a.store.Players()
	if err != nil {
		fail(w, err)
		return
	}
	var me *board.PlayerSummary
	var others []board.PlayerSummary
	for i, p := range all {
		if p.Player == id {
			me = &all[i]
		} else {
			others = append(others, p)
		}
	}
	if me == nil {
		http.NotFound(w, r)
		return
	}
	sort.Slice(others, func(i, j int) bool { return strings.ToLower(others[i].Name) < strings.ToLower(others[j].Name) })
	picked := a.store.PickedName(id)
	render(w, playerAdmin, "admin_player.html", map[string]any{"Title": me.Name, "Tab": "players", "Admin": true, "Token": a.token, "P": me, "Others": others,
		"Picked": picked, "Default": a.store.DefaultName(id)})
}

func (a *admin) server(w http.ResponseWriter, r *http.Request) {
	var d *diskReport
	if a.disk != nil {
		d = a.disk.report()
	}
	render(w, serverAdmin, "admin_server.html", map[string]any{"Title": "Server", "Tab": "server", "Admin": true, "Token": a.token, "D": d})
}

func shortKey(k string) string {
	if len(k) > 12 {
		return k[:12]
	}
	return k
}

type flagRun struct{ ID, Map, Name, Verdict string }

type flagRow struct {
	ID, Created, Matched, Total, Note string
	Reviewed                          bool
	Run, Other                        flagRun
}

func (a *admin) flags(w http.ResponseWriter, r *http.Request) {
	rows, err := a.store.Witness().Query(`SELECT f.id, f.created, f.matched, f.total, f.reviewed, IFNULL(f.note, ''),
		r.id, IFNULL(r.map, ''), IFNULL(r.installkey, ''), IFNULL(r.verdict, ''),
		IFNULL(o.id, ''), IFNULL(o.map, ''), IFNULL(o.installkey, ''), IFNULL(o.verdict, '')
		FROM flags f JOIN runs r ON r.id = f.run LEFT JOIN runs o ON o.id = f.other ORDER BY f.id DESC LIMIT 1000`)
	if err != nil {
		fail(w, err)
		return
	}
	reviews, err := a.store.FlagReviews()
	if err != nil {
		fail(w, err)
		return
	}
	name := func(k string) string {
		if n := a.store.Name(k); n != "" {
			return n
		}
		return shortKey(k)
	}
	var list, done []flagRow
	open := 0
	for _, v := range rows {
		f := flagRow{ID: v[0], Created: when(v[1]), Matched: v[2], Total: v[3], Reviewed: v[4] == "1", Note: v[5],
			Run: flagRun{v[6], v[7], name(v[8]), v[9]}, Other: flagRun{v[10], v[11], name(v[12]), v[13]}}
		if rv, ok := reviews[f.ID]; ok {
			f.Reviewed, f.Note = true, rv.Note
		}
		if f.Reviewed {
			done = append(done, f)
		} else {
			open++
			list = append(list, f)
		}
	}
	render(w, flagsAdmin, "admin_flags.html", map[string]any{"Title": "Copy flags", "Tab": "flags", "Admin": true, "Token": a.token, "Flags": append(list, done...), "Open": open})
}

type bannedRow struct {
	ID, Name, Note, At string
}

func (a *admin) legacy(w http.ResponseWriter, r *http.Request) {
	list, err := a.store.LegacyRecords()
	if err != nil {
		fail(w, err)
		return
	}
	render(w, legacyAdmin, "admin_legacy.html", map[string]any{"Title": "Legacy records", "Tab": "legacy", "Admin": true, "Token": a.token, "Legacy": list,
		"Kinds": runKinds[1:], "Modes": runModes[1:], "Sources": board.LegacySources})
}

func (a *admin) moderation(w http.ResponseWriter, r *http.Request) {
	removed, err := a.store.RemovedRuns()
	if err != nil {
		fail(w, err)
		return
	}
	banned, err := a.store.BannedInstalls()
	if err != nil {
		fail(w, err)
		return
	}
	var rem, ban []bannedRow
	for _, n := range removed {
		rem = append(rem, bannedRow{ID: n.ID, Note: n.Note, At: n.At.UTC().Format("2006-01-02 15:04")})
	}
	for _, n := range banned {
		ban = append(ban, bannedRow{ID: n.ID, Name: a.store.Name(n.ID), Note: n.Note, At: n.At.UTC().Format("2006-01-02 15:04")})
	}
	render(w, moderationAdmin, "admin_moderation.html", map[string]any{"Title": "Moderation", "Tab": "moderation", "Admin": true, "Token": a.token, "Removed": rem, "Banned": ban})
}

func (a *admin) file(w http.ResponseWriter, r *http.Request) {
	run, err := strconv.ParseInt(r.URL.Query().Get("run"), 10, 64)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	(&site{store: a.store}).sendReplay(w, r, run)
}

// act carries out a dashboard form: remove/restore a run, ban/unban an install, re-judge a run,
// review a copy flag. Every form carries the page token.
func (a *admin) act(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if subtle.ConstantTimeCompare([]byte(r.FormValue("token")), []byte(a.token)) != 1 {
		http.Error(w, "this page is out of date: reload it", http.StatusForbidden)
		return
	}
	note := r.FormValue("note")
	if len(note) > 500 {
		note = note[:500]
	}
	run, _ := strconv.ParseInt(r.FormValue("run"), 10, 64)
	install := strings.TrimSpace(r.FormValue("install"))
	var err error
	back := r.Referer()
	switch r.FormValue("do") {
	case "remove":
		err = a.store.Remove(run, note)
	case "restore":
		err = a.store.Restore(run)
	case "ban":
		if install == "" {
			http.Error(w, "which install?", http.StatusBadRequest)
			return
		}
		err = a.store.Ban(install, note)
	case "unban":
		err = a.store.Unban(install)
	case "rejudge":
		err = a.rejudge(run)
	case "rename":
		name := strings.TrimSpace(r.FormValue("name"))
		if len(name) > 64 {
			name = name[:64]
		}
		err = a.store.SetName(install, name)
	case "merge":
		if install == "" {
			http.Error(w, "pick a player to merge in", http.StatusBadRequest)
			return
		}
		err = a.store.Merge(install, strings.TrimSpace(r.FormValue("into")))
	case "split":
		err = a.store.Split(install)
	case "review":
		flag, _ := strconv.ParseInt(r.FormValue("flag"), 10, 64)
		err = a.store.ReviewFlag(flag, note)
	case "legacyset", "legacydelete":
		mode, ok := board.ParseMode(r.FormValue("mode"))
		k := board.Key{Map: strings.TrimSpace(r.FormValue("map")), Kind: r.FormValue("kind"), Mode: mode}
		if !ok {
			http.Error(w, "unknown mode", http.StatusBadRequest)
			return
		}
		if r.FormValue("do") == "legacydelete" {
			err = a.store.DeleteLegacy(k)
			break
		}
		ms, terr := board.ParseTime(r.FormValue("time"))
		if terr != nil {
			http.Error(w, terr.Error(), http.StatusBadRequest)
			return
		}
		err = a.store.SetLegacy(board.Legacy{Key: k, Player: strings.TrimSpace(r.FormValue("player")), MS: ms, Source: strings.TrimSpace(r.FormValue("source")),
			Date: strings.TrimSpace(r.FormValue("date")), Link: strings.TrimSpace(r.FormValue("link"))})
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
	case "legacyimport":
		list, perr := board.ParseLegacyCSV(r.FormValue("csv"))
		if perr != nil {
			http.Error(w, perr.Error()+" (nothing was imported)", http.StatusBadRequest)
			return
		}
		for _, l := range list {
			if err = a.store.SetLegacy(l); err != nil {
				break
			}
		}
	default:
		http.Error(w, "unknown action", http.StatusBadRequest)
		return
	}
	if err != nil {
		fail(w, err)
		return
	}
	http.Redirect(w, r, backTo(back), http.StatusSeeOther)
}

// backTo is the dashboard page a form came from, else the runs page.
func backTo(referer string) string {
	if u, err := url.Parse(referer); err == nil && strings.HasPrefix(u.Path, "/admin/") {
		return u.RequestURI()
	}
	return "/admin/runs"
}

// rejudge asks the witness to judge a run again: it picks up a file named after the run.
func (a *admin) rejudge(run int64) error {
	if a.rejudgeDir == "" {
		return fmt.Errorf("re-judging needs -rejudgedir")
	}
	if run <= 0 {
		return fmt.Errorf("bad run")
	}
	if err := os.MkdirAll(a.rejudgeDir, 0755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(a.rejudgeDir, strconv.FormatInt(run, 10)), nil, 0644)
}
