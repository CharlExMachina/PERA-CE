# Notes for agents

This repository ports the Halo: Combat Evolved decompilation (Xbox build
01.01.14.2342, `cachebeta.exe`) to Linux, Windows, macOS, Android, the web and
(as a developer build) the iPhone. The game's C
sources are the decompilation; the port adds a platform layer that
implements the Xbox APIs the game calls (Direct3D on OpenGL, or on Metal on
the Mac), and features the Xbox game did not
have (a new netcode, internet play, the PC version's menus, Custom Edition
maps, touch controls). The project is published as OpenCE; it also carries
the work of CoreCE, a fork of it from the same base.

Start with [README.md](README.md). The other documents are listed at the
end.

## Layout

| Path | Contents |
| --- | --- |
| `source/` | The game: 466 C files of the decompilation, compiled as they are with few changes (each marked, below) |
| `port/linux/src/` | The platform layer the three ports share: Direct3D 8 on OpenGL (`d3d8_gl.c`), DirectSound and XInput on SDL3, files, threads, memory, sockets, settings (`port_config.c`), the mods folder (`port_mods.c`), internet play (`p2p*.c`) |
| `port/linux/game/` | The port's game-side code, compiled as the game's sources are: the distributed netcode (`network_*.c`), co-op, map sharing (`map_share*.c`), the menus (`menu_tags.c`, `menu_functions.c`), Custom Edition maps (`custom_edition_*.c`, `cache_file_formats.c`), the tag validator (`tag_validate.c`, `tag_schema_*.c`), mods (`mods.c`, `mod_*.c`), streamer mode's word filter (`text_filter*.c`), touch, FOV and more |
| `port/linux/include/` | The port's headers: the limits (`halo_port_limits.h`, with `HALO_PORT_NETWORK_VERSION`), capacities (`halo_port_capacity.h`), the prefix header, `halo_linux_source_fixups.h` (port functions the game's sources call) |
| `port/include/xdk/` | Stand-ins for the Xbox SDK's headers (declarations only) |
| `port/windows/` | The Windows build's own files (`win32_*.c`, headers) |
| `port/android/` | The Android app: the guest runtime (the macOS build's too), the host library (`host/`), the Java app (`app/`), host imports (`host_imports.list`) |
| `port/macos/` | The macOS app: the host (`host/`: loader, window memory, Linux system calls on Darwin, SDL, Metal), the guest's Metal Direct3D device and desktop SDL calls (`guest/`), the Metal interface (`include/halo_metal.h`), host imports (`host_imports.list`) |
| `port/ios/` | CoreCE's iPhone app (a developer build): the app's files, its host imports and the Xcode project's inputs; built with `tools/ios_build.py` |
| `port/apple/` | The iPhone app's host pieces: the host (`host/`), the guest compiler's pieces (`compiler/`), native code (`native/`), graphics through ANGLE, the game data import (`game_store.m`, `xiso.c`), licences |
| `port/web/` | The web build: its runtime (`src/`: entry, sockets inside the page, memory watch), the page and its service worker (`site/`), the SDL port, tests |
| `port/assets/` | What the builds embed or ship: high-res HUD (`hud/`), fonts, titles, menus (`menus/`), icons, network brokers |
| `port/third_party/` | Vendored libraries, each with a README naming its upstream, version and checksum |
| `port/tools/` | Stand-alone tools (`cache_file_report.c`) |
| `tools/` | Build scripts (`linux_build.py`, `windows_build.py`, `android_build.py`, `macos_build.py` with `macos_asm_rebase.py`, `macos_imports.py` and `macos_image_check.py`, `web_build.py`, `ios_build.py` with `ios_setup.py`, `ci_build.py`, `web_serve.py`), generators (`ce_menus.py`, `port_settings.py`, `hud_assets.py`, `title_assets.py`, `embed_assets.py`, `xdk_headers.py`), tests, and `stagekit/` (scenarios staged and instrumented on the real engine, no retail assets) |
| `docs/` | Longer design notes (`custom_edition_caches.md`, `mods.md`, `xbox-fidelity.md`, the upstream reviews) |
| `pgo/` | Profile-guided optimisation profiles |
| `assets/` | Game data for local runs (gitignored: `maps/`, `custom_maps/`) |
| `.github/workflows/` | CI: builds every platform on each push, publishes releases from `main` |

## Building

```sh
python configure.py            # debug build; add --release, --portable, --profile, --pgo=off, --lto=off
ninja linux                    # build/linux/halo
ninja android                  # the Android guest image and native libraries
ninja android_apk              # the APK (Gradle)
ninja web                      # build/web/site (Emscripten: port/web/README.md)
ninja windows                  # on Windows, or on Linux after configure.py --windows-cross
ninja macos                    # build/macos/OpenCE.app, on an Apple silicon Mac only
python tools/ci_build.py linux release   # what CI builds
```

- `configure.py` writes `build.ninja`; `configure_args` at its top records
  the options it was run with. Restore them after trying other options.
- The Linux and Windows builds are 32-bit x86 with clang, in MSVC's ABI
  (`-fms-extensions -fshort-wchar -malign-double -fcommon`), C89 (`gnu89`):
  declarations come before statements.
- Every native build compiles without fused multiply-add and with the
  vendored musl maths (`halo_math.h`), so that every machine of a network
  game computes the same results. Do not call the system's maths functions.
- Android: the game is an ILP32 AArch64 guest (`arm64_32`) in a 64-bit app.
  Guest code can call only the host imports declared in
  `port/android/host_imports.list` and `port/android/guest/runtime/guest_host.h`,
  and only the SDL functions `guest_sdl.c` bridges. Run `ninja android`
  after changing anything it compiles: `port/linux/src`, `port/linux/game`,
  `port/android`, `source/`, the build scripts.
- macOS: the game is the same ILP32 AArch64 guest as Android's, run in a 4 GB
  window of the process at a 4 GB boundary (macOS maps nothing below 4 GB):
  `tools/macos_asm_rebase.py` rewrites every load, store and indirect branch
  of the compiler's assembly to go through the window's base in `x28`, and
  `tools/macos_image_check.py` refuses an image with one that does not.
  Guest code must not be assembly the rewrite has not seen. Host functions
  take pointers the import stubs rebased (`tools/macos_imports.py`, from the
  prototypes in `port/macos/include` and `guest_host.h`) and return guest
  addresses or handles. The guest's pages are 16 KB. Run `ninja macos`
  after changing what it compiles (as for Android, and `port/macos`).
  Refer to [port/macos/README.md](port/macos/README.md).
- iOS: the game is the Android build's guest (`HALO_ANDROID`, `HALO_GLES`,
  `HALO_ARM64_GUEST`, with `HALO_IOS`), drawn through ANGLE on Metal, in
  CoreCE's iPhone app; `tools/ios_build.py` builds it on an Apple silicon Mac
  (not `ninja`). `HALO_MACOS` is the Metal Mac build's, never the iPhone's:
  code for the iPhone is under `HALO_IOS`. Refer to
  [port/ios/README.md](port/ios/README.md).
