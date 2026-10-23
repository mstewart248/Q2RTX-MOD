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

SOUND TRACE

client/sound/steamaudio.c's reflections, traced on the GPU when
s_reflections_gpu is on. Steam Audio hands each run of its real-time
reflection simulator to steamaudio.c (a hook the phonon.dll that
setup/build_phonon.py builds has), which hands it here through R_SoundTrace.
What comes back is what the CPU would have made - an energy field per
source - and Steam Audio does the rest as before. The algorithm is in
sound_trace.comp.

And the ways round corners (s_diffraction): steamaudio.c's pathing runs ask,
for each hidden voice, what comes round the edges of the world, which
sound_diffract.comp works out. A run may ask for either or both.

THE SCENE is the sound's own, not the renderer's: steamaudio.c's copy of the
map's faces with its materials, without the sky, fences and grates, and the
world's edges; a BLAS per BSP model, and a TLAS rebuilt every run from where
steamaudio.c last placed the brush entities. A run with a geometry key not
seen before builds it, and so does the first run after a vid_restart.

THREADS. All but the setup and teardown runs on the sound's simulation
thread, which submits to a queue no other thread uses (qvk.queue_sound) and
waits for it. snd_lock keeps that apart from the renderer: the teardown and a
shader reload take it, and so does vkpt_device_wait_idle, because
vkDeviceWaitIdle must not run while another thread submits to any queue. The
lock is never freed: the sound may ask after the renderer has gone.

Nothing here uses the zone or vk_util's buffer registry, which are the main
thread's.

==============================================================================
*/

#include "shared/shared.h"
#include "common/common.h"
#include "vkpt.h"
#include "vk_util.h"
#include "shader/sound_trace_shared.h"

#include <SDL.h>

typedef struct {
	VkBuffer        buffer;
	VkDeviceMemory  memory;
	VkDeviceAddress address;
	void            *mapped;        // the host-visible ones (all coherent)
} snd_buffer_t;

typedef struct {
	VkAccelerationStructureKHR  accel;
	VkDeviceSize                offset, size;   // in snd.blas_mem
	VkDeviceAddress             address;
	uint32_t                    first_tri;  // its triangles' place in snd.tris
} snd_blas_t;

// sound_trace.comp's SoundTraceParams, std140
typedef struct {
	float       listener[4];
	float       sources[SOUND_TRACE_MAX_SOURCES][4];
	float       materials[SOUND_TRACE_MAX_MATERIALS][4];
	uint32_t    counts[4];
	uint32_t    counts2[4];
	float       dists[4];
	float       shift[4];
	float       queries[SOUND_TRACE_MAX_QUERIES][4];
	uint32_t    counts3[4];
} snd_params_t;

// and its SoundTri
typedef struct {
	float       normal[3];
	uint32_t    material;
} snd_tri_t;

#define SND_FIELD_FLOATS    (SOUND_TRACE_CHANNELS * SOUND_TRACE_BANDS * SOUND_TRACE_MAX_BINS)
#define SND_FIELDS_SIZE     ((VkDeviceSize)SOUND_TRACE_MAX_SOURCES * SND_FIELD_FLOATS * sizeof(float))
#define SND_DIFF_SIZE       ((VkDeviceSize)SOUND_TRACE_MAX_QUERIES * SOUND_DIFFRACT_FLOATS * sizeof(float))

static_assert(SOUND_EDGE_FLOATS == R_SOUND_EDGE_FLOATS && SOUND_DIFFRACT_FLOATS == R_SOUND_DIFFRACTION_FLOATS,
              "sound_trace_shared.h and refresh.h disagree");
#define SND_WAIT_NSEC       2000000000ull
#define SND_GROUPS_WANTED   32      // per source, which sets the rays per thread

// never freed, and kept out of snd, which a setup clears under it
static SDL_mutex    *snd_lock;

static struct {
	bool            up;             // set up on this device; the rest under snd_lock
	const char      *why;           // why there is no tracing, NULL = there is

	VkCommandPool   pool;
	VkCommandBuffer cmd;
	VkFence         fence;

	VkDescriptorSetLayout   set_layout;
	VkDescriptorPool        desc_pool;
	VkDescriptorSet         set;
	VkPipelineLayout        pipeline_layout;
	VkPipeline              pipeline;
	VkPipeline              diff_pipeline;

	snd_buffer_t    params, fields, readback;
	snd_buffer_t    diff, diff_readback;
	VkDeviceSize    scratch_align;

