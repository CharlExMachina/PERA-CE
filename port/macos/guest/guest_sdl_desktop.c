/*
GUEST_SDL_DESKTOP.C

The SDL3 functions the desktop's platform layer calls (sdl_platform.c,
xinput_sdl.c, menu_files.c, port_config.c) that the Android guest's does not,
for the macOS guest: the window's size and fullscreen, the displays and
their modes, the mouse's grab and the cursor, the keyboard's focus and keys,
the open file dialog, the audio devices' names and the small renderer of the
disc image's extraction. The rest are in
port/android/guest/runtime/guest_sdl.c.

As there, SDL's objects are handles. What SDL hands back in memory of its own
(a display mode, a list of names) is copied into the guest's: a display
mode's layout differs between the two ABIs (macos_host.h). What needs no
SDL (atomics, properties nothing reads back, the clock, a file's
information) is done here.
*/

/* before SDL: musl's alloca.h must come first */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "guest_host.h"
#include "macos_host.h"

/* ---------- what needs no host */

int SDL_AddAtomicInt(SDL_AtomicInt *a, int v)
{
	return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST);
}

int SDL_GetAtomicInt(SDL_AtomicInt *a)
{
	return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

int SDL_SetAtomicInt(SDL_AtomicInt *a, int v)
{
	return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST);
}

Uint64 SDL_GetTicksNS(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (Uint64)now.tv_sec * 1000000000ULL + (Uint64)now.tv_nsec;
}

void SDL_DelayPrecise(Uint64 ns)
{
	struct timespec duration;

	duration.tv_sec = (time_t)(ns / 1000000000ULL);
	duration.tv_nsec = (long)(ns % 1000000000ULL);
	nanosleep(&duration, NULL);
}

/* (the text input's properties, which only a screen keyboard reads: the Mac
has none) */
SDL_PropertiesID SDL_CreateProperties(void)
{
	return 1;
}

void SDL_DestroyProperties(SDL_PropertiesID props)
{
	(void)props;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value)
{
	(void)props;
	(void)name;
	(void)value;
	return true;
}

bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value)
{
	(void)props;
	(void)name;
	(void)value;
	return true;
}

/* (the window's properties, which only display.hdr_output reads: a Metal
window is SDR, so it has none, and each is its default) */
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window)
{
	(void)window;
	return 0;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value)
{
	(void)props;
	(void)name;
	return default_value;
}

float SDL_GetFloatProperty(SDL_PropertiesID props, const char *name, float default_value)
{
	(void)props;
	(void)name;
	return default_value;
}

/* (OpenGL's window attributes, which display.hdr_output sets before the
window is made: a Metal window has none) */
bool SDL_GL_SetAttribute(SDL_GLAttr attribute, int value)
{
	(void)attribute;
	(void)value;
	return true;
}

/* ---------- the window */

void SDL_DestroyWindow(SDL_Window *window)
{
	host_sdl_destroy_window((unsigned int)window);
}

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window)
{
	return (SDL_WindowFlags)host_sdl_window_flags((unsigned int)window);
}

bool SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
	int width = 0, height = 0;

	host_sdl_window_size((unsigned int)window, &width, &height);
	if (w)
		*w = width;
	if (h)
		*h = height;
	return width > 0;
}

bool SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
	return host_sdl_set_window_size((unsigned int)window, w, h) != 0;
}

bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen)
{
	return host_sdl_set_window_fullscreen((unsigned int)window, fullscreen) != 0;
}

void SDL_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
	host_sdl_warp_mouse((unsigned int)window, x, y);
}

bool SDL_SetWindowMouseGrab(SDL_Window *window, bool grabbed)
{
	return host_sdl_set_window_mouse_grab((unsigned int)window, grabbed) != 0;
}

/* ---------- displays */

