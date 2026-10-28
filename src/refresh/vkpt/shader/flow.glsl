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
the main one, or the railgun trail's finer one (FLOW_GRIDS) - and adds them
together.

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
// The two grids (FLOW_GRIDS) have blocks and bricks of their own sizes, so
// each keeps its own block and marked bricks here: a step is sampled when
// either grid's brick is marked, at the fine grid's shorter step where its
// is, and clear space runs to the nearer of the two grids' ends of it. Each
// grid's state is in variables of its own, picked by `g == 0 ? :` - arrays
// indexed at run time would go to local memory (flow_common.glsl).
vec4 flow_march(vec3 origin, vec3 direction, float t_min, float t_max, float step_scale, float jitter, out float depth)
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

	const ivec3 block_bits = ivec3(flow_params.block_dim_bits.xyz);

	// A grid none of whose layers has blocks is left out altogether.
	const bool used0 = flow_params.grids[0].x > 0.0;
	const bool used1 = flow_params.grids[1].x > 0.0;
	const float step0 = flow_params.grids[0].w * step_scale;
	const float step1 = flow_params.grids[1].w * step_scale;

	// Steps sit on a grid fixed along the ray, so moving the box does not
	// move the samples - the finer grid's steps while it is in use, so that
	// the ray comes out of clear space close to where its gas starts.
	const float lattice = used1 ? step1 : step0;
	float t = (ceil(t_min / lattice - jitter) + jitter) * lattice;

	vec3 radiance = vec3(0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	float depth_weight = 0.0;

	ivec3 block0 = ivec3(0x7fffffff);
	ivec3 block1 = ivec3(0x7fffffff);
	uvec4 occupied0 = uvec4(0);   // the bricks of grid 0's block marked in any of its layers
	uvec4 occupied1 = uvec4(0);
	ivec4 offsets[FLOW_LAYERS];
	for (int l = 0; l < FLOW_LAYERS; l++)
		offsets[l] = ivec4(0);

	// At most limits.x samples; going past empty space costs next to nothing,
	// and is allowed three times as many again. Past half of them the steps
	// grow to reach t_max with what is left: a ray down a rocket's trail,
	// inside its smoke all the way, used them all up some 600 units out and
	// never got to the explosion at the end - a hole in the fire, the wall
	// behind it showing through, with the trail's bricks for its edge (Matt,
	// 10-25).
	uint samples = 0u;
	float step = lattice;
	for (uint i = 0u; i < 4u * flow_params.limits.x && samples < flow_params.limits.x && t < t_max; i++)
	{
		const vec3 position = origin + direction * t;

		// Whether each grid's brick here is marked, and if neither is, where
		// the clear space ends: the nearer of the grids' ends of it.
		bool marked0 = false;
		bool marked1 = false;
		float t_exit = t_max;
		for (int g = 0; g < FLOW_GRIDS; g++)
		{
			if (!(g == 0 ? used0 : used1))
				continue;

			const vec3 cells_per_unit = flow_params.grids[g].xyz;
			const vec3 cell = position * cells_per_unit;
			const ivec3 b = ivec3(floor(cell)) >> block_bits;
			uvec4 occupied = g == 0 ? occupied0 : occupied1;
			if (b != (g == 0 ? block0 : block1))
			{
				occupied = uvec4(0);
				const bool group = flow_coarse_marked(b >> FLOW_COARSE_BITS, g);
				for (int l = 0; l < FLOW_LAYERS; l++)
				{
					if (FLOW_LAYER_GRID(l) != g)
						continue;
					offsets[l] = ivec4(0);
					if (group && flow_params.layers[l].info.x != 0u)
					{
						uint index;
						offsets[l] = flow_block_find(b, int(flow_params.layers[l].info.y), index);
						if (offsets[l].w != 0)
							occupied |= flow_occupancy[index];
					}
				}
				if (g == 0)
				{
					block0 = b;
					occupied0 = occupied;
				}
				else
				{
					block1 = b;
					occupied1 = occupied;
				}
			}

			const ivec3 local = ivec3(floor(cell)) - (b << block_bits);
			const uint bit = flow_brick_bit(local);
			// Not occupied[bit >> 5u]: a vector indexed at run time goes to local memory.
			const uint word = bit < 64u ? (bit < 32u ? occupied.x : occupied.y) : (bit < 96u ? occupied.z : occupied.w);
			if ((word & (1u << (bit & 31u))) != 0u)
			{
				if (g == 0)
					marked0 = true;
				else
					marked1 = true;
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

		if (!marked0 && !marked1)
		{
			t = max(t + step, (ceil(t_exit / lattice - jitter) + jitter) * lattice);
			continue;
		}
		samples++;
		step = marked1 ? step1 : step0;
		if (2u * samples > flow_params.limits.x)
			step = max(step, (t_max - t) / float(flow_params.limits.x - samples + 1u));

		// What the layers here absorb per unit length, and what they put back
		// per unit length: glow, and the smoke's own colour.
		const vec3 cell0 = position * flow_params.grids[0].xyz;
		const vec3 cell1 = position * flow_params.grids[1].xyz;
		float extinction = 0.0;
		vec3 source = vec3(0);
		for (int l = 0; l < FLOW_LAYERS; l++)
		{
			if (offsets[l].w == 0 || position.z > flow_params.layers[l].world_max.w)
				continue;

			// temperature, fuel, burn or shadow, smoke
			const vec3 cell = FLOW_LAYER_GRID(l) == 0 ? cell0 : cell1;
			const vec4 value = textureLod(TEX_FLOW_DENSITY, (cell + vec3(offsets[l].xyz)) * flow_params.dim_inv.xyz, 0);

			const float sigma = max(value.w, 0.0) * flow_params.layers[l].smoke.w;
			extinction += sigma;
			if (sigma > 0.0)
			{
				vec3 colour = flow_params.layers[l].smoke.rgb;
				if (flow_params.counts.y > 0u)
					colour += flow_params.layers[l].albedo.rgb * flow_lit(position, flow_params.layers[l].info.w, clamp(value.z, 0.0, 1.0));
				if (flow_params.env_count.x > 0)
					colour += flow_params.layers[l].albedo.rgb * flow_ambient(position);
				source += colour * sigma;
			}
			source += flow_colormap(flow_params.layers[l].info.z, value.x, flow_params.layers[l].albedo.w) * flow_params.layers[l].emission.x * flow_params.layers[l].emission.yzw;
		}

		if (extinction > 0.0 || any(greaterThan(source, vec3(0))))
		{
			const float alpha = 1.0 - exp(-extinction * step);
			// What a step puts out, less what it absorbs of itself on the way:
			// (1 - e^(-sigma ds)) / sigma, which is ds where nothing absorbs.
			const float path = extinction > 1e-5 ? alpha / extinction : step;
			const vec3 contribution = transmittance * source * path;

			radiance += contribution;
			const float weight = flow_luminance(contribution) + transmittance * alpha;
			depth_sum += weight * t;
			depth_weight += weight;
			transmittance *= 1.0 - alpha;

			if (transmittance < 0.003)
				break;
		}

		t += step;
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
// `reflection` takes the coarser steps of a reflection or refraction ray.
vec4 flow_add_to_effects(vec4 effects, Ray ray, float jitter, bool reflection)
{
	float depth;
	const float step_scale = reflection ? flow_params.world_min.w : 1.0;
	const vec4 volume = flow_march(ray.origin, ray.direction, ray.t_min, ray.t_max, step_scale, jitter, depth);
	if (volume.a <= 0.0 && all(lessThanEqual(volume.rgb, vec3(0))))
		return effects;

	const float effects_near = unpackHalf2x16(ray_payload_effects.distances).x;
	if (effects.a <= 0.0 || effects_near <= 0.0 || depth < effects_near)
		return alpha_blend_premultiplied(volume, effects);

	return alpha_blend_premultiplied(effects, volume);
}

#endif // FLOW_GLSL_
