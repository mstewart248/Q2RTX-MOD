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

/* Host side of the HDR10 conversion around DLSS Frame Generation; the reason
   for it is at the top of shader/dlssg_hdr.comp. In short: under vid_hdr the
   frame DLSS-FG interpolates is scRGB, which the DLSS-FG guide lists as
   unsupported, and that flickered and smeared every generated frame. DLSSGApply
   encodes the frame to HDR10/PQ in place just before the first evaluate and
   decodes it, and each generated frame, back to scRGB afterwards - everything
   downstream (the swapchain, the HUD) keeps seeing scRGB.

   pt_dlss_fg_hdr10 0 switches the conversion off for an A/B.

   The SDR swapchain needs the same thing with the sRGB curve: IMG_DLSS_OUTPUT
   is tone mapped but linear, the _SRGB swapchain format does the encode in the
   final blit, so DLSS-FG was handed linear light where the guide asks for the
   final display-encoded frame. Measured at a still camera (fgspot): generated
   frames came back ~4x brighter in the darkest tones, equal at white - a
   pulsating lighter copy of dim textures at 4x. pt_dlss_fg_srgb 0 for an A/B.
   Same pass, same call sites; the curve follows the swapchain. */

#include "vkpt.h"
#include "DLSS.h"

static VkPipeline       pipeline;
static VkPipelineLayout pipeline_layout;
static cvar_t          *cvar_pt_dlss_fg_hdr10;
static cvar_t          *cvar_pt_dlss_fg_srgb;

struct dlssg_hdr_push_constants {
	uint32_t mode;      // 0 PQ encode, 1 PQ decode, 2 sRGB encode, 3 sRGB decode
	uint32_t target;    // 0 DLSS_OUTPUT, 1..5 DLSS_FG_OUTPUT..5
	uint32_t width;
	uint32_t height;
};

VkResult
vkpt_dlssg_hdr_initialize(void)
{
	cvar_pt_dlss_fg_hdr10 = Cvar_Get("pt_dlss_fg_hdr10", "1", 0);
	cvar_pt_dlss_fg_srgb  = Cvar_Get("pt_dlss_fg_srgb", "1", 0);

	VkDescriptorSetLayout desc_set_layouts[] = {
		qvk.desc_set_layout_ubo,
		qvk.desc_set_layout_textures,
	};

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset     = 0,
		.size       = sizeof(struct dlssg_hdr_push_constants)
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
vkpt_dlssg_hdr_destroy(void)
{
	vkDestroyPipelineLayout(qvk.device, pipeline_layout, NULL);
	pipeline_layout = NULL;

	return VK_SUCCESS;
}

VkResult
vkpt_dlssg_hdr_create_pipelines(void)
{
	VkComputePipelineCreateInfo pipeline_info = {
		.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage  = SHADER_STAGE(QVK_MOD_DLSSG_HDR_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
		.layout = pipeline_layout,
	};

	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline));

	return VK_SUCCESS;
}

VkResult
vkpt_dlssg_hdr_destroy_pipelines(void)
{
	vkDestroyPipeline(qvk.device, pipeline, NULL);
	pipeline = NULL;

	return VK_SUCCESS;
}

// HDR swapchain: scRGB <-> PQ. SDR swapchain: linear <-> sRGB. Either way
// DLSS-FG sees the display-encoded [0,1] frame it asks for.
bool
vkpt_dlssg_hdr_active(void)
{
	if (!pipeline)
		return false;
	if (qvk.surf_is_hdr)
		return cvar_pt_dlss_fg_hdr10 && cvar_pt_dlss_fg_hdr10->integer;
	return cvar_pt_dlss_fg_srgb && cvar_pt_dlss_fg_srgb->integer;
}

static int
target_image(int target)
{
	switch (target) {
	case 1:  return VKPT_IMG_DLSS_FG_OUTPUT;
	case 2:  return VKPT_IMG_DLSS_FG_OUTPUT2;
	case 3:  return VKPT_IMG_DLSS_FG_OUTPUT3;
	case 4:  return VKPT_IMG_DLSS_FG_OUTPUT4;
	case 5:  return VKPT_IMG_DLSS_FG_OUTPUT5;
	default: return VKPT_IMG_DLSS_OUTPUT;
	}
}

/* `target` 0 is the real frame (VKPT_IMG_DLSS_OUTPUT), 1..5 generated frame N. */
void
vkpt_dlssg_hdr_convert(VkCommandBuffer cmd, int target, bool decode, VkExtent2D extent)
{
	const int img = target_image(target);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
	};

	struct dlssg_hdr_push_constants push = {
		.mode   = (decode ? 1u : 0u) + (qvk.surf_is_hdr ? 0u : 2u),
		.target = (uint32_t)target,
		.width  = extent.width,
		.height = extent.height,
	};

	/* One-shot per curve, so the curve in use is on the record. */
	{
		static uint32_t announced_curve = ~0u;
		const uint32_t curve = push.mode & 2u;
		if (announced_curve != curve && target == 0 && !decode) {
			announced_curve = curve;
			Com_Printf("DLSS-G: frame %s pass active (%s swapchain)\n",
				(push.mode & 2u) ? "sRGB" : "HDR10/PQ", qvk.surf_is_hdr ? "HDR" : "SDR");
		}
	}

	BARRIER_COMPUTE(cmd, qvk.images[img]);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(push), &push);
	vkCmdDispatch(cmd, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);

	BARRIER_COMPUTE(cmd, qvk.images[img]);
}
