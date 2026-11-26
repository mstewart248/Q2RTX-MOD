/*
Copyright (C) 2018 Tobias Zirr
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

#ifndef _LIGHT_LISTS_
#define _LIGHT_LISTS_

// the normal-free half of this file, split out so the volumetric passes can
// sample a light without pulling in sample_lights() and its light_stats_bufers
#include "light_sampling.h"

#define MAX_BRUTEFORCE_SAMPLING 8

float
projected_tri_area(mat3 positions, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight)
{
	positions[0] = positions[0] - p;
	positions[1] = positions[1] - p;
	positions[2] = positions[2] - p;

	vec3 g = cross(positions[1] - positions[0], positions[2] - positions[0]);
	if (dot(n, positions[0]) <= 0 && dot(n, positions[1]) <= 0 && dot(n, positions[2]) <= 0)
		return 0;
	if (dot(g, positions[0]) >= 0 && dot(g, positions[1]) >= 0 && dot(g, positions[2]) >= 0)
		return 0;

	vec3 L = normalize(positions * vec3(1.0 / 3.0));
	float specular = phong(n, L, V, phong_exp) * phong_scale;
	float brdf = mix(1.0, specular, phong_weight);

	positions[0] = normalize(positions[0]);
	positions[1] = normalize(positions[1]);
	positions[2] = normalize(positions[2]);

	vec3 a = cross(positions[1] - positions[0], positions[2] - positions[0]);
	float pa = max(length(a) - 1e-5, 0.);
	return pa * brdf;
}

// projected_tri_area with the EXACT solid angle as the selection mass, matching
// sample_spherical_triangle (pt_light_spherical_tri). The planar area above is
// (|cross| of the projected triangle) = twice the area of the flat triangle through the
// three projected vertices, which falls well short of the solid angle once a light is
// large or close - a light covering an octant gets ~0.55x the mass its contribution
// needs, so it is picked too rarely and comes back with too large a weight.
float
spherical_tri_area(mat3 positions, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight)
{
	positions[0] = positions[0] - p;
	positions[1] = positions[1] - p;
	positions[2] = positions[2] - p;

	vec3 g = cross(positions[1] - positions[0], positions[2] - positions[0]);
	if (dot(n, positions[0]) <= 0 && dot(n, positions[1]) <= 0 && dot(n, positions[2]) <= 0)
		return 0;
	if (dot(g, positions[0]) >= 0 && dot(g, positions[1]) >= 0 && dot(g, positions[2]) >= 0)
		return 0;

	vec3 L = normalize(positions * vec3(1.0 / 3.0));
	float specular = phong(n, L, V, phong_exp) * phong_scale;
	float brdf = mix(1.0, specular, phong_weight);

	float area = spherical_triangle_area(normalize(positions[0]), normalize(positions[1]), normalize(positions[2]));
	float pa = max(area - 1e-5, 0.);
	return pa * brdf;
}

// max_irradiance caps the light's projected size, in the 2 * (1 - cos) measure used
// here - see pt_bounce_dlight_cap. The 7-argument overloads below keep the old 2 * pi.
float
projected_sphere_area(mat3 positions, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight, float max_irradiance)
{
	vec3 position = positions[0] - p;
	float sphere_radius = positions[1].x;
	float dist = length(position);
	float rdist = 1.0 / dist;
	vec3 L = position * rdist;

	if (dot(n, L) <= 0)
		return 0.0;

	float specular = phong(n, L, V, phong_exp) * phong_scale;
	float brdf = mix(1.0, specular, phong_weight);

	float irradiance = 2 * (1 - sqrt(max(0, 1 - square(sphere_radius * rdist))));
	irradiance = min(irradiance, max_irradiance); //max solid angle

	return irradiance * brdf;
}

float
projected_sphere_area(mat3 positions, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight)
{
	return projected_sphere_area(positions, p, n, V, phong_exp, phong_scale, phong_weight, 2 * M_PI);
}

float
projected_spotlight_area(mat3 positions, float emission_profile, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight, float max_irradiance)
{
	vec3 position = positions[0] - p;
	float sphere_radius = positions[1].x;

	float dist = length(position);
	float rdist = 1.0 / dist;
	vec3 L = position * rdist;

	if (dot(n, L) <= 0)
		return 0.0;

	float specular = phong(n, L, V, phong_exp) * phong_scale;
	float brdf = mix(1.0, specular, phong_weight);

	float cosTheta = dot(-L, positions[2]); // cosine of angle to spot direction
	float falloff = spotlight_falloff(positions, emission_profile, cosTheta);

	float irradiance = 2 * falloff * square(rdist);

	irradiance = min(irradiance, max_irradiance); //max solid angle

	return irradiance * brdf;
}

float
projected_spotlight_area(mat3 positions, float emission_profile, vec3 p, vec3 n, vec3 V, float phong_exp, float phong_scale, float phong_weight)
{
	return projected_spotlight_area(positions, emission_profile, p, n, V, phong_exp, phong_scale, phong_weight, 2 * M_PI);
}

/* THE LIGHT STATISTICS, ONE SLOT PER LIST ENTRY (2026-11-26; Matt: "go through the
   quake3 team arena demo with the profiler on ... long frame setup times").

   The shadow-ray counts (pt_light_stats, path_tracer_rgen.h's get_direct_illumination
   says what they are for) were laid out for every cluster x every static light,
   (cluster * num_static_lights + light) * 12, and one of the three buffers is zeroed
   every frame. On Team Arena's mpteam1 that is 2535 clusters x 7100 lights x 48
   bytes = 864 MB a buffer, 2.6 GB for the three, and ~1 ms of every GPU frame in
   the clear (the "list time no row covers" after the uploads). Yet only counts of a
   cluster's own list are ever read: sample_lights below and the ReSTIR prior
   (restir.h) look up lights they drew from that list - 1.5 million entries on
   mpteam1, so 72 MB a buffer now and a clear of ~0.1 ms. Every count that is read
   is the one it was. The only counts dropped are those of a light outside the
   cluster's list - ReSTIR shading a light it reused from a neighbour - which were
   written and never read.

   The layout is constants.h's. The lists the GPU walks are the static lists, each
   with the model lights that reach the cluster after it (inject_model_lights, and
   the kept lists the same), so the static part of a cluster's list starts at
   light_list_offsets[cluster] and its entries are in light order
   (build_cluster_light_lists). An entry's counts are found by its place in the
   list, or, for a light alone, by a search of that part (light_stats_find). */

