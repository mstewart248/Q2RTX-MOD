/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019-2021, NVIDIA CORPORATION. All rights reserved.

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

#ifndef _VERTEX_BUFFER_H_
#define _VERTEX_BUFFER_H_

#include "shader_structs.h"

//#define MAX_LIGHT_LISTS         (1 << 15)
#define MAX_LIGHT_LISTS         (1 << 14)
//#define MAX_LIGHT_LIST_NODES    (1 << 22)
#define MAX_LIGHT_LIST_NODES    (3200000)
#define LIGHT_COUNT_HISTORY     16

#define MAX_IQM_MATRICES        32768

#define MAX_LIGHT_POLYS         8192
// 4 until the per-light volumetric scale arrived; p0..p3 were completely full
// (positions in xyz, colour in the three w's, then style/prev-style/type/spot
// profile), so there was no spare lane to steal and a fifth vec4 is the honest
// answer. Costs MAX_LIGHT_POLYS * 16 bytes = 128 KB of the light buffer.
#define LIGHT_POLY_VEC4S        5
// 7th uint carries the dedicated roughness/metallic texture indices
#define MATERIAL_UINTS          7

// should match the same constant declared in material.h
#define MAX_PBR_MATERIALS      8192

#define LIGHT_TEXTURE_SCALE     0

#define ALIGN_SIZE_4(x, n)  ((x * n + 3) & (~3))

#define PRIMITIVE_BUFFER_BINDING_IDX 0
#define POSITION_BUFFER_BINDING_IDX 1
#define LIGHT_BUFFER_BINDING_IDX 2
#define LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX 3
#define IQM_MATRIX_BUFFER_BINDING_IDX 4
#define READBACK_BUFFER_BINDING_IDX 5
#define TONE_MAPPING_BUFFER_BINDING_IDX 6
#define SUN_COLOR_BUFFER_BINDING_IDX 7
#define SUN_COLOR_UBO_BINDING_IDX 8
#define LIGHT_STATS_BUFFER_BINDING_IDX 9

#define VERTEX_BUFFER_WORLD 0
#define VERTEX_BUFFER_INSTANCED 1
#define VERTEX_BUFFER_FIRST_MODEL 2

#define SUN_COLOR_ACCUMULATOR_FIXED_POINT_SCALE 0x100000
#define SKY_COLOR_ACCUMULATOR_FIXED_POINT_SCALE 0x100

// A structure that is used in primitive buffers to store complete information about one triangle. 
// Its size is 8x float4 or 128 bytes to align with GPU cache lines.
// Path tracing accesses the primitive information in a very incoherent way, where every thread
// is likely to read a different primitive. Packing the info into one struct should reduce the
// total traffic from video memory by reading entire cache lines instead of sparse values from
// different buffers.
BEGIN_SHADER_STRUCT( VboPrimitive )
{
	vec3 pos0;
	uint material_id;

	vec3 pos1;
	int cluster;

	vec3 pos2;
	uint texture_flags;   // TEXTURE_FLAG_* - per-face, unlike material_id

	uvec3 normals;
	uint instance;

	uvec3 tangents;
	uint emissive_and_alpha;

	vec2 uv0;
	vec2 uv1;
	vec2 uv2;
	uvec2 custom0;  // The custom fields store motion for instanced meshes in the animated buffer,
	uvec2 custom1;  // or blend indices and weights for skinned meshes before they're animated.
	uvec2 custom2;
}
END_SHADER_STRUCT( VboPrimitive )


BEGIN_SHADER_STRUCT( LightBuffer )
{
	uint material_table[MAX_PBR_MATERIALS * MATERIAL_UINTS];
	vec4 light_polys[MAX_LIGHT_POLYS * LIGHT_POLY_VEC4S];
	uint light_list_offsets[MAX_LIGHT_LISTS];
	uint light_list_lights[MAX_LIGHT_LIST_NODES];
	float light_styles[MAX_LIGHT_STYLES];
	uint cluster_debug_mask[MAX_LIGHT_LISTS / 32];
	uint sky_visibility[MAX_LIGHT_LISTS / 32];
	uint sky_cluster_mask[MAX_LIGHT_LISTS / 32];
}
END_SHADER_STRUCT( LightBuffer )


