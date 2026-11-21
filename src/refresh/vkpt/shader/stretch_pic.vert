/*
Copyright (C) 2018 Christoph Schied

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
// Vertex shader for UI rendering.
// The engine accumulates all UI rendering tasks in an array of StretchPic
// structures, which are passed to this shader. See `src/refresh/vkpt/draw.c`
// for more information.
// ========================================================================== //

#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

#include "utils.glsl"

out gl_PerVertex {
	vec4 gl_Position;
};

layout(location = 0) out vec4 color;
layout(location = 1) out flat uint tex_id;
layout(location = 2) out vec2 tex_coord;
// The quad's own rectangle in the texture, as (min_s, min_t, max_s, max_t).
// For a glyph this is its cell in the character atlas; the pixel shader keeps
// the bilinear footprint inside it. See stretch_pic.frag.
layout(location = 3) out flat vec4 tex_rect;
// Per-quad flags carried in the top five bits of tex_handle (STRETCH_PIC_FLAG_*
// in draw.c - keep in sync); bit 27, the map view's layer, is bit 4 here. Bit 0 here: the image is a video frame. Bit 1:
// draw it opaque, its alpha ignored. Bit 2: nearest texel, no filtering. Bit 3:
// its red channel as grey (a roughness/metallic map as the shader reads it).
layout(location = 4) out flat uint pic_flags;
// Bit 4 of pic_flags ('mat author's map view, the map as a layer over the base
// texture - vkpt_draw_stretch_subpic_layer): the base's handle; ~0 otherwise.
// The quad's colour carried it, and the layer's opacity, which is color.a then.
// Bits 5-6 with it: the emissive map's glow view (DRAW_GLOW_*, the colour's
// bits 24-25) - 0 the plain layer.
layout(location = 5) out flat uint base_id;

struct StretchPic {
	float x, y, w,   h;
	float s, t, w_s, h_t;
	uint color, tex_handle;
	// the quad's edges are (w, skew_y) and (skew_x, h): 0 for every upright
	// pic, set only by R_DrawRotatedPic
	float skew_x, skew_y;
};

layout(set = 0, binding = 0) readonly buffer SBO {
	StretchPic stretch_pics[];
};

vec2 positions[4] = vec2[](
	vec2(0.0, 1.0),
	vec2(0.0, 0.0),
	vec2(1.0, 1.0),
	vec2(1.0, 0.0)
);

void
main()
{
	StretchPic sp = stretch_pics[gl_InstanceIndex];
	vec2 corner   = positions[gl_VertexIndex];
	vec2 pos      = corner.x * vec2(sp.w, sp.skew_y) + corner.y * vec2(sp.skew_x, sp.h) + vec2(sp.x, sp.y);
	color         = unpackUnorm4x8(sp.color);
	
	color = pow(color, vec4(2.4));

	tex_coord     = vec2(sp.s, sp.t) + positions[gl_VertexIndex] * vec2(sp.w_s, sp.h_t);
	tex_id        = sp.tex_handle;
	pic_flags     = 0u;
	base_id       = ~0u;
	// ~0 is TEXNUM_WHITE, "no texture", and has every bit set - not flags
	if (sp.tex_handle != ~0u) {
		tex_id    = sp.tex_handle & 0x07ffffffu;
		// bits 30-31 -> 1, 2; bits 28-29 ('mat author's map view) -> 4, 8; bit 27 (its layer) -> 16
		pic_flags = (sp.tex_handle >> 30) | (((sp.tex_handle >> 28) & 3u) << 2) | (((sp.tex_handle >> 27) & 1u) << 4);
		if ((pic_flags & 16u) != 0u) {
			// no tint: the base's handle, and the opacity as given (not raised to 2.4)
			base_id = sp.color & 0xffffu;
			color   = vec4(1.0, 1.0, 1.0, float((sp.color >> 16) & 0xffu) / 255.0);
			pic_flags |= ((sp.color >> 24) & 3u) << 5;
		}
	}

	vec2 uv0      = vec2(sp.s, sp.t);
	vec2 uv1      = uv0 + vec2(sp.w_s, sp.h_t);
	tex_rect      = vec4(min(uv0, uv1), max(uv0, uv1));

	gl_Position = vec4(pos, 0.0, 1.0);
}

