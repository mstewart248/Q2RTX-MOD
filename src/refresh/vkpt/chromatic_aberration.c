/*
Copyright (C) 2026 Matt Stewart

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

/* Host side of the chromatic aberration pass. The shader, and the reasoning
   behind the filter, is in shader/chromatic_aberration.comp; this file picks
   the image the pass works on this frame, turns the cvar into the per-pixel
   spread the shader wants, and copies the result back over the colour image.

   Structured to match motion_blur.c, which runs at the same two points in the
   frame, has the same "TAA_OUTPUT without DLSS, DLSS_OUTPUT with it" split,
   and owns the scratch image this pass borrows. */

#include "vkpt.h"
#include "DLSS.h"

static VkPipeline       pipeline;
static VkPipelineLayout pipeline_layout;

/* Mirrors ChromaticAberrationPushConstants in chromatic_aberration.comp. */
struct chromatic_aberration_push_constants {
	int32_t output_width,  output_height;
	int32_t image_width,   image_height;

	float spread;
	int32_t dlss_target;
};

cvar_t *cvar_chromatic_aberration        = NULL;
cvar_t *cvar_chromatic_aberration_amount = NULL;

void
vkpt_chromatic_aberration_init_cvars(void)
{
	/* Master switch. Off by default for the same reason motion blur is: it is
	   a taste setting, and it must not turn up in someone's game unasked. */
	cvar_chromatic_aberration = Cvar_Get("pt_chromatic_aberration", "0", CVAR_ARCHIVE);

	/* THE SLIDER. How far the whole spectrum has spread at the CORNERS of the
	   screen, as a fraction of SCREEN HEIGHT so it looks the same at any
	   resolution. It shrinks linearly to nothing at the centre. The red and
	   blue fringes sit about two thirds of this apart; 0.005 is about 7 pixels
	   between them at 4K - visible on a bright edge, not a filter over the
	   picture. Clamped to 5% in the record function. */
	cvar_chromatic_aberration_amount = Cvar_Get("pt_chromatic_aberration_amount", "0.005", CVAR_ARCHIVE);
}

/* True if the pass should run at all this frame. Checked by the caller before
   the profiler markers so a disabled pass costs nothing, not even a marker. */
bool
vkpt_chromatic_aberration_is_enabled(void)
{
	if (!cvar_chromatic_aberration || cvar_chromatic_aberration->integer == 0)
		return false;

	if (!cvar_chromatic_aberration_amount || cvar_chromatic_aberration_amount->value <= 0.f)
		return false;

	/* The pause menu replaces the frame with a heavy blur (vkpt_bloom_update),
	   which would wash a fringe out anyway. */
	if (qvk.frame_menu_mode)
		return false;

	return true;
}

VkResult
vkpt_chromatic_aberration_initialize(void)
{
	/* Registered here as well as from R_Init, like motion blur's - Cvar_Get
	   returns the existing cvar the second time. */
	vkpt_chromatic_aberration_init_cvars();

	VkDescriptorSetLayout desc_set_layouts[] = {
		qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
		qvk.desc_set_layout_vertex_buffer
	};

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset     = 0,
		.size       = sizeof(struct chromatic_aberration_push_constants)
	};

	CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout,
		.setLayoutCount         = LENGTH(desc_set_layouts),
		.pSetLayouts            = desc_set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges    = &push_constant_range
	);
	ATTACH_LABEL_VARIABLE(pipeline_layout, PIPELINE_LAYOUT);

	return VK_SUCCESS;
}

VkResult
vkpt_chromatic_aberration_destroy(void)
{
	vkDestroyPipelineLayout(qvk.device, pipeline_layout, NULL);
	pipeline_layout = NULL;

	return VK_SUCCESS;
}

VkResult
vkpt_chromatic_aberration_create_pipelines(void)
{
	VkComputePipelineCreateInfo pipeline_info = {
		.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage  = SHADER_STAGE(QVK_MOD_CHROMATIC_ABERRATION_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
		.layout = pipeline_layout,
	};

	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline));

	return VK_SUCCESS;
}