BEGIN_SHADER_STRUCT( IqmMatrixBuffer )
{
	vec4 iqm_matrices[MAX_IQM_MATRICES * 3];
}
END_SHADER_STRUCT( IqmMatrixBuffer )


BEGIN_SHADER_STRUCT( ToneMappingBuffer )
{
	int accumulator[HISTOGRAM_BINS];
	float curve[HISTOGRAM_BINS];
	float normalized[HISTOGRAM_BINS];
	float adapted_luminance;
	float tonecurve;
}
END_SHADER_STRUCT( ToneMappingBuffer )


BEGIN_SHADER_STRUCT( ReadbackBuffer )
{
	uint material;
	uint cluster;
	float sun_luminance;
	float sky_luminance;

	vec3 hdr_color;
	float adapted_luminance;

	/* TEMPORARY fog-fade probe, GENERATION 2. uint only, two groups of four.

	   GENERATION 1 IS RETRACTED. It stored every float as
	   `uint(clamp(x * 1e9, 0.0, 4e9))` and the self-ratio as
	   `uint(clamp(r * 1e6, 0.0, 4e9)) + 1u` with r = -1.0 as the "expect <= 0"
	   sentinel. clamp() sends BOTH a negative value and that sentinel to exactly
	   0, so "post = 0" and "selfratio = 0.000000" - the result sessions 9-10 read
	   as proof that `inscatter += vol * 0.005` does nothing - are equally
	   consistent with inscatter being NEGATIVE, or with a NaN (NaN > 0.0 is false,
	   so it takes the sentinel branch too). The encoding cannot tell the three
	   apart. That is the sixth probe defect to imitate this fault.

	   So: store the RAW IEEE BITS. No scaling, no clamping, no quantisation, no
	   sentinel - sign, zero, subnormal, Inf and NaN all survive exactly, and the
	   CPU bit-casts them back. Plain stores from one designated cell, which is the
	   lightest probe available and still reproduced at 744 frames/run. */
	uint dbg_pre_b;    /* floatBitsToUint(luminance(inscatter))     BEFORE the += */
	uint dbg_vol_b;    /* floatBitsToUint(luminance(vol_inscatter))               */
	uint dbg_post_b;   /* floatBitsToUint(luminance(inscatter))     AFTER  the += */
	uint dbg_frame;    /* frame tag, same store - stale tag = the cell never ran  */

	uint dbg_expect_b; /* floatBitsToUint(pre + vol*scale*0.005*ratio)            */
	uint dbg_scale_b;  /* floatBitsToUint(global_ubo.pt_fog_vol_scale)            */
	uint dbg_ratio_b;  /* floatBitsToUint(global_ubo.fog_vol_density_ratio)       */
	uint dbg_sunlum_b; /* floatBitsToUint(luminance(sun_color)) - the only term
	                      that writes inscatter before this line, so if pre is
	                      negative this is where it came from */

	/* GRID-WIDE CLASSIFIER, same site, MONOTONIC - the CPU takes deltas.
	   The single cell above says what ONE cell did; these say what the whole grid
	   did, and they are what decides whether a run manifested at all (no
	   screenshots needed: the collapse is "every cell contributes zero").
	   dbg_n_cells is the shared normaliser - it MUST delta to 1410835 at this
	   site, and a probe whose normaliser is wrong is a broken probe, not a
	   finding. Four atomics, one block: the weight memory records as reproducing
	   best (607-863 collapsed frames/run; 8 suppresses it). */
	uint dbg_n_cells;  /* cells reaching the accumulation                      */
	uint dbg_n_zero;   /* ... with post == 0.0 exactly                         */
	uint dbg_n_neg;    /* ... with post <  0.0   - invisible to a clamped probe */
	uint dbg_n_bad;    /* ... with post NaN/Inf  - invisible to a clamped probe */

	/* GRID-WIDE TOTALS, same site, same normaliser, monotonic (CPU deltas).
	   The counters above say how many cells are zero; these say how much light
	   the grid actually produced, which is what the screen shows.  Scales are
	   chosen so the PER-FRAME DELTA cannot exceed 2^32 - the halves are ~1.8e2x
	   larger than post, so they get 1e4 where post gets 1e7:
	     post  3e-5   * 1e7 =  3e2 per cell * 1.41e6 = 4.2e8  OK
	     halves 5.6e-3 * 1e4 = 5.6e1 per cell * 1.41e6 = 7.9e7 OK
	   Getting this wrong wraps the delta and fakes a collapse. */
	uint dbg_sum_q;    /* sum of luminance(inscatter) after the +=  * 1e7 */
	uint dbg_sum_sky;  /* sum of the SKY half of vol_inscatter      * 1e4 */
	uint dbg_sum_lit;  /* sum of the LIGHT half of vol_inscatter    * 1e4 */
	uint dbg_n_tiny;   /* cells with 0 < post < 1e-9 - a collapse that is not
	                      an exact zero would hide from dbg_n_zero entirely */

	/* WHICH CODE RAN.  dbg_sum_sky / dbg_sum_lit are captured from locals that
	   are only ASSIGNED inside the ReSTIR branch - so "sky = 0 and lit = 0" is
	   equally consistent with the fog collapsing and with the ELSE branch
	   running and leaving those locals at their initial 0.0.  Likewise the
	   single-cell store's frame tag stops advancing on collapsed frames, which
	   says the centre cell never reached it.  These two counters separate
	   "the fog went dark" from "different code ran", which no counter so far
	   can do.  (Session 10's own NEXT list asked for exactly this.) */
	uint dbg_n_restir; /* cells that took the pt_fog_restir branch  */
	uint dbg_n_centre; /* times the designated centre cell was hit  */
	uint dbg_n_lt7;    /* cells with 0 < post < 1e-7 - pins the magnitude claim */
	uint dbg_n_pad;    /* keep the group a multiple of four         */

	/* ONE SLICE, AT A HUNDRED TIMES THE RESOLUTION OF dbg_sum_q.

	   Every aggregate above accumulates uint(clamp(v * 1eN, ...)), an integer
	   truncation, so each one has a quantum below which it reads exactly zero.
	   dbg_sum_q's is 1e-7, and a healthy cell carries only ~100 of its quanta -
	   so a hundredfold DIMMING and a true zero produce the identical reading of
	   0, and every capture in this investigation has been unable to tell them
	   apart.

	   Restricting to the z = FROXEL_GRID_Z/2 slice is what buys the resolution
	   back: 14080 cells instead of 1.45M, so the sum can be scaled by 1e9
	   instead of 1e7 and still sit ~30x inside a uint32. A healthy cell reads
	   about 10^4 here, which leaves four decades of headroom below it before
	   this counter, too, bottoms out.

	   n is the cell count for the same slice, so q/n is a mean that does not
	   need the grid geometry to interpret. */
	uint dbg_slice_q;  /* sum over the centre z slice of post * 1e9 */
	uint dbg_slice_n;  /* cells contributing to it                  */
}
END_SHADER_STRUCT( ReadbackBuffer )


