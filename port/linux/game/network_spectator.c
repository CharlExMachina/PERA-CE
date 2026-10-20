/*
NETWORK_SPECTATOR.C

A network multiplayer game's spectators (port/linux/NETCODE.md). The host
picks which players watch the game instead of playing it: in its lobby, on
a player's row (menu_functions.c), so that they are spectators from the
game's start; in a game, a name on its scoreboard (game_engine.c); or in
either with the console's "spectate <name>". A spectator has no unit and is
given none (game_engine_should_spawn_player), counts for no team (the last
team alive, the teams' balance), and sees the game through the spectator's
cameras (spectator.c). Picked again, the player spawns as one who joined.

Only the host decides, and only the host says: it sends every client the
spectators (a bit a player, by its slot in the host's list of players,
which is its datum in a game) reliably: in a game when they change and when
a machine has loaded it, in the lobby when they change and every two
seconds (a machine that has just joined hears them then). A client asks for
nothing. The host's spectators last from the lobby into the game and from
game to game, each known by its slot, its name and its machine, so that a
slot another player takes is no spectator's.

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
#include "networking/network_game_manager.h"
#include "units/units.h"
#include "network_coop.h"
#include "network_distributed.h"
#include "network_spectator.h"

#include <stdio.h>

/* network_server_manager.c's (the host's game record), cseries_windows.c's */
struct network_game *network_game_server_get_game(struct network_game_server *server);
boolean network_connection_last_read_was_unreliable(void);
unsigned long system_milliseconds(void);

/* ---------- constants */

enum
{
	/* in the lobby, the host says its spectators again this often */
	LOBBY_INTERVAL_MILLISECONDS = 2000,
};

/* ---------- structures */

