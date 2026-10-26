/*
PHOTO_MODE.C

Photo mode, opened from a single-player campaign's pause menu (its PHOTO
MODE: menu_tags.c's pause_campaign_patch, menu_functions.c's "port photo
mode"). The game stays paused while a camera of photo mode's own goes about
the level; the player's biped holds a pose taken from its own animations; a
filter colours the view; and FIRE saves the view as a PNG, at the size the
game draws the window in pixels, in the photos folder
(platform_photos_root: port/linux/README.md's Photo mode).

The controls, each the player's own binding of it:

	move, look       the camera: the orbit camera circles the player (move
	                 nearer and further, and round), the free camera flies
	jump, crouch     the camera up and down
	fire             take a photo
	action, grenade  the next pose, the one before
	switch weapon    the next filter
	switch grenade   the filter before
	melee            the orbit camera or the free camera
	zoom             the field of view
	flashlight       the controls' names shown or not
	start            back to the pause menu (photo mode ends)

The camera is one of the director's, as a spectator's is (spectator.c):
neither first nor third person, so the HUD and the first-person weapon are
not drawn and the player's biped is (director.c, interface.c); it stays
within PHOTO_MODE_RANGE of the player. Render interpolation stands aside
(render_interpolation.c): with the game paused there is nothing to blend,
and the camera and the pose are drawn as they are.

A pose is a frame of one of the biped's animations, found by name for the
weapon it holds ("stand rifle idle", "stand rifle ar melee": its graph's
seat, weapon class and weapon type labels); a pose the graph has not got is
not offered. The biped's own animation state is put back when photo mode
ends, so the game goes on as it was paused.

A filter is a colour matrix and a vignette, drawn by the renderer over the
3D view before anything is drawn over it (halo_screen_photo_filter:
d3d8_gl.c, d3d8_metal.c) with photo_mode_filter's parameters. A photo is the
back buffer, read when the frame is presented (photo_mode_capture), with
nothing drawn over the view that frame (photo_mode_capturing), written as a
PNG with the game's own zlib.

debug.test_input "photo" plays photo mode through the controller, the
pause menu's PHOTO MODE first, saving what it sees (photo_mode_test_update).
*/

#include "cseries.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "sound/sound_manager.h"
#include "tag_files/tag_files.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "memory/zlib/zlib.h"

#include "photo_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (the photos root is a path on this machine's disk, not an Xbox path:
the C library's own, not the game's, port/linux/include/stdio.h) */
#undef fopen
#undef remove

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
char const *config_string(char const *name);
char const *platform_photos_root(void);
unsigned long system_milliseconds(void);
#ifdef HALO_WEB
/* the page offers the photo as a download (port/web/site/app.js) */
void web_js_post(int kind, char const *text);
#endif
/* xinput_sdl.c's: a controller the automated tests hold */
void test_input_pad(unsigned long digital, unsigned long analog, long left_x, long left_y, long right_x,
	long right_y);

/* ---------- constants */

enum
{
	_photo_camera_orbit,
	_photo_camera_free,
	NUMBER_OF_PHOTO_CAMERAS
};

/* the controls' presses */
enum
{
	_photo_press_photo,
	_photo_press_next_pose,
	_photo_press_previous_pose,
	_photo_press_next_filter,
	_photo_press_previous_filter,
	_photo_press_camera,
	_photo_press_field_of_view,
	_photo_press_help,
	NUMBER_OF_PHOTO_PRESSES
};

enum
{
	_photo_filter_none,
	_photo_filter_black_and_white,
	_photo_filter_noir,
	_photo_filter_sepia,
	_photo_filter_vintage,
	_photo_filter_vivid,
	_photo_filter_warm,
	_photo_filter_cool,
	_photo_filter_night_vision,
	NUMBER_OF_PHOTO_FILTERS
};

enum
{
	/* the poses a biped may have: those of photo_poses its graph has, and
	its own (the first) */
	MAXIMUM_PHOTO_POSES = 24,
	NUMBER_OF_PHOTO_FIELDS_OF_VIEW = 5,
	/* how long a photo's message is shown (milliseconds) */
	PHOTO_MESSAGE_TIME = 4000,
	/* frames a photo waits to be presented before it is given up (a
	renderer that presents nothing) */
	PHOTO_CAPTURE_FRAMES = 8,
};

/* how far the camera goes from the player (world units, 3 m each: the
player stands 0.7 high) */
#define PHOTO_MODE_RANGE 12.0f
/* the orbit camera's distances, and where it starts */
#define PHOTO_ORBIT_MINIMUM 0.25f
#define PHOTO_ORBIT_MAXIMUM 4.0f
#define PHOTO_ORBIT_START 0.9f
/* how far the orbit camera's point of focus rises or falls from the
player's centre: no lower than their feet, where it would look from under
the ground (the observer keeps the camera on the focus's side of it) */
#define PHOTO_ORBIT_HIGHEST 0.6f
#define PHOTO_ORBIT_LOWEST -0.25f
/* the free camera's speed and the orbit camera's, world units a second */
#define PHOTO_FREE_SPEED 1.5f
#define PHOTO_ORBIT_SPEED 1.0f
/* how far the camera keeps from the level's surfaces */
#define PHOTO_SURFACE_CLEARANCE 0.05f
/* how far up and down the camera looks (radians) */
#define PHOTO_PITCH_LIMIT 1.5f

/* what is shown, sized for the game's 480 lines */
#define PHOTO_STATUS_SCALE 0.75f
#define PHOTO_HINT_SCALE 0.6f

/* ---------- structures */

/* director.c's camera_control, as much of it as every camera reads */
struct photo_camera_control
{
	short local_player_index;
	boolean active;
	byte pad3;
	real seconds_elapsed;
};

/* a pose: a frame of the animation named for the seat ("stand", "crouch"),
the weapon class the biped holds, its weapon type (typed: tried first) and
the action, its frame a fraction of the animation; and over it, if not
NULL, a frame of an action's replacement or overlay animation (a melee
attack, a reload, a shot), named so */
struct photo_pose
{
	wchar_t const *name;
	char const *seat;
	char const *action;
	boolean typed;
	real frame;
	char const *over;
	real over_frame;
};

struct photo_filter
{
	wchar_t const *name;
	/* 0 grey, 1 the colours as they are, more stronger */
	real saturation;
	/* sepia's toning, 0 to 1 */
	real sepia;
	/* each colour multiplied */
	real tint[3];
	/* the darks lifted (a faded print) */
	real fade;
	real contrast;
	real vignette;
};

/* ---------- globals */

