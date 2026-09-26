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

#include "path_tracer.h"
#include "utils.glsl"
#include "path_tracer_transparency.glsl"

#define RAY_GEN_DESCRIPTOR_SET_IDX 0
layout(set = RAY_GEN_DESCRIPTOR_SET_IDX, binding = 0)
uniform accelerationStructureEXT topLevelAS[TLAS_COUNT];


#define GLOBAL_TEXTURES_DESC_SET_IDX 2
#include "global_textures.h"

#define VERTEX_BUFFER_DESC_SET_IDX 3
#define VERTEX_READONLY 1
#include "vertex_buffer.h"

#include "asvgf.glsl"
#include "brdf.glsl"
#include "water.glsl"

#define DESATURATE_ENVIRONMENT_MAP 1

#define RNG_SEED_SHIFT_X        0u
#define RNG_SEED_SHIFT_Y        8u
#define RNG_SEED_SHIFT_ISODD    16u
#define RNG_SEED_SHIFT_FRAME    17u

#define RNG_PRIMARY_OFF_X   0
#define RNG_PRIMARY_OFF_Y   1
#define RNG_PRIMARY_APERTURE_X   2
#define RNG_PRIMARY_APERTURE_Y   3

#define RNG_NEE_LIGHT_SELECTION(bounce)   		(4 + 0 + 12 * bounce)
#define RNG_NEE_TRI_X(bounce)             		(4 + 1 + 12 * bounce)
#define RNG_NEE_TRI_Y(bounce)             		(4 + 2 + 12 * bounce)
#define RNG_NEE_LIGHT_TYPE(bounce)        		(4 + 3 + 12 * bounce)
#define RNG_BRDF_X(bounce)                		(4 + 4 + 12 * bounce)
#define RNG_BRDF_Y(bounce)                		(4 + 5 + 12 * bounce)
#define RNG_BRDF_FRESNEL(bounce)          		(4 + 6 + 12 * bounce)
#define RNG_SUNLIGHT_X(bounce)			  		(4 + 7 + 12 * bounce)
#define RNG_SUNLIGHT_Y(bounce)			  		(4 + 8 + 12 * bounce)
#define RNG_RESTIR_SP_LIGHT_SELECTION(bounce) 	(4 + 9 + 12 * bounce)
#define RNG_RESTIR_SPATIAL_X(bounce)	  		(4 + 10 + 12 * bounce)
#define RNG_RESTIR_SPATIAL_Y(bounce)	  		(4 + 11 + 12 * bounce)

// AS_FLAG_BLOOD is in all four: a droplet is visible, reflective, bounces light
// and casts a shadow exactly like any other opaque geometry. It is a separate bit
// only so the fog march can skip it - see constants.h.
#define PRIMARY_RAY_CULL_MASK        (AS_FLAG_OPAQUE | AS_FLAG_TRANSPARENT | AS_FLAG_VIEWER_WEAPON | AS_FLAG_SKY | AS_FLAG_BLOOD)
#define REFLECTION_RAY_CULL_MASK     (AS_FLAG_OPAQUE | AS_FLAG_SKY | AS_FLAG_BLOOD)
#define BOUNCE_RAY_CULL_MASK         (AS_FLAG_OPAQUE | AS_FLAG_SKY | AS_FLAG_CUSTOM_SKY | AS_FLAG_BLOOD)
#define SHADOW_RAY_CULL_MASK         (AS_FLAG_OPAQUE | AS_FLAG_BLOOD)

/* no BRDF sampling in last bounce */
#define NUM_RNG_PER_FRAME (RNG_NEE_STATIC_DYNAMIC(1) + 1)

#define BOUNCE_SPECULAR 1

#define MAX_OUTPUT_VALUE 1000

#ifdef KHR_RAY_QUERY

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// Just global variables in RQ mode.
// No shadow payload necessary.
RayPayloadGeometry ray_payload_geometry;
RayPayloadEffects ray_payload_effects;

#include "path_tracer_hit_shaders.h"

#else // !KHR_RAY_QUERY

layout(location = RT_PAYLOAD_GEOMETRY) rayPayloadEXT RayPayloadGeometry ray_payload_geometry;
layout(location = RT_PAYLOAD_EFFECTS) rayPayloadEXT RayPayloadEffects ray_payload_effects;

#endif

uint rng_seed;

struct Ray {
	vec3 origin, direction;
	float t_min, t_max;
};

vec3
env_map(vec3 direction, bool remove_sun)
{
	direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;

	vec3 envmap = vec3(0);
	if (global_ubo.environment_type == ENVIRONMENT_DYNAMIC)
	{
		envmap = textureLod(TEX_PHYSICAL_SKY, direction.xzy, 0).rgb;

		if (remove_sun)
		{
			// roughly remove the sun from the env map
			envmap = min(envmap, vec3((1 - dot(direction, global_ubo.sun_direction_envmap)) * 200));
		}
	}
	else if (global_ubo.environment_type == ENVIRONMENT_STATIC)
	{
		envmap = textureLod(TEX_ENVMAP, direction.xzy, 0).rgb;
#if DESATURATE_ENVIRONMENT_MAP
		float avg = (envmap.x + envmap.y + envmap.z) / 3.0;
		envmap = mix(envmap, avg.xxx, 0.1) * 0.5;
#endif
	}
	return envmap;
}

// depends on env_map
#include "light_lists.h"

// Screen pixel this invocation is responsible for.
ivec2 get_image_position()
{
	ivec2 pos;

	// Full-resolution fields: each field is a complete width x height layer of the
	// screen, so the launch index *is* the screen position. Field 0 traces the
	// reflection path and field 1 the refraction path, and the two are summed back
	// together after lighting - no checkerboard, and every pixel gets both.
	if (global_ubo.pt_fullres_fields != 0)
		return ivec2(rt_LaunchID.xy);

	// Classic checkerboard: the field holds every other pixel, densely packed.
	bool is_even_checkerboard = push_constants.gpu_index == 0 || push_constants.gpu_index < 0 && rt_LaunchID.z == 0;
	if (global_ubo.pt_swap_checkerboard != 0)
		is_even_checkerboard = !is_even_checkerboard;

	if (is_even_checkerboard) {
		pos.x = int(rt_LaunchID.x * 2) + int(rt_LaunchID.y & 1);
	}
	else {
		pos.x = int(rt_LaunchID.x * 2 + 1) - int(rt_LaunchID.y & 1);
	}

	pos.y = int(rt_LaunchID.y);
	return pos;
}

ivec2 get_image_size()
{
	return ivec2(global_ubo.width, global_ubo.height);
}

bool
found_intersection(RayPayloadGeometry rp)
{
	return rp.primitive_id != ~0u;
}

Triangle
get_hit_triangle(RayPayloadGeometry rp)
{
	return load_and_transform_triangle(
		/* instance_idx = */ rp.buffer_and_instance_idx >> 16,
		/* buffer_idx = */ rp.buffer_and_instance_idx & 0xffff,
		rp.primitive_id);
}

vec3
get_hit_barycentric(RayPayloadGeometry rp)
{
	vec3 bary;
	bary.yz = rp.barycentric;
	bary.x = 1.0 - bary.y - bary.z;
	return bary;
}

