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
#include "refresh/refresh.h"
#include "client/client.h"
#include "refresh/images.h"

#include <assert.h>

#include "vkpt.h"
#include "shader/global_textures.h"

enum {
	STRETCH_PIC_SDR,
	STRETCH_PIC_HDR,
	STRETCH_PIC_NUM_PIPELINES
};

#define TEXNUM_WHITE (~0)
#define MAX_STRETCH_PICS (1<<14)

static drawStatic_t draw = {
	.scale = 1.0f,
	.alpha_scale = 1.0f
};

static int num_stretch_pics = 0;
typedef struct {
	float x, y, w,   h;
	float s, t, w_s, h_t;
	uint32_t color, tex_handle;
	// The quad's edges are (w, skew_y) and (skew_x, h), so 0 is an upright
	// rectangle; R_DrawRotatedPic is the only thing that sets them.
	float skew_x, skew_y;
} StretchPic_t;

// Not using global UBO b/c it's only filled when a world is drawn, but here we need it all the time
typedef struct {
	float ui_hdr_nits;
	float tm_hdr_saturation_scale;
} StretchPic_UBO_t;

static clipRect_t clip_rect;
static bool clip_enable = false;

static StretchPic_t stretch_pic_queue[MAX_STRETCH_PICS];

static VkPipelineLayout        pipeline_layout_stretch_pic;
static VkPipelineLayout        pipeline_layout_final_blit;
static VkRenderPass            render_pass_stretch_pic;
static VkPipeline              pipeline_stretch_pic[STRETCH_PIC_NUM_PIPELINES];
static VkPipeline              pipeline_final_blit;
static VkPipeline              pipeline_film_grain;
static VkFramebuffer*          framebuffer_stretch_pic = NULL;
static BufferResource_t        buf_stretch_pic_queue[MAX_FRAMES_IN_FLIGHT];
static BufferResource_t        buf_ubo[MAX_FRAMES_IN_FLIGHT];
static VkDescriptorSetLayout   desc_set_layout_sbo;
static VkDescriptorSetLayout   desc_set_layout_ubo;
static VkDescriptorPool        desc_pool_sbo;
static VkDescriptorPool        desc_pool_ubo;
static VkDescriptorSet         desc_set_sbo[MAX_FRAMES_IN_FLIGHT];
static VkDescriptorSet         desc_set_ubo[MAX_FRAMES_IN_FLIGHT];

extern cvar_t* cvar_ui_hdr_nits;
extern cvar_t* cvar_tm_hdr_saturation_scale;

VkExtent2D
vkpt_draw_get_extent(void)
{
	return qvk.extent_unscaled;
}

/* FINAL BLIT EFFECTS: film grain and the CRT filter. Both are applied in the
   final blit to the swapchain - after everything else that touches the
   picture, frame generation included, and before the HUD. See
   shader/film_grain.glsl and shader/crt.frag for why there. The fragment
   shaders (film_grain.frag, crt.frag, final_blit_lanczos.frag) share one set
   of push constants through pipeline_layout_final_blit; the CRT's grid pass
   (crt_downsample.comp) takes the same set through its own compute layout. */
cvar_t *cvar_film_grain           = NULL;
cvar_t *cvar_film_grain_intensity = NULL;
cvar_t *cvar_crt                  = NULL;
cvar_t *cvar_crt_curvature        = NULL;

/* Mirrors FinalBlitPushConstants in shader/final_blit_pc.glsl. */
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
} FinalBlit_PushConstants_t;

/* crt_mode in the push constants. */
enum {
	CRT_SHADER_OFF,
	CRT_SHADER_GRID,     /* 240 or 480 lines, through crt_downsample.comp */
	CRT_SHADER_NATIVE,   /* the frame itself, no scanlines */
};

/* Width of the grain kernel at 1080 lines, in pixels; it scales with screen
   height from there, so the grain is the same size on screen at any
   resolution. About a pixel and a half across at 1080p. */
#define FILM_GRAIN_SIGMA_1080P 0.6f

static VkPipeline       pipeline_crt;
static VkPipeline       pipeline_crt_downsample;
static VkPipelineLayout pipeline_layout_crt_downsample;

/* Every image the final blit is handed in play, in the order
   final_blit_fetch() in shader/final_blit_pc.glsl numbers them. Anything
   else - the pt_dlss_debug views - goes through the plain blit, without
   grain or CRT. */
static const int final_blit_sources[] = {
	VKPT_IMG_TAA_OUTPUT,
	VKPT_IMG_DLSS_OUTPUT,
	VKPT_IMG_FSR_EASU_OUTPUT,
	VKPT_IMG_FSR_RCAS_OUTPUT,
	VKPT_IMG_DLSS_FG_OUTPUT,
	VKPT_IMG_DLSS_FG_OUTPUT2,
	VKPT_IMG_DLSS_FG_OUTPUT3,
	VKPT_IMG_DLSS_FG_OUTPUT4,
	VKPT_IMG_DLSS_FG_OUTPUT5,
};

/* One per presented frame, generated frames included, so no two frames on
   screen share a grain pattern. */
static uint32_t final_blit_seed;

