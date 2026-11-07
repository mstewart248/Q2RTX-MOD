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
#include "vkpt.h"
#include "vk_util.h"
#include "clusters.h"
#include "shader/vertex_buffer.h"
#include "../../client/client.h"

#include <assert.h>

#define RAY_GEN_ACCEL_STRUCTURE_BINDING_IDX 0
#define RAY_GEN_PARTICLE_COLOR_BUFFER_BINDING_IDX 1
#define RAY_GEN_BEAM_COLOR_BUFFER_BINDING_IDX 2
#define RAY_GEN_SPRITE_INFO_BUFFER_BINDING_IDX 3
#define RAY_GEN_BEAM_INTERSECT_BUFFER_BINDING_IDX 4

#define SIZE_SCRATCH_BUFFER (1 << 25)

static uint32_t shaderGroupHandleSize = 0;
static uint32_t shaderGroupBaseAlignment = 0;
static uint32_t minAccelerationStructureScratchOffsetAlignment = 0;

typedef struct {
	int fast_build;
	uint32_t vertex_count;
	uint32_t index_count;
	uint32_t aabb_count;
	uint32_t instance_count;
} accel_match_info_t;

typedef struct {
	VkAccelerationStructureKHR accel;
	accel_match_info_t match;
	BufferResource_t mem;
	bool present;
} accel_struct_t;

typedef enum {
	PIPELINE_PRIMARY_RAYS,
	// pt_flow_split_pass: primary rays without the Flow march (spec_flow_mode 1),
	// and the march as its own pass (spec_flow_mode 2) - see primary_rays.rgen
	PIPELINE_PRIMARY_RAYS_FLOW_SPLIT,
	PIPELINE_FLOW_PASS,
	PIPELINE_REFLECT_REFRACT_1,
	PIPELINE_REFLECT_REFRACT_2,
	// the same two without the Flow march (spec_flow 0), for frames with no grid
	PIPELINE_REFLECT_REFRACT_1_NOFLOW,
	PIPELINE_REFLECT_REFRACT_2_NOFLOW,
    PIPELINE_DIRECT_LIGHTING,
    PIPELINE_DIRECT_LIGHTING_CAUSTICS,
    PIPELINE_INDIRECT_LIGHTING_FIRST,
    PIPELINE_INDIRECT_LIGHTING_SECOND,
	PIPELINE_SHARC_UPDATE,
	PIPELINE_RESTIR_GI,

	PIPELINE_COUNT
} pipeline_index_t;

       BufferResource_t           buf_accel_scratch;
