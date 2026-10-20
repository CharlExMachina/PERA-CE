/*
HOST_METAL.M

The host's half of the Metal renderer (port/macos/include/halo_metal.h; the
guest's Direct3D device is port/macos/guest/d3d8_metal.c): the window's
CAMetalLayer, the device and its queue, the objects the guest holds handles
to, and the frame's command buffer, into which passes, draws, clears and
copies are encoded as the guest asks for them.

- A pass's encoder is made when it is first drawn into, and a copy (a blit)
  ends it; the next draw begins the same pass again, loading what its
  targets hold, so that the guest's order of work is the GPU's.
- Each encoder keeps what it was last given (struct encoder_state): a draw
  sets only what differs from the draw before it.
- Clears draw a rectangle (the guest's clears are rectangles of a pass, of
  some channels only), and the presentation draws the back buffer into the
  drawable, letterboxed: shaders of this file's own (builtin_source).
- Frames are numbered from 1 as they are committed; a completion handler
  records the latest the GPU has finished.

Everything here runs on the game's main thread, but for the completion
handlers.
*/

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "host.h"
#include "halo_metal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <pthread.h>
#include <string.h>

/* host_sdl.c */
SDL_Window *host_sdl_window(uint32_t window);

/* ---------- handles

Objects the guest names are kept here, each a retained reference, its
handle its index. A handle freed is given out again: what the encoder last
had is forgotten as an object goes (encoder_forget). */

static void **objects;
static uint32_t object_capacity;
static uint32_t *free_handles;
static uint32_t free_count;

static uint32_t object_new(id object)
{
	uint32_t handle;

	if (!object)
		return 0;
	if (free_count)
	{
		handle = free_handles[--free_count];
	}
	else
	{
		if (object_capacity == 0 || objects[object_capacity - 1])
		{
			uint32_t capacity = object_capacity ? object_capacity * 2 : 1024;
			void **grown = realloc(objects, capacity * sizeof(*objects));
			uint32_t *grown_free = realloc(free_handles, capacity * sizeof(*free_handles));

			if (!grown || !grown_free)
				host_fatal("out of memory for Metal's objects");
			memset(grown + object_capacity, 0, (capacity - object_capacity) * sizeof(*objects));
			objects = grown;
			free_handles = grown_free;
			object_capacity = capacity;
		}
		/* (handle 0 is none) */
		for (handle = 1; handle < object_capacity && objects[handle]; handle++)
			;
	}
	objects[handle] = (void *)CFBridgingRetain(object);
	return handle;
}

static id object_get(uint32_t handle)
{
	return handle && handle < object_capacity && objects[handle] ? (__bridge id)objects[handle] : nil;
}

static void encoder_forget(uint32_t handle);

static void object_delete(uint32_t handle)
{
	if (!handle || handle >= object_capacity || !objects[handle])
		return;
	encoder_forget(handle);
	CFBridgingRelease(objects[handle]);
	objects[handle] = NULL;
	free_handles[free_count++] = handle;
}

/* ---------- the device */

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
static SDL_MetalView view;
static SDL_Window *window;
static id<MTLLibrary> builtin_library;
static id<MTLRenderPipelineState> present_pipeline;
static id<MTLSamplerState> present_sampler;
static id<MTLFunction> clear_vertex, clear_fragment;

