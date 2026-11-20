/*
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

#ifndef __MATERIAL_H_
#define __MATERIAL_H_


#include <shared/shared.h>
#include <shared/list.h>
#include <refresh/refresh.h>

#define MAX_PBR_MATERIALS 8192

typedef struct image_s image_t;

//
// PBR material
//

typedef struct pbr_material_s {
	list_t entry;
	char name[MAX_QPATH];
	char filename_base[MAX_IMAGE_PATH];
	char filename_normals[MAX_IMAGE_PATH];
	char filename_emissive[MAX_IMAGE_PATH];
	char filename_mask[MAX_IMAGE_PATH];
	char filename_roughness[MAX_IMAGE_PATH];
	char filename_metallic[MAX_IMAGE_PATH];
	char filename_height[MAX_IMAGE_PATH];
	char source_matfile[MAX_QPATH];
	uint32_t source_line;
	int original_width;
	int original_height;
	image_t * image_base;
	image_t * image_normals;
	image_t * image_emissive;
	image_t * image_mask;
	// dedicated roughness/metallic maps. When present these win over the
	// legacy packing (roughness in base alpha, metallic in normals alpha).
	image_t * image_roughness;
	image_t * image_metallic;
	// Height map for parallax occlusion mapping, RTX-Remix style: white is the
	// surface, black is displace_in below it.
	image_t * image_height;
	float bump_scale;
	// Depth of the height map's black level below the surface, in UV units
	// (0.05 = 5% of one texture repeat) - the same unit as Remix's displaceIn,
	// so values carry over. Scaled globally by pt_pom_scale.
	float displace_in;
	// Height of the height map's white level ABOVE the surface, same units -
	// Remix's displaceOut. Height h lands at h * (in + out) - in, so with both
	// set the surface itself sits at h = in / (in + out).
	float displace_out;
	// displace_clamp 1: for a texture drawn ONCE on its face (a light fixture, a
	// sign), not tiled. POM treats everything outside the tile the hit lands in
	// as flat at the surface and keeps its result inside that tile. Without it a
	// grazing view traces round into the neighbouring copy of the texture, and a
	// lamp shows its lens twice. Default off, because tiled maps need the wrap.
	bool displace_clamp;
	// alpha_glass 1 (or "kind ALPHA_GLASS"): a GLASS material that is glass only
	// where its base texture's alpha is under a half - a window pane cut out of
	// a frame drawn on the same face (Quake III's textures/base_wall/glass_frame).
	// The rest is shaded as an ordinary surface, with the material's own maps.
	// Decided per pixel by the shaders (alpha_glass_solid, path_tracer_rgen.h);
	// apply_q3_shader sets it for an alpha-tested Quake III glass shader.
	bool alpha_glass;
	// caustic <s> (or "kind GLASSCAUSTIC" = GLASS + caustic 1): light from a
	// lamp INSIDE this glass is bent by the mesh's own vertex normals on its way
	// out, bunching up where the surface focuses it and thinning where it
	// spreads it (glass_caustic, path_tracer_rgen.h). 1 = real glass (IOR 1.52),
	// 2 = twice the bend, 0 = off. Needs pt_caustics 1. Shadow rays only.
	float glass_caustic;
	// caustic_ripple <s>: the light through this GLASS is thrown in moving
	// water ripples (the animated water normal map, as water's caustics), as if
	// the glass were rippling water - Matt's health orbs, 2026-11-16. Projected
	// out from the shape's centre of curvature, so a lamp the size of the glass
	// still throws sharp lines (glass_ripple, path_tracer_rgen.h). 1 = the
	// default network, 2 = stronger folds, 0 = off; the total light it lets
	// through stays the same. Separate from `caustic`, which it multiplies.
	// Needs pt_caustics 1. Shadow rays only.
	float caustic_ripple;
	float roughness_override;
	float metalness_factor;
	float emissive_factor;
	// light_factor: scales only the light the surface CASTS (its light polys),
	// not how bright it looks - emissive_factor and default_radiance scale both
	float light_factor;
	float specular_factor;
	float base_factor;
	uint32_t flags;
	int registration_sequence;
	int num_frames;
	int next_frame;
	bool light_styles;
	bool bsp_radiance;
	float default_radiance;
	imageflags_t image_flags;
	imagetype_t image_type;
	bool synth_emissive;
	int emissive_threshold;
	// One bit per .mat attribute (enum AttributeIndex in material.c) that this
	// definition stated ITSELF, as opposed to inheriting it or auto-detecting
	// it. Two things read it: the sidecar auto-detection in MAT_Find, which
	// only fills a texture slot the definition left unsaid, and
	// MAT_InheritScalars, which never overwrites a value the definition spelled
	// out.
	uint32_t specified_fields;
	// The same bits, for the attributes changed with 'mat <attribute> <value>'
	// since the material was loaded: what 'mat save' with no file name writes
	// back into the entry the material came from (material.c save_to_source_files).
	// Only ever set on a live material, never on a definition.
	uint32_t console_fields;
	// The material whose layout-independent tuning this one inherits, recorded
	// by MAT_InheritScalars - for an md5 skin, the classic .md2 skin it
	// replaces. Kept because 'mat reload' has to re-apply that inheritance and
	// no .mat file records it.
	char inherit_source[MAX_QPATH];
	// How much light from surfaces using this material scatters into the
	// volumetric medium, relative to how much it lights other surfaces - RTX
	// Remix's per-light volumetricRadianceScale, expressed per material because
	// that is the only per-fixture handle a Quake II map gives us.
	// LIGHT_VOLUMETRIC_SCALE_UNSET (-1) = fall back to pt_fog_scale_emissive.
	float volumetric_scale;
	// Which of the two split reflection/refraction fields supplies this
	// surface's DLSS-RR guide buffers, overriding pt_dlss_guide_field, in that
	// cvar's own numbering: 0 refraction, 1 brighter layer, 2 reflection,
	// 3 reflection-on-glass. -1 means the material said nothing and the cvar
	// decides, which is every material that does not name it.
	//
	// It exists because the cvar answers per RENDERER what is really a question
	// per SURFACE: whether a pane of glass reads as a mirror you look AT or a
	// window you look THROUGH. Its default of 3 says "mirror" for all glass,
	// which turns a clear window into mush - RR is told the pixel is the
	// reflected surface while the colour is mostly the room behind it.
	// See CHECKERBOARD_FLAG_GUIDE_SET in shader/constants.h.
	int dlss_guide_field;
	// pt_bounce_albedo_power for surfaces using this material: the power their
	// albedo is raised to when they pass light on to the next bounce, in the
	// menu slider's own numbers (1 = physical, lower = more bounce light). It
	// REPLACES the slider rather than scaling it. Not stated (<= 0, the default)
	// means the slider decides.
	float albedo_strength;
	// A Quake III shader drawn stage by stage (q3stages.c): index + 1 of its
	// program, 0 for none. Built by apply_q3_shader for shaders that animate or
	// draw from more than one image; cleared with the material.
	uint16_t q3_program;
	// A Quake III shader that only ADDS light over what is behind it (q3shader.c
	// q3_analyze "additive"): nothing of the surface itself shows, so its faces
	// get alpha 0 (bsp_mesh.c create_poly) and the primary ray goes straight
	// through, picking up only the glow.
	bool q3_additive;
} pbr_material_t;

extern pbr_material_t r_materials[MAX_PBR_MATERIALS];

// clears the material table, adds the mat command
void MAT_Init(void);

// unregisters the mat command
void MAT_Shutdown(void);

// resets all previously loaded wall materials and loads a map-specific material file
void MAT_ChangeMap(const char* map_name);

// finds or loads a material by name, which must have no extension
// all available textures will be initialized in the returned material
pbr_material_t* MAT_Find(const char* name, imagetype_t type, imageflags_t flags);

// copies the layout-independent tuning (brightness, roughness, kind, ...) of the
// material definition named (source_name) onto (mat). Textures are not copied.
void MAT_InheritScalars(pbr_material_t* mat, const char* source_name);

// registration sequence: update registration sequence of images used by the material
void MAT_UpdateRegistration(pbr_material_t * mat);

// Builds the name of a material instance: "<base name>#<8 hex digits>".
// One place so the loader, the lookup and the stub writer cannot disagree.
void MAT_InstanceName(const char* base_name, uint32_t instance_hash, char* out, size_t out_size);

// The material for one connected group of BSP faces, identified by the geometry
// hash bsp_mesh_t::prim_instance_hash carries, or NULL when no .mat section has
// claimed that group - which is the overwhelmingly common case, so this is a
// sorted lookup that fails fast and allocates nothing.
pbr_material_t* MAT_FindInstance(const pbr_material_t* base, uint32_t instance_hash);

// The Quake III shader a material draws (an instance's is its base's), or NULL
// when no script defines one.
const struct q3shader_s* MAT_Q3Shader(const pbr_material_t* mat);

// True if (mat)'s own definition or a 'mat kind' command set its kind, rather
// than it inheriting the kind from a base material or a default.
bool MAT_KindSpecified(const pbr_material_t* mat);

// True if (mat)'s roughness_override is its roughness all over, not a floor
// under its maps: no roughness map, normal map or metallic map to carry one.
bool MAT_RoughnessIsWhole(const pbr_material_t* mat);

// returns a material by index, if it's valid - NULL otherwise
pbr_material_t* MAT_ForIndex(int index);

// 'mat print's picture of the material's albedo (or another map) in the top-right
// corner; both renderers' R_EndFrame call it before drawing the 2D layer
void MAT_DrawPreview(void);
// The materials of the view weapon this frame draws, for 'mat view_weapon':
// the crosshair cannot reach the gun in your hands. vkpt_prepare_frame, both
// renderers.
void MAT_NoteViewWeapon(const refdef_t* fd);
// reload_textures (textures.c): the live materials whose maps include (image),
// as "a, b, ..." - how many; every live material with (filter) in its name and
// the image each map is; and what has to follow images re-read in place
int MAT_ImageUsers(const image_t* image, char* out, size_t size);
void MAT_ReportImages(const char* filter);
void MAT_ImagesReloaded(void);
// The index of a live material (one of r_materials), or -1 for a definition.
int MAT_IndexOf(const pbr_material_t* mat);
// 'mat <args>' on (mat), as if typed with it under the crosshair ('mat author')
void MAT_CommandFor(pbr_material_t* mat, const char* args);
// 'mat save': every changed material written into its .mat file
void MAT_SaveToSourceFiles(void);
// 'mat author' (mat_author.c): the command, with the crosshair's material, and
// its panel - both renderers' R_EndFrame, after MAT_DrawPreview
void MAT_AuthorCommand(pbr_material_t* crosshair);
void MAT_AuthorDraw(void);

// update material when a skin is applied
pbr_material_t* MAT_ForSkin(image_t * image_base);

// reset materials textures (call before loading a level)
int MAT_FreeUnused(void);

// replaces the material kind field with the given value
uint32_t MAT_SetKind(uint32_t material, uint32_t kind);

// tests if the material is of a given kind
bool MAT_IsKind(uint32_t material, uint32_t kind);

// synthesize 'emissive' image for a material, if necessary
void MAT_SynthesizeEmissive(pbr_material_t * mat);

// test if the material is one of the trapsnarent kinds (glass, water, ...)
bool MAT_IsTransparent(uint32_t material);

// test if the material has an alpha mask
bool MAT_IsMasked(uint32_t material);

#endif // __MATERIAL_H_
