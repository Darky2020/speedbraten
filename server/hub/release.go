package hub

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// LatestRelease asks GitHub's API (url: .../repos/<owner>/<repo>/releases/latest, which skips
// drafts and pre-releases) for the newest release's tag and page.
func LatestRelease(client *http.Client, url string) (tag, page string, err error) {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return "", "", err
	}
	req.Header.Set("Accept", "application/vnd.github+json")
	req.Header.Set("User-Agent", "speedbraten-hub")
	resp, err := client.Do(req)
	if err != nil {
		return "", "", err
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != http.StatusOK {
		return "", "", fmt.Errorf("%s", resp.Status)
	}
	var rel struct {
		Tag  string `json:"tag_name"`
		Page string `json:"html_url"`
	}
	if err := json.Unmarshal(data, &rel); err != nil {
		return "", "", err
	}
	if rel.Tag == "" {
		return "", "", fmt.Errorf("no tag_name")
	}
	return rel.Tag, rel.Page, nil
}

// ReleaseLoop keeps the hub's client release up to date from url, checking every interval.
func (h *Hub) ReleaseLoop(url string, every time.Duration, logf func(string, ...any)) {
	client := &http.Client{Timeout: 30 * time.Second}
	for {
		if tag, page, err := LatestRelease(client, url); err != nil {
			logf("latest release: %v", err)
		} else {
			h.SetRelease(tag, page)
		}
		time.Sleep(every)
	}
}

// SetRelease tells every player, now and later, the newest client release and where to get it.
func (h *Hub) SetRelease(tag, page string) {
	var w writer
	w.str(tag)
	w.str(page)
	h.mu.Lock()
	defer h.mu.Unlock()
	if string(w.b) == string(h.release) {
		return
	}
	h.release = w.b
	h.broadcast(MsgRelease, w.b)
}
