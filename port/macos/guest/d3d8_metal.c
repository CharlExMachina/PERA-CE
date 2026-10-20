/*
D3D8_METAL.C

The Xbox Direct3D 8 device of the macOS build, drawn with Metal
(port/macos/README.md, "Metal").

It is port/linux/src/d3d8_gl.c's device with Metal in place of OpenGL: the
game drives it through the XDK's inline functions, which keep the "simple"
render states in D3D__RenderState and call into this file for everything
else, and at each draw the full state is read back and translated - the
vertex program into Metal shading language once per shader (nv2a_vsh.c),
the pixel shader once per combination of its states (nv2a_psh.c), and the
rest into a pipeline state, a depth and stencil state, samplers and the
draw's own settings. Metal itself is the host's (port/macos/host/
host_metal.m): this side holds handles to its objects and hands each draw
over whole (struct halo_metal_draw, port/macos/include/halo_metal.h).

What differs from OpenGL:

- Metal's work runs after it is asked for, a frame later: everything a draw
  reads of guest memory is copied when it is drawn. Vertices and indices
  that do not change are kept in the mirror (buffers of guest memory the
  GPU reads in place, refreshed only once no frame that reads them is still
  in flight); the rest go into the frame's stream chunks.
- Pipeline states hold what OpenGL sets as it goes (the vertex layout, the
  blend, the targets' formats): they are made once for each combination and
  kept. Samplers and depth and stencil states too.
- A draw that samples the target it draws into samples a copy of it.
- The depth buffers are 32-bit float with 8 bits of stencil, which Apple's
  GPUs have in place of 24-bit depth.

Conventions carried over from the Xbox are as in d3d8_gl.c: clip space is
Direct3D's, which is Metal's own (depth 0 to 1, rows from the top), render
targets and textures are known by the physical address in their Data
field, and vertex data is read from guest memory at draw time.
*/

#include "xgpu.h"
#include "sdl_platform.h"
#include "halo_ui_pointer.h"
#include "port_config.h"
#include "main/console.h"

#include <SDL3/SDL.h>

#include "macos_host.h"
#include "posix.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

void d3d8_surface_initialize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);
void d3d8_surface_resize(D3DSurface *surface, D3DFORMAT format, unsigned long width, unsigned long height);

/* ---------- the screen's width (as d3d8_gl.c) */

#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920

static long screen_width;
static float screen_scale[2] = { 1.0f, 1.0f };
static long ui_offset;
#define UI_OFFSET ((long)ui_offset)

/* ---------- anti-aliasing

display.anti_aliasing (d3d8_gl.c): supersampling draws the screen's targets
at twice the resolution each way, which the presentation scales down;
multisampling draws the back buffer and its depth buffer with that many
samples a pixel, resolved into the targets' textures at the end of each
pass. FXAA is a pass over each window's 3D view (halo_screen_anti_alias), and
"smaa" draws FXAA too, as on Android. */

enum
{
	_anti_aliasing_off,
	_anti_aliasing_fxaa,
	_anti_aliasing_smaa,
	_anti_aliasing_ssaa,
	_anti_aliasing_msaa,
};

static const struct
{
	const char *name;
	int mode;
	int samples;
} anti_aliasing_values[] =
{
	{ "off", _anti_aliasing_off, 0 },
	{ "fxaa", _anti_aliasing_fxaa, 0 },
	{ "smaa", _anti_aliasing_smaa, 0 },
	{ "ssaa2x", _anti_aliasing_ssaa, 0 },
	{ "msaa2x", _anti_aliasing_msaa, 2 },
	{ "msaa4x", _anti_aliasing_msaa, 4 },
	{ "msaa8x", _anti_aliasing_msaa, 8 },
};

#define NUMBER_OF_ANTI_ALIASING_VALUES ((int)(sizeof(anti_aliasing_values) / sizeof(anti_aliasing_values[0])))

static int anti_aliasing_value = -1;
static int anti_aliasing_samples;
static long anti_aliasing_maximum_samples, maximum_target_size;

static void anti_aliasing_prepare(void);

static void anti_aliasing_read(void)
{
	const char *setting = config_string("display.anti_aliasing");
	int value;

	for (value = NUMBER_OF_ANTI_ALIASING_VALUES - 1;
		value > 0 && strcmp(setting, anti_aliasing_values[value].name);
		value--)
	{
	}
	if (value == anti_aliasing_value)
		return;
	anti_aliasing_value = value;
	if (strcmp(setting, anti_aliasing_values[value].name))
		platform_log("anti-aliasing: \"%s\" is unknown, so off", setting);
	else if (anti_aliasing_values[value].mode == _anti_aliasing_smaa)
		platform_log("anti-aliasing: smaa, drawn as fxaa with Metal");
	else
		platform_log("anti-aliasing: %s", setting);
	anti_aliasing_prepare();
}

static int anti_aliasing(void)
{
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	return anti_aliasing_values[anti_aliasing_value].mode;
}

static void screen_mode_choose(long *width, float scale[2])
{
	long display_width, display_height;

	*width = 640;
	scale[0] = scale[1] = 1.0f;
	if (platform_screen_mode(&display_width, &display_height) && display_width > 0 && display_height > 0)
	{
		long wanted = (SCREEN_HEIGHT * display_width + display_height / 2) / display_height;

		*width = wanted < 640 ? 640 : wanted > SCREEN_MAXIMUM_WIDTH ? SCREEN_MAXIMUM_WIDTH : wanted & ~1L;
		scale[0] = (float)display_width / (float)*width;
		scale[1] = (float)display_height / (float)SCREEN_HEIGHT;
		if (*width != wanted && *width != (wanted & ~1L))
			scale[0] = scale[1] = scale[0] < scale[1] ? scale[0] : scale[1];
		if (anti_aliasing() == _anti_aliasing_ssaa && maximum_target_size > 0)
		{
			float factor = 2.0f;

			if (*width * scale[0] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (*width * scale[0]);
			if (SCREEN_HEIGHT * scale[1] * factor > (float)maximum_target_size)
				factor = (float)maximum_target_size / (SCREEN_HEIGHT * scale[1]);
			if (factor > 1.0f)
			{
				scale[0] *= factor;
				scale[1] *= factor;
			}
		}
	}
}

long halo_screen_width(void)
{
	if (!screen_width)
	{
		screen_mode_choose(&screen_width, screen_scale);
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", screen_width, SCREEN_HEIGHT,
			screen_width * screen_scale[0], SCREEN_HEIGHT * screen_scale[1]);
	}
	return screen_width;
}

float halo_screen_pixel_scale(void)
{
	halo_screen_width();
	return screen_scale[1];
}

float halo_screen_scale(void)
{
	halo_screen_width();
	return screen_scale[0] > screen_scale[1] ? screen_scale[0] : screen_scale[1];
}

/* display.shadow_resolution (d3d8_gl.c) */
#define SHADOW_MAP_SIZE 128
#define SHADOW_MAP_MAXIMUM_SCALE 8

static long shadow_scale;
static unsigned long shadow_scale_read_at;

static long shadow_scale_choose(void)
{
	long resolution = config_integer("display.shadow_resolution");
	long scale = 1;

	while (scale < SHADOW_MAP_MAXIMUM_SCALE && SHADOW_MAP_SIZE * scale * 2 <= resolution)
		scale *= 2;
	return scale;
}

long halo_shadow_map_scale(void)
{
	if (!shadow_scale)
	{
		shadow_scale_read_at = config_changes();
		shadow_scale = shadow_scale_choose();
		platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow_scale, SHADOW_MAP_SIZE * shadow_scale);
	}
	return shadow_scale;
}

void halo_screen_ui_offset(unsigned char centered)
{
	ui_offset = centered ? (halo_screen_width() - 640) / 2 : 0;
}

/* ---------- state the XDK header's inline functions read and write */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;
BYTE D3D__StateBlockDirty[1024];

/* ---------- vertex shaders */

#define VERTEX_SHADER_SIGNATURE 0x76736864UL /* 'vshd' */
#define VERTEX_PROGRAM_SLOTS 136

struct vertex_element
{
	unsigned char reg;
	unsigned char stream;
	unsigned char type;
	unsigned char bytes;
	unsigned short offset;
};

/* a vertex program's Metal function for one set of inputs: which
attributes come from streams, which of them are packed normals and which
whole numbers, and whether it hands the lighting on (nv2a_vertex_shader_to_msl) */
struct vertex_function_entry
{
	struct vertex_function_entry *next;
	unsigned long streamed, packed, integer;
	BOOL lit;
	uint32_t function;
};

struct vertex_shader_object
{
	unsigned long signature;
	unsigned long id;
	DWORD *instructions;
	unsigned long instruction_count;
	struct vertex_element elements[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long element_count;
	unsigned long packed_mask;
	struct vertex_function_entry *functions;
	/* the function its draws last used (a program's draws mostly take one) */
	struct vertex_function_entry *recent;
	struct nv2a_vertex_lighting lighting;
	BOOL lighting_failed;
};

/* ---------- the device */

#define VISIBILITY_TEST_SLOTS 4096
/* frames the CPU may be ahead of the GPU, each with its own stream chunks
and visibility results */
#define FRAMES_IN_FLIGHT 3

struct metal_device
{
	D3DPRESENT_PARAMETERS presentation;
	D3DSurface back_buffer;
	D3DSurface depth_buffer;
	D3DSurface *render_target;
	D3DSurface *depth_stencil;
	D3DVIEWPORT8 viewport;
	D3DMATRIX transforms[D3DTS_MAX];
	D3DBaseTexture *textures[D3DTSS_MAXSTAGES];
	D3DPalette *palettes[D3DTSS_MAXSTAGES];
	D3DSHADERCONSTANTMODE shader_constant_mode;

	struct vertex_shader_object *vertex_shader;
	struct vertex_shader_object *program_slots[VERTEX_PROGRAM_SLOTS];
	unsigned long program_address;
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4];
	float viewport_scale[4];
	float viewport_offset[4];

	struct
	{
		DWORD data;
		UINT stride;
	} streams[16];
	UINT base_vertex_index;

	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
	BOOL immediate_active;
	D3DPRIMITIVETYPE immediate_type;
	unsigned long immediate_written;
	float *immediate_vertices;
	unsigned long immediate_count;
	unsigned long immediate_capacity;

	/* visibility tests: each frame's results (8 bytes a test, which the GPU
	counts samples into), the test running, and for each of the game's
	slots the frame and the counter of its latest test */
	uint64_t *visibility_results[FRAMES_IN_FLIGHT];
	uint32_t visibility_buffers[FRAMES_IN_FLIGHT];
	unsigned long visibility_next[FRAMES_IN_FLIGHT];
	BOOL visibility_test_active;
	unsigned long visibility_counter;
	unsigned long visibility_frame[VISIBILITY_TEST_SLOTS];
	unsigned long visibility_slot_counter[VISIBILITY_TEST_SLOTS];
	BOOL query_pending[VISIBILITY_TEST_SLOTS];
	float query_area[VISIBILITY_TEST_SLOTS];
	unsigned long visibility_known[VISIBILITY_TEST_SLOTS];

	/* frames presented (the serial host_metal_present gave the latest) */
	unsigned long frame;
	uint32_t frame_serial;
	unsigned long next_vertex_shader_id;
	BOOL metal_ready;
	BOOL created;
};

static struct metal_device device;

static struct
{
	unsigned long draws, immediate_draws, clears, presents;
	unsigned long skipped_no_program, skipped_no_target, skipped_pipeline;
	unsigned long target_changes, passes;
	unsigned long mirrored_bytes, streamed_bytes;
} stats;

static D3DDevice *device_pointer(void)
{
	return (D3DDevice *)&device;
}

static float dword_to_float(DWORD value)
{
	union { DWORD d; float f; } u;

	u.d = value;
	return u.f;
}

static void color_to_vec4(D3DCOLOR color, float *out)
{
	out[0] = ((color >> 16) & 0xff) / 255.0f;
	out[1] = ((color >> 8) & 0xff) / 255.0f;
	out[2] = (color & 0xff) / 255.0f;
	out[3] = ((color >> 24) & 0xff) / 255.0f;
}

static struct
{
	const char *skip_vertex_shaders;
	const char *dump_shaders;
	BOOL statistics;
} debug_settings;

/* the shared code's GL state hooks (xgpu.h): the host keeps Metal's state,
and forgets a texture that goes (host_metal_texture_delete) */
void xgpu_gl_state_invalidate(void)
{
}

void xgpu_gl_texture_deleted(unsigned int texture)
{
	(void)texture;
}

GLuint xgpu_metal_texture_rgba(const void *pixels, unsigned long width, unsigned long height, unsigned long levels)
{
	struct halo_metal_texture_description description;
	struct halo_metal_region region;
	uint32_t texture;

	memset(&description, 0, sizeof(description));
	description.type = HALO_METAL_TEXTURE_2D;
	description.format = HALO_METAL_FORMAT_RGBA8;
	description.width = (uint32_t)width;
	description.height = (uint32_t)height;
	description.depth = 1;
	description.levels = (uint32_t)(levels ? levels : 1);
	description.samples = 1;
	description.usage = HALO_METAL_USAGE_SAMPLED;
	texture = host_metal_texture_create(&description);
	if (!texture || !pixels)
		return texture;
	memset(&region, 0, sizeof(region));
	region.width = (uint32_t)width;
	region.height = (uint32_t)height;
	region.depth = 1;
	region.bytes_per_row = (uint32_t)(width * 4);
	region.bytes_per_image = (uint32_t)(width * height * 4);
	host_metal_texture_upload(texture, &region, pixels);
	if (levels > 1)
		host_metal_texture_generate_mipmaps(texture, 0);
	return texture;
}

/* ---------- vertical blank emulation (as d3d8_gl.c) */

#define VERTICAL_BLANK_NANOSECONDS (1000000000L / 60)

static pthread_mutex_t vertical_blank_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vertical_blank_condition = PTHREAD_COND_INITIALIZER;
static D3DCALLBACK vertical_blank_callback;
static unsigned long vertical_blank_count;
static volatile unsigned int flip_count;
static unsigned long pending_flips;
static BOOL vertical_blank_thread_started = FALSE;

