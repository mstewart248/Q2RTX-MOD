/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "shared/shared.h"
#include "common/bsp.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/math.h"
#include "client/video.h"
#include "client/client.h"
#include "refresh/refresh.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "system/hunk.h"
#include "vkpt.h"
#include "material.h"
#include "fog.h"
#include "cameras.h"
#include "physical_sky.h"
#include "conversion.h"
#include "../../client/client.h"

#ifdef _WIN32
// VK_EXT_full_screen_exclusive is Win32-only, so its declarations live in
// vulkan_win32.h, which needs windows.h. Kept local to this file - vkpt.h must not drag
// windows.h through the whole renderer. NOMINMAX matters: shared.h has its own min/max.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#include <SDL_syswm.h>

static PFN_vkAcquireFullScreenExclusiveModeEXT qvkAcquireFullScreenExclusiveModeEXT = NULL;

// The monitor exclusive fullscreen should take over, via the SDL window's HWND.
static HMONITOR vkpt_get_window_monitor(void)
{
	SDL_SysWMinfo wm_info;
	SDL_VERSION(&wm_info.version);
	if (!qvk.window || !SDL_GetWindowWMInfo(qvk.window, &wm_info))
		return NULL;
	if (wm_info.subsystem != SDL_SYSWM_WINDOWS)
		return NULL;
	return MonitorFromWindow(wm_info.info.win.window, MONITOR_DEFAULTTONEAREST);
}

/* The GDI display name ("\\.\DISPLAY1" and friends) of the monitor the window is on.
   FGPresent_VBlankStart() needs it to open the adapter for the scanout clock.

   This replaced a DwmGetCompositionTimingInfo version. DWM's composition clock is only
   the display's clock while DWM is the one flipping: with exclusive fullscreen acquired
   the compositor is bypassed and those timestamps describe nothing the screen is doing,
   which is how snapping to them cost 4 fps in fullscreen. The adapter's own vblank event
   is valid either way. */
static bool vkpt_get_window_gdi_device(char *out, size_t out_size)
{
	HMONITOR mon = vkpt_get_window_monitor();
	if (!mon)
		return false;

	MONITORINFOEXA mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoA(mon, (MONITORINFO *)&mi))
		return false;

	Q_strlcpy(out, mi.szDevice, out_size);
	return out[0] != 0;
}
#endif
#include "../../client/ui/ui.h"
#include "server/server.h"

#include "shader/vertex_buffer.h"
#include "DLSS.h"
#include "fg_present.h"
#include "reflex.h"
#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

cvar_t *cvar_profiler = NULL;
cvar_t *cvar_profiler_samples = NULL;
cvar_t *cvar_profiler_scale = NULL;
cvar_t *cvar_hdr = NULL;
cvar_t *cvar_vsync = NULL;
cvar_t *cvar_swapchain_images = NULL;
cvar_t *cvar_vsync_mailbox = NULL;
cvar_t *cvar_present_stats = NULL;
cvar_t *cvar_fullscreen_exclusive = NULL;
cvar_t *cvar_pt_caustics = NULL;
cvar_t *cvar_pt_enable_nodraw = NULL;
cvar_t *cvar_pt_enable_surface_lights = NULL;
cvar_t *cvar_pt_enable_surface_lights_warp = NULL;
cvar_t* cvar_pt_surface_lights_fake_emissive_algo = NULL;
cvar_t* cvar_pt_surface_lights_threshold = NULL;
cvar_t* cvar_pt_bsp_radiance_scale = NULL;
cvar_t *cvar_pt_bsp_sky_lights = NULL;
cvar_t *cvar_pt_accumulation_rendering = NULL;
cvar_t *cvar_pt_accumulation_rendering_framenum = NULL;
cvar_t *cvar_pt_projection = NULL;
cvar_t *cvar_pt_dof = NULL;
cvar_t *cvar_pt_dlss_indirect_spec = NULL;
cvar_t* cvar_pt_freecam = NULL;
cvar_t *cvar_pt_nearest = NULL;
cvar_t *cvar_pt_bilerp_chars = NULL;
cvar_t *cvar_pt_bilerp_pics = NULL;
cvar_t *cvar_drs_enable = NULL;
cvar_t *cvar_drs_target = NULL;
cvar_t *cvar_drs_minscale = NULL;
cvar_t *cvar_drs_maxscale = NULL;
cvar_t *cvar_drs_adjust_up = NULL;
cvar_t *cvar_drs_adjust_down = NULL;
cvar_t *cvar_drs_gain = NULL;
cvar_t *cvar_drs_last_scale = NULL;
cvar_t *cvar_tm_blend_enable = NULL;
extern cvar_t *scr_viewsize;
extern cvar_t *cvar_bloom_enable;
extern cvar_t* cvar_flt_taa;
static int drs_current_scale = 0;
static int drs_effective_scale = 0;
static bool drs_last_frame_world = false;

cvar_t* cvar_min_driver_version_nvidia = NULL;
cvar_t* cvar_min_driver_version_amd = NULL;
cvar_t *cvar_ray_tracing_api = NULL;
cvar_t *cvar_vk_validation = NULL;

extern uiStatic_t uis;

#ifdef VKPT_DEVICE_GROUPS
cvar_t *cvar_sli = NULL;
#endif

#ifdef VKPT_IMAGE_DUMPS
cvar_t *cvar_dump_image = NULL;
#endif

byte cluster_debug_mask[VIS_MAX_BYTES];
int cluster_debug_index;

#define UBO_CVAR_DO(name, default_value) cvar_t *cvar_##name;
UBO_CVAR_LIST
#undef UBO_CVAR_DO

static bsp_t *bsp_world_model;

static bool temporal_frame_valid = false;

// Previous frame's path-tracer field width, for shaders that reproject into last
// frame's field-packed images.
static uint32_t pt_field_offset_prev = 0;

static int world_anim_frame = 0;

static vec3_t avg_envmap_color = { 0.f };

static image_t *water_normal_texture = NULL;

int num_accumulated_frames = 0;

static bool frame_ready = false;

static float sky_rotation = 0.f;
static int sky_autorotate = 0;
static vec3_t sky_axis = { 0.f };

#define NUM_TAA_SAMPLES 128
static vec2_t taa_samples[NUM_TAA_SAMPLES];

#define VK_NVX_BINARY_IMPORT "VK_NVX_binary_import"
#define VK_NVX_IMAGE_VIEW_HANDLE "VK_NVX_image_view_handle"

typedef enum {
	VKPT_INIT_DEFAULT            = (0),
	VKPT_INIT_SWAPCHAIN_RECREATE = (1 << 1),
	VKPT_INIT_RELOAD_SHADER      = (1 << 2),
} VkptInitFlags_t;

typedef struct VkptInit_s {
	const char *name;
	VkResult (*initialize)(void);
	VkResult (*destroy)(void);
	VkptInitFlags_t flags;
	int is_initialized;
} VkptInit_t;
VkptInit_t vkpt_initialization[] = {
	{ "profiler", vkpt_profiler_initialize,            vkpt_profiler_destroy,                VKPT_INIT_DEFAULT,            0 },
	{ "vbo",      vkpt_vertex_buffer_create,           vkpt_vertex_buffer_destroy,           VKPT_INIT_DEFAULT,            0 },
	{ "ubo",      vkpt_uniform_buffer_create,          vkpt_uniform_buffer_destroy,          VKPT_INIT_DEFAULT,            0 },
	{ "textures", vkpt_textures_initialize,            vkpt_textures_destroy,                VKPT_INIT_DEFAULT,            0 },
	{ "shadowmap", 	vkpt_shadow_map_initialize,        vkpt_shadow_map_destroy,              VKPT_INIT_DEFAULT,            0 },
	{ "shadowmap|", vkpt_shadow_map_create_pipelines,  vkpt_shadow_map_destroy_pipelines,    VKPT_INIT_RELOAD_SHADER ,     0 },
	{ "images",   vkpt_create_images,                  vkpt_destroy_images,                  VKPT_INIT_SWAPCHAIN_RECREATE, 0 },
	{ "draw",     vkpt_draw_initialize,                vkpt_draw_destroy,                    VKPT_INIT_DEFAULT,            0 },
	{ "pt",       vkpt_pt_init,                        vkpt_pt_destroy,                      VKPT_INIT_DEFAULT,            0 },
	{ "pt|",      vkpt_pt_create_pipelines,            vkpt_pt_destroy_pipelines,            VKPT_INIT_RELOAD_SHADER,      0 },
	{ "draw|",    vkpt_draw_create_pipelines,          vkpt_draw_destroy_pipelines,          VKPT_INIT_SWAPCHAIN_RECREATE
	                                                                                       | VKPT_INIT_RELOAD_SHADER,      0 },
	{ "vbo|",     vkpt_vertex_buffer_create_pipelines, vkpt_vertex_buffer_destroy_pipelines, VKPT_INIT_RELOAD_SHADER,      0 },
	{ "asvgf",    vkpt_asvgf_initialize,               vkpt_asvgf_destroy,                   VKPT_INIT_DEFAULT,            0 },
	{ "asvgf|",   vkpt_asvgf_create_pipelines,         vkpt_asvgf_destroy_pipelines,         VKPT_INIT_RELOAD_SHADER,      0 },
	{ "bloom",    vkpt_bloom_initialize,               vkpt_bloom_destroy,                   VKPT_INIT_DEFAULT,            0 },
	{ "bloom|",   vkpt_bloom_create_pipelines,         vkpt_bloom_destroy_pipelines,         VKPT_INIT_RELOAD_SHADER,      0 },
	{ "tonemap",  vkpt_tone_mapping_initialize,        vkpt_tone_mapping_destroy,            VKPT_INIT_DEFAULT,            0 },
	{ "tonemap|", vkpt_tone_mapping_create_pipelines,  vkpt_tone_mapping_destroy_pipelines,  VKPT_INIT_RELOAD_SHADER,      0 },
	{ "fsr",      vkpt_fsr_initialize,                 vkpt_fsr_destroy,                     VKPT_INIT_DEFAULT,            0 },
	{ "fsr|",     vkpt_fsr_create_pipelines,           vkpt_fsr_destroy_pipelines,           VKPT_INIT_RELOAD_SHADER,      0 },

	{ "physicalSky", vkpt_physical_sky_initialize,         vkpt_physical_sky_destroy,            VKPT_INIT_DEFAULT,        0 },
	{ "physicalSky|", vkpt_physical_sky_create_pipelines,  vkpt_physical_sky_destroy_pipelines,  VKPT_INIT_RELOAD_SHADER,  0 },
	{ "godrays",    vkpt_initialize_god_rays,           vkpt_destroy_god_rays,              VKPT_INIT_DEFAULT,             0 },
	{ "godrays|",   vkpt_god_rays_create_pipelines,     vkpt_god_rays_destroy_pipelines,    VKPT_INIT_RELOAD_SHADER,       0 },
	{ "godraysI",   vkpt_god_rays_update_images,        vkpt_god_rays_noop,                 VKPT_INIT_SWAPCHAIN_RECREATE,  0 },
};

VkImage GetDLSSImage();
VkExtent2D GetDLSSExtent();

// Values returned by pick_surface_format_*
typedef struct picked_surface_format_s {
	// Swapchain surface format
	VkSurfaceFormatKHR surface_fmt;
	// Swapchain image view format. This will always be an *_SRGB format, while the surface format may not.
	VkFormat swapchain_view_fmt;
} picked_surface_format_t;

void debug_output(const char* format, ...);
static void recreate_swapchain(void);

/* Which optional screen-image groups the CURRENT set of images was allocated for.
   vkpt_create_images() only allocates the groups the configuration uses, so turning
   A-SVGF, FSR or photo mode back on has to rebuild them - otherwise the pass that was
   just re-enabled writes into the 1x1 placeholders. ~0u forces the first comparison to
   miss, which is right: nothing is allocated yet. */
static uint32_t screen_image_profile_current = ~0u;
// Why the most recent swapchain (re)create happened, for the Swapchain: log line.
static const char *swapchain_reason = "initial create";

static void viewsize_changed(cvar_t *self)
{
	Cvar_ClampInteger(scr_viewsize, 25, 200);
	Com_Printf("Resolution scale: %d%%\n", scr_viewsize->integer);
}

static void pt_nearest_changed(cvar_t* self)
{
	vkpt_invalidate_texture_descriptors();
}

static void drs_target_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 30, 240);
}

static void drs_minscale_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 25, 100);
}

static void drs_maxscale_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 50, 200);
}

static void accumulation_cvar_changed(cvar_t* self)
{
	// Reset accumulation rendering on DoF parameter change
	num_accumulated_frames = 0;
}

static inline bool extents_equal(VkExtent2D a, VkExtent2D b)
{
	return a.width == b.width && a.height == b.height;
}

static VkExtent2D get_render_extent(void)
{
	int scale;
	if(drs_effective_scale)
	{
		scale = drs_effective_scale;
	}
	else
	{
		scale = scr_viewsize->integer;
		if(cvar_drs_enable->integer)
		{
			// Ensure render extent stays below get_screen_image_extent() result
			scale = min(cvar_drs_maxscale->integer, scale);
		}
	}

	VkExtent2D result;
	result.width = (uint32_t)(qvk.extent_unscaled.width * (float)scale / 100.f);
	result.height = (uint32_t)(qvk.extent_unscaled.height * (float)scale / 100.f);

	result.width = (result.width + 1) & ~1;

	return result;
}

// Width of one path-tracer field. The two fields are packed side by side into the
// screen images, field 1 starting at this x offset.
//   classic checkerboard:  width/2, each field holds one checkerboard half
//   full-resolution split: width,   each field is a full layer of the whole screen
uint32_t vkpt_pt_field_width(void)
{
	return DLSSSplitFieldsEnabled() ? qvk.extent_render.width : (qvk.extent_render.width / 2);
}

// Total packed width of the path-tracer screen images (both fields).
static uint32_t get_pt_packed_width(void)
{
	return vkpt_pt_field_width() * 2;
}

static VkExtent2D get_screen_image_extent(void)
{
	VkExtent2D result;
	if (cvar_drs_enable->integer)
	{
		int image_scale = max(cvar_drs_minscale->integer, cvar_drs_maxscale->integer);

		// In case FSR enable we'll always upscale to 100% and thus need at least the unscaled extent
		if(vkpt_fsr_is_enabled())
			image_scale = max(image_scale, 100);

		result.width = (uint32_t)(qvk.extent_unscaled.width * (float)image_scale / 100.f);
		result.height = (uint32_t)(qvk.extent_unscaled.height * (float)image_scale / 100.f);
	}
	else
	{
		result.width = max(qvk.extent_render.width, qvk.extent_unscaled.width);
		result.height = max(qvk.extent_render.height, qvk.extent_unscaled.height);
	}

	// With full-resolution fields the screen images must hold two width x height layers
	// side by side. At the usual DLSS scales this is at or below what the images were
	// already allocated at (Performance: 2 * 50% == 100% of the output width).
	if (DLSSSplitFieldsEnabled())
		result.width = max(result.width, qvk.extent_render.width * 2);

	result.width = (result.width + 1) & ~1;

	return result;
}

// Pending DLSS/DLSS-RR history reset. DLSS exposes InReset for scene transitions
// (DLSS-RR guide 3.8); it used to be hard-coded to 0, so a map change, teleport or
// resolution change carried stale history into the new scene. Set on any event that
// invalidates temporal history, consumed once by the next DLSS evaluation.
static bool dlss_reset_history = true;
// Same idea for frame generation: DLSS-G keeps its own history of the previous
// backbuffer, so it needs telling when that history is meaningless.
static bool dlssg_reset_history = true;

/* Smoothed interval between RENDERED frames, in microseconds, with the presentation
   stall already subtracted - i.e. what the GPU can actually do, not the cadence the
   pacer imposed. File scope because the frame-budget decision below needs last frame's
   value before the schedule recomputes it. */
static double fg_render_interval_us = 0.0;

/* The RAW frame interval, stall included - i.e. actual wall-clock time per rendered frame.
   Kept separate from fg_render_interval_us because the two answer different questions and
   conflating them is what left presents 9.9 ms late even after the lead was raised: the
   SLOT wants the stall removed (how fast could we go), the LEAD wants it left in (how long
   until this frame's GPU work is actually finished). The corrected figure is always the
   smaller of the two, so using it for the lead under-anchors the group by exactly the
   stall - and every present in it then falls due before the GPU is done. */
static double fg_frame_interval_us = 0.0;

/* What each swapchain image was last WRITTEN as, recorded at blit time and reported by the
   dump. The index log says the real frame is enqueued against a different image every group,
   and the dump says every image holds generated content; both cannot be true, so stop
   correlating them by hand and have the code state which write happened last. */
#define FG_ROLE_NONE 0
#define FG_ROLE_GEN  1
#define FG_ROLE_REAL 2
static uint8_t fg_image_role[16];
static unsigned int fg_dump_run = 0;

void vkpt_dlss_request_history_reset(void)
{
	dlss_reset_history = true;
	dlssg_reset_history = true;
}

void vkpt_reset_accumulation()
{
	num_accumulated_frames = 0;
	vkpt_dlss_request_history_reset();
}

VkResult
vkpt_initialize_all(VkptInitFlags_t init_flags)
{
	vkpt_device_wait_idle();

	qvk.extent_render = get_render_extent();
	qvk.extent_screen_images = get_screen_image_extent();	
	qvk.extent_taa_images.width = max(qvk.extent_screen_images.width, qvk.extent_unscaled.width);
	qvk.extent_taa_images.height = max(qvk.extent_screen_images.height, qvk.extent_unscaled.height);
	qvk.gpu_slice_width = (get_pt_packed_width() + qvk.device_count - 1) / qvk.device_count;

	if (DLSSEnabled()) {		
		qvk.extent_taa_images = qvk.extent_render;
	}
	else {
		qvk.extent_taa_images.width = max(qvk.extent_screen_images.width, qvk.extent_unscaled.width);
		qvk.extent_taa_images.height = max(qvk.extent_screen_images.height, qvk.extent_unscaled.height);
	}

	for(int i = 0; i < LENGTH(vkpt_initialization); i++) {
		VkptInit_t *init = vkpt_initialization + i;
		if((init->flags & init_flags) != init_flags)
			continue;
		
		// some entries will respond to multiple events --- do not initialize twice
		if (init->is_initialized)
			continue;

		init->is_initialized = init->initialize
			? (init->initialize() == VK_SUCCESS)
			: 1;
		assert(init->is_initialized);

		if (!init->is_initialized)
		  Com_Error(ERR_FATAL, "Couldn't initialize %s.\n", init->name);
	}

	if ((VKPT_INIT_DEFAULT & init_flags) == init_flags)
	{
		if (!initialize_transparency())
			return VK_RESULT_MAX_ENUM;
	}

	vkpt_textures_prefetch();

	water_normal_texture = IMG_Find("textures/water_n.tga", IT_SKIN, IF_PERMANENT);

	//char* vkExtensions = GetDLSSVulkanInstanceExtensions();
	//char* dlssExtensions = GetDLSSVulkanDeviceExtensions();

	return VK_SUCCESS;
}

VkResult
vkpt_destroy_all(VkptInitFlags_t destroy_flags)
{
	vkpt_device_wait_idle();

	for(int i = LENGTH(vkpt_initialization) - 1; i >= 0; i--) {
		VkptInit_t *init = vkpt_initialization + i;
		if((init->flags & destroy_flags) != destroy_flags)
			continue;
		
		// some entries will respond to multiple events --- do not destroy twice
		if (!init->is_initialized)
			continue;

		init->is_initialized = init->destroy
			? !(init->destroy() == VK_SUCCESS)
			: 0;
		assert(!init->is_initialized);
	}

	if ((VKPT_INIT_DEFAULT & destroy_flags) == destroy_flags)
	{
		destroy_transparency();
		vkpt_light_buffers_destroy();
	}

	return VK_SUCCESS;
}

void
vkpt_reload_shader(void)
{
	char buf[1024];
#ifdef _WIN32
	FILE *f = _popen("compile_shaders.bat", "r");
#else
	FILE *f = popen("make -j compile_shaders", "r");
#endif
	if(f) {
		while(fgets(buf, sizeof buf, f)) {
			Com_Printf("%s", buf);
		}
#ifdef _WIN32
		_pclose(f);
#else
		pclose(f);
#endif
	}

	vkpt_destroy_shader_modules();
	vkpt_load_shader_modules();

	vkpt_destroy_all(VKPT_INIT_RELOAD_SHADER);
	vkpt_initialize_all(VKPT_INIT_RELOAD_SHADER);
}

static void vkpt_reload_textures(void)
{
	IMG_ReloadAll();
}

//
//
//

vkpt_refdef_t vkpt_refdef = {
	.z_near = 1.0f,
	.z_far  = 4096.0f,
};

QVK_t qvk = {
	.win_width          = 1920,
	.win_height         = 1080,
	.frame_counter      = 0,
};

#define VK_EXTENSION_DO(a) PFN_##a q##a = 0;
LIST_EXTENSIONS_ACCEL_STRUCT
LIST_EXTENSIONS_RAY_PIPELINE
LIST_EXTENSIONS_DEBUG
LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

const char *vk_validation_layers[] = {
	"VK_LAYER_KHRONOS_validation"
};

const char *vk_requested_instance_extensions[] = {
	VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
	// Required by VK_EXT_full_screen_exclusive.
	VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
#ifdef VKPT_DEVICE_GROUPS
	VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
#endif
};

const char *vk_requested_device_extensions_common[] = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	// DLSS Frame Generation needs vkCmdPushDescriptorSetKHR. Without this the FG
	// snippet logs "Failed to load VK device function: vkCmdPushDescriptorSetKHR"
	// at NGX init, and create/evaluate fail later.
	VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
#ifdef VKPT_DEVICE_GROUPS
	VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
	VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
#endif
};

const char *vk_requested_device_extensions_ray_pipeline[] = {
	VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	VK_NVX_BINARY_IMPORT,
	VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
	VK_NVX_IMAGE_VIEW_HANDLE
};

const char* vk_requested_device_extensions_ray_query[] = {
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
	VK_NVX_BINARY_IMPORT,
	VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
	VK_NVX_IMAGE_VIEW_HANDLE
};

const char *vk_requested_device_extensions_debug[] = {
	VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
};

static const VkApplicationInfo vk_app_info = {
	.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	.pApplicationName   = "quake 2 pathtracing",
	.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	.pEngineName        = "vkpt",
	.engineVersion      = VK_MAKE_VERSION(1, 0, 0),
	.apiVersion         = VK_API_VERSION_1_2,
};

/* use this to override file names */
static const char *shader_module_file_names[NUM_QVK_SHADER_MODULES];

void
get_vk_extension_list(
		const char *layer,
		uint32_t *num_extensions,
		VkExtensionProperties **ext)
{
	_VK(vkEnumerateInstanceExtensionProperties(layer, num_extensions, NULL));
	*ext = malloc(sizeof(**ext) * *num_extensions);
	_VK(vkEnumerateInstanceExtensionProperties(layer, num_extensions, *ext));
}

void
get_vk_layer_list(
		uint32_t *num_layers,
		VkLayerProperties **ext)
{
	_VK(vkEnumerateInstanceLayerProperties(num_layers, NULL));
	*ext = malloc(sizeof(**ext) * *num_layers);
	_VK(vkEnumerateInstanceLayerProperties(num_layers, *ext));
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_callback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
		void *user_data)
{
	Com_EPrintf("validation layer %i %i: %s\n", (int32_t)type, (int32_t)severity,  callback_data->pMessage);
	debug_output("Vulkan error: %s\n", callback_data->pMessage);

	if (callback_data->cmdBufLabelCount)
	{
		Com_EPrintf("~~~ ");
		for (uint32_t i = 0; i < callback_data->cmdBufLabelCount; ++i)
		{
			const VkDebugUtilsLabelEXT* label = &callback_data->pCmdBufLabels[i];
			Com_EPrintf("%s ~ ", label->pLabelName);
		}
		Com_EPrintf("\n");
	}

	if (callback_data->objectCount)
	{
		for (uint32_t i = 0; i < callback_data->objectCount; ++i)
		{
			const VkDebugUtilsObjectNameInfoEXT* obj = &callback_data->pObjects[i];
			Com_EPrintf("--- %s %i\n", obj->pObjectName, (int32_t)obj->objectType);
		}
	}

	Com_EPrintf("\n");
	return VK_FALSE;
}