void
vkpt_film_grain_init_cvars(void)
{
	/* Master switch. Off by default like the other taste effects (motion
	   blur, chromatic aberration). */
	cvar_film_grain = Cvar_Get("pt_film_grain", "0", CVAR_ARCHIVE);

	/* THE SLIDER. Standard deviation of the grain at mid-grey, in perceptual
	   (sRGB-encoded) units: 0.04 moves a mid-tone pixel by about 10 of 255
	   on average. It fades toward black and white - see film_grain.glsl.
	   Clamped to 0.5 in final_blit_params. */
	cvar_film_grain_intensity = Cvar_Get("pt_film_grain_intensity", "0.04", CVAR_ARCHIVE);

	/* CRT filter: 0 off, 1 low (240 lines, as 320x240 had), 2 medium (480
	   lines, as 640x480 had), 3 high (the tube at the game's own resolution,
	   no scanlines). Pixels stay square, so on a 16:9 screen the low grid is
	   427x240 rather than 320 stretched-wide columns. */
	cvar_crt = Cvar_Get("pt_crt", "0", CVAR_ARCHIVE);

	/* How far the tube face bulges, 0 (flat, square corners) to 1. 0.5 is
	   the curvature of Timothy Lottes' CRT shader. Also sets the corner
	   rounding. */
	cvar_crt_curvature = Cvar_Get("pt_crt_curvature", "0.5", CVAR_ARCHIVE);
}

static bool
crt_enabled(void)
{
	return cvar_crt && cvar_crt->integer >= 1 && cvar_crt->integer <= 3;
}

static bool
film_grain_enabled(void)
{
	if (!cvar_film_grain || cvar_film_grain->integer == 0)
		return false;

	if (!cvar_film_grain_intensity || cvar_film_grain_intensity->value <= 0.f)
		return false;

	/* The pause menu's blurred backdrop is not the picture; leave it clean,
	   as chromatic aberration does. */
	if (qvk.frame_menu_mode)
		return false;

	return true;
}

/* The CRT half of the push constants, from pt_crt and pt_crt_curvature. The
   per-mode numbers are the look of each tube: a TV for low (soft signal,
   thick scanlines, coarse mask, more glow), a VGA monitor for medium, and a
   fine high-resolution tube for high. */
static void
crt_params(FinalBlit_PushConstants_t *pc)
{
	if (!crt_enabled())
		return;

	const int out_w = pc->out_width;
	const int out_h = pc->out_height;

	const float curvature = max(0.f, min(1.f, cvar_crt_curvature->value));
	pc->crt_warp_x           = curvature / 16.f;
	pc->crt_warp_y           = curvature / 12.f;
	pc->crt_corner_radius_px = curvature * 0.06f * (float)out_h;

	pc->crt_mask_pitch = 3;

	int lines = 0;
	switch (cvar_crt->integer)
	{
	case 1:
		lines                     = 240;
		pc->crt_hard_pix          = -2.5f;
		pc->crt_scan_hard_dark    = -12.f;
		pc->crt_scan_hard_bright  = -6.f;
		pc->crt_mask_strength     = 0.4f;
		/* A TV's mask is coarse; keep it about 3 pixels a triad at 1080p,
		   which is 6 at 4K. */
		pc->crt_mask_pitch        = 3 * max(1, (int)((float)out_h / 1080.f + 0.5f));
		pc->crt_glow              = 0.15f;
		pc->crt_vignette          = 0.15f;
		break;
	case 2:
		lines                     = 480;
		pc->crt_hard_pix          = -4.f;
		pc->crt_scan_hard_dark    = -10.f;
		pc->crt_scan_hard_bright  = -5.f;
		pc->crt_mask_strength     = 0.3f;
		pc->crt_glow              = 0.08f;
		pc->crt_vignette          = 0.12f;
		break;
	default:
		pc->crt_mask_strength     = 0.25f;
		pc->crt_vignette          = 0.1f;
		break;
	}

	if (lines > 0)
	{
		/* Square pixels: as many columns as the screen's aspect gives 240 or
		   480 lines. Never more than the output has. */
		pc->crt_mode        = CRT_SHADER_GRID;
		pc->crt_grid_height = min(lines, out_h);
		pc->crt_grid_width  = min(out_w, (int)((float)pc->crt_grid_height * (float)out_w / (float)out_h + 0.5f));
	}
	else
	{
		pc->crt_mode = CRT_SHADER_NATIVE;
	}
}

/* Everything but the source. Grain intensity is 0 and crt_mode is off when
   they are off, which the Lanczos blit relies on. */
static FinalBlit_PushConstants_t
final_blit_params(void)
{
	FinalBlit_PushConstants_t pc = { 0 };
	const VkExtent2D out = vkpt_draw_get_extent();

	pc.source     = -1;
	pc.out_width  = (int32_t)out.width;
	pc.out_height = (int32_t)out.height;

	pc.hdr            = qvk.surf_is_hdr ? 1 : 0;
	pc.hdr_white_nits = cvar_ui_hdr_nits ? max(cvar_ui_hdr_nits->value, 1.f) : 200.f;

	pc.seed = final_blit_seed++;

	if (film_grain_enabled())
		pc.grain_intensity = max(0.f, min(0.5f, cvar_film_grain_intensity->value));

	pc.grain_sigma_px = FILM_GRAIN_SIGMA_1080P * (float)out.height / 1080.f;
	pc.grain_radius   = max(1, min(3, (int)ceilf(2.f * pc.grain_sigma_px)));

	crt_params(&pc);

	return pc;
}