static struct photo_pose const photo_poses[] =
{
	{ L"STANDING", "stand", "idle", FALSE, 0.0f, NULL, 0.0f },
	{ L"READY", "stand", "idle", FALSE, 0.0f, "ready", 0.6f },
	{ L"CROUCHING", "crouch", "idle", FALSE, 0.0f, NULL, 0.0f },
	{ L"WALKING", "stand", "move-front", FALSE, 0.25f, NULL, 0.0f },
	{ L"STRAFING", "stand", "move-left", FALSE, 0.25f, NULL, 0.0f },
	{ L"CREEPING", "crouch", "move-front", FALSE, 0.25f, NULL, 0.0f },
	{ L"JUMPING", "stand", "airborne", FALSE, 0.5f, NULL, 0.0f },
	{ L"LANDING", "stand", "land-hard", FALSE, 0.3f, NULL, 0.0f },
	{ L"FIRING", "stand", "idle", FALSE, 0.0f, "fire-1", 0.5f },
	{ L"MELEE", "stand", "idle", FALSE, 0.0f, "melee", 0.45f },
	{ L"RELOADING", "stand", "idle", FALSE, 0.0f, "reload-1", 0.4f },
	{ L"THROWING A GRENADE", "stand", "throw-grenade", FALSE, 0.45f, NULL, 0.0f },
	{ L"TAKING A HIT", "stand", "stunned-front", FALSE, 0.4f, NULL, 0.0f },
	{ L"DIVING", "stand", "dive-front", FALSE, 0.5f, NULL, 0.0f },
	{ L"SIGNALING", "stand", "signal-move", FALSE, 0.5f, NULL, 0.0f },
	{ L"CELEBRATING", "stand", "celebrate", FALSE, 0.5f, NULL, 0.0f },
};

static struct photo_filter const photo_filters[NUMBER_OF_PHOTO_FILTERS] =
{
	{ L"NONE", 1.0f, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.0f, 1.0f, 0.0f },
	{ L"BLACK AND WHITE", 0.0f, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.0f, 1.1f, 0.1f },
	{ L"NOIR", 0.0f, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.0f, 1.5f, 0.6f },
	{ L"SEPIA", 1.0f, 1.0f, { 1.0f, 1.0f, 1.0f }, 0.02f, 1.05f, 0.3f },
	{ L"VINTAGE", 0.55f, 0.25f, { 1.06f, 1.0f, 0.86f }, 0.08f, 0.9f, 0.45f },
	{ L"VIVID", 1.6f, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.0f, 1.15f, 0.0f },
	{ L"WARM", 1.1f, 0.0f, { 1.12f, 1.02f, 0.84f }, 0.0f, 1.0f, 0.15f },
	{ L"COOL", 0.95f, 0.0f, { 0.86f, 1.0f, 1.16f }, 0.0f, 1.0f, 0.15f },
	{ L"NIGHT VISION", 0.0f, 0.0f, { 0.45f, 1.25f, 0.5f }, 0.03f, 1.3f, 0.7f },
};

/* horizontal, degrees */
static short const photo_fields_of_view[NUMBER_OF_PHOTO_FIELDS_OF_VIEW] = { 70, 90, 110, 30, 50 };

static struct
{
	/* PHOTO MODE chosen: begins once the menus have closed */
	boolean requested;
	boolean active;
	short local_player_index;
	long unit_index;
	/* the biped's animation as it was, put back when photo mode ends */
	struct object_animation_state object_animation;
	struct unit_animation unit_animation;
	long animation_graph_index;
	/* the poses offered: the first the biped's own; each other's animation
	and frame */
	short pose_count;
	short pose;
	short pose_names[MAXIMUM_PHOTO_POSES];
	short pose_animations[MAXIMUM_PHOTO_POSES];
	short pose_frames[MAXIMUM_PHOTO_POSES];
	/* each pose's action over it (NONE for none), its frame, and whether it
	is a replacement (else an overlay) */
	short pose_overs[MAXIMUM_PHOTO_POSES];
	short pose_over_frames[MAXIMUM_PHOTO_POSES];
	boolean pose_over_replaces[MAXIMUM_PHOTO_POSES];
	short filter;
	short field_of_view;
	short camera;
	boolean help_hidden;
	/* the free camera's position, and where it looks; the orbit camera's
	angles about the player, distance and height */
	real_point3d position;
	real_euler_angles2d facing;
	real_euler_angles2d orbit;
	real distance;
	real height;
	/* the controls since the camera was last updated: how far it turned,
	its throttle, up less down, and the presses */
	real_euler_angles2d turn;
	real_vector2d throttle;
	real rise;
	unsigned long held;
	unsigned long pressed;
	/* this frame is a photo (frames it has waited); a test's picture, of
	the screen as it is, named so */
	boolean capture;
	short capture_frames;
	char const *capture_name;
	/* what the last photo became */
	wchar_t message[128];
	unsigned long message_time;
} photo;

/* ---------- private code: the pose */

/* the label of one of the graph's blocks' elements (each starts with it),
NULL for none */
static char const *photo_label(
	struct tag_block const *block,
	long index,
	long element_size)
{
	if (index < 0 || index >= block->count)
		return NULL;
	return (char const *)block->address + index * element_size;
}

/* the graph's animation for the seat, the weapon class, its weapon type
(typed: tried first, then without it) and the action; NONE if it has none */
static short photo_animation_find(
	long graph_index,
	char const *seat,
	char const *class_label,
	char const *type_label,
	char const *action,
	boolean typed)
{
	char name[4 * (TAG_STRING_LENGTH + 1)];
	short animation_index = NONE;

	if (typed && type_label && *type_label)
	{
		snprintf(name, sizeof(name), "%s %s %s %s", seat, class_label, type_label, action);
		animation_index = animation_graph_get_animation_by_name(graph_index, name);
	}
	if (animation_index == NONE)
	{
		snprintf(name, sizeof(name), "%s %s %s", seat, class_label, action);
		animation_index = animation_graph_get_animation_by_name(graph_index, name);
	}
	return animation_index;
}

/* the poses the biped's graph has, for the weapon it holds */
static void photo_poses_find(
	struct unit_datum const *unit)
{
	long graph_index = unit->object.animation.animation_graph_index;
	struct animation_graph *graph;
	struct animation_graph_unit_seat const *seat;
	struct animation_graph_weapon_class const *weapon_class;
	char const *class_label;
	char const *type_label = NULL;
	short pose_index;

