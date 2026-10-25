/*
Copyright (C) 2026 Q2RTX contributors

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

/*
==============================================================================

NVIDIA FLOW

Fire and smoke for explosions, simulated by NVIDIA Flow 2 - the sparse-grid
gas solver in the flow/ directory of the PhysX 5 SDK - and drawn by the path
tracer. The client hands effects here through R_FlowEmit (cl_flow_explosions):
explosions from client/tent.c, the BFG's ball and lasers from there and from
client/entities.c. shader/flow.glsl marches the result along the primary and
reflection rays and lays it over the effects overlay.

nvflow.dll and nvflowext.dll are loaded at run time and come from
setup/build_flow.py, which builds them from a pinned PhysX commit with one
addition, NvFlowVulkanExternal.h: Flow's Vulkan backend running on OUR
VkDevice instead of a private one of its own. That is the whole design.

  - Flow records its compute passes into its own command buffers and submits
    them to qvk.queue_graphics from vkpt_flow_update, before anything of this
    frame is submitted. Every Flow frame starts its transitions from all
    stages and ends with an all-stages barrier, so submission order on the
    one queue is all the synchronisation there is, in both directions.
  - The result - the density atlas (RGBA16F: temperature, fuel, burn or
    shadow, smoke) and the sparse block table that says where each block of
    cells sits in it - is ACQUIRED from Flow every frame: a reference that
    keeps Flow's pool from handing it out again. Its VkImageView and VkBuffer
    go straight into the global texture set at BINDING_OFFSET_FLOW_*, with no
    copy.
  - That set alternates even/odd per frame, and a set is only rewritten once
    the frame that last used it has passed its fence. So each parity keeps
    its acquisitions until its set is rewritten, and every descriptor always
    names something alive. With Flow idle, missing or switched off, the sets
    name small dummies and FlowParams says there is nothing to draw.

The FG present thread submits to the same queue under FGPresent_SwapchainLock,
so Flow is given a vkGetInstanceProcAddr whose vkQueueSubmit and
vkQueueWaitIdle take that lock.

THE SIMULATION has a Flow layer per kind of effect (r_flow_style_t), all of
pt_flow_cell_size cells:

  - FIRE. An explosion is a sphere emitter that lives pt_flow_burst seconds
    of game time: it drives the grid inside it to hot, fuel-laden and a
    little smoky, and pushes outward (the emitter's divergence, which the
    pressure solve turns into an expanding flow round it). Then it goes, and
    Flow's combustion runs on by itself: fuel above the ignition temperature
    burns, burning makes heat and smoke, heat rises and cools, smoke fades.
  - PLASMA, the BFG's: heat and smoke with no fuel, so nothing burns. It
    glows green while it cools and leaves dark green smoke behind. The ball
    in flight is a TRAIL - a sphere that follows it, drawing a glowing tail
    of cooling plasma - its lasers are BEAMS, rows of thin boxes, and its
    BLAST is some ninety small bursts in a set order (flow_emit_blast).
    What it does to each target it zaps is a small burst of its own, about
    the size of one of the blast's lumps.
  - BUBBLES, an explosion under water: no heat, only "smoke" that floats
    (Flow's buoyancy per smoke) and is drawn as pale, scattering foam, up to
    the water's surface and no further.

Flow allocates blocks where there is smoke and frees them once it has faded,
so when nothing is going on the grid is empty and this module skips Flow
entirely until the next explosion.

WHAT IT COSTS is Flow's steps and flow.glsl's march, per pixel, along every
primary ray and reflection that crosses the box round the blocks. So:

  - Flow takes at most pt_flow_rate steps a second. Below that frame rate
    each frame steps by its own time; above it Flow's own stepper takes a
    step on only some frames, and the frames between draw the last one.
  - flow_occupancy.comp marks, after every step, the 4x4x4-cell bricks of
    each block that can show anything, and the march only samples those -
    measured headless, 12-21% of the steps it would take through allocated
    blocks. Reflections march pt_flow_reflect_step times coarser.
  - Fire's leftover fuel fades (pt_flow_fuel_fade), or it smoulders on just
    above Flow's ignition temperature, making smoke too thin to see that
    keeps its blocks for seconds.

Two things in the menu go further (pt_flow_lighting, pt_flow_collision):

  - LIGHT. Each fire or plasma burst takes a light slot. flow_lights.comp adds
    up the glow of every block round the slot's seed; two frames later, past
    that frame's fence, flow_lights_collect reads the sums back and puts a
    sphere light where the glow is, as wide as it is spread and as bright as
    all of it (FLOW_LIGHT_UNIT), and moves the seed there to follow it as it
    rises. The light goes into the scene like any other dlight, so it casts
    shadows and lights the walls; and the smoke scatters it (flow_lit in
    flow.glsl), shadowed by Flow's own shadow pass toward the brightest.
  - COLLISION. The world's opaque triangles near the gas go to Flow as a
    collision mesh (flow_collision_update): Flow stops the gas from moving in
    a band from pt_flow_collision_depth units behind each face to
    pt_flow_collision_margin in front, which holds smoke in a room.

==============================================================================
*/

#include "shared/shared.h"
#include "common/common.h"
#include "system/system.h"
#include "vkpt.h"
#include "vk_util.h"
#include "fg_present.h"
#include "shader/flow.h"

#include <float.h>

#if USE_FLOW
#define NV_FLOW_CPU 1
#include "NvFlowExt.h"
#include "NvFlowVulkanExternal.h"
#endif

#ifdef _WIN32
#define FLOW_LIBNAME        "nvflow.dll"
#define FLOW_EXT_LIBNAME    "nvflowext.dll"
#else
#define FLOW_LIBNAME        "libnvflow.so"
#define FLOW_EXT_LIBNAME    "libnvflowext.so"
#endif

// emitters of every kind at once; the oldest goes when another comes. A BFG
// blast is about 90 by itself.
#define FLOW_MAX_EMITTERS   256

// the even and odd global texture sets, see qvk_get_current_desc_set_textures
#define FLOW_SET_COUNT      2

// A beam is cut into boxes no longer than this. Flow allocates blocks over a
// box's axis-aligned bounds, so one box along a long diagonal would take
// every block of the box round it.
#define FLOW_BEAM_SEGMENT   48.f

// Light power - radiance x radius^2, the way a sphere light's brightness
// goes - per unit of glow, colour map x cubic units. Measured headless with
// the defaults: a rocket's fireball holds about 25 million of glow for most
// of a second, which comes out at 5000, three times the classic explosion
// light's 1600 (ex_poly_light in client/tent.c at its widest).
#define FLOW_LIGHT_UNIT     2e-4f

// The most collision triangles handed to Flow at once.
#define FLOW_MAX_MESH_TRIS  65536

// The collision mesh is cut for a box round the gas in steps of this, so it
// is cut again only when the gas has moved on by about as much.
#define FLOW_MESH_STEP      128.f

typedef struct {
	r_flow_kind_t   kind;
	int             layer;      // an r_flow_style_t
	vec3_t          origin;
	vec3_t          end;        // a beam segment's other end
	float           scale;
	float           age;        // seconds of game time it has been emitting; below 0, yet to start
	uint64_t        luid;       // Flow's identity for the emitter
	int             key;        // a trail's source
	bool            seen;       // a trail: given again since the last frame
	// A piece of a BFG blast: its own radius in units, push, emitting time and
	// outward velocity instead of the cvars'. 0 radius for any other emitter.
	float           radius;
	float           expansion;
	float           life;
	vec3_t          velocity;
} flow_emitter_t;

// A light that a fire or plasma burst casts, see flow_lights_collect.
typedef struct {
	int             layer;      // an r_flow_style_t; -1 for a free slot
	uint32_t        generation; // bumped each time the slot is taken
	vec3_t          seed;       // where flow_lights.comp gathers its glow
	float           gather;     // how far from the seed a block may be
	float           age;        // seconds of game time since it was taken
	float           strength;   // pt_flow_light or pt_flow_bfg_light
	bool            lit;        // the glow has come back: the light below is real
	vec3_t          origin;
	vec3_t          radiance;   // absolute, like any light's
	float           radius;
} flow_light_t;

static cvar_t *cvar_flow_available;
static cvar_t *cvar_pt_flow;
static cvar_t *cvar_pt_flow_lighting;
static cvar_t *cvar_pt_flow_collision;
static cvar_t *cvar_pt_flow_cell_size;
static cvar_t *cvar_pt_flow_max_blocks;
static cvar_t *cvar_pt_flow_substeps;
static cvar_t *cvar_pt_flow_radius;
static cvar_t *cvar_pt_flow_burst;
static cvar_t *cvar_pt_flow_expansion;
static cvar_t *cvar_pt_flow_temperature;
static cvar_t *cvar_pt_flow_fuel;
static cvar_t *cvar_pt_flow_smoke;
static cvar_t *cvar_pt_flow_couple_rate;
static cvar_t *cvar_pt_flow_buoyancy;
static cvar_t *cvar_pt_flow_cooling;
static cvar_t *cvar_pt_flow_smoke_fade;
static cvar_t *cvar_pt_flow_smoke_damping;
static cvar_t *cvar_pt_flow_smoke_per_burn;
static cvar_t *cvar_pt_flow_vorticity;
static cvar_t *cvar_pt_flow_fire_brightness;
static cvar_t *cvar_pt_flow_fire_orange;
static cvar_t *cvar_pt_flow_smoke_brightness;
static cvar_t *cvar_pt_flow_smoke_albedo;
static cvar_t *cvar_pt_flow_smoke_scatter;
static cvar_t *cvar_pt_flow_density;
static cvar_t *cvar_pt_flow_step;
static cvar_t *cvar_pt_flow_max_steps;
static cvar_t *cvar_pt_flow_rate;
static cvar_t *cvar_pt_flow_reflect_step;
static cvar_t *cvar_pt_flow_fuel_fade;
static cvar_t *cvar_pt_flow_bfg_radius;
static cvar_t *cvar_pt_flow_bfg_expansion;
static cvar_t *cvar_pt_flow_bfg_burst;
static cvar_t *cvar_pt_flow_bfg_cooling;
static cvar_t *cvar_pt_flow_bfg_buoyancy;
static cvar_t *cvar_pt_flow_bfg_brightness;
static cvar_t *cvar_pt_flow_bfg_smoke;
static cvar_t *cvar_pt_flow_bfg_smoke_fade;
static cvar_t *cvar_pt_flow_bfg_smoke_damping;
static cvar_t *cvar_pt_flow_bfg_smoke_brightness;
static cvar_t *cvar_pt_flow_bfg_smoke_density;
static cvar_t *cvar_pt_flow_bfg_puffs;
static cvar_t *cvar_pt_flow_bfg_lumps;
static cvar_t *cvar_pt_flow_bfg_jets;
static cvar_t *cvar_pt_flow_bfg_jet_length;
static cvar_t *cvar_pt_flow_bfg_blast_size;
static cvar_t *cvar_pt_flow_bfg_blast_expansion;
static cvar_t *cvar_pt_flow_bfg_blast_light;
static cvar_t *cvar_pt_flow_bfg_vorticity;
static cvar_t *cvar_pt_flow_trail_radius;
static cvar_t *cvar_pt_flow_trail_expansion;
static cvar_t *cvar_pt_flow_trail_smoke;
static cvar_t *cvar_pt_flow_beam_radius;
static cvar_t *cvar_pt_flow_beam_expansion;
static cvar_t *cvar_pt_flow_beam_burst;
static cvar_t *cvar_pt_flow_bubble_radius;
static cvar_t *cvar_pt_flow_bubble_expansion;
static cvar_t *cvar_pt_flow_bubble_amount;
static cvar_t *cvar_pt_flow_bubble_rise;
static cvar_t *cvar_pt_flow_bubble_fade;
static cvar_t *cvar_pt_flow_bubble_brightness;
static cvar_t *cvar_pt_flow_bubble_density;
static cvar_t *cvar_pt_flow_light;
static cvar_t *cvar_pt_flow_bfg_light;
static cvar_t *cvar_pt_flow_light_size;
static cvar_t *cvar_pt_flow_light_radius;
static cvar_t *cvar_pt_flow_shadow;
static cvar_t *cvar_pt_flow_shadow_min;
static cvar_t *cvar_pt_flow_collision_depth;
static cvar_t *cvar_pt_flow_collision_margin;

static struct {
	// Always there, so the descriptors always name something.
	VkImage             dummy_image;
	VkDeviceMemory      dummy_memory;
	VkImageView         dummy_view;
	BufferResource_t    dummy_table;
	BufferResource_t    params;         // a FlowParams per set, params_stride apart
	byte                *params_mapped;
	size_t              params_stride;

	// What flow_lights.comp writes: FLOW_LIGHT_SUM_VEC4S vec4s a block, a
	// set's worth light_sums_stride apart. Read on the host once the set's
	// frame has passed its fence.
	BufferResource_t    light_sums;
	byte                *light_sums_mapped;
	size_t              light_sums_stride;
	VkPipelineLayout    lights_pipeline_layout;
	VkPipeline          lights_pipeline;

	// What flow_occupancy.comp writes for the march: a uvec4 a block, a set's
	// worth occupancy_stride apart.
	BufferResource_t    occupancy;
	size_t              occupancy_stride;
	VkPipeline          occupancy_pipeline;