VkResult
qvkCreateDebugUtilsMessengerEXT(
		VkInstance instance,
		const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
		const VkAllocationCallbacks* pAllocator,
		VkDebugUtilsMessengerEXT* pCallback)
{
	PFN_vkCreateDebugUtilsMessengerEXT func = (PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if(func)
		return func(instance, pCreateInfo, pAllocator, pCallback);
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult
qvkDestroyDebugUtilsMessengerEXT(
		VkInstance instance,
		VkDebugUtilsMessengerEXT callback,
		const VkAllocationCallbacks* pAllocator)
{
	PFN_vkDestroyDebugUtilsMessengerEXT func = (PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if(func) {
		func(instance, callback, pAllocator);
		return VK_SUCCESS;
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static bool pick_surface_format_hdr(picked_surface_format_t* picked_fmt, const VkSurfaceFormatKHR avail_surface_formats[], size_t num_avail_surface_formats)
{
	VkSurfaceFormatKHR acceptable_formats[] = {
		{ VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT }
	};

	for(int i = 0; i < LENGTH(acceptable_formats); i++) {
		for(int j = 0; j < num_avail_surface_formats; j++) {
			if((acceptable_formats[i].format == avail_surface_formats[j].format)
				&& (acceptable_formats[i].colorSpace == avail_surface_formats[j].colorSpace)){
				picked_fmt->surface_fmt = avail_surface_formats[j];
				picked_fmt->swapchain_view_fmt = avail_surface_formats[j].format;
				return true;
			}
		}
	}
	return false;
}

static bool pick_surface_format_sdr(picked_surface_format_t* picked_fmt, const VkSurfaceFormatKHR avail_surface_formats[], size_t num_avail_surface_formats)
{
	struct {
		VkFormat format;
		VkFormat swapchain_view_fmt;
	} acceptable_formats[] = {
		{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB},
		{VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB},
		{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
		{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
	};

	for(int i = 0; i < LENGTH(acceptable_formats); i++) {
		for(int j = 0; j < num_avail_surface_formats; j++) {
			if(acceptable_formats[i].format == avail_surface_formats[j].format) {
				picked_fmt->surface_fmt = avail_surface_formats[j];
				picked_fmt->swapchain_view_fmt = acceptable_formats[i].swapchain_view_fmt;
				return true;
			}
		}
	}
	return false;
}


/* The image count the app is ASKING for, or 0 for "no explicit request, use
   minImageCount + 1". Recorded at create time so the per-frame recreate check can
   compare request against request. Comparing a request against the CLAMPED result
   in qvk.surf_num_images would recreate the swapchain every single frame whenever
   the request cannot be honoured exactly - which the FG floor above makes easy to
   hit, since vid_swapchain_images is pinned to 2 in both q2config.cfg files. */
/* ARE THE GENERATED FRAMES ACTUALLY DIFFERENT IMAGES?

   Every diagnostic up to here answered "which swapchain image reached the display", never
   "is there anything new in it". pt_dlss_fg_debug paints over the content, so it cannot
   tell a working 2x from one presenting the same picture twice - and both look identical
   on a counter. The two symptoms that pacing has never explained, NO TEARING on real
   content and motion pinned to the base rate, are exactly what duplicated frames look
   like: a tear between two identical images is invisible.

   So compare the pixels. A small centre region of VKPT_IMG_DLSS_OUTPUT (the real frame
   DLSS-G is given) and of each DLSS_FG_OUTPUT is copied back and the fraction of differing
   16-bit components reported, with the real-vs-PREVIOUS-real figure alongside as the scale
   of genuine frame-to-frame change.

     gen-vs-real ~= 0%          -> generation is returning a copy. Pacing cannot fix that.
     gen-vs-real  <  real-vs-prev -> plausible: an interpolated frame sits BETWEEN them,
                                     so it should differ from the real frame by less than
                                     a whole frame of motion, and by clearly more than 0.

   Debug path, so it takes the cheap route: idle the device, one throwaway command buffer,
   read back, print. Once a second while pt_dlss_fg_compare is on. */

#define FG_CMP_W 256
#define FG_CMP_H 144
#define FG_CMP_BYTES (FG_CMP_W * FG_CMP_H * 8)   /* rgba16f */

static bool fg_cmp_read_region(VkImage image, void *out)
{
	static BufferResource_t staging;
	static bool staging_ready = false;

	if (!staging_ready) {
		if (buffer_create(&staging, FG_CMP_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			!= VK_SUCCESS)
			return false;
		staging_ready = true;
	}

	VkExtent2D ext = GetDLSSExtent();
	if (ext.width < FG_CMP_W || ext.height < FG_CMP_H)
		return false;

	vkpt_device_wait_idle();

	VkCommandBuffer cmd = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0, .levelCount = 1,
		.baseArrayLayer = 0, .layerCount = 1,
	};

	IMAGE_BARRIER(cmd,
		.image = image, .subresourceRange = range,
		.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy copy = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
		},
		.imageOffset = { (int32_t)(ext.width - FG_CMP_W) / 2,
		                 (int32_t)(ext.height - FG_CMP_H) / 2, 0 },
		.imageExtent = { FG_CMP_W, FG_CMP_H, 1 },
	};
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging.buffer, 1, &copy);

	IMAGE_BARRIER(cmd,
		.image = image, .subresourceRange = range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL);

	vkpt_submit_command_buffer_simple(cmd, qvk.queue_graphics, false);
	vkpt_device_wait_idle();

	void *mapped = buffer_map(&staging);
	if (!mapped)
		return false;
	memcpy(out, mapped, FG_CMP_BYTES);
	buffer_unmap(&staging);
	return true;
}

/* rgba16f -> float. Needed because the first version of this compared RAW BITS, which
   saturated uselessly: every reading came back ~75.00%, i.e. all three colour components
   of every pixel differed and only alpha matched. In a path-traced image essentially no
   pixel is ever bit-identical to another frame, so "do the bits differ" cannot tell a
   real interpolation from a near-copy with denoiser noise on top. Magnitude can. */
static float fg_half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h >> 15) & 1u;
	uint32_t exp  = (uint32_t)(h >> 10) & 0x1Fu;
	uint32_t mant = (uint32_t)h & 0x3FFu;
	uint32_t bits;

	if (exp == 0) {
		if (mant == 0) {
			bits = sign << 31;
		} else {
			exp = 127 - 15 + 1;
			while (!(mant & 0x400u)) { mant <<= 1; exp--; }
			mant &= 0x3FFu;
			bits = (sign << 31) | (exp << 23) | (mant << 13);
		}
	} else if (exp == 31) {
		bits = (sign << 31) | 0x7F800000u | (mant << 13);
	} else {
		bits = (sign << 31) | ((exp + 127 - 15) << 23) | (mant << 13);
	}

	float out;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

/* Mean pixel VALUE over RGB. The difference alone cannot tell "these images hold
   different content" from "my reader is looking at the wrong memory": a steady gen-vs-real
   difference of 12.5 against a 0.07 frame step is far too large to be interpolation error,
   yet both images go through the same blit to an 8-bit swapchain, where anything near 12
   would clamp to solid white - and the screen is not white. One of those two facts has to
   give, and the means say which. */
static double fg_cmp_mean_value(const uint16_t *a)
{
	const size_t pixels = FG_CMP_W * FG_CMP_H;
	double sum = 0.0;
	size_t counted = 0;

	for (size_t p = 0; p < pixels; p++) {
		for (int ch = 0; ch < 3; ch++) {
			float f = fg_half_to_float(a[p * 4 + ch]);
			if (!isfinite(f))
				continue;
			sum += (double)f;
			counted++;
		}
	}

	return counted ? sum / (double)counted : 0.0;
}

/* Mean absolute difference over the RGB components. Alpha is skipped - it is a constant
   1.0 and including it just dilutes the number by a quarter. */
static double fg_cmp_mean_abs_diff(const uint16_t *a, const uint16_t *b)
{
	const size_t pixels = FG_CMP_W * FG_CMP_H;
	double sum = 0.0;
	size_t counted = 0;

	for (size_t p = 0; p < pixels; p++) {
		for (int ch = 0; ch < 3; ch++) {
			float fa = fg_half_to_float(a[p * 4 + ch]);
			float fb = fg_half_to_float(b[p * 4 + ch]);
			if (!isfinite(fa) || !isfinite(fb))
				continue;
			sum += fabs((double)fa - (double)fb);
			counted++;
		}
	}

	return counted ? sum / (double)counted : 0.0;
}

/* WRITE THE ACTUAL IMAGES OUT. `pt_dlss_fg_dump 1`, one shot.

   Statistics took this as far as they can: with a static camera the generated frame is 26%
   darker than the real one and differs from it by more than its own mean value, which says
   "different content" without saying what. Two PNGs answer in a glance what another metric
   would take another round to argue about.

   Written as linear -> sRGB, matching what the swapchain blit does: the vkCmdBlitImage
   target is a _SRGB format, so the encode happens on write and these float images are
   LINEAR display-referred. A mean of 0.03 linear is about 0.19 sRGB, which is an ordinary
   dark Quake scene rather than the near-black the raw number suggests. */
extern int stbi_write_png(char const *filename, int w, int h, int comp,
                          const void *data, int stride_in_bytes);

static void fg_dump_write_png(const char *name, const uint16_t *src, int w, int h)
{
	byte *rgb = Z_Malloc((size_t)w * h * 3);
	if (!rgb)
		return;

	for (int i = 0; i < w * h; i++) {
		for (int ch = 0; ch < 3; ch++) {
			float v = fg_half_to_float(src[(size_t)i * 4 + ch]);
			if (!isfinite(v) || v < 0.0f) v = 0.0f;
			if (v > 1.0f) v = 1.0f;
			/* sRGB encode, the same curve the _SRGB blit target applies. */
			float e = (v <= 0.0031308f) ? (v * 12.92f)
			                            : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
			rgb[(size_t)i * 3 + ch] = (byte)(e * 255.0f + 0.5f);
		}
	}

	if (stbi_write_png(name, w, h, 3, rgb, w * 3))
		Com_Printf("DLSS-G: wrote %s (%dx%d)\n", name, w, h);
	else
		Com_EPrintf("DLSS-G: failed to write %s\n", name);

	Z_Free(rgb);
}

static bool fg_dump_read_full(VkImage image, uint16_t *out, VkExtent2D ext,
                              BufferResource_t *staging)
{
	vkpt_device_wait_idle();

	VkCommandBuffer cmd = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0, .levelCount = 1,
		.baseArrayLayer = 0, .layerCount = 1,
	};

	IMAGE_BARRIER(cmd,
		.image = image, .subresourceRange = range,
		.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy copy = {
		.bufferOffset = 0,
		.bufferRowLength = 0,
		.bufferImageHeight = 0,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
		},
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { ext.width, ext.height, 1 },
	};
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging->buffer, 1, &copy);

	IMAGE_BARRIER(cmd,
		.image = image, .subresourceRange = range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL);

	vkpt_submit_command_buffer_simple(cmd, qvk.queue_graphics, false);
	vkpt_device_wait_idle();

	void *mapped = buffer_map(staging);
	if (!mapped)
		return false;
	memcpy(out, mapped, (size_t)ext.width * ext.height * 8);
	buffer_unmap(staging);
	return true;
}

/* GROUND TRUTH: the swapchain image itself, i.e. exactly what was put on the screen.

   Added because Matt reported that fgdump_real.png does NOT look like the game - the
   generated frame looked closer. That contradicts the assumption the other two dumps rest
   on, namely that VKPT_IMG_DLSS_OUTPUT converted linear->sRGB is what the display shows.
   The swapchain is a _SRGB format so vkCmdBlitImage applies exactly that encode, and the
   two should match. One of those steps is wrong and this says which, without guessing:
   whatever is in here IS what was displayed.

   Read at dump time it holds the PREVIOUS frame's presented content, which is fine - the
   question is what the game looks like, not which frame. Already 8-bit encoded, so it is
   copied straight out with no transfer applied. */
/* EVERY swapchain image, not just the current one.

   The single-image version was NOT ground truth, and reading it as such inverted the
   whole diagnosis: at 2x the generated frame is blitted into the image acquired in
   BeginFrame, so `qvk.current_swap_chain_image_index` at dump time names an image that
   last held a GENERATED frame. It matching the generated dump proved nothing.

   Dumping all of them does answer it, because both kinds are in there: if some look like
   the real frame and some like the generated one, both are reaching the screen. If they
   ALL look generated, real frames are never presented - which is what Matt has said from
   the start. */
static void fg_dump_swapchain_image(uint32_t index)
{
	VkExtent2D ext = qvk.extent_unscaled;
	VkDeviceSize bytes = (VkDeviceSize)ext.width * ext.height * 4;

	BufferResource_t staging;
	if (buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		!= VK_SUCCESS)
		return;

	VkImage img = qvk.swap_chain_images[index];

	vkpt_device_wait_idle();
	VkCommandBuffer cmd = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImageSubresourceRange range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0, .levelCount = 1,
		.baseArrayLayer = 0, .layerCount = 1,
	};

	IMAGE_BARRIER(cmd,
		.image = img, .subresourceRange = range,
		.srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy copy = {
		.bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
		.imageSubresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1,
		},
		.imageOffset = { 0, 0, 0 },
		.imageExtent = { ext.width, ext.height, 1 },
	};
	vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging.buffer, 1, &copy);

	IMAGE_BARRIER(cmd,
		.image = img, .subresourceRange = range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	vkpt_submit_command_buffer_simple(cmd, qvk.queue_graphics, false);
	vkpt_device_wait_idle();

	byte *src = buffer_map(&staging);
	if (src) {
		byte *rgb = Z_Malloc((size_t)ext.width * ext.height * 3);
		if (rgb) {
			bool bgr = (qvk.surf_format.format == VK_FORMAT_B8G8R8A8_SRGB
			         || qvk.surf_format.format == VK_FORMAT_B8G8R8A8_UNORM);
			for (size_t i = 0; i < (size_t)ext.width * ext.height; i++) {
				byte a = src[i * 4 + 0];
				byte b = src[i * 4 + 1];
				byte c = src[i * 4 + 2];
				rgb[i * 3 + 0] = bgr ? c : a;
				rgb[i * 3 + 1] = b;
				rgb[i * 3 + 2] = bgr ? a : c;
			}
			char name[64];
			Q_snprintf(name, sizeof(name), "fgdump%u_swap%u.png", fg_dump_run, index);
			if (stbi_write_png(name, ext.width, ext.height, 3,
					rgb, ext.width * 3))
				Com_Printf("DLSS-G: wrote %s (%dx%d) last written as %s%s", name,
					ext.width, ext.height,
					index >= LENGTH(fg_image_role) ? "?" :
					fg_image_role[index] == FG_ROLE_GEN ? "GENERATED" :
					fg_image_role[index] == FG_ROLE_REAL ? "REAL" : "never",
					index == qvk.current_swap_chain_image_index ? " [current]\n" : "\n");
			Z_Free(rgb);
		}
		buffer_unmap(&staging);
	}

	buffer_destroy(&staging);
}

static void fg_debug_dump_frames(unsigned int generated_count)
{
	static cvar_t *cv_dump = NULL;
	if (!cv_dump) cv_dump = Cvar_Get("pt_dlss_fg_dump", "0", 0);
	if (!cv_dump->integer)
		return;

	Cvar_Set("pt_dlss_fg_dump", "0");   /* one shot */

	/* Each run gets its own prefix. The first control run was destroyed by the run
	   after it, which is exactly the comparison it existed to make. */
	fg_dump_run++;

	for (uint32_t i = 0; i < qvk.num_swap_chain_images; i++)
		fg_dump_swapchain_image(i);

	VkExtent2D ext = GetDLSSExtent();
	VkDeviceSize bytes = (VkDeviceSize)ext.width * ext.height * 8;

	BufferResource_t staging;
	if (buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		!= VK_SUCCESS) {
		Com_EPrintf("DLSS-G: dump staging buffer (%llu bytes) failed\n",
			(unsigned long long)bytes);
		return;
	}

	uint16_t *pixels = Z_Malloc((size_t)bytes);
	if (pixels) {
		if (fg_dump_read_full(qvk.images[VKPT_IMG_DLSS_OUTPUT], pixels, ext, &staging))
			{
				char rname[64];
				Q_snprintf(rname, sizeof(rname), "fgdump%u_real.png", fg_dump_run);
				fg_dump_write_png(rname, pixels, ext.width, ext.height);
			}

		for (unsigned int i = 1; i <= generated_count; i++) {
			char name[64];
			Q_snprintf(name, sizeof(name), "fgdump%u_gen%u.png", fg_dump_run, i);
			if (fg_dump_read_full(GetDLSSGImage(i), pixels, ext, &staging))
				fg_dump_write_png(name, pixels, ext.width, ext.height);
		}
		Z_Free(pixels);
	}

	buffer_destroy(&staging);
}

static void fg_debug_compare_frames(unsigned int generated_count)
{
	static cvar_t *cv_cmp = NULL;
	if (!cv_cmp) cv_cmp = Cvar_Get("pt_dlss_fg_compare", "0", 0);
	if (!cv_cmp->integer || generated_count == 0)
		return;

	static uint16_t *real_now = NULL, *real_prev = NULL, *gen = NULL;
	if (!real_now) {
		real_now  = Z_Malloc(FG_CMP_BYTES);
		real_prev = Z_Malloc(FG_CMP_BYTES);
		gen       = Z_Malloc(FG_CMP_BYTES);
		if (!real_now || !real_prev || !gen)
			return;
	}

	/* TWO CONSECUTIVE FRAMES, not two samples a second apart.

	   The first version stored the previous SAMPLE as the reference, so "real-vs-prev"
	   described a second of motion rather than one frame, and the numbers were unusable
	   as a scale - sometimes far larger than a frame step, sometimes smaller if the view
	   had come back around. The gen1 > gen2 ordering was still meaningful because those
	   share one reference, but at 2x there is no ordering to check, which is exactly the
	   case that needed answering.

	   So arm on the trigger frame, capture the real image, and do the real work on the
	   NEXT frame - whose generated images interpolate between precisely those two. */
	static bool     armed = false;
	static uint64_t last_us = 0;
	uint64_t now = Sys_Microseconds();

	if (!armed) {
		if (last_us != 0 && now - last_us < 1000000)
			return;
		if (fg_cmp_read_region(qvk.images[VKPT_IMG_DLSS_OUTPUT], real_prev))
			armed = true;
		return;
	}

	armed = false;
	last_us = now;

	if (!fg_cmp_read_region(qvk.images[VKPT_IMG_DLSS_OUTPUT], real_now))
		return;

	double step = fg_cmp_mean_abs_diff(real_now, real_prev);

	char line[512];
	int off = Q_snprintf(line, sizeof(line),
		"DLSS-G compare: frame step %.5f, real mean %.4f |", step,
		fg_cmp_mean_value(real_now));

	for (unsigned int i = 1; i <= generated_count; i++) {
		if (!fg_cmp_read_region(GetDLSSGImage(i), gen))
			continue;

		double d = fg_cmp_mean_abs_diff(gen, real_now);

		/* As a FRACTION of one frame step, which is the interpretable number: a frame
		   generated at phase k/(N+1) should sit that far back from the real frame, so
		   2x wants ~50%, and 3x wants ~67% then ~33%. Near 0% is a copy of the real
		   frame; near or above 100% is not an in-between frame at all. */
		off += Q_snprintf(line + off, sizeof(line) - off,
			" gen%u mean %.4f diff %.5f (%.0f%%)",
			i, fg_cmp_mean_value(gen), d, step > 0.0 ? d * 100.0 / step : 0.0);
	}

	Com_Printf("%s\n", line);
}

/* Debug: paint the CURRENT swapchain image a solid colour. Used by pt_dlss_fg_debug to
   answer "are the generated frames actually reaching the display?" without argument - if
   they are, the screen strobes; if the picture looks normal, they never arrive. One
   colour per generated frame index, so at 4x/6x you can also see how many get through.
   vkpt_final_blit_simple leaves the swapchain image in PRESENT_SRC_KHR, so bounce it
   through TRANSFER_DST and put it back. */
static void
fg_debug_paint_current_swapchain_image(VkCommandBuffer cmd_buf, unsigned int gen_index)
{
	static const VkClearColorValue colours[] = {
		{ { 1.0f, 0.0f, 0.0f, 1.0f } },   // red
		{ { 0.0f, 1.0f, 0.0f, 1.0f } },   // green
		{ { 0.0f, 0.0f, 1.0f, 1.0f } },   // blue
		{ { 1.0f, 1.0f, 0.0f, 1.0f } },   // yellow
		{ { 1.0f, 0.0f, 1.0f, 1.0f } },   // magenta
	};

	VkImageSubresourceRange range = {
		.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel   = 0,
		.levelCount     = 1,
		.baseArrayLayer = 0,
		.layerCount     = 1,
	};

	VkImage img = qvk.swap_chain_images[qvk.current_swap_chain_image_index];

	IMAGE_BARRIER(cmd_buf,
		.image = img,
		.subresourceRange = range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	vkCmdClearColorImage(cmd_buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&colours[gen_index % LENGTH(colours)], 1, &range);

	IMAGE_BARRIER(cmd_buf,
		.image = img,
		.subresourceRange = range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);
}

/* Acquire an image with the swapchain lock held, WITHOUT holding it while blocked.

   VkSwapchainKHR needs external synchronisation and the frame generation present thread
   uses it too, so the acquire has to be inside the lock. But a blocking acquire inside
   the lock deadlocks: the images it is waiting for are released by presents that need the
   same lock. So poll with a short timeout and drop the lock between attempts. */
static VkResult
acquire_next_image_locked(VkSemaphore semaphore, uint32_t *out_index)
{
	for (;;) {
		VkResult res;

		FGPresent_SwapchainLock();
#ifdef VKPT_DEVICE_GROUPS
		VkAcquireNextImageInfoKHR acquire_info = {
			.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
			.swapchain = qvk.swap_chain,
			.timeout = 1000000, /* 1 ms */
			.semaphore = semaphore,
			.fence = VK_NULL_HANDLE,
			.deviceMask = (1 << qvk.device_count) - 1,
		};
		res = vkAcquireNextImage2KHR(qvk.device, &acquire_info, out_index);
#else
		res = vkAcquireNextImageKHR(qvk.device, qvk.swap_chain, 1000000,
			semaphore, VK_NULL_HANDLE, out_index);
#endif
		FGPresent_SwapchainUnlock();

		/* Nothing was acquired and the semaphore was not signalled, so retrying is safe.
		   Yield so the present thread can actually release an image. */
		if (res == VK_TIMEOUT || res == VK_NOT_READY) {
			SDL_Delay(0);
			continue;
		}

		return res;
	}
}

void vkpt_device_wait_idle(void)
{
	FGPresent_SwapchainLock();
	vkDeviceWaitIdle(qvk.device);
	FGPresent_SwapchainUnlock();
}

VkResult vkpt_queue_wait_idle(VkQueue queue)
{
	FGPresent_SwapchainLock();
	VkResult res = vkQueueWaitIdle(queue);
	FGPresent_SwapchainUnlock();
	return res;
}

static uint32_t swapchain_requested_images = 0;


/* Whether the swapchain in use had its vsync overridden by frame generation.
   Recorded at create time: toggling FG while vid_vsync is on changes the present
   mode, and the image-count comparison alone will not notice if the count happens
   to be unchanged (an explicit vid_swapchain_images above the FG floor). */
static bool swapchain_fg_forced_no_vsync = false;

/* True when frame generation should override vid_vsync. Kept as a function so the
   create path and the per-frame recreate check cannot disagree. */
static bool
fg_wants_no_vsync(void)
{
	cvar_t *forced = Cvar_Get("pt_dlss_fg_force_novsync", "0", CVAR_ARCHIVE);
	return forced->integer != 0 && DLSSGMultiplier() > 1 && cvar_vsync->integer != 0;
}

static uint32_t
desired_swapchain_images(void)
{
	uint32_t n = (cvar_swapchain_images->integer > 0)
		? (uint32_t)cvar_swapchain_images->integer : 0;


/* Frame generation presents once per generated frame plus once for the real one,
	   i.e. exactly `multiplier` presents per rendered frame, so a 2-image swapchain
	   cannot work. Ideally MAX_FRAMES_IN_FLIGHT frames each hold a full set plus one
	   being scanned out. At high multipliers that exceeds maxImageCount (8 here) and
	   gets clamped below - which is fine: the acquire then blocks until the
	   presentation engine releases an image, and that backpressure IS the pacing. It
	   cannot deadlock, because the previous frame's presents are already queued and
	   complete independently of this one. */
	unsigned int mult = DLSSGMultiplier();
	if (mult > 1)
	{
		/* MEASURED: do NOT tighten this to mult + 1 on a latency theory. Under FIFO each
		   extra image is another queued present, so fewer images LOOKS like it should cut
		   latency - it does the opposite here. 3 images at 2x measured 395 ms against
		   52 ms at 5: the acquire starves, and the polling loop in
		   acquire_next_image_locked() then burns the frame waiting. Tried 2026-08-29. */
		/* One full group MORE than the frames in flight. With the floor at
		   MAX_FRAMES_IN_FLIGHT * mult + 1 the acquire limit leaves max_pending = 1 at
		   2x, so the main thread blocks until the group has almost drained: measured
		   38.7 ms of stall in a 40 ms frame, settling at 25 rendered / 50 presented on
		   a 60 Hz display that should take 30 / 60. Once the present schedule decides
		   when frames go out, extra images cost nothing in latency - the queue depth is
		   bounded by the schedule, not by the image count - and they stop the acquire
		   starving. (Do NOT read this as a licence to tighten it: 3 images at 2x
		   measured 395 ms per frame, see the note above.) */
		n = max(n, (uint32_t)(MAX_FRAMES_IN_FLIGHT * mult + mult + 1));
	}

	return n;
}

VkResult
create_swapchain(void)
{
    num_accumulated_frames = 0;

	/* create swapchain (query details and ignore them afterwards :-) )*/
	VkSurfaceCapabilitiesKHR surf_capabilities;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(qvk.physical_device, qvk.surface, &surf_capabilities);

	if (surf_capabilities.currentExtent.width == 0 || surf_capabilities.currentExtent.height == 0)
		return VK_SUCCESS;

	uint32_t num_formats = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(qvk.physical_device, qvk.surface, &num_formats, NULL);
	VkSurfaceFormatKHR *avail_surface_formats = alloca(sizeof(VkSurfaceFormatKHR) * num_formats);
	vkGetPhysicalDeviceSurfaceFormatsKHR(qvk.physical_device, qvk.surface, &num_formats, avail_surface_formats);
	/* Com_Printf("num surface formats: %d\n", num_formats);

	Com_Printf("available surface formats:\n");
	for(int i = 0; i < num_formats; i++)
		Com_Printf("  %s\n", vk_format_to_string(avail_surface_formats[i].format)); */ 


	picked_surface_format_t picked_format;
	bool surface_format_found = false;
	if(cvar_hdr->integer != 0) {
		surface_format_found = pick_surface_format_hdr(&picked_format, avail_surface_formats, num_formats);
		qvk.surf_is_hdr = surface_format_found;
		if(!surface_format_found) {
			Com_WPrintf("HDR was requested but no supported surface format was found.\n");
			Cvar_SetByVar(cvar_hdr, "0", FROM_CODE);
			}
	} else {
		qvk.surf_is_hdr = false;
	}
	if(!surface_format_found) {
		// HDR disabled, or fallback to SDR
		surface_format_found = pick_surface_format_sdr(&picked_format, avail_surface_formats, num_formats);
	}
	if(!surface_format_found) {
		Com_EPrintf("no acceptable surface format available!\n");
		return 1;
	}
	qvk.surf_format = picked_format.surface_fmt;

	uint32_t num_present_modes = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(qvk.physical_device, qvk.surface, &num_present_modes, NULL);
	VkPresentModeKHR *avail_present_modes = alloca(sizeof(VkPresentModeKHR) * num_present_modes);
	vkGetPhysicalDeviceSurfacePresentModesKHR(qvk.physical_device, qvk.surface, &num_present_modes, avail_present_modes);
	bool immediate_mode_available = false;

	for (int i = 0; i < num_present_modes; i++) {
		if (avail_present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
			immediate_mode_available = true;
			break;
		}
	}

	bool mailbox_mode_available = false;
	for (int i = 0; i < num_present_modes; i++) {
		if (avail_present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
			mailbox_mode_available = true;
			break;
		}
	}

	qvk.surf_vsync = (cvar_vsync->integer != 0);
	qvk.surf_vsync_mailbox = (cvar_vsync_mailbox->integer != 0);

	/* Frame generation used to force a non-vsync present mode here, because back when
	   presents were issued from the main thread FIFO divided the render rate by the
	   multiplier. THAT REASONING IS OBSOLETE: presents now go through the present
	   thread, so FIFO paces them to vblank without touching the render loop, and it
	   is the only way to get tear-free, refresh-aligned output on a fixed-refresh
	   panel. An unaligned present rate (83/s into 60Hz, say) beats against the
	   display and reads as "same motion, worse pacing".
	
	   vid_vsync is therefore respected again. pt_dlss_fg_force_novsync 1 restores the
	   old override for comparison. NOTE: under FIFO the presented rate is capped at
	   the refresh rate, so pick a multiplier with rendered * multiplier <= refresh -
	   beyond that the queue backs up and the acquire throttles rendering. */
	static cvar_t *cvar_fg_force_novsync = NULL;
	if (!cvar_fg_force_novsync)
		cvar_fg_force_novsync = Cvar_Get("pt_dlss_fg_force_novsync", "0", CVAR_ARCHIVE);

	bool fg_forces_no_vsync = cvar_fg_force_novsync->integer != 0
		&& fg_wants_no_vsync()
		&& (immediate_mode_available || mailbox_mode_available);
	swapchain_fg_forced_no_vsync = fg_forces_no_vsync;

	if (qvk.surf_vsync && !fg_forces_no_vsync) {
		qvk.present_mode = (qvk.surf_vsync_mailbox && mailbox_mode_available)
			? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
	} else if (immediate_mode_available) {
		qvk.present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	} else {
		qvk.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
	}

	if (fg_forces_no_vsync) {
		static bool fg_vsync_warned = false;
		if (!fg_vsync_warned) {
			fg_vsync_warned = true;
			Com_Printf("DLSS-G: vid_vsync is on; overriding to a non-vsync present mode."
				" Frame generation cannot help under FIFO.\n");
		}
	}

	if(surf_capabilities.currentExtent.width != ~0u) {
		qvk.extent_unscaled = surf_capabilities.currentExtent;
	}
	else {
		qvk.extent_unscaled.width = min(surf_capabilities.maxImageExtent.width, qvk.win_width);
		qvk.extent_unscaled.height = min(surf_capabilities.maxImageExtent.height, qvk.win_height);

		qvk.extent_unscaled.width = max(surf_capabilities.minImageExtent.width, qvk.extent_unscaled.width);
		qvk.extent_unscaled.height = max(surf_capabilities.minImageExtent.height, qvk.extent_unscaled.height);
	}

	/* Swapchain image count.

	   This used to be a hard 2. With FIFO (vid_vsync 1) two images means the
	   presentation engine owns one and the app owns the other, so nothing can
	   be queued ahead: after presenting, the next acquire blocks until vblank
	   releases the other image. The GPU then sits idle for whatever is left of
	   the refresh interval, and any frame that overruns the interval - which
	   the added DLSS work makes far easier - misses the next vblank entirely
	   and the rate halves (60 -> 30, 144 -> 72). That reads as "the frame rate
	   dropped and the GPU is not busy", and it clears up when the swapchain is
	   recreated (a resolution change, or toggling fullscreen) purely because
	   that re-rolls the phase against vblank.

	   minImageCount + 1 gives one image of slack, so the app can build the next
	   frame while the previous one waits to be scanned out.

	   The image COUNT is not the bug, measured 2026-08-28: starting at 2 is bad,
	   switching to 4 is bad, switching back to 2 is good - and the log shows identical
	   swapchain parameters in the bad and good states. What fixes it is the RECREATE,
	   not what is recreated, which is also why toggling fullscreen has always cleared
	   it. Whatever is wrong is state that survives the initial create and is reset by
	   any subsequent one. Do not spend time tuning this number FOR THE STALL.

	   Frame generation is a separate matter: it genuinely needs more than 2, because
	   it presents twice per rendered frame. desired_swapchain_images() floors the
	   count at DLSSG_MIN_SWAPCHAIN_IMAGES whenever pt_dlss_fg is on, overriding the
	   vid_swapchain_images cvar (which both q2config.cfg files pin to 2). */
	swapchain_requested_images = desired_swapchain_images();

	uint32_t num_images;
	if (swapchain_requested_images > 0)
		num_images = swapchain_requested_images;
	else
		num_images = surf_capabilities.minImageCount + 1;

	num_images = max(num_images, surf_capabilities.minImageCount);
	if(surf_capabilities.maxImageCount > 0)
		num_images = min(num_images, surf_capabilities.maxImageCount);

	qvk.surf_num_images = num_images;

	Com_Printf("Swapchain: %u images (min %u, max %u), %ux%u, present mode %s [%s]\n",
		num_images, surf_capabilities.minImageCount, surf_capabilities.maxImageCount,
		qvk.extent_unscaled.width, qvk.extent_unscaled.height,
		qvk.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)" :
		qvk.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX",
		swapchain_reason);

	// Ask for exclusive fullscreen. SDL_WINDOW_FULLSCREEN only sets the display mode -
	// with Vulkan the swapchain is still an ordinary windowed one to the compositor, and
	// whether it gets independent flip or gets composited is DWM's choice, made afresh on
	// every swapchain creation. This is the only way to claim the flip outright.
	qvk.surf_fse_acquired = false;
	bool want_fse = qvk.supports_fse
		&& cvar_fullscreen_exclusive->integer != 0
		&& vid_fullscreen->integer != 0;

#ifdef _WIN32
	VkSurfaceFullScreenExclusiveWin32InfoEXT fse_win32 = {
		.sType    = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT,
		.hmonitor = vkpt_get_window_monitor(),
	};
	if (!fse_win32.hmonitor)
		want_fse = false;
#endif

	VkSurfaceFullScreenExclusiveInfoEXT fse_info = {
		.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT,
#ifdef _WIN32
		.pNext = &fse_win32,
#endif
		.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT,
	};

	/* Reflex latency tracking is opted into per swapchain, at creation. Chained ahead
	   of the full-screen-exclusive info so both can be present. */
	/* THE SWAPCHAIN OPT-IN FOLLOWS pt_reflex NOW, AND THAT MATTERS.
	
	   latencyModeEnable was hardcoded VK_TRUE whenever VK_NV_low_latency2 existed, so
	   pt_reflex 0 only skipped the sleep call - the swapchain stayed opted into
	   NVIDIA's low-latency frame pacing and there was no way to switch Reflex off
	   from inside the game.
	
	   That is the leading suspect for the engine NEVER tearing with vid_vsync 0:
	   IMMEDIATE, exclusive fullscreen acquired, 100 fps into a 60 Hz panel, frame
	   generation off - and no tear line at any rate. Matt confirmed other titles DO
	   tear on the same display, so it is this engine, not the screen. Present mode
	   selection is verified correct (IMMEDIATE is queried for support before use),
	   which leaves the driver-side pacing this flag turns on.
	
	   pt_reflex 0 + vid_restart now genuinely removes Reflex from the swapchain. */
	cvar_t *cvar_reflex_mode = Cvar_Get("pt_reflex", "1", CVAR_ARCHIVE);
	const bool want_latency_mode = qvk.supports_low_latency
	                            && cvar_reflex_mode->integer != 0;

	VkSwapchainLatencyCreateInfoNV latency_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,
		.pNext = want_fse ? &fse_info : NULL,
		.latencyModeEnable = VK_TRUE,
	};

	const void* swpch_pnext = want_fse ? (const void*)&fse_info : NULL;
	if (want_latency_mode)
		swpch_pnext = &latency_info;

	VkSwapchainCreateInfoKHR swpch_create_info = {
		.sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.pNext                 = swpch_pnext,
		.surface               = qvk.surface,
		.minImageCount         = num_images,
		.imageFormat           = qvk.surf_format.format,
		.imageColorSpace       = qvk.surf_format.colorSpace,
		.imageExtent           = qvk.extent_unscaled,
		.imageArrayLayers      = 1, /* only needs to be changed for stereoscopic rendering */ 
		.imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
							   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
							   | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE, /* VK_SHARING_MODE_CONCURRENT if not using same queue */
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices   = NULL,
		.preTransform          = surf_capabilities.currentTransform,
		.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, /* no alpha for window transparency */
		.presentMode           = qvk.present_mode,
		.clipped               = VK_FALSE, /* do not render pixels that are occluded by other windows */
		//.clipped               = VK_TRUE, /* do not render pixels that are occluded by other windows */
		.oldSwapchain          = VK_NULL_HANDLE, /* need to provide previous swapchain in case of window resize */
	};

	if(vkCreateSwapchainKHR(qvk.device, &swpch_create_info, NULL, &qvk.swap_chain) != VK_SUCCESS) {
		Com_EPrintf("error creating swapchain\n");
		return 1;
	}

	// APPLICATION_CONTROLLED only makes the mode available - it has to be taken. Failing
	// is not fatal: the swapchain still works, we just did not get the flip.
	if (want_fse && qvkAcquireFullScreenExclusiveModeEXT)
	{
		VkResult fse_res = qvkAcquireFullScreenExclusiveModeEXT(qvk.device, qvk.swap_chain);
		qvk.surf_fse_acquired = (fse_res == VK_SUCCESS);
		Com_Printf("Full-screen exclusive: %s\n",
			qvk.surf_fse_acquired ? "acquired" : qvk_result_to_string(fse_res));
	}

	/* Frame generation phase-locks its presents to this. Started here rather than at
	   renderer init because it is per-MONITOR, and the window may not have been on this
	   one before. Starting twice is a no-op. */
#ifdef _WIN32
	{
		char gdi_device[64];
		if (vkpt_get_window_gdi_device(gdi_device, sizeof(gdi_device)))
			FGPresent_VBlankStart(gdi_device);
	}
#endif

	vkGetSwapchainImagesKHR(qvk.device, qvk.swap_chain, &qvk.num_swap_chain_images, NULL);
	assert(qvk.num_swap_chain_images);
	qvk.swap_chain_images = malloc(qvk.num_swap_chain_images * sizeof(*qvk.swap_chain_images));
	vkGetSwapchainImagesKHR(qvk.device, qvk.swap_chain, &qvk.num_swap_chain_images, qvk.swap_chain_images);

	/* The Reflex sleep mode is a property of the swapchain, so it has to be re-applied
	   to each new one. */
	Reflex_OnSwapchainCreated(qvk.swap_chain);

	qvk.swap_chain_image_views = malloc(qvk.num_swap_chain_images * sizeof(*qvk.swap_chain_image_views));
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		VkImageViewCreateInfo img_create_info = {
			.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image      = qvk.swap_chain_images[i],
			.viewType   = VK_IMAGE_VIEW_TYPE_2D,
			.format     = picked_format.swapchain_view_fmt,
#if 1
			.components = {
				VK_COMPONENT_SWIZZLE_R,
				VK_COMPONENT_SWIZZLE_G,
				VK_COMPONENT_SWIZZLE_B,
				VK_COMPONENT_SWIZZLE_A
			},
#endif
			.subresourceRange = {
				.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel   = 0,
				.levelCount     = 1,
				.baseArrayLayer = 0,
				.layerCount     = 1
			}
		};

		if(vkCreateImageView(qvk.device, &img_create_info, NULL, qvk.swap_chain_image_views + i) != VK_SUCCESS) {
			Com_EPrintf("error creating image view!");

			free(qvk.swap_chain_image_views);
			qvk.swap_chain_image_views = NULL;

			free(qvk.swap_chain_images);
			qvk.swap_chain_images = NULL;

			qvk.num_swap_chain_images = 0;
			return 1;
		}
	}

	/* One present-wait semaphore per swapchain image - see the field comment in vkpt.h. */
	qvk.semaphores_present = malloc(qvk.num_swap_chain_images * sizeof(*qvk.semaphores_present));
	for (int i = 0; i < qvk.num_swap_chain_images; i++) {
		VkSemaphoreCreateInfo sem_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		_VK(vkCreateSemaphore(qvk.device, &sem_info, NULL, qvk.semaphores_present + i));
		ATTACH_LABEL_VARIABLE(qvk.semaphores_present[i], SEMAPHORE);
	}

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	for (int image_index = 0; image_index < qvk.num_swap_chain_images; image_index++)
	{
		IMAGE_BARRIER(cmd_buf,
			.image = qvk.swap_chain_images[image_index],
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.srcAccessMask = 0,
			.dstAccessMask = 0,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		);
	}

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	return VK_SUCCESS;
}

VkResult
create_command_pool_and_fences(void)
{
	VkCommandPoolCreateInfo cmd_pool_create_info = {
		.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qvk.queue_idx_graphics,
		.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	/* command pool and buffers */
	_VK(vkCreateCommandPool(qvk.device, &cmd_pool_create_info, NULL, &qvk.cmd_buffers_graphics.command_pool));
	
	cmd_pool_create_info.queueFamilyIndex = qvk.queue_idx_transfer;
	_VK(vkCreateCommandPool(qvk.device, &cmd_pool_create_info, NULL, &qvk.cmd_buffers_transfer.command_pool));

	/* fences and semaphores */
	for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
	{
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			semaphore_group_t* group = &qvk.semaphores[frame][gpu];

			VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->image_available));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->render_finished));
			for (int fg = 0; fg < DLSSG_MAX_GENERATED_FRAMES; fg++)
				_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->image_available_fg[fg]));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->transfer_finished));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->trace_finished));

			ATTACH_LABEL_VARIABLE(group->image_available, SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->render_finished, SEMAPHORE);
			for (int fg = 0; fg < DLSSG_MAX_GENERATED_FRAMES; fg++)
				ATTACH_LABEL_VARIABLE(group->image_available_fg[fg], SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->transfer_finished, SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->trace_finished, SEMAPHORE);

			group->trace_signaled = false;
		}
	}

	VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT, /* fence's initial state set to be signaled
												  to make program not hang */
	};
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		_VK(vkCreateFence(qvk.device, &fence_info, NULL, qvk.fences_frame_sync + i));
		ATTACH_LABEL_VARIABLE(qvk.fences_frame_sync[i], FENCE);
	}

	return VK_SUCCESS;
}

static void
append_string_list(const char** dst, uint32_t* dst_count, uint32_t dst_capacity, const char** src, uint32_t src_count)
{
	assert(*dst_count + src_count <= dst_capacity);
	dst += *dst_count;
	memcpy((void*)dst, src, sizeof(char*) * src_count);
	*dst_count += src_count;
}

