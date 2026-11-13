/*
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
NVIDIA Flow's fire, plasma, bubbles and rail trails, marched along a primary or reflection
ray and laid over the effects overlay (see flow.c for where the volume comes
from, flow_common.glsl for how it is found). The march follows Flow's
NvFlowRayMarchBlock; a step looks each layer up at its block on its grid -
the main one, the railgun trail's finer one, or the one its far part is on
(FLOW_GRIDS) - and adds them together.

Shading is an emissive, scattering medium. Smoke absorbs - a step of length
ds with smoke s takes 1 - exp(-s * density * ds) of what is behind it - and
puts back that much of its own colour: a flat, unlit colour, plus what it
scatters of the light the fire and plasma cast (flow_lights.comp, flow.c),
darkened by Flow's shadow toward the brightest of them where flow.c runs
that, and of the world's own light round it (flow_ambient.comp), where that
is on - then the flat colour is only what stands in for light bounced off
the walls, which the probes do not see. Heat glows through a colour map at so much per unit of length,
whether or not there is smoke in front of it to hide it, so thin smoke does
not mean dim fire. Colours are relative to the adapted luminance, the way
the particles and explosion sprites are, so they read the same under any
exposure; the lights are absolute, like any other light. Needs
path_tracer_rgen.h first.
*/

#ifndef FLOW_GLSL_
#define FLOW_GLSL_

#include "flow_common.glsl"

// Bounce 3 never samples the sun, so this slot is free in every ray-gen
// shader that marches the volume.
#define RNG_FLOW_JITTER RNG_SUNLIGHT_Y(3)

// pt_flow_stats: count what the primary march does (readback.flow_*, printed
// by vkpt_flow_stats). A diagnostic - its counters cost registers in the
// march loop, so it is compiled out unless this is 1.
#ifndef FLOW_STATS
#define FLOW_STATS 1
#endif
#if FLOW_STATS
bool flow_stat_ray = false;
uint flow_stat_iterations = 0u;
uint flow_stat_samples = 0u;
uint flow_stat_blocks = 0u;
uint flow_stat_seen = 0u;
uint flow_stat_faint = 0u;
#endif

// Which bricks of each block can show anything, see flow_occupancy.comp.
layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_FLOW_OCCUPANCY, std430)
readonly buffer FLOW_OCCUPANCY_BUFFER { uvec4 flow_occupancy[]; };

// Whether group `group` of grid `grid`'s blocks has anything to see, or
// shares its bit with one that has (flow_coarse_bit).
bool flow_coarse_marked(ivec3 group, int grid)
{
	const uint bit = flow_coarse_bit(group, grid);
	const uvec4 words = flow_occupancy[flow_params.limits.z + (bit >> 7u)];
	// Not words[(bit >> 5u) & 3u]: a vector indexed at run time goes to local memory.
	const uint word = (bit & 64u) == 0u ? ((bit & 32u) == 0u ? words.x : words.y) : ((bit & 32u) == 0u ? words.z : words.w);
	return (word & (1u << (bit & 31u))) != 0u;
}

// Whether block `location` of grid `grid` has anything to see, or shares its
// bit with one that has (flow_block_bit): the block bitset, after the groups'.
bool flow_block_marked(ivec3 location, int grid)
{
	const uint bit = flow_block_bit(location, grid);
	const uvec4 words = flow_occupancy[flow_params.limits.z + FLOW_COARSE_UVEC4S + (bit >> 7u)];
	const uint word = (bit & 64u) == 0u ? ((bit & 32u) == 0u ? words.x : words.y) : ((bit & 32u) == 0u ? words.z : words.w);
	return (word & (1u << (bit & 31u))) != 0u;
}

// The world's light round the gas, see flow_ambient.comp: rgb mean radiance
// times a, a the share of the probe that is in the open air.
layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_FLOW_AMBIENT)
uniform sampler3D TEX_FLOW_AMBIENT;

// What smoke at `position` scatters per unit of albedo of the world's light
// - the sun, the sky, the map's lights - relative to the adapted luminance.
// The probes are premultiplied by how much each is in the open, so one in a
// wall or the void gives the filter nothing rather than darkness, and a room
// never takes light from the far side of its wall. The texture wraps, and so
// does its sampler.
vec3 flow_ambient(vec3 position)
{
	const vec3 probe = position / flow_params.env_params.x + 0.5;
	const vec4 a = textureLod(TEX_FLOW_AMBIENT, probe / vec3(FLOW_ENV_DIM_X, FLOW_ENV_DIM_Y, FLOW_ENV_DIM_Z), 0);
	if (!(a.w > 1e-3))
		return vec3(0);
	return a.rgb / a.w * (flow_params.env_params.z / max(global_ubo.prev_adapted_luminance, 1e-9));
}