static void mode_from_fields(const int *fields, SDL_DisplayMode *mode)
{
	memset(mode, 0, sizeof(*mode));
	mode->displayID = (SDL_DisplayID)fields[0];
	mode->format = (SDL_PixelFormat)fields[1];
	mode->w = fields[2];
	mode->h = fields[3];
	memcpy(&mode->pixel_density, &fields[4], sizeof(float));
	memcpy(&mode->refresh_rate, &fields[5], sizeof(float));
	mode->refresh_rate_numerator = fields[6];
	mode->refresh_rate_denominator = fields[7];
}

static void fields_from_mode(const SDL_DisplayMode *mode, int *fields)
{
	fields[0] = (int)mode->displayID;
	fields[1] = (int)mode->format;
	fields[2] = mode->w;
	fields[3] = mode->h;
	memcpy(&fields[4], &mode->pixel_density, sizeof(float));
	memcpy(&fields[5], &mode->refresh_rate, sizeof(float));
	fields[6] = mode->refresh_rate_numerator;
	fields[7] = mode->refresh_rate_denominator;
}

bool SDL_SetWindowFullscreenMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	int fields[HOST_SDL_MODE_FIELDS];

	if (!mode)
		return host_sdl_set_window_fullscreen_mode((unsigned int)window, NULL) != 0;
	fields_from_mode(mode, fields);
	return host_sdl_set_window_fullscreen_mode((unsigned int)window, fields) != 0;
}

SDL_DisplayID SDL_GetPrimaryDisplay(void)
{
	return (SDL_DisplayID)host_sdl_primary_display();
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window)
{
	return (SDL_DisplayID)host_sdl_display_for_window((unsigned int)window);
}

/* (each kept until it is asked for again, as the callers read it at once) */
static const SDL_DisplayMode *display_mode(SDL_DisplayID displayID, int which)
{
	static __thread SDL_DisplayMode modes[2];
	int fields[HOST_SDL_MODE_FIELDS];

	if (!host_sdl_display_mode(displayID, which, fields))
		return NULL;
	mode_from_fields(fields, &modes[which]);
	return &modes[which];
}

const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID displayID)
{
	return display_mode(displayID, 0);
}

const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID displayID)
{
	return display_mode(displayID, 1);
}

#define MAXIMUM_DISPLAY_MODES 256

/* (one allocation, freed with SDL_free, as SDL's: the pointers, then the
modes) */
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID displayID, int *count)
{
	int *fields = malloc(MAXIMUM_DISPLAY_MODES * HOST_SDL_MODE_FIELDS * sizeof(int));
	int found, index;
	SDL_DisplayMode **list;
	SDL_DisplayMode *modes;

	if (count)
		*count = 0;
	if (!fields)
		return NULL;
	found = host_sdl_fullscreen_modes(displayID, fields, MAXIMUM_DISPLAY_MODES);
	list = malloc((size_t)(found + 1) * sizeof(*list) + (size_t)found * sizeof(*modes));
	if (!list)
	{
		free(fields);
		return NULL;
	}
	modes = (SDL_DisplayMode *)(list + found + 1);
	for (index = 0; index < found; index++)
	{
		mode_from_fields(fields + index * HOST_SDL_MODE_FIELDS, &modes[index]);
		list[index] = &modes[index];
	}
	list[found] = NULL;
	free(fields);
	if (count)
		*count = found;
	return list;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID displayID, int w, int h, float refresh_rate,
	bool include_high_density_modes, SDL_DisplayMode *closest)
{
	int fields[HOST_SDL_MODE_FIELDS];

	if (!host_sdl_closest_fullscreen_mode(displayID, w, h, refresh_rate, include_high_density_modes, fields))
		return false;
	mode_from_fields(fields, closest);
	return true;
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID displayID, SDL_Rect *rect)
{
	int rectangle[4];

	if (!host_sdl_display_usable_bounds(displayID, rectangle))
		return false;
	rect->x = rectangle[0];
	rect->y = rectangle[1];
	rect->w = rectangle[2];
	rect->h = rectangle[3];
	return true;
}

/* ---------- events and input */

void SDL_PumpEvents(void)
{
	host_sdl_pump_events();
}