static void *vertical_blank_thread(void *unused)
{
	struct timespec next;

	(void)unused;
	clock_gettime(CLOCK_MONOTONIC, &next);
	for (;;)
	{
		D3DCALLBACK callback;

		next.tv_nsec += VERTICAL_BLANK_NANOSECONDS;
		if (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		pthread_mutex_lock(&vertical_blank_lock);
		vertical_blank_count++;
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		callback = vertical_blank_callback;
		pthread_cond_broadcast(&vertical_blank_condition);
		pthread_mutex_unlock(&vertical_blank_lock);

		if (callback)
			callback(0);
	}
	return NULL;
}

static void vertical_blank_start(void)
{
	pthread_mutex_lock(&vertical_blank_lock);
	if (!vertical_blank_thread_started)
	{
		pthread_t thread;

		if (pthread_create(&thread, NULL, vertical_blank_thread, NULL) == 0)
		{
			pthread_detach(thread);
			vertical_blank_thread_started = TRUE;
		}
		else
		{
			platform_log("cannot start the vertical blank thread");
		}
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

volatile unsigned int *d3d_find_flipcount(void)
{
	return &flip_count;
}

void WINAPI D3DDevice_SetVerticalBlankCallback(D3DCALLBACK callback)
{
	pthread_mutex_lock(&vertical_blank_lock);
	vertical_blank_callback = callback;
	pthread_mutex_unlock(&vertical_blank_lock);
	vertical_blank_start();
}

void WINAPI D3DDevice_BlockUntilVerticalBlank(void)
{
	unsigned long count;

	vertical_blank_start();
	pthread_mutex_lock(&vertical_blank_lock);
	count = vertical_blank_count;
	while (vertical_blank_count == count)
		pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
	pthread_mutex_unlock(&vertical_blank_lock);
}

/* ---------- guest memory the GPU reads in place */

/* pages of guest memory, as a Metal buffer (host_metal_buffer_wrap wants
whole pages) */
static void *gpu_memory(unsigned long size, uint32_t *buffer)
{
	void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	*buffer = 0;
	if (memory == MAP_FAILED)
		return NULL;
	*buffer = host_metal_buffer_wrap(memory, (uint32_t)size);
	if (!*buffer)
	{
		munmap(memory, size);
		return NULL;
	}
	return memory;
}

/* ---------- render targets

As d3d8_gl.c's: a texture for each surface address the game draws into,
the screen's targets at the screen's scale, the shadow maps at
display.shadow_resolution's. */

enum render_target_kind
{
	_render_target_plain,
	_render_target_screen,
	_render_target_shadow_map,
};

struct render_target_entry
{
	struct render_target_entry *next;
	struct render_target_entry *next_in_bucket;
	struct xgpu_render_target target;
	unsigned long last_rendered;
	BOOL screen_buffer;
	enum render_target_kind kind;
};

#define RENDER_TARGET_BUCKET_COUNT 256

static struct render_target_entry *render_target_buckets[RENDER_TARGET_BUCKET_COUNT];
static struct render_target_entry *render_targets;

static struct render_target_entry **render_target_bucket(unsigned long data)
{
	return &render_target_buckets[((data >> 12) ^ (data >> 20)) % RENDER_TARGET_BUCKET_COUNT];
}

static void render_targets_evict(void);

static void surface_dimensions(const D3DSurface *surface, unsigned long *width, unsigned long *height, BOOL *depth)
{
	struct xgpu_texture_description description;
	DWORD format;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	*width = description.width;
	*height = description.height;
	format = description.format;
	*depth = format == D3DFMT_D24S8 || format == D3DFMT_F24S8 || format == D3DFMT_D16 || format == D3DFMT_F16 ||
		format == D3DFMT_LIN_D24S8 || format == D3DFMT_LIN_F24S8 || format == D3DFMT_LIN_D16 || format == D3DFMT_LIN_F16;
}

static BOOL surface_is_shadow_map(const D3DSurface *surface)
{
	struct xgpu_texture_description description;

	xgpu_texture_describe(surface->Format, surface->Size, &description);
	return description.format == D3DFMT_R5G6B5 && description.width == SHADOW_MAP_SIZE &&
		description.height == SHADOW_MAP_SIZE;
}

#define SECONDARY_TARGET_WIDTH 320
#define SECONDARY_TARGET_HEIGHT 240
#define RECENT_RENDER_TARGET_COUNT 4

static struct
{
	DWORD data, format, size;
	long screen_width;
	float scale[2];
	struct render_target_entry *entry;
} recent_render_targets[RECENT_RENDER_TARGET_COUNT];
static unsigned long recent_render_target_next;

static struct render_target_entry *render_target_remember(const D3DSurface *surface, long screen,
	struct render_target_entry *entry)
{
	unsigned long slot = recent_render_target_next++ % RECENT_RENDER_TARGET_COUNT;

	recent_render_targets[slot].data = surface->Data;
	recent_render_targets[slot].format = surface->Format;
	recent_render_targets[slot].size = surface->Size;
	recent_render_targets[slot].screen_width = screen;
	recent_render_targets[slot].scale[0] = screen_scale[0];
	recent_render_targets[slot].scale[1] = screen_scale[1];
	recent_render_targets[slot].entry = entry;
	return entry;
}

/* a target's Metal textures for samples a pixel (0 or 1: none): with
multisampling, the multisampled texture draws go to, resolved into the
target's own at the end of each pass */
static void render_target_textures(struct xgpu_render_target *target, int samples)
{
	struct halo_metal_texture_description description;

	memset(&description, 0, sizeof(description));
	description.type = HALO_METAL_TEXTURE_2D;
	description.format = target->depth ? HALO_METAL_FORMAT_DEPTH32F_STENCIL8 : HALO_METAL_FORMAT_BGRA8;
	description.width = (uint32_t)target->gl_width;
	description.height = (uint32_t)target->gl_height;
	description.depth = 1;
	description.levels = 1;
	description.samples = 1;
	description.usage = HALO_METAL_USAGE_SAMPLED | HALO_METAL_USAGE_RENDER_TARGET;
	if (!target->texture)
		target->texture = host_metal_texture_create(&description);
	if (samples > 1 && target->samples != samples)
	{
		if (target->multisample)
			host_metal_texture_delete(target->multisample);
		description.samples = (uint32_t)samples;
		description.usage = HALO_METAL_USAGE_RENDER_TARGET;
		target->multisample = host_metal_texture_create(&description);
		target->samples = target->multisample ? samples : 0;
	}
}

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	struct render_target_entry *entry;
	unsigned long width, height, slot;
	long screen;
	BOOL depth;
	float scale[2] = { 1.0f, 1.0f };
	enum render_target_kind kind = _render_target_plain;

	if (!surface || !surface->Data)
		return NULL;
	screen = halo_screen_width();
	for (slot = 0; slot < RECENT_RENDER_TARGET_COUNT; slot++)
	{
		if (recent_render_targets[slot].entry && recent_render_targets[slot].data == surface->Data &&
			recent_render_targets[slot].format == surface->Format && recent_render_targets[slot].size == surface->Size &&
			recent_render_targets[slot].screen_width == screen && recent_render_targets[slot].scale[0] == screen_scale[0] &&
			recent_render_targets[slot].scale[1] == screen_scale[1])
		{
			return recent_render_targets[slot].entry;
		}
	}
	surface_dimensions(surface, &width, &height, &depth);
	if ((width == (unsigned long)halo_screen_width() && height == SCREEN_HEIGHT) ||
		(!depth && width == SECONDARY_TARGET_WIDTH && height == SECONDARY_TARGET_HEIGHT))
	{
		scale[0] = screen_scale[0];
		scale[1] = screen_scale[1];
		kind = _render_target_screen;
	}
	else if (!depth && surface_is_shadow_map(surface))
	{
		scale[0] = (float)halo_shadow_map_scale();
		scale[1] = scale[0];
		kind = _render_target_shadow_map;
	}
	for (entry = *render_target_bucket(surface->Data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == surface->Data && entry->target.width == width &&
			entry->target.height == height && entry->target.depth == depth &&
			entry->target.scale[0] == scale[0] && entry->target.scale[1] == scale[1])
		{
			return render_target_remember(surface, screen, entry);
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->target.data = surface->Data;
	entry->target.width = width;
	entry->target.height = height;
	entry->target.depth = depth;
	entry->target.scale[0] = scale[0];
	entry->target.scale[1] = scale[1];
	entry->kind = kind;
	entry->target.gl_width = (unsigned long)(width * scale[0] + 0.5f);
	entry->target.gl_height = (unsigned long)(height * scale[1] + 0.5f);
	if (!entry->target.gl_width)
		entry->target.gl_width = 1;
	if (!entry->target.gl_height)
		entry->target.gl_height = 1;
	render_target_textures(&entry->target, 0);
	entry->screen_buffer = surface->Data == (depth ? device.depth_buffer.Data : device.back_buffer.Data);
	entry->next = render_targets;
	render_targets = entry;
	entry->next_in_bucket = *render_target_bucket(entry->target.data);
	*render_target_bucket(entry->target.data) = entry;
	return entry;
}

struct xgpu_render_target *xgpu_render_target_find(unsigned long data)
{
	struct render_target_entry *entry, *best = NULL;

	for (entry = *render_target_bucket(data); entry; entry = entry->next_in_bucket)
	{
		if (entry->target.data == data && !entry->target.depth && (!best || entry->last_rendered > best->last_rendered))
			best = entry;
	}
	return best ? &best->target : NULL;
}

/* ---------- passes

Draws go into the pass of the targets they draw into; a draw with other
targets than the pass's begins another. A pass loads what its targets
held, and with multisampling draws into their multisampled textures and
resolves them into the targets' at its end. */

static struct
{
	struct xgpu_render_target *color, *depth;
	int samples;
	BOOL open;
} pass;

static unsigned long render_target_write_serial;
static float target_scale[2] = { 1.0f, 1.0f };
/* the pass's targets' size in pixels */
static unsigned long target_pixels[2];

static long scaled_pixel(float coordinate, float scale)
{
	float value = coordinate * scale + 0.5f;
	long pixel = (long)value;

	if ((float)pixel > value)
		pixel--;
	return pixel;
}

static long target_pixel(float coordinate, int axis)
{
	return scaled_pixel(coordinate, target_scale[axis]);
}

/* the frame's visibility results buffer slot (FRAMES_IN_FLIGHT ring) */
static unsigned long frame_ring(void)
{
	return device.frame % FRAMES_IN_FLIGHT;
}

static void pass_begin(struct xgpu_render_target *color, struct xgpu_render_target *depth, int samples)
{
	struct halo_metal_pass_description description;

	memset(&description, 0, sizeof(description));
	if (samples > 1)
	{
		description.color = color ? color->multisample : 0;
		description.depth = depth ? depth->multisample : 0;
		description.color_resolve = color ? color->texture : 0;
		description.depth_resolve = depth ? depth->texture : 0;
		/* (the multisampled textures hold nothing between passes: the
		targets' pixels are loaded from their resolved ones by the host) */
	}
	else
	{
		description.color = color ? color->texture : 0;
		description.depth = depth ? depth->texture : 0;
	}
	description.color_load = HALO_METAL_LOAD;
	description.depth_load = HALO_METAL_LOAD;
	description.stencil_load = HALO_METAL_LOAD;
	description.visibility_buffer = device.visibility_buffers[frame_ring()];
	host_metal_pass_begin(&description);
	pass.color = color;
	pass.depth = depth;
	pass.samples = samples;
	pass.open = TRUE;
	stats.passes++;
}

/* the targets for the current render target and depth buffer, their pass
begun if it is not the one draws go to; FALSE if there is nothing to draw
into */
static BOOL bind_targets(BOOL *has_depth)
{
	struct render_target_entry *color = render_target_get(device.render_target);
	struct render_target_entry *depth = render_target_get(device.depth_stencil);
	int samples;

	if (depth && !depth->target.depth)
		depth = NULL;
	if (!color && !depth)
		return FALSE;
	if (color)
	{
		color->last_rendered = device.frame + 1;
		color->target.written = ++render_target_write_serial;
	}
	target_scale[0] = color ? color->target.scale[0] : depth->target.scale[0];
	target_scale[1] = color ? color->target.scale[1] : depth->target.scale[1];
	target_pixels[0] = color ? color->target.gl_width : depth->target.gl_width;
	target_pixels[1] = color ? color->target.gl_height : depth->target.gl_height;
	/* with multisampling, where either is a screen buffer */
	samples = anti_aliasing() == _anti_aliasing_msaa ? anti_aliasing_samples : 0;
	if (samples && !((color && color->screen_buffer) || (depth && depth->screen_buffer)))
		samples = 0;
	/* (the two must agree: a target of another size than the other's is
	drawn without multisampling, and so is a depth buffer drawn with no
	multisampled color target) */
	if (samples && color && depth &&
		(color->target.gl_width != depth->target.gl_width || color->target.gl_height != depth->target.gl_height))
	{
		samples = 0;
	}
	if (color)
		render_target_textures(&color->target, samples);
	if (depth)
		render_target_textures(&depth->target, samples);
	if (samples && ((color && !color->target.multisample) || (depth && !depth->target.multisample)))
		samples = 0;
	if (!pass.open || pass.color != (color ? &color->target : NULL) || pass.depth != (depth ? &depth->target : NULL) ||
		pass.samples != (samples > 1 ? samples : 0))
	{
		pass_begin(color ? &color->target : NULL, depth ? &depth->target : NULL, samples > 1 ? samples : 0);
	}
	*has_depth = depth != NULL;
	return TRUE;
}

/* ---------- device creation */

static void metal_initialize(void)
{
	struct halo_metal_capabilities capabilities;
	int index;

	memset(&capabilities, 0, sizeof(capabilities));
	host_metal_capabilities(&capabilities);
	anti_aliasing_maximum_samples = capabilities.maximum_samples;
	maximum_target_size = capabilities.maximum_texture_size;
	for (index = 0; index < FRAMES_IN_FLIGHT; index++)
	{
		device.visibility_results[index] = gpu_memory(VISIBILITY_TEST_SLOTS * sizeof(uint64_t),
			&device.visibility_buffers[index]);
		if (!device.visibility_results[index])
			platform_log("GPU: no memory for the visibility tests' results");
	}
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
		device.attributes[index][3] = 1.0f;
	memory_watch_initialize();
	debug_settings.skip_vertex_shaders = config_string("debug.gpu_skip_vertex_shaders");
	debug_settings.dump_shaders = *config_string("debug.gpu_dump_shaders") ?
		config_string("debug.gpu_dump_shaders") : NULL;
	debug_settings.statistics = config_boolean("debug.gpu_stats");
	platform_log("Metal on %s: largest texture %lu, %lu samples a pixel at most", capabilities.name,
		(unsigned long)capabilities.maximum_texture_size, (unsigned long)capabilities.maximum_samples);
	device.metal_ready = TRUE;
	if (anti_aliasing_value < 0)
		anti_aliasing_read();
	else
		anti_aliasing_prepare();
}

static void anti_aliasing_prepare(void)
{
	if (!device.metal_ready || anti_aliasing_value < 0)
		return;
	anti_aliasing_samples = anti_aliasing_values[anti_aliasing_value].samples;
	if (anti_aliasing_samples > anti_aliasing_maximum_samples)
	{
		platform_log("anti-aliasing: the GPU has at most %ld samples a pixel", anti_aliasing_maximum_samples);
		anti_aliasing_samples = anti_aliasing_maximum_samples < 2 ? 0 : (int)anti_aliasing_maximum_samples;
	}
}

Direct3D *WINAPI Direct3DCreate8(UINT sdk_version)
{
	(void)sdk_version;
	return (Direct3D *)1;
}

void WINAPI Direct3D_SetPushBufferSize(DWORD push_buffer_size, DWORD segment_count)
{
	(void)push_buffer_size;
	(void)segment_count;
}

/* the vertex constants, each with the serial its last change took (the
uniforms are handed over again only when one has changed since: struct
halo_metal_draw's serials) */
static unsigned long long constants_serial = 1;
static unsigned long long constant_serials[XGPU_VERTEX_CONSTANT_COUNT];

static void constants_store(unsigned long first, const void *data, unsigned long count)
{
	const float (*values)[4] = data;
	unsigned long index;

	for (index = 0; index < count; index++)
	{
		if (memcmp(device.constants[first + index], values[index], sizeof(device.constants[0])))
		{
			memcpy(device.constants[first + index], values[index], sizeof(device.constants[0]));
			constant_serials[first + index] = ++constants_serial;
		}
	}
}

static void viewport_update_constants(void)
{
	float zscale = 16777215.0f;

	if (device.depth_stencil)
	{
		struct xgpu_texture_description description;

		xgpu_texture_describe(device.depth_stencil->Format, device.depth_stencil->Size, &description);
		if (description.format == D3DFMT_D16 || description.format == D3DFMT_LIN_D16 ||
			description.format == D3DFMT_F16 || description.format == D3DFMT_LIN_F16)
		{
			zscale = 65535.0f;
		}
	}
	device.viewport_scale[0] = device.viewport.Width * 0.5f;
	device.viewport_scale[1] = -(float)device.viewport.Height * 0.5f;
	device.viewport_scale[2] = zscale * (device.viewport.MaxZ - device.viewport.MinZ);
	device.viewport_scale[3] = 0.0f;
	device.viewport_offset[0] = device.viewport.X + device.viewport.Width * 0.5f;
	device.viewport_offset[1] = device.viewport.Y + device.viewport.Height * 0.5f;
	device.viewport_offset[2] = zscale * device.viewport.MinZ;
	device.viewport_offset[3] = 0.0f;
	if (!(device.shader_constant_mode & D3DSCM_NORESERVEDCONSTANTS))
	{
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 38, device.viewport_scale, 1);
		constants_store(XGPU_VERTEX_CONSTANT_BIAS - 37, device.viewport_offset, 1);
	}
}

HRESULT WINAPI Direct3D_CreateDevice(UINT adapter, D3DDEVTYPE device_type, void *unused, DWORD behavior_flags,
	D3DPRESENT_PARAMETERS *presentation_parameters, D3DDevice **returned_device)
{
	unsigned long width, height;
	int index;

	(void)adapter;
	(void)device_type;
	(void)unused;
	(void)behavior_flags;
	if (!device.created)
	{
		memset(&device, 0, sizeof(device));
		if (presentation_parameters)
			device.presentation = *presentation_parameters;
		width = device.presentation.BackBufferWidth ? device.presentation.BackBufferWidth : 640;
		height = device.presentation.BackBufferHeight ? device.presentation.BackBufferHeight : 480;
		/* room for the widest screen, which F11 can switch to */
		d3d8_surface_initialize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_initialize(&device.depth_buffer, D3DFMT_LIN_D24S8, SCREEN_MAXIMUM_WIDTH, height);
		d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, width, height);
		d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, width, height);
		device.render_target = &device.back_buffer;
		device.depth_stencil = &device.depth_buffer;
		for (index = 0; index < D3DTS_MAX; index++)
		{
			device.transforms[index]._11 = 1.0f;
			device.transforms[index]._22 = 1.0f;
			device.transforms[index]._33 = 1.0f;
			device.transforms[index]._44 = 1.0f;
		}
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MaxZ = 1.0f;
		device.next_vertex_shader_id = 1;
		D3D__RenderState[D3DRS_ZENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZWRITEENABLE] = TRUE;
		D3D__RenderState[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
		D3D__RenderState[D3DRS_COLORWRITEENABLE] = D3DCOLORWRITEENABLE_ALL;
		D3D__RenderState[D3DRS_SRCBLEND] = D3DBLEND_ONE;
		D3D__RenderState[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
		D3D__RenderState[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
		D3D__RenderState[D3DRS_CULLMODE] = D3DCULL_CCW;
		D3D__RenderState[D3DRS_FRONTFACE] = D3DFRONT_CW;
		D3D__RenderState[D3DRS_FILLMODE] = D3DFILL_SOLID;
		D3D__RenderState[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
		D3D__RenderState[D3DRS_STENCILMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILWRITEMASK] = 0xff;
		D3D__RenderState[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
		D3D__RenderState[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
		for (index = 0; index < D3DTSS_MAXSTAGES; index++)
		{
			D3D__TextureState[index][D3DTSS_ADDRESSU] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_ADDRESSW] = D3DTADDRESS_WRAP;
			D3D__TextureState[index][D3DTSS_MAGFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MINFILTER] = D3DTEXF_POINT;
			D3D__TextureState[index][D3DTSS_MAXANISOTROPY] = 1;
		}
		viewport_update_constants();

		if (!config_boolean("debug.null_renderer") && platform_video_initialize(width, height))
			metal_initialize();
		else
			platform_log("Direct3D: running without a window (nothing is displayed)");
		device.created = TRUE;
	}
	*returned_device = device_pointer();
	return S_OK;
}

/* ---------- the menus' pointer (as d3d8_gl.c) */

static void ui_point_from_window_on(float window_x, float window_y, int centered, short *x, short *y)
{
	struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
	int window_width, window_height, pixel_width, pixel_height, width, height, left, top;
	float screen_x, screen_y;

	*x = *y = -1;
	if (!back_buffer)
		return;
	platform_video_window_size(&window_width, &window_height);
	platform_video_drawable_size(&pixel_width, &pixel_height);
	if (window_width <= 0 || window_height <= 0)
		return;
	width = pixel_width;
	height = (int)((long)pixel_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
	if (height > pixel_height)
	{
		height = pixel_height;
		width = (int)((long)pixel_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
	}
	left = (pixel_width - width) / 2;
	top = (pixel_height - height) / 2;
	screen_x = (window_x * pixel_width / window_width - left) * (float)back_buffer->target.width / (float)width;
	screen_y = (window_y * pixel_height / window_height - top) * (float)back_buffer->target.height / (float)height;
	*x = (short)floorf(screen_x - (centered ? (float)(halo_screen_width() - 640) / 2.0f : 0.0f));
	*y = (short)floorf(screen_y);
}

static void ui_point_from_window(float window_x, float window_y, short *x, short *y)
{
	ui_point_from_window_on(window_x, window_y, TRUE, x, y);
}

int halo_scoreboard_pointer_update(int offered, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	memset(pointer, 0, sizeof(*pointer));
	if (!platform_scoreboard_pointer(offered != 0, &state) || !device.metal_ready)
		return 0;
	ui_point_from_window_on(state.x, state.y, FALSE, &pointer->x, &pointer->y);
	ui_point_from_window_on(state.click_x, state.click_y, FALSE, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	return 1;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	struct platform_ui_pointer state;

	platform_menus_set_active(menus_active != 0);
	platform_ui_pointer_set_active(menus_active != 0);
	if (!menus_active || !device.metal_ready || !platform_ui_pointer_read(&state))
		return 0;
	memset(pointer, 0, sizeof(*pointer));
	ui_point_from_window(state.x, state.y, &pointer->x, &pointer->y);
	ui_point_from_window(state.click_x, state.click_y, &pointer->click_x, &pointer->click_y);
	pointer->moved = state.moved != FALSE;
	pointer->left_clicks = (unsigned char)(state.left_clicks < 255 ? state.left_clicks : 255);
	pointer->right_clicks = (unsigned char)(state.right_clicks < 255 ? state.right_clicks : 255);
	pointer->wheel_steps = (signed char)(state.wheel_steps < -8 ? -8 : state.wheel_steps > 8 ? 8 : state.wheel_steps);
	pointer->touch = state.touch != FALSE;
	pointer->downs = (unsigned char)(state.downs < 255 ? state.downs : 255);
	ui_point_from_window(state.down_x, state.down_y, &pointer->down_x, &pointer->down_y);
	return 1;
}

long halo_screen_commit(void)
{
	long width;
	float scale[2];

	anti_aliasing_read();
	if (shadow_scale && shadow_scale_read_at != config_changes())
	{
		long shadow = shadow_scale_choose();

		shadow_scale_read_at = config_changes();
		if (shadow != shadow_scale)
		{
			platform_log("shadow maps: %ldx%ld", SHADOW_MAP_SIZE * shadow, SHADOW_MAP_SIZE * shadow);
			shadow_scale = shadow;
			render_targets_evict();
		}
	}
	if (!screen_width)
		return halo_screen_width();
	screen_mode_choose(&width, scale);
	if (width != screen_width || scale[0] != screen_scale[0] || scale[1] != screen_scale[1])
	{
		platform_log("screen: %ldx%d drawn at %.0fx%.0f", width, SCREEN_HEIGHT,
			width * scale[0], SCREEN_HEIGHT * scale[1]);
		screen_width = width;
		screen_scale[0] = scale[0];
		screen_scale[1] = scale[1];
		if (device.created)
		{
			device.presentation.BackBufferWidth = (UINT)width;
			d3d8_surface_resize(&device.back_buffer, D3DFMT_LIN_A8R8G8B8, (unsigned long)width, SCREEN_HEIGHT);
			d3d8_surface_resize(&device.depth_buffer, D3DFMT_LIN_D24S8, (unsigned long)width, SCREEN_HEIGHT);
		}
		render_targets_evict();
	}
	return screen_width;
}

ULONG WINAPI D3DDevice_Release(void)
{
	return 1;
}

void WINAPI D3DDevice_GetDeviceCaps(D3DCAPS8 *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->DeviceType = D3DDEVTYPE_HAL;
	caps->MaxTextureWidth = 4096;
	caps->MaxTextureHeight = 4096;
	caps->MaxVolumeExtent = 512;
	caps->MaxTextureRepeat = 8192;
	caps->MaxTextureAspectRatio = 4096;
	caps->MaxAnisotropy = 4;
	caps->MaxTextureBlendStages = 4;
	caps->MaxSimultaneousTextures = 4;
	caps->MaxActiveLights = 8;
	caps->MaxVertexBlendMatrices = 4;
	caps->MaxPointSize = 64.0f;
	caps->MaxPrimitiveCount = 0xfffff;
	caps->MaxVertexIndex = 0xffff;
	caps->MaxStreams = 16;
	caps->MaxStreamStride = 255;
	caps->VertexShaderVersion = D3DVS_VERSION(1, 1);
	caps->MaxVertexShaderConst = 192;
	caps->PixelShaderVersion = D3DPS_VERSION(1, 1);
	caps->MaxPixelShaderValue = 1.0f;
}

void WINAPI D3DDevice_GetBackBuffer(INT back_buffer, D3DBACKBUFFER_TYPE type, D3DSurface **result)
{
	(void)back_buffer;
	(void)type;
	device.back_buffer.Common++;
	*result = &device.back_buffer;
}

HRESULT WINAPI D3DDevice_GetDepthStencilSurface(D3DSurface **result)
{
	*result = device.depth_stencil;
	if (!*result)
		return D3DERR_NOTFOUND;
	(*result)->Common++;
	return S_OK;
}

static BOOL trace_frame(void);

void WINAPI D3DDevice_SetRenderTarget(D3DSurface *render_target, D3DSurface *depth_stencil)
{
	if (trace_frame())
		platform_log("set render target %08lx depth %08lx", render_target ? (unsigned long)render_target->Data : 0,
			depth_stencil ? (unsigned long)depth_stencil->Data : 0);
	stats.target_changes++;
	if (render_target)
		device.render_target = render_target;
	device.depth_stencil = depth_stencil;
	if (device.render_target)
	{
		unsigned long width, height;
		BOOL depth;

		surface_dimensions(device.render_target, &width, &height, &depth);
		device.viewport.X = 0;
		device.viewport.Y = 0;
		device.viewport.Width = width;
		device.viewport.Height = height;
		device.viewport.MinZ = 0.0f;
		device.viewport.MaxZ = 1.0f;
	}
	viewport_update_constants();
}

void WINAPI D3DDevice_SetViewport(CONST D3DVIEWPORT8 *viewport)
{
	device.viewport = *viewport;
	viewport_update_constants();
}

void WINAPI D3DDevice_SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		device.transforms[state] = *matrix;
}

void WINAPI D3DDevice_GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX *matrix)
{
	if ((unsigned long)state < D3DTS_MAX)
		*matrix = device.transforms[state];
}

void WINAPI D3DDevice_SetFlickerFilter(DWORD filter) { (void)filter; }
void WINAPI D3DDevice_SetSoftDisplayFilter(BOOL enable) { (void)enable; }

void WINAPI D3DDevice_SetShaderConstantMode(D3DSHADERCONSTANTMODE mode)
{
	device.shader_constant_mode = mode;
	viewport_update_constants();
}

/* ---------- GPU synchronisation: everything a draw reads is copied as it
is drawn, so the game may write it again at once */

BOOL WINAPI D3DDevice_IsBusy(void)
{
	return FALSE;
}

void WINAPI D3DDevice_KickPushBuffer(void)
{
}

void WINAPI D3DDevice_InsertCallback(D3DCALLBACKTYPE type, D3DCALLBACK callback, DWORD context)
{
	(void)type;
	if (callback)
		callback(context);
}

/* ---------- visibility (occlusion) tests

A test's draws count the samples that pass into a counter of the frame's
results buffer (Metal's counting visibility results). A result is read once
the GPU has finished the frame that counted it; until then the slot's
earlier count stands, as with d3d8_gl.c's query buffer: the game asks at the
start of the next frame and spins on a result said to be incomplete. */

void WINAPI D3DDevice_BeginVisibilityTest(void)
{
	unsigned long ring = frame_ring();

	if (!device.metal_ready || device.visibility_test_active || !device.visibility_results[ring])
		return;
	device.visibility_test_active = TRUE;
	device.visibility_counter = device.visibility_next[ring] < VISIBILITY_TEST_SLOTS ? device.visibility_next[ring]++ :
		VISIBILITY_TEST_SLOTS;
}

HRESULT WINAPI D3DDevice_EndVisibilityTest(DWORD index)
{
	if (!device.metal_ready || !device.visibility_test_active)
		return S_OK;
	device.visibility_test_active = FALSE;
	index %= VISIBILITY_TEST_SLOTS;
	/* (a frame with more tests than counters: the extra ones keep the
	slot's earlier count) */
	if (device.visibility_counter >= VISIBILITY_TEST_SLOTS)
		return S_OK;
	device.visibility_frame[index] = device.frame;
	device.visibility_slot_counter[index] = device.visibility_counter;
	device.query_area[index] = target_scale[0] * target_scale[1] * (float)(pass.samples > 1 ? pass.samples : 1);
	device.query_pending[index] = TRUE;
	return S_OK;
}

/* the frames of results the GPU has finished, read into their slots (as
the presentation does each frame, and a result asked for) */
static void visibility_collect(void)
{
	uint32_t completed = host_metal_frame_completed();
	unsigned long slot;

	for (slot = 0; slot < VISIBILITY_TEST_SLOTS; slot++)
	{
		unsigned long frame = device.visibility_frame[slot];

		/* (frame f's serial is f + 1) */
		if (!device.query_pending[slot] || frame + 1 > completed || device.frame - frame >= FRAMES_IN_FLIGHT)
			continue;
		{
			float area = device.query_area[slot];
			uint64_t samples = device.visibility_results[frame % FRAMES_IN_FLIGHT][device.visibility_slot_counter[slot]];

			device.visibility_known[slot] = area > 1.0f ? (unsigned long)(samples / area + 0.5f) : (unsigned long)samples;
			device.query_pending[slot] = FALSE;
		}
	}
}

HRESULT WINAPI D3DDevice_GetVisibilityTestResult(DWORD index, UINT *result, ULONGLONG *time_stamp)
{
	if (time_stamp)
		*time_stamp = 0;
	index %= VISIBILITY_TEST_SLOTS;
	if (result)
		*result = device.metal_ready ? (UINT)device.visibility_known[index] : 0;
	return S_OK;
}

/* ---------- render and texture stage state (as d3d8_gl.c) */

void D3DFASTCALL D3DDevice_SetRenderState_Simple(DWORD method, DWORD value)
{
	(void)method;
	(void)value;
}

void D3DFASTCALL D3DDevice_SetRenderState_Deferred(D3DRENDERSTATETYPE state, DWORD value)
{
	if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value);

void WINAPI D3DDevice_SetRenderStateNotInline(D3DRENDERSTATETYPE state, DWORD value)
{
	if (state == D3DRS_ZBIAS)
		D3DDevice_SetRenderState_ZBias(value);
	else if ((unsigned long)state < D3DRS_MAX)
		D3D__RenderState[state] = value;
}

void WINAPI D3DDevice_SetRenderState_ZBias(DWORD value)
{
	float offset = -(float)value;
	float slope = offset * 0.25f;
	DWORD enable = value != 0;

	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZSLOPESCALE], &slope, sizeof(slope));
	memcpy(&D3D__RenderState[D3DRS_POLYGONOFFSETZOFFSET], &offset, sizeof(offset));
	D3D__RenderState[D3DRS_POINTOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_WIREFRAMEOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_SOLIDOFFSETENABLE] = enable;
	D3D__RenderState[D3DRS_ZBIAS] = value;
}

#define COMPLEX_RENDER_STATE(name, state) \
	void WINAPI D3DDevice_SetRenderState_##name(DWORD value) { D3D__RenderState[state] = value; }

COMPLEX_RENDER_STATE(PSTextureModes, D3DRS_PSTEXTUREMODES)
COMPLEX_RENDER_STATE(VertexBlend, D3DRS_VERTEXBLEND)
COMPLEX_RENDER_STATE(FogColor, D3DRS_FOGCOLOR)
COMPLEX_RENDER_STATE(FillMode, D3DRS_FILLMODE)
COMPLEX_RENDER_STATE(BackFillMode, D3DRS_BACKFILLMODE)
COMPLEX_RENDER_STATE(TwoSidedLighting, D3DRS_TWOSIDEDLIGHTING)
COMPLEX_RENDER_STATE(NormalizeNormals, D3DRS_NORMALIZENORMALS)
COMPLEX_RENDER_STATE(ZEnable, D3DRS_ZENABLE)
COMPLEX_RENDER_STATE(StencilEnable, D3DRS_STENCILENABLE)
COMPLEX_RENDER_STATE(StencilFail, D3DRS_STENCILFAIL)
COMPLEX_RENDER_STATE(FrontFace, D3DRS_FRONTFACE)
COMPLEX_RENDER_STATE(CullMode, D3DRS_CULLMODE)
COMPLEX_RENDER_STATE(TextureFactor, D3DRS_TEXTUREFACTOR)
COMPLEX_RENDER_STATE(LogicOp, D3DRS_LOGICOP)
COMPLEX_RENDER_STATE(EdgeAntiAlias, D3DRS_EDGEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleAntiAlias, D3DRS_MULTISAMPLEANTIALIAS)
COMPLEX_RENDER_STATE(MultiSampleMask, D3DRS_MULTISAMPLEMASK)
COMPLEX_RENDER_STATE(MultiSampleType, D3DRS_MULTISAMPLETYPE)
COMPLEX_RENDER_STATE(ShadowFunc, D3DRS_SHADOWFUNC)
COMPLEX_RENDER_STATE(LineWidth, D3DRS_LINEWIDTH)
COMPLEX_RENDER_STATE(Dxt1NoiseEnable, D3DRS_DXT1NOISEENABLE)
COMPLEX_RENDER_STATE(YuvEnable, D3DRS_YUVENABLE)
COMPLEX_RENDER_STATE(OcclusionCullEnable, D3DRS_OCCLUSIONCULLENABLE)
COMPLEX_RENDER_STATE(StencilCullEnable, D3DRS_STENCILCULLENABLE)
COMPLEX_RENDER_STATE(RopZCmpAlwaysRead, D3DRS_ROPZCMPALWAYSREAD)
COMPLEX_RENDER_STATE(RopZRead, D3DRS_ROPZREAD)
COMPLEX_RENDER_STATE(DoNotCullUncompressed, D3DRS_DONOTCULLUNCOMPRESSED)

void D3DFASTCALL D3DDevice_SetTextureState_Deferred(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTextureState_TexCoordIndex(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_TEXCOORDINDEX] = value;
}

void WINAPI D3DDevice_SetTextureState_BorderColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_BORDERCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_ColorKeyColor(DWORD stage, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES)
		D3D__TextureState[stage][D3DTSS_COLORKEYCOLOR] = value;
}

void WINAPI D3DDevice_SetTextureState_BumpEnv(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
	if (stage < D3DTSS_MAXSTAGES && (unsigned long)type < D3DTSS_MAX)
		D3D__TextureState[stage][type] = value;
}

void WINAPI D3DDevice_SetTexture(DWORD stage, D3DBaseTexture *texture)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.textures[stage] = texture;
}

void WINAPI D3DDevice_SetPalette(DWORD stage, D3DPalette *palette)
{
	if (stage < D3DTSS_MAXSTAGES)
		device.palettes[stage] = palette;
}

void WINAPI D3DDevice_SetPixelShaderProgram(D3DPIXELSHADERDEF *definition)
{
	if (!definition)
		return;
	memcpy(&D3D__RenderState[D3DRS_PSALPHAINPUTS0], definition->PSAlphaInputs, sizeof(definition->PSAlphaInputs));
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSABCD] = definition->PSFinalCombinerInputsABCD;
	D3D__RenderState[D3DRS_PSFINALCOMBINERINPUTSEFG] = definition->PSFinalCombinerInputsEFG;
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT0_0], definition->PSConstant0, sizeof(definition->PSConstant0));
	memcpy(&D3D__RenderState[D3DRS_PSCONSTANT1_0], definition->PSConstant1, sizeof(definition->PSConstant1));
	memcpy(&D3D__RenderState[D3DRS_PSALPHAOUTPUTS0], definition->PSAlphaOutputs, sizeof(definition->PSAlphaOutputs));
	memcpy(&D3D__RenderState[D3DRS_PSRGBINPUTS0], definition->PSRGBInputs, sizeof(definition->PSRGBInputs));
	D3D__RenderState[D3DRS_PSCOMPAREMODE] = definition->PSCompareMode;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0] = definition->PSFinalCombinerConstant0;
	D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1] = definition->PSFinalCombinerConstant1;
	memcpy(&D3D__RenderState[D3DRS_PSRGBOUTPUTS0], definition->PSRGBOutputs, sizeof(definition->PSRGBOutputs));
	D3D__RenderState[D3DRS_PSCOMBINERCOUNT] = definition->PSCombinerCount;
	D3D__RenderState[D3DRS_PSTEXTUREMODES] = definition->PSTextureModes;
	D3D__RenderState[D3DRS_PSDOTMAPPING] = definition->PSDotMapping;
	D3D__RenderState[D3DRS_PSINPUTTEXTURE] = definition->PSInputTexture;
}

