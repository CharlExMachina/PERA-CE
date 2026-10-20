/*
SKULLS.C

The skulls: twelve of Halo: Combat Evolved Anniversary's changes to how the
game plays, each on or off for a whole game.

- They are chosen before a game starts, and cannot be changed in it: a
  campaign level's on the SKULLS screen after its difficulty (main.c's
  main_set_skulls; config.toml's game.skulls keeps the screen's choice), a
  network game's by its host on Server Setup's SKULLS (network.skulls),
  which every machine takes from the host's game settings (network_game's
  skull_flags).
- A game's skulls are in its options' flags (game.c's game_skulls_get),
  which the Xbox's game left unused: they are in the game state, so a
  checkpoint and a saved game keep them. Continue and Load Game go on with
  the saved game's skulls; a New Game on a level with a saved game plays
  it with the skulls chosen.
- Each skull's change is made where the game does what it changes (the
  calls in skulls.h), on the machine that decides it: the host of a
  network game deals the damage, spawns the players and keeps the shields,
  every machine spends its own players' ammunition as the host does.

The skulls:

- Iron: alone, dying (or losing the mission) plays the level again from
  its start; in co-op, any player's death reverts every player to the last
  checkpoint.
- Mythic: the players' enemies have twice their health and shields (the
  campaign's: the difficulty's enemies). Sentinels, which have no shields
  for the game to give them, have twice their health.
- Foreign: players can neither pick up nor use the Covenant's weapons.
- Famine: the weapons a unit drops as it dies hold half of what it had
  (those placed on the map, its racks and crates, keep theirs).
- Bandanna: players' reserve ammunition, batteries and grenades are never
  spent, and they always have a grenade of each kind (as the infinite
  ammunition cheat gives them); magazines still empty and need reloading,
  and weapons still overheat.
- Fog: no motion tracker.
- Malfunction: each life (a spawn, a respawn or a checkpoint loaded), one
  part of the HUD is hidden: the crosshair, the shield meter, the health
  meter, the motion tracker or the ammunition.
- Recession: each shot of a player's weapon spends twice its rounds (or
  battery).
- Black Eye: players' shields never recharge by themselves; each melee blow
  struck on a living enemy recharges a third of the shield.
- Eye Patch: players' weapons do not aim for them: no autoaim, no friction
  on the view over a target and no magnetism of the bullets.
- Piñata: an enemy struck with a melee blow drops a live plasma grenade.
- Boom: explosions reach twice as far and push twice as hard.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/projectiles.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "main/main.h"
#include "math/real_math.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "tag_files/tag_files.h"
#include "units/units.h"

#include "network_coop.h"
#include "skulls.h"

#include <string.h>

/* damage.c's: a fraction of a full shield given back to an object */
void damage_port_shield_restore(long object_index, real fraction);

/* ---------- constants */

/* Black Eye: the part of a full shield each melee blow on an enemy gives
back */
#define BLACK_EYE_SHIELD_PER_BLOW (1.0f / 3.0f)
/* Piñata: how the grenade leaves the unit struck, in world units a tick:
up, and out to a side */
#define PINATA_GRENADE_UP_SPEED 0.08f
#define PINATA_GRENADE_OUT_SPEED 0.04f

/* ---------- structures */

struct skull_definition
{
	char const *name;
	char const *key;
	boolean silver;
	char const *description;
};

/* ---------- globals */

static struct skull_definition const skull_definitions[NUMBER_OF_SKULLS] =
{
	{ "IRON", "iron", FALSE,
		"Alone, dying starts the level over from its beginning. In co-op, any player's death sends every "
		"player back to the last checkpoint." },
	{ "MYTHIC", "mythic", FALSE,
		"Enemies have twice the health and twice the shields: every fight takes twice the ammunition." },
	{ "FOREIGN", "foreign", FALSE,
		"You can neither pick up nor fire the Covenant's weapons: the plasma pistol and rifle, the needler "
		"and the fuel rod gun." },
	{ "FAMINE", "famine", FALSE,
		"Weapons dropped by the fallen, enemies and marines alike, hold half their ammunition. Weapons "
		"found on racks and in crates hold all of theirs." },
	{ "BANDANNA", "bandanna", FALSE,
		"Your reserve ammunition, batteries and grenades never run out. Magazines still need reloading, "
		"and plasma weapons still overheat." },
	{ "FOG", "fog", FALSE,
		"No motion tracker: what is coming, you see coming." },
	{ "MALFUNCTION", "malfunction", FALSE,
		"Each time you spawn, or a checkpoint is loaded, one part of your HUD is gone: the crosshair, the "
		"shields, the health, the motion tracker or the ammunition." },
	{ "RECESSION", "recession", FALSE,
		"Every shot spends twice the ammunition, or twice the battery." },
	{ "BLACK EYE", "black_eye", FALSE,
		"Your shields never recharge by themselves. Each melee blow you land on an enemy gives back a third "
		"of them." },
	{ "EYE PATCH", "eye_patch", FALSE,
		"No aim assist: no autoaim, no slowing over a target and no bending of shots towards it." },
	{ "PINATA", "pinata", FALSE,
		"An enemy you strike with a melee blow drops a live plasma grenade at its feet. Stand clear." },
	{ "BOOM", "boom", TRUE,
		"Explosions reach twice as far and throw twice as hard: grenades, rockets, fuel rods, vehicles "
		"and plasma coils." },
};

