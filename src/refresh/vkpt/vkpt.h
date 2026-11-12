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

#ifndef  __VKPT_H__
#define  __VKPT_H__


#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#if !defined(HAVE_M_PI)
#define HAVE_M_PI
#endif // !defined(HAVE_M_PI)

#include "vk_util.h"

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

#include "shader/global_ubo.h"
#include "shader/global_textures.h"
#include "shader/vertex_buffer.h"

#define LENGTH(a) ((sizeof (a)) / (sizeof(*(a))))

#define LOG_FUNC_(f) do {} while(0)
//#define LOG_FUNC_(f) Com_Printf("%s\n", f)
#define LOG_FUNC() LOG_FUNC_(__func__)

#ifdef USE_DEBUG
#define _VK(...) \
	do { \
		VkResult _res = __VA_ARGS__; \
		if(_res != VK_SUCCESS) { \
			Com_EPrintf("error %d executing %s! %s:%d\n", _res, # __VA_ARGS__, __FILE__, __LINE__); \
		} \
	} while(0)
#else
#define _VK(...) do { __VA_ARGS__; } while(0)
#endif

/* see main.c to override default file path. By default it will strip away
 * QVK_MOD_, fix the file ending, and convert to lower case */
#define LIST_SHADER_MODULES \
	SHADER_MODULE_DO(QVK_MOD_STRETCH_PIC_VERT)                       \
	SHADER_MODULE_DO(QVK_MOD_STRETCH_PIC_FRAG)                       \
	SHADER_MODULE_DO(QVK_MOD_FINAL_BLIT_VERT)                        \
	SHADER_MODULE_DO(QVK_MOD_FINAL_BLIT_LANCZOS_FRAG)                \
	SHADER_MODULE_DO(QVK_MOD_FILM_GRAIN_FRAG)                        \
	SHADER_MODULE_DO(QVK_MOD_CRT_FRAG)                               \
	SHADER_MODULE_DO(QVK_MOD_CRT_DOWNSAMPLE_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_INSTANCE_GEOMETRY_COMP)                 \
	SHADER_MODULE_DO(QVK_MOD_ANIMATE_MATERIALS_COMP)                 \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_GRADIENT_IMG_COMP)                \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_GRADIENT_ATROUS_COMP)             \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_GRADIENT_REPROJECT_COMP)          \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_ATROUS_COMP)                      \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_LF_COMP)                          \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_TEMPORAL_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_ASVGF_TAAU_COMP)                        \
	SHADER_MODULE_DO(QVK_MOD_BLOOM_BLUR_COMP)                        \
	SHADER_MODULE_DO(QVK_MOD_BLOOM_COMPOSITE_COMP)                   \
	SHADER_MODULE_DO(QVK_MOD_BLOOM_DOWNSCALE_COMP)                   \
	SHADER_MODULE_DO(QVK_MOD_MOTION_BLUR_COMP)                       \
	SHADER_MODULE_DO(QVK_MOD_CHROMATIC_ABERRATION_COMP)              \
	SHADER_MODULE_DO(QVK_MOD_DLSSG_HDR_COMP)                         \
	SHADER_MODULE_DO(QVK_MOD_DLSS5_CONVERT_COMP)                     \
	SHADER_MODULE_DO(QVK_MOD_TONE_MAPPING_HISTOGRAM_COMP)            \
	SHADER_MODULE_DO(QVK_MOD_TONE_MAPPING_CURVE_COMP)                \
	SHADER_MODULE_DO(QVK_MOD_TONE_MAPPING_APPLY_COMP)                \
	SHADER_MODULE_DO(QVK_MOD_PHYSICAL_SKY_COMP)                      \
	SHADER_MODULE_DO(QVK_MOD_PHYSICAL_SKY_SPACE_COMP)                \
	SHADER_MODULE_DO(QVK_MOD_SKY_BUFFER_RESOLVE_COMP)                \
	SHADER_MODULE_DO(QVK_MOD_CHECKERBOARD_INTERLEAVE_COMP)           \
	SHADER_MODULE_DO(QVK_MOD_GOD_RAYS_COMP)                          \
	SHADER_MODULE_DO(QVK_MOD_GOD_RAYS_FILTER_COMP)                   \
	SHADER_MODULE_DO(QVK_MOD_FROXEL_RESERVOIR_COMP)                  \
	SHADER_MODULE_DO(QVK_MOD_FROXEL_SPATIAL_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_FROXEL_SCATTER_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_FROXEL_INTEGRATE_COMP)                  \
	SHADER_MODULE_DO(QVK_MOD_SKY_EXPOSURE_BAKE_COMP)                 \
	SHADER_MODULE_DO(QVK_MOD_SHADOW_MAP_VERT)                        \
	SHADER_MODULE_DO(QVK_MOD_COMPOSITING_COMP)                       \
	SHADER_MODULE_DO(QVK_MOD_NORMALIZE_NORMAL_MAP_COMP)              \
	SHADER_MODULE_DO(QVK_MOD_SHARC_RESOLVE_COMP)                     \
	SHADER_MODULE_DO(QVK_MOD_SOUND_TRACE_COMP)                       \
	SHADER_MODULE_DO(QVK_MOD_SOUND_DIFFRACT_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_FLOW_LIGHTS_COMP)                       \
	SHADER_MODULE_DO(QVK_MOD_FLOW_OCCUPANCY_COMP)                    \
	SHADER_MODULE_DO(QVK_MOD_FLOW_AMBIENT_COMP)                      \

#define LIST_RT_RGEN_SHADER_MODULES \
	SHADER_MODULE_DO(QVK_MOD_PRIMARY_RAYS_RGEN)                      \
	SHADER_MODULE_DO(QVK_MOD_REFLECT_REFRACT_RGEN)                   \
	SHADER_MODULE_DO(QVK_MOD_DIRECT_LIGHTING_RGEN)                   \
	SHADER_MODULE_DO(QVK_MOD_INDIRECT_LIGHTING_RGEN)                 \
	SHADER_MODULE_DO(QVK_MOD_SHARC_UPDATE_RGEN)                      \
	SHADER_MODULE_DO(QVK_MOD_RESTIR_GI_RGEN)                         \

#define LIST_RT_PIPELINE_SHADER_MODULES \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_RCHIT)                      \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_MASKED_RAHIT)               \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_PARTICLE_RAHIT)             \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_BEAM_RAHIT)                 \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_BEAM_RINT)                  \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_RMISS)                      \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_EXPLOSION_RAHIT)            \
	SHADER_MODULE_DO(QVK_MOD_PATH_TRACER_SPRITE_RAHIT)               \


#define SHADER_STAGE(_module, _stage) \
	{ \
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, \
		.stage = _stage, \
		.module = qvk.shader_modules[_module], \
		.pName = "main" \
	}

#define SHADER_STAGE_SPEC(_module, _stage, _spec) \
	{ \
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, \
		.stage = _stage, \
		.module = qvk.shader_modules[_module], \
		.pName = "main", \
		.pSpecializationInfo = _spec, \
	}

enum QVK_SHADER_MODULES {
#define SHADER_MODULE_DO(a) a,
	LIST_SHADER_MODULES
	LIST_RT_RGEN_SHADER_MODULES
	LIST_RT_PIPELINE_SHADER_MODULES
#undef SHADER_MODULE_DO
	NUM_QVK_SHADER_MODULES
};

#define MAX_FRAMES_IN_FLIGHT 2

typedef struct cmd_buf_group_s {
	uint32_t count_per_frame;
	uint32_t used_this_frame;
	VkCommandBuffer* buffers;
	VkCommandPool command_pool;
#ifdef USE_DEBUG
	void** buffer_begin_addrs;
#endif
} cmd_buf_group_t;

/* Maximum generated (interpolated) frames per real frame, i.e. up to 6x. Must match
   the number of DLSS_FG_OUTPUT* images in shader/global_textures.h. Defined here
   rather than in DLSS.h because DLSS.h includes this header, not the other way. */
#define DLSSG_MAX_GENERATED_FRAMES 5

typedef struct semaphore_group_s {
	VkSemaphore image_available;
	VkSemaphore render_finished;
	/* Frame generation acquires one EXTRA swapchain image per generated frame, so an Nx
	   multiplier acquires N images beyond the one R_BeginFrame already took. Each
	   acquire needs its own binary semaphore. Used on GPU 0 only, and only while
	   pt_dlss_fg is on; the matching presents wait on qvk.semaphores_present[image]. */
	VkSemaphore image_available_fg[DLSSG_MAX_GENERATED_FRAMES];
	VkSemaphore transfer_finished;
	VkSemaphore trace_finished;
	/* Async compute (pt_async_compute): the froxel fog runs on queue_compute in two
	   halves. fog_inputs_ready releases the first half (the ReSTIR light selection,
	   which needs only the uniform buffer and the light buffer), primary_finished
	   releases the second (scatter and integrate need the TLAS, the shadow map and
	   this frame's PT_PRIMARY_DIST), and fog_finished holds the god rays filter -
	   the first graphics pass to read the integrated volume - until both are done. */
	VkSemaphore fog_inputs_ready;
	VkSemaphore primary_finished;
	VkSemaphore fog_finished;
	bool trace_signaled;
} semaphore_group_t;

