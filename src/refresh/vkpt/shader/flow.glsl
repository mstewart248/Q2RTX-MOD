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
NVIDIA Flow's fire, plasma and bubbles, marched along a primary or reflection
ray and laid over the effects overlay (see flow.c for where the volume comes
from, flow_common.glsl for how it is found). The march follows Flow's
NvFlowRayMarchBlock; the layers all share one cell grid, so a step looks
each layer up at the same block and adds them together.

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
// sampled every `step_length` world units; `depth` is where along the ray
// most of what was seen is, t_max if nothing. `jitter` in [0, 1) shifts the
// steps along the ray, per pixel and per frame, so the denoiser sees noise
// rather than slices.
//
// Only bricks that flow_occupancy.comp marked are sampled; the ray goes
// straight to where it leaves any other brick, or the whole block if none of
// it is marked or it is not allocated. Most of an allocated block is clear
// air - Flow keeps blocks round the gas and long after the gas has faded to
// nothing - so this is most of what a march would otherwise cost.
vec4 flow_march(vec3 origin, vec3 direction, float t_min, float t_max, float step_length, float jitter, out float depth)
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

	const vec3 cells_per_unit = flow_params.cell_size_inv.xyz;
	const ivec3 block_bits = ivec3(flow_params.block_dim_bits.xyz);

	// Steps sit on a grid fixed along the ray, so moving the box does not
	// move the samples.
	float t = (ceil(t_min / step_length - jitter) + jitter) * step_length;

	vec3 radiance = vec3(0);
	float transmittance = 1.0;
	float depth_sum = 0.0;
	float depth_weight = 0.0;

	ivec3 block = ivec3(0x7fffffff);
	ivec4 offsets[FLOW_LAYERS];
	uvec4 occupied = uvec4(0);   // the bricks of this block marked in any layer

	// At most limits.x samples; going past empty space costs next to nothing,
	// and is allowed three times as many again.
	uint samples = 0u;
	for (uint i = 0u; i < 4u * flow_params.limits.x && samples < flow_params.limits.x && t < t_max; i++)
	{
		const vec3 position = origin + direction * t;
		const vec3 cell = position * cells_per_unit;
		const ivec3 b = ivec3(floor(cell)) >> block_bits;
		if (b != block)
		{
			block = b;
			occupied = uvec4(0);
			for (int l = 0; l < FLOW_LAYERS; l++)
			{
				offsets[l] = ivec4(0);
				if (flow_params.layers[l].info.x != 0u)
				{
					uint index;
					offsets[l] = flow_block_find(b, int(flow_params.layers[l].info.y), index);
					if (offsets[l].w != 0)
						occupied |= flow_occupancy[index];
				}
			}
		}

		const ivec3 local = ivec3(floor(cell)) - (b << block_bits);
		const uint bit = flow_brick_bit(local);
		if ((occupied[bit >> 5u] & (1u << (bit & 31u))) == 0u)
		{
			// Nothing to see here: go to where the ray leaves this brick, or
			// this block when none of it is marked.
			vec3 box_min, box_max;
			if (all(equal(occupied, uvec4(0))))
			{
				box_min = vec3(b << block_bits) / cells_per_unit;
				box_max = vec3((b + 1) << block_bits) / cells_per_unit;
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
			const float t_exit = min(e_far.x, min(e_far.y, e_far.z));
			t = max(t + step_length, (ceil(t_exit / step_length - jitter) + jitter) * step_length);
			continue;
		}
		samples++;

		// What the layers here absorb per unit length, and what they put back
		// per unit length: glow, and the smoke's own colour.
		float extinction = 0.0;
		vec3 source = vec3(0);
		for (int l = 0; l < FLOW_LAYERS; l++)
		{
			if (offsets[l].w == 0 || position.z > flow_params.layers[l].world_max.w)
				continue;

			// temperature, fuel, burn or shadow, smoke
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
			source += flow_colormap(flow_params.layers[l].info.z, value.x) * flow_params.layers[l].emission.x * flow_params.layers[l].emission.yzw;
		}

		if (extinction > 0.0 || any(greaterThan(source, vec3(0))))
		{
			const float alpha = 1.0 - exp(-extinction * step_length);
			// What a step puts out, less what it absorbs of itself on the way:
			// (1 - e^(-sigma ds)) / sigma, which is ds where nothing absorbs.
			const float path = extinction > 1e-5 ? alpha / extinction : step_length;
			const vec3 contribution = transmittance * source * path;

			radiance += contribution;
			const float weight = flow_luminance(contribution) + transmittance * alpha;
			depth_sum += weight * t;
			depth_weight += weight;
			transmittance *= 1.0 - alpha;

			if (transmittance < 0.003)
				break;
		}

		t += step_length;
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
	const float step_length = reflection ? flow_params.world_min.w : flow_params.cell_size_inv.w;
	const vec4 volume = flow_march(ray.origin, ray.direction, ray.t_min, ray.t_max, step_length, jitter, depth);
	if (volume.a <= 0.0 && all(lessThanEqual(volume.rgb, vec3(0))))
		return effects;

	const float effects_near = unpackHalf2x16(ray_payload_effects.distances).x;
	if (effects.a <= 0.0 || effects_near <= 0.0 || depth < effects_near)
		return alpha_blend_premultiplied(volume, effects);

	return alpha_blend_premultiplied(effects, volume);
}

#endif // FLOW_GLSL_