- Web: the game is `wasm32` (also ILP32), run on a Web Worker; calls must
  match their callee's type exactly (the link fails on a mismatch), and
  WebGL 2 is ES 3.0. Run `ninja web` after changing what it compiles (as
  for Android, and `port/web`). Refer to [port/web/README.md](port/web/README.md).

### Defines

| Define | Set by | Meaning |
| --- | --- | --- |
| `HALO_ANDROID` | Android | The app: its display, input, files and lifecycle |
| `HALO_GLES` | Android, web | The OpenGL ES renderer |
| `HALO_ARM64_GUEST` | Android | The guest's ABI (ILP32 AArch64) |
| `HALO_WEB` | Web | The browser: its frames, storage, sockets and WebGL 2's limits (with `HALO_GLES`, not `HALO_ANDROID`) |
| `HALO_METAL` | macOS | The Metal renderer (`port/macos/guest/d3d8_metal.c` in place of `d3d8_gl.c`); with `HALO_ARM64_GUEST` |
| `HALO_MACOS` | macOS | The Mac app: its files and window (with `HALO_ARM64_GUEST` and `HALO_METAL`, not `HALO_ANDROID` or `HALO_GLES`) |
| `HALO_IOS` | iOS (`tools/ios_build.py`) | iOS: CoreCE's iPhone app (with `HALO_ANDROID`, `HALO_GLES`, `HALO_ARM64_GUEST`); `HALO_IOS_SIMULATOR` for the simulator's |
| `HALO_WINDOWS` | Windows (`halo_windows_prefix.h`) | The Windows build |
| `HALO_RELEASE` | `--release` | No assertions checked |
| `HALO_PROFILE` | `--profile` | The profiling build; a normal build must be unchanged by it |

