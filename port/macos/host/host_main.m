/*
HOST_MAIN.M

Entry point of the macOS port (CoreCE.app's executable).

It loads the guest image (the game, built as ILP32 code: tools/macos_build.py)
from the app's resources, gives the game an environment and runs its
main() on the process's main thread, which SDL and Cocoa need for the
window and its events, on a stack in guest memory (host_thread.c).

The game's own files - config.toml, the maps folder it extracts from a disc
image at the first start, the saves, the log - go in ~/Library/Application
Support/CoreCE, as CoreCE's own Mac app kept them (the first start copies
an earlier app's folder there: support_migrate). To the guest, that is the
folder its executable is in
(/proc/self/exe, host_syscall.c; SDL_GetBasePath, host_sdl.c), which is where
the desktop ports keep them (port/linux/src/port_config.c, xbox_files.c).
Started from a folder holding maps (or assets/maps, as the repository's
root), the game plays those instead, as the Linux build does.
*/

#import <Foundation/Foundation.h>

#include "host.h"

#include <SDL3/SDL.h>
#include <copyfile.h>
#include <dirent.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <removefile.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---------- logging and termination */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	pthread_mutex_lock(&log_lock);
	fprintf(stderr, "%s", priority >= HOST_LOG_ERROR ? "host error: " : priority == HOST_LOG_WARN ? "host warning: " :
		"host: ");
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	pthread_mutex_unlock(&log_lock);
}

void host_log(int priority, const char *text)
{
	host_logf(priority, "%s", text);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_logf(HOST_LOG_ERROR, "%s", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo", message, NULL);
	_exit(1);
}

void host_abort(const char *reason)
{
	host_logf(HOST_LOG_ERROR, "guest abort: %s", reason);
	abort();
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	fflush(stderr);
	SDL_Quit();
	_exit(code);
}

/* ---------- folders */

static char support_directory[1024];
static char resource_directory[1024];

const char *host_support_directory(void)
{
	return support_directory;
}

const char *host_resource_directory(void)
{
	return resource_directory;
}

/* the folders the earlier apps kept their files in, tried in this order:
this app's before it was named CoreCE (with its saves in saves/), then
CoreCE's own earlier names */
static const char *const legacy_folders[] = { "OpenCE", "Halo OG", "Halo CE Universal" };

/* copies the folder from into to, which is made if need be: regular files
and folders only (a link is not followed), and a file already in to is
kept. Each file is a clone where the volume can make one (APFS), which takes
no room. skip names one entry of from not to copy. Returns nonzero if every
file was copied */
static int folder_copy(const char *from, const char *to, const char *skip)
{
	DIR *directory = opendir(from);
	struct dirent *entry;
	int succeeded = 1;

	if (!directory)
		return 0;
	if (mkdir(to, 0755) != 0 && errno != EEXIST)
	{
		closedir(directory);
		return 0;
	}
	while ((entry = readdir(directory)) != NULL)
	{
		char source[2048], destination[2048];
		struct stat information;

		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || (skip && !strcmp(entry->d_name, skip)))
			continue;
		if (snprintf(source, sizeof(source), "%s/%s", from, entry->d_name) >= (int)sizeof(source) ||
			snprintf(destination, sizeof(destination), "%s/%s", to, entry->d_name) >= (int)sizeof(destination) ||
			lstat(source, &information) != 0)
		{
			succeeded = 0;
			continue;
		}
		if (S_ISDIR(information.st_mode))
			succeeded &= folder_copy(source, destination, NULL);
		else if (S_ISREG(information.st_mode) && access(destination, F_OK) != 0 &&
			copyfile(source, destination, NULL, COPYFILE_CLONE | COPYFILE_EXCL | COPYFILE_NOFOLLOW) != 0)
		{
			host_logf(HOST_LOG_WARN, "could not copy %s: %s", source, strerror(errno));
			succeeded = 0;
		}
	}
	closedir(directory);
	return succeeded;
}

