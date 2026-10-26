# Halo: Combat Evolved for Linux, Windows, macOS, Android, iPhone and the web

[![Join our Discord](https://invidget.switchblade.xyz/4q7tpz7Gs)](https://discord.gg/4q7tpz7Gs)

This project is a port of the Halo: Combat Evolved decompilation to Linux,
Windows, macOS, Android and web browsers, with a developer build for the
iPhone. The decompilation is of the Xbox build 2342
(`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

The port starts from the decompilation of [bnunu/halo-1](https://github.com/bnunu/halo-1).
That project is a fork of [punpckhdq/halo](https://github.com/punpckhdq/halo).
This is [CoreCE](https://github.com/mastercheese-dev/CoreCE), a fork of
[OpenCE](https://github.com/OpenCommunityEdition/OpenCE): OpenCE's work, with
CoreCE's own (its iPhone app, Hardcore, PB Options and Match Options, and
its fixes).
High-res redraw artists are in [AUTHORS.md](AUTHORS.md).

You can play with up to 128 players, run campaign co-op online, load Custom
Edition maps, change the field of view, browse servers, host peer-to-peer,
download missing maps and use voice chat. With Smooth Motion on, the game
draws a frame for every refresh of the display. The simulation stays at 30
ticks a second, as on the Xbox.

## Download

GitHub Actions builds the game for each commit. These links download the
builds of the latest release:

| Platform | Release | Debug |
| --- | --- | --- |
| Linux | [halo-linux-release.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-linux-release.zip) | [halo-linux-debug.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-linux-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-windows-debug.zip) |
| Android | [halo-android-release.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-android-debug.zip) |
| Web | [halo-web-release.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-web-release.zip), for your own server or the repository's GitHub Pages ([port/web/README.md](port/web/README.md)) | [halo-web-debug.zip](https://github.com/mastercheese-dev/CoreCE/releases/latest/download/halo-web-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion and writes it to the log. Use the debug build to find and report
problems.

The experimental [iPhone port](port/ios/README.md) is a developer build:
build it yourself with `tools/ios_build.py`. These releases have no iPhone
package.

The game updates itself. At start-up it looks for a newer release, and asks
if you want to install it. Refer to "Updates" in
[port/linux/README.md](port/linux/README.md#updates). The web site offers
each new build when it is out.

Each build of the `main` branch that passes on all four platforms is a new
release, and the web site then plays it. The [Releases](https://github.com/mastercheese-dev/CoreCE/releases)
page keeps the last five releases. If the latest build has a problem, get
an older build from that page.

### Arch-based Linux Distributions

The above Linux binary release is available on the [AUR](https://aur.archlinux.org/packages/CoreCE-bin).

### Steam Deck

On a Steam Deck, do the setup in Desktop Mode. Unzip the release, then in
Steam open the Games menu, choose Add a Game, and add `halo` as a non-Steam
game. It then starts from Game Mode as well. If the Deck sleeps while it is
hosting, the other players lose the host after 15 seconds. Refer to "Steam
Deck" in [port/linux/README.md](port/linux/README.md#steam-deck).

## Game data

The port does not include the game data. Download an Xbox disc image
(`.xiso` or `.iso`) of Halo: Combat Evolved. All versions of the game
operate. The maps of the European (PAL) version were made for a slower
console. The port changes them to play as the North American (NTSC) maps do,
so players of the two versions can play together.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it in the
   file picker. On Linux and Windows, a disc image (`.iso` or `.xiso`) next
   to the executable is used without the file picker. Where no file picker
   can be shown (on Linux, without the desktop portal or `zenity`), the game
   says to put the disc image next to the executable, and stops.
3. The game extracts the `maps/` folder. Then the game starts.

The `maps/` folder holds the Xbox maps only. If it holds Halo PC's maps
instead (a Halo PC or Custom Edition install's `maps` folder), the game says
so and stops: Custom Edition maps go in `custom_maps/`, beside `maps/`.

On Linux and Windows, the game puts `maps/` next to the executable; on
macOS, in `~/Library/Application Support/CoreCE`. On
Android, copy the disc image to the phone first. The app puts `maps/` in its
data folder. Refer to [port/android/README.md](port/android/README.md). On
the web, the page copies `maps/` out of the disc image into the browser's
storage for the site; nothing is uploaded. Refer to
[port/web/README.md](port/web/README.md).

## Mods

Mods are folders of Halo PC tag files, as the Halo Editing Kit and Invader
write them, in `mods/` next to `maps/`: texture, sound, text and model packs
that play over the maps' on your machine only, in any game. Each has a
`mod.toml`. Every mod in the folder is on; the main menu's Mods turns them
off and on and sets the order they load in.
Refer to [docs/mods.md](docs/mods.md).

## Platforms

Each platform has its own instructions:

| Platform | Instructions |
| --- | --- |
| Linux (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/linux/README.md](port/linux/README.md) |
| Windows (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/windows/README.md](port/windows/README.md) |
| macOS (Apple silicon app, Metal, SDL3) | [port/macos/README.md](port/macos/README.md) |
| Android (arm64 app, OpenGL ES 3, SDL3) | [port/android/README.md](port/android/README.md) |
| Web (WebAssembly, WebGL 2, SDL3; desktop Chrome, Edge and Firefox) | [port/web/README.md](port/web/README.md) |
| iPhone (arm64 developer build, OpenGL ES 3 through ANGLE on Metal, SDL3) | [port/ios/README.md](port/ios/README.md) |

The Linux README also gives the controls, the settings and the multiplayer
functions. These are almost the same on all platforms.

## Multiplayer

The game can play system link games on a local network and on the internet:

- A system link game can have up to 128 players on up to 128 machines.
- Linux, Windows, macOS and Android machines and browsers can play in the same
  game. On a local network, the native builds play system link; browsers
  play each other in rooms, over WebRTC
  ([port/web/README.md](port/web/README.md)).
- An invite link, or the server browser, lets a machine join a game on the
  internet, browsers too: a native build takes a browser's WebRTC
  ([port/linux/README.md](port/linux/README.md), "Browsers"). No server of
  this project is necessary.
- The netcode is new. Each machine moves its own player at once,
  and the host makes the decisions for the game. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).
- Up to 4 players on one machine in split screen.
- A host lists a game as public, or keeps it private for the players with
  the invite. With Discord, the host can send the invite through Discord.
- Online co-op campaign, with friendly fire, player collisions, and more
  enemies as the player count grows.
- Voice chat: push to talk (V) or an open microphone, changed in the audio
  settings.
- Map sharing: a player missing the game's Custom Edition map downloads it
  from the other players.
- Moderation: the host can kick and ban from the scoreboard, or with the
  `kick` and `ban` console commands. Players can vote to kick.
- The scoreboard scrolls (the mouse wheel, or Page Up and Page Down) through
  a large lobby.

## Custom Edition maps

Put Halo Custom Edition maps in `custom_maps`, beside `maps`. That folder
also needs `bitmaps.map`, `sounds.map` and `loc.map` from your installed copy
of Halo Custom Edition.

The Gearbox maps that shipped with Halo PC (Ice Fields, Death Island, Danger
Canyon, Infinity, Timberland and Gephyrophobia) are a separate set. Copy them
from a Halo Custom Edition install's `maps` folder into `custom_maps`. They
then appear under Custom Singleplayer and Custom Multiplayer. A custom
singleplayer map plays as a campaign level, alone or as online co-op. Maps
that require OpenSauce do not load.

The server browser and the LAN browser can download a missing custom map from
the other players, including `bitmaps.map`, `sounds.map` and `loc.map`. This
works on a LAN, and on the internet when the host has map sharing on (the
default). Refer to [docs/custom_edition_caches.md](docs/custom_edition_caches.md).

## Community maps and competitive options

Community maps built for the Xbox's v5 NTSC data are installed by hand:
refer to [the community-map setup](docs/playtesting.md#community-maps).

**PB Options** offers spawn markers, timer announcements, and optional silent
movement and weapon-ready sounds. Those modifications default off. The host
chooses them. Use **Stock** for original-rule play. The match clock is each
machine's Graphics setting: Off, On, or On with Voice. On with Voice plays
the countdown shipped with the game. Refer to
[PB Options](docs/performance-options.md).

**Match Options**, on the game type, covers movement sounds, training,
camouflage, team scores, weapon spread and field of view. The host's choices
apply to the game. Default field of view leaves each player's Graphics
setting.

## Graphics

The game keeps the Xbox look until a setting says otherwise. Every setting
is in "Settings" in [port/linux/README.md](port/linux/README.md#settings).

**Display**

| Option | What it does | Default |
| --- | --- | --- |
| DISPLAY MODE, RESOLUTION, WINDOW SIZE | Fullscreen, borderless, or windowed. F11 switches mode. | Borderless, at the display's resolution |
| RESOLUTION SCALING | Draw at the window's resolution, or at the Xbox's 640x480 and scale it up | Native |
| V-SYNC, FRAME RATE LIMIT | Wait for the display, or run free up to a frame cap | V-sync on |
| SMOOTH MOTION | A frame for every refresh, blended between the game's 30 ticks a second | On |
| INSTANT AIM | In first person, the view turns the frame the mouse moves | On |
| FOV | On-foot field of view, from 80 to 150 degrees | Stock |

**Image quality**

| Option | What it does | Default |
| --- | --- | --- |
| ANTI-ALIASING | FXAA, SMAA, MSAA, or SSAA | Off, as on the Xbox |
| SHADOW RESOLUTION | Object shadows at 128 (the Xbox's), 256, 512, or 1024 pixels | 128 |
| PER-PIXEL LIGHTING | Lights models for each pixel | Off |
| MODEL DETAIL | Stock uses the Xbox's 480 lines. Native uses this display. Highest keeps the most detail. | Stock |
| HIGH-RES HUD, BUTTON STYLE, HIGH-RES TEXT | Redrawn HUD art, button art, and text that stay sharp when scaled | On for the high-res HUD and text |
| CAMPAIGN HUD | On a custom map, draw the campaign HUD. The reticle stays the map's. | Off |
| MATCH TIMER | A clock at the bottom right in multiplayer. On with Voice also plays the countdown | Off |

**Extras**

| Option | What it does | Default |
| --- | --- | --- |
| SHIELD GLOW, GLOW INTENSITY | Energy shields light what is around them as they flare | Off |
| SHIELD COLOR | Your own shield in a multiplayer armor color, as your machine draws it | Default |

## Build the game

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](port/include/xdk/README.md).

To build the game:

1. Install Python and [ninja](https://ninja-build.org/).
2. Install the tools for your platform. Refer to the README for the
   platform.
3. In the root folder of the repository, enter `python configure.py`.
4. Enter `ninja` with the target for the platform:

| Target | Result |
| --- | --- |
| `ninja linux` | `build/linux/halo` |
| `ninja windows` (on Windows, or on Linux after `configure.py --windows-cross`) | `build/windows/halo.exe` and `SDL3.dll` |
| `ninja macos` (on a Mac) | `build/macos/CoreCE.app` |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |
| `ninja web` | `build/web/site/`, the web site (`python tools/web_serve.py` serves it) |
| `python tools/ios_build.py` (on an Apple silicon Mac, not with ninja) | The iPhone app: refer to [port/ios/README.md](port/ios/README.md) |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use.

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py linux release`.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Linux and Windows builds operate on all x86-64 processors. The Linux build also operates on older distributions and on SteamOS: refer to "Portable build" in [port/linux/README.md](port/linux/README.md#portable-build). Use this option for builds that you give to other persons. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile. Refer to "Optimization profiles". |
| `--profile` | A profiling build. It records CPU times. Refer to "Profiling builds". |
| `--web-emcc PATH` | The Emscripten `emcc` for `ninja web` (else the one on the PATH, or `~/emsdk`'s). |

Without `--portable`, the Linux and Windows builds use all the instructions
of the processor that builds them (`-march=native`). Such a build does not
always start on a different computer.

### Optimization profiles

The builds use profiles of the game to optimize the code:

- `pgo/halo_linux.profdata` for Linux and Android.
- `pgo/halo_windows.profdata` for Windows.

The profiles need clang 22 or later. With an older clang, the builds do not
use the profiles.

To record a new profile:

1. Delete the profile.
2. Enter `python configure.py --pgo=train`.
3. Enter `ninja linux` or `ninja windows`.

The build then plays the main menu and the first minute of each campaign
level. This procedure continues for approximately 15 minutes. The game
data must be in `assets/`.

### Profiling builds

A profiling build records where the game spends its time.

1. Enter `python configure.py --profile`, then build as usual.
2. Start a recording. Enter `profile_record` in the console (or the telnet
   console), or set `debug.profile_record = true` in `config.toml`. To
   record each game of a session, also set
   `debug.profile_record_when = "game"`.
3. Stop the recording. Enter `profile_stop`, load another map, or quit.

The game writes numbered `.part<n>.json` files under `profiles/` in its data root
and logs their paths. Parts are retained after the recording stops. Logs go to
standard error; a Windows release build without standard error writes them to
`halo.log` next to `halo.exe`.

`tools/net_report.py` is a helper for developers. Enter
`python tools/net_report.py <recording.part1.json>` to read a recording
as tables. The command also writes `<recording>.summary.txt`, a short text
file.

| Command | Result |
| --- | --- |
| `profile_record [seconds]` | Starts a recording. With a number of seconds, the recording stops after that time. |
| `profile_stop` | Stops the recording. |

`debug.profile_memory` sets the memory of a recording in MB (default 256,
from 4 to 1024). A recording has no length limit:
the game writes a part each time the memory is full. Refer to "Settings" in
[port/linux/README.md](port/linux/README.md).

On Android, the files are in `/sdcard/Android/data/com.halo.decomp/files/profiles/`.
Enter `adb pull` to copy the recording parts to a computer.

A profiling build plays with normal builds. It does not use
`--pgo=train`.

## Testing

The tests are listed in [AGENTS.md](AGENTS.md) ("Testing"). Most run
without the game data, in GitHub Actions too.

One check needs the retail maps, so it runs on your own computer (or on a
self-hosted runner), not in GitHub Actions: `tools/net_compare.py` plays
two builds together in a network game on one computer and checks that they
stay in step. Run it when a change can alter how a build calculates the
game, as a change to the simulation, the compiler or the maths does,
before it is merged. For example, to check a build with such a change
against a build without it:

```sh
python tools/net_compare.py --a build/linux/halo --b <other checkout>/build/linux/halo \
    --data <folder with maps/> --maps bloodgulch,beavercreek,hangemhigh --swap
```

Each game prints PASS or FAIL. A FAIL names the first player, tick and
value that differ. Refer to "Comparing two builds" in
[port/linux/NETCODE.md](port/linux/NETCODE.md).

## Licence

CoreCE is free software: you can give it to others and change it under the
terms of the GNU Affero General Public License as published by the Free
Software Foundation, version 3 of the License or (at your option) any later
version (`AGPL-3.0-or-later`). It comes with no warranty. The full text is in
[LICENSE](LICENSE).

- **From the change.** The AGPL applies from the commit that made the change,
  and to each release built from that commit or a later one. Earlier
  releases, and the source at every earlier commit, were released under
  [CC0 1.0](LICENSES/CC0-1.0.txt) and stay that way.
- **What CoreCE builds on.** OpenCE, under the same licence; the decompilation of
  [punpckhdq/halo](https://github.com/punpckhdq/halo) is under the AGPL 3.0
  from 17 October 2026, and under CC0 1.0 before that date.
  [bnunu/halo-1](https://github.com/bnunu/halo-1) is under CC0 1.0.
- **Contributions** are under `AGPL-3.0-or-later`. A change taken from a
  project under GPL 3.0 keeps that licence and its notices; the GPL 3.0 and
  the AGPL 3.0 let the two be combined (section 13 of each).
- **Hosts and servers.** If you change the game and let other persons play
  with your version over a network (for example, as the host of an internet
  game, or on a web site), the AGPL asks you to offer them the source of
  your version (section 13). The game as released has its source here, and
  each release's source is its tag (`build-<n>`).
- **Third-party code, fonts and art** keep their own licences, which are
  next to them (`port/third_party`, `port/assets/fonts`,
  `port/assets/icons`, `port/assets/text_filter`). The release downloads and
  the web site's `licenses.txt` carry their notices.
- **Halo is not covered.** The licence covers the work of CoreCE's and OpenCE's authors.
  It gives no rights in Halo, its maps or anything else that Microsoft owns.

## Disclaimer

This is an unofficial fan project. It is not endorsed by or affiliated with
Microsoft, Xbox Game Studios, Halo Studios, Activision, or Bungie. Halo and
Halo: Combat Evolved are trademarks of Microsoft Corporation. Halo is
copyright Microsoft Corporation.

Bring your own copy of the game. The download has no game data.
