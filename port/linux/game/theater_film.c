/*
THEATER_FILM.C

Saved films (theater.h): written as a network game is played, and read back
for the client that plays one.

A film is the file of one game, from when this machine's client started it
to when it left it:

- its header (struct theater_film_header): "OCEFILM", the film's version, the
  network version (HALO_PORT_NETWORK_VERSION) and the game settings record's
  size of the build that wrote it, the machine it was written on and whether
  that was the host, the game's random seed, the game's time at its start,
  its ticks (0 until it is closed), when it was made and the map's name;
  then the game settings record the client had (struct network_game);
- then its chunks: each its sizes, its first tick and its ticks (struct
  theater_film_chunk), and its records compressed with the game's zlib, a
  second of ticks or THEATER_CHUNK_SIZE bytes of records, whichever is
  first. A game cut short (the game stopped) keeps its whole chunks, and its
  ticks are counted from them.

A record is a byte of its kind and then:

- a message: a byte of flags (it came in a datagram), its size (2 bytes) and
  the message as the client read it, header and all;
- a tick: the game's time at its start (4 bytes), how many of this machine's
  players it has (a byte), and for each its local player's index (a byte) and
  the action its queue gave it (struct player_action).

The records are in the order the machine had them: a tick's record is
written as its tick starts, after the messages the client read before it.
The client that plays the film is given the same messages before the same
ticks, and its players the same actions.

Only the machine's own build plays its films: one of another network
version or game settings record is refused (as machines of different
versions do not play together). A film is a file anyone may be sent, so
nothing in it is trusted: every size is checked, a chunk must unpack to the
size it says, the game settings record is checked as one from a host is
(network_client_manager.c), the messages are taken by the client's own
handlers, which take a host's messages as untrusted, and an action is taken
only finite and with its choices in range.
*/

#include "cseries.h"
#include "bungie_net/common/message_header.h"
#include "game/game.h"
#include "game/players.h"
#include "math/real_math.h"
#include "memory/zlib/zlib.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_messages.h"
#include "units/units.h"

#include "theater.h"
#include "../src/posix.h" /* port/linux/src/posix_files.c */

#include <stdio.h>
#include <string.h>
#include <time.h>

/* (films are files on this machine's disk, not Xbox paths: the C library's
own, not the game's, port/linux/include/stdio.h) */
#undef fopen
#undef remove

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
int config_boolean(char const *name);
char const *platform_films_root(void);
/* network_client_manager.c's */
boolean network_game_client_port_read_film_messages(void);
/* network_distributed.c's */
void network_distributed_film_machine_loaded(void);
void network_distributed_film_machine_left(void);
boolean network_distributed_film_machine_free(void);

/* ---------- constants */

enum
{
	THEATER_FILM_VERSION = 1,
	/* a chunk's records, at most (a record never is split) */
	THEATER_CHUNK_SIZE = 0x30000,
	/* ... and its ticks */
	THEATER_CHUNK_TICKS = TICKS_PER_SECOND,
	/* the largest message (its header's size: 12 bits) */
	THEATER_MESSAGE_MAXIMUM_SIZE = 0x1000,
	/* a message's record past the message: its kind, flags and size */
	THEATER_MESSAGE_RECORD_OVERHEAD = 4,
	/* a tick's record: its kind, time and count, then each player's */
	THEATER_TICK_RECORD_OVERHEAD = 6,
	THEATER_TICK_PLAYER_SIZE = 1 + sizeof(struct player_action),
	THEATER_RECORD_MAXIMUM_SIZE = THEATER_MESSAGE_RECORD_OVERHEAD + THEATER_MESSAGE_MAXIMUM_SIZE,
	/* zlib 1.1.3's bound on what a chunk packs to (compress2: a thousandth
	more and 12 bytes) */
	THEATER_CHUNK_PACKED_SIZE = THEATER_CHUNK_SIZE + THEATER_CHUNK_SIZE / 1000 + 12 + 0x100,
};

enum
{
	_theater_record_message = 1,
	_theater_record_tick,
};

enum
{
	_theater_message_datagram_bit,
};

enum
{
	_theater_film_host_bit,
};

/* the distributed netcode's messages' kind of header (network_distributed.c's
"data" messages, beside the game's packets) */
enum
{
	_theater_message_type_data = 2,
};

static char const theater_film_signature[8] = "OCEFILM";