	photo.pose_count = 1;
	photo.pose = 0;
	photo.pose_names[0] = NONE;
	photo.animation_graph_index = graph_index;
	/* (on foot only: a seat's pose is its vehicle's) */
	if (graph_index == NONE || unit->object.parent_object_index != NONE ||
		!object_try_and_get_and_verify_type(photo.unit_index, _object_mask_biped))
	{
		platform_log("photo mode: no poses (graph %lx, parent %lx)", graph_index, unit->object.parent_object_index);
		return;
	}
	graph = animation_graph_definition_get(graph_index);
	if (unit->unit.animation.seat_index < 0 || unit->unit.animation.seat_index >= graph->unit_seats.count)
	{
		platform_log("photo mode: no poses (seat %d of %ld)", unit->unit.animation.seat_index, graph->unit_seats.count);
		return;
	}
	seat = TAG_BLOCK_GET_ELEMENT(&graph->unit_seats, unit->unit.animation.seat_index,
		struct animation_graph_unit_seat);
	class_label = photo_label(&seat->weapon_classes, unit->unit.animation.weapon_index,
		sizeof(struct animation_graph_weapon_class));
	if (!class_label)
	{
		platform_log("photo mode: no poses (weapon class %d)", unit->unit.animation.weapon_index);
		return;
	}
	weapon_class = TAG_BLOCK_GET_ELEMENT(&seat->weapon_classes, unit->unit.animation.weapon_index,
		struct animation_graph_weapon_class);
	type_label = photo_label(&weapon_class->weapon_types, unit->unit.animation.weapon_type_index,
		sizeof(struct animation_graph_weapon_type));
	for (pose_index = 0; pose_index < (short)NUMBEROF(photo_poses) && photo.pose_count < MAXIMUM_PHOTO_POSES;
		pose_index++)
	{
		struct photo_pose const *pose = &photo_poses[pose_index];
		short animation_index = photo_animation_find(graph_index, pose->seat, class_label, type_label, pose->action,
			pose->typed);
		short over_index = pose->over ?
			photo_animation_find(graph_index, pose->seat, class_label, type_label, pose->over, TRUE) : NONE;
		struct animation const *animation;
		struct animation const *over;

		if (animation_index == NONE || (pose->over && over_index == NONE))
			continue;
		animation = TAG_BLOCK_GET_ELEMENT(&graph->animations, animation_index, struct animation);
		over = over_index != NONE ? TAG_BLOCK_GET_ELEMENT(&graph->animations, over_index, struct animation) : NULL;
		if (animation->type != _animation_base || animation->frame_count <= 0 ||
			(over && (over->type == _animation_base || over->frame_count <= 0)))
		{
			continue;
		}
		photo.pose_names[photo.pose_count] = pose_index;
		photo.pose_animations[photo.pose_count] = animation_index;
		photo.pose_frames[photo.pose_count] = (short)(pose->frame * (real)(animation->frame_count - 1));
		photo.pose_overs[photo.pose_count] = over_index;
		photo.pose_over_frames[photo.pose_count] = over ? (short)(pose->over_frame * (real)(over->frame_count - 1)) : 0;
		photo.pose_over_replaces[photo.pose_count] = over && over->type == _animation_replacement;
		photo.pose_count++;
		platform_log("photo mode: pose %s %s%s%s", pose->seat, pose->action, pose->over ? " with " : "",
			pose->over ? pose->over : "");
	}
	platform_log("photo mode: %d poses for %s%s%s", (int)photo.pose_count - 1, class_label,
		type_label && *type_label ? " " : "", type_label ? type_label : "");
}

/* the biped's own animation, as it was paused */
static void photo_pose_restore(
	void)
{
	struct unit_datum *unit = (struct unit_datum *)object_try_and_get_and_verify_type(photo.unit_index,
		_object_mask_unit);

	if (!unit)
		return;
	unit->object.animation = photo.object_animation;
	unit->unit.animation = photo.unit_animation;
	object_compute_node_matrices_recursive(photo.unit_index);
}

static void photo_pose_set(
	short pose)
{
	photo.pose = pose;
	photo_pose_restore();
	if (pose > 0 && object_try_and_get_and_verify_type(photo.unit_index, _object_mask_unit))
	{
		struct unit_datum *unit = unit_get(photo.unit_index);

		unit_port_play_user_animation(photo.unit_index, photo.animation_graph_index, photo.pose_animations[pose],
			FALSE, photo.pose_frames[pose]);
		/* (the action over it, as the unit plays one: unit_preprocess_node_orientations) */
		unit->unit.animation.action_animation.index = NONE;
		unit->unit.animation.overlay_action_animation.index = NONE;
		unit->unit.animation.soft_ping_animation.index = NONE;
		if (photo.pose_overs[pose] != NONE)
		{
			struct animation_state *state = photo.pose_over_replaces[pose] ?
				&unit->unit.animation.action_animation : &unit->unit.animation.overlay_action_animation;

			state->index = photo.pose_overs[pose];
			state->frame_index = photo.pose_over_frames[pose];
		}
		object_compute_node_matrices_recursive(photo.unit_index);
	}
}

/* ---------- private code: the camera */

/* where the orbit camera looks: the player's centre, raised or lowered */
static void photo_focus(
	real_point3d *focus)
{
	struct object_datum *object = object_try_and_get(photo.unit_index);

	if (!object)
	{
		*focus = photo.position;
		return;
	}
	*focus = object->object.bounding_sphere_center;
	focus->z += photo.height;
}

/* a command that takes the camera there at once */
static void photo_command_begin(
	struct observer_command *command)
{
	short parameter;

	command->flags = FLAG(_observer_command_valid_bit);
	command->timer = 0.0f;
	for (parameter = 0; parameter < NUMBER_OF_OBSERVER_COMMAND_PARAMETERS; parameter++)
		command->parameter_flags[parameter] = FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
	command->field_of_view = DEGREES_TO_RADIANS((real)photo_fields_of_view[photo.field_of_view]);
}

/* the camera's point kept within range of the player */
static void photo_keep_in_range(
	real_point3d *point)
{
	real_point3d center;
	real_vector3d away;
	real distance;
	struct object_datum *object = object_try_and_get(photo.unit_index);

	if (!object)
		return;
	center = object->object.bounding_sphere_center;
	vector_from_points3d(&center, point, &away);
	distance = magnitude3d(&away);
	if (distance > PHOTO_MODE_RANGE)
	{
		point->x = center.x + away.i * PHOTO_MODE_RANGE / distance;
		point->y = center.y + away.j * PHOTO_MODE_RANGE / distance;
		point->z = center.z + away.k * PHOTO_MODE_RANGE / distance;
	}
}

/* how far along the move from the point the camera may go before the
level's surfaces (the ground, walls: not objects), short of them, 0 to 1 */
static real photo_clear_fraction(
	real_point3d const *from,
	real_vector3d const *move)
{
	struct collision_result collision;
	real length = magnitude3d(move);

	if (length < 0.0001f ||
		!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
			from, move, NONE, &collision))
	{
		return 1.0f;
	}
	return PIN(collision.t - PHOTO_SURFACE_CLEARANCE / length, 0.0f, 1.0f);
}

