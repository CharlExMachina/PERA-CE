/*
SPECTATOR.C

A multiplayer spectator's view. The host picks the spectators
(network_spectator.c); a spectator has no unit, and its player's controls
move its camera and pick whom it watches instead (player_control.c gives
them here, each frame).

The cameras (one of the director's, director.c): the free camera, which
flies through the map, and third person, which stays behind the player
watched and turns about them. A client has every player's unit and aim from
the host as its own players would see them (port/linux/NETCODE.md), and the
host sends a spectator's machine all of them every tick.

The controls, each the player's own binding of it:

	fire             the next player
	throw grenade    the player before
	action           the free camera, back to the player
	melee            a team game: every player, the red team's, the blue team's
	switch weapon    what is shown, in turn: all of it; the players; the players
	                 and the items; the items; all but the scores; the scores;
	                 nothing
	switch grenade   the free camera's speed
	flashlight       the controls' names, shown or not
	move, look       the free camera flies; in third person, look turns the
	                 camera about the player
	jump, crouch     the free camera up and down

With a team picked, the next player and the one before are that team's
living players only. A player watched who dies is watched still: the camera
stays on the body, and the time to their respawn is shown, as the game
counts it on this machine (a client's count is its own, started when it
learnt of the death).

What is shown: at the foot of the view, whom the spectator watches, with
their shields and health; at its top left, every player's shields and
health, or the time to their respawn; at its top, the scores (a team game's
teams', else the leading players'); at its bottom right, the controls, each
with the key or button the player has it on; and at its top right the map's
power items (overshields, camouflage, rocket launchers, sniper rifles), each
with the time to its next spawn. An item spawns again every so
often by the game's clock, whether it was taken or not
(game_engine_update_item_spawn), so every machine counts the same time from
the map's own placements: the host sends nothing for it.
*/

#include "cseries.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "items/equipment_definitions.h"
#include "items/item_definitions.h"
#include "items/items.h"
#include "math/integer_math.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "objects/object_definitions.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"

#include "network_distributed.h"
#include "network_spectator.h"
#include "spectator.h"

/* game_engine.c's, port_config.c's */
boolean game_engine_netgame_equipment_period(struct scenario_netgame_equipment const *equipment, long *period);
char const *config_string(char const *name);

/* ---------- constants */

enum
{
	_spectator_camera_free,
	_spectator_camera_third_person,
	NUMBER_OF_SPECTATOR_CAMERAS
};

/* (a team's index and one: red is team 0, blue team 1) */
enum
{
	_spectator_team_all,
	_spectator_team_red,
	_spectator_team_blue,
	NUMBER_OF_SPECTATOR_TEAMS
};

/* what is shown, which switch weapon changes in turn */
enum
{
	_spectator_overlay_all,
	_spectator_overlay_players,
	_spectator_overlay_players_and_items,
	_spectator_overlay_items,
	_spectator_overlay_all_but_scores,
	_spectator_overlay_scores,
	_spectator_overlay_none,
	NUMBER_OF_SPECTATOR_OVERLAYS
};

/* its parts: the player watched (the foot of the view), the players (its
top left), the items' timers (its top right), the scores (its top) and the
controls (its bottom right) */
enum
{
	_spectator_part_watched_bit,
	_spectator_part_players_bit,
	_spectator_part_items_bit,
	_spectator_part_scores_bit,
	_spectator_part_controls_bit,
};

/* the controls' presses */
enum
{
	_spectator_press_next,
	_spectator_press_previous,
	_spectator_press_free,
	_spectator_press_team,
	_spectator_press_overlay,
	_spectator_press_speed,
	_spectator_press_help,
	NUMBER_OF_SPECTATOR_PRESSES
};

enum
{
	_spectator_item_overshield,
	_spectator_item_camouflage,
	_spectator_item_rocket_launcher,
	_spectator_item_sniper_rifle,
	NUMBER_OF_SPECTATOR_ITEM_KINDS,

	/* the timers shown: a kind and how often it spawns */
	MAXIMUM_SPECTATOR_ITEM_TIMERS = 8,
	/* the players listed */
	MAXIMUM_SPECTATOR_LISTED_PLAYERS = 16,
	/* a view not updated for this many ticks starts over (its player was
	no spectator meanwhile) */
	SPECTATOR_STALE_TICKS = 15,
	NUMBER_OF_SPECTATOR_SPEEDS = 3,
};

#define SPECTATOR_FIELD_OF_VIEW DEGREES_TO_RADIANS(70.0f)
/* how far behind the player third person stays, and behind their vehicle */
#define SPECTATOR_FOLLOW_DISTANCE 2.2f
#define SPECTATOR_VEHICLE_DISTANCE 5.0f
/* how far up and down a camera looks (radians) */
#define SPECTATOR_PITCH_LIMIT 1.5f
/* a body is watched from above: how far the camera looks down at it, and
how far over the body it looks */
#define SPECTATOR_BODY_PITCH -0.7f
#define SPECTATOR_BODY_HEIGHT 0.25f
/* how near its placement an item counts as there (world units) */
#define SPECTATOR_ITEM_DISTANCE 0.75f
/* how far from the origin the free camera flies (the observer's bound) */
#define SPECTATOR_WORLD_BOUND 4500.0f

/* what is shown, sized for the game's 480 lines */
#define SPECTATOR_NAME_SCALE 1.0f
#define SPECTATOR_STATUS_SCALE 0.75f
#define SPECTATOR_HINT_SCALE 0.6f
#define SPECTATOR_LIST_SCALE 0.65f

enum
{
	SPECTATOR_BAR_WIDTH = 160,
	SPECTATOR_BAR_HEIGHT = 5,
	/* how far from the view's edges what is shown in its corners is */
	SPECTATOR_MARGIN = 10,
	SPECTATOR_LIST_NAME_WIDTH = 110,
	SPECTATOR_LIST_BAR_WIDTH = 56,
	SPECTATOR_LIST_BAR_HEIGHT = 3,
	SPECTATOR_TIMER_WIDTH = 190,
	/* the scores: a team game's row's width, a player's row's, and the
	players' rows in a game without teams */
	SPECTATOR_TEAM_SCORE_WIDTH = 230,
	SPECTATOR_SCORE_WIDTH = 150,
	MAXIMUM_SPECTATOR_SCORE_ROWS = 4,
	MAXIMUM_SPECTATOR_CONTROLS = 10,
	SPECTATOR_CONTROL_TEXT_LENGTH = 48,
};

/* ---------- structures */

/* director.c's camera_control, as much of it as every camera reads */
struct spectator_camera_control
{
	short local_player_index;
	boolean active;
	byte pad3;
	real seconds_elapsed;
};

