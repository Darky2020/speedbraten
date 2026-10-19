# Speedbraten

A client for speedrunning and flagrunning in Cube 2: Sauerbraten, built on top of
[p1xbraten](https://github.com/sauerbraten/p1xbraten) (see
[its docs](https://github.com/sauerbraten/p1xbraten/blob/main/README.md) for the features it
adds).

Offline games in speedbraten are deterministic: the same map and the same inputs give exactly
the same run, at any frame rate and on every supported system. Runs are timed by the game
simulation, not the wall clock, and every run is recorded as a replay that plays back
identically on any other computer.

## Installation

Speedbraten is added to an existing Sauerbraten 2020 installation on Windows (x64), Linux
(x86_64 and ARM64) and macOS 11+ (Apple Silicon and Intel). Download it from the
[releases](https://github.com/Darky2020/speedbraten/releases) and follow
[INSTALL.md](INSTALL.md).

## Features

Open **speedrun..** in the main menu for the settings. Changes per release are in
[CHANGELOG.md](CHANGELOG.md).

### Timing and rules
- **Run timer** with millisecond precision, timed by the game simulation, for single-player,
  DMSP and multi-map runs. SP runs start when the map loads and end when the map ends.
- **Flagruns** in offline CTF, in three categories:
  - **effic** (effic ctf) and **insta** (insta ctf);
  - **weaponless** (plain ctf): no pickups, no ammo, no attacks.

  The timer starts once you carry the flag out of its base and stops when you score. Flags
  return and players respawn instantly, either flag scores, and there is no capture limit.
- **Race rules** for offline ffa and coop edit:
  - the timer starts at every spawn and stops when you touch any flag;
  - items respawn at once, and your own shots push you without damaging you;
  - entering edit mode voids the attempt.
- **Clean attempts:** with flagrun or race rules, killing yourself also removes your rockets and
  grenades still in flight.

### Practice
- **Save states:** save the whole game state with a key and load it back instantly, with an
  adjustable pause before play resumes. **Step back** rewinds to automatic states taken at a
  set interval.
- **Segmented runs:** load states during a run and finish with one complete run. Its replay
  marks every splice, and segmented runs are kept as their own category.
- **Custom flagrun spawn:** stand still for a second and press a key to spawn there from now
  on. It's saved per map and mode.
- **Fixed race start:** pick one of the map's playerstarts so every race starts in the same
  place. Switching respawns you there at once.
- **Restart run** and **suicide + respawn** keys, and a speedometer (horizontal or 3D speed,
  optional smoothing).

### Splits and ghosts
- **PB ghost:** a translucent copy of your best run plays alongside you, with a live time
  difference to it. Your segmented PB, or any replay you pick, can be the ghost too.
- **Splits** per map and run kind, in any order, triggered by:
  - entering a zone marked in edit mode;
  - picking up a given item, or using a given teleport;
  - a map-script condition, such as a door opening or a number of monsters killed.

  The HUD shows each split against your PB and against your best time for that segment, the
  sum of best, and your PB difference at the finish. New best segments are highlighted.
  Split files live in `splits/<map>_<kind>.cfg` in your home folder.

### Replays and statistics
- **Replays** of every run, saved automatically. Flagrun and race replays hold just the life of
  the run.
- **Replay browser:** list, filter, sort, rename, keep and play replays.
- **Desync check:** a replay checks the game state while it plays, and says so if it no longer
  matches its recording ("replay desync at tick ...").
- **Statistics** of every run, in `speedbraten.db` in your home folder:
  - per map and mode: PB, average of the last 10 finishes, finished/started count, time played,
    and a separate segmented PB;
  - per map: a list of runs that plays any run's replay.
- **Cleanup rules** pick replays to delete: short or idle runs, runs that never started,
  replays made with an older build, old replays, or all but the fastest N per map. They only
  run when you ask, with a preview before anything is deleted. Kept replays are never touched.

### Map fixes
- **File patches:** speedbraten ships fixed versions of map files (for example missile path's
  script, `crnsp1.cfg`) in `speedbraten_overrides/`. They take priority over your own copies,
  and a replay warns if it was recorded with a different map script.

### Tools
- **sauer_resim:** a headless build (`make resim` in `src/`) that re-simulates a replay without a
  window and prints the result as JSON, for checking runs automatically.

Found a replay that plays back differently? See
[reporting a problem](INSTALL.md#reporting-a-problem-with-a-replay).

## Roadmap

- [x] Save states for practice
- [x] Run statistics
- [ ] Custom backgrounds
- [ ] Server for watching other players' runs live, with chat
- [ ] Run submission site that re-simulates submitted runs
