/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2018 Tobias Zirr
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2022 Jorge Gustavo Martinez

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
// This rgen shader computes direct lighting for the first opaque surface. 
// The parameters of that surface are loaded from the G-buffer, stored there
// previously by the `primary_rays.rgen` and `reflect_refract.rgen` shaders.
//
// See `path_tracer.h` for an overview of the path tracer.
// ========================================================================== //

#ifndef  _RESTIR_H_
#define  _RESTIR_H_

#include "path_tracer_rgen.h"

#define RESTIR_INVALID_ID       0xFFFF
#define RESTIR_ENV_ID           0xFFFE

#define RESTIR_SPACIAL_DISTANCE 32
#define RESTIR_SPACIAL_SAMPLES  8

// A fresh reservoir must scale its chosen sample by roughly list_size/RESTIR_SAMPLING_M
// to stay unbiased, because that is how many candidates it did not look at. Measured
// cold-start W on mgu5m1 was ~32 with this at 4, implying list_size ~128 - and that
// up-weighting is exactly the bright flash seen on newly disoccluded geometry, which
// then relaxes as reuse accumulates real samples. Raising this lowers the flash
// proportionally and costs only target-function evaluations (projected area math), no
// extra rays.
#define RESTIR_SAMPLING_M       16
#define RESTIR_M_CLAMP          32
#define RESTIR_M_VC_CLAMP       16

struct Reservoir
{
	uint y;
	uint M;
	float w_sum;
	float W;
	float p_hat;
	vec2 y_pos;
	vec3 normal;
};

void
init_reservoir(inout Reservoir r)
{
	r.y = RESTIR_INVALID_ID;
	r.M = 0;
	r.w_sum = 0.0;
	r.W = 0.0;
	r.p_hat = 0.0;
	r.y_pos = vec2(0.0);
}

bool
update_reservoir(uint xi, float wi, vec2 xi_pos, float p_hat, inout float rng, inout Reservoir r)
{
	r.w_sum += wi;
	r.M++;
	float p_s = (wi / r.w_sum);
	if (rng < p_s)
	{
		r.y = xi;
		r.y_pos = xi_pos;
		r.p_hat = p_hat;
		rng /= p_s;
		return true;
	}
	else
	{
		rng = (rng - p_s) / (1.0f - p_s);
		return false;
	}
}

uvec4
pack_reservoir(Reservoir r)
{
	uvec4 vec;
	r.W = r.y == RESTIR_INVALID_ID ? 0.0 : r.W;
	vec.x = packHalf2x16(vec2(r.W, r.w_sum));
	vec.y = packHalf2x16(r.y_pos);
	return vec;
}

void
unpack_reservoir(uvec4 packed, uint light_idx, out Reservoir r)
{
	r.y = light_idx;
	// A reused reservoir carries no real sample count - it CLAIMS this one, and that
	// claim is its entire temporal weight: it is streamed against this frame's fresh
	// candidates with weight M, so at 32 the history outweighs the current frame 32:1 and
	// the lighting lags by roughly that many frames. Combined with permutation sampling
	// moving the fetch a few pixels every frame, 32 frames of history also diffuses the
	// light spatially - which is what "streaking along the direction of motion" is.
	// Now pt_restir_m_clamp so it can be traded against the noise a shorter history buys;
	// RESTIR_M_CLAMP remains the default and the documented original value.
	uint m_clamp = uint(max(1.0, global_ubo.pt_restir_m_clamp));
	r.M = light_idx == RESTIR_INVALID_ID ? 0 : (global_ubo.pt_restir != 3 ? m_clamp : uint(RESTIR_M_VC_CLAMP));
	vec2 val = unpackHalf2x16(packed.x);
	r.W = val.x;
	if (isnan(r.W) || isinf(r.W) || r.y == RESTIR_INVALID_ID) r.W = 0.0;
	r.w_sum = val.y;
	r.y_pos = unpackHalf2x16(packed.y);
	r.p_hat = 0.0;
}

// Functions

uint
get_light_current_idx(uint index)
{
	if (index < global_ubo.num_static_lights || index == RESTIR_INVALID_ID || index == RESTIR_ENV_ID)
	{
		return index;
	}
	else
	{
		uint light_id_curr = instance_buffer.mlight_prev_to_current[index - global_ubo.num_static_lights];
		if (light_id_curr != ~0u) return light_id_curr + global_ubo.num_static_lights;
		else
		{
			return RESTIR_INVALID_ID;
		}
	}
}