static inline void enqueue_stretch_pic(
		float x, float y, float w, float h,
		float s1, float t1, float s2, float t2,
		uint32_t color, int tex_handle)
{
	if (draw.alpha_scale == 0.f)
		return;

	if(num_stretch_pics == MAX_STRETCH_PICS) {
		Com_EPrintf("Error: stretch pic queue full!\n");
		assert(0);
		return;
	}
	assert(tex_handle);
	StretchPic_t *sp = stretch_pic_queue + num_stretch_pics++;

	if (clip_enable)
	{
		if (x >= clip_rect.right || x + w <= clip_rect.left || y >= clip_rect.bottom || y + h <= clip_rect.top)
			return;

		if (x < clip_rect.left)
		{
			float dw = clip_rect.left - x;
			s1 += dw / w * (s2 - s1);
			w -= dw;
			x = clip_rect.left;

			if (w <= 0) return;
		}

		if (x + w > clip_rect.right)
		{
			float dw = (x + w) - clip_rect.right;
			s2 -= dw / w * (s2 - s1);
			w -= dw;

			if (w <= 0) return;
		}

		if (y < clip_rect.top)
		{
			float dh = clip_rect.top - y;
			t1 += dh / h * (t2 - t1);
			h -= dh;
			y = clip_rect.top;

			if (h <= 0) return;
		}

		if (y + h > clip_rect.bottom)
		{
			float dh = (y + h) - clip_rect.bottom;
			t2 -= dh / h * (t2 - t1);
			h -= dh;

			if (h <= 0) return;
		}
	}

	float width = r_config.width * draw.scale;
	float height = r_config.height * draw.scale;

	x = 2.0f * x / width - 1.0f;
	y = 2.0f * y / height - 1.0f;

	w = 2.0f * w / width;
	h = 2.0f * h / height;

	sp->x = x;
	sp->y = y;
	sp->w = w;
	sp->h = h;
	sp->skew_x = 0.0f;
	sp->skew_y = 0.0f;

	sp->s   = s1;
	sp->t   = t1;
	sp->w_s = s2 - s1;
	sp->h_t = t2 - t1;

	if (draw.alpha_scale < 1.f)
	{
		float alpha = (color >> 24) & 0xff;
		alpha *= draw.alpha_scale;
		alpha = max(0.f, min(255.f, alpha));
		color = (color & 0xffffff) | ((int)(alpha) << 24);
	}

	sp->color = color;
	sp->tex_handle = tex_handle;
	if(tex_handle >= 0 && tex_handle < MAX_RIMAGES
	&& !r_images[tex_handle].registration_sequence) {
		sp->tex_handle = TEXNUM_WHITE;
	}
}

static void
create_render_pass(void)
{
	LOG_FUNC();
	VkAttachmentDescription color_attachment = {
		.format         = qvk.surf_format.format,
		.samples        = VK_SAMPLE_COUNT_1_BIT,
		.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
		//.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		//.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
		.initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	};

	VkAttachmentReference color_attachment_ref = {
		.attachment = 0, /* index in fragment shader */
		.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};

	VkSubpassDescription subpass = {
		.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments    = &color_attachment_ref,
	};

	VkSubpassDependency dependencies[] = {
		{
			.srcSubpass    = VK_SUBPASS_EXTERNAL,
			.dstSubpass    = 0, /* index for own subpass */
			.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0, /* XXX verify */
			.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
			               | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		},
	};

	VkRenderPassCreateInfo render_pass_info = {
		.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments    = &color_attachment,
		.subpassCount    = 1,
		.pSubpasses      = &subpass,
		.dependencyCount = LENGTH(dependencies),
		.pDependencies   = dependencies,
	};

	_VK(vkCreateRenderPass(qvk.device, &render_pass_info, NULL, &render_pass_stretch_pic));
	ATTACH_LABEL_VARIABLE(render_pass_stretch_pic, RENDER_PASS);
}

VkResult
vkpt_draw_initialize()
{
	num_stretch_pics = 0;
	LOG_FUNC();
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		_VK(buffer_create(buf_stretch_pic_queue + i, sizeof(StretchPic_t) * MAX_STRETCH_PICS, 
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

		_VK(buffer_create(buf_ubo + i, sizeof(StretchPic_UBO_t),
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
	}

	VkDescriptorSetLayoutBinding layout_bindings[] = {
		{
			.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.binding         = 0,
			.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT,
		},
	};

	VkDescriptorSetLayoutCreateInfo layout_info = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(layout_bindings),
		.pBindings    = layout_bindings,
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info, NULL, &desc_set_layout_sbo));
	ATTACH_LABEL_VARIABLE(desc_set_layout_sbo, DESCRIPTOR_SET_LAYOUT);

	VkDescriptorPoolSize pool_size = {
		.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.descriptorCount = MAX_FRAMES_IN_FLIGHT,
	};

	VkDescriptorPoolCreateInfo pool_info = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.poolSizeCount = 1,
		.pPoolSizes    = &pool_size,
		.maxSets       = MAX_FRAMES_IN_FLIGHT,
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_info, NULL, &desc_pool_sbo));
	ATTACH_LABEL_VARIABLE(desc_pool_sbo, DESCRIPTOR_POOL);

	VkDescriptorSetLayoutBinding layout_bindings_ubo[] = {
		{
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.binding         = 2,
			.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
		},
	};

	VkDescriptorSetLayoutCreateInfo layout_info_ubo = {
		.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = LENGTH(layout_bindings_ubo),
		.pBindings    = layout_bindings_ubo,
	};

	_VK(vkCreateDescriptorSetLayout(qvk.device, &layout_info_ubo, NULL, &desc_set_layout_ubo));
	ATTACH_LABEL_VARIABLE(desc_set_layout_ubo, DESCRIPTOR_SET_LAYOUT);

	VkDescriptorPoolSize pool_size_ubo = {
		.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		.descriptorCount = MAX_FRAMES_IN_FLIGHT,
	};

	VkDescriptorPoolCreateInfo pool_info_ubo = {
		.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.poolSizeCount = 1,
		.pPoolSizes    = &pool_size_ubo,
		.maxSets       = MAX_FRAMES_IN_FLIGHT,
	};

	_VK(vkCreateDescriptorPool(qvk.device, &pool_info_ubo, NULL, &desc_pool_ubo));
	ATTACH_LABEL_VARIABLE(desc_pool_ubo, DESCRIPTOR_POOL);


	VkDescriptorSetAllocateInfo descriptor_set_alloc_info = {
		.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool     = desc_pool_sbo,
		.descriptorSetCount = 1,
		.pSetLayouts        = &desc_set_layout_sbo,
	};

	VkDescriptorSetAllocateInfo descriptor_set_alloc_info_ubo = {
		.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool     = desc_pool_ubo,
		.descriptorSetCount = 1,
		.pSetLayouts        = &desc_set_layout_ubo,
	};

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		_VK(vkAllocateDescriptorSets(qvk.device, &descriptor_set_alloc_info, desc_set_sbo + i));
		BufferResource_t *sbo = buf_stretch_pic_queue + i;

		VkDescriptorBufferInfo buf_info = {
			.buffer = sbo->buffer,
			.offset = 0,
			.range  = sizeof(stretch_pic_queue),
		};

		VkWriteDescriptorSet output_buf_write = {
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = desc_set_sbo[i],
			.dstBinding      = 0,
			.dstArrayElement = 0,
			.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 1,
			.pBufferInfo     = &buf_info,
		};

		vkUpdateDescriptorSets(qvk.device, 1, &output_buf_write, 0, NULL);

		_VK(vkAllocateDescriptorSets(qvk.device, &descriptor_set_alloc_info_ubo, desc_set_ubo + i));
		BufferResource_t *ubo = buf_ubo + i;

		VkDescriptorBufferInfo buf_info_ubo = {
			.buffer = ubo->buffer,
			.offset = 0,
			.range  = sizeof(StretchPic_UBO_t),
		};

		VkWriteDescriptorSet output_buf_write_ubo = {
			.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet          = desc_set_ubo[i],
			.dstBinding      = 2,
			.dstArrayElement = 0,
			.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.pBufferInfo     = &buf_info_ubo,
		};

		vkUpdateDescriptorSets(qvk.device, 1, &output_buf_write_ubo, 0, NULL);
	}
	return VK_SUCCESS;
}