	// the scene, of the geometry with this key; 0 = none
	unsigned        key;
	unsigned        failed_key;     // one whose scene could not be built: not tried again
	int             num_meshes;
	snd_blas_t      *blas;
	snd_buffer_t    blas_mem;       // all of them: a map has hundreds of brush models
	snd_buffer_t    tris;
	snd_buffer_t    edges;          // at least one, for the descriptor
	int             num_edges;
	VkAccelerationStructureKHR tlas;
	snd_buffer_t    tlas_mem, instances, scratch;
	VkDeviceAddress scratch_address;
} snd;

void vkpt_sound_lock(void)
{
	if (snd_lock)
		SDL_LockMutex(snd_lock);
}

void vkpt_sound_unlock(void)
{
	if (snd_lock)
		SDL_UnlockMutex(snd_lock);
}

static int snd_memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < qvk.mem_properties.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (qvk.mem_properties.memoryTypes[i].propertyFlags & want) == want)
			return (int)i;
	return -1;
}

static void snd_buffer_destroy(snd_buffer_t *b)
{
	if (b->mapped)
		vkUnmapMemory(qvk.device, b->memory);
	if (b->buffer)
		vkDestroyBuffer(qvk.device, b->buffer, NULL);
	if (b->memory)
		vkFreeMemory(qvk.device, b->memory, NULL);
	memset(b, 0, sizeof(*b));
}

// `prefer` is tried on top of `need` first
static bool snd_buffer_create(snd_buffer_t *b, VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryAllocateFlagsInfo flags = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
	};
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &flags,
	};
	VkMemoryRequirements req;
	int type;

	memset(b, 0, sizeof(*b));
	if (vkCreateBuffer(qvk.device, &info, NULL, &b->buffer) != VK_SUCCESS) {
		b->buffer = VK_NULL_HANDLE;
		return false;
	}

	vkGetBufferMemoryRequirements(qvk.device, b->buffer, &req);
	type = snd_memory_type(req.memoryTypeBits, need | prefer);
	if (type < 0)
		type = snd_memory_type(req.memoryTypeBits, need);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = type;

	if (type < 0 || vkAllocateMemory(qvk.device, &alloc, NULL, &b->memory) != VK_SUCCESS) {
		b->memory = VK_NULL_HANDLE;
		goto fail;
	}
	if (vkBindBufferMemory(qvk.device, b->buffer, b->memory, 0) != VK_SUCCESS)
		goto fail;
	if ((need & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
	    vkMapMemory(qvk.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS) {
		b->mapped = NULL;
		goto fail;
	}

	b->address = get_buffer_device_address(b->buffer);
	return true;

fail:
	snd_buffer_destroy(b);
	return false;
}

static void snd_barrier(VkPipelineStageFlags src_stage, VkAccessFlags src,
                        VkPipelineStageFlags dst_stage, VkAccessFlags dst)
{
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = src,
		.dstAccessMask = dst,
	};

	vkCmdPipelineBarrier(snd.cmd, src_stage, dst_stage, 0, 1, &barrier, 0, NULL, 0, NULL);
}

static bool snd_begin(void)
{
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	if (vkBeginCommandBuffer(snd.cmd, &begin) != VK_SUCCESS)
		return false;

	// what the last submit wrote, seen by this one
	snd_barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	return true;
}

// Submits and waits. A GPU that has not finished in SND_WAIT_NSEC is hung or
// lost; once it is done with what it has, nothing is submitted here again.
static bool snd_submit(void)
{
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &snd.cmd,
	};

	if (vkEndCommandBuffer(snd.cmd) != VK_SUCCESS)
		return false;
	if (vkQueueSubmit(qvk.queue_sound, 1, &submit, snd.fence) != VK_SUCCESS) {
		snd.why = "the GPU refused a run";
		return false;
	}
	if (vkWaitForFences(qvk.device, 1, &snd.fence, VK_TRUE, SND_WAIT_NSEC) != VK_SUCCESS) {
		vkQueueWaitIdle(qvk.queue_sound);
		snd.why = "the GPU did not finish a run";
		return false;
	}
	vkResetFences(qvk.device, 1, &snd.fence);
	return true;
}