/* ---------- structures */

struct theater_film_header
{
	char signature[8];
	unsigned long version;
	unsigned long network_version;
	unsigned long game_size;
	long machine_index;
	long random_seed;
	long start_time;
	long ticks;
	unsigned long created;
	unsigned long flags;
	char map_name[0x80];
	byte reserved[0x44];
};

typedef char theater_film_header_size_assert[
	sizeof(struct theater_film_header) == 0xF0 ? 1 : -1];

struct theater_film_chunk
{
	unsigned long packed_size;
	unsigned long size;
	long first_tick;
	long ticks;
};

/* ---------- globals */

static struct
{
	FILE *file;
	boolean host;
	long ticks;
	long chunk_ticks;
	long chunk_first_tick;
	long size;
	byte data[THEATER_CHUNK_SIZE];
	char path[512];
} theater_recorder;

static struct
{
	FILE *file;
	struct theater_film_header header;
	long size;
	long cursor;
	byte data[THEATER_CHUNK_SIZE];
	long ticks_played;
	boolean ended;
	boolean action_valid[MAXIMUM_LOCAL_PLAYERS];
	struct player_action actions[MAXIMUM_LOCAL_PLAYERS];
	boolean time_told;
} theater_player;

/* what a chunk packs to, read or written (one at a time) */
static byte theater_packed[THEATER_CHUNK_PACKED_SIZE];

/* ---------- private code: recording */

static void theater_record_flush(
	void)
{
	struct theater_film_chunk chunk;
	uLongf packed_size = sizeof(theater_packed);
	int result;

	if (!theater_recorder.file || !theater_recorder.size)
		return;
	result = compress2(theater_packed, &packed_size, theater_recorder.data, (uLong)theater_recorder.size,
		Z_DEFAULT_COMPRESSION);
	chunk.packed_size = (unsigned long)packed_size;
	chunk.size = (unsigned long)theater_recorder.size;
	chunk.first_tick = theater_recorder.chunk_first_tick;
	chunk.ticks = theater_recorder.chunk_ticks;
	if (result != Z_OK || fwrite(&chunk, sizeof(chunk), 1, theater_recorder.file) != 1 ||
		fwrite(theater_packed, packed_size, 1, theater_recorder.file) != 1)
	{
		platform_log("theater: could not write the film %s (%d); it ends here", theater_recorder.path, result);
		fclose(theater_recorder.file);
		theater_recorder.file = NULL;
		if (theater_recorder.host)
			network_distributed_film_machine_left();
		return;
	}
	theater_recorder.size = 0;
	theater_recorder.chunk_ticks = 0;
	theater_recorder.chunk_first_tick = theater_recorder.ticks;
}

/* room for a record of size bytes in the chunk (the chunk written first
when full) */
static byte *theater_record_room(
	long size)
{
	if (theater_recorder.size + size > THEATER_CHUNK_SIZE)
		theater_record_flush();
	if (!theater_recorder.file)
		return NULL;
	return theater_recorder.data + theater_recorder.size;
}

/* the level's name of the map's path (levels\test\bloodgulch\bloodgulch:
bloodgulch), made fit for a file's name */
static void theater_film_level_name(
	char const *map_name,
	char *name,
	size_t size)
{
	char const *last = strrchr(map_name, '\\');
	size_t index;

	snprintf(name, size, "%s", last ? last + 1 : map_name);
	for (index = 0; name[index]; index++)
	{
		if (!((name[index] >= 'a' && name[index] <= 'z') || (name[index] >= 'A' && name[index] <= 'Z') ||
			(name[index] >= '0' && name[index] <= '9') || name[index] == '_' || name[index] == '-'))
		{
			name[index] = '_';
		}
	}
	if (!name[0])
		snprintf(name, size, "film");
}

/* the new film's path: the level's and the time's, numbered after the first
of the same second */
static boolean theater_film_new_path(
	char const *map_name,
	char *path,
	size_t size)
{
	char const *root = platform_films_root();
	char level[64];
	char stamp[32];
	time_t now = time(NULL);
	struct tm const *local = localtime(&now);
	short collision;

	if (!root || !*root)
		return FALSE;
	theater_film_level_name(map_name, level, sizeof(level));
	if (local)
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", local);
	else
		snprintf(stamp, sizeof(stamp), "%lu", (unsigned long)now);
	for (collision = 0; collision < 100; collision++)
	{
		FILE *existing;

		if (collision)
			snprintf(path, size, "%s/%s %s (%d).film", root, level, stamp, collision + 1);
		else
			snprintf(path, size, "%s/%s %s.film", root, level, stamp);
		existing = fopen(path, "rb");
		if (!existing)
			break;
		fclose(existing);
	}
	return collision < 100;
}