struct spectator_view
{
	boolean initialized;
	short camera;
	short team;
	short overlay;
	short speed;
	/* the game's tick it was last updated at */
	long last_time;
	/* the player watched, NONE for none */
	long watched_player_index;
	/* where the camera is (the free camera's), or looks at; and which way
	it faces, or the player watched does */
	real_point3d position;
	real_euler_angles2d facing;
	/* how far third person is turned from behind the player */
	real_euler_angles2d orbit;
	/* the controls since the camera was last updated: how far it turned,
	its throttle, up less down, and the presses */
	real_euler_angles2d turn;
	real_vector2d throttle;
	real rise;
	unsigned long held;
	unsigned long pressed;
};

struct spectator_item_timer
{
	short kind;
	/* ticks between its spawns */
	long period;
	/* its placements, and those with an item at them now */
	short count;
	short present;
};

/* ---------- globals */

static struct spectator_view spectator_views[MAXIMUM_LOCAL_PLAYERS];

/* the free camera's speeds, world units a second */
static real const spectator_speeds[NUMBER_OF_SPECTATOR_SPEEDS] = { 3.0f, 8.0f, 20.0f };

/* the map's item timers, counted again each second of the game */
static struct spectator_item_timer spectator_item_timers[MAXIMUM_SPECTATOR_ITEM_TIMERS];
static short spectator_item_timer_count;
static long spectator_item_timers_second = NONE;

/* whether each local player has put away the controls' names at the foot of
its view (kept from game to game) */
static boolean spectator_help_hidden[MAXIMUM_LOCAL_PLAYERS];

/* the parts of each of what is shown */
static byte const spectator_overlay_parts[NUMBER_OF_SPECTATOR_OVERLAYS] =
{
	FLAG(_spectator_part_watched_bit) | FLAG(_spectator_part_players_bit) | FLAG(_spectator_part_items_bit) |
		FLAG(_spectator_part_scores_bit) | FLAG(_spectator_part_controls_bit),
	FLAG(_spectator_part_players_bit),
	FLAG(_spectator_part_players_bit) | FLAG(_spectator_part_items_bit),
	FLAG(_spectator_part_items_bit),
	FLAG(_spectator_part_watched_bit) | FLAG(_spectator_part_players_bit) | FLAG(_spectator_part_items_bit) |
		FLAG(_spectator_part_controls_bit),
	FLAG(_spectator_part_scores_bit),
	0,
};

static wchar_t const *const spectator_item_names[NUMBER_OF_SPECTATOR_ITEM_KINDS] =
{
	L"OVERSHIELD",
	L"CAMOUFLAGE",
	L"ROCKETS",
	L"SNIPER",
};

/* ---------- private code: whom to watch */

static struct spectator_view *spectator_view_get(
	short local_player_index)
{
	return local_player_index >= 0 && local_player_index < MAXIMUM_LOCAL_PLAYERS ?
		&spectator_views[local_player_index] : NULL;
}

/* whether the view was updated lately: its player is a spectator now */
static boolean spectator_view_current(
	struct spectator_view const *view)
{
	long now = game_time_get();

	return view && view->initialized && now >= view->last_time && now <= view->last_time + SPECTATOR_STALE_TICKS;
}

/* whether the player is one a spectator watches at all: in the game, and
no spectator */
static boolean spectator_player_watchable(
	long self,
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player_index != self && !player->quit_out_of_game && !network_spectator_player(player_index);
}

/* ... and one the next player may be: with a team picked, of that team and
alive */
static boolean spectator_player_is_candidate(
	struct spectator_view const *view,
	long self,
	long player_index,
	boolean living)
{
	struct player_datum *player;

	if (!spectator_player_watchable(self, player_index))
		return FALSE;
	player = player_get(player_index);
	if (view->team != _spectator_team_all && game_engine_has_teams())
	{
		if (player->team_index != view->team - _spectator_team_red)
			return FALSE;
		living = TRUE;
	}
	return !living || player->unit_index != NONE;
}

/* the candidate after the player (or before), round them all; NONE for none.
The living come first: the dead only when no one is alive. */
static long spectator_next_player(
	struct spectator_view const *view,
	long self,
	long from,
	boolean forward)
{
	short pass;

	for (pass = 0; pass < 2; pass++)
	{
		struct data_iterator iterator;
		long first = NONE;
		long last = NONE;
		long before = NONE;
		long after = NONE;
		long from_index = from != NONE ? DATUM_INDEX_TO_ABSOLUTE_INDEX(from) : NONE;

		data_iterator_new(&iterator, player_data);
		while (data_iterator_next(&iterator))
		{
			long index = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

			if (!spectator_player_is_candidate(view, self, iterator.datum_index, pass == 0))
				continue;
			if (first == NONE)
				first = iterator.datum_index;
			last = iterator.datum_index;
			if (from_index != NONE && index < from_index)
				before = iterator.datum_index;
			if (from_index != NONE && index > from_index && after == NONE)
				after = iterator.datum_index;
		}
		if (first != NONE)
		{
			if (forward)
				return after != NONE ? after : first;
			return before != NONE ? before : last;
		}
	}
	return NONE;
}

/* the unit the camera is on: the player's living unit (alive), else their
body; NONE for neither */
static long spectator_watched_unit(
	struct spectator_view const *view,
	boolean *alive)
{
	struct player_datum *player = view->watched_player_index != NONE ?
		player_try_and_get(view->watched_player_index) : NULL;
	long unit_index = NONE;

	*alive = FALSE;
	if (player)
	{
		if (player->unit_index != NONE && object_try_and_get(player->unit_index))
		{
			unit_index = player->unit_index;
			*alive = TRUE;
		}
		else if (player->dead_unit_index != NONE && object_try_and_get(player->dead_unit_index))
		{
			unit_index = player->dead_unit_index;
		}
	}
	return unit_index;
}

/* ---------- private code: the cameras */

static void spectator_view_begin(
	struct spectator_view *view,
	short local_player_index)
{
	if (!spectator_view_current(view))
	{
		struct observer_result const *observer = observer_get_camera(local_player_index);

		csmemset(view, 0, sizeof(*view));
		view->initialized = TRUE;
		view->camera = _spectator_camera_third_person;
		view->speed = 1;
		view->watched_player_index = NONE;
		/* (from where the player's own camera was) */
		view->position = observer->position;
		if (magnitude_squared3d(&observer->forward) > 0.5f)
			euler_angles2d_from_vector3d(&view->facing, &observer->forward);
	}
	view->last_time = game_time_get();
}

