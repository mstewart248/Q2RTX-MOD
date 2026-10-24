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

// ========================================================================== //
// The final blit with film grain. Stands in for vkpt_final_blit_simple's
// vkCmdBlitImage while pt_film_grain is on: the same nearest-neighbour copy
// to the swapchain, with grain added on the way. See film_grain.glsl.
// ========================================================================== //

#version 450
#extension GL_GOOGLE_include_directive    : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier    : enable

#include "utils.glsl"

#define GLOBAL_UBO_DESC_SET_IDX 0
#include "global_ubo.h"

#define GLOBAL_TEXTURES_DESC_SET_IDX 1
#include "global_textures.h"

#include "film_grain.glsl"

layout(location = 0) in vec2 tex_coord;
layout(location = 0) out vec4 outColor;

void
main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);

	/* The pixel vkCmdBlitImage with VK_FILTER_NEAREST would have taken - the
	   source can be half the output size (the 2x nearest upscale). */
	ivec2 src_size = ivec2(blit_pc.src_width, blit_pc.src_height);
	vec2 scale = vec2(src_size) / vec2(blit_pc.out_width, blit_pc.out_height);
	ivec2 src = clamp(ivec2((vec2(pixel) + 0.5) * scale), ivec2(0), src_size - 1);

	vec3 color = final_blit_fetch(src);

	outColor = vec4(film_grain_apply(color, pixel), 1);
}
