/*
PHOTO_MODE.H

Photo mode (photo_mode.c), opened from a single-player campaign's pause
menu: its camera, the player's pose, the view's filter and the photos.
main.c, director.c, player_control.c, interface.c, render_interpolation.c,
menu_functions.c and the renderers (d3d8_gl.c, d3d8_metal.c) call in here.
*/

#ifndef __PHOTO_MODE_H
#define __PHOTO_MODE_H
#pragma once

/* the floats of a filter's parameters (photo_mode_filter): a colour matrix
(three rows of red, green and blue weights and an offset), the vignette's
strength, and three unused; the renderer adds the 3D view's rectangle in
pixels (x0, y0, x1, y1) after them */
#define PHOTO_MODE_FILTER_PARAMETERS 16
#define PHOTO_MODE_FILTER_UNIFORMS 20

/* whether photo mode is on: the game paused, the photo mode's camera */
boolean photo_mode_active(
	void);

/* the pause menu's PHOTO MODE (menu_functions.c's "port photo mode"):
whether it may begin, in a single-player campaign with the player on foot
or in a vehicle; it begins once the menus have closed */
boolean photo_mode_request(
	short local_player_index);

/* each frame, after the menus (main.c): photo mode begun or ended, and the
game kept paused */
void photo_mode_update(
	void);

/* photo mode's input this frame (player_control.c), the player's own
controls: how far it turned, its throttle, its unit control flags, and
whether switch weapon, switch grenade and zoom are held */
void photo_mode_handle_input(
	short local_player_index,
	real_euler_angles2d const *facing_delta,
	real_vector2d const *throttle,
	unsigned long unit_control_flags,
	boolean switch_weapon,
	boolean switch_grenade,
	boolean zoom);

/* photo mode's camera (a director_camera_update_proc: director.c) */
void photo_mode_camera_update(
	void *camera,
	void *controls,
	void *command);

/* what photo mode shows over the view: the pose, the filter, the controls
and the last photo saved (interface.c's full screen overlays) */
void photo_mode_draw(
	void);

/* whether this frame is a photo: nothing is drawn over the view */
boolean photo_mode_capturing(
	void);

/* the renderers' (d3d8_gl.c, d3d8_metal.c): the filter's parameters,
FALSE for none (PHOTO_MODE_FILTER_PARAMETERS floats); whether the frame
presented now is a photo; and the photo, its rows from the top, 4 bytes a
pixel (blue, green, red when bgra, else red, green, blue; the fourth
unused) */
int photo_mode_filter(
	float *parameters);
int photo_mode_capture_wanted(
	void);
void photo_mode_capture(
	unsigned char const *pixels,
	long width,
	long height,
	long pitch,
	int bgra);

#endif // __PHOTO_MODE_H