// PCG hash. Low 'bigcrush' failure count, which is what DLSS-RR guide 3.5 asks for when it
// says to use a high quality hash function.
uint pcg_hash(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float
get_rng(uint idx)
{
	// DLSS-RR guide 3.5 requires samples with minimal spatial and temporal correlation, and
	// explicitly lists "sharing of sampling patterns across the screen" and screen-space
	// dithering among the practices to avoid. The blue-noise path below does exactly that:
	// rng_seed packs ipos mod BLUE_NOISE_RES, so every pixel 256 apart draws an identical
	// sequence. A-SVGF was built around that; RR was not.
	if(global_ubo.pt_rr_white_noise != 0)
	{
		uint h = pcg_hash(rng_seed + idx * 0x9E3779B9u);
		return min(float(h) * (1.0 / 4294967296.0), 0.9999999999999);
	}

	uvec3 p = uvec3(rng_seed, rng_seed >> 10, rng_seed >> 20);
	p.z = (p.z + idx);
	p &= uvec3(BLUE_NOISE_RES - 1, BLUE_NOISE_RES - 1, NUM_BLUE_NOISE_TEX - 1);

	return min(texelFetch(TEX_BLUE_NOISE, ivec3(p), 0).r, 0.9999999999999);
}

bool
is_water(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_WATER;
}

bool
is_blood(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_BLOOD;
}

bool
is_slime(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SLIME;
}

bool
is_lava(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_LAVA;
}

bool
is_glass(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_GLASS;
}

bool
is_transparent(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_TRANSPARENT;
}

/*
A TRANSLUCENT SPLAT MUST NOT SHADOW WHAT YOU ARE LOOKING THROUGH IT AT.

This is the trap in the whole idea, and it is invisible until you look for it. A
landed splat lies flush ON the surface - its base is sunk below the floor plane
so the rim feathers in - so the thing "behind" it is the floor directly beneath
it, at almost the same point. That floor point's shadow rays go straight back up
into the puddle, which is AS_FLAG_BLOOD, FORCE_OPAQUE and in the shadow mask. So
without this the transparency reveals a floor in the splat's own shadow: a black
hole with a red rim, which is worse than the solid splat it replaced.

Water and glass avoid it by not being in the shadow mask at all, and that is the
precedent followed here. Nothing is really lost: a splat's shadow falls entirely
underneath itself and a droplet in flight is 1.2 units across, so blood's
contribution to shadowing was never visible in the first place.

All or nothing, because the mask is per-INSTANCE and every droplet shares one
BLAS - so this only takes effect once splat transparency is actually turned on.
*/
int
shadow_ray_cull_mask()
{
	int mask = SHADOW_RAY_CULL_MASK;

	if (global_ubo.pt_blood_splat_alpha < 0.999)
		mask &= ~AS_FLAG_BLOOD;

	return mask;
}

bool
is_chrome(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_CHROME || kind == MATERIAL_KIND_CHROME_MODEL;
}

bool
is_sky(uint material)
{
	uint kind = material & MATERIAL_KIND_MASK;
	return kind == MATERIAL_KIND_SKY;
}

bool
is_screen(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_SCREEN;
}

bool
is_camera(uint material)
{
	return (material & MATERIAL_KIND_MASK) == MATERIAL_KIND_CAMERA;
}

vec3
correct_emissive(uint material_id, vec3 emissive)
{
	return max(vec3(0), emissive.rgb + vec3(EMISSIVE_TRANSFORM_BIAS));
}

void
trace_geometry_ray(Ray ray, bool cull_back_faces, int instance_mask)
{
	uint rayFlags = 0;
	if (cull_back_faces)
		rayFlags |= gl_RayFlagsCullBackFacingTrianglesEXT;
	rayFlags |= gl_RayFlagsSkipProceduralPrimitives;

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = 0;

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		switch (sbtOffset)
		{
		case SBTO_MASKED:
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
			break;
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry,
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT(topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask,
		SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
		ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif
}

float vmin(vec3 v) { return min(v.x, min(v.y, v.z)); }
float vmax(vec3 v) { return max(v.x, max(v.y, v.z)); }

// Loops over the defined fog volumes and finds the two closest ones along the ray.
// They are stored in the order of min distance in rp.fog1 (closer) and rp.fog2 (further away).
// If the ray starts in a fog volume, that volume will be rp.fog1 with t_min = ray.t_min.
void find_fog_volumes(inout RayPayloadEffects rp, Ray ray)
{
	vec3 inv_dir = vec3(1.0) / ray.direction;
	for (int i = 0; i < MAX_FOG_VOLUMES; i++)
	{
		const ShaderFogVolume volume = global_ubo.fog_volumes[i];

		if (volume.is_active == 0)
			return;

		vec3 t1 = (volume.mins - ray.origin) * inv_dir;
		vec3 t2 = (volume.maxs - ray.origin) * inv_dir;
		float t_in = vmax(min(t1, t2));
		float t_out = vmin(max(t1, t2));
		t_in = max(t_in, ray.t_min);
		t_out = min(t_out, ray.t_max);

		if (t_out > t_in)
		{
			vec2 first_t_min_max = unpackHalf2x16(rp.fog1.w);
			vec2 second_t_min_max = unpackHalf2x16(rp.fog2.w);

			bool replaces_first = t_in < first_t_min_max.x || first_t_min_max.y == 0;
			bool replaces_second = t_in < second_t_min_max.x || second_t_min_max.y == 0;

			if (replaces_first || replaces_second)
			{
				uvec4 packed;
				packed.xy = packHalf4x16(vec4(volume.color * global_ubo.pt_fog_brightness, 0));
				packed.z = packHalf2x16(vec2(t_in, t_out));

				// Convert the volumetric density function into a 1D function along the ray
				float density_variable = dot(volume.density.xyz, ray.direction) * 0.5;
				float density_constant = dot(volume.density.xyz, ray.origin) + volume.density.w;
				// Scale the density stored here because typical values are very small, in fp16 denormal range
				packed.w = packHalf2x16(vec2(density_variable, density_constant) * 65536.0);

				if (replaces_first)
				{
					// Push fog1 to fog2, replace fog1 with the new volume
					rp.fog2 = rp.fog1;
					rp.fog1 = packed;
				}
				else // if (replaces_second) -- must be true
				{
					// Replace fog2 with the new volume
					rp.fog2 = packed;
				}
			}
		}
	}
}

vec4
trace_effects_ray(Ray ray, bool skip_procedural, bool reflection_ray)
{
	uint rayFlags = 0;
	if (skip_procedural)
		rayFlags |= gl_RayFlagsSkipProceduralPrimitives;

	uint instance_mask = AS_FLAG_EFFECTS;

	ray_payload_effects.reflection_ray = reflection_ray ? 1 : 0;
	ray_payload_effects.transparency = uvec2(0);
	ray_payload_effects.distances = 0;
	ray_payload_effects.fog1 = uvec4(0);
	ray_payload_effects.fog2 = uvec4(0);
#ifndef KHR_RAY_QUERY
	ray_payload_effects.rayTmax = ray.t_max;
#endif

	if (!skip_procedural)
		find_fog_volumes(ray_payload_effects, ray);

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_EFFECTS], rayFlags, instance_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	// Start traversal: return false if traversal is complete
	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		float hitT = rayQueryGetIntersectionTEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		vec4 transparent = vec4(0);

		if (isProcedural)
		{
			if (!skip_procedural) // this should be a compile-time constant
			{
				// We only have one type of procedural primitives: beams.

				// Run the intersection shader first...
				float tShapeHit;
				vec2 beam_fade_and_thickness;
				bool intersectsWithBeam = pt_logic_beam_intersection(primitiveID,
					ray.origin, ray.direction, ray.t_min, ray.t_max,
					beam_fade_and_thickness, tShapeHit);

				// Then the any-hit shader.
				if (intersectsWithBeam)
				{
					transparent = pt_logic_beam(primitiveID, beam_fade_and_thickness, tShapeHit, ray.t_max);
					hitT = tShapeHit;
				}
			}
		}
		else
		{
			switch (sbtOffset)
			{
			case SBTO_PARTICLE: // particles
				transparent = pt_logic_particle(primitiveID, bary);
				break;

			case SBTO_EXPLOSION: // explosions
				transparent = pt_logic_explosion(primitiveID, instanceID, instanceCustomIndex, ray.direction, bary, reflection_ray);
				break;

			case SBTO_SPRITE: // sprites
				transparent = pt_logic_sprite(primitiveID, bary);
				break;
			}
		}

		if (transparent.a > 0)
		{
			update_payload_transparency(ray_payload_effects, transparent, hitT);
		}
	}

#else

	traceRayEXT(topLevelAS[TLAS_INDEX_EFFECTS], rayFlags, instance_mask,
		SBT_RCHIT_EFFECTS /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
		ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_EFFECTS);

#endif

	if (skip_procedural)
		return get_payload_transparency(ray_payload_effects);

	return get_payload_transparency_with_fog(ray_payload_effects, ray.t_max);
}

Ray get_shadow_ray(vec3 p1, vec3 p2, float tmin)
{
	vec3 l = p2 - p1;
	float dist = length(l);
	l /= dist;

	Ray ray;
	ray.origin = p1 + l * tmin;
	ray.t_min = 0;
	ray.t_max = dist - tmin - 0.01;
	ray.direction = l;

	return ray;
}

float
trace_shadow_ray(Ray ray, int cull_mask)
{
	const uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsSkipProceduralPrimitives;


#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, cull_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	while (rayQueryProceedEXT(rayQuery))
	{
		uint sbtOffset = rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(rayQuery, false);
		int primitiveID = rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, false);
		int instanceID = rayQueryGetIntersectionInstanceIdEXT(rayQuery, false);
		int geometryIndex = rayQueryGetIntersectionGeometryIndexEXT(rayQuery, false);
		uint instanceCustomIndex = rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, false);
		vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rayQuery, false);
		bool isProcedural = rayQueryGetIntersectionTypeEXT(rayQuery, false) == gl_RayQueryCandidateIntersectionAABBEXT;

		if (!isProcedural && sbtOffset == SBTO_MASKED)
		{
			if (pt_logic_masked(primitiveID, instanceID, geometryIndex, instanceCustomIndex, bary))
				rayQueryConfirmIntersectionEXT(rayQuery);
		}
	}

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT)
		return 0.0f;
	else
		return 1.0f;

