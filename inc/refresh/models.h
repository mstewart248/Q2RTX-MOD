/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2008 Andrey Nazarov
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#ifndef MODELS_H
#define MODELS_H

//
// models.h -- common models manager
//

#include "system/hunk.h"
#include "common/error.h"

#define MOD_Malloc(size)    Hunk_Alloc(&model->hunk, size)

#define CHECK(x)    if (!(x)) { ret = Q_ERR(ENOMEM); goto fail; }

#define MAX_ALIAS_SKINS     32
#define MAX_ALIAS_VERTS     4096

typedef struct mspriteframe_s {
    int             width, height;
    int             origin_x, origin_y;
    struct image_s  *image;
} mspriteframe_t;

typedef enum
{
	MCLASS_REGULAR,
	MCLASS_EXPLOSION,
	MCLASS_FLASH,
	MCLASS_SMOKE,
    MCLASS_STATIC_LIGHT,
    MCLASS_FLARE
} model_class_t;

typedef struct
{
	vec3_t translate;
	quat_t rotate;
	vec3_t scale;
} iqm_transform_t;

typedef struct
{
	char name[MAX_QPATH];
	uint32_t first_frame;
	uint32_t num_frames;
	bool loop;
} iqm_anim_t;

// inter-quake-model
typedef struct
{
	uint32_t num_vertexes;
	uint32_t num_triangles;
	uint32_t num_frames;
	uint32_t num_meshes;
	uint32_t num_joints;
	uint32_t num_poses;
	uint32_t num_animations;
	struct iqm_mesh_s* meshes;

	uint32_t* indices;

	// vertex arrays
	float* positions;
	float* texcoords;
	float* normals;
	float* tangents;
	byte* colors;
    byte* blend_indices; // byte4 per vertex
	byte* blend_weights; // byte4 per vertex
	
	char* jointNames;
	int* jointParents;
	float* bindJoints; // [num_joints * 12]
	float* invBindJoints; // [num_joints * 12]
	iqm_transform_t* poses; // [num_frames * num_poses]
	float* bounds;
	
	iqm_anim_t* animations;
} iqm_model_t;

// inter-quake-model mesh
typedef struct iqm_mesh_s
{
	char name[MAX_QPATH];
	char material[MAX_QPATH];
	iqm_model_t* data;
	uint32_t first_vertex, num_vertexes;
	uint32_t first_triangle, num_triangles;
	uint32_t first_influence, num_influences;
} iqm_mesh_t;

typedef struct light_poly_s {
	float positions[9]; // 3x vec3_t
	vec3_t off_center;
	vec3_t color;
	struct pbr_material_s* material;
	int cluster;
	int style;
	float emissive_factor;
	int type;
	// For DYNLIGHT_SPOT only: which emission profile positions[4] and positions[5]
	// describe. See add_dlights() - the two profiles use those slots differently.
	int spot_emission_profile;
	// How much this light scatters into the volumetric medium, relative to how
	// much it lights surfaces. RTX Remix's per-light volumetricRadianceScale.
	// LIGHT_VOLUMETRIC_SCALE_UNSET means "nobody said", and copy_light() then
	// picks the default for this light's class (emissive surface / sky / dynamic).
	// EVERY site that creates a light_poly_t must set this - the struct is not
	// zero-initialised on any of the paths that build one on the stack.
	float volumetric_scale;
} light_poly_t;

typedef struct model_s {
    enum {
        MOD_FREE,
        MOD_ALIAS,
        MOD_SPRITE,
        MOD_EMPTY
    } type;
    char name[MAX_QPATH];
    int registration_sequence;
    memhunk_t hunk;

    // alias models
    int numframes;
    struct maliasframe_s *frames;
#if USE_REF == REF_GL || USE_REF == REF_VKPT
    int nummeshes;
    struct maliasmesh_s *meshes;
	model_class_t model_class;
#else
    int numskins;
    struct image_s *skins[MAX_ALIAS_SKINS];
    int numtris;
    struct maliastri_s *tris;
    int numsts;
    struct maliasst_s *sts;
    int numverts;
    int skinwidth;
    int skinheight;
#endif

    // sprite models
    struct mspriteframe_s *spriteframes;
	bool sprite_vertical;

	iqm_model_t* iqmData;

	int num_light_polys;
	light_poly_t* light_polys;
} model_t;

extern model_t      r_models[];
extern int          r_numModels;

extern int registration_sequence;

typedef struct entity_s entity_t;

// these are implemented in r_models.c
void MOD_FreeUnused(void);
void MOD_FreeAll(void);
void MOD_Init(void);
void MOD_Shutdown(void);

model_t *MOD_ForHandle(qhandle_t h);
qhandle_t R_RegisterModel(const char *name);

/*
Tracing a ray against an alias model's actual triangles.

The collision world knows nothing about model geometry: entity_state_t::solid
carries one packed axis-aligned box, so to every trace in the engine a soldier,
its corpse and a crate are the same rectangular prism.  That is fine for anything
that only has to STOP something, and useless for anything that has to be DRAWN
where it touches - a blood splat parked at a box contact hangs in mid-air a good
ten units clear of the body inside it.

This is the escape hatch, and it is deliberately NOT part of the collision
system: it reads per-frame renderer data, it costs orders of magnitude more than
a box test, and it is only worth paying for the handful of rays whose result the
player is going to look at.

A function pointer because maliasmesh_t is defined per backend and both backends
link into the same binary; MOD_LoadMD2 below is the same pattern.  NULL when the
active renderer has no implementation, so every caller must check.
*/
// The pose to test against.  Deliberately the DISCRETE current frame, with no
// oldframe/backlerp: the renderer's own interpolation moves every vertex every
// frame, which would defeat the pose cache behind this and rebuild a whole
// monster's geometry per ray.  Snapping to the current frame costs at most half
// an animation frame of placement error on a body that is moving anyway, and
// costs nothing at all on the case that matters most - a corpse, whose frame is
// frozen the moment it lands.
typedef struct {
    vec3_t  origin;
    vec3_t  angles;
    float   scale;      // 0 means 1, as everywhere else
    int     frame;
} mod_pose_t;

typedef bool (*mod_trace_mesh_t)(const model_t *model, const mod_pose_t *pose,
                                 const vec3_t start, const vec3_t end,
                                 float *out_frac, vec3_t out_normal);
extern mod_trace_mesh_t MOD_TraceMesh;

struct dmd2header_s;
int MOD_ValidateMD2(struct dmd2header_s *header, size_t length);

int MOD_LoadIQM_Base(model_t* mod, const void* rawdata, size_t length, const char* mod_name);
int MOD_LoadMD5_Base(model_t* mod, const void* rawdata, size_t length, const char* mod_name);
bool R_ComputeIQMTransforms(const iqm_model_t* model, const entity_t* entity, float* pose_matrices);

// these are implemented in [gl,sw]_models.c
typedef int (*mod_load_t)(model_t *, const void *, size_t, const char*);
extern int (*MOD_LoadMD2)(model_t *model, const void *rawdata, size_t length, const char* mod_name);
#if USE_MD3
extern int (*MOD_LoadMD3)(model_t *model, const void *rawdata, size_t length, const char* mod_name);
#endif
extern int(*MOD_LoadIQM)(model_t* model, const void* rawdata, size_t length, const char* mod_name);
extern int(*MOD_LoadMD5)(model_t* model, const void* rawdata, size_t length, const char* mod_name);
extern void (*MOD_Reference)(model_t *model);

#endif // MODELS_H