// What smoke at `position` scatters per unit of extinction of the lights the
// fire casts, relative to the adapted luminance; `shadow` is Flow's shadow
// toward light `shadow_light` - 1, if the layer has one.
vec3 flow_lit(vec3 position, uint shadow_light, float shadow)
{
	vec3 light = vec3(0);
	for (uint i = 0u; i < flow_params.counts.y; i++)
	{
		const vec3 d = flow_params.lights[i].xyz - position;
		const float radius = flow_params.lights[i].w;
		vec3 l = flow_params.light_colors[i].rgb / (dot(d, d) + radius * radius);
		if (i + 1u == shadow_light)
			l *= shadow;
		light += l;
	}
	return light / max(global_ubo.prev_adapted_luminance, 1e-9);
}

// The march's key for block `b` of a grid, to tell when a ray has gone into
// another: 10 bits a coordinate, wrapping round, and never FLOW_NO_BLOCK. Two
// blocks one step apart along a ray could share a key only if they were 1024
// blocks apart every way at once - further than any step goes. One register
// a grid, where the block's ivec3 took three: with the railgun trail's far
// grid the march came to more than the 128 the ray-gen shaders get, and
// spilled (11-08).
#define FLOW_NO_BLOCK 0x80000000u

// Two blocks' places in Flow's table in one register, 16 bits each - there
// are FLOW_POOL_MAX (4096) blocks at most - FLOW_NO_INDEX for none.
#define FLOW_NO_INDEX 0xFFFFu
#define FLOW_NO_INDICES 0xFFFFFFFFu
uint flow_index(uint pair, bool high)
{
	return high ? pair >> 16u : pair & 0xFFFFu;
}
uint flow_set_index(uint pair, bool high, uint index)
{
	return high ? (pair & 0xFFFFu) | (index << 16u) : (pair & 0xFFFF0000u) | index;
}

uint flow_block_key(ivec3 b)
{
	return (uint(b.x) & 0x3ffu) | ((uint(b.y) & 0x3ffu) << 10u) | ((uint(b.z) & 0x3ffu) << 20u);
}

// Whether brick `bit` is marked in a block's occupancy mask.
bool flow_brick_marked(uvec4 occupied, uint bit)
{
	// Not occupied[bit >> 5u]: a vector indexed at run time goes to local memory.
	const uint word = bit < 64u ? (bit < 32u ? occupied.x : occupied.y) : (bit < 96u ? occupied.z : occupied.w);
	return (word & (1u << (bit & 31u))) != 0u;
}

// What layer `l` adds to one march step at `position`, from its cell `value`:
// how much it absorbs per unit length, and what it puts back - glow, and the
// smoke's own colour. With `shared_ambient` the world's light is read once a
// step, by the first layer with smoke there, and kept in `ambient` for the
// rest (`have_ambient`); without, every layer with smoke reads it again. Only
// where there is smoke, either way: reading it for every step ahead of the
// smoke was tried (11-03) and cost more than it saved - the probe texture is
// rgba32f, and most steps of a fireball have glow but no smoke.
//
// The glow goes into `glow`, apart from the smoke's light in `source`, for
// flow_step_add's floor. `lit` false: the ray is already past where anything
// shows through but glow, so the smoke's light is not worked out at all.
void flow_layer_add(int l, vec4 value, vec3 position, bool shared_ambient, bool lit,
	inout vec3 ambient, inout bool have_ambient, inout float extinction, inout vec3 source, inout vec3 glow)
{
	// temperature, fuel, burn or shadow, smoke
	const float sigma = max(value.w, 0.0) * flow_params.layers[l].smoke.w;
	extinction += sigma;
	if (sigma > 0.0 && lit)
	{
		vec3 colour = flow_params.layers[l].smoke.rgb;
		if (flow_params.counts.y > 0u)
			colour += flow_params.layers[l].albedo.rgb * flow_lit(position, flow_params.layers[l].info.w, clamp(value.z, 0.0, 1.0));
		if (flow_params.env_count.x > 0)
		{
			if (!shared_ambient || !have_ambient)
			{
				ambient = flow_ambient(position);
				have_ambient = true;
			}
			colour += flow_params.layers[l].albedo.rgb * ambient;
		}
		source += colour * sigma;
	}
	// FLOW_COLORMAP_WEIGHTED_FIRE: as bright as the cell's fuel says, and
	// white above 1 (the plasma beam's rings, flow_white_hot).
	const uint map = flow_params.layers[l].info.z;
	const bool weighted = map == FLOW_COLORMAP_WEIGHTED_FIRE;
	const float weight = weighted ? max(value.y, 0.0) : 1.0;
	vec3 heat = flow_colormap(map, value.x, flow_params.layers[l].albedo.w) * flow_params.layers[l].emission.yzw;
	if (weighted)
		heat = flow_white_hot(heat, value.x);
	glow += heat * (flow_params.layers[l].emission.x * weight);
}

