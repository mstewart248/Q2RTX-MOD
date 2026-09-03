/*
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
#include "shader/froxel_shared.h"

static const uint32_t THREAD_GROUP_SIZE = 16;
static const uint32_t FILTER_THREAD_GROUP_SIZE = 16;

/*
===============================================================================

THE FROXEL VOLUMES live here rather than in a subsystem of their own, because
the grid IS this pass's cheap implementation: it needs the same shadow map and
the same TLAS binding, and god_rays_filter.comp is what consumes the result.
Giving it a parallel descriptor set, pool and pipeline layout would duplicate
all of that for nothing.

Two SCATTER volumes, ping-ponged by frame index. Frame i writes volume[i] and
SAMPLES volume[1-i] as its history, so the temporal blend needs no copy at all -
which is the whole reason there are two rather than a volume plus a copy step.
MAX_FRAMES_IN_FLIGHT is 2, so the parity works out exactly.

One INTEGRATED volume: it is written and consumed inside a single frame and is
never read across one, so it does not need doubling.

The volumes are view-frustum aligned and a fixed cell count, so unlike almost
everything else in this renderer they do NOT depend on the render resolution and
survive a vid_restart untouched.

===============================================================================
*/
typedef struct {
	VkImage image;
	VkImageView view;
	VkDeviceMemory memory;
} froxel_volume_t;

struct
{
	VkPipeline pipelines[6];
	VkPipelineLayout pipeline_layout;
	VkDescriptorSetLayout descriptor_set_layout;

	VkDescriptorPool descriptor_pool;
	// one per frame in flight: the TLAS handle is double-buffered, so the set
	// that names it has to be too. The froxel ping-pong rides the same index.
	VkDescriptorSet descriptor_set[MAX_FRAMES_IN_FLIGHT];

	VkImageView shadow_image_view;
	VkSampler shadow_sampler;

	froxel_volume_t froxel_scatter[MAX_FRAMES_IN_FLIGHT];
	froxel_volume_t froxel_integrated;

	/* THE RESERVOIR PAIR - ReSTIR's working set, and there have to be two.

	   froxel_reservoir.comp writes [0]; froxel_spatial.comp reads every
	   neighbour out of [0] while writing its own answer to [1]; the scatter pass
	   then reads [1]. Doing the middle step in place is a data race whose result
	   depends on dispatch order - some of a cell's neighbours would already have
	   been overwritten with THEIR combined value, so the reuse would compound
	   across the grid in whatever order the hardware happened to schedule.

	   Unlike the scatter pair these are NOT ping-ponged by frame: nothing here
	   crosses a frame boundary, so both are written and consumed inside one. */
	froxel_volume_t froxel_reservoir[2];

	VkSampler froxel_sampler;
	bool froxel_initialized;

	cvar_t* intensity;
	cvar_t* eccentricity;
	cvar_t* enable;
} god_rays;

// read by profiler.c to decide whether to show the fog rows
cvar_t* cvar_pt_fog_froxel = NULL;

// Gates the two extra dispatches on the C side. The shaders read the same cvar
// out of the UBO; this copy exists so the passes can be skipped entirely rather
// than dispatched to do nothing.
static cvar_t* cvar_pt_fog_restir = NULL;

enum {
	GOD_RAYS_PIPELINE_TRACE = 0,
	GOD_RAYS_PIPELINE_FILTER,
	GOD_RAYS_PIPELINE_FROXEL_SCATTER,
	GOD_RAYS_PIPELINE_FROXEL_INTEGRATE,
	GOD_RAYS_PIPELINE_FROXEL_RESERVOIR,
	GOD_RAYS_PIPELINE_FROXEL_SPATIAL,
};

static void create_froxel_volumes(void);
static void destroy_froxel_volumes(void);

static void create_image_views(void);
static void create_pipeline_layout(void);
static void create_pipelines(void);
static void create_descriptor_set(void);
static void update_descriptor_set(void);

extern cvar_t *physical_sky_space;

VkResult vkpt_initialize_god_rays(void)
{
	memset(&god_rays, 0, sizeof(god_rays));

	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(qvk.physical_device, &properties);

	// DEFAULTS OFF while the grid's in-scatter magnitude is still being
	// calibrated against the march - see the note in froxel_scatter.comp.
	// Same treatment cl_fog itself got: land it, default it off, tune in play.
	cvar_pt_fog_froxel = Cvar_Get("pt_fog_froxel", "0", CVAR_ARCHIVE);

	// Must match the UBO_CVAR_DO default in global_ubo.h - the shaders read that
	// copy, this one only decides whether to dispatch the two extra passes.
	cvar_pt_fog_restir = Cvar_Get("pt_fog_restir", "0", CVAR_ARCHIVE);

	god_rays.intensity = Cvar_Get("gr_intensity", "2.0", 0);
	god_rays.eccentricity = Cvar_Get("gr_eccentricity", "0.75", 0);
	god_rays.enable = Cvar_Get("gr_enable", "1", 0);

	return VK_SUCCESS;
}