/* the free camera from where the view is now */
static void spectator_view_free(
	struct spectator_view *view,
	short local_player_index)
{
	struct observer_result const *observer = observer_get_camera(local_player_index);

	view->camera = _spectator_camera_free;
	view->position = observer->position;
	if (magnitude_squared3d(&observer->forward) > 0.5f)
		euler_angles2d_from_vector3d(&view->facing, &observer->forward);
}

/* what the controls pressed since the last update do */
static void spectator_view_controls(
	struct spectator_view *view,
	long self,
	short local_player_index)
{
	unsigned long pressed = view->pressed;

	view->pressed = 0;
	if (TEST_FLAG(pressed, _spectator_press_team) && game_engine_has_teams())
	{
		view->team = (short)((view->team + 1) % NUMBER_OF_SPECTATOR_TEAMS);
		/* (the player watched is of another team: the team's first) */
		if (view->camera != _spectator_camera_free &&
			!spectator_player_is_candidate(view, self, view->watched_player_index, FALSE))
		{
			long next = spectator_next_player(view, self, view->watched_player_index, TRUE);

			if (next != NONE)
			{
				view->watched_player_index = next;
				view->orbit.yaw = view->orbit.pitch = 0.0f;
			}
		}
	}
	if (TEST_FLAG(pressed, _spectator_press_next) || TEST_FLAG(pressed, _spectator_press_previous))
	{
		long next = spectator_next_player(view, self, view->watched_player_index,
			TEST_FLAG(pressed, _spectator_press_next));

		if (next != NONE)
		{
			view->watched_player_index = next;
			view->orbit.yaw = view->orbit.pitch = 0.0f;
			if (view->camera == _spectator_camera_free)
				view->camera = _spectator_camera_third_person;
		}
	}
	if (TEST_FLAG(pressed, _spectator_press_free))
	{
		if (view->camera == _spectator_camera_free)
			view->camera = _spectator_camera_third_person;
		else
			spectator_view_free(view, local_player_index);
	}
	if (TEST_FLAG(pressed, _spectator_press_overlay))
		view->overlay = (short)((view->overlay + 1) % NUMBER_OF_SPECTATOR_OVERLAYS);
	if (TEST_FLAG(pressed, _spectator_press_help))
		spectator_help_hidden[local_player_index] = !spectator_help_hidden[local_player_index];
	if (TEST_FLAG(pressed, _spectator_press_speed))
		view->speed = (short)((view->speed + 1) % NUMBER_OF_SPECTATOR_SPEEDS);

	/* (a player's camera needs a player: the one watched while they are in
	the game, else the next; with none, the free camera) */
	if (view->camera != _spectator_camera_free)
	{
		if (!spectator_player_watchable(self, view->watched_player_index))
		{
			view->watched_player_index = spectator_next_player(view, self, view->watched_player_index, TRUE);
			view->orbit.yaw = view->orbit.pitch = 0.0f;
		}
		if (view->watched_player_index == NONE)
			spectator_view_free(view, local_player_index);
	}
}

/* a command that takes the camera there at once (the cameras' own state is
each frame's already) */
static void spectator_command_begin(
	struct observer_command *command)
{
	command->flags = FLAG(_observer_command_valid_bit);
	command->timer = 0.0f;
	command->parameter_flags[_observer_command_parameter_focus_position] =
		FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->parameter_flags[_observer_command_parameter_focus_offset] =
		FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->parameter_flags[_observer_command_parameter_focus_distance] =
		FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->parameter_flags[_observer_command_parameter_orientation] =
		FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->parameter_flags[_observer_command_parameter_field_of_view] =
		FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->field_of_view = SPECTATOR_FIELD_OF_VIEW;
}

static void spectator_camera_free(
	struct spectator_view *view,
	struct observer_command *command,
	real seconds)
{
	real_vector3d forward;
	real_vector3d left;
	real speed = spectator_speeds[view->speed] * seconds;
	real flat;

	view->facing.yaw += view->turn.yaw;
	view->facing.pitch = PIN(view->facing.pitch + view->turn.pitch, -SPECTATOR_PITCH_LIMIT, SPECTATOR_PITCH_LIMIT);
	vector3d_from_euler_angles2d(&forward, &view->facing);
	/* (it flies where it looks, sideways level) */
	flat = square_root(forward.i * forward.i + forward.j * forward.j);
	left.i = flat > 0.0001f ? -forward.j / flat : 0.0f;
	left.j = flat > 0.0001f ? forward.i / flat : 1.0f;
	left.k = 0.0f;
	view->position.x += (forward.i * view->throttle.i + left.i * view->throttle.j) * speed;
	view->position.y += (forward.j * view->throttle.i + left.j * view->throttle.j) * speed;
	view->position.z += (forward.k * view->throttle.i + view->rise) * speed;
	view->position.x = PIN(view->position.x, -SPECTATOR_WORLD_BOUND, SPECTATOR_WORLD_BOUND);
	view->position.y = PIN(view->position.y, -SPECTATOR_WORLD_BOUND, SPECTATOR_WORLD_BOUND);
	view->position.z = PIN(view->position.z, -SPECTATOR_WORLD_BOUND, SPECTATOR_WORLD_BOUND);

	spectator_command_begin(command);
	command->focus_position = view->position;
	command->focus_distance = 0.0f;
	command->forward = forward;
	observer_up_from_forward(&command->forward, &command->up);
}

/* behind the unit (a living player's, which the camera turns with, or a
body), turned about it as the spectator looks */
static void spectator_camera_third_person(
	struct spectator_view *view,
	struct observer_command *command,
	long unit_index,
	boolean alive)
{
	real_euler_angles2d angles;
	real distance = SPECTATOR_FOLLOW_DISTANCE;

	if (unit_index != NONE)
	{
		struct unit_datum *unit = unit_get(unit_index);

		if (alive)
		{
			unit_get_camera_position(unit_index, &view->position);
			if (magnitude_squared3d(&unit->unit.aiming_vector) > 0.5f)
				euler_angles2d_from_vector3d(&view->facing, &unit->unit.aiming_vector);
			if (unit->object.parent_object_index != NONE)
				distance = SPECTATOR_VEHICLE_DISTANCE;
		}
		else
		{
			view->position = unit->object.bounding_sphere_center;
			view->position.z += SPECTATOR_BODY_HEIGHT;
			view->facing.pitch = SPECTATOR_BODY_PITCH;
		}
	}
	view->orbit.yaw += view->turn.yaw;
	view->orbit.pitch = PIN(view->orbit.pitch + view->turn.pitch, -2.0f * SPECTATOR_PITCH_LIMIT,
		2.0f * SPECTATOR_PITCH_LIMIT);
	angles.yaw = view->facing.yaw + view->orbit.yaw;
	angles.pitch = PIN(view->facing.pitch + view->orbit.pitch, -SPECTATOR_PITCH_LIMIT, SPECTATOR_PITCH_LIMIT);

	spectator_command_begin(command);
	command->focus_position = view->position;
	command->focus_distance = distance;
	vector3d_from_euler_angles2d(&command->forward, &angles);
	observer_up_from_forward(&command->forward, &command->up);
}