typedef struct QVK_s {
	VkInstance                  instance;
	VkPhysicalDevice            physical_device;
	VkPhysicalDeviceMemoryProperties mem_properties;

	// number of GPUs we're rendering to --- if DG is disabled, this is 1
	int							device_count;
#ifdef VKPT_DEVICE_GROUPS
	VkPhysicalDevice			device_group_physical_devices[VKPT_MAX_GPUS];
#endif

	VkDevice                    device;
	VkQueue                     queue_graphics;
	VkQueue                     queue_transfer;
	/* A SECOND queue from the graphics family, used ONLY by the frame-generation
	   present thread. vkQueuePresentKHR is a queue operation: on a single queue it is
	   processed in submission order, so a present issued after the next frame's render
	   submit cannot flip until that render has finished on the GPU. That is what made
	   every present in a frame-generation group flip together in a burst. Equal to
	   queue_graphics when the family exposes only one queue. */
	VkQueue                     queue_present;
	bool                        queue_present_dedicated;
	int32_t                     queue_idx_graphics;
	int32_t                     queue_idx_transfer;
	/* A queue from a compute-only family, for work that can overlap the ray tracing
	   passes (the froxel fog). VK_NULL_HANDLE when the device has no such family;
	   async_compute_available is false then and every pass stays on queue_graphics.
	   Images that both queues touch are created VK_SHARING_MODE_CONCURRENT over
	   queue_families_graphics_compute - see vkpt_image_sharing_graphics_compute. */
	VkQueue                     queue_compute;
	int32_t                     queue_idx_compute;
	bool                        async_compute_available;
	/* The sound's own queue: steamaudio.c's reflections and diffraction traced on the GPU are
	   submitted from its simulation thread (sound_trace.c), so no other thread may
	   use it. From a compute-only family, after the queues the transfer and async
	   compute paths took of it; VK_NULL_HANDLE when there is none left. */
	VkQueue                     queue_sound;
	int32_t                     queue_idx_sound;
	uint32_t                    queue_families_graphics_compute[2];
	VkSurfaceKHR                surface;
	VkSwapchainKHR              swap_chain;
	VkSurfaceFormatKHR          surf_format;
	bool                        surf_is_hdr;
	bool                        surf_vsync;
	bool                        surf_vsync_mailbox;
	bool                        supports_fse;       /* VK_EXT_full_screen_exclusive present */
	bool                        surf_fse_acquired;  /* we currently hold exclusive fullscreen */
	uint32_t                    surf_num_images;    /* images actually requested */
	VkPresentModeKHR            present_mode;
	VkExtent2D                  extent_screen_images;
	VkExtent2D                  extent_render;
	VkExtent2D                  extent_render_prev;
	VkExtent2D                  extent_unscaled;
	VkExtent2D                  extent_taa_images;
	VkExtent2D                  extent_taa_output;
	uint32_t                    gpu_slice_width;
	uint32_t                    gpu_slice_width_prev;
	uint32_t                    num_swap_chain_images;
	VkImage*                    swap_chain_images;
	VkImageView*                swap_chain_image_views;
	
	bool                        use_ray_query;
	bool                        enable_validation;
	bool                        supports_fp16;
	/* shaderStorageImageRead/WriteWithoutFormat, which NVIDIA Flow's shaders
	   need (flow.c). Enabled on the device when present; nothing of ours uses them. */
	bool                        supports_storage_image_without_format;

	cmd_buf_group_t             cmd_buffers_graphics;
	cmd_buf_group_t             cmd_buffers_transfer;
	cmd_buf_group_t             cmd_buffers_compute;
	semaphore_group_t           semaphores[MAX_FRAMES_IN_FLIGHT][VKPT_MAX_GPUS];

	uint32_t                    num_extensions;
	VkExtensionProperties       *extensions;

	uint32_t                    num_layers;
	VkLayerProperties           *layers;

	VkDebugUtilsMessengerEXT    dbg_messenger;

	VkFence                     fences_frame_sync[MAX_FRAMES_IN_FLIGHT];


	int                         win_width;
	int                         win_height;
	uint64_t                    frame_counter;

	int                         effective_aa_mode;

	SDL_Window                  *window;
	uint32_t                    num_sdl2_extensions;
	const char                  **sdl2_extensions;

/* Present-wait semaphores, one PER SWAPCHAIN IMAGE (not per frame in flight).
   A present's wait semaphore stays in use until the presentation engine is done with
   that image, which is not bounded by the frame-in-flight fence, so keying them by
   frame index lets a submit re-signal one that a pending present still owns -
   VUID-vkQueueSubmit-pSignalSemaphores-00067. Keying by image index makes the
   semaphore's lifetime match the thing it actually guards. */
	VkSemaphore                 *semaphores_present;

	/* NVIDIA Reflex (VK_NV_low_latency2) and the present-id extension it correlates
	   timings with. Both optional; see reflex.c. */
	bool                        supports_low_latency;
	bool                        supports_present_id;
	/* VK_NVX_binary_import + VK_NVX_image_view_handle on the picked device. NGX (DLSS,
	   DLSS-G, DLSS 5) cannot run without them, and only NVIDIA drivers expose them. */
	bool                        supports_ngx;
	bool                        supports_device_fault;   /* VK_EXT_device_fault */
	bool                        supports_checkpoints;    /* VK_NV_device_diagnostic_checkpoints */
	bool                        supports_address_binding_report; /* VK_EXT_device_address_binding_report */
	/* VK_NV_partitioned_acceleration_structure with its feature: the partitioned TLAS
	   (ptlas.h, pt_ptlas) */
	bool                        supports_ptlas;
	/* VK_NV_cluster_acceleration_structure with its feature: clustered dynamic
	   geometry (clusters.h, pt_clusters) - on this GPU (pt_clusters_available), and
	   enabled on this device (only while pt_clusters is 1) */
	bool                        supports_clusters;
	bool                        clusters_enabled;

	uint32_t                    current_swap_chain_image_index;
	uint32_t                    current_frame_index;
	// when set, we'll do a WFI before acquire for this many frames
	uint32_t                    wait_for_idle_frames;
	float                       timestampPeriod;
	bool                        frame_menu_mode;
	/* 0..1, how far the item wheel is up. Separate from frame_menu_mode on
	   purpose: the wheel blurs the world the same way the menu does, but the
	   world is still being RENDERED behind it, so frame generation must keep
	   treating these as ordinary game frames. */
	float                       frame_ui_blur;

	VkShaderModule              shader_modules[NUM_QVK_SHADER_MODULES];

	VkDescriptorSetLayout       desc_set_layout_ubo;
	VkDescriptorSet             desc_set_ubo;

	VkDescriptorSetLayout       desc_set_layout_textures;
	VkDescriptorSet             desc_set_textures_even;
	VkDescriptorSet             desc_set_textures_odd;
	VkImage                     images      [NUM_VKPT_IMAGES]; // todo: rename to make consistent
	VkImageView                 images_views[NUM_VKPT_IMAGES]; // todo: rename to make consistent

#ifdef VKPT_DEVICE_GROUPS
	// local per-GPU image bindings for SLI
	VkImage						images_local[VKPT_MAX_GPUS][NUM_VKPT_IMAGES];
	VkImageView					images_views_local[VKPT_MAX_GPUS][NUM_VKPT_IMAGES];
#endif

	VkDescriptorSetLayout       desc_set_layout_vertex_buffer;
	VkDescriptorSet             desc_set_vertex_buffer;
	
	BufferResource_t            buf_world;
	BufferResource_t            buf_primitive_instanced;
	BufferResource_t            buf_positions_instanced;

	BufferResource_t            buf_light;
	BufferResource_t            buf_light_staging[MAX_FRAMES_IN_FLIGHT];
	BufferResource_t            buf_light_stats[NUM_LIGHT_STATS_BUFFERS];
	BufferResource_t            buf_sharc_keys;
	BufferResource_t            buf_sharc_accum;
	BufferResource_t            buf_sharc_resolved;
	BufferResource_t            buf_light_counts_history[LIGHT_COUNT_HISTORY];
	
	BufferResource_t            buf_iqm_matrices;
	BufferResource_t            buf_iqm_matrices_staging[MAX_FRAMES_IN_FLIGHT];
	float*                      iqm_matrices_shadow;
	float*                      iqm_matrices_prev;

	BufferResource_t            buf_readback;
	BufferResource_t            buf_readback_staging[MAX_FRAMES_IN_FLIGHT];

	BufferResource_t            buf_tonemap;
	BufferResource_t            buf_sun_color;

    VkSampler                   tex_sampler, 
                                tex_sampler_nearest,
                                tex_sampler_nearest_mipmap_aniso,
                                tex_sampler_linear_clamp;
	
	float                       sintab[256];

	VkImage screenshot_image;
	VkDeviceMemory screenshot_image_memory;
	VkDeviceSize screenshot_image_memory_size;

#ifdef VKPT_IMAGE_DUMPS
	// host-visible image for dumping FB data through
	VkImage dump_image;
	VkDeviceMemory dump_image_memory;
	VkDeviceSize dump_image_memory_size;
#endif
} QVK_t;

extern QVK_t qvk;

#define LIST_EXTENSIONS_ACCEL_STRUCT \
	VK_EXTENSION_DO(vkCreateAccelerationStructureKHR) \
	VK_EXTENSION_DO(vkDestroyAccelerationStructureKHR) \
	VK_EXTENSION_DO(vkCmdBuildAccelerationStructuresKHR) \
	VK_EXTENSION_DO(vkCmdCopyAccelerationStructureKHR) \
	VK_EXTENSION_DO(vkGetAccelerationStructureDeviceAddressKHR) \
	VK_EXTENSION_DO(vkCmdWriteAccelerationStructuresPropertiesKHR) \
	VK_EXTENSION_DO(vkGetAccelerationStructureBuildSizesKHR) \
	VK_EXTENSION_DO(vkGetBufferDeviceAddress) \

#define LIST_EXTENSIONS_RAY_PIPELINE \
	VK_EXTENSION_DO(vkCreateRayTracingPipelinesKHR) \
	VK_EXTENSION_DO(vkCmdTraceRaysKHR) \
	VK_EXTENSION_DO(vkGetRayTracingShaderGroupHandlesKHR) \

#define LIST_EXTENSIONS_DEBUG \
	VK_EXTENSION_DO(vkDebugMarkerSetObjectNameEXT) \

#define LIST_EXTENSIONS_INSTANCE \
	VK_EXTENSION_DO(vkCmdBeginDebugUtilsLabelEXT) \
	VK_EXTENSION_DO(vkCmdEndDebugUtilsLabelEXT)

/* POST-MORTEM FOR A GPU FAULT.

   VK_ERROR_DEVICE_LOST is the only thing the engine ever learns about a
   shader that touches memory it does not own: the driver kills the context
   and every call after that fails. On NVIDIA it shows up in the Windows
   system log as nvlddmkm event 153 with NO event 4101 - an engine error
   recovery rather than a display-driver hang - and because the handler calls
   exit(1) rather than raising, the crash handler never runs and no
   Q2RTX_CrashReport is written. From the outside the process simply vanishes,
   which is why "it crashed" and "there is no crash report" are both true.

   These two extensions turn that into an answer:

     VK_EXT_device_fault reports the faulting address, its precision and the
       vendor description, and stays queryable AFTER the device is lost.
     VK_NV_device_diagnostic_checkpoints reports how far the queue actually
       got - one marker per profiled pass, written by begin_perf_marker - so
       the faulting pass is the one still at TOP_OF_PIPE while everything
       before it reached BOTTOM_OF_PIPE.

   Both are diagnostic-only. The checkpoint costs one command-stream token per
   pass and neither does any work until the device is already gone. */
#define LIST_EXTENSIONS_DEVICE_FAULT \
	VK_EXTENSION_DO(vkGetDeviceFaultInfoEXT)

#define LIST_EXTENSIONS_CHECKPOINTS \
	VK_EXTENSION_DO(vkCmdSetCheckpointNV) \
	VK_EXTENSION_DO(vkGetQueueCheckpointDataNV)