static void snd_write_buffer(uint32_t binding, VkDescriptorType type, const snd_buffer_t *b, VkDeviceSize range)
{
	VkDescriptorBufferInfo info = { .buffer = b->buffer, .offset = 0, .range = range };
	VkWriteDescriptorSet write = {
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = snd.set,
		.dstBinding = binding,
		.descriptorCount = 1,
		.descriptorType = type,
		.pBufferInfo = &info,
	};

	vkUpdateDescriptorSets(qvk.device, 1, &write, 0, NULL);
}

static void snd_free_scene(void)
{
	int i;

	for (i = 0; i < snd.num_meshes; i++)
		if (snd.blas[i].accel)
			qvkDestroyAccelerationStructureKHR(qvk.device, snd.blas[i].accel, NULL);
	free(snd.blas);
	snd.blas = NULL;
	snd.num_meshes = 0;
	snd_buffer_destroy(&snd.blas_mem);

	if (snd.tlas)
		qvkDestroyAccelerationStructureKHR(qvk.device, snd.tlas, NULL);
	snd.tlas = VK_NULL_HANDLE;
	snd_buffer_destroy(&snd.tlas_mem);
	snd_buffer_destroy(&snd.instances);
	snd_buffer_destroy(&snd.scratch);
	snd_buffer_destroy(&snd.tris);
	snd_buffer_destroy(&snd.edges);
	snd.num_edges = 0;
	snd.key = 0;
}

