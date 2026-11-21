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
    MCLASS_FLARE,
    // rogue's plasma / heat beam. The class exists only so the material can be
    // tagged MATERIAL_FLAG_PLAYER_BEAM and lose the primary-ray depth test to
    // the view weapon - see primary_rays.rgen.
    MCLASS_PLAYER_BEAM
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
	// [num_joints], or NULL: 1 = a joint that JUMPS between frames and is not
	// blended - a Quake muzzle flash (model_md5.c md5_snap_joint), R_ComputeIQMTransforms
	byte* jointSnap;
	// [num_frames * num_joints], or NULL: for each frame, the joint a snap joint
	// RIDES there (-1: none) and its 3x4 offset from that joint - a muzzle flash
	// on its barrel through the barrel's blended spin (model_md5.c MD5_SnapRiders)
	short* jointSnapRider;
	float* jointSnapOffset;     // [num_frames * num_joints * 12]
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
	// DYNLIGHT_WINDOW only (a stained glass tile's light, bsp_mesh.c): the
	// tile's u and v offsets and the glass's plane distance. positions[0..2]
	// then hold the tile's S and T axes and the glass's room-side normal, not
	// points - see WindowLight in shader/stained_glass.h. Only read for that
	// type, so no other site need set it.
	float window[3];
	// A Quake III program's glow (q3stages.c vkpt_q3_light_glow): copy_light
	// takes the light's colour from the program every frame - what the surface
	// shows at this moment, averaged over the triangle - instead of `color`,
	// which only stands in until the program's images are in memory. q3_uv is
	// the texture coordinate at each corner (1 = one repeat), q3_samples the
	// sample grid (n x n). Only read when `material` has a program, so the
	// sites that make lights without a material need not set it; bsp_mesh.c's
	// do, and the model-light copy carries it.
	bool q3_glow;
	uint8_t q3_samples;
	float q3_uv[6];
	// a still program's glow, worked out once (q3_glow_key = the program's
	// vkpt_q3_program_key when it was; 0 = not yet)
	uint32_t q3_glow_key;
	vec3_t q3_glow_rgb;
	// ... and its stage<N>_emissive_factor stages' glow, which the material's
	// emissive_factor does not scale
	vec3_t q3_glow_stages_rgb;
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
	// added to entity->skinnum before it indexes the skin list; nonzero only
	// for a model loaded in place of a merged one, see MOD_MergedModel
	int skin_base;
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
	bool sprite_oriented;	// Quake's SPR_ORIENTED: flat in the plane the entity's angles give (hipnotic's bullet holes)

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

/*
A HANDLE IS NOT ALWAYS AN INDEX INTO r_models, and the difference is what
made "MOD_ForHandle: bad model handle -63" a fatal error rather than a
returned NULL.  R_RegisterModel hands back ~N for a "*N" configstring - an
INLINE BSP MODEL, a brush that lives in the world geometry and has no
model_t of its own - so every door, lift and platform on the map reaches the
client as a NEGATIVE handle.  ~62 is -63.

Those handles travel through entity_t::model and cl.model_draw[] exactly like
the real ones, and the renderer dispatches on them with `model & 0x80000000`.
Any caller that skips that test and asks for the model_t used to die.

MOD_ForHandle answers NULL for one now - that IS the honest answer, the same
one an unloaded slot gets - and the macro carries the call site into the
message for the handles that really are corrupt, because the old text named
the model system and pointed nowhere near the caller that was at fault.
*/
model_t *MOD_ForHandle_(qhandle_t h, const char *file, int line);
#define MOD_ForHandle(h)    MOD_ForHandle_(h, __FILE__, __LINE__)
qhandle_t R_RegisterModel(const char *name);

// A model the renderer builds in memory instead of reading a file (vkpt
// q3deform.c): a free slot, named and registered for the map being loaded, of
// type MOD_ALIAS, its hunk begun with room for `hunk_size` bytes. The caller
// fills it with MOD_Malloc and ends the hunk; MOD_FreeUnused drops it with the
// map like any other. NULL if every slot is taken.
model_t *MOD_AllocProcedural(const char *name, size_t hunk_size);

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

// What a hit was. tested is set either way: false means the segment missed the
// pose's bounds and no triangle was looked at (callers budget only the tested).
typedef struct {
    float   frac;           // along start -> end
    vec3_t  normal;         // world space, interpolated
    // cl_blood_skin (11-15): where on the SKIN it landed, for painting blood there
    int     mesh;           // index into the model's meshes
    vec2_t  st;             // texture coordinate at the hit
    float   st_per_unit;    // texture-space distance per world unit, at the hit
    bool    tested;
} mod_trace_result_t;

