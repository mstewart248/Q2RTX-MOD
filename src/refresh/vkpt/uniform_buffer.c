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

#include "vkpt.h"

#include <assert.h>

static BufferResource_t host_uniform_buffers[MAX_FRAMES_IN_FLIGHT];
static BufferResource_t device_uniform_buffer;
static VkDescriptorPool desc_pool_ubo;
static size_t ubo_alignment = 0;

/*
THE INSTANCE BUFFER IS UPLOADED IN TWO PIECES - pt_upload_ranges, default 1.

InstanceBuffer is ~1.8 MB, and 1.5 MB of that is model_instances (8192 x 192 B),
of which a frame typically fills a few hundred rows. The whole struct was copied
into write-combined staging and then to the device at the head of the graphics
queue every frame.

The model_instances rows past the frame's count are never written by anything:
prepare_entities fills rows 0..n-1 plus the blood row at n, and nothing clears or
patches beyond that. So the host struct only CHANGES below the count, and copying
just that prefix - every frame, from the first full copy on - keeps the device
row-for-row equal to the host struct everywhere, which is exactly what the whole
copy produced. That holds however a shader indexes the array, stale or not: a
cached blood primitive whose `instance` field still names last frame's blood row,
for one, reads the same bytes it always did.

Everything around it is copied whole, since it is small and its counts are not
known yet when the copy is recorded: the UBO itself, animated_model_indices and
the two -1-filled prev/current maps in front (96 KB), mlight_prev_to_current and
the two tlas_* arrays behind (128 KB) - the tlas_* ones are only filled by
vkpt_pt_create_toplevel, after this copy is recorded and before the staging
memcpy, which is why the regions are fixed here and replayed by
vkpt_uniform_buffer_upload_to_staging.

The count comes from main.c (vkpt_uniform_buffer_set_model_instance_count),
stamped with the frame: a frame that did not report one gets the whole copy, so
until main.c makes the call nothing changes.
*/
static cvar_t* cvar_pt_upload_ranges = NULL;

static struct {
	uint64_t count_frame;       // qvk.frame_counter the count below was reported for
	uint32_t model_instances;   // model_instances rows written that frame
	bool     full_pending;      // first copy after the buffers were created

	// what copy_from_staging recorded, per staging buffer, for upload_to_staging
	uint64_t recorded_frame[MAX_FRAMES_IN_FLIGHT];
	size_t   head_bytes[MAX_FRAMES_IN_FLIGHT];  // InstanceBuffer bytes [0, head)
	size_t   tail_offset[MAX_FRAMES_IN_FLIGHT]; // and [tail, sizeof) - sizeof when none
} instance_upload;

/* main.c, at the end of prepare_entities: how many model_instances rows it wrote
   this frame, counting the blood row. Several calls in one frame keep the
   largest. */
void
vkpt_uniform_buffer_set_model_instance_count(uint32_t count)
{
	if (instance_upload.count_frame != qvk.frame_counter)
	{
		instance_upload.count_frame = qvk.frame_counter;
		instance_upload.model_instances = 0;
	}

	instance_upload.model_instances = max(instance_upload.model_instances, min(count, (uint32_t)MAX_MODEL_INSTANCES));
}

