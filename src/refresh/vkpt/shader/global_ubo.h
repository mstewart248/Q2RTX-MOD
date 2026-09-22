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

#ifndef  _GLOBAL_UBO_H_
#define  _GLOBAL_UBO_H_

#include "constants.h"
#include "shader_structs.h"

#define GLOBAL_UBO_BINDING_IDX               0
#define GLOBAL_INSTANCE_BUFFER_BINDING_IDX   1


#define UBO_CVAR_DO(name, default_value) GLOBAL_UBO_VAR_LIST_DO(float, name)

// The variables listed in UBO_CVAR_LIST are registered as console variables and copied directly into the UBO.
// See main.c for the implementation of that.

// Variables that have "_lf", "_hf" or "_spec" suffix apply to the low-frequency, high-frequency or specular lighting channels, respectively.

#define UBO_CVAR_LIST \
	UBO_CVAR_DO(flt_antilag_hf, 1) /* A-SVGF anti-lag filter strength, [0..inf) */ \
	UBO_CVAR_DO(flt_antilag_lf, 0.2) \
	UBO_CVAR_DO(flt_antilag_spec, 2) \
	UBO_CVAR_DO(flt_antilag_spec_motion, 0.004) /* scaler for motion vector scaled specular anti-blur adjustment */ \
	UBO_CVAR_DO(flt_atrous_depth, 0.5) /* wavelet fitler sensitivity to depth, [0..inf) */ \
	UBO_CVAR_DO(flt_atrous_deflicker_lf, 2) /* max brightness difference between adjacent pixels in the LF channel, (0..inf) */ \
	UBO_CVAR_DO(flt_atrous_hf, 4) /* number of a-trous wavelet filter iterations on the LF channel, [0..4] */ \
	UBO_CVAR_DO(flt_atrous_lf, 4) \
	UBO_CVAR_DO(flt_atrous_spec, 3) \
	UBO_CVAR_DO(flt_atrous_lum_hf, 16) /* wavelet filter sensitivity to luminance, [0..inf) */ \
	UBO_CVAR_DO(flt_atrous_normal_hf, 64) /* wavelet filter sensitivity to normals, [0..inf) */ \
	UBO_CVAR_DO(flt_atrous_normal_lf, 8) \
	UBO_CVAR_DO(flt_atrous_normal_spec, 1) \
	UBO_CVAR_DO(flt_enable, 1) /* switch for the entire SVGF reconstruction, 0 or 1 */ \
	UBO_CVAR_DO(flt_fixed_albedo, 0) /* if nonzero, replaces surface albedo with that value after filtering */ \
	UBO_CVAR_DO(flt_grad_weapon, 0.25) /* gradient scale for the first person weapon, [0..1] */ \
	UBO_CVAR_DO(flt_min_alpha_color_hf, 0.02) /* minimum weight for the new frame data, color channel, (0..1] */ \
	UBO_CVAR_DO(flt_min_alpha_color_lf, 0.01) \
	UBO_CVAR_DO(flt_min_alpha_color_spec, 0.01) \
	UBO_CVAR_DO(flt_min_alpha_moments_hf, 0.01) /* minimum weight for the new frame data, moments channel, (0..1] */  \
	UBO_CVAR_DO(flt_scale_hf, 1) /* overall per-channel output scale, [0..inf) */ \
	UBO_CVAR_DO(flt_scale_lf, 1) \
	UBO_CVAR_DO(flt_scale_overlay, 1.0) /* scale for transparent and emissive objects visible with primary rays */ \
	UBO_CVAR_DO(flt_scale_spec, 1) \
	UBO_CVAR_DO(flt_show_gradients, 0) /* switch for showing the gradient values as overlay image, 0 or 1 */ \
	UBO_CVAR_DO(flt_taa, 2) /* temporal anti-aliasing mode: 0 = off, 1 = regular TAA, 2 = temporal upscale */ \
	UBO_CVAR_DO(flt_taa_anti_sparkle, 0.25) /* strength of the anti-sparkle filter of TAA, [0..1] */ \
	UBO_CVAR_DO(flt_taa_variance, 1.0) /* temporal AA variance window scale, 0 means disable NCC, [0..inf) */ \
	UBO_CVAR_DO(flt_taa_history_weight, 0.95) /* temporal AA weight of the history sample, [0..1) */ \
	UBO_CVAR_DO(flt_temporal_hf, 1) /* temporal filter strength, [0..1] */ \
	UBO_CVAR_DO(flt_temporal_lf, 1) \
	UBO_CVAR_DO(flt_temporal_spec, 1) \
	UBO_CVAR_DO(pt_aperture, 2.0) /* aperture size for the Depth of Field effect, in world units */ \
	UBO_CVAR_DO(pt_aperture_angle, 0) /* rotation of the polygonal aperture, [0..1] */ \
	UBO_CVAR_DO(pt_aperture_type, 0) /* number of aperture polygon edges, circular if less than 3 */ \
	UBO_CVAR_DO(pt_beam_softness, 1.0) /* beam softness */ \
	UBO_CVAR_DO(pt_bump_scale, 1.0) /* scale for normal maps [0..1] */ \
	UBO_CVAR_DO(pt_cameras, 1) /* switch for security cameras, 0 or 1 */ \
	UBO_CVAR_DO(pt_direct_polygon_lights, 1) /* switch for direct lighting from local polygon lights, 0 or 1 */ \
	UBO_CVAR_DO(pt_direct_roughness_threshold, 0.18) /* roughness value where the path tracer switches direct light specular sampling from NDF based to light based, [0..1] */ \
	UBO_CVAR_DO(pt_direct_dyn_lights, 1) /* switch for direct lighting from local sphere lights, 0 or 1 */ \
	UBO_CVAR_DO(pt_direct_sun_light, 1) /* switch for direct lighting from the sun, 0 or 1 */ \
	UBO_CVAR_DO(pt_explosion_brightness, 4.0) /* brightness factor for explosions */ \
	UBO_CVAR_DO(pt_fake_roughness_threshold, 0.20) /* roughness value where the path tracer starts switching indirect light specular sampling from NDF based to SH based, [0..1] */ \
	UBO_CVAR_DO(pt_focus, 200) /* focal distance for the Depth of Field effect, in world units */ \
	UBO_CVAR_DO(pt_fog_brightness, 0.01) /* global multiplier for the color of fog volumes */ \
	UBO_CVAR_DO(pt_indirect_polygon_lights, 1) /* switch for bounce lighting from local polygon lights, 0 or 1 */ \
	UBO_CVAR_DO(pt_indirect_dyn_lights, 1) /* switch for bounce lighting from local sphere lights, 0 or 1 */ \
	UBO_CVAR_DO(pt_light_stats, 1) /* switch for statistical light PDF correction, 0 or 1 */ \
	UBO_CVAR_DO(pt_max_log_sky_luminance, -3) /* maximum sky luminance, log2 scale, used for polygon light selection, (-inf..inf) */ \
	UBO_CVAR_DO(pt_min_log_sky_luminance, -10) /* minimum sky luminance, log2 scale, used for polygon light selection, (-inf..inf) */ \
	UBO_CVAR_DO(pt_metallic_override, -1) /* overrides metallic parameter of all materials if non-negative, [0..1] */ \
	UBO_CVAR_DO(pt_ndf_trim, 0.9) /* trim factor for GGX NDF sampling (0..1] */ \
	UBO_CVAR_DO(pt_num_bounce_rays, 1) /* number of bounce rays, valid values are 0 (disabled), 0.5 (half-res diffuse), 1 (full-res diffuse + specular), 2 (two bounces) */ \
	UBO_CVAR_DO(pt_particle_softness, 0.7) /* particle softness */ \
	UBO_CVAR_DO(pt_particle_brightness, 100) /* particle brightness */ \
	UBO_CVAR_DO(pt_reflect_refract, 2) /* number of reflection or refraction bounces: 0, 1 or 2 */ \
	UBO_CVAR_DO(pt_restir, 1) /* switch for using RIS or ReSTIR, 0 or 1 */ \
	UBO_CVAR_DO(pt_restir_spatial, 0) /* ReSTIR spatial samples; 0 = temporal-only, the right setting under DLSS-RR */ \
	UBO_CVAR_DO(pt_restir_max_w, 12.0) /* ReSTIR max weight clamp */ \
	UBO_CVAR_DO(pt_restir_permutation, 1) /* ReSTIR permutation sampling, 0 or 1 */ \
	UBO_CVAR_DO(pt_restir_pairwise, 0) /* ReSTIR spatial reuse weighting: 0 legacy count-weighted sum, 1 pairwise MIS */ \
	UBO_CVAR_DO(pt_restir_boiling, 0) /* ReSTIR boiling filter threshold as a multiple of the local mean, 0 disables */ \
	UBO_CVAR_DO(pt_restir_m_clamp, 32) /* ReSTIR temporal history length: how many samples a reused reservoir claims to carry. 32 was hard-coded; lower trades noise for less lag and streaking */ \
	UBO_CVAR_DO(pt_restir_debug, 0) /* ReSTIR debug view, same numbering the pt_restir 10..26 views used, 0 disables */ \
	UBO_CVAR_DO(pt_glass_secondary_stochastic, 0) /* secondary glass Fresnel: 0 most-probable path, 1 importance sampled, 2 clamped like reference mode */ \
	UBO_CVAR_DO(pt_roughness_override, -1) /* overrides roughness of all materials if non-negative, [0..1] */ \
	UBO_CVAR_DO(pt_specular_anti_flicker, 2) /* fade factor for rough reflections of surfaces far away, [0..inf) */ \
	UBO_CVAR_DO(pt_specular_mis, 1) /* enables the use of MIS between specular direct lighting and BRDF specular rays */ \
	UBO_CVAR_DO(pt_show_sky, 0) /* switch for showing the sky polygons, 0 or 1 */ \
	UBO_CVAR_DO(pt_sun_bounce_range, 2000) /* range limiter for indirect lighting from the sun, helps reduce noise, (0..inf) */ \
	UBO_CVAR_DO(pt_sun_specular, 1.0) /* scale for the direct specular reflection of the sun */ \
	UBO_CVAR_DO(pt_texture_lod_bias, 0) /* LOD bias for textures, (-inf..inf) */ \
	UBO_CVAR_DO(pt_toksvig, 1) /* intensity of Toksvig roughness correction, [0..inf) */ \
	UBO_CVAR_DO(pt_thick_glass, 0) /* switch for thick glass refraction: 0 (disabled), 1 (reference mode only), 2 (real-time mode) */ \
	UBO_CVAR_DO(pt_dlss, 0) \
	UBO_CVAR_DO(pt_dlssdn, 0) \
	UBO_CVAR_DO(pt_dlss_spec_hitdist, 1) /* feed DLSS-RR a real specular hit distance on opaque surfaces (RR guide 3.4.9); 0 restores the old constant 0 */ \
	UBO_CVAR_DO(pt_dlss_mirror_guides, 1) /* chrome models (the viewer weapon) hand DLSS-RR the mirror's own normal/albedo/roughness instead of the reflected surface's; 0 restores the old behaviour */ \
	UBO_CVAR_DO(pt_dlss_linear_z, 1) /* DLSS depth is view-space Z (matches the motion vectors, RR guide 3.4.7); 0 restores radial distance */ \
	UBO_CVAR_DO(pt_dlss_sky_mv_jitter, 1) /* anchor sky/reflected-sky motion vectors at the jittered sample position like geometry; 0 restores the pixel-centre anchor */ \
	UBO_CVAR_DO(pt_dlss_mv_bounce_guard, 1) /* only build the "apparent position" reflection/refraction motion vector while the path is still first-order, and reject a projection that blows up; 0 restores the old unguarded behaviour that put Inf into PT_MOTION at pt_reflect_refract > 1 */ \
	UBO_CVAR_DO(pt_dlss_guide_field, 3) /* split-field glass/water: which layer the DLSS guides describe. 0 refraction (base surface), 1 whichever layer is brighter (what the classic checkerboard interleave does - a per-pixel compare of two NOISY luminances, so the guides flip pixel to pixel and frame to frame and RR answers by blurring), 2 reflection, 3 reflection on GLASS only - water and slime keep the refraction, 4 Fresnel-weighted: geometry follows whichever layer the split's F says dominates (smooth, noise-free, one coherent region edge per pane) while the diffuse and specular albedo RR demodulates by are MIXED by F so the round-trip matches a pixel that really holds F*reflection + (1-F)*refraction, 5 mirror guides on the SPLITTING surface - the primary guides describe the pane itself and the reflection rides entirely in the specular channels, which is the split DLSS-RR documents for a mirror and the only mode that hands RR the REFLECTOR roughness (every other mode hands it the roughness of whatever the ray landed on, and that is what sizes RR specular filter and blurs the reflection). Suits water and glossy floors; a clear window is better served by 0, so prefer dlss_guide_field in the material over one global answer */ \
	UBO_CVAR_DO(pt_dlss_guide_motion, 0) /* which field supplies the PRIMARY motion vector and depth on a split pixel, independently of pt_dlss_guide_field. 0 follow the guide field (old behaviour). 1 refraction - the see-through base surface, which is what most of a window pixel actually is; stops the reflected character's mirrored motion being stamped on the background seen through the glass, which is what makes scenery behind a window bob with the player under guide field 2/3. 2 reflection. The reflection keeps its own vector either way, through pInMotionVectorsReflections. */ \
	UBO_CVAR_DO(pt_dlss_guide_mirror_roughness, 0.0) /* pt_dlss_guide_field 5 only: the roughness handed to DLSS-RR for the splitting surface. The reflection traced for glass and water is a PERFECT mirror - reflect(), no GGX lobe, no roughness sampling anywhere in that path - so the honest answer is 0, and 0 is what tells RR to keep the reflection sharp. It must not be the material roughness: get_material defaults that to 1 when a material has no roughness map, which every BSP-surface-flag glass pane lacks, and 1 asks RR for its widest specular filter. Raise slightly for normal-mapped (bumpy) glass if the reflection reads too hard, [0..1] */ \
	UBO_CVAR_DO(pt_dlss_guide_fresnel, 0.5) /* pt_dlss_guide_field 4 only: the Fresnel weight at or above which the GEOMETRIC guides (depth, normal, motion, roughness) follow the reflection rather than the refraction. 0.5 is the physical crossover - the reflection is more than half the pixel. Lower hands the reflection the geometry over more of the pane, which keeps it sharper at the cost of describing the view through the glass less well; the albedo mix is unaffected either way, [0..1] */ \
	UBO_CVAR_DO(pt_water_density, 0.5) /* scale for light extinction in water and other media, [0..inf) */ \
	UBO_CVAR_DO(tm_debug, 0) /* switch to show the histogram (1) or tonemapping curve (2) */ \
	UBO_CVAR_DO(tm_dyn_range_stops, 7.0) /* Effective display dynamic range in linear stops = log2((max+refl)/(darkest+refl)) (eqn. 6), (-inf..0) */ \
	UBO_CVAR_DO(tm_enable, 1) /* switch for tone mapping, 0 or 1 */ \
	UBO_CVAR_DO(tm_exposure_bias, -1.0) /* exposure bias, log-2 scale */ \
	UBO_CVAR_DO(tm_exposure_speed_down, 1) /* speed of exponential eye adaptation when scene gets darker, 0 means instant */ \
	UBO_CVAR_DO(tm_exposure_speed_up, 2) /* speed of exponential eye adaptation when scene gets brighter, 0 means instant */ \
	UBO_CVAR_DO(tm_blend_scale_border, 1) /* scale factor for full screen blend intensity, at screen border */ \
	UBO_CVAR_DO(tm_blend_scale_center, 0) /* scale factor for full screen blend intensity, at screen center */ \
	UBO_CVAR_DO(tm_blend_scale_fade_exp, 4) /* exponent used to interpolate between "border" and "center" factors */ \
	UBO_CVAR_DO(tm_blend_distance_factor, 1.2) /* scale for the distance from the screen center when computing full screen blend intensity */ \
	UBO_CVAR_DO(tm_blend_max_alpha, 0.2) /* maximum opacity for full screen blend effects */ \
	UBO_CVAR_DO(tm_high_percentile, 90) /* high percentile for computing histogram average, (0..100] */ \
	UBO_CVAR_DO(tm_knee_start, 0.6) /* where to switch from a linear to a rational function ramp in the post-tonemapping process, (0..1)  */ \
	UBO_CVAR_DO(tm_low_percentile, 70) /* low percentile for computing histogram average, [0..100) */ \
	UBO_CVAR_DO(tm_max_luminance, 1.0) /* auto-exposure maximum luminance, (0..inf) */ \
	UBO_CVAR_DO(tm_min_luminance, 0.0002) /* auto-exposure minimum luminance, (0..inf) */ \
	UBO_CVAR_DO(tm_noise_blend, 0.5) /* Amount to blend noise values between autoexposed and flat image [0..1] */ \
	UBO_CVAR_DO(tm_noise_stops, -12) /* Absolute noise level in photographic stops, (-inf..inf) */ \
	UBO_CVAR_DO(tm_reinhard, 0.5) /* blend factor between adaptive curve tonemapper (0) and Reinhard curve (1) */ \
	UBO_CVAR_DO(tm_slope_blur_sigma, 12.0) /* sigma for Gaussian blur of tone curve slopes, (0..inf) */ \
	UBO_CVAR_DO(tm_white_point, 10.0) /* how bright colors can be before they become white, (0..inf) */ \
	UBO_CVAR_DO(tm_hdr_peak_nits, 800.0) /* Exposure value 0 is mapped to this display brightness (post tonemapping) */ \
	UBO_CVAR_DO(tm_hdr_saturation_scale, 100) /* HDR mode saturation adjustment, percentage [0..200], with 0% -> desaturated, 100% -> normal, 200% -> oversaturated */ \
	UBO_CVAR_DO(ui_hdr_nits, 300) /* HDR mode UI (stretch pic) brightness in nits */ \
	/* Appended LAST on purpose: a new cvar inserted mid-list shifts every cvar after it in */ \
	/* the shaders' view, and if the exe and .spv ever go out of sync (a failed link) that */ \
	/* silently corrupts the tm_* tone-mapping values. Appending keeps that blast radius 0. */ \
	UBO_CVAR_DO(pt_rr_white_noise, 0) /* hashed white noise instead of the tiled blue-noise texture, per DLSS-RR guide 3.5; 0 keeps the blue noise */ \
	UBO_CVAR_DO(pt_fog_light_scale, 1.0) /* brightness of local lights scattered in map fog (cl_fog 2) */ \
	UBO_CVAR_DO(pt_fog_sky_scale, 1.0) /* brightness of SKY light scattered in map fog (cl_fog 2) */ \
	UBO_CVAR_DO(pt_fog_light_knee, 2.0) /* soft roll-off on the SUMMED local-light total; SATURATES AT THIS VALUE, so it crushes the bright-core-to-dim-wash contrast that makes a light read as a glowing cloud. 0 disables it, and 0 is the right starting point for tuning localised glow. NOT a distance falloff - that is pt_fog_light_falloff */ \
	UBO_CVAR_DO(pt_fog_light_falloff, 4.0) /* distance exponent for a light's fog glow. 2 = plain inverse square; 4 is Matt's calibration - it pulls the glow in tight around each fixture instead of washing the whole room. Only ever steepens BEYOND pt_fog_light_pivot, so the near field keeps its correct brightness at any setting */ \
	UBO_CVAR_DO(pt_fog_light_pivot, 128.0) /* world radius inside which a light keeps plain inverse-square brightness; pt_fog_light_falloff only steepens the decay BEYOND this, so the near field can never brighten however high the exponent goes */ \
	UBO_CVAR_DO(pt_fog_light_radius, 0.0) /* hard cutoff in world units past which a light adds no fog glow at all, windowed so it eases to zero rather than clipping a visible edge; 0 = no cutoff */ \
	UBO_CVAR_DO(pt_fog_light_samples, 8) /* how many of a cluster's lights each march step samples. The direct trade of frame time against fog noise, and it matters MORE the steeper pt_fog_light_falloff is, because a steep falloff concentrates the energy in the few nearest lights and a strided subset either catches one or does not */ \
	UBO_CVAR_DO(pt_fog_light_shadow, 1) /* trace an occlusion ray per sampled light, so map fog is no longer lit THROUGH walls, floors and roofs. This is what makes a glow pool around a fixture instead of appearing above the roof it is under. Affordable with pt_fog_froxel 1 (one ray per cell); very expensive in the per-pixel march */ \
	UBO_CVAR_DO(pt_fog_sky_soften, 0.0) /* world units of overhead clearance over which sky lighting of the fog fades in. 0 = the original HARD BINARY up-ray, which paints full-strength blocky slabs of fog in open air above rooftops and is what the froxel jitter flickers across. Try 256-1024 */ \
	UBO_CVAR_DO(pt_sky_brightness, 1.0) /* brightness of the SKY AS SEEN; does NOT change how much light it casts */ \
	UBO_CVAR_DO(pt_fog_density_max, 64.0) /* ceiling on map-fog density in getDensity; was a hard-coded 4.0, which no map reaches at cl_fog_scale 1 */ \
	UBO_CVAR_DO(pt_fog_eccentricity, -1.0) /* map-fog scattering directionality, 0 = even in all directions .. 0.95 = tight shafts; -1 follows gr_eccentricity */ \
	UBO_CVAR_DO(pt_fog_ambient, 0.0) /* fraction of the sun's radiance that lights map fog IN SHADOW, isotropically; 0 = the hard shadow-map cliff this always had */ \
	UBO_CVAR_DO(pt_fog_extinction, 0.0) /* real extinction per unit density for map fog, so a long ray SATURATES instead of accumulating without bound; 0 = the legacy flat 0.0001 */ \
	UBO_CVAR_DO(pt_fog_opacity, 1.0) /* how much of the accumulated transmittance dims what is BEHIND the fog; only has any effect when pt_fog_extinction > 0 */ 	UBO_CVAR_DO(pt_fog_froxel, 0.0) /* 1 = evaluate map fog in the froxel grid (cheap, temporally reused); 0 = the old per-pixel march in god_rays.comp */ 	UBO_CVAR_DO(pt_fog_froxel_history, 0.95) /* how much of the PREVIOUS frame's froxel volume to keep; this is what removes the noise. 0 disables temporal reuse */ \
	/* What REFLECTION and REFRACTION pixels get for map fog. A negative PT_VIEW_DEPTH_A is reflect_refract's marker for such a pixel, and its magnitude is the UNFOLDED camera->mirror->object path length (PRIMARY_RAY_T_MAX for a reflected sky), so handing that straight to the view-aligned grid asks for a full-length column measured along the CAMERA ray - which is why reflections of a green sky came out orange on mgu5m1. 0 = grid, but stopped at the reflecting surface (smooth; no fog within the reflected image, which the grid cannot see at any price). 1 = the march's two-pass result (complete, but the march is the per-pixel estimator with no temporal history, so the fog inside reflections is NOISY). 2 = the old full-length column, kept only to reproduce the orange bug. See god_rays_filter.comp. */ \
	UBO_CVAR_DO(pt_fog_froxel_reflect, 0.0) \
	/* Frame rate at which pt_fog_froxel_history means exactly what it says. That weight is applied ONCE PER FRAME, so on its own its time constant is measured in frames - 0.95 averages over ~20 of them, a third of a second at 60fps and a full second at 20 - and the fog therefore converges at whatever rate the machine runs at. The scatter pass rescales it to w^(dt*this) so the TIME constant is what stays fixed. 0 disables the correction and restores the raw per-frame weight. */ \
	UBO_CVAR_DO(pt_fog_froxel_history_hz, 60.0) \
	/* SPATIAL RESERVOIR REUSE (ReSTIR) for the froxel grid, cl_fog 3 only. 1 = a cell may borrow which LIGHT its neighbours' candidate draws picked, then still traces its own visibility ray on the winner. It costs NO extra rays - the reservoir and spatial passes trace nothing - and unlike pt_fog_froxel_filter it does not blur, so it cuts noise without softening the sky shafts. Defaults OFF; the grid renders exactly as before at 0. */ \
	UBO_CVAR_DO(pt_fog_restir, 0.0) \
	/* How many neighbouring reservoirs each cell pulls in, 0..8. 0 makes the spatial pass a pass-through and is equivalent to pt_fog_restir 0 except for the memory. Cost is linear in this; quality saturates quickly because the taps are correlated through their shared light list. */ \
	UBO_CVAR_DO(pt_fog_restir_taps, 3.0) \
	/* Radius of the reuse neighbourhood in CELLS across the screen. Too large and the taps stop being about the same region of the map, which costs variance rather than saving it - the estimator stays unbiased either way. */ \
	UBO_CVAR_DO(pt_fog_restir_radius, 3.0) \
	/* Radius of the reuse neighbourhood along DEPTH, in slices, 0..8. Kept separate from the screen radius because the axes are not comparable: slices are exponentially spaced, so one z step is a fixed 6.3% of the distance and out in the distance is a far bigger world-space stride than several cells of x or y. */ \
	UBO_CVAR_DO(pt_fog_restir_radius_z, 1.0) \
	/* cl_fog 3 only. Its own brightness knob rather than sharing pt_fog_light_scale, because mode 3 has no knee compressing the total and so sits at a completely different level - one cvar could not calibrate both, and switching modes to compare would retune the other every time. */ \
	UBO_CVAR_DO(pt_fog_vol_scale, 1.0) \
	/* cl_fog 3 only. Ceiling on one light sample's luminance, which is what stops a froxel cell that landed very close to a small emitter from blowing out into a solid bright block. Biased on purpose; RTX Remix calls this froxelFireflyFilteringLuminanceThreshold. LOWER THIS FIRST if the fog looks blotchy. 0 disables it. */ \
	UBO_CVAR_DO(pt_fog_firefly, 20.0) \
	/* 3x3 blur across each froxel SLICE, applied in the integrate pass. Every cell is one light sample with one BINARY visibility ray, and that all-or-nothing term is the one thing RIS cannot importance-sample away - this is what smooths it. Blurring within a slice is sound because every cell of a given z sits at the same view depth. RTX Remix does the same thing in a pass of its own. 0 = off. */ \
	/* RADIUS of the spatial blur over the froxel scatter volume, applied in the
	   integrate pass. 0 = off, 1 = 3x3x3 (27 taps, the default and identical to
	   the 1-2-1 binomial this used to be), 2 = 5x5x5 (125), 3 = 7x7x7 (343),
	   clamped there.

	   THE ONE NOISE KNOB THAT COSTS NO RAYS. Each cell is a single light sample
	   with TWO BINARY visibility rays, and binary is precisely what RIS cannot
	   importance-sample away - so pt_fog_light_samples cannot reach this noise
	   and more neighbours can. A tap is an imageLoad, not a trace.

	   It trades DETAIL for smoothness - the shafts and the pooled glow under a
	   fixture soften as it goes up - so it is a taste knob, not a free win. */ \
	UBO_CVAR_DO(pt_fog_froxel_filter, 1.0) \
	/* Froxel debug views, cl_fog 3 with pt_fog_froxel 1. 1 = this frame raw in-scatter with no temporal blend and no spatial filter (the true noise). 2 = what the temporal reuse did - GREEN blended, RED reprojected into last frame but found nothing usable, BLUE not visible last frame. At a STATIONARY camera this should be solid green; flicker means the reuse is dropping out. 3 = the depth slice as a ramp. 4 = the SKY term alone, scaled exactly as the real path scales it - if this is black where sky is visible overhead the term is genuinely absent, if it is not black the sky IS in the volume and the problem is brightness or compositing. 10/11 = THE ROUND-TRIP SELF TEST: both throw the fog away and push a known 1.0/0.25 checkerboard through the volume instead, so they measure the PLUMBING rather than the medium - 10 shows the value fetched back a frame later as greyscale (must be a crisp checkerboard at a stationary camera), 11 is the verdict on |fetched - expected|/expected as GREEN under 1% / YELLOW under 10% / RED under 50% / MAGENTA worse or nothing returned. Read 11 first: GREEN means the history path is faithful and the fault is magnitude or precision, not plumbing. The test values sit in fp16's NORMAL range on purpose, which is what separates those two. 0 = off. */ \
	UBO_CVAR_DO(pt_fog_froxel_debug, 0.0) \
	/* ISOLATE ONE HALF OF THE FOG'S LIGHTING, fully composited. 0 = both,
	   1 = the SKY term only, 2 = the local lights only.

	   This is not a debug view and that is the point. Every pt_fog_froxel_debug
	   view stops the integrate pass, so what reaches the screen is the ONE cell
	   at that pixel's own surface depth - which for a wall 500 units away is the
	   cell AT the wall, in rock or in shadow, not the forty cells of open lit air
	   in front of it that actually make the fog. The composited fog is the SUM
	   along the ray, so every one of those views shows the least informative cell
	   in the column, and that is why they all read black over most of the screen.

	   This runs the WHOLE pipeline - density, history, spatial filter,
	   integration, the god_rays_filter lookup - with one of the two lighting
	   terms zeroed at source. So it composites at real scale, needs no exposure
	   argument, and it is the only way to watch the sky's own fog appear and
	   disappear as the camera moves.

	   It applies in fog_medium.glsl, so it is honoured by BOTH in-scatter
	   functions: cl_fog 2 and cl_fog 3, march and grid alike. That makes
	   `pt_fog_isolate 1` plus `pt_fog_froxel 0` vs `1` a direct A/B of the two
	   integrators on nothing but the sky term - the comparison the sky-fog bug
	   needs and the one no debug view could give. */ \
	UBO_CVAR_DO(pt_fog_isolate, 0.0) \
	/* Snap the froxel history lookup to the nearest texel CENTRE instead of
	   letting the trilinear sampler interpolate. 0 = interpolate (the old
	   behaviour), 1 = snap.

	   WHY THIS EXISTS. The history is already looked up at the CELL CENTRE
	   rather than at the jittered sample point, because a filtered fetch at a
	   jittered coordinate composed with the blend every frame is a DIFFUSION -
	   result = 0.05*raw + 0.95*blur(result) - which spreads a glow outwards and
	   decays its peak, and raising the history weight makes it worse. That is
	   the bug that made a bright light's volumetric dissolve over a second.

	   But the cell centre only round-trips EXACTLY while the camera is still.
	   The moment it moves, world_to_prev_froxel_uvw returns a coordinate that is
	   a fraction of a texel off centre, the trilinear fetch blends eight texels
	   again, and the diffusion is back - for exactly as long as the player keeps
	   moving. It hurts the SKY term far more than the light term because the sky
	   term has a HARD binary boundary with empty cells on the other side (rock,
	   or above the fog band), so most of what the blur pulls in is zero, while a
	   local light's glow is a smooth blob whose neighbours all carry signal.

	   Snapping makes the reprojection a permutation rather than a filter: no
	   mass spreads and none leaks into empty cells. It costs some temporal
	   aliasing while turning, which is the trade to judge. */ \
	UBO_CVAR_DO(pt_fog_froxel_history_snap, 0.0) \
	UBO_CVAR_DO(pt_fog_sky_sun_only, 1) /* under the PHYSICAL sky, let cl_fog 1's sun term carry the sky's contribution to the fog on its own, instead of cl_fog 3 also adding its ambient sky term. NOTE the sun term is occluded by the SHADOW MAP while mode 3's sky term uses a traced ray, so this trades the crisp clipping under overhangs for the sun's directionality - which is why it defaults OFF. Appended at the END of the cvar list on purpose; see the alignment note at the top of this file */ \
	UBO_CVAR_DO(pt_physical_sky_brightness, 1.0) /* how bright the PHYSICAL sky LOOKS, independent of the light it casts. The map-skybox equivalent is pt_sky_brightness, and the two are deliberately separate cvars: a value tuned to stop a map's skybox blowing out (Matt runs 0.005 for mgu5m1) has no business dimming a procedural atmosphere. 1 = show it at exactly the brightness it casts, which is the physically honest default */ \
	/* Appearance of the cl_blood_spheres droplets. Blood is a glossy DIELECTRIC, so metallic stays 0 and the wet look comes from a low roughness against a high specular factor - NOT from the chrome path, which would make it a mirror ball and cost a second traced ray at half resolution. */ \
	UBO_CVAR_DO(pt_blood_roughness, 0.06) /* surface roughness of a blood droplet. Below ~0.02 the highlight becomes a point the denoiser cannot hold on to; above ~0.2 the droplet stops reading as wet */ \
	UBO_CVAR_DO(pt_blood_specular, 1.6) /* specular factor of a blood droplet, above 1 on purpose - a droplet is small and its highlight is the whole effect */ \
	UBO_CVAR_DO(pt_blood_normal_strength, 0.35) /* how hard the animated ripple perturbs the sphere's normal. 0 = a perfectly smooth ball */ \
	UBO_CVAR_DO(pt_blood_normal_scale, 1.4) /* ripple features per droplet, in normal-map tiles across the sphere's direction cube. Small numbers = a few big wobbles, large = a fine seethe */ \
	UBO_CVAR_DO(pt_blood_normal_speed, 1.0) /* how fast the ripple animates */ 	UBO_CVAR_DO(pt_fog_step, 1.0) /* multiplies the fog march's step length. 1 = unchanged. THE COST KNOB - see getStep in fog_medium.glsl, and read it before touching cl_fog_scale */ \
	/* How dark a LANDED splat is drawn, and how much darker its feather edge. Airborne droplets are untouched: a bead in flight is a lit red sphere and reads correctly bright, while blood lying on a floor is a thin absorbing film over a surface and almost never reads as saturated red. The thickness the ramp runs on comes from the puddle's own shading normal against the surface plane it is lying on - see the tangent-slot note in blood.c's write_blood_geometry */ \
	UBO_CVAR_DO(pt_blood_splat_dark, 0.55) /* colour multiplier over the body of a landed splat. 1 = the old behaviour, a splat exactly as bright as the droplet that made it */ \
	UBO_CVAR_DO(pt_blood_thin_dark, 0.12) /* colour multiplier at the feather edge, where the film is thinnest. Well below pt_blood_splat_dark on purpose: a hard bright rim is most of what makes a puddle read as a decal */ \
	UBO_CVAR_DO(pt_blood_thin_power, 2.0) /* how far the dark edge reaches inward. The dome's normals are steepened by cl_blood_flatten, so the raw thickness saturates quickly and a power above 1 is what gives the edge a visible width. Higher = a wider dark band */ \
	UBO_CVAR_DO(pt_blood_splat_alpha, 0.95) /* opacity of a LANDED splat. DEFAULT 0.95 - just off solid, so the floor reads through the blood a little. 1 = fully solid, the behaviour before this existed. Below 1 the surface under the splat is traced through it, so the floor's own texture and lighting show through the blood. Airborne droplets are always solid - a bead in flight is a body of liquid, and making it translucent would mean tracing through every droplet of a spray. Read by blood.c as well as by the shader: it is written into each landed primitive's alpha, and it also drops blood out of the SHADOW ray mask, without which a splat shadows the very floor you are looking through it at */ \
	UBO_CVAR_DO(pt_fog_const_src, 0.0) /* TEMPORARY BISECTION: 1 replaces the fog's LIGHTING with a constant in froxel_scatter.comp, keeping albedo and density. Asks whether the fade survives a source term that cannot vary - if it does, the fault is downstream of the scatter computation. APPENDED AT THE END on purpose: a cvar inserted mid-list shifts every cvar after it in the shaders' view whenever the .spv and the exe disagree. See the alignment note at the top of this file. */ \
	UBO_CVAR_DO(pt_fog_printf, 0.0) /* 1 = emit debugPrintfEXT from one froxel cell. Needs vk_validation 1 AND vk_shader_printf 1; output arrives through vk_debug_callback at INFO severity and lands in console.log. Separate from pt_fog_const_src because any non-zero value of THAT switches the lighting to a constant. APPENDED AT THE END - see the alignment note at the top. */ \
	UBO_CVAR_DO(pt_fog_history_clamp, 0.0) /* HISTORY VALIDATION - the floor under the temporal blend, and the one thing RTX Remix's volumetrics has that this does not. The blend h <- (1-w)*raw + w*h has exactly one fixed point, h == raw, ONLY if the history round trip is lossless. It is not: with a survival fraction k per trip the volume settles at h = (1-w)*raw / (1 - w*k), which at w = 0.95 and the measured k of ~0.9 is 35% of the true value - and the deficit is amplified by 1/(1-w), so a 10% leak becomes a 65% loss. Nothing in the current shader can detect that state, let alone leave it: every frame reads a value that is already too dark and blends 95% of it forward. Remix does not rely on the fixed point. It carries a per-froxel accumulation AGE (an R8_UNORM volume beside the radiance, see rtx_global_volumetrics.cpp), weights by that age rather than by a constant, caps it at maxAccumulationFrames, and resets it wherever reprojection fails - so a froxel whose history has drifted is thrown away rather than averaged forward forever. This is the cheap half of that idea, and it is a MEASUREMENT before it is a fix: a one-sided floor saying the history may not sit more than this factor below the value this frame computed. Set it to 8 and the volume physically cannot hold less than an eighth of the raw estimate. THE FOG STOPS FADING -> the fade IS the history sitting below raw, and the age-and-reset machinery is the real fix. IT STILL FADES -> `raw` itself is dying and the temporal path is innocent, which retires the whole blend as a suspect in one run. ONE-SIDED, and that matters: max(), never a symmetric clamp. A cell whose light sample legitimately missed this frame has raw == 0, the floor is then 0, and the averaging that removes the noise is untouched. A symmetric clamp would pin that cell to zero and turn the grid back into the per-frame static the blend exists to remove. Applied INSIDE the accept guard, after the empty/non-finite test, so an unwritten history is still refused outright rather than lifted to raw/ratio and then blended - which would darken a newly visible cell, the exact failure that guard was written for. 0 disables it and restores the unbounded blend. APPENDED AT THE END - see the alignment note at the top. */ \
	UBO_CVAR_DO(pt_fog_history_hold, 0.0) /* HOLD, DO NOT DECAY, A CELL THAT WAS NOT MEASURED THIS FRAME. The bug this tests for is a one-line structural mistake in froxel_scatter.comp that every piece of algebra in BASELINE.md stepped straight past. `result` is initialised to vec4(0) at the top of the z-loop and written ONLY inside `if (cell_has_density)`. The temporal blend runs OUTSIDE that gate. So on any frame the gate is shut for a cell, the blend evaluates mix(vec4(0), history, 0.95) = 0.95 * history - not an average, a GEOMETRIC DECAY. 0.95^n is 0.6% after 100 frames, half a second at 216 fps, and the cell then sits at zero: a fade with a clean onset that sticks, and that recovers only when something weights the raw term back up (a camera rotation failing the reprojection test, a frame hitch pulling w down through the _hz correction). The fixed-point argument that h == raw is the only equilibrium does not apply, because on those frames raw is not a measurement - nothing was measured, and the shader nonetheless asserts "there is no fog in this cell" at 5% per frame on the strength of a branch it did not take. It is also why pt_fog_history_clamp read the same at every value from 0 to 8: its floor is raw/ratio, the gate being shut makes raw 0, so the floor is 0 and the max() is a no-op. That test was blind to exactly the mechanism it most needed to see. Remix cannot fail this way: it accumulates by a per-froxel AGE rather than a constant weight, so a froxel with no new sample simply contributes no new sample and its stored radiance is left alone. Nothing there multiplies a froxel history by a constant below 1 in the absence of a measurement. 1 sets w to 1.0 for gate-shut cells only, so result = history exactly and the cell holds; the instant the gate reopens w returns to normal and the cell converges as usual, so nothing is permanently frozen. THE COST, stated plainly: a cell whose density genuinely dropped to zero - the camera moved and it is now inside rock - keeps its last value instead of clearing, so stale fog can linger where it should drain. That is why this is a cvar and not the default; the real fix is the age channel, which can tell "no measurement yet" from "no fog here" and expire a held cell. 0 = the old behaviour. APPENDED AT THE END - see the alignment note at the top. */ \
	/* The underwater screen warp - see water_warp.glsl. APPENDED AT THE END, like everything below pt_fog_const_src: a cvar inserted mid-list shifts every cvar after it in the shaders view of the UBO, so a stale .spv reads the wrong ones. */ \
	UBO_CVAR_DO(pt_water_warp, 1) /* master switch for the underwater warp, 0 or 1 */ \
	UBO_CVAR_DO(pt_water_warp_amp, 0.010) /* ripple amplitude as a fraction of SCREEN HEIGHT, so it is square on any aspect ratio. Quakes software warp is AMP2 = 3 pixels on a 320x200 screen, i.e. 0.015; this is a little gentler. [0..inf) */ \
	UBO_CVAR_DO(pt_water_warp_freq, 1.6) /* how many full ripples fit across the screen. D_WarpScreen walks a 128-entry sine table down 200 rows, which is this. [0..inf) */ \
	UBO_CVAR_DO(pt_water_warp_speed, 0.2) /* ripple travel in cycles per second. The original advances SPEED = 20 table entries per second through those 128, i.e. 0.156 - a slow lazy roll, not a shimmer. */ \
	UBO_CVAR_DO(pt_water_warp_fade, 6.0) /* how fast the warp eases in and out as you break the surface, in 1/seconds. Snapping it on would move every pixel sample position in one frame and discard the whole screen history; 0 snaps anyway. */ \
	UBO_CVAR_DO(pt_water_warp_zoom, 0) /* amplitude of the slow breathing zoom - the FOV-wobble half of the original effect - as a fraction of the screen. Off by default. */ \
	UBO_CVAR_DO(pt_water_warp_zoom_speed, 0.2) /* breathing zoom rate, in Hz */ \
	UBO_CVAR_DO(pt_fog_const_density, 0.25) /* The constant density pt_fog_const_src 17 uses. A cvar rather than a literal so the MAGNITUDE can be swept without a rebuild, which matters because stage 17 is a saturation control and is worthless unless its brightness matches the real fog. Procedure: run an arm through fogtmp/auto, read the FIRST shot mean luma out of the score, and compare against a healthy control frame (0.130 on xswamp). A fully collapsed frame sits at 0.070 with no fog at all, so the fog contributes (luma - 0.070). The first attempt hard-coded 0.25 from exactly that arithmetic and landed at 0.263 instead of 0.130 - the model was 2x out, because density drives the extinction the integrate pass accumulates as well as the in-scatter. Sweep it, do not predict it. APPENDED AT THE END - see the alignment note at the top. */ \ \
	UBO_CVAR_DO(pt_fog_hoist_sky, 0.0) /* HOIST TEST for the control-flow fault. 1 = ALSO call fog_sky_inscatter from main's z-loop, OUTSIDE the density gate and outside the fog_lights / fog_mode nest, with its result discarded. This exists to settle whether the same function runs when it is called unconditionally and fails when it is called inside that nest. Debug view 39 reads the two separately: green is "the hoisted call ran", blue is "the nested call ran". Green at 100% with blue at ~5% is proof that the branch structure is the fault rather than anything in the fog, and reduces the repro to four lines. Costs a second sky evaluation per cell while on, so it is a diagnostic and not a setting. APPENDED AT THE END - see the alignment note at the top. */ \
	/* SCREEN BLENDING WHILE SUBMERGED. tm_blend_enable switches the full-screen blend on and off globally, and tm_blend_scale_center/border ramp it from the middle of the screen out to the edges. All of it applies to EVERY blend the game sends - the damage flash, a powerup and the liquid tint alike - and there is no way to treat the liquid tint differently, which is what these two add. The tint is on CONTINUOUSLY while you swim, so the border ramp stops reading as a flash at the edge of vision and becomes a permanent dark ring. NOTE that the game sums the liquid tint and any damage flash into ONE blend colour before the renderer ever sees it, so a hit taken underwater is treated as underwater. APPENDED AT THE END - see the alignment note at the top. */ \
	UBO_CVAR_DO(tm_blend_water_enable, 1) /* tm_blend_enable, but scoped to when the eye is in water, slime or lava. 0 drops the full-screen blend while submerged and leaves it alone everywhere else. */ \
	UBO_CVAR_DO(tm_blend_water_vignette, 0) /* how much of the centre-to-border ramp survives while submerged, when the blend is on. 0 = flat, the tint fills the screen evenly; 1 = the same vignette everything else gets, which is what this renderer did before the cvar existed. */ \
	UBO_CVAR_DO(pt_fog_underwater, 0.0) /* WHAT THE MAP'S FOG DOES WHILE THE EYE IS IN WATER, SLIME OR LAVA. DEFAULT 0 - the map's fog medium is switched off and the CLASSIC SUN-ONLY GOD RAYS take over, which is what the base game shows underwater. 1 keeps the map's fog down there, which is the behaviour from before this cvar existed. THE FOG AND THE SHAFTS ARE NOT THE SAME THING, and that is the whole point of this control. Water is ALREADY a participating medium here, with its own per-channel extinction (get_extinction_factors in water.glsl, applied to the primary ray in primary_rays.rgen), so a froxel grid that ALSO fills the submerged frustum with the map's height fog is a SECOND medium stacked on the same path: attenuated twice, and the in-scatter added back was computed for air - the map's fog colour, and cluster lights on the other side of the surface. The SUN term is not part of that complaint. A shaft of sunlight in water is real, the base game draws one, and killing the medium outright took it away along with the haze - which is why this is a mode and not an off switch. HOW 0 WORKS: prepare_ubo clears fog_enable while submerged, so a map WITH a fog definition behaves down there exactly like a map WITHOUT one. That single flag already means all four of the right things and is a path every classic map takes every frame: getDensity returns the bare flat world_box medium that god_rays_intensity is calibrated against and getFogColor returns white (fog_medium.glsl); god_rays.comp drops its local cluster lights (fog_lights) and its fog extinction (fog_medium), leaving the shadow-map sun term alone; and god_rays_filter.comp stops letting the froxel grid replace that march and forces transmittance to 1, so the shafts composite purely ADDITIVELY over a picture the water has already attenuated. No shader needed a line for this. It is a SWITCH, not a ramp - there is no cross-fade between the grid's fog and the march's shafts because they are two different estimators and the filter takes one or the other. Crossing a water surface is already a discontinuity (the liquid tint arrives in one frame from the server) and the base game pops here too. */

