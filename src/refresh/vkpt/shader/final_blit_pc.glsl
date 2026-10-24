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
   Push constants and source access for the effects the final blit applies on
   its way to the swapchain - film grain (film_grain.glsl) and the CRT filter
   (crt.frag, crt_downsample.comp). Include after global_textures.h.
   ============================================================================ */

#ifndef _FINAL_BLIT_PC_GLSL_
#define _FINAL_BLIT_PC_GLSL_

/* Mirrors FinalBlit_PushConstants_t in draw.c. */
layout(push_constant) uniform FinalBlitPushConstants {
	/* Which image to read - see final_blit_fetch - and the region of it the
	   blit covers. Unused by final_blit_lanczos.frag, which picks its own. */
	int   source;
	int   src_width;
	int   src_height;
	/* Size of the swapchain region being written. */
	int   out_width;
	int   out_height;
	/* 1 when the swapchain is scRGB (HDR), 0 for sRGB. */
	int   hdr;
	/* HDR only: the brightness that plays the part of SDR white. */
	float hdr_white_nits;
	/* Changes for every presented frame. */
	uint  seed;

	/* FILM GRAIN. Standard deviation in perceptual units at mid-grey; 0 is
	   off. Kernel width and half-width in output pixels. */
	float grain_intensity;
	float grain_sigma_px;
	int   grain_radius;

	/* CRT. 0 off, 1 a low line count (crt_downsample.comp fills the grid in
	   IMG_MOTION_BLUR first), 2 the tube at native resolution. */
	int   crt_mode;
	int   crt_grid_width;
	int   crt_grid_height;
	/* Barrel distortion of the tube face, and the radius of its corners. */
	float crt_warp_x;
	float crt_warp_y;
	float crt_corner_radius_px;
	/* exp2 sharpness of the horizontal beam, in grid pixels. */
	float crt_hard_pix;
	/* exp2 sharpness of a scanline, in lines, for black and for white. */
	float crt_scan_hard_dark;
	float crt_scan_hard_bright;
	/* Phosphor mask: how far the lit stripe rises above 1, and its period in
	   output pixels (a multiple of 3: one stripe per colour). */
	float crt_mask_strength;
	int   crt_mask_pitch;
	/* Share of the picture replaced by a blurred copy - halation. */
	float crt_glow;
	/* Exponent of the edge falloff; 0 is none. */
	float crt_vignette;
} blit_pc;

/* One texel of the source. The order mirrors final_blit_sources[] in draw.c:
   every image the final blit is handed in play. */
vec3
final_blit_fetch(ivec2 p)
{
	switch (blit_pc.source)
	{
	case 0:  return texelFetch(TEX_TAA_OUTPUT,      p, 0).rgb;
	case 1:  return texelFetch(TEX_DLSS_OUTPUT,     p, 0).rgb;
	case 2:  return texelFetch(TEX_FSR_EASU_OUTPUT, p, 0).rgb;
	case 3:  return texelFetch(TEX_FSR_RCAS_OUTPUT, p, 0).rgb;
	case 4:  return texelFetch(TEX_DLSS_FG_OUTPUT,  p, 0).rgb;
	case 5:  return texelFetch(TEX_DLSS_FG_OUTPUT2, p, 0).rgb;
	case 6:  return texelFetch(TEX_DLSS_FG_OUTPUT3, p, 0).rgb;
	case 7:  return texelFetch(TEX_DLSS_FG_OUTPUT4, p, 0).rgb;
	case 8:  return texelFetch(TEX_DLSS_FG_OUTPUT5, p, 0).rgb;
	}
	return vec3(0);
}

#define FINAL_BLIT_BILINEAR(tex) textureLod(tex, pos_px / vec2(textureSize(tex, 0)), 0).rgb

/* The source filtered bilinearly at a position in source pixels. Clamped
   inside the covered region: the images are often larger than it, and the
   sampler repeats, so a tap past the edge would wrap to the far side. */
vec3
final_blit_sample(vec2 pos_px)
{
	pos_px = clamp(pos_px, vec2(0.5), vec2(blit_pc.src_width, blit_pc.src_height) - vec2(0.5));

	switch (blit_pc.source)
	{
	case 0:  return FINAL_BLIT_BILINEAR(TEX_TAA_OUTPUT);
	case 1:  return FINAL_BLIT_BILINEAR(TEX_DLSS_OUTPUT);
	case 2:  return FINAL_BLIT_BILINEAR(TEX_FSR_EASU_OUTPUT);
	case 3:  return FINAL_BLIT_BILINEAR(TEX_FSR_RCAS_OUTPUT);
	case 4:  return FINAL_BLIT_BILINEAR(TEX_DLSS_FG_OUTPUT);
	case 5:  return FINAL_BLIT_BILINEAR(TEX_DLSS_FG_OUTPUT2);
	case 6:  return FINAL_BLIT_BILINEAR(TEX_DLSS_FG_OUTPUT3);
	case 7:  return FINAL_BLIT_BILINEAR(TEX_DLSS_FG_OUTPUT4);
	case 8:  return FINAL_BLIT_BILINEAR(TEX_DLSS_FG_OUTPUT5);
	}
	return vec3(0);
}

#undef FINAL_BLIT_BILINEAR

#endif /* _FINAL_BLIT_PC_GLSL_ */