// Lays one step of length `step` at `t` over what the march has gathered in
// front of it; true once so little shows through that the march can stop.
//
// GLOW THROUGH SMOKE THAT HIDES EVERYTHING ELSE (pt_flow_glow_floor, in
// flow_params.world_max.w). Seen from where it was fired, a rocket's trail
// runs straight at the eye, and a ray to its explosion goes down the length
// of it: some 1700 units of smoke, which lets through e^-10 or so. The blast
// came out as a ring round a black disc, or a crescent - and twice the fire's
// brightness did not fill it (Matt, 2026-11-03; the march's sample cap made a
// hole of the same shape on 10-25, and still would without its fix). Real
// smoke in front of a fire glows with the fire's light, scattered on through
// it many times; one-bounce lighting from a dark smoke has none of that. So
// glow is never let through less than this share of itself: where smoke in
// front lets more through than that - all of a fireball's own smoke at the
// defaults - nothing changes, and nothing else behind the smoke shows. 0 is
// the old march, which stopped once next to nothing showed through.
bool flow_step_add(float extinction, vec3 source, vec3 glow, float step, float t,
	inout vec3 radiance, inout float transmittance, inout float depth_sum, inout float depth_weight)
{
#if FLOW_STATS
	flow_stat_samples++;
#endif
	if (!(extinction > 0.0 || any(greaterThan(source + glow, vec3(0)))))
	{
#if FLOW_STATS
		flow_stat_faint++;
#endif
		return false;
	}

	const float glow_floor = flow_params.world_max.w;
	// Past here the old march had stopped: only glow can add anything.
	const bool hidden = transmittance < 0.003;

	const float alpha = 1.0 - exp(-extinction * step);
	// What a step puts out, less what it absorbs of itself on the way:
	// (1 - e^(-sigma ds)) / sigma, which is ds where nothing absorbs.
	const float path = extinction > 1e-5 ? alpha / extinction : step;
	const vec3 contribution = ((hidden ? vec3(0) : transmittance * source) + max(transmittance, glow_floor) * glow) * path;

#if FLOW_STATS
	{
		const float covered = transmittance * alpha;
		const float light = flow_luminance(contribution);
		if (covered >= 1e-3 || light >= 1e-3)
			flow_stat_seen++;
		else if (covered < 1e-4 && light < 1e-4)
			flow_stat_faint++;
	}
#endif

	radiance += contribution;
	const float weight = flow_luminance(contribution) + transmittance * alpha;
	depth_sum += weight * t;
	depth_weight += weight;
	transmittance *= 1.0 - alpha;

	// With a floor, glow further on can still show.
	return transmittance < 0.003 && glow_floor <= 0.0;
}