/* THE PARTITIONED TLAS (ptlas.h, pt_ptlas): NVIDIA's VK_NV_partitioned_acceleration_
   structure, the D3D12 renderer's NVAPI route's twin.

   OFF ON VULKAN (VKPT_PTLAS 0; GitHub issue #27, 11-08). pt_ptlas 1 crashed in the
   NVIDIA driver (access violation in nvoglv64, under vkUpdateDescriptorSets) the
   first frame it was on - a laptop 5090 in the report, and the same on the 5070 Ti
   here with a headless repro. On Vulkan a PTLAS needs a descriptor of ITS OWN TYPE,
   VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV, in a binding of that
   type. path_tracer.c, god_rays.c and flow.c wrote its address with
   VkWriteDescriptorSetPartitionedAccelerationStructureNV into their ordinary
   ACCELERATION_STRUCTURE_KHR bindings, which the validation layer rejects
   (VUID-VkWriteDescriptorSet-descriptorType-02382) and the driver dies on. With the
   right type on both sides the same write and build are clean. (On D3D12 a PTLAS is
   just a GPU address in the TLAS's own SRV, which is why that renderer is fine.)

   Doing it properly is more than a type swap: a binding has one type, and the rt
   set's binding 0 is topLevelAS[TLAS_COUNT] - the geometry TLAS and the effects TLAS
   side by side - so the geometry TLAS would need a binding of its own (shaders
   included), and since a classic TLAS may not go into a PARTITIONED binding either,
   the layouts and pipelines would have to be made for one or the other (pt_ptlas
   restarting the renderer, as pt_clusters does). Not worth it for an A/B that
   measured no gain (Matt, on the issue). At 0 the extension is never asked for,
   pt_ptlas_available stays 0, the menu row stays hidden and pt_ptlas does nothing;
   path_tracer.c's PTLAS code is kept for whoever does it properly. */
#define VKPT_PTLAS 0

#define LIST_EXTENSIONS_PTLAS \
	VK_EXTENSION_DO(vkGetPartitionedAccelerationStructuresBuildSizesNV) \
	VK_EXTENSION_DO(vkCmdBuildPartitionedAccelerationStructuresNV)

/* CLUSTERED DYNAMIC GEOMETRY (clusters.h, pt_clusters): VK_NV_cluster_acceleration_
   structure, the D3D12 renderer's NVAPI cluster operations' twin */
#define LIST_EXTENSIONS_CLUSTERS \
	VK_EXTENSION_DO(vkGetClusterAccelerationStructureBuildSizesNV) \
	VK_EXTENSION_DO(vkCmdBuildClusterAccelerationStructureIndirectNV)

// VK_EXT_full_screen_exclusive is Win32-only and lives in vulkan_win32.h, which this
// header deliberately does not pull in - it would drag windows.h through the whole
// renderer. It is loaded and used entirely inside main.c.

#define VK_EXTENSION_DO(a) extern PFN_##a q##a;
LIST_EXTENSIONS_ACCEL_STRUCT
LIST_EXTENSIONS_RAY_PIPELINE
LIST_EXTENSIONS_DEBUG
LIST_EXTENSIONS_INSTANCE
LIST_EXTENSIONS_DEVICE_FAULT
LIST_EXTENSIONS_CHECKPOINTS
LIST_EXTENSIONS_PTLAS
LIST_EXTENSIONS_CLUSTERS
#undef VK_EXTENSION_DO

/* Dumps everything the driver will still tell us about a lost device and does
   not return - see the comment on LIST_EXTENSIONS_DEVICE_FAULT. `context`
   names the call that reported the loss. */
void vkpt_report_device_lost(const char* context);

/* Counts recorded froxel dispatches - see the comment on its definition in
   god_rays.c. Printed as a per-frame delta in the FOGGRID line. */
extern uint32_t vkpt_froxel_dispatch_count;

#define MAX_SKY_CLUSTERS 1024

typedef mat3 prim_positions_t;

typedef struct
{
	uint8_t* geometry_storage;
	VkAccelerationStructureGeometryKHR* geometries;
	VkAccelerationStructureBuildRangeInfoKHR* build_ranges;
	uint32_t* prim_counts;
	uint32_t* prim_offsets;
	uint32_t num_geometries;
	uint32_t max_geometries;
	VkAccelerationStructureBuildSizesInfoKHR build_sizes;
	VkDeviceSize blas_data_offset;
	VkAccelerationStructureKHR accel;
	VkDeviceAddress blas_device_address;
	VkGeometryInstanceFlagsKHR instance_flags;
	uint32_t instance_mask;
	uint32_t sbt_offset;
} model_geometry_t;

/* This geometry has a static BLAS to instance. On Vulkan that is `accel` (the
   address comes with it); under vid_rtx 3 there is no Vulkan device and the
   D3D12 backend fills in only the address, of its own BLAS. */
static inline bool vkpt_model_geometry_has_blas(const model_geometry_t* geom)
{
	return geom->blas_device_address != 0 && (geom->accel != VK_NULL_HANDLE || qvk.device == VK_NULL_HANDLE);
}

typedef struct {
	BufferResource_t buffer;
	BufferResource_t staging_buffer;
	int registration_sequence;
	model_geometry_t geom_opaque;
	model_geometry_t geom_transparent;
	model_geometry_t geom_masked;
	size_t vertex_data_offset;
	uint32_t total_tris;
	bool is_static;
	// the backend holds this model's buffer (Vulkan's `buffer`, or the D3D12
	// backend's own) - what the shared code reads instead of a Vulkan handle
	bool present;
} model_vbo_t;

typedef struct
{
	model_geometry_t geometry;

	vec3_t center;
	vec3_t aabb_min;
	vec3_t aabb_max;

	int num_light_polys;
	int allocated_light_polys;
	light_poly_t *light_polys;

	bool transparent;
	bool masked;
} bsp_model_t;

typedef struct aabb_s {
	vec3_t mins;
	vec3_t maxs;
} aabb_t;

typedef struct bsp_mesh_s {
	bsp_model_t *models;
	int num_models;

	aabb_t world_aabb;

	VboPrimitive* primitives;
	uint32_t num_primitives_allocated;
	uint32_t num_primitives;
	size_t vertex_data_offset;

	model_geometry_t geom_opaque;
	model_geometry_t geom_transparent;
	model_geometry_t geom_masked;
	model_geometry_t geom_sky;
	model_geometry_t geom_custom_sky;

	int num_clusters;

	int num_cluster_lights;
	int *cluster_light_offsets;
	int *cluster_lights;

	int num_light_polys;
	int allocated_light_polys;
	light_poly_t *light_polys;

	// lights only the fog reads (lava), and their per-cluster lists, which
	// already hold final light_polys indices - see MAX_FOG_LIGHT_POLYS
	int num_fog_light_polys;
	int allocated_fog_light_polys;
	light_poly_t *fog_light_polys;
	int num_fog_cluster_lights;
	int *fog_cluster_light_offsets;
	int *fog_cluster_lights;

	uint32_t sky_clusters[MAX_SKY_CLUSTERS];
	int num_sky_clusters;
	bool all_lava_emissive;

	struct { vec3_t pos; vec3_t dir; } cameras[MAX_CAMERAS];
	int num_cameras;

	byte sky_visibility[VIS_MAX_BYTES];
	// clusters that actually CONTAIN sky geometry, as opposed to sky_visibility
	// above which is the far more permissive union of their PVS
	byte sky_cluster_mask[VIS_MAX_BYTES];

	aabb_t* cluster_aabbs;

	/* MATERIAL INSTANCE IDENTITY, one entry per world primitive.

	   A hash of the CONNECTED GROUP of BSP faces the primitive belongs to -
	   every face reachable from it across a shared edge that draws with the
	   same material - taken over the group's quantised vertex positions. It is
	   what lets one light fixture be tuned separately from another fixture
	   built out of the same texture: `mat create_instance` reads the hash under
	   the crosshair and writes a .mat section keyed on it.

	   Derived purely from BSP geometry, so it is stable across runs and across
	   rebuilds of this renderer - which it has to be, because it ends up
	   written into a material file by hash. 0 means "no group", which only
	   happens for primitives that did not come from a BSP face.
	   See build_face_instance_hashes in bsp_mesh.c. */
	uint32_t *prim_instance_hash;
} bsp_mesh_t;

void bsp_mesh_create_from_bsp(bsp_mesh_t *wm, bsp_t *bsp, const char* map_name);
void bsp_mesh_destroy(bsp_mesh_t *wm);
void bsp_mesh_register_textures(bsp_t *bsp);
void bsp_mesh_animate_light_polys(bsp_mesh_t *wm);
uint32_t encode_normal(const vec3_t normal);

typedef struct vkpt_refdef_s {
	QVKUniformBuffer_t uniform_buffer;
	InstanceBuffer uniform_instance_buffer;
	refdef_t *fd;
	float view_matrix[16];
	float projection_matrix[16];
	float view_projection_matrix[16];

	float view_matrix_prev[16];
	float projection_matrix_prev[16];
	float view_projection_matrix_prev[16];
	float z_near, z_far;

	bsp_mesh_t bsp_mesh_world;
	int bsp_mesh_world_loaded;

	lightstyle_t prev_lightstyles[MAX_LIGHTSTYLES];
} vkpt_refdef_t;

extern vkpt_refdef_t vkpt_refdef;

extern BufferResource_t buf_accel_scratch;

typedef struct sun_light_s {
	vec3_t direction;
	vec3_t direction_envmap;
	vec3_t color;
	float angular_size_rad;
	bool use_physical_sky;
	bool visible;
} sun_light_t;

typedef struct entity_hash_s {
	unsigned int mesh : 8;
	unsigned int model : 9;
	unsigned int entity : 14;
	unsigned int bsp : 1;
} entity_hash_t;

void mult_matrix_matrix(mat4_t p, const mat4_t a, const mat4_t b);
void mult_matrix_vector(vec4_t v, const mat4_t a, const vec4_t b);
void create_entity_matrix(mat4_t matrix, entity_t *e, bool enable_left_hand);
void create_projection_matrix(mat4_t matrix, float znear, float zfar, float fov_x, float fov_y);
void create_view_matrix(mat4_t matrix, refdef_t *fd);
void inverse(const mat4_t m, mat4_t inv);
void create_orthographic_matrix(mat4_t matrix, float xmin, float xmax,
		float ymin, float ymax, float znear, float zfar);

// the profiler's rows and actions, which the D3D12 renderer times too
#include "profiler.h"
#include "ptlas.h"

typedef struct EntityUploadInfo
{
	uint32_t num_instances;
	uint32_t num_prims;
	uint32_t opaque_prim_count;
	uint32_t transparent_prim_offset;
	uint32_t transparent_prim_count;
	uint32_t masked_prim_offset;
	uint32_t masked_prim_count;
	uint32_t viewer_model_prim_offset;
	uint32_t viewer_model_prim_count;
	uint32_t viewer_weapon_prim_offset;
	uint32_t viewer_weapon_prim_count;
	uint32_t explosions_prim_offset;
	uint32_t explosions_prim_count;
	// cl_blood_spheres. One more section of the instanced buffers, filled from
	// the CPU rather than by instance_geometry.comp - see blood.c.
	uint32_t blood_prim_offset;
	uint32_t blood_prim_count;
	bool weapon_left_handed;
} EntityUploadInfo;

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

/* The frame's CPU half (main.c), shared with the D3D12 renderer. */
typedef enum {
	VKPT_STAGING_LIGHTS,            // LightBuffer
	VKPT_STAGING_IQM_MATRICES,      // IqmMatrixBuffer
	VKPT_STAGING_LIGHT_COUNTS,      // uint per cluster, history entry frame_counter % LIGHT_COUNT_HISTORY
	VKPT_STAGING_READBACK,          // ReadbackBuffer, read: what this frame slot's last frame wrote
} vkpt_staging_t;

