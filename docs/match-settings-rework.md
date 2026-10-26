# Match settings rework

Status: phase 5 is checked on the installed app. Phase 4's one-machine
checks still stand, including a new server at 2 players, the blue match
clock, and lobby Settings on the settings-page skeleton. In a later
private Team Slayer on Battle Creek, listing PRIVATE, weapon set PISTOLS
spawned the magnum. With a few rounds left in the clip, the word Reload
flashed on the center of the view, a short way under the reticle. The
plasma pistol has no reload word. The assault rifle was not opened; it
uses that same low-clip crosshair. A typed line, "mastrcheese: hi", was
smaller than the ammo digits. Teammate panes and a client locked to the
host chat switch were not exercised. The existing config still has
map_download and map_seed true, so those new defaults apply only when
the keys are absent. Phase 6 was measured and left unchanged.
A private LAN Team Slayer on Battle Creek, with the shader cache already
warm, had no shader compile misses. Entering the level paused frames for
about 620 ms. When the player appeared, the high-res HUD textures uploaded
and that frame's texture work was about 183 ms. Startup shader warmup was
31 ms and is not the join. `hardcore_assets_open` inflates a 7.6 MB pack
to 12 MB in about 24 ms, and a stock game does not call it. It runs when
spawn markers or training areas first draw. That is smaller than the HUD
upload, so the pack is not prepared ahead of the match.
Do one phase, check that phase, then stop. Do not start the next phase in the
same pass. Do not commit, push, or install unless the user asks.

Checked against the tree on October 24, 2026. The guiding choice is that
hardcore stops being its own front door. Options that stay become normal
game-type settings, lobby controls, or settings on this machine.

Retail Xbox rules stay the baseline. A stock game type with Match Options
left alone plays with retail sounds, retail weapon spread, retail overshield
drain, and no training percent. The changes below are the ones the user
explicitly asked for. Record the simulation changes in
[the fidelity policy](xbox-fidelity.md) when phase 3 lands.

## How a loop should use this

- Work the phases in order. Each phase lists the files to touch, the
  approach, and the checks that mean it is done.
- Menu screens come from `tools/port_settings.py`. Do not run
  `tools/ce_menus.py`. It deletes `port/assets/menus/ce` before writing.
  Regenerate only the XML files a phase names, by calling
  `settings_files()` and `multiplayer_files()` and writing those files
  in place. Match Options XML is hand-written and is not in that list.
  Server setup label indexes are listing 9, co-op 10, voice 11, password
  12, and map sharing 13.
- The extra rules record is 28 bytes inside a 35-byte message, version 3
  (`HARDCORE_RULES_VERSION`, `HARDCORE_RULES_MESSAGE_SIZE` in
  `port/linux/game/hardcore_rules.c`). Keep every byte in place. A field
  that leaves the menus is written as the retail value.
- `struct game_variant_options` is 0x1C and is the version 11 wire record
  (`source/game/game_variant_options.h`). Do not add fields to it.
- `HALO_PORT_NETWORK_VERSION` is 34 (`port/linux/include/halo_port_limits.h`).
  Machines of different versions cannot play. Phase 3 raised it to 33.
  The opening ring and the held match clock raised it to 34, because a
  version 33 machine starts that clock at the load and spawns at 3 seconds.
- PB Options stay a separate host-selected practice preset. See
  [PB Options](performance-options.md). This rework does not remove them.
- Campaign behavior stays as it is.

## Corrections found while checking

These are easy to get wrong from the menu labels alone.

**The first item pass stays.** `game_engine_update_item_spawn` places the
map's weapons and powerups on the first tick at game time 0. The opening
holds that clock for many ticks, and those later ticks do not place the
items again. It does not hold players. The player hold is described in
phase 3.

**There are three clocks.**

| Clock | Where it lives | Who turns it on |
| --- | --- | --- |
| Hardcore clock | `hardcore_timer.c`. Blue, bottom right, from `game_time_get`. Team scores draw beside it. A map's `talking_timer` script global is forced from `audio.hardcore_timer_voice`. | A hardcore match. The lobby Timer Voice row is this machine only. |
| Graphics match timer | `hud_draw_match_timer` in `source/interface/hud.c`, setting `display.match_timer`. White HUD clock. Skipped while hardcore rules are active. | This machine. Graphics already has an on/off row. Default off. |
| PB match timer | `performance_timer_render.c`. Position and size are `display.timer_position` and `display.timer_scale`. Spoken cues are the Timer Audio page. | The host, through PB Options. Local cue toggles never play audio when the host's Timer Sounds rule is off. |