float
get_unshadowed_path_contrib(
	uint light_idx,
	vec3 position,
	vec3 normal,
	vec3 view_direction,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	vec2 rng)
{
	if (light_idx == RESTIR_ENV_ID) return get_unshadowed_env_path_contrib(normal, view_direction, phong_exp, phong_scale, phong_weight, rng);
	LightPolygon light = get_light_polygon(light_idx);

	float m = 0.0f;
	switch (uint(light.type))
	{
	case DYNLIGHT_POLYGON:
		m = projected_tri_area(light.positions, position, normal, view_direction, phong_exp, phong_scale, phong_weight);
		break;
	case DYNLIGHT_SPHERE:
		m = projected_sphere_area(light.positions, position, normal, view_direction, phong_exp, phong_scale, phong_weight);
		break;
	case DYNLIGHT_SPOT:
		m = projected_spotlight_area(light.positions, light.spot_emission_profile, position, normal, view_direction, phong_exp, phong_scale, phong_weight);
		break;
	}

	float light_lum = luminance(light.color);

	// Apply light style scaling.
	light_lum *= light.light_style_scale;

	if (light_lum < 0 && global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
	{
		// Set limits on sky luminance to avoid oversampling the sky in shadowed areas, or undersampling at dusk and dawn.
		// Note: the log -> linear conversion of the cvars happens on the CPU, in main.c
		m *= clamp(sun_color_ubo.sky_luminance, global_ubo.pt_min_log_sky_luminance, global_ubo.pt_max_log_sky_luminance);
	}
	else
		m *= abs(light_lum); // abs because sky lights have negative color

	return m;
}


void
process_selected_light_restir(
	uint light_idx,
	vec2 light_position,
	float weight,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	int shadow_cull_mask,
	vec3 view_direction,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	float direct_specular_weight,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	bool check_vis,
	uint cluster_idx,
	out vec3 diffuse,
	out vec3 specular,
	out float vis)
{
	float polygonal_light_pdfw = 0;
	vec3 contrib_polygonal = vec3(0);
	vec3 L, pos_on_light_polygonal;
	bool polygonal_light_is_sky = false;
	diffuse = vec3(0);
	specular = vec3(0);
	vis = 1.0;

	if (light_idx != RESTIR_ENV_ID)
	{
		LightPolygon light = get_light_polygon(light_idx);

		vec3 light_normal;

		switch (uint(light.type))
		{
		case DYNLIGHT_POLYGON:
			pos_on_light_polygonal = sample_projected_triangle(position, light.positions, light_position, light_normal, polygonal_light_pdfw);
			break;
		case DYNLIGHT_SPHERE:
			pos_on_light_polygonal = sample_projected_sphere(position, light.positions, light_position, light_normal, polygonal_light_pdfw);
			break;
		case DYNLIGHT_SPOT:
			pos_on_light_polygonal = sample_projected_spotlight(position, light.positions, light.spot_emission_profile, light_position, light_normal, polygonal_light_pdfw);
			break;
		}

		L = normalize(pos_on_light_polygonal - position);

		if (dot(L, geo_normal) <= 0)
			polygonal_light_pdfw = 0;

		if (polygonal_light_pdfw > 0) {
			float LdotNL = max(0, -dot(light_normal, L));
			float spotlight = sqrt(LdotNL);
			float inv_pdfw = 1.0 / polygonal_light_pdfw;

			if (light.color.r >= 0)
			{
				contrib_polygonal = light.color * (inv_pdfw * spotlight * light.light_style_scale);
			}
			else
			{
				contrib_polygonal = env_map(L, true) * inv_pdfw * global_ubo.pt_env_scale;
				polygonal_light_is_sky = true;
			}
		}

	}
	else
	{
		vec2 disk = sample_disk(light_position);
		disk.xy *= global_ubo.sun_tan_half_angle;
		L = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);
		polygonal_light_pdfw = global_ubo.sun_solid_angle;
		pos_on_light_polygonal = position + L * 10000;
		contrib_polygonal = env_map(L, false) * polygonal_light_pdfw * global_ubo.pt_env_scale;
	}

	contrib_polygonal *= min(weight, global_ubo.pt_restir_max_w);

	float spec_polygonal = phong(normal, L, view_direction, phong_exp) * phong_scale;

	float l_polygonal = luminance(abs(contrib_polygonal)) * mix(1, spec_polygonal, phong_weight);

	bool null_light = (l_polygonal == 0);

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light_polygonal, 0);

	if (check_vis) vis *= trace_shadow_ray(shadow_ray, null_light ? 0 : shadow_cull_mask);

#ifdef ENABLE_SHADOW_CAUSTICS
	if (enable_caustics)
	{
		contrib_polygonal *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	if (null_light)
	{
		vis = 0.0f;
		return;
	}

	vec3 radiance = vis * contrib_polygonal;

	if (direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		// MIS with direct specular and indirect specular.
		// Only applied to sky lights, for two reasons:
		//  1) Non-sky lights are trimmed to match the light texture, and indirect rays don't see that;
		//  2) Non-sky lights are usually away from walls, so the direct sampling issue is not as pronounced.

		direct_specular_weight *= get_specular_sampled_lighting_weight(roughness,
			normal, -view_direction, L, polygonal_light_pdfw);
	}

	vec3 F = vec3(0);

	if (vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, L,
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = radiance * specular_brdf * direct_specular_weight;
	}

	float NdotL = max(0, dot(normal, L));

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}