static size_t                     scratch_buf_ptr = 0;
static BufferResource_t           buf_instances[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_dynamic[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_transparent_models[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_masked_models[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_viewer_models[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_viewer_weapon[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_explosions[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_particles[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_beams[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_sprites[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             blas_blood[MAX_FRAMES_IN_FLIGHT];

static accel_struct_t             tlas_geometry[MAX_FRAMES_IN_FLIGHT];
static accel_struct_t             tlas_effects[MAX_FRAMES_IN_FLIGHT];

static BufferResource_t      buf_shader_binding_table;

static VkDescriptorPool      rt_descriptor_pool;
static VkDescriptorSet       rt_descriptor_set[MAX_FRAMES_IN_FLIGHT];
static VkDescriptorSetLayout rt_descriptor_set_layout;
static VkPipelineLayout      rt_pipeline_layout;
static VkPipeline            rt_pipelines[PIPELINE_COUNT];

cvar_t*                      cvar_pt_enable_particles = NULL;
cvar_t*                      cvar_pt_enable_beams = NULL;
cvar_t*                      cvar_pt_enable_sprites = NULL;
cvar_t*                      cvar_pt_tlas_fast_trace = NULL;
cvar_t*                      cvar_pt_ptlas = NULL;
cvar_t*                      cvar_pt_blas_fast_trace = NULL;

extern cvar_t *cvar_pt_caustics;
extern cvar_t *cvar_pt_reflect_refract;
extern cvar_t* cvar_pt_restir;
extern cvar_t* cvar_pt_sharc_stride;
extern cvar_t* cvar_pt_restir_gi;
extern cvar_t* cvar_pt_restir_gi_spatial;

typedef struct QvkGeometryInstance_s {
	float    transform[12];
	uint32_t instance_id     : 24;
	uint32_t mask            :  8;
	uint32_t instance_offset : 24;
	uint32_t flags           :  8;
	VkDeviceAddress acceleration_structure;
} QvkGeometryInstance_t;

static uint32_t g_num_instances;
static uint32_t g_num_world_instances;     // the static world's, at the list's head

/* THE PARTITIONED TLAS (ptlas.h, pt_ptlas), per frame slot: the geometry TLAS as a
   persistent, partitioned one, updated with what changed since the slot's last
   frame. Its own memory (a descriptor takes it by address); the classic TLAS stays
   for pt_ptlas 0 and for the effects. */
typedef struct {
	ptlas_t             state;
	BufferResource_t    memory;     // the PTLAS
	BufferResource_t    scratch;
	BufferResource_t    stage;      // the op list (host-visible)
	bool                active;     // this frame's geometry TLAS is the PTLAS
} vk_ptlas_t;
static vk_ptlas_t       g_ptlas[MAX_FRAMES_IN_FLIGHT];
static bool             g_ptlas_ready;

/* CLUSTERED DYNAMIC GEOMETRY (clusters.h, pt_clusters), per frame slot: the model
   sections' BLASes built from CLASes in two VK_NV_cluster_acceleration_structure
   operations - the D3D12 renderer's NVAPI path's twin. Only on a device made with
   the feature, which main.c enables while pt_clusters is 1 (CVAR_REFRESH). Each
   buffer grows to fit and is only this slot's, so the slot's fence covers it. */
typedef struct {
	BufferResource_t    clas;       // the CLASes, packed by the driver (implicit destinations)
	BufferResource_t    blas;       // the BLASes, each at the offset the plan gave it
	BufferResource_t    addresses;  // step 1 writes the CLAS addresses, step 2 reads them
	BufferResource_t    stage;      // host-visible: clusters.h's staging
	BufferResource_t    scratch;    // both steps, one after the other
	uint64_t            run_address[PT_CLUSTER_MAX_BLASES];   // this frame's BLAS of each run, 0 = none
	bool                active;     // this frame's model sections are these
} vk_clusters_t;
static vk_clusters_t    g_clusters[MAX_FRAMES_IN_FLIGHT];
static bool             g_clusters_ready;
static VkPhysicalDeviceClusterAccelerationStructurePropertiesNV g_cluster_props;
// the runs: vkpt's model sections, VKPT_DYN_OPAQUE..VKPT_DYN_VIEWER_MODELS. Not the
// explosions, whose hits (pt_logic_explosion) read no geometry index, and not the
// blood: its slots are padded with zeroed triangles, so a cluster straddling a
// droplet's tail and the padding spans the droplet and the world origin, and a
// floor of splats crawled (D3D12, 11-06). Built whole it also keeps
// pt_blood_blas_reuse.
#define CLUSTER_RUNS    (VKPT_DYN_VIEWER_MODELS - VKPT_DYN_OPAQUE + 1)
typedef char cluster_runs_check[(CLUSTER_RUNS <= PT_CLUSTER_MAX_BLASES && VKPT_DYN_OPAQUE == 0) ? 1 : -1];
// clusters.c writes NVAPI's structs, which are Vulkan's
typedef char cluster_vk_args_check[sizeof(pt_cluster_args_t) == sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV) ? 1 : -1];
typedef char cluster_vk_blas_args_check[sizeof(pt_cluster_blas_args_t) == sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV) ? 1 : -1];
static QvkGeometryInstance_t g_instances[MAX_TLAS_INSTANCES];

typedef struct {
	int gpu_index;
	int bounce;
	// TILE COMPACTION (shader/constants.h): list index + 1, 0 = none
	uint32_t tile_read_list;
	uint32_t tile_append_list;
	VkDeviceAddress tile_buffer;
} pt_push_constants_t;

/* TILE COMPACTION, pt_tile_compaction (default 1; 0 = launch every bounce over
   the whole screen as before). See the note in shader/constants.h. Sized for
   the screen images, so it is rebuilt with them. */
static BufferResource_t buf_tiles;
static vkpt_tile_layout_t tiles_layout;  // buf_tiles' (vkpt_pt_tile_layout)
static uint32_t tiles_max_groups;    // maxComputeWorkGroupCount[0]
static bool     tiles_built;         // primary_rays filled the lists this view
static cvar_t  *cvar_pt_tile_compaction;

/* FLOW SPLIT PASS, pt_flow_split_pass (see the note in primary_rays.rgen):
   1 (default) = with Flow's grid in view, the primary pass leaves the march
   to a pass of its own, PROFILER_FLOW_MARCH; 0 = the march inline, as before.
   Measured 11-04 at 3782x2064 with two rockets' smoke on screen: primary
   2.67 ms inline against 0.83 + 1.61..1.71 split, whole GPU frame 14.10 ->
   13.94 ms (all three split arms faster than all three inline ones). Photo
   mode on one frozen explosion: the two differ by no more than two shots of
   the same mode do. The march itself runs at 96 registers there, not 128.
   One GPU only. */
static cvar_t  *cvar_pt_flow_split_pass;
static bool     flow_split_ran;      // last view, for the profiler overlay
static cvar_t  *cvar_pt_merge_barriers;  // MERGED BARRIERS, at BARRIER_COMPUTE

static void
flow_split_pass_changed(cvar_t *self)
{
	vkpt_reset_accumulation();
}

/* WHICH STAGE ACTUALLY CONSUMES AN ACCELERATION STRUCTURE ON THIS DEVICE.

   This was a hard VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, and on a
   RAY QUERY device that is not merely imprecise - it is an INVALID stage bit.
   The validation layer says so on every frame:

     vkCmdPipelineBarrier(): dstStageMask includes
     VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR when the device does not
     have rayTracing feature enabled

   When qvk.use_ray_query is set, this renderer does not create ray tracing
   PIPELINES at all: every trace is a rayQueryEXT inside a COMPUTE shader. So the
   barrier that is supposed to separate "the acceleration structure has just been
   built" from "shaders are about to trace against it" named a stage that cannot
   execute here, and did not name the compute stage that actually reads it.

   A barrier whose destination stage is invalid is undefined behaviour, and the
   plausible driver behaviours - drop the stage, or drop the barrier - both leave
   ray queries free to run against an acceleration structure that is still being
   written. That is a timing-dependent, intermittent, whole-frame fault, and it is
   the leading suspect for fog that vanishes and returns (getSkyVisibility and the
   fog's shadow rays are ray queries; if they answer wrongly the sky term AND the
   light term both go, which matches "it is not just the sky fog, it is ALL the
   fog") and for frame generation warping.

   NOT a blind widening: the compute stage is added because that is where the
   consumers genuinely are, and the ray tracing stage is kept only on devices
   where it is legal, so a real ray-tracing-pipeline device is unchanged. */
#define ACCEL_STRUCT_READ_STAGES \
	(qvk.use_ray_query \
		? (VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT \
		: (VkPipelineStageFlags)(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR \
		                       | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT))

/* THIS BARRIER HAS TWO JOBS, AND IT USED TO DO ONLY ONE.

   The obvious one is publishing: a finished acceleration structure has to
   become visible to whatever traces it, which is the ACCELERATION_STRUCTURE_READ
   half and ACCEL_STRUCT_READ_STAGES above.

   The other one is the SCRATCH BUFFER. Every build in this file takes its
   scratch from one shared allocation through the `scratch_buf_ptr` bump
   allocator, and that pointer is RESET TO ZERO at the top and bottom of
   vkpt_pt_create_all_dynamic and again for the top level. Within one batch the
   bump keeps the regions disjoint, but across a reset the next batch's builds
   write the same scratch bytes the previous batch's builds were still writing.
   Nothing ordered those two against each other, because a barrier whose
   destination is only READ at the TRACE stages says nothing to a later BUILD.

   Synchronisation validation names it exactly:

     vkCmdBuildAccelerationStructuresKHR(): WRITE_AFTER_WRITE hazard detected.
     ... writes to scratch buffer ..., which was previously written by another
     vkCmdBuildAccelerationStructuresKHR command. The current synchronization
     allows VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR accesses at
     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, but to prevent this hazard, it must
     allow VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR accesses at
     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR.

   Two builders sharing scratch corrupt each other's intermediate nodes, and the
   result is a structurally invalid BVH that traverses into nonsense - the
   "acceleration-structure traversal over malformed geometry" case called out in
   the note on vkpt_report_device_lost, which robustBufferAccess does not cover
   and which surfaces as a wild READ_INVALID in whatever pass traces next rather
   than at the build that caused it.

   So the destination side now names the BUILD stage and the WRITE access as
   well. This does not serialise the builds inside a batch - they are already
   separated by the bump allocator and no barrier sits between them - it only
   orders one batch against the next, which is one barrier per frame. */
#define MEM_BARRIER_BUILD_ACCEL(cmd_buf, ...) \
	do { \
		VkMemoryBarrier mem_barrier = {  \
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,  \
			.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR \
						   | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, \
			.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR \
						   | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, \
			__VA_ARGS__  \
		};  \
	 \
		vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, \
				ACCEL_STRUCT_READ_STAGES \
				| VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, \
				&mem_barrier, 0, 0, 0, 0); \
	} while(0)

// vkpt_init_frontend: vkpt_pt_append_dynamic_instances reads them in the
// frame's CPU half, under either backend
void
vkpt_pt_init_cvars(void)
{
	cvar_pt_enable_particles = Cvar_Get("pt_enable_particles", "1", 0);
	cvar_pt_enable_beams = Cvar_Get("pt_enable_beams", "1", 0);
	cvar_pt_enable_sprites= Cvar_Get("pt_enable_sprites", "1", 0);

	// 1 = PREFER_FAST_TRACE, 0 = PREFER_FAST_BUILD (the 2019 default for both).
	// Measured on a 5070 Ti at the base1 spawn: TLAS -0.30 ms, so on. Dynamic BLAS came
	// out slightly slower - they rebuild every frame - so off; don't "fix" the asymmetry.
	// Static world/model BLAS are always FAST_TRACE (vertex_buffer.c). Safe to change
	// live: fast_build is in accel_match_info_t, so the structure gets recreated.
	cvar_pt_tlas_fast_trace = Cvar_Get("pt_tlas_fast_trace", "1", 0);
	cvar_pt_blas_fast_trace = Cvar_Get("pt_blas_fast_trace", "0", 0);

	// THE PARTITIONED TLAS (ptlas.h): 1 = the geometry TLAS is a persistent,
	// partitioned one, updated with only what changed, where the GPU and driver
	// have it (pt_ptlas_available: the backend sets it). An A/B against the
	// classic rebuild-every-frame TLAS, so not archived. Safe to change live.
	cvar_pt_ptlas = Cvar_Get("pt_ptlas", "0", 0);

	// TILE COMPACTION: vkpt_pt_tile_header reads it under either backend
	cvar_pt_tile_compaction = Cvar_Get("pt_tile_compaction", "1", 0);
}

/* the partitioned TLAS's inputs for the shape ptlas.h gives every slot */
static VkPartitionedAccelerationStructureInstancesInputNV
ptlas_inputs(const ptlas_t* p, bool fast_trace)
{
	VkPartitionedAccelerationStructureInstancesInputNV in = {
		.sType = VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV,
		.flags = fast_trace ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
		                    : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
		.instanceCount = p->max_instances,
		.maxInstancePerPartitionCount = PTLAS_INSTANCES_PER_PARTITION,
		.partitionCount = ptlas_partition_count(p),
		.maxInstanceInGlobalPartitionCount = p->max_global,
	};
	return in;
}

// the A/B's layouts: ptlas.c writes NVAPI's, which are Vulkan's
typedef char ptlas_vk_write_check[sizeof(ptlas_write_t) == sizeof(VkPartitionedAccelerationStructureWriteInstanceDataNV) ? 1 : -1];
typedef char ptlas_vk_op_check[sizeof(ptlas_op_t) == sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV) ? 1 : -1];

static void
ptlas_destroy(void)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vk_ptlas_t* s = &g_ptlas[i];
		ptlas_shutdown(&s->state);
		buffer_destroy(&s->memory);
		buffer_destroy(&s->scratch);
		buffer_destroy(&s->stage);
		s->active = false;
	}
	g_ptlas_ready = false;
}

/* With the extension and its feature (qvk.supports_ptlas): every slot's memory,
   sized for either of pt_tlas_fast_trace's builds. pt_ptlas_available says. */
static void
ptlas_create(void)
{
	g_ptlas_ready = false;
	if (qvk.supports_ptlas && qvkGetPartitionedAccelerationStructuresBuildSizesNV &&
	    qvkCmdBuildPartitionedAccelerationStructuresNV)
	{
		ptlas_t shape;
		if (ptlas_init(&shape, MAX_TLAS_INSTANCES, PTLAS_MAX_GLOBAL))
		{
			VkDeviceSize result = 0, scratch = 0;
			for (int fast = 0; fast < 2; fast++) {
				VkPartitionedAccelerationStructureInstancesInputNV in = ptlas_inputs(&shape, fast);
				VkAccelerationStructureBuildSizesInfoKHR sizes = {
					.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
				};
				qvkGetPartitionedAccelerationStructuresBuildSizesNV(qvk.device, &in, &sizes);
				result = max(result, sizes.accelerationStructureSize);
				scratch = max(scratch, max(sizes.buildScratchSize, sizes.updateScratchSize));
			}
			ptlas_shutdown(&shape);

			bool ok = result > 0;
			for (int i = 0; ok && i < MAX_FRAMES_IN_FLIGHT; i++) {
				vk_ptlas_t* s = &g_ptlas[i];
				ok = ptlas_init(&s->state, MAX_TLAS_INSTANCES, PTLAS_MAX_GLOBAL)
					&& buffer_create(&s->memory, result,
						VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_SUCCESS
					&& buffer_create(&s->scratch, max(scratch, 256),
						VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_SUCCESS
					&& buffer_create(&s->stage, ptlas_stage_size(MAX_TLAS_INSTANCES),
						VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
						VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == VK_SUCCESS;
			}
			if (ok) {
				g_ptlas_ready = true;
				Com_Printf("Partitioned TLAS available (VK_NV_partitioned_acceleration_structure; pt_ptlas 1; %llu KB)\n",
					(unsigned long long)(result >> 10));
			} else {
				Com_WPrintf("The partitioned TLAS could not be made; pt_ptlas is off\n");
				ptlas_destroy();
			}
		}
	}
	Cvar_FullSet("pt_ptlas_available", g_ptlas_ready ? "1" : "0", CVAR_ROM, FROM_CODE);
}

static void
clusters_destroy(void)
{
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vk_clusters_t* c = &g_clusters[i];
		buffer_destroy(&c->clas);
		buffer_destroy(&c->blas);
		buffer_destroy(&c->addresses);
		buffer_destroy(&c->stage);
		buffer_destroy(&c->scratch);
		memset(c, 0, sizeof(*c));
	}
	g_clusters_ready = false;
}

/* On a device made with the feature: the limits the plan needs. The buffers come
   with the first frame that has something to build. pt_clusters_available says
   whether the GPU has it at all (the menu row), whatever pt_clusters holds. */
static void
clusters_create(void)
{
	g_clusters_ready = false;
	if (qvk.clusters_enabled && qvkGetClusterAccelerationStructureBuildSizesNV &&
	    qvkCmdBuildClusterAccelerationStructureIndirectNV)
	{
		g_cluster_props = (VkPhysicalDeviceClusterAccelerationStructurePropertiesNV){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV,
		};
		VkPhysicalDeviceProperties2 props = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &g_cluster_props,
		};
		vkGetPhysicalDeviceProperties2(qvk.physical_device, &props);
		if (g_cluster_props.maxTrianglesPerCluster >= PT_CLUSTER_TRIANGLES &&
		    g_cluster_props.maxVerticesPerCluster >= PT_CLUSTER_VERTICES)
		{
			g_clusters_ready = true;
			Com_Printf("Clustered dynamic BLASes (VK_NV_cluster_acceleration_structure; pt_clusters 1, %d triangles a cluster)\n",
				PT_CLUSTER_TRIANGLES);
		}
		else
			Com_WPrintf("pt_clusters 1, but clusters here hold %u triangles / %u vertices (%d / %d needed) - the dynamic BLASes are built whole\n",
				g_cluster_props.maxTrianglesPerCluster, g_cluster_props.maxVerticesPerCluster,
				PT_CLUSTER_TRIANGLES, PT_CLUSTER_VERTICES);
	}
	else if (!qvk.clusters_enabled && Cvar_Get("pt_clusters", "0", CVAR_REFRESH)->integer)
		Com_WPrintf("pt_clusters 1, but this GPU or driver has no VK_NV_cluster_acceleration_structure - the dynamic BLASes are built whole\n");
	Cvar_FullSet("pt_clusters_available", qvk.supports_clusters ? "1" : "0", CVAR_ROM, FROM_CODE);
}

// half as much again, in whole megabytes, as accel.c's grown (D3D12)
static bool
cluster_buffer(BufferResource_t* b, VkDeviceSize needed, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory)
{
	if (b->buffer && b->size >= needed)
		return true;
	buffer_destroy(b);
	const VkDeviceSize mb = 1ull << 20;
	const VkDeviceSize size = (needed + needed / 2 + mb - 1) & ~(mb - 1);
	if (buffer_create(b, size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, memory) != VK_SUCCESS) {
		memset(b, 0, sizeof(*b));
		return false;
	}
	return true;
}

/* This slot's clustered BLASes: planned (clusters.c), sized, staged and both steps
   recorded. False: not this frame (the driver gave no sizes, or no memory) - the
   caller builds the model sections whole. */
static bool
clusters_build(VkCommandBuffer cmd_buf, int idx, const EntityUploadInfo* upload_info, bool fast_build)
{
	vk_clusters_t* c = &g_clusters[idx];
	c->active = false;
	memset(c->run_address, 0, sizeof(c->run_address));

	const uint32_t first[CLUSTER_RUNS] = {
		[VKPT_DYN_OPAQUE]        = 0,
		[VKPT_DYN_TRANSPARENT]   = upload_info->transparent_prim_offset,
		[VKPT_DYN_MASKED]        = upload_info->masked_prim_offset,
		[VKPT_DYN_VIEWER_WEAPON] = upload_info->viewer_weapon_prim_offset,
		[VKPT_DYN_VIEWER_MODELS] = upload_info->viewer_model_prim_offset,
	};
	const uint32_t prims[CLUSTER_RUNS] = {
		[VKPT_DYN_OPAQUE]        = upload_info->opaque_prim_count,
		[VKPT_DYN_TRANSPARENT]   = upload_info->transparent_prim_count,
		[VKPT_DYN_MASKED]        = upload_info->masked_prim_count,
		[VKPT_DYN_VIEWER_WEAPON] = upload_info->viewer_weapon_prim_count,
		[VKPT_DYN_VIEWER_MODELS] = upload_info->viewer_model_prim_count,
	};
	pt_cluster_plan_t plan;
	if (!pt_cluster_plan(&plan, prims, CLUSTER_RUNS))
		return false;
	if (!plan.num_blases) {
		c->active = true;   // nothing to build: every run's address 0
		return true;
	}

	const VkBuildAccelerationStructureFlagsKHR flags = fast_build
		? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
		: VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	VkClusterAccelerationStructureTriangleClusterInputNV triangles = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV,
		.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
		.maxGeometryIndexValue = plan.max_clusters_per_blas - 1,
		.maxClusterUniqueGeometryCount = 1,
		.maxClusterTriangleCount = PT_CLUSTER_TRIANGLES,
		.maxClusterVertexCount = PT_CLUSTER_VERTICES,
		.maxTotalTriangleCount = plan.num_triangles,
		.maxTotalVertexCount = 3 * plan.num_triangles,
	};
	const VkClusterAccelerationStructureInputInfoNV clas_input = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
		.maxAccelerationStructureCount = plan.num_clusters,
		.flags = flags,
		.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_TRIANGLE_CLUSTER_NV,
		.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_IMPLICIT_DESTINATIONS_NV,
		.opInput.pTriangleClusters = &triangles,
	};
	VkClusterAccelerationStructureClustersBottomLevelInputNV bottom = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV,
		.maxTotalClusterCount = plan.num_clusters,
		.maxClusterCountPerAccelerationStructure = plan.max_clusters_per_blas,
	};
	VkClusterAccelerationStructureInputInfoNV blas_input = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,
		.maxAccelerationStructureCount = plan.num_blases,
		.flags = flags,
		.opType = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_TYPE_BUILD_CLUSTERS_BOTTOM_LEVEL_NV,
		.opMode = VK_CLUSTER_ACCELERATION_STRUCTURE_OP_MODE_EXPLICIT_DESTINATIONS_NV,
		.opInput.pClustersBottomLevel = &bottom,
	};
	VkAccelerationStructureBuildSizesInfoKHR clas_sizes = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	VkAccelerationStructureBuildSizesInfoKHR blas_sizes = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetClusterAccelerationStructureBuildSizesNV(qvk.device, &clas_input, &clas_sizes);
	qvkGetClusterAccelerationStructureBuildSizesNV(qvk.device, &blas_input, &blas_sizes);
	if (!clas_sizes.accelerationStructureSize || !blas_sizes.accelerationStructureSize)
		return false;

	// each BLAS's own: the explicit size is the most one of these inputs makes
	VkDeviceSize blas_offset[CLUSTER_RUNS] = { 0 }, blas_total = 0;
	const VkDeviceSize blas_align = max(g_cluster_props.clusterBottomLevelByteAlignment, 256u);
	for (int r = 0; r < CLUSTER_RUNS; r++) {
		if (plan.blas_slot[r] < 0)
			continue;
		VkClusterAccelerationStructureClustersBottomLevelInputNV one = bottom;
		one.maxTotalClusterCount = one.maxClusterCountPerAccelerationStructure = plan.clusters[r];
		VkClusterAccelerationStructureInputInfoNV in = blas_input;
		in.maxAccelerationStructureCount = 1;
		in.opInput.pClustersBottomLevel = &one;
		VkAccelerationStructureBuildSizesInfoKHR s = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		qvkGetClusterAccelerationStructureBuildSizesNV(qvk.device, &in, &s);
		if (!s.accelerationStructureSize)
			return false;
		blas_offset[r] = align(blas_total, blas_align);
		blas_total = blas_offset[r] + s.accelerationStructureSize;
	}

	pt_cluster_layout_t layout;
	pt_cluster_layout(&plan, &layout);
	const VkBufferUsageFlags as_storage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	const VkBufferUsageFlags build_input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (!cluster_buffer(&c->clas, clas_sizes.accelerationStructureSize, as_storage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
	    !cluster_buffer(&c->blas, blas_total, as_storage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
	    !cluster_buffer(&c->addresses, (VkDeviceSize)plan.num_clusters * sizeof(uint64_t), build_input, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
	    !cluster_buffer(&c->stage, layout.size, build_input, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
	    !cluster_buffer(&c->scratch, max(max(clas_sizes.buildScratchSize, blas_sizes.buildScratchSize), 256),
	                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
		return false;

	uint64_t vertices[CLUSTER_RUNS];
	for (int r = 0; r < CLUSTER_RUNS; r++) {
		vertices[r] = qvk.buf_positions_instanced.address + (uint64_t)first[r] * sizeof(prim_positions_t);
		if (plan.blas_slot[r] >= 0)
			c->run_address[r] = c->blas.address + blas_offset[r];
	}
	pt_cluster_stage(&plan, &layout, buffer_map(&c->stage), c->stage.address, vertices, c->run_address,
		c->addresses.address, sizeof(uint64_t));
	buffer_unmap(&c->stage);

	// 1: every cluster's CLAS, packed into this slot's CLAS memory, their addresses written
	const VkClusterAccelerationStructureCommandsInfoNV step1 = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
		.input = clas_input,
		.dstImplicitData = c->clas.address,
		.scratchData = c->scratch.address,
		.dstAddressesArray = { c->addresses.address, sizeof(uint64_t), (VkDeviceSize)plan.num_clusters * sizeof(uint64_t) },
		.srcInfosArray = { c->stage.address + layout.triangle_args, sizeof(pt_cluster_args_t),
		                   (VkDeviceSize)plan.num_clusters * sizeof(pt_cluster_args_t) },
	};
	qvkCmdBuildClusterAccelerationStructureIndirectNV(cmd_buf, &step1);

	// the CLASes and their addresses written and the scratch free again
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	};
	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);

	// 2: each run's BLAS from its slice of those addresses, where the plan put it
	const VkClusterAccelerationStructureCommandsInfoNV step2 = {
		.sType = VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,
		.input = blas_input,
		.scratchData = c->scratch.address,
		.dstAddressesArray = { c->stage.address + layout.blas_addresses, sizeof(uint64_t),
		                       (VkDeviceSize)plan.num_blases * sizeof(uint64_t) },
		.srcInfosArray = { c->stage.address + layout.blas_args, sizeof(pt_cluster_blas_args_t),
		                   (VkDeviceSize)plan.num_blases * sizeof(pt_cluster_blas_args_t) },
	};
	qvkCmdBuildClusterAccelerationStructureIndirectNV(cmd_buf, &step2);

	c->active = true;
	return true;
}

VkResult
vkpt_pt_init()
{
	VkPhysicalDeviceAccelerationStructurePropertiesKHR accel_struct_properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
		.pNext = NULL
	};

	VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_pipeline_properties = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
		.pNext = &accel_struct_properties
	};

	VkPhysicalDeviceProperties2 dev_props2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &ray_pipeline_properties,
	};

	vkGetPhysicalDeviceProperties2(qvk.physical_device, &dev_props2);

	shaderGroupBaseAlignment = ray_pipeline_properties.shaderGroupBaseAlignment;
	tiles_max_groups = dev_props2.properties.limits.maxComputeWorkGroupCount[0];
	cvar_pt_flow_split_pass = Cvar_Get("pt_flow_split_pass", "1", 0);
	// so that a photo-mode A/B of the two starts a fresh image
	cvar_pt_flow_split_pass->changed = flow_split_pass_changed;
	cvar_pt_merge_barriers = Cvar_Get("pt_merge_barriers", "0", 0);
	shaderGroupHandleSize = ray_pipeline_properties.shaderGroupHandleSize;
	minAccelerationStructureScratchOffsetAlignment = accel_struct_properties.minAccelerationStructureScratchOffsetAlignment;

	buffer_create(&buf_accel_scratch, SIZE_SCRATCH_BUFFER, 
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		buffer_create(buf_instances + i, MAX_TLAS_INSTANCES * sizeof(QvkGeometryInstance_t), 
			VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	}

	ptlas_create();
	clusters_create();

	/* create descriptor set layout */
	VkDescriptorSetLayoutBinding bindings[] = {
		{
			.binding         = RAY_GEN_ACCEL_STRUCTURE_BINDING_IDX,
			.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
			.descriptorCount = TLAS_COUNT,
			.stageFlags      = qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_RAYGEN_BIT_KHR,
		},
		{
			.binding         = RAY_GEN_PARTICLE_COLOR_BUFFER_BINDING_IDX,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.descriptorCount = 1,
			.stageFlags      = qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
		},
		{
			.binding         = RAY_GEN_BEAM_COLOR_BUFFER_BINDING_IDX,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.descriptorCount = 1,
			.stageFlags      = qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
		},
		{
			.binding         = RAY_GEN_SPRITE_INFO_BUFFER_BINDING_IDX,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.descriptorCount = 1,
			.stageFlags      = qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
		},
		{
			.binding         = RAY_GEN_BEAM_INTERSECT_BUFFER_BINDING_IDX,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.descriptorCount = 1,
			.stageFlags      = qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_INTERSECTION_BIT_KHR,
		},
	};

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(bindings),
		.pBindings    = bindings
	};
	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &rt_descriptor_set_layout));
	ATTACH_LABEL_VARIABLE(rt_descriptor_set_layout, DESCRIPTOR_SET_LAYOUT);


	VkDescriptorSetLayout desc_set_layouts[] = {
		rt_descriptor_set_layout,
		qvk.desc_set_layout_ubo,
		qvk.desc_set_layout_textures,
		qvk.desc_set_layout_vertex_buffer
	};

	/* create pipeline */
	VkPushConstantRange push_constant_range = {
		.stageFlags		= qvk.use_ray_query ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_RAYGEN_BIT_KHR,
		.offset			= 0,
		.size			= sizeof(pt_push_constants_t),
	};

	VkPipelineLayoutCreateInfo pipeline_layout_create_info = {
		.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts    = desc_set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range,
	};

	_VK(vkCreatePipelineLayout(qvk.device, &pipeline_layout_create_info, NULL, &rt_pipeline_layout));
	ATTACH_LABEL_VARIABLE(rt_pipeline_layout, PIPELINE_LAYOUT);

	VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, MAX_FRAMES_IN_FLIGHT * LENGTH(bindings) },
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT * TLAS_COUNT }
	};

	VkDescriptorPoolCreateInfo pool_create_info = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets       = MAX_FRAMES_IN_FLIGHT,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes    = pool_sizes
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_create_info, NULL, &rt_descriptor_pool));
	ATTACH_LABEL_VARIABLE(rt_descriptor_pool, DESCRIPTOR_POOL);

	VkDescriptorSetAllocateInfo descriptor_set_alloc_info = {
		.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool     = rt_descriptor_pool,
		.descriptorSetCount = 1,
		.pSetLayouts        = &rt_descriptor_set_layout,
	};

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		_VK(vkAllocateDescriptorSets(qvk.device, &descriptor_set_alloc_info, rt_descriptor_set + i));
		ATTACH_LABEL_VARIABLE(rt_descriptor_set[i], DESCRIPTOR_SET);
	}

	vkpt_pt_init_cvars();

	return VK_SUCCESS;
}