typedef struct vkpt_prep_backend_s {
	void *(*map_staging)(vkpt_staging_t which);     // this frame's, host-visible
	void (*unmap_staging)(vkpt_staging_t which);
	void (*ensure_primbuf_size)(uint32_t prim_count);
	void (*load_sky_preset)(int preset);            // a SkyPreset
	// R_SetSky_RTX's cube map: six w x h RGBA8 faces back to back, in its
	// order (ft bk up dn rt lf), sampled as TEX_ENVMAP
	void (*upload_envmap)(int w, int h, byte *data);
	// map load, once the BSP mesh is built: its buffers and static BLASes,
	// each geometry's blas_device_address filled in
	void (*load_world)(bsp_mesh_t *mesh, bsp_t *bsp);
	// map load, with blood on: staging for vkpt_blood_preallocate's capacity
	// (primitives), so the first droplet allocates nothing
	void (*reserve_blood_staging)(uint32_t prim_count);
	// the froxel grid's volumes, the first frame they are wanted
	// (vkpt_froxel_ensure, without a Vulkan device); false = not made. NULL on
	// Vulkan, which makes its own.
	bool (*create_froxel_volumes)(void);
	// cpu_bench's copy of the physical sky's cube: six w x h RGBA16F faces back to
	// back, Z_Malloc'd (vkpt_physical_sky_read_envmap); NULL = none. NULL on
	// Vulkan, which reads its own.
	uint16_t *(*read_sky_envmap)(int *width, int *height);
	// `mat reload` and `mat <attribute>` (vkpt_vertex_buffer_invalidate_static_model_vbos):
	// wait for the GPU, and drop model slot `index`'s buffers and BLASes so the next
	// frame uploads it again (vkpt_model_vbo_action). NULL on Vulkan, which does both
	// itself.
	void (*wait_idle)(void);
	void (*free_model)(int index);
} vkpt_prep_backend_t;

extern const vkpt_prep_backend_t *vkpt_prep_backend;

/* R_SetSky for both renderers: the six faces, the sky rotation the UBO's
   environment matrix uses, the map's skybox scope; the cube map goes to the
   GPU through vkpt_prep_backend->upload_envmap. */
void R_SetSky_RTX(const char *name, float rotate, int autorotate, const vec3_t axis);

/* R_BeginRegistration_RTX: the last map's world freed, this one's BSP loaded and
   its mesh built (CPU), then vkpt_prep_backend->load_world. */
void vkpt_begin_registration(const char *name);
/* R_EndRegistration_RTX's CPU half. */
void vkpt_end_registration(void);
/* The world mesh and BSP freed (renderer shutdown, vid_rtx 3). */
void vkpt_unload_world(void);

typedef struct vkpt_frame_prep_s {
	bool render_world;
	float frame_time;
	float frame_wallclock_time;
	bool update_world_animations;
	reference_mode_t ref_mode;
	EntityUploadInfo upload_info;
	sun_light_t sun_light;
	float shadowmap_view_proj[16];
	float shadowmap_depth_scale;
	bool god_rays_enabled;
	bool fog_accum_march;
	bool run_froxel;
	bool god_rays_march_discarded;
	// what vkpt_blood_update / vkpt_blood_prepare take from the preparation
	int blood_model_instance;           // prepare_entities' blood ModelInstance, -1 = none
	bsp_t *bsp;                         // the world's BSP
	// the radiance cache (pt_sharc): whether its update runs this frame, and
	// whether it starts from empty (first use, a new map, back on after a gap)
	bool sharc_active, sharc_clear;
} vkpt_frame_prep_t;

bool vkpt_prepare_frame(refdef_t *fd, int waterLevel, vkpt_frame_prep_t *prep);
/* After the frame is recorded: the history flags the next vkpt_prepare_frame reads. */
void vkpt_finish_frame(const vkpt_frame_prep_t *prep);
void vkpt_init_frontend(void);
void vkpt_bloom_init_cvars(void);
void vkpt_transparency_init_cvars(void);
void vkpt_blood_init_cvars(void);
void vkpt_vertex_buffer_init_cvars(void);
void vkpt_physical_sky_load_preset(int preset);
/* For a backend that owns none of the Vulkan images (vid_rtx 3): the render,
   screen-image and TAA extents for an output of qvk.extent_unscaled, as
   vkpt_initialize_all sets them. */
void vkpt_compute_extents(void);
/* The TLAS instance list the frame's CPU half built (vkpt_pt_instance_model_blas:
   the static world, brush models, static models). Each entry is laid out as
   VkAccelerationStructureInstanceKHR - and so as D3D12_RAYTRACING_INSTANCE_DESC. */
const void *vkpt_pt_instance_list(uint32_t *count);
/* The ranges of the light staging buffer an upload copies (WHAT THE LIGHT AND IQM
   UPLOADS COPY, vertex_buffer.c): 1 or 2, at the same offset in both buffers.
   Call once per upload - the first after a reset is the whole buffer. */
uint32_t vkpt_light_upload_regions(uint64_t offsets[2], uint64_t sizes[2]);
/* The light and IQM staging buffers were (re)created: the next uploads are whole. */
void vkpt_upload_ranges_reset(void);
/* How many bytes from the start of the IQM staging buffer this frame's upload
   copies (0: nothing the device does not hold). Call once per upload. */
uint64_t vkpt_iqm_upload_size(void);

/* The instanced buffers' size, in primitives: pt_primbuf, clamped. */
uint32_t vkpt_primbuf_size(void);
/* ensure_primbuf_size's policy when prim_count does not fit: pt_primbuf grows to
   hold it (rounded up), for the next vkpt_primbuf_size. */
void vkpt_primbuf_grow(uint32_t prim_count);

/* THE MODEL VBOs' CPU HALF (vertex_buffer.c), walked by vkpt_vertex_buffer_upload_models
   and the D3D12 backend alike, slot by slot of model_vertex_data. */
typedef enum {
	VKPT_MODEL_VBO_KEEP,    // no model, or its VBO is current
	VKPT_MODEL_VBO_FREE,    // the model is gone: free its VBO, then vkpt_model_vbo_clear
	VKPT_MODEL_VBO_UPLOAD,  // new, or registered again since: clear, lay out, stage, upload
} vkpt_model_vbo_action_t;
vkpt_model_vbo_action_t vkpt_model_vbo_action(int index);
/* The slot's CPU state freed and zeroed (the backend frees its buffers first). */
void vkpt_model_vbo_clear(int index);
/* Lays out a cleared slot for r_models[index]: is_static, total_tris, the geometries a
   static model's BLASes are built from (prim offsets into its positions). Returns the
   bytes to stage: VboPrimitive[total_tris], then - a static model only - the positions
   at vertex_data_offset, 9 floats per triangle. */
size_t vkpt_model_vbo_layout(int index);
/* Writes those bytes (zeroing them first) and every mesh's tri_offset. */
void vkpt_model_vbo_stage(int index, uint8_t *staging, size_t size);
/* The backend made the slot's buffer (present) for this registration. */
void vkpt_model_vbo_mark_uploaded(int index, bool present);
/* A static model's geometries' TLAS instance masks, flags and SBT offsets. */
void vkpt_model_vbo_set_instance_flags(int index);
/* The slots, one per r_models entry below MAX_MODELS (vertex_buffer.c). */
extern model_vbo_t model_vertex_data[MAX_MODELS];

/* vkpt_pt_create_toplevel's dynamic BLASes, in the order they join the instance list. */
typedef enum {
	VKPT_DYN_OPAQUE,            // the instanced buffer's model sections
	VKPT_DYN_TRANSPARENT,
	VKPT_DYN_MASKED,
	VKPT_DYN_VIEWER_WEAPON,
	VKPT_DYN_VIEWER_MODELS,
	VKPT_DYN_BLOOD,
	VKPT_DYN_EXPLOSIONS,        // the effects TLAS from here on
	VKPT_DYN_PARTICLES,
	VKPT_DYN_BEAMS,
	VKPT_DYN_SPRITES,
	VKPT_NUM_DYN
} vkpt_dyn_blas_t;
/* Appends this frame's dynamic BLASes (device addresses, 0 = none this frame) to the
   instance list, with their entries in the instance buffer. Returns how many of the
   list's instances are geometry; the rest, up to vkpt_pt_instance_list's count, are
   the effects TLAS's. */
uint32_t vkpt_pt_append_dynamic_instances(const uint64_t blas[VKPT_NUM_DYN], const EntityUploadInfo *upload_info,
	bool weapon_left_handed);
/* The TLAS instance masks, flags and SBT offsets of the world's geometries. */
void vkpt_bsp_mesh_set_instance_flags(bsp_mesh_t* bsp_mesh);

VkDescriptorSet qvk_get_current_desc_set_textures(void);

VkResult vkpt_profiler_initialize(void);
VkResult vkpt_profiler_destroy(void);
VkResult vkpt_profiler_query(VkCommandBuffer cmd_buf, int idx, VKPTProfilerAction action);
// the same, but the time is taken once everything submitted before has finished
VkResult vkpt_profiler_query_after(VkCommandBuffer cmd_buf, int idx, VKPTProfilerAction action);

VkResult vkpt_reset_query_pool(VkCommandBuffer cmd_buf);
VkResult vkpt_profiler_next_frame(VkCommandBuffer cmd_buf, qboolean secondPass);
// draw_profiler, vkpt_profiler_dump, vkpt_get_profiler_result: profiler.h

VkResult vkpt_readback(ReadbackBuffer* dst);

VkResult vkpt_textures_initialize(void);
VkResult vkpt_textures_destroy(void);
VkResult vkpt_textures_end_registration(void);
VkResult vkpt_textures_upload_envmap(int w, int h, byte *data);
void vkpt_textures_destroy_unused(void);
void vkpt_textures_update_descriptor_set(void);
/* Which sampler an image is read through - see textures.c. API-free, shared with
   the D3D12 renderer, whose D3D12_GLOBAL_SAMPLER_* are in this order. */
enum {
	VKPT_SAMPLER_DEFAULT,                // qvk.tex_sampler
	VKPT_SAMPLER_NEAREST,                // qvk.tex_sampler_nearest
	VKPT_SAMPLER_NEAREST_MIPMAP_ANISO,   // qvk.tex_sampler_nearest_mipmap_aniso
	VKPT_SAMPLER_LINEAR_CLAMP,           // qvk.tex_sampler_linear_clamp
	VKPT_NUM_SAMPLER_KINDS
};
int vkpt_image_sampler_kind(const image_t *q_img, int pt_nearest, int bilerp_chars, int bilerp_pics);
image_t *vkpt_fake_emissive_texture(image_t *image, int bright_threshold_int);
void vkpt_extract_emissive_texture_info(image_t *image);
void vkpt_fold_emissive_alpha(image_t *image);
void vkpt_textures_prefetch(void);
void vkpt_find_permanent_images(void);
void vkpt_invalidate_texture_descriptors(void);
void vkpt_init_light_textures(void);