/* the free camera from where the view is now, or the orbit camera about the
player from there */
static void photo_camera_switch(
	void)
{
	struct observer_result const *observer = observer_get_camera(photo.local_player_index);
	real_point3d focus;
	real_vector3d toward;

	if (photo.camera == _photo_camera_orbit)
	{
		photo.camera = _photo_camera_free;
		photo.position = observer->position;
		if (magnitude_squared3d(&observer->forward) > 0.5f)
			euler_angles2d_from_vector3d(&photo.facing, &observer->forward);
		return;
	}
	photo.camera = _photo_camera_orbit;
	photo.height = 0.0f;
	photo_focus(&focus);
	vector_from_points3d(&observer->position, &focus, &toward);
	photo.distance = PIN(magnitude3d(&toward), PHOTO_ORBIT_MINIMUM, PHOTO_ORBIT_MAXIMUM);
	if (magnitude_squared3d(&toward) > 0.0001f)
	{
		normalize3d(&toward);
		euler_angles2d_from_vector3d(&photo.orbit, &toward);
	}
	photo.orbit.pitch = PIN(photo.orbit.pitch, -PHOTO_PITCH_LIMIT, PHOTO_PITCH_LIMIT);
}

static void photo_camera_free(
	struct observer_command *command,
	real seconds)
{
	real_vector3d forward;
	real_vector3d left;
	real speed = PHOTO_FREE_SPEED * seconds;
	real flat;

	photo.facing.yaw += photo.turn.yaw;
	photo.facing.pitch = PIN(photo.facing.pitch + photo.turn.pitch, -PHOTO_PITCH_LIMIT, PHOTO_PITCH_LIMIT);
	vector3d_from_euler_angles2d(&forward, &photo.facing);
	/* (it flies where it looks, sideways level) */
	flat = square_root(forward.i * forward.i + forward.j * forward.j);
	left.i = flat > 0.0001f ? -forward.j / flat : 0.0f;
	left.j = flat > 0.0001f ? forward.i / flat : 1.0f;
	left.k = 0.0f;
	{
		real_point3d moved = photo.position;
		real_vector3d move;
		real fraction;

		moved.x += (forward.i * photo.throttle.i + left.i * photo.throttle.j) * speed;
		moved.y += (forward.j * photo.throttle.i + left.j * photo.throttle.j) * speed;
		moved.z += (forward.k * photo.throttle.i + photo.rise) * speed;
		photo_keep_in_range(&moved);
		/* (not through the ground or a wall) */
		vector_from_points3d(&photo.position, &moved, &move);
		fraction = photo_clear_fraction(&photo.position, &move);
		photo.position.x += move.i * fraction;
		photo.position.y += move.j * fraction;
		photo.position.z += move.k * fraction;
	}

	photo_command_begin(command);
	command->focus_position = photo.position;
	command->focus_distance = 0.0f;
	command->forward = forward;
	observer_up_from_forward(&command->forward, &command->up);
}

/* about the player: look turns the camera round them, move brings it
nearer or further and round them, up and down move what it looks at (the
observer keeps it out of the walls) */
static void photo_camera_orbit(
	struct observer_command *command,
	real seconds)
{
	real_point3d focus;

	photo.orbit.yaw += photo.turn.yaw - photo.throttle.j * seconds;
	photo.orbit.pitch = PIN(photo.orbit.pitch + photo.turn.pitch, -PHOTO_PITCH_LIMIT, PHOTO_PITCH_LIMIT);
	photo.distance = PIN(photo.distance - photo.throttle.i * PHOTO_ORBIT_SPEED * seconds, PHOTO_ORBIT_MINIMUM,
		PHOTO_ORBIT_MAXIMUM);
	photo.height = PIN(photo.height + photo.rise * PHOTO_ORBIT_SPEED * seconds, PHOTO_ORBIT_LOWEST,
		PHOTO_ORBIT_HIGHEST);
	photo_focus(&focus);

	photo_command_begin(command);
	command->focus_position = focus;
	vector3d_from_euler_angles2d(&command->forward, &photo.orbit);
	observer_up_from_forward(&command->forward, &command->up);
	/* (in front of the ground or a wall behind it, not in it) */
	{
		real_vector3d back;

		back.i = -command->forward.i * photo.distance;
		back.j = -command->forward.j * photo.distance;
		back.k = -command->forward.k * photo.distance;
		command->focus_distance = MAX(photo.distance * photo_clear_fraction(&focus, &back), 0.05f);
	}
}

/* ---------- private code: what the controls do */