static const char builtin_source[] =
	"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct present_out { float4 position [[position]]; float2 uv; };\n"
	/* a triangle over the whole viewport */
	"vertex present_out present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"	float2 p = float2((id << 1) & 2, id & 2);\n"
	"	present_out out;\n"
	"	out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"	out.uv = float2(p.x, 1.0 - p.y);\n"
	"	return out;\n"
	"}\n"
	/* the game keeps values of its own in destination alpha: opaque */
	"fragment float4 present_fragment(present_out in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler s [[sampler(0)]])\n"
	"{\n"
	"	return float4(picture.sample(s, in.uv).rgb, 1.0);\n"
	"}\n"
	"struct clear_out { float4 position [[position]]; };\n"
	"vertex clear_out clear_vertex(uint id [[vertex_id]], constant float &depth [[buffer(0)]])\n"
	"{\n"
	"	float2 p = float2((id << 1) & 2, id & 2);\n"
	"	clear_out out;\n"
	"	out.position = float4(p * 2.0 - 1.0, depth, 1.0);\n"
	"	return out;\n"
	"}\n"
	"fragment float4 clear_fragment(constant float4 &color [[buffer(0)]])\n"
	"{\n"
	"	return color;\n"
	"}\n"
	/* FXAA, as port/linux/src/xgpu_post.c's fxaa_source: metrics are 1 /
	width, 1 / height, width, height; bounds the rectangle's outermost texel
	centres */
	"struct fxaa_uniforms { float4 metrics; float4 bounds; };\n"
	"constant float FXAA_EDGE_THRESHOLD = 0.125;\n"
	"constant float FXAA_EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"constant float FXAA_SUBPIXEL_QUALITY = 0.75;\n"
	"constant float FXAA_STEP_LENGTHS[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
	"static float3 fxaa_color(texture2d<float> picture, sampler s, constant fxaa_uniforms &u, float2 position)\n"
	"{\n"
	"	return picture.sample(s, clamp(position, u.bounds.xy, u.bounds.zw), level(0.0)).rgb;\n"
	"}\n"
	"static float fxaa_luma(texture2d<float> picture, sampler s, constant fxaa_uniforms &u, float2 position)\n"
	"{\n"
	"	return dot(fxaa_color(picture, s, u, position), float3(0.299, 0.587, 0.114));\n"
	"}\n"
	"fragment float4 fxaa_fragment(clear_out in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler s [[sampler(0)]], constant fxaa_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"#define LUMA(p) fxaa_luma(picture, s, u, (p))\n"
	"	float2 m = u.metrics.xy;\n"
	"	float2 position = in.position.xy * m;\n"
	"	float luma = LUMA(position);\n"
	"	float up = LUMA(position + float2(0.0, -m.y));\n"
	"	float down = LUMA(position + float2(0.0, m.y));\n"
	"	float left = LUMA(position + float2(-m.x, 0.0));\n"
	"	float right = LUMA(position + float2(m.x, 0.0));\n"
	"	float highest = max(luma, max(max(up, down), max(left, right)));\n"
	"	float range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"	if (range < max(FXAA_EDGE_THRESHOLD_MINIMUM, highest * FXAA_EDGE_THRESHOLD))\n"
	"		discard_fragment();\n"
	"	float up_left = LUMA(position + float2(-m.x, -m.y));\n"
	"	float up_right = LUMA(position + float2(m.x, -m.y));\n"
	"	float down_left = LUMA(position + float2(-m.x, m.y));\n"
	"	float down_right = LUMA(position + float2(m.x, m.y));\n"
	"	float average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"	float subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"	subpixel = subpixel * subpixel * FXAA_SUBPIXEL_QUALITY;\n"
	"	bool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"		abs(up_right + down_right - 2.0 * right) >=\n"
	"		abs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"		abs(down_left + down_right - 2.0 * down);\n"
	"	float before = horizontal ? up : left;\n"
	"	float after = horizontal ? down : right;\n"
	"	bool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"	float gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"	float edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"	float2 across = (horizontal ? float2(0.0, m.y) : float2(m.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"	float2 along = horizontal ? float2(m.x, 0.0) : float2(0.0, m.y);\n"
	"	float2 end_before = position + 0.5 * across - along;\n"
	"	float2 end_after = position + 0.5 * across + along;\n"
	"	float luma_before = LUMA(end_before) - edge_luma;\n"
	"	float luma_after = LUMA(end_after) - edge_luma;\n"
	"	bool reached_before = abs(luma_before) >= gradient;\n"
	"	bool reached_after = abs(luma_after) >= gradient;\n"
	"	for (int index = 1; index < 12 && !(reached_before && reached_after); index++)\n"
	"	{\n"
	"		if (!reached_before)\n"
	"		{\n"
	"			end_before -= along * FXAA_STEP_LENGTHS[index];\n"
	"			luma_before = LUMA(end_before) - edge_luma;\n"
	"			reached_before = abs(luma_before) >= gradient;\n"
	"		}\n"
	"		if (!reached_after)\n"
	"		{\n"
	"			end_after += along * FXAA_STEP_LENGTHS[index];\n"
	"			luma_after = LUMA(end_after) - edge_luma;\n"
	"			reached_after = abs(luma_after) >= gradient;\n"
	"		}\n"
	"	}\n"
	"	float distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"	float distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"	float pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"	bool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"	return float4(fxaa_color(picture, s, u, position + max(varies ? pixel_offset : 0.0, subpixel) * across), 1.0);\n"
	"#undef LUMA\n"
	"}\n";

/* ---------- frames */

static id<MTLCommandBuffer> command_buffer;
static uint32_t frames_committed;
static volatile uint32_t frames_completed;
static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completion_condition = PTHREAD_COND_INITIALIZER;

static void command_buffer_begin(void)
{
	command_buffer = [queue commandBuffer];
}

/* ---------- the pass and its encoder */

static struct halo_metal_pass_description current_pass;
static BOOL pass_described;
/* the pass's encoder has been made once (a later one loads what the first
drew) */
static BOOL pass_begun;
static id<MTLRenderCommandEncoder> render_encoder;
static id<MTLBlitCommandEncoder> blit_encoder;

struct encoder_state
{
	uint32_t pipeline, depth_stencil, stencil_reference;
	float viewport[6];
	int32_t scissor[4];
	uint32_t cull, front_face_counterclockwise, wireframe;
	float depth_bias, depth_slope, depth_clamp;
	float blend_color[4];
	uint32_t streams[HALO_METAL_STREAM_COUNT];
	uint32_t stream_offsets[HALO_METAL_STREAM_COUNT];
	uint32_t vertex_constants, vertex_constants_serial;
	uint32_t vertex_uniforms, vertex_uniforms_serial;
	uint32_t fragment_uniforms, fragment_uniforms_serial;
	uint32_t textures[4], samplers[4];
	uint32_t visibility;
};

static struct encoder_state encoder;

static void encoder_reset(void)
{
	/* (all ones: nothing matches it) */
	memset(&encoder, 0xff, sizeof(encoder));
}

static void encoder_forget(uint32_t handle)
{
	int index;

	if (encoder.pipeline == handle)
		encoder.pipeline = ~0u;
	if (encoder.depth_stencil == handle)
		encoder.depth_stencil = ~0u;
	for (index = 0; index < HALO_METAL_STREAM_COUNT; index++)
	{
		if (encoder.streams[index] == handle)
			encoder.streams[index] = ~0u;
	}
	for (index = 0; index < 4; index++)
	{
		if (encoder.textures[index] == handle)
			encoder.textures[index] = ~0u;
		if (encoder.samplers[index] == handle)
			encoder.samplers[index] = ~0u;
	}
}

static void encoders_end(void)
{
	if (render_encoder)
	{
		[render_encoder endEncoding];
		render_encoder = nil;
	}
	if (blit_encoder)
	{
		[blit_encoder endEncoding];
		blit_encoder = nil;
	}
}

static MTLLoadAction load_action(uint32_t load, BOOL resumed)
{
	if (resumed || load == HALO_METAL_LOAD)
		return MTLLoadActionLoad;
	return load == HALO_METAL_CLEAR ? MTLLoadActionClear : MTLLoadActionDontCare;
}

/* the pass's encoder, made if there is none; nil if no pass is described */
static id<MTLRenderCommandEncoder> render_encoder_get(void)
{
	MTLRenderPassDescriptor *descriptor;
	id<MTLTexture> color, depth, color_resolve, depth_resolve;

	if (render_encoder)
		return render_encoder;
	if (!pass_described)
		return nil;
	if (blit_encoder)
	{
		[blit_encoder endEncoding];
		blit_encoder = nil;
	}
	color = object_get(current_pass.color);
	depth = object_get(current_pass.depth);
	color_resolve = object_get(current_pass.color_resolve);
	depth_resolve = object_get(current_pass.depth_resolve);
	if (!color && !depth)
		return nil;
	descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
	if (color)
	{
		descriptor.colorAttachments[0].texture = color;
		descriptor.colorAttachments[0].loadAction = load_action(current_pass.color_load, pass_begun);
		descriptor.colorAttachments[0].clearColor = MTLClearColorMake(current_pass.clear_color[0],
			current_pass.clear_color[1], current_pass.clear_color[2], current_pass.clear_color[3]);
		if (color_resolve)
		{
			descriptor.colorAttachments[0].resolveTexture = color_resolve;
			descriptor.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
		}
		else
		{
			descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
		}
	}
	if (depth)
	{
		descriptor.depthAttachment.texture = depth;
		descriptor.depthAttachment.loadAction = load_action(current_pass.depth_load, pass_begun);
		descriptor.depthAttachment.clearDepth = current_pass.clear_depth;
		descriptor.stencilAttachment.texture = depth;
		descriptor.stencilAttachment.loadAction = load_action(current_pass.stencil_load, pass_begun);
		descriptor.stencilAttachment.clearStencil = current_pass.clear_stencil;
		if (depth_resolve)
		{
			descriptor.depthAttachment.resolveTexture = depth_resolve;
			descriptor.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
			descriptor.stencilAttachment.resolveTexture = depth_resolve;
			descriptor.stencilAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
		}
		else
		{
			descriptor.depthAttachment.storeAction = MTLStoreActionStore;
			descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
		}
	}
	descriptor.visibilityResultBuffer = object_get(current_pass.visibility_buffer);
	render_encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
	pass_begun = YES;
	encoder_reset();
	return render_encoder;
}

/* a blit encoder, the pass's encoder ended (the next draw begins it again) */
static id<MTLBlitCommandEncoder> blit_encoder_get(void)
{
	if (blit_encoder)
		return blit_encoder;
	if (render_encoder)
	{
		[render_encoder endEncoding];
		render_encoder = nil;
	}
	blit_encoder = [command_buffer blitCommandEncoder];
	return blit_encoder;
}

void host_metal_pass_begin(const struct halo_metal_pass_description *description)
{
	encoders_end();
	current_pass = *description;
	pass_described = YES;
	pass_begun = NO;
}

/* ---------- initialisation */

int host_metal_initialize(unsigned int window_handle, int vsync)
{
	NSError *error = nil;
	MTLRenderPipelineDescriptor *descriptor;
	MTLSamplerDescriptor *sampler;

	window = host_sdl_window(window_handle);
	if (!window)
	{
		host_logf(HOST_LOG_ERROR, "Metal: no window");
		return 0;
	}
	device = MTLCreateSystemDefaultDevice();
	if (!device)
	{
		host_logf(HOST_LOG_ERROR, "Metal: no GPU");
		return 0;
	}
	view = SDL_Metal_CreateView(window);
	if (!view)
	{
		host_logf(HOST_LOG_ERROR, "Metal: cannot make the window's view: %s", SDL_GetError());
		return 0;
	}
	layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	layer.displaySyncEnabled = vsync != 0;
	layer.maximumDrawableCount = 3;
	queue = [device newCommandQueue];
	builtin_library = [device newLibraryWithSource:[NSString stringWithUTF8String:builtin_source] options:nil
		error:&error];
	if (!builtin_library)
	{
		host_logf(HOST_LOG_ERROR, "Metal: the built-in shaders do not compile: %s",
			error.localizedDescription.UTF8String);
		return 0;
	}
	descriptor = [[MTLRenderPipelineDescriptor alloc] init];
	descriptor.vertexFunction = [builtin_library newFunctionWithName:@"present_vertex"];
	descriptor.fragmentFunction = [builtin_library newFunctionWithName:@"present_fragment"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	present_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!present_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "Metal: the presentation's pipeline: %s", error.localizedDescription.UTF8String);
		return 0;
	}
	clear_vertex = [builtin_library newFunctionWithName:@"clear_vertex"];
	clear_fragment = [builtin_library newFunctionWithName:@"clear_fragment"];
	sampler = [[MTLSamplerDescriptor alloc] init];
	sampler.minFilter = MTLSamplerMinMagFilterLinear;
	sampler.magFilter = MTLSamplerMinMagFilterLinear;
	sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
	present_sampler = [device newSamplerStateWithDescriptor:sampler];
	encoder_reset();
	command_buffer_begin();
	host_logf(HOST_LOG_INFO, "Metal: %s", device.name.UTF8String);
	return 1;
}

void host_metal_set_vsync(int vsync)
{
	if (layer)
		layer.displaySyncEnabled = vsync != 0;
}

void host_metal_capabilities(struct halo_metal_capabilities *capabilities)
{
	uint32_t samples;

	memset(capabilities, 0, sizeof(*capabilities));
	if (!device)
		return;
	capabilities->maximum_texture_size = 16384;
	for (samples = 8; samples > 1 && ![device supportsTextureSampleCount:samples]; samples /= 2)
		;
	capabilities->maximum_samples = samples;
	snprintf(capabilities->name, sizeof(capabilities->name), "%s", device.name.UTF8String);
}

/* ---------- textures */

static MTLPixelFormat pixel_format(uint32_t format)
{
	switch (format)
	{
	case HALO_METAL_FORMAT_BGRA8: return MTLPixelFormatBGRA8Unorm;
	case HALO_METAL_FORMAT_RGBA8: return MTLPixelFormatRGBA8Unorm;
	case HALO_METAL_FORMAT_R8: return MTLPixelFormatR8Unorm;
	case HALO_METAL_FORMAT_BC1: return MTLPixelFormatBC1_RGBA;
	case HALO_METAL_FORMAT_BC2: return MTLPixelFormatBC2_RGBA;
	case HALO_METAL_FORMAT_BC3: return MTLPixelFormatBC3_RGBA;
	case HALO_METAL_FORMAT_DEPTH32F_STENCIL8: return MTLPixelFormatDepth32Float_Stencil8;
	default: return MTLPixelFormatInvalid;
	}
}

static BOOL compressed(uint32_t format)
{
	return format == HALO_METAL_FORMAT_BC1 || format == HALO_METAL_FORMAT_BC2 || format == HALO_METAL_FORMAT_BC3;
}

static MTLTextureSwizzle swizzle_channel(uint32_t source)
{
	switch (source)
	{
	case HALO_METAL_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
	case HALO_METAL_SWIZZLE_ONE: return MTLTextureSwizzleOne;
	case HALO_METAL_SWIZZLE_GREEN: return MTLTextureSwizzleGreen;
	case HALO_METAL_SWIZZLE_BLUE: return MTLTextureSwizzleBlue;
	case HALO_METAL_SWIZZLE_ALPHA: return MTLTextureSwizzleAlpha;
	default: return MTLTextureSwizzleRed;
	}
}

uint32_t host_metal_texture_create(const struct halo_metal_texture_description *description)
{
	MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
	BOOL target = (description->usage & HALO_METAL_USAGE_RENDER_TARGET) != 0;

	if (!device || pixel_format(description->format) == MTLPixelFormatInvalid || !description->width ||
		!description->height)
		return 0;
	/* (Apple's GPUs take compressed formats for 2D and cube textures) */
	if (compressed(description->format) && description->type == HALO_METAL_TEXTURE_3D)
		return 0;
	descriptor.pixelFormat = pixel_format(description->format);
	descriptor.width = description->width;
	descriptor.height = description->height;
	descriptor.mipmapLevelCount = description->levels ? description->levels : 1;
	descriptor.sampleCount = description->samples > 1 ? description->samples : 1;
	switch (description->type)
	{
	case HALO_METAL_TEXTURE_CUBE:
		descriptor.textureType = MTLTextureTypeCube;
		break;
	case HALO_METAL_TEXTURE_3D:
		descriptor.textureType = MTLTextureType3D;
		descriptor.depth = description->depth ? description->depth : 1;
		break;
	default:
		descriptor.textureType = description->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
		break;
	}
	descriptor.usage = ((description->usage & HALO_METAL_USAGE_SAMPLED) ? MTLTextureUsageShaderRead : 0) |
		(target ? MTLTextureUsageRenderTarget : 0);
	/* the GPU's own memory for what only it writes; the CPU fills the rest */
	descriptor.storageMode = target ? MTLStorageModePrivate : MTLStorageModeShared;
	if (description->swizzle)
	{
		descriptor.swizzle = MTLTextureSwizzleChannelsMake(swizzle_channel(description->swizzle & 0xff),
			swizzle_channel((description->swizzle >> 8) & 0xff), swizzle_channel((description->swizzle >> 16) & 0xff),
			swizzle_channel(description->swizzle >> 24));
	}
	return object_new([device newTextureWithDescriptor:descriptor]);
}

void host_metal_texture_delete(uint32_t texture)
{
	object_delete(texture);
}

void host_metal_texture_upload(uint32_t texture, const struct halo_metal_region *region, const void *data)
{
	id<MTLTexture> object = object_get(texture);

	if (!object || !data || region->level >= object.mipmapLevelCount)
		return;
	[object replaceRegion:MTLRegionMake3D(region->x, region->y, region->z, region->width, region->height,
		region->depth ? region->depth : 1) mipmapLevel:region->level slice:region->slice withBytes:data
		bytesPerRow:region->bytes_per_row bytesPerImage:object.textureType == MTLTextureType3D ? region->bytes_per_image : 0];
}

void host_metal_texture_generate_mipmaps(uint32_t texture, uint32_t base_level)
{
	id<MTLTexture> object = object_get(texture);

	if (!object || object.mipmapLevelCount < 2 || base_level + 1 >= object.mipmapLevelCount)
		return;
	if (base_level)
	{
		/* (the levels from the base on, as a texture of their own) */
		object = [object newTextureViewWithPixelFormat:object.pixelFormat textureType:object.textureType
			levels:NSMakeRange(base_level, object.mipmapLevelCount - base_level)
			slices:NSMakeRange(0, object.textureType == MTLTextureTypeCube ? 6 : 1)];
	}
	[blit_encoder_get() generateMipmapsForTexture:object];
}

void host_metal_texture_copy(uint32_t source, const struct halo_metal_region *source_region, uint32_t destination,
	const struct halo_metal_region *destination_region)
{
	id<MTLTexture> from = object_get(source), to = object_get(destination);

	if (!from || !to)
		return;
	[blit_encoder_get() copyFromTexture:from sourceSlice:source_region->slice sourceLevel:source_region->level
		sourceOrigin:MTLOriginMake(source_region->x, source_region->y, source_region->z)
		sourceSize:MTLSizeMake(source_region->width, source_region->height,
			source_region->depth ? source_region->depth : 1)
		toTexture:to destinationSlice:destination_region->slice destinationLevel:destination_region->level
		destinationOrigin:MTLOriginMake(destination_region->x, destination_region->y, destination_region->z)];
}

int host_metal_texture_read(uint32_t texture, const struct halo_metal_region *region, void *data)
{
	id<MTLTexture> object = object_get(texture);
	id<MTLBuffer> staging;
	unsigned long size = (unsigned long)region->bytes_per_row * region->height;

	if (!object || !size)
		return 0;
	staging = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	[blit_encoder_get() copyFromTexture:object sourceSlice:region->slice sourceLevel:region->level
		sourceOrigin:MTLOriginMake(region->x, region->y, 0) sourceSize:MTLSizeMake(region->width, region->height, 1)
		toBuffer:staging destinationOffset:0 destinationBytesPerRow:region->bytes_per_row
		destinationBytesPerImage:size];
	/* (the frame so far, waited for; the frame goes on in another command
	buffer) */
	encoders_end();
	[command_buffer commit];
	[command_buffer waitUntilCompleted];
	command_buffer_begin();
	memcpy(data, staging.contents, size);
	return 1;
}

/* ---------- buffers */

uint32_t host_metal_buffer_wrap(void *memory, uint32_t size)
{
	if (!device || ((uintptr_t)memory & (HOST_PAGE - 1)) || (size & (HOST_PAGE - 1)) || !size)
		return 0;
	return object_new([device newBufferWithBytesNoCopy:memory length:size options:MTLResourceStorageModeShared
		deallocator:nil]);
}

/* ---------- shaders and states */

uint32_t host_metal_function_compile(const char *source, const char *name, char *log, uint32_t log_size)
{
	MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
	NSError *error = nil;
	id<MTLLibrary> library;
	id<MTLFunction> function;

	if (log_size)
		log[0] = 0;
	if (!device)
		return 0;
	/* as the GL renderer's: no fast math, positions computed the same in
	every pipeline of a program */
	if (@available(macOS 15.0, *))
	{
		options.mathMode = MTLMathModeSafe;
		options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
	}
	else
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		options.fastMathEnabled = NO;
#pragma clang diagnostic pop
	}
	options.preserveInvariance = YES;
	library = [device newLibraryWithSource:[NSString stringWithUTF8String:source] options:options error:&error];
	if (!library)
	{
		if (log_size)
			snprintf(log, log_size, "%s", error.localizedDescription.UTF8String);
		return 0;
	}
	function = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
	if (!function && log_size)
		snprintf(log, log_size, "no function %s", name);
	return object_new(function);
}

