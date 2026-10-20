# Changes from upstream

This is the Asciiverse edition of ASCII Patrol, Marcin Sokalski's
(Gumix) game inspired by Moon Patrol (C++, 2017 to 2022), built from his
repository, github.com/msokalski/ascii-patrol (the `upstream` remote), at
`bd3bd7c` (2022-05-02), twelve commits after the 1.7 tag (2019-12-23):
Michael Hirsch's CMake build and warning fixes and a colour escape fix.
The game still calls itself "alpha 1.7". The `asciiverse` branch
carries the changes below.

## Licence

LICENSE is the GNU GPL version 3 text and nothing in the repository says
"or any later version", so the catalog gives `GPL-3.0-only`. stb_vorbis
(stb_vorbis.cpp) is public domain or MIT; miniaudio (miniaudio/) is
public domain or MIT-0, its licence shipped as MINIAUDIO-LICENSE. Our
changes are under the GPL version 3.

## Debian

Debian packages it (1.7+dfsg-1, trixie and later). Its one patch makes
the upstream Makefile take the build flags from the environment; it is
not applied, as the release builds use `asciiverse/Makefile`. Debian also
leaves out `html5/` (minified JavaScript without source) and `temp_xp/`;
neither is built or shipped here.

## Code

- **Key releases through the kitty keyboard protocol.** The game wants
  to know when a key is let go (speed up, brake and duck last as long
  as the key is held).
  Upstream gets that from the Linux console's raw keyboard or from X11.
  A terminal that answers the kitty protocol's query (kitty, Ghostty,
  foot, Alacritty, WezTerm with the protocol turned on) now reports every
  press, repeat and release itself, with focus changes, so it works
  under Wayland, on macOS and over ssh. The raw console still comes
  first.
- **X11 loaded at run time, and not under Wayland.** libX11 and libXi are
  opened with dlopen() when needed, so nothing links against X11 and the
  game starts without it. Under Wayland (`WAYLAND_DISPLAY` set) X11 is not
  tried, because X11 cannot see a Wayland terminal's keys; that is most
  likely upstream issue 29 (no keys in GNOME Terminal on Ubuntu 24.04).
  Without the kitty protocol or X11 the game plays with sticky keys, as
  it always could.
- **Sound through miniaudio instead of PulseAudio.** miniaudio 0.11.25
  opens what the system has (PulseAudio, PipeWire or ALSA on Linux, Core
  Audio on macOS) and loads it at run time; without sound the game runs
  silent instead of not starting. The game's mixer is unchanged. Windows
  keeps upstream's DirectSound.
- **Player files in the data folder.** Options, the last game's record,
  the downloaded ranking and screen shots (Tab) go to
  `$XDG_DATA_HOME/ascii-patrol` (`~/.local/share/ascii-patrol`) instead
  of `$HOME`. The `ASCII_PATROL_CONFIG_DIR`, `ASCII_PATROL_RECORD_DIR` and
  `ASCII_PATROL_SHOT_DIR` variables still win, and an `asciipat.cfg` or
  `asciipat.rec` already in `$HOME` is used where it is. Windows keeps
  upstream's `%APPDATA%`.
- **Centred on big terminals, clipped on small ones.** The screen grows
  with the terminal from 80x25 to 160x50. It was written from the
  top-left corner with newlines between rows, so beyond 160x50 it sat in
  the corner and below 80x25 the extra rows scrolled it up. Each changed
  row is now drawn at its own position, centred, and what falls outside
  the terminal is left out; a resize clears and redraws everything.
- **Ctrl+C restores the terminal.** The online ranking is fetched by
  running curl through system(), which ignores SIGINT and SIGQUIT while
  the command runs and then puts back what they were when it started.
  The first fetch starts before the game sets its signal handlers, so
  from then on Ctrl+C killed the game without restoring the terminal (or
  the Linux console's keyboard mode). curl now starts with posix_spawn().
- **Small fixes.** get_input_len() added the buffered byte count to a
  pointer instead of the count; a missing `TERM` was passed to strcmp();
  the curl commands are bounded to their buffer (`sprintf_s` is plain
  `sprintf` outside Windows).
- **Windows build with MinGW.** `<shlobj.h>` in lower case, and `-luuid`
  for `GUID_NULL`.

The online ranking (ascii-patrol.com/rank.php) answered with an HTTP 500
error on 2026-10-20; the code is left as it is, so the Hi Scores table
fills again if the server comes back. Playing does not depend on it.

## Build

`asciiverse/Makefile` builds `asciipat` without CMake: `spec_nix.cpp` and
miniaudio on Linux and macOS (X11's headers needed on Linux, its
libraries not), `spec_win.cpp` on Windows. `.github/workflows/release.yml`
builds glibc Linux on Ubuntu 22.04 with libstdc++ linked in (x86_64,
aarch64), macOS (x86_64, arm64) and a MinGW Windows x86_64 binary that
needs only system DLLs, and `asciiverse/package.sh` packs each with
LICENSE, README.md, PATCHES.md and MINIAUDIO-LICENSE. Upstream's CI
workflow, which builds against PulseAudio, is removed. The upstream
Makefile and CMakeLists.txt still work, without PulseAudio or X11
libraries.
