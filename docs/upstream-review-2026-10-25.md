# CoreCE merged into the fork

Merged October 25, 2026: CoreCE's `main` (`mastercheese-dev/CoreCE`, through
`17883811`, "Play the rest of the loading film while it fades off the match")
into this fork's `main` (`8bf56065`). Unlike the earlier reviews, this is a
merge commit of the whole history, not a selection. The two lines split at
OpenCE's `d03f6e27` (pull request #248): 447 commits only here (OpenCE's
pull requests up to #417 and this fork's own work), 248 only in CoreCE, 64 of
them the same change under other hashes. 157 files conflicted.

The rule of the merge: this fork's work stays (the Metal Mac build,
spectators, first-person spectating, skulls, the SKULLS button, Black Eye's
full shield, photo mode, `BUILD_PLATFORMS`, updates from the repository that
built the release), OpenCE's later work stays, and CoreCE's work comes in
where it does not undo either.

## The Apple ports

CoreCE had its own Apple port: an Android-style guest (`HALO_ANDROID`,
`HALO_GLES`) drawn through ANGLE, shared by a macOS app and an iPhone app,
both under `HALO_MACOS`, and an experimental native-Metal renderer
(`HALO_MACOS_NATIVE_METAL`, `port/linux/src/d3d8_metal.c`, `metal_*.c`).

| Part | Decision |
| --- | --- |
| This fork's Metal Mac build (`port/macos`, `tools/macos_build.py`) | **Kept unchanged**, as `HALO_MACOS`. It now also carries the opening film (`opening_frames.bin` in the app's resources, named to the game by the host). |
| CoreCE's macOS app, its DMG, Sparkle updates, Mac menus and installer, `tools/macos_*` and `tools/test_macos_*`, `docs/macos-*.md`, `docs/apple-multiplayer.md`, `docs/apple-regression-checks.md`, the `macos-dmg` and testing-prerelease workflows | **Excluded.** |
| CoreCE's native-Metal renderer and its geometry cache (`HALO_MACOS_GEOMETRY_CACHE`) | **Excluded**: the Mac draws with this fork's Metal device. |
| CoreCE's iPhone app (`port/ios`) | **Adopted**, under `HALO_IOS` only. The host pieces it kept in `port/macos` moved to `port/apple` (`host/`, `compiler/`, `native/HaloMapDownloads.*`, `licenses/`, `tests/guest_memory.c`); `tools/macos_setup.py` is `tools/ios_setup.py`, building into `build/apple`; the iOS guest no longer defines `HALO_MACOS`. CoreCE's `HALO_MACOS` code in shared files is `HALO_IOS` where the iPhone used it, and gone where only their Mac app did. |
| `port/apple/xiso.c` | **Adopted, rewritten in part**: its XDVDFS reading followed extract-xiso, whose 4-clause BSD licence cannot be combined with the AGPL. The directory walk is now written from the format's description, as `port/linux/src/xiso.c` is; CoreCE's map checks and copy are kept. |
| `port/apple/host/posix_net.c` | Reduced to including the shared `posix_net.c`, whose `__APPLE__` paths already translate Darwin's addresses (CoreCE's wrapper translated them a second time). CoreCE's `EISCONN` fix for connected UDP sockets is in the shared file. |

The iPhone app was not built for this merge (no iOS SDK, and its compiler
pass needs LLVM 22): its guest code was compiled with `-DHALO_IOS` and its
host's C files on the Mac.

## Kept from this fork where CoreCE differs

- Mods (CoreCE's `b1176ee0` turned them into stubs) and the Mods screen.
- The FOV AND VIEWMODELS screen and `display.viewmodel_*`, `display.first_person_legs`
  (CoreCE's `bf666dc2` fixed them at the stock view).
- `display.shield_color` (CoreCE's `8f419525` removed the armor color).
- Player names above heads (`display.player_names`).
- The host's voice-chat modes (`network.voice_mode`, `voice_lobby`), text chat's
  hide setting and its "links" option, streamer mode's three word lists.
- The loading screen without `loading.tga` stays black (no painted ring).
- The updater follows the repository whose workflow built the release
  (CoreCE's CI turned the updater off in every fork).
- The menus' layouts this fork has (VOICE CHAT on Teamplay Options, Network
  Setup's pages, Server Setup with SKULLS); CoreCE's Hardcore screens are added.
- The `controls.*` bindings and Controls Setup. CoreCE's `[bindings]` keeps only
  the keys outside them: `console` and `release_mouse`.
- The internet-play brokers: CoreCE's `net.corece.org` first, then this fork's
  four WebSocket-capable brokers, which the web build needs.

## Adopted from CoreCE

Hardcore multiplayer (rules, timer, teammate views, maps), the opening film,
PB Options, the match clock on every split pane (it replaces OpenCE's white
`display.match_timer` clock: the setting is now off / on / voice, and an old
`true` reads as on), the campaign HUD and stock HUD changes, high-res meter
and Custom Edition HUD fixes (`ce_meter_channels`, also set by the Metal
device), GLES texture borders, co-op pickups and vehicles, native community
maps and map downloads (iOS), the device and audio settings, the save root
moving to `halo-og` (Linux) and `Halo OG` (Windows) with its migration, the
CoreCE font and icons, and the other fixes the merge brought without conflict.

## Network version

Both lines had raised `HALO_PORT_NETWORK_VERSION` after OpenCE's 32: this
fork's 33 (spectators, skulls), CoreCE's 33 and 34 (match items at time 0,
the opening film). The merge is 35; no 33 or 34 machine joins it. The
recorded wire format (`port/linux/network_format.txt`) is otherwise unchanged.

## Checks

`ninja macos` and `ninja android` build. The Mac app ran hidden: the main
menu, b30 from its opening cutscene into play, and a hosted Blood Gulch game
(`debug.network_test`), each exiting cleanly. The asset-free tests that run
on this Mac pass as they did before the merge (`test_linux_port.py`,
`test_web_build.py`, `test_network_messages.py`, `test_cache_file_formats.py`,
`test_macos_asm_rebase.py`, the web page's Node tests); the ones that failed
before still fail, for the same reasons (no 32-bit Linux sysroot, the web
build's socket test). Eleven of CoreCE's own unit tests fail on this Mac on
CoreCE's `main` as well, and fail the same way after the merge. Not run: the
Linux, Windows and web builds, the harness (32-bit x86), the iPhone app, and
a game between two machines.
