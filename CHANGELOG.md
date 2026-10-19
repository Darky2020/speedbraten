# Changelog

## Unreleased

- Choose which ghost to race per map and mode: your PB, your segmented PB, or any replay (for
  example a world record copied into `replays/`), with your own label. The choice is saved, and
  the label shows above the ghost and on the time difference line. Replays made before v0.1.0
  have no ghost path and can't be chosen.
- With a chosen ghost replay, every split and the finish also show the difference to it, next
  to the PB difference. For a replay from someone else, zone splits are worked out from its
  ghost path; pickup, teleport and condition splits only compare with your own runs.

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