/* FIELD LAYOUT of the path-tracer screen images (pt_fullres_fields / pt_field_offset).
 *
 * Q2RTX traces two "fields" that are packed side by side into the screen images,
 * field 1 starting at x == pt_field_offset.
 *
 * pt_fullres_fields == 0 (classic checkerboard): each field is width/2 wide and holds
 *   one checkerboard half of the screen. Reflection and refraction are split between
 *   the two fields, so a glass pixel gets either one or the other, and the two are
 *   interleaved and cross-blurred afterwards.
 *
 * pt_fullres_fields == 1 (full-resolution split): each field is a full width x height
  *   layer of the whole screen. Field 0 is the reflection layer, field 1 the refraction
 *   layer, and they are summed back together after lighting. Nothing is checkerboarded
 *   and every pixel gets both paths. This is what DLSS wants - checkerboard rendering
 *   is explicitly listed as a practice to avoid in the DLSS-RR integration guide (3.5).
   *   Field 1 is only lit where the two paths actually diverge, so the extra cost is
   *   proportional to how much glass and water is on screen, not to the frame.
   *
   * pt_fullres_fields == 2 (half-resolution split): as above, but the two layers are
   *   traced on alternating rows and the combine pass fills the untraced rows from
   *   their neighbour. Same ray count as the classic checkerboard without the
   *   interleave. Selected by pt_dlss_field_res 2.
 *
 * Note: comments inside the list below must stay on one line and precede the trailing
 * backslash, otherwise they terminate the macro. */