bool
init_vulkan(void)
{
	Com_Printf("----- init_vulkan -----\n");

	/* layers */
	get_vk_layer_list(&qvk.num_layers, &qvk.layers);
	Com_Printf("Available Vulkan layers: \n");
	for(int i = 0; i < qvk.num_layers; i++) {
		Com_Printf("  %s\n", qvk.layers[i].layerName);
	}
	
	/* instance extensions */

	if (!SDL_Vulkan_GetInstanceExtensions(qvk.window, &qvk.num_sdl2_extensions, NULL)) {
		Com_EPrintf("Couldn't get SDL2 Vulkan extension count\n");
		return false;
	}

	qvk.sdl2_extensions = malloc(sizeof(char*) * qvk.num_sdl2_extensions);
	if (!SDL_Vulkan_GetInstanceExtensions(qvk.window, &qvk.num_sdl2_extensions, qvk.sdl2_extensions)) {
		Com_EPrintf("Couldn't get SDL2 Vulkan extensions\n");
		return false;
	}

	Com_Printf("Vulkan instance extensions required by SDL2: \n");
	for (int i = 0; i < qvk.num_sdl2_extensions; i++) {
		Com_Printf("  %s\n", qvk.sdl2_extensions[i]);
	}

	int num_inst_ext_combined = qvk.num_sdl2_extensions + LENGTH(vk_requested_instance_extensions);
	char **ext = alloca(sizeof(char *) * num_inst_ext_combined);
	memcpy(ext, qvk.sdl2_extensions, qvk.num_sdl2_extensions * sizeof(*qvk.sdl2_extensions));
	memcpy(ext + qvk.num_sdl2_extensions, vk_requested_instance_extensions, sizeof(vk_requested_instance_extensions));

	get_vk_extension_list(NULL, &qvk.num_extensions, &qvk.extensions); /* valid here? */
	Com_Printf("Supported Vulkan instance extensions: \n");
	for(int i = 0; i < qvk.num_extensions; i++) {
		int requested = 0;
		for(int j = 0; j < num_inst_ext_combined; j++) {
			if(!strcmp(qvk.extensions[i].extensionName, ext[j])) {
				requested = 1;
				break;
			}
		}
		Com_Printf("  %s%s\n", qvk.extensions[i].extensionName, requested ? " (requested)" : "");
	}

	/* create instance */
	VkInstanceCreateInfo inst_create_info = {
		.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo        = &vk_app_info,
		.enabledExtensionCount   = num_inst_ext_combined,
		.ppEnabledExtensionNames = (const char * const*)ext,
	};

	qvk.enable_validation = false;

	if (cvar_vk_validation->integer)
	{
		inst_create_info.ppEnabledLayerNames = vk_validation_layers;
		inst_create_info.enabledLayerCount = LENGTH(vk_validation_layers);
	
		qvk.enable_validation = true;
	}

	VkResult result = vkCreateInstance(&inst_create_info, NULL, &qvk.instance);

	if (result == VK_ERROR_LAYER_NOT_PRESENT)
	{
		Com_WPrintf("Vulkan validation layer is requested through cvar %s but is not available.\n", cvar_vk_validation->name);

		// Try again, this time without the validation layer

		inst_create_info.enabledLayerCount = 0;
		result = vkCreateInstance(&inst_create_info, NULL, &qvk.instance);
		qvk.enable_validation = false;
	}
	else if (cvar_vk_validation->integer)
	{
		Com_WPrintf("Vulkan validation layer is enabled, expect degraded game performance.\n");
	}

	if (result != VK_SUCCESS)
	{
		Com_Error(ERR_FATAL, "Failed to initialize a Vulkan instance.\nError code: %s", qvk_result_to_string(result));
		return false;
	}

#define VK_EXTENSION_DO(a) \
		q##a = (PFN_##a) vkGetInstanceProcAddr(qvk.instance, #a); \
		if (!q##a) { Com_EPrintf("warning: could not load instance function %s\n", #a); }
	LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

	/* setup debug callback */
	VkDebugUtilsMessengerCreateInfoEXT dbg_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType =
			  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.pfnUserCallback = vk_debug_callback,
		.pUserData = NULL
	};

	_VK(qvkCreateDebugUtilsMessengerEXT(qvk.instance, &dbg_create_info, NULL, &qvk.dbg_messenger));

	


	/* create surface */
	if(!SDL_Vulkan_CreateSurface(qvk.window, qvk.instance, &qvk.surface)) {
		Com_EPrintf("SDL2 could not create a surface!\n");
		return false;
	}

	/* pick physical device (iterate over all but pick device 0 anyways) */
	uint32_t num_devices = 0;
	_VK(vkEnumeratePhysicalDevices(qvk.instance, &num_devices, NULL));
	if(num_devices == 0)
		return false;
	VkPhysicalDevice *devices = alloca(sizeof(VkPhysicalDevice) *num_devices);
	_VK(vkEnumeratePhysicalDevices(qvk.instance, &num_devices, devices));	

#ifdef VKPT_DEVICE_GROUPS
	uint32_t num_device_groups = 0;

	if (cvar_sli->integer)
		_VK(vkEnumeratePhysicalDeviceGroups(qvk.instance, &num_device_groups, NULL));

	VkDeviceGroupDeviceCreateInfo device_group_create_info;
	VkPhysicalDeviceGroupProperties device_group_info;
	device_group_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES;
	device_group_info.pNext = NULL;

	if(num_device_groups > 0) {
		// we always use the first group
		num_device_groups = 1;
		_VK(vkEnumeratePhysicalDeviceGroups(qvk.instance, &num_device_groups, &device_group_info));

		if (device_group_info.physicalDeviceCount > VKPT_MAX_GPUS)
		{
			Com_EPrintf("SLI: device group 0 has %d devices, which is more than maximum supported count (%d).\n",
				device_group_info.physicalDeviceCount, VKPT_MAX_GPUS);
			return false;
		}

		device_group_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO;
		device_group_create_info.pNext = NULL;
		device_group_create_info.physicalDeviceCount = device_group_info.physicalDeviceCount;
		device_group_create_info.pPhysicalDevices = device_group_info.physicalDevices;

		qvk.device_count = device_group_create_info.physicalDeviceCount;
		for(int i = 0; i < qvk.device_count; i++) {
			qvk.device_group_physical_devices[i] = device_group_create_info.pPhysicalDevices[i];
		}
		Com_Printf("SLI: using device group 0 with %d device(s).\n", qvk.device_count);
	}
	else
	{
		qvk.device_count = 1;
		if (!cvar_sli->integer)
			Com_Printf("SLI: multi-GPU support disabled through the 'sli' console variable.\n");
		else
			Com_Printf("SLI: no device groups found, using a single device.\n");
	}
#else
	qvk.device_count = 1;
#endif

	int picked_device_with_ray_pipeline = -1;
	int picked_device_with_ray_query = -1;
	VkDriverId picked_driver_ray_query = VK_DRIVER_ID_MAX_ENUM;
	qvk.use_ray_query = false;

	for(int i = 0; i < num_devices; i++) 
	{
		VkPhysicalDeviceDriverProperties driver_properties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
			.pNext = NULL
		};

		VkPhysicalDeviceProperties2 dev_properties2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &driver_properties
		};
		vkGetPhysicalDeviceProperties2(devices[i], &dev_properties2);

		VkPhysicalDeviceFeatures dev_features;
		vkGetPhysicalDeviceFeatures(devices[i], &dev_features);

		Com_Printf("Physical device %d: %s\n", i, dev_properties2.properties.deviceName);

		uint32_t num_ext;
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &num_ext, NULL);

		VkExtensionProperties *ext_properties = alloca(sizeof(VkExtensionProperties) * num_ext);
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &num_ext, ext_properties);

		Com_Printf("Supported Vulkan device extensions:\n");
		for(int j = 0; j < num_ext; j++) 
		{
			Com_Printf("  %s\n", ext_properties[j].extensionName);

			if(!strcmp(ext_properties[j].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) 
			{
				if (picked_device_with_ray_pipeline < 0)
				{
					picked_device_with_ray_pipeline = i;
				}
			}

			if (!strcmp(ext_properties[j].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME))
			{
				if (picked_device_with_ray_query < 0)
				{
					picked_device_with_ray_query = i;
					picked_driver_ray_query = driver_properties.driverID;
				}
			}

			if (!strcmp(ext_properties[j].extensionName, VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
				qvk.supports_fse = true;

			if (!strcmp(ext_properties[j].extensionName, VK_NV_LOW_LATENCY_2_EXTENSION_NAME))
				qvk.supports_low_latency = true;

			if (!strcmp(ext_properties[j].extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME))
				qvk.supports_present_id = true;
		}
	}

	int picked_device = -1;

	if (!Q_strcasecmp(cvar_ray_tracing_api->string, "query") && picked_device_with_ray_query >= 0)
	{
		qvk.use_ray_query = true;
		picked_device = picked_device_with_ray_query;
	}
	else if (!Q_strcasecmp(cvar_ray_tracing_api->string, "pipeline") && picked_device_with_ray_pipeline >= 0)
	{
		qvk.use_ray_query = false;
		picked_device = picked_device_with_ray_pipeline;
	}
	
	if (picked_device < 0)
	{
		if (Q_strcasecmp(cvar_ray_tracing_api->string, "auto"))
		{
			Com_WPrintf("Requested Ray Tracing API (%s) is not available, switching to automatic selection.\n", cvar_ray_tracing_api->string);
		}

		if (picked_driver_ray_query == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
		{
			// Pick KHR_ray_query on NVIDIA drivers, if available.
			qvk.use_ray_query = true;
			picked_device = picked_device_with_ray_query;
		}
		else if (picked_device_with_ray_pipeline >= 0)
		{
			// Pick KHR_ray_tracing_pipeline otherwise
			qvk.use_ray_query = false;
			picked_device = picked_device_with_ray_pipeline;
		}
	}

	if (picked_device < 0)
	{
		Com_Error(ERR_FATAL, "No ray tracing capable GPU found.");
	}

	qvk.physical_device = devices[picked_device];

	{
		VkPhysicalDeviceDriverProperties driver_properties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
			.pNext = NULL
		};

		VkPhysicalDeviceProperties2 dev_properties2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &driver_properties
		};

		vkGetPhysicalDeviceProperties2(devices[picked_device], &dev_properties2);

		// Store the timestamp period to get correct profiler results
		qvk.timestampPeriod = dev_properties2.properties.limits.timestampPeriod;

		Com_Printf("Picked physical device %d: %s\n", picked_device, dev_properties2.properties.deviceName);
		Com_Printf("Using %s\n", (qvk.use_ray_query ? VK_KHR_RAY_QUERY_EXTENSION_NAME : VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));

#ifdef _WIN32
		if (dev_properties2.properties.vendorID == 0x10de) // NVIDIA vendor ID
		{
			uint32_t driver_major = (dev_properties2.properties.driverVersion >> 22) & 0x3ff;
			uint32_t driver_minor = (dev_properties2.properties.driverVersion >> 14) & 0xff;

			Com_Printf("NVIDIA GPU detected. Driver version: %u.%02u\n", driver_major, driver_minor);
			
			uint32_t required_major = 0;
			uint32_t required_minor = 0;
			int nfields = sscanf(cvar_min_driver_version_nvidia->string, "%u.%u", &required_major, &required_minor);
			if (nfields == 2)
			{
				if (driver_major < required_major || driver_major == required_major && driver_minor < required_minor)
				{
					Com_Error(ERR_FATAL, "This game requires NVIDIA Graphics Driver version to be at least %u.%02u, "
						"while the installed version is %u.%02u.\nPlease update the NVIDIA Graphics Driver.",
						required_major, required_minor, driver_major, driver_minor);
				}
			}
		}
		else if (driver_properties.driverID == VK_DRIVER_ID_AMD_PROPRIETARY)
		{
			Com_Printf("AMD GPU detected. Driver version: %s\n", driver_properties.driverInfo);

			uint32_t present_major = 0;
			uint32_t present_minor = 0;
			uint32_t present_patch = 0;
			int nfields_present = sscanf(driver_properties.driverInfo, "%u.%u.%u", &present_major, &present_minor, &present_patch);

			uint32_t required_major = 0;
			uint32_t required_minor = 0;
			uint32_t required_patch = 0;
			int nfields_required = sscanf(cvar_min_driver_version_amd->string, "%u.%u.%u", &required_major, &required_minor, &required_patch);

			if (nfields_present == 3 && nfields_required == 3)
			{
				if (present_major < required_major || present_major == required_major && present_minor < required_minor || present_major == required_major && present_minor == required_minor && present_patch < required_patch)
				{
					Com_Error(ERR_FATAL, "This game requires AMD Radeon Software version to be at least %s, while the installed version is %s.\nPlease update the AMD Radeon Software.",
						cvar_min_driver_version_amd->string, driver_properties.driverInfo);
				}
			}
		}
#endif
	}

	// Query device 16-bit float capabilities
	VkPhysicalDevice16BitStorageFeatures features_16bit_storage = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
	};
	{
		VkPhysicalDeviceVulkan12Features device_features_1_2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
			.pNext = &features_16bit_storage
		};
		VkPhysicalDeviceFeatures2 device_features = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
			.pNext = &device_features_1_2
		};
		vkGetPhysicalDeviceFeatures2(qvk.physical_device, &device_features);
		qvk.supports_fp16 = device_features_1_2.shaderFloat16 && features_16bit_storage.storageBuffer16BitAccess;
	}
	Com_Printf("FP16 support: %s\n", qvk.supports_fp16 ? "yes" : "no");

	vkGetPhysicalDeviceMemoryProperties(qvk.physical_device, &qvk.mem_properties);

	/* queue family and create physical device */
	uint32_t num_queue_families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(qvk.physical_device, &num_queue_families, NULL);
	VkQueueFamilyProperties *queue_families = alloca(sizeof(VkQueueFamilyProperties) * num_queue_families);
	vkGetPhysicalDeviceQueueFamilyProperties(qvk.physical_device, &num_queue_families, queue_families);

	// Com_Printf("num queue families: %d\n", num_queue_families);

	qvk.queue_idx_graphics = -1;
	qvk.queue_idx_transfer = -1;
	uint32_t graphics_family_queue_count = 0;

	for(int i = 0; i < num_queue_families; i++) {
		if(!queue_families[i].queueCount)
			continue;
		VkBool32 present_support = 0;
		vkGetPhysicalDeviceSurfaceSupportKHR(qvk.physical_device, i, qvk.surface, &present_support);

		const int supports_graphics = queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
		const int supports_compute = queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
		const int supports_transfer = queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT;

		if(supports_graphics && supports_compute && qvk.queue_idx_graphics < 0) {
			if(!present_support)
				continue;
			qvk.queue_idx_graphics = i;
			graphics_family_queue_count = queue_families[i].queueCount;
		}
		if(supports_transfer && (qvk.queue_idx_transfer < 0 || qvk.queue_idx_graphics == qvk.queue_idx_transfer)) {
			qvk.queue_idx_transfer = i;
		}
	}

	if(qvk.queue_idx_graphics < 0 || qvk.queue_idx_transfer < 0) {
		Com_Error(ERR_FATAL, "Could not find a suitable Vulkan queue family!\n");
		return false;
	}
	
	float queue_priorities = 1.0f;
	int num_create_queues = 0;
	VkDeviceQueueCreateInfo queue_create_info[3];

	/* A DEDICATED PRESENT QUEUE, when the graphics family has a second queue.

	   vkQueuePresentKHR is a queue operation and a single queue processes its
	   submissions in order, so a present issued on the graphics queue AFTER the next
	   frame's render submit cannot reach the display until that render has completed on
	   the GPU. With frame generation that put a whole rendered frame's worth of GPU work
	   between the present call and the flip and made every present of a group flip in one
	   burst - PresentMon measured display changes 0.11 ms apart with a 33 ms gap, i.e.
	   half the presented frames never visible. The present thread therefore gets a queue
	   of its own; the render submits it must not queue behind stay on queue_graphics.
	   Same queue family, so swapchain images need no ownership transfer. */
	const bool want_present_queue = graphics_family_queue_count >= 2;
	static const float graphics_queue_priorities[2] = { 1.0f, 1.0f };

	{
		VkDeviceQueueCreateInfo q = {
			.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount       = want_present_queue ? 2 : 1,
			.pQueuePriorities = want_present_queue ? graphics_queue_priorities : &queue_priorities,
			.queueFamilyIndex = qvk.queue_idx_graphics,
		};

		queue_create_info[num_create_queues++] = q;
	};
	if(qvk.queue_idx_transfer != qvk.queue_idx_graphics) {
		VkDeviceQueueCreateInfo q = {
			.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount       = 1,
			.pQueuePriorities = &queue_priorities,
			.queueFamilyIndex = qvk.queue_idx_transfer,
		};
		queue_create_info[num_create_queues++] = q;
	};

#ifdef VKPT_DEVICE_GROUPS
	if (qvk.device_count > 1) {
		features_16bit_storage.pNext = &device_group_create_info;
	}
#endif

	VkPhysicalDeviceAccelerationStructureFeaturesKHR physical_device_as_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
		.pNext = &features_16bit_storage,
		.accelerationStructure = VK_TRUE,
	};

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR physical_device_rt_pipeline_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
		.pNext = &physical_device_as_features,
		.rayTracingPipeline = VK_TRUE
	};

	VkPhysicalDeviceRayQueryFeaturesKHR physical_device_ray_query_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
		.pNext = &physical_device_as_features,
		.rayQuery = VK_TRUE
	};

	VkPhysicalDeviceVulkan12Features device_features_vk12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.descriptorIndexing = VK_TRUE,
		.shaderFloat16 = qvk.supports_fp16,
		.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
		.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
		.runtimeDescriptorArray = VK_TRUE,
		.samplerFilterMinmax = VK_TRUE,
		.bufferDeviceAddress = VK_TRUE,
		.bufferDeviceAddressMultiDevice = qvk.device_count > 1 ? VK_TRUE : VK_FALSE,
		/* vkLatencySleepNV signals a TIMELINE semaphore, so Reflex cannot work without
		   this. It is core in 1.2 and universally supported on anything that has the
		   low latency extension. */
		.timelineSemaphore = VK_TRUE,
	};

	VkPhysicalDevicePresentIdFeaturesKHR device_features_present_id = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
		.presentId = VK_TRUE,
	};
	VkPhysicalDeviceFeatures2 device_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
		.pNext = &device_features_vk12,
		.features = {
			.robustBufferAccess = VK_TRUE,
			.fullDrawIndexUint32 = VK_TRUE,
			.imageCubeArray = VK_TRUE,
			.independentBlend = VK_TRUE,
			.geometryShader = VK_FALSE,
			.tessellationShader = VK_FALSE,
			.sampleRateShading = VK_FALSE,
			.dualSrcBlend = VK_FALSE,
			.logicOp = VK_FALSE,
			.multiDrawIndirect = VK_FALSE,
			.drawIndirectFirstInstance = VK_FALSE,
			.depthClamp = VK_FALSE,
			.depthBiasClamp = VK_FALSE,
			.fillModeNonSolid = VK_FALSE,
			.depthBounds = VK_FALSE,
			.wideLines = VK_FALSE,
			.largePoints = VK_FALSE,
			.alphaToOne = VK_FALSE,
			.multiViewport = VK_FALSE,
			.samplerAnisotropy = VK_TRUE,
			.textureCompressionETC2 = VK_FALSE,
			.textureCompressionASTC_LDR = VK_FALSE,
			.textureCompressionBC = VK_FALSE,
			.occlusionQueryPrecise = VK_FALSE,
			.pipelineStatisticsQuery = VK_TRUE,
			.vertexPipelineStoresAndAtomics = VK_FALSE,
			.fragmentStoresAndAtomics = VK_FALSE,
			.shaderTessellationAndGeometryPointSize = VK_FALSE,
			.shaderImageGatherExtended = VK_FALSE,
			.shaderStorageImageExtendedFormats = VK_TRUE,
			.shaderStorageImageMultisample = VK_FALSE,
			.shaderStorageImageReadWithoutFormat = VK_FALSE,
			.shaderStorageImageWriteWithoutFormat = VK_FALSE,
			.shaderUniformBufferArrayDynamicIndexing = VK_TRUE,
			.shaderSampledImageArrayDynamicIndexing = VK_TRUE,
			.shaderStorageBufferArrayDynamicIndexing = VK_TRUE,
			.shaderStorageImageArrayDynamicIndexing = VK_TRUE,
			.shaderClipDistance = VK_FALSE,
			.shaderCullDistance = VK_FALSE,
			.shaderFloat64 = VK_FALSE,
			.shaderInt64 = VK_FALSE,
			.shaderInt16 = qvk.supports_fp16,
			.shaderResourceResidency = VK_FALSE,
			.shaderResourceMinLod = VK_FALSE,
			.sparseBinding = VK_FALSE,
			.sparseResidencyBuffer = VK_FALSE,
			.sparseResidencyImage2D = VK_FALSE,
			.sparseResidencyImage3D = VK_FALSE,
			.sparseResidency2Samples = VK_FALSE,
			.sparseResidency4Samples = VK_FALSE,
			.sparseResidency8Samples = VK_FALSE,
			.sparseResidency16Samples = VK_FALSE,
			.sparseResidencyAliased = VK_FALSE,
			.variableMultisampleRate = VK_FALSE,
			.inheritedQueries = VK_FALSE,
		}
	};
	VkDeviceCreateInfo dev_create_info = {
		.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext                   = &device_features,
		.pQueueCreateInfos       = queue_create_info,
		.queueCreateInfoCount    = num_create_queues
	};

	uint32_t max_extension_count = LENGTH(vk_requested_device_extensions_common);
	max_extension_count += max(LENGTH(vk_requested_device_extensions_ray_pipeline), LENGTH(vk_requested_device_extensions_ray_query));
	max_extension_count += LENGTH(vk_requested_device_extensions_debug);
	max_extension_count += 1; /* VK_EXT_full_screen_exclusive */
	max_extension_count += 2; /* VK_NV_low_latency2, VK_KHR_present_id */

	const char** device_extensions = alloca(sizeof(char*) * max_extension_count);
	uint32_t device_extension_count = 0;

	append_string_list(device_extensions, &device_extension_count, max_extension_count, 
		vk_requested_device_extensions_common, LENGTH(vk_requested_device_extensions_common));

	if (qvk.use_ray_query)
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_ray_query, LENGTH(vk_requested_device_extensions_ray_query));

		device_features_vk12.pNext = &physical_device_ray_query_features;
	}
	else
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_ray_pipeline, LENGTH(vk_requested_device_extensions_ray_pipeline));

		device_features_vk12.pNext = &physical_device_rt_pipeline_features;
	}
	
	if (qvk.enable_validation)
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_debug, LENGTH(vk_requested_device_extensions_debug));
	}

	// Full-screen exclusive. Without it a Vulkan swapchain on Windows is an ordinary
	// windowed one as far as the compositor is concerned, whatever SDL did with the
	// display mode, and whether it gets independent flip is entirely DWM's choice.
	if (qvk.supports_fse)
	{
		static const char* fse_ext[] = { VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME };
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			fse_ext, LENGTH(fse_ext));
	}

	// NVIDIA Reflex. Modern Reflex is a plain Vulkan extension - no SDK, no DLL, no
	// Streamline interposer. It only became reachable when the Vulkan headers moved off
	// 1.2.162, which predates the extension.
	if (qvk.supports_low_latency)
	{
		static const char* ll_ext[] = { VK_NV_LOW_LATENCY_2_EXTENSION_NAME };
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			ll_ext, LENGTH(ll_ext));
	}

	// Lets the driver tie a present back to the Reflex markers for that frame.
	if (qvk.supports_present_id)
	{
		static const char* pid_ext[] = { VK_KHR_PRESENT_ID_EXTENSION_NAME };
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			pid_ext, LENGTH(pid_ext));
	}

	/* Chain the present-id feature in only when the extension is actually enabled -
	   passing a feature struct for an unrequested extension is invalid usage. The
	   Reflex extension itself has no feature struct to enable. */
	if (qvk.supports_present_id)
	{
		device_features_present_id.pNext = (void*)device_features.pNext;
		device_features.pNext = &device_features_present_id;
	}

	dev_create_info.enabledExtensionCount = device_extension_count;
	dev_create_info.ppEnabledExtensionNames = device_extensions;

	/* create device and queue */
	result = vkCreateDevice(qvk.physical_device, &dev_create_info, NULL, &qvk.device);
	if (result != VK_SUCCESS)
	{
		Com_Error(ERR_FATAL, "Failed to create a Vulkan device.\nError code: %s", qvk_result_to_string(result));
		return false;
	}


	
	vkGetDeviceQueue(qvk.device, qvk.queue_idx_graphics, 0, &qvk.queue_graphics);
	vkGetDeviceQueue(qvk.device, qvk.queue_idx_transfer, 0, &qvk.queue_transfer);

	qvk.queue_present = qvk.queue_graphics;
	qvk.queue_present_dedicated = false;
	if (want_present_queue) {
		VkQueue present_queue = VK_NULL_HANDLE;
		vkGetDeviceQueue(qvk.device, qvk.queue_idx_graphics, 1, &present_queue);
		if (present_queue != VK_NULL_HANDLE && present_queue != qvk.queue_graphics) {
			qvk.queue_present = present_queue;
			qvk.queue_present_dedicated = true;
		}
	}
	Com_Printf("Present queue: %s\n", qvk.queue_present_dedicated
		? "dedicated (graphics family, queue 1)"
		: "shared with graphics - frame generation will flip in bursts");

#define VK_EXTENSION_DO(a) \
	q##a = (PFN_##a) vkGetDeviceProcAddr(qvk.device, #a); \
	if(!q##a) { Com_EPrintf("warning: could not load function %s\n", #a); }

#ifdef _WIN32
	if (qvk.supports_fse)
		qvkAcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT)
			vkGetDeviceProcAddr(qvk.device, "vkAcquireFullScreenExclusiveModeEXT");
#endif

	LIST_EXTENSIONS_ACCEL_STRUCT

	if (!qvk.use_ray_query)
	{
		LIST_EXTENSIONS_RAY_PIPELINE
	}

	if(qvk.enable_validation)
	{
		LIST_EXTENSIONS_DEBUG
	}

#undef VK_EXTENSION_DO

	Com_Printf("-----------------------\n");


	if (DLSSEnabled()) {
		DLSSConstructor(qvk.instance, qvk.device, qvk.physical_device, "FFA5FAF5-2329-44AB-A423-3D9B3B177C88", qtrue);
	}

	return true;
}

static VkShaderModule
create_shader_module_from_file(const char *name, const char *enum_name, bool is_rt_shader)
{
	const char* suffix = "";
	if (is_rt_shader)
	{
		if (qvk.use_ray_query)
			suffix = ".query";
		else
			suffix = ".pipeline";
	}

	char path[1024];
	snprintf(path, sizeof path, "shader_vkpt/%s%s.spv", name ? name : (enum_name + 8), suffix);
	if(!name) {
		int len = 0;
		for(len = 0; path[len]; len++)
			path[len] = tolower(path[len]);
		while(--len >= 0) {
			if(path[len] == '_') {
				path[len] = '.';
				break;
			}
		}
	}

	char *data;
	size_t size;

	size = FS_LoadFile(path, (void**)&data);
	if(!data) {
		Com_EPrintf("Couldn't find shader module %s!\n", path);
		return VK_NULL_HANDLE;
	}

	VkShaderModule module;

	VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = (uint32_t *) data,
	};

	_VK(vkCreateShaderModule(qvk.device, &create_info, NULL, &module));

	Z_Free(data);

	return module;
}

VkResult
vkpt_load_shader_modules()
{
	VkResult ret = VK_SUCCESS;
#define SHADER_MODULE_DO(a) do { \
	qvk.shader_modules[a] = create_shader_module_from_file(shader_module_file_names[a], #a, IS_RT_SHADER); \
	ret = (ret == VK_SUCCESS && qvk.shader_modules[a]) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED; \
	if(qvk.shader_modules[a]) { \
		ATTACH_LABEL_VARIABLE_NAME((uint64_t)qvk.shader_modules[a], SHADER_MODULE, #a); \
	}\
	} while(0);

#define IS_RT_SHADER false
	LIST_SHADER_MODULES;
#undef IS_RT_SHADER
#define IS_RT_SHADER true
	LIST_RT_RGEN_SHADER_MODULES
	if(!qvk.use_ray_query)
	{
		LIST_RT_PIPELINE_SHADER_MODULES
	}
#undef IS_RT_SHADER

#undef SHADER_MODULE_DO
	return ret;
}

VkResult
vkpt_destroy_shader_modules()
{
	for (int i = 0; i < NUM_QVK_SHADER_MODULES; i++)
	{
		vkDestroyShaderModule(qvk.device, qvk.shader_modules[i], NULL);
		qvk.shader_modules[i] = VK_NULL_HANDLE;
	}

	return VK_SUCCESS;
}

VkResult
destroy_swapchain(void)
{
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		vkDestroyImageView  (qvk.device, qvk.swap_chain_image_views[i], NULL);
		qvk.swap_chain_image_views[i] = VK_NULL_HANDLE;
	}
	free(qvk.swap_chain_image_views);
	qvk.swap_chain_image_views = NULL;

	free(qvk.swap_chain_images);
	qvk.swap_chain_images = NULL;

	if (qvk.semaphores_present) {
		for (int i = 0; i < qvk.num_swap_chain_images; i++)
			vkDestroySemaphore(qvk.device, qvk.semaphores_present[i], NULL);
		free(qvk.semaphores_present);
		qvk.semaphores_present = NULL;
	}

	qvk.num_swap_chain_images = 0;

	vkDestroySwapchainKHR(qvk.device, qvk.swap_chain, NULL);
	qvk.swap_chain = VK_NULL_HANDLE;

	return VK_SUCCESS;
}

int
destroy_vulkan(void)
{
	vkpt_device_wait_idle();

	destroy_swapchain();
	vkDestroySurfaceKHR(qvk.instance, qvk.surface,    NULL);

	for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
	{
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			semaphore_group_t* group = &qvk.semaphores[frame][gpu];

			vkDestroySemaphore(qvk.device, group->image_available, NULL);
			vkDestroySemaphore(qvk.device, group->render_finished, NULL);
			for (int fg = 0; fg < DLSSG_MAX_GENERATED_FRAMES; fg++)
				vkDestroySemaphore(qvk.device, group->image_available_fg[fg], NULL);
			vkDestroySemaphore(qvk.device, group->transfer_finished, NULL);
			vkDestroySemaphore(qvk.device, group->trace_finished, NULL);
		}
	}

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroyFence(qvk.device, qvk.fences_frame_sync[i], NULL);
	}

	vkpt_free_command_buffers(&qvk.cmd_buffers_graphics);
	vkpt_free_command_buffers(&qvk.cmd_buffers_transfer);

	vkDestroyCommandPool(qvk.device, qvk.cmd_buffers_graphics.command_pool, NULL);
	vkDestroyCommandPool(qvk.device, qvk.cmd_buffers_transfer.command_pool, NULL);

	vkDestroyDevice(qvk.device,   NULL);
	_VK(qvkDestroyDebugUtilsMessengerEXT(qvk.instance, qvk.dbg_messenger, NULL));
	vkDestroyInstance(qvk.instance, NULL);

	free(qvk.extensions);
	qvk.extensions = NULL;
	qvk.num_extensions = 0;

	free(qvk.layers);
	qvk.layers = NULL;
	qvk.num_layers = 0;

	// Clear the extension function pointers to make sure they don't refer non-requested extensions after vid_restart
#define VK_EXTENSION_DO(a) q##a = NULL;
	LIST_EXTENSIONS_ACCEL_STRUCT
	LIST_EXTENSIONS_RAY_PIPELINE
	LIST_EXTENSIONS_DEBUG
	LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

	return 0;
}



static int entity_frame_num = 0;
static uint32_t model_entity_ids[2][MAX_MODEL_INSTANCES];
static int model_entity_id_count[2];
static int light_entity_ids[2][MAX_MODEL_LIGHTS];
static int light_entity_id_count[2];
static int iqm_matrix_count[2];
static ModelInstance model_instances_prev[MAX_MODEL_INSTANCES];

static int num_model_lights = 0;
static light_poly_t model_lights[MAX_MODEL_LIGHTS];

static pbr_material_t const * get_mesh_material(const entity_t* entity, const maliasmesh_t* mesh)
{
	if (entity->skin)
	{
		return MAT_ForSkin(IMG_ForHandle(entity->skin));
	}

	int skinnum = 0;
	if (mesh->materials[entity->skinnum])
		skinnum = entity->skinnum;

	return mesh->materials[skinnum];
}

static uint32_t compute_mesh_material_flags(const entity_t* entity, const model_t* model,
	const maliasmesh_t* mesh, bool is_viewer_weapon, bool is_double_sided, float alpha)
{
	pbr_material_t const* material = get_mesh_material(entity, mesh);

	if (!material)
	{
		Com_EPrintf("Cannot find material for model '%s'\n", model->name);
		return 0;
	}

	uint32_t material_id = material->flags;

	if (MAT_IsKind(material_id, MATERIAL_KIND_INVISIBLE))
		return 0; // skip the mesh

	if (MAT_IsKind(material_id, MATERIAL_KIND_CHROME))
		material_id = MAT_SetKind(material_id, MATERIAL_KIND_CHROME_MODEL);

	if (MAT_IsKind(material_id, MATERIAL_KIND_TRANSPARENT) || (MAT_IsKind(material_id, MATERIAL_KIND_REGULAR) && (alpha < 1.0f)))
		material_id = MAT_SetKind(material_id, MATERIAL_KIND_TRANSP_MODEL);

	if (model->model_class == MCLASS_EXPLOSION)
	{
		material_id = MAT_SetKind(material_id, MATERIAL_KIND_EXPLOSION);
		material_id |= MATERIAL_FLAG_LIGHT;
	}

	if (is_viewer_weapon)
		material_id |= MATERIAL_FLAG_WEAPON;

	if (is_double_sided)
		material_id |= MATERIAL_FLAG_DOUBLE_SIDED;

	if (!MAT_IsKind(material_id, MATERIAL_KIND_GLASS))
	{
		if (entity->flags & RF_SHELL_RED)
			material_id |= MATERIAL_FLAG_SHELL_RED;
		if (entity->flags & RF_SHELL_GREEN)
			material_id |= MATERIAL_FLAG_SHELL_GREEN;
		if (entity->flags & RF_SHELL_BLUE)
			material_id |= MATERIAL_FLAG_SHELL_BLUE;
	}

	if (mesh->handedness)
		material_id |= MATERIAL_FLAG_HANDEDNESS;

	return material_id;
}

static void fill_model_instance(ModelInstance* instance, const entity_t* entity, const model_t* model, const maliasmesh_t* mesh,
	const float* transform, uint32_t material_id, int instance_index, int iqm_matrix_index)
{
	int cluster = -1;
	if (bsp_world_model)
		cluster = BSP_PointLeaf(bsp_world_model->nodes, entity->origin)->cluster;
	
	int frame = entity->frame;
	int oldframe = entity->oldframe;
	if (frame >= model->numframes) frame = 0;
	if (oldframe >= model->numframes) oldframe = 0;

	memcpy(instance->transform, transform, sizeof(float) * 16);
	memcpy(instance->transform_prev, transform, sizeof(float) * 16);
	instance->material = material_id;
	instance->cluster = cluster;
	instance->source_buffer_idx = (int)(model - r_models) + VERTEX_BUFFER_FIRST_MODEL;
	instance->prim_count = mesh->numtris;
	instance->prim_offset_curr_pose_curr_frame = mesh->tri_offset + frame * mesh->numtris;
	instance->prim_offset_prev_pose_curr_frame = mesh->tri_offset + oldframe * mesh->numtris;
	instance->prim_offset_curr_pose_prev_frame = instance->prim_offset_curr_pose_curr_frame;
	instance->prim_offset_prev_pose_prev_frame = instance->prim_offset_prev_pose_curr_frame;
	instance->pose_lerp_curr_frame = entity->backlerp;
	instance->pose_lerp_prev_frame = instance->pose_lerp_curr_frame;
	instance->iqm_matrix_offset_curr_frame = iqm_matrix_index;
	instance->iqm_matrix_offset_prev_frame = instance->iqm_matrix_offset_curr_frame;
	instance->frame = 0;
	instance->alpha = (entity->flags & RF_TRANSLUCENT) ? entity->alpha : 1.0f;
	instance->render_buffer_idx = 0; // to be filled later
	instance->render_prim_offset = 0;

	// If this is a static wall light model, the renderer creates a custom set of light polys
	// for this model. Mark the material with the light flag to avoid double contribution and noise
	// from the GI rays. This (together with the custom lights) is a hack that should be replaced
	// by a better model that has a separate mesh for the light rod.
	if (model->model_class == MCLASS_STATIC_LIGHT)
		instance->material |= MATERIAL_FLAG_LIGHT;
}