/*
=================
vkpt_pt_get_geometry_tlas

The opaque-geometry TLAS for a frame. Exposed so the volumetric fog pass can
trace its own sky-visibility rays: it is a COMPUTE shader, and the ray tracing
descriptor set above declares its bindings with raygen/any-hit stage flags
unless qvk.use_ray_query is set, so the fog binds this into its own descriptor
set rather than borrowing that one.
=================
*/
VkAccelerationStructureKHR
vkpt_pt_get_geometry_tlas(int idx)
{
	return tlas_geometry[idx].accel;
}

const void *
vkpt_pt_geometry_tlas_write(int idx, vkpt_tlas_write_t *w)
{
	memset(w, 0, sizeof(*w));
	if (g_ptlas_ready && g_ptlas[idx].active)
	{
		w->address = g_ptlas[idx].memory.address;
		w->nv.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_PARTITIONED_ACCELERATION_STRUCTURE_NV;
		w->nv.accelerationStructureCount = 1;
		w->nv.pAccelerationStructures = &w->address;
		return &w->nv;
	}
	w->handle = tlas_geometry[idx].accel;
	if (w->handle == VK_NULL_HANDLE)
		return NULL;
	w->khr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	w->khr.accelerationStructureCount = 1;
	w->khr.pAccelerationStructures = &w->handle;
	return &w->khr;
}

VkResult
vkpt_pt_update_descripter_set_bindings(int idx)
{
	/* update descriptor set bindings */
	const VkAccelerationStructureKHR tlas[TLAS_COUNT] = {
		[TLAS_INDEX_GEOMETRY] = tlas_geometry[idx].accel,
		[TLAS_INDEX_EFFECTS] = tlas_effects[idx].accel
	};
	
	VkWriteDescriptorSetAccelerationStructureKHR desc_accel_struct = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
		.accelerationStructureCount = TLAS_COUNT,
		.pAccelerationStructures = tlas
	};

	/* pt_ptlas: the geometry TLAS is the partitioned one, which a descriptor takes by
	   address - so it and the effects TLAS go in two writes */
	if (g_ptlas_ready && g_ptlas[idx].active)
	{
		vkpt_tlas_write_t geometry;
		VkWriteDescriptorSetAccelerationStructureKHR effects = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = 1,
			.pAccelerationStructures = &tlas[TLAS_INDEX_EFFECTS]
		};
		VkWriteDescriptorSet as_writes[2] = {
			{
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext           = vkpt_pt_geometry_tlas_write(idx, &geometry),
				.dstSet          = rt_descriptor_set[idx],
				.dstBinding      = RAY_GEN_ACCEL_STRUCTURE_BINDING_IDX,
				.dstArrayElement = TLAS_INDEX_GEOMETRY,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
			},
			{
				.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext           = &effects,
				.dstSet          = rt_descriptor_set[idx],
				.dstBinding      = RAY_GEN_ACCEL_STRUCTURE_BINDING_IDX,
				.dstArrayElement = TLAS_INDEX_EFFECTS,
				.descriptorCount = 1,
				.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
			},
		};
		vkUpdateDescriptorSets(qvk.device, LENGTH(as_writes), as_writes, 0, NULL);
	}
	
	VkBufferView particle_color_buffer_view = get_transparency_particle_color_buffer_view();
	VkBufferView beam_color_buffer_view = get_transparency_beam_color_buffer_view();
	VkBufferView sprite_info_buffer_view = get_transparency_sprite_info_buffer_view();
	VkBufferView beam_intersect_buffer_view = get_transparency_beam_intersect_buffer_view();

	VkWriteDescriptorSet writes[] = {
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext           = (const void*)&desc_accel_struct,
			.dstSet          = rt_descriptor_set[idx],
			.dstBinding      = RAY_GEN_ACCEL_STRUCTURE_BINDING_IDX,
			.descriptorCount = TLAS_COUNT,
			.descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
		},
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = rt_descriptor_set[idx],
			.dstBinding      = RAY_GEN_PARTICLE_COLOR_BUFFER_BINDING_IDX,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.pTexelBufferView = &particle_color_buffer_view
		},
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = rt_descriptor_set[idx],
			.dstBinding      = RAY_GEN_BEAM_COLOR_BUFFER_BINDING_IDX,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.pTexelBufferView = &beam_color_buffer_view
		},
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = rt_descriptor_set[idx],
			.dstBinding      = RAY_GEN_SPRITE_INFO_BUFFER_BINDING_IDX,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.pTexelBufferView = &sprite_info_buffer_view
		},
		{
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = rt_descriptor_set[idx],
			.dstBinding      = RAY_GEN_BEAM_INTERSECT_BUFFER_BINDING_IDX,
			.descriptorCount = 1,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
			.pTexelBufferView = &beam_intersect_buffer_view
		},
	};

	// with the PTLAS both TLASes were written above: writes[0] (the pair) is skipped
	const int skip = (g_ptlas_ready && g_ptlas[idx].active) ? 1 : 0;
	vkUpdateDescriptorSets(qvk.device, LENGTH(writes) - skip, writes + skip, 0, NULL);

	return VK_SUCCESS;
}

static void destroy_accel_struct(accel_struct_t* blas)
{
	buffer_destroy(&blas->mem);

	if (blas->accel)
	{
		qvkDestroyAccelerationStructureKHR(qvk.device, blas->accel, NULL);
		blas->accel = VK_NULL_HANDLE;
	}

	// it has nothing to name now. Left true, a renderer restart into pt_clusters 1 -
	// which skips the model sections' whole builds, the only thing that reset it -
	// handed this NULL handle to vkGetAccelerationStructureDeviceAddressKHR on the
	// first frame and crashed in the driver (11-06).
	blas->present = false;

	blas->match.fast_build = 0;
	blas->match.index_count = 0;
	blas->match.vertex_count = 0;
	blas->match.aabb_count = 0;
	blas->match.instance_count = 0;
}

static void vkpt_pt_destroy_dynamic(int idx)
{
	destroy_accel_struct(&blas_dynamic[idx]);
	destroy_accel_struct(&blas_transparent_models[idx]);
	destroy_accel_struct(&blas_masked_models[idx]);
	destroy_accel_struct(&blas_viewer_models[idx]);
	destroy_accel_struct(&blas_viewer_weapon[idx]);
	destroy_accel_struct(&blas_explosions[idx]);
	destroy_accel_struct(&blas_particles[idx]);
	destroy_accel_struct(&blas_beams[idx]);
	destroy_accel_struct(&blas_sprites[idx]);
	destroy_accel_struct(&blas_blood[idx]);
}

/* The assert this replaces compiled away in release, which is the only build a
   device loss has ever been reported from. Past the end of buf_accel_scratch
   a build scribbles over whatever happens to be mapped next and faults on the
   first unmapped page - so say so, once per frame, before that happens. */
static void check_scratch_overflow(VkDeviceSize build_scratch_size, const char *site)
{
	static uint64_t last_frame = ~0ull;

	/* the buffer grows (ensure_scratch_capacity), so compare against its size
	   now, not the size it was created with */
	if (scratch_buf_ptr <= buf_accel_scratch.size || last_frame == qvk.frame_counter)
		return;

	last_frame = qvk.frame_counter;
	Com_EPrintf("%s: acceleration structure build scratch overflow - this build needs %llu bytes "
		"and ends at %zu, past the %zu byte scratch buffer (frame %llu)\n", site,
		(unsigned long long)build_scratch_size, scratch_buf_ptr, buf_accel_scratch.size,
		(unsigned long long)qvk.frame_counter);
}

static inline int accel_matches(accel_match_info_t *match,
								int fast_build,
								uint32_t vertex_count,
								uint32_t index_count) {
	return match->fast_build == fast_build &&
		   match->vertex_count >= vertex_count &&
		   match->index_count >= index_count;
}

static inline int accel_matches_aabb(accel_match_info_t *match,
								int fast_build,
								uint32_t aabb_count) {
	return match->fast_build == fast_build &&
		   match->aabb_count >= aabb_count;
}

static inline int accel_matches_top_level(accel_match_info_t *match,
								int fast_build,
								uint32_t instance_count) {
	return match->fast_build == fast_build &&
		   match->instance_count >= instance_count;
}

// How much to bloat the dynamic geometry allocations
// to try to avoid later allocations.
#define DYNAMIC_GEOMETRY_BLOAT_FACTOR 2

/* The dynamic bottom-level builds are queued here and recorded with a single
   vkCmdBuildAccelerationStructuresKHR (upstream dd352e77). Recording them back
   to back is slower: some drivers barrier implicitly between separate build
   commands. Each build already has its own scratch range, so batching is legal. */
#define MAX_BATCH_ACCEL_BUILDS 16

