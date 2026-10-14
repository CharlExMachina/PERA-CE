/*
NETWORK_SPECTATOR.C

A network multiplayer game's spectators (port/linux/NETCODE.md). The host
picks which players watch the game instead of playing it: a name on its
scoreboard (game_engine.c), or the console's "spectate <name>". A spectator
has no unit and is given none (game_engine_should_spawn_player), counts for
no team (the last team alive, the teams' balance), and sees the game through
the spectator's cameras (spectator.c). Picked again, the player spawns as
one who joined.

Only the host decides, and only the host says: it sends every client the
spectators (a bit a player) reliably, when they change and when a machine
has loaded the game. A client asks for nothing. The host's spectators last
from game to game, each known by its slot, its name and its machine, so that
a slot another player takes is no spectator's.

A build without spectators drops the message as a kind it does not know: a
player of its made a spectator is not spawned, and watches nothing.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "main/console.h"
#include "networking/network_game_globals.h"
#include "units/units.h"
#include "network_distributed.h"
#include "network_spectator.h"

#include <stdio.h>

/* ---------- structures */

struct distributed_spectators
{
	/* the spectators: a bit a player (absolute index) */
	byte players[MAXIMUM_TRACKED_PLAYERS / 8];
};

typedef char distributed_spectators_size_assert[sizeof(struct distributed_spectators) == 16 ? 1 : -1];

/* ---------- globals */

/* the spectators: the host's own picks, a client's as the host last said */
static struct distributed_spectators spectators;

/* the host: who each of its spectators is (its slot may be another's later) */
static struct
{
	wchar_t name[12];
	char machine_index;
} spectator_identities[MAXIMUM_TRACKED_PLAYERS];

/* the host: its spectators as its clients were last told */
static struct distributed_spectators spectators_sent;

/* ---------- private code */

static boolean spectator_network_game(
	void)
{
	return game_connection() == _game_connection_network_client ||
		game_connection() == _game_connection_network_server;
}

static boolean spectator_bit(
	struct distributed_spectators const *bits,
	short player_index)
{
	return player_index >= 0 && player_index < MAXIMUM_TRACKED_PLAYERS &&
		(bits->players[player_index >> 3] & (1 << (player_index & 7))) != 0;
}

static void spectator_set_bit(
	struct distributed_spectators *bits,
	short player_index,
	boolean spectator)
{
	if (player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS)
		return;
	if (spectator)
		bits->players[player_index >> 3] |= (byte)(1 << (player_index & 7));
	else
		bits->players[player_index >> 3] &= (byte)~(1 << (player_index & 7));
}

/* (the host) whether the player at the slot is still the one made a spectator */
static boolean spectator_identity_matches(
	short player_index,
	struct player_datum const *player)
{
	return player && !player->quit_out_of_game &&
		player->network_player_data.machine_index == spectator_identities[player_index].machine_index &&
		!csmemcmp(player->name, spectator_identities[player_index].name, sizeof(player->name));
}

/* a player's name in ASCII (player_name_character_ascii), at most size */
static void spectator_player_name(
	struct player_datum const *player,
	char *text,
	long size)
{
	long index;

	for (index = 0; index < (long)NUMBEROF(player->name) && player->name[index] && index < size - 1; index++)
		text[index] = player_name_character_ascii(player->name[index]);
	text[index] = 0;
}

/* whether a name begins with the text, in either case; and is it */
static boolean spectator_name_begins_with(
	char const *name,
	char const *text,
	boolean whole)
{
	for (; *text; name++, text++)
	{
		char a = *name >= 'A' && *name <= 'Z' ? *name - 'A' + 'a' : *name;
		char b = *text >= 'A' && *text <= 'Z' ? *text - 'A' + 'a' : *text;

		if (!*name || a != b)
			return FALSE;
	}
	return !whole || !*name;
}

/* (the host) its spectators that are still who they were, to a client
machine (NONE: to every client) */
static void spectator_host_send(
	long machine_index)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_spectators spectators;
	} message;
	short player_index;

	csmemset(&message, 0, sizeof(message));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (spectator_bit(&spectators, player_index) &&
			spectator_identity_matches(player_index, distributed_player(player_index)))
		{
			spectator_set_bit(&message.spectators, player_index, TRUE);
		}
	}
	if (machine_index == NONE)
	{
		spectators_sent = message.spectators;
		distributed_send(&message, _distributed_message_spectators, 1, (word)sizeof(message),
			_distributed_to_clients_reliably);
	}
	else
	{
		distributed_send_to_machine_reliably(machine_index, &message, _distributed_message_spectators, 1,
			(word)sizeof(message));
	}
}

/* ---------- public code (network_distributed.h) */

void network_spectator_new_game(
	void)
{
	/* (a client's are its host's, told again once it has loaded; the host's
	own last) */
	if (game_connection() != _game_connection_network_server)
		csmemset(&spectators, 0, sizeof(spectators));
	csmemset(&spectators_sent, 0, sizeof(spectators_sent));
}

