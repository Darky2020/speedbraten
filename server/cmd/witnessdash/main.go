// witnessdash serves the witness's review page over HTTPS: runs the witness flagged because
// their aim matches an earlier run of another install, next to that run, with both replays,
// to mark as reviewed. The flags are kept from players; this page is the only place they show.
//
//	witnessdash setpass <password file>
//	    reads a password on stdin and writes its bcrypt hash (mode 0600)
//	witnessdash -d <witness data dir> -pass <password file> -cert <file> -key <file> [-listen :8791]
//	    serves the page; any user name, that password (HTTP basic auth)
package main

import (
	"bufio"
	"crypto/rand"
	"crypto/subtle"
	"crypto/tls"
	"encoding/hex"
	"flag"
	"fmt"
	"html/template"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/bcrypt"

	"speedbraten/server/sqlite"
)

const (
	maxFailures = 5
	lockout     = 15 * time.Minute
)

type server struct {
	db      *sqlite.DB
	hash    []byte
	token   string
	mu      sync.Mutex
	fails   map[string]*failure
	now     func() time.Time
	handler http.Handler
}

type failure struct {
	count int
	until time.Time
}

func newServer(db *sqlite.DB, hash []byte) *server {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	s := &server{db: db, hash: hash, token: hex.EncodeToString(b), fails: map[string]*failure{}, now: time.Now}
	mux := http.NewServeMux()
	mux.HandleFunc("/", s.index)
	mux.HandleFunc("/file", s.file)
	mux.HandleFunc("/review", s.review)
	s.handler = s.auth(mux)
	return s
}

func clientIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *server) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Cache-Control", "no-store")
		ip := clientIP(r)
		s.mu.Lock()
		f := s.fails[ip]
		locked := f != nil && f.count >= maxFailures && s.now().Before(f.until)
		s.mu.Unlock()
		if locked {
			http.Error(w, "too many failed logins, try again later", http.StatusTooManyRequests)
			return
		}
		_, pass, ok := r.BasicAuth()
		if !ok || bcrypt.CompareHashAndPassword(s.hash, []byte(pass)) != nil {
			if ok {
				s.mu.Lock()
				if f = s.fails[ip]; f == nil || s.now().After(f.until) {
					f = &failure{}
					s.fails[ip] = f
				}
				f.count++
				f.until = s.now().Add(lockout)
				s.mu.Unlock()
			}
			w.Header().Set("WWW-Authenticate", `Basic realm="witness review"`)
			http.Error(w, "login required", http.StatusUnauthorized)
			return
		}
		s.mu.Lock()
		delete(s.fails, ip)
		s.mu.Unlock()
		next.ServeHTTP(w, r)
	})
}

type runInfo struct {
	ID, Map, Player, Started, Verdict, Install string
}

type flagRow struct {
	ID, Created, Matched, Total, Note string
	Reviewed                          bool
	Run, Other                        runInfo
}

func when(ms string) string {
	v, err := strconv.ParseInt(ms, 10, 64)
	if err != nil || v == 0 {
		return ""
	}
	return time.UnixMilli(v).UTC().Format("2006-01-02 15:04:05 UTC")
}

func short(key string) string {
	if len(key) > 8 {
		return key[:8]
	}
	return key
}

var page = template.Must(template.New("page").Parse(`<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Witness review</title>
<style>
body { font: 14px system-ui, sans-serif; margin: 16px; background: #fff; color: #111; }
table { border-collapse: collapse; width: 100%; }
th, td { border-bottom: 1px solid #ddd; padding: 6px; text-align: left; vertical-align: top; }
tr.done { color: #888; }
input[type=text] { width: 14em; }
@media (prefers-color-scheme: dark) { body { background: #111; color: #eee; } th, td { border-color: #333; } tr.done { color: #777; } }
</style></head><body>
<h1>Flagged runs</h1>
<p>{{len .Flags}} flags, {{.Open}} not reviewed. A flagged run's aim repeats an earlier run of another install.</p>
<table>
<tr><th>Flag</th><th>Run</th><th>Matches</th><th>Fingerprints</th><th>Review</th></tr>
{{range .Flags}}<tr{{if .Reviewed}} class="done"{{end}}>
<td>#{{.ID}}<br>{{.Created}}</td>
<td>run {{.Run.ID}}: {{.Run.Map}}<br>{{.Run.Player}} ({{.Run.Install}})<br>{{.Run.Started}}<br>verdict: {{.Run.Verdict}}<br><a href="/file?run={{.Run.ID}}">replay</a></td>
<td>run {{.Other.ID}}: {{.Other.Map}}<br>{{.Other.Player}} ({{.Other.Install}})<br>{{.Other.Started}}<br>verdict: {{.Other.Verdict}}<br><a href="/file?run={{.Other.ID}}">replay</a></td>
<td>{{.Matched}} of {{.Total}}</td>
<td><form method="post" action="/review"><input type="hidden" name="token" value="{{$.Token}}"><input type="hidden" name="id" value="{{.ID}}">
<input type="text" name="note" value="{{.Note}}" placeholder="note"> <button>{{if .Reviewed}}update{{else}}mark reviewed{{end}}</button></form></td>
</tr>{{end}}
</table></body></html>
`))