	flow_emitter_t      emitters[FLOW_MAX_EMITTERS];
	int                 num_emitters;
	uint64_t            next_luid;

	flow_light_t        lights[FLOW_MAX_LIGHTS];

	// qvk.frame_counter + 1 of the frame vkpt_flow_update last ran in, 0 for none
	uint64_t            updated_frame;

#if USE_FLOW
	bool                libraries_loaded;
	NvFlowOpList        *op_list;
	NvFlowExtOpList     *ext_op_list;
	NvFlowGridInterface         *grid_iface;
	NvFlowGridParamsInterface   *params_iface;
	NvFlowDeviceInterface       *device_iface;
	PFN_NvFlowVulkanExternalCreateDeviceManager create_device_manager;
	PFN_NvFlowVulkanExternalCreateDevice        create_device;
	PFN_NvFlowVulkanExternalGetTexture          get_texture;
	PFN_NvFlowVulkanExternalGetBuffer           get_buffer;

	NvFlowDeviceManager     *device_manager;
	NvFlowDevice            *device;
	NvFlowDeviceQueue       *queue;
	NvFlowContextInterface  *context_iface;
	NvFlowContext           *context;
	NvFlowGrid              *grid;
	NvFlowGridParamsNamed   *params_named;
	NvFlowGridParams        *grid_params;

	// What each set's descriptors name, held until that set is rewritten.
	NvFlowTexture           *set_density[FLOW_SET_COUNT];
	NvFlowBuffer            *set_table[FLOW_SET_COUNT];

	// This frame's parameters. Flow keeps the pointers, not copies, until
	// the simulation that reads them is done, so they live here.
	NvFlowGridSimulateLayerParams   simulate[FLOW_LAYERS];
	NvFlowGridSimulateLayerParams   *simulate_ptrs[FLOW_LAYERS];
	NvFlowGridEmitterSphereParams   spheres[FLOW_MAX_EMITTERS];
	NvFlowGridEmitterSphereParams   *sphere_ptrs[FLOW_MAX_EMITTERS];
	NvFlowGridEmitterBoxParams      boxes[FLOW_MAX_EMITTERS];
	NvFlowGridEmitterBoxParams      *box_ptrs[FLOW_MAX_EMITTERS];
	NvFlowGridEmitterMeshParams     mesh;
	NvFlowGridEmitterMeshParams     *mesh_ptr;
	NvFlowGridOffscreenLayerParams  offscreen[FLOW_LAYERS];
	NvFlowGridOffscreenLayerParams  *offscreen_ptrs[FLOW_LAYERS];
	NvFlowDatabaseTypeSnapshot      type_snapshots[5];
	NvFlowUint64            version;

	double                  sim_time;   // seconds simulated since the grid was made
	float                   last_time;  // refdef time of the last world frame
	bool                    have_last_time;
	float                   frame_dt;   // game time a frame, smoothed, for pt_flow_rate
	bool                    idle;       // nothing allocated and nothing emitting: skip Flow
	bool                    force_clear;
	bool                    shadow_pass;    // this frame's simulation is followed by Flow's shadow pass

	// Where the bubbles stop: the highest water surface over any underwater
	// explosion since the bubble layer was last empty.
	float                   bubble_surface;
	bool                    have_bubble_surface;

	// The corners of every block Flow had last frame, for the collision mesh.
	vec3_t                  blocks_min, blocks_max;
	bool                    have_blocks;

	// The world's opaque triangles, 9 floats each (vkpt_flow_set_world), and
	// the ones near the gas, as Flow's mesh emitter wants them.
	float                   *world_tris;
	int                     num_world_tris;
	NvFlowFloat3            *mesh_positions;
	int                     *mesh_indices;
	int                     *mesh_counts;
	int                     mesh_capacity;      // triangles
	int                     num_mesh_tris;
	NvFlowUint64            mesh_version;
	int                     mesh_region[6];     // what the mesh was cut for, in FLOW_MESH_STEPs
	bool                    mesh_region_valid;

	// Light slots as they went to the GPU for each set: which generation of
	// the slot, and its seed, which the sums are relative to; and how many
	// blocks were added up. 0 blocks is no sums to read.
	uint32_t                light_sum_blocks[FLOW_SET_COUNT];
	uint32_t                light_sum_generation[FLOW_SET_COUNT][FLOW_MAX_LIGHTS];
	vec3_t                  light_sum_seed[FLOW_SET_COUNT][FLOW_MAX_LIGHTS];
	uint64_t                lights_collected_frame;    // qvk.frame_counter + 1

	// The slots with a light, brightest first, as the scene and flow.glsl
	// get them; and per layer, the one whose shadow Flow works out, -1 none.
	dlight_t                dlights[FLOW_MAX_LIGHTS];
	int                     light_slot[FLOW_MAX_LIGHTS];
	int                     num_dlights;
	int                     layer_shadow[FLOW_LAYERS];
#endif
} flow;

#if USE_FLOW

/*
==============================================================================

THE QUEUE LOCK

==============================================================================
*/

static PFN_vkGetDeviceProcAddr  flow_real_get_device_proc_addr;
static PFN_vkQueueSubmit        flow_real_queue_submit;
static PFN_vkQueueWaitIdle      flow_real_queue_wait_idle;

static VKAPI_ATTR VkResult VKAPI_CALL
flow_queue_submit(VkQueue queue, uint32_t submit_count, const VkSubmitInfo *submits, VkFence fence)
{
	FGPresent_SwapchainLock();
	VkResult result = flow_real_queue_submit(queue, submit_count, submits, fence);
	FGPresent_SwapchainUnlock();
	return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL
flow_queue_wait_idle(VkQueue queue)
{
	FGPresent_SwapchainLock();
	VkResult result = flow_real_queue_wait_idle(queue);
	FGPresent_SwapchainUnlock();
	return result;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
flow_get_device_proc_addr(VkDevice device, const char *name)
{
	if (!strcmp(name, "vkQueueSubmit"))
		return (PFN_vkVoidFunction)flow_queue_submit;
	if (!strcmp(name, "vkQueueWaitIdle"))
		return (PFN_vkVoidFunction)flow_queue_wait_idle;
	return flow_real_get_device_proc_addr(device, name);
}

// Flow loads every device function through the vkGetDeviceProcAddr it gets
// from here.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
flow_get_instance_proc_addr(VkInstance instance, const char *name)
{
	if (!strcmp(name, "vkGetDeviceProcAddr"))
		return (PFN_vkVoidFunction)flow_get_device_proc_addr;
	if (!strcmp(name, "vkQueueSubmit"))
		return (PFN_vkVoidFunction)flow_queue_submit;
	if (!strcmp(name, "vkQueueWaitIdle"))
		return (PFN_vkVoidFunction)flow_queue_wait_idle;
	return vkGetInstanceProcAddr(instance, name);
}

static void NV_FLOW_ABI flow_log(NvFlowLogLevel level, const char *format, ...)
{
	char        buffer[MAXPRINTMSG];
	va_list     args;

	va_start(args, format);
	Q_vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	if (level == eNvFlowLogLevel_error)
		Com_EPrintf("NVIDIA Flow: %s\n", buffer);
	else if (level == eNvFlowLogLevel_warning)
		Com_WPrintf("NVIDIA Flow: %s\n", buffer);
	else
		Com_DPrintf("NVIDIA Flow: %s\n", buffer);
}

/*
==============================================================================

SETUP

==============================================================================
*/

static void *flow_load_library(const char *name)
{
	char    path[MAX_OSPATH];
	void    *handle;

	Q_concat(path, sizeof(path), sys_libdir->string, PATH_SEP_STRING, name);
	Sys_LoadLibrary(path, NULL, &handle);
	if (!handle)
		Sys_LoadLibrary(name, NULL, &handle);
	return handle;
}

// Loaded once and never unloaded: Flow's worker threads and statics outlive
// any one renderer, and a vid_restart comes back to the same libraries.
static bool flow_load_libraries(void)
{
	static bool tried;
	void        *lib, *ext_lib;

	if (flow.libraries_loaded || tried)
		return flow.libraries_loaded;
	tried = true;

	lib = flow_load_library(FLOW_LIBNAME);
	ext_lib = lib ? flow_load_library(FLOW_EXT_LIBNAME) : NULL;
	if (!lib || !ext_lib) {
		Com_Printf("NVIDIA Flow: %s not found, explosions stay classic (setup/build_flow.py builds it).\n",
			lib ? FLOW_EXT_LIBNAME : FLOW_LIBNAME);
		return false;
	}

	PFN_NvFlowGetOpList get_op_list = (PFN_NvFlowGetOpList)Sys_GetProcAddress(lib, "NvFlowGetOpList");
	PFN_NvFlowGetExtOpList get_ext_op_list = (PFN_NvFlowGetExtOpList)Sys_GetProcAddress(ext_lib, "NvFlowGetExtOpList");
	PFN_NvFlowGetGridInterface get_grid = (PFN_NvFlowGetGridInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetGridInterface");
	PFN_NvFlowGetGridParamsInterface get_grid_params = (PFN_NvFlowGetGridParamsInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetGridParamsInterface");
	PFN_NvFlowGetDeviceInterface get_device = (PFN_NvFlowGetDeviceInterface)Sys_GetProcAddress(ext_lib, "NvFlowGetDeviceInterface");
	flow.create_device_manager = (PFN_NvFlowVulkanExternalCreateDeviceManager)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalCreateDeviceManager");
	flow.create_device = (PFN_NvFlowVulkanExternalCreateDevice)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalCreateDevice");
	flow.get_texture = (PFN_NvFlowVulkanExternalGetTexture)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalGetTexture");
	flow.get_buffer = (PFN_NvFlowVulkanExternalGetBuffer)Sys_GetProcAddress(ext_lib, "NvFlowVulkanExternalGetBuffer");

	if (!get_op_list || !get_ext_op_list || !get_grid || !get_grid_params || !get_device ||
		!flow.create_device_manager || !flow.create_device || !flow.get_texture || !flow.get_buffer) {
		// A stock Flow build has no NvFlowVulkanExternal*: it can only make a
		// device of its own, which the renderer cannot sample.
		Com_Printf("NVIDIA Flow: %s is not the build from setup/build_flow.py.\n", FLOW_EXT_LIBNAME);
		return false;
	}

	flow.op_list = get_op_list();
	flow.ext_op_list = get_ext_op_list();
	flow.grid_iface = get_grid();
	flow.params_iface = get_grid_params();
	flow.device_iface = get_device(eNvFlowContextApi_vulkan);

	if (!flow.op_list || !flow.ext_op_list || !flow.grid_iface || !flow.params_iface || !flow.device_iface)
		return false;

	flow.libraries_loaded = true;
	return true;
}

static void flow_destroy_simulation(void);

static void flow_create_simulation(void)
{
	if (!cvar_pt_flow->integer) {
		Com_Printf("NVIDIA Flow: off (pt_flow 0).\n");
		return;
	}
	if (qvk.device_count > 1) {
		Com_Printf("NVIDIA Flow: not with more than one GPU.\n");
		return;
	}
	if (!qvk.supports_storage_image_without_format || !qvk.supports_fp16) {
		Com_Printf("NVIDIA Flow: this GPU lacks storage images without a format, or FP16.\n");
		return;
	}
	if (!flow_load_libraries())
		return;

	flow_real_get_device_proc_addr = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(qvk.instance, "vkGetDeviceProcAddr");
	flow_real_queue_submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(qvk.device, "vkQueueSubmit");
	flow_real_queue_wait_idle = (PFN_vkQueueWaitIdle)vkGetDeviceProcAddr(qvk.device, "vkQueueWaitIdle");

	NvFlowVulkanExternalDesc desc = {
		.instance = qvk.instance,
		.physicalDevice = qvk.physical_device,
		.device = qvk.device,
		.queue = qvk.queue_graphics,
		.queueFamilyIndex = (NvFlowUint)qvk.queue_idx_graphics,
		.getInstanceProcAddr = (void *)flow_get_instance_proc_addr,
		.shaderStorageImageWriteWithoutFormat = NV_FLOW_TRUE,
		.logPrint = flow_log,
	};

	flow.device_manager = flow.create_device_manager(&desc);
	if (flow.device_manager)
		flow.device = flow.create_device(flow.device_manager, &desc);
	if (!flow.device) {
		Com_EPrintf("NVIDIA Flow: could not start on the renderer's device.\n");
		flow_destroy_simulation();
		return;
	}

	flow.queue = flow.device_iface->getDeviceQueue(flow.device);
	flow.context_iface = flow.device_iface->getContextInterface(flow.queue);
	flow.context = flow.device_iface->getContext(flow.queue);

	// The block pool is allocated whole up front: an atlas of max_blocks
	// blocks of 34x18x18 RGBA16F texels, about 88 KB each, twice over for
	// the density and again for the velocity and the solver's scratch.
	NvFlowGridDesc grid_desc = NvFlowGridDesc_default;
	grid_desc.maxLocations = (NvFlowUint)Cvar_ClampInteger(cvar_pt_flow_max_blocks, 16, 4096);
	grid_desc.maxLocationsIsosurface = 16;  // the isosurface path is never used

	flow.grid = flow.grid_iface->createGrid(flow.context_iface, flow.context, flow.op_list, flow.ext_op_list, &grid_desc);
	flow.params_named = flow.params_iface->createGridParamsNamed("q2rtx");
	flow.grid_params = flow.params_named ? flow.params_iface->mapGridParamsNamed(flow.params_named) : NULL;

	if (!flow.grid || !flow.grid_params) {
		Com_EPrintf("NVIDIA Flow: could not create the grid.\n");
		flow_destroy_simulation();
		return;
	}

	flow.sim_time = 0.0;
	flow.have_last_time = false;
	flow.idle = true;
	flow.force_clear = false;

	Cvar_FullSet("flow_available", "1", CVAR_ROM, FROM_CODE);
	Com_Printf("NVIDIA Flow: ready, up to %u blocks.\n", grid_desc.maxLocations);
}

static void flow_release_set(int set)
{
	if (flow.set_density[set])
		flow.context_iface->destroyTexture(flow.context, flow.set_density[set]);
	if (flow.set_table[set])
		flow.context_iface->destroyBuffer(flow.context, flow.set_table[set]);
	flow.set_density[set] = NULL;
	flow.set_table[set] = NULL;
	flow.light_sum_blocks[set] = 0;
}

static void flow_destroy_simulation(void)
{
	if (flow.device) {
		NvFlowUint64 flushed_frame;

		flow.device_iface->waitIdle(flow.queue);

		for (int set = 0; set < FLOW_SET_COUNT; set++)
			flow_release_set(set);

		if (flow.grid)
			flow.grid_iface->destroyGrid(flow.context, flow.grid);
		if (flow.params_named)
			flow.params_iface->destroyGridParamsNamed(flow.params_named);

		// what destroying the grid recorded
		flow.device_iface->flush(flow.queue, &flushed_frame, NULL, NULL);
		flow.device_iface->waitIdle(flow.queue);

		flow.device_iface->destroyDevice(flow.device_manager, flow.device);
	}
	if (flow.device_manager)
		flow.device_iface->destroyDeviceManager(flow.device_manager);

	flow.device_manager = NULL;
	flow.device = NULL;
	flow.queue = NULL;
	flow.context_iface = NULL;
	flow.context = NULL;
	flow.grid = NULL;
	flow.params_named = NULL;
	flow.grid_params = NULL;

	Cvar_FullSet("flow_available", "0", CVAR_ROM, FROM_CODE);
}

/*
==============================================================================

LIGHT

==============================================================================
*/

static float flow_luminance(const vec3_t c)
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

static void flow_free_lights(void)
{
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		flow.lights[i].layer = -1;
	flow.num_dlights = 0;
	for (int l = 0; l < FLOW_LAYERS; l++)
		flow.layer_shadow[l] = -1;
}

// A light slot for a burst of `layer` at `origin`, or -1 for none. A burst
// close to one only just started shares its light: a rocket's splash and the
// rocket are one fire.
static int flow_take_light(int layer, const vec3_t origin, float scale)
{
	if (!cvar_pt_flow_lighting->integer || (layer != R_FLOW_FIRE && layer != R_FLOW_PLASMA))
		return -1;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer == layer && l->age < 0.5f && Distance(l->seed, origin) < 48.f * scale)
			return i;
	}

	int slot = -1;

	// a free slot, or the oldest
	float oldest = -1.f;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0) {
			slot = i;
			break;
		}
		if (l->age > oldest) {
			oldest = l->age;
			slot = i;
		}
	}

	flow_light_t *l = &flow.lights[slot];
	l->layer = layer;
	l->generation++;
	VectorCopy(origin, l->seed);
	l->gather = 96.f * scale;
	l->age = 0.f;
	l->strength = layer == R_FLOW_PLASMA ? cvar_pt_flow_bfg_light->value : cvar_pt_flow_light->value;
	l->lit = false;
	return slot;
}