VkResult vkpt_destroy_god_rays(void)
{
	destroy_froxel_volumes();

	vkDestroySampler(qvk.device, god_rays.shadow_sampler, NULL);
	vkDestroyDescriptorPool(qvk.device, god_rays.descriptor_pool, NULL);

	return VK_SUCCESS;
}

VkResult vkpt_god_rays_create_pipelines(void)
{
	create_pipeline_layout();
	create_pipelines();
	create_descriptor_set();

	// The froxel volumes are NOT created here.  They used to be, and that cost
	// every player ~21 MB of VRAM whether or not any fog was being rendered -
	// including with cl_fog 0 and pt_fog_froxel 0, which is why toggling cl_fog
	// appeared to change nothing.  On a card already tight with DLSS frame
	// generation that was enough to run out.  They are allocated on the first
	// frame the grid is actually wanted instead; see vkpt_froxel_ensure().

	// this is a noop outside a shader reload
	update_descriptor_set();

	return VK_SUCCESS;
}

VkResult vkpt_god_rays_destroy_pipelines(void)
{
	for (size_t i = 0; i < LENGTH(god_rays.pipelines); i++) {
		if (god_rays.pipelines[i]) {
			vkDestroyPipeline(qvk.device, god_rays.pipelines[i], NULL);
			god_rays.pipelines[i] = NULL;
		}
	}

	if (god_rays.pipeline_layout) {
		vkDestroyPipelineLayout(qvk.device, god_rays.pipeline_layout, NULL);
		god_rays.pipeline_layout = NULL;
	}
	
	if (god_rays.descriptor_set_layout) {
		vkDestroyDescriptorSetLayout(qvk.device, god_rays.descriptor_set_layout, NULL);
		god_rays.descriptor_set_layout = NULL;
	}

	// create_descriptor_set() allocates a fresh pool on every RELOAD_SHADER
	// init, so without this the old one leaked on each shader reload and each
	// renderer restart - and this pass gets several of those, see the spurious
	// re-init noted in the vsync work. Safe here: vkpt_destroy_all() idles the
	// device before calling us. NULLed so the destroy in vkpt_destroy_god_rays
	// (the DEFAULT group, which also runs on full shutdown) is a no-op.
	if (god_rays.descriptor_pool) {
		vkDestroyDescriptorPool(qvk.device, god_rays.descriptor_pool, NULL);
		god_rays.descriptor_pool = NULL;
		memset(god_rays.descriptor_set, 0, sizeof(god_rays.descriptor_set));
	}

	return VK_SUCCESS;
}

VkResult
vkpt_god_rays_update_images(void)
{
	create_image_views();
	update_descriptor_set();
	return VK_SUCCESS;
}

VkResult
vkpt_god_rays_noop(void)
{
	return VK_SUCCESS;
}

#define BARRIER_COMPUTE(cmd_buf, img) \
	do { \
		VkImageSubresourceRange subresource_range = { \
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT, \
			.baseMipLevel   = 0, \
			.levelCount     = 1, \
			.baseArrayLayer = 0, \
			.layerCount     = 1 \
		}; \
		IMAGE_BARRIER(cmd_buf, \
				.image            = img, \
				.subresourceRange = subresource_range, \
				.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT, \
				.oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
				.newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
		); \
	} while(0)