static MTLVertexFormat vertex_format(uint32_t format)
{
	switch (format)
	{
	case HALO_METAL_VERTEX_FLOAT1: return MTLVertexFormatFloat;
	case HALO_METAL_VERTEX_FLOAT2: return MTLVertexFormatFloat2;
	case HALO_METAL_VERTEX_FLOAT3: return MTLVertexFormatFloat3;
	case HALO_METAL_VERTEX_FLOAT4: return MTLVertexFormatFloat4;
	case HALO_METAL_VERTEX_UCHAR4_NORMALIZED_BGRA: return MTLVertexFormatUChar4Normalized_BGRA;
	case HALO_METAL_VERTEX_UCHAR1_NORMALIZED: return MTLVertexFormatUCharNormalized;
	case HALO_METAL_VERTEX_UCHAR2_NORMALIZED: return MTLVertexFormatUChar2Normalized;
	case HALO_METAL_VERTEX_UCHAR3_NORMALIZED: return MTLVertexFormatUChar3Normalized;
	case HALO_METAL_VERTEX_UCHAR4_NORMALIZED: return MTLVertexFormatUChar4Normalized;
	case HALO_METAL_VERTEX_SHORT1: return MTLVertexFormatShort;
	case HALO_METAL_VERTEX_SHORT2: return MTLVertexFormatShort2;
	case HALO_METAL_VERTEX_SHORT3: return MTLVertexFormatShort3;
	case HALO_METAL_VERTEX_SHORT4: return MTLVertexFormatShort4;
	case HALO_METAL_VERTEX_SHORT1_NORMALIZED: return MTLVertexFormatShortNormalized;
	case HALO_METAL_VERTEX_SHORT2_NORMALIZED: return MTLVertexFormatShort2Normalized;
	case HALO_METAL_VERTEX_SHORT3_NORMALIZED: return MTLVertexFormatShort3Normalized;
	case HALO_METAL_VERTEX_SHORT4_NORMALIZED: return MTLVertexFormatShort4Normalized;
	case HALO_METAL_VERTEX_UINT1: return MTLVertexFormatUInt;
	default: return MTLVertexFormatInvalid;
	}
}

