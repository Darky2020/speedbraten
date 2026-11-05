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

// fakeChannel answers as Discord does for channel 77 on server 5: its messages (newest first),
// the channel, and its members, of whom umeep has the nickname "Meep on server"; the first
// limited message requests get a 429
func fakeChannel(t *testing.T, limited int) (add func(m map[string]any), auth func() []string, members func() int) {
	var mu sync.Mutex
	var msgs []map[string]any
	var auths []string
	memberCalls := 0
	srv := httptest.NewServer(http.HandlerFunc(func(rw http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		auths = append(auths, r.Header.Get("Authorization"))
		switch r.URL.Path {
		case "/channels/77":
			fmt.Fprint(rw, `{"id": "77", "guild_id": "5"}`)
			return
		case "/guilds/5/members/umeep":
			memberCalls++
			fmt.Fprint(rw, `{"nick": "Meep on server", "user": {"id": "umeep"}}`)
			return
		case "/guilds/5/members/umoop":
			memberCalls++
			fmt.Fprint(rw, `{"nick": null, "user": {"id": "umoop"}}`)
			return
		case "/channels/77/messages":
		default:
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
		func() []string { mu.Lock(); defer mu.Unlock(); return append([]string(nil), auths...) },
		func() int { mu.Lock(); defer mu.Unlock(); return memberCalls }
}

func msg(id, author, content string) map[string]any {
	return map[string]any{"id": id, "type": 0, "content": content, "author": map[string]any{"id": "u" + author, "username": author}}
}

// A reader delivers what people post after it started, oldest first, named by their server
// nickname (looked up once) or display name, and skips bots (the feed's own posts), webhooks and
// system messages.
func TestReader(t *testing.T) {
	add, auths, members := fakeChannel(t, 1)
	add(msg("100", "old", "before the start"))
	r := NewReader("tok", "77", 5*time.Millisecond, t.Logf)
	got := make(chan Message, 10)
	stop, done := make(chan struct{}), make(chan struct{})
	go func() { r.Run(stop, func(m Message) { got <- m }); close(done) }()
	defer func() { close(stop); <-done }()
	for r.newest() == "" {
		time.Sleep(time.Millisecond)
	}

	bot := msg("101", "bot", "💬 **alice**: hi")
	bot["author"].(map[string]any)["bot"] = true
	hook := msg("102", "hook", "from a webhook")
	hook["webhook_id"] = "9"
	join := msg("103", "sys", "")
	join["type"] = 7
	mention := msg("1000", "meep", "hi <@555> <:pog:123> <#88> 😂")
	mention["author"].(map[string]any)["global_name"] = "Meep"
	mention["mentions"] = []map[string]any{{"id": "555", "username": "moop"}}
	pic := msg("999", "moop", "")
	pic["attachments"] = []map[string]any{{"filename": "run.png"}}
	again := msg("1001", "meep", "again")
	for _, m := range []map[string]any{bot, hook, join, pic, mention, again} {
		add(m)
	}
	want := []Message{{"999", "umoop", "moop", "[run.png]"}, {"1000", "umeep", "Meep on server", "hi @moop :pog: #channel :joy:"}, {"1001", "umeep", "Meep on server", "again"}}
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
	if n := members(); n != 2 {
		t.Fatalf("%d server name lookups for 2 posters, want 2", n)
	}
}

// Emoji are written as the short names Discord shows; text the game's font can show is kept.
func TestEmojiText(t *testing.T) {
	for in, want := range map[string]string{
		"gg 😂":           "gg :joy:",
		"👍🏽 nice":        ":thumbsup: nice",
		"❤️ ❤":           ":heart: :heart:",
		"🇺🇦 🇬🇧":          ":flag_ua: :flag_gb:",
		"👨‍👩‍👧 ok":       ":family_man_woman_girl: ok",
		"Ňєkø © 1.5":     "Ňєkø © 1.5",
		"run 🏁 at 4.900": "run :checkered_flag: at 4.900",
	} {
		if got := emojiText(in); got != want {
			t.Errorf("emojiText(%q) = %q, want %q", in, got, want)
		}
	}
}