#else

	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;

	traceRayEXT(topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, cull_mask,
		SBT_RCHIT_GEOMETRY /*sbtRecordOffset*/, 0 /*sbtRecordStride*/, SBT_RMISS_EMPTY /*missIndex*/,
		ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

	return found_intersection(ray_payload_geometry) ? 0.0 : 1.0;

#endif
}

vec3
trace_caustic_ray(Ray ray, int surface_medium)
{
	ray_payload_geometry.barycentric = vec2(0);
	ray_payload_geometry.primitive_id = ~0u;
	ray_payload_geometry.buffer_and_instance_idx = 0;
	ray_payload_geometry.hit_distance = -1;


	uint rayFlags = gl_RayFlagsCullBackFacingTrianglesEXT | gl_RayFlagsOpaqueEXT | gl_RayFlagsSkipProceduralPrimitives;
	uint instance_mask = AS_FLAG_TRANSPARENT;

#ifdef KHR_RAY_QUERY

	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask,
		ray.origin, ray.t_min, ray.direction, ray.t_max);

	rayQueryProceedEXT(rayQuery);

	if (rayQueryGetIntersectionTypeEXT(rayQuery, true) == gl_RayQueryCommittedIntersectionTriangleEXT)
	{
		pt_logic_rchit(ray_payload_geometry,
			rayQueryGetIntersectionPrimitiveIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceIdEXT(rayQuery, true),
			rayQueryGetIntersectionGeometryIndexEXT(rayQuery, true),
			rayQueryGetIntersectionInstanceCustomIndexEXT(rayQuery, true),
			rayQueryGetIntersectionTEXT(rayQuery, true),
			rayQueryGetIntersectionBarycentricsEXT(rayQuery, true));
	}

#else

	traceRayEXT(topLevelAS[TLAS_INDEX_GEOMETRY], rayFlags, instance_mask, SBT_RCHIT_GEOMETRY, 0, SBT_RMISS_EMPTY,
		ray.origin, ray.t_min, ray.direction, ray.t_max, RT_PAYLOAD_GEOMETRY);

#endif

	float extinction_distance = ray.t_max - ray.t_min;
	vec3 throughput = vec3(1);

	if (found_intersection(ray_payload_geometry))
	{
		Triangle triangle = get_hit_triangle(ray_payload_geometry);

		vec3 geo_normal = triangle.normals[0];
		bool is_vertical = abs(geo_normal.z) < 0.1;

		if ((is_water(triangle.material_id) || is_slime(triangle.material_id)) && !is_vertical)
		{
			vec3 position = ray.origin + ray.direction * ray_payload_geometry.hit_distance;
			vec3 w = get_water_normal(triangle.material_id, geo_normal, triangle.tangents[0], position, true);

			float caustic = clamp((1 - pow(clamp(1 - length(w.xz), 0, 1), 2)) * 100, 0, 8);
			caustic = mix(1, caustic, clamp(ray_payload_geometry.hit_distance * 0.02, 0, 1));
			throughput = vec3(caustic);

			if (surface_medium != MEDIUM_NONE)
			{
				extinction_distance = ray_payload_geometry.hit_distance;
			}
			else
			{
				if (is_water(triangle.material_id))
					surface_medium = MEDIUM_WATER;
				else
					surface_medium = MEDIUM_SLIME;

				extinction_distance = max(0, ray.t_max - ray_payload_geometry.hit_distance);
			}
		}
		else if (is_glass(triangle.material_id) || is_water(triangle.material_id) && is_vertical)
		{
			vec3 bary = get_hit_barycentric(ray_payload_geometry);
			vec2 tex_coord = triangle.tex_coords * bary;

			MaterialInfo minfo = get_material_info(triangle.material_id);

			vec3 base_color = vec3(minfo.base_factor);
			if (minfo.base_texture > 0)
				base_color *= global_textureLod(minfo.base_texture, tex_coord, 2).rgb;
			base_color = clamp(base_color, vec3(0), vec3(1));

			throughput = base_color;
		}
		else
		{
			throughput = vec3(clamp(1.0 - triangle.alpha, 0.0, 1.0));
		}
	}

	//return vec3(caustic);
	return extinction(surface_medium, extinction_distance) * throughput;
}

vec3 rgbToNormal(vec3 rgb, out float len)
{
	vec3 n = vec3(rgb.xy * 2 - 1, rgb.z);

	len = length(n);
	return len > 0 ? n / len : vec3(0);
}