/* ---------- private code: playback */

/* the ticks of a film whose game was cut short, which has no count of them:
those of its whole chunks, from the file's place (the first chunk's), which
it is left at */
static long theater_film_count_ticks(
	FILE *file)
{
	long start = ftell(file);
	long ticks = 0;
	struct theater_film_chunk chunk;

	while (fread(&chunk, sizeof(chunk), 1, file) == 1 &&
		chunk.packed_size > 0 && chunk.packed_size <= sizeof(theater_packed) && chunk.ticks >= 0 &&
		chunk.first_tick == ticks && fseek(file, (long)chunk.packed_size, SEEK_CUR) == 0)
	{
		/* (one cut off is not counted) */
		long here = ftell(file);

		if (fseek(file, 0, SEEK_END) != 0 || ftell(file) < here || fseek(file, here, SEEK_SET) != 0)
			break;
		ticks += chunk.ticks;
	}
	fseek(file, start, SEEK_SET);
	return ticks;
}

/* a film's entry for the list (theater_films_list), from its header and
game; FALSE for a file that is no film */
static boolean theater_film_describe(
	char const *path,
	struct theater_film_entry *entry)
{
	static struct network_game game;
	struct theater_film_header header;
	FILE *file = fopen(path, "rb");
	short index;

	if (!file)
		return FALSE;
	if (fread(&header, sizeof(header), 1, file) != 1 ||
		csmemcmp(header.signature, theater_film_signature, sizeof(header.signature)) != 0)
	{
		fclose(file);
		return FALSE;
	}
	csmemset(entry, 0, sizeof(*entry));
	csstrncpy(entry->path, path, sizeof(entry->path) - 1);
	header.map_name[sizeof(header.map_name) - 1] = 0;
	csstrncpy(entry->map_name, header.map_name, sizeof(entry->map_name) - 1);
	entry->created = header.created;
	entry->host = TEST_FLAG(header.flags, _theater_film_host_bit);
	entry->playable = header.version == THEATER_FILM_VERSION && header.network_version == HALO_PORT_NETWORK_VERSION &&
		header.game_size == sizeof(game) && header.machine_index >= 0 &&
		header.machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES;
	if (entry->playable && fread(&game, sizeof(game), 1, file) == 1)
	{
		/* (a game type of no name of its own, the network tests', by its game
		engine's; co-op, which has none) */
		static wchar_t const *const engine_names[] = { L"Co-op", L"CTF", L"Slayer", L"Oddball", L"King", L"Race" };

		csmemcpy(entry->game_type, game.variant.human_readable_game_description, sizeof(entry->game_type));
		entry->game_type[NUMBEROF(entry->game_type) - 1] = 0;
		if (!entry->game_type[0] && game.variant.game_engine_index >= 0 &&
			game.variant.game_engine_index < (long)NUMBEROF(engine_names))
		{
			wchar_t const *engine_name = engine_names[game.variant.game_engine_index];

			for (index = 0; index < NUMBEROF(entry->game_type) - 1 && engine_name[index]; index++)
				entry->game_type[index] = engine_name[index];
			entry->game_type[index] = 0;
		}
		for (index = 0; index < NUMBEROF(game.players); index++)
		{
			struct network_player const *player = &game.players[index];

			if (player->controller_index < 0 || player->controller_index >= MAXIMUM_LOCAL_PLAYERS ||
				player->machine_index < 0 || (long)player->machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES)
			{
				continue;
			}
			entry->player_count++;
			if (!entry->player_name[0] && player->machine_index == header.machine_index)
			{
				csmemcpy(entry->player_name, player->name, sizeof(entry->player_name));
				entry->player_name[NUMBEROF(entry->player_name) - 1] = 0;
			}
		}
		entry->ticks = header.ticks > 0 ? header.ticks : theater_film_count_ticks(file);
	}
	else
	{
		entry->playable = FALSE;
	}
	fclose(file);
	return TRUE;
}

