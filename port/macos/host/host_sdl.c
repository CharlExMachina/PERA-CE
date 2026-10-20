/*
HOST_SDL.C

SDL3 on behalf of the guest (port/android/guest/runtime/guest_sdl.c, and the
desktop's functions in port/macos/guest/guest_sdl_desktop.c), as
port/android/host/host_sdl.c does it. SDL objects are 64-bit pointers, which
the guest cannot hold; it gets small handles into the table here instead.
The pointers the guest passes arrive as host addresses (its import stubs
rebase them: tools/macos_imports.py).

The window, its events and the dialogs run on the process's main thread,
which is the game's (host_main.m). SDL's audio thread has no guest stack, so
the audio callback is handed to a thread that has one.
*/

#include "host.h"
#include "macos_host.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <string.h>

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_gamepad,
	_handle_audio,
	_handle_renderer,
};

struct handle
{
	int type;
	void *object;
	/* an audio stream with a callback: its binding (below) */
	void *binding;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			handles[index].binding = NULL;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* the object of a handle, which the handle no longer refers to; NULL when
there is none. *binding gets what handle_new's caller stored (audio) */
static void *handle_release(uint32_t handle, int type, void **binding)
{
	void *object = NULL;

	if (binding)
		*binding = NULL;
	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
	{
		object = handles[handle].object;
		if (binding)
			*binding = handles[handle].binding;
		handles[handle].type = _handle_free;
		handles[handle].object = NULL;
		handles[handle].binding = NULL;
	}
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	return SDL_Init((SDL_InitFlags)flags);
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value);
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_GetCurrentThreadID();
}

/* ---------- video */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	return handle_new(_handle_window, SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags));
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GetWindowSizeInPixels(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

/* the SDL window of a handle (host_metal.m's layer is the window's) */
SDL_Window *host_sdl_window(uint32_t window)
{
	return handle_get(window, _handle_window);
}

/* ---------- events */

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	if (!SDL_PollEvent(&host_event))
		return 0;
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = list[index];
	SDL_free(list);
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	SDL_Gamepad *gamepad = SDL_OpenGamepad((SDL_JoystickID)id);

	if (gamepad)
		host_logf(HOST_LOG_INFO, "gamepad %u: %s (type %d, %04x:%04x)", (unsigned)id, SDL_GetGamepadName(gamepad),
			(int)SDL_GetGamepadType(gamepad), SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
	return handle_new(_handle_gamepad, gamepad);
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

/* (the event thread, when the controller goes: sdl_platform.c) */
void host_sdl_close_gamepad(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_release(gamepad, _handle_gamepad, NULL);

	if (object)
		SDL_CloseGamepad(object);
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadAxis(object, (SDL_GamepadAxis)axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadButton(object, (SDL_GamepadButton)button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_GetGamepadType(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

/* ---------- audio */

/* SDL calls audio_callback on its own audio thread, which cannot run guest
code; it passes each request to the stream's thread (audio_thread), which
can, and waits for it to be done. SDL holds the stream's lock throughout, so
the audio the guest puts into the stream meanwhile (from audio_thread) is
kept in the binding's buffer instead, and put in by audio_callback once the
guest is done: audio_thread putting it in itself would wait for the lock
forever */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	/* the stream is destroyed: the thread frees the binding and ends */
	int quit;
	int additional;
	int total;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

/* the binding whose callback this thread is running, if any */
static __thread struct audio_binding *calling_back;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending && !binding->quit)
			pthread_cond_wait(&binding->requested, &binding->lock);
		if (binding->quit)
			break;
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		calling_back = binding;
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		calling_back = NULL;
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	pthread_mutex_unlock(&binding->lock);
	pthread_cond_destroy(&binding->done);
	pthread_cond_destroy(&binding->requested);
	pthread_mutex_destroy(&binding->lock);
	SDL_free(binding->buffer);
	SDL_free(binding);
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	pthread_mutex_unlock(&binding->lock);
	if (binding->buffer_length)
	{
		SDL_PutAudioStreamData(stream, binding->buffer, binding->buffer_length);
		binding->buffer_length = 0;
	}
}

/* audio the guest puts into its stream during the stream's callback, for
audio_callback to put in; 1 on success */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = SDL_realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding;
	SDL_AudioStream *stream;

	/* (one without a callback, the microphone's, which the guest reads:
	nothing to bind) */
	if (!callback)
	{
		stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec, NULL, NULL);
		return stream ? handle_new(_handle_audio, stream) : 0;
	}
	binding = SDL_calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec,
		callback ? audio_callback : NULL, binding);
	if (!stream)
	{
		SDL_free(binding);
		return 0;
	}
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, stream);
	if (!binding->handle)
	{
		SDL_DestroyAudioStream(stream);
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	pthread_mutex_lock(&handle_lock);
	handles[binding->handle].binding = binding;
	pthread_mutex_unlock(&handle_lock);
	if (host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	if (!object)
		return 0;
	if (calling_back && calling_back->handle == stream)
		return audio_keep(calling_back, data, length);
	return SDL_PutAudioStreamData(object, data, length);
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_ResumeAudioStreamDevice(object) : 0;
}

/* (voice chat's microphone: port/linux/src/voice_audio.c) */
int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamData(object, data, length) : -1;
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_GetAudioStreamAvailable(object) : -1;
}

/* (a stream with a callback has a thread the guest's callbacks run on:
once no callback can run any more, it is told to end, and frees the
binding and its stack as it goes) */
void host_sdl_destroy_audio_stream(uint32_t stream)
{
	void *context;
	SDL_AudioStream *object = handle_release(stream, _handle_audio, &context);
	struct audio_binding *binding = context;

	if (!object)
		return;
	/* (returns once a callback that is running has finished) */
	SDL_DestroyAudioStream(object);
	if (binding)
	{
		pthread_mutex_lock(&binding->lock);
		binding->quit = 1;
		pthread_cond_signal(&binding->requested);
		pthread_mutex_unlock(&binding->lock);
	}
}

/* ---------- the clipboard (internet play's invite links) */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

/* ---------- a message for the player (a host of another network version) */

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, NULL) ? 1 : 0;
}