float
AdjustRoughnessToksvig(float roughness, float normalMapLen, float mip_level)
{
	float effect = global_ubo.pt_toksvig * clamp(mip_level, 0, 1);
	float shininess = RoughnessSquareToSpecPower(roughness) * effect; // not squaring the roughness here - looks better this way
	float ft = normalMapLen / mix(shininess, 1.0f, normalMapLen);
	ft = max(ft, 0.01f);
	return SpecPowerToRoughnessSquare(ft * shininess / effect);
}

float
get_specular_sampled_lighting_weight(float roughness, vec3 N, vec3 V, vec3 L, float pdfw)
{
	float ggxVndfPdf = ImportanceSampleGGX_VNDF_PDF(max(roughness, 0.01), N, V, L);

	// Balance heuristic assuming one sample from each strategy: light sampling and BRDF sampling
	return clamp(pdfw / (pdfw + ggxVndfPdf), 0, 1);
}

float get_unshadowed_env_path_contrib(
	vec3 normal,
	vec3 view_direction,
	float phong_exp,
	float phong_scale,
	float phong_weight,
	vec2 rng)
{
	vec3 direction = global_ubo.sun_direction;
	float NoL = dot(direction, normal);
	if (NoL <= 0.0001) return 0.0;

	float specular = phong(normal, direction, view_direction, phong_exp) * phong_scale;
	float m = mix(1.0, specular, phong_weight);

	float light_lum = sun_color_ubo.sun_luminance;// / global_ubo.sun_solid_angle;

	m *= abs(light_lum); // abs because sky lights have negative color

	return m;
}

void
get_direct_illumination(
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	uint cluster_idx,
	uint material_id,
	int shadow_cull_mask,
	vec3 view_direction,
	vec3 albedo,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	float direct_specular_weight,
	bool enable_polygonal,
	bool enable_dynamic,
	bool is_gradient,
	int bounce,
	out vec3 diffuse,
	out vec3 specular)
{
	diffuse = vec3(0);
	specular = vec3(0);

	vec3 pos_on_light;

	vec3 contrib = vec3(0);

	float alpha = square(roughness);
	float phong_exp = RoughnessSquareToSpecPower(alpha);
	float phong_scale = min(100, 1 / (M_PI * square(alpha)));
	float phong_weight = clamp(specular_factor * luminance(base_reflectivity) / (luminance(base_reflectivity) + luminance(albedo)), 0, 0.9);

	int light_index = -1;
	float light_pdfw = 0;
	bool polygonal_light_is_sky = false;

	vec3 rng = vec3(
		get_rng(RNG_NEE_LIGHT_SELECTION(bounce)),
		get_rng(RNG_NEE_TRI_X(bounce)),
		get_rng(RNG_NEE_TRI_Y(bounce)));

	if (enable_polygonal || enable_dynamic)
	{
		sample_lights(
			cluster_idx,
			position,
			normal,
			geo_normal,
			view_direction,
			phong_exp,
			phong_scale,
			phong_weight,
			is_gradient,
			pos_on_light,
			contrib,
			light_index,
			light_pdfw,
			polygonal_light_is_sky,
			rng);
	}

	bool is_polygonal = true;
	float vis = 1.0;

	float spec_polygonal = phong(normal, normalize(pos_on_light - position), view_direction, phong_exp) * phong_scale;

	float l_polygonal = luminance(abs(contrib)) * mix(1, spec_polygonal, phong_weight);

	bool null_light = (l_polygonal == 0);

	pos_on_light = null_light ? position : pos_on_light;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, pos_on_light, 0);

	vis *= trace_shadow_ray(shadow_ray, null_light ? 0 : shadow_cull_mask);
#ifdef ENABLE_SHADOW_CAUSTICS
	if (enable_caustics)
	{
		contrib *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	/*
		Accumulate light shadowing statistics to guide importance sampling on the next frame.
		Inspired by paper called "Adaptive Shadow Testing for Ray Tracing" by G. Ward, EUROGRAPHICS 1994.

		The algorithm counts the shadowed and unshadowed rays towards each light, per cluster,
		per surface orientation in each cluster. Orientation helps improve accuracy in cases
		when a single cluster has different parts which have the same light mostly shadowed and
		mostly unshadowed.

		On the next frame, the light CDF is built using the counts from this frame, or the frame
		before that in case of gradient rays. See light_lists.h for more info.

		Only applies to polygonal polygon lights (i.e. no model or beam lights) because the spherical
		polygon lights do not have polygonal indices, and it would be difficult to map them
		between frames.
	*/
	if (global_ubo.pt_light_stats != 0
		&& !null_light
		&& light_index >= 0
		&& light_index < global_ubo.num_static_lights)
	{
		uint addr = get_light_stats_addr(cluster_idx, light_index, get_primary_direction(normal));

		// Offset 0 is unshadowed rays,
		// Offset 1 is shadowed rays
		if (vis == 0) addr += 1;

		// Increment the ray counter
		atomicAdd(light_stats_bufers[global_ubo.current_frame_idx % NUM_LIGHT_STATS_BUFFERS].stats[addr], 1);
	}

	if (null_light)
		return;

	vec3 radiance = vis * contrib;

	vec3 L = pos_on_light - position;
	L = normalize(L);

	if (direct_specular_weight > 0 && polygonal_light_is_sky && global_ubo.pt_specular_mis != 0)
	{
		// MIS with direct specular and indirect specular.
		// Only applied to sky lights, for two reasons:
		//  1) Non-sky lights are trimmed to match the light texture, and indirect rays don't see that;
		//  2) Non-sky lights are usually away from walls, so the direct sampling issue is not as pronounced.

		direct_specular_weight *= get_specular_sampled_lighting_weight(roughness,
			normal, -view_direction, L, light_pdfw);
	}

	vec3 F = vec3(0);

	if (vis > 0 && direct_specular_weight > 0)
	{
		vec3 specular_brdf = GGX_times_NdotL(view_direction, normalize(pos_on_light - position),
			normal, roughness, base_reflectivity, 0.0, specular_factor, F);
		specular = radiance * specular_brdf * direct_specular_weight;
	}

	float NdotL = max(0, dot(normal, L));

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}