typedef struct {
	uint32_t numBuilds;
	VkAccelerationStructureGeometryKHR geometries[MAX_BATCH_ACCEL_BUILDS];
	VkAccelerationStructureBuildGeometryInfoKHR buildInfos[MAX_BATCH_ACCEL_BUILDS];
	VkAccelerationStructureBuildRangeInfoKHR rangeInfos[MAX_BATCH_ACCEL_BUILDS];
	const VkAccelerationStructureBuildRangeInfoKHR *rangeInfoPtrs[MAX_BATCH_ACCEL_BUILDS];
} accel_build_batch_t;

static void
accel_batch_add(accel_build_batch_t *batch,
	const VkAccelerationStructureGeometryKHR *geometry,
	const VkAccelerationStructureBuildGeometryInfoKHR *build_info,
	uint32_t primitive_count)
{
	assert(batch->numBuilds < MAX_BATCH_ACCEL_BUILDS);
	uint32_t i = batch->numBuilds++;

	batch->geometries[i] = *geometry;
	batch->buildInfos[i] = *build_info;
	batch->buildInfos[i].pGeometries = &batch->geometries[i];
	batch->rangeInfos[i] = (VkAccelerationStructureBuildRangeInfoKHR){ .primitiveCount = primitive_count };
	batch->rangeInfoPtrs[i] = &batch->rangeInfos[i];
}

static void
accel_batch_flush(VkCommandBuffer cmd_buf, accel_build_batch_t *batch)
{
	if (batch->numBuilds > 0)
		qvkCmdBuildAccelerationStructuresKHR(cmd_buf, batch->numBuilds, batch->buildInfos, batch->rangeInfoPtrs);
	batch->numBuilds = 0;
}


static void
vkpt_pt_create_accel_bottom(
	accel_build_batch_t *batch,
	BufferResource_t* buffer_vertex,
	VkDeviceAddress offset_vertex,
	BufferResource_t* buffer_index,
	VkDeviceAddress offset_index,
	uint32_t num_vertices,
	uint32_t num_indices,
	accel_struct_t* blas,
	bool is_dynamic,
	bool fast_build)
{
	assert(blas);

	if (num_vertices == 0)
	{
		blas->present = false;
		return;
	}
	
	assert(buffer_vertex->address);
	if (buffer_index) assert(buffer_index->address);

	const VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
		.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
		.vertexData = {.deviceAddress = buffer_vertex->address + offset_vertex },
		.vertexStride = sizeof(float) * 3,
		.maxVertex = max(num_vertices, 1) - 1,
		.indexData = {.deviceAddress = buffer_index ? (buffer_index->address + offset_index) : 0 },
		.indexType = buffer_index ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_NONE_KHR,
	};

	const VkAccelerationStructureGeometryDataKHR geometry_data = { 
		.triangles = triangles
	};

	const VkAccelerationStructureGeometryKHR geometry = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
		.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
		.geometry = geometry_data
	};

	const VkAccelerationStructureGeometryKHR* geometries = &geometry;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo;

	// Prepare build info now, acceleration is filled later
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.pNext = NULL;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;
	buildInfo.dstAccelerationStructure = VK_NULL_HANDLE;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = geometries;
	buildInfo.ppGeometries = NULL;

	int doFree = 0;
	int doAlloc = 0;

	if (!is_dynamic || !accel_matches(&blas->match, fast_build, num_vertices, num_indices) || blas->accel == VK_NULL_HANDLE)
	{
		doAlloc = 1;
		doFree = (blas->accel != VK_NULL_HANDLE);
	}

	if (doFree)
	{
		destroy_accel_struct(blas);
	}

	// Find size to build on the device
	uint32_t max_primitive_count = max(num_vertices, num_indices) / 3; // number of tris
	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &max_primitive_count, &sizeInfo);

	/* Scratch is needed for what is BUILT, not for what is allocated. The
	   re-query below for a bloated dynamic allocation used to size the scratch
	   reservation too - twice the scratch this build uses, which is what first
	   ran the shared buffer past its end. */
	const VkDeviceSize scratch_size = sizeInfo.buildScratchSize;

	if (doAlloc)
	{
		uint32_t num_vertices_to_allocate = num_vertices;
		uint32_t num_indices_to_allocate = num_indices;

		// Allocate more memory / larger BLAS for dynamic objects
		if (is_dynamic)
		{
			num_vertices_to_allocate *= DYNAMIC_GEOMETRY_BLOAT_FACTOR;
			num_indices_to_allocate *= DYNAMIC_GEOMETRY_BLOAT_FACTOR;

			max_primitive_count = max(num_vertices_to_allocate, num_indices_to_allocate) / 3;
			qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &max_primitive_count, &sizeInfo);
		}

		// Create acceleration structure
		VkAccelerationStructureCreateInfoKHR createInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			.size = sizeInfo.accelerationStructureSize
		};

		// Create the buffer for the acceleration structure
		buffer_create(&blas->mem, sizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		createInfo.buffer = blas->mem.buffer;

		// Create the acceleration structure
		qvkCreateAccelerationStructureKHR(qvk.device, &createInfo, NULL, &blas->accel);

		blas->match.fast_build = fast_build;
		blas->match.vertex_count = num_vertices_to_allocate;
		blas->match.index_count = num_indices_to_allocate;
		blas->match.aabb_count = 0;
		blas->match.instance_count = 0;
	}

	// set where the build lands
	buildInfo.dstAccelerationStructure = blas->accel;

	// Use shared scratch buffer for holding the temporary data of the acceleration structure builder
	buildInfo.scratchData.deviceAddress = buf_accel_scratch.address + scratch_buf_ptr;
	assert(buf_accel_scratch.address);

	// Update the scratch buffer ptr
	scratch_buf_ptr += scratch_size;
	scratch_buf_ptr = align(scratch_buf_ptr, minAccelerationStructureScratchOffsetAlignment);
	check_scratch_overflow(scratch_size, __func__);

	accel_batch_add(batch, &geometry, &buildInfo, max(num_vertices, num_indices) / 3);

	blas->present = true;
}

static void
vkpt_pt_create_accel_bottom_aabb(
	accel_build_batch_t *batch,
	BufferResource_t* buffer_aabb,
	VkDeviceAddress offset_aabb,
	int num_aabbs,
	accel_struct_t* blas,
	bool is_dynamic,
	bool fast_build)
{
	assert(blas);

	if (num_aabbs == 0)
	{
		blas->present = false;
		return;
	}

	assert(buffer_aabb->address);

	const VkAccelerationStructureGeometryAabbsDataKHR aabbs = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
		.data = {.deviceAddress = buffer_aabb->address + offset_aabb },
		.stride = sizeof(VkAabbPositionsKHR)
	};

	const VkAccelerationStructureGeometryDataKHR geometry_data = { 
		.aabbs = aabbs
	};

	const VkAccelerationStructureGeometryKHR geometry = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
		.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
		.geometry = geometry_data
	};

	const VkAccelerationStructureGeometryKHR* geometries = &geometry;

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo;

	// Prepare build info now, acceleration is filled later
	buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	buildInfo.pNext = NULL;
	buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	buildInfo.flags = fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	buildInfo.srcAccelerationStructure = VK_NULL_HANDLE;
	buildInfo.dstAccelerationStructure = VK_NULL_HANDLE;
	buildInfo.geometryCount = 1;
	buildInfo.pGeometries = geometries;
	buildInfo.ppGeometries = NULL;

	int doFree = 0;
	int doAlloc = 0;

	if (!is_dynamic || !accel_matches_aabb(&blas->match, fast_build, num_aabbs) || blas->accel == VK_NULL_HANDLE)
	{
		doAlloc = 1;
		doFree = (blas->accel != VK_NULL_HANDLE);
	}

	if (doFree)
	{
		destroy_accel_struct(blas);
	}

	// Find size to build on the device
	uint32_t max_primitive_count = num_aabbs;
	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &max_primitive_count, &sizeInfo);

	/* Scratch is needed for what is BUILT, not for what is allocated. The
	   re-query below for a bloated dynamic allocation used to size the scratch
	   reservation too - twice the scratch this build uses, which is what first
	   ran the shared buffer past its end. */
	const VkDeviceSize scratch_size = sizeInfo.buildScratchSize;

	if (doAlloc)
	{
		int num_aabs_to_allocate = num_aabbs;

		// Allocate more memory / larger BLAS for dynamic objects
		if (is_dynamic)
		{
			num_aabs_to_allocate *= DYNAMIC_GEOMETRY_BLOAT_FACTOR;

			max_primitive_count = num_aabs_to_allocate;
			qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &max_primitive_count, &sizeInfo);
		}

		// Create acceleration structure
		VkAccelerationStructureCreateInfoKHR createInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
			.size = sizeInfo.accelerationStructureSize
		};

		// Create the buffer for the acceleration structure
		buffer_create(&blas->mem, sizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		createInfo.buffer = blas->mem.buffer;

		// Create the acceleration structure
		qvkCreateAccelerationStructureKHR(qvk.device, &createInfo, NULL, &blas->accel);

		blas->match.fast_build = fast_build;
		blas->match.vertex_count = 0;
		blas->match.index_count = 0;
		blas->match.aabb_count = num_aabs_to_allocate;
		blas->match.instance_count = 0;
	}

	// set where the build lands
	buildInfo.dstAccelerationStructure = blas->accel;

	// Use shared scratch buffer for holding the temporary data of the acceleration structure builder
	buildInfo.scratchData.deviceAddress = buf_accel_scratch.address + scratch_buf_ptr;
	assert(buf_accel_scratch.address);

	// Update the scratch buffer ptr
	scratch_buf_ptr += scratch_size;
	scratch_buf_ptr = align(scratch_buf_ptr, minAccelerationStructureScratchOffsetAlignment);
	check_scratch_overflow(scratch_size, __func__);

	accel_batch_add(batch, &geometry, &buildInfo, num_aabbs);

	blas->present = true;
}

/* The scratch one bottom-level build of this shape needs - the same query the
   builders make, so the sum below is exact rather than an estimate. */
static VkDeviceSize scratch_for_triangles(uint32_t num_vertices, uint32_t num_indices, bool indexed, bool fast_build)
{
	if (num_vertices == 0)
		return 0;

	const VkAccelerationStructureGeometryKHR geometry = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
		.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
		.geometry = { .triangles = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
			.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
			.vertexStride = sizeof(float) * 3,
			.maxVertex = max(num_vertices, 1) - 1,
			.indexType = indexed ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_NONE_KHR,
		} }
	};
	const VkAccelerationStructureBuildGeometryInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		.flags = fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
		.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.geometryCount = 1,
		.pGeometries = &geometry,
	};
	uint32_t prims = max(num_vertices, num_indices) / 3;
	VkAccelerationStructureBuildSizesInfoKHR size = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &prims, &size);
	return align(size.buildScratchSize, minAccelerationStructureScratchOffsetAlignment);
}

static VkDeviceSize scratch_for_aabbs(uint32_t num_aabbs, bool fast_build)
{
	if (num_aabbs == 0)
		return 0;

	const VkAccelerationStructureGeometryKHR geometry = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
		.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
		.geometry = { .aabbs = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
			.stride = sizeof(VkAabbPositionsKHR),
		} }
	};
	const VkAccelerationStructureBuildGeometryInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		.flags = fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
		.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.geometryCount = 1,
		.pGeometries = &geometry,
	};
	VkAccelerationStructureBuildSizesInfoKHR size = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &num_aabbs, &size);
	return align(size.buildScratchSize, minAccelerationStructureScratchOffsetAlignment);
}