VkResult
vkpt_draw_destroy()
{
	LOG_FUNC();
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		buffer_destroy(buf_stretch_pic_queue + i);
		buffer_destroy(buf_ubo + i);
	}
	vkDestroyDescriptorPool(qvk.device, desc_pool_sbo, NULL);
	vkDestroyDescriptorSetLayout(qvk.device, desc_set_layout_sbo, NULL);
	vkDestroyDescriptorPool(qvk.device, desc_pool_ubo, NULL);
	vkDestroyDescriptorSetLayout(qvk.device, desc_set_layout_ubo, NULL);

	return VK_SUCCESS;
}

VkResult
vkpt_draw_destroy_pipelines()
{
	LOG_FUNC();
	for(int i = 0; i < STRETCH_PIC_NUM_PIPELINES; i++) {
		vkDestroyPipeline(qvk.device, pipeline_stretch_pic[i], NULL);
	}
	vkDestroyPipeline(qvk.device, pipeline_final_blit, NULL);
	vkDestroyPipeline(qvk.device, pipeline_film_grain, NULL);
	vkDestroyPipeline(qvk.device, pipeline_crt, NULL);
	vkDestroyPipeline(qvk.device, pipeline_crt_downsample, NULL);
	vkDestroyPipelineLayout(qvk.device, pipeline_layout_crt_downsample, NULL);
	vkDestroyPipelineLayout(qvk.device, pipeline_layout_stretch_pic, NULL);
	vkDestroyPipelineLayout(qvk.device, pipeline_layout_final_blit, NULL);
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		vkDestroyFramebuffer(qvk.device, framebuffer_stretch_pic[i], NULL);
	}
	free(framebuffer_stretch_pic);
	framebuffer_stretch_pic = NULL;

	vkDestroyRenderPass(qvk.device, render_pass_stretch_pic, NULL);
	
	return VK_SUCCESS;
}