#define GLOBAL_UBO_VAR_LIST \
	GLOBAL_UBO_VAR_LIST_DO(int,             current_frame_idx) \
	GLOBAL_UBO_VAR_LIST_DO(int,             width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             height)\
	GLOBAL_UBO_VAR_LIST_DO(int,             current_gpu_slice_width) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             medium) \
	GLOBAL_UBO_VAR_LIST_DO(float,           time) \
	GLOBAL_UBO_VAR_LIST_DO(int,             first_person_model) \
	GLOBAL_UBO_VAR_LIST_DO(int,             environment_type) \
	\
	GLOBAL_UBO_VAR_LIST_DO(vec3,            sun_direction) \
	GLOBAL_UBO_VAR_LIST_DO(float,           bloom_intensity) \
	GLOBAL_UBO_VAR_LIST_DO(vec3,            sun_tangent) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sun_tan_half_angle) \
	GLOBAL_UBO_VAR_LIST_DO(vec3,            sun_bitangent) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sun_bounce_scale) \
	GLOBAL_UBO_VAR_LIST_DO(vec3,            sun_color) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sun_cos_half_angle) \
	GLOBAL_UBO_VAR_LIST_DO(vec3,            sun_direction_envmap) \
	GLOBAL_UBO_VAR_LIST_DO(int,             sun_visible) \
	\
	GLOBAL_UBO_VAR_LIST_DO(float,           sky_transmittance) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sky_phase_g) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sky_amb_phase_g) \
	GLOBAL_UBO_VAR_LIST_DO(float,           sun_solid_angle) \
	\
	GLOBAL_UBO_VAR_LIST_DO(vec3,            physical_sky_ground_radiance) \
	GLOBAL_UBO_VAR_LIST_DO(int,             physical_sky_flags) \
	\
	GLOBAL_UBO_VAR_LIST_DO(float,           sky_scattering) \
	GLOBAL_UBO_VAR_LIST_DO(float,           temporal_blend_factor) \
	GLOBAL_UBO_VAR_LIST_DO(int,             planet_albedo_map) \
	GLOBAL_UBO_VAR_LIST_DO(int,             planet_normal_map) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             padding1) \
	GLOBAL_UBO_VAR_LIST_DO(int ,            num_static_lights) \
	GLOBAL_UBO_VAR_LIST_DO(uint,            num_static_primitives) \
	GLOBAL_UBO_VAR_LIST_DO(int,             cluster_debug_index) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             water_normal_texture) \
	GLOBAL_UBO_VAR_LIST_DO(float,           pt_env_scale) \
	GLOBAL_UBO_VAR_LIST_DO(float,           cylindrical_hfov) \
	GLOBAL_UBO_VAR_LIST_DO(float,           cylindrical_hfov_prev) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_swap_checkerboard) \
	GLOBAL_UBO_VAR_LIST_DO(float,           shadow_map_depth_scale) \
	GLOBAL_UBO_VAR_LIST_DO(float,           god_rays_intensity) \
	GLOBAL_UBO_VAR_LIST_DO(float,           god_rays_eccentricity) \
	\
	/* rerelease per-map fog from worldspawn - see src/client/mapfog.c. */ \
	/* KEEP IN GROUPS OF EXACTLY FOUR SCALARS - see the note at the top. */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_enable) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_density) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_density) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_falloff) \
	\
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_start_z) /* world Z, TOP of the band    */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_end_z)   /* world Z, BOTTOM of the band */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_color_r) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_color_g) \
	\
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_color_b) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_start_r) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_start_g) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_start_b) \
	\
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_end_r) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_end_g) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_hf_end_b) \
	/* model lights sit contiguously after the static ones in light_buffer, */ \
	/* so the fog march reads [num_static_lights, +fog_num_model_lights).   */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_num_model_lights) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_mode) /* 0 off, 1 sun-only, 2 local lights */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_camera_cluster) /* -1 if outside the world */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_sky_fade) /* 0..1 sky exposure, eased; see main.c */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_sky_trace) /* 1 = trace real sky visibility */ \
	\
	/* The MAP's skybox radiance. sun_color_ubo.sky_color is written only by  */ \
	/* physical_sky.comp, so it is ZERO on every map using its own skybox -   */ \
	/* which is all of them here. This comes from avg_envmap_color instead.   */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_sky_r) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_sky_g) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_sky_b) \
	/* Seconds of the frame just rendered, for the froxel grid's temporal blend. */ \
	/* pt_fog_froxel_history is a PER-FRAME weight, so without this its time    */ \
	/* constant is measured in frames and the fog converges at whatever rate    */ \
	/* the machine happens to be running at - see froxel_scatter.comp.          */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_frame_time) \
	\
	/* cl_volumetric_fog_density / cl_fog_scale - how much denser the LOCAL   */ \
	/* LIGHT half of the fog is than the sky/sun half. The densities above    */ \
	/* already carry cl_fog_scale and drive the sun term in every mode; this  */ \
	/* multiplies the local term only, so cl_fog 3 gets its own density while */ \
	/* the sky fog stays on cl_fog_scale. 1 = the two match.                  */ \
	/* Padded to a full four scalars - see the alignment note at the top.     */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_vol_density_ratio) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_pad0) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_pad1) \
	GLOBAL_UBO_VAR_LIST_DO(float,           fog_pad2) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_fullres_fields) /* see FIELD LAYOUT note above */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_field_offset) /* x offset / width of one field */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_prev_field_offset) /* previous frame's value */ \
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_denoiser_present) /* A-SVGF *or* DLSS-RR is denoising */ \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             num_cameras) \
	GLOBAL_UBO_VAR_LIST_DO(int,             screen_image_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             screen_image_height) \
	GLOBAL_UBO_VAR_LIST_DO(int,             prev_gpu_slice_width) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             prev_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             prev_height)\
	GLOBAL_UBO_VAR_LIST_DO(float,           inv_width) \
	GLOBAL_UBO_VAR_LIST_DO(float,           inv_height) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             unscaled_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             unscaled_height) \
	GLOBAL_UBO_VAR_LIST_DO(int,             taa_image_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             taa_image_height) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             taa_output_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             taa_output_height) \
	GLOBAL_UBO_VAR_LIST_DO(int,             prev_taa_output_width) \
	GLOBAL_UBO_VAR_LIST_DO(int,             prev_taa_output_height) \
	\
	GLOBAL_UBO_VAR_LIST_DO(uvec4,           easu_const0) \
	GLOBAL_UBO_VAR_LIST_DO(uvec4,           easu_const1) \
	GLOBAL_UBO_VAR_LIST_DO(uvec4,           easu_const2) \
	GLOBAL_UBO_VAR_LIST_DO(uvec4,           easu_const3) \
	GLOBAL_UBO_VAR_LIST_DO(uvec4,           rcas_const0) \
	\
	GLOBAL_UBO_VAR_LIST_DO(vec2,            sub_pixel_jitter) \
	GLOBAL_UBO_VAR_LIST_DO(float,           prev_adapted_luminance) \
	GLOBAL_UBO_VAR_LIST_DO(float,           tonemap_hdr_clamp_strength) \
	GLOBAL_UBO_VAR_LIST_DO(vec4,            fs_blend_color) \
	\
	GLOBAL_UBO_VAR_LIST_DO(vec4,            world_center) \
	GLOBAL_UBO_VAR_LIST_DO(vec4,            world_size) \
	GLOBAL_UBO_VAR_LIST_DO(vec4,            world_half_size_inv) \
	\
	GLOBAL_UBO_VAR_LIST_DO(vec4,            cam_pos) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            V) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            invV) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            V_prev) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            P) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            invP) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            P_prev) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            invP_prev) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            environment_rotation_matrix) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            shadow_map_VP) \
	GLOBAL_UBO_VAR_LIST_DO(mat4,            security_camera_data[MAX_CAMERAS]) \
	GLOBAL_UBO_VAR_LIST_DO(ShaderFogVolume, fog_volumes[MAX_FOG_VOLUMES]) \
	\
	/* Underwater warp strength, 0..1, eased by R_RenderFrame_RTX. This, not   */ \
	/* `medium`, is what the shaders test: the fade has to be a continuous     */ \
	/* quantity, or breaking the surface would move every sample position on   */ \
	/* screen within a single frame. The _prev copy, with time_prev, is what   */ \
	/* lets primary_rays.rgen build a correct motion vector ACROSS the fade     */ \
	/* rather than only once it has settled.                                   */ \
	GLOBAL_UBO_VAR_LIST_DO(float,           water_warp) \
	GLOBAL_UBO_VAR_LIST_DO(float,           water_warp_prev) \
	GLOBAL_UBO_VAR_LIST_DO(float,           time_prev) \
	GLOBAL_UBO_VAR_LIST_DO(int,             weapon_left_handed) \
	\
	/* CAMERA PROJECTION - see PROJECTION_* in constants.h and projection.glsl.  */ \
	/* projection_fov_scale is the half-extent, in that projection's own plane   */ \
	/* coordinates, that the screen edge maps to; prepare_ubo computes it on the */ \
	/* CPU because it is one transcendental per frame rather than one per pixel. */ \
	/* The _prev copy is what lets a reprojection run through LAST frame's warp, */ \
	/* so a FOV change does not smear the whole picture for a frame.             */ \
	/* Two vec2 fill exactly one 16-byte std140 slot, and pt_projection then     */ \
	/* gets a padded group of its own - see the alignment note at the top.       */ \
	/* PLACED LAST, immediately before the cvar list, so nothing above moves.    */ \
	GLOBAL_UBO_VAR_LIST_DO(vec2,            projection_fov_scale) \
	GLOBAL_UBO_VAR_LIST_DO(vec2,            projection_fov_scale_prev) \
	\
	GLOBAL_UBO_VAR_LIST_DO(int,             pt_projection) \
	GLOBAL_UBO_VAR_LIST_DO(int,             projection_pad0) \
	GLOBAL_UBO_VAR_LIST_DO(int,             projection_pad1) \
	GLOBAL_UBO_VAR_LIST_DO(int,             projection_pad2) \
	\
	UBO_CVAR_LIST // WARNING: Do not put any other members into global_ubo after this: the CVAR list is not vec4-aligned

