package discord

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

type call struct {
	method, path, content string
	mentions              any
}

// fakeDiscord answers webhook calls as Discord does; the first limited calls get a 429
func fakeDiscord(t *testing.T, limited int) (*httptest.Server, func() []call) {
	var mu sync.Mutex
	var calls []call
	next := 100
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		if limited > 0 {
			limited--
			rw.WriteHeader(http.StatusTooManyRequests)
			fmt.Fprint(rw, `{"message": "You are being rate limited.", "retry_after": 0.01}`)
			return
		}
		var body map[string]any
		json.NewDecoder(r.Body).Decode(&body)
		c := call{method: r.Method, path: r.URL.Path, mentions: body["allowed_mentions"]}
		c.content, _ = body["content"].(string)
		calls = append(calls, c)
		id := strings.TrimPrefix(r.URL.Path, "/hook/messages/")
		if r.Method == "POST" {
			next++
			id = fmt.Sprint(next)
		}
		fmt.Fprintf(rw, `{"id": "%s"}`, id)
	}))
	t.Cleanup(srv.Close)
	return srv, func() []call { mu.Lock(); defer mu.Unlock(); return append([]call(nil), calls...) }
}

func TestPostsAndStatus(t *testing.T) {
	srv, calls := fakeDiscord(t, 0)
	w := New(srv.URL+"/hook", t.Logf)
	w.Post("alice joined the hub")
	w.Status("1 player in 1 lobby")
	w.Status("2 players in 1 lobby")
	w.Status("")
	w.Status("1 player in 1 lobby")
	w.Close()
	got := calls()
	want := []string{"POST /hook alice joined the hub", "POST /hook 1 player in 1 lobby", "PATCH /hook/messages/102 2 players in 1 lobby", "POST /hook 1 player in 1 lobby"}
	if len(got) != len(want) {
		t.Fatalf("calls %+v", got)
	}
	for i, c := range got {
		if s := c.method + " " + c.path + " " + c.content; s != want[i] {
			t.Errorf("call %d: %q, want %q", i, s, want[i])
		}
		if m, ok := c.mentions.(map[string]any); !ok || len(m["parse"].([]any)) != 0 {
			t.Errorf("call %d: allowed_mentions %v, want none", i, c.mentions)
		}
	}
}

func TestRateLimitRetries(t *testing.T) {
	srv, calls := fakeDiscord(t, 2)
	w := New(srv.URL+"/hook", t.Logf)
	w.Post("bob set a new record")
	w.Close()
	if got := calls(); len(got) != 1 || got[0].content != "bob set a new record" {
		t.Fatalf("after two 429s: %+v", got)
	}
}

func TestLongMessageCut(t *testing.T) {
	srv, calls := fakeDiscord(t, 0)
	w := New(srv.URL+"/hook", t.Logf)
	w.Post(strings.Repeat("x", 3000))
	w.Close()
	if got := calls(); len(got) != 1 || len([]rune(got[0].content)) > MaxLen {
		t.Fatalf("long message: %d calls, %d characters", len(got), len([]rune(got[0].content)))
	}
}
