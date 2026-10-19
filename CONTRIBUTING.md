# Contributing to speedbraten

Bug reports, replays that desync, map fixes and code are all welcome. Open an issue or a pull
request on GitHub.

## Building

The repository holds the code and speedbraten's own files; the game data (maps, models,
textures) comes from a Sauerbraten 2020 installation.

### macOS (development build)

Needs the Xcode command line tools (`xcode-select --install`). The SDK headers, enet and the SDL2
frameworks are in the repository (`src/macos/`, `sauerbraten.app/Contents/Frameworks`), and
they're x86_64 only, so this build is x86_64 and runs under Rosetta on Apple Silicon.

1. `cd src`
2. `make -j8 client` builds `src/sauer_client`; `make -j8 resim` builds `src/sauer_resim`.
3. Run it from the game data of an installed `Sauerbraten.app`:
   ```sh
   cd /Applications/Sauerbraten.app/Contents/Resources
   DYLD_FRAMEWORK_PATH=/Applications/Sauerbraten.app/Contents/Frameworks \
     <repo>/src/sauer_client -q<a test home dir> -o<repo>/data/overrides -glog.txt
   ```
   `-q` keeps your normal home folder out of it. `-o` loads the map fixes from the repository;
   without it the client looks for `speedbraten_overrides/` in the current folder, which a
   development checkout doesn't have.

### Linux (development build)

1. Install clang, make and the SDL2 and zlib development packages, e.g. on Debian/Ubuntu:
   `sudo apt install clang make libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev zlib1g-dev`.
2. `cd src && make -j8 client` builds `src/sauer_client` (enet is built from `src/enet/` on the
   way); `make -j8 resim` builds `src/sauer_resim`.
3. Run it from your Sauerbraten 2020 folder (the one with `data/` and `packages/`):
   `<repo>/src/sauer_client -q<a test home dir> -o<repo>/data/overrides -glog.txt`.

### Release builds (all platforms)

Windows clients are only cross-compiled, and release builds for every platform use
[zig](https://ziglang.org) 0.15.1, the version CI uses. From the repository root:

- `tools/clientbuild.sh <target> <out dir>` with target `x86_64-windows-gnu`,
  `x86_64-linux-gnu`, `aarch64-linux-gnu`, `x86_64-macos` or `aarch64-macos`.
  - Linux targets build natively on that architecture against the distro's SDL2 packages (see
    above), linked for glibc 2.31.
  - Windows links against the libraries in `src/windows/`; macOS against the SDK in `src/macos/`
    and the SDL2 frameworks in `sauerbraten.app/Contents/Frameworks`.
  - `aarch64-macos` needs arm64 SDL2 frameworks, and the ones in the repository are x86_64
    only. First replace them with the official universal SDL2 2.32.10, SDL2_image 2.8.12 and
    SDL2_mixer 2.8.2 frameworks (the `.dmg` downloads of the libsdl-org GitHub releases), as
    the "universal SDL2 frameworks" step in `release.yml` does.
- `tools/resimbuild.sh <target> <out dir>` builds `sauer_resim` the same way, compiling zlib
  and enet from source.
- The `release` workflow (`.github/workflows/release.yml`) is the reference: it runs these
  scripts and combines the two macOS builds into one universal binary.

### Adding source files

Add a new `.cpp` file to `CLIENT_OBJS` in `src/Makefile` and its dependency lines at the end of
the Makefile, copying an existing `fpsgame/*.o` entry. Without them, header changes won't
rebuild it.

## Determinism

Offline games must stay deterministic: the same map and inputs give the same run on every
platform. Replays and the gold tests depend on it.

- **The simulation runs in fixed 5 ms ticks.** Anything that changes game state belongs inside
  a tick. Rendering, HUD, sound and statistics must only read it.
- **Player input reaches the game through the input recorder** (`src/fpsgame/inputs.cpp`).
  - A command that changes game state must be recorded: call `deferinput(INPUT_COMMAND, 0, cmd)`
    first and return if it took the command, as `respawn` and `setspawnat` do. Otherwise
    replays won't contain it.
  - Settings that change the simulation go into `simsettings`. A new one also needs an entry in
    `latesettings`, with the value that matches replays made before it.
- **The state hash** (`src/fpsgame/dethash.cpp`) covers everything that affects gameplay. New
  gameplay state should be hashed.
- **Save states** (`src/fpsgame/savestate.cpp`) must save and restore all of it. Never change
  the layout of an existing section, since replays carry saved states. Add a new section at the
  end of the table, with a reset function for states saved before it.
- **No fast math:** the compiler flags (`-fno-fast-math -ffp-contract=off`) and
  `src/shared/detmath.h` keep floating point identical across platforms. Don't change them
  without the `fpconform` workflow passing.
- **Replay file format** (`src/fpsgame/replayfile.cpp`): bump `REPLAYVERSION` for a new field and
  keep reading every older version. Bump `REPLAYSIMVERSION` only when the simulation itself
  changes so that older replays can no longer play back.

## Testing

- **Gold replays** in `tests/gold/` are real runs that must keep playing back exactly. CI
  re-simulates each one with `sauer_resim` on five platforms (the `resim` workflow) and plays it
  in each client before a release (`release`). A change that makes a gold desync breaks
  existing replays.
  - Locally, run `sauer_resim -q<empty home> -o<repo>/data/overrides <gold.sbr>` from an unpacked
    `resimdata-v2` pack (a release asset) and check `"ok":true` in the output.
- **Harness** in `tools/dethash/`: `run.sh` starts the client with a scenario script (`*.cfg`) and
  a fixed clock, and writes a per-tick state hash log; `compare.py` finds where two logs differ.
  - Other scripts: `resimcheck.sh` checks client and `sauer_resim` agree on a replay;
    `snapcycle.sh` saves and restores the state every tick and checks nothing changes;
    `clientgold.sh` plays the golds in a client.
  - Harness commands such as `harnessflag` move the player without being recorded, so a replay of
    such a run desyncs on purpose. Inside a script's `sleep`, use `harnesscommand <cmd>` for
    commands that must be recorded.
- Before a pull request, re-simulate the golds and, for a change to gameplay or the recorder,
  record and replay a short scenario with `run.sh`.

## Menus and settings

The speedrun menus are `data/p1xbraten/speedbraten.cfg`. The client embeds that file, so after
editing it run `python3 tools/embedcfgs.py` from the repository root and rebuild.

## Map fixes

`data/overrides/` mirrors the game's folders (for example
`data/overrides/packages/base/crnsp1.cfg`). Files there replace the game's and the player's own
copies, and ship with every release as `speedbraten_overrides/`. A map script fix changes
gameplay, so replays recorded before it will desync on that map. Say so in the changelog.

`patches/` is something else: p1xbraten's patches against the original Sauerbraten sources.

## Releases

1. Add a `## vX.Y.Z` section to `CHANGELOG.md`.
2. Push a `vX.Y.Z` tag on `main`.

The `release` workflow then builds every client, plays the gold replays in each of them, and
creates a draft GitHub release with the changelog section as its notes. It fails if the section
is missing. Publish the draft once it looks right.