/* Malfunction: the part of each local player's HUD hidden, the unit it was
chosen for (NONE: none yet, or the player is dead) and the saved game
loads seen since */
static struct
{
	long unit_indices[MAXIMUM_LOCAL_PLAYERS];
	long loads[MAXIMUM_LOCAL_PLAYERS];
	short hidden[MAXIMUM_LOCAL_PLAYERS];
	boolean initialized;
} skulls_hud;
static long skulls_game_state_loads;
/* Iron: a co-op player's death has begun a revert, which no game state has
loaded since */
static boolean skulls_iron_revert_pending;

/* ---------- private code */

static boolean skulls_cooperative(void)
{
	return game_is_cooperative() || network_coop_active();
}

/* whether a weapon is the Covenant's, by its tag's name (the stock maps'
and those of Custom Edition maps named as theirs are) */
static boolean skulls_weapon_covenant(long weapon_index)
{
	static char const *const covenant_names[] = { "plasma", "needler", "fuel rod", "fuel_rod", "energy sword",
		"energy_sword", "covenant" };
	char const *tag_name = tag_get_name(weapon_get(weapon_index)->definition_index);
	char name[256];
	short index;

	if (!tag_name)
		return FALSE;
	for (index = 0; tag_name[index] && index < (short)sizeof(name) - 1; index++)
		name[index] = tag_name[index] >= 'A' && tag_name[index] <= 'Z' ? (char)(tag_name[index] - 'A' + 'a') :
			tag_name[index];
	name[index] = 0;
	for (index = 0; index < (short)NUMBEROF(covenant_names); index++)
	{
		if (strstr(name, covenant_names[index]))
			return TRUE;
	}
	return FALSE;
}

/* Famine: a weapon's ammunition halved: its magazines' rounds, loaded and
in reserve, and a battery's charge left (its age, 1 spent: as
weapons.c's weapon_set_current_amount tells a battery) */
static void skulls_weapon_halve(long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct weapon_definition *definition = weapon_definition_get(weapon->definition_index);
	boolean battery = definition->weapon.magazines.count == 0;
	short index;

	for (index = 0; index < (short)MIN(definition->weapon.magazines.count, NUMBEROF(weapon->weapon.magazines));
		index++)
	{
		weapon->weapon.magazines[index].rounds_loaded /= 2;
		weapon->weapon.magazines[index].rounds_total /= 2;
	}
	for (index = 0; index < definition->weapon.triggers.count; index++)
	{
		struct weapon_trigger_definition *trigger = TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, index,
			struct weapon_trigger_definition);

		if (trigger->age_generated_per_round > 0.0f)
			battery = TRUE;
	}
	if (battery)
		weapon->weapon.age = 1.0f - (1.0f - PIN(weapon->weapon.age, 0.0f, 1.0f)) * 0.5f;
}

