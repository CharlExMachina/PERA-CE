/*
THEATER.H

Saved films of network games and their playback (theater_film.c, theater.c).

A film is what one machine's client heard from the host in a game, and its
own players' input, tick by tick: a client's as it read it, the host's as it
would send a machine of its own that has no players (as a spectator's is
sent every player every tick). A film is played by a client whose host is
the film (network_connection.c's film connection), which runs the game as a
client of the distributed netcode does (port/linux/NETCODE.md), its own
players driven by their recorded input.
*/

#ifndef __THEATER_H
#define __THEATER_H
#pragma once

struct network_game;
struct player_action;

/* the slot of the host's machine table its film's machine has: one no
machine that joins is given (network_server_manager.c) */
#define THEATER_FILM_MACHINE (HALO_PORT_MAXIMUM_NETWORK_MACHINES - 1)

/* ---------- recording (theater_film.c) */

/* a network game this machine's client has started (its game, its machine,
the game's random seed): its film begins, if game.record_films is on and no
film is being played */
void theater_record_begin(
	struct network_game const *game,
	short machine_index,
	long random_seed);

/* the film, written out and closed (the game over, left or ended) */
void theater_record_end(void);

/* whether a film is being written */
boolean theater_recording(void);

/* whether the host keeps its film machine's slot from machines that join
(network_server_manager.c): while it records films (game.record_films) */
boolean theater_film_machine_reserved(void);

/* whether the host's film machine is in the game: a film being written on
the host (network_distributed.c sends it what a client is sent) */
boolean theater_film_machine_active(void);

/* a message the client read (network_client_manager.c), with whether it
came in a datagram; the host's to its film machine (network_server_message_handler.c) */
void theater_record_message(
	void const *message,
	long size,
	boolean unreliable);

/* a message this machine's client read from its host in a game
(network_client_manager.c), with whether it came in a datagram: into the
film, but on the host the distributed netcode's, which its film machine has */
void theater_record_read(
	void const *message,
	long size,
	boolean unreliable);

/* the start of a tick (update_client_dequeue): the actions every player's
queue gave it, of which this machine's players' are kept */
void theater_record_tick(
	struct player_action const *actions,
	short action_count);

/* ---------- the films (theater_film.c) */

/* the most films the main menu's THEATER lists (the newest) */
#define THEATER_MAXIMUM_FILMS 100

/* a film in the films folder, as its header has it */
struct theater_film_entry
{
	char path[512];
	/* the level (levels\test\bloodgulch\bloodgulch) */
	char map_name[0x80];
	/* the game type's name, the first player of the machine that made it,
	and the game's players when it began */
	wchar_t game_type[12];
	wchar_t player_name[12];
	short player_count;
	/* when it was made (seconds since 1970), its ticks, and whether it was
	made on the host */
	unsigned long created;
	long ticks;
	boolean host;
	/* whether this build plays it (one of another film or network version
	is listed, not played) */
	boolean playable;
};

/* the films in the films folder (paths.films), the newest first; how many */
short theater_films_list(
	struct theater_film_entry *entries,
	short maximum);

/* ---------- playback (theater_film.c) */

/* whether a film is being played */
boolean theater_playing(void);

/* the film at path opened, its game checked; FALSE (with why in the log)
for one that cannot be played. Its header's game and machine for the
client that plays it */
boolean theater_film_open(
	char const *path,
	struct network_game *game,
	short *machine_index,
	long *random_seed);

/* the film closed */
void theater_film_close(void);

/* the game's time when the film's client started it (a game joined in
progress is past 0) */
long theater_film_start_time(void);

/* the film connection's next message (network_connection.c), before the
next tick's: FALSE for none until that tick has run */
boolean theater_film_read(
	void *buffer,
	word *size,
	boolean *unreliable);

/* the start of a tick of a film being played (update_client_dequeue): its
messages before it given to the client, and its players' input taken */
void theater_film_tick(void);

/* the input of this machine's local player for the tick, from the film;
FALSE for none (the film over) */
boolean theater_film_action(
	short local_player_index,
	struct player_action *action);

/* whether the film being played was made on the host: its players are all
the host's, every one corrected to where the host had it as a client's
copies of other machines' players are (network_distributed.c) */
boolean theater_film_from_host(void);

/* whether the film's next record is a tick: the machine that made it ran
one there, its messages before it read */
boolean theater_film_tick_next(void);

/* whether the film has no tick left */
boolean theater_film_ended(void);

/* the film's ticks played, and all of them (0 if unknown) */
long theater_film_ticks_played(void);
long theater_film_ticks_total(void);

/* ---------- the theater (theater.c) */

/* plays the film at path: a client whose host is the film. FALSE if it
cannot be played */
boolean theater_play(
	char const *path);

/* ... at the next frame, out of the menus' handling of the press that asked
for it (the main menu's THEATER: its map loads, and the menus close) */
void theater_play_soon(
	char const *path);

/* the film's playback stopped, and back to the main menu */
void theater_stop(void);

/* the viewer's controls (spectator.c, and a controller's D-pad): the film
paused or played on, and a speed faster or slower (0.25x to 4x; round: past
the fastest the slowest, and the other way) */
void theater_toggle_pause(void);
void theater_change_speed(
	boolean faster,
	boolean round);

/* whether the viewer has paused the film (its end pauses it too) */
boolean theater_paused(void);

/* what the viewer is shown of the film (spectator.c): whether it plays, at
what speed, and how far into it, as "PLAYING  2x  1:05 / 4:30" */
void theater_status_text(
	wchar_t *text,
	short length);

/* each frame (main.c): debug.film_playback's film started, playback's
speed, its end */
void theater_update(
	boolean main_menu_loaded,
	real seconds);

#endif // __THEATER_H
