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
PHOTON-TRACED CAUSTICS (pt_caustics 2)

Matt, 2026-11-18: "lets build the actual caustic simulation code". pt_caustics 1
never traces light through glass: the shadow ray goes straight and a brightness
factor is multiplied in (glass_caustic, glass_ball_caustic, glass_ripple in
path_tracer_rgen.h). Mode 2 traces it.

Per frame:

  caustics.c           Lists every (light, caustic glass) pair in range as an
                       EMITTER - the glass's bounding sphere, the light, a share of
                       pt_caustics_photons - in LightBuffer.caustic_emitters,
                       sorted by light. Caustic glass = kind GLASS with `caustic`
                       or `caustic_ripple` above 0.
  caustic_photons.rgen Shoots each emitter's photons from its light at its glass
                       (the whole sphere round when the light is inside it),
                       refracts and reflects them through every caustic glass
                       surface they meet (Snell at the interpolated vertex
                       normals, IOR 1 + 0.52 * caustic, Fresnel, total internal
                       reflection), and adds each one's light where it lands on
                       the first ordinary surface, into a world-space hash grid.
  caustic_resolve.comp Folds the frame's sums into a running mean per cell.
  direct_lighting.rgen Adds the cell's light to the first surface's diffuse.

and the light those photons carry is taken OUT of the shadow rays: where a
shadow ray toward a listed light crosses caustic glass inside one of that light's
emitter spheres, its diffuse is 0 (trace_caustic_ray, photon_owned). Its
specular keeps the pt_caustics 1 factor - a highlight is not a caustic, and the
grid has no direction to give one. A pair NOT on the list (out of range, over
MAX_CAUSTIC_EMITTERS, the sun, a sky portal) keeps pt_caustics 1 entirely, which
is what keeps the two halves from counting a light twice or not at all.

UNITS. A cell holds what direct_lighting's high_freq holds: E / pi, the
irradiance over pi (a Lambertian surface's outgoing radiance once albedo
multiplies it). A photon carrying flux F into a cell of area A adds F / (pi A).
The lights' flux is worked out from exactly what NEE gives them
(sample_lights in light_lists.h) - see caustic_photons.rgen.

WHAT IT CANNOT DO (round 1): the sun and sky through glass, water caustics (both
stay on pt_caustics 1), caustics seen by bounce rays, caustic light bouncing on.
*/

#ifndef CAUSTICS_H_
#define CAUSTICS_H_

#define CAUSTIC_PROBES             8
// Largest fixed-point value one photon may add to a channel.
#define CAUSTIC_MAX_FIXED_SAMPLE   16777215.0

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = CAUSTIC_KEY_BUFFER_BINDING_IDX) buffer CAUSTIC_KEY_BUFFER {
	uint caustic_keys[];
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = CAUSTIC_ACCUM_BUFFER_BINDING_IDX) buffer CAUSTIC_ACCUM_BUFFER {
	uint caustic_accum[];
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = CAUSTIC_RESOLVED_BUFFER_BINDING_IDX) buffer CAUSTIC_RESOLVED_BUFFER {
	vec4 caustic_resolved[];
};

// caustics.c sets it: pt_caustics 2, a world, and the buffers there
bool caustics_photons_active()
{
	return light_buffer.caustic_header.z != 0u;
}

uint caustic_pcg(uint v)
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint caustic_hash(uvec4 v, uint seed)
{
	uint h = caustic_pcg(v.x ^ seed);
	h = caustic_pcg(h ^ v.y);
	h = caustic_pcg(h ^ v.z);
	return caustic_pcg(h ^ v.w);
}

/* Fixed point (no float atomics on every device), relative to the exposure as
   SHaRC's is: 1/1024 of the adapted luminance per unit. */
float caustic_fixed_scale()
{
	return 1024.0 / max(global_ubo.prev_adapted_luminance, 1e-5);
}