void network_spectator_host_tick(
	void)
{
	struct distributed_spectators current;
	short player_index;

	if (!network_spectator_host())
		return;
	/* (a spectator gone, or its slot another player's: one no longer) */
	csmemset(&current, 0, sizeof(current));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (!spectator_bit(&spectators, player_index))
			continue;
		if (spectator_identity_matches(player_index, distributed_player(player_index)))
			spectator_set_bit(&current, player_index, TRUE);
		else
			spectator_set_bit(&spectators, player_index, FALSE);
	}
	if (csmemcmp(&current, &spectators_sent, sizeof(current)))
		spectator_host_send(NONE);
}

void network_spectator_machine_loaded(
	long machine_index)
{
	if (network_spectator_host())
		spectator_host_send(machine_index);
}

void network_spectator_handle(
	void const *entries)
{
	if (game_connection() != _game_connection_network_client)
		return;
	csmemcpy(&spectators, entries, sizeof(spectators));
}

word network_spectator_entry_size(
	void)
{
	return sizeof(struct distributed_spectators);
}

/* ---------- public code (network_spectator.h) */

boolean network_spectator_player(
	long player_index)
{
	short absolute_index;

	if (player_index == NONE || !spectator_network_game() || !game_engine_running())
		return FALSE;
	absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (!spectator_bit(&spectators, absolute_index))
		return FALSE;
	/* (the host's: only the player it picked) */
	if (game_connection() == _game_connection_network_server)
		return spectator_identity_matches(absolute_index, player_try_and_get(player_index));
	return TRUE;
}

boolean network_spectator_host(
	void)
{
	return game_connection() == _game_connection_network_server && game_engine_running();
}

boolean network_spectator_host_set(
	short player_index,
	boolean spectator)
{
	struct player_datum *player = distributed_player(player_index);
	char name[SPECTATOR_NAME_TEXT_SIZE];
	char notice[96];

	if (!network_spectator_host())
	{
		console_warning("spectate: only the host of a multiplayer game");
		return FALSE;
	}
	if (!player || player->quit_out_of_game)
	{
		console_warning("spectate: no such player");
		return FALSE;
	}
	if ((spectator_bit(&spectators, player_index) && spectator_identity_matches(player_index, player)) ==
		(spectator != FALSE))
	{
		return TRUE;
	}
	spectator_set_bit(&spectators, player_index, spectator);
	spectator_player_name(player, name, sizeof(name));
	if (spectator)
	{
		csmemcpy(spectator_identities[player_index].name, player->name, sizeof(player->name));
		spectator_identities[player_index].machine_index = player->network_player_data.machine_index;
		/* (out of the game as a player who leaves it is: no death counted) */
		if (player->unit_index != NONE)
			unit_kill_no_statistics(player->unit_index);
		snprintf(notice, sizeof(notice), "%s is a spectator", name);
	}
	else
	{
		snprintf(notice, sizeof(notice), "%s is a player again", name);
	}
	distributed_send_notice(notice);
	error(_error_log, "spectate: %s", notice);
	spectator_host_send(NONE);
	return TRUE;
}

short network_spectator_matching_player_names(
	char const *text,
	char (*names)[SPECTATOR_NAME_TEXT_SIZE],
	short maximum_count)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short count = 0;

	if (!network_spectator_host())
		return 0;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL && count < maximum_count)
	{
		if (player->quit_out_of_game)
			continue;
		spectator_player_name(player, names[count], SPECTATOR_NAME_TEXT_SIZE);
		if (spectator_name_begins_with(names[count], text, FALSE))
			count++;
	}
	return count;
}

boolean network_spectator_player_named(
	char const *text)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long found = NONE;
	long exact_found = NONE;
	short matches = 0;
	short exact = 0;

	if (!network_spectator_host())
	{
		console_warning("spectate: only the host of a multiplayer game");
		return FALSE;
	}
	if (!text[0])
	{
		console_warning("spectate: give a player's name (Tab completes it)");
		return FALSE;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		char name[SPECTATOR_NAME_TEXT_SIZE];

		if (player->quit_out_of_game)
			continue;
		spectator_player_name(player, name, sizeof(name));
		if (spectator_name_begins_with(name, text, TRUE))
		{
			exact++;
			exact_found = iterator.datum_index;
		}
		if (spectator_name_begins_with(name, text, FALSE))
		{
			matches++;
			found = iterator.datum_index;
		}
	}
	if (exact == 1)
	{
		found = exact_found;
		matches = 1;
	}
	if (matches > 1)
	{
		console_warning("spectate: %d players' names begin with \"%s\": give more of it", matches, text);
		return FALSE;
	}
	if (matches == 0)
	{
		console_warning("spectate: no player's name begins with \"%s\"", text);
		return FALSE;
	}
	return network_spectator_host_set((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(found),
		!network_spectator_player(found));
}

void network_spectator_machine_joined(
	long machine_index)
{
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (spectator_bit(&spectators, player_index) &&
			spectator_identities[player_index].machine_index == (char)machine_index)
		{
			spectator_set_bit(&spectators, player_index, FALSE);
		}
	}
}
