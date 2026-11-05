package discord

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

// Message is a message people posted in a channel, its mentions written out as text.
type Message struct {
	ID       string
	AuthorID string
	Author   string // display name
	Text     string
}

// Reader polls a channel with a bot token for the messages people post in it. Messages from
// bots (this one's own posts among them) and webhooks are skipped.
type Reader struct {
	token, channel string
	every          time.Duration
	client         *http.Client
	logf           func(string, ...any)
	mu             sync.Mutex
	last           string // id of the newest message seen
}

func NewReader(token, channel string, every time.Duration, logf func(string, ...any)) *Reader {
	return &Reader{token: token, channel: channel, every: every, client: &http.Client{Timeout: 15 * time.Second}, logf: logf}
}

// Run calls deliver with each new message, oldest first, until stop is closed. Messages posted
// before it started are not delivered.
func (r *Reader) Run(stop <-chan struct{}, deliver func(Message)) {
	for {
		if r.newest() == "" {
			r.start()
		} else if msgs, wait := r.poll(); wait > 0 {
			select {
			case <-stop:
				return
			case <-time.After(wait):
			}
			continue
		} else {
			for _, m := range msgs {
				deliver(m)
			}
		}
		select {
		case <-stop:
			return
		case <-time.After(r.every):
		}
	}
}

type apiUser struct {
	ID         string `json:"id"`
	Username   string `json:"username"`
	GlobalName string `json:"global_name"`
	Bot        bool   `json:"bot"`
}

func (u apiUser) name() string {
	if u.GlobalName != "" {
		return u.GlobalName
	}
	return u.Username
}

type apiMessage struct {
	ID          string    `json:"id"`
	Type        int       `json:"type"`
	Author      apiUser   `json:"author"`
	WebhookID   string    `json:"webhook_id"`
	Content     string    `json:"content"`
	Mentions    []apiUser `json:"mentions"`
	Attachments []struct {
		Filename string `json:"filename"`
	} `json:"attachments"`
}

// start takes the channel's newest message as the point to read on from
func (r *Reader) start() {
	msgs, wait, err := r.get("?limit=1")
	if err != nil {
		r.logf("discord reader: %v", err)
		return
	}
	if wait > 0 {
		return
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	r.last = "0"
	if len(msgs) > 0 {
		r.last = msgs[0].ID
	}
}

func (r *Reader) newest() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.last
}

// poll returns the messages after the last one seen, or how long Discord asks to wait
func (r *Reader) poll() ([]Message, time.Duration) {
	msgs, wait, err := r.get("?limit=50&after=" + r.newest())
	if err != nil {
		r.logf("discord reader: %v", err)
		return nil, 0
	}
	if wait > 0 {
		return nil, wait
	}
	sort.Slice(msgs, func(i, j int) bool { return idLess(msgs[i].ID, msgs[j].ID) })
	var out []Message
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, m := range msgs {
		if idLess(r.last, m.ID) {
			r.last = m.ID
		}
		// 0: a plain message, 19: a reply
		if m.Author.Bot || m.WebhookID != "" || m.Type != 0 && m.Type != 19 {
			continue
		}
		text := readable(m)
		if text == "" {
			continue
		}
		out = append(out, Message{ID: m.ID, AuthorID: m.Author.ID, Author: m.Author.name(), Text: text})
	}
	return out, 0
}

func (r *Reader) get(query string) ([]apiMessage, time.Duration, error) {
	req, err := http.NewRequest("GET", API+"/channels/"+r.channel+"/messages"+query, nil)
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("Authorization", "Bot "+r.token)
	resp, err := r.client.Do(req)
	if err != nil {
		return nil, 0, err
	}
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	resp.Body.Close()
	switch {
	case resp.StatusCode == http.StatusTooManyRequests:
		return nil, retryAfter(resp, data), nil
	case resp.StatusCode >= 300:
		return nil, 0, fmt.Errorf("%s: %s", resp.Status, strings.TrimSpace(string(data)))
	}
	var msgs []apiMessage
	if err := json.Unmarshal(data, &msgs); err != nil {
		return nil, 0, err
	}
	return msgs, 0, nil
}

// Snowflakes are decimal numbers that grow with time.
func idLess(a, b string) bool {
	if len(a) != len(b) {
		return len(a) < len(b)
	}
	return a < b
}

var (
	userMention    = regexp.MustCompile(`<@!?(\d+)>`)
	channelMention = regexp.MustCompile(`<#\d+>`)
	roleMention    = regexp.MustCompile(`<@&\d+>`)
	customEmoji    = regexp.MustCompile(`<a?(:\w+:)\d+>`)
	timestamp      = regexp.MustCompile(`<t:\d+(:\w)?>`)
)

// readable writes a message's mentions and custom emoji as text and names its attachments.
func readable(m apiMessage) string {
	names := map[string]string{}
	for _, u := range m.Mentions {
		names[u.ID] = u.name()
	}
	text := userMention.ReplaceAllStringFunc(m.Content, func(s string) string {
		if n, ok := names[userMention.FindStringSubmatch(s)[1]]; ok {
			return "@" + n
		}
		return "@someone"
	})
	text = channelMention.ReplaceAllString(text, "#channel")
	text = roleMention.ReplaceAllString(text, "@role")
	text = customEmoji.ReplaceAllString(text, "$1")
	text = timestamp.ReplaceAllString(text, "(a time)")
	text = strings.Join(strings.Fields(text), " ")
	for _, a := range m.Attachments {
		if text != "" {
			text += " "
		}
		text += "[" + a.Filename + "]"
	}
	return text
}