// where the cluster's counts start and how many static entries it has; false if none
bool light_stats_cluster(uint cluster, out uint first, out uint count)
{
	first = 0;
	count = 0;
	if (cluster >= light_stats_bufers[0].stats[LIGHT_STATS_HDR_CLUSTERS])
		return false;
	first = light_stats_bufers[0].stats[LIGHT_STATS_HDR_FIRST + cluster];
	count = (light_stats_bufers[0].stats[LIGHT_STATS_HDR_FIRST + cluster + 1] - first) / LIGHT_STATS_WORDS_PER_SLOT;
	return count > 0;
}

// the counts of the entry at `place` in the cluster's list, for one orientation:
// [addr] got through, [addr + 1] shadowed. ~0u past the list's static part.
uint light_stats_entry(uint first, uint count, uint place, uint side)
{
	return place < count ? first + place * LIGHT_STATS_WORDS_PER_SLOT + side * 2 : ~0u;
}

// the same for a static light by its index, searched for in the cluster's list;
// ~0u when the list does not hold it
uint light_stats_find(uint cluster, uint light, uint side)
{
	uint first, count;
	if (!light_stats_cluster(cluster, first, count))
		return ~0u;
	uint base = light_buffer.light_list_offsets[cluster];
	uint lo = 0, hi = count;
	while (lo < hi)
	{
		uint mid = (lo + hi) >> 1;
		if (light_buffer.light_list_lights[base + mid] < light)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < count && light_buffer.light_list_lights[base + lo] == light
		? light_stats_entry(first, count, lo, side) : ~0u;
}

void
sample_lights(
	uint list_idx,
	vec3 p,
	vec3 n,
	vec3 gn,
	vec3 V,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	bool is_gradient,
	out vec3 position_light,
	out vec3 light_color,
	out int light_index,
	out float pdfw,
	out bool is_sky_light,
	vec3 rng,
	float max_dyn_irradiance, // pt_bounce_dlight_cap for bounces, 2 * pi otherwise
	out uint light_stats_addr) // the picked light's counts for n's orientation, ~0u if none (light_stats_entry)
{
	position_light = vec3(0);
	light_index = -1;
	light_color = vec3(0);
	pdfw = 0;
	is_sky_light = false;
	light_stats_addr = ~0u;

	// pt_light_spherical_tri - see global_ubo.h. Mass and sampler must agree, so both
	// switch together.
	bool spherical_tri = global_ubo.pt_light_spherical_tri != 0;

	if (list_idx == ~0u)
		return;

	uint list_start = light_buffer.light_list_offsets[list_idx];
	uint list_end = light_buffer.light_list_offsets[list_idx + 1];

	// the static entries' counts (THE LIGHT STATISTICS, ONE SLOT PER LIST ENTRY)
	const uint list_base = list_start;
	const uint stats_side = get_primary_direction(n);
	uint stats_first, stats_count;
	const bool stats = global_ubo.pt_light_stats != 0 && light_stats_cluster(list_idx, stats_first, stats_count);

	float partitions = ceil(float(list_end - list_start) / float(MAX_BRUTEFORCE_SAMPLING));
	rng.x *= partitions;
	float fpart = min(floor(rng.x), partitions - 1);
	rng.x -= fpart;
	list_start += int(fpart);
	int stride = int(partitions);

	float mass = 0.;

	float light_masses[MAX_BRUTEFORCE_SAMPLING];

#pragma unroll
	for (uint i = 0, n_idx = list_start; i < MAX_BRUTEFORCE_SAMPLING; i++, n_idx += stride) {
		if (n_idx >= list_end)
			break;

		uint current_idx = light_buffer.light_list_lights[n_idx];

		if (current_idx == ~0u)
		{
			light_masses[i] = 0;
			continue;
		}

		LightPolygon light = get_light_polygon(current_idx);

		float m = 0.0f;
		switch (uint(light.type)) {
		case DYNLIGHT_POLYGON:
			m = spherical_tri
				? spherical_tri_area(light.positions, p, n, V, phong_exp, phong_scale, phong_weight)
				: projected_tri_area(light.positions, p, n, V, phong_exp, phong_scale, phong_weight);
			break;
		case DYNLIGHT_SPHERE:
			m = projected_sphere_area(light.positions, p, n, V, phong_exp, phong_scale, phong_weight, max_dyn_irradiance);
			break;
		case DYNLIGHT_SPOT:
			m = projected_spotlight_area(light.positions, light.spot_emission_profile, p, n, V, phong_exp, phong_scale, phong_weight, max_dyn_irradiance);
			break;
		case DYNLIGHT_WINDOW:
			m = window_light_mass(light, p, n);
			break;
		}

		float light_lum = luminance(light.color);

		// Apply light style scaling.
		// For gradient pixels, use the style from the previous frame here
		// in order to keep the CDF consistent and make sure that the same light is picked,
		// regardless of animations. This makes the image more stable around blinking lights,
		// especially in shadowed areas.
		light_lum *= is_gradient ? light.prev_style_scale : light.light_style_scale;

		if (light_lum < 0 && global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
		{
			// Set limits on sky luminance to avoid oversampling the sky in shadowed areas, or undersampling at dusk and dawn.
			// Note: the log -> linear conversion of the cvars happens on the CPU, in main.c
			m *= clamp(sun_color_ubo.sky_luminance, global_ubo.pt_min_log_sky_luminance, global_ubo.pt_max_log_sky_luminance);
		}
		else
			m *= abs(light_lum); // abs because sky lights have negative color

		// Apply CDF adjustment based on light shadowing statistics from one of the previous frames.
		// See comments in function `get_direct_illumination` in `path_tracer_rgen.h`
		// (a static light is in the list's static part: light_stats_entry is ~0u for
		// the model lights after it)
		uint addr = stats && m > 0 && current_idx < global_ubo.num_static_lights
			? light_stats_entry(stats_first, stats_count, n_idx - list_base, stats_side) : ~0u;
		if (addr != ~0u)
		{
			uint buffer_idx = global_ubo.current_frame_idx;
			// Regular pixels get shadowing stats from the previous frame;
			// Gradient pixels get the stats from two frames ago because they need to match
			// the light sampling from the previous frame.
			buffer_idx += is_gradient ? (NUM_LIGHT_STATS_BUFFERS - 2) : (NUM_LIGHT_STATS_BUFFERS - 1);
			buffer_idx = buffer_idx % NUM_LIGHT_STATS_BUFFERS;

			uint num_hits = light_stats_bufers[buffer_idx].stats[addr];
			uint num_misses = light_stats_bufers[buffer_idx].stats[addr + 1];
			uint num_total = num_hits + num_misses;

			if (num_total > 0)
			{
				// Adjust the mass, but set a lower limit on the factor to avoid
				// extreme changes in the sampling.
				m *= max(float(num_hits) / float(num_total), 0.1);
			}
		}

		mass += m;
		light_masses[i] = m;
	}

	if (mass <= 0)
		return;

	rng.x *= mass;
	int current_idx = -1;
	mass *= partitions;
	float pdf = 0;

#pragma unroll
	for (uint i = 0, n_idx = list_start; i < MAX_BRUTEFORCE_SAMPLING; i++, n_idx += stride) {
		if (n_idx >= list_end)
			break;
		pdf = light_masses[i];
		current_idx = int(n_idx);
		rng.x -= pdf;

		if (rng.x <= 0)
			break;
	}

	if (rng.x > 0)
		return;

	pdf /= mass;

	// assert: current_idx >= 0?
	if (current_idx >= 0) {
		if (stats)
			light_stats_addr = light_stats_entry(stats_first, stats_count, uint(current_idx) - list_base, stats_side);
		current_idx = int(light_buffer.light_list_lights[current_idx]);

		LightPolygon light = get_light_polygon(current_idx);

		// DYNLIGHT_WINDOW has no normal: its sample comes back with the glass's
		// colour where it crosses, its radiance and 1 / pdfw already in, in this
		// slot instead (sample_window_light) - one vec3 less held in the loop
		vec3 light_normal = vec3(0);

		switch (uint(light.type)) {
		case DYNLIGHT_POLYGON:
			if (spherical_tri)
			{
				position_light = sample_spherical_triangle(p, light.positions, rng.yz, light_normal, pdfw);
				// Degenerate triangle: no sample, but keep the position finite for the
				// caller's shadow-ray and phong math.
				if (!(pdfw > 0))
					position_light = light.positions[0];
			}
			else
				position_light = sample_projected_triangle(p, light.positions, rng.yz, light_normal, pdfw);
			break;
		case DYNLIGHT_SPHERE:
			position_light = sample_projected_sphere(p, light.positions, rng.yz, light_normal, pdfw);
			break;
		case DYNLIGHT_SPOT:
			position_light = sample_projected_spotlight(p, light.positions, light.spot_emission_profile, rng.yz, light_normal, pdfw);
			break;
		case DYNLIGHT_WINDOW:
			position_light = sample_window_light(p, light, rng.yz, light_normal, pdfw);
			break;
		}

		vec3 L = normalize(position_light - p);

		if (dot(L, gn) <= 0)
			pdfw = 0;

		if (pdfw > 0)
		{
			float LdotNL = max(0, -dot(light_normal, L));
			float spotlight = sqrt(LdotNL);
			float inv_pdfw = 1.0 / pdfw;

			// The same cap the selection mass above was computed with, so the estimate
			// stays consistent: a capped light is picked as often as its capped size says
			// and returns that capped size. 1 / pdfw is the 2 * (1 - cos) size here.
			if (uint(light.type) != DYNLIGHT_POLYGON && uint(light.type) != DYNLIGHT_WINDOW)
				inv_pdfw = min(inv_pdfw, max_dyn_irradiance);

			if (uint(light.type) == DYNLIGHT_WINDOW)
			{
				light_color = light.color * light_normal * light.light_style_scale;
			}
			else if (light.color.r >= 0)
			{
				light_color = light.color * (inv_pdfw * spotlight * light.light_style_scale);
			}
			else
			{
				// less under the clouds (pt_q3_cloud_shadow_sky; q3_sky.glsl)
				light_color = env_map(L, true) * inv_pdfw * global_ubo.pt_env_scale * q3_cloud_sky_shadow(p);
				is_sky_light = true;
			}
		}

		light_index = current_idx;
	}

	light_color /= pdf;
}


#endif /*_LIGHT_LISTS_*/

