/*
Copyright (C) 2026 Matt Stewart

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

#ifndef VKPT_Q3DEFORM_H
#define VKPT_Q3DEFORM_H

#include "vkpt.h"
#include "material.h"

/* Quake III surfaces that move (deformVertexes wave / bulge / move), drawn as
   models built at map load. See the comment at the top of q3deform.c. */

// Forgets the last map's models and decides, from pt_q3_deform, whether this
// map's moving surfaces are taken out of the world. bsp_mesh_create_from_bsp,
// before the world's faces are collected.
void vkpt_q3_deform_begin(const bsp_t *bsp);

// Whether a world face drawn with `material` (its final material_id and
// surface flags, as collect_surfaces has them) moves, and so goes here instead
// of into the world.
bool vkpt_q3_deform_wants(const pbr_material_t *material, uint32_t material_id, int surf_flags);

// The face's triangles, as collect_surfaces made them (cluster set).
void vkpt_q3_deform_collect(const pbr_material_t *material, const VboPrimitive *prims, int count);

// Builds the models from what was collected. End of bsp_mesh_create_from_bsp.
void vkpt_q3_deform_build(void);

// A moving face's light polys (bsp_mesh.c collect_light_polys, after
// vkpt_q3_deform_build), copied to the model of `material`'s shader to be
// carried with it as a model's lights. False if no model took them: the world
// keeps them.
bool vkpt_q3_deform_take_lights(const pbr_material_t *material, const light_poly_t *lights, int count);

// Whether `model` is one of these (its lights keep one identity a frame to the
// next, and the fog's long history: main.c).
bool vkpt_q3_deform_model(const model_t *model);

// Drops everything (the world is unloaded).
void vkpt_q3_deform_reset(void);

// The frame's entities with one more per model, its frames picked for `time`
// (seconds): `entities` itself when there are none, else a copy. *num_entities
// is updated.
const entity_t *vkpt_q3_deform_entities(const entity_t *entities, int *num_entities, float time);

// The cluster one of these models' meshes lies in, for its light list; false
// for any other model.
bool vkpt_q3_deform_mesh_cluster(const model_t *model, const maliasmesh_t *mesh, int *cluster);

#endif // VKPT_Q3DEFORM_H
