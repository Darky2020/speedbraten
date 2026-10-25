package board

import "fmt"

// the game's mode names (fpsgame/game.h gamemodes), from mode -3 up
var modeNames = []string{"SP", "DMSP", "demo", "ffa", "coop edit", "teamplay", "instagib", "insta team", "efficiency",
	"effic team", "tactics", "tac team", "capture", "regen capture", "ctf", "insta ctf", "protect", "insta protect",
	"hold", "insta hold", "effic ctf", "effic protect", "effic hold", "collect", "insta collect", "effic collect"}

func ModeName(mode int) string {
	if i := mode + 3; i >= 0 && i < len(modeNames) {
		return modeNames[i]
	}
	return fmt.Sprintf("mode %d", mode)
}

// the single player menu's campaigns, filed under their first map (fpsgame/runtimer.cpp)
var campaigns = map[string]string{"mpsp6a": "An Army Of One", "mpsp9a": "Private Stan Sauer",
	"lost": "Lost", "skrsp1": "Meltdown", "crnsp1": "Missile Pass", "level9": "Level 9"}

// CampaignCount is how many campaigns there are.
func CampaignCount() int { return len(campaigns) }

// MapLabel is the board's map as players know it: a campaign's name, else the map.
func (k Key) MapLabel() string {
	if k.Kind == "campaign" {
		if n, ok := campaigns[k.Map]; ok {
			return n
		}
	}
	return k.Map
}

// KindLabel is "flagrun, effic ctf" for a flagrun or race (each mode its own board), else the kind.
func (k Key) KindLabel() string {
	if k.Kind == "flagrun" || k.Kind == "race" {
		return k.Kind + ", " + ModeName(k.Mode)
	}
	return k.Kind
}

// Time is a run time as the game shows it: m:ss.mmm.
func Time(ms int64) string { return fmt.Sprintf("%d:%02d.%03d", ms/60000, ms/1000%60, ms%1000) }