// an acceleration structure in `mem` at `offset`, a multiple of 256
static bool snd_create_accel(VkAccelerationStructureTypeKHR type, const snd_buffer_t *mem,
                             VkDeviceSize offset, VkDeviceSize size, VkAccelerationStructureKHR *accel)
{
	VkAccelerationStructureCreateInfoKHR info = {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		.buffer = mem->buffer,
		.offset = offset,
		.size = size,
		.type = type,
	};

	if (qvkCreateAccelerationStructureKHR(qvk.device, &info, NULL, accel) != VK_SUCCESS) {
		*accel = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

static void snd_instance_geometry(VkAccelerationStructureGeometryKHR *geom)
{
	memset(geom, 0, sizeof(*geom));
	geom->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom->geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom->geometry.instances.arrayOfPointers = VK_FALSE;
	geom->geometry.instances.data.deviceAddress = snd.instances.address;
}

// a mesh's triangles as a BLAS's geometry, out of the build's input buffers
static void snd_mesh_geometry(VkAccelerationStructureGeometryKHR *geom, const snd_buffer_t *verts,
                              const snd_buffer_t *indices, size_t first_vert, size_t first_tri, int num_verts)
{
	memset(geom, 0, sizeof(*geom));
	geom->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geom->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geom->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geom->geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geom->geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geom->geometry.triangles.vertexData.deviceAddress = verts->address + first_vert * 3 * sizeof(float);
	geom->geometry.triangles.vertexStride = 3 * sizeof(float);
	geom->geometry.triangles.maxVertex = num_verts - 1;
	geom->geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	geom->geometry.triangles.indexData.deviceAddress = indices->address + first_tri * 3 * sizeof(uint32_t);
}

static void snd_build_info(VkAccelerationStructureBuildGeometryInfoKHR *info, VkAccelerationStructureTypeKHR type,
                           const VkAccelerationStructureGeometryKHR *geom)
{
	memset(info, 0, sizeof(*info));
	info->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info->type = type;
	info->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	info->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info->geometryCount = 1;
	info->pGeometries = geom;
}

/*
The scene of `geom`: its triangles' normals and materials for the shader,
its edges, a BLAS per mesh with faces, and a TLAS with room for every mesh at
once. The build's vertex and index buffers are dropped once it is done.
*/
static bool snd_build_scene(const r_sound_geom_t *geom)
{
	snd_buffer_t    verts = { 0 }, indices = { 0 }, staging = { 0 }, edge_staging = { 0 };
	size_t          *first_vert = NULL;
	size_t          nv = 0, nt = 0, v0, t0;
	size_t          edges_size = (size_t)max(geom->num_edges, 1) * SOUND_EDGE_FLOATS * sizeof(float);
	VkDeviceSize    scratch = 0, blas_size = 0;
	VkAccelerationStructureGeometryKHR          ag;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR    sizes;
	VkAccelerationStructureBuildRangeInfoKHR    range = { 0 };
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkBufferUsageFlags input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	bool    ok = false, submitted = false;
	int     i, j, k;

	snd_free_scene();

	for (i = 0; i < geom->num_meshes; i++) {
		nv += geom->meshes[i].num_verts;
		nt += geom->meshes[i].num_tris;
	}
	// the TLAS instance's custom index, where a mesh's triangles start, is 24 bits
	if (!nt || nt >= (1u << 24))
		return false;

	snd.blas = calloc(geom->num_meshes, sizeof(*snd.blas));
	first_vert = calloc(geom->num_meshes, sizeof(*first_vert));
	if (!snd.blas || !first_vert)
		goto done;
	snd.num_meshes = geom->num_meshes;

	if (!snd_buffer_create(&verts, nv * 3 * sizeof(float), input, host, 0) ||
	    !snd_buffer_create(&indices, nt * 3 * sizeof(uint32_t), input, host, 0) ||
	    !snd_buffer_create(&staging, nt * sizeof(snd_tri_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, 0) ||
	    !snd_buffer_create(&snd.tris, nt * sizeof(snd_tri_t),
	                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0) ||
	    !snd_buffer_create(&edge_staging, edges_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, 0) ||
	    !snd_buffer_create(&snd.edges, edges_size,
	                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0))
		goto done;

	memset(edge_staging.mapped, 0, edges_size);
	if (geom->num_edges > 0)
		memcpy(edge_staging.mapped, geom->edges, (size_t)geom->num_edges * SOUND_EDGE_FLOATS * sizeof(float));
	snd.num_edges = max(geom->num_edges, 0);

	// the vertices as they are, the indices per mesh, and each triangle's
	// normal (either way: the shader turns it to face the ray) and material
	for (i = 0, v0 = 0, t0 = 0; i < geom->num_meshes; i++) {
		const r_sound_mesh_t *m = &geom->meshes[i];
		float       *v = (float *)verts.mapped + v0 * 3;
		uint32_t    *ix = (uint32_t *)indices.mapped + t0 * 3;
		snd_tri_t   *st = (snd_tri_t *)staging.mapped + t0;

		if (m->num_verts)
			memcpy(v, m->verts, m->num_verts * 3 * sizeof(float));
		for (j = 0; j < m->num_tris; j++) {
			const float *p[3];
			vec3_t      e1, e2, n;

			for (k = 0; k < 3; k++) {
				int index = m->tris[j * 3 + k];
				if (index < 0 || index >= m->num_verts)
					goto done;
				ix[j * 3 + k] = index;
				p[k] = m->verts + index * 3;
			}
			VectorSubtract(p[1], p[0], e1);
			VectorSubtract(p[2], p[0], e2);
			CrossProduct(e1, e2, n);
			if (VectorNormalize(n) == 0)
				VectorSet(n, 0, 1, 0);
			VectorCopy(n, st[j].normal);
			st[j].material = m->mats[j] >= 0 && m->mats[j] < SOUND_TRACE_MAX_MATERIALS ? m->mats[j] : 0;
		}

		first_vert[i] = v0;
		snd.blas[i].first_tri = (uint32_t)t0;
		v0 += m->num_verts;
		t0 += m->num_tris;
	}

	// a BLAS per mesh with faces, all in one buffer
	for (i = 0; i < geom->num_meshes; i++) {
		const r_sound_mesh_t *m = &geom->meshes[i];
		uint32_t prims = m->num_tris;

		if (!m->num_tris)
			continue;
		snd_mesh_geometry(&ag, &verts, &indices, first_vert[i], snd.blas[i].first_tri, m->num_verts);
		snd_build_info(&info, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &ag);
		memset(&sizes, 0, sizeof(sizes));
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		                                         &info, &prims, &sizes);
		snd.blas[i].offset = blas_size;
		snd.blas[i].size = sizes.accelerationStructureSize;
		blas_size += (sizes.accelerationStructureSize + 255) & ~(VkDeviceSize)255;
		scratch = max(scratch, sizes.buildScratchSize);
	}
	if (!snd_buffer_create(&snd.blas_mem, blas_size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0))
		goto done;
	for (i = 0; i < geom->num_meshes; i++)
		if (geom->meshes[i].num_tris && !snd_create_accel(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
		                                                  &snd.blas_mem, snd.blas[i].offset, snd.blas[i].size,
		                                                  &snd.blas[i].accel))
			goto done;

	// the TLAS, with room for them all
	if (!snd_buffer_create(&snd.instances, geom->num_meshes * sizeof(VkAccelerationStructureInstanceKHR),
	                       input, host, 0))
		goto done;
	{
		uint32_t prims = geom->num_meshes;

		snd_instance_geometry(&ag);
		snd_build_info(&info, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &ag);
		memset(&sizes, 0, sizeof(sizes));
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		qvkGetAccelerationStructureBuildSizesKHR(qvk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
		                                         &info, &prims, &sizes);
		if (!snd_buffer_create(&snd.tlas_mem, sizes.accelerationStructureSize,
		                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0) ||
		    !snd_create_accel(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &snd.tlas_mem, 0,
		                      sizes.accelerationStructureSize, &snd.tlas))
			goto done;
		scratch = max(scratch, sizes.buildScratchSize);
	}

	// one scratch for every build, as they go one at a time
	if (!snd_buffer_create(&snd.scratch, scratch + snd.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0))
		goto done;
	snd.scratch_address = (snd.scratch.address + snd.scratch_align - 1) & ~(snd.scratch_align - 1);

	if (!snd_begin())
		goto done;

	{
		VkBufferCopy copy = { .size = nt * sizeof(snd_tri_t) };
		vkCmdCopyBuffer(snd.cmd, staging.buffer, snd.tris.buffer, 1, &copy);
		copy.size = edges_size;
		vkCmdCopyBuffer(snd.cmd, edge_staging.buffer, snd.edges.buffer, 1, &copy);
	}

	for (i = 0; i < geom->num_meshes; i++) {
		const r_sound_mesh_t *m = &geom->meshes[i];

		if (!snd.blas[i].accel)
			continue;
		snd_mesh_geometry(&ag, &verts, &indices, first_vert[i], snd.blas[i].first_tri, m->num_verts);
		snd_build_info(&info, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &ag);
		info.dstAccelerationStructure = snd.blas[i].accel;
		info.scratchData.deviceAddress = snd.scratch_address;
		range.primitiveCount = m->num_tris;
		qvkCmdBuildAccelerationStructuresKHR(snd.cmd, 1, &info, &ranges);

		// the next build reuses the scratch
		snd_barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
		            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
	}

	submitted = true;
	if (!snd_submit())
		goto done;

	for (i = 0; i < geom->num_meshes; i++) {
		VkAccelerationStructureDeviceAddressInfoKHR address = {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
			.accelerationStructure = snd.blas[i].accel,
		};

		if (snd.blas[i].accel)
			snd.blas[i].address = qvkGetAccelerationStructureDeviceAddressKHR(qvk.device, &address);
	}

	{
		VkWriteDescriptorSetAccelerationStructureKHR as_info = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
			.accelerationStructureCount = 1,
			.pAccelerationStructures = &snd.tlas,
		};
		VkWriteDescriptorSet write = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = &as_info,
			.dstSet = snd.set,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
		};

		vkUpdateDescriptorSets(qvk.device, 1, &write, 0, NULL);
	}
	snd_write_buffer(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &snd.tris, VK_WHOLE_SIZE);
	snd_write_buffer(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &snd.edges, VK_WHOLE_SIZE);

	snd.key = geom->key;
	ok = true;

done:
	if (submitted && !ok)
		vkQueueWaitIdle(qvk.queue_sound);
	snd_buffer_destroy(&verts);
	snd_buffer_destroy(&indices);
	snd_buffer_destroy(&staging);
	snd_buffer_destroy(&edge_staging);
	free(first_vert);
	if (!ok)
		snd_free_scene();
	return ok;
}

