/*
Copyright (C) 2025 Matt Stewart

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

/* Host side of the motion blur pass. The shader, and the reasoning behind the
   filter it implements, is in shader/motion_blur.comp; this file decides which
   image the pass is working on this frame, turns the cvars into the pixel-space
   numbers the shader wants, and copies the result back over the colour image.

   Structured to match bloom.c, which runs at the same two points in the frame
   and has the same "TAA_OUTPUT without DLSS, DLSS_OUTPUT with it" split. */

#include "vkpt.h"
#include "DLSS.h"

static VkPipeline       pipeline;
static VkPipelineLayout pipeline_layout;

/* Mirrors MotionBlurPushConstants in motion_blur.comp. Keep the two in step -
   there is no reflection here, a mismatch just reads garbage. */
struct motion_blur_push_constants {
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
};

cvar_t *cvar_motion_blur_enable      = NULL;
cvar_t *cvar_motion_blur_strength    = NULL;
cvar_t *cvar_motion_blur_samples     = NULL;
cvar_t *cvar_motion_blur_max         = NULL;
cvar_t *cvar_motion_blur_dynamic     = NULL;
cvar_t *cvar_motion_blur_jitter      = NULL;
cvar_t *cvar_motion_blur_min_px      = NULL;
cvar_t *cvar_motion_blur_soft_z      = NULL;
cvar_t *cvar_motion_blur_fps_ref     = NULL;

/* The frame time the pass last saw, in seconds. Only read when
   pt_motion_blur_fps_ref is on - see the note in the record function. */
static float last_frame_time = 0.f;

void
vkpt_motion_blur_init_cvars(void)
{
	/* Master switch. Off by default: motion blur is a taste setting, and the
	   one thing it must not do is arrive unannounced in someone's save. */
	cvar_motion_blur_enable = Cvar_Get("pt_motion_blur", "0", CVAR_ARCHIVE);

	/* THE SLIDER. Shutter open time as a fraction of the frame interval, which
	   is exactly Remix's rtx.postfx.exposureFraction. 0 is no blur; 1 is a
	   shutter open for the whole frame, i.e. the surface's entire per-frame
	   screen displacement is smeared. Anything above ~0.7 reads as smeared
	   rather than fast, which is why the menu range stops there. */
	cvar_motion_blur_strength = Cvar_Get("pt_motion_blur_strength", "0.5", CVAR_ARCHIVE);

	/* Taps along the swept segment, on top of the centre pixel. This is the
	   cost knob: the pass is one texture fetch for colour, one for depth and
	   one for motion per tap. Below about 8 a fast pan shows the taps as
	   separate ghosts; above about 24 nothing visibly improves. */
	cvar_motion_blur_samples = Cvar_Get("pt_motion_blur_samples", "12", CVAR_ARCHIVE);

	/* Ceiling on the blur length as a fraction of SCREEN HEIGHT, so it means
	   the same thing at any resolution and aspect ratio. Remix calls this
	   blurDiameterFraction. It is not a quality setting, it is a safety rail:
	   a teleport, a respawn or a cut to a security camera produces a motion
	   vector the length of the screen, and without a cap that frame is both a
	   full-screen smear and by far the most expensive frame in the run. */
	cvar_motion_blur_max = Cvar_Get("pt_motion_blur_max", "0.05", CVAR_ARCHIVE);

	/* How much of a pixel's OBJECT-induced motion is discarded before blurring
	   - Remix's motionBlurDynamicDeduction. 0 blurs everything by its true
	   screen motion, which is the physically honest answer and also the one
	   that makes a strafing enemy hardest to shoot. 1 blurs only what the
	   camera is carrying.

	   THIS COSTS SOMETHING TO EVALUATE: separating the two shares means
	   unprojecting each tap and reprojecting it through the previous camera,
	   so the shader skips the whole computation when this is 0. Leave it at 0
	   if the look is what matters more than the frame time. */
	cvar_motion_blur_dynamic = Cvar_Get("pt_motion_blur_dynamic", "0.5", CVAR_ARCHIVE);

	/* Per-pixel dither on the tap positions, 0..1. At 0 the taps sit on a
	   fixed comb and a long blur shows the individual ghosts; at 1 they are
	   spread over a full tap spacing and the ghosts become grain, which the
	   eye forgives far more readily. Costs nothing. */
	cvar_motion_blur_jitter = Cvar_Get("pt_motion_blur_jitter", "1.0", CVAR_ARCHIVE);

	/* Motion below this many pixels is ignored and the pixel is passed through
	   untouched - Remix's motionBlurMinimumVelocityThresholdInPixel. Without
	   it, a camera that is merely breathing still runs the full gather, and a
	   still image picks up a permanent half-pixel softening for no reason. */
	cvar_motion_blur_min_px = Cvar_Get("pt_motion_blur_min_px", "0.5", CVAR_ARCHIVE);

	/* The depth difference, in Quake units, over which the shader's
	   foreground/background test goes from certain to a coin flip. Too small
	   and a gently sloping floor gets classified as an edge, so the blur
	   breaks up along it; too large and a genuine silhouette bleeds. 32 units
	   is about waist height, which is the scale that separates "two surfaces"
	   from "one surface at an angle" in this game's geometry. */
	cvar_motion_blur_soft_z = Cvar_Get("pt_motion_blur_soft_z", "32", CVAR_ARCHIVE);

	/* FRAME RATE INDEPENDENCE, off by default.

	   The blur length is a fraction of the PER-FRAME displacement, so at a
	   fixed camera speed it is twice as long at 30fps as at 60. That is what a
	   real shutter does and it is what Remix does, but it also means the look
	   changes whenever the frame rate does, which in this renderer is
	   constantly - and it is strongest exactly when the machine is struggling.

	   Set this to a frame rate and the blur is scaled to what it would have
	   been at that rate instead, so the amount of blur tracks how fast the
	   camera is moving through the WORLD rather than how long the last frame
	   took. 0 keeps the physical behaviour. */
	cvar_motion_blur_fps_ref = Cvar_Get("pt_motion_blur_fps_ref", "0", CVAR_ARCHIVE);
}