uint32_t host_metal_pipeline_create(const struct halo_metal_pipeline_description *description, char *log,
	uint32_t log_size)
{
	MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
	MTLVertexDescriptor *vertices = [MTLVertexDescriptor vertexDescriptor];
	MTLRenderPipelineColorAttachmentDescriptor *color = descriptor.colorAttachments[0];
	NSError *error = nil;
	id<MTLRenderPipelineState> pipeline;
	int index;

	if (log_size)
		log[0] = 0;
	descriptor.vertexFunction = object_get(description->vertex_function);
	descriptor.fragmentFunction = object_get(description->fragment_function);
	if (!descriptor.vertexFunction || !descriptor.fragmentFunction)
		return 0;
	for (index = 0; index < HALO_METAL_ATTRIBUTE_COUNT; index++)
	{
		const struct halo_metal_vertex_attribute *attribute = &description->attributes[index];

		if (!attribute->format)
			continue;
		vertices.attributes[index].format = vertex_format(attribute->format);
		vertices.attributes[index].offset = attribute->offset;
		vertices.attributes[index].bufferIndex = HALO_METAL_STREAM_BUFFER_BASE + attribute->stream;
	}
	for (index = 0; index < HALO_METAL_STREAM_COUNT; index++)
	{
		uint32_t stride = description->strides[index];
		MTLVertexBufferLayoutDescriptor *layout = vertices.layouts[HALO_METAL_STREAM_BUFFER_BASE + index];

		if (!stride)
			continue;
		if (stride == HALO_METAL_STRIDE_CONSTANT)
		{
			layout.stepFunction = MTLVertexStepFunctionConstant;
			layout.stepRate = 0;
			layout.stride = 16;
		}
		else
		{
			layout.stride = stride;
		}
	}
	descriptor.vertexDescriptor = vertices;
	descriptor.rasterSampleCount = description->samples ? description->samples : 1;
	color.pixelFormat = pixel_format(description->color_format);
	if (color.pixelFormat != MTLPixelFormatInvalid)
	{
		color.writeMask = ((description->write_mask & 1) ? MTLColorWriteMaskRed : 0) |
			((description->write_mask & 2) ? MTLColorWriteMaskGreen : 0) |
			((description->write_mask & 4) ? MTLColorWriteMaskBlue : 0) |
			((description->write_mask & 8) ? MTLColorWriteMaskAlpha : 0);
		color.blendingEnabled = description->blend != 0;
		color.sourceRGBBlendFactor = (MTLBlendFactor)description->source_rgb;
		color.destinationRGBBlendFactor = (MTLBlendFactor)description->destination_rgb;
		color.rgbBlendOperation = (MTLBlendOperation)description->operation_rgb;
		color.sourceAlphaBlendFactor = (MTLBlendFactor)description->source_alpha;
		color.destinationAlphaBlendFactor = (MTLBlendFactor)description->destination_alpha;
		color.alphaBlendOperation = (MTLBlendOperation)description->operation_alpha;
	}
	descriptor.depthAttachmentPixelFormat = pixel_format(description->depth_format);
	descriptor.stencilAttachmentPixelFormat = pixel_format(description->depth_format);
	pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline && log_size)
		snprintf(log, log_size, "%s", error.localizedDescription.UTF8String);
	return object_new(pipeline);
}

