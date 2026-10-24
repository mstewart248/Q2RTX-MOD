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

/* ============================================================================
   CRT FILTER, step 2 of 2: the tube. Stands in for the final blit to the
   swapchain while pt_crt is on, so - like film grain - it runs after
   everything else that touches the picture, frame generation included, and
   before the HUD, which stays sharp. Grain, if it is on as well, goes on top.

   Everything here is done in linear light, which is how a phosphor responds
   to its beam.

   TUBE FACE. The picture is bent outward (barrel distortion, after Timothy
   Lottes' public-domain CRT shader), its corners are rounded, and outside
   it is the black of the bezel. A vignette darkens the edges.

   SCANLINES (pt_crt 1 and 2). The picture is the 240- or 480-line grid
   crt_downsample.comp made. Each line is drawn as a beam with a Gaussian
   cross-section, from the three nearest lines. A bright line's beam is wider
   than a dark one's, as on a real tube, so dark areas show black gaps
   between lines and highlights fill them in. Each beam is scaled to unit
   area, so a flat field keeps its average brightness - Quake is dark enough
   already. The beam is never allowed narrower than about half an output
   pixel, which would alias. Along the line the grid is reconstructed with a
   short Gaussian - the softness of the video signal - and a little of a
   blurred copy is mixed back in for the glow around bright areas.

   NATIVE (pt_crt 3). The frame itself, at full resolution, through the same
   tube face and mask. No scanlines: at 2160 lines they would sit a pixel
   apart and turn into moire under the curvature.

   PHOSPHOR MASK. Vertical red, green and blue stripes - an aperture grille -
   a multiple of three output pixels wide. The lit stripe of each colour is
   raised and the other two lowered so that each channel's average over a
   period is unchanged.
   ============================================================================ */

#version 450
#extension GL_GOOGLE_include_directive    : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_nonuniform_qualifier    : enable

#include "utils.glsl"

#define GLOBAL_UBO_DESC_SET_IDX 0
#include "global_ubo.h"

#define GLOBAL_TEXTURES_DESC_SET_IDX 1
#include "global_textures.h"

#include "final_blit_pc.glsl"
#include "film_grain.glsl"

layout(location = 0) in vec2 tex_coord;
layout(location = 0) out vec4 outColor;

const vec3 CRT_LUMA = vec3(0.2126, 0.7152, 0.0722);

/* Screen position (0..1) to the point of the picture shown there. */
vec2
crt_warp(vec2 uv)
{
	vec2 p = uv * 2.0 - 1.0;
	p *= vec2(1.0 + p.y * p.y * blit_pc.crt_warp_x,
	          1.0 + p.x * p.x * blit_pc.crt_warp_y);
	return p * 0.5 + 0.5;
}

/* 1 on the tube face, 0 on the bezel, with rounded corners and about a pixel
   of soft edge so the border does not stair-step along the curve. */
float
crt_face(vec2 picture_uv)
{
	vec2 size = vec2(blit_pc.out_width, blit_pc.out_height);
	vec2 p = (picture_uv - 0.5) * size;
	float r = blit_pc.crt_corner_radius_px;
	vec2 q = abs(p) - 0.5 * size + r;
	float outside = length(max(q, vec2(0))) + min(max(q.x, q.y), 0.0) - r;
	return clamp(0.5 - outside, 0.0, 1.0);
}

vec3
grid_fetch(int x, int y)
{
	x = clamp(x, 0, blit_pc.crt_grid_width - 1);
	return texelFetch(TEX_MOTION_BLUR, ivec2(x, y), 0).rgb;
}

/* One scanline at horizontal position x, in grid pixels. */
vec3
crt_line(int line, float x)
{
	float cx = x - 0.5;
	float i0 = floor(cx);
	float f = cx - i0;

	vec3 sum = vec3(0);
	float weight_sum = 0.0;

	for (int t = -1; t <= 2; t++)
	{
		float d = float(t) - f;
		float w = exp2(blit_pc.crt_hard_pix * d * d);
		sum += grid_fetch(int(i0) + t, line) * w;
		weight_sum += w;
	}

	return sum / weight_sum;
}

