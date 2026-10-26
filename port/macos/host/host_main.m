/*
HOST_MAIN.M

Entry point of the macOS port (OpenCE.app's executable).

It loads the guest image (the game, built as ILP32 code: tools/macos_build.py)
from the app's resources, gives the game an environment and runs its
main() on the process's main thread, which SDL and Cocoa need for the
window and its events, on a stack in guest memory (host_thread.c).

The game's own files - config.toml, the maps folder it extracts from a disc
image at the first start, the log - go in ~/Library/Application
Support/OpenCE: to the guest, that is the folder its executable is in
(/proc/self/exe, host_syscall.c; SDL_GetBasePath, host_sdl.c), which is where
the desktop ports keep them (port/linux/src/port_config.c, xbox_files.c).
Started from a folder holding maps (or assets/maps, as the repository's
root), the game plays those instead, as the Linux build does.
*/

#import <Foundation/Foundation.h>

#include "host.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <pthread.h>
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

static void folders_find(void)
{
	char executable[1024];
	uint32_t size = sizeof(executable);
	char *slash;
	const char *home = getenv("HOME");

	@autoreleasepool
	{
		NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);

		if (paths.count)
			snprintf(support_directory, sizeof(support_directory), "%s/OpenCE", [paths[0] fileSystemRepresentation]);
		else
			snprintf(support_directory, sizeof(support_directory), "%s/Library/Application Support/OpenCE",
				home ? home : "");
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
	host_logf(HOST_LOG_INFO, "OpenCE for macOS: the game's files are in %s", support_directory);
	host_run_guest_main(make_boot(argc, argv));
}