Phase 4 replaces the Graphics clock with the hardcore clock and adds the
voice choice to that same row. PB Options, including its own clock and
Timer Sounds, stay host-owned. When the client clock is on, draw the
hardcore clock and skip the PB clock on that machine so two clocks do not
stack. PB Timer Sounds still follow the host.

**Lobby Settings is hardcore-only today.** `lobby_settings_apply` in
`port/linux/game/menu_functions.c` shows the button only when the variant's
anniversary bit is set. A classic lobby hides it. Phase 4 shows it in
every lobby, because text chat and Download Maps live there.

**Teammate panes require a hardcore match.** `hardcore_views_team_screen`
in `port/linux/game/hardcore_views.c` requires `hardcore_rules_active()`.
`display.teammate_views` says classic multiplayer is unchanged. Phase 4
lifts that requirement for every team game.

**Comp 2v2 is not a normal built-in yet.** `hardcore_comp_apply` loads a
saved playlist named "Comp 2v2", or builds a slayer (score 50, respawn
150 ticks, no death bonus, no kill penalty, generic starting equipment)
and then forces teams on. `hardcore_stamp` sets the anniversary bit and
also clears the motion sensor. Phase 2 makes it a real built-in and stops
using that stamp on ordinary game types. The built-in save-as-copy path
already exists: a read-only profile that was edited asks for a new name
and `playlist_profile_new` writes the copy (`player_ui_save_profile`).

**The random percent ignores training mode.** `hardcore_random_update`
draws when `random_pct` is set. `hardcore_training_pair` sets that byte
when training turns on and does not clear it when training turns off.
Comp 2v2 sets the byte while leaving training off. Phase 1 draws the
line only while training is on.

**Text chat is two settings.** `network.text_chat` is the host
(`on`, `links`, or `off`). A game listed in the server browser forces it
off, and that row already locks. `display.text_chat` is this machine's
show/hide on the Multiplayer HUD page. Phase 4 keeps one host on/off
control, on the lobby page.

**Map download and seeding default on.** `network.map_download` and
`network.map_seed` default to true in `port/linux/src/port_config.c`.
`map_share_download_begin` returns false while `network.map_download` is
false, so a lobby button needs an explicit allow for that one download.

**Maximum players is one C array.** `maximum_players[]` in
`menu_functions.c` is `{ 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 }`.
The spinner index is the array index. New multiplayer games start at the
last entry, 128. Co-op starts at 16 (`COOPERATIVE_DEFAULT_PLAYERS`).
The spinner strings are generated and must stay in the same order as the
array.

**Profile data and machine data are already different stores.**
`struct player_profile` holds the name, the color, campaign progress, and
`controller_settings`. The menu fields `profile.look_sensitivity` through
`profile.ingame_help` read that struct. Video, audio, network, mouse, and
gamepad bindings are `config.toml` through `port_config.c`. Game Settings
and Profile Settings are already separate choosers inside Settings
(`docs/native-settings.md`). Phase 4 finishes that split on the main menu:
Profiles edits the profile, Settings edits this machine, and Mods moves
under Settings.

**The reload word is the low-clip crosshair.** On the magnum
(`weapons\pistol\pistol`) and the assault rifle it is the `flash_ammo`
element of `ui\hud\master rounds`, the word Reload, not
`_crosshair_state_reload` and not a weapon overlay. The plasma pistol
parents `ui\hud\master plasma` and has no such word. It shows while the
clip is at or below the weapon HUD's loaded-ammo cutoff and reserve is
above zero, and it flashes. Phase 5 centers that element under the
reticle. Ammo, heat, grenades, Low Ammo, and No Ammo stay where the tag
puts them.

## Rules that stay in the message

Kept, host enforced, saved with the game type:

| Rule | Byte | Menu | Default |
| --- | --- | --- | --- |
| Movement sounds | `movement_sounds` | Yes / No | Yes (retail) |
| Training mode | `spawns_visible` and `random_areas`, kept as one switch | No / Yes | No |
| Camo | `camo` | Xbox / MCC / Custom | Xbox (0, the tag) |
| Team scores | `team_scores` | No / Yes | No |
| Pistol spread | `pistol_spread` | Default / Tight / Wide / Double | Default (0, the tag) |
| Shotgun spread | `shotgun_spread` | Default / Tight / Wide / Double | Default (0, the tag) |

Default spread uses the weapon. Tight, Wide, and Double are the host's
spread for everyone. A joined client applies the host's message and
does not substitute the sheet it has open locally. Leaving that game
restores the sheet it had open locally, which is what it hosts next.
The removed bytes below are stored as this retail sheet and are not
applied, so turning on one kept rule does not also change pickup,
hills, ammo, or delays.

Written as the retail value, with the row removed:

`flag_pickup`, `ball_pickup`, `hill_order`, `hill_move_s` (leave the
existing default of 60, which the comment calls the old period),
`fireteams`, `infinite_ammo`, `kill_restores`, `skip_draw`,
`item_spawn_s`, `outlines`, `os_visible`, `friction`, `adhesion`,
`aim_delay`, `trig_delay`. `os_type` stays fading (1). `random_pct` is
set only while training is on. `omit_first` is forced to the always-on
item skip in phase 3 and has no row.

Vanilla game-type pages that already edit pickup, hill, indicator, item,
and vehicle rules stay where they are. PB Options stays below Indicator
Options.

The anniversary bit means "this variant carries the rules message." Set
it when a saved game type differs from the defaults in the table. A stock
game type does not set it. Do not clear the motion sensor from that bit.
Comp 2v2's motion sensor off is part of its own variant preset.

## Phase 1. Small behavior

No screen moves. No version bump.

**Random percent.** In `hardcore_rules_render` / `hardcore_random_update`,
compute and draw `RANDOM n%` only when training mode is on
(`random_areas` and `spawns_visible`). Stop Comp 2v2 from setting
`random_pct` while training is off.

**Overshield.** `hardcore_rules_overshield_fades` always returns true.
That is the retail drain. Remove the constant path. Leave the message
byte on fading. The menu row goes away in phase 2. This phase can ignore
the byte even if an old in-memory value says constant.

**Voice.** Remove the mode choice from behavior, even if the rows are
still on screen until phase 4. The host still sends the existing voice
mode (`network_voice.c`, `voice_route`). Choose it in code:

- Lobby: everyone (`_voice_route_global`).
- Team game: the whole team, no proximity (`_voice_mode_team_global`).
- Free-for-all: anyone nearby (`_voice_mode_team_enemy_proximity`, which
  routes anyone in range).

Clients follow the host message. No new packet.

Done when a training-off hardcore session shows no percent, overshield
drain does not depend on `os_type`, and the three voice cases match the
list above. Stock weapon sounds and spread are unchanged.

## Phase 2. Game types, then remove the Hardcore front door

**Match Options page.** Add one page, generated from
`tools/port_settings.py`, linked from each normal game-type editor
(Slayer, CTF, Oddball, King, Race) the way Player Options is linked.
Rows are the six kept rules. Wire the spinners through the existing
`gametype_options` table in `menu_functions.c` (`_option_hardcore`).

**Persistence.** The 28 bytes do not fit in `game_variant_options`.
Write them beside the playlist profile (`z:\saved\playlists\...\blam.lst`
is the profile file). Read them when the editor opens and when a match
starts. Delete the sidecar in `playlist_profile_delete`. A missing
sidecar means the defaults in the table above and no anniversary bit.

**Comp 2v2.** Register it as a built-in in the normal game-type list,
read-only, with the slayer preset `hardcore_comp_apply` already builds:
teams, score 50, respawn 150, no death bonus, no kill penalty, generic
starting equipment, motion sensor off, team scores on, spreads at
Default, training off. Saving an edit asks for a new name and writes a
copy, including the sidecar. Remove it from the hardcore-only list.

**Stop `hardcore_stamp` on ordinary types.** That function sets the
anniversary bit and clears the motion sensor together. Ordinary Match
Options set the bit only for the rules message. Comp 2v2 clears the
motion sensor through its own variant flags.

