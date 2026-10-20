/*
HALO_METAL.H

The Metal renderer's two halves (port/macos/README.md, "Metal"): the
guest's Xbox Direct3D device (port/macos/guest/d3d8_metal.c, with the
texture cache and the high-res HUD and text of port/linux/src), which turns
the game's state into what Metal draws with, and the host's Metal
(port/macos/host/host_metal.m), which owns the device, the window's layer,
the command buffers and the objects, and draws.

Objects are small integer handles on the guest's side (0 is none). The
guest calls these from the game's main thread only, which is the one that
draws.

This header is included by both halves: its structures are made of 32-bit
members only, so that they have one layout in both ABIs, and memory is
passed as a parameter (the import stubs make it a host address:
tools/macos_imports.py), or inside a structure as a guest address.

Work is in frames: the host keeps one command buffer open, into which
render passes, draws, clears and copies go in the order they are asked for,
until host_metal_present commits it. Uploads (host_metal_texture_upload)
are done at once by the CPU: a texture is filled before it is first drawn
with, and made anew rather than refilled while the GPU may read it.
*/

#ifndef __HALO_METAL_H
#define __HALO_METAL_H

#include <stdint.h>

/* ---------- the device */

/* the window's Metal layer (an SDL window handle, host_sdl.c), the device
and its queue; display sync with vsync; nonzero on success */
int host_metal_initialize(unsigned int window, int vsync);
void host_metal_set_vsync(int vsync);

struct halo_metal_capabilities
{
	uint32_t maximum_texture_size;
	/* the most samples a pixel of a render target */
	uint32_t maximum_samples;
	char name[64];
};

void host_metal_capabilities(struct halo_metal_capabilities *capabilities);

/* ---------- textures */

enum
{
	HALO_METAL_TEXTURE_2D = 0,
	HALO_METAL_TEXTURE_CUBE,
	HALO_METAL_TEXTURE_3D,
};

enum
{
	HALO_METAL_FORMAT_NONE = 0,
	/* 32-bit ARGB words, as the texture cache converts texels to */
	HALO_METAL_FORMAT_BGRA8,
	HALO_METAL_FORMAT_RGBA8,
	/* one byte a texel (the high-res text's atlas) */
	HALO_METAL_FORMAT_R8,
	/* DXT1, DXT3, DXT5 */
	HALO_METAL_FORMAT_BC1,
	HALO_METAL_FORMAT_BC2,
	HALO_METAL_FORMAT_BC3,
	/* the depth buffers (Apple's GPUs have no 24-bit depth) */
	HALO_METAL_FORMAT_DEPTH32F_STENCIL8,
};

enum
{
	HALO_METAL_USAGE_SAMPLED = 1 << 0,
	HALO_METAL_USAGE_RENDER_TARGET = 1 << 1,
};

/* each channel's source (swizzle), a byte each from red to alpha */
enum
{
	HALO_METAL_SWIZZLE_ZERO = 0,
	HALO_METAL_SWIZZLE_ONE,
	HALO_METAL_SWIZZLE_RED,
	HALO_METAL_SWIZZLE_GREEN,
	HALO_METAL_SWIZZLE_BLUE,
	HALO_METAL_SWIZZLE_ALPHA,
};

