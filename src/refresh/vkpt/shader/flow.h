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

/* Flow layers, one per kind of effect (r_flow_style_t): fire, the BFG's
   plasma, underwater bubbles, the railgun's trail and the plasma beam - and
   four no client asks for by name: the far part of the railgun's trail again
   (FLOW_LAYER_RAIL_FAR), a rocket's trail of fire (FLOW_LAYER_FIRE_TRAIL),
   the fire's gas with a brightness of its own, and your own plasma beam in
   first person, twice (r_flow_emit_t.view): down the barrel, seen by the
   camera and through glass (FLOW_LAYER_HEATBEAM_VIEW), and from the gun your
   body holds, seen only in a mirror (FLOW_LAYER_HEATBEAM_MIRROR) - as the
   beam's model is drawn twice, RF_WEAPONMODEL and RF_VIEWERMODEL. Each
   simulates and looks its own way, but those two as the plasma beam. A fire
   light gathers the glow of both fire layers (FlowParams.seed_layers). A
   march sees one of the two first-person beams, never both (flow_march's
   `mirror`). */
#define FLOW_LAYERS 9

/* The cell grids the layers are on (FlowParams.grids): grid 0, the fire's,
   the plasma's and the bubbles', at pt_flow_cell_size; grid 1, the railgun
   trail's, at pt_flow_rail_cell_size - finer, as a spiral a cell or so thick
   came out boxy on the main grid; grid 2, the same trail's beyond
   pt_flow_rail_lod_distance of the view, at pt_flow_rail_far_cell_size - a
   level of detail: out there the fine cells are smaller than anyone can see,
   and cost the most blocks; grid 3, the plasma beam's (FLOW_LAYER_HEATBEAM
   and its first-person pair), at pt_flow_heatbeam_cell_size - its rings are finer than the main grid's
   cells, and it is on for seconds at a time, too long for the railgun's fine
   grid's cost. It shared grid 2 until Matt found it boxy and wanted the
   railgun's cells left as they are (11-09). Every grid has blocks of the
   same cells, so one table and one atlas serve them all. */
#define FLOW_GRIDS 4
#define FLOW_LAYER_RAIL 3
#define FLOW_LAYER_HEATBEAM 4
#define FLOW_LAYER_RAIL_FAR 5
#define FLOW_LAYER_FIRE_TRAIL 6
#define FLOW_LAYER_HEATBEAM_VIEW 7
#define FLOW_LAYER_HEATBEAM_MIRROR 8
#define FLOW_LAYER_IS_HEATBEAM(l) ((l) == FLOW_LAYER_HEATBEAM || (l) == FLOW_LAYER_HEATBEAM_VIEW || (l) == FLOW_LAYER_HEATBEAM_MIRROR)
#define FLOW_LAYER_GRID(l) ((l) == FLOW_LAYER_RAIL ? 1 : ((l) == FLOW_LAYER_RAIL_FAR ? 2 : (FLOW_LAYER_IS_HEATBEAM(l) ? 3 : 0)))

/* FlowLayerParams.info.z, how a layer glows (flow_colormap): not at all, fire,
   the BFG's plasma, the railgun's blue - and fire again, but as bright as the
   fuel in the cell says, a weight that never burns (the plasma beam, whose
   core is dimmed where it is seen end-on). Where only whether a cell glows
   matters - which bricks to march, what a light gathers - it counts as fire. */
#define FLOW_COLORMAP_NONE 0u
#define FLOW_COLORMAP_FIRE 1u
#define FLOW_COLORMAP_PLASMA 2u
#define FLOW_COLORMAP_RAIL 3u
#define FLOW_COLORMAP_WEIGHTED_FIRE 4u

/* Groups of FLOW_COARSE^3 blocks of a grid (block location >> FLOW_COARSE_BITS)
   that hold anything to see, hashed into a bitset of FLOW_COARSE_HASH_BITS
   that flow_occupancy.comp writes after the blocks' bricks - see
   flow_coarse_bit in flow_common.glsl. */
#define FLOW_COARSE_BITS 2
#define FLOW_COARSE_HASH_BITS 8192u
#define FLOW_COARSE_UVEC4S (FLOW_COARSE_HASH_BITS / 128u)

/* And each block of a grid that holds anything to see, a bit in a hashed
   bitset of FLOW_BLOCK_HASH_BITS after the groups' (flow_block_bit in
   flow_common.glsl): in a group that has something, the march passes the
   blocks that have nothing for a bit read each, not a search of Flow's table
   (pt_flow_march 4). A railgun shot's gas is a thin tube in groups many
   blocks across (11-09). */