static void photo_message(
	wchar_t const *format,
	char const *text)
{
	wchar_t wide[96];
	short index;

	for (index = 0; text[index] && index < (short)NUMBEROF(wide) - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
	usnprintf(photo.message, NUMBEROF(photo.message) - 1, format, wide);
	photo.message[NUMBEROF(photo.message) - 1] = 0;
	photo.message_time = system_milliseconds();
}

static void photo_controls(
	void)
{
	unsigned long pressed = photo.pressed;

	photo.pressed = 0;
	if (TEST_FLAG(pressed, _photo_press_next_pose) && photo.pose_count > 1)
		photo_pose_set((short)((photo.pose + 1) % photo.pose_count));
	if (TEST_FLAG(pressed, _photo_press_previous_pose) && photo.pose_count > 1)
		photo_pose_set((short)((photo.pose + photo.pose_count - 1) % photo.pose_count));
	if (TEST_FLAG(pressed, _photo_press_next_filter))
		photo.filter = (short)((photo.filter + 1) % NUMBER_OF_PHOTO_FILTERS);
	if (TEST_FLAG(pressed, _photo_press_previous_filter))
		photo.filter = (short)((photo.filter + NUMBER_OF_PHOTO_FILTERS - 1) % NUMBER_OF_PHOTO_FILTERS);
	if (TEST_FLAG(pressed, _photo_press_camera))
		photo_camera_switch();
	if (TEST_FLAG(pressed, _photo_press_field_of_view))
		photo.field_of_view = (short)((photo.field_of_view + 1) % NUMBER_OF_PHOTO_FIELDS_OF_VIEW);
	if (TEST_FLAG(pressed, _photo_press_help))
		photo.help_hidden = !photo.help_hidden;
	if (TEST_FLAG(pressed, _photo_press_photo) && !photo.capture)
	{
		photo.capture = TRUE;
		photo.capture_frames = 0;
		photo.capture_name = NULL;
	}
}

/* ---------- private code: beginning and ending */

/* a single-player campaign, its player alive and the game not in a
cinematic: where photo mode may begin */
static long photo_mode_unit(
	short local_player_index)
{
	long unit_index;

	if (!game_in_progress() || game_connection() != _game_connection_local || network_game_is_active() ||
		game_is_cooperative() || game_engine_running() || cinematic_in_progress() ||
		local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		local_player_get_player_index(local_player_index) == NONE)
	{
		return NONE;
	}
	unit_index = player_control_get_unit_index(local_player_index);
	if (unit_index == NONE || !object_try_and_get_and_verify_type(unit_index, _object_mask_unit))
		return NONE;
	return unit_index;
}

static void photo_mode_begin(
	void)
{
	struct unit_datum *unit;
	real_euler_angles2d facing;

	photo.unit_index = photo_mode_unit(photo.local_player_index);
	if (photo.unit_index == NONE)
		return;
	unit = unit_get(photo.unit_index);
	photo.object_animation = unit->object.animation;
	photo.unit_animation = unit->unit.animation;
	photo_poses_find(unit);
	photo.active = TRUE;
	photo.held = 0;
	photo.pressed = 0;
	photo.turn.yaw = photo.turn.pitch = 0.0f;
	photo.throttle.i = photo.throttle.j = 0.0f;
	photo.rise = 0.0f;
	photo.capture = FALSE;
	photo.message[0] = 0;
	/* (the orbit camera, in front of the player and to one side, a little
	above, looking back at them) */
	euler_angles2d_from_vector3d(&facing, &unit->object.forward);
	photo.camera = _photo_camera_orbit;
	photo.orbit.yaw = facing.yaw + _pi - 0.6f;
	photo.orbit.pitch = -0.12f;
	photo.distance = PHOTO_ORBIT_START;
	photo.height = 0.0f;
	photo.position = unit->object.bounding_sphere_center;
	photo.facing = facing;
	game_time_set_paused(TRUE);
	sound_pause(TRUE);
	platform_log("photo mode: begun");
}

/* photo mode ends: the biped's pose put back, and the game resumed, unless
a menu has opened (the pause menu, from START), which pauses it itself */
static void photo_mode_end(
	void)
{
	photo_pose_restore();
	photo.active = FALSE;
	photo.capture = FALSE;
	if (!ui_widgets_active())
	{
		game_time_set_paused(FALSE);
		sound_pause(FALSE);
	}
	platform_log("photo mode: ended");
}

/* ---------- private code: what is shown */

/* a line of text from (x0, y0) as wide as x1, at a scale, centered (as
spectator.c's) */
static void photo_draw_text(
	short x0,
	short y0,
	short x1,
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
	bounds.x0 = x0;
	bounds.y0 = y0;
	bounds.x1 = (short)(x0 + (x1 - x0) / scale);
	bounds.y1 = (short)(y0 + 2 * (font->ascending_height + font->descending_height + font->leading_height));
	rasterizer_text_set_scale(scale, (real)x0, (real)y0);
	draw_string_set_draw_mode(font_index, NONE, 2, 0, color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static short photo_line_height(
	real scale)
{
	long font_index = hud_get_font_index();
	struct font_header *font;

	if (font_index == NONE)
		return 0;
	font = font_definition_get(font_index);
	return (short)((font->ascending_height + font->descending_height + font->leading_height) * scale);
}

static void photo_draw_band(
	short x0,
	short y0,
	short x1,
	short y1)
{
	rectangle2d rectangle;

	rectangle.x0 = x0;
	rectangle.y0 = y0;
	rectangle.x1 = x1;
	rectangle.y1 = y1;
	draw_quad(&rectangle, 0x90000000);
}

/* ---------- private code: the photo's file */

static unsigned long photo_be32(
	unsigned char *bytes,
	unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
	return value;
}

/* a PNG chunk: its length, type, data and CRC */
static boolean photo_png_chunk(
	FILE *file,
	char const *type,
	unsigned char const *data,
	unsigned long size)
{
	unsigned char bytes[4];
	unsigned long crc = crc32(0L, (Bytef const *)type, 4);

	if (size)
		crc = crc32(crc, (Bytef const *)data, (uInt)size);
	photo_be32(bytes, size);
	if (fwrite(bytes, 1, 4, file) != 4 || fwrite(type, 1, 4, file) != 4 ||
		(size && fwrite(data, 1, size, file) != size))
	{
		return FALSE;
	}
	photo_be32(bytes, crc);
	return fwrite(bytes, 1, 4, file) == 4;
}

static int photo_paeth(
	int left,
	int up,
	int up_left)
{
	int estimate = left + up - up_left;
	int to_left = estimate > left ? estimate - left : left - estimate;
	int to_up = estimate > up ? estimate - up : up - estimate;
	int to_up_left = estimate > up_left ? estimate - up_left : up_left - estimate;

	if (to_left <= to_up && to_left <= to_up_left)
		return left;
	return to_up <= to_up_left ? up : up_left;
}

/* the pixels as an 8-bit RGB PNG, each row Paeth-filtered */
static boolean photo_png_write(
	char const *path,
	unsigned char const *pixels,
	long width,
	long height,
	long pitch,
	int bgra)
{
	unsigned long row_size = (unsigned long)width * 3 + 1;
	unsigned long raw_size = row_size * (unsigned long)height;
	uLongf packed_size = (uLongf)(raw_size + raw_size / 100 + 1024);
	unsigned char *raw = malloc(raw_size);
	unsigned char *packed = malloc(packed_size);
	unsigned char *previous = NULL;
	unsigned char header[13];
	static unsigned char const signature[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
	boolean written = FALSE;
	FILE *file = NULL;
	long row, column;

	if (!raw || !packed)
	{
		platform_log("photo mode: no memory for a photo of %ldx%ld", width, height);
		goto done;
	}
	for (row = 0; row < height; row++)
	{
		unsigned char const *source = pixels + row * pitch;
		unsigned char *line = raw + row * row_size + 1;
		unsigned char *filtered = raw + row * row_size;

		/* (the row as RGB, then filtered in place from its end, where each
		byte's left and upper neighbours are still unfiltered) */
		for (column = 0; column < width; column++)
		{
			line[column * 3 + 0] = source[column * 4 + (bgra ? 2 : 0)];
			line[column * 3 + 1] = source[column * 4 + 1];
			line[column * 3 + 2] = source[column * 4 + (bgra ? 0 : 2)];
		}
		filtered[0] = 4;
		for (column = width * 3 - 1; column >= 0; column--)
		{
			int left = column >= 3 ? line[column - 3] : 0;
			int up = previous ? previous[column] : 0;
			int up_left = previous && column >= 3 ? previous[column - 3] : 0;

			line[column] = (unsigned char)(line[column] - photo_paeth(left, up, up_left));
		}
		/* (the unfiltered row above: kept aside before this one was
		filtered over it) */
		if (row + 1 < height)
		{
			if (!previous)
				previous = malloc((size_t)width * 3);
			if (!previous)
				goto done;
			for (column = 0; column < width; column++)
			{
				previous[column * 3 + 0] = source[column * 4 + (bgra ? 2 : 0)];
				previous[column * 3 + 1] = source[column * 4 + 1];
				previous[column * 3 + 2] = source[column * 4 + (bgra ? 0 : 2)];
			}
		}
	}
	{
		int result = compress2(packed, &packed_size, raw, (uLong)raw_size, Z_DEFAULT_COMPRESSION);

		if (result != Z_OK)
		{
			platform_log("photo mode: zlib could not compress the photo (%d)", result);
			goto done;
		}
	}
	file = fopen(path, "wb");
	if (!file)
	{
		platform_log("photo mode: cannot open %s", path);
		goto done;
	}
	photo_be32(header, (unsigned long)width);
	photo_be32(header + 4, (unsigned long)height);
	header[8] = 8; /* bits a channel */
	header[9] = 2; /* RGB */
	header[10] = header[11] = header[12] = 0;
	written = fwrite(signature, 1, sizeof(signature), file) == sizeof(signature) &&
		photo_png_chunk(file, "IHDR", header, sizeof(header)) &&
		photo_png_chunk(file, "IDAT", packed, (unsigned long)packed_size) &&
		photo_png_chunk(file, "IEND", NULL, 0);
	written = (fclose(file) == 0) && written;
	if (!written)
		remove(path);

done:
	free(raw);
	free(packed);
	free(previous);
	return written;
}

/* the photo's name: the level's and the time's, numbered after the first
of the same second (a test's picture by its own name) */
static boolean photo_path(
	char *path,
	size_t size,
	char const **name)
{
	char const *root = platform_photos_root();
	char const *level = global_scenario_index == NONE ? "" : tag_name_strip_path(tag_get_name(global_scenario_index));
	char stamp[32];
	time_t now = time(NULL);
	struct tm const *local = localtime(&now);
	short collision;

	if (!root || !*root)
		return FALSE;
	if (photo.capture_name)
	{
		snprintf(path, size, "%s/%s.png", root, photo.capture_name);
		*name = strrchr(path, '/') + 1;
		return TRUE;
	}
	if (local)
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", local);
	else
		snprintf(stamp, sizeof(stamp), "%lu", (unsigned long)now);
	for (collision = 0; collision < 100; collision++)
	{
		FILE *existing;

		if (collision)
			snprintf(path, size, "%s/CoreCE %s %s (%d).png", root, level, stamp, collision + 1);
		else
			snprintf(path, size, "%s/CoreCE %s %s.png", root, level, stamp);
		existing = fopen(path, "rb");
		if (!existing)
			break;
		fclose(existing);
	}
	*name = strrchr(path, '/') + 1;
	return collision < 100;
}

/* ---------- private code: the automated test (debug.test_input "photo") */

/* XINPUT's buttons, as xinput_sdl.c's test_input_pad takes them: the
digital buttons' bits, and a bit for each analog button by its index */
enum
{
	PHOTO_TEST_DPAD_DOWN = 0x0002,
	PHOTO_TEST_START = 0x0010,
	PHOTO_TEST_LEFT_THUMB = 0x0040,
	PHOTO_TEST_RIGHT_THUMB = 0x0080,

	PHOTO_TEST_A = 1 << 0,
	PHOTO_TEST_B = 1 << 1,
	PHOTO_TEST_X = 1 << 2,
	PHOTO_TEST_Y = 1 << 3,
	PHOTO_TEST_BLACK = 1 << 4,
	PHOTO_TEST_WHITE = 1 << 5,
	PHOTO_TEST_LEFT_TRIGGER = 1 << 6,
	PHOTO_TEST_RIGHT_TRIGGER = 1 << 7,
};

/* a step: what is held, for how long (milliseconds), and the picture of the
screen as it is taken at its end, by its name ("" for none) */
struct photo_test_step
{
	unsigned long digital;
	unsigned long analog;
	short left_x, left_y, right_x, right_y;
	unsigned long time;
	char const *picture;
};

#define PHOTO_TEST_HOLD(digital, analog, left_x, left_y, right_x, right_y, time) \
	{ digital, analog, left_x, left_y, right_x, right_y, time, "" }
#define PHOTO_TEST_WAIT(time) PHOTO_TEST_HOLD(0, 0, 0, 0, 0, 0, time)
#define PHOTO_TEST_PRESS(digital, analog) PHOTO_TEST_HOLD(digital, analog, 0, 0, 0, 0, 120), PHOTO_TEST_WAIT(150)
#define PHOTO_TEST_PICTURE(name) { 0, 0, 0, 0, 0, 0, 700, name }
/* (FIRE: a photo) */
#define PHOTO_TEST_PHOTO PHOTO_TEST_PRESS(0, PHOTO_TEST_RIGHT_TRIGGER), PHOTO_TEST_WAIT(500)

/* out of a30's lifeboat onto the grass; the pause menu, PHOTO MODE chosen;
poses, filters, the orbit camera turned and lowered, the field of view
widened, the free camera flown back, a photo of each; the controls put
away, and back to the pause menu (START) and the game (its RESUME GAME).
The poses are a30's player's, with the assault rifle: the first, as paused,
then STANDING, READY, CROUCHING, WALKING, STRAFING, CREEPING, JUMPING,
LANDING, FIRING, MELEE, RELOADING, THROWING A GRENADE, TAKING A HIT. */
static struct photo_test_step const photo_test_steps[] =
{
	PHOTO_TEST_HOLD(0, 0, 0, 32000, 0, 0, 6000),
	PHOTO_TEST_WAIT(1000),
	PHOTO_TEST_PRESS(PHOTO_TEST_START, 0),
	PHOTO_TEST_WAIT(1200),
	PHOTO_TEST_PRESS(PHOTO_TEST_DPAD_DOWN, 0),
	PHOTO_TEST_PICTURE("test 01 pause menu"),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_A),
	PHOTO_TEST_WAIT(800),
	PHOTO_TEST_PICTURE("test 02 photo mode"),
	PHOTO_TEST_PHOTO,
	/* MELEE */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_HOLD(0, 0, 0, 0, 32000, 0, 250),
	PHOTO_TEST_PICTURE("test 03 melee"),
	PHOTO_TEST_PHOTO,
	/* THROWING A GRENADE, sepia, from the side */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y),
	PHOTO_TEST_HOLD(0, 0, 0, 0, 32000, 0, 450),
	PHOTO_TEST_PICTURE("test 04 sepia"),
	PHOTO_TEST_PHOTO,
	/* JUMPING, noir, from below */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_BLACK),
	PHOTO_TEST_HOLD(0, 0, 0, 0, -32000, 0, 700),
	PHOTO_TEST_HOLD(PHOTO_TEST_LEFT_THUMB, 0, 0, 0, 0, 0, 300),
	PHOTO_TEST_HOLD(0, 0, 0, 0, 0, 32000, 250),
	PHOTO_TEST_PICTURE("test 05 noir"),
	PHOTO_TEST_PHOTO,
	/* RELOADING, vivid, wider, from the other side */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X), PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_X),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y),
	PHOTO_TEST_PRESS(PHOTO_TEST_RIGHT_THUMB, 0),
	PHOTO_TEST_HOLD(0, PHOTO_TEST_A, 0, 0, 0, -32000, 400),
	PHOTO_TEST_HOLD(0, 0, 0, 0, -32000, 0, 600),
	PHOTO_TEST_PICTURE("test 06 vivid"),
	PHOTO_TEST_PHOTO,
	/* CROUCHING, vintage */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER), PHOTO_TEST_PRESS(0, PHOTO_TEST_LEFT_TRIGGER),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_BLACK),
	PHOTO_TEST_HOLD(0, 0, 0, 32000, 32000, 0, 500),
	PHOTO_TEST_PICTURE("test 07 vintage"),
	PHOTO_TEST_PHOTO,
	/* the free camera, flown back and up, night vision */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_B),
	PHOTO_TEST_HOLD(0, PHOTO_TEST_A, 0, -20000, 0, 0, 900),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y), PHOTO_TEST_PRESS(0, PHOTO_TEST_Y),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_Y),
	PHOTO_TEST_PICTURE("test 08 night vision"),
	PHOTO_TEST_PHOTO,
	/* the controls put away; START: the pause menu again, then RESUME */
	PHOTO_TEST_PRESS(0, PHOTO_TEST_WHITE),
	PHOTO_TEST_PICTURE("test 09 controls hidden"),
	PHOTO_TEST_PRESS(PHOTO_TEST_START, 0),
	PHOTO_TEST_WAIT(1000),
	PHOTO_TEST_PICTURE("test 10 pause menu again"),
	PHOTO_TEST_PRESS(0, PHOTO_TEST_A),
	PHOTO_TEST_WAIT(1500),
	PHOTO_TEST_PICTURE("test 11 resumed"),
};