/* the first start of a CoreCE app: the files of the newest earlier folder
there is are copied into the new one (as a folder of its own, given the
name only once it is whole); the earlier folder stays as it was. Saves
that were in its saves/ go to the folder itself, where CoreCE keeps them,
before the rest, so a file the CoreCE app had there too is this app's.
Returns the earlier folder when it could not be copied, for this start to
go on using, else NULL */
static const char *support_migrate(const char *parent)
{
	static char legacy[1024];
	char partial[1100], saves[1100];
	struct stat information;
	int index;

	for (index = 0; index < (int)(sizeof(legacy_folders) / sizeof(legacy_folders[0])); index++)
	{
		snprintf(legacy, sizeof(legacy), "%s/%s", parent, legacy_folders[index]);
		if (lstat(legacy, &information) == 0 && S_ISDIR(information.st_mode))
			break;
	}
	if (index == (int)(sizeof(legacy_folders) / sizeof(legacy_folders[0])))
		return NULL;
	snprintf(partial, sizeof(partial), "%s.partial", support_directory);
	removefile(partial, NULL, REMOVEFILE_RECURSIVE);
	host_logf(HOST_LOG_INFO, "copying %s to %s (it stays as it is)", legacy, support_directory);
	snprintf(saves, sizeof(saves), "%s/saves", legacy);
	if ((lstat(saves, &information) != 0 || !S_ISDIR(information.st_mode) || folder_copy(saves, partial, NULL)) &&
		folder_copy(legacy, partial, "saves") && rename(partial, support_directory) == 0)
	{
		return NULL;
	}
	host_logf(HOST_LOG_WARN, "%s could not be copied whole; using it as it is (the next start tries again)", legacy);
	removefile(partial, NULL, REMOVEFILE_RECURSIVE);
	return legacy;
}

static void folders_find(void)
{
	char executable[1024];
	char parent[1024];
	uint32_t size = sizeof(executable);
	char *slash;
	const char *home = getenv("HOME");
	const char *legacy = NULL;
	struct stat information;

	@autoreleasepool
	{
		NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);

		if (paths.count)
			snprintf(parent, sizeof(parent), "%s", [paths[0] fileSystemRepresentation]);
		else
			snprintf(parent, sizeof(parent), "%s/Library/Application Support", home ? home : "");
	}
	snprintf(support_directory, sizeof(support_directory), "%s/CoreCE", parent);
	if (lstat(support_directory, &information) != 0 && errno == ENOENT)
		legacy = support_migrate(parent);
	if (legacy)
	{
		char saves[1100];

		/* (the earlier folder this once; its saves where that app had them) */
		snprintf(support_directory, sizeof(support_directory), "%s", legacy);
		snprintf(saves, sizeof(saves), "%s/saves", legacy);
		setenv("HALO_SAVE_ROOT", lstat(saves, &information) == 0 ? saves : legacy, 0);
	}
	mkdir(support_directory, 0755);

	/* the image beside the executable's folder (Contents/Resources), or for
	the build's own host, build/macos/halo_guest.elf */
	if (_NSGetExecutablePath(executable, &size) != 0)
		executable[0] = 0;
	slash = strrchr(executable, '/');
	if (slash)
		*slash = 0;
	snprintf(resource_directory, sizeof(resource_directory), "%s/../Resources", executable);
	{
		char path[1100];
		struct stat information;

		snprintf(path, sizeof(path), "%s/halo_guest.elf", resource_directory);
		if (stat(path, &information) != 0)
			snprintf(resource_directory, sizeof(resource_directory), "%s/..", executable);
		/* the opening film the app carries (port/assets/opening), which the
		game opens when the maps folder has none of its own
		(platform_open_opening_film, xbox_files.c) */
		snprintf(path, sizeof(path), "%s/opening_frames.bin", resource_directory);
		if (stat(path, &information) == 0)
			setenv("HALO_OPENING_FILM", path, 0);
	}
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 256