VkResult
vkpt_draw_create_pipelines()
{
	LOG_FUNC();

	// The render pass format follows the swapchain (HDR toggle), so it is
	// recreated along with the pipelines rather than once at init.
	create_render_pass();

	assert(desc_set_layout_sbo);
	VkDescriptorSetLayout desc_set_layouts[] = {
		desc_set_layout_sbo, qvk.desc_set_layout_textures, desc_set_layout_ubo
	};
	CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_stretch_pic, 
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts    = desc_set_layouts
	);

	desc_set_layouts[0] = qvk.desc_set_layout_ubo;

	VkPushConstantRange final_blit_push_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset     = 0,
		.size       = sizeof(FinalBlit_PushConstants_t)
	};

	CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_final_blit,
		.setLayoutCount = LENGTH(desc_set_layouts),
		.pSetLayouts = desc_set_layouts,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &final_blit_push_range
	);

	/* The CRT grid pass: global UBO and textures, the same push constants. */
	{
		VkDescriptorSetLayout crt_desc_set_layouts[] = {
			qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures
		};

		VkPushConstantRange crt_push_range = {
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset     = 0,
			.size       = sizeof(FinalBlit_PushConstants_t)
		};

		CREATE_PIPELINE_LAYOUT(qvk.device, &pipeline_layout_crt_downsample,
			.setLayoutCount = LENGTH(crt_desc_set_layouts),
			.pSetLayouts = crt_desc_set_layouts,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &crt_push_range
		);

		VkComputePipelineCreateInfo crt_pipeline_info = {
			.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage  = SHADER_STAGE(QVK_MOD_CRT_DOWNSAMPLE_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
			.layout = pipeline_layout_crt_downsample,
		};

		_VK(vkCreateComputePipelines(qvk.device, 0, 1, &crt_pipeline_info, 0, &pipeline_crt_downsample));
		ATTACH_LABEL_VARIABLE(pipeline_crt_downsample, PIPELINE);
	}

	VkSpecializationMapEntry specEntries[] = {
		{ .constantID = 0, .offset = 0, .size = sizeof(uint32_t) }
	};

	// "HDR display" flag
	uint32_t spec_data[] = {
		0,
		1,
	};

	VkSpecializationInfo specInfo_SDR = {.mapEntryCount = 1, .pMapEntries = specEntries, .dataSize = sizeof(uint32_t), .pData = &spec_data[0]};
	VkSpecializationInfo specInfo_HDR = {.mapEntryCount = 1, .pMapEntries = specEntries, .dataSize = sizeof(uint32_t), .pData = &spec_data[1]};

	VkPipelineShaderStageCreateInfo shader_info_SDR[] = {
		SHADER_STAGE(QVK_MOD_STRETCH_PIC_VERT, VK_SHADER_STAGE_VERTEX_BIT),
		SHADER_STAGE_SPEC(QVK_MOD_STRETCH_PIC_FRAG, VK_SHADER_STAGE_FRAGMENT_BIT, &specInfo_SDR)
	};

	VkPipelineShaderStageCreateInfo shader_info_HDR[] = {
		SHADER_STAGE(QVK_MOD_STRETCH_PIC_VERT, VK_SHADER_STAGE_VERTEX_BIT),
		SHADER_STAGE_SPEC(QVK_MOD_STRETCH_PIC_FRAG, VK_SHADER_STAGE_FRAGMENT_BIT, &specInfo_HDR)
	};

	VkPipelineVertexInputStateCreateInfo vertex_input_info = {
		.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount   = 0,
		.pVertexBindingDescriptions      = NULL,
		.vertexAttributeDescriptionCount = 0,
		.pVertexAttributeDescriptions    = NULL,
	};

	VkPipelineInputAssemblyStateCreateInfo input_assembly_info = {
		.sType                  = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology               = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
		.primitiveRestartEnable = VK_FALSE,
	};

	VkViewport viewport = {
		.x        = 0.0f,
		.y        = 0.0f,
		.width    = (float) vkpt_draw_get_extent().width,
		.height   = (float) vkpt_draw_get_extent().height,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};

	VkRect2D scissor = {
		.offset = { 0, 0 },
		.extent = vkpt_draw_get_extent(),
	};

	VkPipelineViewportStateCreateInfo viewport_state = {
		.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.pViewports    = &viewport,
		.scissorCount  = 1,
		.pScissors     = &scissor,
	};

	VkPipelineRasterizationStateCreateInfo rasterizer_state = {
		.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.depthClampEnable        = VK_FALSE,
		.rasterizerDiscardEnable = VK_FALSE, /* skip rasterizer */
		.polygonMode             = VK_POLYGON_MODE_FILL,
		.lineWidth               = 1.0f,
		.cullMode                = VK_CULL_MODE_BACK_BIT,
		.frontFace               = VK_FRONT_FACE_CLOCKWISE,
		.depthBiasEnable         = VK_FALSE,
		.depthBiasConstantFactor = 0.0f,
		.depthBiasClamp          = 0.0f,
		.depthBiasSlopeFactor    = 0.0f,
	};

	VkPipelineMultisampleStateCreateInfo multisample_state = {
		.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.sampleShadingEnable   = VK_FALSE,
		.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT,
		.minSampleShading      = 1.0f,
		.pSampleMask           = NULL,
		.alphaToCoverageEnable = VK_FALSE,
		.alphaToOneEnable      = VK_FALSE,
	};

	VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT
			                 | VK_COLOR_COMPONENT_G_BIT
			                 | VK_COLOR_COMPONENT_B_BIT
			                 | VK_COLOR_COMPONENT_A_BIT,
		.blendEnable         = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp        = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp        = VK_BLEND_OP_ADD,
	};

	VkPipelineColorBlendStateCreateInfo color_blend_state = {
		.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.logicOpEnable   = VK_FALSE,
		.logicOp         = VK_LOGIC_OP_COPY,
		.attachmentCount = 1,
		.pAttachments    = &color_blend_attachment,
		.blendConstants  = { 0.0f, 0.0f, 0.0f, 0.0f },
	};

	VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount          = LENGTH(shader_info_SDR),

		.pVertexInputState   = &vertex_input_info,
		.pInputAssemblyState = &input_assembly_info,
		.pViewportState      = &viewport_state,
		.pRasterizationState = &rasterizer_state,
		.pMultisampleState   = &multisample_state,
		.pDepthStencilState  = NULL,
		.pColorBlendState    = &color_blend_state,
		.pDynamicState       = NULL,
		
		.layout              = pipeline_layout_stretch_pic,
		.renderPass          = render_pass_stretch_pic,
		.subpass             = 0,

		.basePipelineHandle  = VK_NULL_HANDLE,
		.basePipelineIndex   = -1,
	};

	pipeline_info.pStages = shader_info_SDR;
	_VK(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline_stretch_pic[STRETCH_PIC_SDR]));
	ATTACH_LABEL_VARIABLE(pipeline_stretch_pic[STRETCH_PIC_SDR], PIPELINE);

	pipeline_info.pStages = shader_info_HDR;
	_VK(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline_stretch_pic[STRETCH_PIC_HDR]));
	ATTACH_LABEL_VARIABLE(pipeline_stretch_pic[STRETCH_PIC_HDR], PIPELINE);


	VkPipelineShaderStageCreateInfo shader_info_final_blit[] = {
		SHADER_STAGE(QVK_MOD_FINAL_BLIT_VERT, VK_SHADER_STAGE_VERTEX_BIT),
		SHADER_STAGE(QVK_MOD_FINAL_BLIT_LANCZOS_FRAG, VK_SHADER_STAGE_FRAGMENT_BIT)
	};

	pipeline_info.pStages = shader_info_final_blit;
	pipeline_info.layout = pipeline_layout_final_blit;

	_VK(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline_final_blit));
	ATTACH_LABEL_VARIABLE(pipeline_final_blit, PIPELINE);

	VkPipelineShaderStageCreateInfo shader_info_film_grain[] = {
		SHADER_STAGE(QVK_MOD_FINAL_BLIT_VERT, VK_SHADER_STAGE_VERTEX_BIT),
		SHADER_STAGE(QVK_MOD_FILM_GRAIN_FRAG, VK_SHADER_STAGE_FRAGMENT_BIT)
	};

	pipeline_info.pStages = shader_info_film_grain;

	_VK(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline_film_grain));
	ATTACH_LABEL_VARIABLE(pipeline_film_grain, PIPELINE);

	VkPipelineShaderStageCreateInfo shader_info_crt[] = {
		SHADER_STAGE(QVK_MOD_FINAL_BLIT_VERT, VK_SHADER_STAGE_VERTEX_BIT),
		SHADER_STAGE(QVK_MOD_CRT_FRAG, VK_SHADER_STAGE_FRAGMENT_BIT)
	};

	pipeline_info.pStages = shader_info_crt;

	_VK(vkCreateGraphicsPipelines(qvk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline_crt));
	ATTACH_LABEL_VARIABLE(pipeline_crt, PIPELINE);

	framebuffer_stretch_pic = malloc(qvk.num_swap_chain_images * sizeof(*framebuffer_stretch_pic));
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		VkImageView attachments[] = {
			qvk.swap_chain_image_views[i]
		};

		VkFramebufferCreateInfo fb_create_info = {
			.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass      = render_pass_stretch_pic,
			.attachmentCount = 1,
			.pAttachments    = attachments,
			.width           = vkpt_draw_get_extent().width,
			.height          = vkpt_draw_get_extent().height,
			.layers          = 1,
		};

		_VK(vkCreateFramebuffer(qvk.device, &fb_create_info, NULL, framebuffer_stretch_pic + i));
		ATTACH_LABEL_VARIABLE(framebuffer_stretch_pic[i], FRAMEBUFFER);
	}

	return VK_SUCCESS;
}

