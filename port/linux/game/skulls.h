/*
SKULLS.H

The skulls (skulls.c): twelve of Halo: Combat Evolved Anniversary's
changes to how the game plays (not Grunt Birthday Party), chosen before a
game starts and kept for it. The calls Halo's own code (source/) makes into
skulls.c.
*/

#ifndef __SKULLS_H
#define __SKULLS_H

/* ---------- constants */

/* the skulls, a bit each of a game's skull flags (game_skulls_get), in the
order the SKULLS screen lists them */
enum
{
	_skull_iron = 0,
	_skull_mythic,
	_skull_foreign,
	_skull_famine,
	_skull_bandanna,
	_skull_fog,
	_skull_malfunction,
	_skull_recession,
	_skull_black_eye,
	_skull_eye_patch,
	_skull_pinata,
	_skull_boom,
	NUMBER_OF_SKULLS
};

#define SKULL_FLAGS_MASK ((1UL << NUMBER_OF_SKULLS) - 1)

/* the parts of the HUD that Malfunction hides one of each life, and Fog
the motion tracker of */
enum
{
	_skull_hud_crosshair = 0,
	_skull_hud_shield,
	_skull_hud_health,
	_skull_hud_motion_sensor,
	_skull_hud_ammunition,
	NUMBER_OF_SKULL_HUD_PARTS
};

/* ---------- prototypes/SKULLS.C */

/* the skulls of the game being played (its options', game.c), and whether
one of them is on */
unsigned long skulls_active(void);
boolean skull_active(short skull);

/* a skull's name (SKULLS screen's, upper case), its config.toml key, what
it does, and whether it is a silver skull (else a gold one) */
char const *skull_name(short skull);
char const *skull_key(short skull);
char const *skull_description(short skull);
boolean skull_silver(short skull);
/* config.toml's list of skulls (their keys, separated by spaces) as skull
flags, and back */
unsigned long skulls_parse(char const *text);
void skulls_print(unsigned long skulls, char *text, unsigned long size);

/* Iron. unit_died: a player died; main.c: whether a death (or a mission
lost) plays the level again from its start (alone), and whether it reverts
every player to the last checkpoint (co-op); players.c: whether a dead
player waits for the revert a death began rather than coming back beside a
teammate */
void skulls_player_died(void);
boolean skulls_iron_restarts_level(void);
boolean skulls_iron_reverts(void);
boolean skulls_iron_holds_respawn(void);

/* Mythic (damage.c): what an object's most vitality and shields are
multiplied by, by its team */
real skulls_enemy_vitality_scale(short team_index);

/* Foreign (units.c's unit_can_use_weapon): whether a unit can pick up and
use a weapon */
boolean skulls_weapon_usable(long unit_index, long weapon_index);

/* Famine (units.c's unit_died): a unit's weapons, about to be dropped */
void skulls_unit_dropping_weapons(long unit_index);

/* Bandanna (weapons.c, units.c): whether players' reserve ammunition,
batteries and grenades are never spent */
boolean skulls_infinite_ammunition(void);

/* Recession (weapons.c): the rounds and the battery a shot spends, of a
player's weapon or another's */
short skulls_rounds_per_shot(boolean players, short rounds);
real skulls_age_per_shot(real age);

/* Black Eye (damage.c): whether an object's shield recharges by itself */
boolean skulls_shield_recharges(long object_index);
/* Black Eye and Piñata (damage.c's object_damage_aftermath): a melee blow
struck on an object, by damage's owner; killed, whether it was the
killing blow */
void skulls_melee_struck(long object_index, long owner_object_index, short owner_team_index, boolean killed);

/* Boom (damage.c, network_damage.c, breakable_surfaces.c): what an
explosion's radius and the push it gives are multiplied by */
real skulls_explosion_scale(void);

/* Eye Patch (aim_assist.c): whether players' weapons aim for them
(autoaim, the view's friction and the bullets' magnetism) */
boolean skulls_aim_assist(void);

/* Fog and Malfunction (hud.c, hud_unit.c, hud_weapon.c): each frame, the
unit a local player's HUD is of; whether a part of it is drawn */
void skulls_hud_update(short local_player_index, long unit_index);
boolean skulls_hud_shown(short local_player_index, short part);

/* game_state.c: a saved game or checkpoint was loaded */
void skulls_game_state_loaded(void);

#endif // __SKULLS_H