// Premultiplied colour and coverage of the volume between t_min and t_max,
// sampled every so many world units - each grid's march step, times
// `step_scale`; `depth` is where along the ray most of what was seen is,
// t_max if nothing. `jitter` in [0, 1) shifts the steps along the ray, per
// pixel and per frame, so the denoiser sees noise rather than slices.
//
// Only bricks that flow_occupancy.comp marked are sampled; the ray goes
// straight to where it leaves any other brick, or the whole block if none of
// it is marked or it is not allocated, or the whole group of blocks round it
// (flow_coarse_marked) if none of those is. Most of an allocated block is clear
// air - Flow keeps blocks round the gas and long after the gas has faded to
// nothing - so this is most of what a march would otherwise cost.
//
// The grids (FLOW_GRIDS) have blocks and bricks of their own sizes, so each
// keeps its own block and marked bricks here: a step is sampled when any
// grid's brick is marked, at the shortest step of the grids whose is, and
// clear space runs to the nearest of the grids' ends of it. Each grid's
// state is in variables of its own, picked by `g == 0 ? : g == 1 ? :` -
// arrays indexed at run time would go to local memory (flow_common.glsl).
// Each layer keeps only its block's table value (flow_block_value), a
// register where the atlas offset and a flag took four; a sample works the
// offset out again from the cell it is in, which is the same block.
//
// YOUR OWN PLASMA BEAM IN FIRST PERSON is two layers of gas, one down the
// barrel and one from the gun your body holds (flow.h): a march sees the
// first unless `mirror`, the second if it is - a mirror ray where a mirror
// shows your body, as reflect_refract.rgen's AS_FLAG_VIEWER_MODELS. They are
// the last two layers, and the loops over layers stop short of them
// (FLOW_MARCH_LAYERS); the one seen, `own`, keeps only its block's place in
// Flow's table (index3o), its value read again from there at a sample.
// The loops running over all nine layers took the reflection shader's local
// memory from 16 bytes to 32, and holding both layers' blocks to 48 (11-09).
#define FLOW_MARCH_LAYERS FLOW_LAYER_HEATBEAM_VIEW
vec4 flow_march(vec3 origin, vec3 direction, float t_min, float t_max, float step_scale, float jitter, bool mirror, out float depth)
{
	depth = t_max;

	if (flow_params.block_dim_bits.w == 0u)
		return vec4(0);

	const vec3 dir = mix(direction, vec3(1e-7), equal(direction, vec3(0)));
	const vec3 inv_dir = 1.0 / dir;

	// Only the box round the allocated blocks.
	{
		const vec3 t0 = (flow_params.world_min.xyz - origin) * inv_dir;
		const vec3 t1 = (flow_params.world_max.xyz - origin) * inv_dir;
		const vec3 t_near = min(t0, t1);
		const vec3 t_far = max(t0, t1);
		t_min = max(t_min, max(t_near.x, max(t_near.y, t_near.z)));
		t_max = min(t_max, min(t_far.x, min(t_far.y, t_far.z)));
	}
	if (t_min >= t_max)
		return vec4(0);
#if FLOW_STATS
	flow_stat_ray = true;
#endif

	const ivec3 block_bits = ivec3(flow_params.block_dim_bits.xyz);

	// A grid none of whose layers has blocks (grids[g].x 0) is left out
	// altogether. Which grids are in use and their steps are read from the
	// params where they are wanted, not held: with four grids, held, the
	// reflection shader went from 128 registers to 167 (11-09).

	// Steps sit on a grid fixed along the ray, so moving the box does not
	// move the samples - the finest grid's steps of those in use, so that
	// the ray comes out of clear space close to where its gas starts.
	float lattice = 1e30;
	for (int g = 0; g < FLOW_GRIDS; g++)
	{
		if (flow_params.grids[g].x > 0.0)
			lattice = min(lattice, flow_params.grids[g].w);
	}
	lattice *= step_scale;
	float t = (ceil(t_min / lattice - jitter) + jitter) * lattice;

	vec3 radiance = vec3(0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	float depth_weight = 0.0;

	uint block0 = FLOW_NO_BLOCK;  // flow_block_key of the block of grid 0 the ray is in
	uint block1 = FLOW_NO_BLOCK;
	uint block2 = FLOW_NO_BLOCK;
	uint block3 = FLOW_NO_BLOCK;
	uvec4 occupied0 = uvec4(0);   // the bricks of grid 0's block marked in any of its layers
	// Grids 1 to 3 - the railgun's trail near the view, the same trail far
	// off, and the plasma beam - have one layer each the loops walk, so their
	// marked bricks are that block's flow_occupancy, read again where wanted.
	// Held, as grid 0's are, the fourth grid took the reflection shader from
	// 128 registers to 167, and grid 3 alone so read still spilled 16 bytes;
	// the first-person beam's layer spilled 48 until grid 1 was read so too,
	// the values read again at a sample, and the places two to a register
	// (11-09). Their blocks' places in Flow's table (flow_index): grid 1's
	// and grid 2's, and grid 3's FLOW_LAYER_HEATBEAM and `own`.
	uint index12 = FLOW_NO_INDICES;
	uint index3o = FLOW_NO_INDICES;
	const int own = mirror ? FLOW_LAYER_HEATBEAM_MIRROR : FLOW_LAYER_HEATBEAM_VIEW;
	uint blocks[FLOW_MARCH_LAYERS];   // each layer's flow_block_value there, 0 for none
	for (int l = 0; l < FLOW_MARCH_LAYERS; l++)
		blocks[l] = 0u;

	// At most limits.x samples; going past empty space costs next to nothing,
	// and is allowed three times as many again. Past half of them the steps
	// grow to reach t_max with what is left: a ray down a rocket's trail,
	// inside its smoke all the way, used them all up some 600 units out and
	// never got to the explosion at the end - a hole in the fire, the wall
	// behind it showing through, with the trail's bricks for its edge (Matt,
	// 10-25).
	uint samples = 0u;
	float step = lattice;

	// pt_flow_march (flow.c): 1, the world's light read once a step for all
	// the layers (flow_layer_add); 2, two steps read at a time (pair, below).
	// Neither changes what is sampled or how it adds up.
	const bool shared_ambient = (flow_params.limits.w & 1u) != 0u;
	const bool pairs = (flow_params.limits.w & 2u) != 0u
		&& flow_params.grids[1].x <= 0.0 && flow_params.grids[2].x <= 0.0 && flow_params.grids[3].x <= 0.0;

	for (uint i = 0u; i < 4u * flow_params.limits.x && samples < flow_params.limits.x && t < t_max; i++)
	{
		const vec3 position = origin + direction * t;
#if FLOW_STATS
		flow_stat_iterations++;
#endif

		// Whether each grid's brick here is marked, and if neither is, where
		// the clear space ends: the nearer of the grids' ends of it.
		uint marked = 0u;   // bit g: grid g's brick here is marked
		float t_exit = t_max;
		for (int g = 0; g < FLOW_GRIDS; g++)
		{
			if (flow_params.grids[g].x <= 0.0)
				continue;

			// pt_flow_march 8: outside the box round a grid's blocks there is
			// nothing of it. A railgun shot's fine piece and its coarse ones,
			// or a shot and an explosion, can be far apart in the one box round
			// everything - and the one who fires is inside that - so without
			// this every ray stopped at every group of every grid in use all
			// the way through it (11-09). Ahead of its box the grid only says
			// where the ray comes into it; past it, nothing. Its block is
			// forgotten, so that no step another grid samples reads its layers
			// from where the ray last was. Where the box test and the ray's own
			// t disagree by a rounding, the grid is looked at as usual.
			if ((flow_params.limits.w & 8u) != 0u)
			{
				const vec3 g_min = flow_params.grid_min[g].xyz;
				const vec3 g_max = flow_params.grid_max[g].xyz;
				if (any(lessThan(position, g_min)) || any(greaterThan(position, g_max)))
				{
					const vec3 e0 = (g_min - origin) * inv_dir;
					const vec3 e1 = (g_max - origin) * inv_dir;
					const vec3 e_near = min(e0, e1);
					const vec3 e_far = max(e0, e1);
					const float t_in = max(e_near.x, max(e_near.y, e_near.z));
					const float t_out = min(e_far.x, min(e_far.y, e_far.z));
					if (t_in > t || t_out < t)
					{
						if (t_in > t && t_in <= t_out)
							t_exit = min(t_exit, t_in);
						if (g == 0)
						{
							block0 = FLOW_NO_BLOCK;
							occupied0 = uvec4(0);
						}
						else if (g == 1)
						{
							block1 = FLOW_NO_BLOCK;
							index12 = flow_set_index(index12, false, FLOW_NO_INDEX);
						}
						else if (g == 2)
						{
							block2 = FLOW_NO_BLOCK;
							index12 = flow_set_index(index12, true, FLOW_NO_INDEX);
						}
						else
						{
							block3 = FLOW_NO_BLOCK;
							index3o = FLOW_NO_INDICES;
						}
						for (int l = 0; l < FLOW_MARCH_LAYERS; l++)
						{
							if (FLOW_LAYER_GRID(l) == g)
								blocks[l] = 0u;
						}
						continue;
					}
				}
			}

			const vec3 cells_per_unit = flow_params.grids[g].xyz;
			const vec3 cell = position * cells_per_unit;
			const ivec3 b = ivec3(floor(cell)) >> block_bits;
			const uint key = flow_block_key(b);
			uvec4 occupied = uvec4(0);
			if (g == 0)
				occupied = occupied0;
			else if (g == 1)
			{
				if (flow_index(index12, false) != FLOW_NO_INDEX)
					occupied = flow_occupancy[flow_index(index12, false)];
			}
			else if (g == 2)
			{
				if (flow_index(index12, true) != FLOW_NO_INDEX)
					occupied = flow_occupancy[flow_index(index12, true)];
			}
			else
			{
				if (flow_index(index3o, false) != FLOW_NO_INDEX)
					occupied = flow_occupancy[flow_index(index3o, false)];
				if (flow_index(index3o, true) != FLOW_NO_INDEX)
					occupied |= flow_occupancy[flow_index(index3o, true)];
			}
			if (key != (g == 0 ? block0 : (g == 1 ? block1 : (g == 2 ? block2 : block3))))
			{
#if FLOW_STATS
				flow_stat_blocks++;
#endif
				occupied = uvec4(0);
				// Whether to look the block up in Flow's table at all: its group
				// has something, and - pt_flow_march 4 - so has the block. A
				// railgun shot's gas is a thin tube through groups many blocks
				// across, and each block of them the ray went into cost a table
				// search; most have nothing, or are not allocated (11-09). A block
				// passed here is passed the way an empty one always was: blocks[l]
				// 0, nothing marked, the ray on to where it leaves the block.
				const bool look = flow_coarse_marked(b >> FLOW_COARSE_BITS, g)
					&& ((flow_params.limits.w & 4u) == 0u || flow_block_marked(b, g));
				uint found = FLOW_NO_INDEX;   // the last allocated block's place in the table, for grids 1 to 3
				for (int l = 0; l < FLOW_MARCH_LAYERS; l++)
				{
					if (FLOW_LAYER_GRID(l) != g)
						continue;
					blocks[l] = 0u;
					if (look && flow_params.layers[l].info.x != 0u)
					{
						uint index;
						blocks[l] = flow_block_value(b, int(flow_params.layers[l].info.y), index);
						// the top bit marks an allocated block
						if ((blocks[l] >> 31u) != 0u)
						{
							occupied |= flow_occupancy[index];
							found = index;
						}
					}
				}
				if (g == 0)
				{
					block0 = key;
					occupied0 = occupied;
				}
				else if (g == 1)
				{
					block1 = key;
					index12 = flow_set_index(index12, false, found);
				}
				else if (g == 2)
				{
					block2 = key;
					index12 = flow_set_index(index12, true, found);
				}
				else
				{
					block3 = key;
					uint own_found = FLOW_NO_INDEX;
					if (look && flow_params.layers[own].info.x != 0u)
					{
						uint index;
						// the top bit marks an allocated block
						if ((flow_block_value(b, int(flow_params.layers[own].info.y), index) >> 31u) != 0u)
						{
							occupied |= flow_occupancy[index];
							own_found = index;
						}
					}
					index3o = found | (own_found << 16u);
				}
			}

			const ivec3 local = ivec3(floor(cell)) - (b << block_bits);
			if (flow_brick_marked(occupied, flow_brick_bit(local)))
			{
				marked |= 1u << uint(g);
				continue;
			}

			// Nothing to see on this grid here: clear to where the ray leaves
			// this brick, or this block when none of it is marked, or its group
			// when none of that is.
			vec3 box_min, box_max;
			if (all(equal(occupied, uvec4(0))))
			{
				const int shift = flow_coarse_marked(b >> FLOW_COARSE_BITS, g) ? 0 : FLOW_COARSE_BITS;
				const ivec3 first = (b >> shift) << shift;
				box_min = vec3(first << block_bits) / cells_per_unit;
				box_max = vec3((first + (1 << shift)) << block_bits) / cells_per_unit;
			}
			else
			{
				const ivec3 brick = (b << block_bits) + ((local >> FLOW_BRICK_BITS) << FLOW_BRICK_BITS);
				box_min = vec3(brick) / cells_per_unit;
				box_max = vec3(brick + FLOW_BRICK) / cells_per_unit;
			}
			const vec3 e0 = (box_min - origin) * inv_dir;
			const vec3 e1 = (box_max - origin) * inv_dir;
			const vec3 e_far = max(e0, e1);
			t_exit = min(t_exit, min(e_far.x, min(e_far.y, e_far.z)));
		}

		if (marked == 0u)
		{
			t = max(t + step, (ceil(t_exit / lattice - jitter) + jitter) * lattice);
			continue;
		}
		samples++;
		// the shortest step of the grids marked here
		step = 1e30;
		for (int g = 0; g < FLOW_GRIDS; g++)
		{
			if ((marked & (1u << uint(g))) != 0u)
				step = min(step, flow_params.grids[g].w);
		}
		step *= step_scale;
		if (2u * samples > flow_params.limits.x)
			step = max(step, (t_max - t) / float(flow_params.limits.x - samples + 1u));

		// The step after this one as well, when the loop would sample it next
		// with nothing in between - grid 0 alone in use, the same block, a
		// marked brick of it, short of t_max, of the budget, of where the steps
		// start to grow and of the iteration cap - so that the two steps' reads
		// go out together rather than one after the other. The same samples,
		// laid on in the same order: a march is a chain of texture reads, each
		// waiting on the last, and this halves the waits.
		bool pair = false;
		vec3 position_next = position;
		if (pairs && (marked & 1u) != 0u)
		{
			position_next = origin + direction * (t + step);
			const ivec3 cell = ivec3(floor(position_next * flow_params.grids[0].xyz));
			const ivec3 b = cell >> block_bits;
			pair = flow_block_key(b) == block0
				&& flow_brick_marked(occupied0, flow_brick_bit(cell - (b << block_bits)))
				&& t + step < t_max
				&& samples < flow_params.limits.x
				&& 2u * (samples + 1u) <= flow_params.limits.x
				&& i + 1u < 4u * flow_params.limits.x;
		}

		// What the layers here absorb per unit length, and what they put back
		// per unit length: glow, and the smoke's own colour. The world's light
		// is the same for every layer, so it is read once (shared_ambient).
		const vec3 cell_next = position_next * flow_params.grids[0].xyz;
		vec3 ambient = vec3(0);
		vec3 ambient_next = vec3(0);
		bool have_ambient = false;
		bool have_ambient_next = false;
		float extinction = 0.0;
		float extinction_next = 0.0;
		vec3 source = vec3(0);
		vec3 source_next = vec3(0);
		vec3 glow = vec3(0);
		vec3 glow_next = vec3(0);
		// Past this, flow_step_add takes only glow (pt_flow_glow_floor).
		const bool lit = transmittance >= 0.003;
		for (int l = 0; l < FLOW_MARCH_LAYERS; l++)
		{
			// FLOW_LAYER_GRID(l) is a constant once the loop is unrolled. The
			// layer of grids 1 to 3 has its block's value read again by its
			// place in the table.
			const int g = FLOW_LAYER_GRID(l);
			const uint index = g == 1 ? flow_index(index12, false) : (g == 2 ? flow_index(index12, true) : flow_index(index3o, false));
			if (g == 0 ? (blocks[l] >> 31u) == 0u : index == FLOW_NO_INDEX)
				continue;
			const uint block = g == 0 ? blocks[l] : flow_table[index + flow_params.table_offsets.z];

			// The cell, and the first cell of its block - the one `block` is
			// of. The atlas texel is the block's base plus the cell within it.
			const vec3 cell = position * flow_params.grids[g].xyz;
			const vec3 first = vec3((ivec3(floor(cell)) >> block_bits) << block_bits);
			const vec3 base = vec3(flow_block_base(block)) - first;
			const vec4 value = textureLod(TEX_FLOW_DENSITY, (cell + base) * flow_params.dim_inv.xyz, 0);
			// A pair is on grid 0 alone, so every layer with blocks is too, and
			// in the same block.
			vec4 value_next = vec4(0);
			if (pair)
				value_next = textureLod(TEX_FLOW_DENSITY, (cell_next + base) * flow_params.dim_inv.xyz, 0);

			if (position.z <= flow_params.layers[l].world_max.w)
				flow_layer_add(l, value, position, shared_ambient, lit, ambient, have_ambient, extinction, source, glow);
			if (pair && position_next.z <= flow_params.layers[l].world_max.w)
				flow_layer_add(l, value_next, position_next, shared_ambient, lit, ambient_next, have_ambient_next, extinction_next, source_next, glow_next);
		}
		// `own`, drawn as FLOW_LAYER_HEATBEAM, which it looks like (flow.c).
		// Never a pair: that is grid 0 alone.
		if (flow_index(index3o, true) != FLOW_NO_INDEX)
		{
			const vec3 cell = position * flow_params.grids[3].xyz;
			const vec3 first = vec3((ivec3(floor(cell)) >> block_bits) << block_bits);
			const vec3 base = vec3(flow_block_base(flow_table[flow_index(index3o, true) + flow_params.table_offsets.z])) - first;
			const vec4 value = textureLod(TEX_FLOW_DENSITY, (cell + base) * flow_params.dim_inv.xyz, 0);
			if (position.z <= flow_params.layers[own].world_max.w)
				flow_layer_add(FLOW_LAYER_HEATBEAM, value, position, shared_ambient, lit, ambient, have_ambient, extinction, source, glow);
		}

		if (flow_step_add(extinction, source, glow, step, t, radiance, transmittance, depth_sum, depth_weight))
			break;
		t += step;

		if (pair)
		{
			// The iteration the loop would have spent on it.
			samples++;
			i++;
#if FLOW_STATS
			flow_stat_iterations++;
#endif
			if (flow_step_add(extinction_next, source_next, glow_next, step, t, radiance, transmittance, depth_sum, depth_weight))
				break;
			t += step;
		}
	}

	if (depth_weight > 0.0)
		depth = depth_sum / depth_weight;

	return vec4(radiance * global_ubo.prev_adapted_luminance, 1.0 - transmittance);
}

// `effects` from trace_effects_ray along `ray`, with the Flow volume laid in
// front of them or behind them, whichever is nearer for the most part. The
// effects overlay keeps only its nearest hit distance, so a volume that
// wraps round a sprite is put wholly to one side of it. Glow alone covers
// nothing - it only adds light - so it is not skipped for want of coverage.
// `reflection` takes the coarser steps of a reflection or refraction ray;
// `mirror` sees your own plasma beam as a mirror does (flow_march).
// pt_flow_stats: adds this march's counters to the readback. Primary rays
// only - flow_add_to_effects inline, or primary_rays.rgen's flow pass.
// subgroupAdd sums the invocations that got here, which is every one that
// marched (primary_rays.rgen skips some on field 1).
void flow_stats_flush()
{
#if FLOW_STATS
	if (flow_params.counts.z != 0u)
	{
		const uvec4 a = subgroupAdd(uvec4(flow_stat_ray ? 1u : 0u, flow_stat_iterations, flow_stat_samples, flow_stat_blocks));
		const uvec2 b = subgroupAdd(uvec2(flow_stat_seen, flow_stat_faint));
		if (subgroupElect())
		{
			atomicAdd(readback.flow_rays, a.x);
			atomicAdd(readback.flow_iterations, a.y);
			atomicAdd(readback.flow_samples, a.z);
			atomicAdd(readback.flow_blocks, a.w);
			atomicAdd(readback.flow_seen, b.x);
			atomicAdd(readback.flow_faint, b.y);
		}
	}
#endif
}

/* PRIMARY RAYS PASSED BY THEIR SCREEN TILE (pt_flow_march 16, camera_pos.w).
   A plasma beam or a railgun shot starts at the gun, and the one who fires
   is inside its box: every ray on the screen went into it and walked out
   past the bricks round the gun, some ten steps each, though most of the
   screen has none of the gas in front of it (pt_flow_stats, 11-12).
   flow_occupancy.comp marks each tile of the plane one unit in front of the
   camera that a marked brick covers; a ray from the camera whose tile is
   clear meets no marked brick, and the march samples nothing else, so it
   would add nothing - it is not marched. Only rays from the camera itself
   (not a lens's, pt_aperture), at least a little in front, and on the
   tiles; and none while a marked brick is within a fraction of a unit of
   the camera (flow_occupancy.comp flow_screen_mark). */
bool flow_screen_clear(vec3 origin, vec3 direction)
{
	if (flow_params.camera_pos.w == 0.0 || any(notEqual(origin, flow_params.camera_pos.xyz)))
		return false;
	const uint base = flow_params.limits.z + FLOW_COARSE_UVEC4S + FLOW_BLOCK_UVEC4S;
	if (flow_occupancy[base + FLOW_SCREEN_TX * FLOW_SCREEN_TY / 128u].x != 0u)
		return false;
	const vec3 d = normalize(direction);
	const float z = dot(d, flow_params.camera_forward.xyz);
	if (!(z > 1e-3))
		return false;
	const vec2 t = vec2(dot(d, flow_params.camera_right.xyz), dot(d, flow_params.camera_up.xyz)) / z;
	const vec2 f = (t / vec2(flow_params.camera_forward.w, flow_params.camera_right.w)) * 0.5 + 0.5;
	if (!(f.x >= 0.0 && f.y >= 0.0 && f.x < 1.0 && f.y < 1.0))
		return false;
	const uvec2 tile = min(uvec2(f * vec2(FLOW_SCREEN_TX, FLOW_SCREEN_TY)), uvec2(FLOW_SCREEN_TX - 1u, FLOW_SCREEN_TY - 1u));
	const uint bit = tile.y * FLOW_SCREEN_TX + tile.x;
	const uvec4 words = flow_occupancy[base + (bit >> 7u)];
	const uint c = (bit >> 5u) & 3u;
	const uint word = c < 2u ? (c == 0u ? words.x : words.y) : (c == 2u ? words.z : words.w);
	return (word & (1u << (bit & 31u))) == 0u;
}

// Whether a primary ray is left unmarched. pt_flow_stats 2 turns it round:
// only the rays the tiles pass are marched, so that FLOWSTATS counts what
// they would have sampled - anything at all is the tiles' mistake.
bool flow_screen_pass(vec3 origin, vec3 direction)
{
	const bool clear = flow_screen_clear(origin, direction);
	return flow_params.counts.z == 2u ? !clear : clear;
}

vec4 flow_add_to_effects(vec4 effects, Ray ray, float jitter, bool reflection, bool mirror)
{
	if (!reflection && flow_screen_pass(ray.origin, ray.direction))
		return effects;
	float depth;
	const float step_scale = reflection ? flow_params.world_min.w : 1.0;
	const vec4 volume = flow_march(ray.origin, ray.direction, ray.t_min, ray.t_max, step_scale, jitter, mirror, depth);
	if (!reflection)
		flow_stats_flush();
	if (volume.a <= 0.0 && all(lessThanEqual(volume.rgb, vec3(0))))
		return effects;

	const float effects_near = unpackHalf2x16(ray_payload_effects.distances).x;
	if (effects.a <= 0.0 || effects_near <= 0.0 || depth < effects_near)
		return alpha_blend_premultiplied(volume, effects);

	return alpha_blend_premultiplied(effects, volume);
}

#endif // FLOW_GLSL_
