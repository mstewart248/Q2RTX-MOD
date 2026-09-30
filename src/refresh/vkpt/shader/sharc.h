/*
Copyright (C) 2026

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
SPATIAL HASH RADIANCE CACHE (pt_sharc)

A world-space cache of the light arriving at a surface, keyed by a hash of the
quantized position and normal, in the spirit of NVIDIA's SHaRC. What it buys is
multi-bounce indirect light: the last bounce of the path tracer stops by reading
the cache instead of ending in darkness, and the cache is trained by paths that
themselves stop by reading the cache, so every frame folds one more bounce in.

Three passes per frame, all driven from vkpt_pt_sharc_update():

  sharc_update.rgen    Traces a short diffuse path from one random pixel in every
                       pt_sharc_stride^2 block of the G-buffer, lights every vertex
                       (NEE + sun + emission), ends the path with a cache lookup,
                       and adds what each vertex received into its cache entry.
  sharc_resolve.comp   Folds this frame's sums into the running average and evicts
                       entries nothing has visited for pt_sharc_stale frames.
  indirect_lighting    At the last bounce hit, adds albedo * cache instead of
                       stopping (see the pt_sharc block there).

WHAT AN ENTRY HOLDS. Two view-independent quantities, both without the albedo of
the surface they belong to, so the texture detail comes from the surface that
queries them:

  indirect   the mean radiance arriving over the cosine lobe, EXCLUDING what NEE
             samples at that point (analytic lights). Multiply by albedo to get
             the outgoing diffuse indirect light.
  direct     NEE at that point with the bounce light settings, sun excluded
             (the path tracer evaluates the sun at every bounce hit itself).
             Only the second bounce needs it: indirect_lighting.rgen does no
             NEE there, so that light was simply missing.

Never used in accumulation (photo) mode - it is biased, and that mode is the
unbiased reference.
*/

#ifndef SHARC_H_
#define SHARC_H_

#define SHARC_PROBES             8
#define SHARC_MAX_FRAME_SAMPLES  256u
// Largest fixed-point value one sample may add. 256 of them fit in 32 bits.
#define SHARC_MAX_FIXED_SAMPLE   16777215.0

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = SHARC_KEY_BUFFER_BINDING_IDX) buffer SHARC_KEY_BUFFER {
	uint sharc_keys[];
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = SHARC_ACCUM_BUFFER_BINDING_IDX) buffer SHARC_ACCUM_BUFFER {
	uint sharc_accum[];
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = SHARC_RESOLVED_BUFFER_BINDING_IDX) buffer SHARC_RESOLVED_BUFFER {
	vec4 sharc_resolved[];
};

bool sharc_enabled()
{
	return global_ubo.pt_sharc != 0 && global_ubo.temporal_blend_factor == 0;
}