BEGIN_SHADER_STRUCT( SunColorBuffer )
{
	ivec3 accum_sun_color;
	int padding1;

	ivec4 accum_sky_color;

	vec3 sun_color;
	float sun_luminance;

	vec3 sky_color;
	float sky_luminance;
}
END_SHADER_STRUCT( SunColorBuffer )


#ifdef VKPT_SHADER

#ifdef VERTEX_READONLY
#define VERTEX_READONLY_FLAG readonly
#else
#define VERTEX_READONLY_FLAG
#endif

struct MaterialInfo
{
	uint base_texture;
	uint normals_texture;
	uint emissive_texture;
	uint mask_texture;
	uint roughness_texture;
	uint metallic_texture;
	float bump_scale;
	float roughness_override;
	float metalness_factor;
	float emissive_factor;
	float specular_factor;
	float base_factor;
	float light_style_scale;
	uint num_frames;
	uint next_frame;
};

struct LightPolygon
{
	mat3 positions;
	vec3 color;
	float light_style_scale;
	float prev_style_scale;
	float type;
	// DYNLIGHT_SPOT only: DYNLIGHT_SPOT_EMISSION_PROFILE_*, which says how
	// positions[1].yz are to be read. See spotlight_falloff().
	float spot_emission_profile;
	// How much this light scatters into the volumetric medium, relative to how
	// much it lights surfaces - RTX Remix's per-light volumetricRadianceScale.
	// Already resolved on the CPU (copy_light), so it is never the "unset"
	// sentinel by the time a shader sees it: 1.0 means "same as the surface
	// lighting", 0 means "this light makes no fog at all".
	float volumetric_scale;
};

