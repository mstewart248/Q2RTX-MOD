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
RAY TRACED SOUND: what sound_trace.c (Vulkan) and the D3D12 renderer's
d3d12/sound_trace.c share - a run's checks, the scene's and the parameters'
packing, and the results' unpacking. All of it is sound_trace.c's code; the
GPU half (queues, acceleration structures, dispatches) is each renderer's own.

No Vulkan here: the D3D12 renderer includes it.
*/

#pragma once

#include "shared/shared.h"
#include "refresh/refresh.h"
#include "shader/sound_trace_shared.h"

// sound_trace.comp's SoundTraceParams, std140
typedef struct {
    float       listener[4];
    float       sources[SOUND_TRACE_MAX_SOURCES][4];
    float       materials[SOUND_TRACE_MAX_MATERIALS][4];
    uint32_t    counts[4];
    uint32_t    counts2[4];
    float       dists[4];
    float       shift[4];
    float       queries[SOUND_TRACE_MAX_QUERIES][4];
    uint32_t    counts3[4];
} vkpt_sound_params_t;

// and its SoundTri
typedef struct {
    float       normal[3];
    uint32_t    material;
} vkpt_sound_tri_t;

#define VKPT_SOUND_FIELD_FLOATS (SOUND_TRACE_CHANNELS * SOUND_TRACE_BANDS * SOUND_TRACE_MAX_BINS)
#define VKPT_SOUND_FIELDS_SIZE  ((uint64_t)SOUND_TRACE_MAX_SOURCES * VKPT_SOUND_FIELD_FLOATS * sizeof(float))
#define VKPT_SOUND_DIFF_SIZE    ((uint64_t)SOUND_TRACE_MAX_QUERIES * SOUND_DIFFRACT_FLOATS * sizeof(float))

// NULL, or why `t` cannot be run whatever the GPU has.
const char *vkpt_sound_check_run(const r_sound_trace_t *t);

// The scene's vertex and triangle totals. False when it has no triangles, or more
// than a TLAS instance's 24-bit custom index (where a mesh's triangles start) reaches.
bool vkpt_sound_scene_size(const r_sound_geom_t *geom, size_t *num_verts, size_t *num_tris);
// The vertices as they are, the indices per mesh (uint32, 3 a triangle), each
// triangle's normal and material, and where each mesh's vertices and triangles
// start. False: an index out of its mesh.
bool vkpt_sound_pack_scene(const r_sound_geom_t *geom, float *verts, uint32_t *indices, vkpt_sound_tri_t *tris,
                           size_t *first_vert, uint32_t *first_tri);
// The edges buffer: room for one edge at least, for the descriptor.
size_t vkpt_sound_edges_size(const r_sound_geom_t *geom);
void vkpt_sound_pack_edges(const r_sound_geom_t *geom, void *edges);

// A run's parameters, with the scene's edge count; *groups is the trace's
// dispatch width (by num_sources), *field_floats one source's field.
void vkpt_sound_pack_params(const r_sound_trace_t *t, int num_edges, vkpt_sound_params_t *p,
                            uint32_t *groups, uint32_t *field_floats);
// The readbacks into the trace's fields and diffraction.
void vkpt_sound_unpack(const r_sound_trace_t *t, uint32_t field_floats, const float *fields,
                       const float *diffraction);
