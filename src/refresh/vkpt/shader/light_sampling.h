/*
Copyright (C) 2018 Tobias Zirr
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
Copyright (C) 2026 Q2RTX contributors

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

/*
==============================================================================

LIGHT SAMPLING - draw a point on a light and get its solid-angle pdf.

Moved here VERBATIM out of light_lists.h.  Nothing changed in the move.

WHY IT IS ITS OWN HEADER: every one of these functions takes only the shading
point and the light, and NONE of them needs a surface normal or a BRDF.  That is
what makes them usable from a VOLUME, where there is no normal to give - the fog
passes need exactly this and nothing else from the direct-lighting path.

light_lists.h could not simply be included there: it also defines sample_lights(),
which reads the light_stats_bufers descriptor that the fog passes do not declare.
Splitting the reusable half out is cheaper than either duplicating it (a copy of
spotlight_falloff had already appeared in fog_medium.glsl and would have drifted)
or dragging the whole direct-lighting path into a compute shader that wants none
of it.

What stayed behind in light_lists.h is the IMPORTANCE half - projected_tri_area,
projected_sphere_area, projected_spotlight_area - because those do take a normal
and a phong lobe, and a volume needs a different weight entirely.

The includer must already have brought in:

    constants.h        (M_PI, DYNLIGHT_SPOT_EMISSION_PROFILE_*)
    utils.glsl         (sample_triangle, sample_disk, construct_ONB_frisvad)
    global_textures.h  (global_texture, for the spot emission profile texture)

==============================================================================
*/

#ifndef _LIGHT_SAMPLING_
#define _LIGHT_SAMPLING_

// x*x. brdf.glsl has a square() but that header is the whole BRDF library and
// the volumetric passes include none of it, so this file carries its own rather
// than making every includer take a dependency it does not otherwise want.
float ls_square(float x) { return x * x; }

mat3
project_triangle(mat3 positions, vec3 p)
{
	positions[0] = positions[0] - p;
	positions[1] = positions[1] - p;
	positions[2] = positions[2] - p;

	positions[0] = normalize(positions[0]);
	positions[1] = normalize(positions[1]);
	positions[2] = normalize(positions[2]);

	return positions;
}

// Emission of a spot light at an angle from its axis, given as the cosine of that
// angle. positions[1].yz carry the profile's parameters and mean different things
// per profile - see add_dlights(), which writes them.
float
spotlight_falloff(mat3 positions, float emission_profile, float cosTheta)
{
	if (emission_profile == DYNLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE)
	{
		if (cosTheta < 0)
			return 0;

		const float totalWidth = positions[1].y;
		const uint texture_num = uint(positions[1].z);

		// Index by the angle rather than by its cosine: that spends more of the
		// texture on the middle of the beam, where the detail is.
		float tc = clamp(acos(cosTheta) / totalWidth, 0, 1);
		return global_texture(texture_num, vec2(tc, 0)).r;
	}

	const float cosTotalWidth = positions[1].y;
	const float cosFalloffStart = positions[1].z;

	if (cosTheta < cosTotalWidth)
		return 0;
	if (cosTheta > cosFalloffStart)
		return 1;

	float delta = (cosTheta - cosTotalWidth) / (cosFalloffStart - cosTotalWidth);
	return (delta * delta) * (delta * delta);
}

float pdf_area_to_solid_angle(float pdfA, float distance_, float cos_theta)
{
	return pdfA * ls_square(distance_) / cos_theta;
}

float get_triangle_pdfw(mat3 positions, vec3 sample_pos)
{
	vec3 normal = cross(positions[1] - positions[0], positions[2] - positions[0]);
	float normal_length = length(normal);
	float sample_pos_distance = length(sample_pos);

	// The samples should be more or less on the unit sphere. If they are much closer than
	// 1 unit away, this means the projected light is very large, and the surface is likely
	// on the light itself.
	float clamped_sample_pos_distance = max(sample_pos_distance, 0.1);

	if (normal_length > 0 && sample_pos_distance > 0)
	{
		float cos_theta = -dot(normal / normal_length, sample_pos / sample_pos_distance);
		return pdf_area_to_solid_angle(2.0 / normal_length, clamped_sample_pos_distance, cos_theta);
	}

	return 0;
}

vec3
sample_projected_triangle(vec3 p, mat3 positions, vec2 rnd, out vec3 light_normal, out float pdfw)
{
	light_normal = cross(positions[1] - positions[0], positions[2] - positions[0]);
	light_normal = normalize(light_normal);

	positions[0] = positions[0] - p;
	positions[1] = positions[1] - p;
	positions[2] = positions[2] - p;

	float o = dot(light_normal, positions[0]);

	positions[0] = normalize(positions[0]);
	positions[1] = normalize(positions[1]);
	positions[2] = normalize(positions[2]);

	vec3 direction = positions * sample_triangle(rnd);
	float dl = length(direction);

	// n (p + d * t - p[i]) == 0
	// -n (p - pi) / n d == o / n d == t
	vec3 lo = direction * (o / dot(light_normal, direction));

	pdfw = get_triangle_pdfw(positions, direction);

	return p + lo;
}

vec3
sample_projected_sphere(vec3 p, mat3 positions, vec2 rnd, out vec3 light_normal, out float pdfw)
{
	vec3 light_center = positions[0];
	vec3 position = light_center - p;
	float sphere_radius = positions[1].x;
	float dist = length(position);
	float rdist = 1.0 / dist;
	vec3 L = position * rdist;

	float projected_area = 2 * (1 - sqrt(max(0, 1 - ls_square(sphere_radius * rdist))));
	projected_area = min(projected_area, 2 * M_PI); //max solid angle
	pdfw = 1.0 / projected_area;

	mat3 onb = construct_ONB_frisvad(L);
	vec3 diskpt;
	diskpt.xy = sample_disk(rnd);
	diskpt.z = sqrt(max(0, 1 - diskpt.x * diskpt.x - diskpt.y * diskpt.y));

	vec3 position_light = light_center + (onb[0] * diskpt.x + onb[2] * diskpt.y - L * diskpt.z) * sphere_radius;

	light_normal = normalize(position_light - light_center);

	return position_light;
}

vec3
sample_projected_spotlight(vec3 p, mat3 positions, float emission_profile, vec2 rnd, out vec3 light_normal, out float pdfw)
{
	vec3 light_center = positions[0];
	float emitter_radius = positions[1].x;

	mat3 onb = construct_ONB_frisvad(positions[2]);
	// Emit light from a small disk around the origin
	vec2 diskpt = sample_disk(rnd);
	vec3 position_light = light_center + (onb[0] * diskpt.x + onb[2] * diskpt.y) * emitter_radius;

	vec3 c = position_light - p;
	float dist = length(c);
	float rdist = 1.0 / dist;
	vec3 L = c * rdist;

	// Direction from emission point to surface, in a basis where +Y is the spot direction
	vec3 L_l = -L * onb;
	float cosTheta = L_l.y; // cosine of angle to spot direction
	float falloff = spotlight_falloff(positions, emission_profile, cosTheta);

	float projected_area = 2 * falloff * ls_square(rdist);
	projected_area = min(projected_area, 2 * M_PI); //max solid angle
	pdfw = 1.0 / projected_area;

	light_normal = normalize(positions[2]);

	return position_light;
}

#endif /*_LIGHT_SAMPLING_*/