// NULL, or why the run was not made
static const char *snd_run(const r_sound_trace_t *t)
{
	snd_params_t    *p = snd.params.mapped;
	VkAccelerationStructureInstanceKHR *inst = NULL;
	VkAccelerationStructureGeometryKHR          ag;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildRangeInfoKHR    range = { 0 };
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	uint32_t    field_floats = 0, per_thread = 0, groups = 0, h;
	int         i, n = 0;

	if (t->num_sources < 0 || t->num_sources > SOUND_TRACE_MAX_SOURCES)
		return "too many sources";
	if (t->num_queries < 0 || t->num_queries > SOUND_TRACE_MAX_QUERIES)
		return "too many queries";
	if (t->num_sources > 0) {
		if (t->num_channels != SOUND_TRACE_CHANNELS || t->num_bins < 1 || t->num_bins > SOUND_TRACE_MAX_BINS)
			return "an energy field it cannot make";
		if (t->num_materials < 1 || t->num_materials > SOUND_TRACE_MAX_MATERIALS)
			return "too many materials";
		if (t->num_rays < 1)
			return "nothing to trace";
	}
	if (!t->geom || !t->geom->key || (t->num_sources < 1 && t->num_queries < 1))
		return "nothing to trace";
	if (t->num_queries > 0 && !snd.diff_pipeline)
		return "no shader for it";

	if (snd.key != t->geom->key) {
		if (snd.failed_key == t->geom->key)
			return "couldn't build the map's scene";
		if (!snd_build_scene(t->geom)) {
			snd.failed_key = t->geom->key;
			return snd.why ? snd.why : "couldn't build the map's scene";
		}
	}

	// the meshes where steamaudio.c has placed them
	inst = snd.instances.mapped;
	for (i = 0; i < t->num_instances && n < snd.num_meshes; i++) {
		const r_sound_instance_t *in = &t->instances[i];

		if (in->mesh < 0 || in->mesh >= snd.num_meshes || !snd.blas[in->mesh].accel)
			continue;
		memcpy(inst[n].transform.matrix, in->transform, sizeof(inst[n].transform.matrix));
		inst[n].instanceCustomIndex = snd.blas[in->mesh].first_tri;
		inst[n].mask = 0xff;
		inst[n].instanceShaderBindingTableRecordOffset = 0;
		inst[n].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		inst[n].accelerationStructureReference = snd.blas[in->mesh].address;
		n++;
	}

	memset(p, 0, sizeof(*p));
	VectorCopy(t->listener, p->listener);

	if (t->num_sources > 0) {
		per_thread = max(1, t->num_rays / (SOUND_TRACE_GROUP * SND_GROUPS_WANTED));
		groups = (t->num_rays + SOUND_TRACE_GROUP * per_thread - 1) / (SOUND_TRACE_GROUP * per_thread);
		field_floats = SOUND_TRACE_CHANNELS * SOUND_TRACE_BANDS * t->num_bins;

		for (i = 0; i < t->num_sources; i++)
			VectorCopy(t->sources + i * 3, p->sources[i]);
		for (i = 0; i < t->num_materials; i++)
			Vector4Copy(t->materials[i], p->materials[i]);
		p->counts[0] = t->num_sources;
		p->counts[1] = t->num_rays;
		p->counts[2] = t->num_bounces;
		p->counts[3] = t->num_bins;
		p->counts2[0] = per_thread;
		p->counts2[1] = t->seed;
		p->dists[0] = t->duration * SOUND_TRACE_SPEED_OF_SOUND;
		p->dists[1] = t->min_distance;
		p->dists[2] = 4 * M_PI / t->num_rays;
		h = t->seed * 2654435761u;
		p->shift[0] = (h >> 8) / 16777216.0f;
		h = h * 1664525u + 1013904223u;
		p->shift[1] = (h >> 8) / 16777216.0f;
	}

	for (i = 0; i < t->num_queries; i++)
		VectorCopy(t->queries + i * 3, p->queries[i]);
	p->counts3[0] = t->num_queries;
	p->counts3[1] = snd.num_edges;

	if (!snd_begin())
		return "couldn't record a run";

	snd_instance_geometry(&ag);
	snd_build_info(&info, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &ag);
	info.dstAccelerationStructure = snd.tlas;
	info.scratchData.deviceAddress = snd.scratch_address;
	range.primitiveCount = n;
	qvkCmdBuildAccelerationStructuresKHR(snd.cmd, 1, &info, &ranges);
	snd_barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
	            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

	vkCmdBindDescriptorSets(snd.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, snd.pipeline_layout, 0, 1, &snd.set, 0, NULL);

	if (t->num_sources > 0) {
		vkCmdFillBuffer(snd.cmd, snd.fields.buffer, 0, (VkDeviceSize)t->num_sources * field_floats * sizeof(float), 0);
		snd_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

		vkCmdBindPipeline(snd.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, snd.pipeline);
		vkCmdDispatch(snd.cmd, groups, t->num_sources, 1);
	}

	if (t->num_queries > 0) {
		vkCmdBindPipeline(snd.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, snd.diff_pipeline);
		vkCmdDispatch(snd.cmd, t->num_queries, 1, 1);
	}

	snd_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
	            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	if (t->num_sources > 0) {
		VkBufferCopy copy = { .size = (VkDeviceSize)t->num_sources * field_floats * sizeof(float) };
		vkCmdCopyBuffer(snd.cmd, snd.fields.buffer, snd.readback.buffer, 1, &copy);
	}
	if (t->num_queries > 0) {
		VkBufferCopy copy = { .size = (VkDeviceSize)t->num_queries * SOUND_DIFFRACT_FLOATS * sizeof(float) };
		vkCmdCopyBuffer(snd.cmd, snd.diff.buffer, snd.diff_readback.buffer, 1, &copy);
	}
	snd_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

	if (!snd_submit())
		return snd.why ? snd.why : "a run failed";

	for (i = 0; i < t->num_sources; i++)
		memcpy(t->fields[i], (const float *)snd.readback.mapped + i * field_floats, field_floats * sizeof(float));
	if (t->num_queries > 0)
		memcpy(t->diffraction, snd.diff_readback.mapped,
		       (size_t)t->num_queries * SOUND_DIFFRACT_FLOATS * sizeof(float));
	return NULL;
}