void
get_direct_illumination_restir(
	vec3 position,
	vec3 normal,
	uint cluster_idx,
	vec3 view_direction,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	int bounce,
	Reservoir prev_r,
	out Reservoir reservoir,
	out float dbg_w_fresh,
	out float dbg_W_fresh)
{
	dbg_w_fresh = 0.0;
	dbg_W_fresh = 0.0;

	init_reservoir(reservoir);

	if (cluster_idx == ~0u)
		return;

	vec3 contrib_polygonal = vec3(0);

	float rng, p_hat;

	uint list_start = light_buffer.light_list_offsets[cluster_idx];
	uint list_end = light_buffer.light_list_offsets[cluster_idx + 1];

	rng = get_rng(RNG_NEE_LIGHT_SELECTION(bounce));


	uint add_sun = (global_ubo.sun_visible != 0) && ((cluster_idx == ~0u) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0) ? 1 : 0;

	uint sun_idx = add_sun > 0 ? list_end : -1;
	list_end += add_sun;
	float list_size = float(list_end - list_start);
	float partitions = ceil(list_size / float(RESTIR_SAMPLING_M));
	float inv_pdf = list_size;
	float rng_part = rng * partitions;
	float fpart = min(floor(rng_part), partitions - 1);

	list_start += int(fpart);
	int stride = int(partitions);
	rng = rng_part - floor(rng_part);

	uint current_idx, current_light_idx;

	vec2 rng2 = vec2(
		get_rng(RNG_NEE_TRI_X(bounce)),
		get_rng(RNG_NEE_TRI_Y(bounce)));

	float samples = 1.;

	// Candidates actually DRAWN, which is not always RESTIR_SAMPLING_M: the loop
	// stops early once it walks off the end of a short light list. It has to be
	// counted here rather than taken from update_reservoir's own r.M, because that
	// one only counts candidates whose p_hat was > 0, and a drawn-but-zero
	// candidate is still a drawn candidate as far as the estimator is concerned.
	uint num_drawn = 0;

#pragma unroll
	for (uint i = 0, n_idx = list_start; i < RESTIR_SAMPLING_M; i++, n_idx += stride)
	{
		if (n_idx >= list_end)
			break;

		++num_drawn;

		current_light_idx = n_idx != sun_idx ? light_buffer.light_list_lights[n_idx] : RESTIR_ENV_ID;

		if (current_light_idx == ~0u) continue;

		p_hat = get_unshadowed_path_contrib(current_light_idx, position, normal, view_direction, phong_exp, phong_scale, phong_weight, rng2);
		if (p_hat > 0)update_reservoir(current_light_idx, p_hat * inv_pdf, rng2, p_hat, rng, reservoir);
	}

	// RIS is unbiased only when W divides by the number of candidates DRAWN. This
	// used to be a flat RESTIR_SAMPLING_M, which is correct only while the light
	// list is at least that long.
	//
	// On a sparse list it silently threw the light away. Worked through for an
	// outdoor cluster on a rerelease map, where the list is effectively just the
	// sun, so list_size == 1:
	//
	//     partitions = ceil(1/16) = 1, stride = 1  -> the loop draws ONE candidate
	//     w_sum      = p_hat * inv_pdf, inv_pdf = list_size = 1  -> w_sum = p_hat
	//     W          = p_hat / (p_hat * 16)        =  0.0625
	//
	// i.e. the sun kept 6.25% of its energy, and the shortfall is list_size/16 for
	// any list shorter than 16. That is why mgu1m1 rendered flat and shadowless
	// with ReSTIR on and correct with it off, while base1 - whose clusters carry
	// its sky area lights plus interior lights, so list_size >= 16 - looked fine
	// either way. It is also why pt_restir_max_w had no effect: W was five orders
	// of magnitude BELOW the clamp, not above it.
	reservoir.M = max(num_drawn, 1u);

	// Diagnostics for pt_restir 24 / 25: the fresh-candidate reservoir before any reuse.
	// Gated on the debug range so this costs nothing during play - pt_restir is a uniform,
	// so the branch is wavefront-uniform and the two values stay dead.
	if (global_ubo.pt_restir >= 10)
	{
		dbg_w_fresh = reservoir.w_sum;
		dbg_W_fresh = reservoir.p_hat > 0.0
			? reservoir.w_sum / (reservoir.p_hat * float(reservoir.M))
			: 0.0;
	}

	//Combine with temporal
	if (prev_r.W > 0.0 && prev_r.y != RESTIR_INVALID_ID && prev_r.p_hat > 0)
	{
		update_reservoir(prev_r.y, prev_r.p_hat * prev_r.W * prev_r.M, prev_r.y_pos, prev_r.p_hat, rng, reservoir);
		reservoir.M += prev_r.M - 1;
	}

	reservoir.W = reservoir.w_sum / (reservoir.p_hat * reservoir.M);
	if (isnan(reservoir.W) || isinf(reservoir.W)) reservoir.W = 0.0;
}