struct distributed_spectators
{
	/* the spectators: a bit a player (its slot in the host's list) */
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

/* the host: its spectators as its clients were last told, and when (the
lobby's: system_milliseconds) */
static struct distributed_spectators spectators_sent;
static unsigned long spectators_sent_at;

/* ---------- private code */

static boolean spectator_network_game(
	void)
{
	return game_connection() == _game_connection_network_client ||
		game_connection() == _game_connection_network_server;
}

/* whether this machine hosts a game it is in the lobby of: neither a
multiplayer game nor a network co-op game runs (the user interface's does) */
static boolean spectator_lobby_host(
	void)
{
	return global_network_game_server_get() != NULL && !game_engine_running() && !network_coop_active();
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

/* (the host) the player at a slot: its name and machine, FALSE for none. In
a multiplayer game, the game's player there, who has not quit; else (the
lobby, where the user interface's game has players of its own) the one in
the host's list of players. */
static boolean spectator_host_player(
	short player_index,
	wchar_t *name,
	char *machine_index)
{
	if (player_index < 0 || player_index >= MAXIMUM_TRACKED_PLAYERS)
		return FALSE;
	if (network_spectator_host())
	{
		struct player_datum *player = distributed_player(player_index);

		if (!player || player->quit_out_of_game)
			return FALSE;
		csmemcpy(name, player->name, sizeof(player->name));
		*machine_index = player->network_player_data.machine_index;
		return TRUE;
	}
	else
	{
		struct network_game_server *server = global_network_game_server_get();
		struct network_game *game = server ? network_game_server_get_game(server) : NULL;

		if (!game || !network_player_is_valid(&game->players[player_index]))
			return FALSE;
		csmemcpy(name, game->players[player_index].name, sizeof(game->players[player_index].name));
		*machine_index = game->players[player_index].machine_index;
		return TRUE;
	}
}

/* (the host) whether the player at the slot is still the one made a spectator */
static boolean spectator_identity_matches(
	short player_index)
{
	wchar_t name[12];
	char machine_index;

	return spectator_host_player(player_index, name, &machine_index) &&
		machine_index == spectator_identities[player_index].machine_index &&
		!csmemcmp(name, spectator_identities[player_index].name, sizeof(name));
}

/* a player's name in ASCII (player_name_character_ascii), at most size */
static void spectator_name_text(
	wchar_t const *name,
	char *text,
	long size)
{
	long index;

	for (index = 0; index < 12 && name[index] && index < size - 1; index++)
		text[index] = player_name_character_ascii(name[index]);
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

/* (the host) its spectators that are still who they were (the others
forgotten); TRUE when they are not as its clients were last told */
static boolean spectator_host_current(
	struct distributed_spectators *current)
{
	short player_index;

	csmemset(current, 0, sizeof(*current));
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		if (!spectator_bit(&spectators, player_index))
			continue;
		if (spectator_identity_matches(player_index))
			spectator_set_bit(current, player_index, TRUE);
		else
			spectator_set_bit(&spectators, player_index, FALSE);
	}
	return csmemcmp(current, &spectators_sent, sizeof(*current)) != 0;
}

/* (the host) its spectators to a client machine (NONE: to every client) */
static void spectator_host_send(
	long machine_index)
{
	struct
	{
		struct distributed_message_header header;
		struct distributed_spectators spectators;
	} message;

	csmemset(&message, 0, sizeof(message));
	spectator_host_current(&message.spectators);
	if (machine_index == NONE)
	{
		spectators_sent = message.spectators;
		spectators_sent_at = system_milliseconds();
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
	if (!global_network_game_server_get())
		csmemset(&spectators, 0, sizeof(spectators));
	csmemset(&spectators_sent, 0, sizeof(spectators_sent));
}

void network_spectator_host_tick(
	void)
{
	struct distributed_spectators current;

	/* (a spectator gone, or its slot another player's: one no longer) */
	if (network_spectator_host() && spectator_host_current(&current))
		spectator_host_send(NONE);
}

void network_spectator_machine_loaded(
	long machine_index)
{
	if (network_spectator_host())
		spectator_host_send(machine_index);
}

boolean network_spectator_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_spectators;
}

void network_spectator_handle_message(
	long machine_index,
	word const *message,
	word size)
{
	struct distributed_message_header header;

	/* (the host's word alone, over its connection: a datagram is anyone's) */
	if (!network_spectator_handles_message(message, size) || machine_index != NONE ||
		global_network_game_server_get() || !global_network_game_client_get() ||
		network_connection_last_read_was_unreliable())
	{
		return;
	}
	csmemcpy(&header, message, sizeof(header));
	if (header.count < 1 || size < sizeof(header) + sizeof(spectators))
		return;
	csmemcpy(&spectators, (byte const *)message + sizeof(header), sizeof(spectators));
}

/* ---------- public code (network_spectator.h) */

void network_spectator_update(
	void)
{
	struct distributed_spectators current;

	/* (the lobby's: in a game, network_spectator_host_tick) */
	if (!spectator_lobby_host())
		return;
	if (spectator_host_current(&current) ||
		(long)(system_milliseconds() - spectators_sent_at) >= LOBBY_INTERVAL_MILLISECONDS)
	{
		spectator_host_send(NONE);
	}
}

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
		return spectator_identity_matches(absolute_index);
	return TRUE;
}

boolean network_spectator_listed(
	short player_index)
{
	if (!spectator_bit(&spectators, player_index))
		return FALSE;
	return !global_network_game_server_get() || spectator_identity_matches(player_index);
}

boolean network_spectator_host(
	void)
{
	return game_connection() == _game_connection_network_server && game_engine_running();
}

boolean network_spectator_host_may_pick(
	void)
{
	return network_spectator_host() || spectator_lobby_host();
}

boolean network_spectator_host_set(
	short player_index,
	boolean spectator)
{
	wchar_t wide_name[12];
	char machine_index;
	char name[SPECTATOR_NAME_TEXT_SIZE];
	char notice[96];

	if (!network_spectator_host_may_pick())
	{
		console_warning("spectate: only the host of a multiplayer game");
		return FALSE;
	}
	if (!spectator_host_player(player_index, wide_name, &machine_index))
	{
		console_warning("spectate: no such player");
		return FALSE;
	}
	if ((spectator_bit(&spectators, player_index) && spectator_identity_matches(player_index)) ==
		(spectator != FALSE))
	{
		return TRUE;
	}
	spectator_set_bit(&spectators, player_index, spectator);
	spectator_name_text(wide_name, name, sizeof(name));
	if (spectator)
	{
		csmemcpy(spectator_identities[player_index].name, wide_name, sizeof(wide_name));
		spectator_identities[player_index].machine_index = machine_index;
		snprintf(notice, sizeof(notice), "%s is a spectator", name);
	}
	else
	{
		snprintf(notice, sizeof(notice), "%s is a player again", name);
	}
	/* (in a game: out of it as a player who leaves it is, no death counted,
	and every machine told; in the lobby its rows say) */
	if (network_spectator_host())
	{
		struct player_datum *player = distributed_player(player_index);

		if (spectator && player && player->unit_index != NONE)
			unit_kill_no_statistics(player->unit_index);
		distributed_send_notice(notice);
	}
	error(_error_log, "spectate: %s", notice);
	spectator_host_send(NONE);
	return TRUE;
}

short network_spectator_matching_player_names(
	char const *text,
	char (*names)[SPECTATOR_NAME_TEXT_SIZE],
	short maximum_count)
{
	short player_index;
	short count = 0;

	if (!network_spectator_host_may_pick())
		return 0;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS && count < maximum_count; player_index++)
	{
		wchar_t wide_name[12];
		char machine_index;

		if (!spectator_host_player(player_index, wide_name, &machine_index))
			continue;
		spectator_name_text(wide_name, names[count], SPECTATOR_NAME_TEXT_SIZE);
		if (spectator_name_begins_with(names[count], text, FALSE))
			count++;
	}
	return count;
}

boolean network_spectator_player_named(
	char const *text)
{
	short player_index;
	short found = NONE;
	short exact_found = NONE;
	short matches = 0;
	short exact = 0;

	if (!network_spectator_host_may_pick())
	{
		console_warning("spectate: only the host of a multiplayer game");
		return FALSE;
	}
	if (!text[0])
	{
		console_warning("spectate: give a player's name (Tab completes it)");
		return FALSE;
	}
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		wchar_t wide_name[12];
		char machine_index;
		char name[SPECTATOR_NAME_TEXT_SIZE];

		if (!spectator_host_player(player_index, wide_name, &machine_index))
			continue;
		spectator_name_text(wide_name, name, sizeof(name));
		if (spectator_name_begins_with(name, text, TRUE))
		{
			exact++;
			exact_found = player_index;
		}
		if (spectator_name_begins_with(name, text, FALSE))
		{
			matches++;
			found = player_index;
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
	return network_spectator_host_set(found, !network_spectator_listed(found));
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