#define FLOW_BLOCK_HASH_BITS 65536u
#define FLOW_BLOCK_UVEC4S (FLOW_BLOCK_HASH_BITS / 128u)

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
	uvec4 info;                 // x: 1 when the layer has blocks; y: its Flow layerAndLevel; z: colour map, a FLOW_COLORMAP_*; w: 1 + the light whose shadow the burn channel holds, 0 for none
}
END_SHADER_STRUCT(FlowLayerParams)

/* What flow.glsl needs to find its way round NVIDIA Flow's sparse density
   volume, and how to draw it. flow.c fills one per frame parity, at
   BINDING_OFFSET_FLOW_PARAMS of the global texture set; the table and the
   atlas it describes are at BINDING_OFFSET_FLOW_TABLE and _DENSITY.

   The first block is Flow's NvFlowSparseLevelParams for the density level,
   cut down to what a lookup reads - see NvFlowLocationToBlockIdx and
   NvFlowGlobalVirtualToReal in Flow's NvFlowShader.hlsli, which flow.glsl
   follows. A layer's cells are those of its grid (FLOW_LAYER_GRID). */
BEGIN_SHADER_STRUCT(FlowParams)
{
	uvec4 block_dim_bits;       // xyz: log2 of a density block's size in cells; w: 1 when there is anything to draw
	uvec4 table_dim_less_one;   // xyz: Flow's hash table size - 1; w: tableDimBits_x
	uvec4 table_offsets;        // x: tableDimBits_xy, y: locationOffset, z: blockLevelOffsetGlobal
	uvec4 limits;               // x: most march samples per ray, y: most blocks one hash bucket is searched for, z: the uvec4 of the occupancy buffer the group bitset starts at (the block bitset FLOW_COARSE_UVEC4S after it); w: pt_flow_march, how flow.glsl walks the volume (1 world light once a step, 2 two steps at a time, 4 the block bitset, 8 each grid only inside grid_min/max)
	vec4  dim_inv;              // xyz: 1 / the density atlas size in texels
	vec4  grids[FLOW_GRIDS];    // xyz: density cells per world unit on a grid, 0 for a grid none of whose layers has blocks; w: march step there in world units
	vec4  world_min;            // xyz: corner of every layer's allocated blocks together; w: how many times longer a reflection or refraction ray's steps are
	vec4  world_max;            // xyz: the opposite corner; w: pt_flow_glow_floor, the least share of glow smoke in front lets through (flow_step_add)
	uvec4 counts;               // x: blocks in the density level, y: lights, z: pt_flow_stats - count the primary march into readback.flow_* (flow.glsl FLOW_STATS)
	FlowLayerParams layers[FLOW_LAYERS];
	vec4  lights[FLOW_MAX_LIGHTS];          // xyz: where a light is; w: its radius
	vec4  light_colors[FLOW_MAX_LIGHTS];    // rgb: radiance x radius^2 / 4, absolute - what smoke at distance r scatters is albedo x this / r^2
	vec4  seeds[FLOW_MAX_LIGHTS];           // xyz: where flow_lights.comp gathers a light slot's glow from; w: how far from there a block may be, 0 for a slot not in use
	uvec4 seed_layers[FLOW_MAX_LIGHTS];     // x, y: the Flow layerAndLevels a slot's glow is of (the same twice but for fire, whose rocket trails are a layer of their own)
	ivec4 env_first;            // xyz: world index of the first probe flow_ambient.comp updates - probe i sits at i x env_params.x; w: the BSP cluster whose lights it samples, -1 for none
	ivec4 env_count;            // xyz: how many probes it updates each way, 0 when the smoke takes no light from the world; w: the frame, for the probes' history
	vec4  env_params;           // x: probe spacing in world units; y: the most frames a probe's history averages; z: how bright the world's light is on the smoke; w: the most mean radiance one light sample may bring, relative to the adapted luminance
	ivec4 env_lights;           // x: the first of Flow's own lights in the light buffer, y: how many - the smoke has those from flow_lit, with Flow's shadow; z: the spacing's level, 0 for pt_flow_env_spacing and 1 for twice it...
	vec4  grid_min[FLOW_GRIDS]; // xyz: corner of the allocated blocks of a grid's layers together (layers[].world_min), for a grid in use
	vec4  grid_max[FLOW_GRIDS]; // xyz: the opposite corner
}
END_SHADER_STRUCT(FlowParams)

#endif // FLOW_H_