`HALO_LINUX` and `HALO_CUSTOM_EDITION` are **not** defined by any build:
code guarded by them compiles to nothing. Custom Edition support is a
setting (`game.custom_edition`), checked at run time with
`custom_edition_cache_tags_loaded()`.

## Testing

```sh
python -m pytest -q tools/harness tools/test_touch_menu.py   # the engine's functions in a fake world
python -m pytest -q tools/stagekit/tests                    # scenarios on the real engine, no retail assets (Linux)
python -m pytest -q tools/test_linux_port.py                # the build, the maps' checks (needs assets/maps)
python -m pytest -q tools/test_mods.py                      # mods played over a map (needs assets/maps)
python -m pytest -q tools/test_bots.py                      # the bots in local games (needs assets/maps)
python -m pytest -q tools/test_cache_file_formats.py tools/test_bmp_files.py tools/test_profile.py
python tools/test_touch_layout.py                           # the Android touch layout (JDK 17+)
python tools/test_light_storage.py
python tools/test_death_timing.py
python -m pytest -q tools/test_web_build.py                 # the web build's graph, sockets, menus
node --test "port/web/tests/*.test.js"                      # the web page's importer and service worker
python -m pytest -q tools/test_macos_asm_rebase.py           # the macOS guest's rewrite (runs a guest on an Apple silicon Mac)
node port/web/tests/webrtc_native.mjs chromium firefox      # the native builds' WebRTC with browsers (Playwright)
python -m pytest -q tools/test_net_compare.py               # net_compare.py's judging of logs, its copies and their processes
python tools/net_compare.py --a build/linux/halo --b <other build> --data <game data> --swap
                                                            # two builds in one network game, in step (retail maps: local only)
```

`tools/harness/README.md` explains how to add an asset-free test: the code
under test is taken from the sources, not copied. `tools/stagekit/README.md`
explains how to add a scenario: the real engine, with no retail assets.

Running the game without a person at it:

- `debug.network_test` (`HALO_NETWORK_TEST=host:<map>` or `join`) hosts or
  joins a game without the menus and logs each player's state every second;
  several copies play on one machine on different loopback addresses
  (`HALO_NET_ADDRESS`, `HALO_NET_BROADCAST`). `NETCODE.md` describes the
  test settings.
- `debug.exit_after`, `debug.hidden_window`, `debug.null_renderer`,
  `debug.menu_open` (start on one menu screen), `debug.telnet_console`.
- Use a virtual display (Xvfb, with `SDL_VIDEO_DRIVER=x11`) for windowed
  runs, never the desktop of the person you work for, and stop only the
  processes you started.

## Rules of the project

### Project direction

Preserve original Xbox Halo: Combat Evolved NTSC gameplay and presentation
while improving native performance, portability and online play. Community
maps are separate content imports. Custom maps and explicitly selected
competitive options serve serious OG Halo players; they do not authorize
adopting another engine's rules as the original-game baseline. Keep optional
host tools separate and off by default.

### Fidelity and performance are separate checks

Preserve the original 30 Hz simulation. High-refresh rendering (for example
120 FPS) can be appropriate without changing the game rules; review interpolation
and camera/input latency separately. A 30 FPS reference preset is a comparison
tool, not a requirement for normal play or a reason to reject performance work.
Do not silently impose a rendering cap as part of a gameplay-fidelity change.

Matching decompilation progress refers to the original executable target and
build configuration. Do not describe a native ARM executable as byte-identical
to an Xbox executable, or a successful smoke test as proof of full retail parity.
[docs/xbox-fidelity.md](docs/xbox-fidelity.md) records the baseline, the
upstream changes left out, the current settings and the validation limits.

### Reviewing upstream changes

Changes from the upstream projects (OpenCE, `OpenCommunityEdition/OpenCE`,
formerly `cybersecurity/halo-ce-universal`; and CoreCE) are reviewed before
they are integrated: fetch and review all new commits since the last recorded
review, and inspect the actual diff and its dependencies, not just the commit
title.

- Prioritize verified decompilation/matching accuracy, correctness, stability,
  performance, renderer correctness and platform compatibility fixes that
  preserve the original game's behavior and appearance.
