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
   FILM GRAIN - shared by film_grain.frag (the final blit with grain),
   crt.frag (the CRT filter, grain on top) and final_blit_lanczos.frag (the
   scaled final blit). Needs utils.glsl and global_textures.h.

   WHERE IT RUNS: in the final blit to the swapchain, after everything else
   that touches the picture - tone mapping, FSR, DLSS 5 and frame generation -
   and before the HUD is drawn over it. That placement is the point:

     - DLSS, TAA and frame generation never see the grain. A temporal resolve
       would average it away as noise, and DLSS-G would warp and blend two
       frames' grain into its generated frames.
     - Every presented frame gets fresh grain, generated frames included,
       because each one goes through its own blit with its own seed.
     - The HUD is drawn afterwards and stays clean.

   WHAT IT LOOKS LIKE: monochrome grain that changes only brightness. The
   pixel's luminance is moved in perceptual space (sRGB-encoded, or PQ on an
   HDR display) and the colour is scaled by the change, so hue and saturation
   are kept. The amount follows 4p(1-p) of the perceptual brightness p: full in
   the mid-tones, fading to nothing at black and at white. Black stays black
   rather than being lifted to grey, and white is not pushed into clipping
   that would leave only the dark half of the grain.

   GRAIN SIZE: white noise per output pixel, blurred by a small Gaussian whose
   width is set host side in proportion to screen height, so the grain is the
   same size on screen at any resolution. The blur is evaluated directly - the
   noise is a hash, so a neighbour's value is just another hash - and divided
   by the root of the summed squared weights, which keeps it at unit variance
   whatever the kernel size.
   ============================================================================ */

#ifndef _FILM_GRAIN_GLSL_
#define _FILM_GRAIN_GLSL_

/* The parameters are in the shared final blit push constants. */
#include "final_blit_pc.glsl"

uvec3
grain_pcg3d(uvec3 v)
{
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	v ^= v >> 16u;
	v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
	return v;
}

/* Zero mean, unit variance, one value per pixel per frame. Three uniforms
   summed (variance 1/4) and doubled - close enough to Gaussian for grain, and
   bounded at +-3, so no single pixel ever flares. */
float
grain_white(ivec2 pixel)
{
	uvec3 h = grain_pcg3d(uvec3(uvec2(pixel), blit_pc.seed));
	vec3 u = vec3(h >> 8u) * (1.0 / 16777216.0);
	return (u.x + u.y + u.z - 1.5) * 2.0;
}

/* The grain field at one pixel: white noise through a Gaussian of
   blit_pc.grain_sigma_px, rescaled back to unit variance. */
float
grain_noise(ivec2 pixel)
{
	float inv_two_sigma2 = 0.5 / max(blit_pc.grain_sigma_px * blit_pc.grain_sigma_px, 1e-4);

	/* The Gaussian is separable: one row of weights, used for both axes.
	   grain_radius is at most 3 (draw.c). */
	float w1[4];
	for (int i = 0; i <= blit_pc.grain_radius; i++)
		w1[i] = exp(-float(i * i) * inv_two_sigma2);

	float sum = 0.0;
	float weight2_sum = 0.0;

	for (int dy = -blit_pc.grain_radius; dy <= blit_pc.grain_radius; dy++)
	{
		for (int dx = -blit_pc.grain_radius; dx <= blit_pc.grain_radius; dx++)
		{
			float w = w1[abs(dx)] * w1[abs(dy)];
			sum += w * grain_white(pixel + ivec2(dx, dy));
			weight2_sum += w * w;
		}
	}

	return sum * inversesqrt(weight2_sum);
}

/* SMPTE ST 2084. y is luminance / 10000 nits. */
float
grain_pq_encode(float y)
{
	const float m1 = 0.1593017578125, m2 = 78.84375;
	const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	float ym = pow(max(y, 0.0), m1);
	return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

float
grain_pq_decode(float p)
{
	const float m1 = 0.1593017578125, m2 = 78.84375;
	const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
	float pm = pow(max(p, 0.0), 1.0 / m2);
	return pow(max(pm - c1, 0.0) / (c2 - c3 * pm), 1.0 / m1);
}

/* color is what would go to the swapchain: display-linear, [0,1] for sRGB,
   scRGB (1.0 = 80 nits) for HDR. */
vec3
film_grain_apply(vec3 color, ivec2 pixel)
{
	if (blit_pc.grain_intensity <= 0.0 || any(isnan(color)) || any(isinf(color)))
		return color;

	float Y = dot(color, vec3(0.2126, 0.7152, 0.0722));

	float Y_new;
	if (blit_pc.hdr == 0)
	{
		/* At or past white the weight is zero; leave the pixel alone rather
		   than round-trip it through a clamped encode. */
		if (Y <= 0.0 || Y >= 1.0)
			return color;

		float p = linear_to_srgb(Y);
		float w = 4.0 * p * (1.0 - p);
		float p_new = clamp(p + blit_pc.grain_intensity * grain_noise(pixel) * w, 0.0, 1.0);
		Y_new = srgb_to_linear(p_new);
	}
	else
	{
		/* PQ is close to perceptually even across the HDR range. The mid-tone
		   curve is laid over the part of it below the display's SDR white,
		   and the grain is scaled by that part's size, so it is about as
		   strong against the picture as it is in SDR. Brighter than white
		   gets none, as in SDR. */
		if (Y <= 0.0)
			return color;

		float white = grain_pq_encode(blit_pc.hdr_white_nits / 10000.0);
		float p = grain_pq_encode(Y * (80.0 / 10000.0));
		float r = p / white;
		if (r >= 1.0)
			return color;

		float w = 4.0 * r * (1.0 - r);
		float p_new = max(p + blit_pc.grain_intensity * grain_noise(pixel) * w * white, 0.0);
		Y_new = grain_pq_decode(p_new) * (10000.0 / 80.0);
	}

	return color * (Y_new / Y);
}

#endif /* _FILM_GRAIN_GLSL_ */