#define HALO_METAL_SWIZZLE(r, g, b, a) \
	((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
/* (0: each channel its own) */

struct halo_metal_texture_description
{
	uint32_t type;
	uint32_t format;
	uint32_t width, height, depth;
	uint32_t levels;
	/* 1, or more for a multisampled render target */
	uint32_t samples;
	uint32_t usage;
	uint32_t swizzle;
};

/* a part of a texture: a level, a slice (a cube's face), a box, and the
layout of the bytes it is uploaded from or read into */
struct halo_metal_region
{
	uint32_t level, slice;
	uint32_t x, y, z;
	uint32_t width, height, depth;
	uint32_t bytes_per_row, bytes_per_image;
};

uint32_t host_metal_texture_create(const struct halo_metal_texture_description *description);
/* (the GPU may still be drawing with it: it goes once it is done) */
void host_metal_texture_delete(uint32_t texture);
void host_metal_texture_upload(uint32_t texture, const struct halo_metal_region *region, const void *data);
/* level 0's pixels to the levels after it, from the base level given */
void host_metal_texture_generate_mipmaps(uint32_t texture, uint32_t base_level);
/* a region into another texture's (of the same format and size), in the
frame's order */
void host_metal_texture_copy(uint32_t source, const struct halo_metal_region *source_region, uint32_t destination,
	const struct halo_metal_region *destination_region);
/* reads a region back into data, after the GPU has finished what was asked
before (it waits); nonzero on success */
int host_metal_texture_read(uint32_t texture, const struct halo_metal_region *region, void *data);

/* ---------- buffers */

/* guest memory, at a page (16 KB) and of whole pages, as a buffer the GPU
reads, or writes (visibility results), in place */
uint32_t host_metal_buffer_wrap(void *memory, uint32_t size);

/* ---------- shaders and states */

/* a function of Metal shading language source, compiled; 0 with the
compiler's messages in log */
uint32_t host_metal_function_compile(const char *source, const char *name, char *log, uint32_t log_size);

enum
{
	HALO_METAL_VERTEX_NONE = 0,
	HALO_METAL_VERTEX_FLOAT1,
	HALO_METAL_VERTEX_FLOAT2,
	HALO_METAL_VERTEX_FLOAT3,
	HALO_METAL_VERTEX_FLOAT4,
	HALO_METAL_VERTEX_UCHAR4_NORMALIZED_BGRA,
	HALO_METAL_VERTEX_UCHAR1_NORMALIZED,
	HALO_METAL_VERTEX_UCHAR2_NORMALIZED,
	HALO_METAL_VERTEX_UCHAR3_NORMALIZED,
	HALO_METAL_VERTEX_UCHAR4_NORMALIZED,
	HALO_METAL_VERTEX_SHORT1,
	HALO_METAL_VERTEX_SHORT2,
	HALO_METAL_VERTEX_SHORT3,
	HALO_METAL_VERTEX_SHORT4,
	HALO_METAL_VERTEX_SHORT1_NORMALIZED,
	HALO_METAL_VERTEX_SHORT2_NORMALIZED,
	HALO_METAL_VERTEX_SHORT3_NORMALIZED,
	HALO_METAL_VERTEX_SHORT4_NORMALIZED,
	HALO_METAL_VERTEX_UINT1,
};

#define HALO_METAL_ATTRIBUTE_COUNT 16
#define HALO_METAL_STREAM_COUNT 16
/* the vertex streams' buffer indices start here: buffer 0 holds the vertex
shader's constants, 1 its other uniforms */
#define HALO_METAL_STREAM_BUFFER_BASE 2

struct halo_metal_vertex_attribute
{
	uint32_t format;
	uint32_t offset;
	/* the stream (0 to HALO_METAL_STREAM_COUNT - 1) */
	uint32_t stream;
};

/* MTLBlendFactor's and MTLBlendOperation's values */
enum
{
	HALO_METAL_BLEND_ZERO = 0,
	HALO_METAL_BLEND_ONE = 1,
	HALO_METAL_BLEND_SOURCE_COLOR = 2,
	HALO_METAL_BLEND_ONE_MINUS_SOURCE_COLOR = 3,
	HALO_METAL_BLEND_SOURCE_ALPHA = 4,
	HALO_METAL_BLEND_ONE_MINUS_SOURCE_ALPHA = 5,
	HALO_METAL_BLEND_DESTINATION_COLOR = 6,
	HALO_METAL_BLEND_ONE_MINUS_DESTINATION_COLOR = 7,
	HALO_METAL_BLEND_DESTINATION_ALPHA = 8,
	HALO_METAL_BLEND_ONE_MINUS_DESTINATION_ALPHA = 9,
	HALO_METAL_BLEND_SOURCE_ALPHA_SATURATED = 10,
	HALO_METAL_BLEND_BLEND_COLOR = 11,
	HALO_METAL_BLEND_ONE_MINUS_BLEND_COLOR = 12,
	HALO_METAL_BLEND_BLEND_ALPHA = 13,
	HALO_METAL_BLEND_ONE_MINUS_BLEND_ALPHA = 14,
};

enum
{
	HALO_METAL_BLEND_ADD = 0,
	HALO_METAL_BLEND_SUBTRACT = 1,
	HALO_METAL_BLEND_REVERSE_SUBTRACT = 2,
	HALO_METAL_BLEND_MIN = 3,
	HALO_METAL_BLEND_MAX = 4,
};

/* a stream every vertex reads the same element of (Direct3D's stride 0) */
#define HALO_METAL_STRIDE_CONSTANT 0x80000000u

struct halo_metal_pipeline_description
{
	uint32_t vertex_function, fragment_function;
	struct halo_metal_vertex_attribute attributes[HALO_METAL_ATTRIBUTE_COUNT];
	/* each stream's stride, a multiple of 4 (0: the stream is not read;
	HALO_METAL_STRIDE_CONSTANT: every vertex reads its first element) */
	uint32_t strides[HALO_METAL_STREAM_COUNT];
	uint32_t color_format, depth_format;
	uint32_t samples;
	uint32_t blend;
	uint32_t source_rgb, destination_rgb, operation_rgb;
	uint32_t source_alpha, destination_alpha, operation_alpha;
	/* bits 0 to 3: red, green, blue, alpha */
	uint32_t write_mask;
};

/* 0 with the messages in log */
uint32_t host_metal_pipeline_create(const struct halo_metal_pipeline_description *description, char *log,
	uint32_t log_size);

/* MTLCompareFunction's and MTLStencilOperation's values */
enum
{
	HALO_METAL_COMPARE_NEVER = 0,
	HALO_METAL_COMPARE_LESS,
	HALO_METAL_COMPARE_EQUAL,
	HALO_METAL_COMPARE_LESS_EQUAL,
	HALO_METAL_COMPARE_GREATER,
	HALO_METAL_COMPARE_NOT_EQUAL,
	HALO_METAL_COMPARE_GREATER_EQUAL,
	HALO_METAL_COMPARE_ALWAYS,
};

enum
{
	HALO_METAL_STENCIL_KEEP = 0,
	HALO_METAL_STENCIL_ZERO,
	HALO_METAL_STENCIL_REPLACE,
	HALO_METAL_STENCIL_INCREMENT_CLAMP,
	HALO_METAL_STENCIL_DECREMENT_CLAMP,
	HALO_METAL_STENCIL_INVERT,
	HALO_METAL_STENCIL_INCREMENT_WRAP,
	HALO_METAL_STENCIL_DECREMENT_WRAP,
};

struct halo_metal_depth_stencil_description
{
	uint32_t depth_compare;
	uint32_t depth_write;
	uint32_t stencil;
	uint32_t stencil_compare;
	uint32_t stencil_fail, depth_fail, pass;
	uint32_t read_mask, write_mask;
};

uint32_t host_metal_depth_stencil_create(const struct halo_metal_depth_stencil_description *description);

enum
{
	HALO_METAL_ADDRESS_REPEAT = 0,
	HALO_METAL_ADDRESS_MIRROR,
	HALO_METAL_ADDRESS_CLAMP,
	HALO_METAL_ADDRESS_BORDER,
};

enum
{
	HALO_METAL_BORDER_TRANSPARENT_BLACK = 0,
	HALO_METAL_BORDER_OPAQUE_BLACK,
	HALO_METAL_BORDER_OPAQUE_WHITE,
};

struct halo_metal_sampler_description
{
	/* 0 nearest, 1 linear; the mipmaps': 0 none, 1 nearest, 2 linear */
	uint32_t minification, magnification, mipmaps;
	uint32_t address_u, address_v, address_w;
	uint32_t anisotropy;
	float lod_minimum, lod_maximum;
	uint32_t border;
};

uint32_t host_metal_sampler_create(const struct halo_metal_sampler_description *description);

/* ---------- drawing */

enum
{
	HALO_METAL_LOAD = 0,
	HALO_METAL_CLEAR,
	HALO_METAL_DONT_CARE,
};

/* the render targets draws go to until the next pass: what a target holds
before (loaded, cleared, or neither), and with multisampling, the textures
its samples are resolved into at the end */
struct halo_metal_pass_description
{
	uint32_t color, depth;
	uint32_t color_resolve, depth_resolve;
	uint32_t color_load, depth_load, stencil_load;
	float clear_color[4];
	float clear_depth;
	uint32_t clear_stencil;
	/* draws with visibility tests count samples into this buffer */
	uint32_t visibility_buffer;
};

void host_metal_pass_begin(const struct halo_metal_pass_description *description);

enum
{
	HALO_METAL_POINTS = 0,
	HALO_METAL_LINES,
	HALO_METAL_LINE_STRIP,
	HALO_METAL_TRIANGLES,
	HALO_METAL_TRIANGLE_STRIP,
};

enum
{
	HALO_METAL_CULL_NONE = 0,
	HALO_METAL_CULL_FRONT,
	HALO_METAL_CULL_BACK,
};

/* everything a draw needs; the host sets only what changed since the draw
before it in the pass */
struct halo_metal_draw
{
	uint32_t pipeline;
	uint32_t depth_stencil;
	uint32_t stencil_reference;
	/* x, y, width, height, near, far */
	float viewport[6];
	/* x, y, width, height */
	int32_t scissor[4];
	uint32_t cull;
	/* 0 clockwise, 1 counterclockwise */
	uint32_t front_face_counterclockwise;
	uint32_t wireframe;
	float depth_bias, depth_slope, depth_clamp;
	float blend_color[4];
	uint32_t streams[HALO_METAL_STREAM_COUNT];
	uint32_t stream_offsets[HALO_METAL_STREAM_COUNT];
	/* guest addresses of the shaders' uniforms (vertex buffers 0 and 1,
	fragment buffer 0), each set again only when its serial changes */
	uint32_t vertex_constants, vertex_constants_size, vertex_constants_serial;
	uint32_t vertex_uniforms, vertex_uniforms_size, vertex_uniforms_serial;
	uint32_t fragment_uniforms, fragment_uniforms_size, fragment_uniforms_serial;
	uint32_t textures[4];
	uint32_t samplers[4];
	/* 0, or 1 + the byte offset in the pass's visibility buffer the draw's
	samples are counted at */
	uint32_t visibility;
	uint32_t primitive;
	uint32_t vertex_start, vertex_count;
	/* indexed: the buffer and offset of 16-bit indices, and the vertex index
	0 stands for; index_count 0 draws vertex_count vertices */
	uint32_t index_buffer, index_offset, index_count;
	int32_t base_vertex;
};

void host_metal_draw(const struct halo_metal_draw *draw);

/* fills a rectangle of the pass's targets: the channels of mask (bits 0 to
3), depth, and stencil, as asked */
struct halo_metal_clear
{
	int32_t rectangle[4];
	uint32_t color_mask;
	uint32_t depth, stencil;
	float color[4];
	float depth_value;
	uint32_t stencil_value;
};

void host_metal_clear(const struct halo_metal_clear *clear);

/* ---------- anti-aliasing */

/* FXAA (port/linux/src/xgpu_post.c's) over the rectangle (from corner x0,
y0 to x1, y1, from row 0) of a target, in place, in the frame's order: only
its colour, not its alpha (which the game keeps values of its own in) */
void host_metal_fxaa(uint32_t texture, const int32_t *corners);

/* ---------- frames */

/* ends the frame: the texture drawn over the rectangle (x, y, width, height)
of the window's next drawable, filtered, black around it, then presented
and the frame committed; returns the frame's serial (from 1) */
uint32_t host_metal_present(uint32_t texture, const int32_t *rectangle);
/* the serial of the latest frame the GPU has finished */
uint32_t host_metal_frame_completed(void);
/* waits for the GPU to finish frame serial */
void host_metal_frame_wait(uint32_t serial);

#endif