/* ---------- vertex shaders (as d3d8_gl.c) */

static unsigned long vertex_type_bytes(unsigned long type)
{
	switch (type)
	{
	case D3DVSDT_FLOAT1: return 4;
	case D3DVSDT_FLOAT2: return 8;
	case D3DVSDT_FLOAT3: return 12;
	case D3DVSDT_FLOAT4: return 16;
	case D3DVSDT_D3DCOLOR: return 4;
	case D3DVSDT_SHORT1: return 2;
	case D3DVSDT_SHORT2: return 4;
	case D3DVSDT_SHORT3: return 6;
	case D3DVSDT_SHORT4: return 8;
	case D3DVSDT_NORMSHORT1: return 2;
	case D3DVSDT_NORMSHORT2: return 4;
	case D3DVSDT_NORMSHORT3: return 6;
	case D3DVSDT_NORMSHORT4: return 8;
	case D3DVSDT_NORMPACKED3: return 4;
	case D3DVSDT_PBYTE1: return 1;
	case D3DVSDT_PBYTE2: return 2;
	case D3DVSDT_PBYTE3: return 3;
	case D3DVSDT_PBYTE4: return 4;
	case D3DVSDT_FLOAT2H: return 12;
	default: return 0;
	}
}

static void parse_declaration(struct vertex_shader_object *object, const DWORD *declaration)
{
	unsigned long stream = 0;
	unsigned long offsets[16] = { 0 };

	for (; declaration && *declaration != D3DVSD_END(); declaration++)
	{
		DWORD token = *declaration;
		unsigned long token_type = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;

		switch (token_type)
		{
		case D3DVSD_TOKEN_STREAM:
			stream = token & D3DVSD_STREAMNUMBERMASK;
			break;
		case D3DVSD_TOKEN_STREAMDATA:
			if (token & D3DVSD_DATALOADTYPEMASK)
			{
				unsigned long count = (token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT;

				offsets[stream] += (token & 0x08000000) ? count : count * 4;
			}
			else if (object->element_count < XGPU_VERTEX_ATTRIBUTE_COUNT)
			{
				struct vertex_element *element = &object->elements[object->element_count++];

				element->reg = (unsigned char)(token & D3DVSD_VERTEXREGMASK);
				element->stream = (unsigned char)stream;
				element->type = (unsigned char)((token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT);
				element->bytes = (unsigned char)vertex_type_bytes(element->type);
				element->offset = (unsigned short)offsets[stream];
				offsets[stream] += element->bytes;
				if (element->type == D3DVSDT_NORMPACKED3)
					object->packed_mask |= 1UL << element->reg;
			}
			break;
		case D3DVSD_TOKEN_CONSTMEM:
			declaration += ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT) * 4;
			break;
		case D3DVSD_TOKEN_EXT:
			declaration += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
			break;
		default:
			break;
		}
	}
}

HRESULT WINAPI D3DDevice_CreateVertexShader(CONST DWORD *declaration, CONST DWORD *function, DWORD *handle, DWORD usage)
{
	struct vertex_shader_object *object = calloc(1, sizeof(*object));

	(void)usage;
	if (!object)
		return E_OUTOFMEMORY;
	object->signature = VERTEX_SHADER_SIGNATURE;
	object->id = device.next_vertex_shader_id++;
	if (function)
	{
		object->instruction_count = function[0] >> 16;
		object->instructions = malloc(object->instruction_count * 4 * sizeof(DWORD));
		memcpy(object->instructions, function + 1, object->instruction_count * 4 * sizeof(DWORD));
	}
	parse_declaration(object, declaration);
	*handle = (DWORD)object;
	return S_OK;
}

static struct vertex_shader_object *vertex_shader_from_handle(DWORD handle)
{
	struct vertex_shader_object *object = (struct vertex_shader_object *)handle;

	if (!handle || (handle & 1) || object->signature != VERTEX_SHADER_SIGNATURE)
		return NULL;
	return object;
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle((DWORD)handle);

	if (!object || !object->instructions)
		return;
	if (!nv2a_vertex_shader_lighting(object->instructions, object->instruction_count, &object->lighting))
	{
		memset(&object->lighting, 0, sizeof(object->lighting));
		platform_log("GPU: vertex shader %lu is not lit as the pixel shader would light it: lit for each vertex",
			object->id);
	}
}

void WINAPI D3DDevice_DeleteVertexShader(DWORD handle)
{
	(void)handle;
}

void WINAPI D3DDevice_SetVertexShader(DWORD handle)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
	{
		device.vertex_shader = object;
		device.program_address = 0;
		device.program_slots[0] = object;
	}
}