static struct
{
	boolean checked;
	boolean enabled;
	boolean started;
	boolean finished;
	/* a picture is being taken */
	boolean waiting;
	short step;
	unsigned long step_time;
} photo_test;

/* the test's script, a step at a time once the level has played three
seconds; the picture a step takes, at its end */
static void photo_mode_test_update(
	void)
{
	struct photo_test_step const *step;
	unsigned long now = system_milliseconds();

	if (!photo_test.checked)
	{
		photo_test.checked = TRUE;
		photo_test.enabled = !strcmp(config_string("debug.test_input"), "photo");
	}
	if (!photo_test.enabled || photo_test.finished)
		return;
	if (!photo_test.started)
	{
		long unit_index = game_in_progress() ? photo_mode_unit(local_player_get_next(NONE)) : NONE;

		/* (the player on foot, out of the level's opening cinematic) */
		if (unit_index == NONE || game_time_get_paused() || game_time_get() < 3 * TICKS_PER_SECOND ||
			object_get(unit_index)->object.parent_object_index != NONE)
		{
			return;
		}
		photo_test.started = TRUE;
		photo_test.step = 0;
		photo_test.step_time = now;
		platform_log("photo mode test: started");
	}
	/* (the next step timed from the frame after the picture is written,
	which takes a while) */
	if (photo.capture)
	{
		photo_test.waiting = TRUE;
		return;
	}
	if (photo_test.waiting)
	{
		photo_test.waiting = FALSE;
		photo_test.step_time = now;
	}
	step = &photo_test_steps[photo_test.step];
	if (now - photo_test.step_time < step->time)
	{
		test_input_pad(step->digital, step->analog, step->left_x, step->left_y, step->right_x, step->right_y);
		return;
	}
	test_input_pad(0, 0, 0, 0, 0, 0);
	photo_test.waiting = *step->picture != 0;
	if (*step->picture)
	{
		photo.capture = TRUE;
		photo.capture_frames = 0;
		photo.capture_name = step->picture;
	}
	photo_test.step++;
	photo_test.step_time = now;
	if (photo_test.step >= (short)NUMBEROF(photo_test_steps))
	{
		photo_test.finished = TRUE;
		platform_log("photo mode test: finished");
	}
}

