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

#ifndef FLOW_H_
#define FLOW_H_

#include "shader_structs.h"

/* Flow layers, one per kind of explosion (r_flow_style_t): fire, the BFG's
   plasma and underwater bubbles. Each simulates and looks its own way; all
   share one grid of cells. */
#define FLOW_LAYERS 3

/* Lights the fire and plasma cast, each gathered by flow_lights.comp from
   the glow nearest its seed. */
#define FLOW_MAX_LIGHTS 8

/* vec4s flow_lights.comp writes per density block: glow (rgb) and the light
   slot it went to (w), glow-weighted position from the slot's seed (xyz) and
   glow (w), and glow-weighted squared distance from the seed (x). Relative
   to the seed so that float sums keep their precision far from the origin. */
#define FLOW_LIGHT_SUM_VEC4S 3

/* The ambient light the smoke takes from the world (flow_ambient.comp): a
   probe every so many world units, kept in a texture this many probes each
   way that wraps round - probe (i, j, k) is at texel (i, j, k) mod the size -
   so that a probe keeps its texel, and its history, while the box round the
   gas moves. Powers of two. */
#define FLOW_ENV_DIM_X 64
#define FLOW_ENV_DIM_Y 64
#define FLOW_ENV_DIM_Z 32

/* How flow.glsl draws one layer. */
BEGIN_SHADER_STRUCT(FlowLayerParams)
{
	vec4  world_min;            // xyz: corner of the layer's allocated blocks
	vec4  world_max;            // xyz: the opposite corner; w: nothing of the layer is drawn above this height (a water surface)
	vec4  emission;             // x: colour map multiplier, per world unit, relative to the adapted luminance; yzw: colour balance of the glow
	vec4  smoke;                // rgb: unlit smoke colour, relative to the adapted luminance; w: extinction per unit of smoke per world unit
	vec4  albedo;               // rgb: the fraction of the lights' light the smoke scatters; w: the temperature fire starts to glow at
	uvec4 info;                 // x: 1 when the layer has blocks; y: its Flow layerAndLevel; z: colour map, see flow_colormap; w: 1 + the light whose shadow the burn channel holds, 0 for none
}
END_SHADER_STRUCT(FlowLayerParams)

/* What flow.glsl needs to find its way round NVIDIA Flow's sparse density
   volume, and how to draw it. flow.c fills one per frame parity, at
   BINDING_OFFSET_FLOW_PARAMS of the global texture set; the table and the
   atlas it describes are at BINDING_OFFSET_FLOW_TABLE and _DENSITY.

   The first block is Flow's NvFlowSparseLevelParams for the density level,
   cut down to what a lookup reads - see NvFlowLocationToBlockIdx and
   NvFlowGlobalVirtualToReal in Flow's NvFlowShader.hlsli, which flow.glsl
   follows. Every layer has the same cell size, so one cell grid serves all. */
BEGIN_SHADER_STRUCT(FlowParams)
{
	uvec4 block_dim_bits;       // xyz: log2 of a density block's size in cells; w: 1 when there is anything to draw
	uvec4 table_dim_less_one;   // xyz: Flow's hash table size - 1; w: tableDimBits_x
	uvec4 table_offsets;        // x: tableDimBits_xy, y: locationOffset, z: blockLevelOffsetGlobal
	uvec4 limits;               // x: most march samples per ray, y: most blocks one hash bucket is searched for
	vec4  dim_inv;              // xyz: 1 / the density atlas size in texels
	vec4  cell_size_inv;        // xyz: density cells per world unit; w: march step in world units
	vec4  world_min;            // xyz: corner of every layer's allocated blocks together; w: march step of a reflection or refraction ray, in world units
	vec4  world_max;            // xyz: the opposite corner
	uvec4 counts;               // x: blocks in the density level, y: lights
	FlowLayerParams layers[FLOW_LAYERS];
	vec4  lights[FLOW_MAX_LIGHTS];          // xyz: where a light is; w: its radius
	vec4  light_colors[FLOW_MAX_LIGHTS];    // rgb: radiance x radius^2 / 4, absolute - what smoke at distance r scatters is albedo x this / r^2
	vec4  seeds[FLOW_MAX_LIGHTS];           // xyz: where flow_lights.comp gathers a light slot's glow from; w: how far from there a block may be, 0 for a slot not in use
	uvec4 seed_layers[FLOW_MAX_LIGHTS];     // x: the Flow layerAndLevel a slot's glow is of
	ivec4 env_first;            // xyz: world index of the first probe flow_ambient.comp updates - probe i sits at i x env_params.x; w: the BSP cluster whose lights it samples, -1 for none
	ivec4 env_count;            // xyz: how many probes it updates each way, 0 when the smoke takes no light from the world; w: the frame, for the probes' history
	vec4  env_params;           // x: probe spacing in world units; y: the most frames a probe's history averages; z: how bright the world's light is on the smoke; w: the most mean radiance one light sample may bring, relative to the adapted luminance
	ivec4 env_lights;           // x: the first of Flow's own lights in the light buffer, y: how many - the smoke has those from flow_lit, with Flow's shadow; z: the spacing's level, 0 for pt_flow_env_spacing and 1 for twice it...
}
END_SHADER_STRUCT(FlowParams)

#endif // FLOW_H_