/*
The sums flow_lights.comp wrote for `set` two frames ago, read now that the
frame has passed its fence: each slot's light goes to the glow-weighted middle
of its blocks, as wide as the glow is spread and as bright as all of it, and
its seed follows. A slot whose blocks have stopped glowing is given up. Then
the lights are listed for the scene, brightest first. Once a frame, before
the frame's own sums are dispatched into the same set.
*/
static void flow_lights_collect(int set)
{
	if (flow.lights_collected_frame == qvk.frame_counter + 1)
		return;
	flow.lights_collected_frame = qvk.frame_counter + 1;

	if (!cvar_pt_flow_lighting->integer) {
		flow_free_lights();
		flow.light_sum_blocks[set] = 0;
		return;
	}

	// glow rgb, glow-weighted position from the seed and glow, glow-weighted
	// squared distance from the seed
	double sums[FLOW_MAX_LIGHTS][8];
	bool gathered[FLOW_MAX_LIGHTS];
	memset(sums, 0, sizeof(sums));

	const uint32_t blocks = flow.light_sum_blocks[set];
	flow.light_sum_blocks[set] = 0;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		gathered[i] = blocks > 0 && flow.lights[i].layer >= 0 &&
			flow.light_sum_generation[set][i] == flow.lights[i].generation;

	const vec4_t *src = (const vec4_t *)(flow.light_sums_mapped + set * flow.light_sums_stride);
	for (uint32_t b = 0; b < blocks; b++, src += FLOW_LIGHT_SUM_VEC4S) {
		const int slot = (int)src[0][3];
		if (slot < 0 || slot >= FLOW_MAX_LIGHTS || !gathered[slot])
			continue;
		double *s = sums[slot];
		s[0] += src[0][0];
		s[1] += src[0][1];
		s[2] += src[0][2];
		s[3] += src[1][0];
		s[4] += src[1][1];
		s[5] += src[1][2];
		s[6] += src[1][3];
		s[7] += src[2][0];
	}

	const float size = max(cvar_pt_flow_light_size->value, 0.f);
	const float max_radius = max(cvar_pt_flow_light_radius->value, 4.f);

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		flow_light_t *l = &flow.lights[i];
		if (l->layer < 0)
			continue;

		if (l->age > 12.f) {
			l->layer = -1;
			continue;
		}
		if (!gathered[i])
			continue;

		const double *s = sums[i];
		if (s[6] <= 0.0) {
			// nothing glows near it any more - or nothing yet, for a moment
			if (l->age > 0.3f)
				l->layer = -1;
			l->lit = false;
			continue;
		}

		const double inv_w = 1.0 / s[6];
		vec3_t middle = { (float)(s[3] * inv_w), (float)(s[4] * inv_w), (float)(s[5] * inv_w) };
		const double variance = s[7] * inv_w - DotProduct(middle, middle);
		const float spread = (float)sqrt(max(variance, 0.0));

		VectorAdd(middle, flow.light_sum_seed[set][i], l->origin);
		VectorCopy(l->origin, l->seed);
		l->gather = max(96.f, 3.f * spread);
		l->radius = min(max(size * spread, 4.f), max_radius);

		const float power = FLOW_LIGHT_UNIT * max(l->strength, 0.f) / (l->radius * l->radius);
		VectorSet(l->radiance, (float)s[0] * power, (float)s[1] * power, (float)s[2] * power);
		l->lit = true;
	}

	// The lights, brightest first.
	flow.num_dlights = 0;
	for (int l = 0; l < FLOW_LAYERS; l++)
		flow.layer_shadow[l] = -1;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0 || !l->lit)
			continue;
		// Below this a light lights nothing visible: a classic rocket light
		// is 1600 at its widest.
		const float power = flow_luminance(l->radiance) * l->radius * l->radius;
		if (power < 4.f)
			continue;

		int at = flow.num_dlights;
		while (at > 0) {
			const flow_light_t *o = &flow.lights[flow.light_slot[at - 1]];
			if (flow_luminance(o->radiance) * o->radius * o->radius >= power)
				break;
			flow.light_slot[at] = flow.light_slot[at - 1];
			at--;
		}
		flow.light_slot[at] = i;
		flow.num_dlights++;
	}

	for (int n = 0; n < flow.num_dlights; n++) {
		const flow_light_t *l = &flow.lights[flow.light_slot[n]];
		dlight_t *dl = &flow.dlights[n];
		memset(dl, 0, sizeof(*dl));
		VectorCopy(l->origin, dl->origin);
		VectorCopy(l->radiance, dl->color);
		dl->intensity = 25.f;   // add_dlights takes color x intensity / 25
		dl->radius = l->radius;
		dl->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
		dl->light_type = DLIGHT_SPHERE;

		if (flow.layer_shadow[l->layer] < 0)
			flow.layer_shadow[l->layer] = n;
	}
}

// The light slots and lights as flow.glsl and flow_lights.comp see them.
static void flow_lights_params(int set, FlowParams *params)
{
	params->counts[1] = (uint32_t)flow.num_dlights;
	for (int n = 0; n < flow.num_dlights; n++) {
		const dlight_t *dl = &flow.dlights[n];
		VectorCopy(dl->origin, params->lights[n]);
		params->lights[n][3] = dl->radius;
		// what smoke at distance r scatters is albedo x this / r^2: an
		// isotropic scatterer sees radiance x pi radius^2 / r^2 of a sphere
		// light, a 4 pi'th of which goes each way
		VectorScale(dl->color, 0.25f * dl->radius * dl->radius, params->light_colors[n]);
	}

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		const flow_light_t *l = &flow.lights[i];
		if (l->layer < 0) {
			flow.light_sum_generation[set][i] = 0;
			continue;
		}
		VectorCopy(l->seed, params->seeds[i]);
		params->seeds[i][3] = l->gather;
		params->seed_layers[i][0] = (uint32_t)NvFlow_packLayerAndLevel(l->layer, 0);
		flow.light_sum_generation[set][i] = l->generation;
		VectorCopy(l->seed, flow.light_sum_seed[set][i]);
	}
}

// Marks the bricks of every block that the march needs to sample, for this
// frame's rays. Flow's own submission is already in the queue ahead of this.
static void flow_occupancy_dispatch(uint32_t blocks)
{
	if (!flow.occupancy_pipeline || blocks == 0)
		return;
	// never past the buffer, which has room for the most the grid can have
	blocks = min(blocks, (uint32_t)(flow.occupancy_stride / (4 * sizeof(uint32_t))));

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.occupancy_pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		flow.lights_pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, NULL);
	vkCmdDispatch(cmd_buf, blocks, 1, 1);

	// for the ray-gen shaders, ray query or pipeline, and anything else
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
}

// Adds up the glow round each light slot's seed, for flow_lights_collect two
// frames on. Flow's own submission is already in the queue ahead of this.
static void flow_lights_dispatch(int set, uint32_t blocks)
{
	flow.light_sum_blocks[set] = 0;
	if (!flow.lights_pipeline || blocks == 0)
		return;

	bool any = false;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		any = any || flow.lights[i].layer >= 0;
	if (!any)
		return;

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, flow.lights_pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		flow.lights_pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, NULL);
	vkCmdDispatch(cmd_buf, blocks, 1, 1);

	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		0, 1, &barrier, 0, NULL, 0, NULL);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	flow.light_sum_blocks[set] = blocks;
}

/*
==============================================================================

COLLISION

==============================================================================
*/