/* ---------- a question for the player (the mods at startup: port_mods.c) */

int host_sdl_show_message_box(uint32_t flags, const char *title, const char *message, int default_index,
	const char *button0, const char *button1, const char *button2)
{
	const char *labels[3] = { button0, button1, button2 };
	SDL_MessageBoxButtonData buttons[3];
	SDL_MessageBoxData question;
	int count = 0;
	int answer = -1;
	int index;

	for (index = 0; index < 3; index++)
	{
		if (!labels[index])
			continue;
		buttons[count].flags = index == default_index ?
			SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
		buttons[count].buttonID = index;
		buttons[count].text = labels[index];
		count++;
	}
	question.flags = (SDL_MessageBoxFlags)flags;
	question.window = NULL;
	question.title = title;
	question.message = message;
	question.numbuttons = count;
	question.buttons = buttons;
	question.colorScheme = NULL;
	return SDL_ShowMessageBox(&question, &answer) ? answer : -1;
}

/* ---------- the desktop's (port/macos/include/macos_host.h) */

void host_sdl_destroy_window(uint32_t window)
{
	SDL_Window *object = handle_release(window, _handle_window, NULL);

	if (object)
		SDL_DestroyWindow(object);
}

int64_t host_sdl_window_flags(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? (int64_t)SDL_GetWindowFlags(object) : 0;
}

void host_sdl_window_size(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GetWindowSize(object, width, height);
}

int host_sdl_set_window_size(uint32_t window, int width, int height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowSize(object, width, height) : 0;
}

int host_sdl_set_window_fullscreen(uint32_t window, int fullscreen)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowFullscreen(object, fullscreen != 0) : 0;
}

static void mode_fields(const SDL_DisplayMode *mode, int *fields)
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

