/*
NETWORK_SPECTATOR.H

A network game's spectators (network_spectator.c): what the game
(game_engine.c), the console (hs.c, console.c), the host's server
(network_server_manager.c) and the spectator's view (spectator.c) ask of it.
*/

#ifndef __NETWORK_SPECTATOR_H
#define __NETWORK_SPECTATOR_H
#pragma once

/* ---------- constants */

enum
{
	/* a player's name in ASCII, with its end */
	SPECTATOR_NAME_TEXT_SIZE = 16,
};

/* ---------- prototypes/NETWORK_SPECTATOR.C */

/* whether the player (a datum index) is a spectator: in a network
multiplayer game, as the host says */
boolean network_spectator_player(long player_index);
/* whether the player at a slot of the host's list of players is a
spectator, as the lobby's rows say (menu_functions.c); in a game the slot is
the player's datum */
boolean network_spectator_listed(short player_index);
/* whether this machine hosts a network multiplayer game under way (its
scoreboard offers Spectate) */
boolean network_spectator_host(void);
/* ... or hosts one or its lobby (its console takes "spectate", and its
lobby's rows pick spectators) */
boolean network_spectator_host_may_pick(void);
/* each frame (main.c): the host's lobby tells its machines the spectators */
void network_spectator_update(void);
/* (the host) makes the player (absolute index, or in the lobby its slot) a
spectator, or a player again; every machine is told */
boolean network_spectator_host_set(short player_index, boolean spectator);
/* the host's "spectate <name>": the player of that name (or the one whose
name begins with it) made a spectator, or a player again */
boolean network_spectator_player_named(char const *text);
/* the names of the game's players that begin with the text (the command's
completion: console.c); how many */
short network_spectator_matching_player_names(char const *text, char (*names)[SPECTATOR_NAME_TEXT_SIZE],
	short maximum_count);
/* (the host's server) a machine joined at the slot: no spectator of the
machine before is its */
void network_spectator_machine_joined(long machine_index);

#endif // __NETWORK_SPECTATOR_H