/*
=================
ensure_scratch_capacity

THE SHARED BUILD SCRATCH WAS A FIXED 32 MB, AND ONE FRAME'S DYNAMIC GEOMETRY
CAN NEED MORE. Measured on demo1 with a heavy config: a single build asked for
37.6 MB. The only guard was an assert(), compiled out in release, so the build
ran past the end of the buffer - corrupting whatever was mapped after it (model
vertex buffers, as it happened) and losing the device on the first unmapped
page it reached. Whether it faulted depended on the memory layout of the run,
which is why the crash came and went.

So the buffer grows. It can only be replaced HERE: this is the top of the
frame's builds, before anything recorded in this command buffer points at it,
and the wait covers the frames still in flight that do. It only ever grows, so
a map that needs it pays the wait once. Headroom covers the top-level build,
which reuses the same scratch after these.
=================
*/
static void ensure_scratch_capacity(VkDeviceSize needed)
{
	if (needed <= buf_accel_scratch.size)
		return;

	VkDeviceSize new_size = needed + needed / 4 + (4 << 20);
	new_size = align(new_size, 1 << 20);

	Com_Printf("Growing the acceleration structure build scratch from %zu to %llu bytes (%llu needed).\n",
		buf_accel_scratch.size, (unsigned long long)new_size, (unsigned long long)needed);

	vkpt_device_wait_idle();
	buffer_destroy(&buf_accel_scratch);
	buffer_create(&buf_accel_scratch, new_size,
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	ATTACH_LABEL_VARIABLE(buf_accel_scratch.buffer, BUFFER);
}

VkResult
vkpt_pt_create_all_dynamic(
	VkCommandBuffer cmd_buf,
	int idx, 
	const EntityUploadInfo* upload_info)
{
	scratch_buf_ptr = 0;

	const bool dyn_fast_build = !(cvar_pt_blas_fast_trace && cvar_pt_blas_fast_trace->integer);

	// every build below, sized before the first one is recorded
	{
		BufferResource_t *bv = NULL, *bi = NULL, *ba = NULL;
		uint64_t ov = 0, oi = 0, oa = 0;
		uint32_t nv = 0, ni = 0, na = 0;
		VkDeviceSize needed = 0;

		const uint32_t sections[] = {
			upload_info->opaque_prim_count, upload_info->transparent_prim_count,
			upload_info->masked_prim_count, upload_info->viewer_model_prim_count,
			upload_info->viewer_weapon_prim_count, upload_info->explosions_prim_count,
			upload_info->blood_prim_count,
		};
		for (int i = 0; i < LENGTH(sections); i++)
			needed += scratch_for_triangles(sections[i] * 3, 0, false, dyn_fast_build);

		vkpt_get_transparency_buffers(VKPT_TRANSPARENCY_PARTICLES, &bv, &ov, &bi, &oi, &nv, &ni);
		needed += scratch_for_triangles(nv, ni, bi != NULL, dyn_fast_build);
		vkpt_get_beam_aabb_buffer(&ba, &oa, &na);
		needed += scratch_for_aabbs(na, dyn_fast_build);
		vkpt_get_transparency_buffers(VKPT_TRANSPARENCY_SPRITES, &bv, &ov, &bi, &oi, &nv, &ni);
		needed += scratch_for_triangles(nv, ni, bi != NULL, dyn_fast_build);

		ensure_scratch_capacity(needed);
	}

	static accel_build_batch_t batch;
	batch.numBuilds = 0;

	// pt_clusters: the model sections from CLASes (clusters_build), else whole as below
	g_clusters[idx].active = false;
	const bool clustered = g_clusters_ready && clusters_build(cmd_buf, idx, upload_info, dyn_fast_build);
	if (g_clusters_ready && !clustered) {
		static bool warned;
		if (!warned) {
			warned = true;
			Com_WPrintf("pt_clusters: the driver gave no cluster sizes (or no memory) - the model sections are built whole\n");
		}
	}

	uint64_t offset_vertex_base = 0;
	uint64_t offset_vertex = offset_vertex_base;
	uint64_t offset_index = 0;
	if (!clustered) {
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->opaque_prim_count * 3, 0, blas_dynamic + idx, true, dyn_fast_build);

	offset_vertex = offset_vertex_base + upload_info->transparent_prim_offset * sizeof(prim_positions_t);
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->transparent_prim_count * 3, 0, blas_transparent_models + idx, true, dyn_fast_build);

	offset_vertex = offset_vertex_base + upload_info->masked_prim_offset * sizeof(prim_positions_t);
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->masked_prim_count * 3, 0, blas_masked_models + idx, true, dyn_fast_build);

	offset_vertex = offset_vertex_base + upload_info->viewer_model_prim_offset * sizeof(prim_positions_t);
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->viewer_model_prim_count * 3, 0, blas_viewer_models + idx, true, dyn_fast_build);

	offset_vertex = offset_vertex_base + upload_info->viewer_weapon_prim_offset * sizeof(prim_positions_t);
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->viewer_weapon_prim_count * 3, 0, blas_viewer_weapon + idx, true, dyn_fast_build);
	}

	offset_vertex = offset_vertex_base + upload_info->explosions_prim_offset * sizeof(prim_positions_t);
	vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
		upload_info->explosions_prim_count * 3, 0, blas_explosions + idx, true, dyn_fast_build);

	// Blood droplets (cl_blood_spheres). Just another section of the instanced
	// position buffer, so it builds exactly like the model sections above - the
	// only difference is who wrote the vertices into it; see blood.c.
	offset_vertex = offset_vertex_base + upload_info->blood_prim_offset * sizeof(prim_positions_t);
	// Settled blood: the section has not been written since this BLAS was built
	// from it, so it is still exactly what a rebuild would produce (pt_blood_blas_reuse,
	// see BLAS REUSE in blood.c).
	if (blas_blood[idx].accel == VK_NULL_HANDLE || !blas_blood[idx].present
		|| vkpt_blood_blas_needs_build(idx, upload_info->blood_prim_offset, upload_info->blood_prim_count, dyn_fast_build))
	{
		vkpt_pt_create_accel_bottom(&batch, &qvk.buf_positions_instanced, offset_vertex, NULL, offset_index,
			upload_info->blood_prim_count * 3, 0, blas_blood + idx, true, dyn_fast_build);
		vkpt_blood_blas_built(idx, upload_info->blood_prim_offset, upload_info->blood_prim_count, dyn_fast_build);
	}

	BufferResource_t* buffer_vertex = NULL;
	BufferResource_t* buffer_index = NULL;
	uint32_t num_vertices = 0;
	uint32_t num_indices = 0;
	vkpt_get_transparency_buffers(VKPT_TRANSPARENCY_PARTICLES, &buffer_vertex, &offset_vertex, &buffer_index, &offset_index, &num_vertices, &num_indices);
	vkpt_pt_create_accel_bottom(&batch, buffer_vertex, offset_vertex, buffer_index, offset_index, num_vertices, num_indices, blas_particles + idx, true, dyn_fast_build);

	BufferResource_t *buffer_aabb = NULL;
	uint64_t offset_aabb = 0;
	uint32_t num_aabbs = 0;
	vkpt_get_beam_aabb_buffer(&buffer_aabb, &offset_aabb, &num_aabbs);
	vkpt_pt_create_accel_bottom_aabb(&batch, buffer_aabb, offset_aabb, num_aabbs, blas_beams + idx, true, dyn_fast_build);
	
	vkpt_get_transparency_buffers(VKPT_TRANSPARENCY_SPRITES, &buffer_vertex, &offset_vertex, &buffer_index, &offset_index, &num_vertices, &num_indices);
	vkpt_pt_create_accel_bottom(&batch, buffer_vertex, offset_vertex, buffer_index, offset_index, num_vertices, num_indices, blas_sprites + idx, true, dyn_fast_build);

	accel_batch_flush(cmd_buf, &batch);

	MEM_BARRIER_BUILD_ACCEL(cmd_buf);
	scratch_buf_ptr = 0;

	return VK_SUCCESS;
}

static void
append_blas(QvkGeometryInstance_t *instances, uint32_t *num_instances, uint64_t blas_address, int vbo_index, uint prim_offset, int mask, int flags, int sbt_offset)
{
	if (!blas_address)
		return;

	QvkGeometryInstance_t instance = {
		.transform = {
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
		},
		.instance_id = vbo_index,
		.mask = mask,
		.instance_offset = sbt_offset,
		.flags = flags,
		.acceleration_structure = blas_address
	};
	
	assert(*num_instances < MAX_TLAS_INSTANCES);
	memcpy(instances + *num_instances, &instance, sizeof(instance));
	vkpt_refdef.uniform_instance_buffer.tlas_instance_prim_offsets[*num_instances] = prim_offset;
	vkpt_refdef.uniform_instance_buffer.tlas_instance_model_indices[*num_instances] = -1;
	++*num_instances;
}

void vkpt_pt_reset_instances()
{
	g_num_instances = 0;
	g_num_world_instances = 0;
}

void vkpt_pt_mark_world_instances(void)
{
	g_num_world_instances = g_num_instances;
}

uint32_t vkpt_pt_num_world_instances(void)
{
	return g_num_world_instances;
}

// the D3D12 backend hands this list to DXR as it is (d3d12/world.c)
typedef char qvk_instance_is_vk_instance[(sizeof(QvkGeometryInstance_t) == 64) ? 1 : -1];

const void *vkpt_pt_instance_list(uint32_t *count)
{
	*count = g_num_instances;
	return g_instances;
}

void vkpt_pt_instance_model_blas(const model_geometry_t* geom, const mat4 transform, uint32_t buffer_idx, int model_instance_index, uint32_t override_instance_mask)
{
	if (!vkpt_model_geometry_has_blas(geom))
		return;

	QvkGeometryInstance_t gpu_instance = {
		.transform = { // transpose the matrix
			transform[0][0], transform[1][0], transform[2][0], transform[3][0],
			transform[0][1], transform[1][1], transform[2][1], transform[3][1],
			transform[0][2], transform[1][2], transform[2][2], transform[3][2]
		},
		.instance_id = buffer_idx,
		.mask = override_instance_mask ? override_instance_mask : geom->instance_mask,
		.instance_offset = geom->sbt_offset,
		.flags = geom->instance_flags,
		.acceleration_structure = geom->blas_device_address,
	};

	assert(g_num_instances < MAX_TLAS_INSTANCES);
	memcpy(g_instances + g_num_instances, &gpu_instance, sizeof(gpu_instance));
	vkpt_refdef.uniform_instance_buffer.tlas_instance_prim_offsets[g_num_instances] = geom->prim_offsets[0];
	vkpt_refdef.uniform_instance_buffer.tlas_instance_model_indices[g_num_instances] = model_instance_index;
	++g_num_instances;
}

static void
build_tlas(VkCommandBuffer cmd_buf, accel_struct_t* as, VkDeviceAddress instance_data, uint32_t num_instances)
{
	// Build the TLAS
	VkAccelerationStructureGeometryDataKHR geometry = {
		.instances = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
			.data = {.deviceAddress = instance_data}
		}
	};

	VkAccelerationStructureGeometryKHR topASGeometry = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
		.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
		.geometry = geometry
	};

	// Find size to build on the device
	const bool tlas_fast_build = !(cvar_pt_tlas_fast_trace && cvar_pt_tlas_fast_trace->integer);

	VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
		.flags = tlas_fast_build ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
		                         : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
		.geometryCount = 1,
		.pGeometries = &topASGeometry,
		.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
		.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
		.srcAccelerationStructure = VK_NULL_HANDLE
	};

	VkAccelerationStructureBuildSizesInfoKHR sizeInfo = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &num_instances, &sizeInfo);
	assert(sizeInfo.accelerationStructureSize < SIZE_SCRATCH_BUFFER);

	// The accel == NULL test is load-bearing: with fast_build 0 a zeroed match compares
	// equal, so tlas_effects with 0 instances (a level still loading) was judged current
	// and built into a NULL destination. The hard-coded `true` used to mask that.
	if (!accel_matches_top_level(&as->match, tlas_fast_build, num_instances) || as->accel == VK_NULL_HANDLE)
	{
		/* TLASREUSE, pt_fog_log 1. Prints the handle destroyed here beside the
		   one created immediately after. When those two are EQUAL, the driver
		   has recycled the handle value across a destroy/create - which is the
		   exact condition that made the old `last_written[] != tlas` guard in
		   god_rays.c skip its descriptor update and leave the fog tracing a
		   freed acceleration structure. Kept as a diagnostic because "a handle
		   is not an identity" is easy to re-introduce and hard to see. */
		const VkAccelerationStructureKHR destroyed = as->accel;

		destroy_accel_struct(as);

		// Create the buffer for the acceleration structure
		buffer_create(&as->mem, sizeInfo.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		// Create TLAS
		// Create acceleration structure
		VkAccelerationStructureCreateInfoKHR createInfo = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
			.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
			.size = sizeInfo.accelerationStructureSize,
			.buffer = as->mem.buffer
		};

		// Create the acceleration structure
		qvkCreateAccelerationStructureKHR(qvk.device, &createInfo, NULL, &as->accel);

		if (destroyed != VK_NULL_HANDLE && Cvar_Get("pt_fog_log", "0", 0)->integer)
			if (Cvar_Get("pt_fog_log", "0", 0)->integer == 1)
			Com_Printf("TLASREUSE destroyed=%llu created=%llu same=%d instances=%u\n",
				(unsigned long long)(uintptr_t)destroyed,
				(unsigned long long)(uintptr_t)as->accel,
				destroyed == as->accel, num_instances);

		as->match.fast_build = tlas_fast_build;
		as->match.index_count = 0;
		as->match.vertex_count = 0;
		as->match.aabb_count = 0;
		as->match.instance_count = num_instances;
	}

	// Update build information
	buildInfo.dstAccelerationStructure = as->accel;
	buildInfo.scratchData.deviceAddress = buf_accel_scratch.address + scratch_buf_ptr;
	assert(buf_accel_scratch.address);
	
	// Update the scratch buffer ptr
	scratch_buf_ptr += sizeInfo.buildScratchSize;
	scratch_buf_ptr = align(scratch_buf_ptr, minAccelerationStructureScratchOffsetAlignment);
	check_scratch_overflow(sizeInfo.buildScratchSize, __func__);

	VkAccelerationStructureBuildRangeInfoKHR offset = { .primitiveCount = num_instances };

	const VkAccelerationStructureBuildRangeInfoKHR* offsets = &offset;

	qvkCmdBuildAccelerationStructuresKHR(
		cmd_buf,
		1,
		&buildInfo,
		&offsets);
}

uint32_t
vkpt_pt_append_dynamic_instances(const uint64_t blas[VKPT_NUM_DYN], const EntityUploadInfo* upload_info, bool weapon_left_handed)
{
	append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_OPAQUE], VERTEX_BUFFER_INSTANCED, 0,
		AS_FLAG_OPAQUE, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR, SBTO_OPAQUE);

	append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_TRANSPARENT], VERTEX_BUFFER_INSTANCED, upload_info->transparent_prim_offset,
		AS_FLAG_TRANSPARENT, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR, SBTO_OPAQUE);

	append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_MASKED], VERTEX_BUFFER_INSTANCED, upload_info->masked_prim_offset,
		AS_FLAG_OPAQUE, VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, SBTO_MASKED);

    append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_VIEWER_WEAPON], VERTEX_BUFFER_INSTANCED, upload_info->viewer_weapon_prim_offset,
		AS_FLAG_VIEWER_WEAPON, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR | (weapon_left_handed ? VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR : 0), SBTO_OPAQUE);

	// also without a world: the player setup menu's bounce card is a viewer model
	if (cl_player_model->integer == CL_PLAYER_MODEL_FIRST_PERSON
		|| (vkpt_refdef.fd && (vkpt_refdef.fd->rdflags & RDF_NOWORLDMODEL)))
	{
		append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_VIEWER_MODELS], VERTEX_BUFFER_INSTANCED, upload_info->viewer_model_prim_offset,
			AS_FLAG_VIEWER_MODELS, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR | VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, SBTO_OPAQUE);
	}
	
	// Blood droplets ride in the GEOMETRY TLAS, not the effects one - that is the
	// whole point of them. FORCE_OPAQUE because a droplet is solid: the ray stops
	// on it, which is what buys the shadows and the presence in reflections that a
	// translucent particle quad can never have.
	// AS_FLAG_BLOOD, not AS_FLAG_OPAQUE: the four cull masks in path_tracer_rgen.h
	// all carry it, so nothing about how blood is lit, shadowed or reflected
	// changes - but the fog march's per-step rays cull on AS_FLAG_OPAQUE alone and
	// now skip the droplets. See constants.h.
	append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_BLOOD], VERTEX_BUFFER_INSTANCED, upload_info->blood_prim_offset,
		AS_FLAG_BLOOD, VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR, SBTO_OPAQUE);

	uint32_t num_instances_geometry = g_num_instances;

	// Note: explosions use a different primitive addressing scheme from the other geometry.
	// See the comment in pt_logic_explosion(...) in path_tracer_hit_shaders.h for more info.
	append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_EXPLOSIONS], (int)upload_info->explosions_prim_offset, 0,
		AS_FLAG_EFFECTS, 0, SBTO_EXPLOSION);
	
	if (cvar_pt_enable_particles->integer != 0)
	{
		append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_PARTICLES], 0, 0,
			AS_FLAG_EFFECTS, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, SBTO_PARTICLE);
	}

	if (cvar_pt_enable_beams->integer != 0)
	{
		append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_BEAMS], 0, 0,
			AS_FLAG_EFFECTS, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, SBTO_BEAM);
	}

	if (cvar_pt_enable_sprites->integer != 0)
	{
		append_blas(g_instances, &g_num_instances, blas[VKPT_DYN_SPRITES], 0, 0,
			AS_FLAG_EFFECTS, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, SBTO_SPRITE);
	}

	return num_instances_geometry;
}