/* the next chunk's records, FALSE at the film's end (or a chunk that is not
one: the film is over there) */
static boolean theater_player_next_chunk(
	void)
{
	struct theater_film_chunk chunk;
	uLongf size = sizeof(theater_player.data);

	theater_player.size = 0;
	theater_player.cursor = 0;
	if (!theater_player.file || fread(&chunk, sizeof(chunk), 1, theater_player.file) != 1)
		return FALSE;
	if (chunk.packed_size == 0 || chunk.packed_size > sizeof(theater_packed) || chunk.size == 0 ||
		chunk.size > sizeof(theater_player.data) ||
		fread(theater_packed, chunk.packed_size, 1, theater_player.file) != 1 ||
		uncompress(theater_player.data, &size, theater_packed, (uLong)chunk.packed_size) != Z_OK ||
		size != chunk.size)
	{
		platform_log("theater: the film's chunk at tick %ld is damaged; the film ends there",
			theater_player.ticks_played);
		return FALSE;
	}
	theater_player.size = (long)size;
	return TRUE;
}

/* the kind of the record next (_theater_record_*), with the size it takes;
0 at the film's end, or at a record that is not one */
static short theater_player_peek(
	long *record_size)
{
	byte const *record;
	long left;

	if (theater_player.ended)
		return 0;
	if (theater_player.cursor >= theater_player.size && !theater_player_next_chunk())
	{
		theater_player.ended = TRUE;
		return 0;
	}
	record = theater_player.data + theater_player.cursor;
	left = theater_player.size - theater_player.cursor;
	switch (record[0])
	{
	case _theater_record_message:
		if (left >= THEATER_MESSAGE_RECORD_OVERHEAD)
		{
			word size;

			csmemcpy(&size, record + 2, sizeof(size));
			if (size >= sizeof(word) && size <= THEATER_MESSAGE_MAXIMUM_SIZE &&
				THEATER_MESSAGE_RECORD_OVERHEAD + size <= left)
			{
				*record_size = THEATER_MESSAGE_RECORD_OVERHEAD + size;
				return _theater_record_message;
			}
		}
		break;
	case _theater_record_tick:
		if (left >= THEATER_TICK_RECORD_OVERHEAD && record[5] <= MAXIMUM_LOCAL_PLAYERS &&
			THEATER_TICK_RECORD_OVERHEAD + record[5] * THEATER_TICK_PLAYER_SIZE <= left)
		{
			*record_size = THEATER_TICK_RECORD_OVERHEAD + record[5] * THEATER_TICK_PLAYER_SIZE;
			return _theater_record_tick;
		}
		break;
	}
	platform_log("theater: the film has a damaged record at tick %ld; the film ends there",
		theater_player.ticks_played);
	theater_player.ended = TRUE;
	return 0;
}

/* an action fit to take: finite, its choices in range */
static boolean theater_action_valid(
	struct player_action const *action)
{
	return valid_real(action->desired_facing.yaw) && valid_real(action->desired_facing.pitch) &&
		valid_real(action->throttle.i) && valid_real(action->throttle.j) && valid_real(action->primary_trigger) &&
		(action->desired_weapon_index == NONE ||
			(action->desired_weapon_index >= 0 && action->desired_weapon_index < MAXIMUM_WEAPONS_PER_UNIT)) &&
		(action->desired_grenade_index == NONE ||
			(action->desired_grenade_index >= 0 && action->desired_grenade_index < NUMBER_OF_UNIT_GRENADE_TYPES)) &&
		(action->desired_zoom_level == NONE || action->desired_zoom_level >= 0);
}

/* ---------- public code: recording */