void vkpt_record_god_rays_trace_command_buffer(VkCommandBuffer command_buffer, int pass)
{
	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_PT_GODRAYS_THROUGHPUT_DIST]);
	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_ASVGF_COLOR]);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, god_rays.pipelines[0]);

	// Point this frame's set at this frame's TLAS. Writing set[idx] here is safe
	// because the frame fence guarantees the previous use of THAT set has
	// completed - the sets are per frame in flight for exactly this reason.
	{
		VkAccelerationStructureKHR tlas = vkpt_pt_get_geometry_tlas(qvk.current_frame_index);
		if (tlas != VK_NULL_HANDLE)
		{
			VkWriteDescriptorSetAccelerationStructureKHR as_info = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
				.accelerationStructureCount = 1,
				.pAccelerationStructures = &tlas
			};
			VkWriteDescriptorSet as_write = {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.pNext = &as_info,
				.dstSet = god_rays.descriptor_set[qvk.current_frame_index],
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
			};
			vkUpdateDescriptorSets(qvk.device, 1, &as_write, 0, NULL);
		}
	}

	VkDescriptorSet desc_sets[] = {
		god_rays.descriptor_set[qvk.current_frame_index],
		qvk.desc_set_vertex_buffer,
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};

	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, god_rays.pipeline_layout, 0, LENGTH(desc_sets),
		desc_sets, 0, NULL);

	vkCmdPushConstants(command_buffer, god_rays.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int), &pass);

	uint32_t group_size = THREAD_GROUP_SIZE * 2;
	uint32_t group_num_x = (qvk.gpu_slice_width + (group_size - 1)) / group_size;
	uint32_t group_num_y = (qvk.extent_render.height + (group_size - 1)) / group_size;

	vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_PT_GODRAYS_THROUGHPUT_DIST]);
	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_ASVGF_COLOR]);
}

/*
=================
vkpt_record_froxel_command_buffer

The two grid passes.  Runs BEFORE the god rays trace, because
god_rays_filter.comp reads the integrated volume this produces.

Barriers: the scatter pass writes volume[current] and reads volume[other], and
the integrate pass reads what scatter just wrote, so there is one full
shader-write to shader-read barrier between them.  The read of the OTHER volume
needs no barrier of its own - the frame fence already separates this frame from
the one that wrote it, which is the same argument that makes the per-frame
descriptor sets safe.
=================
*/
void vkpt_record_froxel_command_buffer(VkCommandBuffer command_buffer)
{
	VkDescriptorSet desc_sets[] = {
		god_rays.descriptor_set[qvk.current_frame_index],
		qvk.desc_set_vertex_buffer,
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};

	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
		god_rays.pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, NULL);

	int pass = 0;
	vkCmdPushConstants(command_buffer, god_rays.pipeline_layout,
		VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int), &pass);

	uint32_t group_num_x = (FROXEL_GRID_X + FROXEL_GROUP_X - 1) / FROXEL_GROUP_X;
	uint32_t group_num_y = (FROXEL_GRID_Y + FROXEL_GROUP_Y - 1) / FROXEL_GROUP_Y;

	VkMemoryBarrier mem_barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
	};

	/* --- ReSTIR, when it is on: choose the light before shading it ---

	   Two dispatches, neither of which traces a ray. The first draws each cell's
	   RIS candidates; the second lets a cell borrow which light its neighbours
	   drew. Both must complete for the whole grid before the next one starts -
	   spatial reuse reads cells other threads wrote - which is what the barriers
	   between them are for, and why this could not simply be more code inside
	   the scatter pass.

	   Skipped entirely at pt_fog_restir 0, and then the scatter pass takes its
	   own candidates exactly as it always has. The volumes still exist (the
	   descriptor has to be valid) but nothing reads or writes them. */
	if (cvar_pt_fog_restir && cvar_pt_fog_restir->integer)
	{
		BEGIN_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_RESERVOIR);

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			god_rays.pipelines[GOD_RAYS_PIPELINE_FROXEL_RESERVOIR]);

		vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

		END_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_RESERVOIR);

		vkCmdPipelineBarrier(command_buffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &mem_barrier, 0, NULL, 0, NULL);

		BEGIN_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_SPATIAL);

		vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
			god_rays.pipelines[GOD_RAYS_PIPELINE_FROXEL_SPATIAL]);

		vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

		END_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_SPATIAL);

		vkCmdPipelineBarrier(command_buffer,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 1, &mem_barrier, 0, NULL, 0, NULL);
	}

	// --- scatter: one thread per column of the volume, walking z ---
	BEGIN_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_SCATTER);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
		god_rays.pipelines[GOD_RAYS_PIPELINE_FROXEL_SCATTER]);

	vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

	END_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_SCATTER);

	// the integrate pass reads every cell the scatter pass just wrote
	vkCmdPipelineBarrier(command_buffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &mem_barrier, 0, NULL, 0, NULL);

	// --- integrate: the prefix sum along z ---
	BEGIN_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_INTEGRATE);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
		god_rays.pipelines[GOD_RAYS_PIPELINE_FROXEL_INTEGRATE]);

	group_num_x = (FROXEL_GRID_X + FROXEL_INT_GROUP_X - 1) / FROXEL_INT_GROUP_X;
	group_num_y = (FROXEL_GRID_Y + FROXEL_INT_GROUP_Y - 1) / FROXEL_INT_GROUP_Y;

	vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

	END_PERF_MARKER(command_buffer, PROFILER_FOG_FROXEL_INTEGRATE);

	// and the filter reads the integrated volume
	vkCmdPipelineBarrier(command_buffer,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 1, &mem_barrier, 0, NULL, 0, NULL);
}