static void
add_dlights(const dlight_t* dlights, int num_dlights, light_poly_t* light_list, int* num_lights, int max_lights, bsp_t* bsp, int* light_entity_ids)
{
	for (int i = 0; i < num_dlights; i++)
	{
		if (*num_lights >= max_lights)
			return;

		const dlight_t* dlight = dlights + i;
		light_poly_t* light = light_list + *num_lights;

		light->cluster = BSP_PointLeaf(bsp->nodes, dlight->origin)->cluster;

		entity_hash_t hash;
		hash.entity = i + 1; //entity ID
		hash.mesh = 0xAA;

		if (light->cluster >= 0)
		{
			//Super wasteful but we want to have all lights in the same list.

			VectorCopy(dlight->origin, light->positions + 0);
			VectorScale(dlight->color, dlight->intensity / 25.f, light->color);
			light->positions[3] = dlight->radius;
			light->material = NULL;
			light->style = 0;

			switch (dlight->light_type) {
			case DLIGHT_SPHERE:
				light->type = DYNLIGHT_SPHERE;
				hash.model = 0xFE;
				break;
			case DLIGHT_SPOT:
				light->type = DYNLIGHT_SPOT;
				// Copy spot data
				VectorCopy(dlight->spot.direction, light->positions + 6);
				// The two emission profiles overlap in a union in dlight_t, so the
				// cos_* fields hold garbage for a texture-profile light and must
				// never be read for one. positions[4]/[5] mean whatever the profile
				// recorded in spot_emission_profile says they mean; the shader reads
				// them back the same way in spotlight_falloff().
				light->spot_emission_profile = dlight->spot.emission_profile;
				switch (dlight->spot.emission_profile) {
				case DLIGHT_SPOT_EMISSION_PROFILE_FALLOFF:
					light->positions[4] = dlight->spot.cos_total_width;
					light->positions[5] = dlight->spot.cos_falloff_start;
					break;
				case DLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE:
					// cone half-angle in radians, and the 1D profile texture index
					light->positions[4] = dlight->spot.total_width;
					light->positions[5] = (float)dlight->spot.texture;
					break;
				}
				hash.model = 0xFD;
				break;
			}

			light_entity_ids[(*num_lights)] = *(uint32_t*)&hash;
			(*num_lights)++;

		}
	}
}

static inline void transform_point(const float* p, const float* matrix, float* result)
{
	vec4_t point = { p[0], p[1], p[2], 1.f };
	vec4_t transformed;
	mult_matrix_vector(transformed, matrix, point);
	VectorCopy(transformed, result); // vec4 -> vec3
}

static void instance_model_lights(int num_light_polys, const light_poly_t* light_polys, const float* transform, entity_hash_t hash)
{
	for (int nlight = 0; nlight < num_light_polys; nlight++)
	{
		if (num_model_lights >= MAX_MODEL_LIGHTS)
		{
			assert(!"Model light count overflow");
			break;
		}

		const light_poly_t* src_light = light_polys + nlight;
		light_poly_t* dst_light = model_lights + num_model_lights;

		// Transform the light's positions and center
		transform_point(src_light->positions + 0, transform, dst_light->positions + 0);
		transform_point(src_light->positions + 3, transform, dst_light->positions + 3);
		transform_point(src_light->positions + 6, transform, dst_light->positions + 6);
		transform_point(src_light->off_center, transform, dst_light->off_center);

		// Find the cluster based on the center. Maybe it's OK to use the model's cluster, need to test.
		dst_light->cluster = BSP_PointLeaf(bsp_world_model->nodes, dst_light->off_center)->cluster;

		// We really need to map these lights to a cluster
		if (dst_light->cluster < 0)
			continue;

		// Copy the other light properties
		VectorCopy(src_light->color, dst_light->color);
		dst_light->material = src_light->material;
		dst_light->style = src_light->style;
		dst_light->type = DYNLIGHT_POLYGON;

		hash.mesh = nlight; //More a light index than a mesh
		light_entity_ids[entity_frame_num][num_model_lights] = *(uint32_t*)&hash;

		num_model_lights++;
	}
}
static const mat4 g_identity_transform = {
	{ 1.f, 0.f, 0.f, 0.f },
	{ 0.f, 1.f, 0.f, 0.f },
	{ 0.f, 0.f, 1.f, 0.f },
	{ 0.f, 0.f, 0.f, 1.f }
};

static void process_bsp_entity(const entity_t* entity, int* instance_count)
{
	InstanceBuffer* uniform_instance_buffer = &vkpt_refdef.uniform_instance_buffer;

	const int current_instance_idx = *instance_count;
	if (current_instance_idx >= MAX_MODEL_INSTANCES)
	{
		assert(!"Entity count overflow");
		return;
	}
	
	float transform[16];
	create_entity_matrix(transform, (entity_t*)entity, false);

	bsp_model_t* model = vkpt_refdef.bsp_mesh_world.models + (~entity->model);

	vec3_t origin;
	
	transform_point(model->center, transform, origin);
	int cluster = BSP_PointLeaf(bsp_world_model->nodes, origin)->cluster;

	if (cluster < 0)
	{
		// In some cases, a model slides into a wall, like a push button, so that its center 
		// is no longer in any BSP node. We still need to assign a cluster to the model,
		// so try the corners of the model instead, see if any of them has a valid cluster.

		for (int corner = 0; corner < 8; corner++)
		{
			vec3_t corner_pt = {
				(corner & 1) ? model->aabb_max[0] : model->aabb_min[0],
				(corner & 2) ? model->aabb_max[1] : model->aabb_min[1],
				(corner & 4) ? model->aabb_max[2] : model->aabb_min[2]
			};

			vec3_t corner_pt_world;
			transform_point(corner_pt, transform, corner_pt_world);

			cluster = BSP_PointLeaf(bsp_world_model->nodes, corner_pt_world)->cluster;

			if (cluster >= 0)
				break;
		}
	}

	entity_hash_t hash;
	hash.entity = entity->id;
	hash.model = ~entity->model;
	hash.mesh = 0;
	hash.bsp = 1;

	memcpy(&model_entity_ids[entity_frame_num][current_instance_idx], &hash, sizeof(uint32_t));

	ModelInstance* mi = uniform_instance_buffer->model_instances + current_instance_idx;
	memcpy(&mi->transform, transform, sizeof(transform));
	memcpy(&mi->transform_prev, transform, sizeof(transform));
	mi->material = 0;
	mi->cluster = cluster;
	mi->source_buffer_idx = VERTEX_BUFFER_WORLD;
	mi->prim_count = model->geometry.prim_counts[0];
	mi->prim_offset_curr_pose_curr_frame = 0; // bsp models are not processed by the instancing shader
	mi->prim_offset_prev_pose_curr_frame = 0;
	mi->prim_offset_curr_pose_prev_frame = 0;
	mi->prim_offset_prev_pose_prev_frame = 0;
	mi->pose_lerp_curr_frame = 0.f;
	mi->pose_lerp_prev_frame = 0.f;
	mi->iqm_matrix_offset_curr_frame = -1;
	mi->iqm_matrix_offset_prev_frame = -1;
	mi->frame = entity->frame;
	mi->alpha = (entity->flags & RF_TRANSLUCENT) ? entity->alpha : 1.f;
	mi->render_buffer_idx = VERTEX_BUFFER_WORLD;
	mi->render_prim_offset = model->geometry.prim_offsets[0];
	
	instance_model_lights(model->num_light_polys, model->light_polys, transform, hash);

	if (model->geometry.accel)
	{
		vkpt_pt_instance_model_blas(&model->geometry, mi->transform, VERTEX_BUFFER_WORLD, current_instance_idx, (mi->alpha < 1.f) ? AS_FLAG_TRANSPARENT : 0);
	}

	if (!model->transparent)
	{
		vkpt_shadow_map_add_instance(transform, qvk.buf_world.buffer, vkpt_refdef.bsp_mesh_world.vertex_data_offset
			+ mi->render_prim_offset * sizeof(prim_positions_t), mi->prim_count);
	}

	(*instance_count)++;
}

#define MESH_FILTER_TRANSPARENT 1
#define MESH_FILTER_OPAQUE 2
#define MESH_FILTER_MASKED 4
#define MESH_FILTER_ALL 7

static void process_regular_entity(
	const entity_t* entity, 
	const model_t* model, 
	bool is_viewer_weapon, 
	bool is_double_sided, 
	int* instance_count, 
	int* animated_count, 
	int* num_instanced_prim, 
	int mesh_filter, 
	bool* contains_transparent,
	bool* contains_masked,
	int* iqm_matrix_offset,
	float* iqm_matrix_data)
{
	InstanceBuffer* uniform_instance_buffer = &vkpt_refdef.uniform_instance_buffer;

	float transform[16];
	create_entity_matrix(transform, (entity_t*)entity, is_viewer_weapon);
	
	int current_instance_index = *instance_count;
	int current_animated_index = *animated_count;
	int current_num_instanced_prim = *num_instanced_prim;

	if (contains_transparent)
		*contains_transparent = false;

	int iqm_matrix_index = -1;
	if (model->iqmData && model->iqmData->num_poses)
	{
		iqm_matrix_index = *iqm_matrix_offset;
		
		if (iqm_matrix_index + model->iqmData->num_poses > MAX_IQM_MATRICES)
		{
			assert(!"IQM matrix buffer overflow");
			return;
		}
		
		R_ComputeIQMTransforms(model->iqmData, entity, iqm_matrix_data + (iqm_matrix_index * 12));
		
		*iqm_matrix_offset += (int)model->iqmData->num_poses;
	}

	float alpha = (entity->flags & RF_TRANSLUCENT) ? entity->alpha : 1.f;

	bool use_static_blas = vkpt_model_is_static(model) && (mesh_filter != MESH_FILTER_ALL);

	const model_vbo_t* vbo = vkpt_get_model_vbo(model);

	if (use_static_blas)
	{
		const model_geometry_t* geom = NULL;

		if (mesh_filter & MESH_FILTER_MASKED)
			geom = &vbo->geom_masked;
		else if (mesh_filter & MESH_FILTER_TRANSPARENT)
			geom = &vbo->geom_transparent;
		else
			geom = &vbo->geom_opaque;
		
		if (geom->accel)
		{
			// ugly typecast
			mat4 transform_;
			memcpy(transform_, transform, sizeof(mat4));

			uint32_t model_index = (uint32_t)(model - r_models);

			vkpt_pt_instance_model_blas(geom, transform_, VERTEX_BUFFER_FIRST_MODEL + model_index, current_instance_index, (alpha < 1.f) ? AS_FLAG_TRANSPARENT : 0);
		}
	}

	for (int i = 0; i < model->nummeshes; i++)
	{
		const maliasmesh_t* mesh = model->meshes + i;

		if (current_instance_index >= MAX_MODEL_INSTANCES)
		{
			assert(!"Model instance count overflow");
			break;
		}

		if (!use_static_blas && current_animated_index >= MAX_MODEL_INSTANCES)
		{
			assert(!"Animated model count overflow");
			break;
		}

		if (mesh->tri_offset < 0)
		{
			// failed to upload the vertex data - don't instance this mesh
			continue;
		}

		uint32_t material_id = compute_mesh_material_flags(entity, model, mesh, is_viewer_weapon, is_double_sided, alpha);

		if (!material_id)
			continue;

		if (MAT_IsMasked(material_id))
		{
			if (contains_masked)
				*contains_masked = true;

			if (!(mesh_filter & MESH_FILTER_MASKED))
				continue;
		}
		else if (MAT_IsTransparent(material_id) || (alpha < 1.0f))
		{
			if(contains_transparent)
				*contains_transparent = true;

			if(!(mesh_filter & MESH_FILTER_TRANSPARENT))
				continue;
		}
		else
		{
			if (!(mesh_filter & MESH_FILTER_OPAQUE))
				continue;
		}

		entity_hash_t hash;
		hash.entity = entity->id;
		hash.model = entity->model;
		hash.mesh = i;
		hash.bsp = 0;

		memcpy(&model_entity_ids[entity_frame_num][current_instance_index], &hash, sizeof(uint32_t));
		
		ModelInstance* mi = uniform_instance_buffer->model_instances + current_instance_index;

		fill_model_instance(mi, entity, model, mesh, transform, material_id,
			current_instance_index, iqm_matrix_index);

		if (use_static_blas)
		{
			mi->render_buffer_idx = mi->source_buffer_idx;
			mi->render_prim_offset = mi->prim_offset_curr_pose_curr_frame;

			if (!MAT_IsTransparent(material_id))
			{
				vkpt_shadow_map_add_instance(transform, vbo->buffer.buffer, vbo->vertex_data_offset
					+ mi->render_prim_offset * sizeof(prim_positions_t), mi->prim_count);
			}
		}
		else
		{
			uniform_instance_buffer->animated_model_indices[current_animated_index] = current_instance_index;

			mi->render_buffer_idx = VERTEX_BUFFER_INSTANCED;
			mi->render_prim_offset = current_num_instanced_prim;

			current_animated_index++;
			current_num_instanced_prim += mesh->numtris;
		}

		current_instance_index++;
	}

	// add cylinder lights for wall lamps
	if (model->model_class == MCLASS_STATIC_LIGHT)
	{
		vec4_t begin, end, color;
		vec4_t offset1 = { 0.f, 0.5f, -10.f, 1.f };
		vec4_t offset2 = { 0.f, 0.5f,  10.f, 1.f };

		mult_matrix_vector(begin, transform, offset1);
		mult_matrix_vector(end, transform, offset2);
		VectorSet(color, 0.25f, 0.5f, 0.07f);

		entity_hash_t hash;
		hash.entity = entity->id;
		hash.model = entity->model;
		hash.mesh = 0;
		hash.bsp = 0;

		vkpt_build_cylinder_light(model_lights, &num_model_lights, MAX_MODEL_LIGHTS, bsp_world_model, begin, end, color, 1.5f, hash, light_entity_ids[entity_frame_num]);
	}

	*instance_count = current_instance_index;
	*animated_count = current_animated_index;
	*num_instanced_prim = current_num_instanced_prim;
}

static void
prepare_entities(EntityUploadInfo* upload_info)
{
	entity_frame_num = !entity_frame_num;

	InstanceBuffer* instance_buffer = &vkpt_refdef.uniform_instance_buffer;
	
	static int transparent_model_indices[MAX_ENTITIES];
	static int masked_model_indices[MAX_ENTITIES];
	static int viewer_model_indices[MAX_ENTITIES];
	static int viewer_weapon_indices[MAX_ENTITIES];
	static int explosion_indices[MAX_ENTITIES];
	int transparent_model_num = 0;
	int masked_model_num = 0;
	int viewer_model_num = 0;
	int viewer_weapon_num = 0;
	int explosion_num = 0;

	int model_instance_idx = 0;
	int num_instanced_prim = 0; /* need to track this here to find lights */
	int instance_idx = 0;
	int iqm_matrix_offset = 0;

	const bool first_person_model = (cl_player_model->integer == CL_PLAYER_MODEL_FIRST_PERSON) && cl.baseclientinfo.model;

	for (int i = 0; i < vkpt_refdef.fd->num_entities; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + i;

		if (entity->model & 0x80000000)
		{
			process_bsp_entity(entity, &model_instance_idx); /* embedded in bsp */
		}
		else
		{
			const model_t* model = MOD_ForHandle(entity->model);
			if (model == NULL || model->meshes == NULL)
				continue;

			if (entity->flags & RF_VIEWERMODEL)
				viewer_model_indices[viewer_model_num++] = i;
			else if (entity->flags & RF_WEAPONMODEL)
				viewer_weapon_indices[viewer_weapon_num++] = i;
			else if (model->model_class == MCLASS_EXPLOSION || model->model_class == MCLASS_FLASH)
				explosion_indices[explosion_num++] = i;
			else
			{
				bool contains_transparent = false;
				bool contains_masked = false;
				process_regular_entity(entity, model, false, false, &model_instance_idx, &instance_idx, &num_instanced_prim,
					MESH_FILTER_OPAQUE, &contains_transparent, &contains_masked, &iqm_matrix_offset, qvk.iqm_matrices_shadow);

				if (contains_transparent)
					transparent_model_indices[transparent_model_num++] = i;
				if (contains_masked)
					masked_model_indices[masked_model_num++] = i;
			}

			if (model->num_light_polys > 0)
			{
				float transform[16];
				const bool is_viewer_weapon = (entity->flags & RF_WEAPONMODEL) != 0;
				create_entity_matrix(transform, (entity_t*)entity, is_viewer_weapon);

				entity_hash_t hash;
				hash.entity = i + 1;
				hash.model = ~entity->model;
				hash.mesh = 0;
				hash.bsp = 0;

				instance_model_lights(model->num_light_polys, model->light_polys, transform, hash);
			}
		}
	}

	upload_info->opaque_prim_count = num_instanced_prim;
	upload_info->transparent_prim_offset = num_instanced_prim;
	
	for (int i = 0; i < transparent_model_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + transparent_model_indices[i];

		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, false, false, &model_instance_idx, &instance_idx, &num_instanced_prim,
			MESH_FILTER_TRANSPARENT, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
	}

	upload_info->transparent_prim_count = num_instanced_prim - upload_info->transparent_prim_offset;
	upload_info->masked_prim_offset = num_instanced_prim;

	for (int i = 0; i < masked_model_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + masked_model_indices[i];
		
		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, false, true, &model_instance_idx, &instance_idx, &num_instanced_prim,
			MESH_FILTER_MASKED, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
	}

	upload_info->masked_prim_count = num_instanced_prim - upload_info->masked_prim_offset;
	upload_info->viewer_model_prim_offset = num_instanced_prim;
	
	if (first_person_model)
	{
		for (int i = 0; i < viewer_model_num; i++)
		{
			const entity_t* entity = vkpt_refdef.fd->entities + viewer_model_indices[i];
			const model_t* model = MOD_ForHandle(entity->model);
			process_regular_entity(entity, model, false, true, &model_instance_idx, &instance_idx, &num_instanced_prim,
				MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
		}
	}

	upload_info->viewer_model_prim_count = num_instanced_prim - upload_info->viewer_model_prim_offset;
	upload_info->viewer_weapon_prim_offset = num_instanced_prim;

	upload_info->weapon_left_handed = false;
	
	for (int i = 0; i < viewer_weapon_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + viewer_weapon_indices[i];
		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, true, false, &model_instance_idx, &instance_idx, &num_instanced_prim,
			MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);

		if (entity->flags & RF_LEFTHAND)
			upload_info->weapon_left_handed = true;
	}

	upload_info->viewer_weapon_prim_count = num_instanced_prim - upload_info->viewer_weapon_prim_offset;
	upload_info->explosions_prim_offset = num_instanced_prim;
	
	for (int i = 0; i < explosion_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + explosion_indices[i];
		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, false, false, &model_instance_idx, &instance_idx, &num_instanced_prim,
			MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
	}

	upload_info->explosions_prim_count = num_instanced_prim - upload_info->explosions_prim_offset;

	upload_info->num_instances = instance_idx;
	upload_info->num_prims  = num_instanced_prim;
	
	memset(instance_buffer->model_current_to_prev, -1, sizeof(instance_buffer->model_current_to_prev));
	memset(instance_buffer->model_prev_to_current, -1, sizeof(instance_buffer->model_prev_to_current));
	memset(instance_buffer->mlight_prev_to_current, ~0u, sizeof(instance_buffer->mlight_prev_to_current));
	
	model_entity_id_count[entity_frame_num] = model_instance_idx;
	for(int i = 0; i < model_entity_id_count[entity_frame_num]; i++) {
		for(int j = 0; j < model_entity_id_count[!entity_frame_num]; j++) {
			entity_hash_t hash;
			memcpy(&hash, &model_entity_ids[entity_frame_num][i], sizeof(entity_hash_t));

			if(model_entity_ids[entity_frame_num][i] == model_entity_ids[!entity_frame_num][j] && hash.entity != 0u) {
				instance_buffer->model_current_to_prev[i] = j;
				instance_buffer->model_prev_to_current[j] = i;

				// Copy the "prev" instance paramters from the previous frame's instance buffer
				ModelInstance* mi_curr = instance_buffer->model_instances + i;
				ModelInstance* mi_prev = model_instances_prev + j;

				memcpy(mi_curr->transform_prev, mi_prev->transform, sizeof(mi_curr->transform_prev));
				mi_curr->prim_offset_curr_pose_prev_frame = mi_prev->prim_offset_curr_pose_curr_frame;
				mi_curr->prim_offset_prev_pose_prev_frame = mi_prev->prim_offset_prev_pose_curr_frame;
				mi_curr->pose_lerp_prev_frame = mi_prev->pose_lerp_curr_frame;
				mi_curr->iqm_matrix_offset_prev_frame = mi_prev->iqm_matrix_offset_curr_frame;
			}
		}
	}

	// Store the number of IQM matrices for the next frame
	iqm_matrix_count[entity_frame_num] = iqm_matrix_offset;

	if (iqm_matrix_count[entity_frame_num] > 0)
	{
		// If we had some matrices previously...
		if (iqm_matrix_count[!entity_frame_num] > 0)
		{
			// Copy over the previous frame IQM matrices into an offset location in the current frame buffer
			memcpy(qvk.iqm_matrices_shadow + (iqm_matrix_count[entity_frame_num] * 12),
				qvk.iqm_matrices_prev, iqm_matrix_count[!entity_frame_num] * 12 * sizeof(float));

			// Patch the previous matrix offsets to point at the new locations
			for (int i = 0; i < model_entity_id_count[entity_frame_num]; i++)
			{
				ModelInstance* instance = &instance_buffer->model_instances[i];
				if (instance->iqm_matrix_offset_prev_frame >= 0) {
					// Offset = current matrix count
					instance->iqm_matrix_offset_prev_frame += iqm_matrix_count[entity_frame_num];
				}
			}
		}

		// Store the current matrices for the next frame
		memcpy(qvk.iqm_matrices_prev, qvk.iqm_matrices_shadow, iqm_matrix_count[entity_frame_num] * 12 * sizeof(float));

		// Upload the current matrices to the staging buffer
		IqmMatrixBuffer* iqm_matrix_staging = buffer_map(&qvk.buf_iqm_matrices_staging[qvk.current_frame_index]);

		int total_matrix_count = (iqm_matrix_count[entity_frame_num] + iqm_matrix_count[!entity_frame_num]);
		memcpy(iqm_matrix_staging, qvk.iqm_matrices_shadow, total_matrix_count * 12 * sizeof(float));

		buffer_unmap(&qvk.buf_iqm_matrices_staging[qvk.current_frame_index]);
	}

	// Save the current model instances for the next frame
	memcpy(model_instances_prev, instance_buffer->model_instances, sizeof(ModelInstance) * model_entity_id_count[entity_frame_num]);
}

#ifdef VKPT_IMAGE_DUMPS
static void 
copy_to_dump_texture(VkCommandBuffer cmd_buf, int src_image_index)
{
	VkImage src_image = qvk.images[src_image_index];
	VkImage dst_image = qvk.dump_image;

	VkImageCopy image_copy_region = {
		.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.srcSubresource.mipLevel = 0,
		.srcSubresource.baseArrayLayer = 0,
		.srcSubresource.layerCount = 1,

		.srcOffset.x = 0,
		.srcOffset.y = 0,
		.srcOffset.z = 0,

		.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.dstSubresource.mipLevel = 0,
		.dstSubresource.baseArrayLayer = 0,
		.dstSubresource.layerCount = 1,

		.dstOffset.x = 0,
		.dstOffset.y = 0,
		.dstOffset.z = 0,

		.extent.width = IMG_WIDTH,
		.extent.height = IMG_HEIGHT,
		.extent.depth = 1
	};

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image = src_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = dst_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_HOST_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	vkCmdCopyImage(cmd_buf,
		src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &image_copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image = src_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = dst_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);
}
#endif

VkDescriptorSet qvk_get_current_desc_set_textures()
{
	return (qvk.frame_counter & 1) ? qvk.desc_set_textures_odd : qvk.desc_set_textures_even;
}

static void
process_render_feedback(ref_feedback_t *feedback, mleaf_t* viewleaf, bool* sun_visible, float* adapted_luminance)
{
	if (viewleaf)
		feedback->viewcluster = viewleaf->cluster;
	else
		feedback->viewcluster = -1;

	{
		static char const * unknown = "<unknown>";
		char const * view_material = unknown;
		char const * view_material_override = unknown;
		ReadbackBuffer readback;
		vkpt_readback(&readback);
		if (readback.material != ~0u)
		{
			int material_id = readback.material & MATERIAL_INDEX_MASK;
			feedback->view_material_index = material_id;
			pbr_material_t const* material = MAT_ForIndex(material_id);
			if (material)
			{
				image_t const* image = material->image_base;
				if (image)
				{
					view_material = image->name;
					view_material_override = image->filepath;
				}
			}
		}
		else
			feedback->view_material_index = -1;
		strcpy(feedback->view_material, view_material);
		strcpy(feedback->view_material_override, view_material_override);

		feedback->lookatcluster = readback.cluster;
		feedback->num_light_polys = 0;

		if (vkpt_refdef.bsp_mesh_world_loaded && feedback->lookatcluster >= 0 && feedback->lookatcluster < vkpt_refdef.bsp_mesh_world.num_clusters)
		{
			int* light_offsets = vkpt_refdef.bsp_mesh_world.cluster_light_offsets + feedback->lookatcluster;
			feedback->num_light_polys = light_offsets[1] - light_offsets[0];
		}

		VectorCopy(readback.hdr_color, feedback->hdr_color);
		feedback->adapted_luminance = readback.adapted_luminance;

		*sun_visible = readback.sun_luminance > 0.f;
		*adapted_luminance = readback.adapted_luminance;
	}
}

typedef struct reference_mode_s 
{
	bool enable_accumulation;
	bool enable_denoiser;
	// DLSS Ray Reconstruction is doing the denoising, so A-SVGF must be bypassed
	// (enable_denoiser is forced false) but the engine should still behave as though a
	// denoiser is present for the material / sampling tunings that care.
	bool rr_denoiser;
	float num_bounce_rays;
	float temporal_blend_factor;
	int reflect_refract;
} reference_mode_t;

static int
get_accumulation_rendering_framenum(void)
{
	return max(128, cvar_pt_accumulation_rendering_framenum->integer);
}

static bool is_accumulation_rendering_active(void)
{
	return cl_paused->integer == 2 && sv_paused->integer && cvar_pt_accumulation_rendering->integer > 0;
}

static void draw_shadowed_string(int x, int y, int flags, size_t maxlen, const char* s)
{
	R_SetColor(0xff000000u);
	SCR_DrawStringEx(x + 1, y + 1, flags, maxlen, s, SCR_GetFont());
	R_SetColor(~0u);
	SCR_DrawStringEx(x, y, flags, maxlen, s, SCR_GetFont());
}

static void
evaluate_reference_mode(reference_mode_t* ref_mode)
{
	if (is_accumulation_rendering_active())
	{
		num_accumulated_frames++;

		const int num_warmup_frames = 5;
		const int num_frames_to_accumulate = get_accumulation_rendering_framenum();

		ref_mode->enable_accumulation = true;
		ref_mode->enable_denoiser = false;
		ref_mode->rr_denoiser = false;
		ref_mode->num_bounce_rays = 2;
		ref_mode->temporal_blend_factor = 1.f / min(max(1, num_accumulated_frames - num_warmup_frames), num_frames_to_accumulate);
		ref_mode->reflect_refract = max(4, cvar_pt_reflect_refract->integer);

		switch (cvar_pt_accumulation_rendering->integer)
		{
		case 1: {
			char text[MAX_QPATH];
			float percentage = powf(max(0.f, (num_accumulated_frames - num_warmup_frames) / (float)num_frames_to_accumulate), 0.5f);
			Q_snprintf(text, sizeof(text), "Photo mode: accumulating samples... %d%%", (int)(min(1.f, percentage) * 100.f));

			int frames_after_accumulation_finished = num_accumulated_frames - num_warmup_frames - num_frames_to_accumulate;
			float hud_alpha = max(0.f, min(1.f, (50 - frames_after_accumulation_finished) * 0.02f)); // fade out for 50 frames after accumulation finishes

			int x = r_config.width / 4;
			int y = 30;
			R_SetScale(0.5f);
			R_SetAlphaScale(hud_alpha);
			draw_shadowed_string(x, y, UI_CENTER, MAX_QPATH, text);

			if (cvar_pt_dof->integer)
			{
				x = 5;
				y = r_config.height / 2 - 55;
				Q_snprintf(text, sizeof(text), "Focal Distance: %.1f", cvar_pt_focus->value);
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, text);

				y += 10;
				Q_snprintf(text, sizeof(text), "Aperture: %.2f", cvar_pt_aperture->value);
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, text);

				y += 10;
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, "Use Mouse Wheel, Shift, Ctrl to adjust");
			}

			R_SetAlphaScale(1.f);

			SCR_SetHudAlpha(hud_alpha);
			break;
		}
		case 2:
			SCR_SetHudAlpha(0.f);
			break;
		}
	}
	else
	{
		num_accumulated_frames = 0;

		ref_mode->enable_accumulation = false;
		ref_mode->enable_denoiser = !!cvar_flt_enable->integer;

		// DLSS Ray Reconstruction replaces the denoiser - it must be fed the raw noisy
		// path-traced signal. Running A-SVGF first violates RR's core assumption of
		// independent samples (RR guide 3.5: "RR assumes independent samples") because
		// A-SVGF accumulates temporally, and stacking two denoisers destroys the noise
		// statistics RR was trained on.
		//
		// enable_denoiser == false is the engine's existing, self-consistent "no Q2RTX
		// denoiser" configuration: it makes get_is_gradient() stop reading A-SVGF's
		// gradient image, and keeps specular demodulation paired up between
		// direct_lighting.rgen and compositing.comp.
		ref_mode->rr_denoiser = DLSSBypassDenoiser() ? true : false;
		if (ref_mode->rr_denoiser)
			ref_mode->enable_denoiser = false;
		if (cvar_pt_num_bounce_rays->value == 0.5f)
			ref_mode->num_bounce_rays = 0.5f;
		else
			ref_mode->num_bounce_rays = max(0, min(2, round(cvar_pt_num_bounce_rays->value)));
		ref_mode->temporal_blend_factor = 0.f;
		ref_mode->reflect_refract = max(0, cvar_pt_reflect_refract->integer);
	}

	ref_mode->reflect_refract = min(10, ref_mode->reflect_refract);
}

static void
evaluate_taa_settings(const reference_mode_t* ref_mode)
{
	qvk.effective_aa_mode = AA_MODE_OFF;
	qvk.extent_taa_output = qvk.extent_render;

	if (!ref_mode->enable_denoiser)
		return;

	int flt_taa = cvar_flt_taa->integer;
	// FSR RCAS needs upscaled input; if EASU was disabled, force to TAAU
	bool force_upscaling = vkpt_fsr_is_enabled() && vkpt_fsr_needs_upscale();
	qboolean dlssEnabled = DLSSEnabled();

	if(force_upscaling && !dlssEnabled)
	{
		flt_taa = AA_MODE_UPSCALE;
	}
	
	if (dlssEnabled) {
		flt_taa = AA_MODE_OFF;
	}

	if (flt_taa == AA_MODE_TAA)
	{
		qvk.effective_aa_mode = AA_MODE_TAA;
	}
	else if (flt_taa == AA_MODE_UPSCALE) // TAAU or TAA+FSR
	{
		if (qvk.extent_render.width > qvk.extent_unscaled.width || qvk.extent_render.height > qvk.extent_unscaled.height)
		{
			qvk.effective_aa_mode = AA_MODE_TAA;
		}
		else
		{
			qvk.effective_aa_mode = AA_MODE_UPSCALE;
			if (!vkpt_fsr_is_enabled() || force_upscaling)
				qvk.extent_taa_output = qvk.extent_unscaled;
		}
	}
}

static void
prepare_sky_matrix(float time, vec3_t sky_matrix[3])
{
	if (sky_rotation != 0.f)
	{
		SetupRotationMatrix(sky_matrix, sky_axis, (sky_autorotate ? time : 1.f) * sky_rotation);
	}
	else
	{
		VectorSet(sky_matrix[0], 1.f, 0.f, 0.f);
		VectorSet(sky_matrix[1], 0.f, 1.f, 0.f);
		VectorSet(sky_matrix[2], 0.f, 0.f, 1.f);
	}
}

static void
prepare_camera(const vec3_t position, const vec3_t direction, mat4_t data)
{
	vec3_t forward, right, up;
	VectorCopy(direction, forward);
	VectorNormalize(forward);

	if (fabsf(forward[2]) < 0.99f)
		VectorSet(up, 0.f, 0.f, 1.f);
	else
		VectorSet(up, 0.f, 1.f, 0.f);

	CrossProduct(forward, up, right);
	CrossProduct(right, forward, up);
	VectorNormalize(up);
	VectorNormalize(right);

	float aspect = 1.75f;
	float tan_half_fov_x = 1.f;
	float tan_half_fov_y = tan_half_fov_x / aspect;

	VectorCopy(position, data + 0);
	VectorCopy(forward, data + 4);
	VectorMA(data + 4, -tan_half_fov_x, right, data + 4);
	VectorMA(data + 4, tan_half_fov_y, up, data + 4);
	VectorScale(right, 2.f * tan_half_fov_x, data + 8);
	VectorScale(up, -2.f * tan_half_fov_y, data + 12);
}