void
get_sunlight(
	uint cluster_idx,
	uint material_id,
	vec3 position,
	vec3 normal,
	vec3 geo_normal,
	vec3 view_direction,
	vec3 base_reflectivity,
	float specular_factor,
	float roughness,
	int surface_medium,
	bool enable_caustics,
	out vec3 diffuse,
	out vec3 specular,
	int shadow_cull_mask)
{
	diffuse = vec3(0);
	specular = vec3(0);

	if (global_ubo.sun_visible == 0)
		return;

	bool visible = (cluster_idx == ~0u) || (light_buffer.sky_visibility[cluster_idx >> 5] & (1 << (cluster_idx & 31))) != 0;

	if (!visible)
		return;

	vec2 rng3 = vec2(get_rng(RNG_SUNLIGHT_X(0)), get_rng(RNG_SUNLIGHT_Y(0)));
	vec2 disk = sample_disk(rng3);
	disk.xy *= global_ubo.sun_tan_half_angle;

	vec3 direction = normalize(global_ubo.sun_direction + global_ubo.sun_tangent * disk.x + global_ubo.sun_bitangent * disk.y);

	float NdotL = dot(direction, normal);
	float GNdotL = dot(direction, geo_normal);

	if (NdotL <= 0 || GNdotL <= 0)
		return;

	Ray shadow_ray = get_shadow_ray(position - view_direction * 0.01, position + direction * 10000, 0);

	float vis = trace_shadow_ray(shadow_ray, shadow_cull_mask);

	if (vis == 0)
		return;

#ifdef ENABLE_SUN_SHAPE
	// Fetch the sun color from the environment map. 
	// This allows us to get properly shaped shadows from the sun that is partially occluded
	// by clouds or landscape.

	vec3 envmap_direction = (global_ubo.environment_rotation_matrix * vec4(direction, 0)).xyz;

	vec3 envmap = textureLod(TEX_PHYSICAL_SKY, envmap_direction.xzy, 0).rgb;

	vec3 radiance = (global_ubo.sun_solid_angle * global_ubo.pt_env_scale) * envmap;
#else
	// Fetch the average sun color from the resolved UBO - it's faster.

	vec3 radiance = sun_color_ubo.sun_color;
#endif

#ifdef ENABLE_SHADOW_CAUSTICS
	if (enable_caustics)
	{
		radiance *= trace_caustic_ray(shadow_ray, surface_medium);
	}
#endif

	vec3 F = vec3(0);

	if (global_ubo.pt_sun_specular > 0)
	{
		float NoH_offset = 0.5 * square(global_ubo.sun_tan_half_angle);
		vec3 specular_brdf = GGX_times_NdotL(view_direction, global_ubo.sun_direction,
			normal, roughness, base_reflectivity, NoH_offset, specular_factor, F);
		specular = radiance * specular_brdf;
	}

	float diffuse_brdf = NdotL / M_PI;
	diffuse = radiance * diffuse_brdf * (vec3(1.0) - F);
}

vec3 clamp_output(vec3 c)
{
	if (any(isnan(c)) || any(isinf(c)))
		return vec3(0);
	else
		return clamp(c, vec3(0), vec3(MAX_OUTPUT_VALUE));
}

vec3
sample_emissive_texture(uint material_id, MaterialInfo minfo, vec2 tex_coord, vec2 tex_coord_x, vec2 tex_coord_y, float mip_level)
{
	if (minfo.emissive_texture != 0)
	{
		vec4 image3;
		if (mip_level >= 0)
			image3 = global_textureLod(minfo.emissive_texture, tex_coord, mip_level);
		else
			image3 = global_textureGrad(minfo.emissive_texture, tex_coord, tex_coord_x, tex_coord_y);

		vec3 corrected = correct_emissive(material_id, image3.rgb);

		return corrected * minfo.emissive_factor;
	}

	return vec3(0);
}

vec3 get_emissive_shell(uint material_id)
{
	vec3 c = vec3(0);

	if ((material_id & (MATERIAL_FLAG_SHELL_RED | MATERIAL_FLAG_SHELL_GREEN | MATERIAL_FLAG_SHELL_BLUE)) != 0)
	{
		if ((material_id & MATERIAL_FLAG_SHELL_RED) != 0) c.r += 1;
		if ((material_id & MATERIAL_FLAG_SHELL_GREEN) != 0) c.g += 1;
		if ((material_id & MATERIAL_FLAG_SHELL_BLUE) != 0) c.b += 1;

		if ((material_id & MATERIAL_FLAG_WEAPON) != 0) c *= 0.2;
	}

	if (tonemap_buffer.adapted_luminance > 0)
		c.rgb *= tonemap_buffer.adapted_luminance * 100;

	return c;
}

bool get_is_gradient(ivec2 ipos)
{
	if (global_ubo.flt_enable != 0)
	{
		uint u = texelFetch(TEX_ASVGF_GRAD_SMPL_POS_A, ipos / GRAD_DWN, 0).r;

		ivec2 grad_strata_pos = ivec2(
			u >> (STRATUM_OFFSET_SHIFT * 0),
			u >> (STRATUM_OFFSET_SHIFT * 1)) & STRATUM_OFFSET_MASK;

		return (u > 0 && all(equal(grad_strata_pos, ipos % GRAD_DWN)));
	}

	return false;
}

/*
Parallax occlusion mapping, ported from RTX Remix
(dxvk-remix: rtx/concept/surface_material/opaque_surface_material_interaction.slangh).
Returns the texture coordinate where the view ray really meets the height field;
every map the material has is then sampled there.

pt_pom 1 is Remix's default QuadtreePOM (pomTraceRay): an exact walk through the
height map's texels, using the mip chain as a quadtree of maximum heights to skip
empty space. It needs max-filtered height mips, which the texture upload builds
for images flagged IF_HEIGHT_MAP. pt_pom 2 is Remix's RaymarchPOM: fixed layers,
sampled at mip 0. Both follow Remix line for line, so displace_in / displace_out
carry over unchanged.

Only the texture lookup moves. The hit position, depth and motion vectors stay on
the triangle, and shadow rays still see the flat surface.
*/

// Remix pomSampleHeight: depth below the top of the slab, sampled at mip 0.
float pom_sample_depth(uint height_texture, vec2 uv)
{
	return 1.0 - global_textureLod(height_texture, uv, 0).r;
}

// Remix pomGetBoxHeight: the maximum height inside one texel of a mip level. Sampling a
// texel centre at an exact lod returns that texel unfiltered.
float pom_box_height(uint height_texture, vec2 box_center, int level)
{
	return global_textureLod(height_texture, box_center, float(level)).r;
}

// Remix pomGetBoxSize: the size, in UV, of one texel of a mip level.
vec2 pom_box_size(int level, ivec2 full_size)
{
	return 1.0 / vec2(max(full_size >> level, ivec2(1)));
}

// Remix pomGetPatchCorners: each corner of the box takes the minimum of the four boxes
// around it, so neighbouring boxes share corners and the level-0 surface is watertight.
// Returned as the corners 00, 01, 10, 11 in the ray's forward frame.
vec4 pom_patch_corners(uint tex, vec2 box_center, vec2 box_step, float height, int level)
{
	float box00 = pom_box_height(tex, box_center + box_step * vec2(-1, -1), level);
	float box01 = pom_box_height(tex, box_center + box_step * vec2(-1,  0), level);
	float box02 = pom_box_height(tex, box_center + box_step * vec2(-1,  1), level);
	float box10 = pom_box_height(tex, box_center + box_step * vec2( 0, -1), level);
	float box12 = pom_box_height(tex, box_center + box_step * vec2( 0,  1), level);
	float box20 = pom_box_height(tex, box_center + box_step * vec2( 1, -1), level);
	float box21 = pom_box_height(tex, box_center + box_step * vec2( 1,  0), level);
	float box22 = pom_box_height(tex, box_center + box_step * vec2( 1,  1), level);

	return vec4(
		min(min(box00, box01), min(box10, height)),
		min(min(box01, box02), min(height, box12)),
		min(min(box10, height), min(box20, box21)),
		min(min(height, box12), min(box21, box22)));
}