/*
=================
vkpt_froxel_enabled

The grid only replaces the march for the MAP FOG (cl_fog 2).  The classic
campaign's sun shafts stay on god_rays.comp: they are a different effect with a
different failure mode - a shaft is a thin high-contrast feature that a 160x88
grid would visibly soften, where fog is exactly the low-frequency thing a grid
suits.
=================
*/
bool vkpt_froxel_enabled(void)
{
	if (!cvar_pt_fog_froxel || !cvar_pt_fog_froxel->integer)
		return false;

	if (!god_rays.froxel_initialized)
		return false;

	mapfog_params_t mf;
	return CL_GetMapFog(&mf) && mf.mode >= 2;
}

void vkpt_record_god_rays_filter_command_buffer(VkCommandBuffer command_buffer)
{
	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_PT_TRANSPARENT]);

	vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, god_rays.pipelines[1]);

	VkDescriptorSet desc_sets[] = {
		god_rays.descriptor_set[qvk.current_frame_index],
		qvk.desc_set_vertex_buffer,
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};

	vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, god_rays.pipeline_layout, 0, LENGTH(desc_sets),
		desc_sets, 0, NULL);

	int pass = 0;
	vkCmdPushConstants(command_buffer, god_rays.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(int), &pass);

	uint32_t group_size = FILTER_THREAD_GROUP_SIZE;
	uint32_t group_num_x = (qvk.gpu_slice_width + (group_size - 1)) / group_size;
	uint32_t group_num_y = (qvk.extent_render.height + (group_size - 1)) / group_size;

	vkCmdDispatch(command_buffer, group_num_x, group_num_y, 1);

	BARRIER_COMPUTE(command_buffer, qvk.images[VKPT_IMG_PT_TRANSPARENT]);
}

void vkpt_god_rays_prepare_ubo(
	QVKUniformBuffer_t * ubo,
	const aabb_t* world_aabb,
	const float* proj,
	const float* view, 
	const float* shadowmap_viewproj,
	float shadowmap_depth_scale)
{
	VectorAdd(world_aabb->mins, world_aabb->maxs, ubo->world_center);
	VectorScale(ubo->world_center, 0.5f, ubo->world_center);
	VectorSubtract(world_aabb->maxs, world_aabb->mins, ubo->world_size);
	VectorScale(ubo->world_size, 0.5f, ubo->world_half_size_inv);
	ubo->world_half_size_inv[0] = 1.f / ubo->world_half_size_inv[0];
	ubo->world_half_size_inv[1] = 1.f / ubo->world_half_size_inv[1];
	ubo->world_half_size_inv[2] = 1.f / ubo->world_half_size_inv[2];
	ubo->shadow_map_depth_scale = shadowmap_depth_scale;

	ubo->god_rays_intensity = max(0.f, god_rays.intensity->value);
	ubo->god_rays_eccentricity = god_rays.eccentricity->value;

	// allocate the froxel volumes if this is the first frame that wants them
	vkpt_froxel_ensure();

	// Shadow parameters
	memcpy(ubo->shadow_map_VP, shadowmap_viewproj, 16 * sizeof(float));
}

static void create_image_views(void)
{
	god_rays.shadow_image_view = vkpt_shadow_map_get_view();

	VkSamplerReductionModeCreateInfo redutcion_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
		.reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN
	};

	const VkSamplerCreateInfo sampler_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = &redutcion_create_info,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
	};

	_VK(vkCreateSampler(qvk.device, &sampler_create_info, NULL, &god_rays.shadow_sampler));
}