/*
Cuts the world's triangles down to those near the gas - the blocks Flow had
last frame and round what the emitters are about to add - and makes them
Flow's collision mesh. False when there is nothing to collide with.

The faces' front, cross(pos1 - pos0, pos2 - pos0), is the side the player is
on, so Flow's signed distance is negative inside the walls. The band where the
gas is held still runs pt_flow_collision_depth units in and only
pt_flow_collision_margin out: in front of a face it leaves a gap, and the gas
crosses it only by soaking through the band. Measured headless against a
floor, a ceiling and a wall: 12 in and 3 out lets none through the floor or
the ceiling and a 0.06% trace through the wall, where +-4.5 let 40% through
the ceiling (hot gas presses up into it harder than into anything else).
*/
static bool flow_collision_update(void)
{
	if (!cvar_pt_flow_collision->integer || flow.num_world_tris == 0)
		return false;

	vec3_t mins, maxs;
	ClearBounds(mins, maxs);
	if (flow.have_blocks) {
		AddPointToBounds(flow.blocks_min, mins, maxs);
		AddPointToBounds(flow.blocks_max, mins, maxs);
	}
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		const float reach = 128.f * e->scale;
		vec3_t p;
		for (int k = 0; k < 3; k++) p[k] = e->origin[k] - reach;
		AddPointToBounds(p, mins, maxs);
		for (int k = 0; k < 3; k++) p[k] = e->origin[k] + reach;
		AddPointToBounds(p, mins, maxs);
		if (e->kind == R_FLOW_BEAM)
			AddPointToBounds(e->end, mins, maxs);
	}
	if (mins[0] > maxs[0])
		return false;

	int region[6];
	for (int k = 0; k < 3; k++) {
		region[k] = (int)floorf((mins[k] - 96.f) / FLOW_MESH_STEP);
		region[k + 3] = (int)ceilf((maxs[k] + 96.f) / FLOW_MESH_STEP);
	}

	if (!flow.mesh_region_valid || memcmp(region, flow.mesh_region, sizeof(region))) {
		memcpy(flow.mesh_region, region, sizeof(region));
		flow.mesh_region_valid = true;

		vec3_t box_min, box_max;
		for (int k = 0; k < 3; k++) {
			box_min[k] = region[k] * FLOW_MESH_STEP;
			box_max[k] = region[k + 3] * FLOW_MESH_STEP;
		}

		int count = 0;
		for (int t = 0; t < flow.num_world_tris && count < FLOW_MAX_MESH_TRIS; t++) {
			const float *v = flow.world_tris + t * 9;
			bool outside = false;
			for (int k = 0; k < 3 && !outside; k++) {
				const float lo = min(v[k], min(v[k + 3], v[k + 6]));
				const float hi = max(v[k], max(v[k + 3], v[k + 6]));
				outside = hi < box_min[k] || lo > box_max[k];
			}
			if (outside)
				continue;

			if (count == flow.mesh_capacity) {
				flow.mesh_capacity = flow.mesh_capacity ? flow.mesh_capacity * 2 : 4096;
				flow.mesh_positions = Z_Realloc(flow.mesh_positions, sizeof(NvFlowFloat3) * 3 * flow.mesh_capacity);
				flow.mesh_indices = Z_Realloc(flow.mesh_indices, sizeof(int) * 3 * flow.mesh_capacity);
				flow.mesh_counts = Z_Realloc(flow.mesh_counts, sizeof(int) * flow.mesh_capacity);
			}
			for (int j = 0; j < 3; j++) {
				NvFlowFloat3 *p = &flow.mesh_positions[count * 3 + j];
				p->x = v[j * 3 + 0];
				p->y = v[j * 3 + 1];
				p->z = v[j * 3 + 2];
				flow.mesh_indices[count * 3 + j] = count * 3 + j;
			}
			flow.mesh_counts[count] = 3;
			count++;
		}

		flow.num_mesh_tris = count;
		flow.mesh_version++;
	}

	if (flow.num_mesh_tris == 0)
		return false;

	NvFlowGridEmitterMeshParams *mesh = &flow.mesh;
	*mesh = NvFlowEmitterMeshParams_default;
	mesh->luid = 0x300;
	mesh->minDistance = -max(cvar_pt_flow_collision_depth->value, 0.f);
	mesh->maxDistance = max(cvar_pt_flow_collision_margin->value, 0.f);
	// Never makes blocks of its own: only where there is gas already.
	mesh->allocateMask = NV_FLOW_FALSE;
	// Still air, everywhere in the band, at once; nothing else is touched,
	// which also keeps Flow from running the mesh on the density at all.
	mesh->velocityIsWorldSpace = NV_FLOW_TRUE;
	mesh->velocity.x = mesh->velocity.y = mesh->velocity.z = 0.f;
	mesh->divergence = 0.f;
	mesh->coupleRateVelocity = 1000.f;
	mesh->coupleRateDivergence = 1000.f;
	mesh->coupleRateTemperature = 0.f;
	mesh->coupleRateFuel = 0.f;
	mesh->coupleRateBurn = 0.f;
	mesh->coupleRateSmoke = 0.f;
	mesh->meshPositions = flow.mesh_positions;
	mesh->meshPositionCount = (NvFlowUint64)flow.num_mesh_tris * 3;
	mesh->meshPositionVersion = flow.mesh_version;
	mesh->meshFaceVertexIndices = flow.mesh_indices;
	mesh->meshFaceVertexIndexCount = (NvFlowUint64)flow.num_mesh_tris * 3;
	mesh->meshFaceVertexIndexVersion = flow.mesh_version;
	mesh->meshFaceVertexCounts = flow.mesh_counts;
	mesh->meshFaceVertexCountCount = (NvFlowUint64)flow.num_mesh_tris;
	mesh->meshFaceVertexCountVersion = flow.mesh_version;
	// After the pressure solve, which knows nothing of walls and would
	// otherwise put the flow straight back through them.
	mesh->applyPostPressure = NV_FLOW_TRUE;
	// Every layer with physicsCollisionEnabled, one mesh for all.
	mesh->isPhysicsCollision = NV_FLOW_TRUE;
	flow.mesh_ptr = mesh;
	return true;
}

/*
==============================================================================

THE FRAME

==============================================================================
*/

// Flow's layer for each r_flow_style_t is the style's own number.
typedef char flow_layers_match_styles[(R_FLOW_STYLES == FLOW_LAYERS) ? 1 : -1];

// How one layer's gas behaves, starting from Flow's defaults.
static void flow_layer_simulate(NvFlowGridSimulateLayerParams *simulate, int layer, float cell_size, bool collide)
{
	*simulate = NvFlowGridSimulateLayerParams_default;
	simulate->luid = 1 + layer;
	simulate->layer = layer;
	simulate->densityCellSize = cell_size;
	// Below pt_flow_rate frames a second every frame steps by its own time,
	// so the gas keeps up with the game; flow_simulate caps how big a step
	// gets. Above it, Flow's own stepper steps pt_flow_rate times a second,
	// on some frames and not others - a frame with no step costs next to
	// nothing, and draws the last one.
	const float rate = cvar_pt_flow_rate->value;
	if (rate > 0.f && flow.frame_dt > 0.f && flow.frame_dt < 1.f / rate) {
		simulate->enableVariableTimeStep = NV_FLOW_FALSE;
		simulate->stepsPerSecond = rate;
		simulate->maxStepsPerSimulate = 1;
	}
	else {
		simulate->enableVariableTimeStep = NV_FLOW_TRUE;
		simulate->maxStepsPerSimulate = (NvFlowUint)Cvar_ClampInteger(cvar_pt_flow_substeps, 1, 4);
	}
	simulate->forceClear = flow.force_clear;
	simulate->vorticity.forceScale = cvar_pt_flow_vorticity->value;
	simulate->physicsCollisionEnabled = collide ? NV_FLOW_TRUE : NV_FLOW_FALSE;

	// Quake is Z-up like Flow, and Flow's gravity of -100 is about right
	// in Quake units: with buoyancy 2 a cell at temperature 1 accelerates
	// upward at 200 units/s^2.
	NvFlowAdvectionCombustionParams *advection = &simulate->advection;

	switch (layer) {
	case R_FLOW_FIRE:
		advection->buoyancyPerTemp = cvar_pt_flow_buoyancy->value;
		advection->coolingRate = max(cvar_pt_flow_cooling->value, 0.f);
		advection->smokePerBurn = max(cvar_pt_flow_smoke_per_burn->value, 0.f);
		// Smoke loses the larger of `damping` of itself and `fade` each
		// second: the first thins a thick cloud quickly, the second finishes
		// off the thin wisps that would otherwise hang about.
		advection->smoke.damping = Cvar_ClampValue(cvar_pt_flow_smoke_damping, 0.f, 1.f);
		advection->smoke.fade = max(cvar_pt_flow_smoke_fade->value, 0.f);
		// Flow's fuel never goes but by burning, and what the fireball did
		// not burn smoulders on just above the ignition temperature: no glow,
		// smoke too thin to see, and blocks kept for seconds. Fading it takes
		// a quarter off the blocks a rocket has at its most (64 to 48);
		// fading it faster (0.5: 44) also cools the fireball's heart sooner.
		advection->fuel.fade = max(cvar_pt_flow_fuel_fade->value, 0.f);
		break;

	case R_FLOW_PLASMA:
		// Heat without fuel: nothing burns, it only glows as it cools, and
		// leaves its smoke hanging after - about 3.5 s at the defaults.
		advection->buoyancyPerTemp = cvar_pt_flow_bfg_buoyancy->value;
		advection->coolingRate = max(cvar_pt_flow_bfg_cooling->value, 0.1f);
		advection->smoke.damping = Cvar_ClampValue(cvar_pt_flow_bfg_smoke_damping, 0.f, 1.f);
		advection->smoke.fade = max(cvar_pt_flow_bfg_smoke_fade->value, 0.f);
		// Strong vorticity tears the blast's puffs into ragged green smoke
		// within half a second; 30% of the speed going each second keeps
		// what the puffs were thrown out with from carrying the cloud off.
		simulate->vorticity.forceScale = max(cvar_pt_flow_bfg_vorticity->value, 0.f);
		advection->velocity.damping = 0.3f;
		break;

	case R_FLOW_BUBBLES:
		// No heat and nothing to burn: the bubbles are smoke that floats.
		// Combustion stays on all the same - it is also what clears the
		// expansion an emitter leaves in the velocity, and without it that
		// stays and spreads until the layer has every block there is.
		advection->buoyancyPerSmoke = max(cvar_pt_flow_bubble_rise->value, 0.f);
		advection->buoyancyMaxSmoke = 1.f;
		// Water drags: half the speed goes each second, so the bubbles
		// settle to a steady rise instead of gathering speed to the surface.
		advection->velocity.damping = 0.5f;
		advection->smoke.damping = 0.3f;
		advection->smoke.fade = max(cvar_pt_flow_bubble_fade->value, 0.f);
		break;
	}
}

// How long an emitter emits; a trail, for as long as it is given.
static float flow_emitter_life(const flow_emitter_t *e)
{
	if (e->radius > 0.f)
		return e->life;
	if (e->kind == R_FLOW_BEAM)
		return max(cvar_pt_flow_beam_burst->value, 0.f);
	return max(cvar_pt_flow_burst->value, 0.f);
}

static bool flow_emitter_emitting(const flow_emitter_t *e)
{
	if (e->kind == R_FLOW_TRAIL)
		return e->seen;
	return e->age >= 0.f && e->age < flow_emitter_life(e);
}

// What an emitter fills itself with: its radius at scale 1 (for a beam, its
// half thickness), how fast it pushes out at its edge, and its heat, fuel
// and smoke.
static void flow_emitter_gas(const flow_emitter_t *e, float *radius, float *expansion,
	float *temperature, float *fuel, float *smoke)
{
	switch (e->layer) {
	case R_FLOW_PLASMA:
		*radius = cvar_pt_flow_bfg_radius->value;
		*expansion = cvar_pt_flow_bfg_expansion->value;
		*temperature = 1.f;
		*fuel = 0.f;
		*smoke = cvar_pt_flow_bfg_smoke->value;
		break;
	case R_FLOW_BUBBLES:
		*radius = cvar_pt_flow_bubble_radius->value;
		*expansion = cvar_pt_flow_bubble_expansion->value;
		*temperature = 0.f;
		*fuel = 0.f;
		*smoke = cvar_pt_flow_bubble_amount->value;
		break;
	default:
		*radius = cvar_pt_flow_radius->value;
		*expansion = cvar_pt_flow_expansion->value;
		*temperature = cvar_pt_flow_temperature->value;
		*fuel = cvar_pt_flow_fuel->value;
		*smoke = cvar_pt_flow_smoke->value;
		break;
	}

	if (e->kind == R_FLOW_TRAIL) {
		*radius = cvar_pt_flow_trail_radius->value;
		*expansion = cvar_pt_flow_trail_expansion->value;
		if (e->layer != R_FLOW_BUBBLES)
			*smoke = cvar_pt_flow_trail_smoke->value;
	}
	else if (e->kind == R_FLOW_BEAM) {
		*radius = cvar_pt_flow_beam_radius->value;
		*expansion = cvar_pt_flow_beam_expansion->value;
		if (e->layer != R_FLOW_BUBBLES)
			*smoke = cvar_pt_flow_trail_smoke->value;
	}

	if (e->radius > 0.f) {
		*radius = e->radius;
		*expansion = e->expansion;
	}
}

// Flow's shadow pass, per layer: toward the brightest light of the layer, if
// it has one; nothing else of the pass is used. The shadow is worked out from
// smoke alone, so the colour map is a flat opacity of 1, and the attenuation
// is the layer's extinction per unit of smoke, as flow.glsl has it. With the
// defaults' thin smoke it darkens the far side of a cloud by about 10%.
static bool flow_build_offscreen(float cell_size)
{
	static const float x_points[2] = { 0.f, 1.f };
	static const NvFlowFloat4 rgba_points[2] = { { 1.f, 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f, 1.f } };
	static const float color_scale_points[2] = { 1.f, 1.f };

	bool any = false;
	for (int layer = 0; layer < FLOW_LAYERS; layer++) {
		NvFlowGridOffscreenLayerParams *off = &flow.offscreen[layer];
		*off = NvFlowGridOffscreenLayerParams_default;
		off->luid = 0x200 + layer;
		off->layer = layer;
		off->level = 0;
		off->colormap.resolution = 2;
		off->colormap.xPoints = x_points;
		off->colormap.xPointCount = 2;
		off->colormap.rgbaPoints = rgba_points;
		off->colormap.rgbaPointCount = 2;
		off->colormap.colorScalePoints = color_scale_points;
		off->colormap.colorScalePointCount = 2;
		off->colormap.colorScale = 1.f;
		off->debugVolume.enabled = NV_FLOW_FALSE;
		off->shadow.enabled = NV_FLOW_FALSE;
		flow.offscreen_ptrs[layer] = off;

		const int n = flow.layer_shadow[layer];
		if (!cvar_pt_flow_shadow->integer || n < 0)
			continue;

		const float density = layer == R_FLOW_PLASMA ? cvar_pt_flow_bfg_smoke_density->value : cvar_pt_flow_density->value;
		off->shadow.enabled = NV_FLOW_TRUE;
		off->shadow.isPointLight = NV_FLOW_TRUE;
		off->shadow.lightPosition.x = flow.dlights[n].origin[0];
		off->shadow.lightPosition.y = flow.dlights[n].origin[1];
		off->shadow.lightPosition.z = flow.dlights[n].origin[2];
		off->shadow.attenuation = max(density, 0.f) / cell_size;
		off->shadow.minIntensity = Cvar_ClampValue(cvar_pt_flow_shadow_min, 0.f, 1.f);
		any = true;
	}
	return any;
}