// Six bins, dominant axis and its sign, so the two faces of a wall never share a cell.
uint caustic_normal_bin(vec3 n)
{
	vec3 a = abs(n);
	uint axis = (a.x >= a.y && a.x >= a.z) ? 0u : (a.y >= a.z ? 1u : 2u);
	// not n[axis]: a runtime index into a vector put 112 B of direct_lighting in
	// local memory on NVIDIA (11-18, spvstats - see shader-local-memory notes)
	float c = (axis == 0u) ? n.x : (axis == 1u ? n.y : n.z);
	return axis * 2u + (c < 0.0 ? 1u : 0u);
}

/* LEVELS. Ten cell sizes, the same everywhere in the world: pt_caustics_cell_min
   (level 0) doubling up to 512x that (level 9, 256 units at the default 0.5).
   Every photon goes into the level-9 cell it lands in, then finer ones while the
   photons allow (caustic_deposit), and a lookup reads the finest that has
   gathered enough (caustic_lookup) - a box-kernel density estimate whose width
   follows the photon density: sharp where a caustic focuses or near the lamp,
   wide and smooth where the light is thin.

   Why levels (Matt, 2026-11-18, image 38: white dots all over the floor): a lamp
   inside a bulb hands ALL its diffuse light to the photons, and far from the lamp,
   near the camera, they arrived at about one per 250 finest cells over the whole
   averaging window - a cell was 0 or one photon's worth, a dot.

   Why the same sizes everywhere (Matt, same day: "still really noisy when the
   player moves"): the sizes used to start from the camera's pixel size at the
   point, so walking moved every point onto other cells. Only the FINEST level a
   lookup may use, and the finest a photon may make, follow the camera now:
   about pt_caustics_cell_px pixels (caustic_camera_level). */
#define CAUSTIC_LEVELS      10
#define CAUSTIC_TOP_LEVEL   9

float caustic_cell_min()
{
	return max(global_ubo.pt_caustics_cell_min, 0.125);
}

float caustic_level_size(int level)
{
	return caustic_cell_min() * exp2(float(level));
}

// The level whose cells span about pt_caustics_cell_px pixels at this point, as a
// fraction: 2.3 = a bit coarser than level 2. 0 up to CAUSTIC_TOP_LEVEL.
float caustic_camera_level(vec3 position)
{
	float dist = max(distance(position, global_ubo.cam_pos.xyz), 1.0);
	// abs: P[1][1] is negative here (Vulkan Y flip, create_projection_matrix)
	float pixel_angle = 2.0 / (abs(global_ubo.P[1][1]) * float(global_ubo.height));
	float target = dist * pixel_angle * max(global_ubo.pt_caustics_cell_px, 0.25);
	return clamp(log2(max(target / caustic_cell_min(), 1.0)), 0.0, float(CAUSTIC_TOP_LEVEL));
}

uint caustic_cell_key(ivec3 cell, int level, uint bin, out uint fingerprint)
{
	uvec4 k = uvec4(uvec3(cell), uint(level) | (bin << 4));
	fingerprint = caustic_hash(k, 0x297a2d39u);
	fingerprint = fingerprint == 0u ? 1u : fingerprint; // 0 marks an empty slot
	return caustic_hash(k, 0x2c1b3c6du) & (CAUSTIC_CAPACITY - 1);
}

ivec3 caustic_cell_of(vec3 position, int level)
{
	return ivec3(floor(position / caustic_level_size(level)));
}

/* The mean area of a plane with this normal inside one cell. A plane crosses a
   cube of side s at offsets spread over s (|nx| + |ny| + |nz|), and the volume
   is s^3, so on average it covers s^2 / (|nx| + |ny| + |nz|) of it - s^2 for a
   floor or a wall, 0.58 s^2 on a 45-degree slope both ways. */
float caustic_cell_area(float size, vec3 normal)
{
	vec3 a = abs(normal);
	return size * size / max(a.x + a.y + a.z, 1.0);
}