VkResult
vkpt_chromatic_aberration_destroy_pipelines(void)
{
	vkDestroyPipeline(qvk.device, pipeline, NULL);
	pipeline = NULL;

	return VK_SUCCESS;
}

static const VkImageSubresourceRange color_range = {
	.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
	.baseMipLevel   = 0,
	.levelCount     = 1,
	.baseArrayLayer = 0,
	.layerCount     = 1
};

VkResult
vkpt_chromatic_aberration_record_cmd_buffer(VkCommandBuffer cmd_buf)
{
	const bool dlss = DLSSEnabled();

	/* Same image selection as motion_blur.c - see the note there. */
	const int target_image = dlss ? VKPT_IMG_DLSS_OUTPUT : VKPT_IMG_TAA_OUTPUT;

	const VkExtent2D output_extent = dlss ? qvk.extent_unscaled : qvk.extent_taa_output;
	const VkExtent2D image_extent  = dlss ? qvk.extent_unscaled : qvk.extent_taa_images;

	if (output_extent.width == 0 || output_extent.height == 0)
		return VK_SUCCESS;

	struct chromatic_aberration_push_constants push = { 0 };

	push.output_width  = (int32_t)output_extent.width;
	push.output_height = (int32_t)output_extent.height;
	push.image_width   = (int32_t)image_extent.width;
	push.image_height  = (int32_t)image_extent.height;

	/* The cvar is the spread at the corner in screen heights; the shader wants
	   it per pixel of distance from the centre, so divide the corner spread in
	   pixels by the corner's distance in pixels. */
	const float w = (float)output_extent.width;
	const float h = (float)output_extent.height;
	const float amount = max(0.f, min(0.05f, cvar_chromatic_aberration_amount->value));
	const float corner_distance = 0.5f * sqrtf(w * w + h * h);

	push.spread      = amount * h / corner_distance;
	push.dlss_target = dlss ? 1 : 0;

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
		qvk.desc_set_vertex_buffer
	};

	/* The colour image was last written by the resolve, DLSS or motion blur's
	   copy back; make that visible to the taps. */
	IMAGE_BARRIER(cmd_buf,
		.image            = qvk.images[target_image],
		.subresourceRange = color_range,
		.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	/* The scratch image may still be being read by motion blur's copy, or
	   hold DLSS 5's last write; finish both before this pass overwrites it. */
	IMAGE_BARRIER(cmd_buf,
		.image            = qvk.images[VKPT_IMG_MOTION_BLUR],
		.subresourceRange = color_range,
		.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);

	vkCmdPushConstants(cmd_buf, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(push), &push);

	vkCmdDispatch(cmd_buf,
		(output_extent.width  + 15) / 16,
		(output_extent.height + 15) / 16,
		1);

	/* Copy the result back over the colour image, so bloom, tone mapping, FSR
	   and the final blit all read the image they always read. */
	IMAGE_BARRIER(cmd_buf,
		.image            = qvk.images[VKPT_IMG_MOTION_BLUR],
		.subresourceRange = color_range,
		.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	IMAGE_BARRIER(cmd_buf,
		.image            = qvk.images[target_image],
		.subresourceRange = color_range,
		.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	VkImageCopy copy_region = {
		.srcSubresource = {
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel       = 0,
			.baseArrayLayer = 0,
			.layerCount     = 1
		},
		.srcOffset = { 0, 0, 0 },
		.dstSubresource = {
			.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
			.mipLevel       = 0,
			.baseArrayLayer = 0,
			.layerCount     = 1
		},
		.dstOffset = { 0, 0, 0 },
		.extent = { output_extent.width, output_extent.height, 1 }
	};

	vkCmdCopyImage(cmd_buf,
		qvk.images[VKPT_IMG_MOTION_BLUR], VK_IMAGE_LAYOUT_GENERAL,
		qvk.images[target_image],         VK_IMAGE_LAYOUT_GENERAL,
		1, &copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image            = qvk.images[target_image],
		.subresourceRange = color_range,
		.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	return VK_SUCCESS;
}