VkResult
vkpt_draw_clear_stretch_pics()
{
	num_stretch_pics = 0;
	return VK_SUCCESS;
}

static VkResult
draw_stretch_pics_into_current_image(VkCommandBuffer cmd_buf, bool keep_queue)
{
	if (num_stretch_pics == 0)
		return VK_SUCCESS;

	BufferResource_t *buf_spq = buf_stretch_pic_queue + qvk.current_frame_index;
	StretchPic_t *spq_dev = (StretchPic_t *) buffer_map(buf_spq);
	memcpy(spq_dev, stretch_pic_queue, sizeof(StretchPic_t) * num_stretch_pics);
	buffer_unmap(buf_spq);
	spq_dev = NULL;

	BufferResource_t *ubo_res = buf_ubo + qvk.current_frame_index;
	StretchPic_UBO_t *ubo = (StretchPic_UBO_t *) buffer_map(ubo_res);
	ubo->ui_hdr_nits = cvar_ui_hdr_nits->value;
	ubo->tm_hdr_saturation_scale = cvar_tm_hdr_saturation_scale->value;
	buffer_unmap(ubo_res);
	ubo = NULL;

	VkRenderPassBeginInfo render_pass_info = {
		.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass        = render_pass_stretch_pic,
		.framebuffer       = framebuffer_stretch_pic[qvk.current_swap_chain_image_index],
		.renderArea.offset = { 0, 0 },
		.renderArea.extent = vkpt_draw_get_extent()
	};

	VkDescriptorSet desc_sets[] = {
		desc_set_sbo[qvk.current_frame_index],
		qvk_get_current_desc_set_textures(),
		desc_set_ubo[qvk.current_frame_index],
	};

	vkCmdBeginRenderPass(cmd_buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
			pipeline_layout_stretch_pic, 0, LENGTH(desc_sets), desc_sets, 0, 0);
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_stretch_pic[qvk.surf_is_hdr ? STRETCH_PIC_HDR : STRETCH_PIC_SDR]);
	vkCmdDraw(cmd_buf, 4, num_stretch_pics, 0, 0);
	vkCmdEndRenderPass(cmd_buf);

	if (!keep_queue)
		num_stretch_pics = 0;
	return VK_SUCCESS;
}

VkResult
vkpt_draw_submit_stretch_pics(VkCommandBuffer cmd_buf)
{
	return draw_stretch_pics_into_current_image(cmd_buf, false);
}

VkResult
vkpt_draw_submit_stretch_pics_keep(VkCommandBuffer cmd_buf)
{
	return draw_stretch_pics_into_current_image(cmd_buf, true);
}

/* The CRT's low-resolution signal: crt_downsample.comp averages the source
   down to the grid, in VKPT_IMG_MOTION_BLUR, for crt.frag to read. */