// a dynamic BLAS's device address for the instance list, 0 when it has nothing
static uint64_t
dynamic_blas_address(const accel_struct_t* blas)
{
	if (!blas->present || blas->accel == VK_NULL_HANDLE)
		return 0;

	VkAccelerationStructureDeviceAddressInfoKHR  as_device_address_info = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
		.accelerationStructure = blas->accel,
	};

	return qvkGetAccelerationStructureDeviceAddressKHR(qvk.device, &as_device_address_info);
}

VkResult
vkpt_pt_create_toplevel(VkCommandBuffer cmd_buf, int idx, const EntityUploadInfo* upload_info, bool weapon_left_handed)
{
	uint64_t blas[VKPT_NUM_DYN];
	blas[VKPT_DYN_OPAQUE]        = dynamic_blas_address(&blas_dynamic[idx]);
	blas[VKPT_DYN_TRANSPARENT]   = dynamic_blas_address(&blas_transparent_models[idx]);
	blas[VKPT_DYN_MASKED]        = dynamic_blas_address(&blas_masked_models[idx]);
	blas[VKPT_DYN_VIEWER_WEAPON] = dynamic_blas_address(&blas_viewer_weapon[idx]);
	blas[VKPT_DYN_VIEWER_MODELS] = dynamic_blas_address(&blas_viewer_models[idx]);
	blas[VKPT_DYN_BLOOD]         = dynamic_blas_address(&blas_blood[idx]);
	blas[VKPT_DYN_EXPLOSIONS]    = dynamic_blas_address(&blas_explosions[idx]);
	blas[VKPT_DYN_PARTICLES]     = dynamic_blas_address(&blas_particles[idx]);
	blas[VKPT_DYN_BEAMS]         = dynamic_blas_address(&blas_beams[idx]);
	blas[VKPT_DYN_SPRITES]       = dynamic_blas_address(&blas_sprites[idx]);
	// pt_clusters: this frame's model sections were built from CLASes
	if (g_clusters[idx].active)
		for (int r = 0; r < CLUSTER_RUNS; r++)
			blas[VKPT_DYN_OPAQUE + r] = g_clusters[idx].run_address[r];

	// the instances so far are the static ones; the dynamic BLASes' come next
	const uint32_t first_dynamic = g_num_instances;
	const uint32_t num_instances_geometry = vkpt_pt_append_dynamic_instances(blas, upload_info, weapon_left_handed);

	uint32_t num_instances_effects = g_num_instances - num_instances_geometry;
	
	void *instance_data = buffer_map(buf_instances + idx);
	memcpy(instance_data, &g_instances, sizeof(QvkGeometryInstance_t) * g_num_instances);

	buffer_unmap(buf_instances + idx);
	instance_data = NULL;

	scratch_buf_ptr = 0;
	vk_ptlas_t* pt = &g_ptlas[idx];
	pt->active = g_ptlas_ready && cvar_pt_ptlas && cvar_pt_ptlas->integer != 0;
	if (pt->active)
	{
		/* THE PARTITIONED TLAS: this slot's, updated in place with what changed since
		   the slot's last frame - the world in the global partition, the dynamic
		   BLASes (rebuilt every frame, maybe at the same address) rewritten always.
		   The slot's fence retired the last frame that read it. */
		const bool fast_trace = cvar_pt_tlas_fast_trace && cvar_pt_tlas_fast_trace->integer;
		if (ptlas_diff(&pt->state, g_instances, num_instances_geometry,
			min(g_num_world_instances, num_instances_geometry), min(first_dynamic, num_instances_geometry),
			fast_trace ? 1 : 2))
		{
			const VkDeviceAddress source = ptlas_diff_is_update(&pt->state) ? pt->memory.address : 0;
			ptlas_stage(&pt->state, buffer_map(&pt->stage), pt->stage.address, false);
			buffer_unmap(&pt->stage);

			VkBuildPartitionedAccelerationStructureInfoNV info = {
				.sType = VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV,
				.input = ptlas_inputs(&pt->state, fast_trace),
				.srcAccelerationStructureData = source,
				.dstAccelerationStructureData = pt->memory.address,
				.scratchData = pt->scratch.address,
				.srcInfos = pt->stage.address + PTLAS_STAGE_OP,
				.srcInfosCount = pt->stage.address,
			};
			qvkCmdBuildPartitionedAccelerationStructuresNV(cmd_buf, &info);
		}
	}
	else
	{
		ptlas_invalidate(&pt->state);
		build_tlas(cmd_buf, &tlas_geometry[idx], buf_instances[idx].address, num_instances_geometry);
	}
	build_tlas(cmd_buf, &tlas_effects[idx], buf_instances[idx].address + num_instances_geometry * sizeof(QvkGeometryInstance_t), num_instances_effects);

	MEM_BARRIER_BUILD_ACCEL(cmd_buf); /* probably not needed here but doesn't matter */

	return VK_SUCCESS;
}

/* MERGED BARRIERS, pt_merge_barriers. Each pass here ends in a list of
   BARRIER_COMPUTE calls - one vkCmdPipelineBarrier per image, ALL_COMMANDS to
   ALL_COMMANDS - of which the first drains the GPU and the rest find it idle.
   Between BARRIER_BATCH_BEGIN and BARRIER_BATCH_END the list is recorded as:
   0 - one call per image, as before;
   1 - one call carrying all the image barriers;
   2 - one global memory barrier. None of these images changes layout (GENERAL
       to GENERAL), so that covers the same writes, and it adds SHADER_READ to
       the destination access, which the per-image ones leave out.
   A BARRIER_COMPUTE outside a batch is recorded on the spot.
   MEASURED 11-04: no difference. Whole GPU frame 11.12 / 11.09 / 11.11 ms
   for 0 / 1 / 2 over three rounds each, inside the noise - the barriers after
   the first really are free. Left at 0, so the frame is recorded as before. The cvar is
   declared with pt_flow_split_pass, at the top. */
static struct {
	VkImageMemoryBarrier barriers[48];
	uint32_t count;
	bool open;
} barrier_batch;

static void
barrier_compute(VkCommandBuffer cmd_buf, VkImage img)
{
	const VkImageMemoryBarrier barrier = {
		.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image               = img,
		.subresourceRange    = {
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel   = 0,
			.levelCount     = 1,
			.baseArrayLayer = 0,
			.layerCount     = 1
		},
		.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout           = VK_IMAGE_LAYOUT_GENERAL,
	};

	if (barrier_batch.open && barrier_batch.count < LENGTH(barrier_batch.barriers))
	{
		barrier_batch.barriers[barrier_batch.count++] = barrier;
		return;
	}

	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void
barrier_batch_begin(void)
{
	barrier_batch.open = cvar_pt_merge_barriers && cvar_pt_merge_barriers->integer != 0;
	barrier_batch.count = 0;
}

static void
barrier_batch_end(VkCommandBuffer cmd_buf)
{
	const uint32_t count = barrier_batch.count;
	barrier_batch.open = false;
	barrier_batch.count = 0;
	if (count == 0)
		return;

	if (cvar_pt_merge_barriers->integer == 2)
	{
		const VkMemoryBarrier barrier = {
			.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		};
		vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
	}
	else
	{
		vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, count, barrier_batch.barriers);
	}
}

#define BARRIER_COMPUTE(cmd_buf, img) barrier_compute(cmd_buf, img)
#define BARRIER_BATCH_BEGIN() barrier_batch_begin()
#define BARRIER_BATCH_END(cmd_buf) barrier_batch_end(cmd_buf)

static void setup_rt_pipeline(VkCommandBuffer cmd_buf, VkPipelineBindPoint bind_point, pipeline_index_t index)
{
	vkCmdBindPipeline(cmd_buf, bind_point, rt_pipelines[index]);

	vkCmdBindDescriptorSets(cmd_buf, bind_point,
		rt_pipeline_layout, 0, 1, rt_descriptor_set + qvk.current_frame_index, 0, 0);

	vkCmdBindDescriptorSets(cmd_buf, bind_point,
		rt_pipeline_layout, 1, 1, &qvk.desc_set_ubo, 0, 0);

	VkDescriptorSet desc_set_textures = qvk_get_current_desc_set_textures();
	vkCmdBindDescriptorSets(cmd_buf, bind_point,
		rt_pipeline_layout, 2, 1, &desc_set_textures, 0, 0);

	vkCmdBindDescriptorSets(cmd_buf, bind_point,
		rt_pipeline_layout, 3, 1, &qvk.desc_set_vertex_buffer, 0, 0);
}

static void
dispatch_rays(VkCommandBuffer cmd_buf, pipeline_index_t pipeline_index, pt_push_constants_t push, uint32_t width, uint32_t height, uint32_t depth)
{
	if (qvk.use_ray_query)
	{
		setup_rt_pipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_index);

		vkCmdPushConstants(cmd_buf, rt_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

		vkCmdDispatch(cmd_buf, (width + 7) / 8, (height + 7) / 8, depth);
	}
	else
	{
		setup_rt_pipeline(cmd_buf, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_index);

		vkCmdPushConstants(cmd_buf, rt_pipeline_layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(push), &push);

		uint32_t sbt_offset = SBT_ENTRIES_PER_PIPELINE * pipeline_index * shaderGroupBaseAlignment;
		
		assert(buf_shader_binding_table.address);

		VkStridedDeviceAddressRegionKHR raygen = {
			.deviceAddress = buf_shader_binding_table.address + sbt_offset,
			.stride = shaderGroupBaseAlignment,
			.size = shaderGroupBaseAlignment
		};

		VkStridedDeviceAddressRegionKHR miss_and_hit = {
			.deviceAddress = buf_shader_binding_table.address + sbt_offset,
			.stride = shaderGroupBaseAlignment,
			.size = (VkDeviceSize)shaderGroupBaseAlignment * SBT_ENTRIES_PER_PIPELINE
		};

		VkStridedDeviceAddressRegionKHR callable = {
			// Not VK_NULL_HANDLE - a uint64 address, not a handle. It was a bare 0 in
			// headers 1.2.162 and is ((void*)0) from 1.3 on.
			.deviceAddress = 0,
			.stride = 0,
			.size = 0
		};

		qvkCmdTraceRaysKHR(cmd_buf,
			&raygen,
			&miss_and_hit,
			&miss_and_hit,
			&callable,
			width, height, depth);
	}
}

/* TILE COMPACTION helpers - see shader/constants.h. */

void
vkpt_pt_tile_layout(uint32_t screen_width, uint32_t screen_height, vkpt_tile_layout_t *layout)
{
	// The launch grid of a field is never wider or taller than the screen images
	// (both fields together fit in them), so this bounds every resolution the
	// images can be rendered at until they are rebuilt.
	const uint32_t tiles_x = (screen_width + 7) / 8;
	const uint32_t tiles_y = (screen_height + 7) / 8;
	layout->max = tiles_x * tiles_y * 2;
	layout->mask_stride = (layout->max + 31) / 32;
	layout->data_offset = PT_TILE_HDR_WORDS + PT_TILE_LISTS * layout->mask_stride;
	layout->size = (uint64_t)(layout->data_offset + PT_TILE_LISTS * layout->max) * sizeof(uint32_t);
}

/* Whether this view uses the lists - pt_tile_compaction, and a launch grid the
   layout and `max_groups` (the most groups one list's dispatch can have) can
   hold - and if so the header the shaders read, with every list empty. */
bool
vkpt_pt_tile_header(const vkpt_tile_layout_t *layout, uint32_t max_groups, uint32_t header[PT_TILE_HDR_WORDS])
{
	if (!cvar_pt_tile_compaction || !cvar_pt_tile_compaction->integer)
		return false;

	const uint32_t tiles_x = (vkpt_pt_field_width() + 7) / 8;
	const uint32_t tiles_y = (qvk.extent_render.height + 7) / 8;
	const uint32_t per_field = tiles_x * tiles_y;
	if (per_field * 2 > layout->max || per_field * 2 > max_groups)
		return false;

	memset(header, 0, PT_TILE_HDR_WORDS * sizeof(uint32_t));
	for (int list = 0; list < PT_TILE_LISTS; list++)
	{
		header[PT_TILE_HDR_ARGS + list * 4 + 0] = 0;   // tile count = groups in x
		header[PT_TILE_HDR_ARGS + list * 4 + 1] = 1;
		header[PT_TILE_HDR_ARGS + list * 4 + 2] = 1;
	}
	header[PT_TILE_HDR_TILES_X] = tiles_x;
	header[PT_TILE_HDR_PER_FIELD] = per_field;
	header[PT_TILE_HDR_MASK] = PT_TILE_HDR_WORDS;
	header[PT_TILE_HDR_MASK_STRIDE] = layout->mask_stride;
	header[PT_TILE_HDR_DATA] = layout->data_offset;
	header[PT_TILE_HDR_DATA_STRIDE] = layout->max;
	return true;
}