// The buffers with primitive data, currently two of them: world and instanced.
// They are stored in an array to allow branchless access with nonuniformEXT.
layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = PRIMITIVE_BUFFER_BINDING_IDX) VERTEX_READONLY_FLAG buffer PRIMITIVE_BUFFER {
	VboPrimitive primitives[];
} primitive_buffers[];

// The buffer with just the position data for animated models.
layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = POSITION_BUFFER_BINDING_IDX) VERTEX_READONLY_FLAG buffer POSITION_BUFFER {
	float positions[];
} instanced_position_buffer;

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = LIGHT_BUFFER_BINDING_IDX) readonly buffer LIGHT_BUFFER {
	LightBuffer light_buffer;
};

/* History of light count in cluster, used for sampling.
 * This is used to make gradient estimation work correctly:
 * "The A-SVGF algorithm uses old random numbers to select lights for a subset of pixels,
 * and expects to get the same result if the lighting didn't change."
 * (quoted from discussion on GH PR 227).
 * One way to achieve this is to also keep a history of light numbers and use that for
 * sampling "old" data.
 *
 * We have multiple buffers b/c we may need to access the history for the current or any previous frame.
 * That'd be harder to do with a light_buffer member since that is backed by alternating buffers */
layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = LIGHT_COUNTS_HISTORY_BUFFER_BINDING_IDX) readonly buffer LIGHT_COUNTS_HISTORY_BUFFER {
	uint sample_light_counts[];
} light_counts_history[LIGHT_COUNT_HISTORY];

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = IQM_MATRIX_BUFFER_BINDING_IDX) readonly buffer IQM_MATRIX_BUFFER {
	IqmMatrixBuffer iqm_matrix_buffer;
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = READBACK_BUFFER_BINDING_IDX) buffer READBACK_BUFFER {
	ReadbackBuffer readback;
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = TONE_MAPPING_BUFFER_BINDING_IDX) buffer TONE_MAPPING_BUFFER {
	ToneMappingBuffer tonemap_buffer;
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = SUN_COLOR_BUFFER_BINDING_IDX) buffer SUN_COLOR_BUFFER {
	SunColorBuffer sun_color_buffer;
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = SUN_COLOR_UBO_BINDING_IDX, std140) uniform SUN_COLOR_UBO {
	SunColorBuffer sun_color_ubo;
};

layout(set = VERTEX_BUFFER_DESC_SET_IDX, binding = LIGHT_STATS_BUFFER_BINDING_IDX) buffer LIGHT_STATS_BUFFERS {
	uint stats[];
} light_stats_bufers[3];

uint animate_material(uint material, int frame);

struct Triangle
{
	mat3x3 positions;
	mat3x3 positions_prev;
	mat3x3 normals;
	mat3x2 tex_coords;
	mat3x3 tangents;
	uint   material_id;
	uint   texture_flags;
	int    cluster;
	uint   instance_index;
	uint   instance_prim;
	float  emissive_factor;
	float  alpha;
};

Triangle
load_triangle(uint buffer_idx, uint prim_id)
{
	VboPrimitive prim = primitive_buffers[nonuniformEXT(buffer_idx)].primitives[prim_id];

	Triangle t;
	t.positions[0] = prim.pos0;
	t.positions[1] = prim.pos1;
	t.positions[2] = prim.pos2;

	t.positions_prev[0] = t.positions[0] + unpackHalf4x16(prim.custom0).xyz;
	t.positions_prev[1] = t.positions[1] + unpackHalf4x16(prim.custom1).xyz;
	t.positions_prev[2] = t.positions[2] + unpackHalf4x16(prim.custom2).xyz;
	
	t.normals[0] = decode_normal(prim.normals.x);
	t.normals[1] = decode_normal(prim.normals.y);
	t.normals[2] = decode_normal(prim.normals.z);

	t.tangents[0] = decode_normal(prim.tangents.x);
	t.tangents[1] = decode_normal(prim.tangents.y);
	t.tangents[2] = decode_normal(prim.tangents.z);

	t.tex_coords[0] = prim.uv0;
	t.tex_coords[1] = prim.uv1;
	t.tex_coords[2] = prim.uv2;

	t.material_id = prim.material_id;
	t.texture_flags = prim.texture_flags;
	t.cluster = prim.cluster;
	t.instance_index = prim.instance;
	t.instance_prim = 0;
	
	vec2 emissive_and_alpha = unpackHalf2x16(prim.emissive_and_alpha);
	t.emissive_factor = emissive_and_alpha.x;
	t.alpha = emissive_and_alpha.y;

	return t;
}

