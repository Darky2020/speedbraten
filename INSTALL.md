# Installing speedbraten

Speedbraten runs on top of Sauerbraten 2020 Edition and uses its game data. Install
Sauerbraten first, then add speedbraten to it. Your Sauerbraten installation keeps working as
before, and both share your settings.

Downloads: the [latest release](https://github.com/Darky2020/speedbraten/releases/latest).
Each package is listed in `SHA256SUMS` next to it.

| System | Package | Needs |
|---|---|---|
| Windows 10/11 (x64) | `speedbraten-<version>-windows-x64.zip` | Sauerbraten 2020 for Windows |
| Linux x86_64 | `speedbraten-<version>-linux-x86_64.tar.gz` | Sauerbraten 2020 for Linux, glibc 2.29+ (Ubuntu 20.04, Debian 11, Fedora 30 or newer), SDL2 |
| Linux ARM64 | `speedbraten-<version>-linux-aarch64.tar.gz` | same as above |
| macOS 11+ (Apple Silicon and Intel) | `speedbraten-<version>-macos.zip` | Sauerbraten 2020 for macOS |

Get Sauerbraten 2020 from [sauerbraten.org](http://sauerbraten.org/) if you do not have it.

## Windows

1. Find your Sauerbraten folder: the one that contains `sauerbraten.bat` (for example
   `C:\Program Files (x86)\Sauerbraten`).
2. Right-click the downloaded zip, choose **Properties**, tick **Unblock** if it is shown, and
   click **OK**. This keeps Windows from blocking the game after unpacking.
3. Unpack the zip into the Sauerbraten folder. You should now have `speedbraten.bat` next to
   `sauerbraten.bat`, and a new `bin64_speedbraten` folder. Windows asks for administrator
   rights if the folder is under `Program Files`.
4. Start the game with `speedbraten.bat`.

If Windows shows "Windows protected your PC", click **More info**, then **Run anyway**. This
happens because the game is not signed with a paid certificate.

## Linux

1. Install SDL2 if Sauerbraten does not already run:
   - Debian, Ubuntu: `sudo apt install libsdl2-2.0-0 libsdl2-image-2.0-0 libsdl2-mixer-2.0-0`
   - Fedora: `sudo dnf install SDL2 SDL2_image SDL2_mixer`
   - Arch: `sudo pacman -S sdl2 sdl2_image sdl2_mixer`
2. Unpack the tarball into your Sauerbraten folder (the one with `sauerbraten_unix`):

   ```sh
   tar xzf speedbraten-<version>-linux-x86_64.tar.gz -C ~/sauerbraten
   ```

3. Start the game:

   ```sh
   ~/sauerbraten/speedbraten.sh
   ```

## macOS

1. Install `Sauerbraten.app` into **Applications** (or `~/Applications`) if it is not there.
2. Unzip the download and move `Speedbraten.app` into **Applications** as well.
3. Open `Speedbraten.app`. The first time, macOS refuses because the app is not notarized:
   open **System Settings → Privacy & Security**, scroll down to the message about
   Speedbraten, click **Open Anyway**, and confirm. You only need to do this once per version.

   Or, in Terminal: `xattr -dr com.apple.quarantine /Applications/Speedbraten.app`

If Speedbraten says "Sauerbraten not found", `Sauerbraten.app` is not in **Applications**,
`~/Applications` or the same folder as `Speedbraten.app`.

## Where your files are

Speedbraten uses the same home folder as Sauerbraten:

| System | Home folder |
|---|---|
| Windows | `Documents\My Games\Sauerbraten` |
| Linux | `~/.sauerbraten` |
| macOS | `~/Library/Application Support/Sauerbraten` |

- `config.cfg` keeps your Sauerbraten settings and binds, shared with Sauerbraten.
- `speedbraten.cfg` keeps the speedbraten-only settings (timer, speedometer, replays).
- `replays/` holds your recorded runs; `replays/kept/` the ones you chose to keep.
- `log.txt` is the log of the last session.

To use a different home folder, start speedbraten with `-q<folder>`, for example
`speedbraten.sh "-q$HOME/speedbraten-test"`.

## Reporting a problem with a replay

Every replay stores checks of the game state. When a replay plays back differently from how
it was recorded, the console shows a red line such as
`replay desync at tick 8672 (43.355 s): player monsters`.

If that happens, or a replay looks different on another computer:

1. Note which replay it was and on which computer it was recorded.
2. Quit speedbraten, so that `log.txt` is complete.
3. Open an [issue](https://github.com/Darky2020/speedbraten/issues) and attach:
   - the replay file (`.sbr`) from `replays/`,
   - `log.txt` from your home folder,
   - your operating system and version, your CPU (for example "Windows 11, Ryzen 5 5600"),
     and the speedbraten version (shown in the console at startup: `p1xbraten version: ...`).