/* ---------- private code: the items' timers */

/* the kind of power item a weapon or equipment tag is, NONE for none: the
powerups by their kind, the weapons by their tags' names (a map's own
rocket launcher or sniper rifle is one too) */
static short spectator_item_kind(
	long definition_index)
{
	short type;

	if (definition_index == NONE)
		return NONE;
	type = object_definition_get(definition_index)->object.type;
	if (type == _object_type_equipment)
	{
		switch (equipment_definition_get(definition_index)->equipment.powerup_type)
		{
		case _equipment_powerup_overshield: return _spectator_item_overshield;
		case _equipment_powerup_active_camouflage: return _spectator_item_camouflage;
		default: return NONE;
		}
	}
	if (type == _object_type_weapon)
	{
		char const *name = tag_name_strip_path(tag_get_name(definition_index));

		if (name && strstr(name, "rocket"))
			return _spectator_item_rocket_launcher;
		if (name && strstr(name, "sniper"))
			return _spectator_item_sniper_rifle;
	}
	return NONE;
}

/* the kind a placement's item collection spawns, as this game has it (its
weapons, its shields and camouflage: game_engine_remap_object_definition),
NONE for no power item */
static short spectator_placement_kind(
	struct scenario_netgame_equipment const *equipment)
{
	struct item_collection_definition *collection;
	long index;

	if (equipment->item_collection.index == NONE)
		return NONE;
	collection = (struct item_collection_definition *)tag_get(ITEM_COLLECTION_DEFINITION_TAG,
		equipment->item_collection.index);
	for (index = 0; index < collection->permutations.count; index++)
	{
		struct item_permutation_definition const *permutation = TAG_BLOCK_GET_ELEMENT(&collection->permutations,
			index, struct item_permutation_definition);

		/* (a power item as the map has it, then as the game spawns it: the
		grenades, which the game picks among at random, are none) */
		if (spectator_item_kind(permutation->item.index) != NONE)
		{
			long definition_index = game_engine_remap_object_definition(permutation->item.index);
			short kind = spectator_item_kind(definition_index);

			if (kind != NONE && !(game_variant_options_get()->no_map_weapons &&
				object_definition_get(definition_index)->object.type == _object_type_weapon))
			{
				return kind;
			}
		}
	}
	return NONE;
}

/* whether an item lies at the placement */
static boolean spectator_placement_has_item(
	struct scenario_netgame_equipment const *equipment)
{
	struct object_iterator iterator;