static void
prepare_ubo(refdef_t *fd, mleaf_t* viewleaf, const reference_mode_t* ref_mode, const vec3_t sky_matrix[3], bool render_world, int waterLevel)
{
	const bsp_mesh_t* wm = &vkpt_refdef.bsp_mesh_world;

	float P[16];
	float V[16];

	QVKUniformBuffer_t *ubo = &vkpt_refdef.uniform_buffer;
	memcpy(ubo->V_prev, ubo->V, sizeof(float) * 16);
	memcpy(ubo->P_prev, ubo->P, sizeof(float) * 16);
	memcpy(ubo->invP_prev, ubo->invP, sizeof(float) * 16);
	ubo->cylindrical_hfov_prev = ubo->cylindrical_hfov;
	ubo->prev_taa_output_width = ubo->taa_output_width;
	ubo->prev_taa_output_height = ubo->taa_output_height;

	{
		float raw_proj[16];
		create_projection_matrix(raw_proj, vkpt_refdef.z_near, vkpt_refdef.z_far, fd->fov_x, fd->fov_y);

		// In some cases (ex.: player setup), 'fd' will describe a viewport that is not full screen.
		// Simulate that with a projection matrix adjustment to avoid modifying the rendering code.

		float viewport_proj[16] = {
			[0] = (float)fd->width / (float)qvk.extent_unscaled.width,
			[12] = (float)(fd->x * 2 + fd->width - (int)qvk.extent_unscaled.width) / (float)qvk.extent_unscaled.width,
			[5] = (float)fd->height / (float)qvk.extent_unscaled.height,
			[13] = -(float)(fd->y * 2 + fd->height - (int)qvk.extent_unscaled.height) / (float)qvk.extent_unscaled.height,
			[10] = 1.f,
			[15] = 1.f
		};

		mult_matrix_matrix(P, viewport_proj, raw_proj);
	}
	create_view_matrix(V, fd);
	memcpy(ubo->V, V, sizeof(float) * 16);
	memcpy(ubo->P, P, sizeof(float) * 16);
	inverse(V, *ubo->invV);
	inverse(P, *ubo->invP);

	if (cvar_pt_projection->integer == 1 && render_world)
	{
		float rad_per_pixel = atanf(tanf(fd->fov_y * M_PI / 360.0f) / ((float)qvk.extent_unscaled.height * 0.5f));
		ubo->cylindrical_hfov = rad_per_pixel * (float)qvk.extent_unscaled.width;
	}
	else
	{
		ubo->cylindrical_hfov = 0.f;
	}
	
	ubo->current_frame_idx = qvk.frame_counter;
	ubo->width = qvk.extent_render.width;
	ubo->height = qvk.extent_render.height;
	ubo->prev_width = qvk.extent_render_prev.width;
	ubo->prev_height = qvk.extent_render_prev.height;
	ubo->inv_width = 1.0f / (float)qvk.extent_render.width;
	ubo->inv_height = 1.0f / (float)qvk.extent_render.height;
	ubo->unscaled_width = qvk.extent_unscaled.width;
	ubo->unscaled_height = qvk.extent_unscaled.height;
	ubo->taa_image_width = qvk.extent_taa_images.width;
	ubo->taa_image_height = qvk.extent_taa_images.height;
	ubo->taa_output_width = qvk.extent_taa_output.width;
	ubo->taa_output_height = qvk.extent_taa_output.height;
	ubo->current_gpu_slice_width = qvk.gpu_slice_width;
	ubo->prev_gpu_slice_width = qvk.gpu_slice_width_prev;
	ubo->screen_image_width = qvk.extent_screen_images.width;
	ubo->screen_image_height = qvk.extent_screen_images.height;
	ubo->water_normal_texture = water_normal_texture - r_images;
	ubo->pt_swap_checkerboard = 0;

	// Field layout. See the comment on pt_fullres_fields in global_ubo.h.
	if (pt_field_offset_prev == 0)
		pt_field_offset_prev = vkpt_pt_field_width();   // first frame - no history yet

	ubo->pt_fullres_fields = DLSSSplitFieldsEnabled()
		? (DLSSFieldHalfRes() ? PT_FIELDS_HALFRES : PT_FIELDS_FULLRES)
		: PT_FIELDS_CHECKERBOARD;
	ubo->pt_field_offset = (int)vkpt_pt_field_width();
	ubo->pt_prev_field_offset = (int)pt_field_offset_prev;
	pt_field_offset_prev = vkpt_pt_field_width();

	// flt_enable answers "is A-SVGF running". This answers "will anything denoise this
	// frame", which is what the path-selection and material heuristics actually want to
	// know - DLSS-RR is a denoiser even when A-SVGF is bypassed.
	ubo->pt_denoiser_present = (ref_mode->enable_denoiser || ref_mode->rr_denoiser) ? 1 : 0;

	qvk.extent_render_prev = qvk.extent_render;
	qvk.gpu_slice_width_prev = qvk.gpu_slice_width;

	int camera_cluster_contents = viewleaf ? viewleaf->contents : 0;

	if (camera_cluster_contents & CONTENTS_WATER)
		ubo->medium = MEDIUM_WATER;
	else if (camera_cluster_contents & CONTENTS_SLIME)
		ubo->medium = MEDIUM_SLIME;
	else if (camera_cluster_contents & CONTENTS_LAVA)
		ubo->medium = MEDIUM_LAVA;
	else
		ubo->medium = MEDIUM_NONE;

	if (waterLevel == 3) {
		ubo->medium = MEDIUM_WATER;
	}

	ubo->time = fd->time;
	ubo->num_static_primitives = 0;
	if (wm->geom_opaque.prim_counts)      ubo->num_static_primitives += wm->geom_opaque.prim_counts[0];
	if (wm->geom_transparent.prim_counts) ubo->num_static_primitives += wm->geom_transparent.prim_counts[0];
	if (wm->geom_masked.prim_counts)      ubo->num_static_primitives += wm->geom_masked.prim_counts[0];
	ubo->num_static_lights = vkpt_refdef.bsp_mesh_world.num_light_polys;

	vkpt_fog_upload(ubo->fog_volumes);

	// The rerelease's per-map atmospheric fog. Unlike fog_volumes above (which
	// are hand-authored AABBs driven by the "fog" console command), this comes
	// off the map's own worldspawn keys and drives the density of the medium the
	// god-ray pass marches, so it scatters light instead of tinting pixels.
	{
		mapfog_params_t mf;
		if (CL_GetMapFog(&mf)) {
			ubo->fog_enable     = 1;
			ubo->fog_density    = mf.density;
			ubo->fog_hf_density = mf.hf_density;
			ubo->fog_hf_falloff = mf.hf_falloff;
			ubo->fog_hf_start_z = mf.hf_start_z;
			ubo->fog_hf_end_z   = mf.hf_end_z;
			ubo->fog_color_r    = mf.color[0];
			ubo->fog_color_g    = mf.color[1];
			ubo->fog_color_b    = mf.color[2];
			ubo->fog_hf_start_r = mf.hf_start_color[0];
			ubo->fog_hf_start_g = mf.hf_start_color[1];
			ubo->fog_hf_start_b = mf.hf_start_color[2];
			ubo->fog_hf_end_r   = mf.hf_end_color[0];
			ubo->fog_hf_end_g   = mf.hf_end_color[1];
			ubo->fog_hf_end_b   = mf.hf_end_color[2];
			ubo->fog_mode       = mf.mode;
		} else {
			ubo->fog_enable = 0;
			ubo->fog_mode   = 0;
		}
		// fog_num_model_lights is NOT set here - num_model_lights is not built
		// until add_dlights() further down this frame. See there.
		//
		// The camera's own cluster. The fog march needs a light list per point
		// along the ray, and the per-pixel cluster texture only gives it the one
		// at the far END of the ray - which is why fog right in front of the
		// player was being lit by lights in a distant room. Near the camera this
		// is the correct list.
		ubo->fog_camera_cluster = viewleaf ? viewleaf->cluster : -1;
		ubo->fog_pad3 = 0;

		// Trace real sky visibility per march point instead of relying on the
		// per-cluster estimate below. The cluster masks cannot distinguish an open
		// canyon floor from a cave with a sight line out; a ray can. Set to 0 to
		// A/B against the old behaviour.
		ubo->fog_sky_trace = Cvar_Get("pt_fog_sky_trace", "1", CVAR_ARCHIVE)->integer;

		// Sky exposure, faded over time rather than switched.
		//
		// The fog samples one cluster - the camera's - for the whole ray, and
		// "does this cluster contain sky" is a hard boolean. Walking from under
		// a rock overhang into the open therefore made the sky fog appear in a
		// single frame, which reads as a glitch. Easing it converts that pop
		// into a short fade. It does NOT make the answer any more correct: the
		// real fix is per-march-point sky visibility, which needs a froxel grid.
		{
			static unsigned prev_ms = 0;
			static float    sky_fade = 0.f;

			// NEITHER available mask answers "can this point see the sky", and
			// they fail in OPPOSITE directions:
			//
			//   sky_cluster_mask - clusters that CONTAIN sky faces. Too strict:
			//     standing in the open on a canyon floor, the sky brushes are
			//     far overhead in a different cluster, so this reads 0 and the
			//     fog vanishes even with open sky directly above.
			//   sky_visibility  - union of the PVS of sky clusters, i.e. "could
			//     POTENTIALLY see sky". Too permissive: caves connected to the
			//     outdoors by any sight line read 1 and fill with sky fog.
			//
			// So take the strict one at full strength and the PVS one at a
			// fraction of it, and expose that fraction. pt_fog_sky_pvs 0 is the
			// strict behaviour, 1 is the permissive one, and in between trades
			// one artifact against the other. This is a stopgap for a real
			// visibility test - i.e. the froxel grid - not a solution.
			float sky_target = 0.f;
			if (viewleaf && viewleaf->cluster >= 0) {
				int c = viewleaf->cluster;
				const bsp_mesh_t *wm = &vkpt_refdef.bsp_mesh_world;
				if (wm->sky_cluster_mask[c >> 3] & (1 << (c & 7)))
					sky_target = 1.f;
				else if (wm->sky_visibility[c >> 3] & (1 << (c & 7)))
					sky_target = Cvar_Get("pt_fog_sky_pvs", "0.5", CVAR_ARCHIVE)->value;
			}

			unsigned now = Sys_Milliseconds();
			float dt = (prev_ms && now > prev_ms) ? (now - prev_ms) * 0.001f : 0.f;
			prev_ms = now;
			if (dt > 0.25f) dt = 0.25f;     // a hitch or a level load must not snap it

			// ~250 ms to cross most of the way
			float rate = 1.f - expf(-dt / 0.1f);
			sky_fade += (sky_target - sky_fade) * rate;

			ubo->fog_sky_fade = sky_fade;
		}

		// The map's own skybox radiance, for the fog's sky term. This is the
		// same value that is handed to the light buffer as sky_radiance, and it
		// is computed from the actual skybox images - unlike
		// sun_color_ubo.sky_color, which physical_sky.comp writes and is
		// therefore ZERO on every map that ships its own skybox.
		ubo->fog_sky_r = avg_envmap_color[0] * ubo->pt_env_scale;
		ubo->fog_sky_g = avg_envmap_color[1] * ubo->pt_env_scale;
		ubo->fog_sky_b = avg_envmap_color[2] * ubo->pt_env_scale;
	}

#define UBO_CVAR_DO(name, default_value) ubo->name = cvar_##name->value;
	UBO_CVAR_LIST
#undef UBO_CVAR_DO

	bool fsr_enabled = vkpt_fsr_is_enabled();
	qboolean dlss_enabled = DLSSEnabled();

	if (!ref_mode->enable_denoiser)
	{
		// Disable fake specular because it is not supported without the denoiser, and the
		// result looks too dark with it missing.
		//
		// This applies to DLSS-RR too, and used to be skipped for it. Fake specular is
		// synthesized from the LF spherical harmonics at the end of asvgf_atrous.comp,
		// which does not run when flt_enable is 0; compositing.comp, the shader that
		// replaces it, does not compute it, and RR cannot - it never sees the SH data.
		// Meanwhile indirect_lighting.rgen scales the *real* traced indirect specular by
		// (1 - fake_specular_weight) on the assumption that the fake term will make up the
		// difference, so at the default threshold of 0.2 every surface rougher than 0.3
		// has its indirect specular multiplied by zero with nothing to add it back. On a
		// rough metal that is nearly all of its indirect light, because get_reflectivity
		// leaves a metal with no diffuse albedo for the LF/HF channels to modulate.
		// Setting the threshold to 1 also flips specular_pdf back to 1.0 for pure metals,
		// so their bounce rays are sampled from the GGX lobe again instead of being split
		// 50/50 with a diffuse lobe that carries no energy.
		if (!ref_mode->rr_denoiser || cvar_pt_dlss_indirect_spec->integer != 0)
			ubo->pt_fake_roughness_threshold = 1.f;

		if (!ref_mode->rr_denoiser)
		{
			// swap the checkerboard fields every frame in reference or noisy mode to accumulate 
			// both reflection and refraction in every pixel
			ubo->pt_swap_checkerboard = (qvk.frame_counter & 1);
		}
		// Swapping the checkerboard is specific to having no temporal denoiser at all: with
		// DLSS-RR it would make every glass pixel alternate between the reflection and
		// refraction path each frame, which is maximal temporal instability for a temporal
		// denoiser.

		if (ref_mode->enable_accumulation)
		{
			ubo->pt_texture_lod_bias = -log2f(sqrtf(get_accumulation_rendering_framenum()));

			// disable the other stabilization hacks
			ubo->pt_specular_anti_flicker = 0.f;
			ubo->pt_sun_bounce_range = 10000.f;
			ubo->pt_ndf_trim = 1.f;
		}
	}
	else if(fsr_enabled || (qvk.effective_aa_mode == AA_MODE_UPSCALE || qvk.effective_aa_mode == AA_MODE_DLSS))
	{
		// adjust texture LOD bias to the resolution scale, i.e. use negative bias if scale is < 100
		float resolution_scale = (drs_effective_scale != 0) ? (float)drs_effective_scale : (float)scr_viewsize->integer;
		resolution_scale *= 0.01f;
		clamp(resolution_scale, 0.1f, 1.f);

		if (qvk.effective_aa_mode == AA_MODE_DLSS) {
			resolution_scale = GetDLSSResolutionScale();
		}

		ubo->pt_texture_lod_bias = cvar_pt_texture_lod_bias->value + log2f(resolution_scale);
	}

	if (DLSSEnabled()) {
		ubo->pt_texture_lod_bias = cvar_pt_texture_lod_bias->value + log2f(GetDLSSResolutionScale());
	}

	{
		// figure out if DoF should be enabled in the current rendering mode

		bool enable_dof = true;

		switch (cvar_pt_dof->integer)
		{
		case 0: enable_dof = false; break;
		case 1: enable_dof = ref_mode->enable_accumulation; break;
		case 2: enable_dof = !ref_mode->enable_denoiser; break;
		default: enable_dof = true; break;
		}

		if (cvar_pt_projection->integer != 0)
		{
			// DoF does not make physical sense with the cylindrical projection
			enable_dof = false;
		}

		if (!enable_dof)
		{
			// if DoF should not be enabled, make the aperture size zero
			ubo->pt_aperture = 0.f;
		}
	}

	// number of polygon vertices must be an integer
	ubo->pt_aperture_type = roundf(ubo->pt_aperture_type);

	ubo->temporal_blend_factor = ref_mode->temporal_blend_factor;
	ubo->flt_enable = ref_mode->enable_denoiser;
	ubo->flt_taa = qvk.effective_aa_mode;
	ubo->pt_dlss =	DLSSMode();
	ubo->pt_dlssdn = DLSSModeDenoise();
	ubo->pt_num_bounce_rays = ref_mode->num_bounce_rays;
	ubo->pt_reflect_refract = ref_mode->reflect_refract;

	if (ref_mode->num_bounce_rays < 1.f)
		ubo->pt_specular_mis = 0; // disable MIS if there are no specular rays

	ubo->pt_min_log_sky_luminance = exp2f(ubo->pt_min_log_sky_luminance);
	ubo->pt_max_log_sky_luminance = exp2f(ubo->pt_max_log_sky_luminance);

	memcpy(ubo->cam_pos, fd->vieworg, sizeof(float) * 3);
	ubo->cluster_debug_index = cluster_debug_index;

	if (!temporal_frame_valid)
	{
		ubo->flt_temporal_lf = 0;
		ubo->flt_temporal_hf = 0;
		ubo->flt_temporal_spec = 0;
		ubo->flt_taa = 0;
	}

	if (qvk.effective_aa_mode == AA_MODE_UPSCALE || DLSSEnabled())
	{
		int taa_index = (int)(qvk.frame_counter % NUM_TAA_SAMPLES);
		ubo->sub_pixel_jitter[0] = taa_samples[taa_index][0];
		ubo->sub_pixel_jitter[1] = taa_samples[taa_index][1];
	}
	else
	{
		ubo->sub_pixel_jitter[0] = 0.f;
		ubo->sub_pixel_jitter[1] = 0.f;
	}

	// Set up constants for FSR
	if (fsr_enabled)
	{
		vkpt_fsr_update_ubo(ubo);
	}

	ubo->first_person_model = cl_player_model->integer == CL_PLAYER_MODEL_FIRST_PERSON;

	memset(ubo->environment_rotation_matrix, 0, sizeof(ubo->environment_rotation_matrix));
	VectorCopy(sky_matrix[0], ubo->environment_rotation_matrix[0]);
	VectorCopy(sky_matrix[1], ubo->environment_rotation_matrix[1]);
	VectorCopy(sky_matrix[2], ubo->environment_rotation_matrix[2]);
	

	if (wm->num_cameras > 0)
	{
		for (int n = 0; n < wm->num_cameras; n++)
		{
			prepare_camera(wm->cameras[n].pos, wm->cameras[n].dir, *ubo->security_camera_data[n]);
		}
	}
	else
	{
		ubo->pt_cameras = 0;
	}

	ubo->num_cameras = wm->num_cameras;
}

static void
update_mlight_prev_to_current()
{
	light_entity_id_count[entity_frame_num] = num_model_lights;
	for (int i = 0; i < light_entity_id_count[entity_frame_num]; i++) {
		entity_hash_t hash = *(entity_hash_t*)&light_entity_ids[entity_frame_num][i];
		if (hash.entity == 0u) continue;
		for (int j = 0; j < light_entity_id_count[!entity_frame_num]; j++) {
			if (light_entity_ids[entity_frame_num][i] == light_entity_ids[!entity_frame_num][j]) {
				vkpt_refdef.uniform_instance_buffer.mlight_prev_to_current[j] = i;
				break;
			}
		}
	}
}

static float lastFrameTime = 0.f;
static float lastWallClocktime = 0.f;

/* renders the map ingame */
void
R_RenderFrame_RTX(refdef_t *fd, int waterLevel)
{
	if (!qvk.swap_chain)
		return;

	vkpt_refdef.fd = fd;
	bool render_world = (fd->rdflags & RDF_NOWORLDMODEL) == 0;

	static float previous_time = -1.f;

	float frame_time = min(1.f, max(0.f, fd->time - previous_time));
	previous_time = fd->time;

	vkpt_freecam_update(cls.frametime);

	static unsigned previous_wallclock_time = 0;
	unsigned current_wallclock_time = Sys_Milliseconds();
	float frame_wallclock_time = (previous_wallclock_time != 0) ? (float)(current_wallclock_time - previous_wallclock_time) * 1e-3f : 0.f;
	previous_wallclock_time = current_wallclock_time;

	if (!temporal_frame_valid)
	{
		if (vkpt_refdef.fd && vkpt_refdef.fd->lightstyles)
			memcpy(vkpt_refdef.prev_lightstyles, vkpt_refdef.fd->lightstyles, sizeof(vkpt_refdef.prev_lightstyles));
		else
			memset(vkpt_refdef.prev_lightstyles, 0, sizeof(vkpt_refdef.prev_lightstyles));
	}

	mleaf_t* viewleaf = bsp_world_model ? BSP_PointLeaf(bsp_world_model->nodes, fd->vieworg) : NULL;
	
	bool sun_visible_prev = false;
	static float prev_adapted_luminance = 0.f;
	float adapted_luminance = 0.f;
	process_render_feedback(&fd->feedback, viewleaf, &sun_visible_prev, &adapted_luminance);

	// Sometimes, the readback returns 1.0 luminance instead of the real value.
	// Ignore these mysterious spikes.
	if (adapted_luminance != 1.0f) 
		prev_adapted_luminance = adapted_luminance;
	
	if (prev_adapted_luminance <= 0.f)
		prev_adapted_luminance = 0.005f;

	LOG_FUNC();
	if (!vkpt_refdef.bsp_mesh_world_loaded && render_world)
	{
		drs_last_frame_world = false;
		return;
	}

	vec3_t sky_matrix[3];
	prepare_sky_matrix(fd->time, sky_matrix);

	sun_light_t sun_light = { 0 };
	if (render_world)
	{
		vkpt_evaluate_sun_light(&sun_light, sky_matrix, fd->time);

		if (!vkpt_physical_sky_needs_update())
			sun_light.visible = sun_light.visible && sun_visible_prev;
	}

	reference_mode_t ref_mode;
	evaluate_reference_mode(&ref_mode);
	evaluate_taa_settings(&ref_mode);
	
	qvk.frame_menu_mode = cl_paused->integer == 1 && uis.menuDepth > 0 && render_world;

	int new_world_anim_frame = (int)(fd->time * 2);
	bool update_world_animations = (new_world_anim_frame != world_anim_frame);
	world_anim_frame = new_world_anim_frame;





	num_model_lights = 0;
	EntityUploadInfo upload_info = { 0 };
	vkpt_pt_reset_instances();
	vkpt_shadow_map_reset_instances();
	prepare_entities(&upload_info);
	if (bsp_world_model && render_world)
	{
		vkpt_pt_instance_model_blas(&vkpt_refdef.bsp_mesh_world.geom_opaque,      g_identity_transform, VERTEX_BUFFER_WORLD, -1, 0);
		vkpt_pt_instance_model_blas(&vkpt_refdef.bsp_mesh_world.geom_transparent, g_identity_transform, VERTEX_BUFFER_WORLD, -1, 0);
		vkpt_pt_instance_model_blas(&vkpt_refdef.bsp_mesh_world.geom_masked,      g_identity_transform, VERTEX_BUFFER_WORLD, -1, 0);
		vkpt_pt_instance_model_blas(&vkpt_refdef.bsp_mesh_world.geom_sky,         g_identity_transform, VERTEX_BUFFER_WORLD, -1, 0);
		vkpt_pt_instance_model_blas(&vkpt_refdef.bsp_mesh_world.geom_custom_sky,  g_identity_transform, VERTEX_BUFFER_WORLD, -1, 0);

		vkpt_build_beam_lights(model_lights, &num_model_lights, MAX_MODEL_LIGHTS, bsp_world_model, fd->entities, fd->num_entities, prev_adapted_luminance, light_entity_ids[entity_frame_num], &num_model_lights);
		add_dlights(vkpt_refdef.fd->dlights, vkpt_refdef.fd->num_dlights, model_lights, &num_model_lights, MAX_MODEL_LIGHTS, bsp_world_model, light_entity_ids[entity_frame_num]);
	}

	update_mlight_prev_to_current();
	vkpt_vertex_buffer_ensure_primbuf_size(upload_info.num_prims);

	QVKUniformBuffer_t *ubo = &vkpt_refdef.uniform_buffer;
	prepare_ubo(fd, viewleaf, &ref_mode, sky_matrix, render_world, waterLevel);
	ubo->prev_adapted_luminance = prev_adapted_luminance;

	// Tell the fog march how many model lights this frame has. Model lights are
	// packed contiguously after the static ones, so the range it reads is
	// [num_static_lights, +fog_num_model_lights). This must come AFTER both
	// add_dlights() (which builds the count) and prepare_ubo() (which would
	// otherwise overwrite it), and before the UBO is uploaded to staging.
	ubo->fog_num_model_lights = num_model_lights;

	if (cvar_tm_blend_enable->integer)
		Vector4Copy(fd->blend, ubo->fs_blend_color);
	else
		Vector4Set(ubo->fs_blend_color, 0.f, 0.f, 0.f, 0.f);

	ubo->weapon_left_handed = upload_info.weapon_left_handed;

	vkpt_physical_sky_update_ubo(ubo, &sun_light, render_world);
	vkpt_bloom_update(ubo, frame_time, ubo->medium != MEDIUM_NONE, qvk.frame_menu_mode);

	if(update_world_animations)
		bsp_mesh_animate_light_polys(&vkpt_refdef.bsp_mesh_world);
	vec3_t sky_radiance;
	VectorScale(avg_envmap_color, ubo->pt_env_scale, sky_radiance);
	vkpt_light_buffer_upload_to_staging(render_world, &vkpt_refdef.bsp_mesh_world, bsp_world_model, num_model_lights, model_lights, sky_radiance);
	
	float shadowmap_view_proj[16];
	float shadowmap_depth_scale;
	vkpt_shadow_map_setup(
		&sun_light,
		vkpt_refdef.bsp_mesh_world.world_aabb.mins,
		vkpt_refdef.bsp_mesh_world.world_aabb.maxs,
		shadowmap_view_proj,
		&shadowmap_depth_scale,
		ref_mode.enable_accumulation && num_accumulated_frames > 1);

	vkpt_god_rays_prepare_ubo(
		ubo,
		&vkpt_refdef.bsp_mesh_world.world_aabb,
		*ubo->P,
		*ubo->V,
		shadowmap_view_proj,
		shadowmap_depth_scale);

	bool god_rays_enabled = vkpt_god_rays_enabled(&sun_light) && render_world;

	VkSemaphore transfer_semaphores[VKPT_MAX_GPUS];
	VkSemaphore trace_semaphores[VKPT_MAX_GPUS];
	VkSemaphore prev_trace_semaphores[VKPT_MAX_GPUS];
	VkPipelineStageFlags wait_stages[VKPT_MAX_GPUS];
	uint32_t device_indices[VKPT_MAX_GPUS];
	uint32_t all_device_mask = (1 << qvk.device_count) - 1;
	bool* prev_trace_signaled = &qvk.semaphores[(qvk.current_frame_index - 1) % MAX_FRAMES_IN_FLIGHT][0].trace_signaled;
	bool* curr_trace_signaled = &qvk.semaphores[qvk.current_frame_index][0].trace_signaled;

	{
		// Transfer the light buffer from staging into device memory.
		// Previous frame's tracing still uses device memory, so only do the copy after that is finished.
		
		VkCommandBuffer transfer_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_transfer);
		VkCommandBuffer trace_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		const VkClearColorValue emptyColor = {
		.float32[0] = 0.0f,
		.float32[1] = 0.0f,
		.float32[2] = 0.0f,
		.float32[3] = 0.0f
			};

		const VkImageSubresourceRange subresource_range_reflect = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1
		};
		VkImageSubresourceRange reflectRange = subresource_range_reflect;
		VkImageSubresourceRange specularRange = subresource_range_reflect;

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_REFLECT_MOTION], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_REFLECT_MOTION],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_ALBEDO], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_ALBEDO],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_RAY_LENGTH], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_RAY_LENGTH],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_ROUGHNESS], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_ROUGHNESS],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);


		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_NORMAL], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_NORMAL],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);






		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_SPECULAR_ALBEDO], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_SPECULAR_ALBEDO],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);


		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_BEFORE_TRANSPARENT], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_BEFORE_TRANSPARENT],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);


		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_DIFFUSE], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_RAYLENGTH_DIFFUSE],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_SPECULAR], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_RAYLENGTH_SPECULAR],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);


		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_PT_REFLECTED_ALBEDO], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &reflectRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_PT_REFLECTED_ALBEDO],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);

		vkCmdClearColorImage(trace_cmd_buf, qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &emptyColor, 1, &specularRange);

		IMAGE_BARRIER(trace_cmd_buf,
			.image = qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR],
			.subresourceRange = subresource_range_reflect,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL,
			);
		

		_VK(vkpt_profiler_query(trace_cmd_buf, PROFILER_FRAME_TIME, PROFILER_START));

		BEGIN_PERF_MARKER(transfer_cmd_buf, PROFILER_UPLOAD_LIGHTS);
		vkpt_light_buffer_upload_staging(transfer_cmd_buf);			
		vkpt_iqm_matrix_buffer_upload_staging(transfer_cmd_buf);
		END_PERF_MARKER(transfer_cmd_buf, PROFILER_UPLOAD_LIGHTS);

		
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			device_indices[gpu] = gpu;
			transfer_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].transfer_finished;
			trace_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].trace_finished;
			prev_trace_semaphores[gpu] = qvk.semaphores[(qvk.current_frame_index - 1) % MAX_FRAMES_IN_FLIGHT][gpu].trace_finished;
			wait_stages[gpu] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		}

		vkpt_submit_command_buffer(
			transfer_cmd_buf, 
			qvk.queue_transfer, 
			all_device_mask, 
			(*prev_trace_signaled) ? qvk.device_count : 0, prev_trace_semaphores, wait_stages, device_indices, 
			qvk.device_count, transfer_semaphores, device_indices, 
			VK_NULL_HANDLE);

		
		*prev_trace_signaled = false;

		

		update_transparency(trace_cmd_buf, *ubo->V, fd->particles, fd->num_particles, fd->entities, fd->num_entities);

		// Copy the UBO contents from the staging buffer.
		// Actual contents are uploaded to the staging UBO below, right before executing the command buffer.
		vkpt_uniform_buffer_copy_from_staging(trace_cmd_buf);

		// put a profiler query without a marker for the frame begin/end - because markers do not 
		// work well across different command lists
		

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_UPDATE_ENVIRONMENT);
		if (render_world)
		{
			vkpt_physical_sky_record_cmd_buffer(trace_cmd_buf);
		}
		END_PERF_MARKER(trace_cmd_buf, PROFILER_UPDATE_ENVIRONMENT);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_INSTANCE_GEOMETRY);
		vkpt_instance_geometry(trace_cmd_buf, upload_info.num_instances, update_world_animations);
		END_PERF_MARKER(trace_cmd_buf, PROFILER_INSTANCE_GEOMETRY);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_BVH_UPDATE);
		vkpt_pt_create_all_dynamic(trace_cmd_buf, qvk.current_frame_index, &upload_info);
		vkpt_pt_create_toplevel(trace_cmd_buf, qvk.current_frame_index, &upload_info, upload_info.weapon_left_handed);
		vkpt_pt_update_descripter_set_bindings(qvk.current_frame_index);
		END_PERF_MARKER(trace_cmd_buf, PROFILER_BVH_UPDATE);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_SHADOW_MAP);
		if (god_rays_enabled)
		{
			vkpt_shadow_map_render(trace_cmd_buf, shadowmap_view_proj,
				vkpt_refdef.bsp_mesh_world.geom_opaque.prim_offsets[0] * 3,
				vkpt_refdef.bsp_mesh_world.geom_opaque.prim_counts[0] * 3,
				0,
				upload_info.opaque_prim_count * 3,
				vkpt_refdef.bsp_mesh_world.geom_transparent.prim_offsets[0] * 3,
				vkpt_refdef.bsp_mesh_world.geom_transparent.prim_counts[0] * 3);
		}
		END_PERF_MARKER(trace_cmd_buf, PROFILER_SHADOW_MAP);

		vkpt_pt_trace_primary_rays(trace_cmd_buf);

		// The host-side image of the uniform buffer is only ready after the `vkpt_pt_create_toplevel` call above
		_VK(vkpt_uniform_buffer_upload_to_staging());

		vkpt_submit_command_buffer(
			trace_cmd_buf,
			qvk.queue_graphics,
			all_device_mask,
			qvk.device_count, transfer_semaphores, wait_stages, device_indices,
			0, 0, 0,
			VK_NULL_HANDLE);
	}

	{
		VkCommandBuffer trace_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		if (god_rays_enabled)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS);
			vkpt_record_god_rays_trace_command_buffer(trace_cmd_buf, 0);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS);
		}

		// [froxel grid] the map fog's cheap path. Runs after the march - which
		// still produces the sun shafts - and before the filter, which is what
		// reads the integrated volume this writes.
		if (god_rays_enabled && vkpt_froxel_enabled())
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_FOG_FROXEL);
			vkpt_record_froxel_command_buffer(trace_cmd_buf);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_FOG_FROXEL);
		}

		if (ref_mode.reflect_refract > 0)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_1);
			vkpt_pt_trace_reflections(trace_cmd_buf, 0);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_1);
		}

		if (god_rays_enabled)
		{
			if (ref_mode.reflect_refract > 0)
			{
				BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_REFLECT_REFRACT);
				vkpt_record_god_rays_trace_command_buffer(trace_cmd_buf, 1);
				END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_REFLECT_REFRACT);
			}

			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_FILTER);
			vkpt_record_god_rays_filter_command_buffer(trace_cmd_buf);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_FILTER);
		}

		if (ref_mode.reflect_refract > 1)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_2);
			for (int pass = 0; pass < ref_mode.reflect_refract - 1; pass++)
			{
				vkpt_pt_trace_reflections(trace_cmd_buf, pass + 1);
			}
			END_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_2);
		}

		if (ref_mode.enable_denoiser)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_ASVGF_GRADIENT_REPROJECT);
			vkpt_asvgf_gradient_reproject(trace_cmd_buf);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_ASVGF_GRADIENT_REPROJECT);
		}

		vkpt_pt_trace_lighting(trace_cmd_buf, ref_mode.num_bounce_rays);
		
		vkpt_submit_command_buffer(
			trace_cmd_buf,
			qvk.queue_graphics,
			all_device_mask,
			0, 0, 0, 0,
			qvk.device_count, trace_semaphores, device_indices,
			VK_NULL_HANDLE);

		*curr_trace_signaled = true;
	}

	{
		VkCommandBuffer post_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_ASVGF_FULL);
		if (ref_mode.enable_denoiser)
		{
			vkpt_asvgf_filter(post_cmd_buf, cvar_pt_num_bounce_rays->value >= 0.5f);
		}
		else
		{
			vkpt_compositing(post_cmd_buf);
		}
		END_PERF_MARKER(post_cmd_buf, PROFILER_ASVGF_FULL);

		vkpt_interleave(post_cmd_buf);

		vkpt_taa(post_cmd_buf);

		if (!DLSSEnabled()) {
			BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_BLOOM);
			if (cvar_bloom_enable->integer != 0 || qvk.frame_menu_mode)
			{
				vkpt_bloom_record_cmd_buffer(post_cmd_buf);
			}
			END_PERF_MARKER(post_cmd_buf, PROFILER_BLOOM);
		}

#ifdef VKPT_IMAGE_DUMPS
		if (cvar_dump_image->integer)
		{
			copy_to_dump_texture(post_cmd_buf, VKPT_IMG_TAA_OUTPUT);
		}
