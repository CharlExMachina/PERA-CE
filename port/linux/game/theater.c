/*
THEATER.C

Saved films' playback (theater.h). A film (theater_film.c) is played by this
machine's client in the film's game as the machine that made it was: its
host is the film (network_connection.c's film connection), whose messages it
takes before the ticks it had them before, and its players take the input
they had. Everything else is the client's own: it runs the game as any
client of the distributed netcode does (port/linux/NETCODE.md).

The film's end stops its clock: the game stays as it was at the film's last
tick until the film is left.

debug.film_playback plays a film once the main menu has loaded, for the
tests: the client's log has the game as any machine's does
(debug.network_test "log").
*/

#include "cseries.h"
#include "game/game.h"
#include "input/input.h"
#include "networking/network_client_manager.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "text/unicode.h"

#include "theater.h"

#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
char const *config_string(char const *name);

/* ---------- globals */

static struct
{
	/* debug.film_playback's film started (or tried) */
	boolean setting_played;
	/* the film's end told */
	boolean end_told;
	/* the viewer's speed (theater_speeds), and whether it paused */
	short speed;
	boolean paused;
	/* the film the main menu's THEATER asked for, played at the next frame
	(theater_play_soon) */
	char soon[512];
} theater;

/* the speeds the viewer picks among, slowest first, and their names; it
starts at normal speed */
static real const theater_speeds[] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
static wchar_t const *const theater_speed_names[] = { L"0.25x", L"0.5x", L"1x", L"2x", L"4x" };
#define THEATER_NORMAL_SPEED 2

/* the clock at the viewer's speed, or stopped (paused, or the film over) */
static void theater_set_clock(
	void)
{
	game_time_set_speed(theater.paused || theater_film_ended() ? 0.0f : theater_speeds[theater.speed]);
}

/* ---------- public code */

boolean theater_play(
	char const *path)
{
	struct network_game game;
	short machine_index;
	long random_seed;

	if (theater_recording())
		theater_record_end();
	if (!theater_film_open(path, &game, &machine_index, &random_seed))
		return FALSE;
	if (!network_game_client_port_play_film(&game, machine_index, random_seed))
	{
		platform_log("theater: the film's game could not be started");
		theater_film_close();
		network_game_abort();
		return FALSE;
	}
	/* (a game joined in progress: the client's clock where it was) */
	if (theater_film_start_time() > game_time_get())
		game_time_set_distributed(theater_film_start_time());
	theater.end_told = FALSE;
	theater.speed = THEATER_NORMAL_SPEED;
	theater.paused = FALSE;
	theater_set_clock();
	return TRUE;
}

void theater_play_soon(
	char const *path)
{
	csstrncpy(theater.soon, path, sizeof(theater.soon) - 1);
	theater.soon[sizeof(theater.soon) - 1] = 0;
}

void theater_stop(
	void)
{
	if (!theater_playing())
		return;
	theater_film_close();
	game_time_set_speed(1.0f);
	network_game_abort();
}

void theater_update(
	boolean main_menu_loaded,
	real seconds)
{
	/* a film being written ends with its game */
	if (theater_recording() && !network_game_client_port_in_game())
		theater_record_end();

	/* (the THEATER screen's film: its map loads, and the menus close, out of
	their own handling of the press) */
	if (theater.soon[0] && main_menu_loaded)
	{
		char path[sizeof(theater.soon)];

		csstrncpy(path, theater.soon, sizeof(path) - 1);
		path[sizeof(path) - 1] = 0;
		theater.soon[0] = 0;
		if (!theater_play(path))
			platform_log("theater: could not play %s", path);
	}
	if (!theater.setting_played && main_menu_loaded)
	{
		char const *path = config_string("debug.film_playback");

		theater.setting_played = TRUE;
		if (path && *path && !theater_play(path))
			platform_log("theater: could not play %s (debug.film_playback)", path);
	}

	if (!theater_playing())
		return;
	/* (a controller's D-pad, which the game's own controls leave alone: up
	pauses and plays, left and right go slower and faster) */
	{
		short gamepad_index;

		for (gamepad_index = 0; gamepad_index < MAXIMUM_LOCAL_PLAYERS; gamepad_index++)
		{
			struct gamepad_state const *gamepad = input_get_gamepad_state(gamepad_index);

			if (!gamepad)
				continue;
			if (gamepad->buttons[_gamepad_binary_button_dpad_up] == 1)
				theater_toggle_pause();
			if (gamepad->buttons[_gamepad_binary_button_dpad_right] == 1)
				theater_change_speed(TRUE, FALSE);
			if (gamepad->buttons[_gamepad_binary_button_dpad_left] == 1)
				theater_change_speed(FALSE, FALSE);
		}
	}
	/* (the film left: its client is gone, and the main menu back) */
	if (!network_game_client_port_in_game())
	{
		theater_film_close();
		game_time_set_speed(1.0f);
		platform_log("theater: left the film");
		return;
	}
	if (theater_film_ended())
	{
		if (!theater.end_told)
		{
			theater.end_told = TRUE;
			platform_log("theater: the film is over (%ld of %ld ticks)", theater_film_ticks_played(),
				theater_film_ticks_total());
		}
	}
	theater_set_clock();
	(void)seconds;
}

void theater_toggle_pause(
	void)
{
	if (!theater_playing())
		return;
	theater.paused = !theater.paused;
	theater_set_clock();
	platform_log("theater: %s at tick %ld", theater.paused ? "paused" : "played on", theater_film_ticks_played());
}

void theater_change_speed(
	boolean faster,
	boolean round)
{
	short count = (short)NUMBEROF(theater_speeds);

	if (!theater_playing())
		return;
	if (faster)
		theater.speed = (short)(theater.speed + 1 < count ? theater.speed + 1 : round ? 0 : count - 1);
	else
		theater.speed = (short)(theater.speed > 0 ? theater.speed - 1 : round ? count - 1 : 0);
	theater_set_clock();
	platform_log("theater: speed %.2f at tick %ld", theater_speeds[theater.speed], theater_film_ticks_played());
}

boolean theater_paused(
	void)
{
	return theater_playing() && (theater.paused || theater_film_ended());
}

void theater_status_text(
	wchar_t *text,
	short length)
{
	long played = theater_film_ticks_played() / TICKS_PER_SECOND;
	long total = theater_film_ticks_total() / TICKS_PER_SECOND;

	usnprintf(text, length - 1, L"%s   %s   %ld:%02ld / %ld:%02ld",
		theater_film_ended() ? L"THE END" : theater.paused ? L"PAUSED" : L"PLAYING",
		theater_speed_names[theater.speed], played / 60, played % 60, total / 60, total % 60);
	text[length - 1] = 0;
}