static void create_pipeline_layout(void)
{
	VkDescriptorSetLayoutBinding bindings[8] = { 0 };
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	// The opaque-geometry TLAS, so the volumetric fog can trace real
	// sky-visibility rays. It gets its own binding here, with a COMPUTE stage
	// flag, rather than borrowing the path tracer's ray tracing descriptor set -
	// that one declares raygen/any-hit stages unless qvk.use_ray_query is set,
	// which would be invalid to bind to a compute pipeline.
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	// The froxel grid. 2 is written by the scatter pass and read by integrate;
	// 3 is the PREVIOUS frame's scatter volume, sampled for the temporal blend;
	// 4 is the integrated result, written by integrate and sampled by the
	// filter. All three are on this set because every froxel shader also needs
	// the shadow map and the TLAS above.
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[3].binding = 3;
	bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[3].descriptorCount = 1;
	bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[4].binding = 4;
	bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[4].descriptorCount = 1;
	bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	// The SAME integrated volume again, as a sampled image this time, so the
	// filter can fetch it trilinearly. A storage image can only be read
	// unfiltered, and reading it that way is visible: the grid is 160x88, so a
	// nearest fetch spreads one cell over a ~12x12 block of screen pixels and
	// the fog comes out in lumps. Interpolating between cells is what makes a
	// grid this coarse read as smooth fog rather than as a grid.
	bindings[5].binding = 5;
	bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[5].descriptorCount = 1;
	bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	/* The ReSTIR reservoir pair. 6 is what froxel_reservoir.comp writes and
	   froxel_spatial.comp reads its neighbours from; 7 is what the spatial pass
	   writes and froxel_scatter.comp shades from.

	   Plain storage images, no sampler, because every access is a point load of
	   one cell. Interpolating a reservoir would be meaningless - .x is a light
	   INDEX, and the average of light 12 and light 46 is light 29. */
	bindings[6].binding = 6;
	bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[6].descriptorCount = 1;
	bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	bindings[7].binding = 7;
	bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[7].descriptorCount = 1;
	bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	const VkDescriptorSetLayoutCreateInfo set_layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(bindings),
		.pBindings = bindings
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &set_layout_create_info, NULL,
		&god_rays.descriptor_set_layout));


	VkDescriptorSetLayout desc_set_layouts[] = {
		god_rays.descriptor_set_layout,
		qvk.desc_set_layout_vertex_buffer,
		qvk.desc_set_layout_ubo,
		qvk.desc_set_layout_textures
	};

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = sizeof(int),
	};

	const VkPipelineLayoutCreateInfo layout_create_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pSetLayouts = desc_set_layouts,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_constant_range
	};

	_VK(vkCreatePipelineLayout(qvk.device, &layout_create_info, NULL,
		&god_rays.pipeline_layout));
}

static void create_pipelines(void)
{
	const VkPipelineShaderStageCreateInfo shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_GOD_RAYS_COMP],
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo filter_shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_GOD_RAYS_FILTER_COMP],
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo froxel_scatter_shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_FROXEL_SCATTER_COMP],
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo froxel_integrate_shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_FROXEL_INTEGRATE_COMP],
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo froxel_reservoir_shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_FROXEL_RESERVOIR_COMP],
		.pName = "main"
	};

	const VkPipelineShaderStageCreateInfo froxel_spatial_shader = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = qvk.shader_modules[QVK_MOD_FROXEL_SPATIAL_COMP],
		.pName = "main"
	};

	const VkComputePipelineCreateInfo pipeline_create_infos[6] = {
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = shader,
			.layout = god_rays.pipeline_layout
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = filter_shader,
			.layout = god_rays.pipeline_layout
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = froxel_scatter_shader,
			.layout = god_rays.pipeline_layout
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = froxel_integrate_shader,
			.layout = god_rays.pipeline_layout
		},
		// The two ReSTIR passes. Order here must match the GOD_RAYS_PIPELINE_*
		// enum, which appends them after the four that already existed so no
		// index moves.
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = froxel_reservoir_shader,
			.layout = god_rays.pipeline_layout
		},
		{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = froxel_spatial_shader,
			.layout = god_rays.pipeline_layout
		},
	};

	_VK(vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, LENGTH(pipeline_create_infos), pipeline_create_infos,
		NULL, god_rays.pipelines));
}

static void create_descriptor_set(void)
{
	const VkDescriptorPoolSize pool_sizes[] = {
		// three samplers per set: the shadow map, the froxel history, and the
		// integrated volume the filter reads
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_FRAMES_IN_FLIGHT * 3 },
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, MAX_FRAMES_IN_FLIGHT },
		// the scatter volume this frame writes, the integrated volume, and the
		// two ReSTIR reservoirs
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, MAX_FRAMES_IN_FLIGHT * 4 }
	};

	const VkDescriptorPoolCreateInfo pool_create_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = MAX_FRAMES_IN_FLIGHT,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes = pool_sizes
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_create_info, NULL, &god_rays.descriptor_pool));

	VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		layouts[i] = god_rays.descriptor_set_layout;

	const VkDescriptorSetAllocateInfo allocate_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = god_rays.descriptor_pool,
		.descriptorSetCount = MAX_FRAMES_IN_FLIGHT,
		.pSetLayouts = layouts
	};

	_VK(vkAllocateDescriptorSets(qvk.device, &allocate_info, god_rays.descriptor_set));
}