VkResult
vkpt_pt_create_tiles(void)
{
	vkpt_pt_tile_layout(qvk.extent_screen_images.width, qvk.extent_screen_images.height, &tiles_layout);

	return buffer_create(&buf_tiles, tiles_layout.size,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

VkResult
vkpt_pt_destroy_tiles(void)
{
	buffer_destroy(&buf_tiles);
	memset(&tiles_layout, 0, sizeof(tiles_layout));
	tiles_built = false;
	return VK_SUCCESS;
}

/* Empties the lists for this view and writes the layout the shaders read.
   Returns whether this view uses them: ray query only (the pipeline path
   launches through an SBT, which this does not drive), one GPU, and a launch
   grid the buffer and the device's group count limit can hold. */
static bool
tile_lists_reset(VkCommandBuffer cmd_buf)
{
	if (!qvk.use_ray_query || qvk.device_count != 1 || !buf_tiles.buffer || !buf_tiles.address)
		return false;

	uint32_t header[PT_TILE_HDR_WORDS];
	if (!vkpt_pt_tile_header(&tiles_layout, tiles_max_groups, header))
		return false;

	// the previous view's bounces read this buffer, indirectly and in shaders
	BUFFER_BARRIER(cmd_buf,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.buffer = buf_tiles.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE);

	vkCmdUpdateBuffer(cmd_buf, buf_tiles.buffer, 0, sizeof(header), header);
	vkCmdFillBuffer(cmd_buf, buf_tiles.buffer, PT_TILE_HDR_WORDS * sizeof(uint32_t),
		(VkDeviceSize)PT_TILE_LISTS * tiles_layout.mask_stride * sizeof(uint32_t), 0);

	BUFFER_BARRIER(cmd_buf,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.buffer = buf_tiles.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE);

	return true;
}

// What a pass appended becomes the next pass's tile ids and dispatch arguments.
static void
tile_lists_barrier(VkCommandBuffer cmd_buf)
{
	BUFFER_BARRIER(cmd_buf,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
		.buffer = buf_tiles.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE);
}

// One 8x8 group per listed tile; the shader turns the group into the tile's launch IDs.
static void
dispatch_rays_indirect(VkCommandBuffer cmd_buf, pipeline_index_t pipeline_index, pt_push_constants_t push, int list)
{
	setup_rt_pipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_index);
	vkCmdPushConstants(cmd_buf, rt_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdDispatchIndirect(cmd_buf, buf_tiles.buffer, (VkDeviceSize)(PT_TILE_HDR_ARGS + list * 4) * sizeof(uint32_t));
}

bool
vkpt_pt_flow_split_ran(void)
{
	return flow_split_ran;
}

// the D3D12 renderer's, which records the split pass itself (d3d12/passes.c)
void
vkpt_pt_set_flow_split_ran(bool ran)
{
	flow_split_ran = ran;
}

VkResult
vkpt_pt_trace_primary_rays(VkCommandBuffer cmd_buf)
{
	int frame_idx = qvk.frame_counter & 1;
	
	BUFFER_BARRIER(cmd_buf,
			.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
			.buffer = qvk.buf_readback.buffer,
			.offset = 0,
			.size = VK_WHOLE_SIZE,
	);

	BEGIN_PERF_MARKER(cmd_buf, PROFILER_PRIMARY_RAYS);

	tiles_built = tile_lists_reset(cmd_buf);

	const bool flow_split = cvar_pt_flow_split_pass->integer != 0 && qvk.device_count == 1 && vkpt_flow_marching();
	flow_split_ran = flow_split;

	for(int i = 0; i < qvk.device_count; i++)
	{
		set_current_gpu(cmd_buf, i);

		pt_push_constants_t push = { 0 };
		push.gpu_index = qvk.device_count == 1 ? -1 : i;
		push.bounce = 0;
		if (tiles_built)
		{
			push.tile_append_list = 1;
			push.tile_buffer = buf_tiles.address;
		}

		dispatch_rays(cmd_buf, flow_split ? PIPELINE_PRIMARY_RAYS_FLOW_SPLIT : PIPELINE_PRIMARY_RAYS, push, vkpt_pt_field_width(), qvk.extent_render.height, qvk.device_count == 1 ? 2 : 1);
	}

	if (tiles_built)
		tile_lists_barrier(cmd_buf);

	set_current_gpu(cmd_buf, ALL_GPUS);

	END_PERF_MARKER(cmd_buf, PROFILER_PRIMARY_RAYS);

	if (flow_split)
	{
		// The hand-off (primary_rays.rgen), and the seed the ray is rebuilt from
		BARRIER_BATCH_BEGIN();
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAY_LENGTH]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_ASVGF_COLOR]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_TRANSPARENT]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_ASVGF_RNG_SEED_A + frame_idx]);
		BARRIER_BATCH_END(cmd_buf);

		BEGIN_PERF_MARKER(cmd_buf, PROFILER_FLOW_MARCH);
		pt_push_constants_t push = { 0 };
		push.gpu_index = -1;
		dispatch_rays(cmd_buf, PIPELINE_FLOW_PASS, push, vkpt_pt_field_width(), qvk.extent_render.height, 2);
		END_PERF_MARKER(cmd_buf, PROFILER_FLOW_MARCH);

		// god_rays pass 0 writes ASVGF_COLOR next
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_ASVGF_COLOR]);
	}

	BARRIER_BATCH_BEGIN();
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VISBUF_PRIM_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VISBUF_BARY_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_TRANSPARENT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_MOTION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_SHADING_POSITION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DIRECTION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_THROUGHPUT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BOUNCE_THROUGHPUT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_GODRAYS_THROUGHPUT_DIST]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BASE_COLOR_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_METALLIC_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_CLUSTER_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DEPTH_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_NORMAL_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_ASVGF_RNG_SEED_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DEPTH_SPLIT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAY_LENGTH]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_TRANSPARENT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_ROUGHNESS]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_ALBEDO]);
	// The opaque-overlay accumulator for pt_dlss_bt_guide, read back by reflect_refract.
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BEFORE_TRANSPARENT]);
	BARRIER_BATCH_END(cmd_buf);


	return VK_SUCCESS;
}

VkResult
vkpt_pt_trace_reflections(VkCommandBuffer cmd_buf, int bounce)
{
	int frame_idx = qvk.frame_counter & 1;

	/* pt_reflect_noflow (default 1; 0 = always the full shader). With no Flow
	   grid this frame the march in reflect_refract.rgen returns at its first
	   test, so the variant compiled without it gives the same image - and drops
	   the shader from 167 to 128 registers, which is what it costs every
	   reflect/refract pixel even where there is no fire anywhere. */
	static cvar_t *cvar_pt_reflect_noflow = NULL;
	if (!cvar_pt_reflect_noflow)
		cvar_pt_reflect_noflow = Cvar_Get("pt_reflect_noflow", "1", 0);
	const bool noflow = cvar_pt_reflect_noflow->integer != 0 && !vkpt_flow_marching();

	for (int i = 0; i < qvk.device_count; i++)
    {
        set_current_gpu(cmd_buf, i);

        pipeline_index_t pipeline = (bounce == 0) ? PIPELINE_REFLECT_REFRACT_1 : PIPELINE_REFLECT_REFRACT_2;
        if (noflow)
            pipeline = (bounce == 0) ? PIPELINE_REFLECT_REFRACT_1_NOFLOW : PIPELINE_REFLECT_REFRACT_2_NOFLOW;

        pt_push_constants_t push = { 0 };
        push.gpu_index = qvk.device_count == 1 ? -1 : i;
        push.bounce = bounce;

        if (tiles_built && bounce < PT_TILE_LISTS)
        {
            // walk the tiles this bounce has work in; add the next bounce's
            push.tile_read_list = bounce + 1;
            push.tile_append_list = (bounce + 1 < PT_TILE_LISTS) ? bounce + 2 : 0;
            push.tile_buffer = buf_tiles.address;
            dispatch_rays_indirect(cmd_buf, pipeline, push, bounce);
        }
        else
            dispatch_rays(cmd_buf, pipeline, push, vkpt_pt_field_width(), qvk.extent_render.height, qvk.device_count == 1 ? 2 : 1);
	}

	if (tiles_built)
		tile_lists_barrier(cmd_buf);

	set_current_gpu(cmd_buf, ALL_GPUS);

	BARRIER_BATCH_BEGIN();
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_TRANSPARENT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_MOTION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_SHADING_POSITION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DIRECTION]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_THROUGHPUT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_GODRAYS_THROUGHPUT_DIST]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BASE_COLOR_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_METALLIC_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_CLUSTER_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DEPTH_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_NORMAL_A + frame_idx]);
	/* These are written by reflect_refract.rgen and read again by the NEXT bounce of the
	   same shader, so they need the same treatment as the rest of the list. GEO_NORMAL is
	   the reflection plane the apparent-position motion vector is folded across, and
	   VIEW_DEPTH_SPLIT is what checkerboard_interleave.comp turns into the DLSS and DLSS-G
	   depth buffers - a stale read of either lands directly in what DLSS is handed. */
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_GEO_NORMAL_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VISBUF_PRIM_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VISBUF_BARY_A + frame_idx]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DEPTH_SPLIT]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_REFLECT_MOTION]);
	// pt_dlss_bt_guide's opaque-overlay accumulator, carried bounce to bounce like
	// PT_TRANSPARENT above.
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BEFORE_TRANSPARENT]);
	BARRIER_BATCH_END(cmd_buf);

	return VK_SUCCESS;
}

static void
sharc_barrier(VkCommandBuffer cmd_buf)
{
	VkMemoryBarrier barrier = {
		.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
	};

	vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Radiance cache (pt_sharc, shader/sharc.h): train it from this frame's G-buffer and
   resolve it, ready for indirect_lighting.rgen to read. Needs the primary surfaces
   and must run before the first bounce pass, which overwrites SHADING_POSITION with
   the bounce hit when there is a second bounce. */
VkResult
vkpt_pt_sharc_update(VkCommandBuffer cmd_buf, bool clear)
{
	BEGIN_PERF_MARKER(cmd_buf, PROFILER_SHARC);

	if (clear)
		vkpt_sharc_clear(cmd_buf);

	uint32_t stride = (uint32_t)max(1, min(16, (int)cvar_pt_sharc_stride->value));

	pt_push_constants_t push = { 0 };
	push.gpu_index = -1;
	push.bounce = 0;

	dispatch_rays(cmd_buf, PIPELINE_SHARC_UPDATE, push,
		(qvk.extent_render.width + stride - 1) / stride,
		(qvk.extent_render.height + stride - 1) / stride, 1);

	sharc_barrier(cmd_buf);

	vkpt_sharc_resolve(cmd_buf);

	sharc_barrier(cmd_buf);

	END_PERF_MARKER(cmd_buf, PROFILER_SHARC);

	return VK_SUCCESS;
}

VkResult
vkpt_pt_trace_lighting(VkCommandBuffer cmd_buf, float num_bounce_rays)
{
	int frame_idx = qvk.frame_counter & 1;

	BEGIN_PERF_MARKER(cmd_buf, PROFILER_DIRECT_LIGHTING);

	for (int i = 0; i < qvk.device_count; i++)
	{
		set_current_gpu(cmd_buf, i);

		pipeline_index_t pipeline = (cvar_pt_caustics->value != 0) ? PIPELINE_DIRECT_LIGHTING_CAUSTICS : PIPELINE_DIRECT_LIGHTING;

		pt_push_constants_t push = { 0 };
		push.gpu_index = qvk.device_count == 1 ? -1 : i;
		push.bounce = 0;

		dispatch_rays(cmd_buf, pipeline, push, vkpt_pt_field_width(), qvk.extent_render.height, qvk.device_count == 1 ? 2 : 1);
	}

	set_current_gpu(cmd_buf, ALL_GPUS);

	END_PERF_MARKER(cmd_buf, PROFILER_DIRECT_LIGHTING);

	BARRIER_BATCH_BEGIN();
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_SH]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_COCG]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_HF]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_SPEC]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAY_LENGTH]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_DIFFUSE]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_SPECULAR]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_ALBEDO]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_ROUGHNESS]);
	BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_NORMAL]);

	if (cvar_pt_restir->value != 0) {
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_A + frame_idx]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_ID_A + frame_idx]);
	}
	BARRIER_BATCH_END(cmd_buf);

	BUFFER_BARRIER(cmd_buf,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.buffer = qvk.buf_readback.buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	);

	BEGIN_PERF_MARKER(cmd_buf, PROFILER_INDIRECT_LIGHTING);

	assert(num_bounce_rays <= 8);
	if (num_bounce_rays > 0)
	{
		for (int i = 0; i < qvk.device_count; i++)
		{
			set_current_gpu(cmd_buf, i);

            int height;
            if (num_bounce_rays == 0.5f)
                height = qvk.extent_render.height / 2;
            else
                height = qvk.extent_render.height;

			const int num_bounces = (int)ceilf(num_bounce_rays);
			for (int bounce_ray = 0; bounce_ray < num_bounces; bounce_ray++)
            {
				// Marker 0 is the first bounce; marker 1 spans every later bounce, which all
				// run the second-bounce pipeline with their number in the push constant.
				if (bounce_ray <= 1)
					BEGIN_PERF_MARKER(cmd_buf, PROFILER_INDIRECT_LIGHTING_0 + bounce_ray);
                pipeline_index_t pipeline = (bounce_ray == 0) ? PIPELINE_INDIRECT_LIGHTING_FIRST : PIPELINE_INDIRECT_LIGHTING_SECOND;

                pt_push_constants_t push = { 0 };
                push.gpu_index = qvk.device_count == 1 ? -1 : i;
                push.bounce = bounce_ray;

                dispatch_rays(cmd_buf, pipeline, push, vkpt_pt_field_width(), height, qvk.device_count == 1 ? 2 : 1);

				BARRIER_BATCH_BEGIN();
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_SH]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_COCG]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_HF]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_SPEC]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_BOUNCE_THROUGHPUT]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_POS_A + frame_idx]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_DATA_A + frame_idx]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_ORIGIN_A + frame_idx]);
				// The next bounce's starting point, written by this one.
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_SHADING_POSITION]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_GEO_NORMAL2]);
				BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_VIEW_DIRECTION2]);
				BARRIER_BATCH_END(cmd_buf);

				if (bounce_ray == 0 || bounce_ray == num_bounces - 1)
					END_PERF_MARKER(cmd_buf, PROFILER_INDIRECT_LIGHTING_0 + (bounce_ray == 0 ? 0 : 1));
			}
		}
	}

	END_PERF_MARKER(cmd_buf, PROFILER_INDIRECT_LIGHTING);

	// ReSTIR GI (shader/restir_gi.h): merge the first-bounce candidates with last
	// frame's reservoirs and light the diffuse channel. The shader itself stands down
	// in photo mode.
	if (cvar_pt_restir_gi->value != 0 && num_bounce_rays >= 1)
	{
		BEGIN_PERF_MARKER(cmd_buf, PROFILER_RESTIR_GI);

		for (int i = 0; i < qvk.device_count; i++)
		{
			set_current_gpu(cmd_buf, i);

			pt_push_constants_t push = { 0 };
			push.gpu_index = qvk.device_count == 1 ? -1 : i;
			push.bounce = 0;

			dispatch_rays(cmd_buf, PIPELINE_RESTIR_GI, push, vkpt_pt_field_width(), qvk.extent_render.height, qvk.device_count == 1 ? 2 : 1);
		}

		set_current_gpu(cmd_buf, ALL_GPUS);

		BARRIER_BATCH_BEGIN();
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_SH]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_COCG]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_DIFFUSE]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_POS_A + frame_idx]);
		BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RESTIR_GI_DATA_A + frame_idx]);
		BARRIER_BATCH_END(cmd_buf);

		// Spatial reuse (pt_restir_gi_spatial): the same shader again, bounce_index 1,
		// reading the reservoirs every pixel's temporal pass has just written - which is
		// why it is a separate dispatch behind the barriers above.
		if (cvar_pt_restir_gi_spatial->value > 0)
		{
			for (int i = 0; i < qvk.device_count; i++)
			{
				set_current_gpu(cmd_buf, i);

				pt_push_constants_t push = { 0 };
				push.gpu_index = qvk.device_count == 1 ? -1 : i;
				push.bounce = 1;

				dispatch_rays(cmd_buf, PIPELINE_RESTIR_GI, push, vkpt_pt_field_width(), qvk.extent_render.height, qvk.device_count == 1 ? 2 : 1);
			}

			set_current_gpu(cmd_buf, ALL_GPUS);

			BARRIER_BATCH_BEGIN();
			BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_SH]);
			BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_COLOR_LF_COCG]);
			BARRIER_COMPUTE(cmd_buf, qvk.images[VKPT_IMG_PT_RAYLENGTH_DIFFUSE]);
			BARRIER_BATCH_END(cmd_buf);
		}

		END_PERF_MARKER(cmd_buf, PROFILER_RESTIR_GI);
	}

	set_current_gpu(cmd_buf, ALL_GPUS);

	return VK_SUCCESS;
}

