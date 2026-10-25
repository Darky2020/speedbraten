package board

// FlagrunMaps are the maps flagruns count on: flagruns.com's list. A flagrun on any other map is
// left out of the boards.
var FlagrunMaps = []string{
	"aastha", "abbey", "akimiski", "akroseum", "alloy", "arbana", "asgard", "authentic", "autumn", "bad_moon",
	"berlin_wall", "bklyn", "breakout", "bt_falls", "campo", "capture_night", "caribbean", "casa", "catch22",
	"collide", "core_refuge", "core_transfer", "croma", "ctf_suite", "daemex", "damnation", "depot",
	"desecration", "destiny", "disc", "disruption", "divine", "donya", "duomo", "dust2", "earthsea",
	"earthstation", "enigma", "eris", "eternal_valley", "europium", "evilness", "face-capture", "fc4", "fc5",
	"fire_keep", "flagstone", "forge", "forgotten", "fortress", "fragnostic", "frostbyte", "fusion", "garden",
	"gubo", "hallo", "harbor", "haste", "hidden", "idris", "infamy", "killcore3", "kiryu", "konkuri-to",
	"kopenhagen", "l_ctf", "laucin", "luna", "mach2", "mbt1", "mbt12", "mbt4", "mc-lab", "meltdown2", "memoria",
	"mercury", "metro", "mill", "new_energy", "nitro", "nucleus", "ogrosupply", "overdrive", "ow", "pandora",
	"pul1ctf", "ra", "recovery", "redemption", "regal", "reissen", "risk", "river_keep", "ruebli", "rust",
	"sacrifice", "shellshock2", "shipwreck", "siberia", "skycastle-r", "snapper_rocks", "spcr", "stadium",
	"stronghold", "subterra", "suburb", "surge", "tatooine", "tectonic", "tejen", "tempest", "tortuga",
	"triforts", "tubes", "turbulence", "twinforts", "unworld3", "urban_c", "valhalla", "warlock", "wdcd",
	"xenon",
}

var flagrunMap = func() map[string]bool {
	m := map[string]bool{}
	for _, name := range FlagrunMaps {
		m[name] = true
	}
	return m
}()

// IsFlagrunMap tells whether flagruns on the map count.
func IsFlagrunMap(name string) bool { return flagrunMap[name] }
