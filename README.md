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

Open **speedrun..** in the main menu for the settings.

- **Run timer** with millisecond precision, for single-player, DMSP and multi-map runs. SP runs
  start when the map loads and end when the map ends; flagruns start once you carry the flag
  out of its base and end when you score.
- **Flagrun rules** for offline CTF: instant flag return and respawn, either flag scores, no
  capture limit.
- **Race rules** for offline ffa and coop edit: the timer starts at every spawn and stops when
  you touch any flag, items respawn at once, and your own shots push you without damaging you.
  Entering edit mode voids the attempt.
- **Replays** of every run, saved automatically. The replay browser lists, filters, sorts,
  renames, keeps and plays them. A replay checks the game state while it plays, so a replay
  that does not match its recording says so ("replay desync at tick ...").
- **Restart run** and **suicide + respawn** keys, and a speedometer (horizontal or 3D speed,
  optional smoothing).
- **sauer_resim**: a headless build (`make resim` in `src/`) that re-simulates a replay without a
  window and prints the result as JSON, for checking runs automatically.

Found a replay that plays back differently? See
[reporting a problem](INSTALL.md#reporting-a-problem-with-a-replay).

## Roadmap

- [ ] Save states for practice
- [ ] Run statistics
- [ ] Custom backgrounds
- [ ] Server for watching other players' runs live, with chat
- [ ] Run submission site that re-simulates submitted runs
