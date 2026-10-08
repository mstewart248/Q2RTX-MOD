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

/*
EXACT SPHERICAL-TRIANGLE SAMPLING (pt_light_spherical_tri).

Upstream Q2RTX's sampler (Frank Richter, 2023), which ff3ec4ef replaced with the planar
projection above. The planar version samples the flat triangle spanned by the three
projected vertices and converts its area pdf to solid angle per sample, so the weight
of a sample changes across a large or close light - about 5x across a light covering
an octant of the hemisphere. This one is uniform in solid angle, so every sample of a
triangle carries the same weight.

Kept beside the planar functions rather than replacing them: ReSTIR DI and the fog
still call those, and their target functions were tuned against them.
*/

// Solid angle of a triangle whose vertices are already projected onto the unit sphere
// around the shading point. From "On the Measure of Solid Angles", F. Eriksson, 1990.
float spherical_triangle_area(vec3 A, vec3 B, vec3 C)
{
	return 2.0 * atan(abs(dot(A, cross(B, C))), 1.0 + dot(A, B) + dot(B, C) + dot(A, C));
}

// pdf per steradian of sample_spherical_triangle, for a triangle already projected with
// project_triangle(). Uniform, so it does not depend on where the sample landed.
float get_spherical_triangle_pdfw(mat3 projected_positions)
{
	float area = spherical_triangle_area(projected_positions[0], projected_positions[1], projected_positions[2]);
	return area > 0.0 ? 1.0 / area : 0.0;
}

/* Sample a triangle, projected to a unit sphere.
 *
 * The implementation is based on the algorithm described in:
 * James Arvo. 1995. Stratified sampling of spherical triangles.
 * Proceedings of the 22nd annual conference on Computer graphics and interactive techniques (SIGGRAPH '95).
 * Association for Computing Machinery, New York, NY, USA, 437-438.
 * https://doi.org/10.1145/218380.218500
 *
 * pdfw is 0 for a degenerate triangle; the caller must not use the sample then.
 */
vec3
sample_spherical_triangle(vec3 pt, mat3 positions, vec2 rnd, out vec3 light_normal, out float pdfw)
{
	light_normal = cross(positions[1] - positions[0], positions[2] - positions[0]);
	light_normal = normalize(light_normal);

	// Use surface point as origin
	positions[0] = positions[0] - pt;
	positions[1] = positions[1] - pt;
	positions[2] = positions[2] - pt;

	// Distance of triangle to origin
	float o = dot(light_normal, positions[0]);

	// Project triangle to unit sphere
	vec3 A = normalize(positions[0]);
	vec3 B = normalize(positions[1]);
	vec3 C = normalize(positions[2]);
	// Planes passing through two vertices and origin. They'll be used to obtain the angles.
	vec3 norm_AB = normalize(cross(A, B));
	vec3 norm_CA = normalize(cross(C, A));
	// Side of spherical triangle
	float cos_c = dot(A, B);
	// Angle at vertex A
	float cos_alpha = dot(norm_AB, -norm_CA);

	float area = spherical_triangle_area(A, B, C);

	// Use one random variable to select the new area.
	float new_area = rnd.x * area;

	float sin_alpha = sqrt(max(0.0, 1.0 - cos_alpha * cos_alpha)); // = sin(acos(cos_alpha))
	float sin_new_area = sin(new_area);
	float cos_new_area = cos(new_area);
	// Save the sine and cosine of the angle phi.
	float p = sin_new_area * cos_alpha - cos_new_area * sin_alpha;
	float q = cos_new_area * cos_alpha + sin_new_area * sin_alpha;

	// Compute the pair (u, v) that determines new_beta.
	float u = q - cos_alpha;
	float v = p + sin_alpha * cos_c;

	// Let cos_b be the cosine of the new edge length new_b.
	float cos_b = clamp(((v * q - u * p) * cos_alpha - v) / ((v * p + u * q) * sin_alpha), -1.0, 1.0);

	// Compute the third vertex of the sub-triangle.
	vec3 new_C = cos_b * A + sqrt(max(0.0, 1.0 - cos_b * cos_b)) * normalize(C - dot(C, A) * A);

	// Use the other random variable to select cos(phi).
	float z = 1.0 - rnd.y * (1.0 - dot(new_C, B));

	// Construct the corresponding point on the sphere.
	vec3 direction = z * B + sqrt(max(0.0, 1.0 - z * z)) * normalize(new_C - dot(new_C, B) * B);
	// ...which is also the direction!

	// Line-plane intersection
	vec3 lo = direction * (o / dot(light_normal, direction));

	// Since the solid angle is distributed uniformly, the PDF wrt to solid angle is simply:
	pdfw = area > 0.0 ? 1.0 / area : 0.0;

	// A sliver triangle or a sample on its edge can produce a NaN or Inf above. Report
	// no sample rather than hand the caller a non-finite light position.
	if (any(isnan(lo)) || any(isinf(lo)) || isnan(pdfw) || isinf(pdfw))
	{
		pdfw = 0.0;
		lo = positions[0];
	}

	return pt + lo;
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