#endif


		// Skip FSR (upscaling) if image is going to be heavily blurred anyway (menu mode)
		if(vkpt_fsr_is_enabled() && !qvk.frame_menu_mode)
		{
			vkpt_fsr_do(post_cmd_buf);
		}

		if (DLSSEnabled()) {
			DLSSRenderResolution resObj;
			resObj.inputWidth = qvk.extent_render.width;
			resObj.inputHeight = qvk.extent_render.height;
			resObj.outputWidth = qvk.extent_unscaled.width;
			resObj.outputHeight = qvk.extent_unscaled.height;

			// GPU timestamps around the NGX Evaluate call. DLSS (SR or Ray Reconstruction)
			// records its own work into post_cmd_buf, so this brackets the real cost.
			BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_DLSS);
			DLSSApply(post_cmd_buf, qvk, resObj, ubo->sub_pixel_jitter,
				frame_time <= 0.f ? frame_wallclock_time : frame_time,
				dlss_reset_history ? qtrue : qfalse);
			END_PERF_MARKER(post_cmd_buf, PROFILER_DLSS);
			dlss_reset_history = false;
			lastFrameTime = frame_time;
			lastWallClocktime = frame_wallclock_time;
		}
		else {
			BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_TONE_MAPPING);
			if (cvar_tm_enable->integer != 0)
			{
				vkpt_tone_mapping_record_cmd_buffer(post_cmd_buf, frame_time <= 0.f ? frame_wallclock_time : frame_time, DLSSEnabled());
			}
			END_PERF_MARKER(post_cmd_buf, PROFILER_TONE_MAPPING);
		}

		

		{
			VkBufferCopy copyRegion = { 0, 0, sizeof(ReadbackBuffer) };
			vkCmdCopyBuffer(post_cmd_buf, qvk.buf_readback.buffer, qvk.buf_readback_staging[qvk.current_frame_index].buffer, 1, &copyRegion);
		}

		_VK(vkpt_profiler_query(post_cmd_buf, PROFILER_FRAME_TIME, PROFILER_STOP));

		vkpt_submit_command_buffer_simple(post_cmd_buf, qvk.queue_graphics, true);
	}

	temporal_frame_valid = ref_mode.enable_denoiser;
	
	frame_ready = true;
	drs_last_frame_world = true;

	if (vkpt_refdef.fd && vkpt_refdef.fd->lightstyles) {
		memcpy(vkpt_refdef.prev_lightstyles, vkpt_refdef.fd->lightstyles, sizeof(vkpt_refdef.prev_lightstyles));
	}
}

static void temporal_cvar_changed(cvar_t *self)
{
	temporal_frame_valid = false;
	vkpt_dlss_request_history_reset();
}

// vid_present_stats accumulators. There are only two places this thread can block per
// frame - our own GPU fence and the presentation engine - and splitting them tells a
// GPU-bound frame apart from a present stall, which a utilisation overlay cannot.
static uint64_t present_stats_fence_us = 0;
static uint64_t present_stats_acquire_us = 0;
static uint64_t present_stats_last_us = 0;
static uint64_t present_stats_prev_frame_us = 0;
static uint64_t present_stats_worst_us = 0;
static uint64_t present_stats_worst_fence_us = 0;
static uint64_t present_stats_worst_acquire_us = 0;
static double   present_stats_worst_gpu_ms = 0;
static uint64_t present_stats_prev_fence_total = 0;
static uint64_t present_stats_prev_acquire_total = 0;
static uint32_t present_stats_frames = 0;
static uint32_t present_stats_long_frames = 0;

static void report_present_stats(void)
{
	uint64_t now = Sys_Microseconds();
	if (present_stats_last_us == 0)
	{
		// Discard whatever the two accumulators picked up before the window opened -
		// they are unsigned, and subtracting them from a shorter window underflows into
		// a nonsense "cpu" figure.
		present_stats_last_us = now;
		present_stats_prev_frame_us = now;
		present_stats_fence_us = 0;
		present_stats_acquire_us = 0;
		present_stats_prev_fence_total = 0;
		present_stats_prev_acquire_total = 0;
		present_stats_frames = 0;
		return;
	}

	// A 5% tail is invisible in a mean, and a missed vblank is exactly a tail event, so
	// track the worst frame in the window and how many ran long. "long" is relative to
	// the median-ish frame, so it needs no assumption about the refresh rate.
	uint64_t this_frame_us = now - present_stats_prev_frame_us;
	present_stats_prev_frame_us = now;
	if (this_frame_us > present_stats_worst_us)
	{
		// Keep the worst frame's own split, not just its duration. That is what says
		// WHERE a long frame went: low fence means the time was spent outside the
		// renderer entirely (CPU), high fence with normal GPU means we waited an extra
		// vblank, high fence with high GPU means the GPU genuinely spiked.
		present_stats_worst_us = this_frame_us;
		present_stats_worst_fence_us = present_stats_fence_us - present_stats_prev_fence_total;
		present_stats_worst_acquire_us = present_stats_acquire_us - present_stats_prev_acquire_total;
		present_stats_worst_gpu_ms = vkpt_get_profiler_result(PROFILER_FRAME_TIME);
	}
	present_stats_prev_fence_total = present_stats_fence_us;
	present_stats_prev_acquire_total = present_stats_acquire_us;

	++present_stats_frames;
	uint64_t window_us = now - present_stats_last_us;
	if (window_us < 1000000)
	{
		// Count frames that took at least 1.5x the running average so far.
		uint64_t avg_so_far = window_us / present_stats_frames;
		if (avg_so_far > 0 && this_frame_us * 2 > avg_so_far * 3)
			++present_stats_long_frames;
		return;
	}

	// GPU ms comes from the timestamp profiler, which is recorded unconditionally -
	// cvar_profiler only gates drawing it. It is the number the fence wait cannot give
	// us: under FIFO, vkAcquireNextImageKHR returns an index immediately and defers the
	// vblank wait to the image_available semaphore, which the GPU waits on, so the wait
	// for vsync lands inside the fence time and not in acquire. GPU well below fence
	// means we are waiting for the display, not for our own work.
	double secs = (double)window_us / 1000000.0;
	Com_Printf("present: %.1f fps | avg %.2f (fence %.2f, acq %.2f, cpu %.2f) GPU %.2f | %u long, worst %.2f (fence %.2f, acq %.2f, cpu %.2f, GPU %.2f) | %s, %u images\n",
		present_stats_frames / secs,
		(double)window_us / 1000.0 / present_stats_frames,
		(double)present_stats_fence_us / 1000.0 / present_stats_frames,
		(double)present_stats_acquire_us / 1000.0 / present_stats_frames,
		(double)(window_us - present_stats_fence_us - present_stats_acquire_us) / 1000.0 / present_stats_frames,
		vkpt_get_profiler_result(PROFILER_FRAME_TIME),
		present_stats_long_frames,
		(double)present_stats_worst_us / 1000.0,
		(double)present_stats_worst_fence_us / 1000.0,
		(double)present_stats_worst_acquire_us / 1000.0,
		(double)(present_stats_worst_us - present_stats_worst_fence_us - present_stats_worst_acquire_us) / 1000.0,
		present_stats_worst_gpu_ms,
		qvk.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" :
		qvk.present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "IMMEDIATE",
		qvk.surf_num_images);

	present_stats_fence_us = 0;
	present_stats_acquire_us = 0;
	present_stats_worst_us = 0;
	present_stats_worst_fence_us = 0;
	present_stats_worst_acquire_us = 0;
	present_stats_worst_gpu_ms = 0;
	present_stats_frames = 0;
	present_stats_long_frames = 0;
	present_stats_last_us = now;
}

static void
recreate_swapchain(void)
{
	vkpt_device_wait_idle();

	/* vkpt_destroy_all() below destroys and rebuilds EVERY vkpt image, including the
	   colour, depth, motion-vector and DLSS_FG_OUTPUT* images that DLSS-G was created
	   against. The feature has to go with them: keeping it alive across a rebuild left
	   it referring to freed resources, and the next create - triggered by a DLSS mode
	   change, which alters the render extent without touching the output extent -
	   crashed inside nvngx with an access violation. Destroying it here means it is
	   rebuilt lazily by ValidateDLSSGFeature() on the next frame that needs it. */
	/* A queued present still refers to the swapchain we are about to destroy. */
	FGPresent_Drain();

	DestroyDLSSGFeature();

	vkpt_dlss_request_history_reset();
	vkpt_destroy_all(VKPT_INIT_SWAPCHAIN_RECREATE);
	destroy_swapchain();
	SDL_GetWindowSize(qvk.window, &qvk.win_width, &qvk.win_height);
	create_swapchain();
	vkpt_initialize_all(VKPT_INIT_SWAPCHAIN_RECREATE);

	qvk.wait_for_idle_frames = MAX_FRAMES_IN_FLIGHT * 2;
}

static int compare_doubles(const void* pa, const void* pb)
{
	double a = *(double*)pa;
	double b = *(double*)pb;

	if (a < b) return -1; 
	if (a > b) return 1;
	return 0;
}

// DRS (Dynamic Resolution Scaling) functions

static void drs_init(void)
{
	cvar_drs_enable = Cvar_Get("drs_enable", "0", CVAR_ARCHIVE);
	// Target FPS value
	cvar_drs_target = Cvar_Get("drs_target", "60", CVAR_ARCHIVE);
	cvar_drs_target->changed = drs_target_changed;
	// Minimum resolution scale in percents
	cvar_drs_minscale = Cvar_Get("drs_minscale", "50", 0);
	cvar_drs_minscale->changed = drs_minscale_changed;
	// Maximum resolution scale in percents
	cvar_drs_maxscale = Cvar_Get("drs_maxscale", "100", 0);
	cvar_drs_maxscale->changed = drs_maxscale_changed;
	// Resolution regulator parameters, see the `dynamic_resolution_scaling()` function
	cvar_drs_gain = Cvar_Get("drs_gain", "20", 0);
	cvar_drs_adjust_up = Cvar_Get("drs_adjust_up", "0.92", 0);
	cvar_drs_adjust_down = Cvar_Get("drs_adjust_down", "0.98", 0);

	// Value of drs_current_scale from last run. To start with a close-to-desired scale
	cvar_drs_last_scale = Cvar_Get("drs_last_scale", "0", CVAR_ARCHIVE);
}

static void drs_process(void)
{
#define SCALING_FRAMES 5
	static int num_valid_frames = 0;
	static double valid_frame_times[SCALING_FRAMES];

	if (cvar_drs_enable->integer == 0)
	{
		num_valid_frames = 0;

		if (is_accumulation_rendering_active())
			drs_effective_scale = max(100, scr_viewsize->integer);
		else
			drs_effective_scale = 0;

		return;
	}

	if (is_accumulation_rendering_active())
	{
		num_valid_frames = 0;
		drs_effective_scale = max(cvar_drs_minscale->integer, cvar_drs_maxscale->integer);
		return;
	}

	int last_scale = drs_current_scale;
	if(!last_scale)
		last_scale = cvar_drs_last_scale->integer;
	if(!last_scale)
		last_scale = scr_viewsize->integer;

	if (!drs_last_frame_world)
	{
		/* Last frame world was not rendered:
		* Frame times wouldn't be representative, so don't adjust DRS.
		* If DRS wasn't adjusted previously return a constrained default value */
		drs_effective_scale = max(cvar_drs_minscale->integer, min(cvar_drs_maxscale->integer, last_scale));
		return;
	}

	drs_effective_scale = last_scale;

	double ms = vkpt_get_profiler_result(PROFILER_FRAME_TIME);

	// 0ms frame may happen if we just played a cinematic
	if (ms <= 0 || ms > 1000)
		return;

	valid_frame_times[num_valid_frames] = ms;
	num_valid_frames++;

	if (num_valid_frames < SCALING_FRAMES)
		return;

	num_valid_frames = 0;

	qsort(valid_frame_times, SCALING_FRAMES, sizeof(double), compare_doubles);

	double representative_time = 0;
	for(int i = 1; i < SCALING_FRAMES - 1; i++)
		representative_time += valid_frame_times[i];
	representative_time /= (SCALING_FRAMES - 2);

	double target_time = 1000.0 / cvar_drs_target->value;
	double f = cvar_drs_gain->value * (1.0 - representative_time / target_time) - 1.0;

	int scale = drs_effective_scale;
	if (representative_time < target_time * cvar_drs_adjust_up->value)
	{
		f += 0.5;
		clamp(f, 1, 10);
		scale += (int)f;
	}
	else if (representative_time > target_time * cvar_drs_adjust_down->value)
	{
		f -= 0.5;
		clamp(f, -1, -10);
		scale += f;
	}

	drs_current_scale = max(cvar_drs_minscale->integer, min(cvar_drs_maxscale->integer, scale));
	drs_effective_scale = drs_current_scale;
}

void
R_BeginFrame_RTX(void)
{
	LOG_FUNC();

	qvk.current_frame_index = qvk.frame_counter % MAX_FRAMES_IN_FLIGHT;

	const bool present_stats = (cvar_present_stats && cvar_present_stats->integer != 0);
	uint64_t t_block_begin = present_stats ? Sys_Microseconds() : 0;

	VkResult res_fence = vkWaitForFences(qvk.device, 1, qvk.fences_frame_sync + qvk.current_frame_index, VK_TRUE, ~((uint64_t) 0));

	if (present_stats)
		present_stats_fence_us += Sys_Microseconds() - t_block_begin;
	
	if (res_fence == VK_ERROR_DEVICE_LOST)
	{
		// TODO implement a good error box or vid_restart or something
		Com_EPrintf("Device lost!\n");
		exit(1);
	}

	if (!qvk.swap_chain)
	{
		VkSurfaceCapabilitiesKHR surf_capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(qvk.physical_device, qvk.surface, &surf_capabilities);

		// see if we're un-minimized again
		if (surf_capabilities.currentExtent.width != 0 && surf_capabilities.currentExtent.height != 0)
		{
			swapchain_reason = "un-minimized";
			recreate_swapchain();
		}
	}

	drs_process();
	if (vkpt_refdef.fd)
	{
		vkpt_refdef.fd->feedback.resolution_scale = (drs_effective_scale != 0) ? drs_effective_scale : scr_viewsize->integer;
	}

	qvk.extent_render = get_render_extent();
	qvk.gpu_slice_width = (get_pt_packed_width() + qvk.device_count - 1) / qvk.device_count;
	
	VkExtent2D extent_screen_images = get_screen_image_extent();
	uint32_t screen_image_profile = vkpt_screen_image_profile();

	if(!extents_equal(extent_screen_images, qvk.extent_screen_images) || (!!cvar_hdr->integer != qvk.surf_is_hdr) || (!!cvar_vsync->integer != qvk.surf_vsync)
	   || (!!cvar_vsync_mailbox->integer != qvk.surf_vsync_mailbox)
	   || (screen_image_profile != screen_image_profile_current)
	   || (desired_swapchain_images() != swapchain_requested_images)
	   || (fg_wants_no_vsync() != swapchain_fg_forced_no_vsync))
	{
		swapchain_reason =
			!extents_equal(extent_screen_images, qvk.extent_screen_images) ? "screen image extent changed" :
			(screen_image_profile != screen_image_profile_current) ? "screen image profile changed" :
			(!!cvar_hdr->integer != qvk.surf_is_hdr) ? "vid_hdr changed" :
			(!!cvar_vsync->integer != qvk.surf_vsync) ? "vid_vsync changed" :
			(!!cvar_vsync_mailbox->integer != qvk.surf_vsync_mailbox) ? "vid_vsync_mailbox changed" :
			(desired_swapchain_images() != swapchain_requested_images)
			? "swapchain image count changed" : "frame generation vsync override changed";
		qvk.extent_screen_images = extent_screen_images;
		screen_image_profile_current = screen_image_profile;
		recreate_swapchain();
	}

retry:;

	if (!qvk.swap_chain) // we're minimized, don't render
		return;

#ifdef VKPT_DEVICE_GROUPS
	VkAcquireNextImageInfoKHR acquire_info = {
		.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
		.swapchain = qvk.swap_chain,
		.timeout = (~((uint64_t) 0)),
		.semaphore = qvk.semaphores[qvk.current_frame_index][0].image_available,
		.fence = VK_NULL_HANDLE,
		.deviceMask = (1 << qvk.device_count) - 1,
	};

	if (present_stats) t_block_begin = Sys_Microseconds();
	VkResult res_swapchain = acquire_next_image_locked(
		qvk.semaphores[qvk.current_frame_index][0].image_available,
		&qvk.current_swap_chain_image_index);
#else
	if (present_stats) t_block_begin = Sys_Microseconds();
	VkResult res_swapchain = acquire_next_image_locked(
		qvk.semaphores[qvk.current_frame_index][0].image_available,
		&qvk.current_swap_chain_image_index);
#endif
	if (present_stats)
	{
		present_stats_acquire_us += Sys_Microseconds() - t_block_begin;
		report_present_stats();
	}
	if(res_swapchain == VK_ERROR_OUT_OF_DATE_KHR || res_swapchain == VK_SUBOPTIMAL_KHR
	   || res_swapchain == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
		swapchain_reason = (res_swapchain == VK_ERROR_OUT_OF_DATE_KHR) ? "acquire: OUT_OF_DATE" :
			(res_swapchain == VK_SUBOPTIMAL_KHR) ? "acquire: SUBOPTIMAL" : "acquire: FSE MODE LOST";
		recreate_swapchain();
		goto retry;
	}
	else if(res_swapchain != VK_SUCCESS) {
		Com_EPrintf("Error %d in vkAcquireNextImageKHR\n", res_swapchain);
	}

	if (qvk.wait_for_idle_frames) {
		vkpt_device_wait_idle();
		qvk.wait_for_idle_frames--;
	}

	vkResetFences(qvk.device, 1, qvk.fences_frame_sync + qvk.current_frame_index);

	vkpt_reset_command_buffers(&qvk.cmd_buffers_graphics);
	vkpt_reset_command_buffers(&qvk.cmd_buffers_transfer);

	// Process the profiler queries - always enabled to support DRS
	{
		VkCommandBuffer reset_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics); 		
		VkCommandBuffer reset_cmd_buf_transfer = vkpt_begin_command_buffer(&qvk.cmd_buffers_transfer);

		vkpt_submit_command_buffer_simple(reset_cmd_buf, qvk.queue_graphics, true);
		vkpt_reset_query_pool(reset_cmd_buf_transfer);
		_VK(vkpt_profiler_next_frame(reset_cmd_buf, qfalse));	
		
		
	}

	vkpt_textures_destroy_unused();
	vkpt_textures_end_registration();
	vkpt_textures_update_descriptor_set();

	vkpt_vertex_buffer_upload_models();
	vkpt_draw_clear_stretch_pics();

	SCR_SetHudAlpha(1.f);
}

void
R_EndFrame_RTX(void)
{
	LOG_FUNC();

	if (!qvk.swap_chain)
	{
		vkpt_draw_clear_stretch_pics();
		return;
	}

	if (cvar_profiler->integer) {
		VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_transfer);

		draw_profiler(cvar_flt_enable->integer != 0);
	}
		
	if(cvar_tm_debug->integer)
		vkpt_tone_mapping_draw_debug();

	/* Simulation is over by the time the frame is being submitted; the render submit
	   span starts here and closes after vkQueueSubmit. */
	Reflex_SetMarker(VK_LATENCY_MARKER_SIMULATION_END_NV);
	Reflex_SetMarker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	/* Swapchain images holding the interpolated frames, filled in by the frame
	   generation path below. fg_generated_count stays 0 when FG is off or fell back
	   to the ordinary single present. */
	uint32_t fg_interp_image_index[DLSSG_MAX_GENERATED_FRAMES];
	unsigned int fg_generated_count = 0;

	/* Microseconds this frame spent BLOCKED on the presentation path: waiting for the
	   present queue to drain, and waiting for a swapchain image to come free. This is
	   NOT render work - it is the pacer holding the render loop back - and it has to be
	   subtracted out before the frame interval is used to schedule the next group. See
	   the RUNAWAY note further down for why that subtraction is load-bearing. */
	uint64_t fg_stall_us = 0;

	/* The swapchain image holding the REAL frame. It is now the image R_BeginFrame
	   acquired, blitted BEFORE DLSSGApply - see the note at that blit. */
	uint32_t fg_real_image_index = 0;
	bool fg_real_blitted = false;


	if (frame_ready)
	{
		if (vkpt_fsr_is_enabled() && !qvk.frame_menu_mode)
		{
			vkpt_fsr_final_blit(cmd_buf);
		}
		else if (qvk.effective_aa_mode == AA_MODE_UPSCALE)
		{
			vkpt_final_blit_simple(cmd_buf, qvk.images[VKPT_IMG_TAA_OUTPUT], qvk.extent_taa_output);
		}
		else if (DLSSEnabled()) {

			BEGIN_PERF_MARKER(cmd_buf, PROFILER_BLOOM);
			if (cvar_bloom_enable->integer != 0 || qvk.frame_menu_mode)
			{
				vkpt_bloom_record_cmd_buffer(cmd_buf);
			}
			END_PERF_MARKER(cmd_buf, PROFILER_BLOOM);

			BEGIN_PERF_MARKER(cmd_buf, PROFILER_TONE_MAPPING);
			if (cvar_tm_enable->integer != 0)
			{
				vkpt_tone_mapping_record_cmd_buffer(cmd_buf, lastFrameTime <= 0.f ? lastWallClocktime : lastFrameTime, DLSSEnabled());
			}
			END_PERF_MARKER(cmd_buf, PROFILER_TONE_MAPPING);

			
			// Frame generation runs here, on the FINAL pre-HUD image: DLSS output after
			// bloom and tone mapping, and before vkpt_draw_submit_stretch_pics() paints
			// the HUD into the swapchain. That ordering is what makes VKPT_IMG_DLSS_OUTPUT
			// a usable HUD-less colour buffer.
			//
			// NOTE: this only PRODUCES the interpolated frame. Presenting it needs a
			// second, timed vkQueuePresentKHR roughly halfway between two real frames,
			// which does not exist yet - there is exactly one present per rendered frame
			// further down. Until then "pt_dlss_fg_show 1" displays the interpolated
			// image in place of the real one so it can be judged by eye.
			if (DLSSGEnabled()) {
				/* HOW MANY GENERATED FRAMES CAN THIS DISPLAY ACTUALLY SHOW?

				   A fixed-refresh display shows at most one frame per refresh interval. Presenting
				   more than that does not raise the frame rate the eye receives - the display just
				   samples the group at whatever phase it drifts into, and because the group repeats
				   once per RENDERED frame, the motion you perceive collapses back to the base rate.
				   That is what 4x at a 35 fps base looks like on a 60Hz panel: 140 presents/s that
				   read as 35, plus the interpolation artifacts and latency of 4x. pt_dlss_fg_debug
				   shows it directly - one colour holds, then drifts, rather than cycling evenly.

				   So cap the generated frames at what fits: round(render interval / refresh
				   interval) presents per group. base 15 -> 4x, base 20 -> 3x, base 30 -> 2x, which
				   is the multiplier table that was previously left to the player to get right.

				   This is deliberately NOT a swapchain change - desired_swapchain_images() keys off
				   DLSSGMultiplier(), which stays at what the user asked for, so the image count is
				   unchanged and nothing recreates. Under FIFO the interval is already
				   multiplier x refresh, so the budget equals the multiplier and this is a no-op.

				   Floored at 1 generated frame: if FG is on the player gets at least 2x rather than
				   having it silently switch itself off, which would oscillate around the boundary. */
				{
					static cvar_t *cv_adapt = NULL;
					if (!cv_adapt) cv_adapt = Cvar_Get("pt_dlss_fg_adapt", "0", CVAR_ARCHIVE);
				   /* DEFAULT 0 - THIS SERVO DOES NOT CONVERGE, do not switch it back on
				      without reading this.

				      The budget is computed from the render interval, but CHANGING the
				      budget changes the render interval - each extra generated frame is
				      another Evaluate and another present. So the control input moves the
				      measured variable and it oscillates. A deadband and a one-second dwell
				      only slowed it down: Matt's log still shows 2x/3x alternating once a
				      second, 81 changes in 89 seconds, saturating the dwell.

				      A group whose size keeps changing cannot be paced, so this was doing
				      more harm than the mismatch it was meant to correct. The multiplier
				      table further down this file is guidance for the PLAYER; picking it
				      automatically needs a measurement that does not depend on the choice
				      (GPU time with FG excluded, say), which does not exist here yet. */

					unsigned int budget = 0;   /* 0 = no limit */
				int hz = Reflex_DisplayRefreshHz();
				if (cv_adapt->integer && hz > 0 && fg_render_interval_us > 0.0) {
					double refresh_us = 1000000.0 / (double)hz;
					double ratio = fg_render_interval_us / refresh_us;   /* presents that fit */

					/* HYSTERESIS IS NOT OPTIONAL HERE.

					   The first version of this rounded `ratio` every frame, and with the
					   interval sitting near a boundary it flapped 2x <-> 3x on ALTERNATE
					   FRAMES - 44 multiplier changes in one short session in Matt's log.
					   A group whose SIZE changes every frame cannot be paced: the slot
					   count, the group duration and the swapchain acquire all move
					   together, so the schedule never settles and the debug colours read
					   as noise. It was almost certainly the worst of the three problems
					   and it was one I introduced.

					   So: a deadband wide enough that ordinary interval jitter cannot
					   cross it, one step at a time, and at most one change a second. */
					static int      cur_presents   = 0;
					static uint64_t last_change_us = 0;
					uint64_t now_cmp = Sys_Microseconds();

					if (cur_presents == 0) {
						/* First look: go straight to the right answer rather than
						   climbing to it one step per second. */
						cur_presents = (int)(ratio + 0.5);
						if (cur_presents < 2) cur_presents = 2;
						last_change_us = now_cmp;
					}
					else {
						int want = cur_presents;
						if (ratio > (double)cur_presents + 0.35)
							want = cur_presents + 1;
						else if (ratio < (double)cur_presents - 0.35)
							want = cur_presents - 1;
						if (want < 2)
							want = 2;

						if (want != cur_presents && now_cmp - last_change_us > 1000000) {
							cur_presents = want;
							last_change_us = now_cmp;
						}
					}

					budget = (unsigned int)(cur_presents - 1);
				}
				DLSSGSetFrameBudget(budget);
				}

				/* Reads the PREVIOUS frame's finished images, before this frame overwrites
				   them. pt_dlss_fg_compare 1, once a second. */
				fg_debug_compare_frames(DLSSGGeneratedFrames());

				/* RETAIN THE REAL FRAME BEFORE EVALUATE, NOT AFTER.
				
				   The real frame used to be blitted from VKPT_IMG_DLSS_OUTPUT *after* DLSSGApply,
				   and that image is the backbuffer handed to DLSS-G. NVIDIA's guide is explicit
				   that manual retention is only safe if the architecture GUARANTEES the buffer is
				   not overwritten, and nothing here guaranteed it. Measured: all 7 swapchain images
				   held GENERATED content (0.14-0.87 from the generated image, 10.2 from the real
				   one) even though the present layer named a different image for the real frame
				   every group. No real frame ever reached the screen - which is why there was no
				   tearing with vsync off and why motion stayed at the base rate however well the
				   presents were paced. Matt called this from the symptoms long before it was
				   measured.
				
				   So take the copy FIRST, into the image R_BeginFrame already acquired. The
				   generated frames go into the extra images instead - the same number of acquires
				   either way, and the present order (generated first, real last) is set by the
				   enqueue, not by which image plays which role. Correct regardless of whether
				   DLSS-G actually clobbers the backbuffer: it is the ordering the SDK asks for. */
				if (DLSSGFeatureReady() && !DLSSGShowInterpolated()
				    && DLSSGGeneratedFrames() > 0 && qvk.device_count == 1)
				{
					fg_real_image_index = qvk.current_swap_chain_image_index;
					vkpt_final_blit_simple(cmd_buf, GetDLSSImage(), GetDLSSExtent());
					if (fg_real_image_index < LENGTH(fg_image_role))
						fg_image_role[fg_real_image_index] = FG_ROLE_REAL;
					fg_real_blitted = true;
				}

				DLSSGApply(cmd_buf, dlssg_reset_history ? qtrue : qfalse);
				dlssg_reset_history = false;
			}

			/* Real frame generation: present every interpolated frame, THEN the real one.
			   The interpolated frames represent moments between the previous real frame and
			   this one, so they all go out first, in index order.
			
			   An Nx multiplier needs N swapchain images: the one R_BeginFrame already took
			   holds generated frame 1, and N-1 more are acquired here along with one for the
			   real frame. Acquiring at this point rather than up front matters: by now the
			   frame is known to be presentable, so no path acquires an image and then fails
			   to present it, which would starve the swapchain. If any acquire fails we fall
			   back to the ordinary single present - nothing has been retargeted yet, and the
			   images acquired so far are still presented, just without interpolation. */
			unsigned int fg_want = DLSSGGeneratedFrames();
			if (fg_want > 0 && DLSSGFeatureReady() && !DLSSGShowInterpolated()
			    && qvk.device_count == 1)
			{
				/* fg_want extra images: fg_want-1 for the remaining generated frames, plus
				   one for the real frame, which is always the last one acquired. */
				/* Vulkan only permits numImages - minImageCount + 1 images to be acquired at
				   once, and a queued present still holds its image. Wait for just enough of the
				   backlog to clear that this group fits, rather than draining completely - a
				   full drain would collapse the one-frame scheduling lead below, which is what
				   lets the GPU finish a frame before its first present falls due. */
				{
					/* Under FIFO every queued present costs a whole refresh interval of latency, so
					   let the previous group finish completely before starting the next: the driver
					   then never holds more than one group and latency stops accumulating with the
					   swapchain image count. This also throttles the render rate to refresh /
					   multiplier, which under FIFO is what it would settle at anyway.
					
					   Outside FIFO nothing is waiting on vblank, so only the acquire limit matters
					   and a deeper queue is free throughput. */
					unsigned int hold_limit = (qvk.surf_num_images > 2)
						? (unsigned int)qvk.surf_num_images - 2u : 1u;
					unsigned int group = fg_want + 1u;
					unsigned int max_pending = (hold_limit > group) ? hold_limit - group : 0u;

					/* Deliberately NOT 0 under FIFO. Waiting for the entire group to drain couples
					   the main thread's frame time to the group duration, which is the other half of
					   the runaway described above. The acquire limit alone is enough. */

					uint64_t stall_begin_us = Sys_Microseconds();
					FGPresent_WaitUntilPending(max_pending);
					fg_stall_us += Sys_Microseconds() - stall_begin_us;
				}

				uint32_t fg_extra[DLSSG_MAX_GENERATED_FRAMES];
				unsigned int fg_acquired = 0;
				bool fg_acquire_ok = true;

				uint64_t acquire_begin_us = Sys_Microseconds();
				for (unsigned int i = 0; i < fg_want; i++) {
					VkResult res_fg = acquire_next_image_locked(
						qvk.semaphores[qvk.current_frame_index][0].image_available_fg[i],
						&fg_extra[i]);

					if (res_fg != VK_SUCCESS && res_fg != VK_SUBOPTIMAL_KHR) {
						static bool fg_acquire_warned = false;
						if (!fg_acquire_warned) {
							fg_acquire_warned = true;
							Com_WPrintf("DLSS-G: swapchain acquire %u/%u failed (%d),"
								" falling back to single present\n", i + 1, fg_want, res_fg);
						}
						fg_acquire_ok = false;
						break;
					}
					fg_acquired++;
				}
				fg_stall_us += Sys_Microseconds() - acquire_begin_us;

				/* A short acquire is not fatal: with fg_acquired extra images we can still show
				   fg_acquired generated frames plus the real one, using the images we did get.
				   DLSS-G already produced all fg_want of them; the surplus is simply not shown
				   this frame. Only fg_acquired == 0 falls all the way back to a single present.
				   Doing it this way means every image acquired above is written and presented,
				   so none can be orphaned. */
				(void)fg_acquire_ok;
				unsigned int fg_show = fg_acquired;

				if (fg_show > 0) {
					/* Generated frame 1 goes into the image R_BeginFrame acquired; the rest go
					   into fg_extra[0 .. fg_show-2]. fg_extra[fg_show-1] takes the real frame. */
					/* Every generated frame goes into an extra image now; the real frame already
					   owns the one R_BeginFrame acquired. Same number of acquires as before. */
					for (unsigned int i = 0; i < fg_show; i++)
						fg_interp_image_index[i] = fg_extra[i];

					/* Each interpolated frame + the HUD. _keep leaves the queued pics in place
					   so the same HUD can be drawn again into every later image; without it the
					   HUD would only appear on the first presented frame of each group. */
					for (unsigned int i = 0; i < fg_show; i++) {
						qvk.current_swap_chain_image_index = fg_interp_image_index[i];
						vkpt_final_blit_simple(cmd_buf, GetDLSSGImage(i + 1), GetDLSSExtent());
						if (qvk.current_swap_chain_image_index < LENGTH(fg_image_role))
							fg_image_role[qvk.current_swap_chain_image_index] = FG_ROLE_GEN;

						{
							static cvar_t *cv_fgdbg = NULL;
							if (!cv_fgdbg) cv_fgdbg = Cvar_Get("pt_dlss_fg_debug", "0", 0);
							if (cv_fgdbg->integer)
								fg_debug_paint_current_swapchain_image(cmd_buf, i);
						}

						vkpt_draw_submit_stretch_pics_keep(cmd_buf);
					}

					fg_generated_count = fg_show;

					/* One-shot: proves frame generation actually engaged at the requested
					   multiplier rather than silently falling back. */
					static unsigned int fg_announced_mult = 0;
					if (fg_announced_mult != fg_show + 1) {
						fg_announced_mult = fg_show + 1;
						Com_Printf("DLSS-G: %ux ACTIVE - %u generated + 1 real = %u presents"
							" per rendered frame, %u swapchain images\n",
							fg_show + 1, fg_show, fg_show + 1, qvk.surf_num_images);
					}

					/* Everything from here on - the real blit below and the HUD pass after the
					   branch - targets the last acquired image instead. */
					qvk.current_swap_chain_image_index = fg_real_image_index;
				}
			}

			/* THE CONTROL. Runs whether or not frame generation is on, because the whole
			   "no real frame reaches the swapchain" finding rests on VKPT_IMG_DLSS_OUTPUT
			   being the real frame, and that has never been verified. With pt_dlss_fg 0 there
			   is no generated content anywhere, so every swapchain image MUST match it. If it
			   does, the reference is sound and the FG path corrupts the copy. If it does not,
			   the reference is the wrong image and every conclusion drawn from it is void -
			   including that one. Matt said the real dump did not look like the game, and that
			   was set aside too readily. */
			fg_debug_dump_frames(DLSSGEnabled() ? DLSSGGeneratedFrames() : 0);

			if (DLSSGShowInterpolated())
				vkpt_final_blit_simple(cmd_buf, GetDLSSGImage(1), GetDLSSExtent());
			else if (!fg_real_blitted)
				{
				/* Only when frame generation did not already take the copy above. */
				vkpt_final_blit_simple(cmd_buf, GetDLSSImage(), GetDLSSExtent());
				if (qvk.current_swap_chain_image_index < LENGTH(fg_image_role))
					fg_image_role[qvk.current_swap_chain_image_index] = FG_ROLE_REAL;
			}
		}
		else
		{
			VkExtent2D extent_unscaled_half;
			extent_unscaled_half.width = qvk.extent_unscaled.width / 2;
			extent_unscaled_half.height = qvk.extent_unscaled.height / 2;

			if (extents_equal(qvk.extent_render, qvk.extent_unscaled) ||
				(extents_equal(qvk.extent_render, extent_unscaled_half) && drs_effective_scale == 0)) // don't do nearest filter 2x upscale with DRS enabled
				vkpt_final_blit_simple(cmd_buf, qvk.images[VKPT_IMG_TAA_OUTPUT], qvk.extent_taa_output);
			else
				vkpt_final_blit_filtered(cmd_buf);
		}

		frame_ready = false;
	}

	vkpt_draw_submit_stretch_pics(cmd_buf);

	/* Room for one extra entry each: the second acquire to wait on, and the second
	   present to signal. A binary semaphore can only be waited on once, so the two
	   presents cannot share render_finished. */
	enum { FG_SEM_SLOTS = DLSSG_MAX_GENERATED_FRAMES + 1 };
	VkSemaphore wait_semaphores[FG_SEM_SLOTS];
	VkPipelineStageFlags wait_stages[FG_SEM_SLOTS];
	uint32_t wait_device_indices[FG_SEM_SLOTS];
	for (int i = 0; i < FG_SEM_SLOTS; i++) {
		wait_semaphores[i] = VK_NULL_HANDLE;
		wait_stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		wait_device_indices[i] = 0;
	}
	wait_semaphores[0] = qvk.semaphores[qvk.current_frame_index][0].image_available;
	int wait_semaphore_count = 1;

	/* Single GPU presents wait on the per-IMAGE semaphores; the multi-GPU device-group
	   path keeps its per-GPU render_finished semaphores untouched. */
	bool per_image_present_sems = (qvk.device_count == 1 && qvk.semaphores_present != NULL);

	VkSemaphore signal_semaphores[VKPT_MAX_GPUS + DLSSG_MAX_GENERATED_FRAMES];
	uint32_t signal_device_indices[VKPT_MAX_GPUS + DLSSG_MAX_GENERATED_FRAMES];
	int signal_semaphore_count;

	/* With frame generation the present thread establishes readiness through a TIMELINE
	   semaphore and presents with no wait semaphore, so these binary per-image present
	   semaphores must NOT be signalled - nothing would ever wait on them. See
	   fg_present.h: signalling them is what made every present in a group become
	   presentable at the same instant and flip in pairs. */
	const bool fg_thread_presents = (fg_generated_count > 0)
	                             && FGPresent_TimelineAvailable();

	if (fg_thread_presents)
	{
		signal_semaphore_count = 0;
	}
	else if (per_image_present_sems)
	{
		signal_semaphores[0] = qvk.semaphores_present[qvk.current_swap_chain_image_index];
		signal_device_indices[0] = 0;
		signal_semaphore_count = 1;
	}
	else
	{
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			signal_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].render_finished;
			signal_device_indices[gpu] = gpu;
		}
		signal_semaphore_count = qvk.device_count;
	}

	/* One extra acquire to wait on, and one extra present to signal, per generated
	   frame. Each interpolated frame lives in its own swapchain image and so gets
	   that image's own present semaphore; frame generation only ever runs on the
	   single-GPU path, so per_image_present_sems is necessarily true here. */
	for (unsigned int i = 0; i < fg_generated_count; i++)
	{
		wait_semaphores[wait_semaphore_count] = qvk.semaphores[qvk.current_frame_index][0].image_available_fg[i];
		wait_semaphore_count++;

		if (!fg_thread_presents)
		{
			signal_semaphores[signal_semaphore_count] = qvk.semaphores_present[fg_interp_image_index[i]];
			signal_device_indices[signal_semaphore_count] = 0;
			signal_semaphore_count++;
		}
	}

	vkpt_submit_command_buffer(
		cmd_buf,
		qvk.queue_graphics,
		(1 << qvk.device_count) - 1,
		wait_semaphore_count, wait_semaphores, wait_stages, wait_device_indices,
		signal_semaphore_count, signal_semaphores, signal_device_indices,
		qvk.fences_frame_sync[qvk.current_frame_index]);

	/* Right after the frame submit, so it completes when the frame's work does. */
	uint64_t fg_timeline_value = fg_thread_presents
	                           ? FGPresent_SignalTimeline(qvk.queue_graphics) : 0;

	Reflex_SetMarker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);