/* ---------- public code */

boolean photo_mode_active(
	void)
{
	return photo.active;
}

boolean photo_mode_request(
	short local_player_index)
{
	if (photo.active || photo_mode_unit(local_player_index) == NONE)
		return FALSE;
	photo.requested = TRUE;
	photo.local_player_index = local_player_index;
	return TRUE;
}

void photo_mode_update(
	void)
{
	photo_mode_test_update();
	if (photo.capture && ++photo.capture_frames > PHOTO_CAPTURE_FRAMES)
	{
		photo.capture = FALSE;
		photo_message(L"THE PHOTO COULD NOT BE TAKEN%s", "");
	}
	if (photo.requested)
	{
		if (ui_widgets_active())
			return;
		photo.requested = FALSE;
		photo_mode_begin();
	}
	if (!photo.active)
		return;
	/* (START opened the pause menu; the level ended, the player died or
	a cinematic began) */
	if (ui_widgets_active() || photo_mode_unit(photo.local_player_index) != photo.unit_index)
	{
		photo_mode_end();
		return;
	}
	if (!game_time_get_paused())
		game_time_set_paused(TRUE);
	if (photo.held || photo.pressed)
		photo_controls();
}

void photo_mode_handle_input(
	short local_player_index,
	real_euler_angles2d const *facing_delta,
	real_vector2d const *throttle,
	unsigned long unit_control_flags,
	boolean switch_weapon,
	boolean switch_grenade,
	boolean zoom)
{
	unsigned long down = 0;

	if (!photo.active || local_player_index != photo.local_player_index)
		return;
	SET_FLAG(down, _photo_press_photo, TEST_FLAG(unit_control_flags, _unit_control_weapon_primary_trigger_bit));
	SET_FLAG(down, _photo_press_next_pose, TEST_FLAG(unit_control_flags, _unit_control_action_bit));
	SET_FLAG(down, _photo_press_previous_pose, TEST_FLAG(unit_control_flags, _unit_control_throw_grenade_bit));
	SET_FLAG(down, _photo_press_next_filter, switch_weapon);
	SET_FLAG(down, _photo_press_previous_filter, switch_grenade);
	SET_FLAG(down, _photo_press_camera, TEST_FLAG(unit_control_flags, _unit_control_use_equipment_bit));
	SET_FLAG(down, _photo_press_field_of_view, zoom);
	SET_FLAG(down, _photo_press_help, TEST_FLAG(unit_control_flags, _unit_control_integrated_light_bit));
	/* (a press once, however long it is held) */
	photo.pressed |= down & ~photo.held;
	photo.held = down;
	photo.turn.yaw += facing_delta->yaw;
	photo.turn.pitch += facing_delta->pitch;
	photo.throttle = *throttle;
	photo.rise = (TEST_FLAG(unit_control_flags, _unit_control_jump_bit) ? 1.0f : 0.0f) -
		(TEST_FLAG(unit_control_flags, _unit_control_crouch_modifier_bit) ? 1.0f : 0.0f);
}