func (s *server) index(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	rows, err := s.db.Query(`SELECT f.id, f.created, f.matched, f.total, f.reviewed, coalesce(f.note, ''),
		r.id, coalesce(r.map, ''), coalesce(r.player, ''), r.started, coalesce(r.verdict, ''), coalesce(r.installkey, ''),
		coalesce(o.id, ''), coalesce(o.map, ''), coalesce(o.player, ''), coalesce(o.started, ''), coalesce(o.verdict, ''), coalesce(o.installkey, '')
		FROM flags f JOIN runs r ON r.id = f.run LEFT JOIN runs o ON o.id = f.other
		ORDER BY f.reviewed, f.id DESC LIMIT 1000`)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	data := struct {
		Flags []flagRow
		Open  int
		Token string
	}{Token: s.token}
	for _, v := range rows {
		f := flagRow{ID: v[0], Created: when(v[1]), Matched: v[2], Total: v[3], Reviewed: v[4] == "1", Note: v[5],
			Run:   runInfo{v[6], v[7], v[8], when(v[9]), v[10], short(v[11])},
			Other: runInfo{v[12], v[13], v[14], when(v[15]), v[16], short(v[17])}}
		if !f.Reviewed {
			data.Open++
		}
		data.Flags = append(data.Flags, f)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := page.Execute(w, data); err != nil {
		log.Print(err)
	}
}

func (s *server) file(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.URL.Query().Get("run"), 10, 64)
	if err != nil {
		http.Error(w, "bad run", http.StatusBadRequest)
		return
	}
	rows, err := s.db.Query("SELECT file FROM runs WHERE id = ?", id)
	if err != nil || len(rows) == 0 || rows[0][0] == "" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", fmt.Sprintf("attachment; filename=%q", "run-"+strconv.FormatInt(id, 10)+"-"+filepath.Base(rows[0][0])))
	http.ServeFile(w, r, rows[0][0])
}

func (s *server) review(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	if subtle.ConstantTimeCompare([]byte(r.FormValue("token")), []byte(s.token)) != 1 {
		http.Error(w, "stale page, reload it", http.StatusForbidden)
		return
	}
	id, err := strconv.ParseInt(r.FormValue("id"), 10, 64)
	if err != nil {
		http.Error(w, "bad flag", http.StatusBadRequest)
		return
	}
	note := r.FormValue("note")
	if len(note) > 1000 {
		note = note[:1000]
	}
	if _, err := s.db.Exec("UPDATE flags SET reviewed = 1, note = ? WHERE id = ?", note, id); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

func setpass(file string) {
	fmt.Fprint(os.Stderr, "password: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	pass := strings.TrimRight(line, "\r\n")
	if (err != nil && line == "") || len(pass) < 12 {
		log.Fatal("a password of at least 12 characters is needed")
	}
	hash, err := bcrypt.GenerateFromPassword([]byte(pass), 12)
	if err != nil {
		log.Fatal(err)
	}
	if err := os.WriteFile(file, append(hash, '\n'), 0600); err != nil {
		log.Fatal(err)
	}
}

func main() {
	if len(os.Args) == 3 && os.Args[1] == "setpass" {
		setpass(os.Args[2])
		return
	}
	dir := flag.String("d", "", "witness data dir (holds witness.db)")
	passfile := flag.String("pass", "", "password hash file (witnessdash setpass)")
	cert := flag.String("cert", "", "TLS certificate file")
	key := flag.String("key", "", "TLS key file")
	listen := flag.String("listen", ":8791", "address to listen on")
	flag.Parse()
	if *dir == "" || *passfile == "" || *cert == "" || *key == "" {
		flag.Usage()
		os.Exit(2)
	}
	hash, err := os.ReadFile(*passfile)
	if err != nil {
		log.Fatal(err)
	}
	db, err := sqlite.Open(filepath.Join(*dir, "witness.db"))
	if err != nil {
		log.Fatal(err)
	}
	s := newServer(db, []byte(strings.TrimSpace(string(hash))))
	srv := &http.Server{
		Addr:              *listen,
		Handler:           s.handler,
		TLSConfig:         &tls.Config{MinVersion: tls.VersionTLS12},
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      60 * time.Second,
		IdleTimeout:       120 * time.Second,
	}
	log.Printf("witness review on https://%s", *listen)
	log.Fatal(srv.ListenAndServeTLS(*cert, *key))
}