#ifdef VKPT_IMAGE_DUMPS
	if (cvar_dump_image->integer) {
		_VK(vkpt_queue_wait_idle(qvk.queue_graphics));

		VkImageSubresource subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.arrayLayer = 0,
			.mipLevel = 0
		};

		VkSubresourceLayout subresource_layout;
		vkGetImageSubresourceLayout(qvk.device, qvk.dump_image, &subresource, &subresource_layout);

		void *data;
		_VK(vkMapMemory(qvk.device, qvk.dump_image_memory, 0, qvk.dump_image_memory_size, 0, &data));
		save_to_pfm_file("color_buffer", qvk.frame_counter, IMG_WIDTH, IMG_HEIGHT, (char *)data, subresource_layout.rowPitch, 0);
		vkUnmapMemory(qvk.device, qvk.dump_image_memory);

		Cvar_SetInteger(cvar_dump_image, 0, FROM_CODE);
	}
#endif

	/* Tag the present with this frame's Reflex id so the driver can tie it back to
	   the markers above. Harmless when Reflex is off. */
	uint64_t reflex_present_id = Reflex_CurrentPresentID();
	VkPresentIdKHR present_id_info = {
		.sType          = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
		.swapchainCount = 1,
		.pPresentIds    = &reflex_present_id,
	};

	VkPresentInfoKHR present_info = {
		.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pNext              = (Reflex_PresentIdAvailable() && reflex_present_id)
			? &present_id_info : NULL,
		.waitSemaphoreCount = per_image_present_sems ? 1 : (uint32_t)qvk.device_count,
		.pWaitSemaphores    = per_image_present_sems
			? &qvk.semaphores_present[qvk.current_swap_chain_image_index]
			: signal_semaphores,
		.swapchainCount     = 1,
		.pSwapchains        = &qvk.swap_chain,
		.pImageIndices      = &qvk.current_swap_chain_image_index,
		.pResults           = NULL,
	};

#ifdef VKPT_DEVICE_GROUPS
	uint32_t present_device_mask = 1;
	VkDeviceGroupPresentInfoKHR group_present_info = {
		.sType				= VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,
		.swapchainCount		= 1,
		.pDeviceMasks		= &present_device_mask,
		.mode				= VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR,
	};

	if (qvk.device_count > 1) {
		present_info.pNext = &group_present_info;
	}
#endif

	/* The interpolated frame goes out FIRST. Both presents wait on the same submit, so
	   the only thing establishing their order is the order of these two calls.
	
	   No explicit sleep between them: under FIFO the presentation engine already spaces
	   consecutive presents one refresh interval apart, which is exactly the pacing FG
	   wants as long as the renderer keeps up with half the refresh rate. Pacing only
	   needs solving explicitly for the non-FIFO modes and for the case where it does
	   not - see the notes on present pacing. */
	/* ---- Frame generation: hand the whole group to the present thread ----
	
	   The group is [generated 1 .. generated N, real], spread evenly across one frame
	   interval. Issuing them inline does not work: measured, all six of a 6x group
	   landed within ~300us of a 5000us frame, so the display only ever scanned out the
	   last one and every generated frame was discarded unseen.
	
	   The schedule is kept in absolute time and simply advanced by one interval per
	   group, so it self-corrects: if the renderer falls behind, the deadlines are
	   already in the past and everything goes out immediately. */
	bool fg_presented_by_thread = false;

	if (fg_generated_count > 0)
	{
		static uint64_t fg_sched_next_us = 0;
		static uint64_t fg_prev_group_us = 0;

		uint64_t now_us = Sys_Microseconds();

		/* Smoothed interval between rendered frames, with absurd deltas ignored so a
		   level load or a breakpoint does not poison the average.
		
		   SUBTRACT THE STALL - this is the fix for "the base rate never recovers".
		
		   The raw group-to-group delta is NOT the render capability: the main thread is
		   deliberately held back by FGPresent_WaitUntilPending() and by the swapchain
		   acquire, both of which are paced by this very schedule. Feeding that delta back
		   in makes the loop self-referential - the group duration sets the frame time and
		   the frame time sets the group duration - so it is a fixed point at ANY rate. It
		   can only ever ratchet DOWN (a hitch widens the interval permanently) and has no
		   term at all that responds to the GPU getting faster. That is why switching to a
		   lighter DLSS mode did not raise the frame rate: nothing in the schedule could
		   observe the extra headroom.
		
		   `dt - fg_stall_us` is what the frame actually needed with the pacer removed, so
		   the interval converges on what the GPU can really do and the schedule opens up
		   to match. */
		if (fg_prev_group_us != 0) {
			double raw = (double)(now_us - fg_prev_group_us);
			double dt  = raw - (double)fg_stall_us;
			if (raw > 100.0 && raw < 250000.0)
				fg_frame_interval_us = (fg_frame_interval_us > 0.0)
					? fg_frame_interval_us * 0.85 + raw * 0.15 : raw;
			if (dt > 100.0 && dt < 250000.0)
				fg_render_interval_us = (fg_render_interval_us > 0.0)
					? fg_render_interval_us * 0.85 + dt * 0.15 : dt;
		}
		fg_prev_group_us = now_us;

		unsigned int total = fg_generated_count + 1;
		double interval = (fg_render_interval_us > 0.0) ? fg_render_interval_us : 16666.0;

		/* ORDER MATTERS HERE, and getting it wrong is what produced 14.2 ms present gaps
		   on a 16.67 ms display. The slot, the group duration and the floor all have to be
		   settled BEFORE the anchor, because the anchor's re-anchor window and the floor
		   are both expressed in terms of them. Previously the anchor was computed first
		   from `interval`, which at 30 fps with a 30 ms stall is about 3 ms, so the window
		   collapsed and the group was re-anchored every single frame. */

		/* ---- 1. the slot: how far apart consecutive presents should be ---- */
		double slot = interval / (double)total;

		int refresh_hz = Reflex_DisplayRefreshHz();
		if (refresh_hz > 0) {
			double refresh_slot = 1000000.0 / (double)refresh_hz;

			if (qvk.present_mode == VK_PRESENT_MODE_FIFO_KHR) {
				/* FIFO retires one present per vblank, so the presented rate is the
				   refresh rate whatever we ask for. Pin the slot to it rather than derive
				   it from our own frame rate, and no feedback path is left to run away.
				   Asking for less is actively harmful: the present thread would block
				   inside vkQueuePresentKHR holding the swapchain lock. */
				slot = refresh_slot;
			}
			else if (refresh_slot < slot) {
				/* Never stretch a group wider than the display can resolve. */
				slot = refresh_slot;
			}
		}

		/* ---- 2. the display's own clock, which overrides both ---- */
		static cvar_t *cv_vblank = NULL;
		if (!cv_vblank) cv_vblank = Cvar_Get("pt_dlss_fg_vblank", "1", CVAR_ARCHIVE);
		static cvar_t *cv_vboffset = NULL;
		if (!cv_vboffset) cv_vboffset = Cvar_Get("pt_dlss_fg_vblank_offset", "500", 0);

		uint64_t vb_us = 0;
		double   vb_period_us = 0.0;
		bool     phase_locked = cv_vblank->integer
			&& qvk.present_mode != VK_PRESENT_MODE_FIFO_KHR
			&& FGPresent_VBlankInfo(&vb_us, &vb_period_us)
			&& vb_period_us >= 4000.0 && vb_period_us <= 100000.0;

		if (phase_locked)
			slot = vb_period_us;

		double group_us = slot * (double)total;

		/* ---- 3. the anchor ---- */
		static cvar_t *cv_lead = NULL;
		if (!cv_lead) cv_lead = Cvar_Get("pt_dlss_fg_lead", "1.0", 0);   /* NOT archived.

			   Every present waits on a semaphore for GPU work submitted moments earlier.
			   With no lead the deadlines are all in the PAST by the time that work
			   finishes, so they steer nothing: the group unblocks together and the display
			   shows one frame per RENDERED frame - the base rate, with no tearing, because
			   there is effectively one flip per refresh. An earlier sweep chose 0 on a
			   latency measurement taken before any of this worked, i.e. it optimised
			   latency while the pacing was broken. The lead IS one frame of latency and it
			   is what frame generation inherently costs. */
		double lead = cv_lead->value;
		if (lead < 0.0) lead = 0.0;
		if (lead > 2.0) lead = 2.0;

		/* The lead must cover submit -> this frame's GPU work finished, and neither EMA is
		   that number. The stall-corrected interval is too short by the stall; the RAW
		   interval is too long AND is positive feedback (it contains the stall, and more
		   lead makes more stall - it ran away to 4 fps). So take the work time and add the
		   lateness the present thread measures: too little lead shows up as lateness and
		   lengthens the lead, enough lead drives it to zero. Negative feedback, converges. */
		/* USE THE GPU'S OWN CLOCK. The lead must cover submit -> this frame's GPU work
		   finished, and every proxy tried for it has failed in a different direction:
		
		     - stall-corrected interval: too SHORT. It measures the main thread's
		       unblocked CPU time, which at 4K is ~4 ms while the GPU needs ~35.
		     - raw frame interval: too LONG and POSITIVE FEEDBACK - it contains the
		       stall, more lead makes more stall, and it ran away to 4 fps.
		     - corrected + measured lateness: converges to HALF the correction. Classic
		       proportional offset - the fixed point is lateness = (GPU - corrected)/2,
		       which with GPU 35 and corrected 4 predicts ~15.5 ms and Matt's log showed
		       17.3. Right shape, no integral term, so the error never closes.
		
		   PROFILER_FRAME_TIME is the actual GPU frame time from the timestamp queries.
		   It is measured on the GPU and does not depend on when we present, so there is
		   no loop to run away and no offset to leave behind. The lateness EMA stays as a
		   small additive trim for whatever the timestamps do not cover (submit latency,
		   queue handoff); with the base right it should sit near zero. */
		double gpu_us = vkpt_get_profiler_result(PROFILER_FRAME_TIME) * 1000.0;
		double lead_interval;
		if (gpu_us > 100.0 && gpu_us < 200000.0)
			lead_interval = gpu_us + FGPresent_MeanLatenessUs();
		else
			lead_interval = interval + FGPresent_MeanLatenessUs();
		if (lead_interval < interval)
			lead_interval = interval;

		/* The re-anchor window is in GROUP durations, not in `interval`. With the stall
		   subtracted, `interval` can be a few milliseconds while the group spans a whole
		   33 ms, and a window that small re-anchors every frame - which throws away the
		   absolute schedule and leaves the floor below doing all the spacing. */
		uint64_t base_us = fg_sched_next_us;
		uint64_t anchor_us = now_us + (uint64_t)(lead_interval * lead);
		if (base_us < now_us || base_us > anchor_us + (uint64_t)(group_us * 2.0))
			base_us = anchor_us;

		/* ---- 4. snap onto a real vblank ---- */
		if (phase_locked) {
			int64_t period = (int64_t)vb_period_us;
			int64_t offset = cv_vboffset->integer;
			int64_t delta  = (int64_t)base_us - (int64_t)vb_us - offset;
			int64_t k = (delta > 0) ? ((delta + period - 1) / period) : 0;
			uint64_t snapped = vb_us + (uint64_t)(k * period) + (uint64_t)offset;

			/* Bounded, because this is a timestamp from outside the process: a stale or
			   unrelated clock must only ever cost alignment, never the frame rate. A
			   deadline far in the future holds swapchain images the acquire then spins on,
			   which is this engine's documented route to hundreds of ms per frame. */
			if (snapped >= base_us && snapped <= base_us + (uint64_t)period)
				base_us = snapped;
			else
				phase_locked = false;

			static bool vb_announced = false;
			if (!vb_announced) {
				vb_announced = true;
				Com_Printf("DLSS-G: vblank phase lock ON - period %.2f ms (%.1f Hz), "
					"offset %d us, snap %s\n",
					vb_period_us / 1000.0, 1000000.0 / vb_period_us,
					cv_vboffset->integer, phase_locked ? "accepted" : "REJECTED");
			}
		}

		/* ---- 5. the floor ---- */
		/* THE FLOOR MUST NOT BE SHORTER THAN A REFRESH WHEN PHASE-LOCKED.

		   At 0.85 of a slot it was 14.2 ms against a 16.67 ms display, and it was the
		   BINDING constraint - the measured gaps were 14235, 14221, 14225 us, not the
		   vblank spacing. 14.2 does not divide into 16.67, so the presents walked through
		   the refresh phase continuously: stretches where the display latched the same
		   slot of every group (motion at the base rate) broken by moments where it caught
		   both (a split second of real 60). That is exactly what Matt kept describing.

		   Once the deadlines are vblank-aligned the floor only has to stop genuine
		   bunching, so a full period is right: metering at the floor then still lands one
		   present per refresh instead of sliding between them. */
		uint64_t min_gap_us = phase_locked
			? (uint64_t)vb_period_us
			: (uint64_t)(slot * 0.85);

		/* TRIED AND REVERTED: letting FIFO do the pacing by presenting the whole group
		   immediately (base = now, no slot, no floor). It DOES cut latency hard - 70 ms
		   to 33 ms measured - because FIFO already spaces presents one vblank apart, so
		   the wall-clock schedule looks like pure duplicated delay.
		
		   But it is not duplicated: under FIFO vkQueuePresentKHR BLOCKS once the queue
		   is full, and the present thread holds the swapchain lock while it blocks, which
		   stalls the main thread's acquire and submit. Issuing the group back to back
		   fills the queue immediately and the render loop hitches - smoothness went away
		   even as the latency number improved. Spacing the presents keeps the thread out
		   of that blocking path.
		
		   Latency is bounded by the queue-depth limit below instead.
		   pt_dlss_fg_fifo_pacing 1 re-enables the experiment. */
		static cvar_t *cv_fifo_pacing = NULL;
		if (!cv_fifo_pacing)
			cv_fifo_pacing = Cvar_Get("pt_dlss_fg_fifo_pacing", "0", 0);   /* NOT archived */

		if (qvk.present_mode == VK_PRESENT_MODE_FIFO_KHR && cv_fifo_pacing->integer) {
			base_us = now_us;
			slot = 0.0;
			min_gap_us = 0;
		}

		/* WHICH IMAGE GETS WHAT. All seven swapchain images came back holding GENERATED
		   frames, none holding a real one, while the retarget
		   (qvk.current_swap_chain_image_index = fg_extra[fg_show - 1]) and the real blit
		   after it both read correctly. So print the indices as actually enqueued and
		   stop inferring: if the real frame names an image that a dump then shows holding
		   generated content, the blit is not landing where the present says it is. */
		{
			static cvar_t *cv_idx = NULL;
			if (!cv_idx) cv_idx = Cvar_Get("pt_dlss_fg_indices", "0", 0);
			if (cv_idx->integer > 0) {
				Cvar_SetInteger(cv_idx, cv_idx->integer - 1, FROM_CODE);
				char line[192];
				int off = Q_snprintf(line, sizeof(line), "DLSS-G images: generated");
				for (unsigned int i = 0; i < fg_generated_count; i++)
					off += Q_snprintf(line + off, sizeof(line) - off, " %u",
						fg_interp_image_index[i]);
				Q_snprintf(line + off, sizeof(line) - off, ", real %u (of %u images)",
					qvk.current_swap_chain_image_index, qvk.surf_num_images);
				Com_Printf("%s\n", line);
			}
		}

		fg_presented_by_thread = true;

		for (unsigned int i = 0; i < fg_generated_count; i++) {
			uint64_t target_us = base_us + (uint64_t)(slot * (double)i);

			if (!FGPresent_Enqueue(qvk.swap_chain, fg_interp_image_index[i],
				qvk.semaphores_present[fg_interp_image_index[i]], target_us, min_gap_us, 0,
				qvk.frame_counter, fg_timeline_value))
			{
				/* Queue full or no thread: present inline rather than drop the frame,
				   which would leave its semaphore signalled and its image never
				   released back to the swapchain. */
				/* With the timeline path on, this frame's submit signals NO binary
				   present semaphore, so waiting on one here would stall the
				   presentation engine forever. Establish readiness on the host the
				   same way the present thread does. */
				bool ready = fg_timeline_value != 0
					&& FGPresent_WaitTimeline(fg_timeline_value);
				VkPresentInfoKHR fallback = {
					.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
					.waitSemaphoreCount = ready ? 0 : 1,
					.pWaitSemaphores    = ready ? NULL
						: &qvk.semaphores_present[fg_interp_image_index[i]],
					.swapchainCount     = 1,
					.pSwapchains        = &qvk.swap_chain,
					.pImageIndices      = &fg_interp_image_index[i],
					.pResults           = NULL,
				};
				FGPresent_SwapchainLock();
				Reflex_NotifyOutOfBandPresent(qvk.queue_present);
				vkQueuePresentKHR(qvk.queue_present, &fallback);
				FGPresent_SwapchainUnlock();
			}
		}

		/* The real frame is the LAST of the group in time - the generated frames sit
		   between the previous real frame and this one. */
		uint64_t real_target_us = base_us + (uint64_t)(slot * (double)fg_generated_count);
		if (!FGPresent_Enqueue(qvk.swap_chain, qvk.current_swap_chain_image_index,
			qvk.semaphores_present[qvk.current_swap_chain_image_index], real_target_us,
			min_gap_us, Reflex_CurrentPresentID(), qvk.frame_counter, fg_timeline_value))
		{
			fg_presented_by_thread = false;   // fall through to the inline present below
		}

		/* Advance by the GROUP DURATION (total x slot), not by the measured render
		   interval. `slot` is clamped to the display refresh above, so the group rate is
		   then anchored to the display and NOT to our own frame rate - which is the
		   whole point of the clamp. Advancing by `interval` here left the group rate
		   tied to the render rate, and since the render rate is in turn limited by how
		   fast we present, the loop closed again one level up: 2x settled at 22 rendered
		   / 44 presented on a 60Hz display that could take 30 / 60, and stayed there. */
		fg_sched_next_us = base_us + (uint64_t)(slot * (double)total);
	}

	/* Tell the client how many presents this rendered frame produced, so the FPS
	   readout can report presented frames as well as rendered ones. */
	if (vkpt_refdef.fd)
		vkpt_refdef.fd->feedback.presented_frames = (int)(fg_generated_count + 1);

	/* When the present thread owns this frame's group the real present is already
	   queued with the rest of it; issuing it again here would present the image
	   twice and wait on an already-consumed semaphore. Collect the thread's most
	   recent failure instead, one frame late, which is soon enough to recreate. */
	VkResult res_present;
	if (fg_presented_by_thread) {
		res_present = FGPresent_TakeLastResult();
	} else {
		/* Same hazard as the generated-frame fallback above: when the frame submit went
		   out with no binary present semaphore, this present must not wait on one. */
		if (fg_timeline_value != 0 && FGPresent_WaitTimeline(fg_timeline_value)) {
			present_info.waitSemaphoreCount = 0;
			present_info.pWaitSemaphores    = NULL;
		}
		Reflex_SetMarker(VK_LATENCY_MARKER_PRESENT_START_NV);
		FGPresent_SwapchainLock();
		res_present = vkQueuePresentKHR(qvk.queue_graphics, &present_info);
		FGPresent_SwapchainUnlock();
		Reflex_SetMarker(VK_LATENCY_MARKER_PRESENT_END_NV);
	}
	if(res_present == VK_ERROR_OUT_OF_DATE_KHR || res_present == VK_SUBOPTIMAL_KHR
	   || res_present == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT || DLSSChanged()) {
		swapchain_reason = (res_present == VK_ERROR_OUT_OF_DATE_KHR) ? "present: OUT_OF_DATE" :
			(res_present == VK_SUBOPTIMAL_KHR) ? "present: SUBOPTIMAL" :
			(res_present == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) ? "present: FSE MODE LOST" :
			"DLSS settings changed";
		DLSSSwapChainRecreated();
		recreate_swapchain();
	}
	/* Rendered-vs-presented rate, so "frame generation looks like the base rate" can be
	   answered with a number: if the rendered rate collapses when FG is switched on,
	   the interpolation is costing exactly the frames it produces. */
	{
		static cvar_t *cv_stats = NULL;
		if (!cv_stats) cv_stats = Cvar_Get("pt_dlss_fg_stats", "0", 0);
		if (cv_stats->integer) {
			static uint64_t win_start_us = 0;
			static unsigned int win_frames = 0;
			static unsigned int win_presents = 0;
			static uint64_t win_stall_us = 0;
			uint64_t t = Sys_Microseconds();
			if (win_start_us == 0) win_start_us = t;
			win_frames++;
			win_presents += fg_generated_count + 1;
			win_stall_us += fg_stall_us;
			if (t - win_start_us >= 2000000) {
				double secs = (double)(t - win_start_us) / 1000000.0;
				/* `stall` is how much of each frame the PACER held the render loop back.
				   A large stall with a low rendered/s means the schedule, not the GPU, is
				   the limit - under FIFO that is expected (refresh / multiplier), under
				   IMMEDIATE it is a bug. */
				Com_Printf("DLSS-G rate: %.1f rendered/s, %.1f presented/s (x%.2f), stall %.1f ms/frame\n",
					win_frames / secs, win_presents / secs,
					win_frames ? (double)win_presents / (double)win_frames : 1.0,
					win_frames ? (double)win_stall_us / (double)win_frames / 1000.0 : 0.0);
				win_start_us = t; win_frames = 0; win_presents = 0; win_stall_us = 0;
			}
		}
	}

	qvk.frame_counter++;
}

VkExtent2D GetDLSSExtent() {
	if (Cvar_Get("pt_dlss_debug", "0", CVAR_ARCHIVE)->integer == 0) {
		return qvk.extent_unscaled;
	}
	else {
		return qvk.extent_taa_output;
	}
}

VkImage GetDLSSImage() {
	VkImage DisplayImage = NULL;

	switch (Cvar_Get("pt_dlss_debug", "0", CVAR_ARCHIVE)->integer) {
	
	case 1:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_REFLECT_MOTION];
		break;
	case 2:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_ALBEDO];
		break;
	case 3:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_ROUGHNESS];
		break;
	case 4:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_NORMAL];
		break;
	case 5:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_SPECULAR_ALBEDO];
		break;
	case 6:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_DEPTH];
		break;
	case 7:
		DisplayImage = qvk.images[VKPT_IMG_PT_DLSS_MOTION];
		break;
	case 8:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_BEFORE_TRANSPARENT];
		break;
	case 9:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_RAYLENGTH_DIFFUSE];
		break;
	case 10:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_RAYLENGTH_SPECULAR];
		break;
	case 11:
		DisplayImage = qvk.images[VKPT_IMG_PT_TRANSPARENT];
		break;
	case 12:
		DisplayImage = qvk.images[VKPT_IMG_PT_MOTION];
		break;
	case 13:
		DisplayImage = qvk.images[VKPT_IMG_PT_DLSS_MOTION];
		break;
	case 14:
		DisplayImage = qvk.images[VKPT_IMG_ASVGF_HIST_COLOR_HF];
		break;
	case 15:
		DisplayImage = qvk.images[VKPT_IMG_PT_SHADING_POSITION];
		break;
	case 16:
		DisplayImage = qvk.images[VKPT_IMG_FLAT_COLOR];
		break;
	case 17:
		DisplayImage = qvk.images[VKPT_IMG_FLAT_MOTION];
		break;
	case 18:
		DisplayImage = qvk.images[VKPT_IMG_TAA_OUTPUT];
		break;
	case 19:
		DisplayImage = qvk.images[VKPT_IMG_PT_THROUGHPUT];
		break;
	case 20:
		DisplayImage = qvk.images[VKPT_IMG_PT_BOUNCE_THROUGHPUT];
		break;
	case 21:
		DisplayImage = qvk.images[VKPT_IMG_HQ_COLOR_INTERLEAVED];
		break;
	case 22:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_REFLECTED_ALBEDO];
		break;
	case 23:
		DisplayImage = qvk.images[VKPT_IMG_PT_COLOR_LF_SH];
		break;
	case 24:
		DisplayImage = qvk.images[VKPT_IMG_PT_COLOR_HF];
		break;
	case 25:
		DisplayImage = qvk.images[VKPT_IMG_PT_COLOR_SPEC];
		break;
	case 26:
		DisplayImage = qvk.images[VKPT_IMG_PT_GEO_NORMAL2];
		break;
	case 27:
		DisplayImage = qvk.images[VKPT_IMG_PT_VISBUF_PRIM_B];
		break;
	case 28:
		DisplayImage = qvk.images[VKPT_IMG_PT_VISBUF_PRIM_A];
		break;
	case 29:
		DisplayImage = qvk.images[VKPT_IMG_PT_VISBUF_BARY_B];
		break;
	case 30:
		DisplayImage = qvk.images[VKPT_IMG_PT_VISBUF_BARY_A];
		break;
	case 31:
		DisplayImage = qvk.images[VKPT_IMG_PT_BASE_COLOR_B];
		break;
	case 32:
		DisplayImage = qvk.images[VKPT_IMG_PT_BASE_COLOR_A];
		break;
	case 33:
		DisplayImage = qvk.images[VKPT_IMG_PT_METALLIC_B];
		break;
	case 34:
		DisplayImage = qvk.images[VKPT_IMG_PT_METALLIC_A];
		break;
	case 35:
		DisplayImage = qvk.images[VKPT_IMG_PT_VIEW_DEPTH_B];
		break;
	case 36:
		DisplayImage = qvk.images[VKPT_IMG_PT_VIEW_DEPTH_A];
		break;
	default:
		DisplayImage = qvk.images[VKPT_IMG_DLSS_OUTPUT];
		break;
	}

	return DisplayImage;
}

void
R_ModeChanged_RTX(int width, int height, int flags, int rowbytes, void *pixels)
{
	Com_DPrintf("mode changed %d %d\n", width, height);

	r_config.width  = width;
	r_config.height = height;
	r_config.flags  = flags;

	qvk.wait_for_idle_frames = MAX_FRAMES_IN_FLIGHT * 2;
}

static void
vkpt_show_pvs(void)
{
	if (!vkpt_refdef.fd)
		return;

	if (vkpt_refdef.fd->feedback.lookatcluster < 0)
	{
		memset(cluster_debug_mask, 0, sizeof(cluster_debug_mask));
		cluster_debug_index = -1;
		return;
	}

	BSP_ClusterVis(bsp_world_model, cluster_debug_mask, vkpt_refdef.fd->feedback.lookatcluster, DVIS_PVS);
	cluster_debug_index = vkpt_refdef.fd->feedback.lookatcluster;
}

static float halton(int base, int index) {
	float f = 1.f;
	float r = 0.f;
	int i = index;

	while (i > 0)
	{
		f = f / base;
		r = r + f * (i % base);
		i = i / base;
	}
	return r;
};

// Autocompletion support for ray_tracing_api cvar

/*
=================
vkpt_fog_debug

Prints what the fog march is actually being fed. Exists because the pass can run
- and cost frame time - while producing nothing visible, and the two reasons for
that (no model lights, or a density/brightness of zero) are indistinguishable on
screen. Run it in-game rather than launching an instrumented build.
=================
*/
static void vkpt_fog_debug(void)
{
	mapfog_params_t mf;
	bool have = CL_GetMapFog(&mf);

	Com_Printf("map fog: %s\n", have ? "ACTIVE" : "off (cl_fog 0, or map has no fog keys)");
	if (have) {
		Com_Printf("  mode %d   density %g   heightfog %g  z %.0f..%.0f  falloff %g\n",
			mf.mode, mf.density, mf.hf_density, mf.hf_end_z, mf.hf_start_z, mf.hf_falloff);
		Com_Printf("  fog colour %.2f %.2f %.2f   hf %.2f %.2f %.2f -> %.2f %.2f %.2f\n",
			mf.color[0], mf.color[1], mf.color[2],
			mf.hf_end_color[0], mf.hf_end_color[1], mf.hf_end_color[2],
			mf.hf_start_color[0], mf.hf_start_color[1], mf.hf_start_color[2]);
	}
	// cl_fog 2 scatters the CLUSTER light list, which is the static lights plus
	// any model lights injected into it - not the model lights alone. A zero
	// model-light count is normal and no longer means the fog has nothing to
	// scatter; the static count is the one that matters.
	Com_Printf("  static lights: %d   (emissive surfaces + sky - the main fog light source)\n",
		vkpt_refdef.bsp_mesh_world.num_light_polys);
	Com_Printf("  model lights last frame: %d   (dynamic: flares, muzzle flashes, dynamic_light)\n",
		num_model_lights);
	Com_Printf("  cluster light list nodes: %d\n", vkpt_refdef.bsp_mesh_world.num_cluster_lights);
}


static void ray_tracing_api_g(genctx_t *ctx)
{
	Prompt_AddMatch(ctx, "auto");
	Prompt_AddMatch(ctx, "query");
	Prompt_AddMatch(ctx, "pipeline");
}