uint32_t host_metal_depth_stencil_create(const struct halo_metal_depth_stencil_description *description)
{
	MTLDepthStencilDescriptor *descriptor = [[MTLDepthStencilDescriptor alloc] init];

	descriptor.depthCompareFunction = (MTLCompareFunction)description->depth_compare;
	descriptor.depthWriteEnabled = description->depth_write != 0;
	if (description->stencil)
	{
		MTLStencilDescriptor *stencil = [[MTLStencilDescriptor alloc] init];

		stencil.stencilCompareFunction = (MTLCompareFunction)description->stencil_compare;
		stencil.stencilFailureOperation = (MTLStencilOperation)description->stencil_fail;
		stencil.depthFailureOperation = (MTLStencilOperation)description->depth_fail;
		stencil.depthStencilPassOperation = (MTLStencilOperation)description->pass;
		stencil.readMask = description->read_mask;
		stencil.writeMask = description->write_mask;
		descriptor.frontFaceStencil = stencil;
		descriptor.backFaceStencil = stencil;
	}
	return object_new([device newDepthStencilStateWithDescriptor:descriptor]);
}

static MTLSamplerAddressMode sampler_address(uint32_t mode)
{
	switch (mode)
	{
	case HALO_METAL_ADDRESS_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
	case HALO_METAL_ADDRESS_CLAMP: return MTLSamplerAddressModeClampToEdge;
	case HALO_METAL_ADDRESS_BORDER: return MTLSamplerAddressModeClampToBorderColor;
	default: return MTLSamplerAddressModeRepeat;
	}
}