// Remix pomTraceRay. origin.z = 1 is the top of the slab and z = 0 its floor; xy is UV.
vec2 pom_trace_quadtree(uint tex, vec3 origin, vec3 direction,
                        vec2 tex_coord_x, vec2 tex_coord_y, float mip_level, float neutral_height)
{
	ivec2 full_size = global_textureSize(tex, 0);
	int num_mips = global_textureQueryLevels(tex);
	bool outwards = direction.z > 0;

	int min_mip;
	if (mip_level >= 0)
		min_mip = int(mip_level);
	else
	{
		vec2 ddx_tex = tex_coord_x * vec2(full_size);
		vec2 ddy_tex = tex_coord_y * vec2(full_size);
		float dd_max_sq = max(dot(ddx_tex, ddx_tex), dot(ddy_tex, ddy_tex));
		min_mip = int(max(0.0, 0.5 * log2(max(dd_max_sq, 1e-20))));
	}
	min_mip = clamp(min_mip, 0, num_mips - 1);

	origin.xy += direction.xy * (1.0 - neutral_height) / direction.z;

	// Remix starts inward rays halfway down the quadtree, outward ones at the bottom.
	int level = outwards ? 0 : max((num_mips - 1) / 2, min_mip);
	vec3 cur_pos = origin;

	vec2 forward_step = vec2(direction.x >= 0 ? 1.0 : -1.0, direction.y >= 0 ? 1.0 : -1.0);
	vec2 forward_half_step = forward_step * 0.5;
	int max_iterations = int(clamp(global_ubo.pt_pom_max_steps, 4.0, 256.0));
	int iterations = 0;
	float prev_height = 1.0;

	while (level >= min_mip && iterations < max_iterations && (!outwards || cur_pos.z <= 1.0))
	{
		iterations++;
		vec2 box_size = pom_box_size(level, full_size);
		vec2 box_center = floor(cur_pos.xy / box_size) * box_size + 0.5 * box_size;
		float height = pom_box_height(tex, box_center, level);

		// Below the box's maximum height above the finest level: descend one level.
		if (level != min_mip && cur_pos.z <= height)
		{
			level--;
			continue;
		}

		// Where the ray leaves the box sideways, and where it meets the box's top.
		vec2 box_far_corner = box_center + forward_half_step * box_size;
		vec3 intersect_dist = (vec3(box_far_corner, height) - origin) / direction;
		float bounds_intersect = min(intersect_dist.x, intersect_dist.y);
		vec3 box_exit = origin + bounds_intersect * direction;

		if (!outwards && intersect_dist.z <= bounds_intersect)
		{
			if (level == 0)
			{
				// Intersect a bilinear patch through the box corners rather than the
				// flat top of the texel, so magnified height maps stay smooth.
				vec2 box_near_corner = box_center - forward_half_step * box_size;
				vec2 box_entrance_dist = (box_near_corner - origin.xy) / direction.xy;
				vec3 box_entrance = origin + max(0.0, max(box_entrance_dist.x, box_entrance_dist.y)) * direction;

				vec2 forward_box_step = box_size * forward_step;
				vec4 corners = pom_patch_corners(tex, box_center, forward_box_step, height, min_mip);
				iterations += 8;

				vec2 entrance_coord = (box_entrance.xy - box_near_corner) / forward_box_step;
				vec2 exit_coord = (box_exit.xy - box_near_corner) / forward_box_step;

				float entrance_height = mix(mix(corners[0], corners[2], entrance_coord.x),
				                            mix(corners[1], corners[3], entrance_coord.x), entrance_coord.y);
				float exit_height = mix(corners[2], mix(corners[1], corners[3], exit_coord.x), exit_coord.y);

				float frac = (box_entrance.z - entrance_height)
				           / ((exit_height - entrance_height) - (box_exit.z - box_entrance.z));
				vec3 intercept = box_entrance + (box_exit - box_entrance) * frac;

				if (entrance_height > box_entrance.z || (frac <= 1.0 && intercept.z <= height && frac >= 0.0))
					return intercept.xy;
				// Missed the patch: carry on into the next box.
			}
			else if (level == min_mip)
			{
				// Too far away for level 0: interpolate between the previous box's height
				// at the entrance and this box's at the exit.
				float frac = (cur_pos.z - prev_height) / ((height - prev_height) - (box_exit.z - cur_pos.z));
				return (cur_pos + (box_exit - cur_pos) * frac).xy;
			}
			else
			{
				// Drop onto the box's top and descend.
				cur_pos = origin + intersect_dist.z * direction;
				level--;
				continue;
			}
		}

		// Left the box through a side: step just past it, and climb a level if the next
		// box is in a different parent.
		int next_level = min(num_mips - 1, level + 1);
		vec3 next_pos = box_exit + direction * min(box_size.x, box_size.y) * 0.01;
		vec2 next_box_size = pom_box_size(next_level, full_size);
		if (any(notEqual(floor(cur_pos.xy / next_box_size), floor(next_pos.xy / next_box_size))))
			level = next_level;
		cur_pos = next_pos;
		prev_height = height;
	}

	// Outward rays can pass the top of the slab; stop them there.
	if (cur_pos.z > 1.0)
		return (origin + ((1.0 - origin.z) / direction.z) * direction).xy;

	return cur_pos.xy;
}

// Remix RaymarchPOM (the first branch of pomCalculateTexcoord).
vec2 pom_trace_raymarch(uint tex, vec2 tex_coord, vec3 view_dir, float total_height, float neutral_height)
{
	float max_samples = clamp(global_ubo.pt_pom_max_steps, 4.0, 256.0);
	float num_layers = mix(max_samples, max_samples * 0.25, abs(view_dir.z));
	float layer_size = 1.0 / num_layers;
	vec2 step = view_dir.xy / view_dir.z * total_height * layer_size;

	vec2 uv = tex_coord + step * num_layers * (1.0 - neutral_height);
	float curr_height = pom_sample_depth(tex, uv);

	float curr_depth = 0.0, prev_height = 0.0;
	int max_iterations = int(num_layers) + 2;
	for (int i = 0; i < max_iterations && curr_depth < curr_height; i++)
	{
		uv -= step;
		prev_height = curr_height;
		curr_height = pom_sample_depth(tex, uv);
		curr_depth += layer_size;
	}

	// Interpolate the last two samples to reduce sample aliasing.
	float depth_n_minus_1 = prev_height - curr_depth + layer_size;
	float depth_n = curr_height - curr_depth;
	float weight = clamp(depth_n / (depth_n - depth_n_minus_1), 0.0, 1.0);
	return uv + step * weight;
}