Triangle
load_and_transform_triangle(int instance_idx, uint buffer_idx, uint prim_id)
{
	Triangle t = load_triangle(buffer_idx, prim_id);

	if (instance_idx >= 0)
	{
		// Instance of a static mesh: transform the vertices.

		ModelInstance mi = instance_buffer.model_instances[instance_idx];
		
		t.positions[0] = vec3(mi.transform * vec4(t.positions[0], 1.0));
		t.positions[1] = vec3(mi.transform * vec4(t.positions[1], 1.0));
		t.positions[2] = vec3(mi.transform * vec4(t.positions[2], 1.0));

		t.positions_prev[0] = vec3(mi.transform_prev * vec4(t.positions_prev[0], 1.0));
		t.positions_prev[1] = vec3(mi.transform_prev * vec4(t.positions_prev[1], 1.0));
		t.positions_prev[2] = vec3(mi.transform_prev * vec4(t.positions_prev[2], 1.0));

		t.normals[0] = normalize(vec3(mi.transform * vec4(t.normals[0], 0.0)));
		t.normals[1] = normalize(vec3(mi.transform * vec4(t.normals[1], 0.0)));
		t.normals[2] = normalize(vec3(mi.transform * vec4(t.normals[2], 0.0)));

		t.tangents[0] = normalize(vec3(mi.transform * vec4(t.tangents[0], 0.0)));
		t.tangents[1] = normalize(vec3(mi.transform * vec4(t.tangents[1], 0.0)));
		t.tangents[2] = normalize(vec3(mi.transform * vec4(t.tangents[2], 0.0)));

		if (mi.material != 0) {
			t.material_id = mi.material;
		}
		t.material_id = animate_material(t.material_id, mi.frame);
		t.cluster = mi.cluster;
		t.emissive_factor = 1.0;
		t.alpha = mi.alpha;

		// Store the index of that instance and the prim offset relative to the instance.
		t.instance_index = uint(instance_idx);
		t.instance_prim = prim_id - mi.render_prim_offset;
	}
	else if (buffer_idx == VERTEX_BUFFER_INSTANCED)
	{
		// Instance of an animated or skinned mesh, coming from the primbuf.
		// In this case, `instance_idx` is -1 because it's not a static mesh, 
		// so load the original animated instance to find out its prim offset.

		ModelInstance mi = instance_buffer.model_instances[t.instance_index];
		t.instance_prim = prim_id - mi.render_prim_offset;
	}
	else if (buffer_idx == VERTEX_BUFFER_WORLD)
	{
		// Static BSP primitive.
		
		t.instance_index = ~0u;
		t.instance_prim = prim_id;
	}

	return t;
}

#ifndef VERTEX_READONLY
void
store_triangle(Triangle t, uint buffer_idx, uint prim_id)
{
	VboPrimitive prim;

	prim.pos0 = t.positions[0];
	prim.pos1 = t.positions[1];
	prim.pos2 = t.positions[2];

	prim.custom0 = packHalf4x16(vec4(t.positions_prev[0] - t.positions[0], 0));
	prim.custom1 = packHalf4x16(vec4(t.positions_prev[1] - t.positions[1], 0));
	prim.custom2 = packHalf4x16(vec4(t.positions_prev[2] - t.positions[2], 0));

	prim.normals.x = encode_normal(t.normals[0]);
	prim.normals.y = encode_normal(t.normals[1]);
	prim.normals.z = encode_normal(t.normals[2]);

	prim.tangents.x = encode_normal(t.tangents[0]);
	prim.tangents.y = encode_normal(t.tangents[1]);
	prim.tangents.z = encode_normal(t.tangents[2]);

	prim.uv0 = t.tex_coords[0];
	prim.uv1 = t.tex_coords[1];
	prim.uv2 = t.tex_coords[2];

	prim.material_id = t.material_id;
	prim.texture_flags = t.texture_flags;
	prim.cluster = t.cluster;
	prim.instance = t.instance_index;
	prim.emissive_and_alpha = packHalf2x16(vec2(t.emissive_factor, t.alpha));
	
	primitive_buffers[nonuniformEXT(buffer_idx)].primitives[prim_id] = prim;

	if (buffer_idx == VERTEX_BUFFER_INSTANCED)
	{
		for (int vert = 0; vert < 3; vert++)
		{
			for (int axis = 0; axis < 3; axis++)
			{
				instanced_position_buffer.positions[prim_id * 9 + vert * 3 + axis] 
					= t.positions[vert][axis];
			}
		}
	}
}
#endif