VkCommandBuffer vkpt_begin_command_buffer(cmd_buf_group_t* group);
// Sets sharingMode / queueFamilyIndexCount / pQueueFamilyIndices on an image
// create info so the image may be used by both queue_graphics and
// queue_compute with no ownership transfer. Leaves EXCLUSIVE when there is no
// separate compute family.
void vkpt_image_sharing_graphics_compute(VkImageCreateInfo* info);
void vkpt_free_command_buffers(cmd_buf_group_t* group);
void vkpt_reset_command_buffers(cmd_buf_group_t* group);
/* vkQueueWaitIdle with the swapchain/queue lock held. A VkQueue needs external
   synchronisation and the frame generation present thread presents on the graphics
   queue, so every queue operation - including the wait-idles scattered through texture
   and geometry upload - has to be serialised against it. */
VkResult vkpt_queue_wait_idle(VkQueue queue);

/* vkDeviceWaitIdle with the same lock held. vkDeviceWaitIdle is equivalent to waiting
   on EVERY queue, so it carries the same external-synchronisation requirement as
   vkQueueWaitIdle and races the frame generation present thread without it. This was
   the source of the "VkQueue is simultaneously used" reports that clustered around
   swapchain recreation, which is where most of these calls live. */
void vkpt_device_wait_idle(void);

void vkpt_wait_idle(VkQueue queue, cmd_buf_group_t* group);

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
	VkFence fence);

void vkpt_submit_command_buffer_simple(
	VkCommandBuffer cmd_buf,
	VkQueue queue,
	bool all_gpus);


#define ALL_GPUS (-1)
void set_current_gpu(VkCommandBuffer cmd_buf, int gpu_index);

VkResult allocate_gpu_memory(VkMemoryRequirements mem_req, VkDeviceMemory* pMemory);

#ifdef VKPT_DEVICE_GROUPS
void vkpt_mgpu_global_barrier(VkCommandBuffer cmd_buf);

void
vkpt_mgpu_image_copy(VkCommandBuffer cmd_buf,
					int src_image_index,
					int dst_image_index,
					int src_gpu_index,
					int dst_gpu_index,
					VkOffset2D src_offset,
					VkOffset2D dst_offset,
					VkExtent2D size);
#endif

void
vkpt_image_copy(VkCommandBuffer cmd_buf,
					int src_image_index,
					int dst_image_index,
					VkOffset2D src_offset,
					VkOffset2D dst_offset,
					VkExtent2D size);

VkResult vkpt_draw_initialize(void);
VkResult vkpt_draw_destroy(void);
VkResult vkpt_draw_destroy_pipelines(void);
VkResult vkpt_draw_create_pipelines(void);
VkResult vkpt_draw_submit_stretch_pics(VkCommandBuffer cmd_buf);
/* Same, but leaves the queued pics in place so the HUD can be drawn a second time
   into another swapchain image. Frame generation needs the HUD on BOTH the real and
   the interpolated frame, otherwise it flickers at half the presented rate. */
VkResult vkpt_draw_submit_stretch_pics_keep(VkCommandBuffer cmd_buf);
VkResult vkpt_final_blit_simple(VkCommandBuffer cmd_buf, VkImage image, VkExtent2D extent);
VkResult vkpt_final_blit_simpleDLSS(VkCommandBuffer cmd_buf, VkImage image, VkExtent2D extent);
VkResult vkpt_final_blit_filtered(VkCommandBuffer cmd_buf);
/* Film grain and the CRT filter (pt_film_grain*, pt_crt*), added by the final
   blits - see draw.c, shader/film_grain.glsl and shader/crt.frag. */
void vkpt_film_grain_init_cvars(void);
/* FinalBlitPushConstants in shader/final_blit_pc.glsl; the D3D12 blit sends the same. */
typedef struct {
	int32_t  source;
	int32_t  src_width, src_height;
	int32_t  out_width, out_height;
	int32_t  hdr;
	float    hdr_white_nits;
	uint32_t seed;

	float    grain_intensity;
	float    grain_sigma_px;
	int32_t  grain_radius;

	int32_t  crt_mode;
	int32_t  crt_grid_width, crt_grid_height;
	float    crt_warp_x, crt_warp_y;
	float    crt_corner_radius_px;
	float    crt_hard_pix;
	float    crt_scan_hard_dark, crt_scan_hard_bright;
	float    crt_mask_strength;
	int32_t  crt_mask_pitch;
	float    crt_glow;
	float    crt_vignette;
} vkpt_final_blit_push_t;
// grain or the CRT on this frame: the final blit then draws through them
bool vkpt_final_blit_effects_enabled(void);
// draw.c's final_blit_params without the swapchain's part, for an out_width x
// out_height target: grain and CRT (zero / off when off), the next seed, source -1
void vkpt_final_blit_effects_params(int out_width, int out_height, vkpt_final_blit_push_t *pc);
/* ui_hdr_video_gamma: the SDR->HDR curve for cinematic frames - see draw.c. */
void vkpt_draw_init_cvars(void);
// stretch_pic.frag's UBO values (ui_hdr_nits, tm_hdr_saturation_scale, ui_hdr_video_gamma)
void vkpt_draw_ui_hdr_params(float *ui_hdr_nits, float *saturation_scale, float *video_gamma);
// ...and its ui_hdr_gamma: tm_hdr_sdr_gamma, the UI's shadows as an SDR monitor shows them
float vkpt_draw_ui_hdr_gamma(void);
VkResult vkpt_draw_clear_stretch_pics(void);
/* The 2D queue the R_Draw*_RTX functions fill, for the D3D12 renderer, which
   draws it with the same shaders (src/refresh/d3d12/frontend.c). Records are
   StretchPic_t: shader/stretch_pic.vert's StretchPic, *record_size bytes. */
const void *vkpt_draw_stretch_pic_queue(int *count, size_t *record_size, int *max_count);

VkResult vkpt_uniform_buffer_create(void);
VkResult vkpt_uniform_buffer_destroy(void);
VkResult vkpt_uniform_buffer_upload_to_staging(void);
void vkpt_uniform_buffer_copy_from_staging(VkCommandBuffer command_buffer);
void vkpt_uniform_buffer_set_model_instance_count(uint32_t count);

void vkpt_init_model_geometry(model_geometry_t* info, uint32_t max_geometries);
void vkpt_destroy_model_geometry(model_geometry_t* info);
void vkpt_append_model_geometry(model_geometry_t* info, uint32_t num_prims, uint32_t prim_offset, const char* model_name);
VkResult vkpt_vertex_buffer_create(void);
VkResult vkpt_vertex_buffer_destroy(void);
void vkpt_vertex_buffer_ensure_primbuf_size(uint32_t prim_count);
uint32_t vkpt_primbuf_generation(void);
// A backend made its instanced buffers afresh: blood's device copy is void
void vkpt_primbuf_new_generation(void);
VkResult vkpt_vertex_buffer_upload_bsp_mesh(bsp_mesh_t* bsp_mesh);
void vkpt_vertex_buffer_cleanup_bsp_mesh(bsp_mesh_t *bsp_mesh);
VkResult vkpt_vertex_buffer_create_pipelines(void);
VkResult vkpt_vertex_buffer_destroy_pipelines(void);
VkResult vkpt_instance_geometry(VkCommandBuffer cmd_buf, uint32_t num_instances, bool update_world_animations);
void vkpt_sharc_clear(VkCommandBuffer cmd_buf);
void vkpt_sharc_resolve(VkCommandBuffer cmd_buf);
void vkpt_vertex_buffer_invalidate_static_model_vbos(int material_index);
VkResult vkpt_vertex_buffer_upload_models(void);
void vkpt_light_buffer_reset_counts(void);
VkResult vkpt_light_buffer_upload_to_staging(bool render_world, bsp_mesh_t *bsp_mesh, bsp_t* bsp, int num_model_lights, light_poly_t* transformed_model_lights, const float* sky_radiance);
VkResult vkpt_light_buffer_upload_staging(VkCommandBuffer cmd_buf);
VkResult vkpt_light_buffers_create(bsp_mesh_t *bsp_mesh);
VkResult vkpt_light_buffers_destroy(void);
bool vkpt_model_is_static(const model_t* model);
const model_vbo_t* vkpt_get_model_vbo(const model_t* model);

VkResult vkpt_iqm_matrix_buffer_upload_staging(VkCommandBuffer cmd_buf);
void vkpt_iqm_matrix_buffer_staged(uint32_t num_matrices);

VkResult vkpt_load_shader_modules(void);
VkResult vkpt_destroy_shader_modules(void);
VkResult vkpt_create_images(void);
VkResult vkpt_destroy_images(void);

VkResult vkpt_pt_init(void);
void vkpt_pt_init_cvars(void);
VkResult vkpt_pt_destroy(void);
VkResult vkpt_pt_create_pipelines(void);
// tile lists for the reflect/refract bounces, sized with the screen images (TILE COMPACTION, shader/constants.h)
VkResult vkpt_pt_create_tiles(void);
VkResult vkpt_pt_destroy_tiles(void);
// ...remade at a frame's start (the GPU idle) when pt_bounce_cull's path lists change it
void vkpt_pt_ensure_tiles(void);
/* ...and their API-free half, which the D3D12 backend calls too: the lists' layout
   for screen images of a size, and this view's header (false: no lists this view -
   pt_tile_compaction 0, or more tiles than one list's dispatch can hold). */
typedef struct {
	uint32_t max;           // tile ids one list can hold
	uint32_t mask_stride;   // words per list's dedupe mask
	uint32_t data_offset;   // word offset of list 0's tile ids
	// PATH LISTS (pt_bounce_cull, shader/constants.h), after the tile ids: 0 regions
	// while it is off, so the buffer costs nothing extra then
	uint32_t path_regions;  // 0, 1 (two bounces) or 2 (more)
	uint32_t path_stride;   // words per region: every pixel a launch grid can have
	uint32_t path_offset;   // word offset of region 0
	uint64_t size;          // the buffer, in bytes
} vkpt_tile_layout_t;
void vkpt_pt_tile_layout(uint32_t screen_width, uint32_t screen_height, vkpt_tile_layout_t *layout);
bool vkpt_pt_tile_header(const vkpt_tile_layout_t *layout, uint32_t max_groups, uint32_t header[PT_TILE_HDR_WORDS]);

#ifdef _WIN32
/* The GDI display name (DISPLAY1 and friends) of the monitor qvk.window is on (main.c): frame
   generation's scanout clock and VRR detection key off it, in either renderer. */
bool vkpt_get_window_gdi_device(char *out, size_t out_size);
#endif
VkResult vkpt_pt_destroy_pipelines(void);
// pt_flow_split_pass ran the Flow march as its own pass last view (PROFILER_FLOW_MARCH)
bool vkpt_pt_flow_split_ran(void);
// ...on the D3D12 renderer, which records that pass itself
void vkpt_pt_set_flow_split_ran(bool ran);

