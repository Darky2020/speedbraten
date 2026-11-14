package main

import (
	"strings"
	"testing"
	"time"

	"speedbraten/server/board"
)

func TestPlayersPage(t *testing.T) {
	store, _ := fixture(t)
	// alice's 9.000 was forge's first effic ctf time, so she is the one who beat this; bob's campaign
	// time is slower than its legacy record
	if err := store.SetLegacy(board.Legacy{Key: board.Key{Map: "forge", Kind: "flagrun", Mode: 17}, Player: "old", MS: 9200, Source: "flagruns.com"}); err != nil {
		t.Fatal(err)
	}
	if err := store.SetLegacy(board.Legacy{Key: board.Key{Map: "mpsp6a", Kind: "campaign", Mode: -3}, Player: "old", MS: 300000, Source: "speedyruns"}); err != nil {
		t.Fatal(err)
	}
	s := newSite(store)

	rows, err := s.playerRows()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2 {
		t.Fatalf("%d players, want alice and bob (carl has no counted run)", len(rows))
	}
	// one record each: bob, with times on two boards, ranks above alice's one
	bob, alice := rows[0], rows[1]
	if bob.Install != "kb" || alice.Install != "ka" {
		t.Fatalf("order %s, %s; want kb, ka", bob.Install, alice.Install)
	}
	if alice.Records != 1 || alice.Runs != 1 || alice.Legacy != 1 || alice.Done != 1 {
		t.Fatalf("alice %+v", alice)
	}
	if bob.Records != 1 || bob.Runs != 2 || bob.Legacy != 0 || bob.Done != 2 {
		t.Fatalf("bob %+v", bob)
	}
	cat := func(p playerRow, key string) playerCategory {
		for i, g := range completionGroups {
			if g.Key == key {
				return p.Categories[i]
			}
		}
		t.Fatalf("no category %s", key)
		return playerCategory{}
	}
	if c := cat(alice, "ectf"); c.Records != 1 || c.Done != 1 || c.Of != len(board.FlagrunMaps) {
		t.Fatalf("alice effic ctf %+v", c)
	}
	if c := cat(bob, "ectf"); c.Records != 0 || c.Done != 1 {
		t.Fatalf("bob effic ctf %+v", c)
	}
	if c := cat(bob, "sp"); c.Records != 1 || c.Done != 1 || c.Of != board.CampaignCount() {
		t.Fatalf("bob campaigns %+v", c)
	}

	code, body := get(t, s, "/players")
	for _, want := range []string{"<h1>Players</h1>", `href="/players" class="on"`, `href="/player?id=kb"`, `href="/player?id=ka"`, "alice", "data-sort"} {
		if code != 200 || !strings.Contains(body, want) {
			t.Fatalf("players page %d lacks %q", code, want)
		}
	}
	if strings.Contains(body, "<script>bob") {
		t.Fatal("a player name isn't escaped")
	}
	if strings.Index(body, `href="/player?id=kb"`) > strings.Index(body, `href="/player?id=ka"`) {
		t.Fatal("the page doesn't list players in the default order")
	}

	for id, want := range map[string]string{"ka": "1 legacy broken", "kb": "0 legacy broken"} {
		if code, body := get(t, s, "/player?id="+id); code != 200 || !strings.Contains(body, want) {
			t.Fatalf("player %s page %d lacks %q", id, code, want)
		}
	}
}

func TestFirstUnder(t *testing.T) {
	at := func(ms int64, when int64) board.Record { return board.Record{MS: ms, When: time.UnixMilli(when)} }
	history := []board.Record{at(12000, 1), at(10000, 2), at(9000, 3)}
	if r := firstUnder(history, 11000); r == nil || r.MS != 10000 {
		t.Fatalf("first under 11.000: %+v, want the 10.000", r)
	}
	if r := firstUnder(history, 9000); r != nil {
		t.Fatalf("first under 9.000: %+v, want none (ties don't beat it)", r)
	}
}

func TestSortableTables(t *testing.T) {
	store, _ := fixture(t)
	s := newSite(store)
	for _, path := range []string{"/races", "/flagruns", "/singleplayer", "/records", "/players", "/map?map=forge&kind=flagrun&mode=17", "/player?id=ka", "/player?id=kb"} {
		code, body := get(t, s, path)
		if code != 200 || !strings.Contains(body, `<table class="sortable">`) && !strings.Contains(body, `class="sortable"`) || !strings.Contains(body, `data-sort="`) {
			t.Fatalf("%s %d has no sortable table", path, code)
		}
		if strings.Contains(body, `name="sort"`) {
			t.Fatalf("%s still has the sort dropdown", path)
		}
	}
}
