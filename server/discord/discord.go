// Package discord talks to Discord channels as a bot: it posts plain messages and one status
// message that is edited in place, and (reader.go) reads what people post. Sends are queued and
// made in order by one goroutine, waiting out Discord's rate limits; mentions in the text never
// ping anyone.
package discord

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"time"
)

// API is Discord's REST API; tests point it elsewhere.
var API = "https://discord.com/api/v10"

const (
	MaxLen   = 2000 // Discord's limit for a message's content
	queueLen = 256
	tries    = 5
)

type job struct {
	status bool
	text   string
}

type Channel struct {
	url    string // the channel's messages
	token  string
	client *http.Client
	logf   func(string, ...any)
	q      chan job
	status string // id of the status message being kept up to date, "" when there is none
	done   chan struct{}
}

// New starts posting to a channel, by its id, as the bot whose token it is.
func New(token, channel string, logf func(string, ...any)) *Channel {
	w := &Channel{url: API + "/channels/" + channel + "/messages", token: token, client: &http.Client{Timeout: 15 * time.Second}, logf: logf, q: make(chan job, queueLen), done: make(chan struct{})}
	go w.run()
	return w
}

// All sends the same feed to several channels, each with its own status message.
type All []*Channel

func (a All) Post(text string) {
	for _, w := range a {
		w.Post(text)
	}
}

func (a All) Status(text string) {
	for _, w := range a {
		w.Status(text)
	}
}

// Post queues a message.
func (w *Channel) Post(text string) { w.enqueue(job{text: text}) }

// Status sets the status message: the first one after an empty status is a new message, later
// ones edit it. "" ends it, so the next status starts a new message.
func (w *Channel) Status(text string) { w.enqueue(job{status: true, text: text}) }

// Close sends what is queued and stops.
func (w *Channel) Close() {
	close(w.q)
	<-w.done
}

func (w *Channel) enqueue(j job) {
	select {
	case w.q <- j:
	default:
		w.logf("discord: queue full, dropped a message")
	}
}

func (w *Channel) run() {
	defer close(w.done)
	for j := range w.q {
		switch {
		case !j.status:
			w.send("POST", w.url, j.text)
		case j.text == "":
			w.status = ""
		case w.status == "":
			w.status = w.send("POST", w.url, j.text)
		default:
			if w.send("PATCH", w.url+"/"+w.status, j.text) == "" {
				w.status = w.send("POST", w.url, j.text)
			}
		}
	}
}

// send returns the message's id, "" when it failed
func (w *Channel) send(method, url, text string) string {
	if r := []rune(text); len(r) > MaxLen {
		text = string(r[:MaxLen-1]) + "…"
	}
	body, _ := json.Marshal(map[string]any{"content": text, "allowed_mentions": map[string]any{"parse": []string{}}})
	for try := 0; try < tries; try++ {
		req, err := http.NewRequest(method, url, bytes.NewReader(body))
		if err != nil {
			w.logf("discord: %v", err)
			return ""
		}
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("Authorization", "Bot "+w.token)
		resp, err := w.client.Do(req)
		if err != nil {
			w.logf("discord: %v", err)
			time.Sleep(time.Duration(try+1) * time.Second)
			continue
		}
		data, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
		resp.Body.Close()
		switch {
		case resp.StatusCode == http.StatusTooManyRequests:
			time.Sleep(retryAfter(resp, data))
			continue
		case resp.StatusCode >= 300:
			w.logf("discord: %s: %s %s", method, resp.Status, bytes.TrimSpace(data))
			return ""
		}
		var msg struct {
			ID string `json:"id"`
		}
		json.Unmarshal(data, &msg)
		return msg.ID
	}
	w.logf("discord: %s gave up after %d tries", method, tries)
	return ""
}

// how long a 429 asks to wait: the body's retry_after (seconds) or the Retry-After header
func retryAfter(resp *http.Response, data []byte) time.Duration {
	var rl struct {
		RetryAfter float64 `json:"retry_after"`
	}
	if json.Unmarshal(data, &rl) == nil && rl.RetryAfter > 0 {
		return time.Duration(rl.RetryAfter*1000) * time.Millisecond
	}
	if s, err := strconv.ParseFloat(resp.Header.Get("Retry-After"), 64); err == nil && s > 0 {
		return time.Duration(s*1000) * time.Millisecond
	}
	return time.Second
}