void WINAPI D3DDevice_LoadVertexShader(DWORD handle, DWORD address)
{
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_slots[address] = vertex_shader_from_handle(handle);
}

void WINAPI D3DDevice_SelectVertexShader(DWORD handle, DWORD address)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	if (object)
		device.vertex_shader = object;
	if (address < VERTEX_PROGRAM_SLOTS)
		device.program_address = address;
}

void WINAPI D3DDevice_GetVertexShaderSize(DWORD handle, UINT *size)
{
	struct vertex_shader_object *object = vertex_shader_from_handle(handle);

	*size = object ? object->instruction_count : 0;
}

void WINAPI D3DDevice_SetVertexShaderConstant(INT reg, CONST void *constant_data, DWORD constant_count)
{
	long first = reg + XGPU_VERTEX_CONSTANT_BIAS;

	if (first < 0 || first >= XGPU_VERTEX_CONSTANT_COUNT)
		return;
	if (first + (long)constant_count > XGPU_VERTEX_CONSTANT_COUNT)
		constant_count = XGPU_VERTEX_CONSTANT_COUNT - first;
	constants_store((unsigned long)first, constant_data, constant_count);
}

static struct vertex_shader_object *current_program(void)
{
	struct vertex_shader_object *program = device.program_slots[device.program_address];

	return program ? program : device.vertex_shader;
}

/* ---------- Metal functions */

static unsigned long hash_words(const void *data, unsigned long size)
{
	const DWORD *words = data;
	unsigned long hash = 2166136261UL;

	for (size /= 4; size; size--)
		hash = (hash ^ *words++) * 16777619UL;
	return hash;
}

static BOOL words_equal(const void *a, const void *b, unsigned long size)
{
	const DWORD *words_a = a;
	const DWORD *words_b = b;
	DWORD difference = 0;
	unsigned long index;

	for (index = 0; index < size / 4; index++)
		difference |= words_a[index] ^ words_b[index];
	return difference == 0;
}

static void shader_dump(const char *name, const char *source)
{
	char path[512];
	FILE *file;

	if (!debug_settings.dump_shaders)
		return;
	snprintf(path, sizeof(path), "%s/%s.metal", debug_settings.dump_shaders, name);
	if ((file = fopen(path, "w")) != NULL)
	{
		fputs(source, file);
		fclose(file);
	}
}

static uint32_t function_compile(const char *source, const char *what)
{
	static char log[8192];
	uint32_t function = host_metal_function_compile(source, "main0", log, sizeof(log));

	if (!function)
		platform_log("cannot compile the %s shader:\n%s\n%s", what, log, source);
	return function;
}

/* the program's vertex function for these inputs, compiled the first time;
0 if it does not compile (it is not tried again) */
static uint32_t vertex_function_get(struct vertex_shader_object *program, unsigned long streamed, unsigned long packed,
	unsigned long integer, BOOL lit)
{
	struct vertex_function_entry *entry = program->recent;

	if (!entry || entry->streamed != streamed || entry->packed != packed || entry->integer != integer ||
		entry->lit != lit)
	{
		for (entry = program->functions; entry; entry = entry->next)
		{
			if (entry->streamed == streamed && entry->packed == packed && entry->integer == integer && entry->lit == lit)
				break;
		}
	}
	if (!entry)
	{
		char *source;

		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return 0;
		entry->streamed = streamed;
		entry->packed = packed;
		entry->integer = integer;
		entry->lit = lit;
		source = nv2a_vertex_shader_to_msl(program->instructions, program->instruction_count, packed, streamed,
			integer, lit ? &program->lighting : NULL);
		if (source)
		{
			char name[64];

			entry->function = function_compile(source, "vertex");
			snprintf(name, sizeof(name), "vs%03lu_%08lx%s", program->id, streamed, lit ? "_lit" : "");
			shader_dump(name, source);
			free(source);
		}
		entry->next = program->functions;
		program->functions = entry;
	}
	program->recent = entry;
	return entry->function;
}

struct fragment_entry
{
	struct fragment_entry *next;
	unsigned long hash;
	struct nv2a_pixel_shader_key key;
	uint32_t function;
};

#define FRAGMENT_BUCKETS 1024

static struct fragment_entry *fragment_buckets[FRAGMENT_BUCKETS];

typedef char pixel_shader_key_size_assert[sizeof(struct nv2a_pixel_shader_key) % 4 == 0 ? 1 : -1];