static void
crt_downsample(VkCommandBuffer cmd_buf, const FinalBlit_PushConstants_t *push)
{
	const VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	/* The scratch image's last users this frame - motion blur's and
	   chromatic aberration's copies, DLSS 5, or the previous presented
	   frame's crt.frag - finish before the grid overwrites it. */
	IMAGE_BARRIER(cmd_buf,
		.image = qvk.images[VKPT_IMG_MOTION_BLUR],
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
		               | VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures()
	};

	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_crt_downsample);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeline_layout_crt_downsample, 0, LENGTH(desc_sets), desc_sets, 0, 0);
	vkCmdPushConstants(cmd_buf, pipeline_layout_crt_downsample, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(*push), push);
	vkCmdDispatch(cmd_buf,
		(push->crt_grid_width  + 7) / 8,
		(push->crt_grid_height + 7) / 8,
		1);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.images[VKPT_IMG_MOTION_BLUR],
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);
}

/* vkpt_final_blit_simple with film grain and/or the CRT filter: into the
   current swapchain image, drawn by film_grain.frag or crt.frag instead of
   vkCmdBlitImage. Leaves the swapchain image in PRESENT_SRC_KHR and the
   source in GENERAL, as the plain blit does. */
static VkResult
final_blit_effects(VkCommandBuffer cmd_buf, VkImage image, int source, VkExtent2D extent)
{
	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	/* The source was last written by a shader (resolve, tone mapping, DLSS-G)
	   or by a copy (motion blur, chromatic aberration). */
	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	FinalBlit_PushConstants_t push = final_blit_params();
	push.source     = source;
	push.src_width  = (int32_t)extent.width;
	push.src_height = (int32_t)extent.height;

	if (push.crt_mode == CRT_SHADER_GRID)
		crt_downsample(cmd_buf, &push);

	VkRenderPassBeginInfo render_pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass_stretch_pic,
		.framebuffer = framebuffer_stretch_pic[qvk.current_swap_chain_image_index],
		.renderArea.offset = { 0, 0 },
		.renderArea.extent = vkpt_draw_get_extent()
	};

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures()
	};

	vkCmdBeginRenderPass(cmd_buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout_final_blit, 0, LENGTH(desc_sets), desc_sets, 0, 0);
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		push.crt_mode != CRT_SHADER_OFF ? pipeline_crt : pipeline_film_grain);
	vkCmdPushConstants(cmd_buf, pipeline_layout_final_blit, VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(push), &push);
	vkCmdDraw(cmd_buf, 4, 1, 0, 0);
	vkCmdEndRenderPass(cmd_buf);

	/* The HUD pass loads what this one stored. */
	IMAGE_BARRIER(cmd_buf,
		.image = qvk.swap_chain_images[qvk.current_swap_chain_image_index],
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	/* Whatever writes the source next waits for this read. */
	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	return VK_SUCCESS;
}

VkResult
vkpt_final_blit_simple(VkCommandBuffer cmd_buf, VkImage image, VkExtent2D extent)
{
	/* Every presented frame passes through here - the real one and each
	   generated one - so this is the one place grain and the CRT need
	   adding. */
	if (film_grain_enabled() || crt_enabled()) {
		for (int i = 0; i < LENGTH(final_blit_sources); i++) {
			if (qvk.images[final_blit_sources[i]] == image)
				return final_blit_effects(cmd_buf, image, i, extent);
		}
	}

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.swap_chain_images[qvk.current_swap_chain_image_index],
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	VkOffset3D blit_size = {
		.x = extent.width,
		.y = extent.height,
		.z = 1
	};

	
	VkOffset3D blit_size_unscaled = {
		.x = qvk.extent_unscaled.width,.y = qvk.extent_unscaled.height,.z = 1
	};
	VkImageBlit img_blit = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffsets = { [1] = blit_size },
		.dstOffsets = { [1] = blit_size_unscaled },
	};
	vkCmdBlitImage(cmd_buf,
		image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		qvk.swap_chain_images[qvk.current_swap_chain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &img_blit, VK_FILTER_NEAREST);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.swap_chain_images[qvk.current_swap_chain_image_index],
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	return VK_SUCCESS;
}


VkResult
vkpt_final_blit_simpleDLSS(VkCommandBuffer cmd_buf, VkImage image, VkExtent2D extent)
{
	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.swap_chain_images[qvk.current_swap_chain_image_index],
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);


	VkOffset3D blit_size = {
		.x = qvk.extent_unscaled.width,
		.y = qvk.extent_unscaled.height,
		.z = 1
	};


	VkOffset3D blit_size_unscaled = {
		.x = qvk.extent_unscaled.width,.y = qvk.extent_unscaled.height,.z = 1
	};
	VkImageBlit img_blit = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.srcOffsets = { [1] = blit_size },
		.dstOffsets = { [1] = blit_size_unscaled },
	};
	vkCmdBlitImage(cmd_buf,
		image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		qvk.swap_chain_images[qvk.current_swap_chain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &img_blit, VK_FILTER_NEAREST);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.swap_chain_images[qvk.current_swap_chain_image_index],
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	IMAGE_BARRIER(cmd_buf,
		.image = image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	return VK_SUCCESS;
}


VkResult
vkpt_final_blit_filtered(VkCommandBuffer cmd_buf)
{
	/* The CRT does its own resampling, so it takes the frame as it is; this
	   path only runs without DLSS, so that is TAA_OUTPUT. */
	if (crt_enabled())
		return final_blit_effects(cmd_buf, qvk.images[VKPT_IMG_TAA_OUTPUT],
			0 /* final_blit_sources[0] */, qvk.extent_taa_output);

	VkRenderPassBeginInfo render_pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = render_pass_stretch_pic,
		.framebuffer = framebuffer_stretch_pic[qvk.current_swap_chain_image_index],
		.renderArea.offset = { 0, 0 },
		.renderArea.extent = vkpt_draw_get_extent()
	};

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures()
	};

	/* Grain is added in final_blit_lanczos.frag on this path; intensity is 0
	   when it is off. */
	FinalBlit_PushConstants_t push = final_blit_params();

	vkCmdBeginRenderPass(cmd_buf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout_final_blit, 0, LENGTH(desc_sets), desc_sets, 0, 0);
	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_final_blit);
	vkCmdPushConstants(cmd_buf, pipeline_layout_final_blit, VK_SHADER_STAGE_FRAGMENT_BIT,
		0, sizeof(push), &push);
	vkCmdDraw(cmd_buf, 4, 1, 0, 0);
	vkCmdEndRenderPass(cmd_buf);

	return VK_SUCCESS;
}