BEGIN_SHADER_STRUCT( ModelInstance )
{
	mat4 transform;
	mat4 transform_prev;

	uint material;
	int cluster;
	uint source_buffer_idx;
	uint prim_count;

	uint prim_offset_curr_pose_curr_frame;
	uint prim_offset_prev_pose_curr_frame;
	uint prim_offset_curr_pose_prev_frame;
	uint prim_offset_prev_pose_prev_frame;
	
	float pose_lerp_curr_frame;
	float pose_lerp_prev_frame;
	int iqm_matrix_offset_curr_frame;
	int iqm_matrix_offset_prev_frame;

	int frame;
	float alpha;
	uint render_buffer_idx;
	uint render_prim_offset;
}
END_SHADER_STRUCT( ModelInstance )

BEGIN_SHADER_STRUCT( ShaderFogVolume )
{
	vec3 mins;
	uint is_active;
	vec3 maxs;
	float pad2;
	vec3 color;
	float pad3;
	vec4 density;
}
END_SHADER_STRUCT( ShaderFogVolume )

BEGIN_SHADER_STRUCT( InstanceBuffer )
{
	uint            animated_model_indices   [MAX_MODEL_INSTANCES];
	uint            model_current_to_prev    [MAX_MODEL_INSTANCES];
	uint            model_prev_to_current    [MAX_MODEL_INSTANCES];
	ModelInstance   model_instances          [MAX_MODEL_INSTANCES];
	uint            mlight_prev_to_current   [MAX_MODEL_LIGHTS];
	uint            tlas_instance_prim_offsets[MAX_TLAS_INSTANCES];
	int             tlas_instance_model_indices[MAX_TLAS_INSTANCES];
}
END_SHADER_STRUCT( InstanceBuffer )