void theater_record_begin(
	struct network_game const *game,
	short machine_index,
	long random_seed)
{
	struct theater_film_header header;
	short connection = game_connection();

	theater_record_end();
	if (theater_playing() || !config_boolean("game.record_films") ||
		(connection != _game_connection_network_client && connection != _game_connection_network_server))
	{
		return;
	}
	/* (the host's film machine has a slot of its own: none while a machine
	that joined before films were recorded has it) */
	if (connection == _game_connection_network_server && !network_distributed_film_machine_free())
	{
		platform_log("theater: machine %d is in the slot of the film's: no film of this game", THEATER_FILM_MACHINE);
		return;
	}
	if (!theater_film_new_path(game->map.name, theater_recorder.path, sizeof(theater_recorder.path)))
	{
		platform_log("theater: no name for a new film in %s", platform_films_root());
		return;
	}
	theater_recorder.file = fopen(theater_recorder.path, "wb");
	if (!theater_recorder.file)
	{
		platform_log("theater: could not make the film %s", theater_recorder.path);
		return;
	}
	csmemset(&header, 0, sizeof(header));
	csmemcpy(header.signature, theater_film_signature, sizeof(header.signature));
	header.version = THEATER_FILM_VERSION;
	header.network_version = HALO_PORT_NETWORK_VERSION;
	header.game_size = sizeof(*game);
	header.machine_index = machine_index;
	header.random_seed = random_seed;
	header.start_time = game_time_get();
	header.created = (unsigned long)time(NULL);
	theater_recorder.host = connection == _game_connection_network_server;
	SET_FLAG(header.flags, _theater_film_host_bit, theater_recorder.host);
	csstrncpy(header.map_name, game->map.name, sizeof(header.map_name) - 1);
	if (fwrite(&header, sizeof(header), 1, theater_recorder.file) != 1 ||
		fwrite(game, sizeof(*game), 1, theater_recorder.file) != 1)
	{
		platform_log("theater: could not write the film %s", theater_recorder.path);
		fclose(theater_recorder.file);
		theater_recorder.file = NULL;
		remove(theater_recorder.path);
		return;
	}
	theater_recorder.ticks = 0;
	theater_recorder.chunk_ticks = 0;
	theater_recorder.chunk_first_tick = 0;
	theater_recorder.size = 0;
	/* (the host's film is sent what a machine of its own with no players
	would be: network_distributed.c) */
	if (theater_recorder.host)
		network_distributed_film_machine_loaded();
	platform_log("theater: recording %s (machine %d%s)", theater_recorder.path, machine_index,
		theater_recorder.host ? ", the host" : "");
}

void theater_record_end(
	void)
{
	long ticks = theater_recorder.ticks;

	if (!theater_recorder.file)
		return;
	theater_record_flush();
	if (theater_recorder.host)
		network_distributed_film_machine_left();
	if (!theater_recorder.file)
		return;
	if (fseek(theater_recorder.file, (long)offsetof(struct theater_film_header, ticks), SEEK_SET) != 0 ||
		fwrite(&ticks, sizeof(ticks), 1, theater_recorder.file) != 1)
	{
		platform_log("theater: could not finish the film %s", theater_recorder.path);
	}
	fclose(theater_recorder.file);
	theater_recorder.file = NULL;
	if (!ticks)
	{
		remove(theater_recorder.path);
		platform_log("theater: %s had no ticks; removed", theater_recorder.path);
		return;
	}
	platform_log("theater: wrote %s (%ld ticks)", theater_recorder.path, ticks);
}

boolean theater_recording(
	void)
{
	return theater_recorder.file != NULL;
}

boolean theater_film_machine_reserved(
	void)
{
	return config_boolean("game.record_films") != 0;
}

boolean theater_film_machine_active(
	void)
{
	return theater_recorder.file != NULL && theater_recorder.host;
}

void theater_record_message(
	void const *message,
	long size,
	boolean unreliable)
{
	byte *record;
	word header;
	word record_message_size = (word)size;

	if (!theater_recorder.file || size < (long)sizeof(word) + 1 || size > THEATER_MESSAGE_MAXIMUM_SIZE)
		return;
	/* (not the game's own messages that end it or look for games: the film
	ends with the game) */
	csmemcpy(&header, message, sizeof(header));
	if (GET_MESSAGE_TYPE(header) == _message_type_packet)
	{
		byte packet_type = ((byte const *)message)[size - 1];

		if (packet_type == _message_server_switch_to_pregame ||
			packet_type == _message_server_graceful_game_exit_pregame ||
			packet_type == _message_server_graceful_game_exit_postgame ||
			packet_type == _message_server_game_advertise ||
			packet_type == _message_server_pong)
		{
			return;
		}
	}
	record = theater_record_room(THEATER_MESSAGE_RECORD_OVERHEAD + size);
	if (!record)
		return;
	record[0] = _theater_record_message;
	record[1] = (byte)(unreliable ? FLAG(_theater_message_datagram_bit) : 0);
	csmemcpy(record + 2, &record_message_size, sizeof(record_message_size));
	csmemcpy(record + THEATER_MESSAGE_RECORD_OVERHEAD, message, size);
	theater_recorder.size += THEATER_MESSAGE_RECORD_OVERHEAD + size;
}