- Consider netcode and matchmaking improvements case by case. Review their
  effects on simulation timing, input, fairness, session compatibility and
  service dependencies before adoption. Their category alone is not approval.
- Changes to sounds, weapons, balance or game rules do not belong in the
  original-game baseline; timers and other competitive modifications are
  settings, off by default (Settings, below).
- Split mixed commits or port the relevant fix when possible. Do not take an
  unwanted modification merely to obtain a performance or matching improvement;
  defer the change with a reason if it cannot be separated safely.
- Record each reviewed commit's hash, purpose, decision (adopt, partially adopt,
  exclude or defer), behavioral effect and relevant validation in
  [docs/xbox-fidelity.md](docs/xbox-fidelity.md) or a linked review record
  (`docs/upstream-review-*.md`). Track the last reviewed revision separately
  from the last integrated baseline so excluded commits are not accidentally
  reintroduced during later updates.
- Run checks appropriate to the selected change. Assess performance with
  comparable settings and workloads; verify gameplay-sensitive changes against
  the chosen Xbox reference and record any remaining uncertainty.

### The game's sources

- Change `source/` as little as possible, and mark each change from the
  original with a comment that starts `port:`. Write like the surrounding
  code: its naming, its comment density, and the project's plain,
  descriptive comment style.
- A port function the game's sources call is declared in
  `halo_linux_source_fixups.h` or included with a note naming its file
  (`#include "view_fov.h" /* port: port/linux/game/view_fov.c */`).
- Notes such as `BUG (original, preserved for exact matching)` mark the
  original's bugs. Fixing one is allowed; say so in a `port:` comment.

### The Xbox SDK

`port/include/xdk` was written without the SDK's headers, from the January
build's debug information and public documentation. Never read the SDK's
headers or anything derived from them, never copy from them, and do not
read `xbox/include`. To add a name, follow `port/include/xdk/README.md`.

### Networking

- Raise `HALO_PORT_NETWORK_VERSION` (`port/linux/include/halo_port_limits.h`)
  with any change to what the machines send each other, or to anything they
  must agree on, and say why in the commit. Machines of different versions
  do not play together.
- Only the host decides; clients predict their own players. Read
  `port/linux/NETCODE.md` before changing the netcode.
- Internet play is the same code in every build, the web's too: a browser's
  tunnel is a WebRTC data channel (`port/web/src/web_p2p.c`), which the
  native builds take on their tunnel's socket (`p2p_webrtc.c`, `posix_dtls.c`).
  Check a change with both (`port/web/tests/webrtc_native.mjs`, and with the
  maps `port/web/tests/internet.mjs`).
- Anything another machine sends is untrusted: check sizes, indices and
  rates, as the existing handlers do. `debug.network_corrupt` tests this.

### Menus

- The menus must match Halo Custom Edition's. Their XML (`port/assets/menus/ce/`)
  is generated by `tools/ce_menus.py` from the PC version's tags; the
  settings screens come from `tools/port_settings.py`. Do not edit the XML
  by hand: change the generator and run it (the command is in
  `port/assets/menus/README.md`, "Writing them again"), then check that
  `git status` shows only the screens meant to change.
- Functions the menus call but the port has not wired are listed in
  `port/assets/menus/UNWIRED.md`.

### Art and credit

- Replacement HUD, title and menu art is made of hand-drawn redraws.
- Prefer an existing redraw (the ui-svg-handmade set), then a new redraw. A
  Custom Edition picture that cannot reasonably be redrawn (a 3D render, a
  screenshot, a logo) may be used only if it is listed in
  `port/assets/menus/NON_HANDDRAWN.md`.
- `AUTHORS.md` credits art only.
- Fonts: Overpass and CoreCE (Newtown, respaced) only; see
  `port/assets/fonts/README.md`. Never ship the maps' commercial typefaces.

### Custom Edition maps

Custom Edition maps live in the data root's `custom_maps` folder with
`bitmaps.map`, `sounds.map` and `loc.map`; their level names are
`custom_maps\<name>`. OpenSauce features are not supported and must not be
added: a map that needs them is refused, and one that only carries
OpenSauce's header runs as stock Custom Edition runs it. Map files are
untrusted input: read them through bounds-checked readers. See
`docs/custom_edition_caches.md`.