#ifndef VKPT_SHADER

typedef struct QVKUniformBuffer_s {
#define GLOBAL_UBO_VAR_LIST_DO(type, name) type name;
	GLOBAL_UBO_VAR_LIST
#undef  GLOBAL_UBO_VAR_LIST_DO
} QVKUniformBuffer_t;

#else

struct GlobalUniformBuffer {
#define GLOBAL_UBO_VAR_LIST_DO(type, name) type name;
	GLOBAL_UBO_VAR_LIST
#undef  GLOBAL_UBO_VAR_LIST_DO
};

layout(set = GLOBAL_UBO_DESC_SET_IDX, binding = GLOBAL_UBO_BINDING_IDX, std140) uniform UBO {
	GlobalUniformBuffer global_ubo;
};

layout(set = GLOBAL_UBO_DESC_SET_IDX, binding = GLOBAL_INSTANCE_BUFFER_BINDING_IDX) readonly buffer InstanceSSBO {
	InstanceBuffer instance_buffer;
};

/*
=================
fog_sun_is_the_sky

Whether the SKY's contribution to the map fog is left to cl_fog 1's sun term
instead of cl_fog 3's own ambient sky term. PHYSICAL SKY ONLY - a map drawing its
own skybox has no sun, so there would be nothing to leave it to, and cl_fog 3
there is unchanged.

WHAT THIS DOES, and what it deliberately does NOT do.

The froxel grid and the march BOTH already compute the sun term - shadow map,
Henyey-Greenstein phase about the sun direction, god_rays_intensity * 0.0001.
That term IS cl_fog 1's fog. So "use cl_fog 1's sky fog" needs nothing added and
nothing composited: the sun is already there in both estimators.

All this does is stop cl_fog 3 ALSO adding its ambient, isotropic, unshadowed sky
term on top of it - `fog_sky_inscatter` in fog_medium.glsl returns 0. One site.

An earlier version also cleared fog_lights in god_rays.comp and made
god_rays_filter.comp ADD the march to the grid rather than replacing it. That
DOUBLE-COUNTED THE SUN and is reverted; see the note at the assignment in
god_rays_filter.comp before trying it again.
=================
*/
bool fog_sun_is_the_sky()
{
	return global_ubo.pt_fog_sky_sun_only != 0
	    && global_ubo.environment_type == ENVIRONMENT_DYNAMIC;
}

#endif

#undef UBO_CVAR_DO

#endif  /*_GLOBAL_UBO_H_*/