	object_iterator_new(&iterator, FLAG(_object_type_weapon) | FLAG(_object_type_equipment), 0);
	while (object_iterator_next(&iterator))
	{
		struct item_datum *item = item_get(iterator.index);

		if (!TEST_FLAG(item->item.flags, _item_attached_to_unit_bit) &&
			distance_squared3d(&item->object.position, &equipment->position) <
				SPECTATOR_ITEM_DISTANCE * SPECTATOR_ITEM_DISTANCE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* the map's power items, a timer for each kind and time between spawns,
counted once a second */
static void spectator_item_timers_update(
	void)
{
	struct scenario *scenario = global_scenario_get();
	long second = game_time_get() / TICKS_PER_SECOND;
	long equipment_index;

	if (second == spectator_item_timers_second)
		return;
	spectator_item_timers_second = second;
	spectator_item_timer_count = 0;
	for (equipment_index = 0; equipment_index < scenario->netgame_equipment.count; equipment_index++)
	{
		struct scenario_netgame_equipment const *equipment = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_equipment,
			equipment_index, struct scenario_netgame_equipment);
		struct spectator_item_timer *timer = NULL;
		long period;
		short kind;
		short index;

		if (!game_engine_netgame_equipment_period(equipment, &period) || period <= 0)
			continue;
		kind = spectator_placement_kind(equipment);
		if (kind == NONE)
			continue;
		for (index = 0; index < spectator_item_timer_count; index++)
		{
			if (spectator_item_timers[index].kind == kind && spectator_item_timers[index].period == period)
				timer = &spectator_item_timers[index];
		}
		if (!timer)
		{
			if (spectator_item_timer_count >= MAXIMUM_SPECTATOR_ITEM_TIMERS)
				continue;
			timer = &spectator_item_timers[spectator_item_timer_count++];
			timer->kind = kind;
			timer->period = period;
			timer->count = 0;
			timer->present = 0;
		}
		timer->count++;
		if (spectator_placement_has_item(equipment))
			timer->present++;
	}
}

/* ---------- private code: what is shown */

static pixel32 spectator_pixel(
	real_argb_color const *color)
{
	return ((pixel32)(long)(PIN(color->alpha, 0.0f, 1.0f) * 255.0f) << 24) |
		((pixel32)(long)(PIN(color->red, 0.0f, 1.0f) * 255.0f) << 16) |
		((pixel32)(long)(PIN(color->green, 0.0f, 1.0f) * 255.0f) << 8) |
		(pixel32)(long)(PIN(color->blue, 0.0f, 1.0f) * 255.0f);
}

static void spectator_draw_quad(
	short x0,
	short y0,
	short x1,
	short y1,
	pixel32 color)
{
	rectangle2d rectangle;

	if (x1 <= x0 || y1 <= y0)
		return;
	rectangle.x0 = x0;
	rectangle.y0 = y0;
	rectangle.x1 = x1;
	rectangle.y1 = y1;
	draw_quad(&rectangle, color);
}

/* a line of text from (x0, y0) as wide as x1, at a scale: left (0), right
(1) or centered (2) */
static void spectator_draw_text(
	short x0,
	short y0,
	short x1,
	short justification,
	real scale,
	real_argb_color const *color,
	wchar_t const *text)
{
	long font_index = hud_get_font_index();
	struct font_header *font;
	rectangle2d bounds;

	if (font_index == NONE || x1 <= x0)
		return;
	font = font_definition_get(font_index);
	/* (laid out at full size, then drawn scaled about its top left) */
	bounds.x0 = x0;
	bounds.y0 = y0;
	bounds.x1 = (short)(x0 + (x1 - x0) / scale);
	bounds.y1 = (short)(y0 + 2 * (font->ascending_height + font->descending_height + font->leading_height));
	rasterizer_text_set_scale(scale, (real)x0, (real)y0);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static short spectator_line_height(
	real scale)
{
	long font_index = hud_get_font_index();
	struct font_header *font;

	if (font_index == NONE)
		return 0;
	font = font_definition_get(font_index);
	return (short)((font->ascending_height + font->descending_height + font->leading_height) * scale);
}

/* a bar as full as the fraction, on a dark ground */
static void spectator_draw_bar(
	short x0,
	short y0,
	short width,
	short height,
	real fraction,
	pixel32 color,
	real alpha)
{
	pixel32 ground = (pixel32)(long)(160.0f * alpha) << 24;
	pixel32 fill = ((pixel32)(long)(255.0f * alpha) << 24) | (color & 0x00FFFFFF);

	spectator_draw_quad((short)(x0 - 1), (short)(y0 - 1), (short)(x0 + width + 1), (short)(y0 + height + 1), ground);
	spectator_draw_quad(x0, y0, (short)(x0 + (short)(width * PIN(fraction, 0.0f, 1.0f))), (short)(y0 + height), fill);
}

/* a unit's shields over its health: an overshield, past full, in another
color over the shield's */
static void spectator_draw_vitality(
	short x0,
	short y0,
	short width,
	short height,
	long unit_index)
{
	struct object_datum *object = object_try_and_get(unit_index);
	real shield = object ? object->object.shield_vitality : 0.0f;
	real body = object ? object->object.body_vitality : 0.0f;
	pixel32 body_color = body > 0.6f ? 0x0050D050 : body > 0.3f ? 0x00E0C040 : 0x00E04040;

	spectator_draw_bar(x0, y0, width, height, shield, 0x0050C8FF, 1.0f);
	if (shield > 1.0f)
	{
		spectator_draw_quad(x0, y0, (short)(x0 + (short)(width * PIN((shield - 1.0f) * 0.5f, 0.0f, 1.0f))),
			(short)(y0 + height), 0xFF80FF90);
	}
	spectator_draw_bar(x0, (short)(y0 + height + 2), width, height, body, body_color, 1.0f);
}

/* a player's color: its team's in a team game */
static void spectator_player_color(
	struct player_datum const *player,
	real_argb_color *color)
{
	*color = *global_real_argb_white;
	if (game_engine_has_teams())
	{
		if (player->team_index == 0)
		{
			color->red = 1.0f;
			color->green = 0.45f;
			color->blue = 0.45f;
		}
		else
		{
			color->red = 0.5f;
			color->green = 0.65f;
			color->blue = 1.0f;
		}
	}
}

/* the seconds until a dead player's respawn as this machine counts them,
NONE for one who has none coming (out of lives, or out of the game) */
static long spectator_respawn_seconds(
	long player_index)
{
	struct player_datum *player = player_get(player_index);

	if (game_engine_man_out(player_index))
		return NONE;
	return (player->respawn_timer + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
}

/* how wide a line of text is drawn at a scale */
static short spectator_text_width(
	real scale,
	wchar_t const *text)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds;
	rectangle2d ink;
	rectangle2d cursor;

	if (font_index == NONE || !text[0])
		return 0;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 4000;
	bounds.y1 = 200;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, global_real_argb_white);
	draw_unicode_string_compute_bounds(&bounds, text, &ink, &cursor);
	return ink.x1 > bounds.x0 ? (short)((ink.x1 - bounds.x0) * scale + 1.0f) : 0;
}

/* every player, in its team's color: its shields and health, or the time to
its respawn; the player watched lit. At the view's top left. */
static void spectator_draw_players(
	struct spectator_view const *view,
	long self,
	rectangle2d const *frame)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short row_height = (short)(spectator_line_height(SPECTATOR_LIST_SCALE) + 2);
	short y = frame->y0;
	short x = frame->x0;
	short rows = 0;
	short pass;

	if (row_height <= 2)
		return;
	/* (a team game's by team: red, then blue) */
	for (pass = 0; pass < 2; pass++)
	{
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL &&
			rows < MAXIMUM_SPECTATOR_LISTED_PLAYERS)
		{
			real_argb_color color;
			wchar_t text[32];
			short bar_x = (short)(x + 8 + SPECTATOR_LIST_NAME_WIDTH);

			if (!spectator_player_watchable(self, iterator.datum_index))
				continue;
			if (game_engine_has_teams() ? (player->team_index == 0) != (pass == 0) : pass != 0)
				continue;
			spectator_player_color(player, &color);
			if (iterator.datum_index == view->watched_player_index && view->camera != _spectator_camera_free)
			{
				spectator_draw_quad((short)(x - 2), (short)(y - 1),
					(short)(bar_x + SPECTATOR_LIST_BAR_WIDTH + 4), (short)(y + row_height - 1), 0x50FFFFFF);
			}
			else
			{
				spectator_draw_quad((short)(x - 2), (short)(y - 1),
					(short)(bar_x + SPECTATOR_LIST_BAR_WIDTH + 4), (short)(y + row_height - 1), 0x50000000);
			}
			spectator_draw_quad(x, y, (short)(x + 3), (short)(y + row_height - 2), spectator_pixel(&color));
			usnprintf(text, NUMBEROF(text) - 1, L"%.12s", player->name);
			text[NUMBEROF(text) - 1] = 0;
			spectator_draw_text((short)(x + 8), y, bar_x, 0, SPECTATOR_LIST_SCALE, &color, text);
			if (player->unit_index != NONE)
			{
				spectator_draw_vitality(bar_x, (short)(y + (row_height - 2 * SPECTATOR_LIST_BAR_HEIGHT - 2) / 2 - 1),
					SPECTATOR_LIST_BAR_WIDTH, SPECTATOR_LIST_BAR_HEIGHT, player->unit_index);
			}
			else
			{
				long seconds = spectator_respawn_seconds(iterator.datum_index);
				real_argb_color dim = *global_real_argb_white;

				dim.alpha = 0.7f;
				if (seconds == NONE)
					usnprintf(text, NUMBEROF(text) - 1, L"OUT");
				else
					usnprintf(text, NUMBEROF(text) - 1, L"%ld:%02ld", seconds / 60, seconds % 60);
				text[NUMBEROF(text) - 1] = 0;
				spectator_draw_text(bar_x, y, (short)(bar_x + SPECTATOR_LIST_BAR_WIDTH), 1, SPECTATOR_LIST_SCALE,
					&dim, text);
			}
			y = (short)(y + row_height);
			rows++;
		}
	}
}