**Remove the front door** after Match Options works: the Hardcore main
multiplayer item, the hardcore map list, server page, and rules pages
(`_hardcore_screen` in `port_settings.py` and the `port hardcore *`
handlers that only those screens call). Keep `hardcore_rules.c`,
`hardcore_views.c`, and `hardcore_timer.c`. They still implement the
kept rules, the panes, and the clock.

Done when a stock Slayer saves and reloads with retail defaults and no
anniversary bit, a Match Options change survives a restart, Comp 2v2
edits into a named copy, and the Hardcore menu item is gone.

## Phase 3. Opening of every multiplayer match

This is the version bump. The rules message layout does not change.
Item skip and the first opening spawn raised it to 33, so a version 32
build cannot join. The loading ring and the held match clock raised it
to 34, so a version 33 build cannot join.

**Initial items.** For every multiplayer match, place map weapons and
powerups once at game time 0. The opening holds the clock there, so
later ticks at 0 do not place them again. There is no row that skips
that first pass. Campaign is unchanged.

**Opening screen.** A multiplayer match does not keep the lobby up.
`pc_menus_screen` turns the pregame widget into the lobby, so the hold
loads the ring (`progress_bar_show_ring`, the black screen and the blue
halo) instead. The host places every participating player during that
screen. A player is ready when the weapon in their hand has finished
its draw. An empty server slot is not a player. A loadout that gives
no weapon is placed. The screen stays up for 3 seconds even when
the map is already loaded, and longer if a player is still being armed.
The match clock (`game_time_get`, the blue clock and the time limit)
does not advance until the picture has faded off the match. Co-op and campaign do not
take this hold. A game joined in progress keeps the host's clock.

Done when a private match shows the ring instead of the lobby, the
blue clock reads 00:00 as the world appears with the weapon in hand
and the map's weapons and powerups already placed, and a version 33
advertisement is refused.
Update `docs/xbox-fidelity.md` with what changed and what was checked.

## Phase 4. This machine, the lobby, and split screen

Edit `tools/port_settings.py` and regenerate. Then point the C at the
new spinner names.

**Split screen.** `hardcore_views_team_screen` becomes: a team game is
in progress, and this machine has one local player. Free-for-all does
not split. The panes stay the hardcore ones: up to four windows, first
person on a living teammate, the death camera on a dead one, roster
order locked. Update the `display.teammate_views` comment, which still
says classic multiplayer is unchanged. Move Split Screen and Your View
to Settings. Remove them from the lobby. Add Player stays a second
local player with their own controller.

**Match timer row.** On Graphics, change Match Timer from on/off to
Off, On, and On with Voice. On draws `hardcore_timer_render` in any
multiplayer game and does not draw `hud_draw_match_timer`. On with
Voice also forces a map's `talking_timer` global when that global
exists. Off leaves that global as the map authored it, and does not
force `power_call`. Team scores draw beside the clock only when the
game type turned `team_scores` on. Remove the lobby Timer Voice row.
Migrate a config that had `display.match_timer` true and
`audio.hardcore_timer_voice` true to On with Voice.

When the client clock is on, skip `performance_timer_render` on that
machine. PB Timer Sounds, spawn markers, and silent-sound rules are
unchanged.

**Streamer mode.** One switch on the Graphics page. On uses one fixed
filter: mild slurs, strong swearing, insults off. Off filters nothing.
Remove the severity page. `text_filter.c` reads the switch and ignores
the old severity keys while the switch is on.

**Extra Effects.** Remove the page and the Video row. Leave
`display.shield_glow` and `display.shield_glow_intensity` in
`port_config.c` so old files still load. The default remains glow off.

**Profiles, Settings, Mods.** Profiles gains the profile fields: name,
color, and the `profile.*` controller settings. Settings keeps mouse,
gamepads, audio, video, network, and About. Move the main-menu Mods
item into Settings and remove it from the ring.

**Lobby.** Show Settings in every lobby (`lobby_settings_apply` no
longer checks the anniversary bit). Rebuild that page on the settings
skeleton: header, option rows, help, button bar.