// ========================================================================== //
// Pairwise MIS for spatial reuse.
//
// The legacy combine streams a neighbour with weight  p_hat * W * M  and then
// does  M += neighbour.M - 1,  normalising at the end with  W = w_sum / (p_hat * M).
// Those M terms are a stand-in for MIS weights, and they are only correct when every
// neighbour's sample could have been drawn at this pixel with the same probability.
// At a shadow or geometry boundary that is false: neighbours whose sample contributes
// nothing here still inflate M, so W is divided by a count the numerator never got
// weight from, and the estimate loses energy. That is the sun visibly fading in as
// reuse accumulates.
//
// Pairwise MIS replaces the counts with a balance heuristic evaluated between each
// neighbour's domain and this pixel's domain, so a neighbour that could not have
// produced the sample contributes ~0 weight instead of diluting the average. The
// weights sum to one by construction, so the final estimate normalises as
// W = w_sum / p_hat with no division by M at all.
//
// Reference: Bitterli et al. 2020, and RTXDI's RTXDI_StreamNeighborWithPairwiseMIS /
// RTXDI_StreamCanonicalWithPairwiseStep.
// ========================================================================== //

// The shading parameters get_unshadowed_path_contrib needs, for one pixel.
struct RestirSurface
{
	vec3  position;
	vec3  normal;
	vec3  view_direction;
	float phong_exp;
	float phong_scale;
	float phong_weight;
};

// Rebuild a neighbour's shading parameters from the previous frame's G-buffer.
//
// NOTE on position: the neighbour's own world position is not available. The
// G-buffer has no history copy of PT_SHADING_POSITION, and reconstructing it from
// PT_VIEW_DEPTH_B needs an inverse of V_prev the UBO does not carry plus the field
// packing inverse, both of which are easy to get subtly wrong. This uses the centre
// pixel's position instead, keeping the neighbour's own normal and BRDF.
//
// That is an approximation of the MIS weight, not of the estimator. MIS weights only
// have to sum to one across strategies for the result to stay unbiased - the balance
// heuristic is merely the lowest-variance choice among the valid ones. So this costs
// a little variance at grazing geometry and nothing in correctness. The neighbour is
// already required to be within 10% depth and ~25 degrees of normal, so over a few
// pixels the position error is small next to the distance to the light anyway.
//
// Upgrade path if it ever matters: add invV_prev to the UBO, unproject
// PT_VIEW_DEPTH_B through projection_screen_to_view(.., true), and pass the real
// position here. Nothing else in this file changes.
RestirSurface
load_neighbour_surface(ivec2 pos, vec3 centre_position, vec3 view_direction)
{
	RestirSurface s;
	s.position       = centre_position;
	s.view_direction = view_direction;
	s.normal         = decode_normal(texelFetch(TEX_PT_NORMAL_B, pos, 0).x);

	vec2 metal_rough = texelFetch(TEX_PT_METALLIC_B, pos, 0).xy;
	vec4 base_color  = texelFetch(TEX_PT_BASE_COLOR_B, pos, 0);

	float alpha    = square(metal_rough.y);
	s.phong_exp    = RoughnessSquareToSpecPower(alpha);
	s.phong_scale  = min(100, 1 / (M_PI * square(alpha)));

	vec3 albedo, base_reflectivity;
	get_reflectivity(base_color.rgb, metal_rough.x, albedo, base_reflectivity);
	s.phong_weight = clamp(base_color.a * luminance(base_reflectivity)
		/ (luminance(base_reflectivity) + luminance(albedo)), 0, 0.9);

	return s;
}

float
eval_target_at(uint light_idx, vec2 light_pos, RestirSurface s)
{
	if (light_idx == RESTIR_INVALID_ID) return 0.0;
	return get_unshadowed_path_contrib(light_idx, s.position, s.normal, s.view_direction,
		s.phong_exp, s.phong_scale, s.phong_weight, light_pos);
}

// Balance heuristic between two domains, each weighted by how many samples it stands for.
// Returns the share of the weight that belongs to the "this" domain.
float
pairwise_mis_weight(float w_this, float w_other, float m_this, float m_other)
{
	float denom = m_this * w_this + m_other * w_other;
	return denom > 0.0 ? (m_this * w_this) / denom : 0.0;
}

#endif  /*_RESTIR_H_*/