/* called when the library is loaded */
ref_type_t
R_Init_RTX(bool total)
{
	registration_sequence = 1;

	if (!VID_Init(GAPI_VULKAN)) {
		Com_Error(ERR_FATAL, "VID_Init failed\n");
		return REF_TYPE_NONE;
	}

	/* Paces the presents when frame generation is on. Idle and harmless otherwise. */
	/* REGISTER THE FRAME-GENERATION DEBUG CVARS UP FRONT.
	
	   They were all created lazily by Cvar_Get inside the render loop, which meant
	   they did not exist until a frame with FG active had run - and the console is
	   open exactly when that is not happening, so typing one got "unknown command".
	   That cost a test run with pt_dlss_fg_stats and another with pt_dlss_fg_indices.
	   The lazy Cvar_Get calls downstream simply fetch these instead. Any new FG debug
	   cvar belongs in this list too. */
	Cvar_Get("pt_dlss_fg_stats", "0", 0);
	Cvar_Get("pt_dlss_fg_debug", "0", 0);
	Cvar_Get("pt_dlss_fg_compare", "0", 0);
	Cvar_Get("pt_dlss_fg_dump", "0", 0);
	Cvar_Get("pt_dlss_fg_indices", "0", 0);
	Cvar_Get("pt_dlss_fg_lead", "1.0", 0);
	Cvar_Get("pt_dlss_fg_vblank_offset", "500", 0);
	Cvar_Get("pt_dlss_fg_fifo_pacing", "0", 0);
	Cvar_Get("pt_dlss_fg_timeline", "1", 0);
	Cvar_Get("pt_dlss_fg_queue", "1", 0);
	Cvar_Get("pt_dlss_fg_ready_wait", "0", 0);
	/* These three live in DLSS.c and were created inside DLSSGApply, which does not
	   run while the console is open - so they could never be typed. Third time this
	   trap has cost a test run. */
	Cvar_Get("pt_dlss_fg_mvsign", "1", 0);
	Cvar_Get("pt_dlss_fg_cammotion", "1", 0);
	Cvar_Get("pt_dlss_fg_depthinv", "0", 0);
	Cvar_Get("pt_dlss_fg_mvscale", "0", CVAR_ARCHIVE);

	FGPresent_Init();

	extern SDL_Window *sdl_window;
	qvk.window = sdl_window;

	cvar_profiler = Cvar_Get("profiler", "0", 0);
	cvar_profiler_samples = Cvar_Get("profiler_samples", "60", CVAR_ARCHIVE);
	cvar_profiler_scale = Cvar_Get("profiler_scale", "1", CVAR_ARCHIVE);
	cvar_vsync = Cvar_Get("vid_vsync", "0", CVAR_ARCHIVE);
	cvar_vsync->changed = NULL; // in case the GL renderer has set it
	/* How many images the swapchain asks for. 0 = pick automatically, which is
	   what you want; 2 reproduces the old hard double-buffered behaviour, and
	   4 adds another frame of queue depth at the cost of latency. Changing it
	   recreates the swapchain, so it can be A/B tested without a restart. */
	cvar_swapchain_images = Cvar_Get("vid_swapchain_images", "0", CVAR_ARCHIVE);
	/* With vsync on, use MAILBOX instead of FIFO. Both are tear-free, but FIFO blocks
	   the acquire and MAILBOX does not - so if the stall is the compositor holding
	   presents, this is immune to it. Note MAILBOX does NOT cap the frame rate, so
	   vid_vsync stops limiting fps; pair it with r_maxfps. Recreates the swapchain. */
	cvar_vsync_mailbox = Cvar_Get("vid_vsync_mailbox", "0", CVAR_ARCHIVE);
	/* Log, once per second, the milliseconds this thread spent blocked in the only two
	   places it can block per frame: our own GPU fence, and the presentation engine,
	   plus GPU ms and the worst frame in the window. Archived, because the fault it
	   exists to catch is intermittent and you cannot switch this on after the fact. */
	cvar_present_stats = Cvar_Get("vid_present_stats", "0", CVAR_ARCHIVE);
	/* Take exclusive fullscreen via VK_EXT_full_screen_exclusive when vid_fullscreen is
	   on. Only applies at swapchain creation, so it needs a vid_restart or a fullscreen
	   toggle to take effect. 0 restores asking DWM nicely. */
	cvar_fullscreen_exclusive = Cvar_Get("vid_fullscreen_exclusive", "1", CVAR_ARCHIVE);
	cvar_hdr = Cvar_Get("vid_hdr", "0", CVAR_ARCHIVE);
	cvar_pt_caustics = Cvar_Get("pt_caustics", "1", CVAR_ARCHIVE);
	cvar_pt_enable_nodraw = Cvar_Get("pt_enable_nodraw", "0", 0);
	/* Synthesize materials for surfaces with LIGHT flag.
	 * 0: disabled
	 * 1: enabled for "custom" materials (not in materials.csv)
	 * 2: enabled for all materials w/o an emissive texture */
	cvar_pt_enable_surface_lights = Cvar_Get("pt_enable_surface_lights", "1", CVAR_FILES);
	/* LIGHT flag synthesis for "warp" surfaces (water, slime),
	 * separately controlled for aesthetic reasons
	 * 0: disabled
	 * 1: hack up a material that emits light but doesn't render with an emissive texture
	 * 2: "full" synthesis (incl emissive texture) */
	cvar_pt_enable_surface_lights_warp = Cvar_Get("pt_enable_surface_lights_warp", "0", CVAR_FILES);
	/* How to choose emissive texture for LIGHT flag synthesis:
	 * 0: Just use diffuse texture
	 * 1: Use (diffuse) pixels above a certain relative brightness for emissive texture */
	cvar_pt_surface_lights_fake_emissive_algo = Cvar_Get("pt_surface_lights_fake_emissive_algo", "1", CVAR_FILES);

	// Threshold for pixel values used when constructing a fake emissive image.
	cvar_pt_surface_lights_threshold = Cvar_Get("pt_surface_lights_threshold", "215", CVAR_FILES);

	// Multiplier for texinfo radiance field to convert radiance to emissive factors
	cvar_pt_bsp_radiance_scale = Cvar_Get("pt_bsp_radiance_scale", "0.001", CVAR_FILES);

	// Controls which sky surfaces become poly-lights.
	// 0 -> only the SKY surfaces in clusters listed in sky_clusters.txt
	// 1 -> also surfaces with both SKY and LIGHT flags set
	// 2 -> also surfaces with SKY, LIGHT, and NODRAW flags set become invisible portal lights
	// Nonzero settings should only be used for custom maps where sky surfaces are marked properly for Q2RTX.
	cvar_pt_bsp_sky_lights = Cvar_Get("pt_bsp_sky_lights", "0", 0);

	// 0 -> disabled, regular pause; 1 -> enabled; 2 -> enabled, hide GUI
	cvar_pt_accumulation_rendering = Cvar_Get("pt_accumulation_rendering", "1", CVAR_ARCHIVE);

	// number of frames to accumulate with linear weights in accumulation rendering modes
	cvar_pt_accumulation_rendering_framenum = Cvar_Get("pt_accumulation_rendering_framenum", "500", 0);

	// 0 -> perspective, 1 -> cylindrical
	cvar_pt_projection = Cvar_Get("pt_projection", "0", CVAR_ARCHIVE);

	// depth of field control:
	// 0 -> disabled
	// 1 -> enabled only in the reference mode
	// 2 -> enabled in the reference and no-denoiser modes
	// 3 -> always enabled (where are my glasses?)
	cvar_pt_dof = Cvar_Get("pt_dof", "1", CVAR_ARCHIVE);

	// DLSS-RR: keep the full real indirect specular instead of the A-SVGF fake specular.
	// 1 -> forces pt_fake_roughness_threshold to 1 whenever no A-SVGF pass runs (default)
	// 0 -> previous behaviour, for A/B
	cvar_pt_dlss_indirect_spec = Cvar_Get("pt_dlss_indirect_spec", "1", CVAR_ARCHIVE);

	// freecam mode toggle
	cvar_pt_freecam = Cvar_Get("pt_freecam", "1", CVAR_ARCHIVE);

	// texture filtering mode for non-UI elements:
	// 0 -> linear magnification, anisotropic minification
	// 1 -> nearest magnification, anisotropic minification
	// 2 -> nearest magnification and minification, no mipmaps (noisy)
	cvar_pt_nearest = Cvar_Get("pt_nearest", "0", CVAR_ARCHIVE);
	cvar_pt_nearest->changed = pt_nearest_changed;

	// texture filtering mode for UI elements, follows
	// the gl_bilerp_ cvars, except for `cvar_pt_bilerp_pics` which
	// is only on/off, since vk has no scrap
	cvar_pt_bilerp_chars = Cvar_Get("pt_bilerp_chars", "0", CVAR_ARCHIVE);
	cvar_pt_bilerp_pics = Cvar_Get("pt_bilerp_pics", "0", CVAR_ARCHIVE);
	cvar_pt_bilerp_chars->changed = cvar_pt_bilerp_pics->changed = pt_nearest_changed;

#ifdef VKPT_DEVICE_GROUPS
	cvar_sli = Cvar_Get("sli", "1", CVAR_REFRESH | CVAR_ARCHIVE);
#endif

#ifdef VKPT_IMAGE_DUMPS
	cvar_dump_image = Cvar_Get("dump_image", "0", 0);
#endif

	scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
	scr_viewsize->changed = viewsize_changed;

	// enables or disables full screen blending effects
	cvar_tm_blend_enable = Cvar_Get("tm_blend_enable", "1", CVAR_ARCHIVE);

	drs_init();
	vkpt_fsr_init_cvars();
	InitDLSSCvars();
	InitDLSSGCvars();

	// Minimum NVIDIA driver version - this is a cvar in case something changes in the future,
	// and the current test no longer works.
	cvar_min_driver_version_nvidia = Cvar_Get("min_driver_version_nvidia", "460.82", 0);

	// Minimum AMD driver version
	cvar_min_driver_version_amd = Cvar_Get("min_driver_version_amd", "21.1.1", 0);

	// Selects which RT API to use:
	//  auto     - automatic selection based on the GPU
	//  query    - prefer KHR_ray_query
	//  pipeline - prefer KHR_ray_tracing_pipeline
	cvar_ray_tracing_api = Cvar_Get("ray_tracing_api", "auto", CVAR_REFRESH | CVAR_ARCHIVE);
	cvar_ray_tracing_api->generator = &ray_tracing_api_g;

	// When nonzero, the Vulkan validation layer is requested
	cvar_vk_validation = Cvar_Get("vk_validation", "0", CVAR_REFRESH | CVAR_ARCHIVE);

	InitialiseSkyCVars();

	MAT_Init();

#define UBO_CVAR_DO(name, default_value) cvar_##name = Cvar_Get(#name, #default_value, 0);
	UBO_CVAR_LIST
#undef UBO_CVAR_LIST

	cvar_flt_temporal_hf->changed = temporal_cvar_changed;
	cvar_flt_temporal_lf->changed = temporal_cvar_changed;
	cvar_flt_temporal_spec->changed = temporal_cvar_changed;
	cvar_flt_enable->changed = temporal_cvar_changed;

	cvar_pt_dof->changed = accumulation_cvar_changed;
	cvar_pt_aperture->changed = accumulation_cvar_changed;
	cvar_pt_aperture_type->changed = accumulation_cvar_changed;
	cvar_pt_aperture_angle->changed = accumulation_cvar_changed;
	cvar_pt_focus->changed = accumulation_cvar_changed;
	cvar_pt_freecam->changed = accumulation_cvar_changed;
	cvar_pt_projection->changed = accumulation_cvar_changed;

	cvar_pt_num_bounce_rays->flags |= CVAR_ARCHIVE;

	qvk.win_width  = r_config.width;
	qvk.win_height = r_config.height;

	IMG_Init();
	IMG_GetPalette();
	MOD_Init();
	
	if(!init_vulkan()) {
		Com_Error(ERR_FATAL, "Couldn't initialize Vulkan.\n");
		return REF_TYPE_NONE;
	}

	_VK(create_command_pool_and_fences());
	_VK(create_swapchain());

	vkpt_load_shader_modules();

	/* AFTER init_vulkan: this needs the device to exist and the extension scan to have
	   run, so it cannot sit up beside FGPresent_Init(). */
	Reflex_Init();

	_VK(vkpt_initialize_all(VKPT_INIT_DEFAULT));
	_VK(vkpt_initialize_all(VKPT_INIT_RELOAD_SHADER));
	_VK(vkpt_initialize_all(VKPT_INIT_SWAPCHAIN_RECREATE));

	Cmd_AddCommand("fog_debug", (xcommand_t)&vkpt_fog_debug);
	Cmd_AddCommand("reload_shader", (xcommand_t)&vkpt_reload_shader);
	Cmd_AddCommand("reload_textures", (xcommand_t)&vkpt_reload_textures);
	Cmd_AddCommand("show_pvs", (xcommand_t)&vkpt_show_pvs);
	Cmd_AddCommand("next_sun", (xcommand_t)&vkpt_next_sun_preset);

	vkpt_fog_init();
	vkpt_cameras_init();

	for (int i = 0; i < 256; i++) {
		qvk.sintab[i] = sinf(i * (2 * M_PI / 255));
	}

	for (int i = 0; i < NUM_TAA_SAMPLES; i++)
	{
		taa_samples[i][0] = halton(2, i + 1) - 0.5f;
		taa_samples[i][1] = halton(3, i + 1) - 0.5f;
	}

	return REF_TYPE_VKPT;
}

/* called before the library is unloaded */
void
R_Shutdown_RTX(bool total)
{
	vkpt_freecam_reset();

	// Idle FIRST. DLSSDeconstructor releases the NGX feature and everything NGX
	// allocated behind it, and this ran before the wait - so on a vid_restart it could
	// free resources the GPU was still executing against. That is a race, which is the
	// shape of a crash that only shows up after enough resolution or fullscreen
	// switches.
	/* Stop the present thread before anything it might touch is torn down: it holds
	   the swapchain handle and calls into the queue. */
	FGPresent_Shutdown();

	vkpt_device_wait_idle();
	Reflex_Shutdown();
	DLSSDeconstructor();

	// Persist current DRS scale
	if (drs_current_scale != 0)
		Cvar_SetInteger(cvar_drs_last_scale, drs_current_scale, FROM_CODE);

	Cmd_RemoveCommand("reload_shader");
	Cmd_RemoveCommand("reload_textures");
	Cmd_RemoveCommand("show_pvs");
	Cmd_RemoveCommand("next_sun");

	if (vkpt_refdef.bsp_mesh_world_loaded)
	{
		vkpt_vertex_buffer_cleanup_bsp_mesh(&vkpt_refdef.bsp_mesh_world);
		bsp_mesh_destroy(&vkpt_refdef.bsp_mesh_world);
	}

	vkpt_fog_shutdown();
	vkpt_cameras_shutdown();
	MAT_Shutdown();
	IMG_FreeAll();
	vkpt_textures_destroy_unused();

	_VK(vkpt_destroy_all(VKPT_INIT_DEFAULT));
	vkpt_destroy_shader_modules();

	if(destroy_vulkan()) {
		Com_EPrintf("destroy_vulkan failed\n");
	}

	IMG_Shutdown();
	MOD_Shutdown(); // todo: currently leaks memory, need to clear submeshes
	VID_Shutdown();
	
}

// for screenshots
byte *
IMG_ReadPixels_RTX(int *width, int *height, int *rowbytes)
{
	if (qvk.surf_format.format != VK_FORMAT_B8G8R8A8_SRGB &&
		qvk.surf_format.format != VK_FORMAT_R8G8B8A8_SRGB)
	{
		Com_EPrintf("IMG_ReadPixels: unsupported swap chain format (%d)!\n", qvk.surf_format.format);
		return NULL;
	}

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImage swap_chain_image = qvk.swap_chain_images[qvk.current_swap_chain_image_index];

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};
		
	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_HOST_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	VkImageCopy img_copy_region = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.extent = { qvk.extent_unscaled.width, qvk.extent_unscaled.height, 1 }
	};

	vkCmdCopyImage(cmd_buf,
		swap_chain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		qvk.screenshot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &img_copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, false);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	VkImageSubresource subresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0
	};

	VkSubresourceLayout subresource_layout;
	vkGetImageSubresourceLayout(qvk.device, qvk.screenshot_image, &subresource, &subresource_layout);

	void *device_data;
	_VK(vkMapMemory(qvk.device, qvk.screenshot_image_memory, 0, qvk.screenshot_image_memory_size, 0, &device_data));
	
	int pitch = qvk.extent_unscaled.width * 3;
	byte *pixels = FS_AllocTempMem(pitch * qvk.extent_unscaled.height);

	for (int row = 0; row < qvk.extent_unscaled.height; row++)
	{
		byte* src_row = (byte*)device_data + subresource_layout.rowPitch * row;
		byte* dst_row = pixels + pitch * (qvk.extent_unscaled.height - row - 1);

		if (qvk.surf_format.format == VK_FORMAT_B8G8R8A8_SRGB)
		{
			for (int col = 0; col < qvk.extent_unscaled.width; col++)
			{
				dst_row[0] = src_row[2];
				dst_row[1] = src_row[1];
				dst_row[2] = src_row[0];

				src_row += 4;
				dst_row += 3;
			}
		}
		else // must be VK_FORMAT_R8G8B8A8_SRGB then
		{
			for (int col = 0; col < qvk.extent_unscaled.width; col++)
			{
				dst_row[0] = src_row[0];
				dst_row[1] = src_row[1];
				dst_row[2] = src_row[2];

				src_row += 4;
				dst_row += 3;
			}
		}
	}

	vkUnmapMemory(qvk.device, qvk.screenshot_image_memory);

	*width = qvk.extent_unscaled.width;
	*height = qvk.extent_unscaled.height;
	*rowbytes = pitch;
	return pixels;
}

float *
IMG_ReadPixelsHDR_RTX(int *width, int *height)
{
	if (qvk.surf_format.format != VK_FORMAT_R16G16B16A16_SFLOAT)
	{
		Com_EPrintf("IMG_ReadPixelsHDR: unsupported swap chain format (%d)!\n", qvk.surf_format.format);
		return NULL;
	}

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImage swap_chain_image = qvk.swap_chain_images[qvk.current_swap_chain_image_index];

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};
		
	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_HOST_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	VkImageCopy img_copy_region = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.extent = { qvk.extent_unscaled.width, qvk.extent_unscaled.height, 1 }
	};

	vkCmdCopyImage(cmd_buf,
		swap_chain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		qvk.screenshot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &img_copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, false);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	VkImageSubresource subresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0
	};

	VkSubresourceLayout subresource_layout;
	vkGetImageSubresourceLayout(qvk.device, qvk.screenshot_image, &subresource, &subresource_layout);

	void *device_data;
	_VK(vkMapMemory(qvk.device, qvk.screenshot_image_memory, 0, qvk.screenshot_image_memory_size, 0, &device_data));
	
	int pitch = qvk.extent_unscaled.width * 3;
	float *pixels = FS_AllocTempMem(pitch * qvk.extent_unscaled.height * sizeof(float));

	for (int row = 0; row < qvk.extent_unscaled.height; row++)
	{
		uint16_t* src_row = (uint16_t*)((byte*)device_data + subresource_layout.rowPitch * row);
		float* dst_row = pixels + pitch * (qvk.extent_unscaled.height - row - 1);

		for (int col = 0; col < qvk.extent_unscaled.width; col++)
		{
			dst_row[0] = halfToFloat(src_row[0]);
			dst_row[1] = halfToFloat(src_row[1]);
			dst_row[2] = halfToFloat(src_row[2]);

			src_row += 4;
			dst_row += 3;
		}
	}

	vkUnmapMemory(qvk.device, qvk.screenshot_image_memory);

	*width = qvk.extent_unscaled.width;
	*height = qvk.extent_unscaled.height;
	return pixels;
}

void
R_SetSky_RTX(const char *name, float rotate, int autorotate, const vec3_t axis)
{
	int     i;
	char    pathname[MAX_QPATH];
	// 3dstudio environment map names
	const char *suf[6] = { "ft", "bk", "up", "dn", "rt", "lf" };

	byte *data = NULL;
	// all six faces have to load, at a matching size, for the cube map to be usable
	bool complete = name[0] != 0;

	sky_rotation = rotate;
	sky_autorotate = autorotate;
	VectorNormalize2(axis, sky_axis);

	int avg_color[3] = { 0 };
	int w_prev = 1, h_prev = 1;
	for (i = 0; complete && i < 6; i++) {
		Q_concat(pathname, sizeof(pathname), "env/", name, suf[i], ".tga");
		FS_NormalizePath(pathname);
		image_t *img = IMG_Find(pathname, IT_SKY, IF_NONE);

		if(img == R_NOTEXTURE) {
			complete = false;
			break;
		}

		if(!data) {
			w_prev = img->upload_width;
			h_prev = img->upload_height;
			data = Z_Malloc((size_t)w_prev * h_prev * 4 * 6);
		} else if (img->upload_width != w_prev || img->upload_height != h_prev) {
			Com_WPrintf("Sky face %s is %dx%d, expected %dx%d\n", pathname,
				img->upload_width, img->upload_height, w_prev, h_prev);
			complete = false;
			break;
		}

		size_t s = (size_t)w_prev * h_prev * 4;

		memcpy(data + s * i, img->pix_data, s);

		for (int p = 0; p < w_prev * h_prev; p++)
		{
			uint32_t pix = *((uint32_t*)img->pix_data + p);
			avg_color[0] += pix & 0xff;
			avg_color[1] += (pix >> 8) & 0xff;
			avg_color[2] += (pix >> 16) & 0xff;
		}

		List_Remove(&img->entry);

		IMG_Unload(img);

		memset(img, 0, sizeof(*img));
	}

	if (!complete) {
		// no usable skybox: fall back to a 1x1 magenta cube map
		if(data) {
			Z_Free(data);
		}
		data = Z_Malloc(6 * sizeof(uint32_t));
		for(int j = 0; j < 6; j++)
			((uint32_t *)data)[j] = 0xff00ffffu;
		w_prev = h_prev = 1;
		avg_color[0] = avg_color[1] = 255 * 6;
		avg_color[2] = 0;
	}

	float inv_num_pixels = 1.0f / (w_prev * h_prev * 6);

	VectorSet(avg_envmap_color,
		(float)avg_color[0] * inv_num_pixels / 255.f,
		(float)avg_color[1] * inv_num_pixels / 255.f,
		(float)avg_color[2] * inv_num_pixels / 255.f
	);

	vkpt_textures_upload_envmap(w_prev, h_prev, data);
	Z_Free(data);

	// Let the sky renderer know whether this map brought a skybox of its own,
	// and which game it came from - see sky_use_map_skybox.
	vkpt_physical_sky_set_map_skybox(!complete ? MAP_SKYBOX_NONE :
		(Q_stricmp(cl.gamedir, "rerelease") == 0 ? MAP_SKYBOX_RERELEASE : MAP_SKYBOX_OTHER));
}

void R_AddDecal_RTX(decal_t *d)
{ }

void
R_BeginRegistration_RTX(const char *name)
{
	registration_sequence++;
	LOG_FUNC();
	Com_Printf("loading %s\n", name);
	vkpt_device_wait_idle();

	// New level - DLSS must not reproject from the previous one.
	vkpt_dlss_request_history_reset();

	vkpt_fog_reset();

	// Undo the previous map's "mapcvar" settings before this map's cfgs run, so a
	// per-map override lasts exactly one map and leaves the player's own value
	// behind when they move on.
	Cmd_RestoreMapCvars();

	Com_AddConfigFile("maps/default.cfg", 0);
	Com_AddConfigFile(va("maps/%s.cfg", name), 0);

	if(vkpt_refdef.bsp_mesh_world_loaded) {
		vkpt_vertex_buffer_cleanup_bsp_mesh(&vkpt_refdef.bsp_mesh_world);
		bsp_mesh_destroy(&vkpt_refdef.bsp_mesh_world);
		vkpt_refdef.bsp_mesh_world_loaded = 0;
	}

	if(bsp_world_model) {
		BSP_Free(bsp_world_model);
		bsp_world_model = NULL;
	}

	char bsp_path[MAX_QPATH];
	Q_concat(bsp_path, sizeof(bsp_path), "maps/", name, ".bsp");
	bsp_t *bsp;
	int ret = BSP_Load(bsp_path, &bsp);
	if(!bsp) {
		Com_Error(ERR_DROP, "%s: couldn't load %s: %s", __func__, bsp_path, Q_ErrorString(ret));
	}
	if (!bsp->vis) {
		Hunk_Free(&bsp->hunk);
		Z_Free(bsp);
		Com_Error(ERR_DROP, "BSP not vis'd; this is required for Q2RTX.");
	}
	bsp_world_model = bsp;
	bsp_mesh_register_textures(bsp);
	bsp_mesh_create_from_bsp(&vkpt_refdef.bsp_mesh_world, bsp, name);
	vkpt_light_buffers_create(&vkpt_refdef.bsp_mesh_world);
	_VK(vkpt_vertex_buffer_upload_bsp_mesh(&vkpt_refdef.bsp_mesh_world));
	vkpt_refdef.bsp_mesh_world_loaded = 1;
	bsp = NULL;
	world_anim_frame = 0;

	Cvar_Set("sv_novis", vkpt_refdef.bsp_mesh_world.num_cameras > 0 ? "1" : "0");

	// register physical sky attributes based on map name lookup
	vkpt_physical_sky_beginRegistration();
	UpdatePhysicalSkyCVars();

	vkpt_physical_sky_latch_local_time();
	vkpt_bloom_reset();
	vkpt_tone_mapping_request_reset();
	vkpt_light_buffer_reset_counts();

	memset(cluster_debug_mask, 0, sizeof(cluster_debug_mask));
	cluster_debug_index = -1;

	drs_last_frame_world = false;
}

void
R_EndRegistration_RTX(void)
{
	LOG_FUNC();
	
	vkpt_physical_sky_endRegistration();

	IMG_FreeUnused();
	MOD_FreeUnused();
	MAT_FreeUnused();
}

VkCommandBuffer vkpt_begin_command_buffer(cmd_buf_group_t* group)
{
	if (group->used_this_frame == group->count_per_frame)
	{
		uint32_t new_count = max(4, group->count_per_frame * 2);
		VkCommandBuffer* new_buffers = Z_Mallocz(new_count * MAX_FRAMES_IN_FLIGHT * sizeof(VkCommandBuffer));

		for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
		{
			if (group->count_per_frame > 0)
			{
				memcpy(new_buffers + new_count * frame, group->buffers + group->count_per_frame * frame, group->count_per_frame * sizeof(VkCommandBuffer));
			}

			VkCommandBufferAllocateInfo cmd_buf_alloc_info = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = group->command_pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = new_count - group->count_per_frame
			};

			_VK(vkAllocateCommandBuffers(qvk.device, &cmd_buf_alloc_info, new_buffers + new_count * frame + group->count_per_frame));
		}

#ifdef USE_DEBUG
		void** new_addrs = Z_Mallocz(new_count * MAX_FRAMES_IN_FLIGHT * sizeof(void*));

		if (group->count_per_frame > 0)
		{
			for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
			{
				memcpy(new_addrs + new_count * frame, group->buffer_begin_addrs + group->count_per_frame * frame, group->count_per_frame * sizeof(void*));
			}
		}

		Z_Free(group->buffer_begin_addrs);
		group->buffer_begin_addrs = new_addrs;
#endif

		Z_Free(group->buffers);
		group->buffers = new_buffers;
		group->count_per_frame = new_count;
	}

	VkCommandBuffer cmd_buf = group->buffers[group->count_per_frame * qvk.current_frame_index + group->used_this_frame];

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = NULL,
	};
	_VK(vkResetCommandBuffer(cmd_buf, 0));
	_VK(vkBeginCommandBuffer(cmd_buf, &begin_info));


#ifdef USE_DEBUG
	void** begin_addr = group->buffer_begin_addrs + group->count_per_frame * qvk.current_frame_index + group->used_this_frame;

#if (defined __GNUC__)
	*begin_addr = __builtin_return_address(0);
#elif (defined _MSC_VER)
	*begin_addr = _ReturnAddress();
#else
	*begin_addr = NULL;
#endif
#endif

	group->used_this_frame += 1;

	return cmd_buf;
}

void vkpt_free_command_buffers(cmd_buf_group_t* group)
{
	if (group->count_per_frame == 0)
		return;

	vkFreeCommandBuffers(qvk.device, group->command_pool, group->count_per_frame * MAX_FRAMES_IN_FLIGHT, group->buffers);

	Z_Free(group->buffers);
	group->buffers = NULL;

#ifdef USE_DEBUG
	Z_Free(group->buffer_begin_addrs);
	group->buffer_begin_addrs = NULL;
#endif

	group->count_per_frame = 0;
	group->used_this_frame = 0;
}

void vkpt_reset_command_buffers(cmd_buf_group_t* group)
{
	group->used_this_frame = 0;

#if 0 // defined(USE_DEBUG)
	for (int i = 0; i < group->count_per_frame; i++)
	{
		void* addr = group->buffer_begin_addrs[group->count_per_frame * qvk.current_frame_index + i];
		//seth: this seems unrelated to the raytracing changes, but skip it until raytracing is working
		//assert(addr == 0);
	}
#endif
}

void vkpt_wait_idle(VkQueue queue, cmd_buf_group_t* group)
{
	vkpt_queue_wait_idle(queue);
	vkpt_reset_command_buffers(group);
}

void vkpt_submit_command_buffer(
	VkCommandBuffer cmd_buf,
	VkQueue queue,
	uint32_t execute_device_mask,
	int wait_semaphore_count,
	VkSemaphore* wait_semaphores,
	VkPipelineStageFlags* wait_stages,
	uint32_t* wait_device_indices,
	int signal_semaphore_count,
	VkSemaphore* signal_semaphores,
	uint32_t* signal_device_indices,
	VkFence fence)
{
	_VK(vkEndCommandBuffer(cmd_buf));

	/* Ties this submission to the frame's Reflex presentID. WITHOUT this the driver
	   cannot match GPU work to a frame, so gpuRenderStart/End come back as zero and
	   the reported end-to-end latency is only the CPU span - which reads as an
	   impossibly good sub-millisecond number. */
	uint64_t reflex_submit_present_id = Reflex_CurrentPresentID();
	VkLatencySubmissionPresentIdNV reflex_submit_id = {
		.sType     = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV,
		.presentID = reflex_submit_present_id,
	};

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = (Reflex_Available() && reflex_submit_present_id)
			? &reflex_submit_id : NULL,
		.waitSemaphoreCount = wait_semaphore_count,
		.pWaitSemaphores = wait_semaphores,
		.pWaitDstStageMask = wait_stages,
		.signalSemaphoreCount = signal_semaphore_count,
		.pSignalSemaphores = signal_semaphores,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd_buf,
	};

#ifdef VKPT_DEVICE_GROUPS
	VkDeviceGroupSubmitInfo device_group_submit_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO,
		.pNext = NULL,
		.waitSemaphoreCount = wait_semaphore_count,
		.pWaitSemaphoreDeviceIndices = wait_device_indices,
		.commandBufferCount = 1,
		.pCommandBufferDeviceMasks = &execute_device_mask,
		.signalSemaphoreCount = signal_semaphore_count,
		.pSignalSemaphoreDeviceIndices = signal_device_indices,
	};

	if (qvk.device_count > 1) {
		device_group_submit_info.pNext = submit_info.pNext;
		submit_info.pNext = &device_group_submit_info;
	}
#endif

	/* The present thread calls vkQueuePresentKHR on this same queue, and the two
	   need external synchronisation. */
	FGPresent_SwapchainLock();
	_VK(vkQueueSubmit(queue, 1, &submit_info, fence));
	FGPresent_SwapchainUnlock();

#ifdef USE_DEBUG
	cmd_buf_group_t* groups[] = { &qvk.cmd_buffers_graphics, &qvk.cmd_buffers_transfer };
	for (int ngroup = 0; ngroup < LENGTH(groups); ngroup++)
	{
		cmd_buf_group_t* group = groups[ngroup];
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT * group->count_per_frame; i++)
		{
			if (group->buffers[i] == cmd_buf)
			{
				group->buffer_begin_addrs[i] = NULL;
				return;
			}
		}
	}
#endif
}

void vkpt_submit_command_buffer_simple(
	VkCommandBuffer cmd_buf,
	VkQueue queue,
	bool all_gpus)
{
	vkpt_submit_command_buffer(cmd_buf, queue, all_gpus ? (1 << qvk.device_count) - 1 : 1, 0, NULL, NULL, NULL, 0, NULL, NULL, 0);
}

#if _WIN32
	#include <windows.h>
#else
	#include <stdio.h>
#endif

void debug_output(const char* format, ...)
{
	char buffer[2048];

	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

#if _WIN32
	OutputDebugStringA(buffer);
#else
	fprintf(stderr, "%s", buffer);
#endif
}

static bool R_IsHDR_RTX(void)
{
	return qvk.surf_is_hdr;
}

void R_RegisterFunctionsRTX()
{
	R_Init = R_Init_RTX;
	R_Shutdown = R_Shutdown_RTX;
	R_BeginRegistration = R_BeginRegistration_RTX;
	R_EndRegistration = R_EndRegistration_RTX;
	R_SetSky = R_SetSky_RTX;
	R_RenderFrame = R_RenderFrame_RTX;
	R_LightPoint = R_LightPoint_RTX;
	R_ClearColor = R_ClearColor_RTX;
	R_SetAlpha = R_SetAlpha_RTX;
	R_SetAlphaScale = R_SetAlphaScale_RTX;
	R_SetColor = R_SetColor_RTX;
	R_SetClipRect = R_SetClipRect_RTX;
	R_SetScale = R_SetScale_RTX;
	R_DrawChar = R_DrawChar_RTX;
	R_DrawString = R_DrawString_RTX;
	R_DrawPic = R_DrawPic_RTX;
	R_DrawStretchPic = R_DrawStretchPic_RTX;
	R_TileClear = R_TileClear_RTX;
	R_DrawFill8 = R_DrawFill8_RTX;
	R_DrawFill32 = R_DrawFill32_RTX;
	R_BeginFrame = R_BeginFrame_RTX;
	R_EndFrame = R_EndFrame_RTX;
	R_ModeChanged = R_ModeChanged_RTX;
	R_AddDecal = R_AddDecal_RTX;
	R_InterceptKey = R_InterceptKey_RTX;
	R_IsHDR = R_IsHDR_RTX;
	R_LatencySleep = Reflex_SleepAndBeginFrame;
	IMG_Load = IMG_Load_RTX;
	IMG_Unload = IMG_Unload_RTX;
	IMG_ReadPixels = IMG_ReadPixels_RTX;
	IMG_ReadPixelsHDR = IMG_ReadPixelsHDR_RTX;
	MOD_LoadMD2 = MOD_LoadMD2_RTX;
	MOD_LoadMD3 = MOD_LoadMD3_RTX;
	MOD_LoadIQM = MOD_LoadIQM_RTX;
	MOD_LoadMD5 = MOD_LoadMD5_RTX;
	MOD_Reference = MOD_Reference_RTX;
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