static void update_descriptor_set(void)
{
	// if we end up here during init before we've called create_image_views(), punt --- we will be called again later
	if (god_rays.shadow_image_view == NULL)
		return;

	VkDescriptorImageInfo image_info = {
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.imageView = god_rays.shadow_image_view,
		.sampler = god_rays.shadow_sampler
	};
	
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		VkWriteDescriptorSet writes[7] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = god_rays.descriptor_set[i],
				.descriptorCount = 1,
				.dstBinding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_info
			}
		};
		uint32_t num_writes = 1;

		// THE PING-PONG. Set i writes scatter volume i and SAMPLES volume 1-i
		// as its history, so the temporal blend costs no copy at all - which is
		// the whole reason there are two volumes rather than one plus a copy.
		// MAX_FRAMES_IN_FLIGHT is 2, so the parity works out exactly.
		VkDescriptorImageInfo froxel_write_info;
		VkDescriptorImageInfo froxel_history_info;
		VkDescriptorImageInfo froxel_integrated_info;
		VkDescriptorImageInfo froxel_sampled_info;
		VkDescriptorImageInfo froxel_reservoir_info[2];

		if (god_rays.froxel_initialized)
		{
			froxel_write_info = (VkDescriptorImageInfo) {
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
				.imageView = god_rays.froxel_scatter[i].view
			};
			froxel_history_info = (VkDescriptorImageInfo) {
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
				.imageView = god_rays.froxel_scatter[(i + 1) % MAX_FRAMES_IN_FLIGHT].view,
				.sampler = god_rays.froxel_sampler
			};
			froxel_integrated_info = (VkDescriptorImageInfo) {
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
				.imageView = god_rays.froxel_integrated.view
			};
			froxel_sampled_info = (VkDescriptorImageInfo) {
				.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
				.imageView = god_rays.froxel_integrated.view,
				.sampler = god_rays.froxel_sampler
			};

			writes[num_writes++] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = god_rays.descriptor_set[i],
				.descriptorCount = 1,
				.dstBinding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo = &froxel_write_info
			};
			writes[num_writes++] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = god_rays.descriptor_set[i],
				.descriptorCount = 1,
				.dstBinding = 3,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &froxel_history_info
			};
			writes[num_writes++] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = god_rays.descriptor_set[i],
				.descriptorCount = 1,
				.dstBinding = 4,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
				.pImageInfo = &froxel_integrated_info
			};
			writes[num_writes++] = (VkWriteDescriptorSet) {
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = god_rays.descriptor_set[i],
				.descriptorCount = 1,
				.dstBinding = 5,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &froxel_sampled_info
			};

			/* The reservoirs are written UNCONDITIONALLY, not only when ReSTIR
			   is switched on, and that is a Vulkan requirement rather than
			   tidiness: froxel_scatter.comp declares binding 7 and references it
			   inside a branch, and a descriptor that a shader statically uses
			   must be valid even when the branch is never taken. Leaving them
			   unwritten is undefined behaviour that happens to work until a
			   driver decides otherwise. So the volumes are created with the rest
			   of the grid and these two writes always happen; pt_fog_restir only
			   decides whether the passes that fill them are dispatched. */
			for (int r = 0; r < 2; r++)
			{
				froxel_reservoir_info[r] = (VkDescriptorImageInfo) {
					.imageLayout = VK_IMAGE_LAYOUT_GENERAL,
					.imageView = god_rays.froxel_reservoir[r].view
				};

				writes[num_writes++] = (VkWriteDescriptorSet) {
					.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					.dstSet = god_rays.descriptor_set[i],
					.descriptorCount = 1,
					.dstBinding = 6 + r,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
					.pImageInfo = &froxel_reservoir_info[r]
				};
			}
		}

		vkUpdateDescriptorSets(qvk.device, num_writes, writes, 0, NULL);
	}
}