uint32_t host_metal_sampler_create(const struct halo_metal_sampler_description *description)
{
	MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];

	descriptor.minFilter = description->minification ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.magFilter = description->magnification ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.mipFilter = description->mipmaps == 2 ? MTLSamplerMipFilterLinear :
		description->mipmaps == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
	descriptor.sAddressMode = sampler_address(description->address_u);
	descriptor.tAddressMode = sampler_address(description->address_v);
	descriptor.rAddressMode = sampler_address(description->address_w);
	descriptor.maxAnisotropy = description->anisotropy > 1 ? description->anisotropy : 1;
	descriptor.lodMinClamp = description->lod_minimum;
	descriptor.lodMaxClamp = description->lod_maximum;
	descriptor.borderColor = description->border == HALO_METAL_BORDER_OPAQUE_WHITE ? MTLSamplerBorderColorOpaqueWhite :
		description->border == HALO_METAL_BORDER_OPAQUE_BLACK ? MTLSamplerBorderColorOpaqueBlack :
		MTLSamplerBorderColorTransparentBlack;
	return object_new([device newSamplerStateWithDescriptor:descriptor]);
}

/* ---------- drawing */

static const MTLPrimitiveType primitives[] =
{
	MTLPrimitiveTypePoint, MTLPrimitiveTypeLine, MTLPrimitiveTypeLineStrip, MTLPrimitiveTypeTriangle,
	MTLPrimitiveTypeTriangleStrip,
};

