# Changelog

## Unreleased

- **New replay file format (SBR2).** Replays from v0.1.0 can't be read any more.
  - Files are split into 1-second segments, each with a CRC, zstd compression and a BLAKE3
    hash chain; the whole file ends in a content hash.
  - A replay now also holds the raw keyboard and mouse input, frame timing, the hashes of the
    map files it was played on, and how the run was produced (client build, session, history,
    window, input settings).
  - Recorded commands are limited to a fixed list; anything else is refused on playback.
  - `replayverify <file>` checks a replay for leaderboard use (map files, timing, pause,
    gamespeed, fixedframe, provenance, raw input) and its witness signatures.
  - `statsexporthistory <file>` exports the chain over all your recorded attempts, which
    `tools/sbr2.py history` can check replays against.
  - `tools/sbr2.py` reads and checks replay files outside the game.
  - Replays list the connected keyboards, mice and other input devices (vendor and product ID,
    connection, product name; no serial numbers) and the `maxfps` setting.
- Runs played with a changed game speed, single-player slow motion (`slowmosp`), a pause or
  `fixedframe` are recorded but never count as PB, best segment or PB ghost. Replays flag them.
  - Replays record which aids were on during the run (ghost, speedometer, splits, third person,
    fullbright, custom or forced spawns, …), the run's category, and the OS and CPU model.
- Choose which ghost to race per map and mode: your PB, your segmented PB, or any replay (for
  example a world record copied into `replays/`), with your own label. The choice is saved, and
  the label shows above the ghost and on the time difference line. Replays made before v0.1.0
  have no ghost path and can't be chosen.
- With a chosen ghost replay, every split and the finish also show the difference to it, next
  to the PB difference. For a replay from someone else, zone splits are worked out from its
  ghost path; pickup, teleport and condition splits only compare with your own runs.
- `ghostdiffwindow` (seconds, default 3) sets how far ahead or behind the ghost you can be
  and still see the live time difference.
- When a replay ends, the player no longer keeps moving or shooting with the replay's last
  inputs.
- Weaponless flagruns keep the pistol selected; switching to the chainsaw is blocked.
  Weaponless replays made with v0.1.0 that switched weapons will desync.
- Setting a flagrun spawn starts a one-second countdown under the crosshair; moving cancels it.
- The ghost isn't shown while you watch the replay it comes from.
- Loading a state clears particles, decals, dynamic lights and ragdolls from before the load,
  instead of playing them backwards.
- Fixed: after a CTF game (flagruns included), the next map could put you in the middle of the
  map instead of at its start, because the spawn still used the CTF team's spawn points. Going
  the other way, the first spawn of a flagrun after a non-CTF game ignored the team spawns.
  Replays recorded with the wrong spawn desync.
- Races: every item, including quad damage and the health boost, is there from the start
  instead of appearing after up to 70 seconds, and there is no "will spawn in 10 seconds"
  announcement. Race replays recorded before this will desync.
- Races: a picked-up item still comes back at once, but you can only pick it up again after
  leaving it. Standing on quad damage used to pick it up hundreds of times a second and lag the
  game.
- New flagrun option: walking back onto the flag after taking it restarts the timer, so it only
  starts once you really leave the flag (no flag drop needed). Off by default; replays keep
  the setting they were recorded with.

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
