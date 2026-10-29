package board

import (
	"testing"
	"time"
)

func TestReplayDelay(t *testing.T) {
	f := newFixture(t)
	now := time.Now().UnixMilli()
	// forge: flagrun boards only take maps on the flagrun list
	judged := func(id int, install, kind string, mode int, ago time.Duration) {
		f.exec("INSERT INTO runs(id, installkey, map, player, runkind, mode, runtimems, verdict, judged, file) VALUES(?, ?, 'forge', 'p', ?, ?, 9000, 'valid', ?, 'x')",
			id, install, kind, mode, now-ago.Milliseconds())
	}
	judged(1, "ka", "race", 1, 0)
	judged(2, "kb", "flagrun", 17, 50*time.Hour)
	judged(3, "kb", "dmsp", -2, 0)
	judged(4, "kb", "race", 1, 0)
	s := f.open()

	lock := func(run int64, viewer string) time.Time {
		t.Helper()
		until, ok, err := s.ReplayLock(run, viewer)
		if err != nil || !ok {
			t.Fatalf("run %d: listed %v, %v", run, ok, err)
		}
		return until
	}
	// without settings every replay is public at once
	for _, run := range []int64{1, 2, 3} {
		if !lock(run, "").IsZero() {
			t.Fatalf("run %d is locked with no delay set", run)
		}
	}

	for kind, hours := range map[string]int64{"race": 24, "flagrun": 48, "campaign": 1} {
		if err := s.SetReplayDelay(kind, hours); err != nil {
			t.Fatal(err)
		}
	}
	if d, _ := s.ReplayDelays(); d["race"] != 24*time.Hour || d["flagrun"] != 48*time.Hour || d["campaign"] != time.Hour {
		t.Fatalf("delays %v", d)
	}
	if until := lock(1, ""); until.Sub(time.UnixMilli(now)) != 24*time.Hour {
		t.Fatalf("race run 1 locked until %v, want judged + 24h", until)
	}
	if !lock(1, "ka").IsZero() {
		t.Fatal("a player's own run is locked for them")
	}
	if lock(4, "ka").IsZero() {
		t.Fatal("another player's run is public within its delay")
	}
	if !lock(2, "").IsZero() {
		t.Fatal("a flagrun judged 50 h ago is locked by a 48 h delay")
	}
	if until := lock(3, ""); until.Sub(time.UnixMilli(now)) != time.Hour {
		t.Fatalf("dmsp run 3 locked until %v, want the campaign delay (1h)", until)
	}
	if _, ok, _ := s.ReplayLock(99, ""); ok {
		t.Fatal("a run that isn't on a board is listed")
	}

	// a shorter delay unlocks at once, and SetReplayDelay keeps no negative delays
	if err := s.SetReplayDelay("race", -5); err != nil {
		t.Fatal(err)
	}
	if !lock(4, "").IsZero() {
		t.Fatal("a race run is locked after the race delay went to 0")
	}
}

func TestWait(t *testing.T) {
	for d, want := range map[time.Duration]string{
		30 * time.Minute:          "under an hour",
		5*time.Hour + time.Minute: "5h",
		48 * time.Hour:            "2d",
		6*24*time.Hour + 4*time.Hour + 59*time.Minute: "6d 4h",
	} {
		if got := Wait(d); got != want {
			t.Fatalf("Wait(%v) = %q, want %q", d, got, want)
		}
	}
}