void R_SetClipRect_RTX(const clipRect_t *clip) 
{ 
	if (clip)
	{
		clip_enable = true;
		clip_rect = *clip;
	}
	else
	{
		clip_enable = false;
	}
}

void
R_ClearColor_RTX(void)
{
	draw.colors[0].u32 = U32_WHITE;
	draw.colors[1].u32 = U32_WHITE;
}

void
R_SetAlpha_RTX(float alpha)
{
    alpha = powf(fabsf(alpha), 0.4545f); // un-sRGB the alpha
	draw.colors[0].u8[3] = draw.colors[1].u8[3] = alpha * 255;
}

void
R_SetAlphaScale_RTX(float alpha)
{
	draw.alpha_scale = alpha;
}

void
R_SetColor_RTX(uint32_t color)
{
	draw.colors[0].u32   = color;
	draw.colors[1].u8[3] = draw.colors[0].u8[3];
}

void
R_LightPoint_RTX(const vec3_t origin, vec3_t light)
{
	VectorSet(light, 1, 1, 1);
}

void
R_SetScale_RTX(float scale)
{
	draw.scale = scale;
}

void
R_DrawStretchPic_RTX(int x, int y, int w, int h, qhandle_t pic)
{
	enqueue_stretch_pic(
		x,    y,    w,    h,
		0.0f, 0.0f, 1.0f, 1.0f,
		draw.colors[0].u32, pic);
}

// A pic w x h centred on (cx, cy), turned clockwise by `angle` radians. The
// queue works in normalized device coordinates, which stretch x and y by
// different amounts, so the turn is done here in pixels and handed over as the
// quad's two edge vectors. The clip rect is not applied to these.
void
R_DrawRotatedPic_RTX(float cx, float cy, float w, float h, float angle, qhandle_t pic)
{
	float c = cosf(angle), s = sinf(angle);
	float ux = w * c, uy = w * s;      // along the pic's width
	float vx = -h * s, vy = h * c;     // down its height
	float width = r_config.width * draw.scale;
	float height = r_config.height * draw.scale;
	bool clip = clip_enable;
	StretchPic_t *sp;

	if (draw.alpha_scale == 0.f || num_stretch_pics == MAX_STRETCH_PICS)
		return;

	clip_enable = false;
	enqueue_stretch_pic(cx - (ux + vx) * 0.5f, cy - (uy + vy) * 0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 1.0f, draw.colors[0].u32, pic);
	clip_enable = clip;

	sp = stretch_pic_queue + num_stretch_pics - 1;
	sp->w = 2.0f * ux / width;
	sp->skew_y = 2.0f * uy / height;
	sp->skew_x = 2.0f * vx / width;
	sp->h = 2.0f * vy / height;
}

void
R_DrawPic_RTX(int x, int y, qhandle_t pic)
{
	image_t *image = IMG_ForHandle(pic);
	R_DrawStretchPic(x, y, image->width, image->height, pic);
}

#define DIV64 (1.0f / 64.0f)

void
R_TileClear_RTX(int x, int y, int w, int h, qhandle_t pic)
{
	enqueue_stretch_pic(x, y, w, h,
		x * DIV64, y * DIV64, (x + w) * DIV64, (y + h) * DIV64,
		U32_WHITE, pic);
}

void
R_DrawFill8_RTX(int x, int y, int w, int h, int c)
{
	if(!w || !h)
		return;
	enqueue_stretch_pic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
		d_8to24table[c & 0xff], TEXNUM_WHITE);
}

void
R_DrawFill32_RTX(int x, int y, int w, int h, uint32_t color)
{
	if(!w || !h)
		return;
	enqueue_stretch_pic(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
		color, TEXNUM_WHITE);
}

static inline void
draw_char(int x, int y, int flags, int c, qhandle_t font)
{
	if ((c & 127) == 32) {
		return;
	}

	if (flags & UI_ALTCOLOR) {
		c |= 0x80;
	}
	if (flags & UI_XORCOLOR) {
		c ^= 0x80;
	}

	/* The glyph's cell in the 16x16 character atlas, passed exactly. Keeping
	   the bilinear tap from reaching into the neighbouring cells is the pixel
	   shader's job, since only it knows the font's texel size; see the clamp in
	   shader/stretch_pic.frag. Nudging these coordinates inwards here cannot do
	   it - the inset needed is half a texel, which depends on a resolution this
	   code does not have, and paying it on the coordinates would squeeze the
	   glyph into 7 of its 8 texels and soften it at every scale. */
	float s = (c & 15) * 0.0625f;
	float t = (c >> 4) * 0.0625f;

	enqueue_stretch_pic(x, y, CHAR_WIDTH, CHAR_HEIGHT,
		s, t, s + 0.0625f, t + 0.0625f,
		draw.colors[c >> 7].u32, font);
}

void
R_DrawChar_RTX(int x, int y, int flags, int c, qhandle_t font)
{
	draw_char(x, y, flags, c & 255, font);
}

int
R_DrawString_RTX(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font)
{
	while(maxlen-- && *s) {
		byte c = *s++;
		draw_char(x, y, flags, c, font);
		x += CHAR_WIDTH;
	}

	return x;
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
