/*
MACOS_HOST.H

The host services only the macOS guest imports (the guest's view; the host
defines them in port/macos/host), besides those it shares with Android
(port/android/guest/runtime/guest_host.h) and the Metal renderer's
(halo_metal.h). Every name here must also be listed in
port/macos/host_imports.list, which generates the import stubs from these
prototypes (tools/macos_imports.py): pointers are passed as they are, and
the stubs make them host addresses; 32-bit values are int or unsigned int,
64-bit ones long long, and callbacks guest addresses (unsigned int).

These are the SDL functions the desktop's platform layer calls that the
Android guest's does not (port/macos/guest/guest_sdl_desktop.c). Windows
and renderers are handles, as in guest_sdl.c.
*/

#ifndef __MACOS_HOST_H
#define __MACOS_HOST_H

/* SDL_DisplayMode's members the guest reads, which has a pointer at its end
and so another layout in each ABI: an SDL_DisplayMode's fields in order
(display, format, w, h, then pixel_density and refresh_rate as floats'
bits, then the refresh rate's numerator and denominator) */
#define HOST_SDL_MODE_FIELDS 8

/* ---------- the window */

void host_sdl_destroy_window(unsigned int window);
long long host_sdl_window_flags(unsigned int window);
void host_sdl_window_size(unsigned int window, int *width, int *height);
int host_sdl_set_window_size(unsigned int window, int width, int height);
int host_sdl_set_window_fullscreen(unsigned int window, int fullscreen);
/* with a mode's fields (HOST_SDL_MODE_FIELDS), or NULL for the desktop's
(borderless) */
int host_sdl_set_window_fullscreen_mode(unsigned int window, const int *mode);
int host_sdl_warp_mouse(unsigned int window, float x, float y);

/* ---------- displays */

unsigned int host_sdl_primary_display(void);
unsigned int host_sdl_display_for_window(unsigned int window);
/* which 0: the desktop's mode, 1: the current one; nonzero if there is one */
int host_sdl_display_mode(unsigned int display, int which, int *mode);
/* the display's fullscreen modes, HOST_SDL_MODE_FIELDS each, up to
capacity; their count */
int host_sdl_fullscreen_modes(unsigned int display, int *modes, int capacity);
int host_sdl_closest_fullscreen_mode(unsigned int display, int width, int height, float refresh_rate,
	int high_density, int *mode);
/* x, y, w, h */
int host_sdl_display_usable_bounds(unsigned int display, int *rectangle);

/* ---------- events and input */

void host_sdl_pump_events(void);
int host_sdl_push_event(const void *event);
int host_sdl_hint_boolean(const char *name, int otherwise);
int host_sdl_text_input_active(unsigned int window);
int host_sdl_start_text_input(unsigned int window);
int host_sdl_stop_text_input(unsigned int window);
int host_sdl_has_screen_keyboard(void);

/* ---------- audio devices */

/* the devices' ids, up to capacity; their count */
int host_sdl_audio_devices(int recording, unsigned int *ids, int capacity);
/* its name into buffer; nonzero if it has one */
int host_sdl_audio_device_name(unsigned int device, char *buffer, unsigned int size);

/* ---------- files */

/* the folder the game's own files are in (SDL_GetBasePath: host_main.m),
with its separator */
void host_sdl_base_path(char *buffer, unsigned int size);
/* SDL_GlobDirectory's names, each NUL-terminated, one after another into
buffer; their count, or -1 */
int host_sdl_glob_directory(const char *path, const char *pattern, unsigned int flags, char *buffer,
	unsigned int size);
/* rows of 32-bit BGRA pixels (pitch bytes apart) as a PNG file; nonzero on
success, else the error into error */
int host_sdl_save_png(const char *path, const void *pixels, int width, int height, int pitch, char *error,
	unsigned int error_size);
/* the open file dialog, waited for: the file chosen into path; nonzero if
one was. Filters are a name and a pattern, two of them (NULL: none) */
int host_sdl_choose_file(const char *name0, const char *pattern0, const char *name1, const char *pattern1,
	char *path, unsigned int size);

/* ---------- the renderer (the disc image's extraction: sdl_platform.c) */

unsigned int host_sdl_create_renderer(unsigned int window);
void host_sdl_destroy_renderer(unsigned int renderer);
int host_sdl_set_render_vsync(unsigned int renderer, int vsync);
int host_sdl_set_render_draw_color(unsigned int renderer, unsigned int red, unsigned int green, unsigned int blue,
	unsigned int alpha);
int host_sdl_render_clear(unsigned int renderer);
int host_sdl_set_render_scale(unsigned int renderer, float x, float y);
int host_sdl_render_debug_text(unsigned int renderer, float x, float y, const char *text);
/* x, y, w, h */
int host_sdl_render_fill_rect(unsigned int renderer, const float *rectangle);
int host_sdl_render_present(unsigned int renderer);

#endif