int caustic_find_cell(ivec3 cell, int level, uint bin)
{
	uint fingerprint;
	uint slot = caustic_cell_key(cell, level, bin, fingerprint);

	for (uint i = 0; i < CAUSTIC_PROBES; i++)
	{
		uint s = (slot + i) & (CAUSTIC_CAPACITY - 1);
		// no early out at an empty slot: the resolve frees cells, which leaves holes
		if (caustic_keys[s] == fingerprint)
			return int(s);
	}

	return -1;
}

// How many frames a cell's mean reaches back: pt_caustics_frames, or all of them
// in photo mode (the camera holds still there and the mean never stops).
float caustic_frame_limit()
{
	return (global_ubo.temporal_blend_factor != 0.0) ? 1e9 : max(global_ubo.pt_caustics_frames, 1.0);
}

// The photons behind a cell's mean: photons per frame times the frames it covers.
float caustic_cell_photons(int slot)
{
	vec4 mean = caustic_resolved[slot * CAUSTIC_RESOLVED_VEC4S];
	float per_frame = caustic_resolved[slot * CAUSTIC_RESOLVED_VEC4S + 1].x;
	return per_frame * min(mean.w, caustic_frame_limit());
}

/* One photon landing at `position`: flux / pi, divided by each cell's area as it
   goes in. Level 9 first, then finer. A finer cell is MADE only when the one
   around it has gathered twice pt_caustics_min_photons (by the last resolve), as
   its quarter of them is then about what a lookup asks of it, and never finer than
   one level below what the camera can use there - so thin light never makes fine
   cells nobody will read. A finer cell that already exists always takes the
   photon: if it missed the frames its parent dipped under the mark, it would read
   darker than it is. */
void caustic_deposit(vec3 position, vec3 normal, vec3 flux_over_pi, float rnd)
{
	if (any(isnan(flux_over_pi)) || any(isinf(flux_over_pi)))
		return;

	uint bin = caustic_normal_bin(normal);
	int finest = max(int(floor(caustic_camera_level(position))) - 1, 0);
	float refine_at = 2.0 * max(global_ubo.pt_caustics_min_photons, 1.0);
	float scale = caustic_fixed_scale();
	bool may_create = true;

	for (int level = CAUSTIC_TOP_LEVEL; level >= finest; level--)
	{
		ivec3 cell = caustic_cell_of(position, level);
		int found = -1;
		if (may_create)
		{
			uint fingerprint;
			uint slot = caustic_cell_key(cell, level, bin, fingerprint);
			for (uint i = 0; i < CAUSTIC_PROBES; i++)
			{
				uint s = (slot + i) & (CAUSTIC_CAPACITY - 1);
				uint prev = atomicCompSwap(caustic_keys[s], 0u, fingerprint);
				if (prev == 0u || prev == fingerprint)
				{
					found = int(s);
					break;
				}
			}
		}
		else
		{
			found = caustic_find_cell(cell, level, bin);
		}
		if (found < 0)
			return; // not made (or the bucket is full): the coarser cells have the photon

		float size = caustic_level_size(level);
		vec3 v = flux_over_pi * (scale / caustic_cell_area(size, normal));
		uvec3 q = uvec3(clamp(floor(v + rnd), vec3(0), vec3(CAUSTIC_MAX_FIXED_SAMPLE)));

		uint base_index = uint(found) * CAUSTIC_ACCUM_UINTS;
		if (q.r != 0u) atomicAdd(caustic_accum[base_index + 0], q.r);
		if (q.g != 0u) atomicAdd(caustic_accum[base_index + 1], q.g);
		if (q.b != 0u) atomicAdd(caustic_accum[base_index + 2], q.b);
		atomicAdd(caustic_accum[base_index + 3], 1u);

		may_create = caustic_cell_photons(found) >= refine_at;
	}
}

/* One level's light at a point, blended between the four cells round it in the
   surface's plane (the two axes that are not the normal's; along the normal it
   stays in the point's own cell). rgb = light, w = photons behind it. A missing
   cell leaves the blend instead of counting as dark - next to cells that exist it
   is one that has not been made yet far more often than a shadow. */