/* True if the pass should run at all this frame. Checked by the caller before
   the profiler markers so a disabled pass costs nothing, not even a marker. */
bool
vkpt_motion_blur_is_enabled(void)
{
	if (!cvar_motion_blur_enable || cvar_motion_blur_enable->integer == 0)
		return false;

	if (!cvar_motion_blur_strength || cvar_motion_blur_strength->value <= 0.f)
		return false;

	/* The menu blurs the whole frame on purpose (see vkpt_bloom_update), and
	   the camera does not move behind it. Streaking that would be all downside. */
	if (qvk.frame_menu_mode)
		return false;

	return true;
}

/* Remembers how long the last frame took, for the optional frame rate
   normalisation. Called from the same place bloom's per-frame update is. */
void
vkpt_motion_blur_update(float frame_time)
{
	if (frame_time > 0.f)
		last_frame_time = frame_time;
}

VkResult
vkpt_motion_blur_initialize(void)
{
	/* Registered here as well as from R_Init so that the pass is usable even
	   if it is somehow brought up without the renderer's cvar pass having run.
	   Cvar_Get returns the existing cvar the second time, so this is free. */
	vkpt_motion_blur_init_cvars();

	VkDescriptorSetLayout desc_set_layouts[] = {
		qvk.desc_set_layout_ubo, qvk.desc_set_layout_textures,
		qvk.desc_set_layout_vertex_buffer
	};

	VkPushConstantRange push_constant_range = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset     = 0,
		.size       = sizeof(struct motion_blur_push_constants)
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
vkpt_motion_blur_destroy(void)
{
	vkDestroyPipelineLayout(qvk.device, pipeline_layout, NULL);
	pipeline_layout = NULL;

	return VK_SUCCESS;
}

VkResult
vkpt_motion_blur_create_pipelines(void)
{
	VkComputePipelineCreateInfo pipeline_info = {
		.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage  = SHADER_STAGE(QVK_MOD_MOTION_BLUR_COMP, VK_SHADER_STAGE_COMPUTE_BIT),
		.layout = pipeline_layout,
	};

	_VK(vkCreateComputePipelines(qvk.device, 0, 1, &pipeline_info, 0, &pipeline));

	return VK_SUCCESS;
}

VkResult
vkpt_motion_blur_destroy_pipelines(void)
{
	vkDestroyPipeline(qvk.device, pipeline, NULL);
	pipeline = NULL;

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
				.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT, \
				.oldLayout        = VK_IMAGE_LAYOUT_GENERAL, \
				.newLayout        = VK_IMAGE_LAYOUT_GENERAL, \
		); \
	} while(0)

/* The scratch image has just been written by the shader and is about to be
   read by a TRANSFER; the colour image is about to be written by that same
   TRANSFER. Submission order gives execution order but not visibility, so both
   sides need saying explicitly. Kept as one barrier call because the two
   transitions have to happen at the same point in the pipeline anyway. */
static void
barrier_before_copy(VkCommandBuffer cmd_buf, VkImage src, VkImage dst)
{
	VkImageSubresourceRange subresource_range = {
		.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel   = 0,
		.levelCount     = 1,
		.baseArrayLayer = 0,
		.layerCount     = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image            = src,
		.subresourceRange = subresource_range,
		.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);

	IMAGE_BARRIER(cmd_buf,
		.image            = dst,
		.subresourceRange = subresource_range,
		.srcAccessMask    = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);
}

/* And back the other way: the copy is done, everything downstream reads the
   colour image as a shader resource again. */
static void
barrier_after_copy(VkCommandBuffer cmd_buf, VkImage dst)
{
	VkImageSubresourceRange subresource_range = {
		.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel   = 0,
		.levelCount     = 1,
		.baseArrayLayer = 0,
		.layerCount     = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image            = dst,
		.subresourceRange = subresource_range,
		.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout        = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout        = VK_IMAGE_LAYOUT_GENERAL,
	);
}