vec2 parallax_occlusion(Triangle triangle, MaterialInfo minfo, vec3 geo_normal, vec3 ray_direction,
                        vec2 tex_coord, vec2 tex_coord_x, vec2 tex_coord_y, float mip_level)
{
	float displace_in  = max(minfo.displace_in, 0.0) * global_ubo.pt_pom_scale;
	float displace_out = max(minfo.displace_out, 0.0) * global_ubo.pt_pom_scale;
	float total_height = displace_in + displace_out;
	if (total_height <= 0)
		return tex_coord;
	float neutral_height = displace_in / total_height;

	// Remix genTangSpace: the raw dP/du and dP/dv, both scaled by the length of the
	// longer one. The texture-space frame is (rawTangent, rawBitangent, normal), so a
	// displacement of 1 is one texture repeat along the longer axis, in world units.
	vec2 duv0 = triangle.tex_coords[1] - triangle.tex_coords[0];
	vec2 duv1 = triangle.tex_coords[2] - triangle.tex_coords[0];
	vec3 dp0  = triangle.positions[1] - triangle.positions[0];
	vec3 dp1  = triangle.positions[2] - triangle.positions[0];

	float det = duv0.x * duv1.y - duv1.x * duv0.y;
	if (abs(det) < 1e-10)
		return tex_coord;

	vec3 dPdu = (dp0 * duv1.y - dp1 * duv0.y) / det;
	vec3 dPdv = (dp1 * duv0.x - dp0 * duv1.x) / det;
	float longer = sqrt(max(dot(dPdu, dPdu), dot(dPdv, dPdv)));
	if (longer <= 0)
		return tex_coord;

	vec3 V = -ray_direction;
	vec3 N = geo_normal;
	if (dot(N, V) < 0)
		N = -N;

	// Remix: worldToTexture = inverse(transpose(mat3(rawTangent, rawBitangent, normal))).
	mat3 texture_to_world = mat3(dPdu / longer, dPdv / longer, N);
	if (abs(determinant(texture_to_world)) < 1e-6)
		return tex_coord;
	vec3 view_dir = normalize(inverse(texture_to_world) * V);
	if (view_dir.z < 1e-3)
		return tex_coord;

	vec2 result;
	if (global_ubo.pt_pom == 2)
	{
		result = pom_trace_raymarch(minfo.height_texture, tex_coord, view_dir, total_height, neutral_height);
	}
	else
	{
		// A ray exactly along a texture axis would divide 0 by 0 at the box planes.
		vec3 direction = -vec3(view_dir.xy, view_dir.z / total_height);
		if (abs(direction.x) < 1e-7) direction.x = 1e-7;
		if (abs(direction.y) < 1e-7) direction.y = 1e-7;
		result = pom_trace_quadtree(minfo.height_texture, vec3(tex_coord, 1.0), direction,
		                            tex_coord_x, tex_coord_y, mip_level, neutral_height);
	}

	// Remix WAR for REMIX-3709: POM sometimes produces NaNs.
	if (any(isnan(result)) || any(isinf(result)))
		return tex_coord;
	return result;
}

void
get_material(
	Triangle triangle,
	vec3 bary,
	vec2 tex_coord,
	vec2 tex_coord_x,
	vec2 tex_coord_y,
	float mip_level,
	vec3 geo_normal,
	vec3 ray_direction,
	out vec3 base_color,
	out vec3 normal,
	out float metallic,
	out float roughness,
	out vec3 emissive,
	out float specular_factor)
{
	// Blood droplets (cl_blood_spheres) are procedurally generated geometry with
	// no material entry, no albedo texture and no UV unwrap - which is exactly
	// why get_blood_normal() is driven by the geometric normal rather than by a
	// tangent basis. Their per-droplet colour rides in the otherwise dead UV
	// slots; see write_blood_geometry() in blood.c for the packing. Intercepted
	// here, ahead of every texture fetch below.
	if (is_blood(triangle.material_id))
	{
		base_color = clamp(vec3(triangle.tex_coords[0].x,
		                        triangle.tex_coords[0].y,
		                        triangle.tex_coords[1].x), vec3(0), vec3(1));
		// uv2.x carries "this droplet has landed" - see write_blood_geometry.
		bool blood_landed = triangle.tex_coords[2].x > 0.5;

		normal = get_blood_normal(geo_normal, triangle.tex_coords[1].y, blood_landed);

		/*
		A LANDED SPLAT IS A FILM, NOT A BEAD, AND IT IS THE ONE THAT MUST GO DARK.

		A droplet in flight is a lit sphere of blood and reads correctly as
		bright red; blood lying on a floor is a thin absorbing layer over a
		surface, and drawing it at the same saturated red is what makes a pool
		read as spilled paint. Only the landed branch is touched, so the spray
		keeps the look it has.

		The thickness is not stored anywhere - it is measured. For a splat,
		triangle.tangents carries the SURFACE PLANE NORMAL rather than a tangent
		(blood never reaches the tangent-space code; see the note in blood.c),
		and the puddle's dome runs from a rim whose normal lies in that plane to
		a body whose normal is along it. dot() of the two is therefore 0 at the
		feather edge and ~1 over the pool, with no new vertex channel, no
		interpolation of our own and no second ray.

		The dome's normals are steepened by the inverse of cl_blood_flatten, so
		that dot saturates within the outermost band of the disc - which is why
		pt_blood_thin_power exists and defaults above 1. It is what sets how far
		the dark edge reaches in, and it is the knob to reach for first.
		*/
		if (blood_landed)
		{
			float thickness = clamp(dot(geo_normal, triangle.tangents[0]), 0.0, 1.0);
			thickness = pow(thickness, max(0.01, global_ubo.pt_blood_thin_power));

			base_color *= mix(global_ubo.pt_blood_thin_dark,
			                  global_ubo.pt_blood_splat_dark,
			                  thickness);
		}

		metallic = 0;
		roughness = clamp(global_ubo.pt_blood_roughness, 0.0, 1.0);
		emissive = vec3(0);
		specular_factor = global_ubo.pt_blood_specular;
		return;
	}

	MaterialInfo minfo = get_material_info(triangle.material_id);

	perturb_tex_coord(triangle.material_id, triangle.texture_flags, global_ubo.time, tex_coord);

	// After the warp, so a height-mapped surface that also scrolls or warps
	// is displaced where it is drawn.
	if (global_ubo.pt_pom != 0 && minfo.height_texture != 0)
		tex_coord = parallax_occlusion(triangle, minfo, geo_normal, ray_direction,
		                               tex_coord, tex_coord_x, tex_coord_y, mip_level);

	vec4 image1 = vec4(1);
	if (minfo.base_texture != 0)
	{
		if (mip_level >= 0)
			image1 = global_textureLod(minfo.base_texture, tex_coord, mip_level);
		else
			image1 = global_textureGrad(minfo.base_texture, tex_coord, tex_coord_x, tex_coord_y);
	}

	base_color = image1.rgb * minfo.base_factor;
	base_color = clamp(base_color, vec3(0), vec3(1));

	normal = geo_normal;
	metallic = 0;
	roughness = 1;

	// Dedicated roughness / metallic maps, RTX-Remix style.
	//
	// The legacy packing hides roughness in the BASE texture's alpha and
	// metallic in the NORMAL map's alpha, which means neither can be authored
	// without also authoring a normal map, and that both have to be smuggled
	// through an alpha channel. When a material supplies texture_roughness or
	// texture_metallic that map wins, and each works on its own.
	//
	// With neither supplied the behaviour below is exactly as it was.
	bool have_roughness_tex = (minfo.roughness_texture != 0);
	bool have_metallic_tex = (minfo.metallic_texture != 0);

	if (have_roughness_tex)
	{
		if (mip_level >= 0)
			roughness = global_textureLod(minfo.roughness_texture, tex_coord, mip_level).r;
		else
			roughness = global_textureGrad(minfo.roughness_texture, tex_coord, tex_coord_x, tex_coord_y).r;
	}

	if (have_metallic_tex)
	{
		float metallic_sample;
		if (mip_level >= 0)
			metallic_sample = global_textureLod(minfo.metallic_texture, tex_coord, mip_level).r;
		else
			metallic_sample = global_textureGrad(minfo.metallic_texture, tex_coord, tex_coord_x, tex_coord_y).r;

		metallic = clamp(metallic_sample * minfo.metalness_factor, 0, 1);
	}

	if (minfo.normals_texture != 0)
	{
		vec4 image2;
		if (mip_level >= 0)
			image2 = global_textureLod(minfo.normals_texture, tex_coord, mip_level);
		else
			image2 = global_textureGrad(minfo.normals_texture, tex_coord, tex_coord_x, tex_coord_y);

		float normalMapLen;
		vec3 local_normal = rgbToNormal(image2.rgb, normalMapLen);

		if (dot(triangle.tangents[0], triangle.tangents[0]) > 0)
		{
			vec3 tangent = normalize(triangle.tangents * bary);

			// The bitangent's sign belongs to THIS triangle's UV winding, not to
			// the mesh.  A mirrored UV island winds the opposite way from its
			// neighbours, so shading it with the mesh-wide MATERIAL_FLAG_HANDEDNESS
			// bit (one bool per mesh, taken from whichever triangle happened to be
			// first - see compute_missing_model_tangents in vkpt/models.c) inverts
			// local_normal.y across the whole island and leaves a hard lighting
			// seam along its boundary.  62 of the 136 rerelease md5 models have at
			// least one mirrored island; the soldier's runs down the centre of his
			// helmet.  Everything needed to get it right per triangle is already in
			// the Triangle, so derive it here and ignore the flag.
			//
			// The 3D winding term is what makes this independent of whether a
			// loader reversed its index order relative to the shading normal, so
			// BSP, MD2 and MD5 geometry all land on the same answer.  A degenerate
			// triangle gives 0 and is left unflipped, matching the CPU code that
			// skipped those when accumulating the tangent.
			vec2 duv0 = triangle.tex_coords[1] - triangle.tex_coords[0];
			vec2 duv1 = triangle.tex_coords[2] - triangle.tex_coords[0];
			vec3 dp0  = triangle.positions[1] - triangle.positions[0];
			vec3 dp1  = triangle.positions[2] - triangle.positions[0];

			float uv_winding = duv0.x * duv1.y - duv1.x * duv0.y;
			float tri_facing = dot(cross(dp0, dp1), geo_normal);

			vec3 bitangent = cross(geo_normal, tangent);

			if (uv_winding * tri_facing < 0)
				bitangent = -bitangent;

			normal = tangent * local_normal.x + bitangent * local_normal.y + geo_normal * local_normal.z;

			float bump_scale = global_ubo.pt_bump_scale * minfo.bump_scale;
			if (is_glass(triangle.material_id))
				bump_scale *= 0.2;

			normal = normalize(mix(geo_normal, normal, bump_scale));
		}

		if (!have_metallic_tex)
			metallic = clamp(image2.a * minfo.metalness_factor, 0, 1);

		if (!have_roughness_tex)
			roughness = image1.a;

		if (minfo.roughness_override >= 0)
			roughness = max(roughness, minfo.roughness_override);

		roughness = clamp(roughness, 0, 1);

		float effective_mip = mip_level;

		if (effective_mip < 0)
		{
			ivec2 texSize = global_textureSize(minfo.normals_texture, 0);
			vec2 tx = tex_coord_x * texSize;
			vec2 ty = tex_coord_y * texSize;
			float d = max(dot(tx, tx), dot(ty, ty));
			effective_mip = 0.5 * log2(d);
		}

		bool is_mirror = (roughness < MAX_MIRROR_ROUGHNESS) && (is_chrome(triangle.material_id) || is_screen(triangle.material_id));

		if (normalMapLen > 0 && global_ubo.pt_toksvig > 0 && effective_mip > 0 && !is_mirror)
		{
			roughness = AdjustRoughnessToksvig(roughness, normalMapLen, effective_mip);
		}
	}
	else
	{
		// no normal map, so the block above did not run
		if (minfo.roughness_override >= 0)
			roughness = max(roughness, minfo.roughness_override);

		roughness = clamp(roughness, 0, 1);
	}

	if (global_ubo.pt_roughness_override >= 0) roughness = global_ubo.pt_roughness_override;
	if (global_ubo.pt_metallic_override >= 0) metallic = global_ubo.pt_metallic_override;

	// The specular factor parameter should only affect dielectrics, so make it 1.0 for metals
	specular_factor = mix(minfo.specular_factor, 1.0, metallic);

	if (triangle.emissive_factor > 0)
	{
		emissive = sample_emissive_texture(triangle.material_id, minfo, tex_coord, tex_coord_x, tex_coord_y, mip_level);
		emissive *= triangle.emissive_factor;
	}
	else
		emissive = vec3(0);

	emissive += get_emissive_shell(triangle.material_id) * base_color * (1 - metallic * 0.9);
}