static uint32_t fragment_function_get(const struct nv2a_pixel_shader_key *key)
{
#define RECENT_FRAGMENT_COUNT 4
	static struct fragment_entry *recent[RECENT_FRAGMENT_COUNT];
	static unsigned long recent_next;
	unsigned long hash, index;
	struct fragment_entry **bucket;
	struct fragment_entry *entry;
	char *source;

	for (index = 0; index < RECENT_FRAGMENT_COUNT; index++)
	{
		if (recent[index] && words_equal(&recent[index]->key, key, sizeof(*key)))
			return recent[index]->function;
	}
	hash = hash_words(key, sizeof(*key));
	bucket = &fragment_buckets[hash % FRAGMENT_BUCKETS];
	for (entry = *bucket; entry; entry = entry->next)
	{
		if (entry->hash == hash && words_equal(&entry->key, key, sizeof(*key)))
		{
			recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
			return entry->function;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return 0;
	entry->hash = hash;
	entry->key = *key;
	source = nv2a_pixel_shader_to_msl(key);
	if (source)
	{
		char name[32];

		entry->function = function_compile(source, "pixel");
		snprintf(name, sizeof(name), "ps_%08lx", hash);
		shader_dump(name, source);
		free(source);
	}
	entry->next = *bucket;
	*bucket = entry;
	recent[recent_next++ % RECENT_FRAGMENT_COUNT] = entry;
	return entry->function;
}

/* ---------- pipelines, depth and stencil states, samplers

Each is made once for each combination of what it holds and kept in a hash
table of its description's bytes (descriptions are cleared whole before
they are filled, so that equal ones compare equal). */

struct object_entry
{
	struct object_entry *next;
	unsigned long hash;
	uint32_t object;
	/* (the description, its size the table's) */
	DWORD description[1];
};

struct object_table
{
	struct object_entry *buckets[1024];
	unsigned long size;
	/* the latest found, which the next draw mostly asks for again */
	struct object_entry *recent;
};

static struct object_entry *object_find(struct object_table *table, const void *description, unsigned long *hash)
{
	struct object_entry *entry = table->recent;

	if (entry && words_equal(entry->description, description, table->size))
		return entry;
	*hash = hash_words(description, table->size);
	for (entry = table->buckets[*hash % 1024]; entry; entry = entry->next)
	{
		if (entry->hash == *hash && words_equal(entry->description, description, table->size))
		{
			table->recent = entry;
			return entry;
		}
	}
	return NULL;
}

static struct object_entry *object_add(struct object_table *table, const void *description, unsigned long hash,
	uint32_t object)
{
	struct object_entry *entry = malloc(offsetof(struct object_entry, description) + table->size);

	if (!entry)
		return NULL;
	entry->hash = hash;
	entry->object = object;
	memcpy(entry->description, description, table->size);
	entry->next = table->buckets[hash % 1024];
	table->buckets[hash % 1024] = entry;
	table->recent = entry;
	return entry;
}

static struct object_table pipelines = { { 0 }, sizeof(struct halo_metal_pipeline_description), 0 };
static struct object_table depth_stencils = { { 0 }, sizeof(struct halo_metal_depth_stencil_description), 0 };
static struct object_table samplers = { { 0 }, sizeof(struct halo_metal_sampler_description), 0 };

/* (one that fails is remembered as 0, and not made again) */
static uint32_t pipeline_get(const struct halo_metal_pipeline_description *description)
{
	static char log[4096];
	unsigned long hash = 0;
	struct object_entry *entry = object_find(&pipelines, description, &hash);
	uint32_t pipeline;

	if (entry)
		return entry->object;
	pipeline = host_metal_pipeline_create(description, log, sizeof(log));
	if (!pipeline)
		platform_log("GPU: cannot make a pipeline state: %s", log);
	object_add(&pipelines, description, hash, pipeline);
	return pipeline;
}

static uint32_t depth_stencil_get(const struct halo_metal_depth_stencil_description *description)
{
	unsigned long hash = 0;
	struct object_entry *entry = object_find(&depth_stencils, description, &hash);
	uint32_t state;

	if (entry)
		return entry->object;
	state = host_metal_depth_stencil_create(description);
	object_add(&depth_stencils, description, hash, state);
	return state;
}

static uint32_t sampler_get(const struct halo_metal_sampler_description *description)
{
	unsigned long hash = 0;
	struct object_entry *entry = object_find(&samplers, description, &hash);
	uint32_t sampler;

	if (entry)
		return entry->object;
	sampler = host_metal_sampler_create(description);
	object_add(&samplers, description, hash, sampler);
	return sampler;
}

/* Direct3D's (GL's) comparisons, stencil operations and blend factors in
Metal's terms */
static uint32_t compare_function(DWORD function)
{
	/* D3DCMP_NEVER (GL_NEVER, 0x200) to D3DCMP_ALWAYS (0x207), Metal's in
	the same order; 0 is never too */
	return function >= 0x200 && function <= 0x207 ? function - 0x200 : HALO_METAL_COMPARE_NEVER;
}

static uint32_t stencil_operation(DWORD operation)
{
	switch (operation)
	{
	case 0: return HALO_METAL_STENCIL_ZERO;
	case D3DSTENCILOP_KEEP: return HALO_METAL_STENCIL_KEEP;
	case D3DSTENCILOP_REPLACE: return HALO_METAL_STENCIL_REPLACE;
	case D3DSTENCILOP_INCRSAT: return HALO_METAL_STENCIL_INCREMENT_CLAMP;
	case D3DSTENCILOP_DECRSAT: return HALO_METAL_STENCIL_DECREMENT_CLAMP;
	case D3DSTENCILOP_INVERT: return HALO_METAL_STENCIL_INVERT;
	case D3DSTENCILOP_INCR: return HALO_METAL_STENCIL_INCREMENT_WRAP;
	case D3DSTENCILOP_DECR: return HALO_METAL_STENCIL_DECREMENT_WRAP;
	default: return HALO_METAL_STENCIL_KEEP;
	}
}

static uint32_t blend_factor(DWORD factor)
{
	switch (factor)
	{
	case D3DBLEND_ZERO: return HALO_METAL_BLEND_ZERO;
	case D3DBLEND_ONE: return HALO_METAL_BLEND_ONE;
	case D3DBLEND_SRCCOLOR: return HALO_METAL_BLEND_SOURCE_COLOR;
	case D3DBLEND_INVSRCCOLOR: return HALO_METAL_BLEND_ONE_MINUS_SOURCE_COLOR;
	case D3DBLEND_SRCALPHA: return HALO_METAL_BLEND_SOURCE_ALPHA;
	case D3DBLEND_INVSRCALPHA: return HALO_METAL_BLEND_ONE_MINUS_SOURCE_ALPHA;
	case D3DBLEND_DESTALPHA: return HALO_METAL_BLEND_DESTINATION_ALPHA;
	case D3DBLEND_INVDESTALPHA: return HALO_METAL_BLEND_ONE_MINUS_DESTINATION_ALPHA;
	case D3DBLEND_DESTCOLOR: return HALO_METAL_BLEND_DESTINATION_COLOR;
	case D3DBLEND_INVDESTCOLOR: return HALO_METAL_BLEND_ONE_MINUS_DESTINATION_COLOR;
	case D3DBLEND_SRCALPHASAT: return HALO_METAL_BLEND_SOURCE_ALPHA_SATURATED;
	case D3DBLEND_CONSTANTCOLOR: return HALO_METAL_BLEND_BLEND_COLOR;
	case D3DBLEND_INVCONSTANTCOLOR: return HALO_METAL_BLEND_ONE_MINUS_BLEND_COLOR;
	case D3DBLEND_CONSTANTALPHA: return HALO_METAL_BLEND_BLEND_ALPHA;
	case D3DBLEND_INVCONSTANTALPHA: return HALO_METAL_BLEND_ONE_MINUS_BLEND_ALPHA;
	default: return HALO_METAL_BLEND_ONE;
	}
}

static uint32_t blend_operation(DWORD operation)
{
	switch (operation)
	{
	case D3DBLENDOP_SUBTRACT: return HALO_METAL_BLEND_SUBTRACT;
	case D3DBLENDOP_REVSUBTRACT:
	case D3DBLENDOP_REVSUBTRACTSIGNED: return HALO_METAL_BLEND_REVERSE_SUBTRACT;
	case D3DBLENDOP_MIN: return HALO_METAL_BLEND_MIN;
	case D3DBLENDOP_MAX: return HALO_METAL_BLEND_MAX;
	default: return HALO_METAL_BLEND_ADD;
	}
}

static uint32_t address_mode(DWORD mode)
{
	switch (mode)
	{
	case D3DTADDRESS_MIRROR: return HALO_METAL_ADDRESS_MIRROR;
	case D3DTADDRESS_CLAMP: return HALO_METAL_ADDRESS_CLAMP;
	case D3DTADDRESS_BORDER: return HALO_METAL_ADDRESS_BORDER;
	case D3DTADDRESS_CLAMPTOEDGE: return HALO_METAL_ADDRESS_CLAMP;
	default: return HALO_METAL_ADDRESS_REPEAT;
	}
}

/* a stage's sampler (configure_sampler in d3d8_gl.c); Metal's border colors
are transparent black, opaque black and opaque white only */
static uint32_t stage_sampler(int stage, BOOL mipmapped, BOOL hires)
{
	DWORD *state = D3D__TextureState[stage];
	struct halo_metal_sampler_description description;
	DWORD min_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MINFILTER];
	DWORD mip_filter = hires ? D3DTEXF_LINEAR : mipmapped ? state[D3DTSS_MIPFILTER] : D3DTEXF_NONE;
	DWORD mag_filter = hires ? D3DTEXF_LINEAR : state[D3DTSS_MAGFILTER];
	DWORD border = state[D3DTSS_BORDERCOLOR];

	memset(&description, 0, sizeof(description));
	description.minification = min_filter == D3DTEXF_POINT ? 0 : 1;
	description.magnification = mag_filter == D3DTEXF_POINT ? 0 : 1;
	description.mipmaps = mip_filter == D3DTEXF_NONE ? 0 : mip_filter == D3DTEXF_POINT ? 1 : 2;
	description.address_u = address_mode(state[D3DTSS_ADDRESSU]);
	description.address_v = address_mode(state[D3DTSS_ADDRESSV]);
	description.address_w = address_mode(state[D3DTSS_ADDRESSW]);
	description.anisotropy = min_filter == D3DTEXF_ANISOTROPIC && state[D3DTSS_MAXANISOTROPY] > 1 ?
		(state[D3DTSS_MAXANISOTROPY] > 16 ? 16 : state[D3DTSS_MAXANISOTROPY]) : 1;
	description.lod_minimum = hires ? 0.0f : (float)state[D3DTSS_MAXMIPLEVEL];
	description.lod_maximum = 1000.0f;
	description.border = !(border >> 24) ? HALO_METAL_BORDER_TRANSPARENT_BLACK :
		(border & 0xffffff) ? HALO_METAL_BORDER_OPAQUE_WHITE : HALO_METAL_BORDER_OPAQUE_BLACK;
	return sampler_get(&description);
}

/* ---------- render targets sampled with their mip chain (as d3d8_gl.c)

The levels' targets are copied into a texture with all of them; the levels
the game did not draw are made from level 0. */

#define MIP_COMPOSITE_LEVELS 16

struct mip_composite
{
	struct mip_composite *next;
	unsigned long data, width, height, levels;
	uint32_t texture;
	unsigned long rendered_levels;
	uint32_t level_sources[MIP_COMPOSITE_LEVELS];
	unsigned long level_written[MIP_COMPOSITE_LEVELS];
};

static struct mip_composite *mip_composites;

static BOOL render_target_stale(const struct render_target_entry *entry)
{
	switch (entry->kind)
	{
	case _render_target_screen:
		return entry->target.scale[0] != screen_scale[0] || entry->target.scale[1] != screen_scale[1] ||
			(entry->target.height == SCREEN_HEIGHT && entry->target.width != (unsigned long)halo_screen_width());
	case _render_target_shadow_map:
		return entry->target.scale[0] != (float)halo_shadow_map_scale();
	default:
		return FALSE;
	}
}

static void render_target_delete(struct render_target_entry *entry)
{
	struct render_target_entry **link;
	struct mip_composite *composite;

	for (link = render_target_bucket(entry->target.data); *link != entry; link = &(*link)->next_in_bucket)
		;
	*link = entry->next_in_bucket;
	for (composite = mip_composites; composite; composite = composite->next)
	{
		unsigned long level;

		for (level = 0; level < composite->rendered_levels; level++)
		{
			if (composite->level_sources[level] == entry->target.texture)
			{
				composite->rendered_levels = 0;
				break;
			}
		}
	}
	if (pass.color == &entry->target || pass.depth == &entry->target)
		pass.open = FALSE;
	host_metal_texture_delete(entry->target.texture);
	if (entry->target.multisample)
		host_metal_texture_delete(entry->target.multisample);
	free(entry);
}

static void render_targets_evict(void)
{
	struct render_target_entry *entry, **link = &render_targets;
	BOOL any = FALSE;

	while ((entry = *link) != NULL)
	{
		if (render_target_stale(entry))
		{
			*link = entry->next;
			render_target_delete(entry);
			any = TRUE;
		}
		else
		{
			link = &entry->next;
		}
	}
	if (any)
		memset(recent_render_targets, 0, sizeof(recent_render_targets));
}

/* a region of a whole level of a 2D texture */
static void level_region(struct halo_metal_region *region, unsigned long level, unsigned long width,
	unsigned long height)
{
	memset(region, 0, sizeof(*region));
	region->level = (uint32_t)level;
	region->width = (uint32_t)width;
	region->height = (uint32_t)height;
	region->depth = 1;
}

static uint32_t mip_composite_get(const struct xgpu_texture_description *description, unsigned long data)
{
	struct mip_composite *composite;
	struct xgpu_render_target *targets[MIP_COMPOSITE_LEVELS];
	unsigned long level, rendered_levels = 0;
	BOOL changed;

	for (composite = mip_composites; composite; composite = composite->next)
	{
		if (composite->data == data && composite->width == description->width &&
			composite->height == description->height && composite->levels == description->levels)
		{
			break;
		}
	}
	if (!composite)
	{
		struct halo_metal_texture_description metal;

		composite = calloc(1, sizeof(*composite));
		if (!composite)
			return 0;
		composite->data = data;
		composite->width = description->width;
		composite->height = description->height;
		composite->levels = description->levels;
		memset(&metal, 0, sizeof(metal));
		metal.type = HALO_METAL_TEXTURE_2D;
		metal.format = HALO_METAL_FORMAT_BGRA8;
		metal.width = (uint32_t)description->width;
		metal.height = (uint32_t)description->height;
		metal.depth = 1;
		metal.levels = (uint32_t)description->levels;
		metal.samples = 1;
		metal.usage = HALO_METAL_USAGE_SAMPLED | HALO_METAL_USAGE_RENDER_TARGET;
		composite->texture = host_metal_texture_create(&metal);
		composite->rendered_levels = ~0UL;
		composite->next = mip_composites;
		mip_composites = composite;
	}
	for (level = 0; level < description->levels && level < MIP_COMPOSITE_LEVELS; level++)
	{
		unsigned long width = description->width >> level ? description->width >> level : 1;
		unsigned long height = description->height >> level ? description->height >> level : 1;
		struct xgpu_render_target *target =
			xgpu_render_target_find(data + xgpu_texture_level_offset(description, level));

		if (!target || target->width != width || target->height != height ||
			target->gl_width != width || target->gl_height != height)
			break;
		targets[level] = target;
		rendered_levels++;
	}
	changed = rendered_levels != composite->rendered_levels;
	for (level = 0; level < rendered_levels && !changed; level++)
	{
		changed = targets[level]->texture != composite->level_sources[level] ||
			targets[level]->written != composite->level_written[level];
	}
	if (!changed || !composite->texture)
		return composite->texture;
	composite->rendered_levels = rendered_levels;
	for (level = 0; level < rendered_levels; level++)
	{
		struct halo_metal_region source, destination;

		composite->level_sources[level] = targets[level]->texture;
		composite->level_written[level] = targets[level]->written;
		level_region(&source, 0, targets[level]->width, targets[level]->height);
		level_region(&destination, level, targets[level]->width, targets[level]->height);
		host_metal_texture_copy(targets[level]->texture, &source, composite->texture, &destination);
	}
	/* levels the game did not render come from the ones it did */
	if (rendered_levels < description->levels)
		host_metal_texture_generate_mipmaps(composite->texture, rendered_levels ? (uint32_t)rendered_levels - 1 : 0);
	return composite->texture;
}

/* Metal draws no pass that samples a texture it draws into, which GL draws
sampling its pixels as they were: a copy of the target is sampled instead,
made again only once the target has been drawn into since (as the web
build's feedback_copy) */
static uint32_t feedback_copy(struct xgpu_render_target *target)
{
	static uint32_t copy;
	static unsigned long copy_width, copy_height;
	static uint32_t copied_texture;
	static unsigned long copied_written;
	struct halo_metal_region region;

	if (!copy || copy_width != target->gl_width || copy_height != target->gl_height)
	{
		struct halo_metal_texture_description description;

		if (copy)
			host_metal_texture_delete(copy);
		memset(&description, 0, sizeof(description));
		description.type = HALO_METAL_TEXTURE_2D;
		description.format = HALO_METAL_FORMAT_BGRA8;
		description.width = (uint32_t)target->gl_width;
		description.height = (uint32_t)target->gl_height;
		description.depth = 1;
		description.levels = 1;
		description.samples = 1;
		description.usage = HALO_METAL_USAGE_SAMPLED;
		copy = host_metal_texture_create(&description);
		copy_width = target->gl_width;
		copy_height = target->gl_height;
		copied_texture = 0;
	}
	if (copy && (copied_texture != target->texture || copied_written != target->written))
	{
		level_region(&region, 0, target->gl_width, target->gl_height);
		host_metal_texture_copy(target->texture, &region, copy, &region);
		copied_texture = target->texture;
		copied_written = target->written;
	}
	return copy ? copy : target->texture;
}

static unsigned long stage_texture_mode(int stage)
{
	return (D3D__RenderState[D3DRS_PSTEXTUREMODES] >> (5 * stage)) & 0x1f;
}

static void bind_textures(struct nv2a_pixel_shader_key *key, float texture_scale[4][4], struct halo_metal_draw *draw)
{
	int stage;

	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		D3DBaseTexture *texture = device.textures[stage];
		unsigned long mode = stage_texture_mode(stage);
		struct xgpu_render_target *target;
		struct xgpu_texture_description description;
		GLenum gl_target = GL_TEXTURE_2D;
		uint32_t handle;

		texture_scale[stage][0] = texture_scale[stage][1] = 1.0f;
		texture_scale[stage][2] = texture_scale[stage][3] = 1.0f;
		draw->textures[stage] = 0;
		draw->samplers[stage] = 0;
		if (!texture || !texture->Data || mode == 0 || mode == 0x04 || mode == 0x05 || mode == 0x11)
		{
			key->sampler_type[stage] = mode == 0x11 ? _xgpu_sampler_2d : _xgpu_sampler_none;
			continue;
		}
		target = xgpu_render_target_find(texture->Data);
		if (target)
		{
			xgpu_texture_describe(texture->Format, texture->Size, &description);
			handle = target->texture;
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)target->width;
				texture_scale[stage][1] = 1.0f / (float)target->height;
			}
			if (!description.linear && !description.cube_map && description.levels > 1 &&
				target->width == description.width && target->height == description.height)
				handle = mip_composite_get(&description, texture->Data);
			else
				description.levels = 1;
		}
		else
		{
			const D3DCOLOR *palette = device.palettes[stage] && device.palettes[stage]->Data ?
				(const D3DCOLOR *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.palettes[stage]->Data) : NULL;

			handle = xgpu_texture_get((const DWORD *)texture, palette, &gl_target, &description);
			if (description.linear)
			{
				texture_scale[stage][0] = 1.0f / (float)description.width;
				texture_scale[stage][1] = 1.0f / (float)description.height;
			}
		}
		if (!handle)
		{
			/* (nothing to sample: a texture not yet uploaded, or not drawn) */
			key->sampler_type[stage] = _xgpu_sampler_none;
			continue;
		}
		draw->textures[stage] = handle;
		draw->samplers[stage] = stage_sampler(stage, description.levels > 1, description.hires);
		if (stage == 0)
		{
			key->coverage_alpha = description.hires_coverage != FALSE;
			key->point_threshold = description.hires_point_threshold != FALSE;
		}
		key->sampler_type[stage] = gl_target == GL_TEXTURE_CUBE_MAP ? _xgpu_sampler_cube :
			gl_target == GL_TEXTURE_3D ? _xgpu_sampler_3d : _xgpu_sampler_2d;
	}
}

/* (after the targets: a stage that samples the pass's color target samples
a copy of it) */
static void resolve_feedback(struct halo_metal_draw *draw)
{
	int stage;

	if (!pass.color)
		return;
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		if (draw->textures[stage] && draw->textures[stage] == pass.color->texture)
			draw->textures[stage] = feedback_copy(pass.color);
	}
}

/* ---------- uniforms (nv2a_vsh.c's and nv2a_psh.c's structures) */

struct vertex_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float screen_offset;
	float unused[2];
	float attributes[XGPU_VERTEX_ATTRIBUTE_COUNT][4];
};

struct fragment_uniforms
{
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float texture_lod_bias[4];
	float blend_constant[4];
	float alpha_reference;
	float unused[3];
	float model_lights[XGPU_MODEL_LIGHT_COUNT][4];
};

/* each the latest draw's, and a serial that changes when they do */
static struct vertex_uniforms vertex_uniforms;
static struct fragment_uniforms fragment_uniforms;
static uint32_t vertex_uniforms_serial, fragment_uniforms_serial;

static unsigned long model_light_register(int light)
{
	return (unsigned long)(XGPU_VERTEX_CONSTANT_BIAS + (light ? -80 + light : -82));
}

static void uniforms_update(const float texture_scale[4][4], BOOL immediate)
{
	struct vertex_uniforms vertex;
	struct fragment_uniforms fragment;
	int stage, light;

	memset(&vertex, 0, sizeof(vertex));
	memcpy(vertex.viewport_scale, device.viewport_scale, sizeof(vertex.viewport_scale));
	memcpy(vertex.viewport_offset, device.viewport_offset, sizeof(vertex.viewport_offset));
	vertex.point_size = D3D__RenderState[D3DRS_POINTSIZE] ? dword_to_float(D3D__RenderState[D3DRS_POINTSIZE]) : 1.0f;
	vertex.screen_offset = (float)UI_OFFSET;
	memcpy(vertex.attributes, device.attributes, sizeof(vertex.attributes));
	(void)immediate;
	if (!vertex_uniforms_serial || !words_equal(&vertex, &vertex_uniforms, sizeof(vertex)))
	{
		vertex_uniforms = vertex;
		vertex_uniforms_serial++;
	}

	memset(&fragment, 0, sizeof(fragment));
	for (stage = 0; stage < 8; stage++)
	{
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT0_0 + stage], fragment.ps_c0[stage]);
		color_to_vec4(D3D__RenderState[D3DRS_PSCONSTANT1_0 + stage], fragment.ps_c1[stage]);
	}
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT0], fragment.ps_final_c0);
	color_to_vec4(D3D__RenderState[D3DRS_PSFINALCOMBINERCONSTANT1], fragment.ps_final_c1);
	color_to_vec4(D3D__RenderState[D3DRS_FOGCOLOR], fragment.fog_color);
	fragment.fog_parameters[0] = dword_to_float(D3D__RenderState[D3DRS_FOGSTART]);
	fragment.fog_parameters[1] = dword_to_float(D3D__RenderState[D3DRS_FOGEND]);
	fragment.fog_parameters[2] = dword_to_float(D3D__RenderState[D3DRS_FOGDENSITY]);
	fragment.alpha_reference = (float)(D3D__RenderState[D3DRS_ALPHAREF] & 0xff);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		DWORD *state = D3D__TextureState[stage];

		fragment.bump_matrix[stage][0] = dword_to_float(state[D3DTSS_BUMPENVMAT00]);
		fragment.bump_matrix[stage][1] = dword_to_float(state[D3DTSS_BUMPENVMAT01]);
		fragment.bump_matrix[stage][2] = dword_to_float(state[D3DTSS_BUMPENVMAT10]);
		fragment.bump_matrix[stage][3] = dword_to_float(state[D3DTSS_BUMPENVMAT11]);
		fragment.bump_luminance[stage][0] = dword_to_float(state[D3DTSS_BUMPENVLSCALE]);
		fragment.bump_luminance[stage][1] = dword_to_float(state[D3DTSS_BUMPENVLOFFSET]);
		fragment.texture_lod_bias[stage] = dword_to_float(state[D3DTSS_MIPMAPLODBIAS]);
	}
	memcpy(fragment.texture_scale, texture_scale, sizeof(fragment.texture_scale));
	color_to_vec4(D3D__RenderState[D3DRS_BLENDCOLOR], fragment.blend_constant);
	for (light = 0; light < XGPU_MODEL_LIGHT_COUNT; light++)
		memcpy(fragment.model_lights[light], device.constants[model_light_register(light)], sizeof(fragment.model_lights[0]));
	if (!fragment_uniforms_serial || !words_equal(&fragment, &fragment_uniforms, sizeof(fragment)))
	{
		fragment_uniforms = fragment;
		fragment_uniforms_serial++;
	}
}

/* ---------- per-draw state */

/* display.per_pixel_lighting (d3d8_gl.c) */
static BOOL per_pixel_lighting(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static BOOL enabled;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		enabled = config_boolean("display.per_pixel_lighting") != 0;
	}
	return enabled;
}