static void flow_build_params(float dt)
{
	const float cell_size = max(cvar_pt_flow_cell_size->value, 0.5f);
	const float couple_rate = max(cvar_pt_flow_couple_rate->value, 0.f);
	const bool collide = flow_collision_update();

	for (int layer = 0; layer < FLOW_LAYERS; layer++) {
		flow_layer_simulate(&flow.simulate[layer], layer, cell_size, collide);
		flow.simulate_ptrs[layer] = &flow.simulate[layer];
	}

	int num_spheres = 0;
	int num_boxes = 0;
	for (int i = 0; i < flow.num_emitters; i++) {
		const flow_emitter_t *e = &flow.emitters[i];
		if (!flow_emitter_emitting(e))
			continue;

		float radius, expansion, temperature, fuel, smoke;
		flow_emitter_gas(e, &radius, &expansion, &temperature, &fuel, &smoke);
		radius = max(radius * e->scale, cell_size);
		// The divergence Flow's pressure solve keeps in each velocity cell is
		// half the difference of the velocities either side of it, in world
		// units per second (PressureDivergenceCS.hlsl). A ball growing at
		// `expansion` units/s at its surface diverges by 3 * expansion /
		// radius per unit length, and the velocity grid's cells are twice the
		// density grid's.
		const float divergence = 3.f * expansion * (2.f * cell_size) / radius;

		if (e->kind == R_FLOW_BEAM) {
			// A box along the segment: its x axis the beam, centred between
			// the ends. Flow's matrices act on row vectors, so the rows are
			// the axes and the last is the translation.
			vec3_t dir, side, up;
			VectorSubtract(e->end, e->origin, dir);
			const float length = VectorNormalize(dir);
			if (length < 1.f)
				continue;
			// Quake's `up` here is side x dir; dir x side keeps the box a rotation
			MakeNormalVectors(dir, side, up);
			CrossProduct(dir, side, up);

			NvFlowGridEmitterBoxParams *box = &flow.boxes[num_boxes];
			*box = NvFlowEmitterBoxParams_default;
			box->luid = e->luid;
			box->layer = e->layer;
			const NvFlowFloat4x4 to_world = {
				dir[0], dir[1], dir[2], 0.f,
				side[0], side[1], side[2], 0.f,
				up[0], up[1], up[2], 0.f,
				0.5f * (e->origin[0] + e->end[0]), 0.5f * (e->origin[1] + e->end[1]), 0.5f * (e->origin[2] + e->end[2]), 1.f
			};
			box->localToWorld = to_world;
			box->localToWorldVelocity = to_world;
			box->position.x = box->position.y = box->position.z = 0.f;
			box->halfSize.x = 0.5f * length;
			box->halfSize.y = box->halfSize.z = radius;
			box->velocity.x = box->velocity.y = box->velocity.z = 0.f;
			box->divergence = divergence;
			box->temperature = temperature;
			box->fuel = fuel;
			box->burn = 0.f;
			box->smoke = smoke;
			box->coupleRateVelocity = 0.f;
			box->coupleRateDivergence = couple_rate;
			box->coupleRateTemperature = couple_rate;
			box->coupleRateFuel = couple_rate;
			box->coupleRateBurn = 0.f;
			box->coupleRateSmoke = couple_rate;
			flow.box_ptrs[num_boxes++] = box;
			continue;
		}

		NvFlowGridEmitterSphereParams *sphere = &flow.spheres[num_spheres];
		*sphere = NvFlowEmitterSphereParams_default;
		sphere->luid = e->luid;
		sphere->layer = e->layer;
		sphere->position.x = e->origin[0];
		sphere->position.y = e->origin[1];
		sphere->position.z = e->origin[2];
		sphere->radius = radius;
		sphere->velocity.x = e->velocity[0];
		sphere->velocity.y = e->velocity[1];
		sphere->velocity.z = e->velocity[2];
		sphere->divergence = divergence;
		sphere->temperature = temperature;
		sphere->fuel = fuel;
		sphere->burn = 0.f;
		sphere->smoke = smoke;
		// The rates are per second: at 30 the cells inside reach the targets
		// within a frame or two, and the velocity is left to the expansion.
		// A blast's puff emits for only three frames, so it fills itself five
		// times as hard, and gives the gas its outward throw at the usual rate.
		const float rate = e->radius > 0.f ? 5.f * couple_rate : couple_rate;
		sphere->coupleRateVelocity = VectorEmpty(e->velocity) ? 0.f : couple_rate;
		sphere->coupleRateDivergence = rate;
		sphere->coupleRateTemperature = rate;
		sphere->coupleRateFuel = rate;
		sphere->coupleRateBurn = 0.f;
		sphere->coupleRateSmoke = rate;
		flow.sphere_ptrs[num_spheres++] = sphere;
	}

	flow.shadow_pass = flow_build_offscreen(cell_size);

	flow.version++;
	flow.type_snapshots[0] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.simulate_ptrs, FLOW_LAYERS
	};
	flow.type_snapshots[1] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.sphere_ptrs, (NvFlowUint64)num_spheres
	};
	flow.type_snapshots[2] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterBoxParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.box_ptrs, (NvFlowUint64)num_boxes
	};
	flow.type_snapshots[3] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridEmitterMeshParams_NvFlowReflectDataType,
		(NvFlowUint8 **)&flow.mesh_ptr, collide ? 1u : 0u
	};
	flow.type_snapshots[4] = (NvFlowDatabaseTypeSnapshot) {
		flow.version, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType,
		(NvFlowUint8 **)flow.offscreen_ptrs, FLOW_LAYERS
	};

	flow.sim_time += dt;

	NvFlowGridParamsDescSnapshot snapshot = {
		.snapshot = { flow.version, flow.type_snapshots, LENGTH(flow.type_snapshots) },
		.absoluteSimTime = flow.sim_time,
		.deltaTime = dt,
		.globalForceClear = flow.force_clear,
	};
	flow.params_iface->commitParams(flow.grid_params, &snapshot);
}

// Ages the emitters and the light slots by `dt`, and lets go of the emitters
// that are done: a burst or a beam that has had its time, a trail that was not
// given again this frame. Flow drops what it knew of an emitter once its luid
// stops coming.
static void flow_age(float dt)
{
	int kept = 0;
	for (int i = 0; i < flow.num_emitters; i++) {
		flow_emitter_t *e = &flow.emitters[i];
		e->age += dt;
		if (e->kind == R_FLOW_TRAIL) {
			if (!e->seen)
				continue;
			e->seen = false;
		}
		else if (e->age >= flow_emitter_life(e)) {
			continue;
		}
		flow.emitters[kept++] = *e;
	}
	flow.num_emitters = kept;

	for (int i = 0; i < FLOW_MAX_LIGHTS; i++) {
		if (flow.lights[i].layer >= 0)
			flow.lights[i].age += dt;
	}
}

// Runs this frame's simulation and acquires its result for `set`, filling
// `params` with where everything is. False when there is nothing to draw.
static bool flow_simulate(const refdef_t *fd, int set, FlowParams *params)
{
	float dt = 0.f;
	if (flow.have_last_time && fd->time >= flow.last_time)
		dt = fd->time - flow.last_time;
	flow.last_time = fd->time;
	flow.have_last_time = true;

	// Below 20 fps the gas slows down rather than taking steps too long
	// for the solver; paused, it holds still.
	dt = min(dt, 0.05f);
	if (dt > 0.f)
		flow.frame_dt = flow.frame_dt > 0.f ? 0.9f * flow.frame_dt + 0.1f * dt : dt;

	if (flow.idle && flow.num_emitters == 0 && !flow.force_clear)
		return false;

	flow_build_params(dt);

	NvFlowGridParamsSnapshot *snapshot = flow.params_iface->getParamsSnapshot(flow.grid_params, flow.sim_time, 0);
	NvFlowGridParamsDesc params_desc = { 0 };
	NvFlowTextureAcquire *density_acquire = NULL;
	NvFlowBufferAcquire *table_acquire = NULL;
	bool have_blocks = false;
	bool have_bubbles = false;

	if (flow.params_iface->mapParamsDesc(flow.grid_params, snapshot, &params_desc)) {
		NvFlowGridRenderData render_data = { 0 };

		flow.grid_iface->simulate(flow.context, flow.grid, &params_desc, flow.force_clear);
		// The shadow goes into the density's burn channel, and what
		// getRenderData hands over after this is that copy.
		if (flow.shadow_pass)
			flow.grid_iface->offscreen(flow.context, flow.grid, &params_desc);
		flow.grid_iface->getRenderData(flow.context, flow.grid, &render_data);

		const NvFlowSparseParams *sparse = &render_data.sparseParams;
		if (render_data.densityTexture && render_data.sparseBuffer &&
			sparse->levelCount > 0 && sparse->levels[0].numLocations > 0) {
			// The density level; every layer's blocks are in its one table.
			const NvFlowSparseLevelParams *level = &sparse->levels[0];
			params->block_dim_bits[0] = level->blockDimBits.x;
			params->block_dim_bits[1] = level->blockDimBits.y;
			params->block_dim_bits[2] = level->blockDimBits.z;
			params->table_dim_less_one[0] = level->tableDimLessOne.x;
			params->table_dim_less_one[1] = level->tableDimLessOne.y;
			params->table_dim_less_one[2] = level->tableDimLessOne.z;
			params->table_dim_less_one[3] = level->tableDimBits_x;
			params->table_offsets[0] = level->tableDimBits_xy;
			params->table_offsets[1] = level->locationOffset;
			params->table_offsets[2] = level->blockLevelOffsetGlobal;
			params->dim_inv[0] = level->dimInv.x;
			params->dim_inv[1] = level->dimInv.y;
			params->dim_inv[2] = level->dimInv.z;
			params->counts[0] = level->numLocations;

			vec3_t world_min = { FLT_MAX, FLT_MAX, FLT_MAX };
			vec3_t world_max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

			for (NvFlowUint i = 0; i < sparse->layerCount; i++) {
				const NvFlowSparseLayerParams *layer = &sparse->layers[i];
				int l = 0;
				while (l < FLOW_LAYERS && layer->layerAndLevel != NvFlow_packLayerAndLevel(l, 0))
					l++;
				if (l == FLOW_LAYERS || layer->numLocations == 0)
					continue;

				FlowLayerParams *dst = &params->layers[l];
				VectorCopy(&layer->worldMin.x, dst->world_min);
				VectorCopy(&layer->worldMax.x, dst->world_max);
				dst->info[0] = 1;
				dst->info[1] = (uint32_t)layer->layerAndLevel;

				for (int k = 0; k < 3; k++) {
					world_min[k] = min(world_min[k], dst->world_min[k]);
					world_max[k] = max(world_max[k], dst->world_max[k]);
				}

				// Every layer has the same cells.
				params->cell_size_inv[0] = (float)(level->blockDimLessOne.x + 1) * layer->blockSizeWorldInv.x;
				params->cell_size_inv[1] = (float)(level->blockDimLessOne.y + 1) * layer->blockSizeWorldInv.y;
				params->cell_size_inv[2] = (float)(level->blockDimLessOne.z + 1) * layer->blockSizeWorldInv.z;

				have_blocks = true;
				have_bubbles = have_bubbles || l == R_FLOW_BUBBLES;
			}

			if (have_blocks) {
				VectorCopy(world_min, params->world_min);
				VectorCopy(world_max, params->world_max);
				density_acquire = flow.context_iface->enqueueAcquireTexture(flow.context, render_data.densityTexture);
				table_acquire = flow.context_iface->enqueueAcquireBuffer(flow.context, render_data.sparseBuffer);
			}

			// for next frame's collision mesh
			VectorCopy(world_min, flow.blocks_min);
			VectorCopy(world_max, flow.blocks_max);
			flow.have_blocks = have_blocks;
		}
		else {
			flow.have_blocks = false;
		}

		flow.params_iface->unmapParamsDesc(flow.grid_params, snapshot);
	}

	flow.force_clear = false;

	// Submits to qvk.queue_graphics, through flow_queue_submit.
	NvFlowUint64 flushed_frame;
	if (flow.device_iface->flush(flow.queue, &flushed_frame, NULL, NULL)) {
		// the renderer's own next submission reports it
		return false;
	}

	if (density_acquire)
		flow.context_iface->getAcquiredTexture(flow.context, density_acquire, &flow.set_density[set]);
	if (table_acquire)
		flow.context_iface->getAcquiredBuffer(flow.context, table_acquire, &flow.set_table[set]);

	flow_age(dt);

	// Once the last bubbles are gone, the next ones may be in other water.
	if (!have_bubbles) {
		bool emitting = false;
		for (int i = 0; i < flow.num_emitters; i++)
			emitting = emitting || flow.emitters[i].layer == R_FLOW_BUBBLES;
		if (!emitting)
			flow.have_bubble_surface = false;
	}

	if (!have_blocks || !flow.set_density[set] || !flow.set_table[set]) {
		flow.idle = (flow.num_emitters == 0);
		flow_release_set(set);
		return false;
	}

	flow.idle = false;
	return true;
}

