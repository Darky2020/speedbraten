# Changelog

## Unreleased

- Replays are written in a new format while you play. Older replays still play.
- Keys typed into the console or chat are no longer stored in replays.
- In races, an item you just took can be taken again after a respawn.

## v0.2.1

- Races finish the way racing servers detect it. Older race replays no longer verify.

## v0.2.0

### Replays and statistics
- New replay format with integrity checks. Replays from v0.1.0 can't be read any more.
- `replayverify <file>` checks a replay for leaderboard use.
- `statsexporthistory <file>` exports your attempt history.
- `tools/sbr2.py` reads and checks replays outside the game.
- Runs with a changed game speed, `slowmosp`, a pause or `fixedframe` don't count as PBs.
- Finishing after a load, even into an earlier life, saves just that life.
- When a replay ends, the player no longer keeps its last inputs.

### Ghosts and splits
- Choose the ghost per map and mode: your PB, your segmented PB or any replay.
- Splits and the finish also show the difference to a chosen ghost replay.
- `ghostdiffwindow` sets how far from the ghost the live difference still shows.
- The live ghost difference is smoothed (`ghostdiffsmooth`).
- The ghost is hidden while you watch its own replay.

### Flagruns and races
- Flagrun and race rules are on by default and remembered.
- New flagrun option: stepping back onto the flag restarts the timer.
- Setting a flagrun spawn starts a one-second countdown.
- Weaponless flagruns keep the pistol selected.
- Races: all items are there from the start, without an announcement.
- Races: an item can only be picked up again after you leave it.

### Menus
- The speedrun menu has tabs for general, singleplayer, flagruns, races and keys.
- Replay browser: filter by kind, flagrun mode, result, segmented and kept.
- Replay browser: total size next to the count.
- Replay browser: PB replays are marked, with a PBs-only filter.
- Replay browser: assisted runs are marked, with a filter for them.
- Stats: filter by kind, flagrun mode or PB, sort by map, runs or time played, and a PB date column.

### HUD
- Speedometer colour bands by speed.
- Run timer colours: running, before the start, and on finish for a new PB, faster than the ghost or slower.
- Colour picker with RGB sliders and presets for the HUD colours.
- Menu background images from `backgrounds/`, picked from thumbnails: a chosen one or a random rotation with a fade, optionally dimmed.
- Matching your PB or the ghost's time counts as beating it.

### Fixes
- The first spawn after a CTF game could be in the middle of the map.
- Frag messages from the previous map no longer stay on screen.
- Loading a state clears effects from before the load.
- The launch message shows the speedbraten version.

## v0.1.0

The first speedbraten release, on top of p1xbraten. Offline games are deterministic: the same
map and inputs give exactly the same run on every supported system.

### Timing and rules
- Run timer with millisecond precision, timed by the game simulation, for single-player, DMSP
  and multi-map runs.
- Flagruns in offline CTF in three categories:
  - effic (effic ctf) and insta (insta ctf);
  - weaponless (plain ctf): no pickups, no ammo, no attacks.

  Flags return and players respawn instantly, either flag scores, and there is no capture limit.
- Race rules for offline ffa and coop edit:
  - the timer starts at every spawn and stops at any flag;
  - items respawn at once, and your own shots push you without damage.

### Practice
- Save states with a key, with an adjustable pause before play resumes; step back to
  automatic states.
- Segmented runs: load states mid-run and finish with one complete run. The replay marks
  every splice.
- Custom flagrun spawn: stand still for a second and press a key. It's saved per map and mode.
- Fixed race start: pick one of the map's playerstarts. Switching respawns you there at once.
- Restart run and suicide + respawn keys, plus a speedometer.

### Splits and ghosts
- PB ghost with a live time difference, optionally against your segmented PB, or against any
  replay you pick.
- Splits per map, triggered by:
  - zones marked in edit mode;
  - item pickups and teleports;
  - map-script conditions.

  The HUD shows each split against your PB and your best segment, plus the sum of best and the
  PB difference at the finish.

### Replays and statistics
- Replays of every run, saved automatically; flagrun and race replays hold just the life of the
  run.
- Replay browser: list, filter, sort, rename, keep and play. A replay reports when it no
  longer matches its recording.
- Run statistics per map and mode: PB, average of the last 10, finished/started, time played,
  and a segmented PB.
- Replay cleanup rules (short or idle runs, runs that never started, outdated or old replays,
  all but the fastest N) with a preview before deleting.

### Map fixes
- Fixed versions of map files (the missile path script) ship with speedbraten and take priority
  over your own copies. Replays warn when they were recorded with a different map script.