static bool vkpt_sound_trace(const r_sound_trace_t *trace, const char **why)
{
	const char *reason;

	if (!snd_lock) {
		if (why)
			*why = "the renderer is not up";
		return false;
	}

	SDL_LockMutex(snd_lock);
	if (!snd.up)
		reason = snd.why ? snd.why : "the renderer is not up";
	else if (snd.why)
		reason = snd.why;
	else if (!snd.pipeline)
		reason = "no shader for it";
	else
		reason = trace ? snd_run(trace) : NULL;
	SDL_UnlockMutex(snd_lock);

	if (why)
		*why = reason;
	return !reason;
}

void vkpt_sound_register(void)
{
	if (!snd_lock)
		snd_lock = SDL_CreateMutex();
	R_SoundTrace = snd_lock ? vkpt_sound_trace : NULL;
}

static void snd_destroy_objects(void)
{
	snd_free_scene();
	snd_buffer_destroy(&snd.params);
	snd_buffer_destroy(&snd.fields);
	snd_buffer_destroy(&snd.readback);
	snd_buffer_destroy(&snd.diff);
	snd_buffer_destroy(&snd.diff_readback);
	if (snd.pipeline_layout)
		vkDestroyPipelineLayout(qvk.device, snd.pipeline_layout, NULL);
	if (snd.desc_pool)
		vkDestroyDescriptorPool(qvk.device, snd.desc_pool, NULL);
	if (snd.set_layout)
		vkDestroyDescriptorSetLayout(qvk.device, snd.set_layout, NULL);
	if (snd.fence)
		vkDestroyFence(qvk.device, snd.fence, NULL);
	if (snd.pool)
		vkDestroyCommandPool(qvk.device, snd.pool, NULL);
	snd.pipeline_layout = VK_NULL_HANDLE;
	snd.desc_pool = VK_NULL_HANDLE;
	snd.set = VK_NULL_HANDLE;
	snd.set_layout = VK_NULL_HANDLE;
	snd.fence = VK_NULL_HANDLE;
	snd.cmd = VK_NULL_HANDLE;
	snd.pool = VK_NULL_HANDLE;
}