int host_sdl_set_window_fullscreen_mode(uint32_t window, const int *fields)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_DisplayMode closest;
	float refresh_rate;

	if (!object)
		return 0;
	if (!fields)
		return SDL_SetWindowFullscreenMode(object, NULL);
	/* (SDL's own mode, which carries its internals: the one of these
	dimensions) */
	memcpy(&refresh_rate, &fields[5], sizeof(float));
	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)fields[0], fields[2], fields[3], refresh_rate, true,
		&closest))
	{
		return 0;
	}
	return SDL_SetWindowFullscreenMode(object, &closest);
}

int host_sdl_warp_mouse(uint32_t window, float x, float y)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (object)
		SDL_WarpMouseInWindow(object, x, y);
	return object != NULL;
}

uint32_t host_sdl_primary_display(void)
{
	return SDL_GetPrimaryDisplay();
}

uint32_t host_sdl_display_for_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GetDisplayForWindow(object) : 0;
}

int host_sdl_display_mode(uint32_t display, int which, int *fields)
{
	const SDL_DisplayMode *mode = which ? SDL_GetCurrentDisplayMode((SDL_DisplayID)display) :
		SDL_GetDesktopDisplayMode((SDL_DisplayID)display);

	if (!mode)
		return 0;
	mode_fields(mode, fields);
	return 1;
}

int host_sdl_fullscreen_modes(uint32_t display, int *fields, int capacity)
{
	int count = 0, index;
	SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes((SDL_DisplayID)display, &count);

	if (!modes)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		mode_fields(modes[index], fields + index * HOST_SDL_MODE_FIELDS);
	SDL_free(modes);
	return count;
}

int host_sdl_closest_fullscreen_mode(uint32_t display, int width, int height, float refresh_rate, int high_density,
	int *fields)
{
	SDL_DisplayMode closest;

	if (!SDL_GetClosestFullscreenDisplayMode((SDL_DisplayID)display, width, height, refresh_rate, high_density != 0,
		&closest))
	{
		return 0;
	}
	mode_fields(&closest, fields);
	return 1;
}

int host_sdl_display_usable_bounds(uint32_t display, int *rectangle)
{
	SDL_Rect bounds;

	if (!SDL_GetDisplayUsableBounds((SDL_DisplayID)display, &bounds))
		return 0;
	rectangle[0] = bounds.x;
	rectangle[1] = bounds.y;
	rectangle[2] = bounds.w;
	rectangle[3] = bounds.h;
	return 1;
}

void host_sdl_pump_events(void)
{
	SDL_PumpEvents();
}

int host_sdl_push_event(const void *event)
{
	SDL_Event copy;

	memcpy(&copy, event, sizeof(copy));
	return SDL_PushEvent(&copy) ? 1 : 0;
}

int host_sdl_hint_boolean(const char *name, int otherwise)
{
	return SDL_GetHintBoolean(name, otherwise != 0) ? 1 : 0;
}

int host_sdl_text_input_active(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_TextInputActive(object) : 0;
}

int host_sdl_start_text_input(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_StartTextInput(object) : 0;
}

int host_sdl_stop_text_input(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_StopTextInput(object) : 0;
}

int host_sdl_has_screen_keyboard(void)
{
	return SDL_HasScreenKeyboardSupport() ? 1 : 0;
}

int host_sdl_audio_devices(int recording, uint32_t *ids, int capacity)
{
	int count = 0, index;
	SDL_AudioDeviceID *devices = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count);

	if (!devices)
		return 0;
	if (count > capacity)
		count = capacity;
	for (index = 0; index < count; index++)
		ids[index] = devices[index];
	SDL_free(devices);
	return count;
}

int host_sdl_audio_device_name(uint32_t device, char *buffer, uint32_t size)
{
	const char *name = SDL_GetAudioDeviceName((SDL_AudioDeviceID)device);

	SDL_strlcpy(buffer, name ? name : "", size);
	return name != NULL;
}