// A new emitter. When they are all taken the oldest that has started makes
// way - a BFG blast's pieces wait their turn for up to a quarter of a second,
// and lasers coming in every frame must not push them out half drawn - and
// failing that the oldest of all.
static flow_emitter_t *flow_new_emitter(r_flow_kind_t kind, int layer, const vec3_t origin, float scale)
{
	if (flow.num_emitters == FLOW_MAX_EMITTERS) {
		int oldest = 0;
		while (oldest < FLOW_MAX_EMITTERS - 1 && flow.emitters[oldest].age < 0.f)
			oldest++;
		if (flow.emitters[oldest].age < 0.f)
			oldest = 0;
		memmove(flow.emitters + oldest, flow.emitters + oldest + 1, sizeof(flow.emitters[0]) * (FLOW_MAX_EMITTERS - 1 - oldest));
		flow.num_emitters--;
	}

	flow_emitter_t *e = &flow.emitters[flow.num_emitters++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	e->layer = layer;
	VectorCopy(origin, e->origin);
	VectorCopy(origin, e->end);
	e->scale = scale;
	e->luid = ++flow.next_luid + 0x1000;   // clear of the layers' and the mesh's luids
	flow.idle = false;
	return e;
}

// How far a blast's gas may go from its origin along `dir`, up to `distance`:
// the client's trace, or as far as it likes without one.
static float flow_blast_reach(const r_flow_emit_t *emit, const vec3_t dir, float distance)
{
	if (!emit->reach)
		return distance;
	return min(max(emit->reach(emit->origin, dir, distance), 0.f), distance);
}

// A piece of a blast: a plasma burst `distance` along `dir`, starting `start`
// seconds from now, thrown outward at 100 to 300 units/s.
static void flow_blast_puff(const vec3_t origin, const vec3_t dir, float distance,
	float radius, float expansion, float start, float life)
{
	vec3_t position;
	VectorMA(origin, distance, dir, position);

	flow_emitter_t *e = flow_new_emitter(R_FLOW_BURST, R_FLOW_PLASMA, position, 1.f);
	e->radius = radius;
	e->expansion = expansion;
	e->life = life;
	e->age = -start;
	if (distance > 1.f)
		VectorScale(dir, 200.f * (0.5f + frand()), e->velocity);
}

// A blast's piece `want` out along `dir`, or short of the wall that way by
// half its radius.
static float flow_blast_distance(const r_flow_emit_t *emit, const vec3_t dir, float want, float radius)
{
	return min(want, max(flow_blast_reach(emit, dir, want + radius) - 0.5f * radius, 0.f));
}

static void flow_random_dir(vec3_t dir)
{
	const float z = crand();
	const float r = sqrtf(max(1.f - z * z, 0.f));
	const float a = frand() * 2.f * M_PI;
	VectorSet(dir, r * cosf(a), r * sinf(a), z);
}

/*
The BFG's blast, after NVIDIA's 2019 RTX trailer (2:07 on). In the first
frames, a flower of pt_flow_bfg_puffs round puffs about the middle; then
pt_flow_bfg_lumps smaller ones further out and a little later, so that it
swells into a lumpy cloud and not a ball; and pt_flow_bfg_jets jets shot out
up to pt_flow_bfg_jet_length, strings of beads that grow as they go and end
in a bigger puff. Every piece is a burst of its own, a few frames long and
started late by a negative age, so the order is set here once and Flow
plays it out; the plasma's vorticity (pt_flow_bfg_vorticity) then tears it
all into ragged green smoke. `reach` - the client's trace - keeps each piece
short of the walls and points the jets where there is room for them.

Tuned headless against the trailer's frames: a flower at 0.05 s, the jets
out by 0.2 s, torn smoke from 0.4 s, the glow gone by 0.8 s.
*/
static void flow_emit_blast(const r_flow_emit_t *emit)
{
	const float u = max(cvar_pt_flow_bfg_blast_size->value, 0.1f) * emit->scale;
	const float push = max(cvar_pt_flow_bfg_blast_expansion->value, 0.f);
	const float puff_life = 0.05f;
	vec3_t dir;

	// the middle
	VectorClear(dir);
	flow_blast_puff(emit->origin, dir, 0.f, 12.f * u, push, 0.f, puff_life);

	// The flower: spread evenly round a sphere (a golden-angle spiral) turned
	// a random way, all but at once.
	const int lobes = Cvar_ClampInteger(cvar_pt_flow_bfg_puffs, 0, 16);
	const float turn = frand() * 2.f * M_PI;
	const float tilt = frand() * M_PI;
	for (int i = 0; i < lobes; i++) {
		const float z = 1.f - (2.f * i + 1.f) / lobes;
		const float r = sqrtf(max(1.f - z * z, 0.f));
		const float a = turn + i * 2.39996323f;
		VectorSet(dir, r * cosf(a),
			r * sinf(a) * cosf(tilt) - z * sinf(tilt),
			r * sinf(a) * sinf(tilt) + z * cosf(tilt));
		const float radius = 15.f * u;
		flow_blast_puff(emit->origin, dir, flow_blast_distance(emit, dir, 34.f * u, radius),
			radius, push, 0.005f * i, puff_life);
	}

	// The lumps: the further out, the later.
	const int lumps = Cvar_ClampInteger(cvar_pt_flow_bfg_lumps, 0, 48);
	for (int i = 0; i < lumps; i++) {
		flow_random_dir(dir);
		const float f = frand();
		const float radius = 11.f * u;
		flow_blast_puff(emit->origin, dir, flow_blast_distance(emit, dir, (35.f + 35.f * f) * u, radius),
			radius, push * (2.f / 3.f), 0.04f + 0.11f * (0.7f * f + 0.3f * frand()), puff_life);
	}

	// The jets, at 1000 units/s from a moment after the flower: each the way
	// with the most room of a few tried, at least 60 degrees from the others.
	const int jets = Cvar_ClampInteger(cvar_pt_flow_bfg_jets, 0, 8);
	const float longest = max(cvar_pt_flow_bfg_jet_length->value, 0.f) * u;
	const float end_radius = 11.f * u;
	const float first = 25.f * u;
	const float speed = 1000.f;
	vec3_t jet_dirs[8];

	for (int j = 0; j < jets; j++) {
		float room = -1.f;
		VectorClear(jet_dirs[j]);
		for (int tries = 0; tries < 16 && room < longest + end_radius; tries++) {
			flow_random_dir(dir);
			bool apart = true;
			for (int k = 0; k < j; k++)
				apart = apart && DotProduct(dir, jet_dirs[k]) < 0.5f;
			if (!apart)
				continue;
			const float r = flow_blast_reach(emit, dir, longest + end_radius);
			if (r > room) {
				room = r;
				VectorCopy(dir, jet_dirs[j]);
			}
		}

		const float length = min(longest * (0.6f + 0.4f * frand()), room - end_radius);
		if (length <= first)
			continue;

		const float delay = 0.06f * (0.6f + 0.8f * frand());
		for (float d = first; d < length; ) {
			const float radius = (5.f + 3.f * (d - first) / (length - first)) * u;
			flow_blast_puff(emit->origin, jet_dirs[j], d, radius, 0.5f * push, delay + d / speed, 0.04f);
			d += 1.7f * radius;
		}
		flow_blast_puff(emit->origin, jet_dirs[j], length, end_radius, push, delay + length / speed, 0.06f);
	}
}

static int vkpt_flow_emit(const r_flow_emit_t *emit)
{
	if (!flow.grid || !emit || emit->scale <= 0.f || (int)emit->style < 0 || emit->style >= R_FLOW_STYLES)
		return 0;

	int result = R_FLOW_DRAWN;

	switch (emit->kind) {
	case R_FLOW_BURST:
		{
			flow_emitter_t *e = flow_new_emitter(R_FLOW_BURST, emit->style, emit->origin, emit->scale);
			// A plasma burst is what the BFG does to a target it zaps: a small
			// puff that fills itself in a few frames, like a piece of its
			// blast, and pushes out gently - not a ball of its own. At the old
			// radius 24 and push 500 each swelled to some 50 blocks, measured
			// headless, and four of them took the BFG from 133 blocks to 339;
			// at 16 and 40 one takes 10 to 20.
			if (emit->style == R_FLOW_PLASMA) {
				e->radius = max(cvar_pt_flow_bfg_radius->value, 1.f);
				e->expansion = cvar_pt_flow_bfg_expansion->value;
				e->life = max(cvar_pt_flow_bfg_burst->value, 0.f);
			}
		}

		if (flow_take_light(emit->style, emit->origin, emit->scale) >= 0)
			result |= R_FLOW_LIT;

		if (emit->style == R_FLOW_BUBBLES) {
			flow.bubble_surface = flow.have_bubble_surface ? max(flow.bubble_surface, emit->surface) : emit->surface;
			flow.have_bubble_surface = true;
		}
		break;

	case R_FLOW_TRAIL:
		for (int i = 0; i < flow.num_emitters; i++) {
			flow_emitter_t *e = &flow.emitters[i];
			if (e->kind == R_FLOW_TRAIL && e->key == emit->key && e->layer == (int)emit->style) {
				VectorCopy(emit->origin, e->origin);
				e->scale = emit->scale;
				e->seen = true;
				flow.idle = false;
				return result;
			}
		}
		{
			flow_emitter_t *e = flow_new_emitter(R_FLOW_TRAIL, emit->style, emit->origin, emit->scale);
			e->key = emit->key;
			e->seen = true;
		}
		break;

	case R_FLOW_BEAM:
		{
			vec3_t delta;
			VectorSubtract(emit->end, emit->origin, delta);
			const float length = VectorLength(delta);
			const int segments = min(max((int)ceilf(length / FLOW_BEAM_SEGMENT), 1), 32);
			for (int s = 0; s < segments; s++) {
				vec3_t a, b;
				VectorMA(emit->origin, (float)s / segments, delta, a);
				VectorMA(emit->origin, (float)(s + 1) / segments, delta, b);
				flow_emitter_t *e = flow_new_emitter(R_FLOW_BEAM, emit->style, a, emit->scale);
				VectorCopy(b, e->end);
			}
		}
		break;

	case R_FLOW_BLAST:
		flow_emit_blast(emit);
		{
			// one light for all of it, gathered from as far as the jets go
			const int slot = flow_take_light(R_FLOW_PLASMA, emit->origin, emit->scale);
			if (slot >= 0) {
				flow_light_t *l = &flow.lights[slot];
				const float u = max(cvar_pt_flow_bfg_blast_size->value, 0.1f) * emit->scale;
				l->gather = max(l->gather, (max(cvar_pt_flow_bfg_jet_length->value, 0.f) + 24.f) * u);
				l->strength = cvar_pt_flow_bfg_blast_light->value;
				result |= R_FLOW_LIT;
			}
		}
		break;

	default:
		return 0;
	}

	return result;
}

#endif // USE_FLOW

/*
==============================================================================

DESCRIPTORS AND DUMMIES

==============================================================================
*/

static void flow_write_descriptors(int set, VkImageView density_view, VkImageLayout density_layout, VkBuffer table)
{
	VkDescriptorSet desc_set = set ? qvk.desc_set_textures_odd : qvk.desc_set_textures_even;

	VkDescriptorImageInfo density_info = {
		.sampler = qvk.tex_sampler_linear_clamp,
		.imageView = density_view,
		.imageLayout = density_layout,
	};
	VkDescriptorBufferInfo table_info = {
		.buffer = table,
		.offset = 0,
		.range = VK_WHOLE_SIZE,
	};
	VkDescriptorBufferInfo params_info = {
		.buffer = flow.params.buffer,
		.offset = set * flow.params_stride,
		.range = sizeof(FlowParams),
	};
	VkDescriptorBufferInfo lights_info = {
		.buffer = flow.light_sums.buffer,
		.offset = set * flow.light_sums_stride,
		.range = flow.light_sums_stride,
	};
	VkDescriptorBufferInfo occupancy_info = {
		.buffer = flow.occupancy.buffer,
		.offset = set * flow.occupancy_stride,
		.range = flow.occupancy_stride,
	};

	VkWriteDescriptorSet writes[] = {
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_DENSITY,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &density_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_TABLE,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &table_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_PARAMS,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &params_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_LIGHTS,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &lights_info,
		},
		{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = desc_set,
			.dstBinding = BINDING_OFFSET_FLOW_OCCUPANCY,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &occupancy_info,
		},
	};

	vkUpdateDescriptorSets(qvk.device, LENGTH(writes), writes, 0, NULL);
}