/* the raster state into the draw, and the blend into the pipeline's
description (apply_raster_state in d3d8_gl.c) */
static void raster_state(BOOL has_depth, struct halo_metal_draw *draw, struct halo_metal_pipeline_description *pipeline)
{
	DWORD *rs = D3D__RenderState;
	DWORD write = rs[D3DRS_COLORWRITEENABLE];
	long viewport[4];
	BOOL depth_test = has_depth && rs[D3DRS_ZENABLE];
	struct halo_metal_depth_stencil_description depth;

	viewport[0] = target_pixel((float)device.viewport.X, 0);
	viewport[1] = target_pixel((float)device.viewport.Y, 1);
	viewport[2] = target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - viewport[0];
	viewport[3] = target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - viewport[1];
	draw->viewport[0] = (float)viewport[0];
	draw->viewport[1] = (float)viewport[1];
	draw->viewport[2] = (float)viewport[2];
	draw->viewport[3] = (float)viewport[3];
	draw->viewport[4] = device.viewport.MinZ;
	draw->viewport[5] = device.viewport.MaxZ;
	/* the NV2A's scissor follows the viewport, which keeps a split-screen
	window's geometry out of the other (d3d8_gl.c); Metal's must lie inside
	the targets */
	{
		long x0 = viewport[0] < 0 ? 0 : viewport[0], y0 = viewport[1] < 0 ? 0 : viewport[1];
		long x1 = viewport[0] + viewport[2], y1 = viewport[1] + viewport[3];

		if (x1 > (long)target_pixels[0])
			x1 = (long)target_pixels[0];
		if (y1 > (long)target_pixels[1])
			y1 = (long)target_pixels[1];
		draw->scissor[0] = (int32_t)x0;
		draw->scissor[1] = (int32_t)y0;
		draw->scissor[2] = (int32_t)(x1 > x0 ? x1 - x0 : 0);
		draw->scissor[3] = (int32_t)(y1 > y0 ? y1 - y0 : 0);
	}

	memset(&depth, 0, sizeof(depth));
	depth.depth_compare = depth_test ? compare_function(rs[D3DRS_ZFUNC]) : HALO_METAL_COMPARE_ALWAYS;
	depth.depth_write = depth_test && rs[D3DRS_ZWRITEENABLE];
	if (has_depth && rs[D3DRS_STENCILENABLE])
	{
		depth.stencil = 1;
		depth.stencil_compare = compare_function(rs[D3DRS_STENCILFUNC]);
		depth.stencil_fail = stencil_operation(rs[D3DRS_STENCILFAIL]);
		depth.depth_fail = stencil_operation(rs[D3DRS_STENCILZFAIL]);
		depth.pass = stencil_operation(rs[D3DRS_STENCILPASS]);
		depth.read_mask = rs[D3DRS_STENCILMASK] & 0xff;
		depth.write_mask = rs[D3DRS_STENCILWRITEMASK] & 0xff;
		draw->stencil_reference = rs[D3DRS_STENCILREF] & 0xff;
	}
	draw->depth_stencil = depth_stencil_get(&depth);

	pipeline->blend = rs[D3DRS_ALPHABLENDENABLE] != 0;
	if (pipeline->blend)
	{
		uint32_t operation = blend_operation(rs[D3DRS_BLENDOP]);

		pipeline->source_rgb = pipeline->source_alpha = blend_factor(rs[D3DRS_SRCBLEND]);
		pipeline->destination_rgb = pipeline->destination_alpha = blend_factor(rs[D3DRS_DESTBLEND]);
		pipeline->operation_rgb = pipeline->operation_alpha = operation;
		color_to_vec4(rs[D3DRS_BLENDCOLOR], draw->blend_color);
	}
	pipeline->write_mask = ((write & D3DCOLORWRITEENABLE_RED) ? 1u : 0u) | ((write & D3DCOLORWRITEENABLE_GREEN) ? 2u : 0u) |
		((write & D3DCOLORWRITEENABLE_BLUE) ? 4u : 0u) | ((write & D3DCOLORWRITEENABLE_ALPHA) ? 8u : 0u);

	/* the cull mode names the winding to discard; FRONTFACE names the front
	winding (as desktop GL's, whose clip space is Metal's here) */
	draw->cull = rs[D3DRS_CULLMODE] == D3DCULL_NONE ? HALO_METAL_CULL_NONE :
		rs[D3DRS_CULLMODE] == rs[D3DRS_FRONTFACE] ? HALO_METAL_CULL_FRONT : HALO_METAL_CULL_BACK;
	draw->front_face_counterclockwise = rs[D3DRS_FRONTFACE] == D3DFRONT_CCW;
	draw->wireframe = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME;
	if (rs[D3DRS_SOLIDOFFSETENABLE])
	{
		draw->depth_slope = dword_to_float(rs[D3DRS_POLYGONOFFSETZSLOPESCALE]);
		draw->depth_bias = dword_to_float(rs[D3DRS_POLYGONOFFSETZOFFSET]);
	}
}

/* what a draw has before its vertices are known */
struct draw_context
{
	struct vertex_shader_object *program;
	uint32_t fragment_function;
	BOOL lit;
	struct halo_metal_draw draw;
	struct halo_metal_pipeline_description pipeline;
};

static BOOL prepare_draw(BOOL immediate, struct draw_context *context)
{
	struct vertex_shader_object *program = current_program();
	struct nv2a_pixel_shader_key key;
	float texture_scale[4][4];
	BOOL has_depth = FALSE;
	int stage;

	if (!device.metal_ready || !program || !device.vertex_shader || !program->instructions)
	{
		stats.skipped_no_program++;
		return FALSE;
	}
	{
		const char *skip = debug_settings.skip_vertex_shaders;

		while (skip && *skip)
		{
			if ((unsigned long)atol(skip) == program->id)
				return FALSE;
			skip = strchr(skip, ',');
			if (skip)
				skip++;
		}
	}
	memset(context, 0, sizeof(*context));
	context->program = program;
	memset(&key, 0, sizeof(key));
	memcpy(key.combiner_state, D3D__RenderState, sizeof(key.combiner_state));
	memset(&key.combiner_state[D3DRS_PSCONSTANT0_0], 0, 16 * sizeof(DWORD));
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT0] = 0;
	key.combiner_state[D3DRS_PSFINALCOMBINERCONSTANT1] = 0;
	key.texture_modes = D3D__RenderState[D3DRS_PSTEXTUREMODES];
	/* the textures before the targets: making a mip composite copies
	targets' pixels, outside any pass */
	bind_textures(&key, texture_scale, &context->draw);
	if (!bind_targets(&has_depth))
	{
		stats.skipped_no_target++;
		return FALSE;
	}
	resolve_feedback(&context->draw);
	raster_state(has_depth, &context->draw, &context->pipeline);
	for (stage = 0; stage < D3DTSS_MAXSTAGES; stage++)
	{
		key.alpha_kill[stage] = D3D__TextureState[stage][D3DTSS_ALPHAKILL] == D3DTALPHAKILL_ENABLE;
		key.color_sign[stage] = (unsigned char)((D3D__TextureState[stage][D3DTSS_COLORSIGN] >> 28) & 0xf);
	}
	key.coverage_alpha = key.coverage_alpha && D3D__RenderState[D3DRS_ALPHABLENDENABLE] &&
		D3D__RenderState[D3DRS_SRCBLEND] == D3DBLEND_CONSTANTCOLOR &&
		D3D__RenderState[D3DRS_DESTBLEND] == D3DBLEND_SRCALPHA;
	key.point_threshold = key.point_threshold && key.coverage_alpha;
	key.alpha_test_function = D3D__RenderState[D3DRS_ALPHATESTENABLE] ? D3D__RenderState[D3DRS_ALPHAFUNC] : 0;
	if (pass.samples > 1 && key.alpha_test_function && !D3D__RenderState[D3DRS_ALPHABLENDENABLE])
		key.alpha_test_samples = (unsigned char)pass.samples;
	key.fog_enable = D3D__RenderState[D3DRS_FOGENABLE] != 0;
	key.fog_table_mode = (unsigned char)D3D__RenderState[D3DRS_FOGTABLEMODE];
	if (program->lighting.lights && !program->lighting_failed && per_pixel_lighting())
	{
		key.per_pixel_lighting = (unsigned char)program->lighting.lights;
		context->lit = TRUE;
	}
	context->fragment_function = fragment_function_get(&key);
	if (!context->fragment_function && context->lit)
	{
		platform_log("GPU: vertex shader %lu cannot be lit for each pixel here (refer to the shader log above): "
			"lit for each vertex", program->id);
		program->lighting_failed = TRUE;
		key.per_pixel_lighting = 0;
		context->lit = FALSE;
		context->fragment_function = fragment_function_get(&key);
	}
	if (!context->fragment_function)
	{
		stats.skipped_pipeline++;
		return FALSE;
	}
	uniforms_update(texture_scale, immediate);
	context->pipeline.fragment_function = context->fragment_function;
	context->pipeline.color_format = pass.color ? HALO_METAL_FORMAT_BGRA8 : HALO_METAL_FORMAT_NONE;
	context->pipeline.depth_format = pass.depth ? HALO_METAL_FORMAT_DEPTH32F_STENCIL8 : HALO_METAL_FORMAT_NONE;
	context->pipeline.samples = pass.samples > 1 ? (uint32_t)pass.samples : 1;
	context->draw.vertex_constants = (uint32_t)device.constants;
	context->draw.vertex_constants_size = sizeof(device.constants);
	context->draw.vertex_constants_serial = (uint32_t)constants_serial;
	context->draw.vertex_uniforms = (uint32_t)&vertex_uniforms;
	context->draw.vertex_uniforms_size = sizeof(vertex_uniforms);
	context->draw.vertex_uniforms_serial = vertex_uniforms_serial;
	context->draw.fragment_uniforms = (uint32_t)&fragment_uniforms;
	context->draw.fragment_uniforms_size = sizeof(fragment_uniforms);
	context->draw.fragment_uniforms_serial = fragment_uniforms_serial;
	if (device.visibility_test_active && device.visibility_counter < VISIBILITY_TEST_SLOTS)
		context->draw.visibility = 1 + (uint32_t)(device.visibility_counter * sizeof(uint64_t));
	if (immediate)
		stats.immediate_draws++;
	else
		stats.draws++;
	return TRUE;
}

/* the draw's vertex function and pipeline, for the inputs setup_streams
found; FALSE if either cannot be made */
static BOOL finish_draw(struct draw_context *context, unsigned long streamed, unsigned long packed,
	unsigned long integer)
{
	uint32_t function = vertex_function_get(context->program, streamed, packed, integer, context->lit);

	if (!function)
	{
		stats.skipped_pipeline++;
		return FALSE;
	}
	context->pipeline.vertex_function = function;
	context->draw.pipeline = pipeline_get(&context->pipeline);
	if (!context->draw.pipeline)
	{
		stats.skipped_pipeline++;
		return FALSE;
	}
	return TRUE;
}

/* ---------- tracing (debug.gpu_trace_frame) */

static BOOL trace_frame(void)
{
	static long frame = -2;

	if (frame == -2)
		frame = config_integer("debug.gpu_trace_frame");
	return frame >= 0 && device.frame == (unsigned long)frame;
}

static void trace_draw(const char *kind, D3DPRIMITIVETYPE type, unsigned long count)
{
	struct vertex_shader_object *program = current_program();
	DWORD *rs = D3D__RenderState;

	if (!trace_frame())
		return;
	platform_log("%s type %d count %lu vs %lu (decl %lu) vp %lu,%lu %lux%lu z%.2f-%.2f zen %lu zw %lu zf %lx blend %lu %lx/%lx cull %lx cw %08lx tm %05lx cc %lx",
		kind, type, count, program ? program->id : 0, device.vertex_shader ? device.vertex_shader->id : 0,
		device.viewport.X, device.viewport.Y, device.viewport.Width, device.viewport.Height,
		device.viewport.MinZ, device.viewport.MaxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
		rs[D3DRS_ALPHABLENDENABLE], rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_CULLMODE],
		rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_PSTEXTUREMODES], rs[D3DRS_PSCOMBINERCOUNT]);
}

/* ---------- the stream chunks

What a draw copies of guest memory (vertices that change, indices made for
it) goes into the frame's chunks: guest memory the GPU reads in place, taken
again once the GPU has finished the frame that last wrote into them. */

#define STREAM_CHUNK_SIZE (4UL * 1024 * 1024)

struct stream_chunk
{
	struct stream_chunk *next;
	unsigned char *memory;
	uint32_t buffer;
	unsigned long size;
	/* the latest frame that wrote into it */
	unsigned long frame;
};

static struct stream_chunk *stream_chunks;
static struct stream_chunk *stream_current;
static unsigned long stream_offset;

/* frame f is finished once the GPU's latest finished serial is f + 1 */
static BOOL frame_finished(unsigned long frame)
{
	return (unsigned long)host_metal_frame_completed() >= frame + 1;
}

static struct stream_chunk *stream_chunk_take(unsigned long size)
{
	struct stream_chunk *chunk;
	unsigned long chunk_size = size > STREAM_CHUNK_SIZE ? (size + 0x3fff) & ~0x3fffUL : STREAM_CHUNK_SIZE;

	for (chunk = stream_chunks; chunk; chunk = chunk->next)
	{
		if (chunk != stream_current && chunk->size >= size && chunk->frame != device.frame && frame_finished(chunk->frame))
			return chunk;
	}
	chunk = calloc(1, sizeof(*chunk));
	if (!chunk)
		return NULL;
	chunk->memory = gpu_memory(chunk_size, &chunk->buffer);
	if (!chunk->memory)
	{
		free(chunk);
		platform_log("GPU: no memory for %lu bytes of vertices", chunk_size);
		return NULL;
	}
	chunk->size = chunk_size;
	chunk->frame = device.frame;
	chunk->next = stream_chunks;
	stream_chunks = chunk;
	return chunk;
}

/* room for size bytes (at a 16-byte boundary) in the frame's chunks: where
in which buffer; NULL if there is none */
static unsigned char *stream_reserve(unsigned long size, uint32_t *buffer, unsigned long *offset)
{
	unsigned long start = (stream_offset + 15) & ~15UL;

	if (!stream_current || start + size > stream_current->size)
	{
		stream_current = stream_chunk_take(size);
		start = 0;
		if (!stream_current)
			return NULL;
	}
	stream_current->frame = device.frame;
	stream_offset = start + size;
	*buffer = stream_current->buffer;
	*offset = start;
	stats.streamed_bytes += size;
	return stream_current->memory + start;
}

static BOOL stream_upload(const void *data, unsigned long size, uint32_t *buffer, unsigned long *offset)
{
	unsigned char *room = stream_reserve(size, buffer, offset);

	if (!room)
		return FALSE;
	memcpy(room, data, size);
	return TRUE;
}

/* ---------- the mirror

As d3d8_gl.c's: vertex and index data in the Xbox's contiguous memory,
which mostly never changes once loaded, is kept in buffers, a segment of 4
MB each made when first needed, and copied there again only once the game
has written it (pages are write-protected once copied: memory_watch.c).
Pages the game rewrites frame after frame count as volatile for a while,
and their draws stream. A page is copied again only once no frame the GPU
has yet to finish read it; until then its draws stream too. */

#define MIRROR_SEGMENT_SIZE 0x400000UL
#define MIRROR_SEGMENT_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_SEGMENT_SIZE)
#define MIRROR_PAGE_SIZE 0x1000UL
#define MIRROR_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / MIRROR_PAGE_SIZE)
#define MIRROR_REWRITE_FRAMES 2
#define MIRROR_VOLATILE_REWRITES 4
#define MIRROR_VOLATILE_FRAMES 600

enum
{
	_mirror_page_absent,
	_mirror_page_present,
	_mirror_page_volatile
};

static struct
{
	unsigned char *memory[MIRROR_SEGMENT_COUNT];
	uint32_t buffers[MIRROR_SEGMENT_COUNT];
	unsigned char state[MIRROR_PAGE_COUNT];
	unsigned char rewrites[MIRROR_PAGE_COUNT];
	unsigned long generation[MIRROR_PAGE_COUNT];
	unsigned long rewritten_frame[MIRROR_PAGE_COUNT];
	/* the latest frame a draw read the page from the mirror, +1 (0: never) */
	unsigned long used_frame[MIRROR_PAGE_COUNT];
} mirror;

static BOOL page_in_flight(unsigned long page)
{
	return mirror.used_frame[page] && !frame_finished(mirror.used_frame[page] - 1);
}