/* the map's power items: each kind, the time to its next spawn, and how
many of it lie where they spawn. At the view's top right. */
static void spectator_draw_item_timers(
	rectangle2d const *frame)
{
	short row_height = (short)(spectator_line_height(SPECTATOR_STATUS_SCALE) + 2);
	short y = frame->y0;
	short index;

	spectator_item_timers_update();
	if (row_height <= 2)
		return;
	for (index = 0; index < spectator_item_timer_count; index++)
	{
		struct spectator_item_timer const *timer = &spectator_item_timers[index];
		long seconds = (timer->period - game_time_get() % timer->period + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND;
		real_argb_color color = *global_real_argb_white;
		wchar_t state[16];
		wchar_t text[64];

		if (timer->present >= timer->count)
			usnprintf(state, NUMBEROF(state) - 1, L"UP");
		else if (timer->present == 0)
			usnprintf(state, NUMBEROF(state) - 1, L"TAKEN");
		else
			usnprintf(state, NUMBEROF(state) - 1, L"%d OF %d", timer->present, timer->count);
		state[NUMBEROF(state) - 1] = 0;
		usnprintf(text, NUMBEROF(text) - 1, L"%s   %s   %ld:%02ld", spectator_item_names[timer->kind], state,
			seconds / 60, seconds % 60);
		text[NUMBEROF(text) - 1] = 0;
		/* (about to spawn: lit) */
		if (seconds <= 10)
		{
			color.green = 0.85f;
			color.blue = 0.3f;
		}
		else if (timer->present == 0)
		{
			color.alpha = 0.75f;
		}
		spectator_draw_quad((short)(frame->x1 - SPECTATOR_TIMER_WIDTH - 4), (short)(y - 1), (short)(frame->x1 + 2),
			(short)(y + row_height - 1), 0x50000000);
		spectator_draw_text((short)(frame->x1 - SPECTATOR_TIMER_WIDTH), y, frame->x1, 1, SPECTATOR_STATUS_SCALE, &color,
			text);
		y = (short)(y + row_height);
	}
}

/* the scores, as the game type words them: a team game's two teams', side
by side; else the leading players', a row each. At the middle of the view's
top. */
static void spectator_draw_scores(
	long self,
	rectangle2d const *frame)
{
	short center = (short)((frame->x0 + frame->x1) / 2);
	short y = frame->y0;
	wchar_t score[256];
	wchar_t text[64];

	if (!game_engine || !game_engine->format_player_score)
		return;
	if (game_engine_has_teams() && game_engine->format_team_name)
	{
		short row_height = (short)(spectator_line_height(SPECTATOR_NAME_SCALE) + 2);
		struct player_datum team;
		real_argb_color color;

		if (row_height <= 2)
			return;
		spectator_draw_quad((short)(center - SPECTATOR_TEAM_SCORE_WIDTH / 2), (short)(y - 1),
			(short)(center + SPECTATOR_TEAM_SCORE_WIDTH / 2), (short)(y + row_height - 1), 0x50000000);
		/* (red's to the left of the middle, blue's to its right) */
		csmemset(&team, 0, sizeof(team));
		team.team_index = 0;
		spectator_player_color(&team, &color);
		score[0] = 0;
		game_engine->format_team_name(0, score);
		usnprintf(text, NUMBEROF(text) - 1, L"RED   %.20s", score);
		text[NUMBEROF(text) - 1] = 0;
		spectator_draw_text((short)(center - SPECTATOR_TEAM_SCORE_WIDTH / 2), y, (short)(center - 8), 1,
			SPECTATOR_NAME_SCALE, &color, text);
		team.team_index = 1;
		spectator_player_color(&team, &color);
		score[0] = 0;
		game_engine->format_team_name(1, score);
		usnprintf(text, NUMBEROF(text) - 1, L"%.20s   BLUE", score);
		text[NUMBEROF(text) - 1] = 0;
		spectator_draw_text((short)(center + 8), y, (short)(center + SPECTATOR_TEAM_SCORE_WIDTH / 2), 0,
			SPECTATOR_NAME_SCALE, &color, text);
	}
	else if (game_engine->get_player_score)
	{
		short row_height = (short)(spectator_line_height(SPECTATOR_STATUS_SCALE) + 2);
		long leaders[MAXIMUM_SPECTATOR_SCORE_ROWS];
		long scores[MAXIMUM_SPECTATOR_SCORE_ROWS];
		struct data_iterator iterator;
		short count = 0;
		short index;

		if (row_height <= 2)
			return;
		/* (the players of the highest scores, the first of two alike first) */
		data_iterator_new(&iterator, player_data);
		while (data_iterator_next(&iterator))
		{
			long player_score;
			short place;

			if (!spectator_player_watchable(self, iterator.datum_index))
				continue;
			player_score = game_engine->get_player_score(iterator.datum_index, _get_score_individual);
			for (place = count; place > 0 && scores[place - 1] < player_score; place--)
			{
				if (place < MAXIMUM_SPECTATOR_SCORE_ROWS)
				{
					leaders[place] = leaders[place - 1];
					scores[place] = scores[place - 1];
				}
			}
			if (place < MAXIMUM_SPECTATOR_SCORE_ROWS)
			{
				leaders[place] = iterator.datum_index;
				scores[place] = player_score;
				if (count < MAXIMUM_SPECTATOR_SCORE_ROWS)
					count++;
			}
		}
		for (index = 0; index < count; index++)
		{
			struct player_datum *player = player_get(leaders[index]);
			real_argb_color color = *global_real_argb_white;

			spectator_draw_quad((short)(center - SPECTATOR_SCORE_WIDTH / 2 - 4), (short)(y - 1),
				(short)(center + SPECTATOR_SCORE_WIDTH / 2 + 4), (short)(y + row_height - 1), 0x50000000);
			usnprintf(text, NUMBEROF(text) - 1, L"%d   %.12s", index + 1, player->name);
			text[NUMBEROF(text) - 1] = 0;
			spectator_draw_text((short)(center - SPECTATOR_SCORE_WIDTH / 2), y, (short)(center + 30), 0,
				SPECTATOR_STATUS_SCALE, &color, text);
			score[0] = 0;
			game_engine->format_player_score(leaders[index], score);
			usnprintf(text, NUMBEROF(text) - 1, L"%.20s", score);
			text[NUMBEROF(text) - 1] = 0;
			spectator_draw_text((short)(center + 30), y, (short)(center + SPECTATOR_SCORE_WIDTH / 2), 1,
				SPECTATOR_STATUS_SCALE, &color, text);
			y = (short)(y + row_height);
		}
	}
}

/* a control with the key or button it is on, as "(MB1) Next Player": the
first of its setting's (controls.fire and the others, config.toml: a list of
keys' and mouse buttons' names), the mouse's buttons by their numbers */
static void spectator_control_text(
	char const *setting,
	wchar_t const *does,
	wchar_t *text)
{
	static struct
	{
		char const *name;
		char const *shown;
	} const names[] =
	{
		{ "MOUSE LEFT", "MB1" },
		{ "MOUSE RIGHT", "MB2" },
		{ "MOUSE MIDDLE", "MB3" },
		{ "MOUSE 4", "MB4" },
		{ "MOUSE 5", "MB5" },
		{ "WHEEL UP", "WHEEL UP" },
		{ "WHEEL DOWN", "WHEEL DOWN" },
		{ "LEFT CTRL", "CTRL" },
		{ "RIGHT CTRL", "RCTRL" },
		{ "LEFT SHIFT", "SHIFT" },
		{ "RIGHT SHIFT", "RSHIFT" },
		{ "LEFT ALT", "ALT" },
		{ "RIGHT ALT", "RALT" },
	};
	char const *value = config_string(setting);
	char key[24];
	wchar_t wide_key[24];
	short length = 0;
	short index;

	while (value && (*value == ' ' || *value == '\t'))
		value++;
	for (; value && *value && *value != ',' && length < (short)sizeof(key) - 1; value++)
		key[length++] = (char)(*value >= 'a' && *value <= 'z' ? *value - 'a' + 'A' : *value);
	while (length > 0 && (key[length - 1] == ' ' || key[length - 1] == '\t'))
		length--;
	key[length] = 0;
	for (index = 0; index < (short)NUMBEROF(names); index++)
	{
		if (!strcmp(key, names[index].name))
		{
			csstrncpy(key, names[index].shown, sizeof(key) - 1);
			key[sizeof(key) - 1] = 0;
		}
	}
	for (index = 0; key[index]; index++)
		wide_key[index] = (wchar_t)(unsigned char)key[index];
	wide_key[index] = 0;
	if (wide_key[0])
		usnprintf(text, SPECTATOR_CONTROL_TEXT_LENGTH - 1, L"(%s) %s", wide_key, does);
	else
		usnprintf(text, SPECTATOR_CONTROL_TEXT_LENGTH - 1, L"%s", does);
	text[SPECTATOR_CONTROL_TEXT_LENGTH - 1] = 0;
}

/* the controls, a row each, one under the other at the view's bottom right:
those of the camera the spectator has */
static void spectator_draw_controls(
	struct spectator_view const *view,
	rectangle2d const *frame)
{
	wchar_t rows[MAXIMUM_SPECTATOR_CONTROLS][SPECTATOR_CONTROL_TEXT_LENGTH];
	real_argb_color color = *global_real_argb_white;
	short row_height = (short)(spectator_line_height(SPECTATOR_LIST_SCALE) + 1);
	boolean free_camera = view->camera == _spectator_camera_free;
	short count = 0;
	short width = 0;
	short index;
	short x, y;

	if (row_height <= 1)
		return;
	spectator_control_text("controls.fire", L"Next Player", rows[count++]);
	spectator_control_text("controls.throw_grenade", L"Previous Player", rows[count++]);
	spectator_control_text("controls.action", free_camera ? L"Follow Player" : L"Freecam", rows[count++]);
	if (game_engine_has_teams())
		spectator_control_text("controls.melee", L"Team Filter", rows[count++]);
	if (free_camera)
	{
		spectator_control_text("controls.jump", L"Freecam Up", rows[count++]);
		spectator_control_text("controls.crouch", L"Freecam Down", rows[count++]);
		spectator_control_text("controls.switch_grenade", L"Freecam Speed", rows[count++]);
	}
	spectator_control_text("controls.switch_weapon", L"Change Display", rows[count++]);
	spectator_control_text("controls.flashlight", L"Hide Controls", rows[count++]);
	for (index = 0; index < count; index++)
		width = (short)MAX(width, spectator_text_width(SPECTATOR_LIST_SCALE, rows[index]));
	x = (short)(frame->x1 - width);
	y = (short)(frame->y1 - count * row_height);
	spectator_draw_quad((short)(x - 6), (short)(y - 3), (short)(frame->x1 + 4), (short)(frame->y1 + 2), 0x50000000);
	color.alpha = 0.9f;
	for (index = 0; index < count; index++)
	{
		spectator_draw_text(x, (short)(y + index * row_height), (short)(frame->x1 + 4), 0, SPECTATOR_LIST_SCALE, &color,
			rows[index]);
	}
}

/* at the foot of the view: whom the spectator watches, their shields and
health or the time to their respawn, and the camera */
static void spectator_draw_watched(
	struct spectator_view const *view,
	rectangle2d const *bounds)
{
	static wchar_t const *const camera_names[NUMBER_OF_SPECTATOR_CAMERAS] =
	{
		L"FREE CAMERA",
		L"THIRD PERSON",
	};
	static wchar_t const *const team_names[NUMBER_OF_SPECTATOR_TEAMS] =
	{
		L"",
		L"   RED TEAM",
		L"   BLUE TEAM",
	};
	struct player_datum *player = view->camera != _spectator_camera_free && view->watched_player_index != NONE ?
		player_try_and_get(view->watched_player_index) : NULL;
	real_argb_color color = *global_real_argb_white;
	short status_height = spectator_line_height(SPECTATOR_STATUS_SCALE);
	short name_height = spectator_line_height(SPECTATOR_NAME_SCALE);
	short status_y = (short)(bounds->y1 - status_height - 4);
	short bars_y = (short)(status_y - 2 * SPECTATOR_BAR_HEIGHT - 8);
	short name_y = (short)(bars_y - name_height - 2);
	short center = (short)((bounds->x0 + bounds->x1) / 2);
	wchar_t text[96];

	if (name_height <= 0)
		return;
	if (!player)
	{
		usnprintf(text, NUMBEROF(text) - 1, L"%s%s", camera_names[_spectator_camera_free],
			game_engine_has_teams() ? team_names[view->team] : L"");
		text[NUMBEROF(text) - 1] = 0;
		spectator_draw_text(bounds->x0, status_y, bounds->x1, 2, SPECTATOR_STATUS_SCALE, &color, text);
		return;
	}
	spectator_player_color(player, &color);
	usnprintf(text, NUMBEROF(text) - 1, L"%.12s", player->name);
	text[NUMBEROF(text) - 1] = 0;
	spectator_draw_text(bounds->x0, name_y, bounds->x1, 2, SPECTATOR_NAME_SCALE, &color, text);
	color = *global_real_argb_white;
	if (player->unit_index != NONE)
	{
		struct object_datum *object = object_try_and_get(player->unit_index);

		spectator_draw_vitality((short)(center - SPECTATOR_BAR_WIDTH / 2), bars_y, SPECTATOR_BAR_WIDTH,
			SPECTATOR_BAR_HEIGHT, player->unit_index);
		if (object)
		{
			/* (beside the bars, of 100: shields before them, health after) */
			usnprintf(text, NUMBEROF(text) - 1, L"%ld", (long)(object->object.shield_vitality * 100.0f + 0.5f));
			text[NUMBEROF(text) - 1] = 0;
			spectator_draw_text((short)(center - SPECTATOR_BAR_WIDTH / 2 - 46), (short)(bars_y - 2),
				(short)(center - SPECTATOR_BAR_WIDTH / 2 - 6), 1, SPECTATOR_HINT_SCALE, &color, text);
			usnprintf(text, NUMBEROF(text) - 1, L"%ld", (long)(object->object.body_vitality * 100.0f + 0.5f));
			text[NUMBEROF(text) - 1] = 0;
			spectator_draw_text((short)(center + SPECTATOR_BAR_WIDTH / 2 + 6), (short)(bars_y - 2),
				(short)(center + SPECTATOR_BAR_WIDTH / 2 + 46), 0, SPECTATOR_HINT_SCALE, &color, text);
		}
		usnprintf(text, NUMBEROF(text) - 1, L"%s%s", camera_names[view->camera],
			game_engine_has_teams() ? team_names[view->team] : L"");
	}
	else
	{
		long seconds = spectator_respawn_seconds(view->watched_player_index);

		if (seconds == NONE)
			usnprintf(text, NUMBEROF(text) - 1, L"OUT OF THE GAME");
		else if (seconds <= 0)
			usnprintf(text, NUMBEROF(text) - 1, L"RESPAWNING...");
		else
			usnprintf(text, NUMBEROF(text) - 1, L"RESPAWNING IN %ld SECOND%s...", seconds, seconds == 1 ? L"" : L"S");
	}
	text[NUMBEROF(text) - 1] = 0;
	spectator_draw_text(bounds->x0, status_y, bounds->x1, 2, SPECTATOR_STATUS_SCALE, &color, text);
}

/* ---------- public code */

boolean spectator_local_player(
	short local_player_index)
{
	long player_index = local_player_index >= 0 && local_player_index < MAXIMUM_LOCAL_PLAYERS ?
		local_player_get_player_index(local_player_index) : NONE;

	return player_index != NONE && network_spectator_player(player_index);
}

void spectator_handle_input(
	short local_player_index,
	real_euler_angles2d const *facing_delta,
	real_vector2d const *throttle,
	unsigned long unit_control_flags,
	boolean switch_weapon,
	boolean switch_grenade)
{
	struct spectator_view *view = spectator_view_get(local_player_index);
	unsigned long down = 0;

	if (!view)
		return;
	SET_FLAG(down, _spectator_press_next, TEST_FLAG(unit_control_flags, _unit_control_weapon_primary_trigger_bit));
	SET_FLAG(down, _spectator_press_previous, TEST_FLAG(unit_control_flags, _unit_control_throw_grenade_bit));
	SET_FLAG(down, _spectator_press_free, TEST_FLAG(unit_control_flags, _unit_control_action_bit));
	SET_FLAG(down, _spectator_press_team, TEST_FLAG(unit_control_flags, _unit_control_use_equipment_bit));
	SET_FLAG(down, _spectator_press_overlay, switch_weapon);
	SET_FLAG(down, _spectator_press_speed, switch_grenade);
	SET_FLAG(down, _spectator_press_help, TEST_FLAG(unit_control_flags, _unit_control_integrated_light_bit));
	/* (a press once, however long it is held; none carried into the scores
	shown at a game's end) */
	if (!game_engine_showing_postgame())
		view->pressed |= down & ~view->held;
	view->held = down;
	view->turn.yaw += facing_delta->yaw;
	view->turn.pitch += facing_delta->pitch;
	view->throttle = *throttle;
	view->rise = (TEST_FLAG(unit_control_flags, _unit_control_jump_bit) ? 1.0f : 0.0f) -
		(TEST_FLAG(unit_control_flags, _unit_control_crouch_modifier_bit) ? 1.0f : 0.0f);
}

void spectator_camera_update(
	void *camera,
	void *controls,
	void *command_pointer)
{
	struct spectator_camera_control const *control = (struct spectator_camera_control const *)controls;
	struct observer_command *command = (struct observer_command *)command_pointer;
	struct spectator_view *view = spectator_view_get(control->local_player_index);
	long self;
	long unit_index;
	boolean alive;

	if (!view)
		return;
	self = local_player_get_player_index(control->local_player_index);
	if (self == NONE)
		return;
	spectator_view_begin(view, control->local_player_index);
	spectator_view_controls(view, self, control->local_player_index);
	unit_index = spectator_watched_unit(view, &alive);
	if (view->camera == _spectator_camera_free)
		spectator_camera_free(view, command, PIN(control->seconds_elapsed, 0.0f, 0.1f));
	else
		spectator_camera_third_person(view, command, unit_index, alive);
	view->turn.yaw = view->turn.pitch = 0.0f;
}

void spectator_draw(
	short local_player_index)
{
	struct spectator_view const *view = spectator_view_get(local_player_index);
	rectangle2d bounds = render.camera.window_bounds;
	rectangle2d frame;
	unsigned long parts;
	long self;

	if (!spectator_view_current(view) || !spectator_local_player(local_player_index) ||
		cinematic_in_progress() || game_engine_showing_postgame())
	{
		return;
	}
	parts = spectator_overlay_parts[PIN(view->overlay, 0, NUMBER_OF_SPECTATOR_OVERLAYS - 1)];
	if (!parts)
		return;
	self = local_player_get_player_index(local_player_index);
	/* (the foot's within the frame the game keeps its own text in; the
	corners' and the top's near the view's edges) */
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	frame.x0 = SPECTATOR_MARGIN;
	frame.y0 = SPECTATOR_MARGIN;
	frame.x1 = (short)(render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0 - SPECTATOR_MARGIN);
	frame.y1 = (short)(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0 - SPECTATOR_MARGIN);
	if (TEST_FLAG(parts, _spectator_part_players_bit))
		spectator_draw_players(view, self, &frame);
	if (TEST_FLAG(parts, _spectator_part_items_bit))
		spectator_draw_item_timers(&frame);
	if (TEST_FLAG(parts, _spectator_part_scores_bit))
		spectator_draw_scores(self, &frame);
	if (TEST_FLAG(parts, _spectator_part_watched_bit))
		spectator_draw_watched(view, &bounds);
	if (TEST_FLAG(parts, _spectator_part_controls_bit) && !spectator_help_hidden[local_player_index])
		spectator_draw_controls(view, &frame);
}
