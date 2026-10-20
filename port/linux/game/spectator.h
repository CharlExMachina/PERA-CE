/*
SPECTATOR.H

A multiplayer spectator's view (spectator.c): its cameras, what its
controls do, and what it is shown. director.c, player_control.c and
interface.c call in here, and the code that shows a local player what their
player sees (the view player, below).
*/

#ifndef __SPECTATOR_H
#define __SPECTATOR_H
#pragma once

struct player_datum;

/* whether the local player is a spectator of a network multiplayer game
(network_spectator.c: the host says) */
boolean spectator_local_player(
	short local_player_index);

/* a spectator's input this frame (player_control.c), which moves its camera
and picks whom it watches: how far it turned, its throttle, its unit control
flags, and whether it pressed switch weapon and switch grenade */
void spectator_handle_input(
	short local_player_index,
	real_euler_angles2d const *facing_delta,
	real_vector2d const *throttle,
	unsigned long unit_control_flags,
	boolean switch_weapon,
	boolean switch_grenade);

/* the spectator's camera (a director_camera_update_proc: director.c) */
void spectator_camera_update(
	void *camera,
	void *controls,
	void *command);

/* what a spectator is shown over its view: whom it watches, their shields
and health, every player's, the scores, the items' timers and the controls */
void spectator_draw(
	short local_player_index);

/* the perspective a local spectator sees with as the player it watches in
first person (director.c): _director_perspective_first_person, or third
person where the player's own view would be (a vehicle's seat); NONE while
it watches none so, or they are dead */
short spectator_perspective(
	short local_player_index);

/* whether a local spectator watches the player in first person (their
damage's screen effects are the spectator's too: network_damage.c) */
boolean spectator_first_person_watched(
	long player_index);

/* the player a local player sees the game as: their own, or the player a
spectator watches in first person. The game's code that shows a local
player their unit (the first-person weapon, the HUD, the motion sensor,
the screen's effects of damage) asks for these in place of the local
player's own. NONE for none. */
long local_player_get_view_player_index(
	short local_player_index);
/* ... that player's unit, NONE for none */
long local_player_get_view_unit_index(
	short local_player_index);
/* ... that player, as the local player's (its local player index this
one's, for the HUD's code that takes a player): the local player's own
player, or a copy of the one watched, good until the next call */
struct player_datum *local_player_get_view_player(
	short local_player_index);
/* ... the zoom level it sees with (player_control_get_zoom_level), NONE
unzoomed */
short local_player_get_view_zoom_level(
	short local_player_index);

#endif // __SPECTATOR_H