VkResult
vkpt_uniform_buffer_create()
{
	VkDescriptorPoolSize pool_sizes[2] = { };
	VkDescriptorSetLayoutBinding ubo_layout_bindings[2] = { 0 };

	ubo_layout_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	ubo_layout_bindings[0].descriptorCount  = 1;
	ubo_layout_bindings[0].binding  = GLOBAL_UBO_BINDING_IDX;
	ubo_layout_bindings[0].stageFlags  = VK_SHADER_STAGE_ALL;

	pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	pool_sizes[0].descriptorCount = 1;

	ubo_layout_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	ubo_layout_bindings[1].descriptorCount  = 1;
	ubo_layout_bindings[1].binding  = GLOBAL_INSTANCE_BUFFER_BINDING_IDX;
	ubo_layout_bindings[1].stageFlags  = VK_SHADER_STAGE_ALL;

	pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	pool_sizes[1].descriptorCount = 1;

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(ubo_layout_bindings),
		.pBindings    = ubo_layout_bindings,
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &qvk.desc_set_layout_ubo));

	const VkMemoryPropertyFlags host_memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkMemoryPropertyFlags device_memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	// A new device buffer holds nothing yet, so its first copy is whole.
	cvar_pt_upload_ranges = Cvar_Get("pt_upload_ranges", "1", 0);
	memset(&instance_upload, 0, sizeof(instance_upload));
	instance_upload.full_pending = true;
	instance_upload.count_frame = ~0ull;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		instance_upload.recorded_frame[i] = ~0ull;

	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(qvk.physical_device, &properties);
	ubo_alignment = properties.limits.minUniformBufferOffsetAlignment;

	const size_t buffer_size = align(sizeof(QVKUniformBuffer_t), ubo_alignment) + sizeof(InstanceBuffer);
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		buffer_create(host_uniform_buffers + i, buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host_memory_flags);

	buffer_create(&device_uniform_buffer, buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		device_memory_flags);

	VkDescriptorPoolCreateInfo pool_info = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.poolSizeCount = LENGTH(pool_sizes),
		.pPoolSizes    = pool_sizes,
		.maxSets       = 1,
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_info, NULL, &desc_pool_ubo));

	VkDescriptorSetAllocateInfo descriptor_set_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = desc_pool_ubo,
		.descriptorSetCount = 1,
		.pSetLayouts = &qvk.desc_set_layout_ubo,
	};

	_VK(vkAllocateDescriptorSets(qvk.device, &descriptor_set_alloc_info, &qvk.desc_set_ubo));
	
	VkDescriptorBufferInfo buf_info = {
		.buffer = device_uniform_buffer.buffer,
		.offset = 0,
		.range  = sizeof(QVKUniformBuffer_t),
	};

	VkDescriptorBufferInfo buf1_info = {
		.buffer = device_uniform_buffer.buffer,
		.offset = align(sizeof(QVKUniformBuffer_t), ubo_alignment),
		.range  = sizeof(InstanceBuffer),
	};

	VkWriteDescriptorSet writes[2] = { 0 };

	writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	writes[0].dstSet          = qvk.desc_set_ubo,
	writes[0].dstBinding      = GLOBAL_UBO_BINDING_IDX,
	writes[0].dstArrayElement = 0,
	writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
	writes[0].descriptorCount = 1,
	writes[0].pBufferInfo     = &buf_info,

	writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	writes[1].dstSet          = qvk.desc_set_ubo,
	writes[1].dstBinding      = GLOBAL_INSTANCE_BUFFER_BINDING_IDX,
	writes[1].dstArrayElement = 0,
	writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
	writes[1].descriptorCount = 1,
	writes[1].pBufferInfo     = &buf1_info,

	vkUpdateDescriptorSets(qvk.device, LENGTH(writes), writes, 0, NULL);

	return VK_SUCCESS;
}

VkResult
vkpt_uniform_buffer_destroy()
{
	vkDestroyDescriptorPool(qvk.device, desc_pool_ubo, NULL);
	vkDestroyDescriptorSetLayout(qvk.device, qvk.desc_set_layout_ubo, NULL);
	desc_pool_ubo = VK_NULL_HANDLE;
	qvk.desc_set_layout_ubo = VK_NULL_HANDLE;

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		buffer_destroy(host_uniform_buffers + i);
	buffer_destroy(&device_uniform_buffer);

	return VK_SUCCESS;
}

VkResult
vkpt_uniform_buffer_upload_to_staging()
{
	BufferResource_t* ubo = host_uniform_buffers + qvk.current_frame_index;
	assert(ubo);
	assert(ubo->memory != VK_NULL_HANDLE);
	assert(ubo->buffer != VK_NULL_HANDLE);
	assert(qvk.current_frame_index < MAX_FRAMES_IN_FLIGHT);

	QVKUniformBuffer_t* mapped_ubo = buffer_map(ubo);
	assert(mapped_ubo);
	if (!mapped_ubo)
		return VK_ERROR_MEMORY_MAP_FAILED;

	memcpy(mapped_ubo, &vkpt_refdef.uniform_buffer, sizeof(QVKUniformBuffer_t));

	const size_t offset = align(sizeof(QVKUniformBuffer_t), ubo_alignment);
	uint8_t* dst = (uint8_t*)mapped_ubo + offset;
	const uint8_t* src = (const uint8_t*)&vkpt_refdef.uniform_instance_buffer;

	// Fill exactly what this frame's copy reads - see THE INSTANCE BUFFER IS
	// UPLOADED IN TWO PIECES. Anything else in this staging buffer is never read.
	const int fi = qvk.current_frame_index;
	if (instance_upload.recorded_frame[fi] == qvk.frame_counter)
	{
		memcpy(dst, src, instance_upload.head_bytes[fi]);
		if (instance_upload.tail_offset[fi] < sizeof(InstanceBuffer))
			memcpy(dst + instance_upload.tail_offset[fi], src + instance_upload.tail_offset[fi],
				sizeof(InstanceBuffer) - instance_upload.tail_offset[fi]);
	}
	else
	{
		memcpy(dst, src, sizeof(InstanceBuffer));
	}

	buffer_unmap(ubo);

	return VK_SUCCESS;
}