/* copies the pages of [first, last) that are absent or stale; FALSE if one
of them is volatile, or still read by a frame in flight */
static BOOL mirror_refresh(unsigned long first, unsigned long last)
{
	unsigned long page, run;
	BOOL refused = FALSE;

	for (page = first; page < last; page++)
	{
		BOOL written = mirror.state[page] == _mirror_page_present &&
			memory_watch_generation((PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE), MIRROR_PAGE_SIZE) >
			mirror.generation[page];

		if (!written)
			continue;
		if (device.frame - mirror.rewritten_frame[page] <= MIRROR_REWRITE_FRAMES)
			mirror.rewrites[page]++;
		else
			mirror.rewrites[page] = 1;
		mirror.rewritten_frame[page] = device.frame;
		if (mirror.rewrites[page] >= MIRROR_VOLATILE_REWRITES)
		{
			mirror.state[page] = _mirror_page_volatile;
			refused = TRUE;
		}
		else if (page_in_flight(page))
		{
			/* (stale, but the GPU may still read the copy there) */
			mirror.state[page] = _mirror_page_absent;
			refused = TRUE;
		}
		else
		{
			mirror.state[page] = _mirror_page_absent;
		}
	}
	if (refused)
		return FALSE;
	for (page = first; page < last; page++)
	{
		if (mirror.state[page] != _mirror_page_present && page_in_flight(page))
			return FALSE;
	}
	for (page = first; page < last; page = run)
	{
		unsigned long segment = page * MIRROR_PAGE_SIZE / MIRROR_SEGMENT_SIZE;
		unsigned long address, size;

		if (mirror.state[page] == _mirror_page_present)
		{
			run = page + 1;
			continue;
		}
		for (run = page; run < last && mirror.state[run] != _mirror_page_present &&
			run * MIRROR_PAGE_SIZE / MIRROR_SEGMENT_SIZE == segment; run++)
		{
			mirror.rewrites[run] = mirror.state[run] == _mirror_page_volatile ? 0 : mirror.rewrites[run];
		}
		if (!mirror.memory[segment])
		{
			mirror.memory[segment] = gpu_memory(MIRROR_SEGMENT_SIZE, &mirror.buffers[segment]);
			if (!mirror.memory[segment])
				return FALSE;
		}
		address = PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE;
		size = (run - page) * MIRROR_PAGE_SIZE;
		/* protect first, so a write racing with the copy is noticed */
		memory_watch_protect(address, size);
		for (; page < run; page++)
		{
			mirror.generation[page] = memory_watch_generation((PLATFORM_CONTIGUOUS_BASE + page * MIRROR_PAGE_SIZE),
				MIRROR_PAGE_SIZE);
			mirror.state[page] = _mirror_page_present;
		}
		memcpy(mirror.memory[segment] + (address - PLATFORM_CONTIGUOUS_BASE) % MIRROR_SEGMENT_SIZE, (const void *)address,
			size);
	}
	return TRUE;
}

/* makes [address, address + size) current in the mirror, giving its buffer,
its offset there and the newest generation of its pages; FALSE if the range
is outside the contiguous memory, spans two segments, is volatile or cannot
be copied now */
static BOOL mirror_range(unsigned long address, unsigned long size, uint32_t *buffer, unsigned long *offset,
	unsigned long *generation)
{
	unsigned long start = address - PLATFORM_CONTIGUOUS_BASE;
	unsigned long segment, first, last, page, oldest = ~0UL, newest = 0;
	BOOL present = TRUE;

	if (!size || address < PLATFORM_CONTIGUOUS_BASE || start + size > PLATFORM_CONTIGUOUS_SIZE)
		return FALSE;
	segment = start / MIRROR_SEGMENT_SIZE;
	if ((start + size - 1) / MIRROR_SEGMENT_SIZE != segment)
		return FALSE;
	first = start / MIRROR_PAGE_SIZE;
	last = (start + size - 1) / MIRROR_PAGE_SIZE + 1;
	for (page = first; page < last; page++)
	{
		if (mirror.state[page] == _mirror_page_volatile)
		{
			if (device.frame - mirror.rewritten_frame[page] < MIRROR_VOLATILE_FRAMES)
				return FALSE;
			mirror.state[page] = _mirror_page_absent;
		}
		if (mirror.state[page] != _mirror_page_present)
		{
			present = FALSE;
		}
		else
		{
			if (mirror.generation[page] < oldest)
				oldest = mirror.generation[page];
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	if (!present || memory_watch_generation(address, size) > oldest)
	{
		if (!mirror_refresh(first, last))
			return FALSE;
		for (newest = 0, page = first; page < last; page++)
		{
			if (mirror.generation[page] > newest)
				newest = mirror.generation[page];
		}
	}
	for (page = first; page < last; page++)
		mirror.used_frame[page] = device.frame + 1;
	*buffer = mirror.buffers[segment];
	*offset = start - segment * MIRROR_SEGMENT_SIZE;
	if (generation)
		*generation = newest;
	stats.mirrored_bytes += size;
	return TRUE;
}

/* the smallest and largest index of an index range the mirror holds (as
d3d8_gl.c) */
#define INDEX_RANGE_SLOTS 4096

static struct
{
	unsigned long address;
	unsigned long count;
	unsigned long generation;
	WORD minimum;
	WORD maximum;
} index_ranges[INDEX_RANGE_SLOTS];

static void index_extent(const WORD *indices, unsigned long count, unsigned long generation, BOOL cached,
	unsigned long *minimum, unsigned long *maximum)
{
	unsigned long slot = (((unsigned long)indices >> 1) ^ (count * 2654435761UL)) % INDEX_RANGE_SLOTS;
	unsigned long index, low = 0xffff, high = 0;

	if (cached && index_ranges[slot].address == (unsigned long)indices && index_ranges[slot].count == count &&
		index_ranges[slot].generation == generation)
	{
		*minimum = index_ranges[slot].minimum;
		*maximum = index_ranges[slot].maximum;
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (indices[index] < low)
			low = indices[index];
		if (indices[index] > high)
			high = indices[index];
	}
	if (cached)
	{
		index_ranges[slot].address = (unsigned long)indices;
		index_ranges[slot].count = count;
		index_ranges[slot].generation = generation;
		index_ranges[slot].minimum = (WORD)low;
		index_ranges[slot].maximum = (WORD)high;
	}
	*minimum = low;
	*maximum = high;
}

/* ---------- vertex data */

/* an element's Metal vertex format, and whether the shader takes it as a
whole number (the unnormalized shorts) */
static uint32_t attribute_format(const struct vertex_element *element, BOOL *integer)
{
	*integer = FALSE;
	switch (element->type)
	{
	case D3DVSDT_FLOAT1: return HALO_METAL_VERTEX_FLOAT1;
	case D3DVSDT_FLOAT2: return HALO_METAL_VERTEX_FLOAT2;
	case D3DVSDT_FLOAT3: case D3DVSDT_FLOAT2H: return HALO_METAL_VERTEX_FLOAT3;
	case D3DVSDT_FLOAT4: return HALO_METAL_VERTEX_FLOAT4;
	case D3DVSDT_D3DCOLOR: return HALO_METAL_VERTEX_UCHAR4_NORMALIZED_BGRA;
	case D3DVSDT_SHORT1: *integer = TRUE; return HALO_METAL_VERTEX_SHORT1;
	case D3DVSDT_SHORT2: *integer = TRUE; return HALO_METAL_VERTEX_SHORT2;
	case D3DVSDT_SHORT3: *integer = TRUE; return HALO_METAL_VERTEX_SHORT3;
	case D3DVSDT_SHORT4: *integer = TRUE; return HALO_METAL_VERTEX_SHORT4;
	case D3DVSDT_NORMSHORT1: return HALO_METAL_VERTEX_SHORT1_NORMALIZED;
	case D3DVSDT_NORMSHORT2: return HALO_METAL_VERTEX_SHORT2_NORMALIZED;
	case D3DVSDT_NORMSHORT3: return HALO_METAL_VERTEX_SHORT3_NORMALIZED;
	case D3DVSDT_NORMSHORT4: return HALO_METAL_VERTEX_SHORT4_NORMALIZED;
	case D3DVSDT_NORMPACKED3: return HALO_METAL_VERTEX_UINT1;
	case D3DVSDT_PBYTE1: return HALO_METAL_VERTEX_UCHAR1_NORMALIZED;
	case D3DVSDT_PBYTE2: return HALO_METAL_VERTEX_UCHAR2_NORMALIZED;
	case D3DVSDT_PBYTE3: return HALO_METAL_VERTEX_UCHAR3_NORMALIZED;
	case D3DVSDT_PBYTE4: return HALO_METAL_VERTEX_UCHAR4_NORMALIZED;
	default: return HALO_METAL_VERTEX_NONE;
	}
}

/* a stream Metal reads as it is: a stride and attribute offsets of whole
words (else it is copied with each attribute at a word: stream_repack) */
static BOOL stream_aligned(const struct vertex_shader_object *declaration, unsigned long stream, unsigned long stride)
{
	unsigned long index;

	if (stride & 3)
		return FALSE;
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->stream == stream && element->type != D3DVSDT_NONE && (element->offset & 3))
			return FALSE;
	}
	return TRUE;
}

/* the stream's vertices copied into the frame's chunks with each attribute
at a word (offsets[] gets each element's new offset); its new stride */
static unsigned long stream_repack(const struct vertex_shader_object *declaration, unsigned long stream,
	const unsigned char *source, unsigned long count, unsigned long stride, unsigned short *offsets, uint32_t *buffer,
	unsigned long *offset)
{
	unsigned long index, vertex, packed = 0;
	unsigned char *destination;

	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];

		if (element->stream != stream || element->type == D3DVSDT_NONE)
			continue;
		offsets[index] = (unsigned short)packed;
		packed += (element->bytes + 3) & ~3UL;
	}
	if (!packed)
		packed = 4;
	destination = stream_reserve(packed * count, buffer, offset);
	if (!destination)
		return 0;
	memset(destination, 0, packed * count);
	for (vertex = 0; vertex < count; vertex++)
	{
		for (index = 0; index < declaration->element_count; index++)
		{
			const struct vertex_element *element = &declaration->elements[index];

			if (element->stream != stream || element->type == D3DVSDT_NONE)
				continue;
			memcpy(destination + vertex * packed + offsets[index], source + vertex * stride + element->offset,
				element->bytes);
		}
	}
	return packed;
}

/* the streams of vertices [first, first + count) into the draw (each at the
first of them) and the vertex layout into the pipeline's description;
FALSE if they cannot be read */
static BOOL setup_streams(struct draw_context *context, unsigned long first, unsigned long count)
{
	struct vertex_shader_object *declaration = device.vertex_shader;
	unsigned long streamed = 0, integer = 0, index;
	unsigned short offsets[XGPU_VERTEX_ATTRIBUTE_COUNT];
	BOOL placed[16] = { FALSE };

	for (index = 0; index < declaration->element_count; index++)
		offsets[index] = declaration->elements[index].offset;
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		unsigned long stream = element->stream;
		unsigned long stride = device.streams[stream].stride;

		if (!device.streams[stream].data || element->type == D3DVSDT_NONE || placed[stream])
			continue;
		placed[stream] = TRUE;
		{
			const unsigned char *base = (const unsigned char *)PLATFORM_PHYSICAL_TO_VIRTUAL(device.streams[stream].data) +
				first * stride;
			unsigned long bytes = stride ? stride * count : 64;
			uint32_t buffer = 0;
			unsigned long offset = 0;

			if (!stream_aligned(declaration, stream, stride))
			{
				unsigned long packed = stream_repack(declaration, stream, base, stride ? count : 1, stride, offsets,
					&buffer, &offset);

				if (!packed)
					return FALSE;
				context->pipeline.strides[stream] = stride ? (uint32_t)packed : HALO_METAL_STRIDE_CONSTANT;
			}
			else
			{
				if (((unsigned long)base & 3) || !mirror_range((unsigned long)base, bytes, &buffer, &offset, NULL))
				{
					if (!stream_upload(base, bytes, &buffer, &offset))
						return FALSE;
				}
				context->pipeline.strides[stream] = stride ? (uint32_t)stride : HALO_METAL_STRIDE_CONSTANT;
			}
			context->draw.streams[stream] = buffer;
			context->draw.stream_offsets[stream] = (uint32_t)offset;
		}
	}
	for (index = 0; index < declaration->element_count; index++)
	{
		const struct vertex_element *element = &declaration->elements[index];
		struct halo_metal_vertex_attribute *attribute = &context->pipeline.attributes[element->reg];
		BOOL whole;

		if (!device.streams[element->stream].data || element->type == D3DVSDT_NONE)
			continue;
		attribute->format = attribute_format(element, &whole);
		if (!attribute->format)
			continue;
		attribute->offset = offsets[index];
		attribute->stream = element->stream;
		streamed |= 1UL << element->reg;
		if (whole)
			integer |= 1UL << element->reg;
	}
	return finish_draw(context, streamed, declaration->packed_mask, integer);
}

static uint32_t primitive_type(D3DPRIMITIVETYPE type)
{
	switch (type)
	{
	case D3DPT_POINTLIST: return HALO_METAL_POINTS;
	case D3DPT_LINELIST: return HALO_METAL_LINES;
	case D3DPT_LINESTRIP: return HALO_METAL_LINE_STRIP;
	case D3DPT_TRIANGLESTRIP:
	case D3DPT_QUADSTRIP: return HALO_METAL_TRIANGLE_STRIP;
	default: return HALO_METAL_TRIANGLES;
	}
}

/* the primitives Metal has not, as lists of the ones it has (the indices of
them, or of vertices 0 to count - 1); NULL when the type needs none */
static WORD *converted_indices(D3DPRIMITIVETYPE type, const WORD *indices, unsigned long count, unsigned long *out_count)
{
	WORD *result;
	unsigned long index;

#define SOURCE(i) (indices ? indices[i] : (WORD)(i))
	switch (type)
	{
	case D3DPT_QUADLIST:
	{
		unsigned long quads = count / 4, quad;

		result = malloc(quads * 6 * sizeof(WORD) + 2);
		if (!result)
			return NULL;
		for (quad = 0; quad < quads; quad++)
		{
			result[quad * 6 + 0] = SOURCE(quad * 4);
			result[quad * 6 + 1] = SOURCE(quad * 4 + 1);
			result[quad * 6 + 2] = SOURCE(quad * 4 + 2);
			result[quad * 6 + 3] = SOURCE(quad * 4);
			result[quad * 6 + 4] = SOURCE(quad * 4 + 2);
			result[quad * 6 + 5] = SOURCE(quad * 4 + 3);
		}
		*out_count = quads * 6;
		return result;
	}
	case D3DPT_TRIANGLEFAN:
	case D3DPT_POLYGON:
		if (count < 3)
		{
			*out_count = 0;
			return malloc(2);
		}
		result = malloc((count - 2) * 3 * sizeof(WORD) + 2);
		if (!result)
			return NULL;
		for (index = 0; index + 2 < count; index++)
		{
			result[index * 3 + 0] = SOURCE(0);
			result[index * 3 + 1] = SOURCE(index + 1);
			result[index * 3 + 2] = SOURCE(index + 2);
		}
		*out_count = (count - 2) * 3;
		return result;
	case D3DPT_LINELOOP:
		result = malloc((count + 1) * sizeof(WORD) + 2);
		if (!result)
			return NULL;
		for (index = 0; index < count; index++)
			result[index] = SOURCE(index);
		result[count] = SOURCE(0);
		*out_count = count + 1;
		return result;
	default:
		return NULL;
	}
#undef SOURCE
}

static BOOL needs_indices(D3DPRIMITIVETYPE type)
{
	return type == D3DPT_QUADLIST || type == D3DPT_TRIANGLEFAN || type == D3DPT_POLYGON || type == D3DPT_LINELOOP;
}

void WINAPI D3DDevice_SetStreamSource(UINT stream_number, D3DVertexBuffer *stream_data, UINT stride)
{
	if (stream_number >= 16)
		return;
	device.streams[stream_number].data = stream_data ? stream_data->Data : 0;
	device.streams[stream_number].stride = stride;
}

void WINAPI D3DDevice_SetIndices(D3DIndexBuffer *index_data, UINT base_vertex_index)
{
	device.base_vertex_index = base_vertex_index;
	D3D__IndexData = index_data ? (WORD *)index_data->Data : NULL;
}

/* the converted primitive's indices into the draw (counting from vertex 0
of what setup_streams placed, less minimum); FALSE if there is no room */
static BOOL draw_indices(struct halo_metal_draw *draw, const WORD *indices, unsigned long count, unsigned long minimum)
{
	uint32_t buffer;
	unsigned long offset, index;
	WORD *room = (WORD *)stream_reserve(count * sizeof(WORD), &buffer, &offset);

	if (!room)
		return FALSE;
	for (index = 0; index < count; index++)
		room[index] = (WORD)(indices[index] - minimum);
	draw->index_buffer = buffer;
	draw->index_offset = (uint32_t)offset;
	draw->index_count = (uint32_t)count;
	draw->base_vertex = 0;
	return TRUE;
}

void WINAPI D3DDevice_DrawVertices(D3DPRIMITIVETYPE primitive_type_value, UINT start_vertex, UINT vertex_count)
{
	struct draw_context context;

	if (!vertex_count || !prepare_draw(FALSE, &context))
		return;
	trace_draw("draw", primitive_type_value, vertex_count);
	if (!setup_streams(&context, start_vertex, vertex_count))
		return;
	context.draw.primitive = primitive_type(primitive_type_value);
	if (needs_indices(primitive_type_value))
	{
		unsigned long count;
		WORD *indices = converted_indices(primitive_type_value, NULL, vertex_count, &count);

		if (!indices)
			return;
		if (count && draw_indices(&context.draw, indices, count, 0))
			host_metal_draw(&context.draw);
		free(indices);
		return;
	}
	context.draw.vertex_start = 0;
	context.draw.vertex_count = vertex_count;
	host_metal_draw(&context.draw);
}

void WINAPI D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE primitive_type_value, UINT vertex_count, CONST WORD *index_data)
{
	struct draw_context context;
	unsigned long minimum, maximum, generation = 0, index_offset = 0;
	uint32_t index_buffer = 0;
	BOOL mirrored;

	if (!vertex_count || !index_data || !prepare_draw(FALSE, &context))
		return;
	mirrored = !needs_indices(primitive_type_value) &&
		mirror_range((unsigned long)index_data, vertex_count * sizeof(WORD), &index_buffer, &index_offset, &generation);
	/* (Metal reads indices from a whole word) */
	if (mirrored && (index_offset & 3))
		mirrored = FALSE;
	index_extent(index_data, vertex_count, generation, mirrored, &minimum, &maximum);
	trace_draw("indexed", primitive_type_value, vertex_count);
	/* the streams from the base vertex on: index i is vertex base + i */
	if (!setup_streams(&context, device.base_vertex_index + minimum, maximum - minimum + 1))
		return;
	context.draw.primitive = primitive_type(primitive_type_value);
	if (mirrored)
	{
		context.draw.index_buffer = index_buffer;
		context.draw.index_offset = (uint32_t)index_offset;
		context.draw.index_count = vertex_count;
		context.draw.base_vertex = -(int32_t)minimum;
		host_metal_draw(&context.draw);
		return;
	}
	if (needs_indices(primitive_type_value))
	{
		unsigned long count;
		WORD *indices = converted_indices(primitive_type_value, index_data, vertex_count, &count);

		if (!indices)
			return;
		if (count && draw_indices(&context.draw, indices, count, minimum))
			host_metal_draw(&context.draw);
		free(indices);
		return;
	}
	if (draw_indices(&context.draw, index_data, vertex_count, minimum))
		host_metal_draw(&context.draw);
}