void photo_mode_camera_update(
	void *camera,
	void *controls,
	void *command_pointer)
{
	struct photo_camera_control const *control = (struct photo_camera_control const *)controls;
	struct observer_command *command = (struct observer_command *)command_pointer;
	real seconds = PIN(control->seconds_elapsed, 0.0f, 0.1f);

	(void)camera;
	if (!photo.active || control->local_player_index != photo.local_player_index)
		return;
	photo_controls();
	if (photo.camera == _photo_camera_free)
		photo_camera_free(command, seconds);
	else
		photo_camera_orbit(command, seconds);
	photo.turn.yaw = photo.turn.pitch = 0.0f;
}

boolean photo_mode_capturing(
	void)
{
	return photo.capture && !photo.capture_name;
}

void photo_mode_draw(
	void)
{
	/* (the screen's, as the letterbox's bars are laid out) */
	rectangle2d bounds = render.camera.viewport_bounds;
	real_argb_color color = *global_real_argb_white;
	real_argb_color dim = *global_real_argb_white;
	short status_height = photo_line_height(PHOTO_STATUS_SCALE);
	short hint_height = photo_line_height(PHOTO_HINT_SCALE);
	short bottom, top;
	wchar_t text[160];

	if (!photo.active || photo_mode_capturing() || status_height <= 0)
		return;
	dim.alpha = 0.8f;
	bottom = (short)(bounds.y1 - 6);
	top = (short)(bottom - status_height - (photo.help_hidden ? 0 : 2 * hint_height) - 8);
	photo_draw_band(bounds.x0, top, bounds.x1, bounds.y1);
	usnprintf(text, NUMBEROF(text) - 1, L"PHOTO MODE     POSE: %s     FILTER: %s     %s     FOV %d",
		photo.pose ? photo_poses[photo.pose_names[photo.pose]].name : L"AS PAUSED",
		photo_filters[photo.filter].name,
		photo.camera == _photo_camera_orbit ? L"ORBIT CAMERA" : L"FREE CAMERA",
		(int)photo_fields_of_view[photo.field_of_view]);
	text[NUMBEROF(text) - 1] = 0;
	photo_draw_text(bounds.x0, (short)(top + 4), bounds.x1, PHOTO_STATUS_SCALE, &color, text);
	if (!photo.help_hidden)
	{
		short hint_y = (short)(top + 4 + status_height + 2);

		photo_draw_text(bounds.x0, hint_y, bounds.x1, PHOTO_HINT_SCALE, &dim,
			L"FIRE  TAKE PHOTO     ACTION, GRENADE  POSE     SWITCH WEAPON, SWITCH GRENADE  FILTER     "
			L"MELEE  CAMERA");
		photo_draw_text(bounds.x0, (short)(hint_y + hint_height), bounds.x1, PHOTO_HINT_SCALE, &dim,
			L"MOVE, LOOK  CAMERA     JUMP, CROUCH  UP, DOWN     ZOOM  FIELD OF VIEW     FLASHLIGHT  HIDE THIS     "
			L"START  BACK");
	}
	if (photo.message[0] && system_milliseconds() - photo.message_time < PHOTO_MESSAGE_TIME)
	{
		photo_draw_band(bounds.x0, (short)(bounds.y0 + 8), bounds.x1, (short)(bounds.y0 + 14 + status_height));
		photo_draw_text(bounds.x0, (short)(bounds.y0 + 11), bounds.x1, PHOTO_STATUS_SCALE, &color, photo.message);
	}
}

int photo_mode_filter(
	float *parameters)
{
	struct photo_filter const *filter;
	real matrix[3][3];
	real offset;
	short row, column;
	static real const luma[3] = { 0.299f, 0.587f, 0.114f };
	static real const sepia[3][3] =
	{
		{ 0.393f, 0.769f, 0.189f },
		{ 0.349f, 0.686f, 0.168f },
		{ 0.272f, 0.534f, 0.131f },
	};

	if (!photo.active || photo.filter == _photo_filter_none)
		return FALSE;
	filter = &photo_filters[photo.filter];
	/* saturation about the luma, then sepia's toning, the tint, the fade
	and the contrast about the middle grey: one matrix and offset */
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			real saturated = (1.0f - filter->saturation) * luma[column] + (row == column ? filter->saturation : 0.0f);
			real toned = 0.0f;
			short inner;

			for (inner = 0; inner < 3; inner++)
			{
				real saturated_inner = (1.0f - filter->saturation) * luma[column] +
					(inner == column ? filter->saturation : 0.0f);

				toned += sepia[row][inner] * saturated_inner;
			}
			matrix[row][column] = (saturated * (1.0f - filter->sepia) + toned * filter->sepia) * filter->tint[row] *
				(1.0f - filter->fade) * filter->contrast;
		}
	}
	offset = filter->fade * filter->contrast + 0.5f * (1.0f - filter->contrast);
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
			parameters[row * 4 + column] = matrix[row][column];
		parameters[row * 4 + 3] = offset;
	}
	parameters[12] = filter->vignette;
	parameters[13] = parameters[14] = parameters[15] = 0.0f;
	return TRUE;
}

int photo_mode_capture_wanted(
	void)
{
	return photo.capture;
}

void photo_mode_capture(
	unsigned char const *pixels,
	long width,
	long height,
	long pitch,
	int bgra)
{
	char path[1024];
	char const *name = "";
	boolean test_picture = photo.capture_name != NULL;

	photo.capture = FALSE;
	if (!pixels || width <= 0 || height <= 0)
	{
		photo_message(L"THE PHOTO COULD NOT BE TAKEN%s", "");
		return;
	}
	if (!photo_path(path, sizeof(path), &name) || !photo_png_write(path, pixels, width, height, pitch, bgra))
	{
		platform_log("photo mode: could not write %s", path);
		photo_message(L"THE PHOTO COULD NOT BE SAVED IN %s", platform_photos_root());
		return;
	}
	platform_log("photo mode: saved %s (%ldx%ld)", path, width, height);
	if (test_picture)
		return;
	photo_message(L"PHOTO SAVED: %s", name);
#ifdef HALO_WEB
	/* (the page reads it from the browser's storage, which is the data
	root, /data: port/web/src/web_main.c) */
	if (!strncmp(path, "/data/", 6))
		web_js_post(5, path + 6);
#endif
}