/* (only events without pointers: the platform layer pushes SDL_EVENT_QUIT) */
bool SDL_PushEvent(SDL_Event *event)
{
	return host_sdl_push_event(event) != 0;
}

bool SDL_GetHintBoolean(const char *name, bool default_value)
{
	return host_sdl_hint_boolean(name, default_value) != 0;
}

bool SDL_TextInputActive(SDL_Window *window)
{
	return host_sdl_text_input_active((unsigned int)window) != 0;
}

bool SDL_StartTextInputWithProperties(SDL_Window *window, SDL_PropertiesID props)
{
	(void)props;
	return host_sdl_start_text_input((unsigned int)window) != 0;
}

bool SDL_StopTextInput(SDL_Window *window)
{
	return host_sdl_stop_text_input((unsigned int)window) != 0;
}

bool SDL_HasScreenKeyboardSupport(void)
{
	return host_sdl_has_screen_keyboard() != 0;
}

SDL_Window *SDL_GetKeyboardFocus(void)
{
	return (SDL_Window *)host_sdl_keyboard_focus();
}

bool SDL_ShowCursor(void)
{
	return host_sdl_show_cursor(1) != 0;
}

bool SDL_HideCursor(void)
{
	return host_sdl_show_cursor(0) != 0;
}

SDL_Keycode SDL_GetKeyFromScancode(SDL_Scancode scancode, SDL_Keymod modstate, bool key_event)
{
	return (SDL_Keycode)host_sdl_key_from_scancode_state((int)scancode, (int)modstate, key_event);
}

/* ---------- audio devices */

#define MAXIMUM_AUDIO_DEVICES 64

static SDL_AudioDeviceID *audio_devices(int recording, int *count)
{
	unsigned int ids[MAXIMUM_AUDIO_DEVICES];
	int found = host_sdl_audio_devices(recording, ids, MAXIMUM_AUDIO_DEVICES);
	SDL_AudioDeviceID *result = malloc((size_t)(found + 1) * sizeof(*result));
	int index;

	if (count)
		*count = 0;
	if (!result)
		return NULL;
	for (index = 0; index < found; index++)
		result[index] = ids[index];
	result[found] = 0;
	if (count)
		*count = found;
	return result;
}

SDL_AudioDeviceID *SDL_GetAudioPlaybackDevices(int *count)
{
	return audio_devices(0, count);
}

SDL_AudioDeviceID *SDL_GetAudioRecordingDevices(int *count)
{
	return audio_devices(1, count);
}

/* (SDL keeps a device's name for as long as the device: so does this, a few
of them) */
const char *SDL_GetAudioDeviceName(SDL_AudioDeviceID devid)
{
	static struct
	{
		SDL_AudioDeviceID id;
		char name[128];
	} names[MAXIMUM_AUDIO_DEVICES];
	static unsigned int next;
	unsigned int index;

	for (index = 0; index < MAXIMUM_AUDIO_DEVICES; index++)
	{
		if (names[index].id == devid && devid)
			return names[index].name;
	}
	index = next++ % MAXIMUM_AUDIO_DEVICES;
	if (!host_sdl_audio_device_name(devid, names[index].name, sizeof(names[index].name)))
		return NULL;
	names[index].id = devid;
	return names[index].name;
}

/* ---------- files */

/* (the guest's own stat, whose system call the host makes on the Mac's
files) */
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
	struct stat status;

	if (!path || stat(path, &status) != 0)
		return false;
	if (info)
	{
		memset(info, 0, sizeof(*info));
		info->type = S_ISREG(status.st_mode) ? SDL_PATHTYPE_FILE :
			S_ISDIR(status.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_OTHER;
		info->size = (Uint64)status.st_size;
		info->create_time = SDL_SECONDS_TO_NS(status.st_ctim.tv_sec) + status.st_ctim.tv_nsec;
		info->modify_time = SDL_SECONDS_TO_NS(status.st_mtim.tv_sec) + status.st_mtim.tv_nsec;
		info->access_time = SDL_SECONDS_TO_NS(status.st_atim.tv_sec) + status.st_atim.tv_nsec;
	}
	return true;
}

