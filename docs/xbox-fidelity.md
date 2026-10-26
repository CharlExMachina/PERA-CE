# Original Xbox fidelity

This fork targets original Xbox Halo: Combat Evolved NTSC gameplay on native
macOS, with controller support and separately imported community maps. Matching
decompilation, core performance, stability and platform improvements should stay
current through selective upstream review. Original game presentation and rules
remain the baseline; an upstream change is not accepted merely because it is new.

## Pinned baseline and upstream review

The last full upstream integration is `c55e4e2b` (build 64), merged into the
Apple fork as `aabe44417431c36e46aaa385a69a89a3c9d685c0`. On October 2, the network
protocol portion of `d1c7243cb20eab4488efa1266e259b1f4d5240f6` was selectively
adopted for version 10 compatibility, retaining the original scoreboard and
presentation. On October 3, the version-11 wire record and required received-input
handling from `c9ee319ab5f2964a32372fffdc7756111e39727d` were selectively ported,
preserving the fork's native menus, PB Options and existing gameplay rules.
Unsupported upstream PC gametype settings are refused before precache/play.
See the [v10 review](upstream-review-2026-10-02.md),
[the October 3 review](upstream-review-2026-10-03.md), and
[the October 18 review](upstream-review-2026-10-18.md).
This does not integrate intervening upstream changes. During upstream-update work, review all
changes since the last recorded review so useful core improvements are not missed.
Fetching upstream does not authorize merging it. Do not merge upstream `main`
wholesale; select focused changes and split mixed commits when necessary.

## Match opening, October 24, 2026

Network version 34. A multiplayer match places the map's weapons and
powerups once at game time 0, including while the opening holds the
clock there. Later ticks at 0 do not place them again. The retail disc does not include loading.tga, so the opening
plays maps/opening_frames.bin, a film of that loading screen. The builds
ship that film. A copy already in the maps folder is used when it is there. It runs for
its own length, about 2.7 seconds, and the last frame stays if a player is
still being placed or is still drawing a weapon. The screen stays at least
3 seconds even when the map is already loaded. Those happen while it is up.
When the screen ends, the film fades off the match over a short moment.
The match clock stays at 0 until that fade is finished. On a private LAN
Invasion on Sidewinder the ring thinned over the world, the shotgun was
already out, and the clock read 00:00.
The lobby does not stay up over the start. A campaign load, and a co-op
load, play that same film instead of the stand-in ring and fade it off the
level. They do not hold the mission clock or wait for a weapon.
A version 33 game cannot join. Overshield drain is the retail fade in every
match. These are explicit player directions, not an upstream import.

Match settings on this machine, the same day. Graphics' Match Timer is
Off, On, or On with Voice. On draws the blue bottom-right clock in every
multiplayer game and does not draw the old white HUD clock. A private
LAN Team Race showed 00:20 there. On with Voice also forces a map's
talking timer when that global exists. Team panes are the hardcore split
in every team game when this machine has one local player. Free-for-all
does not split. That pane path was not played with a second player.
Streamer mode is one switch. A new server starts at 2 players. Text chat
is a host switch in every lobby. A client sees that switch locked and
follows the host's chat message. Match Options that leave retail
(movement sounds, training, camo, team scores, pistol spread, shotgun
spread) ride the host's rules message. A joined client applies that
message and does not substitute its own sheet. Leaving that game
restores the sheet saved with its game type, which is what it hosts
next. The removed rows stay retail, including flag and ball pickup
and the next hill, even when another match option turns the sheet on. Map download defaults off for a new
config, and the lobby has a download button. The host row reads SHARING
MAPS while map sharing stays on. These are explicit player directions.

Chat lines use the small UI font. On October 24 a typed line in a private
Team Slayer was smaller than the ammo digits. The magnum's low-clip Reload
word is drawn centered under the reticle. The plasma pistol has no such
word. These are explicit player directions.

The last review compared this fork with upstream `main` at
`c20866353` on October 18, 2026. The WebRTC gap recovery was adopted.
The halo-1 reconcile's restored original bugs, the PC pause pitch, and the
HUD button styles were not. The web build stays deferred. The October 3
record still covers the seven commits through
`23b542601f2ca505c7a0143703e92fbda6075e18`. The following
earlier exclusions also remain in force; neither is integrated:

| Upstream change | Category and effect | Decision and reason | Validation / integration status |
| --- | --- | --- | --- |
| [`0182da817285b67eee8264663ca79f7c32d66b5e`](https://github.com/cybersecurity/halo-ce-universal/commit/0182da817285b67eee8264663ca79f7c32d66b5e) | Presentation / branding: high-resolution text and CoreCE titles. | Exclude: keep the game data's bitmap fonts and original menu titles. | Diff reviewed; not integrated; no adoption tests required. |
| [`f2ba71d9af4c6fc65d7419cc22e8f4899b16da88`](https://github.com/cybersecurity/halo-ce-universal/commit/f2ba71d9af4c6fc65d7419cc22e8f4899b16da88) | Presentation / player information: names above players' heads. | Exclude: preserve original target-name/teammate-indicator behavior. | Diff reviewed; not integrated; no adoption tests required. |

Every upstream commit requires inspection of its actual diff and behavioral
effects, including defaults, assets, dependencies and bundled changes. Use the
following policy when deciding what to integrate:

| Change category | Policy |
| --- | --- |
| Matching decompilation / byte accuracy | Prioritize verified corrections toward the stated Xbox reconstruction target; validate their effect on the native port separately. |
| Core performance, stability, platform compatibility and renderer correctness | Prioritize improvements that preserve original gameplay and presentation. Verify that an optimization does not silently change simulation timing or player behavior. |
| Netcode and potential matchmaking | Consider individually for more reliable, fair online play. Review compatibility, latency, authority, timing and fairness effects; eligibility is not automatic acceptance. |
| Renaming or rebranding, including CoreCE names/titles | Exclude from this fork unless the user explicitly requests a separate change. |
| Replacement fonts, artwork or HUD; new overhead player labels | Exclude by default. Preserve original presentation and original target-name/teammate-indicator behavior. |
| Timers, silent weapon switches, balance changes or other rule modifications | Exclude from the baseline unless the user explicitly requests and separately reviews them. |

Renderer correctness fixes can be appropriate: the existing `bfbac357`
glyph-padding fix preserves the original font pixels and prevents adjacent atlas
cells bleeding into enlarged text. A cosmetic replacement bundled with a useful
fix is not a reason to accept the replacement; isolate the fix or defer the commit.
Uncertain fidelity or fairness effects should be recorded and deferred pending
evidence, rather than silently changing the baseline.

Keep an upstream review ledger alongside the relevant update documentation. For
each reviewed commit, record its full hash, category, actual behavioral effect,
decision (adopt, partially adopt, exclude or defer), reason, relevant tests and
integration status (including the local commit when integrated). Include rejected,
deferred and already-equivalent/no-change decisions so later reviews do not mistake
them for omissions. Record the upstream revision reviewed through separately from
the last integrated revision. A matching-build result, native regression result
and untested claim must remain distinguishable.

Generic macOS/v5-map fixes can still be offered upstream as focused contributions,
independently of this fork's presentation choices. This policy does not schedule
automatic fetching, merging or monitoring.

## Explicit optional PB Options exception

On October 2, 2026, the user separately authorized an optional elapsed match
timer, separately toggled NHE timer announcements, and spawn markers under
**PB Options**, including markers derived from stock-map spawn data. All default to off and
are selected in a saved game type or applied by the host for the session. This
authorization was extended to host-controlled silent movement and weapon-ready
sounds, which default to Normal, plus local timer-audio and display preferences.
These are limited exceptions to the presentation and sound exclusions above;
they do not authorize other Performance Build mechanics, weapon behavior changes
or a different simulation rate. The original 30 Hz simulation remains in place.

See [PB Options](performance-options.md) for the native editor and pause-menu
flows, saved variant format, host authority and v11 compatibility, restrictions
on inert marker tags, stock-map marker rendering and the single-Mac
Prisoner/Downrush validation evidence.
The current marker audits cover 24 placements in converted NHE Prisoner and 19
in converted Downrush. Original PC v7 caches remain unsupported. Marker drawing
is included in the engine; optional user-local voice recordings are separate
from the maps. Retail map checksums are preserved.

## Defaults and comparison profile

The current implementation creates new `config.toml` files with
`display.high_res_hud = false`,
`display.interpolation = false` and `display.direct_camera = false`. The
simulation stays at 30 Hz. These current defaults are not a permanent requirement
to cap rendering at 30 FPS. High-refresh rendering, including 120 FPS through
interpolation, can be appropriate while preserving the 30 Hz simulation. Review
interpolation and camera behavior for their effect on fidelity and responsiveness.
Existing configurations are preserved; explicitly set these keys to false when
using an older save directory for reference comparisons.

[xbox-ntsc.toml](../port/macos/profiles/xbox-ntsc.toml) supplies an isolated
comparison profile. Launch it with the existing `HALO_SCREEN_WIDTH=640` override
for a 4:3 window. Fullscreen/widescreen and optional rendering settings remain
available, but the reference comparison uses original presentation settings.
Keep this 30 FPS reference preset separate from decisions about normal-play
rendering performance.

Modern native networking and community-map loading are deliberate extensions.
They must preserve the baseline's timing and weapon/player rules. Native map
files may use a bounded 128 MiB disk cache; the original 22 MiB tag arena and
non-native Xbox build limits are retained. Imported map geometry, spawn/item
placements and necessary custom content are reviewed separately from the engine.

## What “100% matching” means here

The decompilation target is Xbox build **2342**, `cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`.
The local retail data set is USA NTSC cache v5 build **2276**,
`01.10.12.2276`. These are distinct identifiers.

The [bnunu decompilation README](https://github.com/bnunu/halo-1#readme) reported
99.5% byte matching when inspected on October 1, 2026. This is a reported status,
not a matching build verified in this checkout. Track future matching progress
against its stated executable and build configuration. Do not call the ARM Mac
executable byte-identical to an Xbox executable: it uses a different compiler,
instruction set and platform layer, with replacement networking.

A matching Xbox reconstruction and a faithful native port require different
evidence. Native fidelity still needs physical-controller play, reference Xbox
comparisons, campaign/save coverage and multiplayer timing tests. A successful
Mac build, stock-map hash or community-map smoke test does not certify those.
