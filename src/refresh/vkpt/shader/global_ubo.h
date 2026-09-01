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
	UBO_CVAR_DO(pt_dlss_mv_bounce_guard, 1) /* only build the "apparent position" reflection/refraction motion vector while the path is still first-order, and reject a projection that blows up; 0 restores the old unguarded behaviour that put Inf into PT_MOTION at pt_reflect_refract > 1 */ 	UBO_CVAR_DO(pt_dlss_guide_field, 3) /* split-field glass/water: which layer the DLSS guides describe. 0 refraction (base surface), 1 whichever layer is brighter (what the classic checkerboard interleave does), 2 reflection, 3 reflection on GLASS only - water and slime keep the refraction */ \
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
	UBO_CVAR_DO(pt_fog_opacity, 1.0) /* how much of the accumulated transmittance dims what is BEHIND the fog; only has any effect when pt_fog_extinction > 0 */ 	UBO_CVAR_DO(pt_fog_froxel, 0.0) /* 1 = evaluate map fog in the froxel grid (cheap, temporally reused); 0 = the old per-pixel march in god_rays.comp */ 	UBO_CVAR_DO(pt_fog_froxel_history, 0.9) /* how much of the PREVIOUS frame's froxel volume to keep; this is what removes the noise. 0 disables temporal reuse */ \

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
	GLOBAL_UBO_VAR_LIST_DO(int,             fog_pad3) \
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
	GLOBAL_UBO_VAR_LIST_DO(int,             weapon_left_handed) \
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

#endif

#undef UBO_CVAR_DO

#endif  /*_GLOBAL_UBO_H_*/