void
vkpt_uniform_buffer_copy_from_staging(VkCommandBuffer command_buffer)
{
	BufferResource_t* ubo = host_uniform_buffers + qvk.current_frame_index;

	/* WAIT FOR THE PREVIOUS FRAME'S SHADERS TO STOP READING THE UBO.

	   The staging buffers are per frame in flight, but `device_uniform_buffer`
	   - the one every shader actually reads - is a SINGLE buffer that every
	   frame copies into. The barrier after the copy below orders
	   TRANSFER_WRITE -> UNIFORM_READ, i.e. this frame's copy before this
	   frame's reads. Nothing orders the other direction: the PREVIOUS frame's
	   reads before this frame's WRITE.

	   The frame fence only proves the frame MAX_FRAMES_IN_FLIGHT (2) ago
	   finished, so frame N-1 can still be executing when frame N's copy is
	   submitted - a write-after-read hazard on the uniform buffer, and one
	   whose likelihood rises with how far the CPU runs ahead of the GPU. That
	   is a frame-rate dependence, and the fog fade is measured to be exactly
	   that: 32-62% of frames collapse uncapped, 0% at r_maxfps 60 or 30.

	   WAR needs only an execution dependency - no cache flush - so the access
	   masks are 0 and the stage masks do the work. Kept to the shader stages
	   that read the UBO rather than ALL_COMMANDS, because a heavier barrier
	   costs frame rate, and LOWERING THE FRAME RATE WOULD SUPPRESS THE FADE ON
	   ITS OWN and fake a fix. Check the logged frame count stays ~730.

	   pt_fog_ubo_war 0 disables it for A/B. */
	/* THE FRAGMENT STAGE BELONGS IN THIS MASK AND WAS MISSING.

	   "the shader stages that read the UBO" was taken to mean compute (plus ray
	   tracing where legal). It is not the whole set: `final_blit_lanczos.frag`
	   includes global_ubo.h and reads device_uniform_buffer from a FRAGMENT
	   shader. It is also the LAST pass of the frame, which makes it the reader
	   most likely to still be in flight when the next frame's copy is submitted
	   - exactly the overlap that running the GPU flat out maximises.

	   With only COMPUTE named, that read was never ordered against the
	   following frame's TRANSFER_WRITE, so the write-after-read this barrier
	   exists to prevent was still wide open for one reader - and the one with
	   the worst timing.

	   Still not ALL_COMMANDS: the added stage is the one shader stage that
	   demonstrably reads this buffer, so the barrier stays cheap. The warning
	   above stands and must be honoured when reading any result from this -
	   CHECK THE ACHIEVED FRAME RATE. A change that fixes the fade by slowing
	   the card down has not fixed anything. */
	if (Cvar_Get("pt_fog_ubo_war", "1", 0)->integer)
	{
		vkCmdPipelineBarrier(command_buffer,
			(qvk.use_ray_query
				? (VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				: (VkPipelineStageFlags)(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				                       | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR))
			| VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 0, NULL);
	}

	const size_t ib_base = align(sizeof(QVKUniformBuffer_t), ubo_alignment);
	const size_t total_size = ib_base + sizeof(InstanceBuffer);
	const int fi = qvk.current_frame_index;

	// The UBO and the instance buffer up to the end of the rows written this
	// frame, then everything from mlight_prev_to_current on.
	size_t head = sizeof(InstanceBuffer);
	size_t tail = sizeof(InstanceBuffer);

	if (!instance_upload.full_pending
		&& instance_upload.count_frame == qvk.frame_counter
		&& (!cvar_pt_upload_ranges || cvar_pt_upload_ranges->integer))
	{
		head = offsetof(InstanceBuffer, model_instances)
			+ (size_t)instance_upload.model_instances * sizeof(ModelInstance);
		tail = offsetof(InstanceBuffer, mlight_prev_to_current);
	}

	instance_upload.full_pending = false;
	instance_upload.recorded_frame[fi] = qvk.frame_counter;
	instance_upload.head_bytes[fi] = head;
	instance_upload.tail_offset[fi] = tail;

	VkBufferCopy copies[2] = {
		{ .srcOffset = 0, .dstOffset = 0, .size = ib_base + head },
		{ .srcOffset = ib_base + tail, .dstOffset = ib_base + tail, .size = sizeof(InstanceBuffer) - tail },
	};
	vkCmdCopyBuffer(command_buffer, ubo->buffer, device_uniform_buffer.buffer, (tail < sizeof(InstanceBuffer)) ? 2 : 1, copies);

	// Over the whole buffer, as before: a superset of what was written.
	VkBufferMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = device_uniform_buffer.buffer,
		.size = total_size
	};

	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 1, &barrier, 0, NULL);
}


// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