/*
=================
create_froxel_volumes

The three 3D images the grid lives in.  Unlike everything in qvk.images these
are NOT tied to the render resolution - a froxel is addressed in NDC - so they
are created once and a vid_restart leaves them alone.

They stay in VK_IMAGE_LAYOUT_GENERAL for their whole life.  A storage image
that is also sampled has to be in GENERAL for the store, and bouncing to
SHADER_READ_ONLY_OPTIMAL and back between the two uses would buy nothing here.
=================
*/
static void create_froxel_volumes(void)
{
	if (god_rays.froxel_initialized)
		return;

	enum { MAX_FROXEL_VOLUMES = MAX_FRAMES_IN_FLIGHT + 1 + 2 };

	VkImage* image_slots[MAX_FROXEL_VOLUMES];
	VkImageView* views[MAX_FROXEL_VOLUMES];
	VkDeviceMemory* memories[MAX_FROXEL_VOLUMES];
	VkFormat formats[MAX_FROXEL_VOLUMES];
	int num_volumes = 0;
	VkDeviceSize total_bytes = 0;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		image_slots[num_volumes] = &god_rays.froxel_scatter[i].image;
		views[num_volumes] = &god_rays.froxel_scatter[i].view;
		memories[num_volumes] = &god_rays.froxel_scatter[i].memory;
		formats[num_volumes] = VK_FORMAT_R16G16B16A16_SFLOAT;
		num_volumes++;
	}
	image_slots[num_volumes] = &god_rays.froxel_integrated.image;
	views[num_volumes] = &god_rays.froxel_integrated.view;
	memories[num_volumes] = &god_rays.froxel_integrated.memory;
	formats[num_volumes] = VK_FORMAT_R16G16B16A16_SFLOAT;
	num_volumes++;

	/* THE RESERVOIRS ARE RGBA32F, AND HALVING THAT WOULD BREAK THEM.

	   .x holds a light INDEX and fp16 represents integers exactly only to 2048;
	   a map with more lights than that in one cluster list would start choosing
	   the wrong light, silently and only on that map. .y holds the RIS weight W,
	   whose entire job is to be large exactly when the target function was
	   small, so it has the widest dynamic range of anything in the grid.

	   The scatter volume gets away with fp16 only because FROXEL_STORAGE_SCALE
	   was added to drag it out of the subnormals after that cost a long hunt;
	   there is no equivalent trick available here, because the four channels do
	   not share a scale. */
	for (int r = 0; r < 2; r++)
	{
		image_slots[num_volumes] = &god_rays.froxel_reservoir[r].image;
		views[num_volumes] = &god_rays.froxel_reservoir[r].view;
		memories[num_volumes] = &god_rays.froxel_reservoir[r].memory;
		formats[num_volumes] = VK_FORMAT_R32G32B32A32_SFLOAT;
		num_volumes++;
	}

	for (int i = 0; i < num_volumes; i++)
	{
		VkImageCreateInfo img_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_3D,
			.format = formats[i],
			.extent = { FROXEL_GRID_X, FROXEL_GRID_Y, FROXEL_GRID_Z },
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
			       | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
		};

		_VK(vkCreateImage(qvk.device, &img_info, NULL, image_slots[i]));

		VkMemoryRequirements mem_req;
		vkGetImageMemoryRequirements(qvk.device, *image_slots[i], &mem_req);

		VkMemoryAllocateInfo mem_alloc_info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = mem_req.size,
			.memoryTypeIndex = get_memory_type(mem_req.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
		};

		_VK(vkAllocateMemory(qvk.device, &mem_alloc_info, NULL, memories[i]));
		_VK(vkBindImageMemory(qvk.device, *image_slots[i], *memories[i], 0));

		total_bytes += mem_alloc_info.allocationSize;

		VkImageViewCreateInfo view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = *image_slots[i],
			.viewType = VK_IMAGE_VIEW_TYPE_3D,
			.format = formats[i],
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			}
		};

		_VK(vkCreateImageView(qvk.device, &view_info, NULL, views[i]));
	}

	// LINEAR, because the reprojected history lookup lands between cells and
	// nearest would quantise the temporal blend straight back into a grid.
	// CLAMP_TO_BORDER with a transparent-black border so a sample that walks off
	// the volume reads as "no history" rather than as a smeared edge cell - the
	// shader's own frustum test should catch that first, but the border is what
	// makes a miss harmless when it does not.
	const VkSamplerCreateInfo sampler_create_info = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
		.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
	};
	_VK(vkCreateSampler(qvk.device, &sampler_create_info, NULL, &god_rays.froxel_sampler));

	// Move all three out of UNDEFINED and zero them, once.  The first frame's
	// history read happens before anything has written that volume, and a
	// storage image read in UNDEFINED is invalid - not merely undefined content.
	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	for (int i = 0; i < num_volumes; i++)
	{
		VkImageSubresourceRange subresource_range = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1
		};

		IMAGE_BARRIER(cmd_buf,
			.image = *image_slots[i],
			.subresourceRange = subresource_range,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_GENERAL
		);

		VkClearColorValue clear = { .float32 = { 0, 0, 0, 0 } };
		vkCmdClearColorImage(cmd_buf, *image_slots[i], VK_IMAGE_LAYOUT_GENERAL,
			&clear, 1, &subresource_range);
	}

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, true);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	// Say what this cost.  A 3D image in OPTIMAL tiling is padded, so print what
	// the driver actually charged rather than what the maths predicts. The two
	// RGBA32F reservoirs are the larger half of this now - they are allocated
	// whether or not pt_fog_restir is on, because the scatter pass names one of
	// them in a descriptor either way.
	Com_Printf("froxel grid: %d volumes of %dx%dx%d, %.1f MB of VRAM\n",
		num_volumes, FROXEL_GRID_X, FROXEL_GRID_Y, FROXEL_GRID_Z,
		(double)total_bytes / (1024.0 * 1024.0));

	god_rays.froxel_initialized = true;
}

