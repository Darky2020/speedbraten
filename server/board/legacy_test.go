package board

import "testing"

func TestParseTime(t *testing.T) {
	for in, want := range map[string]int64{"0:05.912": 5912, "5.9": 5900, "12": 12000, "6:01.845": 361845, "1:00:00.5": 3600500} {
		if got, err := ParseTime(in); err != nil || got != want {
			t.Errorf("%q: %d %v, want %d", in, got, err, want)
		}
	}
	for _, in := range []string{"", "abc", "1:x", "5.1234", "5."} {
		if _, err := ParseTime(in); err == nil {
			t.Errorf("%q accepted", in)
		}
	}
}

func TestLegacyRecords(t *testing.T) {
	s := newFixture(t).open()
	k := Key{Map: "akimiski", Kind: "flagrun", Mode: 17}
	if l, err := s.LegacyRecord(k); l != nil || err != nil {
		t.Fatalf("before: %v %v", l, err)
	}
	list, err := ParseLegacyCSV("# map,kind,mode,player,time,source,date,link\nakimiski, flagrun, effic ctf, Dino, 0:05.912, flagruns.com, 2019-03-02, https://flagruns.com/x\n\nmpsp6a,campaign,-3,frosty,7:01.2,speedrun.com\n")
	if err != nil || len(list) != 2 {
		t.Fatalf("csv: %d %v", len(list), err)
	}
	for _, l := range list {
		if err := s.SetLegacy(l); err != nil {
			t.Fatal(err)
		}
	}
	l, err := s.LegacyRecord(k)
	if err != nil || l == nil || l.Player != "Dino" || l.MS != 5912 || l.Source != "flagruns.com" || l.Date != "2019-03-02" || l.Link != "https://flagruns.com/x" {
		t.Fatalf("after: %+v %v", l, err)
	}
	// one per board: a second one replaces it
	if err := s.SetLegacy(Legacy{Key: k, Player: "Rex", MS: 5800, Source: "speedyruns"}); err != nil {
		t.Fatal(err)
	}
	if all, _ := s.LegacyRecords(); len(all) != 2 || all[0].Player != "Rex" || all[0].Date != "" || all[1].Key.Mode != -3 || all[1].MS != 421200 {
		t.Fatalf("list: %+v", all)
	}
	if err := s.DeleteLegacy(k); err != nil {
		t.Fatal(err)
	}
	if l, _ := s.LegacyRecord(k); l != nil {
		t.Fatal("not deleted")
	}
	for _, bad := range []string{"akimiski,flagrun,17,Dino,0:05.9", "akimiski,ctf,17,Dino,5.9,x", "akimiski,flagrun,warp,Dino,5.9,x", "akimiski,flagrun,17,Dino,5.9,x,,javascript:alert(1)"} {
		if _, err := ParseLegacyCSV(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}