void theater_record_read(
	void const *message,
	long size,
	boolean unreliable)
{
	word header;

	if (!theater_recorder.file || size < (long)sizeof(word))
		return;
	/* (the host's own client is sent the distributed netcode's reliable
	messages too, which the film machine had from the host already) */
	csmemcpy(&header, message, sizeof(header));
	if (theater_recorder.host && GET_MESSAGE_TYPE(header) == _theater_message_type_data)
		return;
	theater_record_message(message, size, unreliable);
}

void theater_record_tick(
	struct player_action const *actions,
	short action_count)
{
	byte *record;
	long tick_time = game_time_get();
	short local_player_index;
	byte count = 0;

	if (!theater_recorder.file)
		return;
	record = theater_record_room(THEATER_TICK_RECORD_OVERHEAD + MAXIMUM_LOCAL_PLAYERS * THEATER_TICK_PLAYER_SIZE);
	if (!record)
		return;
	record[0] = _theater_record_tick;
	csmemcpy(record + 1, &tick_time, sizeof(tick_time));
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		short absolute_index = player_index != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
		byte *entry = record + THEATER_TICK_RECORD_OVERHEAD + count * THEATER_TICK_PLAYER_SIZE;

		if (absolute_index == NONE || absolute_index >= action_count)
			continue;
		entry[0] = (byte)local_player_index;
		csmemcpy(entry + 1, &actions[absolute_index], sizeof(actions[absolute_index]));
		count++;
	}
	record[5] = count;
	theater_recorder.size += THEATER_TICK_RECORD_OVERHEAD + count * THEATER_TICK_PLAYER_SIZE;
	theater_recorder.ticks++;
	if (++theater_recorder.chunk_ticks >= THEATER_CHUNK_TICKS)
		theater_record_flush();
}

/* ---------- public code: the films */

short theater_films_list(
	struct theater_film_entry *entries,
	short maximum)
{
	static struct theater_film_entry entry;
	char const *root = platform_films_root();
	void *directory = root && *root ? posix_directory_open(root) : NULL;
	char name[256];
	short count = 0;

	if (!directory)
		return 0;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);
		char path[512];
		short place;

		if (length <= 5 || strcmp(name + length - 5, ".film") != 0 || name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", root, name);
		if (!theater_film_describe(path, &entry))
			continue;
		/* (the newest first; past the most, the oldest left out) */
		for (place = count; place > 0 && entries[place - 1].created < entry.created; place--)
		{
		}
		if (place >= maximum)
			continue;
		if (count < maximum)
			count++;
		csmemmove(&entries[place + 1], &entries[place], (count - 1 - place) * sizeof(entries[0]));
		entries[place] = entry;
	}
	posix_directory_close(directory);
	return count;
}

/* ---------- public code: playback */

boolean theater_playing(
	void)
{
	return theater_player.file != NULL;
}

boolean theater_film_open(
	char const *path,
	struct network_game *game,
	short *machine_index,
	long *random_seed)
{
	struct theater_film_header *header = &theater_player.header;

	theater_film_close();
	theater_player.file = fopen(path, "rb");
	if (!theater_player.file)
	{
		platform_log("theater: could not open the film %s", path);
		return FALSE;
	}
	if (fread(header, sizeof(*header), 1, theater_player.file) != 1 ||
		csmemcmp(header->signature, theater_film_signature, sizeof(header->signature)) != 0)
	{
		platform_log("theater: %s is not a film", path);
		theater_film_close();
		return FALSE;
	}
	if (header->version != THEATER_FILM_VERSION || header->network_version != HALO_PORT_NETWORK_VERSION ||
		header->game_size != sizeof(*game))
	{
		platform_log("theater: %s was made by another build (film version %lu, network version %lu); "
			"this one plays films of version %d, network version %d", path, header->version,
			header->network_version, THEATER_FILM_VERSION, HALO_PORT_NETWORK_VERSION);
		theater_film_close();
		return FALSE;
	}
	if (header->machine_index < 0 || header->machine_index >= HALO_PORT_MAXIMUM_NETWORK_MACHINES ||
		header->start_time < 0 || header->start_time > 0x3FFFFFFF ||
		fread(game, sizeof(*game), 1, theater_player.file) != 1)
	{
		platform_log("theater: %s is damaged", path);
		theater_film_close();
		return FALSE;
	}
	header->map_name[sizeof(header->map_name) - 1] = 0;
	if (header->ticks <= 0)
		header->ticks = theater_film_count_ticks(theater_player.file);
	*machine_index = (short)header->machine_index;
	*random_seed = header->random_seed;
	theater_player.size = 0;
	theater_player.cursor = 0;
	theater_player.ticks_played = 0;
	theater_player.ended = FALSE;
	theater_player.time_told = FALSE;
	csmemset(theater_player.action_valid, 0, sizeof(theater_player.action_valid));
	platform_log("theater: playing %s (%s, %ld ticks, machine %ld%s)", path, header->map_name, header->ticks,
		header->machine_index, TEST_FLAG(header->flags, _theater_film_host_bit) ? ", the host" : "");
	return TRUE;
}