void vkpt_pt_reset_instances(void);
// vkpt_prepare_frame: the instances so far are the static world's (a partitioned
// TLAS's global partition, ptlas.h)
void vkpt_pt_mark_world_instances(void);
uint32_t vkpt_pt_num_world_instances(void);
void vkpt_pt_instance_model_blas(const model_geometry_t* geom, const mat4 transform, uint32_t buffer_idx, int model_instance_index, uint32_t override_instance_mask);

VkResult vkpt_pt_create_toplevel(VkCommandBuffer cmd_buf, int idx, const EntityUploadInfo* upload_info, bool weapon_left_handed);
VkResult vkpt_pt_trace_primary_rays(VkCommandBuffer cmd_buf);
VkResult vkpt_pt_trace_reflections(VkCommandBuffer cmd_buf, int bounce);
VkResult vkpt_pt_trace_lighting(VkCommandBuffer cmd_buf, float num_bounce_rays);
VkResult vkpt_pt_sharc_update(VkCommandBuffer cmd_buf, bool clear);
VkResult vkpt_pt_update_descripter_set_bindings(int idx);
VkAccelerationStructureKHR vkpt_pt_get_geometry_tlas(int idx);
/* A descriptor write of frame slot idx's geometry TLAS: the classic one's handle,
   or - with pt_ptlas - the partitioned TLAS's address, which a descriptor takes
   through VkWriteDescriptorSetPartitionedAccelerationStructureNV. Points into
   itself: fill it where it stays. Returns the VkWriteDescriptorSet's pNext for
   one descriptor, or NULL when the slot has no TLAS (nothing to write). */
typedef struct {
	VkWriteDescriptorSetAccelerationStructureKHR            khr;
	VkWriteDescriptorSetPartitionedAccelerationStructureNV  nv;
	VkAccelerationStructureKHR                              handle;
	VkDeviceAddress                                         address;
} vkpt_tlas_write_t;
const void *vkpt_pt_geometry_tlas_write(int idx, vkpt_tlas_write_t *w);
VkResult vkpt_pt_create_all_dynamic(VkCommandBuffer cmd_buf, int idx, const EntityUploadInfo* upload_info);

VkResult vkpt_asvgf_initialize(void);
VkResult vkpt_asvgf_destroy(void);
VkResult vkpt_asvgf_create_pipelines(void);
VkResult vkpt_asvgf_destroy_pipelines(void);
VkResult vkpt_asvgf_filter(VkCommandBuffer cmd_buf, bool enable_lf);
VkResult vkpt_compositing(VkCommandBuffer cmd_buf);
VkResult vkpt_interleave(VkCommandBuffer cmd_buf);
VkResult vkpt_taa(VkCommandBuffer cmd_buf);
VkResult vkpt_asvgf_gradient_reproject(VkCommandBuffer cmd_buf);

/* WHICH SCREEN IMAGES ARE WORTH ALLOCATING THIS SESSION.

   Every image in LIST_IMAGES used to be allocated at full size whether or not
   anything would ever write it, which at 4K with split fields cost about 2 GB for
   subsystems that were switched off - the A-SVGF chain alone is 25 images, and
   pt_dlss_bypass_denoiser 1 means it never runs at all.

   An inactive image is still CREATED, so every descriptor stays valid and no shader
   or barrier needs a special case; it is just created 1x1. The groups below are the
   ones whose passes are skipped wholesale, and the profile is compared each frame in
   R_RenderFrame_RTX so that turning a subsystem back on rebuilds the images. */
#define SCREEN_IMG_GROUP_ASVGF_FILTER  (1u << 0)
#define SCREEN_IMG_GROUP_ACCUM         (1u << 2)
uint32_t vkpt_screen_image_profile(void);
// image_is_active, for the D3D12 renderer's G-buffer (textures.c)
bool vkpt_screen_image_active(int index, uint32_t profile);

VkResult vkpt_bloom_initialize(void);
VkResult vkpt_bloom_destroy(void);
VkResult vkpt_bloom_create_pipelines(void);
VkResult vkpt_bloom_destroy_pipelines(void);
void vkpt_bloom_reset(void);
void vkpt_bloom_update(QVKUniformBuffer_t * ubo, float frame_time, bool under_water, bool menu_mode, float ui_blur);

/* HDR OUTPUT (hdr_display.c), both path tracers: what the screen the window is on
   reports, the per-frame HDR values of the UBO, the test patterns. */
typedef struct {
	float max_mastering_nits;   // the peak the frame is tone-mapped to
	float min_mastering_nits;   // the screen's reported black, 0 if unknown
	float max_cll;              // brightest pixel: the peak
	float max_fall;             // brightest frame average: the screen's full-frame limit, else game brightness
} vkpt_hdr_metadata_t;

void  vkpt_hdr_init_cvars(void);                    // vkpt_init_frontend
void  vkpt_hdr_register_commands(void);             // hdr_detect (vkpt_register_commands)
void  vkpt_hdr_unregister_commands(void);
void  vkpt_hdr_detect_display(void);                // the window's screen, asked again (a new swapchain)
void  vkpt_hdr_prepare_ubo(QVKUniformBuffer_t *ubo, bool hdr_page_open, bool render_world);
bool  vkpt_hdr_calibrating(void);                   // a test pattern is on screen this frame
float vkpt_hdr_peak_nits(void);                     // tm_hdr_peak_nits, 0 resolved
void  vkpt_hdr_metadata(vkpt_hdr_metadata_t *md);   // for the swapchain's HDR10 metadata
VkResult vkpt_bloom_record_cmd_buffer(VkCommandBuffer cmd_buf);
// whether R_EndFrame_RTX records bloom this frame; motion blur's copy hand-off depends on it (bloom.c)
bool vkpt_bloom_will_run(void);
// asvgf.c's taa_folded: the interleave pass writes TAA_OUTPUT, and TAA is skipped
bool vkpt_taa_folded(void);
// vid_present_stats (main.c's report) for the D3D12 renderer: whether it is on, and a
// frame's two waits in microseconds - its fence, and the presentation - with the
// presentation's name and image count for the report
bool vkpt_present_stats_enabled(void);
void vkpt_present_stats_frame(uint64_t fence_us, uint64_t present_wait_us, const char *mode, uint32_t images);
// the FG debug tools (pt_dlss_fg_compare, pt_dlss_fg_dump; main.c) for the D3D12
// renderer: whether either is on, and the tools over `read` - RGBA16F texels of a
// VKPT_IMG_* image's w x h region at (x, y), packed, the GPU idle around it
typedef bool (*vkpt_fg_read_t)(int image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t *out);
bool vkpt_fg_debug_wanted(void);
void vkpt_fg_debug_frames(unsigned int generated, vkpt_fg_read_t read);
// the console commands both path tracers have (fog, camera, cpu_bench, show_pvs, ...)
void vkpt_register_commands(void);
void vkpt_unregister_commands(void);
// main.c's dlss_reset_history, consumed by the frame evaluating DLSS (both renderers);
// render_world false = the player setup's model view (photo mode's preview rule)
bool vkpt_dlss_take_history_reset(bool render_world);
// DLSS 5's (DLSS5Apply's resetHistory, and the D3D12 renderer's evaluation): taken once
bool vkpt_dlss5_take_history_reset(void);
// ...and dlssg_reset_history (the D3D12 renderer's frame generation)
bool vkpt_dlssg_take_history_reset(void);
// the blur's Gaussian at an output extent_height tall (both backends record it)
void vkpt_bloom_blur_params(uint32_t extent_height, float *argument_scale, float *normalization_scale, int *num_samples);
void vkpt_bloom_take_motion_blur(void);

/* Screen-space motion blur - see motion_blur.c and shader/motion_blur.comp. */
VkResult vkpt_motion_blur_initialize(void);
VkResult vkpt_motion_blur_destroy(void);
VkResult vkpt_motion_blur_create_pipelines(void);
VkResult vkpt_motion_blur_destroy_pipelines(void);

VkResult vkpt_dlssg_hdr_initialize(void);
VkResult vkpt_dlssg_hdr_destroy(void);
VkResult vkpt_dlssg_hdr_create_pipelines(void);
VkResult vkpt_dlssg_hdr_destroy_pipelines(void);
bool vkpt_dlssg_hdr_active(void);
void vkpt_dlssg_hdr_convert(VkCommandBuffer cmd, int target, bool decode, VkExtent2D extent);
void vkpt_motion_blur_init_cvars(void);
bool vkpt_motion_blur_is_enabled(void);
void vkpt_motion_blur_update(float frame_time);
VkResult vkpt_motion_blur_record_cmd_buffer(VkCommandBuffer cmd_buf);
/* The pass's push constants, worked out once for both backends: the D3D12
   renderer dispatches the same shader. MotionBlurPushConstants in motion_blur.comp. */
typedef struct {
	int32_t output_width,  output_height;
	int32_t image_width,   image_height;
	int32_t motion_width,  motion_height;
	int32_t render_width,  render_height;

	float strength;
	float max_blur_px;
	float dynamic_deduction;
	float jitter;
	float min_velocity_px;
	float soft_z_extent;

	int32_t num_samples;
	int32_t frame_idx;
	int32_t dlss_target;
} vkpt_motion_blur_push_t;
// this frame's (DLSSEnabled picks TAA_OUTPUT or DLSS_OUTPUT); false = an empty frame
bool vkpt_motion_blur_params(vkpt_motion_blur_push_t *push);
// the DLSS arm's hand-off: bloom's downscale writes the result back, no copy
bool vkpt_motion_blur_bloom_takes_copy(void);

/* Lateral chromatic aberration - see chromatic_aberration.c and
   shader/chromatic_aberration.comp. Borrows motion blur's scratch image. */
VkResult vkpt_chromatic_aberration_initialize(void);
VkResult vkpt_chromatic_aberration_destroy(void);
VkResult vkpt_chromatic_aberration_create_pipelines(void);
VkResult vkpt_chromatic_aberration_destroy_pipelines(void);
void vkpt_chromatic_aberration_init_cvars(void);
bool vkpt_chromatic_aberration_is_enabled(void);
VkResult vkpt_chromatic_aberration_record_cmd_buffer(VkCommandBuffer cmd_buf);
/* ChromaticAberrationPushConstants in chromatic_aberration.comp, for both backends. */
typedef struct {
	int32_t output_width,  output_height;
	int32_t image_width,   image_height;

	float spread;
	int32_t dlss_target;
} vkpt_chromatic_aberration_push_t;
// this frame's, as vkpt_motion_blur_params
bool vkpt_chromatic_aberration_params(vkpt_chromatic_aberration_push_t *push);

VkResult vkpt_tone_mapping_initialize(void);
VkResult vkpt_tone_mapping_destroy(void);
VkResult vkpt_tone_mapping_create_pipelines(void);
VkResult vkpt_tone_mapping_reset(VkCommandBuffer cmd_buf);
VkResult vkpt_tone_mapping_destroy_pipelines(void);
VkResult vkpt_tone_mapping_record_cmd_buffer(VkCommandBuffer cmd_buf, float frame_time, bool bDlssEnabled);
void vkpt_tone_mapping_request_reset(void);
void vkpt_tone_mapping_draw_debug(void);