static VkResult flow_create_dummies(void)
{
	VkImageCreateInfo img_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_3D,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.extent = { 1, 1, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	_VK(vkCreateImage(qvk.device, &img_info, NULL, &flow.dummy_image));
	ATTACH_LABEL_VARIABLE(flow.dummy_image, IMAGE);

	VkMemoryRequirements mem_req;
	vkGetImageMemoryRequirements(qvk.device, flow.dummy_image, &mem_req);

	VkMemoryAllocateInfo mem_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	_VK(vkAllocateMemory(qvk.device, &mem_alloc_info, NULL, &flow.dummy_memory));
	_VK(vkBindImageMemory(qvk.device, flow.dummy_image, flow.dummy_memory, 0));

	const VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1,
	};

	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = flow.dummy_image,
		.viewType = VK_IMAGE_VIEW_TYPE_3D,
		.format = img_info.format,
		.subresourceRange = subresource_range,
	};
	_VK(vkCreateImageView(qvk.device, &view_info, NULL, &flow.dummy_view));
	ATTACH_LABEL_VARIABLE(flow.dummy_view, IMAGE_VIEW);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	IMAGE_BARRIER(cmd_buf,
		.image = flow.dummy_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	VkClearColorValue clear = { .float32 = { 0, 0, 0, 0 } };
	vkCmdClearColorImage(cmd_buf, flow.dummy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &subresource_range);

	IMAGE_BARRIER(cmd_buf,
		.image = flow.dummy_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	// Never read: FlowParams.block_dim_bits.w is 0 whenever this is bound.
	_VK(buffer_create(&flow.dummy_table, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

	// 256 is the most minStorageBufferOffsetAlignment may be.
	flow.params_stride = align(sizeof(FlowParams), 256);
	_VK(buffer_create(&flow.params, flow.params_stride * FLOW_SET_COUNT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	flow.params_mapped = buffer_map(&flow.params);
	memset(flow.params_mapped, 0, flow.params_stride * FLOW_SET_COUNT);

	// A block's sums for every block the grid can have.
	const size_t max_blocks = (size_t)Cvar_ClampInteger(cvar_pt_flow_max_blocks, 16, 4096);
	flow.light_sums_stride = align(max_blocks * FLOW_LIGHT_SUM_VEC4S * sizeof(vec4_t), 256);
	_VK(buffer_create(&flow.light_sums, flow.light_sums_stride * FLOW_SET_COUNT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	flow.light_sums_mapped = buffer_map(&flow.light_sums);
	memset(flow.light_sums_mapped, 0, flow.light_sums_stride * FLOW_SET_COUNT);

	// A block's bricks for every block the grid can have. Written before it
	// is read, every frame there are blocks.
	flow.occupancy_stride = align(max_blocks * 4 * sizeof(uint32_t), 256);
	_VK(buffer_create(&flow.occupancy, flow.occupancy_stride * FLOW_SET_COUNT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

	return VK_SUCCESS;
}

static void flow_destroy_dummies(void)
{
	buffer_destroy(&flow.occupancy);

	if (flow.light_sums_mapped)
		buffer_unmap(&flow.light_sums);
	flow.light_sums_mapped = NULL;
	buffer_destroy(&flow.light_sums);

	if (flow.params_mapped)
		buffer_unmap(&flow.params);
	flow.params_mapped = NULL;
	buffer_destroy(&flow.params);
	buffer_destroy(&flow.dummy_table);

	if (flow.dummy_view)
		vkDestroyImageView(qvk.device, flow.dummy_view, NULL);
	if (flow.dummy_image)
		vkDestroyImage(qvk.device, flow.dummy_image, NULL);
	if (flow.dummy_memory)
		vkFreeMemory(qvk.device, flow.dummy_memory, NULL);
	flow.dummy_view = VK_NULL_HANDLE;
	flow.dummy_image = VK_NULL_HANDLE;
	flow.dummy_memory = VK_NULL_HANDLE;
}

/*
==============================================================================

ENTRY POINTS

==============================================================================
*/

VkResult vkpt_flow_initialize(void)
{
	cvar_flow_available = Cvar_Get("flow_available", "0", CVAR_ROM);

	// Whether to start Flow at all, read when the renderer starts. Starting it
	// builds all of Flow's compute pipelines on the spot, in the driver's
	// shader compiler - which is where a startup hang once came from (see
	// EmitterNanoVdb in setup/build_flow.py) - so +set pt_flow 0 is the way
	// round one without moving the DLLs.
	cvar_pt_flow = Cvar_Get("pt_flow", "1", 0);

	// The menu's two: fire and plasma light the world and the smoke, and the
	// world holds the gas in.
	cvar_pt_flow_lighting = Cvar_Get("pt_flow_lighting", "1", CVAR_ARCHIVE);
	cvar_pt_flow_collision = Cvar_Get("pt_flow_collision", "1", CVAR_ARCHIVE);

	// The grid. The block count is read when the renderer starts.
	cvar_pt_flow_cell_size = Cvar_Get("pt_flow_cell_size", "3", 0);
	cvar_pt_flow_max_blocks = Cvar_Get("pt_flow_max_blocks", "512", 0);
	cvar_pt_flow_substeps = Cvar_Get("pt_flow_substeps", "1", 0);

	// An explosion: its size at scale 1 (a rocket), how long it emits, how
	// fast it pushes out, and what it fills itself with.
	cvar_pt_flow_radius = Cvar_Get("pt_flow_radius", "24", 0);
	cvar_pt_flow_burst = Cvar_Get("pt_flow_burst", "0.12", 0);
	cvar_pt_flow_expansion = Cvar_Get("pt_flow_expansion", "350", 0);
	cvar_pt_flow_temperature = Cvar_Get("pt_flow_temperature", "1", 0);
	cvar_pt_flow_fuel = Cvar_Get("pt_flow_fuel", "1", 0);
	cvar_pt_flow_smoke = Cvar_Get("pt_flow_smoke", "0.3", 0);
	cvar_pt_flow_couple_rate = Cvar_Get("pt_flow_couple_rate", "30", 0);

	// Flow's combustion, at its own defaults but for the smoke, which goes
	// sooner, and the heat, which stays a little longer (Flow's cooling 1.5:
	// the trailer's fireballs stay orange for over a second). Smoke loses
	// the larger of pt_flow_smoke_damping of itself (Flow's 0.3) and
	// pt_flow_smoke_fade (0.65) every second; burning makes
	// pt_flow_smoke_per_burn (Flow's 3) of it.
	cvar_pt_flow_buoyancy = Cvar_Get("pt_flow_buoyancy", "2", 0);
	cvar_pt_flow_cooling = Cvar_Get("pt_flow_cooling", "1.3", 0);
	cvar_pt_flow_smoke_fade = Cvar_Get("pt_flow_smoke_fade", "0.65", 0);
	cvar_pt_flow_smoke_damping = Cvar_Get("pt_flow_smoke_damping", "0.5", 0);
	cvar_pt_flow_smoke_per_burn = Cvar_Get("pt_flow_smoke_per_burn", "3", 0);
	cvar_pt_flow_vorticity = Cvar_Get("pt_flow_vorticity", "0.6", 0);

	// The look. Colours are relative to the adapted luminance, like the
	// explosion sprites' (pt_explosion_brightness 4 x 500). Fire glows by
	// its colour map times pt_flow_fire_brightness for every 32 units of it
	// looked through, however much smoke there is, and pt_flow_fire_orange
	// more orange than Flow's own map (0 to 1: up to 40% less green and 80%
	// less blue, in the glow and its light); the smoke darkens what is
	// behind it by pt_flow_density per unit of smoke per cell, shows
	// pt_flow_smoke_albedo x pt_flow_smoke_brightness of its own, and
	// scatters pt_flow_smoke_scatter of the fire's light.
	cvar_pt_flow_fire_brightness = Cvar_Get("pt_flow_fire_brightness", "40", 0);
	cvar_pt_flow_fire_orange = Cvar_Get("pt_flow_fire_orange", "0.3", 0);
	cvar_pt_flow_smoke_brightness = Cvar_Get("pt_flow_smoke_brightness", "20", 0);
	cvar_pt_flow_smoke_albedo = Cvar_Get("pt_flow_smoke_albedo", "0.25", 0);
	cvar_pt_flow_smoke_scatter = Cvar_Get("pt_flow_smoke_scatter", "0.8", 0);
	cvar_pt_flow_density = Cvar_Get("pt_flow_density", "0.01", 0);
	cvar_pt_flow_step = Cvar_Get("pt_flow_step", "0.75", 0);          // march step, in cells
	cvar_pt_flow_max_steps = Cvar_Get("pt_flow_max_steps", "256", 0);  // samples, past empty bricks

	// What it costs (see the top of this file): Flow's steps a second at
	// most, 0 for one every frame; how much coarser a reflection marches;
	// how much of fire's leftover fuel goes each second.
	cvar_pt_flow_rate = Cvar_Get("pt_flow_rate", "60", 0);
	cvar_pt_flow_reflect_step = Cvar_Get("pt_flow_reflect_step", "2", 0);
	cvar_pt_flow_fuel_fade = Cvar_Get("pt_flow_fuel_fade", "0.25", 0);

	// The BFG's plasma (R_FLOW_PLASMA): green heat and dark green smoke.
	// A burst of it is a target the BFG zaps: its radius, push and how long
	// it emits, like one of the blast's lumps. Brightness as
	// pt_flow_fire_brightness; the smoke as the fire's, its own colour green;
	// vorticity the plasma's own, strong enough to tear its blast apart (0.6,
	// the fire's, leaves the ball's trail smoother).
	cvar_pt_flow_bfg_radius = Cvar_Get("pt_flow_bfg_radius", "16", 0);
	cvar_pt_flow_bfg_expansion = Cvar_Get("pt_flow_bfg_expansion", "40", 0);
	cvar_pt_flow_bfg_burst = Cvar_Get("pt_flow_bfg_burst", "0.05", 0);
	cvar_pt_flow_bfg_cooling = Cvar_Get("pt_flow_bfg_cooling", "3", 0);
	cvar_pt_flow_bfg_buoyancy = Cvar_Get("pt_flow_bfg_buoyancy", "0.5", 0);
	cvar_pt_flow_bfg_brightness = Cvar_Get("pt_flow_bfg_brightness", "40", 0);
	cvar_pt_flow_bfg_smoke = Cvar_Get("pt_flow_bfg_smoke", "1", 0);
	cvar_pt_flow_bfg_smoke_fade = Cvar_Get("pt_flow_bfg_smoke_fade", "0.3", 0);
	cvar_pt_flow_bfg_smoke_damping = Cvar_Get("pt_flow_bfg_smoke_damping", "0.3", 0);
	cvar_pt_flow_bfg_smoke_brightness = Cvar_Get("pt_flow_bfg_smoke_brightness", "2", 0);
	cvar_pt_flow_bfg_smoke_density = Cvar_Get("pt_flow_bfg_smoke_density", "0.02", 0);
	cvar_pt_flow_bfg_vorticity = Cvar_Get("pt_flow_bfg_vorticity", "6", 0);

	// The BFG's own blast (R_FLOW_BLAST, flow_emit_blast): the flower's puffs,
	// the lumps round it, the jets and how far they go, the size of all of
	// it, how hard its pieces push out, and its light, which goes in place of
	// the classic one - about 1.6 times its peak, measured headless, and gone
	// after half a second.
	cvar_pt_flow_bfg_puffs = Cvar_Get("pt_flow_bfg_puffs", "8", 0);
	cvar_pt_flow_bfg_lumps = Cvar_Get("pt_flow_bfg_lumps", "24", 0);
	cvar_pt_flow_bfg_jets = Cvar_Get("pt_flow_bfg_jets", "3", 0);
	cvar_pt_flow_bfg_jet_length = Cvar_Get("pt_flow_bfg_jet_length", "200", 0);
	cvar_pt_flow_bfg_blast_size = Cvar_Get("pt_flow_bfg_blast_size", "1", 0);
	cvar_pt_flow_bfg_blast_expansion = Cvar_Get("pt_flow_bfg_blast_expansion", "60", 0);
	cvar_pt_flow_bfg_blast_light = Cvar_Get("pt_flow_bfg_blast_light", "2.5", 0);

	// A trail (the BFG ball in flight) and a beam (its lasers): radius - a
	// beam's half thickness - push and smoke at scale 1; a beam emits for
	// pt_flow_beam_burst seconds, about the laser's own life.
	cvar_pt_flow_trail_radius = Cvar_Get("pt_flow_trail_radius", "10", 0);
	cvar_pt_flow_trail_expansion = Cvar_Get("pt_flow_trail_expansion", "60", 0);
	cvar_pt_flow_trail_smoke = Cvar_Get("pt_flow_trail_smoke", "0.5", 0);
	cvar_pt_flow_beam_radius = Cvar_Get("pt_flow_beam_radius", "4", 0);
	cvar_pt_flow_beam_expansion = Cvar_Get("pt_flow_beam_expansion", "100", 0);
	cvar_pt_flow_beam_burst = Cvar_Get("pt_flow_beam_burst", "0.1", 0);

	// An explosion under water (R_FLOW_BUBBLES): foam that rises to the
	// surface. Rise is Flow's buoyancy per unit of smoke; fade the amount
	// lost per second, on top of 30% of it.
	cvar_pt_flow_bubble_radius = Cvar_Get("pt_flow_bubble_radius", "24", 0);
	cvar_pt_flow_bubble_expansion = Cvar_Get("pt_flow_bubble_expansion", "250", 0);
	cvar_pt_flow_bubble_amount = Cvar_Get("pt_flow_bubble_amount", "1", 0);
	cvar_pt_flow_bubble_rise = Cvar_Get("pt_flow_bubble_rise", "2", 0);
	cvar_pt_flow_bubble_fade = Cvar_Get("pt_flow_bubble_fade", "0.4", 0);
	cvar_pt_flow_bubble_brightness = Cvar_Get("pt_flow_bubble_brightness", "6", 0);
	cvar_pt_flow_bubble_density = Cvar_Get("pt_flow_bubble_density", "0.05", 0);

	// The light (pt_flow_lighting): fire at pt_flow_light times FLOW_LIGHT_UNIT,
	// plasma at pt_flow_bfg_light (the BFG's blast at pt_flow_bfg_blast_light),
	// a sphere pt_flow_light_size times as wide
	// as the glow is spread, up to pt_flow_light_radius; the smoke's shadow
	// toward it no darker than pt_flow_shadow_min.
	cvar_pt_flow_light = Cvar_Get("pt_flow_light", "1", 0);
	cvar_pt_flow_bfg_light = Cvar_Get("pt_flow_bfg_light", "0.6", 0);
	cvar_pt_flow_light_size = Cvar_Get("pt_flow_light_size", "0.5", 0);
	cvar_pt_flow_light_radius = Cvar_Get("pt_flow_light_radius", "24", 0);
	cvar_pt_flow_shadow = Cvar_Get("pt_flow_shadow", "1", 0);
	cvar_pt_flow_shadow_min = Cvar_Get("pt_flow_shadow_min", "0.15", 0);

	// The world holding the gas (pt_flow_collision): units behind a face and
	// in front of it where the gas is held still.
	cvar_pt_flow_collision_depth = Cvar_Get("pt_flow_collision_depth", "12", 0);
	cvar_pt_flow_collision_margin = Cvar_Get("pt_flow_collision_margin", "3", 0);

	Cvar_FullSet("flow_available", "0", CVAR_ROM, FROM_CODE);

	VkResult result = flow_create_dummies();
	if (result != VK_SUCCESS)
		return result;

	for (int set = 0; set < FLOW_SET_COUNT; set++)
		flow_write_descriptors(set, flow.dummy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, flow.dummy_table.buffer);

	VkDescriptorSetLayout desc_set_layouts[] = {
		qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
	};
	CREATE_PIPELINE_LAYOUT(qvk.device, &flow.lights_pipeline_layout,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts = desc_set_layouts,
	);
	ATTACH_LABEL_VARIABLE(flow.lights_pipeline_layout, PIPELINE_LAYOUT);

	flow.num_emitters = 0;
	flow.updated_frame = 0;
	for (int i = 0; i < FLOW_MAX_LIGHTS; i++)
		flow.lights[i].layer = -1;

#if USE_FLOW
	flow_free_lights();
	flow_create_simulation();
#endif

	return VK_SUCCESS;
}

VkResult vkpt_flow_destroy(void)
{
#if USE_FLOW
	flow_destroy_simulation();
	Z_Free(flow.world_tris);
	Z_Free(flow.mesh_positions);
	Z_Free(flow.mesh_indices);
	Z_Free(flow.mesh_counts);
	flow.world_tris = NULL;
	flow.num_world_tris = 0;
	flow.mesh_positions = NULL;
	flow.mesh_indices = NULL;
	flow.mesh_counts = NULL;
	flow.mesh_capacity = 0;
	flow.num_mesh_tris = 0;
	flow.mesh_region_valid = false;
#endif
	if (flow.lights_pipeline_layout)
		vkDestroyPipelineLayout(qvk.device, flow.lights_pipeline_layout, NULL);
	flow.lights_pipeline_layout = VK_NULL_HANDLE;
	flow_destroy_dummies();
	flow.num_emitters = 0;
	return VK_SUCCESS;
}

VkResult vkpt_flow_create_pipelines(void)
{
	VkComputePipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = SHADER_STAGE(QVK_MOD_FLOW_LIGHTS_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
		.layout = flow.lights_pipeline_layout,
	};

	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &flow.lights_pipeline));
	ATTACH_LABEL_VARIABLE(flow.lights_pipeline, PIPELINE);

	pipeline_info.stage = (VkPipelineShaderStageCreateInfo)SHADER_STAGE(QVK_MOD_FLOW_OCCUPANCY_COMP, VK_SHADER_STAGE_COMPUTE_BIT);
	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &flow.occupancy_pipeline));
	ATTACH_LABEL_VARIABLE(flow.occupancy_pipeline, PIPELINE);

	return VK_SUCCESS;
}

VkResult vkpt_flow_destroy_pipelines(void)
{
	if (flow.lights_pipeline)
		vkDestroyPipeline(qvk.device, flow.lights_pipeline, NULL);
	flow.lights_pipeline = VK_NULL_HANDLE;
	if (flow.occupancy_pipeline)
		vkDestroyPipeline(qvk.device, flow.occupancy_pipeline, NULL);
	flow.occupancy_pipeline = VK_NULL_HANDLE;

	return VK_SUCCESS;
}

void vkpt_flow_register(void)
{
#if USE_FLOW
	R_FlowEmit = vkpt_flow_emit;
#else
	R_FlowEmit = NULL;
#endif
}

void vkpt_flow_reset(void)
{
	flow.num_emitters = 0;
#if USE_FLOW
	flow.have_last_time = false;
	flow.have_bubble_surface = false;
	flow.have_blocks = false;
	flow_free_lights();
	for (int set = 0; set < FLOW_SET_COUNT; set++)
		flow.light_sum_blocks[set] = 0;
	if (flow.grid) {
		flow.force_clear = true;
		flow.idle = false;
	}
#endif
}

void vkpt_flow_set_world(const bsp_mesh_t *wm)
{
#if USE_FLOW
	Z_Free(flow.world_tris);
	flow.world_tris = NULL;
	flow.num_world_tris = 0;
	flow.num_mesh_tris = 0;
	flow.mesh_region_valid = false;

	if (!wm || !wm->primitives)
		return;

	// Only the static, opaque world: glass, water and grates let smoke
	// through, and brush models move.
	const model_geometry_t *geom = &wm->geom_opaque;
	int total = 0;
	for (uint32_t g = 0; g < geom->num_geometries; g++) {
		if (geom->prim_offsets[g] + geom->prim_counts[g] <= wm->num_primitives)
			total += (int)geom->prim_counts[g];
	}
	if (total == 0)
		return;

	flow.world_tris = Z_Malloc(sizeof(float) * 9 * total);
	int n = 0;
	for (uint32_t g = 0; g < geom->num_geometries; g++) {
		if (geom->prim_offsets[g] + geom->prim_counts[g] > wm->num_primitives)
			continue;
		for (uint32_t p = 0; p < geom->prim_counts[g]; p++) {
			const VboPrimitive *prim = wm->primitives + geom->prim_offsets[g] + p;
			float *dst = flow.world_tris + n * 9;
			VectorCopy(prim->pos0, dst + 0);
			VectorCopy(prim->pos1, dst + 3);
			VectorCopy(prim->pos2, dst + 6);
			n++;
		}
	}
	flow.num_world_tris = n;
#else
	(void)wm;
#endif
}

int vkpt_flow_lights(dlight_t *lights, int max_lights)
{
#if USE_FLOW
	if (!flow.grid || max_lights <= 0)
		return 0;
	flow_lights_collect(qvk.frame_counter & 1);
	const int count = min(flow.num_dlights, max_lights);
	memcpy(lights, flow.dlights, sizeof(dlight_t) * count);
	return count;
#else
	(void)lights;
	(void)max_lights;
	return 0;
#endif
}

#if USE_FLOW
// How flow.glsl draws a layer; flow_simulate has said where it is.
static void flow_layer_look(FlowLayerParams *look, int layer, float cell_size)
{
	// Fire and plasma brightness is per 32 units of it looked through.
	const float per_unit = 1.f / 32.f;
	const float scatter = max(cvar_pt_flow_smoke_scatter->value, 0.f);
	float smoke = 0.f;

	look->world_max[3] = FLT_MAX;

	// The glow's colour balance, none but fire's.
	look->emission[1] = look->emission[2] = look->emission[3] = 1.f;

	switch (layer) {
	case R_FLOW_FIRE:
		look->info[2] = 1;
		look->emission[0] = max(cvar_pt_flow_fire_brightness->value, 0.f) * per_unit;
		{
			// Less green and less blue still: the hot heart goes from
			// yellow-white toward yellow and its edges toward orange.
			const float orange = Cvar_ClampValue(cvar_pt_flow_fire_orange, 0.f, 1.f);
			look->emission[2] = 1.f - 0.4f * orange;
			look->emission[3] = 1.f - 0.8f * orange;
		}
		smoke = cvar_pt_flow_smoke_albedo->value * cvar_pt_flow_smoke_brightness->value;
		VectorSet(look->smoke, smoke, smoke, smoke);
		look->smoke[3] = max(cvar_pt_flow_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, scatter, scatter, scatter);
		break;

	case R_FLOW_PLASMA:
		// Green-white heat, then dark green smoke that the green light picks out.
		look->info[2] = 2;
		look->emission[0] = max(cvar_pt_flow_bfg_brightness->value, 0.f) * per_unit;
		smoke = max(cvar_pt_flow_bfg_smoke_brightness->value, 0.f);
		VectorSet(look->smoke, 0.15f * smoke, 0.45f * smoke, 0.12f * smoke);
		look->smoke[3] = max(cvar_pt_flow_bfg_smoke_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, 0.7f * scatter, scatter, 0.6f * scatter);
		break;

	case R_FLOW_BUBBLES:
		// Pale blue-white foam that stops at the water's surface.
		look->info[2] = 0;
		smoke = max(cvar_pt_flow_bubble_brightness->value, 0.f);
		VectorSet(look->smoke, 0.85f * smoke, 0.95f * smoke, smoke);
		look->smoke[3] = max(cvar_pt_flow_bubble_density->value, 0.f) / cell_size;
		VectorSet(look->albedo, 0.9f * scatter, 0.95f * scatter, scatter);
		if (flow.have_bubble_surface)
			look->world_max[3] = flow.bubble_surface;
		break;
	}

	// The burn channel holds the shadow toward this light, numbered from 1.
	look->info[3] = (flow.shadow_pass && flow.layer_shadow[layer] >= 0) ? (uint32_t)flow.layer_shadow[layer] + 1 : 0;
}
#endif

void vkpt_flow_update(const refdef_t *fd, bool render_world)
{
	// Once a frame. timerefresh renders many views between one R_BeginFrame
	// and R_EndFrame, and by the second this frame's set is bound in command
	// buffers already submitted - it must not be rewritten under them.
	if (flow.updated_frame == qvk.frame_counter + 1)
		return;
	flow.updated_frame = qvk.frame_counter + 1;

	const int set = qvk.frame_counter & 1;
	FlowParams *params = (FlowParams *)(flow.params_mapped + set * flow.params_stride);

	VkImageView density_view = flow.dummy_view;
	VkImageLayout density_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkBuffer table = flow.dummy_table.buffer;
	uint32_t blocks = 0;

	memset(params, 0, sizeof(*params));

#if USE_FLOW
	// This set was last read by the frame before last, whose fence
	// R_BeginFrame_RTX has waited for - so its light sums are in, too.
	if (flow.grid) {
		flow_lights_collect(set);
		flow_release_set(set);
	}

	// The player setup menu's model view is not the world; the fire holds
	// still and stays out of it.
	if (flow.grid && render_world && flow_simulate(fd, set, params)) {
		NvFlowVulkanExternalTexture density;
		NvFlowVulkanExternalBuffer sparse_table;
		flow.get_texture(flow.set_density[set], &density);
		flow.get_buffer(flow.set_table[set], &sparse_table);

		if (density.imageView && sparse_table.buffer) {
			const float cell_size = 1.f / max(params->cell_size_inv[0], 1e-6f);

			density_view = (VkImageView)density.imageView;
			density_layout = (VkImageLayout)density.layout;
			table = (VkBuffer)sparse_table.buffer;

			params->block_dim_bits[3] = 1;
			params->limits[0] = (uint32_t)Cvar_ClampInteger(cvar_pt_flow_max_steps, 16, 4096);
			params->limits[1] = 64;
			params->cell_size_inv[3] = max(cvar_pt_flow_step->value, 0.1f) * cell_size;
			params->world_min[3] = params->cell_size_inv[3] * max(cvar_pt_flow_reflect_step->value, 1.f);
			for (int layer = 0; layer < FLOW_LAYERS; layer++)
				flow_layer_look(&params->layers[layer], layer, cell_size);
			flow_lights_params(set, params);
			blocks = params->counts[0];
		}
		else {
			memset(params, 0, sizeof(*params));
		}
	}
	else {
		memset(params, 0, sizeof(*params));
	}
#else
	(void)fd;
	(void)render_world;
#endif

	flow_write_descriptors(set, density_view, density_layout, table);

#if USE_FLOW
	flow_occupancy_dispatch(blocks);
	flow_lights_dispatch(set, blocks);
#else
	(void)blocks;
#endif
}