typedef bool (*mod_trace_mesh_t)(const model_t *model, const mod_pose_t *pose,
                                 const vec3_t start, const vec3_t end,
                                 mod_trace_result_t *out);
extern mod_trace_mesh_t MOD_TraceMesh;

/*
BLOOD ON BODIES (cl_blood_skin, 11-15): a droplet that hits a monster, a corpse or
an item paints a splat into a small mask in that model's texture space, kept per
entity and mesh by the renderer (vkpt/blood_skin.c) and blended over the skin by
the material shader - so it follows the animation for nothing. The key is the
entity's entity_t.blood_key; a different model under the same key (a monster
gibbed into its head) starts the key afresh. NULL where the renderer has none.
*/
typedef struct {
    int         key;        // entity_t.blood_key of the body (entity number + 1)
    qhandle_t   model;      // the model it landed on
    int         mesh;       // mod_trace_result_t.mesh
    vec2_t      st;         // the centre, in texture space
    float       radius;     // in texture space
    float       strength;   // 0..1 at the centre
    int         color;      // as a particle's: palette index, or -1 for rgba
    color_t     rgba;
} mod_blood_splat_t;

typedef void (*mod_blood_skin_splat_t)(const mod_blood_splat_t *splat);
extern mod_blood_skin_splat_t MOD_BloodSkinSplat;

struct dmd2header_s;
int MOD_ValidateMD2(struct dmd2header_s *header, size_t length);

int MOD_LoadIQM_Base(model_t* mod, const void* rawdata, size_t length, const char* mod_name);
int MOD_LoadMD5_Base(model_t* mod, const void* rawdata, size_t length, const char* mod_name);

// The skin list of a rerelease .md5mesh as image names (model_md5.c), shared by
// every renderer: the RTX ones MAT_Find each path and inherit the classic
// material's tuning from (inherit), GL and software IMG_Find it. (skins) holds
// MAX_ALIAS_SKINS; the result is always at least 1.
typedef struct {
    char    path[MAX_QPATH];        // the image to draw
    char    inherit[MAX_QPATH];     // classic material to take tuning from; "" = none
} md5_skin_t;

int MD5_ResolveSkins(const char *mod_name, md5_skin_t *skins);

// The next MOD_LoadMD5_Base scales the whole model by (scale) about its origin
// and then moves it by (offset) - bind pose, joints and every frame - and
// drops the .md5anim's bounds; NULL (and scale 1) stops it. Quake 1's box
// stand-ins (model_q1.c) are drawn 1.5 times as big and stand centred where
// the box had its corner.
void MD5_SetLoadOffset(const vec_t *offset);
void MD5_SetLoadScale(float scale);

// Quake 1's model formats (model_q1.c), each turned into one every renderer
// loads: .mdl -> .md2 (or its rerelease .md5mesh), .spr -> .sp2, a box's
// maps/b_*.bsp -> .md3 (or its .md5mesh). MOD_Q1Build's (*data) is Z_Free'd
// by the caller and loaded under (load_name), an md5 scaled by (md5_scale)
// and moved by (md5_offset); (*sprite_type) is a sprite's Quake orientation,
// -1 for anything else.
bool MOD_Q1IsModel(const char *name);
int MOD_Q1Build(const char *name, bool md5, void **data, size_t *length,
                char *load_name, size_t size, vec3_t md5_offset, float *md5_scale, int *sprite_type);
// the 8-bit pixels images.c turns into an .mdl's skins and a sprite's frames,
// Z_Free'd by the caller; a box's wall texture, kept by model_q1.c
byte *MOD_Q1ModelSkin(const char *path, int skin, int frame, int *width, int *height);
byte *MOD_Q1SpriteFrame(const char *path, int frame, int *width, int *height);
const byte *MOD_Q1ItemMiptex(const char *name, int *width, int *height);

// GL and software: skin a skeletal model (model->iqmData) on the CPU for one
// entity. Writes num_vertexes positions and normals, model space, from
// (pose_matrices) as R_ComputeIQMTransforms fills them. (normals) may be NULL.
void MD5_SkinVertices(const iqm_model_t *model, const float *pose_matrices,
                      uint32_t first_vertex, uint32_t num_vertexes,
                      float *positions, float *normals);
// ...and the box one animation frame fits in, for culling
void MD5_FrameBounds(const iqm_model_t *model, int frame, vec3_t mins, vec3_t maxs);
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