VkResult
vkpt_pt_destroy()
{
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		destroy_accel_struct(tlas_geometry + i);
		destroy_accel_struct(tlas_effects + i);
		buffer_destroy(buf_instances + i);
		vkpt_pt_destroy_dynamic(i);
	}
	
	buffer_destroy(&buf_accel_scratch);
	ptlas_destroy();
	clusters_destroy();
	vkDestroyDescriptorSetLayout(qvk.device, rt_descriptor_set_layout, NULL);
	vkDestroyPipelineLayout(qvk.device, rt_pipeline_layout, NULL);
	vkDestroyDescriptorPool(qvk.device, rt_descriptor_pool, NULL);

	return VK_SUCCESS;
}

VkResult
vkpt_pt_create_pipelines()
{
	VkSpecializationMapEntry specEntry = {
		.constantID = 0,
		.offset = 0,
		.size = sizeof(uint32_t),
	};

	uint32_t numbers[2] = { 0, 1 };

	VkSpecializationInfo specInfo[2] = {
		{
			.mapEntryCount = 1,
			.pMapEntries = &specEntry,
			.dataSize = sizeof(uint32_t),
			.pData = &numbers[0],
		},
		{
			.mapEntryCount = 1,
			.pMapEntries = &specEntry,
			.dataSize = sizeof(uint32_t),
			.pData = &numbers[1],
		}
	};

	// reflect_refract.rgen: constant 0 = spec_bounce_index, constant 1 = spec_flow.
	// Indexed [bounce != 0][no flow].
	const VkSpecializationMapEntry specEntriesRR[2] = {
		{ .constantID = 0, .offset = 0,                .size = sizeof(uint32_t) },
		{ .constantID = 1, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) },
	};
	const uint32_t numbersRR[4][2] = { { 0, 1 }, { 0, 0 }, { 1, 1 }, { 1, 0 } };

	// primary_rays.rgen: constant 1 = spec_flow_mode (0 inline, 1 split primary, 2 flow pass)
	const VkSpecializationMapEntry specEntryFlow = { .constantID = 1, .offset = 0, .size = sizeof(uint32_t) };
	const uint32_t numbersFlow[3] = { 0, 1, 2 };
	VkSpecializationInfo specInfoFlow[3];
	for (int i = 0; i < 3; i++)
	{
		specInfoFlow[i] = (VkSpecializationInfo) {
			.mapEntryCount = 1,
			.pMapEntries = &specEntryFlow,
			.dataSize = sizeof(uint32_t),
			.pData = &numbersFlow[i],
		};
	}
	VkSpecializationInfo specInfoRR[4];
	for (int i = 0; i < 4; i++)
	{
		specInfoRR[i] = (VkSpecializationInfo) {
			.mapEntryCount = 2,
			.pMapEntries = specEntriesRR,
			.dataSize = sizeof(numbersRR[i]),
			.pData = numbersRR[i],
		};
	}

	uint32_t num_shader_groups = SBT_ENTRIES_PER_PIPELINE * PIPELINE_COUNT;
	char* shader_handles = alloca(num_shader_groups * shaderGroupHandleSize);
	memset(shader_handles, 0, num_shader_groups * shaderGroupHandleSize);

	VkPipelineShaderStageCreateInfo shader_stages[] = {
		// Stages used by all pipelines. Count must match num_base_shader_stages below!
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
			.pName = "main"
			// Shader module is set below
		},
		SHADER_STAGE(QVK_MOD_PATH_TRACER_RMISS,               VK_SHADER_STAGE_MISS_BIT_KHR),
		SHADER_STAGE(QVK_MOD_PATH_TRACER_RCHIT,               VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR),
		SHADER_STAGE(QVK_MOD_PATH_TRACER_MASKED_RAHIT,        VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
		// Stages used by all pipelines that consider transparency
		SHADER_STAGE(QVK_MOD_PATH_TRACER_PARTICLE_RAHIT,      VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
		SHADER_STAGE(QVK_MOD_PATH_TRACER_EXPLOSION_RAHIT,     VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
		SHADER_STAGE(QVK_MOD_PATH_TRACER_SPRITE_RAHIT,        VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
		// Must be last
		SHADER_STAGE(QVK_MOD_PATH_TRACER_BEAM_RAHIT,          VK_SHADER_STAGE_ANY_HIT_BIT_KHR),
		SHADER_STAGE(QVK_MOD_PATH_TRACER_BEAM_RINT,           VK_SHADER_STAGE_INTERSECTION_BIT_KHR),
	};
	const unsigned num_base_shader_stages = 5;
	const unsigned num_transparent_no_beam_shader_stages = 8;

	for (pipeline_index_t index = 0; index < PIPELINE_COUNT; index++)
	{
		bool needs_beams = index <= PIPELINE_REFLECT_REFRACT_2_NOFLOW;
		bool needs_transparency = needs_beams || index == PIPELINE_INDIRECT_LIGHTING_FIRST;
		unsigned int num_shader_stages = needs_beams ? LENGTH(shader_stages) : (needs_transparency ? num_transparent_no_beam_shader_stages : num_base_shader_stages);

		switch (index)
		{
		case PIPELINE_PRIMARY_RAYS:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_PRIMARY_RAYS_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoFlow[0];
			break;
		case PIPELINE_PRIMARY_RAYS_FLOW_SPLIT:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_PRIMARY_RAYS_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoFlow[1];
			break;
		case PIPELINE_FLOW_PASS:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_PRIMARY_RAYS_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoFlow[2];
			break;
		case PIPELINE_REFLECT_REFRACT_1:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_REFLECT_REFRACT_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoRR[0];
			break;
		case PIPELINE_REFLECT_REFRACT_2:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_REFLECT_REFRACT_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoRR[2];
			break;
		case PIPELINE_REFLECT_REFRACT_1_NOFLOW:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_REFLECT_REFRACT_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoRR[1];
			break;
		case PIPELINE_REFLECT_REFRACT_2_NOFLOW:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_REFLECT_REFRACT_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfoRR[3];
			break;
		case PIPELINE_DIRECT_LIGHTING:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_DIRECT_LIGHTING_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfo[0];
			break;
		case PIPELINE_DIRECT_LIGHTING_CAUSTICS:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_DIRECT_LIGHTING_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfo[1];
			break;
		case PIPELINE_INDIRECT_LIGHTING_FIRST:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_INDIRECT_LIGHTING_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfo[0];
			break;
		case PIPELINE_INDIRECT_LIGHTING_SECOND:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_INDIRECT_LIGHTING_RGEN];
			shader_stages[0].pSpecializationInfo = &specInfo[1];
			break;
		case PIPELINE_SHARC_UPDATE:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_SHARC_UPDATE_RGEN];
			shader_stages[0].pSpecializationInfo = NULL;
			break;
		case PIPELINE_RESTIR_GI:
			shader_stages[0].module = qvk.shader_modules[QVK_MOD_RESTIR_GI_RGEN];
			shader_stages[0].pSpecializationInfo = NULL;
			break;
		default:
			assert(!"invalid pipeline index");
			break;
		}

		if (qvk.use_ray_query)
		{
			VkComputePipelineCreateInfo compute_pipeline_info = {
				.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
				.layout = rt_pipeline_layout,
				.stage = {
					.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
					.stage = VK_SHADER_STAGE_COMPUTE_BIT,
					.pName = "main",
					.module = shader_stages[0].module,
					.pSpecializationInfo = shader_stages[0].pSpecializationInfo
				}
			};

			VkResult res = vkCreateComputePipelines(qvk.device, 0, 1, &compute_pipeline_info, NULL, &rt_pipelines[index]);

			if (res != VK_SUCCESS)
			{
				Com_EPrintf("Failed to create ray tracing compute pipeline #%d, vkCreateComputePipelines error code is %s\n", index, qvk_result_to_string(res));
				return res;
			}
		}
		else
		{
			VkRayTracingShaderGroupCreateInfoKHR rt_shader_group_info[] = {
				[SBT_RGEN] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
					.generalShader      = 0,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RMISS_EMPTY] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
					.generalShader      = 1,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RCHIT_GEOMETRY] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = 2,
					.anyHitShader       = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RAHIT_MASKED] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = 2,
					.anyHitShader       = 3,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RCHIT_EFFECTS] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = VK_SHADER_UNUSED_KHR,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RAHIT_PARTICLE] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = 4,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RAHIT_EXPLOSION] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = 5,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RAHIT_SPRITE] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = 6,
					.intersectionShader = VK_SHADER_UNUSED_KHR
				},
				[SBT_RINT_BEAM] = {
					.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
					.type               = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
					.generalShader      = VK_SHADER_UNUSED_KHR,
					.closestHitShader   = VK_SHADER_UNUSED_KHR,
					.anyHitShader       = 7,
					.intersectionShader = 8
				}
			};

			unsigned int num_shader_groups = needs_beams ? LENGTH(rt_shader_group_info) : (needs_transparency ? SBT_RINT_BEAM : SBT_FIRST_TRANSPARENCY);

			VkPipelineLibraryCreateInfoKHR library_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR };
			// pt_clusters: a pipeline made without it must not meet a clustered BLAS (ray
			// queries need nothing but the device feature)
			VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV cluster_info = {
				.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV,
				.allowClusterAccelerationStructure = VK_TRUE,
			};
			VkRayTracingPipelineCreateInfoKHR rt_pipeline_info = {
				.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
				.pNext = qvk.clusters_enabled ? &cluster_info : NULL,
				.flags = 0,
				.stageCount = num_shader_stages,
				.pStages = shader_stages,
				.groupCount = num_shader_groups,
				.pGroups = rt_shader_group_info,
				.maxPipelineRayRecursionDepth = 1,
				.pLibraryInfo = &library_info,
				.pLibraryInterface = NULL,
				.pDynamicState = NULL,
				.layout = rt_pipeline_layout,
				.basePipelineHandle = VK_NULL_HANDLE,
				.basePipelineIndex = 0
			};

			assert(LENGTH(rt_shader_group_info) == SBT_ENTRIES_PER_PIPELINE);

			VkResult res = qvkCreateRayTracingPipelinesKHR(qvk.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rt_pipeline_info, NULL, &rt_pipelines[index]);
			
			if (res != VK_SUCCESS)
			{
				Com_EPrintf("Failed to create ray tracing pipeline #%d, vkCreateRayTracingPipelinesKHR error code is %s\n", index, qvk_result_to_string(res));
				return res;
			}

			_VK(qvkGetRayTracingShaderGroupHandlesKHR(
				qvk.device, rt_pipelines[index], 0, num_shader_groups,
				/* dataSize = */ num_shader_groups * shaderGroupHandleSize,
				/* pData = */ shader_handles + SBT_ENTRIES_PER_PIPELINE * shaderGroupHandleSize * index));
		}
	}

	if (qvk.use_ray_query)
	{
		// No SBT in RQ mode, just return
		return VK_SUCCESS;
	}

	// create the SBT buffer
	uint32_t shader_binding_table_size = num_shader_groups * shaderGroupBaseAlignment;
	_VK(buffer_create(&buf_shader_binding_table, shader_binding_table_size,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT));

	// copy/unpack the shader handles into the SBT:
	// shaderGroupBaseAlignment is likely greater than shaderGroupHandleSize (64 vs 32 on NV)
	char* shader_binding_table = (char*)buffer_map(&buf_shader_binding_table);
	for (uint32_t group = 0; group < num_shader_groups; group++)
	{
		memcpy(
			shader_binding_table + group * shaderGroupBaseAlignment,
			shader_handles + group * shaderGroupHandleSize,
			shaderGroupHandleSize);
	}
	buffer_unmap(&buf_shader_binding_table);
	shader_binding_table = NULL;

	return VK_SUCCESS;
}

VkResult
vkpt_pt_destroy_pipelines()
{
	buffer_destroy(&buf_shader_binding_table);

	for (pipeline_index_t index = 0; index < PIPELINE_COUNT; index++)
	{
		vkDestroyPipeline(qvk.device, rt_pipelines[index], NULL);
		rt_pipelines[index] = VK_NULL_HANDLE;
	}

	return VK_SUCCESS;
}
