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

	"speedbraten/server/sqlite"
)

const password = "correct horse battery"

func testServer(t *testing.T) (*server, string) {
	dir := t.TempDir()
	db, err := sqlite.Create(filepath.Join(dir, "witness.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(db.Close)
	replay := filepath.Join(dir, "1-1.sbr")
	if err := os.WriteFile(replay, []byte("SBR2 test bytes"), 0600); err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		"CREATE TABLE runs(id INTEGER PRIMARY KEY, installkey TEXT, started INTEGER, file TEXT, verdict TEXT, map TEXT, player TEXT)",
		"CREATE TABLE flags(id INTEGER PRIMARY KEY, run INTEGER, kind TEXT, other INTEGER, matched INTEGER, total INTEGER, created INTEGER, reviewed INTEGER DEFAULT 0, note TEXT)",
	} {
		if _, err := db.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	db.Exec("INSERT INTO runs VALUES(1, 'aaaaaaaaaaaa', 1700000000000, ?, 'valid', 'janela', 'original')", replay)
	db.Exec("INSERT INTO runs VALUES(2, 'bbbbbbbbbbbb', 1700000100000, ?, 'valid', 'janela', '<b>copier</b>')", replay)
	db.Exec("INSERT INTO flags(run, kind, other, matched, total, created) VALUES(2, 'copy', 1, 58, 156, 1700000200000)")
	hash, err := bcrypt.GenerateFromPassword([]byte(password), bcrypt.MinCost)
	if err != nil {
		t.Fatal(err)
	}
	return newServer(db, hash), dir
}

func do(s *server, method, target, pass string, form url.Values, ip string) *httptest.ResponseRecorder {
	var body *strings.Reader
	if form != nil {
		body = strings.NewReader(form.Encode())
	} else {
		body = strings.NewReader("")
	}
	r := httptest.NewRequest(method, target, body)
	if form != nil {
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	}
	if pass != "" {
		r.SetBasicAuth("review", pass)
	}
	r.RemoteAddr = ip + ":1234"
	w := httptest.NewRecorder()
	s.handler.ServeHTTP(w, r)
	return w
}

func TestLogin(t *testing.T) {
	s, _ := testServer(t)
	if w := do(s, "GET", "/", "", nil, "10.0.0.1"); w.Code != http.StatusUnauthorized || w.Header().Get("WWW-Authenticate") == "" {
		t.Fatalf("no login: %d", w.Code)
	}
	if w := do(s, "GET", "/", "wrong password", nil, "10.0.0.1"); w.Code != http.StatusUnauthorized {
		t.Fatalf("wrong password: %d", w.Code)
	}
	w := do(s, "GET", "/", password, nil, "10.0.0.1")
	if w.Code != http.StatusOK {
		t.Fatalf("right password: %d", w.Code)
	}
	body := w.Body.String()
	for _, want := range []string{"run 2: janela", "run 1: janela", "58 of 156", "&lt;b&gt;copier&lt;/b&gt;", "1 not reviewed"} {
		if !strings.Contains(body, want) {
			t.Errorf("page lacks %q", want)
		}
	}
	if strings.Contains(body, "<b>copier</b>") {
		t.Error("a player name was not escaped")
	}
}

func TestLockout(t *testing.T) {
	s, _ := testServer(t)
	clock := time.Unix(1700000000, 0)
	s.now = func() time.Time { return clock }
	for i := 0; i < maxFailures; i++ {
		do(s, "GET", "/", "wrong password", nil, "10.0.0.2")
	}
	if w := do(s, "GET", "/", password, nil, "10.0.0.2"); w.Code != http.StatusTooManyRequests {
		t.Fatalf("locked out with the right password: %d", w.Code)
	}
	if w := do(s, "GET", "/", password, nil, "10.0.0.3"); w.Code != http.StatusOK {
		t.Fatalf("another address: %d", w.Code)
	}
	clock = clock.Add(lockout + time.Second)
	if w := do(s, "GET", "/", password, nil, "10.0.0.2"); w.Code != http.StatusOK {
		t.Fatalf("after the lockout: %d", w.Code)
	}
}

func TestFileAndReview(t *testing.T) {
	s, _ := testServer(t)
	w := do(s, "GET", "/file?run=2", password, nil, "10.0.0.4")
	if w.Code != http.StatusOK || w.Body.String() != "SBR2 test bytes" || !strings.Contains(w.Header().Get("Content-Disposition"), "attachment") {
		t.Fatalf("download: %d %q", w.Code, w.Body.String())
	}
	if w := do(s, "GET", "/file?run=9", password, nil, "10.0.0.4"); w.Code != http.StatusNotFound {
		t.Fatalf("unknown run: %d", w.Code)
	}
	if w := do(s, "POST", "/review", password, url.Values{"id": {"1"}, "note": {"x"}, "token": {"bad"}}, "10.0.0.4"); w.Code != http.StatusForbidden {
		t.Fatalf("review without the page token: %d", w.Code)
	}
	page := do(s, "GET", "/", password, nil, "10.0.0.4").Body.String()
	m := regexp.MustCompile(`name="token" value="([0-9a-f]+)"`).FindStringSubmatch(page)
	if m == nil {
		t.Fatal("no token on the page")
	}
	if w := do(s, "POST", "/review", password, url.Values{"id": {"1"}, "note": {"same aim, confirmed copy"}, "token": {m[1]}}, "10.0.0.4"); w.Code != http.StatusSeeOther {
		t.Fatalf("review: %d", w.Code)
	}
	rows, _ := s.db.Query("SELECT reviewed, note FROM flags WHERE id = 1")
	if len(rows) != 1 || rows[0][0] != "1" || rows[0][1] != "same aim, confirmed copy" {
		t.Fatalf("flag after review: %v", rows)
	}
	if !strings.Contains(do(s, "GET", "/", password, nil, "10.0.0.4").Body.String(), "0 not reviewed") {
		t.Error("the reviewed flag still counts as open")
	}
}