MaterialInfo
get_material_info(uint material_id)
{
	uint material_index = material_id & MATERIAL_INDEX_MASK;
	
	uint data[MATERIAL_UINTS];
	data[0] = light_buffer.material_table[material_index * MATERIAL_UINTS + 0];
	data[1] = light_buffer.material_table[material_index * MATERIAL_UINTS + 1];
	data[2] = light_buffer.material_table[material_index * MATERIAL_UINTS + 2];
	data[3] = light_buffer.material_table[material_index * MATERIAL_UINTS + 3];
	data[4] = light_buffer.material_table[material_index * MATERIAL_UINTS + 4];
	data[5] = light_buffer.material_table[material_index * MATERIAL_UINTS + 5];
	data[6] = light_buffer.material_table[material_index * MATERIAL_UINTS + 6];

	MaterialInfo minfo;
	minfo.base_texture = data[0] & 0xffff;
	minfo.normals_texture = data[0] >> 16;
	minfo.emissive_texture = data[1] & 0xffff;
	minfo.mask_texture = data[1] >> 16;
	minfo.roughness_texture = data[6] & 0xffff;
	minfo.metallic_texture = data[6] >> 16;
	minfo.bump_scale = unpackHalf2x16(data[2]).x;
	minfo.roughness_override = unpackHalf2x16(data[2]).y;
	minfo.metalness_factor = unpackHalf2x16(data[3]).x;
	minfo.emissive_factor = unpackHalf2x16(data[3]).y;
	minfo.specular_factor = unpackHalf2x16(data[5]).x;
	minfo.base_factor = unpackHalf2x16(data[5]).y;
	minfo.num_frames = data[4] & 0xffff;
	minfo.next_frame = (data[4] >> 16) & (MAX_PBR_MATERIALS - 1);

	// Apply the light style for non-camera materials.
	// Camera materials use the same bits to store the camera ID.
	if((material_id & MATERIAL_KIND_MASK) != MATERIAL_KIND_CAMERA)
	{
		uint light_style = (material_id & MATERIAL_LIGHT_STYLE_MASK) >> MATERIAL_LIGHT_STYLE_SHIFT;
		if(light_style != 0) 
		{
			minfo.emissive_factor *= light_buffer.light_styles[light_style];
		}
	}

	return minfo;
}

uint
animate_material(uint material, int frame)
{
	// Apply frame-based material animation: go through the linked list of materials.
	if (frame > 0)
	{
		uint new_material = material;
		MaterialInfo minfo = get_material_info(new_material);
		frame = frame % int(minfo.num_frames);

		while (frame --> 0) {
			new_material = minfo.next_frame;
			minfo = get_material_info(new_material);
		}

		material = new_material | (material & ~MATERIAL_INDEX_MASK); // preserve flags
	}
	return material;
}

LightPolygon
get_light_polygon(uint index)
{
	vec4 p0 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 0];
	vec4 p1 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 1];
	vec4 p2 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 2];
	vec4 p3 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 3];
	vec4 p4 = light_buffer.light_polys[index * LIGHT_POLY_VEC4S + 4];

	LightPolygon light;
	light.positions = mat3x3(p0.xyz, p1.xyz, p2.xyz);
	light.color = vec3(p0.w, p1.w, p2.w);
	light.light_style_scale = p3.x;
	light.prev_style_scale = p3.y;
	light.type = p3.z;
	light.spot_emission_profile = p3.w;
	light.volumetric_scale = p4.x;
	return light;
}

mat3x4
get_iqm_matrix(uint index)
{
	mat3x4 result;
	result[0] = iqm_matrix_buffer.iqm_matrices[index * 3 + 0];
	result[1] = iqm_matrix_buffer.iqm_matrices[index * 3 + 1];
	result[2] = iqm_matrix_buffer.iqm_matrices[index * 3 + 2];
	return result;
}

#endif
#endif