vec4 caustic_level_light(vec3 position, int level, uint bin, vec3 axis_n, vec3 axis_t, vec3 axis_b)
{
	vec3 g = position / caustic_level_size(level);
	vec3 lo = floor(g - 0.5);
	vec3 f = g - 0.5 - lo;
	// the cell along the normal is the point's own; the other two start at lo
	vec3 start = axis_n * floor(g) + (vec3(1.0) - axis_n) * lo;
	float ft = dot(f, axis_t);
	float fb = dot(f, axis_b);

	vec4 sum = vec4(0);
	float weight_sum = 0.0;
	for (int i = 0; i < 4; i++)
	{
		float it = float(i & 1);
		float ib = float(i >> 1);
		float weight = mix(1.0 - ft, ft, it) * mix(1.0 - fb, fb, ib);
		int slot = caustic_find_cell(ivec3(start + axis_t * it + axis_b * ib), level, bin);
		if (slot >= 0 && weight > 0.0)
		{
			sum += weight * vec4(caustic_resolved[slot * CAUSTIC_RESOLVED_VEC4S].rgb, caustic_cell_photons(slot));
			weight_sum += weight;
		}
	}

	return weight_sum > 0.0 ? sum / weight_sum : vec4(0);
}

/* The caustic light at a first surface: E / pi, to add to its diffuse. Nothing in
   it is random, so the same spot reads the same light from any view.

   The level: from 9 down, while the point's own cell exists and (below 9) holds at
   least half pt_caustics_min_photons, never finer than the camera can use. Then
   that level blended with the one above: all the coarser one at half
   pt_caustics_min_photons, all the finer at the full number - so a change of size
   has no edge - and the camera's finest level fades in by how much finer than a
   pixel it is, so walking toward a caustic sharpens it smoothly. */
vec3 caustic_lookup(vec3 position, vec3 normal)
{
	if (!caustics_photons_active())
		return vec3(0);

	uint bin = caustic_normal_bin(normal);
	float camera_level = caustic_camera_level(position);
	int finest = int(floor(camera_level));
	float need = max(global_ubo.pt_caustics_min_photons, 1.0);

	int chosen = -1;
	int chosen_slot = -1;
	for (int level = CAUSTIC_TOP_LEVEL; level >= finest; level--)
	{
		int slot = caustic_find_cell(caustic_cell_of(position, level), level, bin);
		if (slot < 0)
			break;
		// level 9 is the best there is, however few it holds
		if (chosen >= 0 && caustic_cell_photons(slot) < 0.5 * need)
			break;
		chosen = level;
		chosen_slot = slot;
	}

	if (chosen < 0)
		return vec3(0);

	if (global_ubo.pt_caustics_smooth == 0)
		return max(caustic_resolved[chosen_slot * CAUSTIC_RESOLVED_VEC4S].rgb, vec3(0));

	// the surface's axes, picked without indexing a vector by a runtime value
	// (that put 112 B of direct_lighting in local memory - see caustic_normal_bin)
	vec3 a = abs(normal);
	vec3 axis_n = (a.x >= a.y && a.x >= a.z) ? vec3(1, 0, 0) : (a.y >= a.z ? vec3(0, 1, 0) : vec3(0, 0, 1));
	vec3 axis_t = axis_n.x > 0.0 ? vec3(0, 1, 0) : vec3(1, 0, 0);
	vec3 axis_b = axis_n.z > 0.0 ? vec3(0, 1, 0) : vec3(0, 0, 1);

	vec4 fine = caustic_level_light(position, chosen, bin, axis_n, axis_t, axis_b);
	if (chosen == CAUSTIC_TOP_LEVEL)
		return max(fine.rgb, vec3(0));

	vec4 coarse = caustic_level_light(position, chosen + 1, bin, axis_n, axis_t, axis_b);
	float w = clamp(fine.w / (0.5 * need) - 1.0, 0.0, 1.0);
	if (chosen == finest)
		w *= 1.0 - fract(camera_level);

	return max(mix(coarse.rgb, fine.rgb, w), vec3(0));
}

#endif // CAUSTICS_H_