const char *SDL_GetBasePath(void)
{
	static char path[1024];

	if (!path[0])
		host_sdl_base_path(path, sizeof(path));
	return path;
}

#define GLOB_BUFFER_SIZE (256 * 1024)

/* (one allocation, freed with SDL_free, as SDL's: the pointers, then the
names) */
char **SDL_GlobDirectory(const char *path, const char *pattern, SDL_GlobFlags flags, int *count)
{
	char *buffer = malloc(GLOB_BUFFER_SIZE);
	int found, index;
	char **list;
	char *names, *name;
	size_t length = 0;

	if (count)
		*count = 0;
	if (!buffer)
		return NULL;
	found = host_sdl_glob_directory(path, pattern, (unsigned int)flags, buffer, GLOB_BUFFER_SIZE);
	if (found < 0)
	{
		free(buffer);
		return NULL;
	}
	for (index = 0, name = buffer; index < found; index++)
	{
		length += strlen(name) + 1;
		name += strlen(name) + 1;
	}
	list = malloc((size_t)(found + 1) * sizeof(*list) + length);
	if (!list)
	{
		free(buffer);
		return NULL;
	}
	names = (char *)(list + found + 1);
	memcpy(names, buffer, length);
	for (index = 0, name = names; index < found; index++)
	{
		list[index] = name;
		name += strlen(name) + 1;
	}
	list[found] = NULL;
	free(buffer);
	if (count)
		*count = found;
	return list;
}

/* (waited for, and answered on this thread before it returns, as the
platform layer waits for it anyway) */
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
	const SDL_DialogFileFilter *filters, int nfilters, const char *default_location, bool allow_many)
{
	char path[1024];
	const char *chosen[2] = { NULL, NULL };

	(void)window;
	(void)default_location;
	(void)allow_many;
	if (host_sdl_choose_file(nfilters > 0 ? filters[0].name : NULL, nfilters > 0 ? filters[0].pattern : NULL,
		nfilters > 1 ? filters[1].name : NULL, nfilters > 1 ? filters[1].pattern : NULL, path, sizeof(path)))
	{
		chosen[0] = path;
	}
	callback(userdata, chosen, 0);
}

/* ---------- the renderer */

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name)
{
	(void)name;
	return (SDL_Renderer *)host_sdl_create_renderer((unsigned int)window);
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
	host_sdl_destroy_renderer((unsigned int)renderer);
}

bool SDL_SetRenderVSync(SDL_Renderer *renderer, int vsync)
{
	return host_sdl_set_render_vsync((unsigned int)renderer, vsync) != 0;
}

bool SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
	return host_sdl_set_render_draw_color((unsigned int)renderer, r, g, b, a) != 0;
}

bool SDL_RenderClear(SDL_Renderer *renderer)
{
	return host_sdl_render_clear((unsigned int)renderer) != 0;
}

bool SDL_SetRenderScale(SDL_Renderer *renderer, float scaleX, float scaleY)
{
	return host_sdl_set_render_scale((unsigned int)renderer, scaleX, scaleY) != 0;
}

bool SDL_RenderDebugText(SDL_Renderer *renderer, float x, float y, const char *str)
{
	return host_sdl_render_debug_text((unsigned int)renderer, x, y, str) != 0;
}

bool SDL_RenderFillRect(SDL_Renderer *renderer, const SDL_FRect *rect)
{
	float rectangle[4];

	rectangle[0] = rect->x;
	rectangle[1] = rect->y;
	rectangle[2] = rect->w;
	rectangle[3] = rect->h;
	return host_sdl_render_fill_rect((unsigned int)renderer, rectangle) != 0;
}

bool SDL_RenderPresent(SDL_Renderer *renderer)
{
	return host_sdl_render_present((unsigned int)renderer) != 0;
}