VkResult vkpt_shadow_map_initialize(void);
VkResult vkpt_shadow_map_destroy(void);
VkResult vkpt_shadow_map_create_pipelines(void);
VkResult vkpt_shadow_map_destroy_pipelines(void);
VkResult vkpt_shadow_map_render(VkCommandBuffer cmd_buf, float* view_projection_matrix,
	uint32_t static_offset, uint32_t num_static_verts,
	uint32_t dynamic_offset, uint32_t num_dynamic_verts,
	uint32_t transparent_offset, uint32_t num_transparent_verts);
VkImageView vkpt_shadow_map_get_view(void);
void vkpt_shadow_map_setup(const sun_light_t* light, const float* bbox_min, const float* bbox_max,
	float* VP, float* depth_scale, bool random_sampling);
/* A static model or brush entity the sun's shadow map draws (vkpt_prepare_frame's
   entity walk): its positions in vertex buffer `buffer_idx` - VERTEX_BUFFER_WORLD,
   or VERTEX_BUFFER_FIRST_MODEL + its model slot - from byte `vertex_offset` on. An
   index rather than a VkBuffer: the D3D12 renderer draws the same list. */
typedef struct {
	mat4_t model_matrix;
	uint32_t buffer_idx;
	size_t vertex_offset;
	uint32_t prim_count;
} vkpt_shadow_map_instance_t;

void vkpt_shadow_map_reset_instances(void);
void vkpt_shadow_map_add_instance(const float* model_matrix, uint32_t buffer_idx, size_t vertex_offset, uint32_t prim_count);
uint32_t vkpt_shadow_map_instances(const vkpt_shadow_map_instance_t** list);

int load_img(const char *name, image_t *image);
// Transparency module API

bool initialize_transparency(void);
void destroy_transparency(void);

void update_transparency(VkCommandBuffer command_buffer, const float* view_matrix,
	const particle_t* particles, int particle_num, const entity_t* entities, int entity_num);

// The CPU half both backends share (transparency.c): the frame's particles,
// beams and sprites written into a CPU buffer, then copied by the backend -
// each stream to the start of its own device buffer.
enum {
	VKPT_TR_VERTICES,           // float3: the particles' quads, then the sprites'
	VKPT_TR_BEAM_AABBS,         // VkAabbPositionsKHR / D3D12_RAYTRACING_AABB
	VKPT_TR_PARTICLE_COLORS,    // RGBA32F texel buffer
	VKPT_TR_BEAM_COLORS,        // RGBA32F texel buffer
	VKPT_TR_SPRITE_INFOS,       // RGBA32UI texel buffer
	VKPT_TR_BEAM_INTERSECT,     // RGBA32UI texel buffer, 3 texels a beam
	VKPT_TR_NUM_STREAMS,
	VKPT_TR_INDICES = VKPT_TR_NUM_STREAMS,  // uint16 quads, filled once (capacity only)
};
typedef struct {
	const void* data;           // the frame, upload_size bytes
	size_t upload_size;
	size_t offset[VKPT_TR_NUM_STREAMS], size[VKPT_TR_NUM_STREAMS];
	uint32_t particle_num, beam_num, sprite_num;
	size_t sprite_vertex_offset; // in the vertex buffer, bytes
} vkpt_transparency_frame_t;
bool vkpt_transparency_cpu_init(void);
void vkpt_transparency_cpu_shutdown(void);
// false: nothing this frame (the counts are 0)
bool vkpt_transparency_prepare(const float* view_matrix,
	const particle_t* particles, int particle_num, const entity_t* entities, int entity_num);
void vkpt_transparency_frame(vkpt_transparency_frame_t* frame);
size_t vkpt_transparency_frame_capacity(void);
size_t vkpt_transparency_capacity(int stream);      // a device buffer's size
void vkpt_transparency_fill_indices(uint16_t* indices);  // capacity(VKPT_TR_INDICES) bytes

typedef enum {
	VKPT_TRANSPARENCY_PARTICLES,
	VKPT_TRANSPARENCY_SPRITES,

	VKPT_TRANSPARENCY_COUNT
} vkpt_transparency_t;

void vkpt_get_transparency_buffers(
	vkpt_transparency_t ttype, 
	BufferResource_t** vertex_buffer,
	uint64_t* vertex_offset, 
	BufferResource_t** index_buffer,
	uint64_t* index_offset,
	uint32_t* num_vertices,
	uint32_t* num_indices);
void vkpt_get_beam_aabb_buffer(
	BufferResource_t** aabb_buffer,
	uint64_t* aabb_offset,
	uint32_t* num_aabbs);

VkBufferView get_transparency_particle_color_buffer_view(void);
VkBufferView get_transparency_beam_color_buffer_view(void);
VkBufferView get_transparency_sprite_info_buffer_view(void);
VkBufferView get_transparency_beam_intersect_buffer_view(void);
void get_transparency_counts(int* particle_num, int* beam_num, int* sprite_num);

// Blood droplets as shaded sphere geometry - blood.c
VkResult vkpt_blood_initialize(void);
VkResult vkpt_blood_destroy(void);
uint32_t vkpt_blood_prim_count(int num_spheres, int num_films);
// map load: the shadow copy at its worst-case size when blood is on; the capacity
// in primitives for the backend's staging, 0 = blood off
uint32_t vkpt_blood_preallocate(void);
void vkpt_blood_reserve_vk_staging(uint32_t capacity);
// The CPU half both backends share: the frame's droplets written into blood.c's
// shadow copy of the section, and what has to reach the device.
typedef struct {
	uint32_t src;       // first primitive in the shadow copy
	uint32_t dst;       // ... in the instanced buffers
	uint32_t count;     // primitives
} vkpt_blood_region_t;
typedef struct {
	uint32_t prim_offset, prim_count;   // the section; prim_count 0 = no blood this frame
	bool zero_section;                  // the device's copy is void: zero the section, then copy
	uint32_t num_regions;
	const vkpt_blood_region_t* regions; // ascending and disjoint
	const VboPrimitive* prims;          // the shadow copy, `capacity` primitives
	const prim_positions_t* positions;
	uint32_t capacity;
} vkpt_blood_frame_t;
void vkpt_blood_prepare(
	const blood_sphere_t* spheres,
	int num_spheres,
	const blood_film_t* films,      // cl_blood_film tiles; may be NULL / 0
	int num_films,
	bsp_t* bsp,
	const vec3_t cam_pos,
	int model_instance_index,
	uint32_t prim_offset,
	vkpt_blood_frame_t* frame);
void vkpt_blood_print_stats(uint64_t copy_usec);
// a backend dropped a prepared frame's upload: the next frame is an epoch
void vkpt_blood_invalidate(void);
// Vulkan's: vkpt_blood_prepare, then the fill, the copies and the barrier
void vkpt_blood_update(
	VkCommandBuffer cmd_buf,
	const blood_sphere_t* spheres,
	int num_spheres,
	const blood_film_t* films,
	int num_films,
	bsp_t* bsp,
	const vec3_t cam_pos,
	int model_instance_index,
	uint32_t prim_offset,
	uint32_t* prim_count_out);
bool vkpt_blood_blas_needs_build(int frame_idx, uint32_t prim_offset, uint32_t prim_count, bool fast_build);
void vkpt_blood_blas_built(int frame_idx, uint32_t prim_offset, uint32_t prim_count, bool fast_build);
void vkpt_build_beam_lights(light_poly_t* light_list, int* num_lights, int max_lights, bsp_t* bsp, entity_t* entities, int num_entites, float adapted_luminance, int* light_entity_ids, int* num_light_entities);
bool vkpt_build_cylinder_light(light_poly_t* light_list, int* num_lights, int max_lights, bsp_t* bsp, vec3_t begin, vec3_t end, vec3_t color, float radius, entity_hash_t hash, int* light_entity_ids);
bool get_triangle_off_center(const float* positions, float* center, float* anti_center, float offset);

// True while photo mode bypasses DLSS, and with it frame generation (main.c).
bool vkpt_fg_bypassed(void);
// The D3D12 renderer's drs_process (main.c): DRS's step, or photo mode's pin; the
// render scale in use
int vkpt_drs_update(void);
// The largest render extent DLSS evaluates without remaking its feature (main.c;
// DLSSFeatureResolution)
void vkpt_dlss_input_capacity(int *width, int *height);

// steamaudio.c's reflections and ways round corners traced on the GPU - see sound_trace.c
VkResult vkpt_sound_initialize(void);
VkResult vkpt_sound_destroy(void);
VkResult vkpt_sound_create_pipelines(void);
VkResult vkpt_sound_destroy_pipelines(void);
void vkpt_sound_register(void);
void vkpt_sound_lock(void);
void vkpt_sound_unlock(void);

// NVIDIA Flow fire and smoke for explosions - see flow.c
VkResult vkpt_flow_initialize(void);
VkResult vkpt_flow_destroy(void);
void vkpt_flow_register(void);
void vkpt_flow_reset(void);
void vkpt_flow_update(const refdef_t *fd, bool render_world);
// pt_flow_stats: the primary march's counters from this frame's readback.
void vkpt_flow_stats(const ReadbackBuffer *readback);
VkResult vkpt_flow_create_pipelines(void);
VkResult vkpt_flow_destroy_pipelines(void);
// the world's opaque triangles, which hold the gas in (pt_flow_collision)
void vkpt_flow_set_world(const struct bsp_mesh_s *wm);
// the lights the fire and plasma cast (pt_flow_lighting); how many
int vkpt_flow_lights(dlight_t *lights, int max_lights);
// the world's light on the smoke (pt_flow_env): this frame's BSP and which of
// the light buffer's lights are Flow's own, before vkpt_flow_update; then the
// probes, recorded right after the TLAS is built
void vkpt_flow_set_env(const bsp_t *bsp, int first_flow_light, int num_flow_lights);
void vkpt_flow_ambient(VkCommandBuffer cmd_buf);
// whether the profiler has rows to show for Flow, and for its probes
bool vkpt_flow_profiled(void);
bool vkpt_flow_env_profiled(void);
// this frame's march has a grid to read (set by vkpt_flow_update)
bool vkpt_flow_marching(void);

/* NVIDIA Flow on another renderer (vid_rtx 3): its device and queue for Flow's
   own D3D12 backend, and its halves of what flow.c does with Vulkan - the
   buffers and images the shaders read, the textures set's Flow bindings, and
   the passes, which it records from vkpt_flow_dispatches. Set before
   vkpt_flow_initialize, cleared after vkpt_flow_destroy; NULL = Vulkan. */
