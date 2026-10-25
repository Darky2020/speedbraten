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
	if _, body := get(t, f.s, "/board?map=forge&kind=flagrun&mode=17"); strings.Contains(body, "0:09.000") {
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