vec3
crt_scanlines(vec2 picture_uv)
{
	vec2 pos = picture_uv * vec2(blit_pc.crt_grid_width, blit_pc.crt_grid_height);
	int line = int(floor(pos.y));
	float dy = pos.y - (float(line) + 0.5);

	/* exp2(h d^2) has a standard deviation of 1/sqrt(-2 h ln2) lines. Keep
	   that at least half an output pixel. */
	float px_per_line = float(blit_pc.out_height) / float(blit_pc.crt_grid_height);
	float h_limit = -2.885 * px_per_line * px_per_line;

	/* What counts as white for the beam width: 1 in SDR, SDR white in HDR. */
	float white = blit_pc.hdr != 0 ? blit_pc.hdr_white_nits / 80.0 : 1.0;

	vec3 sum = vec3(0);

	for (int k = -1; k <= 1; k++)
	{
		int l = line + k;
		if (l < 0 || l >= blit_pc.crt_grid_height)
			continue;

		vec3 c = crt_line(l, pos.x);
		float lum = clamp(dot(c, CRT_LUMA) / white, 0.0, 1.0);
		float h = max(mix(blit_pc.crt_scan_hard_dark, blit_pc.crt_scan_hard_bright, lum), h_limit);

		float d = dy - float(k);
		/* Unit area: the integral of exp2(h d^2) is sqrt(pi / (-h ln2)). */
		float w = exp2(h * d * d) * sqrt(-h * 0.693147 / 3.14159265);
		sum += c * w;
	}

	return sum;
}

/* A soft copy of the grid around picture_uv, for the glow. Nine bilinear
   taps 1.5 grid pixels apart - each averaging four grid pixels - weighted
   as a Gaussian. */
vec3
crt_glow(vec2 picture_uv)
{
	vec2 grid = vec2(blit_pc.crt_grid_width, blit_pc.crt_grid_height);
	vec2 pos = picture_uv * grid;
	vec2 tex_size = vec2(textureSize(TEX_MOTION_BLUR, 0));

	vec3 sum = vec3(0);
	float weight_sum = 0.0;

	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec2 o = vec2(x, y) * 1.5;
			float w = exp(-dot(o, o) * (1.0 / 4.5));
			vec2 p = clamp(pos + o, vec2(1.0), grid - vec2(1.0));
			sum += textureLod(TEX_MOTION_BLUR, p / tex_size, 0).rgb * w;
			weight_sum += w;
		}
	}

	return sum / weight_sum;
}

vec3
crt_mask(ivec2 pixel)
{
	int pitch = max(3, blit_pc.crt_mask_pitch);
	int stripe = (pixel.x % pitch) * 3 / pitch;

	float s = blit_pc.crt_mask_strength;
	vec3 m = vec3(1.0 - 0.5 * s);
	m[stripe] = 1.0 + s;
	return m;
}

void
main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(blit_pc.out_width, blit_pc.out_height);
	vec2 picture_uv = crt_warp(uv);

	float face = crt_face(picture_uv);
	if (face <= 0.0)
	{
		outColor = vec4(0, 0, 0, 1);
		return;
	}

	vec3 color;
	if (blit_pc.crt_mode == 1)
	{
		color = crt_scanlines(picture_uv);
		if (blit_pc.crt_glow > 0.0)
			color = mix(color, crt_glow(picture_uv), blit_pc.crt_glow);
	}
	else
	{
		color = final_blit_sample(picture_uv * vec2(blit_pc.src_width, blit_pc.src_height));
	}

	color *= crt_mask(pixel);

	vec2 edge = picture_uv * (1.0 - picture_uv);
	color *= pow(clamp(16.0 * edge.x * edge.y, 0.0, 1.0), blit_pc.crt_vignette);

	color *= face;

	outColor = vec4(film_grain_apply(color, pixel), 1);
}
