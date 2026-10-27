package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/bcrypt"
)

const password = "correct horse battery"

type adminFixture struct {
	t     *testing.T
	s     *site
	a     *admin
	token string
	dir   string
}

func newAdminFixture(t *testing.T) *adminFixture {
	store, dir := fixture(t)
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.MinCost)
	if err != nil {
		t.Fatal(err)
	}
	s := newSite(store)
	a := newAdmin(store, hash, filepath.Join(dir, "rejudge"))
	a.register(s.mux)
	return &adminFixture{t: t, s: s, a: a, token: a.token, dir: dir}
}

func (f *adminFixture) do(method, target, pass string, form url.Values, ip string) *httptest.ResponseRecorder {
	body := strings.NewReader("")
	if form != nil {
		body = strings.NewReader(form.Encode())
	}
	r := httptest.NewRequest(method, target, body)
	if form != nil {
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if pass != "" {
		r.SetBasicAuth("admin", pass)
	}
	r.RemoteAddr = ip + ":1234"
	w := httptest.NewRecorder()
	f.s.ServeHTTP(w, r)
	return w
}

func (f *adminFixture) act(form url.Values) *httptest.ResponseRecorder {
	f.t.Helper()
	form.Set("token", f.token)
	w := f.do("POST", "/admin/act", password, form, "10.0.0.1")
	if w.Code != http.StatusSeeOther {
		f.t.Fatalf("act %v: %d %s", form, w.Code, w.Body.String())
	}
	return w
}

func TestAdminLogin(t *testing.T) {
	f := newAdminFixture(t)
	if w := f.do("GET", "/admin/runs", "", nil, "10.0.0.1"); w.Code != http.StatusUnauthorized {
		t.Fatalf("no login: %d", w.Code)
	}
	if w := f.do("GET", "/admin/runs", password, nil, "10.0.0.1"); w.Code != http.StatusOK {
		t.Fatalf("right password: %d", w.Code)
	}
	for i := 0; i < maxFailures; i++ {
		f.do("GET", "/admin/runs", "wrong", nil, "10.0.0.2")
	}
	if w := f.do("GET", "/admin/runs", password, nil, "10.0.0.2"); w.Code != http.StatusTooManyRequests {
		t.Fatalf("after %d failures: %d", maxFailures, w.Code)
	}
	if w := f.do("GET", "/admin/runs", password, nil, "10.0.0.3"); w.Code != http.StatusOK {
		t.Fatalf("another address: %d", w.Code)
	}
	// behind nginx every request comes from loopback: the lockout goes by X-Real-IP
	r := httptest.NewRequest("GET", "/admin/runs", nil)
	r.RemoteAddr = "127.0.0.1:5555"
	r.Header.Set("X-Real-IP", "10.0.0.2")
	if got := clientIP(r); got != "10.0.0.2" {
		t.Fatalf("client ip behind the proxy %q", got)
	}
	r.RemoteAddr = "10.9.9.9:5555"
	if got := clientIP(r); got != "10.9.9.9" {
		t.Fatalf("X-Real-IP from a non-local peer was trusted: %q", got)
	}
	// public pages need no login
	if w := f.do("GET", "/", "", nil, "10.0.0.1"); w.Code != http.StatusOK {
		t.Fatalf("public page: %d", w.Code)
	}
}

func TestAdminRuns(t *testing.T) {
	f := newAdminFixture(t)
	w := f.do("GET", "/admin/runs", password, nil, "10.0.0.1")
	body := w.Body.String()
	if !strings.Contains(body, "the replay desynced at tick 50") || strings.Contains(body, "<script>bob") {
		t.Fatalf("runs page: reason missing or a name not escaped")
	}
	w = f.do("GET", "/admin/runs?verdict=rejected", password, nil, "10.0.0.1")
	if n := len(regexp.MustCompile(`<td class="num">\d+</td>`).FindAllString(w.Body.String(), -1)); n != 1 {
		t.Fatalf("rejected filter: %d rows", n)
	}
	w = f.do("GET", "/admin/runs?player=alice", password, nil, "10.0.0.1")
	if n := len(regexp.MustCompile(`<td class="num">\d+</td>`).FindAllString(w.Body.String(), -1)); n != 1 {
		t.Fatalf("player filter by name: %d rows", n)
	}
	rows := func(target string) []string {
		t.Helper()
		w := f.do("GET", target, password, nil, "10.0.0.1")
		if w.Code != http.StatusOK {
			t.Fatalf("%s: %d", target, w.Code)
		}
		var ids []string
		for _, m := range regexp.MustCompile(`<td class="num">(\d+)</td>`).FindAllStringSubmatch(w.Body.String(), -1) {
			ids = append(ids, m[1])
		}
		return ids
	}
	for target, want := range map[string]string{
		"/admin/runs?kind=campaign":                    "4",
		"/admin/runs?mode=17&sort=fast":                "3 1 2 5",
		"/admin/runs?sort=slow&verdict=valid":          "4 2 1",
		"/admin/runs?sort=old":                         "1 2 3 4 5",
		"/admin/runs?from=2026-10-25&to=2026-10-25":    "5",
		"/admin/runs?problems=violations":              "",
		"/admin/runs?sort=bogus&kind=bogus&mode=bogus": "5 4 3 2 1",
	} {
		if got := strings.Join(rows(target), " "); got != want {
			t.Errorf("%s: runs %q, want %q", target, got, want)
		}
	}
	f.act(url.Values{"do": {"remove"}, "run": {"1"}, "note": {"copied"}})
	f.act(url.Values{"do": {"ban"}, "install": {"kc"}, "note": {"cheating"}})
	for target, want := range map[string]string{
		"/admin/runs?show=deleted": "1",
		"/admin/runs?show=banned":  "5",
		"/admin/runs?show=hide":    "4 3 2",
	} {
		if got := strings.Join(rows(target), " "); got != want {
			t.Errorf("%s: runs %q, want %q", target, got, want)
		}
	}
	runsPerPage = 2
	defer func() { runsPerPage = 100 }()
	if got := strings.Join(rows("/admin/runs?sort=old&page=1"), " "); got != "3 4" {
		t.Errorf("page 2: runs %q", got)
	}
	if w := f.do("GET", "/admin/runs?sort=old&page=1", password, nil, "10.0.0.1"); !strings.Contains(w.Body.String(), `href="/admin/runs?page=2&amp;sort=old"`) || !strings.Contains(w.Body.String(), `href="/admin/runs?sort=old"`) {
		t.Errorf("page 2: no next or prev link")
	}
	// any run's file, rejected ones included
	if w = f.do("GET", "/admin/file?run=3", password, nil, "10.0.0.1"); w.Code != 200 || w.Body.String() != "SBR2 c.sbr" {
		t.Fatalf("file: %d", w.Code)
	}
}

func TestAdminModeration(t *testing.T) {
	f := newAdminFixture(t)
	// a form without the page's token is refused
	if w := f.do("POST", "/admin/act", password, url.Values{"do": {"remove"}, "run": {"1"}, "token": {"stale"}}, "10.0.0.1"); w.Code != http.StatusForbidden {
		t.Fatalf("stale token: %d", w.Code)
	}
	f.act(url.Values{"do": {"remove"}, "run": {"1"}, "note": {"copied"}})
	if _, body := get(t, f.s, "/map?map=forge&kind=flagrun&mode=17"); strings.Contains(body, "0:09.000") {
		t.Fatal("a removed run is still on the board")
	}
	w := f.do("GET", "/admin/moderation", password, nil, "10.0.0.1")
	if !strings.Contains(w.Body.String(), "copied") {
		t.Fatal("the removed run isn't listed")
	}
	f.act(url.Values{"do": {"restore"}, "run": {"1"}})
	f.act(url.Values{"do": {"ban"}, "install": {"kb"}, "note": {"cheating"}})
	if code, _ := get(t, f.s, "/player?id=kb"); code != 404 {
		t.Fatalf("a banned player's page: %d", code)
	}
	f.act(url.Values{"do": {"unban"}, "install": {"kb"}})
	if code, _ := get(t, f.s, "/player?id=kb"); code != 200 {
		t.Fatalf("after unbanning: %d", code)
	}

	// re-judge drops a file for the witness; the form returns to the page it came from
	form := url.Values{"do": {"rejudge"}, "run": {"3"}, "token": {f.token}}
	r := httptest.NewRequest("POST", "/admin/act", strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	r.Header.Set("Referer", "https://leaderboard.example/admin/runs?verdict=rejected")
	r.SetBasicAuth("admin", password)
	r.RemoteAddr = "10.0.0.1:1"
	rec := httptest.NewRecorder()
	f.s.ServeHTTP(rec, r)
	if rec.Code != http.StatusSeeOther || rec.Header().Get("Location") != "/admin/runs?verdict=rejected" {
		t.Fatalf("rejudge: %d %q", rec.Code, rec.Header().Get("Location"))
	}
	if _, err := os.Stat(filepath.Join(f.dir, "rejudge", "3")); err != nil {
		t.Fatal(err)
	}
	if got := backTo("https://evil.example/elsewhere"); got != "/admin/runs" {
		t.Fatalf("redirect outside the dashboard: %q", got)
	}

	// flag review is kept in the site's database
	w = f.do("GET", "/admin/flags", password, nil, "10.0.0.1")
	if !strings.Contains(w.Body.String(), "58 of 156") || !strings.Contains(w.Body.String(), "1 not reviewed") {
		t.Fatal("flags page")
	}
	f.act(url.Values{"do": {"review"}, "flag": {"1"}, "note": {"checked the demo"}})
	if w = f.do("GET", "/admin/flags", password, nil, "10.0.0.1"); !strings.Contains(w.Body.String(), "0 not reviewed") || !strings.Contains(w.Body.String(), "checked the demo") {
		t.Fatal("review not kept")
	}
}

func TestAdminPlayers(t *testing.T) {
	f := newAdminFixture(t)
	// an unfinished run shows how long it was streamed
	w := f.do("GET", "/admin/runs?verdict=unfinished", password, nil, "10.0.0.1")
	if body := w.Body.String(); !strings.Contains(body, "0:12.500") || !strings.Contains(body, "a state was loaded") || strings.Contains(body, "0:09.000") {
		t.Fatal("unfinished runs with their length")
	}
	w = f.do("GET", "/admin/players", password, nil, "10.0.0.1")
	if body := w.Body.String(); !strings.Contains(body, "alice") || !strings.Contains(body, "carl") || !strings.Contains(body, "&lt;script&gt;bob&lt;/script&gt;, bob") {
		t.Fatal("players with aliases")
	}
	// kb is alice's other install: merged, the boards have one entry with kb's runs under alice
	f.act(url.Values{"do": {"merge"}, "install": {"kb"}, "into": {"ka"}})
	if _, body := get(t, f.s, "/map?map=forge&kind=flagrun&mode=17"); strings.Count(body, `href="/player?id=`) != 1 || !strings.Contains(body, "alice") {
		t.Fatal("merged players on the board")
	}
	w = f.do("GET", "/admin/player?id=kb", password, nil, "10.0.0.1")
	if body := w.Body.String(); !strings.Contains(body, "<code>ka</code> <span class=\"dim\">main</span>") || !strings.Contains(body, "<code>kb</code>") {
		t.Fatal("the merged player's keys")
	}
	f.act(url.Values{"do": {"rename"}, "install": {"ka"}, "name": {"Alice A."}})
	if _, body := get(t, f.s, "/singleplayer"); !strings.Contains(body, "Alice A.") {
		t.Fatal("the picked name on the boards")
	}
	f.act(url.Values{"do": {"split"}, "install": {"kb"}})
	if _, body := get(t, f.s, "/map?map=forge&kind=flagrun&mode=17"); strings.Count(body, `href="/player?id=`) != 2 {
		t.Fatal("after splitting")
	}
	// deleting is called deleting
	f.act(url.Values{"do": {"remove"}, "run": {"1"}, "note": {"x"}})
	if body := f.do("GET", "/admin/runs", password, nil, "10.0.0.1").Body.String(); !strings.Contains(body, "undelete") {
		t.Fatal("a deleted run can be undeleted")
	}
}

func TestAdminServer(t *testing.T) {
	f := newAdminFixture(t)
	folders, err := parseFolders("replays=" + f.dir + ", nothing=" + filepath.Join(f.dir, "missing"))
	if err != nil {
		t.Fatal(err)
	}
	f.a.disk = &diskWatch{folders: folders, ttl: time.Minute}
	body := f.do("GET", "/admin/server", password, nil, "10.0.0.1").Body.String()
	for _, want := range []string{"Speedbraten's folders", "replays", "of the disk", "free"} {
		if !strings.Contains(body, want) {
			t.Fatalf("server page lacks %q", want)
		}
	}
	r := f.a.disk.report()
	if r.Total == 0 || r.Free == 0 || r.Folders[0].Files < 4 || r.Folders[0].Size == 0 || r.Folders[1].Size != 0 {
		t.Fatalf("disk report %+v", r)
	}
	if _, err := parseFolders("noequals"); err == nil {
		t.Fatal("a folder without a name")
	}
}

func TestAdminLegacy(t *testing.T) {
	f := newAdminFixture(t)
	f.act(url.Values{"do": {"legacyset"}, "map": {"forge"}, "kind": {"flagrun"}, "mode": {"17"}, "player": {"Dino"}, "time": {"0:05.912"}, "source": {"flagruns.com"}, "link": {"https://flagruns.com/forge"}})
	if _, body := get(t, f.s, "/map?map=forge&kind=flagrun&mode=17"); !strings.Contains(body, "Legacy record") || !strings.Contains(body, "0:05.912") || !strings.Contains(body, "https://flagruns.com/forge") {
		t.Fatal("the map page doesn't show the legacy record")
	}
	// it against the witnessed #1 (alice, 0:09.000)
	if _, body := get(t, f.s, "/map?map=forge&kind=flagrun&mode=17"); !strings.Contains(body, "vs #1 −3.088") || !strings.Contains(body, `class="small faster"`) {
		t.Fatal("no diff to the witnessed record")
	}
	f.act(url.Values{"do": {"legacyimport"}, "csv": {"lost,campaign,-3,Raze,11:48,speedrun.com\nmpsp6a,campaign,-3,x,9:00,speedrun.com"}})
	if _, body := get(t, f.s, "/singleplayer"); !strings.Contains(body, "Lost") || strings.Index(body, "Lost") < strings.Index(body, "An Army Of One") {
		t.Fatal("a campaign with only a legacy record isn't listed after the witnessed ones")
	}
	if _, body := get(t, f.s, "/map?map=mpsp6a&kind=campaign&mode=-3"); !strings.Contains(body, "vs #1 &#43;2:50.740") || !strings.Contains(body, `class="small slower"`) {
		t.Fatal("no diff for a legacy record slower than the witnessed one")
	}
	// a board without witnessed runs gets a page for its legacy record
	f.act(url.Values{"do": {"legacyimport"}, "csv": {"# header\nnadefly,race,ffa,Rex,1:02.5,speedyruns,2015\n"}})
	if code, body := get(t, f.s, "/map?map=nadefly&kind=race&mode=0"); code != 200 || !strings.Contains(body, "1:02.500") || !strings.Contains(body, "speedyruns · 2015") {
		t.Fatalf("legacy-only board: %d", code)
	}
	if _, body := get(t, f.s, "/races"); !strings.Contains(body, "nadefly") {
		t.Fatal("a race with only a legacy record isn't listed")
	}
	if _, body := get(t, f.s, "/map?map=nadefly&kind=race&mode=0"); strings.Contains(body, "vs #1") {
		t.Fatal("a diff without a witnessed record")
	}
	if w := f.do("GET", "/admin/legacy", password, nil, "10.0.0.1"); !strings.Contains(w.Body.String(), "Dino") || !strings.Contains(w.Body.String(), "Rex") {
		t.Fatal("the legacy tab doesn't list both")
	}
	// a bad line imports nothing
	form := url.Values{"do": {"legacyimport"}, "csv": {"dust2,flagrun,17,A,5.1,x\ndust2,flagrun,17,B,oops,x"}, "token": {f.token}}
	if w := f.do("POST", "/admin/act", password, form, "10.0.0.1"); w.Code != http.StatusBadRequest || !strings.Contains(w.Body.String(), "line 2") {
		t.Fatalf("bad csv: %d %s", w.Code, w.Body.String())
	}
	if w := f.do("GET", "/admin/legacy", password, nil, "10.0.0.1"); strings.Contains(w.Body.String(), "dust2") {
		t.Fatal("a bad import added lines")
	}
	f.act(url.Values{"do": {"legacydelete"}, "map": {"nadefly"}, "kind": {"race"}, "mode": {"0"}})
	if code, _ := get(t, f.s, "/map?map=nadefly&kind=race&mode=0"); code != 404 {
		t.Fatalf("after delete: %d", code)
	}
}
