package discord

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"
)

// fakeChannel answers GET /channels/<id>/messages as Discord does, newest first; the first
// limited calls get a 429
func fakeChannel(t *testing.T, limited int) (add func(m map[string]any), auth func() []string) {
	var mu sync.Mutex
	var msgs []map[string]any
	var auths []string
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		auths = append(auths, r.Header.Get("Authorization"))
		if r.URL.Path != "/channels/77/messages" {
			rw.WriteHeader(http.StatusNotFound)
			return
		}
		if limited > 0 {
			limited--
			rw.WriteHeader(http.StatusTooManyRequests)
			fmt.Fprint(rw, `{"retry_after": 0.01}`)
			return
		}
		after := r.URL.Query().Get("after")
		var out []map[string]any
		for i := len(msgs) - 1; i >= 0; i-- {
			if after == "" || idLess(after, msgs[i]["id"].(string)) {
				out = append(out, msgs[i])
			}
			if r.URL.Query().Get("limit") == "1" && len(out) == 1 {
				break
			}
		}
		if out == nil {
			out = []map[string]any{}
		}
		json.NewEncoder(rw).Encode(out)
	}))
	t.Cleanup(srv.Close)
	old := API
	API = srv.URL
	t.Cleanup(func() { API = old })
	return func(m map[string]any) { mu.Lock(); msgs = append(msgs, m); mu.Unlock() },
		func() []string { mu.Lock(); defer mu.Unlock(); return append([]string(nil), auths...) }
}

func msg(id, author, content string) map[string]any {
	return map[string]any{"id": id, "type": 0, "content": content, "author": map[string]any{"id": "u" + author, "username": author}}
}

// A reader delivers what people post after it started, oldest first, and skips bots (the feed's
// own posts), webhooks and system messages.
func TestReader(t *testing.T) {
	add, auths := fakeChannel(t, 1)
	add(msg("100", "old", "before the start"))
	r := NewReader("tok", "77", 5*time.Millisecond, t.Logf)
	got := make(chan Message, 10)
	stop := make(chan struct{})
	go r.Run(stop, func(m Message) { got <- m })
	defer close(stop)
	for r.newest() == "" {
		time.Sleep(time.Millisecond)
	}

	bot := msg("101", "bot", "💬 **alice**: hi")
	bot["author"].(map[string]any)["bot"] = true
	hook := msg("102", "hook", "from a webhook")
	hook["webhook_id"] = "9"
	join := msg("103", "sys", "")
	join["type"] = 7
	mention := msg("1000", "meep", "hi <@555> <:pog:123> <#88>")
	mention["author"].(map[string]any)["global_name"] = "Meep"
	mention["mentions"] = []map[string]any{{"id": "555", "username": "moop"}}
	pic := msg("999", "moop", "")
	pic["attachments"] = []map[string]any{{"filename": "run.png"}}
	for _, m := range []map[string]any{bot, hook, join, pic, mention} {
		add(m)
	}
	want := []Message{{"999", "umoop", "moop", "[run.png]"}, {"1000", "umeep", "Meep", "hi @moop :pog: #channel"}}
	for _, w := range want {
		select {
		case m := <-got:
			if m != w {
				t.Fatalf("got %+v, want %+v", m, w)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("no message, want %+v", w)
		}
	}
	select {
	case m := <-got:
		t.Fatalf("delivered %+v", m)
	case <-time.After(50 * time.Millisecond):
	}
	for _, a := range auths() {
		if a != "Bot tok" {
			t.Fatalf("authorization %q", a)
		}
	}
}