bool get_camera_uv(vec2 tex_coord, out vec2 cameraUV)
{
	const vec2 minUV = vec2(11.0 / 256.0, 14.0 / 256.0);
	const vec2 maxUV = vec2(245.0 / 256.0, 148.0 / 256.0);

	tex_coord = fract(tex_coord);
	cameraUV = (tex_coord - minUV) / (maxUV - minUV);

	//vec2 resolution = vec2(7, 4) * 50;
	//cameraUV = (floor(cameraUV * resolution) + vec2(0.5)) / resolution;

	return all(greaterThan(cameraUV, vec2(0))) && all(lessThan(cameraUV, vec2(1)));
}

// Anisotropic texture sampling algorithm from 
// "Improved Shader and Texture Level of Detail Using Ray Cones"
// by T. Akenine-Moller et al., JCGT Vol. 10, No. 1, 2021.
// See section 5. Anisotropic Lookups.
void compute_anisotropic_texture_gradients(
	vec3 intersection,
	vec3 normal,
	vec3 ray_direction,
	float cone_radius,
	mat3 positions,
	mat3x2 tex_coords,
	vec2 tex_coords_at_intersection,
	out vec2 texGradient1,
	out vec2 texGradient2,
	out float fwidth_depth)
{
	// Compute ellipse axes.
	vec3 a1 = ray_direction - dot(normal, ray_direction) * normal;
	vec3 p1 = a1 - dot(ray_direction, a1) * ray_direction;
	a1 *= cone_radius / max(0.0001, length(p1));

	vec3 a2 = cross(normal, a1);
	vec3 p2 = a2 - dot(ray_direction, a2) * ray_direction;
	a2 *= cone_radius / max(0.0001, length(p2));

	// Compute texture coordinate gradients.
	vec3 eP, delta = intersection - positions[0];
	vec3 e1 = positions[1] - positions[0];
	vec3 e2 = positions[2] - positions[0];
	float inv_tri_area = 1.0 / dot(normal, cross(e1, e2));

	eP = delta + a1;
	float u1 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v1 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient1 = (1.0 - u1 - v1) * tex_coords[0] + u1 * tex_coords[1] +
		v1 * tex_coords[2] - tex_coords_at_intersection;

	eP = delta + a2;
	float u2 = dot(normal, cross(eP, e2)) * inv_tri_area;
	float v2 = dot(normal, cross(e1, eP)) * inv_tri_area;
	texGradient2 = (1.0 - u2 - v2) * tex_coords[0] + u2 * tex_coords[1] +
		v2 * tex_coords[2] - tex_coords_at_intersection;

	fwidth_depth = 1.0 / max(0.1, abs(dot(a1, ray_direction)) + abs(dot(a2, ray_direction)));
}