/*
=================
vkpt_froxel_ensure

Called once per frame from vkpt_god_rays_prepare_ubo, which runs BEFORE any
command buffer is opened - create_froxel_volumes submits and then idles the
graphics queue, so it cannot be called from inside a recording block.

Allocation is deferred to the first frame the grid is genuinely wanted.  With
pt_fog_froxel at its default of 0 the volumes are never created at all and the
feature costs nothing but code.

Freeing again when the grid is switched back OFF is deliberately NOT done here:
the descriptor sets name these views, and rewriting a set that a frame in flight
may still be reading means idling the device mid-frame.  So the memory is held
until the renderer is torn down.  Turning the grid on is a one-way decision for
the session; turning it off stops the passes running but does not give the VRAM
back until a vid_restart.
=================
*/
void vkpt_froxel_ensure(void)
{
	if (god_rays.froxel_initialized)
		return;

	if (!cvar_pt_fog_froxel || !cvar_pt_fog_froxel->integer)
		return;

	mapfog_params_t mf;
	if (!CL_GetMapFog(&mf) || mf.mode < 2)
		return;

	create_froxel_volumes();

	// the sets were written without bindings 2-5; they exist now
	update_descriptor_set();
}

static void destroy_froxel_volumes(void)
{
	if (!god_rays.froxel_initialized)
		return;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		vkDestroyImageView(qvk.device, god_rays.froxel_scatter[i].view, NULL);
		vkDestroyImage(qvk.device, god_rays.froxel_scatter[i].image, NULL);
		vkFreeMemory(qvk.device, god_rays.froxel_scatter[i].memory, NULL);
	}

	vkDestroyImageView(qvk.device, god_rays.froxel_integrated.view, NULL);
	vkDestroyImage(qvk.device, god_rays.froxel_integrated.image, NULL);
	vkFreeMemory(qvk.device, god_rays.froxel_integrated.memory, NULL);

	for (int r = 0; r < 2; r++)
	{
		vkDestroyImageView(qvk.device, god_rays.froxel_reservoir[r].view, NULL);
		vkDestroyImage(qvk.device, god_rays.froxel_reservoir[r].image, NULL);
		vkFreeMemory(qvk.device, god_rays.froxel_reservoir[r].memory, NULL);
	}

	vkDestroySampler(qvk.device, god_rays.froxel_sampler, NULL);

	memset(god_rays.froxel_scatter, 0, sizeof(god_rays.froxel_scatter));
	memset(&god_rays.froxel_integrated, 0, sizeof(god_rays.froxel_integrated));
	memset(god_rays.froxel_reservoir, 0, sizeof(god_rays.froxel_reservoir));
	god_rays.froxel_sampler = VK_NULL_HANDLE;
	god_rays.froxel_initialized = false;
}

bool vkpt_god_rays_enabled(const sun_light_t* sun_light)
{
	if (physical_sky_space->integer)
		return false;   // looks wrong in space - the rays appear outside the station too

	// cl_fog 2: the volumetric is lit by the map's own local lights, so it has to
	// run with no sun at all. NONE of the rerelease maps have a sun - their own
	// skyboxes contain none and sky_use_map_skybox is on by default - so gating
	// this pass on sun_light->visible made the fog dead code on every one of them.
	{
		mapfog_params_t mf;
		if (CL_GetMapFog(&mf) && mf.mode >= 2)
			return true;
	}

	return god_rays.enable->integer
		&& god_rays.intensity->value > 0.f
		&& sun_light->visible;
}