static bool snd_create_objects(void)
{
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = qvk.queue_idx_sound,
	};
	VkCommandBufferAllocateInfo cmd_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkDescriptorSetLayoutBinding bindings[] = {
		{ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
		{ 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
	};
	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(bindings),
		.pBindings = bindings,
	};
	VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 },
	};
	VkDescriptorPoolCreateInfo desc_pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes = pool_sizes,
	};
	VkDescriptorSetAllocateInfo set_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorSetCount = 1,
	};
	VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
	};

	if (vkCreateCommandPool(qvk.device, &pool_info, NULL, &snd.pool) != VK_SUCCESS)
		return false;
	cmd_info.commandPool = snd.pool;
	if (vkAllocateCommandBuffers(qvk.device, &cmd_info, &snd.cmd) != VK_SUCCESS)
		return false;
	if (vkCreateFence(qvk.device, &fence_info, NULL, &snd.fence) != VK_SUCCESS)
		return false;

	if (vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &snd.set_layout) != VK_SUCCESS)
		return false;
	if (vkCreateDescriptorPool(qvk.device, &desc_pool_info, NULL, &snd.desc_pool) != VK_SUCCESS)
		return false;
	set_info.descriptorPool = snd.desc_pool;
	set_info.pSetLayouts = &snd.set_layout;
	if (vkAllocateDescriptorSets(qvk.device, &set_info, &snd.set) != VK_SUCCESS)
		return false;
	pipeline_layout_info.pSetLayouts = &snd.set_layout;
	if (vkCreatePipelineLayout(qvk.device, &pipeline_layout_info, NULL, &snd.pipeline_layout) != VK_SUCCESS)
		return false;

	if (!snd_buffer_create(&snd.params, sizeof(snd_params_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, 0) ||
	    !snd_buffer_create(&snd.fields, SND_FIELDS_SIZE,
	                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0) ||
	    !snd_buffer_create(&snd.readback, SND_FIELDS_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                       host, VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ||
	    !snd_buffer_create(&snd.diff, SND_DIFF_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0) ||
	    !snd_buffer_create(&snd.diff_readback, SND_DIFF_SIZE, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                       host, VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
		return false;

	snd_write_buffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &snd.params, sizeof(snd_params_t));
	snd_write_buffer(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &snd.fields, VK_WHOLE_SIZE);
	snd_write_buffer(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &snd.diff, VK_WHOLE_SIZE);
	return true;
}

VkResult vkpt_sound_initialize(void)
{
	VkPhysicalDeviceAccelerationStructurePropertiesKHR as_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &as_props,
	};
	vkpt_sound_lock();
	memset(&snd, 0, sizeof(snd));

	vkGetPhysicalDeviceProperties2(qvk.physical_device, &props);
	snd.scratch_align = max(as_props.minAccelerationStructureScratchOffsetAlignment, 1);

	if (!qvk.use_ray_query)
		snd.why = "it needs the ray query API (ray_tracing_api query)";
	else if (!qvk.queue_sound)
		snd.why = "this GPU has no queue left for it";
	else if (props.properties.limits.maxComputeSharedMemorySize < SND_FIELD_FLOATS * sizeof(uint32_t))
		snd.why = "this GPU has too little shared memory for it";
	else if (!snd_create_objects()) {
		snd_destroy_objects();
		snd.why = "couldn't set it up";
	}
	snd.up = !snd.why;
	if (snd.why)
		Com_DPrintf("Sound tracing on the GPU: not available, %s.\n", snd.why);

	vkpt_sound_unlock();

	// The sound menu offers "hardware: GPU" only when this is 1, and
	// steamaudio.c sets up for the CPU otherwise (SA_InitSimulation).
	Cvar_FullSet("s_gpu_trace_available", snd.up ? "1" : "0", CVAR_ROM, FROM_CODE);
	return VK_SUCCESS;
}

VkResult vkpt_sound_destroy(void)
{
	vkpt_sound_lock();
	if (snd.up) {
		vkQueueWaitIdle(qvk.queue_sound);
		snd_destroy_objects();
	}
	snd.up = false;
	vkpt_sound_unlock();

	// the GL renderer, or none, may come next
	Cvar_FullSet("s_gpu_trace_available", "0", CVAR_ROM, FROM_CODE);
	return VK_SUCCESS;
}

VkResult vkpt_sound_create_pipelines(void)
{
	VkComputePipelineCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = qvk.shader_modules[QVK_MOD_SOUND_TRACE_COMP],
			.pName = "main",
		},
	};

	vkpt_sound_lock();
	if (snd.up) {
		info.layout = snd.pipeline_layout;
		if (vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, 1, &info, NULL, &snd.pipeline) != VK_SUCCESS) {
			snd.pipeline = VK_NULL_HANDLE;
			Com_WPrintf("Sound tracing on the GPU: couldn't create its pipeline.\n");
		}
		info.stage.module = qvk.shader_modules[QVK_MOD_SOUND_DIFFRACT_COMP];
		if (vkCreateComputePipelines(qvk.device, VK_NULL_HANDLE, 1, &info, NULL, &snd.diff_pipeline) != VK_SUCCESS) {
			snd.diff_pipeline = VK_NULL_HANDLE;
			Com_WPrintf("Sound tracing on the GPU: couldn't create its diffraction pipeline.\n");
		}
	}
	vkpt_sound_unlock();
	return VK_SUCCESS;
}

VkResult vkpt_sound_destroy_pipelines(void)
{
	vkpt_sound_lock();
	if (snd.pipeline)
		vkDestroyPipeline(qvk.device, snd.pipeline, NULL);
	if (snd.diff_pipeline)
		vkDestroyPipeline(qvk.device, snd.diff_pipeline, NULL);
	snd.pipeline = VK_NULL_HANDLE;
	snd.diff_pipeline = VK_NULL_HANDLE;
	vkpt_sound_unlock();
	return VK_SUCCESS;
}
