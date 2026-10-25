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
NVIDIA Flow's volume as the shaders see it: where it is bound, how a block of
it is found, and what colour its heat glows. flow.glsl marches it along rays;
flow_lights.comp adds up its glow into lights. See flow.c for where it comes
from. Needs global_textures.h first.

The volume is SPARSE: Flow only allocates blocks of cells where there is
something, and packs them into one 3D atlas. A world position becomes a
virtual cell index, its block is looked up in Flow's hash table to find
where that block sits in the atlas, and the atlas is sampled there. Each
block in the atlas carries a one-cell border copied from its neighbours, so
linear filtering across a block edge is right. This is NvFlowGlobalVirtualToReal
from Flow's NvFlowShader.hlsli. Each layer (FLOW_LAYERS) has blocks of its
own in the same table and atlas, under its own layerAndLevel.

A cell holds temperature, fuel, burn and smoke - or, when flow.c runs Flow's
shadow pass, temperature, fuel, the shadow toward one light, and smoke.
*/

#ifndef FLOW_COMMON_GLSL_
#define FLOW_COMMON_GLSL_

#include "flow.h"

layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_FLOW_DENSITY)
uniform sampler3D TEX_FLOW_DENSITY;

layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_FLOW_TABLE, std430)
readonly buffer FLOW_TABLE_BUFFER { uint flow_table[]; };

layout(set = GLOBAL_TEXTURES_DESC_SET_IDX, binding = BINDING_OFFSET_FLOW_PARAMS, std430)
readonly buffer FLOW_PARAMS_BUFFER { FlowParams flow_params; };

// The atlas texel of the first cell of a block, past its one-cell border,
// from the block's table value; NvFlowTableValueToIndexRead at thread 0.
ivec3 flow_block_base(uint value)
{
	return ivec3(
		int((value << 1u) | 1u) & 0xFFF,
		int((value >> 10u) | 1u) & 0x7FF,
		int((value >> 20u) | 1u) & 0x7FF);
}

// The density atlas offset of the block at `location` (in blocks) of the
// layer `layer_and_level`, so that virtual cell + offset is the atlas texel;
// w is 0 when the block is not allocated. `index` is its place in the table,
// as flow_lights.comp and flow_occupancy.comp number blocks.
// NvFlowLocationToBlockIdx + NvFlowGlobalVirtualToRealOffset.
ivec4 flow_block_find(ivec3 location, int layer_and_level, out uint index)
{
	const uvec3 bucket = uvec3(location) & flow_params.table_dim_less_one.xyz;
	const uint bucket_1d = (bucket.z << flow_params.table_offsets.x)
	                     | (bucket.y << flow_params.table_dim_less_one.w)
	                     | bucket.x;

	const uint range_begin = flow_table[(bucket_1d << 1u) + 0u];
	const uint range_end = min(flow_table[(bucket_1d << 1u) + 1u], range_begin + flow_params.limits.y);

	uint value = 0u;
	index = 0u;
	for (uint block_idx = range_begin; block_idx < range_end; block_idx++)
	{
		const uint i = (block_idx << 2u) + flow_params.table_offsets.y;
		if (int(flow_table[i + 0u]) == location.x &&
			int(flow_table[i + 1u]) == location.y &&
			int(flow_table[i + 2u]) == location.z &&
			int(flow_table[i + 3u]) == layer_and_level)
		{
			value = flow_table[block_idx + flow_params.table_offsets.z];
			index = block_idx;
			break;
		}
	}

	// The top bit marks an allocated block.
	const ivec3 base = flow_block_base(value);
	return ivec4(base - (location << ivec3(flow_params.block_dim_bits.xyz)), int(value >> 31u));
}

ivec4 flow_block_offset(ivec3 location, int layer_and_level)
{
	uint index;
	return flow_block_find(location, layer_and_level, index);
}

// flow_occupancy.comp cuts each block into bricks of FLOW_BRICK^3 cells and
// marks those that can show anything, a bit each, in a uvec4 - room for a
// block of Flow's standard 32x16x16 cells or less. The bit of the brick
// holding `cell`, a cell index within its block:
#define FLOW_BRICK 4
#define FLOW_BRICK_BITS 2
uint flow_brick_bit(ivec3 cell)
{
	const ivec3 bricks = max(ivec3(1) << ivec3(flow_params.block_dim_bits.xyz), ivec3(FLOW_BRICK)) >> FLOW_BRICK_BITS;
	const ivec3 brick = cell >> FLOW_BRICK_BITS;
	return uint((brick.z * bricks.y + brick.y) * bricks.x + brick.x);
}

// Extinction per world unit below which flow_occupancy.comp calls a cell's
// smoke nothing. A brick is 12 units across at the default cell size, so
// skipping one of them like this leaves out less than 1e-3 of what is behind
// it, and as little of the smoke's own colour, which is a few times the
// adapted luminance at most.
#define FLOW_EMPTY_EXTINCTION 1e-5

// A colour map: six points over 0..1, linear in between.
vec3 flow_ramp(float t, const float x[6], const vec3 c[6])
{
	t = clamp(t, 0.0, 1.0);
	for (int i = 1; i < 6; i++)
	{
		if (t <= x[i])
			return mix(c[i - 1], c[i], (t - x[i - 1]) / (x[i] - x[i - 1]));
	}
	return c[5];
}

// The glow of fire over temperature 0..1: Flow's default fire colour map
// (NvFlowRayMarchColormapParams_default_rgbaPoints_fire in NvFlowExt.h)
// times its default colour scale of 2.5 - dull red, orange, then
// yellow-white at the hottest - but black below 0.15, where Flow's own map
// has the dark grey of unlit smoke. Flow's renderer hides that grey wherever
// there is no smoke by weighting the map with it; here the glow does not
// depend on the smoke, so the grey would make every allocated block of
// cold, clear air glow.
vec3 flow_fire_color(float temperature)
{
	const float x[6] = float[](0.0, 0.05, 0.15, 0.6, 0.85, 1.0);
	const vec3 c[6] = vec3[](
		vec3(0.0),
		vec3(0.0),
		vec3(0.0),
		vec3(1.0, 0.1594125, 0.0135315),
		vec3(13.534992, 2.986956, 0.125991),
		vec3(78.08, 39.04, 6.1));
	return flow_ramp(temperature, x, c) * 2.5;
}

// The BFG's plasma: black when cool, then the green of its sprite and light,
// going yellow-white at the hottest. About as bright as the fire map at the top.
vec3 flow_plasma_color(float temperature)
{
	const float x[6] = float[](0.0, 0.05, 0.2, 0.5, 0.8, 1.0);
	const vec3 c[6] = vec3[](
		vec3(0.0),
		vec3(0.0),
		vec3(0.03, 0.2, 0.02),
		vec3(0.5, 3.0, 0.3),
		vec3(5.0, 20.0, 3.0),
		vec3(35.0, 55.0, 20.0));
	return flow_ramp(temperature, x, c) * 2.5;
}

// FlowLayerParams.info.z: 0 is a layer that does not glow.
vec3 flow_colormap(uint map, float temperature)
{
	if (map == 1u)
		return flow_fire_color(temperature);
	if (map == 2u)
		return flow_plasma_color(temperature);
	return vec3(0);
}

float flow_luminance(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

#endif // FLOW_COMMON_GLSL_
