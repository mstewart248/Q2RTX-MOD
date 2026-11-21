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
	float ui_hdr_video_gamma;
	float ui_hdr_gamma;     // tm_hdr_sdr_gamma: the whole UI as an SDR monitor shows it; 0 = off
};

layout(location = 0) in vec4 color;
layout(location = 1) in flat uint tex_id;
layout(location = 2) in vec2 tex_coord;
layout(location = 3) in flat vec4 tex_rect;
layout(location = 4) in flat uint pic_flags;  // bit 0: video frame; bit 1: ignore alpha; bit 2: nearest; bit 3: red as grey; bit 4: layer over base_id; bits 5-6: its glow view
layout(location = 5) in flat uint base_id;    // bit 4's base texture

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
		vec2 tex_size = vec2(max(global_textureSize(tex_id, 0), ivec2(1)));
		vec2 half_texel = 0.5 / tex_size;
		vec2 tc = clamp(tex_coord, tex_rect.xy + half_texel, tex_rect.zw - half_texel);
		// 'mat author's map view: each texel a sharp square (sampled at its centre)
		if ((pic_flags & 4u) != 0u)
			tc = (min(floor(clamp(tex_coord, vec2(0), vec2(1)) * tex_size), tex_size - 1) + 0.5) / tex_size;
		vec4 t = global_textureLod(tex_id, tc, 0);
		const float stored_alpha = t.a;     // a painted emissive map's brightness (the glow view)

		// 'mat print' showing a map as stored, whatever its alpha holds
		if ((pic_flags & 2u) != 0u)
			t.a = 1.0;
		// a roughness or metallic map as the path tracer reads it: red only
		if ((pic_flags & 8u) != 0u)
			t.rgb = t.rrr;

		/* 'mat author's map view: the map as a layer over the base texture, at
		   color.a. Mixed here, in sRGB-encoded values as an image editor's layer
		   opacity is, not blended as light - in light a dark base under a bright
		   map is lost, and more so with vid_hdr (vkpt_draw_stretch_subpic_layer).
		   Same coordinates as the map, at the base's own size; opaque. */
		if ((pic_flags & 16u) != 0u) {
			vec2 base_size = vec2(max(global_textureSize(base_id, 0), ivec2(1)));
			vec2 base_tc = clamp(tex_coord, tex_rect.xy + 0.5 / base_size, tex_rect.zw - 0.5 / base_size);
			if ((pic_flags & 4u) != 0u)
				base_tc = (min(floor(clamp(tex_coord, vec2(0), vec2(1)) * base_size), base_size - 1) + 0.5) / base_size;
			vec3 b = global_textureLod(base_id, base_tc, 0).rgb;
			const uint glow = (pic_flags >> 5) & 3u;
			if (glow != 0u) {
				/* The emissive map's GLOW VIEW (DRAW_GLOW_*): the base texture as
				   bright as it glows, what glows less under a purple veil - color.a
				   of it at 0%, none at 100% and up (Matt, 11-21: "I can't see what
				   has been marked as emissive"). The brightness, 1 = 100%: a painted
				   map's alpha, stored at half; any other glow's luminance over the
				   texture's, as the first stroke converts it (create_emit_map). */
				const vec3 rec709 = vec3(0.2126, 0.7152, 0.0722);
				const float lb = dot(b, rec709);
				float k = glow == 1u ? stored_alpha * 2.0
				        : glow == 2u ? (lb > 1e-4 ? dot(t.rgb, rec709) / lb : 0.0)
				        : 0.0;
				k = clamp(k, 0.0, 2.0);
				// over 100% the texture brighter; under it, veiled
				vec3 lit = min(linear_to_srgb(clamp(b, vec3(0), vec3(1))) * max(k, 1.0), vec3(1));
				t.rgb = srgb_to_linear(mix(lit, vec3(0.32, 0.02, 0.42), color.a * (1.0 - min(k, 1.0))));
			}
			else
				t.rgb = srgb_to_linear(mix(linear_to_srgb(clamp(b, vec3(0), vec3(1))),
				                           linear_to_srgb(clamp(t.rgb, vec3(0), vec3(1))), color.a));
			t.a = 1.0;
			c = vec4(1.0);
		}

		/* A video frame on an HDR screen: undo the piecewise sRGB decode the
		   sampler did and apply the power curve an SDR monitor shows instead.
		   The piecewise toe lifts near black 10-60x and with it the 8x8 block
		   grid of every dark Theora frame - see ui_hdr_video_gamma in draw.c. */
		if (spec_tone_mapping_hdr != 0 && (pic_flags & 1u) != 0u && ui_hdr_video_gamma > 0)
			t.rgb = pow(linear_to_srgb(clamp(t.rgb, vec3(0), vec3(1))), vec3(ui_hdr_video_gamma));

		c *= t;
	}
	if(spec_tone_mapping_hdr != 0) {
		/* The UI as an SDR monitor shows it - the sRGB encode an SDR swapchain does,
		   decoded with a plain power - the same match tone_mapping_hdr.glsl gives the
		   world (tm_hdr_sdr_gamma). Without it dark panels, the console and shadowed
		   HUD art come out lifted. A video frame already had its own curve above. */
		bool video_curve = (pic_flags & 1u) != 0u && ui_hdr_video_gamma > 0;
		if (ui_hdr_gamma > 0 && !video_curve)
			c.rgb = pow(linear_to_srgb(clamp(c.rgb, vec3(0), vec3(1))), vec3(ui_hdr_gamma));
		c.rgb *= ui_hdr_nits / 80;
		c.rgb = apply_saturation_scale(c.rgb, tm_hdr_saturation_scale * 0.01);
	}
	outColor = c;
}