VkResult
vkpt_motion_blur_record_cmd_buffer(VkCommandBuffer cmd_buf)
{
	const bool dlss = DLSSEnabled();

	/* WHICH IMAGE HOLDS THE FRAME, and at what size.

	   Without DLSS the resolved frame is in TAA_OUTPUT, whose written region
	   is extent_taa_output inside an image allocated at extent_taa_images.
	   With DLSS it is in DLSS_OUTPUT, written and allocated at
	   extent_unscaled. The two differ, and the shader needs both numbers: the
	   region to bound its dispatch, and the allocation to turn a pixel into a
	   UV. */
	const int target_image = dlss ? VKPT_IMG_DLSS_OUTPUT : VKPT_IMG_TAA_OUTPUT;

	const VkExtent2D output_extent = dlss ? qvk.extent_unscaled : qvk.extent_taa_output;
	const VkExtent2D image_extent  = dlss ? qvk.extent_unscaled : qvk.extent_taa_images;

	/* The motion field. asvgf_taau writes PT_DLSS_MOTION over exactly the same
	   region it writes TAA_OUTPUT, so without DLSS this is one-to-one with the
	   output. With DLSS, motion blur runs AFTER the upscale while the motion
	   field is still at render resolution, so the shader has to sample it
	   proportionally - hence passing the extent rather than assuming. */
	const VkExtent2D motion_extent = qvk.extent_taa_output;

	if (output_extent.width == 0 || output_extent.height == 0)
		return VK_SUCCESS;

	struct motion_blur_push_constants push = { 0 };

	push.output_width  = (int32_t)output_extent.width;
	push.output_height = (int32_t)output_extent.height;
	push.image_width   = (int32_t)image_extent.width;
	push.image_height  = (int32_t)image_extent.height;
	push.motion_width  = (int32_t)motion_extent.width;
	push.motion_height = (int32_t)motion_extent.height;

	/* DLSS_DEPTH is filled by the interleave pass, which runs on the render
	   grid whatever the upscaler is doing afterwards. */
	push.render_width  = (int32_t)qvk.extent_render.width;
	push.render_height = (int32_t)qvk.extent_render.height;

	float strength = max(0.f, cvar_motion_blur_strength->value);

	/* Optional frame rate normalisation - see the cvar's own note. A frame
	   that took 1/30s at a reference of 60 gets its blur halved, so what is
	   left is proportional to world-space speed instead of to frame time.

	   Clamped hard on both sides. An alt-tab or a level load produces a frame
	   time of a second or more, and without the ceiling that one frame would
	   be scaled to nothing; a 1000fps menu frame would be scaled up 16x. */
	if (cvar_motion_blur_fps_ref->value > 0.f && last_frame_time > 0.f)
	{
		float reference_frame_time = 1.f / cvar_motion_blur_fps_ref->value;
		float scale = reference_frame_time / last_frame_time;

		strength *= max(0.125f, min(8.f, scale));
	}

	push.strength = strength;

	/* The cap is a fraction of screen height; the shader wants pixels. Never
	   let it reach zero - the shader divides by it when clamping. */
	push.max_blur_px = max(1.f, cvar_motion_blur_max->value * (float)output_extent.height);

	push.dynamic_deduction = max(0.f, min(1.f, cvar_motion_blur_dynamic->value));
	push.jitter            = max(0.f, min(1.f, cvar_motion_blur_jitter->value));
	push.min_velocity_px   = max(0.f, cvar_motion_blur_min_px->value);
	push.soft_z_extent     = max(0.01f, cvar_motion_blur_soft_z->value);

	push.num_samples = max(2, min(64, cvar_motion_blur_samples->integer));

	/* Advances the tap dither every frame so the ghosting a fixed pattern
	   leaves averages out over time rather than standing still. */
	push.frame_idx   = (int32_t)qvk.frame_counter;
	push.dlss_target = dlss ? 1 : 0;

	VkDescriptorSet desc_sets[] = {
		qvk.desc_set_ubo,
		qvk_get_current_desc_set_textures(),
		qvk.desc_set_vertex_buffer
	};

	/* The colour image was written by the resolve (TAA) or by DLSS; make those
	   writes visible to the reads this pass is about to do. */
	BARRIER_COMPUTE(cmd_buf, qvk.images[target_image]);

	vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
		pipeline_layout, 0, LENGTH(desc_sets), desc_sets, 0, 0);

	vkCmdPushConstants(cmd_buf, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(push), &push);

	vkCmdDispatch(cmd_buf,
		(output_extent.width  + 15) / 16,
		(output_extent.height + 15) / 16,
		1);

	/* Copy the blurred frame back over the colour image. This is what keeps
	   the pass transparent to everything downstream: bloom, tone mapping, FSR
	   and the final blit all still read the image they always read, and none
	   of them has to know whether this ran. */
	barrier_before_copy(cmd_buf, qvk.images[VKPT_IMG_MOTION_BLUR], qvk.images[target_image]);

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

	barrier_after_copy(cmd_buf, qvk.images[target_image]);

	return VK_SUCCESS;
}
