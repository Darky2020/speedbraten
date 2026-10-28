package board

import (
	"fmt"
	"strconv"
	"time"
)

// DelayKinds are the run kinds whose replays have their own publish delay; dmsp runs use the
// campaign delay.
var DelayKinds = []string{"race", "flagrun", "campaign"}

func delayKind(kind string) string {
	if kind == "dmsp" {
		return "campaign"
	}
	return kind
}

// ReplayDelays is the publish delay of each of DelayKinds; a kind without a setting has none.
func (s *Store) ReplayDelays() (map[string]time.Duration, error) {
	d := map[string]time.Duration{}
	for _, k := range DelayKinds {
		d[k] = 0
	}
	rows, err := s.site.Query("SELECT key, value FROM settings WHERE key LIKE 'replaydelay.%'")
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		k := r[0][len("replaydelay."):]
		if _, ok := d[k]; ok {
			hours, _ := strconv.ParseInt(r[1], 10, 64)
			d[k] = time.Duration(hours) * time.Hour
		}
	}
	return d, nil
}

// SetReplayDelay sets the publish delay of one of DelayKinds, in whole hours.
func (s *Store) SetReplayDelay(kind string, hours int64) error {
	if hours < 0 {
		hours = 0
	}
	_, err := s.site.Exec("INSERT INTO settings(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
		"replaydelay."+delayKind(kind), strconv.FormatInt(hours, 10))
	return err
}

// ReplayLock tells whether a run is a board entry and, if its replay is not public yet, when it
// will be: the time the run was judged plus its kind's delay. A player's own runs are never
// locked for them (viewer: the asking player's install, "" when unknown).
func (s *Store) ReplayLock(run int64, viewer string) (until time.Time, listed bool, err error) {
	if listed, err = s.Listed(run); err != nil || !listed {
		return time.Time{}, listed, err
	}
	locks, err := s.ReplayLocks([]int64{run}, viewer)
	return locks[run], true, err
}

// ReplayLocks is ReplayLock for many runs: the board entries among them whose replays are not
// public yet, with the time each will be.
func (s *Store) ReplayLocks(runs []int64, viewer string) (map[int64]time.Time, error) {
	delays, err := s.ReplayDelays()
	if err != nil {
		return nil, err
	}
	if viewer != "" {
		viewer = s.Canonical(viewer)
	}
	locks := map[int64]time.Time{}
	now := time.Now()
	err = s.locked(func() {
		for _, run := range runs {
			l, ok := s.listed[run]
			if !ok || viewer != "" && l.install == viewer {
				continue
			}
			if at := l.when.Add(delays[delayKind(l.kind)]); now.Before(at) {
				locks[run] = at
			}
		}
	})
	return locks, err
}

// Wait is a wait to the hour: "6d 4h", "5h", "under an hour".
func Wait(d time.Duration) string {
	h := int64(d / time.Hour)
	switch {
	case h <= 0:
		return "under an hour"
	case h < 24:
		return fmt.Sprintf("%dh", h)
	case h%24 == 0:
		return fmt.Sprintf("%dd", h/24)
	}
	return fmt.Sprintf("%dd %dh", h/24, h%24)
}
