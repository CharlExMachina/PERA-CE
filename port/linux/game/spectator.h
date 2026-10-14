/*
SPECTATOR.H

A multiplayer spectator's view (spectator.c): its cameras, what its
controls do, and what it is shown. director.c, player_control.c and
interface.c call in here.
*/

#ifndef __SPECTATOR_H
#define __SPECTATOR_H
#pragma once

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
and health, every player's, and the items' timers */
void spectator_draw(
	short local_player_index);

#endif // __SPECTATOR_H