uint sharc_pcg(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint sharc_hash(uvec4 v, uint seed)
{
	uint h = sharc_pcg(v.x ^ seed);
	h = sharc_pcg(h ^ v.y);
	h = sharc_pcg(h ^ v.z);
	return sharc_pcg(h ^ v.w);
}

/* Accumulation is in fixed point (no float atomics on the device), scaled by the
   exposure so the precision follows the picture: 1/1024 of the adapted luminance
   per unit, with stochastic rounding so small values are not biased down. */
float sharc_fixed_scale()
{
	return 1024.0 / max(global_ubo.prev_adapted_luminance, 1e-5);
}

// Six bins, dominant axis and its sign, so the two faces of a wall never share an entry.
uint sharc_normal_bin(vec3 n)
{
	vec3 a = abs(n);
	uint axis = (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
	return axis * 2u + (n[axis] < 0 ? 1u : 0u);
}

/* Voxel size grows with camera distance so a voxel covers roughly
   pt_sharc_voxel_px pixels wherever it is, in power-of-two steps from
   pt_sharc_voxel_min world units. */
float sharc_voxel_size(vec3 position, out uint level)
{
	float dist = max(distance(position, global_ubo.cam_pos.xyz), 1.0);
	// abs: P[1][1] is negative here (Vulkan Y flip, create_projection_matrix)
	float pixel_angle = 2.0 / (abs(global_ubo.P[1][1]) * float(global_ubo.height));
	float voxel_min = max(global_ubo.pt_sharc_voxel_min, 0.25);
	float target = dist * pixel_angle * global_ubo.pt_sharc_voxel_px;
	level = uint(clamp(ceil(log2(max(target / voxel_min, 1.0))), 0.0, 15.0));
	return voxel_min * exp2(float(level));
}

void sharc_key(vec3 position, vec3 normal, out uint slot, out uint fingerprint)
{
	uint level;
	float size = sharc_voxel_size(position, level);

	ivec3 cell = ivec3(floor(position / size));
	uvec4 k = uvec4(uvec3(cell), level | (sharc_normal_bin(normal) << 4));

	slot = sharc_hash(k, 0x5bd1e995u) & (SHARC_CAPACITY - 1);
	fingerprint = sharc_hash(k, 0x27d4eb2fu);
	fingerprint = fingerprint == 0u ? 1u : fingerprint; // 0 marks an empty slot
}

int sharc_find(vec3 position, vec3 normal)
{
	uint slot, fingerprint;
	sharc_key(position, normal, slot, fingerprint);

	for (uint i = 0; i < SHARC_PROBES; i++)
	{
		uint s = (slot + i) & (SHARC_CAPACITY - 1);
		if (sharc_keys[s] == fingerprint)
			return int(s);
	}

	return -1;
}

int sharc_insert(vec3 position, vec3 normal)
{
	uint slot, fingerprint;
	sharc_key(position, normal, slot, fingerprint);

	for (uint i = 0; i < SHARC_PROBES; i++)
	{
		uint s = (slot + i) & (SHARC_CAPACITY - 1);
		uint prev = atomicCompSwap(sharc_keys[s], 0u, fingerprint);
		if (prev == 0u || prev == fingerprint)
			return int(s);
	}

	return -1; // bucket full - drop the sample
}

/* Reads a resolved entry. False when there is none, or it has fewer than
   pt_sharc_min_samples behind it - the caller then carries on without it,
   which is exactly the renderer's behaviour before the cache existed. */
bool sharc_lookup(vec3 position, vec3 normal, out vec3 indirect, out vec3 direct)
{
	indirect = vec3(0);
	direct = vec3(0);

	int slot = sharc_find(position, normal);
	if (slot < 0)
		return false;

	vec4 ind = sharc_resolved[slot * SHARC_RESOLVED_VEC4S + 0];
	if (ind.w < max(global_ubo.pt_sharc_min_samples, 1.0))
		return false;

	indirect = ind.rgb;
	direct = sharc_resolved[slot * SHARC_RESOLVED_VEC4S + 1].rgb;
	return true;
}

/* The path tracer's lookup at its last bounce hit. A cache entry is one average per
   voxel, and two things keep that from showing:

   - The lookup point is jittered along the surface by up to pt_sharc_jitter voxels,
     so the edge between two voxels becomes noise that the denoiser removes instead
     of a visible block.
   - A hit closer than pt_sharc_min_dist voxels is not trusted, fading to zero at
     contact. In a corner or crevice the voxel averages lit and shadowed surfaces
     together, and a short bounce ray reading it is how light leaks into corners.
     SHaRC proper keeps tracing there; the last bounce cannot, so that light is lost
     instead - dark is the right way to be wrong in a corner. */
bool sharc_lookup_bounce(vec3 position, vec3 normal, float hit_distance, uint seed, out vec3 indirect, out vec3 direct)
{
	indirect = vec3(0);
	direct = vec3(0);

	uint level;
	float size = sharc_voxel_size(position, level);

	float trust = 1.0;
	if (global_ubo.pt_sharc_min_dist > 0)
		trust = clamp(hit_distance / (size * global_ubo.pt_sharc_min_dist), 0.0, 1.0);
	if (trust <= 0)
		return false;

	vec3 tangent = normalize(cross(normal, abs(normal.x) > 0.5 ? vec3(0, 1, 0) : vec3(1, 0, 0)));
	vec3 bitangent = cross(normal, tangent);
	uint h1 = sharc_pcg(seed);
	uint h2 = sharc_pcg(h1);
	vec2 jitter = vec2(float(h1 >> 8), float(h2 >> 8)) * (1.0 / 16777216.0) - 0.5;
	position += (tangent * jitter.x + bitangent * jitter.y) * size * global_ubo.pt_sharc_jitter;

	if (!sharc_lookup(position, normal, indirect, direct))
		return false;

	indirect *= trust;
	direct *= trust;
	return true;
}

uvec3 sharc_quantize(vec3 v, float scale, float rnd)
{
	v = v * scale;
	if (any(isnan(v)) || any(isinf(v)))
		return uvec3(0);
	return uvec3(clamp(floor(v + rnd), vec3(0), vec3(SHARC_MAX_FIXED_SAMPLE)));
}

void sharc_accumulate(int slot, vec3 indirect, vec3 direct, float rnd)
{
	if (slot < 0)
		return;

	uint base = uint(slot) * SHARC_ACCUM_UINTS;

	// Past the cap the 32-bit sums could overflow; the resolve clamps the count to match.
	uint n = atomicAdd(sharc_accum[base + 3], 1u);
	if (n >= SHARC_MAX_FRAME_SAMPLES)
		return;

	float scale = sharc_fixed_scale();
	uvec3 qi = sharc_quantize(indirect, scale, rnd);
	uvec3 qd = sharc_quantize(direct, scale, rnd);

	if (qi.x != 0) atomicAdd(sharc_accum[base + 0], qi.x);
	if (qi.y != 0) atomicAdd(sharc_accum[base + 1], qi.y);
	if (qi.z != 0) atomicAdd(sharc_accum[base + 2], qi.z);
	if (qd.x != 0) atomicAdd(sharc_accum[base + 4], qd.x);
	if (qd.y != 0) atomicAdd(sharc_accum[base + 5], qd.y);
	if (qd.z != 0) atomicAdd(sharc_accum[base + 6], qd.z);
}

#endif // SHARC_H_