/* ---------- immediate mode (as d3d8_gl.c) */

void WINAPI D3DDevice_Begin(D3DPRIMITIVETYPE primitive_type_value)
{
	device.immediate_active = TRUE;
	device.immediate_type = primitive_type_value;
	device.immediate_count = 0;
	device.immediate_written = 0;
}

static void immediate_emit(void)
{
	unsigned long floats = XGPU_VERTEX_ATTRIBUTE_COUNT * 4;

	if (device.immediate_count == device.immediate_capacity)
	{
		unsigned long capacity = device.immediate_capacity ? device.immediate_capacity * 2 : 256;
		float *vertices = realloc(device.immediate_vertices, capacity * floats * sizeof(float));

		if (!vertices)
			return;
		device.immediate_vertices = vertices;
		device.immediate_capacity = capacity;
	}
	memcpy(device.immediate_vertices + device.immediate_count * floats, device.attributes, floats * sizeof(float));
	device.immediate_count++;
}

void WINAPI D3DDevice_End(void)
{
	unsigned long count = device.immediate_count, index, vertex, slot;
	D3DPRIMITIVETYPE type = device.immediate_type;
	struct draw_context context;
	unsigned long registers[XGPU_VERTEX_ATTRIBUTE_COUNT];
	unsigned long register_count = 0, vertex_size, offset, streamed = 0;
	uint32_t buffer;
	float *packed;

	device.immediate_active = FALSE;
	if (!count || !prepare_draw(TRUE, &context))
		return;
	trace_draw("immediate", type, count);
	/* the registers written since Begin, one after another in each vertex;
	the others are the uniforms' constant values */
	for (index = 0; index < XGPU_VERTEX_ATTRIBUTE_COUNT; index++)
	{
		if (device.immediate_written & (1UL << index))
			registers[register_count++] = index;
	}
	if (!register_count)
		return;
	vertex_size = register_count * 4 * sizeof(float);
	packed = (float *)stream_reserve(count * vertex_size, &buffer, &offset);
	if (!packed)
		return;
	for (vertex = 0; vertex < count; vertex++)
	{
		for (slot = 0; slot < register_count; slot++)
		{
			memcpy(packed + (vertex * register_count + slot) * 4,
				device.immediate_vertices + (vertex * XGPU_VERTEX_ATTRIBUTE_COUNT + registers[slot]) * 4,
				4 * sizeof(float));
		}
	}
	for (slot = 0; slot < register_count; slot++)
	{
		struct halo_metal_vertex_attribute *attribute = &context.pipeline.attributes[registers[slot]];

		attribute->format = HALO_METAL_VERTEX_FLOAT4;
		attribute->offset = (uint32_t)(slot * 4 * sizeof(float));
		attribute->stream = 0;
		streamed |= 1UL << registers[slot];
	}
	context.pipeline.strides[0] = (uint32_t)vertex_size;
	context.draw.streams[0] = buffer;
	context.draw.stream_offsets[0] = (uint32_t)offset;
	if (!finish_draw(&context, streamed, 0, 0))
		return;
	context.draw.primitive = primitive_type(type);
	if (needs_indices(type))
	{
		unsigned long index_count;
		WORD *indices = converted_indices(type, NULL, count, &index_count);

		if (!indices)
			return;
		if (index_count && draw_indices(&context.draw, indices, index_count, 0))
			host_metal_draw(&context.draw);
		free(indices);
		return;
	}
	context.draw.vertex_count = (uint32_t)count;
	host_metal_draw(&context.draw);
}

static void set_attribute(INT reg, float a, float b, float c, float d)
{
	BOOL emit = FALSE;

	if (reg == D3DVSDE_VERTEX)
	{
		reg = 0;
		emit = TRUE;
	}
	if (reg < 0 || reg >= XGPU_VERTEX_ATTRIBUTE_COUNT)
		return;
	device.attributes[reg][0] = a;
	device.attributes[reg][1] = b;
	device.attributes[reg][2] = c;
	device.attributes[reg][3] = d;
	if (device.immediate_active)
		device.immediate_written |= 1UL << reg;
	if (device.immediate_active && (emit || reg == 0))
		immediate_emit();
}

void WINAPI D3DDevice_SetVertexData2f(INT reg, FLOAT a, FLOAT b)
{
	set_attribute(reg, a, b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4f(INT reg, FLOAT a, FLOAT b, FLOAT c, FLOAT d)
{
	set_attribute(reg, a, b, c, d);
}

void WINAPI D3DDevice_SetVertexData2s(INT reg, SHORT a, SHORT b)
{
	set_attribute(reg, (float)a, (float)b, 0.0f, 1.0f);
}

void WINAPI D3DDevice_SetVertexData4ub(INT reg, BYTE a, BYTE b, BYTE c, BYTE d)
{
	set_attribute(reg, a / 255.0f, b / 255.0f, c / 255.0f, d / 255.0f);
}

void WINAPI D3DDevice_SetVertexDataColor(INT reg, D3DCOLOR color)
{
	float value[4];

	color_to_vec4(color, value);
	set_attribute(reg, value[0], value[1], value[2], value[3]);
}

/* ---------- clearing */

void WINAPI D3DDevice_Clear(DWORD count, CONST D3DRECT *rectangles, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
	struct halo_metal_clear clear;
	BOOL has_depth = FALSE;
	DWORD index;

	if (!device.metal_ready || !bind_targets(&has_depth))
		return;
	if (trace_frame())
		platform_log("clear flags %lx color %08lx z %g count %lu target %08lx depth %08lx", (unsigned long)flags,
			(unsigned long)color, z, (unsigned long)count,
			device.render_target ? (unsigned long)device.render_target->Data : 0,
			device.depth_stencil ? (unsigned long)device.depth_stencil->Data : 0);
	stats.clears++;
	memset(&clear, 0, sizeof(clear));
	if ((flags & D3DCLEAR_TARGET) && pass.color)
	{
		/* the Xbox clears the channels named: the fog screen clears only
		alpha, leaving the picture under the fog */
		clear.color_mask = ((flags & D3DCLEAR_TARGET_R) ? 1u : 0u) | ((flags & D3DCLEAR_TARGET_G) ? 2u : 0u) |
			((flags & D3DCLEAR_TARGET_B) ? 4u : 0u) | ((flags & D3DCLEAR_TARGET_A) ? 8u : 0u);
		color_to_vec4(color, clear.color);
	}
	clear.depth = has_depth && (flags & D3DCLEAR_ZBUFFER);
	clear.depth_value = z;
	clear.stencil = has_depth && (flags & D3DCLEAR_STENCIL);
	clear.stencil_value = stencil & 0xff;
	if (!clear.color_mask && !clear.depth && !clear.stencil)
		return;
	if (!count || !rectangles)
	{
		/* the NV2A clips a clear to the viewport, which keeps a split-screen
		window's clear off the other */
		long x0 = target_pixel((float)device.viewport.X, 0);
		long y0 = target_pixel((float)device.viewport.Y, 1);

		clear.rectangle[0] = (int32_t)x0;
		clear.rectangle[1] = (int32_t)y0;
		clear.rectangle[2] = (int32_t)(target_pixel((float)(device.viewport.X + device.viewport.Width), 0) - x0);
		clear.rectangle[3] = (int32_t)(target_pixel((float)(device.viewport.Y + device.viewport.Height), 1) - y0);
		host_metal_clear(&clear);
		return;
	}
	for (index = 0; index < count; index++)
	{
		INT left = rectangles[index].x1 > device.viewport.X ? rectangles[index].x1 : device.viewport.X;
		INT top = rectangles[index].y1 > device.viewport.Y ? rectangles[index].y1 : device.viewport.Y;
		INT right = rectangles[index].x2 < device.viewport.X + device.viewport.Width ?
			rectangles[index].x2 : device.viewport.X + device.viewport.Width;
		INT bottom = rectangles[index].y2 < device.viewport.Y + device.viewport.Height ?
			rectangles[index].y2 : device.viewport.Y + device.viewport.Height;
		long x0, y0;

		if (left >= right || top >= bottom)
			continue;
		x0 = target_pixel((float)(left + UI_OFFSET), 0);
		y0 = target_pixel((float)top, 1);
		clear.rectangle[0] = (int32_t)x0;
		clear.rectangle[1] = (int32_t)y0;
		clear.rectangle[2] = (int32_t)(target_pixel((float)(right + UI_OFFSET), 0) - x0);
		clear.rectangle[3] = (int32_t)(target_pixel((float)bottom, 1) - y0);
		host_metal_clear(&clear);
	}
}

/* ---------- the anti-aliasing passes

display.anti_aliasing's FXAA over a window's 3D view, before the HUD and menus
are drawn over it (source/render/render.c), as d3d8_gl.c's; with "smaa" too,
as on Android (SMAA's passes are GLSL only: port/linux/src/xgpu_post.c). */

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	struct render_target_entry *target;
	int32_t corners[4];
	int mode = anti_aliasing();

	if (!device.metal_ready || (mode != _anti_aliasing_fxaa && mode != _anti_aliasing_smaa))
		return;
	target = render_target_get(&device.back_buffer);
	if (!target)
		return;
	corners[0] = (int32_t)scaled_pixel(x0, target->target.scale[0]);
	corners[1] = (int32_t)scaled_pixel(y0, target->target.scale[1]);
	corners[2] = (int32_t)scaled_pixel(x1, target->target.scale[0]);
	corners[3] = (int32_t)scaled_pixel(y1, target->target.scale[1]);
	host_metal_fxaa(target->target.texture, corners);
}

/* ---------- presentation */

/* debug.screenshot_every's BMP, read back from the GPU */
static void write_screenshot(struct render_target_entry *target)
{
	const char *directory = *config_string("debug.screenshot_directory") ?
		config_string("debug.screenshot_directory") : NULL;
	unsigned long width = target->target.gl_width, height = target->target.gl_height;
	unsigned long image_size = width * height * 4, row;
	unsigned char header[54] = { 'B', 'M' };
	struct halo_metal_region region;
	unsigned char *pixels;
	char path[512];
	FILE *file;

	if (!directory)
		return;
	pixels = malloc(image_size);
	if (!pixels)
		return;
	level_region(&region, 0, width, height);
	region.bytes_per_row = (uint32_t)(width * 4);
	region.bytes_per_image = (uint32_t)image_size;
	if (!host_metal_texture_read(target->target.texture, &region, pixels))
	{
		free(pixels);
		return;
	}
	/* the display ignores destination alpha, which the game uses as scratch */
	for (row = 0; row < width * height; row++)
		pixels[row * 4 + 3] = 0xff;
	snprintf(path, sizeof(path), "%s/frame%05lu.bmp", directory, device.frame);
	file = fopen(path, "wb");
	if (file)
	{
		*(unsigned int *)(header + 2) = (unsigned int)(54 + image_size);
		*(unsigned int *)(header + 10) = 54;
		*(unsigned int *)(header + 14) = 40;
		*(int *)(header + 18) = (int)width;
		*(int *)(header + 22) = -(int)height;
		*(unsigned short *)(header + 26) = 1;
		*(unsigned short *)(header + 28) = 32;
		*(unsigned int *)(header + 34) = (unsigned int)image_size;
		fwrite(header, 1, sizeof(header), file);
		fwrite(pixels, 1, image_size, file);
		fclose(file);
	}
	free(pixels);
}

/* the screenshot key's PNG (controls.screenshot, as d3d8_gl.c's): in the
data folder's screenshots folder, named by the time */
static void write_key_screenshot(struct render_target_entry *target)
{
	unsigned long width = target->target.gl_width, height = target->target.gl_height, index;
	char directory[512], path[640], name[64], error[256] = "";
	struct halo_metal_region region;
	struct posix_file_information information;
	unsigned char *pixels;
	time_t now = time(NULL);
	struct tm local;
	unsigned int collision;

	snprintf(directory, sizeof(directory), "%s/screenshots", platform_data_root());
	posix_make_directory(directory);
	localtime_r(&now, &local);
	for (collision = 0; collision < 1000; collision++)
	{
		strftime(name, sizeof(name), "%Y-%m-%d_%H.%M.%S", &local);
		if (collision)
			snprintf(path, sizeof(path), "%s/%s_%u.png", directory, name, collision + 1);
		else
			snprintf(path, sizeof(path), "%s/%s.png", directory, name);
		if (posix_stat(path, &information) != 0)
			break;
	}
	pixels = malloc(width * height * 4);
	if (!pixels)
	{
		console_printf(FALSE, "Screenshot failed: no memory");
		return;
	}
	level_region(&region, 0, width, height);
	region.bytes_per_row = (uint32_t)(width * 4);
	region.bytes_per_image = (uint32_t)(width * height * 4);
	if (!host_metal_texture_read(target->target.texture, &region, pixels))
	{
		snprintf(error, sizeof(error), "the picture cannot be read back");
	}
	else
	{
		/* destination alpha is scratch data in the game, not transparency */
		for (index = 0; index < width * height; index++)
			pixels[index * 4 + 3] = 0xff;
		host_sdl_save_png(path, pixels, (int)width, (int)height, (int)(width * 4), error, sizeof(error));
	}
	free(pixels);
	if (error[0])
		console_printf(FALSE, "Screenshot failed: %.180s", error);
	else
		console_printf(FALSE, "Saved screenshot as %s", strrchr(path, '/') + 1);
}

void WINAPI D3DDevice_Present(CONST RECT *source_rectangle, CONST RECT *destination_rectangle,
	void *unused, void *unused2)
{
	static long screenshot_every = -1;

	(void)source_rectangle;
	(void)destination_rectangle;
	(void)unused;
	(void)unused2;
	if (screenshot_every < 0)
		screenshot_every = config_integer("debug.screenshot_every");

	if (device.metal_ready)
	{
		struct render_target_entry *back_buffer = render_target_get(&device.back_buffer);
		int window_width, window_height, width, height;
		int32_t rectangle[4];

		if (back_buffer)
		{
			if (trace_frame())
				platform_log("present back buffer %08lx texture %u", (unsigned long)device.back_buffer.Data,
					back_buffer->target.texture);
			if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0)
				write_screenshot(back_buffer);
			if (platform_screenshot_take_request())
				write_key_screenshot(back_buffer);
			platform_video_drawable_size(&window_width, &window_height);
			/* letterboxed to the back buffer's shape */
			width = window_width;
			height = (int)((long)window_width * back_buffer->target.gl_height / back_buffer->target.gl_width);
			if (height > window_height)
			{
				height = window_height;
				width = (int)((long)window_height * back_buffer->target.gl_width / back_buffer->target.gl_height);
			}
			rectangle[0] = (window_width - width) / 2;
			rectangle[1] = (window_height - height) / 2;
			rectangle[2] = width;
			rectangle[3] = height;
			device.frame_serial = host_metal_present(back_buffer->target.texture, rectangle);
		}
		pass.open = FALSE;
		platform_video_swap();
		memory_watch_begin_frame();
		xgpu_texture_cache_begin_frame();
		/* the next frame's results buffer: free once the GPU has finished
		the frame that used it last */
		{
			unsigned long next = device.frame + 1;

			if (next >= FRAMES_IN_FLIGHT)
				host_metal_frame_wait((uint32_t)(next - FRAMES_IN_FLIGHT + 1));
			visibility_collect();
			device.visibility_next[next % FRAMES_IN_FLIGHT] = 0;
			if (device.visibility_results[next % FRAMES_IN_FLIGHT])
				memset(device.visibility_results[next % FRAMES_IN_FLIGHT], 0, VISIBILITY_TEST_SLOTS * sizeof(uint64_t));
		}
	}
	device.frame++;
	stats.presents++;
	if (debug_settings.statistics && device.frame % 60 == 0)
	{
		platform_log("frame %lu: %lu draws, %lu immediate, %lu clears, %lu target changes, %lu passes; skipped %lu "
			"no program, %lu no target, %lu pipeline; %lu KB mirrored, %lu KB streamed",
			device.frame, stats.draws / stats.presents, stats.immediate_draws / stats.presents,
			stats.clears / stats.presents, stats.target_changes / stats.presents, stats.passes / stats.presents,
			stats.skipped_no_program, stats.skipped_no_target, stats.skipped_pipeline,
			stats.mirrored_bytes / stats.presents / 1024, stats.streamed_bytes / stats.presents / 1024);
		memset(&stats, 0, sizeof(stats));
	}
	platform_pump_events();

	pthread_mutex_lock(&vertical_blank_lock);
	if (halo_interpolation_enabled())
	{
		flip_count++;
	}
	else
	{
		while (pending_flips >= 2)
			pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
		pending_flips++;
	}
	pthread_mutex_unlock(&vertical_blank_lock);
}

HRESULT WINAPI D3DDevice_PersistDisplay(void)
{
	return S_OK;
}