### Settings

A setting is an entry in `port/linux/src/port_config.c`, with a default, an
environment variable and a comment that `config.toml` gets. Document it in
the settings table of `port/linux/README.md` (and the Android README for an
Android-only one). A departure from the stock game's look or behaviour
should be a setting, off or stock by default, unless the project decides
otherwise.

### Third-party code and licences

The repository is `AGPL-3.0-or-later` (`LICENSE`); the commits before the
change, and the releases built from them, stay CC0 (`LICENSES/CC0-1.0.txt`).
The README's "Licence" section says what each part is under. Vendored code
goes in `port/third_party/<name>`, copied unchanged, with a README naming
the upstream, the version or commit, the licence, the checksum and what uses
it. Its licence must let it be combined with the AGPL 3.0: MIT, BSD (without
an advertising clause), ISC, zlib, Apache 2.0, MPL 2.0, LGPL or GPL 3.0 or
later, public domain. GPL-2.0-only code and the 4-clause BSD licence cannot
be. Code taken from another project, GPL 3.0 code included, keeps its
notices and its licence, and says where it came from.
The decompilation's upstream, punpckhdq/halo, is AGPL 3.0 from 17 October
2026; bnunu/halo-1 is CC0.

### Commits and pull requests

- Commit subjects are plain sentences saying what changed; the body says
  why and what was checked.
- A pull request is merged with a merge commit (`Merge pull request #N from
  <owner>/<branch>`), and follow-up changes go in separate commits whose
  subject ends with `(#N)`.
- A commit taken from a fork keeps its author and is made with
  `git cherry-pick -x`.

## Documents

| Document | About |
| --- | --- |
| [README.md](README.md) | Downloads, game data, building, build options, profiling builds |
| [port/linux/README.md](port/linux/README.md) | The Linux build, controls, menus, every setting, updates, frame rate, field of view, system link, internet play, voice chat, the server browser, map sharing, security, map checks, how the port operates, the game source changes |
| [port/linux/NETCODE.md](port/linux/NETCODE.md) | The distributed netcode, network versions, joining a game in progress, map sharing, transport, testing |
| [port/windows/README.md](port/windows/README.md) | The Windows build, its headers, inline functions, crash reports |
| [port/android/README.md](port/android/README.md) | The Android build, the guest and host, touch controls, Android settings, finding problems |
| [port/macos/README.md](port/macos/README.md) | The macOS build, the guest's window and its rewrite, the host, Metal, finding problems |
| [port/ios/README.md](port/ios/README.md) | The iPhone developer build: building, signing, installing, the simulator |
| [port/web/README.md](port/web/README.md) | The web build, the page, the game data in the browser, limits, how it operates, hosting on GitHub Pages |
| [port/include/xdk/README.md](port/include/xdk/README.md) | How the SDK declarations were written and are checked |
| [docs/custom_edition_caches.md](docs/custom_edition_caches.md) | Loading and converting Custom Edition maps, and the evidence for each layout |
| [docs/mods.md](docs/mods.md) | Mods: folders of Halo PC tag files whose sounds, bitmaps, text and models play over a map's on one machine |
| [docs/xbox-fidelity.md](docs/xbox-fidelity.md) | The original Xbox baseline, the upstream changes reviewed and left out, validation limits |
| [docs/performance-options.md](docs/performance-options.md) | The Performance Build (PB) options |
| [port/assets/text_filter/README.md](port/assets/text_filter/README.md) | Streamer mode's words: Surge AI's list, the review, the allowed words |
| [port/assets/menus/README.md](port/assets/menus/README.md) | The menu files' format and how to write them again |
| [port/assets/menus/NON_HANDDRAWN.md](port/assets/menus/NON_HANDDRAWN.md) | Menu pictures that are not redraws |
| [port/assets/menus/UNWIRED.md](port/assets/menus/UNWIRED.md) | Menu functions that do nothing yet |
| [port/assets/fonts/README.md](port/assets/fonts/README.md) | The fonts and their licences |
| [tools/harness/README.md](tools/harness/README.md) | Asset-free regression tests |
| [tools/stagekit/README.md](tools/stagekit/README.md) | Scenarios staged and instrumented on the real engine, no retail assets |
| `port/third_party/*/README.md` | Each vendored library |
| [AUTHORS.md](AUTHORS.md) | Art credits |
