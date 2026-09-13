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

VkResult
vkpt_uniform_buffer_create()
{
	VkDescriptorSetLayoutBinding ubo_layout_bindings[2] = { 0 };

	ubo_layout_bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	ubo_layout_bindings[0].descriptorCount  = 1;
	ubo_layout_bindings[0].binding  = GLOBAL_UBO_BINDING_IDX;
	ubo_layout_bindings[0].stageFlags  = VK_SHADER_STAGE_ALL;

	ubo_layout_bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	ubo_layout_bindings[1].descriptorCount  = 1;
	ubo_layout_bindings[1].binding  = GLOBAL_INSTANCE_BUFFER_BINDING_IDX;
	ubo_layout_bindings[1].stageFlags  = VK_SHADER_STAGE_ALL;

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(ubo_layout_bindings),
		.pBindings    = ubo_layout_bindings,
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &qvk.desc_set_layout_ubo));

	const VkMemoryPropertyFlags host_memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	const VkMemoryPropertyFlags device_memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(qvk.physical_device, &properties);
	ubo_alignment = properties.limits.minUniformBufferOffsetAlignment;

	const size_t buffer_size = align(sizeof(QVKUniformBuffer_t), ubo_alignment) + sizeof(InstanceBuffer);
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
		buffer_create(host_uniform_buffers + i, buffer_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host_memory_flags);

	buffer_create(&device_uniform_buffer, buffer_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		device_memory_flags);

	VkDescriptorPoolSize pool_size = {
		.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = MAX_FRAMES_IN_FLIGHT,
	};

	VkDescriptorPoolCreateInfo pool_info = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.poolSizeCount = 1,
		.pPoolSizes    = &pool_size,
		.maxSets       = MAX_FRAMES_IN_FLIGHT,
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
	memcpy((uint8_t*)mapped_ubo + offset, &vkpt_refdef.uniform_instance_buffer, sizeof(InstanceBuffer));

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
	if (Cvar_Get("pt_fog_ubo_war", "1", 0)->integer)
	{
		vkCmdPipelineBarrier(command_buffer,
			qvk.use_ray_query
				? (VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				: (VkPipelineStageFlags)(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				                       | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR),
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 0, NULL, 0, NULL);
	}

	VkBufferCopy copy = { 0 };
	copy.size = align(sizeof(QVKUniformBuffer_t), ubo_alignment) + sizeof(InstanceBuffer);
	vkCmdCopyBuffer(command_buffer, ubo->buffer, device_uniform_buffer.buffer, 1, &copy);

	VkBufferMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.buffer = device_uniform_buffer.buffer,
		.size = copy.size
	};

	vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 1, &barrier, 0, NULL);
}


// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