void theater_film_close(
	void)
{
	if (theater_player.file)
		fclose(theater_player.file);
	theater_player.file = NULL;
	theater_player.ended = TRUE;
}

long theater_film_start_time(
	void)
{
	return theater_player.file ? theater_player.header.start_time : 0;
}

boolean theater_film_read(
	void *buffer,
	word *size,
	boolean *unreliable)
{
	long record_size;
	byte const *record;
	word message_size;

	if (!theater_player.file || theater_player_peek(&record_size) != _theater_record_message)
		return FALSE;
	record = theater_player.data + theater_player.cursor;
	csmemcpy(&message_size, record + 2, sizeof(message_size));
	/* (a message larger than the reader's buffer is passed over) */
	theater_player.cursor += record_size;
	if (message_size > *size)
	{
		platform_log("theater: a message of %d bytes in the film is too large; passed over", message_size);
		*size = 0;
		*unreliable = FALSE;
		return FALSE;
	}
	csmemcpy(buffer, record + THEATER_MESSAGE_RECORD_OVERHEAD, message_size);
	*size = message_size;
	*unreliable = TEST_FLAG(record[1], _theater_message_datagram_bit);
	return TRUE;
}

void theater_film_tick(
	void)
{
	long record_size;
	byte const *record;
	long tick_time;
	short index;

	csmemset(theater_player.action_valid, 0, sizeof(theater_player.action_valid));
	if (!theater_player.file)
		return;
	/* (the messages before the tick, which the frame's reading has not had
	when a frame runs more than one) */
	network_game_client_port_read_film_messages();
	if (theater_player_peek(&record_size) != _theater_record_tick)
		return;
	record = theater_player.data + theater_player.cursor;
	theater_player.cursor += record_size;
	theater_player.ticks_played++;
	csmemcpy(&tick_time, record + 1, sizeof(tick_time));
	if (tick_time != game_time_get() && !theater_player.time_told)
	{
		theater_player.time_told = TRUE;
		platform_log("theater: the film's tick %ld was at game time %ld, played at %ld", theater_player.ticks_played,
			tick_time, game_time_get());
	}
	for (index = 0; index < record[5]; index++)
	{
		byte const *entry = record + THEATER_TICK_RECORD_OVERHEAD + index * THEATER_TICK_PLAYER_SIZE;
		struct player_action action;

		csmemcpy(&action, entry + 1, sizeof(action));
		if (entry[0] < MAXIMUM_LOCAL_PLAYERS && theater_action_valid(&action))
		{
			theater_player.actions[entry[0]] = action;
			theater_player.action_valid[entry[0]] = TRUE;
		}
	}
}

boolean theater_film_action(
	short local_player_index,
	struct player_action *action)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		!theater_player.action_valid[local_player_index])
	{
		return FALSE;
	}
	*action = theater_player.actions[local_player_index];
	return TRUE;
}

boolean theater_film_from_host(
	void)
{
	return theater_player.file && TEST_FLAG(theater_player.header.flags, _theater_film_host_bit);
}

boolean theater_film_tick_next(
	void)
{
	long record_size;

	return theater_player.file && theater_player_peek(&record_size) == _theater_record_tick;
}

boolean theater_film_ended(
	void)
{
	long record_size;

	return !theater_player.file || theater_player_peek(&record_size) == 0;
}

long theater_film_ticks_played(
	void)
{
	return theater_player.ticks_played;
}

long theater_film_ticks_total(
	void)
{
	return theater_player.file ? theater_player.header.ticks : 0;
}