void host_sdl_base_path(char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s/", host_support_directory());
}

int host_sdl_glob_directory(const char *path, const char *pattern, uint32_t flags, char *buffer, uint32_t size)
{
	int count = 0, index;
	char **names = SDL_GlobDirectory(path, pattern, (SDL_GlobFlags)flags, &count);
	uint32_t used = 0;

	if (!names)
		return -1;
	for (index = 0; index < count; index++)
	{
		size_t length = strlen(names[index]) + 1;

		if (used + length > size)
			break;
		memcpy(buffer + used, names[index], length);
		used += (uint32_t)length;
	}
	SDL_free(names);
	return index;
}

int host_sdl_save_png(const char *path, const void *pixels, int width, int height, int pitch, char *error,
	uint32_t error_size)
{
	SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_BGRA32, (void *)pixels, pitch);
	bool saved = surface && SDL_SavePNG(surface, path);

	if (!saved)
		SDL_strlcpy(error, SDL_GetError(), error_size);
	SDL_DestroySurface(surface);
	return saved;
}

struct file_choice
{
	SDL_AtomicInt done;
	char path[1024];
};

static void SDLCALL file_chosen(void *userdata, const char *const *files, int filter)
{
	struct file_choice *choice = userdata;

	(void)filter;
	if (files && files[0])
		SDL_strlcpy(choice->path, files[0], sizeof(choice->path));
	SDL_SetAtomicInt(&choice->done, 1);
}

int host_sdl_choose_file(const char *name0, const char *pattern0, const char *name1, const char *pattern1,
	char *path, uint32_t size)
{
	SDL_DialogFileFilter filters[2];
	struct file_choice choice;
	int count = 0;

	memset(&choice, 0, sizeof(choice));
	if (name0 && pattern0)
	{
		filters[count].name = name0;
		filters[count].pattern = pattern0;
		count++;
	}
	if (name1 && pattern1)
	{
		filters[count].name = name1;
		filters[count].pattern = pattern1;
		count++;
	}
	SDL_ShowOpenFileDialog(file_chosen, &choice, NULL, count ? filters : NULL, count, NULL, false);
	/* the dialog answers through events */
	while (!SDL_GetAtomicInt(&choice.done))
	{
		SDL_PumpEvents();
		SDL_Delay(20);
	}
	SDL_strlcpy(path, choice.path, size);
	return choice.path[0] != 0;
}

uint32_t host_sdl_create_renderer(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? handle_new(_handle_renderer, SDL_CreateRenderer(object, NULL)) : 0;
}

void host_sdl_destroy_renderer(uint32_t renderer)
{
	SDL_Renderer *object = handle_release(renderer, _handle_renderer, NULL);

	if (object)
		SDL_DestroyRenderer(object);
}

int host_sdl_set_render_vsync(uint32_t renderer, int vsync)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderVSync(object, vsync) : 0;
}

int host_sdl_set_render_draw_color(uint32_t renderer, uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderDrawColor(object, (Uint8)red, (Uint8)green, (Uint8)blue, (Uint8)alpha) : 0;
}

int host_sdl_render_clear(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderClear(object) : 0;
}

int host_sdl_set_render_scale(uint32_t renderer, float x, float y)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_SetRenderScale(object, x, y) : 0;
}

int host_sdl_render_debug_text(uint32_t renderer, float x, float y, const char *text)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderDebugText(object, x, y, text) : 0;
}

int host_sdl_render_fill_rect(uint32_t renderer, const float *rectangle)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);
	SDL_FRect rect;

	if (!object)
		return 0;
	rect.x = rectangle[0];
	rect.y = rectangle[1];
	rect.w = rectangle[2];
	rect.h = rectangle[3];
	return SDL_RenderFillRect(object, &rect);
}

int host_sdl_render_present(uint32_t renderer)
{
	SDL_Renderer *object = handle_get(renderer, _handle_renderer);

	return object ? SDL_RenderPresent(object) : 0;
}