void host_metal_draw(const struct halo_metal_draw *draw)
{
	id<MTLRenderCommandEncoder> render = render_encoder_get();
	int index;

	if (!render || draw->primitive >= sizeof(primitives) / sizeof(primitives[0]))
		return;
	if (encoder.pipeline != draw->pipeline)
	{
		id<MTLRenderPipelineState> pipeline = object_get(draw->pipeline);

		if (!pipeline)
			return;
		[render setRenderPipelineState:pipeline];
		encoder.pipeline = draw->pipeline;
	}
	if (encoder.depth_stencil != draw->depth_stencil)
	{
		[render setDepthStencilState:object_get(draw->depth_stencil)];
		encoder.depth_stencil = draw->depth_stencil;
	}
	if (encoder.stencil_reference != draw->stencil_reference)
	{
		[render setStencilReferenceValue:draw->stencil_reference];
		encoder.stencil_reference = draw->stencil_reference;
	}
	if (memcmp(encoder.viewport, draw->viewport, sizeof(encoder.viewport)))
	{
		MTLViewport viewport = { draw->viewport[0], draw->viewport[1], draw->viewport[2], draw->viewport[3],
			draw->viewport[4], draw->viewport[5] };

		[render setViewport:viewport];
		memcpy(encoder.viewport, draw->viewport, sizeof(encoder.viewport));
	}
	if (memcmp(encoder.scissor, draw->scissor, sizeof(encoder.scissor)))
	{
		MTLScissorRect scissor = { (NSUInteger)draw->scissor[0], (NSUInteger)draw->scissor[1],
			(NSUInteger)draw->scissor[2], (NSUInteger)draw->scissor[3] };

		/* (an empty scissor draws nothing) */
		if (!scissor.width || !scissor.height)
			return;
		[render setScissorRect:scissor];
		memcpy(encoder.scissor, draw->scissor, sizeof(encoder.scissor));
	}
	if (encoder.cull != draw->cull)
	{
		[render setCullMode:draw->cull == HALO_METAL_CULL_FRONT ? MTLCullModeFront :
			draw->cull == HALO_METAL_CULL_BACK ? MTLCullModeBack : MTLCullModeNone];
		encoder.cull = draw->cull;
	}
	if (encoder.front_face_counterclockwise != draw->front_face_counterclockwise)
	{
		[render setFrontFacingWinding:draw->front_face_counterclockwise ? MTLWindingCounterClockwise : MTLWindingClockwise];
		encoder.front_face_counterclockwise = draw->front_face_counterclockwise;
	}
	if (encoder.wireframe != draw->wireframe)
	{
		[render setTriangleFillMode:draw->wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
		encoder.wireframe = draw->wireframe;
	}
	if (encoder.depth_bias != draw->depth_bias || encoder.depth_slope != draw->depth_slope ||
		encoder.depth_clamp != draw->depth_clamp)
	{
		[render setDepthBias:draw->depth_bias slopeScale:draw->depth_slope clamp:draw->depth_clamp];
		encoder.depth_bias = draw->depth_bias;
		encoder.depth_slope = draw->depth_slope;
		encoder.depth_clamp = draw->depth_clamp;
	}
	if (memcmp(encoder.blend_color, draw->blend_color, sizeof(encoder.blend_color)))
	{
		[render setBlendColorRed:draw->blend_color[0] green:draw->blend_color[1] blue:draw->blend_color[2]
			alpha:draw->blend_color[3]];
		memcpy(encoder.blend_color, draw->blend_color, sizeof(encoder.blend_color));
	}
	for (index = 0; index < HALO_METAL_STREAM_COUNT; index++)
	{
		if (!draw->streams[index])
			continue;
		if (encoder.streams[index] != draw->streams[index])
		{
			[render setVertexBuffer:object_get(draw->streams[index]) offset:draw->stream_offsets[index]
				atIndex:HALO_METAL_STREAM_BUFFER_BASE + index];
			encoder.streams[index] = draw->streams[index];
			encoder.stream_offsets[index] = draw->stream_offsets[index];
		}
		else if (encoder.stream_offsets[index] != draw->stream_offsets[index])
		{
			[render setVertexBufferOffset:draw->stream_offsets[index] atIndex:HALO_METAL_STREAM_BUFFER_BASE + index];
			encoder.stream_offsets[index] = draw->stream_offsets[index];
		}
	}
	if (encoder.vertex_constants != draw->vertex_constants || encoder.vertex_constants_serial != draw->vertex_constants_serial)
	{
		[render setVertexBytes:GUEST_TO_HOST(const void *, draw->vertex_constants) length:draw->vertex_constants_size
			atIndex:0];
		encoder.vertex_constants = draw->vertex_constants;
		encoder.vertex_constants_serial = draw->vertex_constants_serial;
	}
	if (encoder.vertex_uniforms != draw->vertex_uniforms || encoder.vertex_uniforms_serial != draw->vertex_uniforms_serial)
	{
		[render setVertexBytes:GUEST_TO_HOST(const void *, draw->vertex_uniforms) length:draw->vertex_uniforms_size
			atIndex:1];
		encoder.vertex_uniforms = draw->vertex_uniforms;
		encoder.vertex_uniforms_serial = draw->vertex_uniforms_serial;
	}
	if (encoder.fragment_uniforms != draw->fragment_uniforms ||
		encoder.fragment_uniforms_serial != draw->fragment_uniforms_serial)
	{
		[render setFragmentBytes:GUEST_TO_HOST(const void *, draw->fragment_uniforms)
			length:draw->fragment_uniforms_size atIndex:0];
		encoder.fragment_uniforms = draw->fragment_uniforms;
		encoder.fragment_uniforms_serial = draw->fragment_uniforms_serial;
	}
	for (index = 0; index < 4; index++)
	{
		if (draw->textures[index] && encoder.textures[index] != draw->textures[index])
		{
			[render setFragmentTexture:object_get(draw->textures[index]) atIndex:index];
			encoder.textures[index] = draw->textures[index];
		}
		if (draw->samplers[index] && encoder.samplers[index] != draw->samplers[index])
		{
			[render setFragmentSamplerState:object_get(draw->samplers[index]) atIndex:index];
			encoder.samplers[index] = draw->samplers[index];
		}
	}
	if (encoder.visibility != draw->visibility && object_get(current_pass.visibility_buffer))
	{
		[render setVisibilityResultMode:draw->visibility ? MTLVisibilityResultModeCounting :
			MTLVisibilityResultModeDisabled offset:draw->visibility ? draw->visibility - 1 : 0];
		encoder.visibility = draw->visibility;
	}
	if (draw->index_count)
	{
		id<MTLBuffer> indices = object_get(draw->index_buffer);

		if (!indices)
			return;
		[render drawIndexedPrimitives:primitives[draw->primitive] indexCount:draw->index_count
			indexType:MTLIndexTypeUInt16 indexBuffer:indices indexBufferOffset:draw->index_offset instanceCount:1
			baseVertex:draw->base_vertex baseInstance:0];
	}
	else
	{
		[render drawPrimitives:primitives[draw->primitive] vertexStart:draw->vertex_start
			vertexCount:draw->vertex_count];
	}
}

/* ---------- clears */

/* the clears' pipelines and depth and stencil states, by what they write */
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *clear_pipelines;
static id<MTLDepthStencilState> clear_depth_states[4];

void host_metal_clear(const struct halo_metal_clear *clear)
{
	id<MTLRenderCommandEncoder> render = render_encoder_get();
	id<MTLTexture> color = object_get(current_pass.color), depth = object_get(current_pass.depth);
	NSUInteger samples = color ? color.sampleCount : depth ? depth.sampleCount : 1;
	unsigned long width = color ? color.width : depth ? depth.width : 0;
	unsigned long height = color ? color.height : depth ? depth.height : 0;
	uint32_t mask = color ? clear->color_mask : 0;
	int depth_state = (depth && clear->depth ? 1 : 0) | (depth && clear->stencil ? 2 : 0);
	NSNumber *key;
	id<MTLRenderPipelineState> pipeline;
	long x0 = clear->rectangle[0], y0 = clear->rectangle[1];
	long x1 = x0 + clear->rectangle[2], y1 = y0 + clear->rectangle[3];
	MTLViewport viewport;
	MTLScissorRect scissor;
	float depth_value = clear->depth_value;

	if (!render || (!mask && !depth_state))
		return;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > (long)width ? (long)width : x1;
	y1 = y1 > (long)height ? (long)height : y1;
	if (x1 <= x0 || y1 <= y0)
		return;
	if (!clear_pipelines)
		clear_pipelines = [[NSMutableDictionary alloc] init];
	key = @((color ? 1u : 0u) | (depth ? 2u : 0u) | (mask << 2) | ((uint32_t)samples << 8));
	pipeline = clear_pipelines[key];
	if (!pipeline)
	{
		MTLRenderPipelineDescriptor *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
		NSError *error = nil;

		descriptor.vertexFunction = clear_vertex;
		descriptor.fragmentFunction = clear_fragment;
		descriptor.rasterSampleCount = samples;
		if (color)
		{
			descriptor.colorAttachments[0].pixelFormat = color.pixelFormat;
			descriptor.colorAttachments[0].writeMask = ((mask & 1) ? MTLColorWriteMaskRed : 0) |
				((mask & 2) ? MTLColorWriteMaskGreen : 0) | ((mask & 4) ? MTLColorWriteMaskBlue : 0) |
				((mask & 8) ? MTLColorWriteMaskAlpha : 0);
		}
		if (depth)
		{
			descriptor.depthAttachmentPixelFormat = depth.pixelFormat;
			descriptor.stencilAttachmentPixelFormat = depth.pixelFormat;
		}
		pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!pipeline)
		{
			host_logf(HOST_LOG_ERROR, "Metal: a clear's pipeline: %s", error.localizedDescription.UTF8String);
			return;
		}
		clear_pipelines[key] = pipeline;
	}
	if (!clear_depth_states[depth_state])
	{
		MTLDepthStencilDescriptor *descriptor = [[MTLDepthStencilDescriptor alloc] init];

		descriptor.depthCompareFunction = MTLCompareFunctionAlways;
		descriptor.depthWriteEnabled = (depth_state & 1) != 0;
		if (depth_state & 2)
		{
			MTLStencilDescriptor *stencil = [[MTLStencilDescriptor alloc] init];

			stencil.stencilCompareFunction = MTLCompareFunctionAlways;
			stencil.depthStencilPassOperation = MTLStencilOperationReplace;
			stencil.writeMask = 0xff;
			descriptor.frontFaceStencil = stencil;
			descriptor.backFaceStencil = stencil;
		}
		clear_depth_states[depth_state] = [device newDepthStencilStateWithDescriptor:descriptor];
	}
	viewport.originX = 0;
	viewport.originY = 0;
	viewport.width = width;
	viewport.height = height;
	viewport.znear = 0.0;
	viewport.zfar = 1.0;
	scissor.x = (NSUInteger)x0;
	scissor.y = (NSUInteger)y0;
	scissor.width = (NSUInteger)(x1 - x0);
	scissor.height = (NSUInteger)(y1 - y0);
	[render setRenderPipelineState:pipeline];
	[render setDepthStencilState:clear_depth_states[depth_state]];
	[render setStencilReferenceValue:clear->stencil_value];
	[render setViewport:viewport];
	[render setScissorRect:scissor];
	[render setCullMode:MTLCullModeNone];
	[render setTriangleFillMode:MTLTriangleFillModeFill];
	[render setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
	[render setVertexBytes:&depth_value length:sizeof(depth_value) atIndex:0];
	[render setFragmentBytes:clear->color length:sizeof(clear->color) atIndex:0];
	if (object_get(current_pass.visibility_buffer))
		[render setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
	[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	/* (what the clear set, the next draw sets again) */
	encoder_reset();
}

/* ---------- anti-aliasing */

static id<MTLRenderPipelineState> fxaa_pipeline;
static id<MTLTexture> fxaa_copy;

void host_metal_fxaa(uint32_t texture, const int32_t *corners)
{
	id<MTLTexture> target = object_get(texture);
	MTLRenderPassDescriptor *descriptor;
	id<MTLRenderCommandEncoder> render;
	MTLViewport viewport;
	MTLScissorRect scissor;
	float uniforms[8];
	long x0 = corners[0], y0 = corners[1], x1 = corners[2], y1 = corners[3];

	if (!target || target.sampleCount > 1)
		return;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > (long)target.width ? (long)target.width : x1;
	y1 = y1 > (long)target.height ? (long)target.height : y1;
	if (x1 <= x0 || y1 <= y0)
		return;
	if (!fxaa_pipeline)
	{
		MTLRenderPipelineDescriptor *pipeline = [[MTLRenderPipelineDescriptor alloc] init];
		NSError *error = nil;

		pipeline.vertexFunction = clear_vertex;
		pipeline.fragmentFunction = [builtin_library newFunctionWithName:@"fxaa_fragment"];
		pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		pipeline.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
		fxaa_pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
		if (!fxaa_pipeline)
		{
			host_logf(HOST_LOG_ERROR, "Metal: FXAA's pipeline: %s", error.localizedDescription.UTF8String);
			return;
		}
	}
	if (!fxaa_copy || fxaa_copy.width != target.width || fxaa_copy.height != target.height)
	{
		MTLTextureDescriptor *copy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:target.width height:target.height mipmapped:NO];

		copy.usage = MTLTextureUsageShaderRead;
		copy.storageMode = MTLStorageModePrivate;
		fxaa_copy = [device newTextureWithDescriptor:copy];
	}
	/* the rectangle's pixels as they are, which the pass reads (it writes
	the target) */
	[blit_encoder_get() copyFromTexture:target sourceSlice:0 sourceLevel:0
		sourceOrigin:MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0)
		sourceSize:MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0), 1) toTexture:fxaa_copy
		destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake((NSUInteger)x0, (NSUInteger)y0, 0)];
	encoders_end();
	descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
	descriptor.colorAttachments[0].texture = target;
	descriptor.colorAttachments[0].loadAction = MTLLoadActionLoad;
	descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
	render = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
	viewport.originX = 0;
	viewport.originY = 0;
	viewport.width = target.width;
	viewport.height = target.height;
	viewport.znear = 0.0;
	viewport.zfar = 1.0;
	scissor.x = (NSUInteger)x0;
	scissor.y = (NSUInteger)y0;
	scissor.width = (NSUInteger)(x1 - x0);
	scissor.height = (NSUInteger)(y1 - y0);
	uniforms[0] = 1.0f / (float)target.width;
	uniforms[1] = 1.0f / (float)target.height;
	uniforms[2] = (float)target.width;
	uniforms[3] = (float)target.height;
	uniforms[4] = ((float)x0 + 0.5f) / (float)target.width;
	uniforms[5] = ((float)y0 + 0.5f) / (float)target.height;
	uniforms[6] = ((float)x1 - 0.5f) / (float)target.width;
	uniforms[7] = ((float)y1 - 0.5f) / (float)target.height;
	{
		float depth = 0.0f;

		[render setVertexBytes:&depth length:sizeof(depth) atIndex:0];
	}
	[render setViewport:viewport];
	[render setScissorRect:scissor];
	[render setRenderPipelineState:fxaa_pipeline];
	[render setFragmentTexture:fxaa_copy atIndex:0];
	[render setFragmentSamplerState:present_sampler atIndex:0];
	[render setFragmentBytes:uniforms length:sizeof(uniforms) atIndex:0];
	[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[render endEncoding];
	/* (the pass the draws go to begins again at the next draw, loading
	what its targets now hold) */
	pass_begun = YES;
}

