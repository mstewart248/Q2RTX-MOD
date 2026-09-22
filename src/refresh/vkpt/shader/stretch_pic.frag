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
// Pixel shader for UI rendering.
// ========================================================================== //

#version 450
#extension GL_GOOGLE_include_directive    : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier    : enable

layout(constant_id = 0) const uint spec_tone_mapping_hdr = 0;

#define GLOBAL_TEXTURES_DESC_SET_IDX 1
#include "global_textures.h"

#include "utils.glsl"

layout(set = 2, binding = 2, std140) uniform UBO {
	float ui_hdr_nits;
	float tm_hdr_saturation_scale;
};

layout(location = 0) in vec4 color;
layout(location = 1) in flat uint tex_id;
layout(location = 2) in vec2 tex_coord;
layout(location = 3) in flat vec4 tex_rect;

layout(location = 0) out vec4 outColor;

void
main()
{
	vec4 c = color;
	if(tex_id != ~0u) {
		/* Keep the bilinear footprint inside the quad's own rectangle.

		   The character atlas packs the whole font into one texture as a 16x16
		   grid of cells, and a glyph quad maps to exactly one of those cells.
		   Nothing separates a cell from its neighbours, so as soon as the glyph
		   is magnified the outermost row of screen pixels sits less than half a
		   texel from the cell border and the bilinear tap reaches across it,
		   printing an edge row of whatever letter happens to be stored above,
		   below or beside this one. That is the stray bar over a character that
		   the artifact reports show.

		   How much leaks is set purely by the magnification: the weight on the
		   neighbour is 0.5 - 0.5/M for a glyph blown up M times, so it is zero
		   at 1:1 and reaches 26% at the ~2x a menu runs at on a 1080p window.
		   A quarter of a neighbouring glyph sounds faint, but the UI is written
		   in linear light and displayed through a gamma, which lifts that 26%
		   to roughly 55% of full brightness on screen - hence a bar that reads
		   as solid rather than as a hint. It stays invisible for anyone whose
		   font texture is large enough to land at or below 1:1, which is why
		   the same build looks clean on one machine and streaked on another.

		   Clamping half a texel inside the rectangle pins those edge samples to
		   the centres of the border texels. The mapping is untouched everywhere
		   else, so this costs no sharpness at 1:1 - unlike insetting the quad's
		   texture coordinates, which would shrink the glyph across 7 texels
		   instead of 8 and soften it at every scale.

		   Every other UI quad passes its full image rectangle, so for those this
		   is just clamp-to-edge and stops the opposite edge wrapping in. Tiled
		   backgrounds span many repeats and only ever clamp the outermost half
		   texel of the whole tiled area, leaving the repeats within it intact. */
		/* max() because an out-of-range handle reports a zero size. */
		vec2 half_texel = 0.5 / vec2(max(global_textureSize(tex_id, 0), ivec2(1)));
		vec2 tc = clamp(tex_coord, tex_rect.xy + half_texel, tex_rect.zw - half_texel);
		c *= global_textureLod(tex_id, tc, 0);
	}
	if(spec_tone_mapping_hdr != 0) {
		c.rgb *= ui_hdr_nits / 80;
		c.rgb = apply_saturation_scale(c.rgb, tm_hdr_saturation_scale * 0.01);
	}
	outColor = c;
}