/* Piñata: a live plasma grenade (the globals' second grenade's
projectile), the unit's, falling from it */
static void skulls_pinata_drop(long unit_index)
{
	struct game_globals *globals = scenario_get_game_globals();
	struct game_globals_grenade *grenade;
	struct object_placement_data placement;
	real_vector3d velocity;
	real angle;
	long grenade_index;

	if (!globals || globals->grenades.count <= _unit_grenade_covenant_plasma)
		return;
	grenade = TAG_BLOCK_GET_ELEMENT(&globals->grenades, _unit_grenade_covenant_plasma, struct game_globals_grenade);
	if (grenade->projectile.index == NONE)
		return;
	object_placement_data_new(&placement, grenade->projectile.index, unit_index);
	SET_FLAG(placement.flags, _new_object_never_automatically_delete_bit, TRUE);
	placement.position = object_get(unit_index)->object.bounding_sphere_center;
	placement.forward.i = 0.0f;
	placement.forward.j = 0.0f;
	placement.forward.k = 1.0f;
	normalize3d(perpendicular3d(&placement.forward, &placement.up));
	grenade_index = object_new(&placement);
	if (grenade_index == NONE)
		return;
	/* (out to a side the machine picks: only what it shows) */
	angle = real_local_random_range(0.0f, 2.0f * _pi);
	velocity.i = cosine(angle) * PINATA_GRENADE_OUT_SPEED;
	velocity.j = sine(angle) * PINATA_GRENADE_OUT_SPEED;
	velocity.k = PINATA_GRENADE_UP_SPEED;
	projectile_accelerate(grenade_index, &velocity);
}

/* Malfunction: a part of the HUD hidden, of those Fog leaves */
static short skulls_hud_choose(void)
{
	boolean fog = skull_active(_skull_fog);
	short part = local_random_range(0, fog ? NUMBER_OF_SKULL_HUD_PARTS - 1 : NUMBER_OF_SKULL_HUD_PARTS);

	if (fog && part >= _skull_hud_motion_sensor)
		part++;
	return part;
}

/* ---------- public code */

unsigned long skulls_active(void)
{
	return game_skulls_get() & SKULL_FLAGS_MASK;
}

boolean skull_active(short skull)
{
	return VALID_INDEX(skull, NUMBER_OF_SKULLS) && TEST_FLAG(skulls_active(), skull);
}

char const *skull_name(short skull)
{
	return VALID_INDEX(skull, NUMBER_OF_SKULLS) ? skull_definitions[skull].name : "";
}

char const *skull_key(short skull)
{
	return VALID_INDEX(skull, NUMBER_OF_SKULLS) ? skull_definitions[skull].key : "";
}

char const *skull_description(short skull)
{
	return VALID_INDEX(skull, NUMBER_OF_SKULLS) ? skull_definitions[skull].description : "";
}

boolean skull_silver(short skull)
{
	return VALID_INDEX(skull, NUMBER_OF_SKULLS) && skull_definitions[skull].silver;
}

unsigned long skulls_parse(char const *text)
{
	unsigned long skulls = 0;

	while (text && *text)
	{
		size_t length;
		short skull;

		while (*text == ' ' || *text == ',')
			text++;
		for (length = 0; text[length] && text[length] != ' ' && text[length] != ','; length++)
		{
		}
		for (skull = 0; length && skull < NUMBER_OF_SKULLS; skull++)
		{
			if (strlen(skull_definitions[skull].key) == length &&
				!strncmp(text, skull_definitions[skull].key, length))
			{
				SET_FLAG(skulls, skull, TRUE);
			}
		}
		text += length;
	}
	return skulls;
}

void skulls_print(unsigned long skulls, char *text, unsigned long size)
{
	short skull;

	if (!size)
		return;
	text[0] = 0;
	for (skull = 0; skull < NUMBER_OF_SKULLS; skull++)
	{
		size_t length = strlen(text);

		if (TEST_FLAG(skulls, skull))
			snprintf(text + length, size - length, "%s%s", length ? " " : "", skull_definitions[skull].key);
	}
}

void skulls_player_died(void)
{
	if (!skulls_iron_reverts())
		return;
	skulls_iron_revert_pending = TRUE;
	main_lost_map();
}

boolean skulls_iron_restarts_level(void)
{
	return skull_active(_skull_iron) && !game_engine_running() && !skulls_cooperative() &&
		game_connection() == _game_connection_local;
}

boolean skulls_iron_reverts(void)
{
	return skull_active(_skull_iron) && !game_engine_running() && skulls_cooperative() &&
		!network_game_distributed_client() && game_connection() != _game_connection_film_playback;
}

boolean skulls_iron_holds_respawn(void)
{
	return skulls_iron_revert_pending && skulls_iron_reverts();
}

real skulls_enemy_vitality_scale(short team_index)
{
	return skull_active(_skull_mythic) && !game_engine_running() && team_index != NONE &&
		game_team_is_enemy(_game_team_player, team_index) ? 2.0f : 1.0f;
}