static void *file_read(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		data = malloc((size_t)length);
		if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* POSIX TZ for the current local offset (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	time_t now = time(NULL);
	struct tm local;
	long offset;

	localtime_r(&now, &local);
	offset = -local.tm_gmtoff;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

extern char **environ;

/* copies argv and the environment (the host's own, the HALO_* settings
among it, with the time zone) into guest memory */
static uint32_t make_boot(int argc, char **argv)
{
	size_t size = 0x40000;
	uint32_t memory = host_guest_map(size, PROT_READ | PROT_WRITE);
	uint8_t *base = GUEST_TO_HOST(uint8_t *, memory);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)base;
	uint32_t *guest_argv = (uint32_t *)(base + sizeof(*boot));
	uint32_t *guest_environment = guest_argv + 64;
	char *strings = (char *)(guest_environment + ENVIRONMENT_MAXIMUM + 2);
	char zone[64];
	int index, count = 0;

	if (!memory)
		host_fatal("cannot allocate the guest's environment");
#define PUT(text) do { \
		size_t put_length = strlen(text) + 1; \
		if ((uint8_t *)strings + put_length > base + size) \
			host_fatal("the environment is too large"); \
		memcpy(strings, text, put_length); \
		strings += put_length; \
	} while (0)
	for (index = 0; index < argc && index < 63; index++)
	{
		guest_argv[index] = HOST_TO_GUEST(strings);
		PUT(argv[index]);
	}
	guest_argv[index] = 0;
	boot->argc = (uint32_t)index;
	for (index = 0; environ[index] && count < ENVIRONMENT_MAXIMUM; index++)
	{
		if (!strncmp(environ[index], "TZ=", 3))
			continue;
		guest_environment[count++] = HOST_TO_GUEST(strings);
		PUT(environ[index]);
	}
	time_zone(zone, sizeof(zone));
	{
		char entry[96];

		snprintf(entry, sizeof(entry), "TZ=%s", zone);
		guest_environment[count++] = HOST_TO_GUEST(strings);
		PUT(entry);
	}
#undef PUT
	guest_environment[count] = 0;
	boot->argv = HOST_TO_GUEST(guest_argv);
	boot->environment = HOST_TO_GUEST(guest_environment);
	boot->page_size = (uint32_t)HOST_PAGE;
	return memory;
}

/* ---------- main */

int main(int argc, char *argv[])
{
	char path[1200];
	size_t image_size = 0;
	void *image;

	folders_find();
	host_install_signal_handlers();
	snprintf(path, sizeof(path), "%s/halo_guest.elf", resource_directory);
	image = file_read(path, &image_size);
	if (!image)
		host_fatal("cannot read the game image %s: %s", path, strerror(errno));
	if (host_load_image(image, image_size) != 0)
		host_fatal("cannot load the game image %s", path);
	free(image);
	/* internet play's MQTT brokers (network.brokers_file): the app's list,
	written beside config.toml at each start, as a desktop update replaces
	the file beside its game */
	{
		char source[1200], destination[1200];
		size_t brokers_size = 0;
		void *brokers;

		snprintf(source, sizeof(source), "%s/brokers.txt", resource_directory);
		snprintf(destination, sizeof(destination), "%s/brokers.txt", support_directory);
		brokers = file_read(source, &brokers_size);
		if (brokers)
		{
			FILE *file = fopen(destination, "wb");

			if (file)
			{
				fwrite(brokers, 1, brokers_size, file);
				fclose(file);
			}
			free(brokers);
		}
	}
	/* a run from Finder starts in /: the game's files are in the support
	folder then */
	{
		char cwd[1024];

		if (getcwd(cwd, sizeof(cwd)) && !strcmp(cwd, "/"))
			chdir(support_directory);
	}
	host_logf(HOST_LOG_INFO, "CoreCE for macOS: the game's files are in %s", support_directory);
	host_run_guest_main(make_boot(argc, argv));
}