typedef struct {
	void    *d3d12_device;      // ID3D12Device*
	void    *d3d12_queue;       // ID3D12CommandQueue*: the frame's, which Flow executes on
	// for both sets (frame parity), each this far apart: the FlowParams (mapped, the
	// shaders' uniform buffer), the light sums as the host reads them (mapped) and
	// the occupancy; false = no Flow
	bool    (*create)(size_t params_stride, size_t light_sums_stride, size_t occupancy_stride,
	                  byte **params_mapped, byte **light_sums_mapped);
	void    (*destroy)(void);
	// the probe grid (pt_flow_env); false = none, the smoke keeps its flat colour
	bool    (*create_env)(void);
	void    (*destroy_env)(void);
	// set's Flow bindings: the density texture and the block table (native
	// resources, as Flow's backend hands them over), or the dummies for NULL
	void    (*bind)(int set, void *density, uint32_t density_format, void *table);
	// a timestamp for the profiler's Flow row (PROFILER_FLOW_SIMULATION; action:
	// VKPTProfilerAction) on the queue Flow executes on, before and after its
	// work; NULL = no row
	void    (*profile)(int action);
} vkpt_flow_backend_t;

extern const vkpt_flow_backend_t *vkpt_flow_backend;

// what vkpt_flow_update left for the backend to record this frame, in flow.c's order
typedef struct {
	int         set;                // the frame's parity: the textures set and the buffers' slices
	uint32_t    occupancy_blocks;   // flow_occupancy.comp's groups (the group bitset cleared first), 0 = none
	uint32_t    light_blocks;       // flow_lights.comp's, then the sums copied for the host, 0 = none
	uint32_t    env_groups[3];      // flow_ambient.comp's, after the TLAS, 0 = none
} vkpt_flow_dispatch_t;

void vkpt_flow_dispatches(vkpt_flow_dispatch_t *out);

VkResult vkpt_initialize_god_rays(void);
VkResult vkpt_destroy_god_rays(void);
// the cvars the frame's CPU half reads (vkpt_init_frontend, and the init above)
void vkpt_god_rays_init_cvars(void);
// the D3D12 renderer's start and end: no froxel volumes, no sky-exposure bake
void vkpt_god_rays_cpu_reset(void);
bool vkpt_froxel_restir_enabled(void);
VkResult vkpt_god_rays_create_pipelines(void);
VkResult vkpt_god_rays_destroy_pipelines(void);
VkResult vkpt_god_rays_update_images(void);
VkResult vkpt_god_rays_noop(void);
bool vkpt_god_rays_enabled(const sun_light_t* sun_light);
void vkpt_god_rays_update_tlas_descriptor(void);
void vkpt_record_god_rays_trace_command_buffer(VkCommandBuffer command_buffer, int pass, bool full_res);
void vkpt_record_god_rays_filter_command_buffer(VkCommandBuffer command_buffer);
// [froxel grid] the two map-fog volume passes, and whether they replace the
// per-pixel march this frame. See god_rays.c.
void vkpt_record_froxel_command_buffer(VkCommandBuffer command_buffer);
// The same passes split in two, for pt_async_compute: light selection (ReSTIR
// reservoir + spatial) needs no ray-traced input from this frame, shading
// (scatter + integrate) needs the TLAS, the shadow map and PT_PRIMARY_DIST.
// vkpt_record_froxel_command_buffer is exactly the two back to back.
void vkpt_record_froxel_light_selection(VkCommandBuffer command_buffer);
void vkpt_record_froxel_shading(VkCommandBuffer command_buffer);
bool vkpt_froxel_enabled(void);
// allocates the froxel volumes on the first frame they are wanted, so the
// grid costs no VRAM at all while pt_fog_froxel is off (which is the default)
void vkpt_froxel_ensure(void);
// pt_fog_sky_exposure: set up the new map's sky-exposure grid (bounds, voxel
// size, the solid mask from a BSP point test) and restart its bake. Called from
// R_BeginRegistration_RTX after the device has been idled.
void vkpt_sky_exposure_begin_map(bsp_t* bsp, const aabb_t* world_aabb);
// ...its CPU half, the mask into `bits` (SKY_EXPOSURE_MASK_WORDS words); false =
// no grid for this map. The D3D12 renderer uploads `bits` to its own buffer.
bool vkpt_sky_exposure_fit_map(bsp_t* bsp, const aabb_t* world_aabb, uint32_t* bits);
// The bake's next batch of voxels this frame, or false; taking one counts it as
// recorded. The restart is for a frame that took one and was not drawn.
bool vkpt_sky_exposure_next_batch(uint32_t* first, uint32_t* count);
void vkpt_sky_exposure_restart(void);
void vkpt_god_rays_prepare_ubo(
	QVKUniformBuffer_t * ubo, 
	const aabb_t* world_aabb,
	const float* proj, 
	const float* view, 
	const float* shadowmap_viewproj, 
	float shadowmap_depth_scale);

void vkpt_freecam_reset(void);
void vkpt_freecam_update(float frame_time);
void vkpt_reset_accumulation(void);
void vkpt_dlss_request_history_reset(void);
uint32_t vkpt_pt_field_width(void);

typedef struct maliasframe_s {
    vec3_t  scale;
    vec3_t  translate;
    vec3_t  bounds[2];
    vec_t   radius;
} maliasframe_t;

typedef struct maliasmesh_s {
    int             numverts;
    int             numtris;
    int             numindices;
    int             tri_offset; /* offset in vertex buffer on device */
    int             *indices;
    vec3_t          *positions;
    vec3_t          *normals;
    vec2_t          *tex_coords;
	vec3_t          *tangents;
	uint32_t        *blend_indices; // iqm only
	uint32_t        *blend_weights; // iqm only
	struct pbr_material_s *materials[MAX_ALIAS_SKINS];
    int             numskins;
	bool            handedness;
} maliasmesh_t;

// needed for model.c
#define TESS_MAX_VERTICES   (2 * 16384)
#define TESS_MAX_INDICES    (3 * TESS_MAX_VERTICES)

#define QGL_INDEX_TYPE  GLuint

typedef struct {
    color_t     colors[2]; // 0 - actual color, 1 - transparency (for text drawing)
    float scale;
	float alpha_scale;
} drawStatic_t;

static inline void begin_perf_marker(VkCommandBuffer command_buffer, int index, const char* name)
{
	_VK(vkpt_profiler_query(command_buffer, index, PROFILER_START));

	const VkDebugUtilsLabelEXT label = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
		.pLabelName = name
	};

	if (qvkCmdBeginDebugUtilsLabelEXT != NULL)
		qvkCmdBeginDebugUtilsLabelEXT(command_buffer, &label);

	/* `name` is the #name of the BEGIN_PERF_MARKER macro, a string literal with
	   static storage. The driver hands this very pointer back after the device
	   is lost, so it has to outlive the command buffer - a literal does. */
	if (qvkCmdSetCheckpointNV != NULL)
		qvkCmdSetCheckpointNV(command_buffer, name);
}

static inline void end_perf_marker(VkCommandBuffer command_buffer, int index)
{
	if (qvkCmdEndDebugUtilsLabelEXT != NULL)
		qvkCmdEndDebugUtilsLabelEXT(command_buffer);

	_VK(vkpt_profiler_query(command_buffer, index, PROFILER_STOP));
}

#define BEGIN_PERF_MARKER(command_buffer, name)  begin_perf_marker(command_buffer, name, #name)
#define END_PERF_MARKER(command_buffer, name)    end_perf_marker(command_buffer, name)

void R_SetClipRect_RTX(const clipRect_t *clip);
void R_ClearColor_RTX(void);
void R_SetAlpha_RTX(float alpha);
void R_SetAlphaScale_RTX(float alpha);
void R_SetColor_RTX(uint32_t color);
void R_LightPoint_RTX(const vec3_t origin, vec3_t light);
void R_SetScale_RTX(float scale);
void R_DrawStretchPic_RTX(int x, int y, int w, int h, qhandle_t pic);
void R_DrawRotatedPic_RTX(float cx, float cy, float w, float h, float angle, qhandle_t pic);
void R_DrawPic_RTX(int x, int y, qhandle_t pic);
void R_TileClear_RTX(int x, int y, int w, int h, qhandle_t pic);
void R_DrawFill8_RTX(int x, int y, int w, int h, int c);
void R_DrawFill32_RTX(int x, int y, int w, int h, uint32_t color);
void R_DrawChar_RTX(int x, int y, int flags, int c, qhandle_t font);
int R_DrawString_RTX(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font);
bool R_InterceptKey_RTX(unsigned key, bool down);
// Saves the 2D state (scale, colour, alpha, clipping), resets it with the given
// scale, and puts it back - for drawing from inside R_RenderFrame_RTX.
void vkpt_draw_push_state(float scale);
void vkpt_draw_pop_state(void);

// cpu_trace.c - `cpu_bench`, the frame path traced on the CPU
void vkpt_cpu_trace_init(void);
void vkpt_cpu_trace_shutdown(void);
void vkpt_cpu_trace_stop(void);
// true while the bench owns the screen: R_RenderFrame_RTX draws it instead
bool vkpt_cpu_trace_active(void);
// true when `cpu_bench` is waiting for a world frame to take the scene from
bool vkpt_cpu_trace_capture_pending(void);
void vkpt_cpu_trace_capture(const refdef_t *fd, bsp_t *bsp, const sun_light_t *sun, const vec3_t sky_matrix[3],
	float env_scale, float adapted_luminance, float sun_luminance, float sky_luminance, const QVKUniformBuffer_t *ubo);
void vkpt_cpu_trace_draw(void);
void vkpt_cpu_trace_set_envmap(int width, int height, const byte *data);
// the material id an alias-model mesh is drawn with this frame, 0 = not drawn
uint32_t vkpt_mesh_material_id(const entity_t *entity, const model_t *model, const maliasmesh_t *mesh, bool is_viewer_weapon);
// how much a light scatters into the fog, as copy_light() uploads it
float vkpt_light_volumetric_scale(const light_poly_t *light);

void IMG_Load_RTX(image_t *image, byte *pic);
void IMG_Unload_RTX(image_t *image);
bool IMG_ReloadFrom(image_t *image, const char *filepath);
bool IMG_ReloadIfModified(image_t *image);
byte *IMG_ReadPixels_RTX(int *width, int *height, int *rowbytes);
float *IMG_ReadPixelsHDR_RTX(int *width, int *height);

int MOD_LoadMD2_RTX(model_t *model, const void *rawdata, size_t length, const char* mod_name);
int MOD_LoadMD3_RTX(model_t* model, const void* rawdata, size_t length, const char* mod_name);
int MOD_LoadIQM_RTX(model_t *model, const void *rawdata, size_t length, const char* mod_name);
int MOD_LoadMD5_RTX(model_t *model, const void *rawdata, size_t length, const char* mod_name);
void MOD_Reference_RTX(model_t *model);
// Ray against an alias model's real triangles - see inc/refresh/models.h.
bool MOD_TraceMesh_RTX(const model_t *model, const mod_pose_t *pose,
                       const vec3_t start, const vec3_t end,
                       float *out_frac, vec3_t out_normal);
void MOD_TraceMeshShutdown_RTX(void);

#endif  /*__VKPT_H__*/