boolean skulls_weapon_usable(long unit_index, long weapon_index)
{
	struct unit_datum *unit;

	if (!skull_active(_skull_foreign))
		return TRUE;
	unit = unit_get(unit_index);
	/* (a player's own hands: not a vehicle's guns, which its seats use) */
	if (unit->unit.player_index == NONE || unit->object.type != _object_type_biped)
		return TRUE;
	return !skulls_weapon_covenant(weapon_index);
}

void skulls_unit_dropping_weapons(long unit_index)
{
	short slot;

	if (!skull_active(_skull_famine))
		return;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit_inventory_get_weapon(unit_index, slot);

		if (weapon_index != NONE)
			skulls_weapon_halve(weapon_index);
	}
}

boolean skulls_infinite_ammunition(void)
{
	return skull_active(_skull_bandanna);
}

short skulls_rounds_per_shot(boolean players, short rounds)
{
	return players && skull_active(_skull_recession) ? (short)(rounds * 2) : rounds;
}

real skulls_age_per_shot(real age)
{
	return skull_active(_skull_recession) ? age * 2.0f : age;
}

boolean skulls_shield_recharges(long object_index)
{
	struct unit_datum *unit;

	if (!skull_active(_skull_black_eye))
		return TRUE;
	unit = unit_try_and_get(object_index);
	return !unit || unit->unit.player_index == NONE;
}

void skulls_melee_struck(long object_index, long owner_object_index, short owner_team_index, boolean killed)
{
	struct object_datum *object = object_get(object_index);
	struct unit_datum *attacker;

	if (!skull_active(_skull_black_eye) && !skull_active(_skull_pinata))
		return;
	/* (a living unit, or one this blow killed, of the striker's enemies) */
	if (!TEST_FLAG(_object_mask_unit, object->object.type) ||
		(TEST_FLAG(object->object.damage_flags, _object_dead_bit) && !killed))
	{
		return;
	}
	if (owner_object_index == NONE || owner_object_index == object_index ||
		(attacker = unit_try_and_get(owner_object_index)) == NULL || attacker->unit.player_index == NONE)
	{
		return;
	}
	if (owner_team_index == NONE || !game_team_is_enemy(owner_team_index, object->object.owner_team_index))
		return;
	/* (the shields are the host's: its clients take them from it) */
	if (skull_active(_skull_black_eye) && !network_game_distributed_client())
		damage_port_shield_restore(owner_object_index, BLACK_EYE_SHIELD_PER_BLOW);
	/* (on a network client, a grenade it only shows: the host's deals the
	damage) */
	if (skull_active(_skull_pinata) && object->object.type == _object_type_biped)
		skulls_pinata_drop(object_index);
}

real skulls_explosion_scale(void)
{
	return skull_active(_skull_boom) ? 2.0f : 1.0f;
}

boolean skulls_aim_assist(void)
{
	return !skull_active(_skull_eye_patch);
}

void skulls_hud_update(short local_player_index, long unit_index)
{
	short index;

	if (!skulls_hud.initialized)
	{
		for (index = 0; index < MAXIMUM_LOCAL_PLAYERS; index++)
		{
			skulls_hud.unit_indices[index] = NONE;
			skulls_hud.hidden[index] = NONE;
		}
		skulls_hud.initialized = TRUE;
	}
	if (!VALID_INDEX(local_player_index, MAXIMUM_LOCAL_PLAYERS))
		return;
	/* (a new life: a unit after none, another unit, or the same one in a
	saved game loaded) */
	if (unit_index != NONE && (unit_index != skulls_hud.unit_indices[local_player_index] ||
		skulls_hud.loads[local_player_index] != skulls_game_state_loads))
	{
		skulls_hud.hidden[local_player_index] = skulls_hud_choose();
	}
	skulls_hud.unit_indices[local_player_index] = unit_index;
	skulls_hud.loads[local_player_index] = skulls_game_state_loads;
}

boolean skulls_hud_shown(short local_player_index, short part)
{
	if (part == _skull_hud_motion_sensor && skull_active(_skull_fog))
		return FALSE;
	return !skull_active(_skull_malfunction) || !skulls_hud.initialized ||
		!VALID_INDEX(local_player_index, MAXIMUM_LOCAL_PLAYERS) || skulls_hud.hidden[local_player_index] != part;
}

void skulls_game_state_loaded(void)
{
	skulls_game_state_loads++;
	skulls_iron_revert_pending = FALSE;
}