/* ---------- frames */

uint32_t host_metal_present(uint32_t texture, const int32_t *rectangle)
{
	id<MTLTexture> picture = object_get(texture);
	id<CAMetalDrawable> drawable;
	int width = 0, height = 0;
	uint32_t serial;

	encoders_end();
	pass_described = NO;
	SDL_GetWindowSizeInPixels(window, &width, &height);
	if (width > 0 && height > 0 && (layer.drawableSize.width != width || layer.drawableSize.height != height))
		layer.drawableSize = CGSizeMake(width, height);
	@autoreleasepool
	{
		drawable = picture ? [layer nextDrawable] : nil;
		if (drawable)
		{
			MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
			id<MTLRenderCommandEncoder> render;
			MTLViewport viewport;

			descriptor.colorAttachments[0].texture = drawable.texture;
			descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
			descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
			descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
			render = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
			viewport.originX = rectangle[0];
			viewport.originY = rectangle[1];
			viewport.width = rectangle[2];
			viewport.height = rectangle[3];
			viewport.znear = 0.0;
			viewport.zfar = 1.0;
			[render setViewport:viewport];
			[render setRenderPipelineState:present_pipeline];
			[render setFragmentTexture:picture atIndex:0];
			[render setFragmentSamplerState:present_sampler atIndex:0];
			[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[render endEncoding];
			[command_buffer presentDrawable:drawable];
		}
		serial = ++frames_committed;
		[command_buffer addCompletedHandler:^(id<MTLCommandBuffer> buffer)
		{
			(void)buffer;
			pthread_mutex_lock(&completion_lock);
			if (serial > frames_completed)
				frames_completed = serial;
			pthread_cond_broadcast(&completion_condition);
			pthread_mutex_unlock(&completion_lock);
		}];
		[command_buffer commit];
		command_buffer_begin();
	}
	return serial;
}

uint32_t host_metal_frame_completed(void)
{
	return frames_completed;
}

void host_metal_frame_wait(uint32_t serial)
{
	if (serial > frames_committed)
		serial = frames_committed;
	pthread_mutex_lock(&completion_lock);
	while (frames_completed < serial)
		pthread_cond_wait(&completion_condition, &completion_lock);
	pthread_mutex_unlock(&completion_lock);
}
