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

// ========================================================================== //
// The underwater screen warp, as a PRIMARY RAY perturbation.
//
// Both the software and the GL renderers do this as a post-process: the frame
// is rendered flat and then resampled through a pair of sine waves
// (`D_WarpScreen`, RDF_UNDERWATER). That is the wrong end of the pipe here.
// A post-process warp would run AFTER DLSS/TAAU, so it would resample an
// already-upscaled image and throw the sharpness away; it has no data outside
// the frame, so the edges of the screen have to be either clamped (smeared) or
// zoomed in to hide the gap; and it would drag the HUD and the crosshair along
// with it unless it were threaded in between the world and the 2D pass.
//
// Bending the camera ray instead removes all three problems. There is no
// "outside the image" for a ray - a pixel displaced past the frustum edge just
// traces the geometry that is actually there - the warp happens at native
// sampling rate before any upscaler sees it, and it only ever touches the
// world because the 2D pass does not go through this shader at all. It is also
// closer to what the effect is pretending to be: a rippled water surface
// between the eye and the scene refracting the view direction.
//
// The cost is that the mapping from a pixel to a screen position is no longer
// the identity, so MOTION VECTORS have to be expressed back in pixel space -
// see underwater_unwarp() and its two callers in primary_rays.rgen. Getting
// that wrong does not look like a small error: A-SVGF and DLSS would both
// chase a history that is one warp-offset away from where they think it is,
// and the whole screen would smear.
// ========================================================================== //

#ifndef  _WATER_WARP_GLSL_
#define  _WATER_WARP_GLSL_

// The screen-space offset applied to a pixel's sample position, in UV units.
//
// `previous` selects the previous frame's time and strength, which is what
// makes a motion vector across a changing warp correct - including across the
// fade in and out of the water, where the strength itself is moving.
vec2
underwater_warp_offset(vec2 uv, bool previous)
{
	float strength = previous ? global_ubo.water_warp_prev : global_ubo.water_warp;

	// Dry, and the overwhelmingly common case: no sines, no divides.
	if(strength <= 0)
		return vec2(0);

	float t = previous ? global_ubo.time_prev : global_ubo.time;

	// Amplitude is a fraction of the screen HEIGHT on both axes. Quake's warp
	// is measured in pixels and is therefore square; expressing it in UV
	// directly would stretch it by the aspect ratio and make the horizontal
	// ripple two-and-a-bit times the size of the vertical one at 21:9.
	float aspect = float(global_ubo.width) / float(global_ubo.height);
	vec2 amp = global_ubo.pt_water_warp_amp * vec2(1.0 / aspect, 1.0);

	// The classic cross-coupled pair: x is displaced by a wave travelling down
	// the screen, y by one travelling across it. The .yx swizzle IS the
	// coupling - feeding each axis its own coordinate would give a separable
	// grid that reads as a lens, not as water.
	vec2 phase = uv.yx * global_ubo.pt_water_warp_freq + vec2(t * global_ubo.pt_water_warp_speed);
	vec2 offset = amp * sin(phase * (2.0 * M_PI));

	// A slow breathing zoom on top, which is the other half of what the
	// original games do underwater (they wobble the FOV). Scaling the offset
	// with distance from screen centre is exactly a FOV change, but it rides
	// the same warp/unwarp path as the ripple so the motion vectors stay
	// correct for free - a real FOV wobble would need P and P_prev to
	// disagree, which several other passes read. Off by default.
	float zoom = global_ubo.pt_water_warp_zoom
	           * sin(t * global_ubo.pt_water_warp_zoom_speed * (2.0 * M_PI));
	offset += (uv - vec2(0.5)) * zoom;

	return offset * strength;
}

// Pixel position -> the screen position its ray should be cast through.
vec2
underwater_warp(vec2 uv, bool previous)
{
	return uv + underwater_warp_offset(uv, previous);
}

// Screen position -> the pixel it lands on. The inverse of underwater_warp().
//
// The warp has no closed-form inverse, but it is a small displacement of a
// smooth field, so the fixed point u = uv - offset(u) converges immediately:
// one iteration leaves an error of order (d offset/d uv) times the offset,
// which at the default amplitude is a small fraction of a pixel, and two
// leaves nothing measurable. Both motion-vector endpoints use this same
// function, so what error remains is common to them and cancels rather than
// accumulating into drift.
vec2
underwater_unwarp(vec2 uv, bool previous)
{
	if((previous ? global_ubo.water_warp_prev : global_ubo.water_warp) <= 0)
		return uv;

	vec2 u = uv - underwater_warp_offset(uv, previous);
	     u = uv - underwater_warp_offset(u,  previous);
	return u;
}

#endif /*_WATER_WARP_GLSL_*/