- Text chat: On or Off, writing `network.text_chat`. Drop `links` from
  the UI. A saved `links` value is treated as on. The host can change
  it. A client sees the row locked on the host's value. Extend the
  existing `text_chat_locked` path, which today locks only a listed
  public game. Remove the Multiplayer HUD page. `display.text_chat`
  stops being a separate switch. Player names, name scale, and the
  scoreboard options stay at the config values already on disk, with
  no page.
- Download Maps, at the bottom. Defaults: `network.map_download` false
  and `network.map_seed` false. Remove the Network Setup map-sharing
  page and its upload and seed help. The host may still send
  (`network.map_share` stays true). The button calls
  `map_share_download_begin` with an explicit allow for this lobby's
  map, because that function currently bails out when the config is
  false. If the host is not sharing, the button says so.

**Voice rows.** Remove the voice-mode spinners from Teamplay and from
Server Setup. Phase 1 already chooses the mode.

**Server size.** Change `maximum_players[]` and the generated spinner
strings together: every integer from 2 through 16, then 20, 24, 32, 40,
48, 64, 80, 96, 112, 128. New games, including co-op, start at 2.
`server_settings_maximum_players_index` and the hardcore spinner, if it
still exists at this point, use the same list.

Done when a stock lobby shows text chat and Download Maps, a client
cannot change the host's chat switch, teammate panes work in a normal
team game, Add Player still adds a local player, and a new server
defaults to 2 players.

## Phase 5. Presentation

**Chat font.** `network_chat_draw` uses `hud_get_font_index()`. Draw
those lines with the small UI font the menu buttons already use, and
size the rows to that font's height. Names, scoreboard, and other HUD
text stay on their current fonts.

**Reload prompt.** The reload word is a weapon HUD overlay placed by
the weapon tag, off to the side of the reticle. In the draw path
(`hud_weapon.c` / `hud_draw_static_element`), identify the element by
the reload crosshair state (`_crosshair_state_reload` and the reload
overlay bit) and place that element on the horizontal center of the
pane, a short distance under the reticle. Leave ammo, heat, and grenade
elements on the tag's position. Confirm the element on a pistol and a
rifle before moving anything else.

**Menu style.** Lobby settings from phase 4 uses the settings-screen
skeleton. Other new rows use the existing option-row art, header, help,
and button bar. Do not restyle the retail ring, campaign screens, or
stock game-type pages.

Done when chat is visibly smaller, the reload word sits under the
reticle on at least the pistol and the assault rifle, and lobby
settings matches the settings pages.

## Phase 6. Join hitch

Measure before changing code. Trace a hardcore join and a stock Slayer
join through match start. The hardcore-only candidate is
`hardcore_assets_open`, which decompresses its pack on the main thread
the first time it runs. A stall on both joins is the first shader use
as the game fades in. The phase 3 loading hold is not the fix. Change
only a stall the trace shows. If the asset pack is the stall, prepare
it before the match is shown.

## Leave alone

- The 30 Hz simulation, weapon damage, and hitscan registration.
- PB Options presets, Timer Sounds, spawn markers, and silent movement
  or weapon sounds, except skipping the PB clock graphic while the
  client hardcore clock is on.
- `struct hardcore_rules` field order and the 35-byte message.
- `struct game_variant_options`.
- Add Player and retail local splitscreen.
- Campaign, co-op item placement, and co-op spawn timing. Co-op's
  maximum-player default does change to 2 in phase 4.
- Guest imports, CRT shims, and the original engine functions that are
  unused only because nothing calls them yet.

## Checks for the whole rework

- Stock Slayer, Match Options untouched: retail sounds, retail spread,
  no random percent, no team-score counters, overshield drains.
- Comp 2v2 saved as a copy reloads with the same scores, spreads, and
  motion-sensor choice.
- Training off hides the percent. Training on shows it.
- Lobby voice is everyone. Team voice is the team. Free-for-all voice
  is proximity.
- Version 33 peers spawn together after the loading screen. Version 32
  is refused.
- Teammate panes work in a normal team game. Add Player still works.
- Lobby Settings in a stock game has text chat and Download Maps. A
  client sees chat locked to the host.
- Graphics Match Timer is Off, On, or On with Voice. The lobby has no
  timer row.
- New server setup starts at 2 players and can select 3, 15, 16, and 24.
