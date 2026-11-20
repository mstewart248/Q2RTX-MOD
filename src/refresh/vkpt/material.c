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

#include "material.h"
#include "vkpt.h"
#include "q3stages.h"
#include <common/prompt.h>
#include <common/q3shader.h>
#include <format/bsp3.h>
#include <client/keys.h>        // Key_GetDest: the preview stays while the console is open
#include <system/system.h>      // Sys_Milliseconds

#include <stdlib.h>
#include <string.h>
#include <assert.h>


extern cvar_t *cvar_pt_surface_lights_fake_emissive_algo;
extern cvar_t* cvar_pt_surface_lights_threshold;
extern cvar_t *cvar_pt_enable_surface_lights;
extern cvar_t *cvar_pt_enable_surface_lights_warp;
extern cvar_t *cvar_pt_bsp_radiance_scale;

extern void CL_PrepRefresh(void);

pbr_material_t r_materials[MAX_PBR_MATERIALS];

/* The DEFINITIONS every .mat file in materials/ makes, live or not. Not bound by
   MAX_PBR_MATERIALS, which is what one frame can draw (the GPU's material
   table): baseq2.mat and rerelease.mat already define some 4,600, and
   baseq3/materials/quake3.mat stubs every Quake III texture - at 8192 shared,
   whichever file listed last lost its entries, and the files list
   alphabetically, so quake3.mat would have cut rerelease.mat short. */
#define MAX_PBR_MATERIAL_DEFS 16384
static pbr_material_t r_global_materials[MAX_PBR_MATERIAL_DEFS];
static pbr_material_t r_map_materials[MAX_PBR_MATERIALS];
static uint32_t num_global_materials = 0;
// remembered so 'mat reload' can re-read the per-map overrides
static char current_map_name[MAX_QPATH];
static uint32_t num_map_materials = 0;

#define RMATERIALS_HASH 256
static list_t r_materialsHash[RMATERIALS_HASH];

#define RELOAD_MAP		1
#define RELOAD_EMISSIVE	2

static uint32_t load_material_file(const char* file_name, pbr_material_t* dest, uint32_t max_items);
static void load_global_materials(void);
static void material_command(void);
static void material_completer(genctx_t* ctx, int argnum);

static int compare_materials(const void* a, const void* b)
{
	const pbr_material_t* ma = a;
	const pbr_material_t* mb = b;

	int names = strcmp(ma->name, mb->name);
	if (names != 0)
		return names;

	int sources = strcmp(ma->source_matfile, mb->source_matfile);
	if (sources != 0)
		return sources;

	return (int)ma->source_line - (int)mb->source_line;
}

static void sort_and_deduplicate_materials(pbr_material_t* first, uint32_t* pCount)
{
	const uint32_t count = *pCount;
	
	if (count == 0)
		return;

	// sort the materials by name, then by source file, then by line number
	qsort(first, count, sizeof(pbr_material_t), compare_materials);

	// deduplicate the materials with the same name; latter entries override former ones
	uint32_t write_ptr = 0;
	
	for (uint32_t read_ptr = 0; read_ptr < count; read_ptr++) {
		// if there is a next entry and its name is the same, skip the current entry
		if ((read_ptr + 1 < count) && strcmp(first[read_ptr].name, first[read_ptr + 1].name) == 0)
			continue;

		// copy the input entry to the output entry if they are not the same
		if (read_ptr != write_ptr)
			memcpy(first + write_ptr, first + read_ptr, sizeof(pbr_material_t));

		++write_ptr;
	}

	if (write_ptr < count) {
		// if we've removed some entries, clear the garbage at the end
		memset(first + write_ptr, 0, sizeof(pbr_material_t) * (count - write_ptr));

		// return the new count
		*pCount = write_ptr;
	}
}

// Returns whether the current game is a custom game (not baseq2)
static qboolean is_game_custom(void)
{
	return fs_game->string[0] && strcmp(fs_game->string, BASEGAME) != 0;
}

void MAT_Init()
{
	cmdreg_t commands[2];
	commands[0].name = "mat";
	commands[0].function = (xcommand_t)&material_command;
	commands[0].completer = &material_completer;
	commands[1].name = NULL;
	Cmd_Register(commands);
	
	memset(r_materials, 0, sizeof(r_materials));
	memset(r_global_materials, 0, sizeof(r_global_materials));
	memset(r_map_materials, 0, sizeof(r_map_materials));
	num_global_materials = 0;
	num_map_materials = 0;
	// nothing is loaded now, so the next MAT_ChangeMap must load, not skip
	current_map_name[0] = 0;

	// initialize the hash table
	for (int i = 0; i < RMATERIALS_HASH; i++)
	{
		List_Init(r_materialsHash + i);
	}

	load_global_materials();
}

// Parse every materials/*.mat into the global definition table. Split out of
// MAT_Init so that "mat reload" can re-read them without restarting the game.
static void load_global_materials(void)
{
	memset(r_global_materials, 0, sizeof(r_global_materials));
	num_global_materials = 0;
	vkpt_q3_stage_override_clear(Q3SS_SCOPE_GLOBAL);
	// and what was typed in the console: unsaved, it goes the way of the
	// attributes typed there (q3stages.h)
	vkpt_q3_stage_override_clear(Q3SS_SCOPE_CONSOLE);

	// find all *.mat files in the root
	int num_files;
	void** list = FS_ListFiles("materials", ".mat", 0, &num_files);
	
	for (int i = 0; i < num_files; i++) {
		char* file_name = list[i];
		char buffer[MAX_QPATH];
		Q_concat(buffer, sizeof(buffer), "materials/", file_name);
		
		int mat_slots_available = MAX_PBR_MATERIAL_DEFS - num_global_materials;
		if (mat_slots_available > 0) {
			uint32_t count = load_material_file(buffer, r_global_materials + num_global_materials,
				mat_slots_available);
			num_global_materials += count;
			
			Com_Printf("Loaded %d materials from %s\n", count, buffer);
		}
		else {
			Com_WPrintf("Coundn't load materials from %s: no free slots.\n", buffer);
		}
		
		Z_Free(file_name);
	}
	Z_Free(list);

	sort_and_deduplicate_materials(r_global_materials, &num_global_materials);
}

void MAT_Shutdown()
{
	Cmd_RemoveCommand("mat");
	vkpt_q3_light_cache_clear();
}

static void MAT_SetIndex(pbr_material_t* mat)
{
	uint32_t mat_index = (uint32_t)(mat - r_materials);
	mat->flags = (mat->flags & ~MATERIAL_INDEX_MASK) | (mat_index & MATERIAL_INDEX_MASK);
	mat->next_frame = mat_index;
}

// Remix's default displaceIn, so a bare _height sidecar looks the way it
// does there.
#define MAT_DEFAULT_DISPLACE_IN 0.05f

static void MAT_Reset(pbr_material_t * mat)
{
	memset(mat, 0, sizeof(pbr_material_t));
	mat->bump_scale = 1.0f;
	mat->displace_in = MAT_DEFAULT_DISPLACE_IN;
	mat->roughness_override = -1.0f;
	mat->metalness_factor = 1.f;
	mat->emissive_factor = 1.f;
	mat->light_factor = 1.f;
	mat->specular_factor = 1.f;
	mat->base_factor = 1.f;
	mat->light_styles = true;
	mat->bsp_radiance = true;
	/* Treat absence of SURF_LIGHT flag as "fully emissive" by default.
	 * Typically works well with explicit emissive image. */
	mat->default_radiance = 1.f;
	mat->flags = MATERIAL_KIND_REGULAR;
	mat->num_frames = 1;
	mat->emissive_threshold = cvar_pt_surface_lights_threshold->integer;
	// negative means "not stated", so the class default in vertex_buffer.c wins
	mat->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
	// negative means "not stated", so pt_dlss_guide_field decides
	mat->dlss_guide_field = -1;
	// negative means "not stated", so pt_bounce_albedo_power decides
	mat->albedo_strength = -1.f;
}

//
// material kinds (translations between names & bit flags)
//

static struct MaterialKind {
	const char * name;
	uint32_t flag;
} materialKinds[] = {
	{"INVALID", MATERIAL_KIND_INVALID},
	{"REGULAR", MATERIAL_KIND_REGULAR},
	{"CHROME", MATERIAL_KIND_CHROME},
	{"GLASS", MATERIAL_KIND_GLASS},
	{"WATER", MATERIAL_KIND_WATER},
	{"LAVA", MATERIAL_KIND_LAVA},
	{"SKY", MATERIAL_KIND_SKY},
	{"SLIME", MATERIAL_KIND_SLIME},
	{"INVISIBLE", MATERIAL_KIND_INVISIBLE},
	{"SCREEN", MATERIAL_KIND_SCREEN},
	{"CAMERA", MATERIAL_KIND_CAMERA},
	// These three existed as kinds the renderer understands but had no name
	// here, so a .mat could not ask for them - and getMaterialKind used to
	// answer REGULAR for a name it did not know, so asking silently got you
	// a plain opaque surface with no diagnostic at all.
	{"TRANSPARENT", MATERIAL_KIND_TRANSPARENT},
	{"TRANSP_MODEL", MATERIAL_KIND_TRANSP_MODEL},
	{"CHROME_MODEL", MATERIAL_KIND_CHROME_MODEL},
};

static int nMaterialKinds = sizeof(materialKinds) / sizeof(struct MaterialKind);

// Returns MATERIAL_KIND_UNKNOWN_NAME for a name that is not in the table.
// It used to return MATERIAL_KIND_REGULAR, which is non-zero, so the caller's
// "unknown material kind" branch was unreachable and every misspelt or
// unregistered kind quietly became an ordinary opaque surface.
#define MATERIAL_KIND_UNKNOWN_NAME 0xffffffffu
static uint32_t getMaterialKind(const char * kindname)
{
	for (int i = 0; i < nMaterialKinds; ++i)
		if (Q_stricmp(kindname, materialKinds[i].name) == 0)
			return materialKinds[i].flag;
	return MATERIAL_KIND_UNKNOWN_NAME;
}

static const char * getMaterialKindName(uint32_t flag)
{
	for (int i = 0; i < nMaterialKinds; ++i)
		if ((flag & MATERIAL_KIND_MASK) == materialKinds[i].flag)
			return materialKinds[i].name;
	return NULL;
}

// (src) without its extension, cut to fit (size) - a texture path can be
// longer than a material name (MAX_IMAGE_PATH), and 'mat print' takes anything
static size_t truncate_extension(char const* src, char* dest, size_t size)
{
	size_t len = strlen(src);

	if (len > 4 && src[len - 4] == '.')
		len -= 4;

	Q_strlcpy(dest, src, min(len + 1, size));

	return len;
}

static pbr_material_t* allocate_material(void)
{
	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		pbr_material_t* mat = r_materials + i;
		if (!mat->registration_sequence)
			return mat;
	}

	Com_Error(ERR_FATAL, "Couldn't allocate a new material: insufficient r_materials slots.\n"
		"Increase MAX_PBR_MATERIALS and rebuild the engine.\n");
	return NULL;
}

static pbr_material_t* find_material(const char* name, uint32_t hash, pbr_material_t* first, uint32_t count)
{
	pbr_material_t* mat;
	
	LIST_FOR_EACH(pbr_material_t, mat, &r_materialsHash[hash], entry)
	{
		if (!mat->registration_sequence)
			continue;

		if (!strcmp(name, mat->name))
			return mat;
	}

	return NULL;
}

static pbr_material_t* find_material_sorted(const char* name, pbr_material_t* first, uint32_t count)
{
	// binary search in a sorted table: global and per-map material dictionaries
	int left = 0, right = (int)count - 1;

	while (left <= right)
	{
		int middle = (left + right) / 2;
		pbr_material_t* mat = first + middle;

		int cmp = strcmp(name, mat->name);

		if (cmp < 0)
			right = middle - 1;
		else if (cmp > 0)
			left = middle + 1;
		else
			return mat;
	}

	return NULL;
}

/* Where a MATERIAL INSTANCE's suffix starts - the '#' of the "#%08x" that
   MAT_InstanceName appends - or NULL if (name) is not an instance.

   Only a '#' followed by exactly eight hex digits at the very end counts. Any
   '#' used to: the rerelease's own textures/tomf/#lava_tf1 then read as an
   instance, and its name was cut down to "textures/tomf/". */
static const char* instance_separator(const char* name)
{
	size_t len = strlen(name);

	if (len < 9 || name[len - 9] != '#')
		return NULL;

	for (size_t i = len - 8; i < len; i++)
	{
		char c = Q_tolower(name[i]);
		if (!Q_isdigit(c) && (c < 'a' || c > 'f'))
			return NULL;
	}

	return name + len - 9;
}

// the live material (name) is an instance OF, or NULL if it is not an instance
// or that material is not registered
static pbr_material_t* find_instance_base(const char* name)
{
	const char* sep = instance_separator(name);

	if (!sep)
		return NULL;

	char base_name[MAX_QPATH];
	Q_strlcpy(base_name, name, min(sizeof(base_name), (size_t)(sep - name) + 1));

	return find_material(base_name, Com_HashString(base_name, RMATERIALS_HASH), r_materials, MAX_PBR_MATERIALS);
}

enum AttributeIndex
{
	MAT_BUMP_SCALE,
	MAT_ROUGHNESS_OVERRIDE,
	MAT_METALNESS_FACTOR,
	MAT_EMISSIVE_FACTOR,
	MAT_KIND,
	MAT_IS_LIGHT,
	MAT_BASE_FACTOR,
	MAT_TEXTURE_BASE,
	MAT_TEXTURE_NORMALS,
	MAT_TEXTURE_EMISSIVE,
	MAT_LIGHT_STYLES,
	MAT_BSP_RADIANCE,
	MAT_DEFAULT_RADIANCE,
	MAT_TEXTURE_MASK,
	MAT_SYNTH_EMISSIVE,
	MAT_EMISSIVE_THRESHOLD,
	MAT_SPECULAR_FACTOR,
	MAT_TEXTURE_ROUGHNESS,
	MAT_TEXTURE_METALLIC,
	MAT_VOLUMETRIC_SCALE,
	MAT_CURVED_WATER,
	MAT_DLSS_GUIDE_FIELD,
	MAT_TEXTURE_HEIGHT,
	MAT_DISPLACE_IN,
	MAT_DISPLACE_OUT,
	MAT_ALBEDO_STRENGTH,
	MAT_DOUBLE_SIDED,
	MAT_DISPLACE_CLAMP,
	MAT_ALPHA_GLASS,
	MAT_CAUSTIC,
	MAT_LIGHT_FACTOR,
	MAT_CAUSTIC_RIPPLE,
};
enum AttributeType { ATTR_BOOL, ATTR_FLOAT, ATTR_STRING, ATTR_INT };

static struct MaterialAttribute {
	enum AttributeIndex index;
	const char* name;
	enum AttributeType type;
} c_Attributes[] = {
	{MAT_BUMP_SCALE, "bump_scale", ATTR_FLOAT},
	{MAT_ROUGHNESS_OVERRIDE, "roughness_override", ATTR_FLOAT},
	{MAT_METALNESS_FACTOR, "metalness_factor", ATTR_FLOAT},
	{MAT_EMISSIVE_FACTOR, "emissive_factor", ATTR_FLOAT},
	{MAT_KIND, "kind", ATTR_STRING},
	{MAT_IS_LIGHT, "is_light", ATTR_BOOL},
	{MAT_BASE_FACTOR, "base_factor", ATTR_FLOAT},
	{MAT_TEXTURE_BASE, "texture_base", ATTR_STRING},
	{MAT_TEXTURE_NORMALS, "texture_normals", ATTR_STRING},
	{MAT_TEXTURE_EMISSIVE, "texture_emissive", ATTR_STRING},
	{MAT_LIGHT_STYLES, "light_styles", ATTR_BOOL},
	{MAT_BSP_RADIANCE, "bsp_radiance", ATTR_BOOL},
	{MAT_DEFAULT_RADIANCE, "default_radiance", ATTR_FLOAT},
	{MAT_TEXTURE_MASK, "texture_mask", ATTR_STRING},
	{MAT_SYNTH_EMISSIVE, "synth_emissive", ATTR_BOOL},
	{MAT_EMISSIVE_THRESHOLD, "emissive_threshold", ATTR_INT},
	{MAT_SPECULAR_FACTOR, "specular_factor", ATTR_FLOAT},
	{MAT_TEXTURE_ROUGHNESS, "texture_roughness", ATTR_STRING},
	{MAT_TEXTURE_METALLIC, "texture_metallic", ATTR_STRING},
	{MAT_VOLUMETRIC_SCALE, "volumetric_scale", ATTR_FLOAT},
	{MAT_CURVED_WATER, "curved_water", ATTR_BOOL},
	{MAT_DLSS_GUIDE_FIELD, "dlss_guide_field", ATTR_INT},
	{MAT_TEXTURE_HEIGHT, "texture_height", ATTR_STRING},
	{MAT_DISPLACE_IN, "displace_in", ATTR_FLOAT},
	{MAT_DISPLACE_OUT, "displace_out", ATTR_FLOAT},
	{MAT_ALBEDO_STRENGTH, "albedo_strength", ATTR_FLOAT},
	{MAT_DOUBLE_SIDED, "double_sided", ATTR_BOOL},
	{MAT_DISPLACE_CLAMP, "displace_clamp", ATTR_BOOL},
	{MAT_ALPHA_GLASS, "alpha_glass", ATTR_BOOL},
	{MAT_CAUSTIC, "caustic", ATTR_FLOAT},
	// the light the surface casts, times this; how it looks is untouched (Matt,
	// 2026-11-16: q3dm0's purple band - "I want it to cast light but I want the
	// texture to look the same"). Live: copy_light reads it every frame.
	{MAT_LIGHT_FACTOR, "light_factor", ATTR_FLOAT},
	{MAT_CAUSTIC_RIPPLE, "caustic_ripple", ATTR_FLOAT},
};

static int c_NumAttributes = sizeof(c_Attributes) / sizeof(struct MaterialAttribute);

// pbr_material_t::specified_fields is one bit per entry of c_Attributes, so the
// table has to stay inside a uint32_t. caustic_ripple took the LAST bit
// (2026-11-16): the next attribute needs specified_fields and console_fields
// widened to uint64_t and every `1u << attr` made 64-bit.
static_assert(sizeof(c_Attributes) / sizeof(struct MaterialAttribute) <= 32,
	"too many material attributes for pbr_material_t::specified_fields");

// true if this material's definition stated (attr) itself, rather than leaving
// it at the default for something else to fill in
#define MAT_SPECIFIED(mat, attr) (((mat)->specified_fields & (1u << (attr))) != 0)

/* A TEXTURE SLOT CLEARED ON PURPOSE. "texture_roughness 0", "texture_roughness"
   with nothing after it, "texture_roughness \"\"" in a .mat file (which has no
   quoting, so the two quote marks arrive as they are), or 'mat texture_roughness
   ""' in the console: no map, and nothing may fill the slot in later - not the
   <name>_rough sidecar, not a player skin's own map, not a Quake III shader's
   solid stand-in. Only "0" used to count, so an empty value was skipped as a
   line with no value (the file) or looked up as a texture called "" (the
   console), and the map that was there stayed or came back (Matt, 2026-11-17). */
static bool MAT_TextureValueClears(const char* value)
{
	return !value[0] || !strcmp(value, "0") || !strcmp(value, "\"\"");
}

// a texture_* attribute: its value is an image path, or a clear
static bool is_texture_attribute(enum AttributeIndex index)
{
	switch (index)
	{
	case MAT_TEXTURE_BASE: case MAT_TEXTURE_NORMALS: case MAT_TEXTURE_EMISSIVE: case MAT_TEXTURE_MASK:
	case MAT_TEXTURE_ROUGHNESS: case MAT_TEXTURE_METALLIC: case MAT_TEXTURE_HEIGHT:
		return true;
	default:
		return false;
	}
}

// the slot (attr) that (mat) states has no texture at all: see MAT_TextureValueClears
#define MAT_CLEARED(mat, attr, filename) (MAT_SPECIFIED(mat, attr) && !(mat)->filename[0])

/* THE SOLID STAND-INS, BY NAME. A Quake III mirror or chrome with no maps of
   its own gets one-colour images made for it (apply_q3_shader, q3_solid_slot):
   "<its image>*<tag>.wal", which `mat print` shows like any other path. There
   is no such file. Typed into another entry, the '*' was taken for the
   material's own name - 'mat texture_metallic ...chrome_metal*M1.wal' on
   base_trim/pewter looked for "chrome_metalpewterM1.wal" - and nothing loaded
   (Matt, 2026-11-19). A texture value ending in one of these tags now keeps
   its '*' and is that solid colour (load_solid_by_name). */
static const struct {
	const char* tag;
	byte        r, g, b;
} c_SolidTags[] = {
	{ "W",   255, 255, 255 },  // a mirror's base
	{ "C",   230, 230, 230 },  // chrome's base
	{ "R0",  0,   0,   0   },  // a mirror's roughness
	{ "R20", 20,  20,  20  },  // chrome's roughness
	{ "M1",  255, 255, 255 },  // the metallic of both
};

// the c_SolidTags entry (name) ends in as "*<tag>.wal", or -1
static int solid_tag_index(const char* name)
{
	const char* star = strrchr(name, '*');
	if (!star)
		return -1;
	for (int i = 0; i < (int)q_countof(c_SolidTags); i++)
	{
		size_t len = strlen(c_SolidTags[i].tag);
		if (!Q_strncasecmp(star + 1, c_SolidTags[i].tag, len) && !Q_stricmp(star + 1 + len, ".wal"))
			return i;
	}
	return -1;
}

// (name) is a solid stand-in's: the one already made, or a new one in (mat)'s
// base picture's place. R_NOTEXTURE if (name) is no such name, or there is
// nothing to make one from yet.
static image_t* load_solid_by_name(const char* name, const pbr_material_t* mat, imagetype_t type)
{
	int tag = solid_tag_index(name);
	if (tag < 0)
		return R_NOTEXTURE;
	image_t* solid = vkpt_solid_texture_named(mat->image_base, type, name,
		c_SolidTags[tag].r, c_SolidTags[tag].g, c_SolidTags[tag].b);
	return solid ? solid : R_NOTEXTURE;
}

static void set_material_texture(pbr_material_t* mat, const char* svalue, char mat_texture_path[MAX_IMAGE_PATH],
	image_t** mat_image, imageflags_t flags, bool from_console)
{
	if (MAT_TextureValueClears(svalue)) {
		mat_texture_path[0] = 0;
		*mat_image = NULL;
	}
	else if (from_console) {
		// the file typed, not an image already loaded from its .jpg/.png twin
		// (or another material's solid stand-in, which is no file: c_SolidTags)
		image_t* image = solid_tag_index(svalue) >= 0 ? load_solid_by_name(svalue, mat, mat->image_type)
			: IMG_FindFile(svalue, mat->image_type, flags | (mat->image_flags & IF_SRC_MASK));
		if (image != R_NOTEXTURE) {
			Q_strlcpy(mat_texture_path, svalue, MAX_IMAGE_PATH);
			*mat_image = image;
		}
		else
			Com_WPrintf("Cannot load texture '%s'\n", svalue);
	}
	else {
		Q_strlcpy(mat_texture_path, svalue, MAX_IMAGE_PATH);
	}
}

static int set_material_attribute(pbr_material_t* mat, const char* attribute, const char* value,
	const char* sourceFile, uint32_t lineno, unsigned int* reload_flags)
{
	assert(mat);

	// valid attribute-value pairs

	if (attribute == NULL || value == NULL)
		return Q_ERR_FAILURE;

	struct MaterialAttribute const* t = NULL;
	for (int i = 0; i < c_NumAttributes; ++i)
	{
		if (strcmp(attribute, c_Attributes[i].name) == 0)
			t = &c_Attributes[i];
	}
	
	if (!t)
	{
		if (sourceFile)
			Com_EPrintf("%s:%d: unknown material attribute '%s'\n", sourceFile, lineno, attribute);
		else
			Com_EPrintf("Unknown material attribute '%s'\n", attribute);
		return Q_ERR_FAILURE;
	}

	char svalue[MAX_IMAGE_PATH];

	float fvalue = 0.f; bool bvalue = false;
	int ivalue = 0;
	switch (t->type)
	{
	case ATTR_BOOL:   bvalue = atoi(value) == 0 ? false : true; break;
	case ATTR_FLOAT:  fvalue = (float)atof(value); break;
	case ATTR_STRING: {
		char* asterisk = strchr(value, '*');

		// another material's solid stand-in, named as `mat print` shows it:
		// its '*' is part of the name, not this material's (see c_SolidTags)
		if (asterisk && is_texture_attribute(t->index) && solid_tag_index(value) >= 0)
			asterisk = NULL;

		/* A path that does not fit used to be cut short here and fail later as
		   "Invalid quake path" on a name that was never in the file - "..._light."
		   for "..._light.png". Say so, and keep what the material had. Texture
		   paths may be MAX_IMAGE_PATH (127 characters) now, not the engine's
		   MAX_QPATH (63) - materialDev/ names ran past that. */
		size_t needed = strlen(value);
		if (asterisk) {
			const char* slash = strrchr(mat->name, '/');
			needed += strlen(slash ? slash + 1 : mat->name) - 1;
		}
		if (needed >= sizeof(svalue)) {
			if (sourceFile)
				Com_EPrintf("%s:%d: %s %s: the path is %u characters, and can be %d at most - shorten the file name\n",
					sourceFile, lineno, attribute, value, (unsigned)needed, MAX_IMAGE_PATH - 1);
			else
				Com_EPrintf("%s %s: the path is %u characters, and can be %d at most - shorten the file name\n",
					attribute, value, (unsigned)needed, MAX_IMAGE_PATH - 1);
			return Q_ERR(ENAMETOOLONG);
		}

		if (asterisk) {
			// get the base name of the material, i.e. without the path
			// material names have no extensions, so no need to remove that
			char* slash = strrchr(mat->name, '/');
			char* mat_base = slash ? slash + 1 : mat->name;

			// concatenate: the value before the asterisk, material base name, the rest of the value
			Q_strlcpy(svalue, value, min(asterisk - value + 1, sizeof(svalue)));
			Q_strlcat(svalue, mat_base, sizeof(svalue));
			Q_strlcat(svalue, asterisk + 1, sizeof(svalue));
		}
		else
			Q_strlcpy(svalue, value, sizeof(svalue));
		break;
	case ATTR_INT:
		ivalue = atoi(value);
		break;
	}
	default:
		assert(!"unknown PBR MAT attribute attribute type");
	}

	// set material

	switch (t->index)
	{
	case MAT_BUMP_SCALE: mat->bump_scale = fvalue; break;
	case MAT_DISPLACE_IN: mat->displace_in = fvalue; break;
	case MAT_DISPLACE_OUT: mat->displace_out = fvalue; break;
	case MAT_DISPLACE_CLAMP: mat->displace_clamp = bvalue; break;
	// glass only where the base texture is see-through - see material.h
	case MAT_ALPHA_GLASS: mat->alpha_glass = bvalue; break;
	// light inside this glass bent by the mesh normals - see material.h
	case MAT_CAUSTIC: mat->glass_caustic = max(0.f, min(fvalue, 15.f)); break;
	// that light thrown in moving water ripples - see material.h
	case MAT_CAUSTIC_RIPPLE: mat->caustic_ripple = max(0.f, min(fvalue, 4.f)); break;
	case MAT_VOLUMETRIC_SCALE: mat->volumetric_scale = fvalue; break;
	case MAT_ROUGHNESS_OVERRIDE: mat->roughness_override = fvalue; break;
	case MAT_METALNESS_FACTOR: mat->metalness_factor = fvalue; break;
	case MAT_EMISSIVE_FACTOR: mat->emissive_factor = fvalue; break;
	case MAT_LIGHT_FACTOR: mat->light_factor = fvalue; break;
	case MAT_KIND: {
		// "kind ALPHA_GLASS": shorthand for kind GLASS + alpha_glass 1. There is
		// no kind of its own to give it - the 4-bit kind field is full - so it is
		// glass with a per-pixel test, and prints and saves as the two lines.
		// "kind GLASSCAUSTIC" likewise: GLASS + caustic 1, unless an earlier
		// line of the same entry already gave its own caustic strength.
		const bool alpha_glass_kind = !Q_stricmp(svalue, "ALPHA_GLASS");
		const bool caustic_kind = !Q_stricmp(svalue, "GLASSCAUSTIC") || !Q_stricmp(svalue, "GLASS_CAUSTIC");
		uint32_t kind = (alpha_glass_kind || caustic_kind) ? MATERIAL_KIND_GLASS : getMaterialKind(svalue);
		if (alpha_glass_kind)
		{
			mat->alpha_glass = true;
			mat->specified_fields |= 1u << MAT_ALPHA_GLASS;
		}
		if (caustic_kind && !MAT_SPECIFIED(mat, MAT_CAUSTIC))
		{
			mat->glass_caustic = 1.f;
			mat->specified_fields |= 1u << MAT_CAUSTIC;
		}
		if (kind != MATERIAL_KIND_UNKNOWN_NAME)
			mat->flags = MAT_SetKind(mat->flags, kind);
		else
		{
			if (sourceFile)
				Com_EPrintf("%s:%d: unknown material kind '%s'\n", sourceFile, lineno, svalue);
			else
				Com_EPrintf("Unknown material kind '%s'\n", svalue);
			return Q_ERR_FAILURE;
		}
		if (reload_flags) *reload_flags |= RELOAD_MAP;
	} break;
	case MAT_IS_LIGHT:
		mat->flags = bvalue == true ? mat->flags | MATERIAL_FLAG_LIGHT : mat->flags & ~(MATERIAL_FLAG_LIGHT);
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	// Says this WATER/SLIME surface is a closed shape rather than a flat brush
	// face, so ALL of it takes the force-field (GLASS) path rather than only
	// the part whose normals face sideways - which is both uniform and the
	// only way a water surface shows its own texture.
	// See MATERIAL_FLAG_CURVED_WATER in shader/constants.h.
	case MAT_CURVED_WATER:
		mat->flags = bvalue == true ? mat->flags | MATERIAL_FLAG_CURVED_WATER : mat->flags & ~(MATERIAL_FLAG_CURVED_WATER);
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	// No back-face culling for MODELS drawn with this material, for meshes that
	// are open shells which camera rays otherwise see straight through from
	// inside. The Q2RTX super shotgun pickup (g_shotg2/tris.md3, which borrows
	// v_shotg2's skin) is the case that needed it: its barrels are tubes with no
	// inner wall, built for a first-person gun nobody looks down. See
	// MAT_IsMasked for how. RELOAD_MAP because a static model sorts its meshes
	// into BLASes when it is uploaded, and only a map reload re-uploads it.
	case MAT_DOUBLE_SIDED:
		mat->flags = bvalue == true ? mat->flags | MATERIAL_FLAG_DOUBLE_SIDED : mat->flags & ~(MATERIAL_FLAG_DOUBLE_SIDED);
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	// Overrides pt_dlss_guide_field for surfaces using this material. Only
	// consulted where the renderer splits a pixel into a reflection and a
	// refraction field - glass, water, slime, chrome - and ignored everywhere
	// else. Same numbering as the cvar, so whichever value looked right in the
	// console is the value to write here.
	case MAT_DLSS_GUIDE_FIELD:
		// 0..5, matching pt_dlss_guide_field. 4 is the Fresnel-weighted mix and 5 the
		// mirror split - both are per-surface answers, which is the whole reason this
		// material key exists, so neither would be reachable if the range stayed 0..3.
		if (ivalue < 0 || ivalue > 6)
		{
			if (sourceFile)
				Com_EPrintf("%s:%d: dlss_guide_field must be 0..6, got %d\n", sourceFile, lineno, ivalue);
			else
				Com_EPrintf("dlss_guide_field must be 0..6, got %d\n", ivalue);
			return Q_ERR_FAILURE;
		}
		mat->dlss_guide_field = ivalue;
		break;
	case MAT_BASE_FACTOR:
		mat->base_factor = fvalue;
		break;
	// Overrides pt_bounce_albedo_power for surfaces using this material - same
	// numbers as the "bounce light strength" slider, so whichever value looked
	// right there is the value to write here. 0 or negative hands the surface
	// back to the slider, which is also how 'mat albedo_strength -1' undoes it.
	case MAT_ALBEDO_STRENGTH:
		mat->albedo_strength = fvalue > 0.f ? fvalue : -1.f;
		break;
	case MAT_TEXTURE_BASE:
		set_material_texture(mat, svalue, mat->filename_base, &mat->image_base, IF_SRGB, !sourceFile);
		break;
	case MAT_TEXTURE_NORMALS:
		set_material_texture(mat, svalue, mat->filename_normals, &mat->image_normals, IF_NONE, !sourceFile);
		break;
	case MAT_TEXTURE_EMISSIVE:
		set_material_texture(mat, svalue, mat->filename_emissive, &mat->image_emissive, IF_SRGB, !sourceFile);
		break;
	case MAT_TEXTURE_ROUGHNESS:
		// roughness and metallic are linear data, never sRGB
		set_material_texture(mat, svalue, mat->filename_roughness, &mat->image_roughness, IF_NONE, !sourceFile);
		break;
	case MAT_TEXTURE_METALLIC:
		set_material_texture(mat, svalue, mat->filename_metallic, &mat->image_metallic, IF_NONE, !sourceFile);
		break;
	case MAT_TEXTURE_HEIGHT:
		set_material_texture(mat, svalue, mat->filename_height, &mat->image_height, IF_NONE, !sourceFile);
		break;
	case MAT_LIGHT_STYLES:
		mat->light_styles = bvalue;
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	case MAT_BSP_RADIANCE:
		mat->bsp_radiance = bvalue;
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	case MAT_DEFAULT_RADIANCE:
		mat->default_radiance = fvalue;
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	case MAT_TEXTURE_MASK:
		set_material_texture(mat, svalue, mat->filename_mask, &mat->image_mask, IF_NONE, !sourceFile);
		if (reload_flags) *reload_flags |= RELOAD_MAP;
		break;
	case MAT_SYNTH_EMISSIVE:
		mat->synth_emissive = bvalue;
		if (reload_flags) *reload_flags |= RELOAD_EMISSIVE;
		break;
	case MAT_EMISSIVE_THRESHOLD:
		mat->emissive_threshold = ivalue;
		if (reload_flags) *reload_flags |= RELOAD_EMISSIVE;
		break;
	case MAT_SPECULAR_FACTOR: mat->specular_factor = fvalue; break;
	default:
		assert(!"unknown PBR MAT attribute index");
	}

	// remember that this material says this itself - see specified_fields
	mat->specified_fields |= 1u << t->index;

	return Q_ERR_SUCCESS;
}

static uint32_t load_material_file(const char* file_name, pbr_material_t* dest, uint32_t max_items)
{
	assert(max_items >= 1);
	
	char* filebuf = NULL;
	unsigned source = IF_SRC_GAME;

	if (is_game_custom()) {
		// try the game specific path first
		FS_LoadFileEx(file_name, (void**)&filebuf, FS_PATH_GAME, TAG_FILESYSTEM);
	}

	if (!filebuf) {
		// game specific path not found, or we're playing baseq2
		source = IF_SRC_BASE;
		FS_LoadFileEx(file_name, (void**)&filebuf, FS_PATH_BASE, TAG_FILESYSTEM);
	}
	
	if (!filebuf)
		return 0;

	enum
	{
		INITIAL,
		ACCUMULATING_NAMES,
		READING_PARAMS
	} state = INITIAL;

	const char* ptr = filebuf;
	char linebuf[1024];
	uint32_t count = 0;
	uint32_t lineno = 0;
	
	const char* delimiters = " \t\r\n";
	uint32_t num_materials_in_group = 0;

	while (sgets(linebuf, sizeof(linebuf), &ptr))
	{
		++lineno;
		
		// Remove comments. A '#' only opens a comment when it is the first
		// non-whitespace character on the line: the rerelease ships textures
		// whose NAME contains '#' (textures/tomf/#lava_tf1), and truncating at
		// any '#' made those impossible to express here - the key parsed as
		// "textures/tomf/" and the loader reported "expected key and value".
		// Checked before changing this: no .mat in the tree uses a trailing
		// comment, so nothing relied on the old behaviour.
		{
			char* t = linebuf;
			while (*t == ' ' || *t == '\t')
				++t;
			if (*t == '#')
				*t = 0;
		}
		
		size_t len = strlen(linebuf);

		// remove trailing whitespace
		while (len > 0 && strchr(delimiters, linebuf[len - 1]))
			--len;
		linebuf[len] = 0;

		if (len == 0)
			continue;


		// if the line ends with a colon (:) or comma (,) it's a new material section
		char last = linebuf[len - 1];
		if (last == ':' || last == ',')
		{
			if (state == READING_PARAMS || state == ACCUMULATING_NAMES)
			{
				++dest;

				if (count > max_items)
				{
					Com_WPrintf("%s:%d: too many materials, expected up to %d.\n", file_name, lineno, max_items);
					Z_Free(filebuf);
					return count;
				}	
			}
			++count;
			
			MAT_Reset(dest);

			// copy the material name but not the colon
			linebuf[len - 1] = 0;
			Q_strlcpy(dest->name, linebuf, sizeof(dest->name));
			dest->image_flags = source;
			dest->registration_sequence = registration_sequence;

			// copy the material file name
			Q_strlcpy(dest->source_matfile, file_name, sizeof(dest->source_matfile));
			dest->source_line = lineno;

			if (state == ACCUMULATING_NAMES)
				++num_materials_in_group;
			else
				num_materials_in_group = 1;

			// state transition - same logic for all states
			state = (last == ',') ? ACCUMULATING_NAMES : READING_PARAMS;
			
			continue;
		}

		// all other lines are material parameters in the form of "key value" pairs
		
		char* key = strtok(linebuf, delimiters);
		char* value = strtok(NULL, delimiters);
		char* extra = strtok(NULL, delimiters);

		// "texture_roughness" alone clears the slot (MAT_TextureValueClears); so
		// does "stage2_texture_roughness" alone. Any other key needs its value.
		if (key && !value)
		{
			int stage_field;
			const struct MaterialAttribute* attr = NULL;
			for (int i = 0; i < c_NumAttributes; ++i)
				if (!strcmp(key, c_Attributes[i].name))
					attr = &c_Attributes[i];
			if ((attr && is_texture_attribute(attr->index))
				|| (vkpt_q3_stage_parse_key(key, &stage_field) >= 0 && stage_field >= 0 && stage_field < Q3SS_NUM_PATHS))
				value = "";
		}

		if (!key || !value)
		{
			Com_WPrintf("%s:%d: expected key and value\n", file_name, lineno);
			continue;
		}

		if (extra)
		{
			Com_WPrintf("%s:%d: unexpected extra characters after the key and value\n", file_name, lineno);
			continue;
		}

		if (state == INITIAL)
		{
			Com_WPrintf("%s:%d: expected material name section before any parameters\n", file_name, lineno);
			continue;
		}

		if (state == ACCUMULATING_NAMES)
		{
			Com_WPrintf("%s:%d: expected a final material name section ending with a colon before any parameters\n", file_name, lineno);

			// rollback the current material group
			dest -= (num_materials_in_group - 1);
			num_materials_in_group = 0;
			state = INITIAL;
			continue;
		}

		// "stage<N>_map", "stage<N>_roughness_override", ...: one stage of a
		// Quake III shader - kept beside the definitions, not in them
		// (q3stages.h STAGE SETTINGS)
		int q3_field;
		int q3_stage = vkpt_q3_stage_parse_key(key, &q3_field);
		if (q3_stage >= 0)
		{
			const bool map_scope = dest >= r_map_materials && dest < r_map_materials + MAX_PBR_MATERIALS;
			char err[256];
			for (uint32_t i = 0; i < num_materials_in_group; i++)
			{
				if (!vkpt_q3_stage_setting_add(map_scope ? Q3SS_SCOPE_MAP : Q3SS_SCOPE_GLOBAL, (dest - i)->name,
					q3_stage, q3_field, value, err, sizeof(err)))
				{
					Com_EPrintf("%s:%d: %s %s: %s\n", file_name, lineno, key, value, err);
					break;
				}
			}
			continue;
		}

		for (uint32_t i = 0; i < num_materials_in_group; i++) 
		{
			set_material_attribute(dest - i, key, value, file_name, lineno, NULL);
		}
	}

	Z_Free(filebuf);
	return count;
}

static void save_materials(const char* file_name, bool save_all, bool force)
{
	if (!force && FS_FileExistsEx(file_name, FS_TYPE_REAL))
	{
		Com_WPrintf("File '%s' already exists, add 'force' to overwrite\n", file_name);
		return;
	}

	qhandle_t file = 0;
	int err = FS_OpenFile(file_name, &file, FS_MODE_WRITE);
	
	if (err < 0 || !file)
	{
		Com_WPrintf("Cannot open file '%s' for writing: %s\n", file_name, Q_ErrorString(err));
		return;
	}

	uint32_t count = 0;

	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		const pbr_material_t* mat = r_materials + i;
		
		if (!mat->registration_sequence)
			continue;

		// when save_all == false, only save auto-generated materials,
		// i.e. those without a source file
		if (!save_all && mat->source_matfile[0])
			continue;

		FS_FPrintf(file, "%s:\n", mat->name);

		// A texture that was auto-detected rather than written down is not part
		// of the definition, and baking it back in would turn a tuning-only
		// entry into one that pins today's sidecars forever. Materials with no
		// source file are the generated block, where the detected paths ARE the
		// content, so those keep being written in full.
		// A slot the entry cleared is written as cleared ("0"): left out, the
		// next load would fill it from the sidecar again.
#define WRITE_TEXTURE(attr, field, key) \
		if (mat->field[0] && (!mat->source_matfile[0] || MAT_SPECIFIED(mat, attr))) \
			FS_FPrintf(file, "\t" key " %s\n", mat->field); \
		else if (MAT_CLEARED(mat, attr, field)) \
			FS_FPrintf(file, "\t" key " 0\n")

		WRITE_TEXTURE(MAT_TEXTURE_BASE, filename_base, "texture_base");
		WRITE_TEXTURE(MAT_TEXTURE_NORMALS, filename_normals, "texture_normals");
		WRITE_TEXTURE(MAT_TEXTURE_EMISSIVE, filename_emissive, "texture_emissive");
		WRITE_TEXTURE(MAT_TEXTURE_MASK, filename_mask, "texture_mask");
		WRITE_TEXTURE(MAT_TEXTURE_ROUGHNESS, filename_roughness, "texture_roughness");
		WRITE_TEXTURE(MAT_TEXTURE_METALLIC, filename_metallic, "texture_metallic");
		WRITE_TEXTURE(MAT_TEXTURE_HEIGHT, filename_height, "texture_height");

#undef WRITE_TEXTURE
		
		if (mat->bump_scale != 1.f)
			FS_FPrintf(file, "\tbump_scale %f\n", mat->bump_scale);

		if (mat->displace_in != MAT_DEFAULT_DISPLACE_IN)
			FS_FPrintf(file, "\tdisplace_in %f\n", mat->displace_in);

		if (mat->displace_out != 0.f)
			FS_FPrintf(file, "\tdisplace_out %f\n", mat->displace_out);

		if (mat->displace_clamp)
			FS_FPrintf(file, "\tdisplace_clamp 1\n");

		if (mat->alpha_glass && MAT_SPECIFIED(mat, MAT_ALPHA_GLASS))
			FS_FPrintf(file, "\talpha_glass 1\n");

		if (mat->glass_caustic > 0.f)
			FS_FPrintf(file, "\tcaustic %f\n", mat->glass_caustic);

		if (mat->caustic_ripple > 0.f)
			FS_FPrintf(file, "\tcaustic_ripple %f\n", mat->caustic_ripple);

		if (mat->roughness_override > 0.f)
			FS_FPrintf(file, "\troughness_override %f\n", mat->roughness_override);
		
		if (mat->metalness_factor != 1.f)
			FS_FPrintf(file, "\tmetalness_factor %f\n", mat->metalness_factor);
		
		if (mat->emissive_factor != 1.f)
			FS_FPrintf(file, "\temissive_factor %f\n", mat->emissive_factor);

		if (mat->light_factor != 1.f)
			FS_FPrintf(file, "\tlight_factor %f\n", mat->light_factor);

		if (mat->specular_factor != 1.f)
			FS_FPrintf(file, "\tspecular_factor %f\n", mat->specular_factor);

		if (mat->base_factor != 1.f)
			FS_FPrintf(file, "\tbase_factor %f\n", mat->base_factor);

		if (!MAT_IsKind(mat->flags, MATERIAL_KIND_REGULAR)) {
			const char* kind = getMaterialKindName(mat->flags);
			FS_FPrintf(file, "\tkind %s\n", kind ? kind : "");
		}
		
		if (mat->flags & MATERIAL_FLAG_LIGHT)
			FS_FPrintf(file, "\tis_light 1\n");

		if (mat->flags & MATERIAL_FLAG_CURVED_WATER)
			FS_FPrintf(file, "\tcurved_water 1\n");

		if (mat->flags & MATERIAL_FLAG_DOUBLE_SIDED)
			FS_FPrintf(file, "\tdouble_sided 1\n");
		
		if (!mat->light_styles)
			FS_FPrintf(file, "\tlight_styles 0\n");
		
		if (!mat->bsp_radiance)
			FS_FPrintf(file, "\tbsp_radiance 0\n");

		if (mat->default_radiance != 1.f)
			FS_FPrintf(file, "\tdefault_radiance %f\n", mat->default_radiance);

		// only written when it was explicitly stated, so a `mat save` cannot
		// silently bake the current class default into every material
		if (mat->volumetric_scale >= 0.f)
			FS_FPrintf(file, "\tvolumetric_scale %f\n", mat->volumetric_scale);

		// likewise only written when stated - the default is "let the cvar decide"
		if (mat->dlss_guide_field >= 0)
			FS_FPrintf(file, "\tdlss_guide_field %d\n", mat->dlss_guide_field);

		// likewise - the default is "let the slider decide"
		if (mat->albedo_strength > 0.f)
			FS_FPrintf(file, "\talbedo_strength %f\n", mat->albedo_strength);

		if (mat->synth_emissive)
			FS_FPrintf(file, "\tsynth_emissive 1\n");

		if (mat->emissive_threshold != cvar_pt_surface_lights_threshold->integer)
			FS_FPrintf(file, "\temissive_threshold %d\n", mat->emissive_threshold);
		
		FS_FPrintf(file, "\n");
		
		++count;
	}

	FS_CloseFile(file);

	Com_Printf("saved %d materials\n", count);
}

/*
=================
'mat save' with no file name: what was changed in the console, written back
into the .mat entries those materials came from (Matt, 2026-11-16: "it should
save it into the associated material file and not prompt me to provide a file
name").

Only what 'mat <attribute> <value>' changed since the material was loaded is
written (pbr_material_t::console_fields), and only inside the material's own
entry: the rest of the file, its comments and its line endings stay exactly as
they were. A changed attribute replaces the entry's line for it; failing that, a
commented-out line for it among the entry's own lines ("#\tkey value", the way
the generated stubs list what can be set) is uncommented in place; failing that,
a line is added after the entry's last setting.

The file is the one load_material_file read, wherever that is - rerelease/,
baseq2/ or baseq3/ (FS_RealPath) - replaced whole through a .tmp, with the
version it replaces kept as .bak.
=================
*/

// the .mat attribute called (name), or NULL
static struct MaterialAttribute const* find_attribute(const char* name)
{
	for (int i = 0; i < c_NumAttributes; ++i)
	{
		if (strcmp(name, c_Attributes[i].name) == 0)
			return &c_Attributes[i];
	}
	return NULL;
}

// Where the .mat file a definition was read from is, found the way
// load_material_file found it: the game directory first when there is one, then
// baseq2 and Quake III's baseq3. FS_REALPATH_* (files.h).
static int material_file_real_path(const char* file_name, char* out, size_t size)
{
	int where = FS_REALPATH_NONE;
	if (is_game_custom())
		where = FS_RealPath(file_name, FS_PATH_GAME, out, size);
	if (where == FS_REALPATH_NONE)
		where = FS_RealPath(file_name, FS_PATH_BASE, out, size);
	return where;
}

// (path) from the install folder down - "baseq3/materials/quake3.mat" - for
// messages; the full path when it is somewhere else (a home directory)
static const char* material_short_path(const char* path)
{
	const char* basedir = Cvar_VariableString("basedir");
	size_t n = strlen(basedir);
	if (n && !Q_strncasecmp(path, basedir, n) && (path[n] == '/' || path[n] == '\\'))
		return path + n + 1;
	return path;
}

// The value of attribute (index) as (mat) has it now, the way a .mat file
// states it. False when there is nothing to write (a kind with no name).
static bool format_attribute_value(const pbr_material_t* mat, enum AttributeIndex index, char* out, size_t size)
{
#define FLOAT_VALUE(v)   Q_snprintf(out, size, "%g", (v)); return true
#define INT_VALUE(v)     Q_snprintf(out, size, "%d", (int)(v)); return true
#define TEXTURE_VALUE(f) Q_strlcpy(out, mat->f[0] ? mat->f : "0", size); return true

	switch (index)
	{
	case MAT_BUMP_SCALE:         FLOAT_VALUE(mat->bump_scale);
	case MAT_ROUGHNESS_OVERRIDE: FLOAT_VALUE(mat->roughness_override);
	case MAT_METALNESS_FACTOR:   FLOAT_VALUE(mat->metalness_factor);
	case MAT_EMISSIVE_FACTOR:    FLOAT_VALUE(mat->emissive_factor);
	case MAT_LIGHT_FACTOR:       FLOAT_VALUE(mat->light_factor);
	case MAT_BASE_FACTOR:        FLOAT_VALUE(mat->base_factor);
	case MAT_SPECULAR_FACTOR:    FLOAT_VALUE(mat->specular_factor);
	case MAT_DEFAULT_RADIANCE:   FLOAT_VALUE(mat->default_radiance);
	case MAT_DISPLACE_IN:        FLOAT_VALUE(mat->displace_in);
	case MAT_DISPLACE_OUT:       FLOAT_VALUE(mat->displace_out);
	case MAT_CAUSTIC:            FLOAT_VALUE(mat->glass_caustic);
	case MAT_CAUSTIC_RIPPLE:     FLOAT_VALUE(mat->caustic_ripple);
	case MAT_VOLUMETRIC_SCALE:   FLOAT_VALUE(mat->volumetric_scale);
	case MAT_ALBEDO_STRENGTH:    FLOAT_VALUE(mat->albedo_strength);
	case MAT_EMISSIVE_THRESHOLD: INT_VALUE(mat->emissive_threshold);
	case MAT_DLSS_GUIDE_FIELD:   INT_VALUE(mat->dlss_guide_field);
	case MAT_LIGHT_STYLES:       INT_VALUE(mat->light_styles ? 1 : 0);
	case MAT_BSP_RADIANCE:       INT_VALUE(mat->bsp_radiance ? 1 : 0);
	case MAT_SYNTH_EMISSIVE:     INT_VALUE(mat->synth_emissive ? 1 : 0);
	case MAT_DISPLACE_CLAMP:     INT_VALUE(mat->displace_clamp ? 1 : 0);
	case MAT_ALPHA_GLASS:        INT_VALUE(mat->alpha_glass ? 1 : 0);
	case MAT_IS_LIGHT:           INT_VALUE((mat->flags & MATERIAL_FLAG_LIGHT) ? 1 : 0);
	case MAT_CURVED_WATER:       INT_VALUE((mat->flags & MATERIAL_FLAG_CURVED_WATER) ? 1 : 0);
	case MAT_DOUBLE_SIDED:       INT_VALUE((mat->flags & MATERIAL_FLAG_DOUBLE_SIDED) ? 1 : 0);
	case MAT_TEXTURE_BASE:       TEXTURE_VALUE(filename_base);
	case MAT_TEXTURE_NORMALS:    TEXTURE_VALUE(filename_normals);
	case MAT_TEXTURE_EMISSIVE:   TEXTURE_VALUE(filename_emissive);
	case MAT_TEXTURE_MASK:       TEXTURE_VALUE(filename_mask);
	case MAT_TEXTURE_ROUGHNESS:  TEXTURE_VALUE(filename_roughness);
	case MAT_TEXTURE_METALLIC:   TEXTURE_VALUE(filename_metallic);
	case MAT_TEXTURE_HEIGHT:     TEXTURE_VALUE(filename_height);
	case MAT_KIND: {
		const char* kind = getMaterialKindName(mat->flags);
		if (!kind)
			return false;
		Q_strlcpy(out, kind, size);
		return true;
	}
	}

#undef FLOAT_VALUE
#undef INT_VALUE
#undef TEXTURE_VALUE

	return false;
}

// A .mat file as lines, each kept byte for byte with its own line ending, so
// that what 'mat save' does not change it writes back unchanged.
typedef struct {
	const char* text;   // without the line ending: in the file's buffer, or owned
	size_t      len;
	const char* eol;    // "\r\n", "\n", or "" for an unterminated last line
	bool        owned;  // text was allocated here
} mat_line_t;

typedef struct {
	mat_line_t* lines;
	int         count;
	int         alloc;
	const char* eol;    // the file's own line ending, for lines added to it
} mat_text_t;

static void mat_text_insert(mat_text_t* t, int at, const char* text, size_t len, const char* eol, bool owned)
{
	if (t->count == t->alloc)
	{
		t->alloc = t->alloc ? t->alloc * 2 : 1024;
		t->lines = Z_Realloc(t->lines, sizeof(mat_line_t) * t->alloc);
	}
	memmove(t->lines + at + 1, t->lines + at, sizeof(mat_line_t) * (t->count - at));
	t->lines[at].text = text;
	t->lines[at].len = len;
	t->lines[at].eol = eol;
	t->lines[at].owned = owned;
	t->count++;
}

static void mat_text_parse(mat_text_t* t, const char* buf, size_t size)
{
	memset(t, 0, sizeof(*t));
	t->eol = "\n";
	bool eol_seen = false;

	size_t pos = 0;
	while (pos < size)
	{
		size_t start = pos;
		while (pos < size && buf[pos] != '\n')
			pos++;
		size_t end = pos;
		const char* eol = "";
		if (pos < size)
		{
			pos++;
			if (end > start && buf[end - 1] == '\r')
			{
				end--;
				eol = "\r\n";
			}
			else
				eol = "\n";
			if (!eol_seen)
			{
				t->eol = eol;
				eol_seen = true;
			}
		}
		mat_text_insert(t, t->count, buf + start, end - start, eol, false);
	}
}

static void mat_text_free(mat_text_t* t)
{
	for (int i = 0; i < t->count; i++)
	{
		if (t->lines[i].owned)
			Z_Free((void*)t->lines[i].text);
	}
	Z_Free(t->lines);
	memset(t, 0, sizeof(*t));
}

// line (i) becomes (text), a copy
static void mat_text_set(mat_text_t* t, int i, const char* text)
{
	mat_line_t* l = t->lines + i;
	if (l->owned)
		Z_Free((void*)l->text);
	l->text = Z_CopyString(text);
	l->len = strlen(text);
	l->owned = true;
}

enum { MATLINE_BLANK, MATLINE_COMMENT, MATLINE_HEADER, MATLINE_SETTING };

// What load_material_file makes of a line, by its rules: for a header, the
// name it declares in (*first) and its last character - ':' closes a group of
// names, ',' continues one; for a setting, its key and value. (line) is the
// scratch copy they point into.
static int mat_line_kind(const mat_line_t* l, char line[1024], char** first, char** second, char* header_end)
{
	size_t len = min(l->len, (size_t)1023);
	memcpy(line, l->text, len);
	line[len] = 0;
	*first = *second = NULL;

	const char* t = line;
	while (*t == ' ' || *t == '\t')
		++t;
	if (*t == '#')
		return MATLINE_COMMENT;

	while (len > 0 && strchr(" \t\r\n", line[len - 1]))
		--len;
	line[len] = 0;
	if (len == 0)
		return MATLINE_BLANK;

	char last = line[len - 1];
	if (last == ':' || last == ',')
	{
		line[len - 1] = 0;
		*first = line;  // the loader keeps any leading whitespace in the name too
		if (header_end)
			*header_end = last;
		return MATLINE_HEADER;
	}

	*first = strtok(line, " \t\r\n");
	*second = strtok(NULL, " \t\r\n");
	return MATLINE_SETTING;
}

// The key of a commented-out setting, "#\tkey value" - a comment of exactly two
// words - or NULL
static const char* mat_line_commented_key(const mat_line_t* l, char line[1024])
{
	size_t len = min(l->len, (size_t)1023);
	memcpy(line, l->text, len);
	line[len] = 0;

	char* t = line;
	while (*t == ' ' || *t == '\t')
		++t;
	if (*t != '#')
		return NULL;

	char* key = strtok(t + 1, " \t\r\n");
	char* value = strtok(NULL, " \t\r\n");
	char* extra = strtok(NULL, " \t\r\n");
	return (key && value && !extra) ? key : NULL;
}

// The entry for (name): the header line declaring it - the one nearest
// (near_line), counted from 1, if there are several - the first line of the
// settings after its group's last header and the line after them (the next
// header, or the end), and how many names share those settings.
static bool mat_text_find_entry(const mat_text_t* t, const char* name, uint32_t near_line,
	int* header, int* first, int* end, int* group_size)
{
	char line[1024];
	char *a, *b;
	char last = 0;

	int best = -1;
	for (int i = 0; i < t->count; i++)
	{
		if (mat_line_kind(t->lines + i, line, &a, &b, &last) == MATLINE_HEADER && !Q_stricmp(a, name))
		{
			if (best < 0 || abs(i + 1 - (int)near_line) < abs(best + 1 - (int)near_line))
				best = i;
		}
	}
	if (best < 0)
		return false;

	// "a," "b," "c:" - the three share the settings after "c:"
	int group_first = best;
	for (int i = best - 1; i >= 0; i--)
	{
		int kind = mat_line_kind(t->lines + i, line, &a, &b, &last);
		if (kind == MATLINE_BLANK || kind == MATLINE_COMMENT)
			continue;
		if (kind == MATLINE_HEADER && last == ',')
		{
			group_first = i;
			continue;
		}
		break;
	}

	int colon = -1;
	for (int i = best; i < t->count; i++)
	{
		int kind = mat_line_kind(t->lines + i, line, &a, &b, &last);
		if (kind == MATLINE_BLANK || kind == MATLINE_COMMENT)
			continue;
		if (kind == MATLINE_HEADER)
		{
			if (last == ':')
			{
				colon = i;
				break;
			}
			continue;
		}
		break;  // a setting before the group closed: the loader drops the group
	}
	if (colon < 0)
		return false;

	int names = 0;
	for (int i = group_first; i <= colon; i++)
	{
		if (mat_line_kind(t->lines + i, line, &a, &b, &last) == MATLINE_HEADER)
			names++;
	}

	int e = colon + 1;
	while (e < t->count && mat_line_kind(t->lines + e, line, &a, &b, &last) != MATLINE_HEADER)
		e++;

	*header = best;
	*first = colon + 1;
	*end = e;
	*group_size = names;
	return true;
}

// (key) set to (value) in the entry whose settings are lines [first, *end) - see
// the block comment above for where. (*insert_at) is where a new line goes.
static void mat_text_set_setting(mat_text_t* t, int first, int* end, int* insert_at,
	const char* key, const char* value)
{
	char line[1024], text[1024];
	char *a, *b;
	bool replaced = false;

	for (int i = first; i < *end; i++)
	{
		if (mat_line_kind(t->lines + i, line, &a, &b, NULL) != MATLINE_SETTING || strcmp(a, key) != 0)
			continue;

		// keep the line's indent and the gap between key and value
		const char* src = t->lines[i].text;
		size_t len = t->lines[i].len;
		size_t p = 0;
		while (p < len && (src[p] == ' ' || src[p] == '\t'))
			p++;
		p += strlen(key);
		while (p < len && (src[p] == ' ' || src[p] == '\t'))
			p++;
		Q_snprintf(text, sizeof(text), "%.*s%s", (int)p, src, value);
		mat_text_set(t, i, text);
		replaced = true;
	}
	if (replaced)
		return;

	// a commented-out line for it among the entry's own lines - the unbroken
	// run after its header, so never one heading the entry after it
	int body_end = first;
	while (body_end < *end && mat_line_kind(t->lines + body_end, line, &a, &b, NULL) != MATLINE_BLANK)
		body_end++;
	for (int i = body_end - 1; i >= first; i--)
	{
		const char* commented = mat_line_commented_key(t->lines + i, line);
		if (!commented || strcmp(commented, key) != 0)
			continue;

		// "#\tkey  old" -> "\tkey  new": what followed the '#' is the indent
		const char* src = t->lines[i].text;
		size_t len = t->lines[i].len;
		size_t p = 0;
		while (p < len && src[p] != '#')
			p++;
		p++;
		size_t q = p;
		while (q < len && (src[q] == ' ' || src[q] == '\t'))
			q++;
		size_t g = q + strlen(key);
		while (g < len && (src[g] == ' ' || src[g] == '\t'))
			g++;
		if (q > p)
			Q_snprintf(text, sizeof(text), "%.*s%s", (int)(g - p), src + p, value);
		else
			Q_snprintf(text, sizeof(text), "\t%.*s%s", (int)(g - q), src + q, value);
		mat_text_set(t, i, text);
		return;
	}

	// a new line after the entry's last setting
	Q_snprintf(text, sizeof(text), "\t%s %s", key, value);
	if (*insert_at > 0 && t->lines[*insert_at - 1].eol[0] == 0)
		t->lines[*insert_at - 1].eol = t->eol;  // the file's last line had no ending
	mat_text_insert(t, *insert_at, Z_CopyString(text), strlen(text), t->eol, true);
	(*insert_at)++;
	(*end)++;
}

// (t) over (path), through path.tmp, keeping the version it replaces as path.bak
static bool mat_text_write(const mat_text_t* t, const char* path)
{
	char tmp[MAX_OSPATH], bak[MAX_OSPATH];
	if (Q_concat(tmp, sizeof(tmp), path, ".tmp") >= sizeof(tmp)
		|| Q_concat(bak, sizeof(bak), path, ".bak") >= sizeof(bak))
		return false;

	FILE* f = fopen(tmp, "wb");
	if (!f)
	{
		Com_WPrintf("Cannot write '%s': %s\n", tmp, Q_ErrorString(Q_ERRNO));
		return false;
	}

	bool ok = true;
	for (int i = 0; i < t->count && ok; i++)
	{
		const mat_line_t* l = t->lines + i;
		size_t eol_len = strlen(l->eol);
		if (l->len && fwrite(l->text, 1, l->len, f) != l->len)
			ok = false;
		if (eol_len && fwrite(l->eol, 1, eol_len, f) != eol_len)
			ok = false;
	}
	if (fclose(f) != 0)
		ok = false;
	if (!ok)
	{
		Com_WPrintf("Cannot write '%s'\n", tmp);
		remove(tmp);
		return false;
	}

	remove(bak);
	if (rename(path, bak) != 0)
	{
		Com_WPrintf("Cannot replace '%s' (is it open in another program?): %s\n", path, Q_ErrorString(Q_ERRNO));
		remove(tmp);
		return false;
	}
	if (rename(tmp, path) != 0)
	{
		Com_WPrintf("Cannot replace '%s': %s\n", path, Q_ErrorString(Q_ERRNO));
		rename(bak, path);
		return false;
	}
	return true;
}

// The definition a live material was made from, the way MAT_Find and
// 'mat reload' pick it: a wall takes its map's .mat first
static pbr_material_t* live_material_definition(const pbr_material_t* mat)
{
	pbr_material_t* matdef = NULL;
	if (mat->image_type == IT_WALL)
		matdef = find_material_sorted(mat->name, r_map_materials, num_map_materials);
	if (!matdef)
		matdef = find_material_sorted(mat->name, r_global_materials, num_global_materials);
	return matdef;
}

// something typed in the console that 'mat save' has not written: an attribute,
// or one of its Quake III stages' settings (q3stages.h)
static bool material_unsaved(const pbr_material_t* mat)
{
	return mat->console_fields || vkpt_q3_stage_unsaved(mat->name);
}

// " texture_metallic metalness_factor": the attributes typed in the console
// and not yet saved (console_fields), each with a leading space
static void material_console_field_names(const pbr_material_t* mat, char* out, size_t size)
{
	out[0] = 0;
	for (int i = 0; i < c_NumAttributes; i++)
	{
		if (mat->console_fields & (1u << c_Attributes[i].index))
		{
			Q_strlcat(out, " ", size);
			Q_strlcat(out, c_Attributes[i].name, size);
		}
	}
}

// Every changed material defined in (source_matfile), written into it in one go.
// (tried) marks the materials dealt with, saved or not.
static int save_changes_to_file(const char* source_matfile, uint8_t* tried)
{
	char path[MAX_OSPATH];
	int where = material_file_real_path(source_matfile, path, sizeof(path));

	if (where != FS_REALPATH_DISK)
	{
		for (int i = 0; i < MAX_PBR_MATERIALS; i++)
		{
			pbr_material_t* mat = r_materials + i;
			if (!mat->registration_sequence || !material_unsaved(mat) || tried[i] || strcmp(mat->source_matfile, source_matfile) != 0)
				continue;
			tried[i] = 1;
			if (where == FS_REALPATH_PACK)
				Com_WPrintf("%s: not saved - %s is inside %s. Copy it out of the pack first.\n",
					mat->name, source_matfile, material_short_path(path));
			else
				Com_WPrintf("%s: not saved - %s is no longer on disk.\n", mat->name, source_matfile);
		}
		return 0;
	}

	const char* shown = material_short_path(path);

	FILE* f = fopen(path, "rb");
	if (!f)
	{
		Com_WPrintf("Cannot read '%s': %s\n", shown, Q_ErrorString(Q_ERRNO));
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = Z_Malloc(size > 0 ? (size_t)size : 1);
	size_t got = size > 0 ? fread(buf, 1, (size_t)size, f) : 0;
	fclose(f);
	if (size < 0 || got != (size_t)size)
	{
		Com_WPrintf("Cannot read '%s'\n", shown);
		Z_Free(buf);
		return 0;
	}

	mat_text_t text;
	mat_text_parse(&text, buf, got);

	// the materials written into (text), to finish off once it is on disk
	static int pending[MAX_PBR_MATERIALS];
	int num_pending = 0;

	Com_Printf("%s:\n", shown);

	for (int i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		pbr_material_t* mat = r_materials + i;
		if (!mat->registration_sequence || !material_unsaved(mat) || tried[i] || strcmp(mat->source_matfile, source_matfile) != 0)
			continue;
		tried[i] = 1;

		int header, first, end, group_size;
		if (!mat_text_find_entry(&text, mat->name, mat->source_line, &header, &first, &end, &group_size))
		{
			Com_WPrintf("  %s: not saved - its entry is not in the file any more. 'mat save <file>' "
				"writes it to a file of its own.\n", mat->name);
			continue;
		}

		// a new line goes after the entry's last setting, or else after the
		// comments and stage lines right under its header
		int insert_at = -1;
		char line[1024];
		char *a, *b;
		for (int l = first; l < end; l++)
		{
			if (mat_line_kind(text.lines + l, line, &a, &b, NULL) == MATLINE_SETTING)
				insert_at = l + 1;
		}
		if (insert_at < 0)
		{
			insert_at = first;
			while (insert_at < end && mat_line_kind(text.lines + insert_at, line, &a, &b, NULL) != MATLINE_BLANK)
				insert_at++;
		}

		char list[512] = "";
		for (int k = 0; k < c_NumAttributes; k++)
		{
			const struct MaterialAttribute* attr = c_Attributes + k;
			if (!(mat->console_fields & (1u << attr->index)))
				continue;

			char value[MAX_IMAGE_PATH];
			if (!format_attribute_value(mat, attr->index, value, sizeof(value)))
				continue;

			mat_text_set_setting(&text, first, &end, &insert_at, attr->name, value);
			Q_strlcat(list, list[0] ? ", " : "", sizeof(list));
			Q_strlcat(list, attr->name, sizeof(list));
			Q_strlcat(list, " ", sizeof(list));
			Q_strlcat(list, value, sizeof(list));
		}

		// its Quake III stages' settings, "stage2_roughness_override 0.8"
		char stage_key[64], stage_value[MAX_IMAGE_PATH];
		for (int k = 0; vkpt_q3_stage_unsaved_get(mat->name, k, stage_key, sizeof(stage_key),
			stage_value, sizeof(stage_value)); k++)
		{
			mat_text_set_setting(&text, first, &end, &insert_at, stage_key, stage_value);
			Q_strlcat(list, list[0] ? ", " : "", sizeof(list));
			Q_strlcat(list, stage_key, sizeof(list));
			Q_strlcat(list, " ", sizeof(list));
			Q_strlcat(list, stage_value, sizeof(list));
		}

		Com_Printf("  %s (line %d): %s\n", mat->name, header + 1, list);
		if (group_size > 1)
			Com_Printf("    that entry is shared by %d materials: it changes for all of them ('mat reload' shows them now)\n",
				group_size);

		pending[num_pending++] = i;
	}

	int saved = 0;
	if (num_pending > 0 && mat_text_write(&text, path))
	{
		for (int p = 0; p < num_pending; p++)
		{
			pbr_material_t* mat = r_materials + pending[p];

			/* The definition too, which is where the next map, or this material's
			   next registration, takes its values from - the global table is
			   only re-read by 'mat reload', so without this a saved change
			   would seem to come undone on a map change. */
			pbr_material_t* matdef = live_material_definition(mat);
			int header, first, end, group_size;
			uint32_t line = mat->source_line;
			if (mat_text_find_entry(&text, mat->name, mat->source_line, &header, &first, &end, &group_size))
				line = (uint32_t)header + 1;  // lines added above it moved it down

			for (int k = 0; k < c_NumAttributes; k++)
			{
				const struct MaterialAttribute* attr = c_Attributes + k;
				char value[MAX_IMAGE_PATH];
				if (!(mat->console_fields & (1u << attr->index)) || !format_attribute_value(mat, attr->index, value, sizeof(value)))
					continue;
				if (matdef && !strcmp(matdef->source_matfile, source_matfile))
					set_material_attribute(matdef, attr->name, value, source_matfile, line, NULL);
			}

			mat->source_line = line;
			if (matdef && !strcmp(matdef->source_matfile, source_matfile) && !strcmp(matdef->name, mat->name))
				matdef->source_line = line;
			mat->console_fields = 0;

			// the stage settings into the scope of the file they went to, so a
			// map change keeps them before the next 'mat reload' reads them
			vkpt_q3_stage_saved(mat->name,
				matdef && matdef >= r_map_materials && matdef < r_map_materials + MAX_PBR_MATERIALS);
			saved++;
		}
	}

	mat_text_free(&text);
	Z_Free(buf);
	return saved;
}

static void save_to_source_files(void)
{
	static uint8_t tried[MAX_PBR_MATERIALS];
	memset(tried, 0, sizeof(tried));

	int saved = 0, changed = 0;
	char no_entry[512] = "";
	int num_no_entry = 0;

	/* Counted before anything is written: a save writes every changed material
	   of that file and marks them saved, so counting in the loop below missed
	   the ones a save had already done - "Saved 2 of 1 changed material"
	   (Matt, 2026-11-18: m_shell and medkit_sphere, both in quake3.mat). */
	for (int i = 0; i < MAX_PBR_MATERIALS; i++)
		if (r_materials[i].registration_sequence && material_unsaved(r_materials + i))
			changed++;

	for (int i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		pbr_material_t* mat = r_materials + i;
		if (!mat->registration_sequence || !material_unsaved(mat))
			continue;
		if (tried[i])
			continue;

		if (!mat->source_matfile[0])
		{
			tried[i] = 1;
			if (num_no_entry++ < 6)
			{
				Q_strlcat(no_entry, no_entry[0] ? ", " : "", sizeof(no_entry));
				Q_strlcat(no_entry, mat->name, sizeof(no_entry));
			}
			continue;
		}

		saved += save_changes_to_file(mat->source_matfile, tried);
	}

	if (changed == 0)
	{
		Com_Printf("Nothing to save: no material was changed with 'mat <attribute> <value>' since it was loaded.\n");
		return;
	}

	if (num_no_entry > 0)
		Com_Printf("%d changed material%s with no .mat entry, not saved: %s%s - 'mat save <file>' writes those.\n",
			num_no_entry, num_no_entry == 1 ? "" : "s", no_entry, num_no_entry > 6 ? ", ..." : "");

	Com_Printf("Saved %d of %d changed material%s (each file's old version is kept as .bak).\n",
		saved, changed, changed == 1 ? "" : "s");
}

void MAT_ChangeMap(const char* map_name)
{
	/* THE SAME MAP RELOADING IS NOT A MAP CHANGE. Every CL_PrepRefresh rebuilds
	   the world and lands here - including the one 'mat <attr>' triggers for
	   RELOAD_MAP attributes (is_light, kind, curved_water) so the light list and
	   BLAS pick the edit up. On a map with its own .mat the reset below then
	   re-created every wall material from disk and threw that very edit away,
	   so 'mat is_light 1' appeared to do nothing. Re-reading the files on
	   request is 'mat reload', which reloads them itself. */
	if (map_name && current_map_name[0] && !Q_stricmp(map_name, current_map_name))
		return;

	Q_strlcpy(current_map_name, map_name ? map_name : "", sizeof(current_map_name));

	// clear the old map-specific materials
	uint32_t old_map_materails = num_map_materials;
	if (num_map_materials > 0) {
		memset(r_map_materials, 0, sizeof(pbr_material_t) * num_map_materials);
	}

	// load the new materials
	vkpt_q3_stage_override_clear(Q3SS_SCOPE_MAP);
	vkpt_q3_light_cache_clear();
	char map_name_no_ext[MAX_QPATH];
	truncate_extension(map_name, map_name_no_ext, sizeof(map_name_no_ext));
	char file_name[MAX_QPATH];
	Q_snprintf(file_name, sizeof(file_name), "%s.mat", map_name_no_ext);
	num_map_materials = load_material_file(file_name, r_map_materials, MAX_PBR_MATERIALS);
	if (num_map_materials > 0) {
		// find_material_sorted binary-searches this table. Unsorted, an entry
		// that comes after one with a larger name was never found.
		sort_and_deduplicate_materials(r_map_materials, &num_map_materials);
		Com_Printf("Loaded %d materials from %s\n", num_map_materials, file_name);
	}

	// if there are any overrides now or there were some overrides before,
	// unload all wall materials to re-initialize them with the overrides
	if (old_map_materails > 0 || num_map_materials > 0)
	{
		for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
		{
			pbr_material_t* mat = r_materials + i;

			if (mat->registration_sequence && mat->image_type == IT_WALL)
			{
				// remove the material from the hash table
				List_Remove(&mat->entry);

				// invalidate the material entry
				MAT_Reset(mat);
			}
		}
	}
}

static void load_material_image(image_t** image, const char* filename, pbr_material_t* mat, imagetype_t type, imageflags_t flags)
{
	// another material's solid stand-in, "...chrome_metal*M1.wal": no file
	if (solid_tag_index(filename) >= 0) {
		*image = load_solid_by_name(filename, mat, type);
		return;
	}

	*image = IMG_Find(filename, type, flags | IF_EXACT | (mat->image_flags & IF_SRC_MASK));
	if (*image == R_NOTEXTURE) {
		/* Also try inexact loading in case of an override texture.
		 * Useful for games that ship a texture in a different directory
		 * (compared to the baseq2 location): the override is still picked
		 * up, but if the same path is written to a materials file, a
		 * warning is emitted, since overrides are in generally in baseq2. */
		if(strncmp(filename, "overrides/", 10) == 0)
			*image = IMG_Find(filename, type, flags | IF_EXACT);
	}

	/* A path into materialDev/ names a copy of a pak file at the pak's own path
	   (quake3.mat names one for every Quake III texture). Only the edited copies
	   are in git, so with the copy missing, the pak file it was made from is the
	   same picture - the health crosses' texture_emissive envmap*.jpg, which the
	   material has no other glow to fall back on for. Nothing else missing
	   changes here: the callers leave the material as it is without the line. */
	if (*image == R_NOTEXTURE && !Q_strncasecmp(filename, "materialDev/", 12) && filename[12])
		load_material_image(image, filename + 12, mat, type, flags);
}

/*
=================
player_skin_map

PLAYER SKINS SHARE ONE SET OF SURFACE MAPS PER MODEL, AND ANY SKIN MAY REPLACE
ONE OF THEM.

Every skin of a player model wears the same UV layout, so rerelease.mat points
all of a model's skin entries at one body_n / body_rough / body_metallic in
materialDev/players/<model>/. A skin that needs its own map - one variant that
is metal where the others are cloth - drops it beside its own base texture,
named after it (grunt_metallic.tga next to grunt.tga), and that file is used
instead of the shared one with no .mat edit. Players only: every other entry
that names a texture_base still describes its textures exactly as written.

Returns (shared) unless (mat_name) is players/<model>/<skin>, and
<base_file without extension><suffix>.tga exists.
=================
*/
static const char* player_skin_map(const char* mat_name, const char* base_file,
	const char* shared, const char* suffix, char* buffer, size_t size)
{
	const char* slash;
	char stem[MAX_IMAGE_PATH];

	if (strncmp(mat_name, "players/", 8) || !base_file[0])
		return shared;

	slash = strchr(mat_name + 8, '/');
	if (!slash || slash == mat_name + 8 || strchr(slash + 1, '/'))
		return shared;

	truncate_extension(base_file, stem, sizeof(stem));
	if (Q_snprintf(buffer, size, "%s%s.tga", stem, suffix) >= size || !FS_FileExists(buffer))
		return shared;

	return buffer;
}

static qboolean game_image_identical_to_base(const char* name)
{
	/* Check if a game image is actually different from the base version,
	   as some games (eg rogue) ship image assets that are identical to the
	   baseq2 version.
	   If that is the case, ignore the game image, and just use everything
	   from baseq2, especially overides/other images. */
	qboolean result = false;

	qhandle_t base_file = -1, game_file = -1;
	if((FS_OpenFile(name, &base_file, FS_MODE_READ | FS_PATH_BASE | FS_BUF_NONE) >= 0)
		&& (FS_OpenFile(name, &game_file, FS_MODE_READ | FS_PATH_GAME | FS_BUF_NONE) >= 0))
	{
		int64_t base_len = FS_Length(base_file), game_len = FS_Length(game_file);
		if(base_len == game_len)
		{
			char *base_data = FS_Malloc(base_len);
			char *game_data = FS_Malloc(game_len);
			if(FS_Read(base_data, base_len, base_file) >= 0
				&& FS_Read(game_data, game_len, game_file) >= 0)
			{
				result = memcmp(base_data, game_data, base_len) == 0;
			}
			Z_Free(base_data);
			Z_Free(game_data);
		}
	}
	if (base_file >= 0)
		FS_CloseFile(base_file);
	if (game_file >= 0)
		FS_CloseFile(game_file);

	return result;
}

/*
=================
autodetect_material_textures

Fills the texture slots of (mat) from files sitting next to the base texture -
<name>_n, <name>_rough, <name>_metallic, <name>_light, and <name>_glow for the
rerelease's md5 skins. This is what makes a texture with no material definition
at all still render with its normal and PBR maps.

It also runs for a definition that does not name a texture_base, i.e. one that
only tunes scalars (see MAT_Find), so a slot is only ever filled when the
definition did not state it. A NULL (name) means the caller already has the
base image and wants only the sidecars re-detected - 'mat reload' does that,
because IMG_Find needs the name WITH its extension and only the registration
path still has it. That keeps an explicit texture_normals - and an
explicit "texture_normals 0", which means "this material has none" - winning
over whatever happens to be lying beside the texture.
=================
*/
/* A slot no sidecar fills is EMPTY - its name too. On 'mat reload' the slot can
   still carry a path typed in the console and dropped with the rest of the
   unsaved changes; clearing only the image left 'mat print' showing that path
   as "(NOT LOADED)", which reads as a broken file rather than a lost edit
   (Matt's anarki_g_metal.png, a fine 16-bit PNG, 2026-11-18). */
static void clear_detected_slot(image_t** image, char* filename)
{
	*image = NULL;
	filename[0] = 0;
}

// Whether a stage of `material` names a surface map of its own (mat
// stage<N>_texture_roughness / _metallic / _normals, from any scope): the
// "basic diffuse" rule below counts those as the material's own maps. Sets
// *metallic for a metallic one.
static bool q3_stages_name_maps(const char* material, bool* metallic)
{
	bool any = false;

	*metallic = false;
	for (int i = 0; i < Q3P_MAX_STAGES; i++)
	{
		q3stagesettings_t s;
		if (!vkpt_q3_stage_settings(material, i, &s))
			continue;
		if (s.set & (1u << Q3SS_TEXTURE_METALLIC))
			*metallic = any = true;
		if (s.set & ((1u << Q3SS_TEXTURE_ROUGHNESS) | (1u << Q3SS_TEXTURE_NORMALS)))
			any = true;
	}
	return any;
}

/* A MAP TYPED IN TAKES A SURFACE OFF "BASIC DIFFUSE". The rule at the end of
   autodetect_material_textures runs when the material loads: no normal,
   roughness or metallic map, so specular_factor 0 (and metalness_factor 0). A
   map typed in afterwards loaded and changed nothing, because the zero stayed.
   That was Matt's BFG (models/weapons2/bfg/bfg) given bfg_rough.tga, as
   texture_roughness and as stage1's own, 2026-11-18: specular 0, so no
   reflection for the roughness to act on. The next load, with the map in the
   entry, keeps specular 1; this does the same now. Not over a value the entry
   or the console states, and metalness only for a metallic map. */
static void material_revive_specular(pbr_material_t* mat, bool metallic_map)
{
	if (!MAT_SPECIFIED(mat, MAT_SPECULAR_FACTOR) && mat->specular_factor == 0.f)
	{
		mat->specular_factor = 1.f;
		Com_Printf("  specular_factor 0 -> 1: it had been zeroed at load for having no maps\n");
	}
	if (metallic_map && !MAT_SPECIFIED(mat, MAT_METALNESS_FACTOR) && mat->metalness_factor == 0.f)
	{
		mat->metalness_factor = 1.f;
		Com_Printf("  metalness_factor 0 -> 1: it had been zeroed at load for having no maps\n");
	}
}

static void autodetect_material_textures(pbr_material_t* mat, const char* name, const char* mat_name_no_ext,
	imagetype_t type, imageflags_t flags)
{
	char file_name[MAX_IMAGE_PATH];

	if (name && !MAT_SPECIFIED(mat, MAT_TEXTURE_BASE))
	{
		mat->image_base = IMG_Find(name, type, flags | IF_SRGB);
		mat->original_width = mat->image_base->width;
		mat->original_height = mat->image_base->height;
		if (mat->image_base == R_NOTEXTURE)
			mat->image_base = NULL;
		else
			Q_strlcpy(mat->filename_base, mat->image_base->filepath, sizeof(mat->filename_base));
	}

	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_NORMALS))
	{
		Q_snprintf(file_name, sizeof(file_name), "%s_n.tga", mat_name_no_ext);
		mat->image_normals = IMG_Find(file_name, type, flags);
		if (mat->image_normals == R_NOTEXTURE)
			clear_detected_slot(&mat->image_normals, mat->filename_normals);
		else
			Q_strlcpy(mat->filename_normals, mat->image_normals->filepath, sizeof(mat->filename_normals));
	}

	// RTX-Remix style sidecars: drop <name>_rough / <name>_metallic next to
	// the base texture and they are picked up with no .mat entry at all
	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_ROUGHNESS))
	{
		Q_snprintf(file_name, sizeof(file_name), "%s_rough.tga", mat_name_no_ext);
		mat->image_roughness = IMG_Find(file_name, type, flags);
		if (mat->image_roughness == R_NOTEXTURE)
			clear_detected_slot(&mat->image_roughness, mat->filename_roughness);
		else
			Q_strlcpy(mat->filename_roughness, mat->image_roughness->filepath, sizeof(mat->filename_roughness));
	}

	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_METALLIC))
	{
		Q_snprintf(file_name, sizeof(file_name), "%s_metallic.tga", mat_name_no_ext);
		mat->image_metallic = IMG_Find(file_name, type, flags);
		if (mat->image_metallic == R_NOTEXTURE)
			clear_detected_slot(&mat->image_metallic, mat->filename_metallic);
		else
			Q_strlcpy(mat->filename_metallic, mat->image_metallic->filepath, sizeof(mat->filename_metallic));
	}

	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_HEIGHT))
	{
		Q_snprintf(file_name, sizeof(file_name), "%s_height.tga", mat_name_no_ext);
		mat->image_height = IMG_Find(file_name, type, flags);
		if (mat->image_height == R_NOTEXTURE)
			clear_detected_slot(&mat->image_height, mat->filename_height);
		else
			Q_strlcpy(mat->filename_height, mat->image_height->filepath, sizeof(mat->filename_height));
	}

	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
	{
		Q_snprintf(file_name, sizeof(file_name), "%s_light.tga", mat_name_no_ext);
		mat->image_emissive = IMG_Find(file_name, type, flags | IF_SRGB);
		if (mat->image_emissive == R_NOTEXTURE)
			clear_detected_slot(&mat->image_emissive, mat->filename_emissive);
		else
			Q_strlcpy(mat->filename_emissive, mat->image_emissive->filepath, sizeof(mat->filename_emissive));

		// The rerelease calls its emissive maps "_glow" rather than "_light" and
		// ships one beside nearly every md5/ skin.
		//
		// This is deliberately NOT a general fallback. 1571 _glow.png files
		// exist under rerelease/ + baseq2/ and only 143 are under md5/; of the
		// rest, 509 have no material definition and are mostly textures/e1u1
		// wall lights. Accepting the suffix everywhere would silently turn all
		// of them into light sources and relight every map - see
		// [[q2rtx-material-defs]]: a material change is never neutral.
		if (!mat->image_emissive && strstr(mat_name_no_ext, "/md5/"))
		{
			Q_snprintf(file_name, sizeof(file_name), "%s_glow.png", mat_name_no_ext);
			mat->image_emissive = IMG_Find(file_name, type, flags | IF_SRGB);
			if (mat->image_emissive == R_NOTEXTURE)
				mat->image_emissive = NULL;
			else
			{
				// a _glow map masks with alpha; Q2RTX masks with RGB
				vkpt_fold_emissive_alpha(mat->image_emissive);
				Q_strlcpy(mat->filename_emissive, mat->image_emissive->filepath, sizeof(mat->filename_emissive));
			}
		}
	}

	// If there is no normals/metalness image, assume that the material is a
	// basic diffuse one - unless the definition asked for these itself, in
	// which case zeroing them throws away the only thing it said.
	//
	// A dedicated _metallic / _rough map counts as asking: the shader reads
	// metallic = sample * metalness_factor, so zeroing the factor here threw a
	// metallic map away whenever the texture had no normal map beside it.
	//
	// "Has a map" has to include one the entry only NAMES. On a cold load
	// MAT_Find runs this before it loads the entry's texture_roughness /
	// texture_metallic, so the images are still NULL here and a tuning-only
	// md5 entry with both maps came up specular 0 / metalness 0 - and
	// MAT_InheritScalars then (rightly) refused to inherit over its own maps.
	// 'mat reload' loads the named maps first, which is why it looked right
	// while being tuned and wrong from the next map load on.
	// (Only a name the definition states: a reload refreshes specified_fields
	// but leaves the old filename in a slot the entry has stopped naming.)
	//
	// A Quake III stage's own map counts too (q3_stages_name_maps): the BFG
	// with stage1_texture_roughness alone came up specular 0 every load.
	bool stage_metallic;
	const bool stage_maps = q3_stages_name_maps(mat->name, &stage_metallic);
	const bool has_roughness = mat->image_roughness
		|| (MAT_SPECIFIED(mat, MAT_TEXTURE_ROUGHNESS) && mat->filename_roughness[0]) || stage_maps;
	const bool has_metallic = mat->image_metallic
		|| (MAT_SPECIFIED(mat, MAT_TEXTURE_METALLIC) && mat->filename_metallic[0]) || stage_metallic;

	if (!mat->image_normals)
	{
		if (!MAT_SPECIFIED(mat, MAT_SPECULAR_FACTOR) && !has_roughness && !has_metallic)
			mat->specular_factor = 0.f;
		if (!MAT_SPECIFIED(mat, MAT_METALNESS_FACTOR) && !has_metallic)
			mat->metalness_factor = 0.f;
	}
}

/*
=================
MAT_InheritScalars

Copies the layout-INDEPENDENT tuning of one material definition onto another
material.

This exists for the rerelease's MD5 models. Their meshes are re-unwrapped, so
none of Q2RTX's hand-authored PBR *textures* can be reused - measured across
seven models, the MD5 and MD2 texture coordinates for the same surface point
disagree by ~0.6 of the UV range, which is the same as two unrelated layouts.
What survives a re-unwrap is everything that is not a texture lookup: how bright
the base is, how rough the surface is, how much its emissive counts. Without
this an MD5 model renders at base_factor 1.0 against the classic model's 1.5 to
2.5, i.e. visibly darker than the MD2 it replaces.

Textures, and anything that names one, are deliberately NOT copied.

EXCEPT that roughness_override and specular_factor are not really tuning when
the classic material has no PBR maps to tune. Every one of the 140 .mat entries
carrying "roughness_override 1" pairs it with "specular_factor 0", and it means
"this MD2 has no roughness map, render it fully diffuse" - the MD2 UVs cannot
address one, so there was never anything else to say. Inheriting that pair onto
an MD5 material that DOES have its own _rough.tga sidecar pins
roughness = max(sampled, 1) = 1 in get_material() and zeroes the specular lobe,
which silently discarded all 55 baked MD5 roughness maps in the tree: the image
loaded, bound, and sampled, and the result was thrown away. So skip those two
when this material brought its own roughness map. A material with no sidecar
inherits exactly as before.
=================
*/
void MAT_InheritScalars(pbr_material_t* mat, const char* source_name)
{
	char source_no_ext[MAX_QPATH];
	const pbr_material_t* src;

	if (!mat || !source_name || !*source_name)
		return;

	truncate_extension(source_name, source_no_ext, sizeof(source_no_ext));
	Q_strlwr(source_no_ext);

	// remembered so that 'mat reload' can re-apply this - see
	// material_reapply_definition. Safe when source_name IS inherit_source.
	Q_strlcpy(mat->inherit_source, source_no_ext, sizeof(mat->inherit_source));

	src = find_material_sorted(source_no_ext, r_global_materials, num_global_materials);
	if (!src)
		return;

	// MAT_Find has already run the sidecar lookup, so these are set by now
	const bool has_own_roughness = (mat->image_roughness != NULL);
	const bool has_own_metallic = (mat->image_metallic != NULL);

	// Anything the MD5 skin's own definition states wins over the classic
	// material: writing "specular_factor 3" against the md5 skin is a
	// deliberate statement about that skin, and inheriting over it made the
	// entry look like it did nothing.
#define INHERIT(attr, field) do { if (!MAT_SPECIFIED(mat, attr)) mat->field = src->field; } while (0)

	INHERIT(MAT_BUMP_SCALE, bump_scale);
	if (!has_own_roughness) {
		INHERIT(MAT_ROUGHNESS_OVERRIDE, roughness_override);
		INHERIT(MAT_SPECULAR_FACTOR, specular_factor);
	}
	// same trap on the metallic side: get_material() computes
	// metallic = sampled * metalness_factor, so inheriting the classic
	// "metalness_factor 0" (1911 .mat entries say it, meaning "the MD2 has no
	// metallic map, ignore the normal map's alpha") multiplies an MD5's own
	// baked _metallic.tga away to nothing. Affects 6 of the 72 MD5 sidecars.
	if (!has_own_metallic)
		INHERIT(MAT_METALNESS_FACTOR, metalness_factor);
	INHERIT(MAT_EMISSIVE_FACTOR, emissive_factor);
	INHERIT(MAT_LIGHT_FACTOR, light_factor);
	INHERIT(MAT_BASE_FACTOR, base_factor);
	INHERIT(MAT_LIGHT_STYLES, light_styles);
	INHERIT(MAT_BSP_RADIANCE, bsp_radiance);
	INHERIT(MAT_DEFAULT_RADIANCE, default_radiance);
	INHERIT(MAT_EMISSIVE_THRESHOLD, emissive_threshold);
	INHERIT(MAT_VOLUMETRIC_SCALE, volumetric_scale);
	INHERIT(MAT_DLSS_GUIDE_FIELD, dlss_guide_field);
	INHERIT(MAT_ALBEDO_STRENGTH, albedo_strength);

#undef INHERIT

	// the material kind (chrome, glass, ...) describes the surface, not its
	// texture, so it carries over too - and so does the curved-water bit, which
	// is a statement about that same geometry
	uint32_t inherited_flags = 0;
	if (!MAT_SPECIFIED(mat, MAT_KIND))
		inherited_flags |= MATERIAL_KIND_MASK;
	if (!MAT_SPECIFIED(mat, MAT_CURVED_WATER))
		inherited_flags |= MATERIAL_FLAG_CURVED_WATER;

	mat->flags = (mat->flags & ~inherited_flags) | (src->flags & inherited_flags);
}

/*
=================
material_inherit_instance

A MATERIAL INSTANCE starts as an exact copy of its base - the live material, as
its own .mat entry, sidecars and any synthesized emissive left it - and every
line in the instance's own section overrides just that one thing. An empty
section looks exactly like the texture does everywhere else; "texture_height 0"
and the like drop a map the base has.

It used to start from the texture with NO .mat entry instead, re-detecting the
images from the instance's name. That name has no extension, which IMG_Find
needs, so the base image was never found and the surface rendered white - and a
base whose maps live in materialDev/ or are .png would have lost them anyway,
along with every scalar its entry set (base_factor, metalness_factor, ...).
=================
*/
static const q3shader_t* q3_shader_for(const char* name_no_ext, imagetype_t type);

static void material_inherit_instance(pbr_material_t* mat, const pbr_material_t* base)
{
	// The surface's UVs come from its texinfo and the base .wal's size, which
	// are the same whatever image this instance draws with.
	mat->original_width = base->original_width;
	mat->original_height = base->original_height;

#define INHERIT(attr, field) do { if (!MAT_SPECIFIED(mat, attr)) mat->field = base->field; } while (0)
#define INHERIT_TEXTURE(attr, image, filename) do { if (!MAT_SPECIFIED(mat, attr)) { \
		mat->image = base->image; \
		Q_strlcpy(mat->filename, base->filename, sizeof(mat->filename)); } } while (0)

	INHERIT_TEXTURE(MAT_TEXTURE_BASE,      image_base,      filename_base);
	INHERIT_TEXTURE(MAT_TEXTURE_NORMALS,   image_normals,   filename_normals);
	INHERIT_TEXTURE(MAT_TEXTURE_ROUGHNESS, image_roughness, filename_roughness);
	INHERIT_TEXTURE(MAT_TEXTURE_METALLIC,  image_metallic,  filename_metallic);
	INHERIT_TEXTURE(MAT_TEXTURE_HEIGHT,    image_height,    filename_height);
	INHERIT_TEXTURE(MAT_TEXTURE_MASK,      image_mask,      filename_mask);

	INHERIT(MAT_SYNTH_EMISSIVE, synth_emissive);
	INHERIT(MAT_EMISSIVE_THRESHOLD, emissive_threshold);

	/* A Quake III shader's stages: the instance draws a program of its own,
	   built from the same shader - so its stage<N>_* settings can differ from
	   the material it is one of, whose settings it takes for what it does not
	   state (q3stages.h STAGE SETTINGS). Shared, the program was built from the
	   base's name, and an instance's settings did nothing (Matt's q3dm2 floor,
	   2026-11-16, is an instance). The base's own images count as the
	   instance's, as they do everywhere else here. */
	mat->q3_program = base->q3_program;
	mat->q3_additive = base->q3_additive;
	if (base->q3_program && MAT_IndexOf(mat) >= 0)
	{
		const q3shader_t* q3 = q3_shader_for(base->name, base->image_type);
		if (q3)
			vkpt_q3_build_program(mat, q3, base->image_type, base->image_flags & ~IF_SRC_MASK,
				MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) || MAT_SPECIFIED(base, MAT_TEXTURE_BASE),
				MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) || MAT_SPECIFIED(base, MAT_TEXTURE_EMISSIVE));
	}

	// A synthesized emissive ('*E') is the base image's bright texels at the
	// base's threshold. An instance that changes either needs one of its own, so
	// the slot is left empty for MAT_SynthesizeEmissive - which also covers a BSP
	// surface light, synthesized by bsp_mesh_register_textures with no
	// synth_emissive saying so.
	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
	{
		const bool synthesized = base->image_emissive && strstr(base->image_emissive->name, "*E");

		if (synthesized && (MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) || MAT_SPECIFIED(mat, MAT_EMISSIVE_THRESHOLD)))
		{
			mat->image_emissive = NULL;
			mat->filename_emissive[0] = 0;
			if (!MAT_SPECIFIED(mat, MAT_SYNTH_EMISSIVE))
				mat->synth_emissive = true;
		}
		else
			INHERIT_TEXTURE(MAT_TEXTURE_EMISSIVE, image_emissive, filename_emissive);
	}

	INHERIT(MAT_BUMP_SCALE, bump_scale);
	INHERIT(MAT_DISPLACE_IN, displace_in);
	INHERIT(MAT_DISPLACE_OUT, displace_out);
	INHERIT(MAT_DISPLACE_CLAMP, displace_clamp);
	INHERIT(MAT_ALPHA_GLASS, alpha_glass);
	INHERIT(MAT_CAUSTIC, glass_caustic);
	INHERIT(MAT_CAUSTIC_RIPPLE, caustic_ripple);
	INHERIT(MAT_LIGHT_FACTOR, light_factor);
	INHERIT(MAT_ROUGHNESS_OVERRIDE, roughness_override);
	INHERIT(MAT_METALNESS_FACTOR, metalness_factor);
	INHERIT(MAT_EMISSIVE_FACTOR, emissive_factor);
	INHERIT(MAT_SPECULAR_FACTOR, specular_factor);
	INHERIT(MAT_BASE_FACTOR, base_factor);
	INHERIT(MAT_LIGHT_STYLES, light_styles);
	INHERIT(MAT_BSP_RADIANCE, bsp_radiance);
	INHERIT(MAT_DEFAULT_RADIANCE, default_radiance);
	INHERIT(MAT_VOLUMETRIC_SCALE, volumetric_scale);
	INHERIT(MAT_DLSS_GUIDE_FIELD, dlss_guide_field);
	INHERIT(MAT_ALBEDO_STRENGTH, albedo_strength);

#undef INHERIT_TEXTURE
#undef INHERIT

	uint32_t inherited_flags = 0;
	if (!MAT_SPECIFIED(mat, MAT_KIND))
		inherited_flags |= MATERIAL_KIND_MASK;
	if (!MAT_SPECIFIED(mat, MAT_IS_LIGHT))
		inherited_flags |= MATERIAL_FLAG_LIGHT;
	if (!MAT_SPECIFIED(mat, MAT_CURVED_WATER))
		inherited_flags |= MATERIAL_FLAG_CURVED_WATER;
	if (!MAT_SPECIFIED(mat, MAT_DOUBLE_SIDED))
		inherited_flags |= MATERIAL_FLAG_DOUBLE_SIDED;

	mat->flags = (mat->flags & ~inherited_flags) | (base->flags & inherited_flags);
}

/*
=================
QUAKE III SHADERS AS MATERIAL DEFAULTS

A Quake III surface is named by a shader (scripts/*.shader), not an image: the
name "textures/base_light/ceil1_22a" may draw from ceil1_22a.tga under a
lightmap with ceil1_22a.blend.tga added on top as the glow. When a material is
asked for by a name some script defines, the shader's facts fill whatever the
.mat definition (if any) did not state - base, glow, cut-out mask, kind,
is_light - so a Quake III map lights and looks roughly right with no
materials written, and a .mat entry in baseq3/materials still overrides any
of it one field at a time, the same way a tuning-only entry does for a
Quake II texture.

A name no script defines is an "implicit" shader in Quake III - just the image
of that name - and is left to the usual lookup, which finds the .tga or .jpg.
=================
*/
static const q3shader_t* q3_shader_for(const char* name_no_ext, imagetype_t type)
{
	if (type != IT_WALL && type != IT_SKIN)
		return NULL;
	Q3Shader_Load();    // once; indexes the scripts if there are any
	return Q3Shader_Find(name_no_ext);
}

// The Quake III shader a live material draws - an instance's is the one of the
// material it is one of ("name#1a2b3c4d" is no shader's name).
static const q3shader_t* q3_shader_of(const pbr_material_t* mat)
{
	char name[MAX_QPATH];
	Q_strlcpy(name, mat->name, sizeof(name));
	const char* sep = instance_separator(name);
	if (sep)
		name[sep - name] = 0;
	return q3_shader_for(name, mat->image_type);
}

const struct q3shader_s* MAT_Q3Shader(const pbr_material_t* mat)
{
	return mat ? q3_shader_of(mat) : NULL;
}

// Rebuilds a live material's stage program the way a load builds it (an
// instance's as material_inherit_instance does), after a console change - and
// then its instances', whose programs take what they do not state from it.
static void material_rebuild_program(pbr_material_t* mat, const q3shader_t* q3)
{
	const pbr_material_t* base = find_instance_base(mat->name);
	vkpt_q3_build_program(mat, q3, base ? base->image_type : mat->image_type,
		(base ? base->image_flags : mat->image_flags) & ~IF_SRC_MASK,
		MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) || (base && MAT_SPECIFIED(base, MAT_TEXTURE_BASE)),
		MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) || (base && MAT_SPECIFIED(base, MAT_TEXTURE_EMISSIVE)));

	if (base)
		return;
	for (int i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		pbr_material_t* m = r_materials + i;
		if (m != mat && m->registration_sequence && m->q3_program && instance_separator(m->name)
			&& find_instance_base(m->name) == mat)
			material_rebuild_program(m, q3);
	}
}

// The image stage `index` of `q3` draws from - or the one a .mat
// "stage<N>_map" line puts there instead (q3stages.h).
static image_t* q3_stage_image(const pbr_material_t* mat, const q3shader_t* q3, int index, imagetype_t type, imageflags_t flags)
{
	char path[MAX_IMAGE_PATH];

	if (index < 0 || index >= q3->num_stages)
		return NULL;

	const q3stage_t* stage = &q3->stages[index];
	// "stage<N>_map off": the stage gives the material nothing
	if (mat && vkpt_q3_stage_off(mat->name, index))
		return NULL;
	const char* override = mat ? vkpt_q3_stage_override(mat->name, index) : NULL;

	// IMG_FindFile: the file named, not its .jpg/.png twin already loaded (as
	// q3stages.c load_stage_image)
	if (override)
	{
		image_t* image = IMG_FindFile(override, type, flags);
		if (image != R_NOTEXTURE)
			return image;
		if (Q3Shader_ResolveImage(override, path, sizeof(path)))
		{
			image = IMG_FindFile(path, type, flags);
			if (image != R_NOTEXTURE)
				return image;
		}
		// the file is missing: the stage as it is without the line (q3stages.c)
	}

	if (!stage->map[0] || stage->lightmap || stage->white)
		return NULL;
	if (!Q3Shader_ResolveImage(stage->map, path, sizeof(path)))
		return NULL;

	image_t* image = IMG_FindFile(path, type, flags);
	return image == R_NOTEXTURE ? NULL : image;
}

// The stage a shader's surface colour comes from: its colour stage, or for a
// glow-only effect the glow, or failing both the first image it draws at all
// (a chrome powerup that is nothing but an environment map). -1 for none.
// Not a stage `mat`'s entry turns off (stage<N>_map off); mat may be NULL.
static int q3_base_stage_index(const pbr_material_t* mat, const q3shader_t* q3)
{
#define STAGE_ON(i) (!mat || !vkpt_q3_stage_off(mat->name, (i)))
	if (q3->diffuse_stage >= 0 && STAGE_ON(q3->diffuse_stage))
		return q3->diffuse_stage;
	if (q3->emissive_stage >= 0 && STAGE_ON(q3->emissive_stage))
		return q3->emissive_stage;
	for (int i = 0; i < q3->num_stages; i++)
	{
		const q3stage_t* st = &q3->stages[i];
		if (st->map[0] && !st->lightmap && !st->white && !st->video && STAGE_ON(i))
			return i;
	}
	return -1;
#undef STAGE_ON
}

static const q3stage_t* q3_base_stage(const q3shader_t* q3)
{
	int index = q3_base_stage_index(NULL, q3);
	return index >= 0 ? &q3->stages[index] : NULL;
}

/* The solid stand-ins q3_solid_slot makes (vkpt_solid_texture: "<image>*W.wal"
   and the rest), which apply_q3_shader puts in for glass, mirrors and chrome. */
static bool q3_is_solid_clone(const image_t* image)
{
	return image && solid_tag_index(image->name) >= 0;
}

static void q3_drop_solid_slot(pbr_material_t* mat, int attr, image_t** slot, char* filename)
{
	if (MAT_SPECIFIED(mat, attr) || !q3_is_solid_clone(*slot))
		return;
	*slot = NULL;
	filename[0] = 0;
}

/* Glass in Quake III is a reflection map added over what is behind it and
   nothing else (glass01, the globe of kmlamp_white): a see-through surface
   whose only stages are environment maps that add. */
// every stage but the lightmap a reflection map added over what is there
static bool q3_env_add_only(const q3shader_t* q3)
{
	bool any_env = false;
	for (int i = 0; i < q3->num_stages; i++)
	{
		const q3stage_t* st = &q3->stages[i];
		if (st->lightmap || st->white)
			continue;
		if (!st->env_map || st->blend != Q3BLEND_ADD)
			return false;
		any_env = true;
	}
	return any_env;
}

// a see-through surface drawn only from reflection maps: Quake III's glass
static bool q3_is_env_glass(const q3shader_t* q3, imagetype_t type)
{
	// moving (the electric tube round q3dm0's tesla coil) is a glow, drawn by
	// its program; a lamp globe (q3map_surfacelight) glows, see env_light
	if (!q3->translucent || q3->animated || q3->surface_light > 0)
		return false;
	// A map's own surface needs no "surfaceparm trans": Quake III added the
	// reflection over what was behind it all the same (the console pane in
	// q3dm0, models/mapobjects/console/jacobs). A model's does - the item
	// shaders made of reflection maps are not all glass.
	if (type != IT_WALL && !(q3->trans || (q3->content_flags & Q3_CONTENTS_TRANSLUCENT)))
		return false;
	return q3_env_add_only(q3);
}

// solid images standing in for maps the shader never had (vkpt_solid_texture)
static void q3_solid_slot(pbr_material_t* mat, image_t* like, int attr, image_t** slot, char* filename,
                          const char* tag, byte r, byte g, byte b)
{
	if (MAT_SPECIFIED(mat, attr) || !like)
		return;
	image_t* solid = vkpt_solid_texture(like, tag, r, g, b);
	if (!solid)
		return;
	*slot = solid;
	Q_strlcpy(filename, solid->name, MAX_IMAGE_PATH);
}

static void apply_q3_shader(pbr_material_t* mat, const q3shader_t* q3, imagetype_t type, imageflags_t flags)
{
	const int base_index = q3_base_stage_index(mat, q3);

	/* A KIND TAKEN BACK. Glass, a mirror and chrome swap the base (and the
	   roughness and metallic) for solid stand-ins below, and the image found
	   for a slot is only ever looked for while the slot is EMPTY - so after one
	   'mat reload' as GLASS, the white base stayed through every reload after:
	   Matt's slamp2 lamp shade, put back to REGULAR, printed texture_base
	   slamp2*W.wal and its pink was gone (2026-11-17). The stand-ins are this
	   function's own doing, so it starts without them; what the kind wants now
	   puts them back. Not a slot the entry names. */
	q3_drop_solid_slot(mat, MAT_TEXTURE_BASE, &mat->image_base, mat->filename_base);
	q3_drop_solid_slot(mat, MAT_TEXTURE_ROUGHNESS, &mat->image_roughness, mat->filename_roughness);
	q3_drop_solid_slot(mat, MAT_TEXTURE_METALLIC, &mat->image_metallic, mat->filename_metallic);

	/* The entry's own glow, loaded NOW: the program below draws it in place of
	   the shader's glow stage, and the light list samples the program. A
	   tuning-only entry (no texture_base) reached here from MAT_Find before its
	   texture_emissive was loaded - that load comes after this call - so the
	   program found no image and drew the shader's glow after all: Matt's
	   shinymetaldoor_outside3a2_light.png listed in 'mat print', and the door
	   still showed shinymetaldoor_outside3glow (2026-11-16). 'mat reload' loads
	   before calling this, which is why only a fresh map load showed it. */
	if (MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) && mat->filename_emissive[0] && !mat->image_emissive)
	{
		load_material_image(&mat->image_emissive, mat->filename_emissive, mat, type, flags | IF_SRGB);
		if (mat->image_emissive == R_NOTEXTURE)
			mat->image_emissive = NULL;
	}

	/* What the shader gives a slot the entry leaves unsaid - or names a file
	   for that is missing, which leaves the material as it is without the line
	   (only the edited materialDev copies are in git). The entry's name stays
	   in the slot, so 'mat print' shows what it asked for. */
	const bool base_named = MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) && mat->filename_base[0];
	if ((!MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) || base_named) && !mat->image_base && base_index >= 0)
	{
		mat->image_base = q3_stage_image(mat, q3, base_index, type, flags | IF_SRGB);
		if (mat->image_base)
		{
			if (!base_named)
				Q_strlcpy(mat->filename_base, mat->image_base->filepath, sizeof(mat->filename_base));
			mat->original_width = mat->image_base->width;
			mat->original_height = mat->image_base->height;
		}
	}

	const bool emissive_named = MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) && mat->filename_emissive[0];
	if ((!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) || emissive_named) && !mat->image_emissive && q3->emissive_stage >= 0)
	{
		mat->image_emissive = q3_stage_image(mat, q3, q3->emissive_stage, type, flags | IF_SRGB);
		if (mat->image_emissive && !emissive_named)
			Q_strlcpy(mat->filename_emissive, mat->image_emissive->filepath, sizeof(mat->filename_emissive));
	}

	// Animated or layered: drawn stage by stage (q3stages.c). The material's
	// images above stay as the still picture everything else sees.
	const bool program = vkpt_q3_wants_program(q3);
	const bool env_glass = q3_is_env_glass(q3, type);

	/* A lamp globe drawn only from a reflection map (models/mapobjects/
	   lamplight_y, q3dm7): q3map_surfacelight made it the lamp, and the map
	   added over what was behind it is all Quake III showed of it. So the map
	   is its glow - an additive effect, like a flame - and the light it sheds
	   (MATERIAL_FLAG_LIGHT below). */
	const bool env_light = !program && q3->translucent && q3->surface_light > 0 &&
		!q3->sky && !q3->portal && q3_env_add_only(q3);
	if (env_light && !MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE) && !mat->image_emissive && mat->image_base)
	{
		mat->image_emissive = mat->image_base;
		Q_strlcpy(mat->filename_emissive, mat->image_base->filepath, sizeof(mat->filename_emissive));
	}

	// Alpha-tested: cut out where the colour texture's alpha says. Not for a
	// see-through program: its cut-out stage is one layer of several, and the
	// glows around it (the spinning logo's halo) must not be cut with it.
	if (!MAT_SPECIFIED(mat, MAT_TEXTURE_MASK) && !mat->image_mask && q3->alpha_test && mat->image_base
		&& !(program && q3->translucent))
	{
		int func = q3->stages[q3->diffuse_stage].alpha_func;
		mat->image_mask = vkpt_alpha_mask_texture(mat->image_base, func ? func : Q3ALPHA_GE128);
		if (mat->image_mask)
			Q_strlcpy(mat->filename_mask, mat->image_mask->name, sizeof(mat->filename_mask));
	}

	/* An alpha test that no pixel passes: Quake III never drew the surface.
	   models/mapobjects/console/under (q3dm0, the stand under the console) cuts
	   its image at alpha 128, and under.tga's alpha is 0 all over - the artist's
	   cables never showed. Drawn anyway it came out a white post (Matt,
	   2026-11-16). INVISIBLE leaves it out of the world (bsp_mesh.c); a .mat
	   'kind' brings it back. */
	const bool never_drawn = q3->alpha_test && !(program && q3->translucent)
		&& mat->image_mask && mat->image_mask->alpha_mask_empty
		&& !MAT_SPECIFIED(mat, MAT_TEXTURE_MASK);

	if (!MAT_SPECIFIED(mat, MAT_KIND))
	{
		uint32_t kind = MATERIAL_KIND_REGULAR;
		int contents = q3->content_flags;

		if (never_drawn)
			kind = MATERIAL_KIND_INVISIBLE;
		/* A model's surface whose shader draws nothing - "surfaceparm nodraw", or
		   no stages at all - is not there. Hunter's harpy skin puts its feathers
		   on harpy_f, which is only that: with no image they came out a white
		   spiky hood over her head (Matt, 2026-11-17). compute_mesh_material_flags
		   skips an INVISIBLE mesh. Not the world's: a hidden face there can still
		   be a q3map_surfacelight lamp, and bsp_mesh.c has its own nodraw rules. */
		else if (q3->no_draw && type == IT_SKIN)
			kind = MATERIAL_KIND_INVISIBLE;
		else if (q3->sky || (q3->surface_flags & Q3_SURF_SKY))
			kind = MATERIAL_KIND_SKY;
		else if (contents & Q3_CONTENTS_LAVA)
			kind = MATERIAL_KIND_LAVA;
		else if (contents & Q3_CONTENTS_SLIME)
			kind = MATERIAL_KIND_SLIME;
		else if (contents & Q3_CONTENTS_WATER)
			kind = MATERIAL_KIND_WATER;
		// "portal": a mirror, or a window onto a misc_portal_camera's view
		// (cameras.c) - which bsp_mesh.c makes a mirror after all on a face no
		// portal camera owns, as Quake III does
		else if (q3->portal)
			kind = Q_strHas(q3->name, "mirror") ? MATERIAL_KIND_CHROME : MATERIAL_KIND_CAMERA;
		else if (env_glass || (q3->translucent && Q_strHas(q3->name, "glass")))
			kind = MATERIAL_KIND_GLASS;
		else if (q3->chrome)
			kind = MATERIAL_KIND_CHROME;
		// see-through: an effect that only adds light, or a layered program
		// whose stages leave some of what is behind it showing (per pixel, in
		// the program's alpha)
		else if (q3->additive || env_light || (program && q3->translucent))
			kind = MATERIAL_KIND_TRANSPARENT;

		mat->flags = MAT_SetKind(mat->flags, kind);
	}

	const bool is_glass = MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS);
	const bool is_chrome = MAT_IsKind(mat->flags, MATERIAL_KIND_CHROME) && !q3->portal;

	/* ALPHA GLASS. A glass shader whose colour stage is cut out by its alpha
	   (alphaFunc) is a pane with its frame painted round it on the same face -
	   textures/base_wall/glass_frame, q3dm0's column: a reflection map added,
	   then the frame image alpha-tested over it. Quake III drew the frame solid
	   and the cut-out as only the reflection. Made all glass, the frame's dark
	   pixels tinted the whole column black (Matt, 2026-11-16). So: glass only
	   where the image is see-through, an ordinary surface elsewhere (the
	   shaders' alpha_glass_solid), and NO mask - that would cut the pane out
	   altogether, leaving nothing to be glass. */
	if (!MAT_SPECIFIED(mat, MAT_ALPHA_GLASS))
		mat->alpha_glass = is_glass && q3->alpha_test && mat->image_base != NULL;
	if (mat->alpha_glass && !MAT_SPECIFIED(mat, MAT_TEXTURE_MASK))
	{
		mat->image_mask = NULL;
		mat->filename_mask[0] = 0;
	}

	// An effect that only adds light: none of the surface shows, only its glow
	// over what is behind it (bsp_mesh.c gives its faces alpha 0). A program
	// works out its own alpha per pixel.
	mat->q3_additive = (q3->additive || env_light) && !program && MAT_IsKind(mat->flags, MATERIAL_KIND_TRANSPARENT);

	/* Glass drawn from a reflection map: the map is Quake III's fake of what
	   the glass reflects, and as the base colour it TINTS what is seen through
	   it (glass-base-tints-transmission) - tinfx.tga made a vial of dark smoked
	   glass. Clear glass, a white base; the path tracer does the reflection.
	   The same for a shader of reflection maps alone that a .mat entry calls
	   glass - Quake III's health spheres, which no "surfaceparm trans" marks. */
	if (is_glass && (env_glass || q3_env_add_only(q3)) && mat->image_base)
	{
		q3_solid_slot(mat, mat->image_base, MAT_TEXTURE_BASE, &mat->image_base, mat->filename_base, "W", 255, 255, 255);
		if (!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
		{
			mat->image_emissive = NULL;
			mat->filename_emissive[0] = 0;
		}
	}

	/* A mirror or portal: Matt's call - fully white, roughness 0 - which is also
	   what the path tracer needs to treat it as one at all (a surface at or over
	   MAX_MIRROR_ROUGHNESS is drawn as an ordinary one, and with no maps a
	   material is pinned at roughness 1). Solid clones of the shader's own image
	   (textures.c, vkpt_solid_texture), unless the .mat entry names its own. No
	   glow: what shows is the reflection or the view. */
	if (q3->portal && mat->image_base)
	{
		image_t* image = mat->image_base;
		q3_solid_slot(mat, image, MAT_TEXTURE_BASE, &mat->image_base, mat->filename_base, "W", 255, 255, 255);
		if (!mat->image_roughness)
			q3_solid_slot(mat, image, MAT_TEXTURE_ROUGHNESS, &mat->image_roughness, mat->filename_roughness, "R0", 0, 0, 0);
		if (!mat->image_metallic)
			q3_solid_slot(mat, image, MAT_TEXTURE_METALLIC, &mat->image_metallic, mat->filename_metallic, "M1", 255, 255, 255);
		if (!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
		{
			mat->image_emissive = NULL;
			mat->filename_emissive[0] = 0;
		}
	}
	/* Chrome (chrome_metal): Quake III's reflection map faked a polished metal,
	   so draw polished metal - a bright base, glossy, fully metallic. Not a
	   perfect mirror (roughness over MAX_MIRROR_ROUGHNESS): a pillar reads as
	   chrome with a little blur. The .mat entry can name real maps. */
	else if (is_chrome && mat->image_base)
	{
		image_t* image = mat->image_base;
		q3_solid_slot(mat, image, MAT_TEXTURE_BASE, &mat->image_base, mat->filename_base, "C", 230, 230, 230);
		if (!mat->image_roughness)
			q3_solid_slot(mat, image, MAT_TEXTURE_ROUGHNESS, &mat->image_roughness, mat->filename_roughness, "R20", 20, 20, 20);
		if (!mat->image_metallic)
			q3_solid_slot(mat, image, MAT_TEXTURE_METALLIC, &mat->image_metallic, mat->filename_metallic, "M1", 255, 255, 255);
		if (!MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
		{
			mat->image_emissive = NULL;
			mat->filename_emissive[0] = 0;
		}
	}

	/* ...AND METAL IT IS. The metallic map above is read times metalness_factor,
	   and autodetect_material_textures - which runs before this, when the
	   material had no maps at all - took it for a "basic diffuse" surface and
	   set metalness_factor and specular_factor to 0. So every Quake III chrome
	   came out metallic 0, specular 0: matte grey at R20, and the instances
	   made of it inherited the zeros (Matt's q3dm12 chrome_metal instance,
	   2026-11-17, still fully rough with roughness_override 0.005). Not over
	   what the entry states. */
	if ((q3->portal || is_chrome) && mat->image_base
		&& (mat->image_metallic || (MAT_SPECIFIED(mat, MAT_TEXTURE_METALLIC) && mat->filename_metallic[0])))
	{
		if (!MAT_SPECIFIED(mat, MAT_METALNESS_FACTOR))
			mat->metalness_factor = 1.f;
		if (!MAT_SPECIFIED(mat, MAT_SPECULAR_FACTOR))
			mat->specular_factor = 1.f;
	}

	/* GLASS THAT MOVES. A .mat entry that makes a stage program glass keeps the
	   program for its GLOW, which is the part that animates. That is Matt's
	   textures/sfx/teslacoil, the electric tube round q3dm0's tesla coil, set
	   to kind GLASS. Glass used to get no program at all: a live 'mat kind GLASS'
	   kept the one built at load, so the glass animated until the next restart
	   and then stood still (Matt, 2026-11-16).

	   The program's COLOUR does not tint the glass. Matt: "it should just be
	   flat surface with white albedo and zero roughness". So the base is white
	   unless the entry names its own texture_base, and path_tracer_rgen.h leaves
	   a glass base alone.

	   Glass that Quake III drew from reflection maps keeps its old rules: no
	   program, white base. That covers env_glass and env-only shaders such as
	   the health spheres.

	   ALPHA glass keeps its program too, but not the white base. The base's
	   alpha is the frame (alpha_glass_solid), and the white clone is opaque all
	   over, so the whole face would be frame. The teslacoil entry with
	   alpha_glass 1: the cables (stage3 cabletest2) are the solid frame, the
	   rest is clear glass, and the electric glow animates. Alpha glass used to
	   get no program, so a live 'mat alpha_glass 1' animated until the next
	   restart and then stood still (Matt, 2026-11-17). */
	const bool glass_program = program && is_glass && MAT_SPECIFIED(mat, MAT_KIND)
		&& !env_glass && !q3_env_add_only(q3);
	if (glass_program && !mat->alpha_glass && mat->image_base)
		q3_solid_slot(mat, mat->image_base, MAT_TEXTURE_BASE, &mat->image_base, mat->filename_base, "W", 255, 255, 255);

	if (program && (!is_glass || glass_program) && !is_chrome)
		vkpt_q3_build_program(mat, q3, type, flags,
			MAT_SPECIFIED(mat, MAT_TEXTURE_BASE), MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE));
	else
		mat->q3_program = 0;

	/* pt_q3_env_shine (q3stages.c): a reflection map that stood in for a shine is
	   not drawn; the surface reflects the scene instead. Its starting values go
	   on the material itself, where 'mat print' shows them and the entry or
	   'mat roughness_override' / 'mat metalness_factor' tune them (Matt,
	   2026-11-16). A metal shader: glossy metal all over. Any other: glossy
	   where the reflection map showed (the shaders' q3_shine). Specular 1, which
	   a material with no normal map would otherwise be denied (the "basic
	   diffuse" rule above). Not over the entry's own roughness or metallic map:
	   roughness_override is a floor under one, metalness_factor scales the other. */
	bool shine_metal;
	if (vkpt_q3_program_shines(mat, &shine_metal))
	{
		const bool has_roughness = mat->image_roughness
			|| (MAT_SPECIFIED(mat, MAT_TEXTURE_ROUGHNESS) && mat->filename_roughness[0]);
		const bool has_metallic = mat->image_metallic
			|| (MAT_SPECIFIED(mat, MAT_TEXTURE_METALLIC) && mat->filename_metallic[0]);

		if (!MAT_SPECIFIED(mat, MAT_ROUGHNESS_OVERRIDE) && !has_roughness)
			mat->roughness_override = (float)(shine_metal ? Q3_SHINE_ROUGHNESS_METAL : Q3_SHINE_ROUGHNESS);
		if (!MAT_SPECIFIED(mat, MAT_METALNESS_FACTOR) && !has_metallic && shine_metal)
			mat->metalness_factor = (float)Q3_SHINE_METALLIC;
		if (!MAT_SPECIFIED(mat, MAT_SPECULAR_FACTOR))
			mat->specular_factor = 1.f;
	}

	// q3map_surfacelight is the map author saying "this lights the room"
	if (!MAT_SPECIFIED(mat, MAT_IS_LIGHT) && q3->surface_light > 0 && !q3->sky && !q3->portal)
		mat->flags |= MATERIAL_FLAG_LIGHT;

	/* EVERY GLOW IS A LIGHT (Matt, 2026-11-16: "Every emissive should be a real
	   light source", with "an emissive_factor and default_radiance that I can
	   change and tweak or override with a different emissive map").

	   A map surface with a glow stage sheds the light it shows. One the author
	   marked q3map_surfacelight that has no glow stage gets a glow made from its
	   colour image's bright pixels (synth_emissive), as Quake II's SURF_LIGHT
	   textures do - a program surface still DRAWS its own stages, so there the
	   made-up image only feeds the light list.

	   The brightness lives on the material, where 'mat print' shows it and the
	   entry, 'mat default_radiance' or 'mat emissive_factor' tune it, and
	   'texture_emissive' swaps the glow: q3map_surfacelight's value at
	   pt_bsp_radiance_scale - the very number bsp_radiance gave these faces
	   before, so nothing looks different - and 1, the default, for a glow the
	   author gave no value.

	   Not a sky, portal, glass, chrome or anything never drawn. Not a model's
	   (IT_SKIN): an item would shed a light per triangle - 'is_light 1' on its
	   entry still makes it one. Not a liquid unless pt_enable_surface_lights_warp
	   says so, as for Quake II. Not a see-through surface left REGULAR here,
	   which bsp_mesh.c turns TRANSPARENT only while it is not a light. */
	if (type == IT_WALL && q3->surface_light > 0 && !q3->sky && !q3->portal
		&& !MAT_SPECIFIED(mat, MAT_BSP_RADIANCE) && !MAT_SPECIFIED(mat, MAT_DEFAULT_RADIANCE))
	{
		mat->bsp_radiance = false;
		mat->default_radiance = q3->surface_light * cvar_pt_bsp_radiance_scale->value;

		/* SEEN AT ITS OWN COLOUR. q3map_surfacelight says how much light the
		   surface SHEDS - flame2 5500, the proto_light_2k fixtures 2000 - and at
		   pt_bsp_radiance_scale that is 5.5 and 2 times the glow. Quake III drew
		   the surface itself at its stages' colour, 1, whatever light it gave
		   off; drawn 5.5 times over, the torch flames burned out to a red rim
		   round a white core and the fixtures to white blobs (Matt, 2026-11-16).
		   So the surface shows at 1 and the light it casts stays what it was:
		   emissive_factor 1/R scales both, light_factor R puts the light back
		   (copy_light: R x 1/R x R). Only while the entry states neither -
		   'mat print' shows the two, and either one in the entry takes over. */
		if (mat->default_radiance > 1.f
			&& !MAT_SPECIFIED(mat, MAT_EMISSIVE_FACTOR) && !MAT_SPECIFIED(mat, MAT_LIGHT_FACTOR))
		{
			mat->emissive_factor = 1.f / mat->default_radiance;
			mat->light_factor = mat->default_radiance;
		}
	}

	/* Not a glow that travels with its lamp (Q3Shader_IsTravellingGlow: the
	   flying skull lamp's jets, bot_flare2). It is drawn as a q3deform model,
	   which a light material cannot be, and its light polys would hang where
	   the map put it while the jets bob away. The lamp's light is the spot the
	   client puts in place of its flare (client q3lamps.c). 'is_light 1' in the
	   entry still makes it one - then it stays still in the world. */
	const bool travelling_glow = Q3Shader_IsTravellingGlow(q3);

	const bool liquid =MAT_IsKind(mat->flags, MATERIAL_KIND_WATER) || MAT_IsKind(mat->flags, MATERIAL_KIND_SLIME);
	if (type == IT_WALL && !q3->sky && !q3->portal && !q3->no_draw && !is_glass && !is_chrome && !travelling_glow
		&& !(q3->translucent && MAT_IsKind(mat->flags, MATERIAL_KIND_REGULAR))
		&& (!liquid || cvar_pt_enable_surface_lights_warp->integer))
	{
		if (mat->image_emissive)
		{
			if (!MAT_SPECIFIED(mat, MAT_IS_LIGHT))
				mat->flags |= MATERIAL_FLAG_LIGHT;
		}
		else if (q3->surface_light > 0 && mat->image_base && (mat->flags & MATERIAL_FLAG_LIGHT)
			&& cvar_pt_enable_surface_lights->integer
			&& !MAT_SPECIFIED(mat, MAT_SYNTH_EMISSIVE) && !MAT_SPECIFIED(mat, MAT_TEXTURE_EMISSIVE))
		{
			// MAT_Find / material_reapply_definition make it (MAT_SynthesizeEmissive)
			mat->synth_emissive = true;
		}
	}

	// "cull none": seen, and hit, from both sides - foliage, banners, grates
	if (!MAT_SPECIFIED(mat, MAT_DOUBLE_SIDED) && q3->cull == Q3CULL_NONE)
		mat->flags |= MATERIAL_FLAG_DOUBLE_SIDED;
}

pbr_material_t* MAT_Find(const char* name, imagetype_t type, imageflags_t flags)
{
	char mat_name_no_ext[MAX_QPATH];
	truncate_extension(name, mat_name_no_ext, sizeof(mat_name_no_ext));
	Q_strlwr(mat_name_no_ext);

	/* A MATERIAL INSTANCE is named <texture>#<geometry hash> - see
	   MAT_FindInstance. The suffix is part of the material's IDENTITY, which is
	   the point of it, but it is not part of any path on disk: the instance
	   still draws with its base texture unless its own .mat section says
	   otherwise.

	   So the name splits in two here. `mat_name_no_ext` stays whole and keys
	   the material; `texture_name` has the suffix cut off and is what every
	   lookup that touches the filesystem uses. What the instance's own section
	   leaves unsaid comes from the live base material - see
	   material_inherit_instance - so an empty section looks exactly like the
	   texture does everywhere else. */
	char texture_name[MAX_QPATH];
	char texture_name_no_ext[MAX_QPATH];
	Q_strlcpy(texture_name, name, sizeof(texture_name));
	Q_strlcpy(texture_name_no_ext, mat_name_no_ext, sizeof(texture_name_no_ext));
	{
		const char* sep = instance_separator(texture_name);
		if (sep) texture_name[sep - texture_name] = 0;
		sep = instance_separator(texture_name_no_ext);
		if (sep) texture_name_no_ext[sep - texture_name_no_ext] = 0;
	}

	const pbr_material_t* instance_base = find_instance_base(mat_name_no_ext);

	uint32_t hash = Com_HashString(mat_name_no_ext, RMATERIALS_HASH);
	
	pbr_material_t* mat = find_material(mat_name_no_ext, hash, r_materials, MAX_PBR_MATERIALS);
	
	if (mat)
	{
		MAT_UpdateRegistration(mat);
		return mat;
	}

	mat = allocate_material();

	pbr_material_t* matdef = find_material_sorted(mat_name_no_ext, r_global_materials, num_global_materials);
	
	if (type == IT_WALL)
	{
		pbr_material_t* map_mat = find_material_sorted(mat_name_no_ext, r_map_materials, num_map_materials);

		if (map_mat)
			matdef = map_mat;
	}

	/* Some games override baseq2 assets without changing the name -
	   e.g. 'action' replaces models/weapons/v_blast with something
	   looking completely differently.
	   Using the material definition from baseq2 makes things look wrong.
	   So try to detect if the game is overriding an image from baseq2
	   with a different image and, if that is the case, ignore the material
	   definition (if it's from baseq2 - to allow for a game-specific material
	   definition).
	   There's also the wrinkle that some games ship with a copy of an image
	   that is identical in baseq2 (see game_image_identical_to_base()),
	   in that case, _do_ use the material definition. */
	if (matdef
		&& (matdef->image_flags & IF_SRC_MASK) == IF_SRC_BASE
		&& is_game_custom()
		&& FS_FileExistsEx(texture_name, FS_PATH_GAME) != 0
		&& !game_image_identical_to_base(texture_name))
	{
		matdef = NULL;
		/* Forcing image to load from game prevents a normal or emissive map in baseq2
		 * from being picked up. */
		flags = (flags & ~IF_SRC_MASK) | IF_SRC_GAME;
	}
	if (matdef)
	{
		memcpy(mat, matdef, sizeof(pbr_material_t));
		uint32_t index = (uint32_t)(mat - r_materials);
		mat->flags = (mat->flags & ~MATERIAL_INDEX_MASK) | index;
		mat->next_frame = index;

		// A definition that does not name a base texture is a TUNING-ONLY
		// entry: it exists to say specular_factor / emissive_factor / kind /
		// ... about a texture whose images should keep coming from where they
		// came from before anyone wrote the entry. Without this, adding three
		// lines to a .mat to brighten one rerelease md5 skin threw away its
		// base, normal, _rough, _metallic and _glow maps and rendered the model
		// white - a material definition was never neutral, and this is what
		// made it not neutral.
		//
		// Naming a texture_base still means "this entry describes every
		// texture", i.e. exactly today's behaviour, so nothing that already
		// ships changes.
		if (!MAT_SPECIFIED(mat, MAT_TEXTURE_BASE))
		{
			// an instance takes the slots it leaves unsaid from its base, below
			if (!instance_base)
			{
				const q3shader_t* q3 = q3_shader_for(texture_name_no_ext, type);
				// a Quake III shader names its own colour image; the shader's
				// name is not a file
				autodetect_material_textures(mat, (q3 && q3_base_stage(q3)) ? NULL : texture_name,
				                             texture_name_no_ext, type, flags);
				if (q3)
					apply_q3_shader(mat, q3, type, flags);
			}
		}
		else
		{
			// a player skin's own maps beat its model's shared ones - but not a
			// slot the entry cleared on purpose
			char skin_map[MAX_IMAGE_PATH];
			struct { char* filename; const char* suffix; int attr; } slots[] = {
				{ mat->filename_normals,   "_n",        MAT_TEXTURE_NORMALS },
				{ mat->filename_roughness, "_rough",    MAT_TEXTURE_ROUGHNESS },
				{ mat->filename_metallic,  "_metallic", MAT_TEXTURE_METALLIC },
				{ mat->filename_height,    "_height",   MAT_TEXTURE_HEIGHT },
				{ mat->filename_emissive,  "_light",    MAT_TEXTURE_EMISSIVE },
			};

			for (int i = 0; i < q_countof(slots); i++)
			{
				if (MAT_SPECIFIED(mat, slots[i].attr) && !slots[i].filename[0])
					continue;
				const char* found = player_skin_map(mat->name, mat->filename_base, slots[i].filename,
				                                    slots[i].suffix, skin_map, sizeof(skin_map));
				if (found != slots[i].filename)
					Q_strlcpy(slots[i].filename, found, MAX_IMAGE_PATH);
			}
		}

		if (mat->filename_base[0] && !mat->image_base) {
			load_material_image(&mat->image_base, mat->filename_base, mat, type, flags | IF_SRGB);
			if (mat->image_base == R_NOTEXTURE) {
				/* Only worth a warning if the fallback is missing too. A .mat
				   entry naming a texture that has not been made yet - a stub
				   pointing into materialDev/ - renders with the texture it would
				   have replaced, which is the intended result, and a release
				   carrying thousands of those printed one warning each.
				   developer 1 still lists every one for whoever is authoring.
				   A scripted Quake III shader's name is no file: the texture it
				   would have replaced is the shader's colour stage, which
				   apply_q3_shader below puts in the slot. */
				const q3shader_t* q3 = instance_base ? NULL : q3_shader_for(texture_name_no_ext, type);
				if (q3 && q3_base_stage(q3)) {
					Com_DPrintf("Texture '%s' specified in material '%s' could not be found. Using the shader's own.\n", mat->filename_base, mat_name_no_ext);
					mat->image_base = NULL;
				} else {
					mat->image_base = IMG_Find(texture_name, type, flags | IF_SRGB);
					mat->original_width = mat->image_base->width;
					mat->original_height = mat->image_base->height;
					if (mat->image_base == R_NOTEXTURE) {
						Com_WPrintf("Texture '%s' specified in material '%s' could not be found, and neither could '%s'.\n", mat->filename_base, mat_name_no_ext, texture_name);
						mat->image_base = NULL;
					} else {
						Com_DPrintf("Texture '%s' specified in material '%s' could not be found. Using the low-res texture.\n", mat->filename_base, mat_name_no_ext);
					}
				}
			}
			else
			{
				IMG_GetDimensions(texture_name, &mat->original_width, &mat->original_height);
			}
		}

		if (mat->filename_normals[0] && !mat->image_normals) {
			load_material_image(&mat->image_normals, mat->filename_normals, mat, type, flags);
			if (mat->image_normals == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_normals, mat_name_no_ext);
				mat->image_normals = NULL;
			}
		}
		
		if (mat->filename_roughness[0] && !mat->image_roughness) {
			load_material_image(&mat->image_roughness, mat->filename_roughness, mat, type, flags);
			if (mat->image_roughness == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_roughness, mat_name_no_ext);
				mat->image_roughness = NULL;
			}
		}

		if (mat->filename_metallic[0] && !mat->image_metallic) {
			load_material_image(&mat->image_metallic, mat->filename_metallic, mat, type, flags);
			if (mat->image_metallic == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_metallic, mat_name_no_ext);
				mat->image_metallic = NULL;
			}
		}

		if (mat->filename_height[0] && !mat->image_height) {
			load_material_image(&mat->image_height, mat->filename_height, mat, type, flags);
			if (mat->image_height == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_height, mat_name_no_ext);
				mat->image_height = NULL;
			}
		}

		if (mat->filename_emissive[0] && !mat->image_emissive) {
			load_material_image(&mat->image_emissive, mat->filename_emissive, mat, type, flags | IF_SRGB);
			if (mat->image_emissive == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_emissive, mat_name_no_ext);
				mat->image_emissive = NULL;
			}
		}
		
		if (mat->filename_mask[0] && !mat->image_mask) {
			mat->image_mask = IMG_Find(mat->filename_mask, type, flags | IF_EXACT | (mat->image_flags & IF_SRC_MASK));
			if (mat->image_mask == R_NOTEXTURE) {
				Com_DPrintf("Texture '%s' specified in material '%s' could not be found.\n", mat->filename_mask, mat_name_no_ext);
				mat->image_mask = NULL;
			}
		}

		/* A scripted Quake III shader whose entry names its own base image:
		   the shader still says what kind of surface it is, what glows and how
		   it moves - apply_q3_shader fills only what the entry left unsaid, and
		   the program draws the entry's image for the colour stage. */
		if (MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) && !instance_base)
		{
			const q3shader_t* q3 = q3_shader_for(texture_name_no_ext, type);
			if (q3)
				apply_q3_shader(mat, q3, type, flags);
		}
	}
	else
	{
		MAT_Reset(mat);
		Q_strlcpy(mat->name, mat_name_no_ext, sizeof(mat->name));

		if (!instance_base)
		{
			const q3shader_t* q3 = q3_shader_for(texture_name_no_ext, type);
			autodetect_material_textures(mat, (q3 && q3_base_stage(q3)) ? NULL : texture_name,
			                             texture_name_no_ext, type, flags);
			if (q3)
				apply_q3_shader(mat, q3, type, flags);
		}
	}

	if (instance_base)
		material_inherit_instance(mat, instance_base);

	if(mat->synth_emissive && !mat->image_emissive)
		MAT_SynthesizeEmissive(mat);

	if (mat->image_normals)
		mat->image_normals->flags |= IF_NORMAL_MAP;

	if (mat->image_height)
		mat->image_height->flags |= IF_HEIGHT_MAP;

	if (mat->image_emissive && !mat->image_emissive->processing_complete)
		vkpt_extract_emissive_texture_info(mat->image_emissive);

	mat->image_type = type;
	mat->image_flags |= flags;

	MAT_SetIndex(mat);
	MAT_UpdateRegistration(mat);

	List_Append(&r_materialsHash[hash], &mat->entry);

	return mat;
}

void MAT_InstanceName(const char* base_name, uint32_t instance_hash, char* out, size_t out_size)
{
	/* Lower-case hex, because every material name is lower-cased on the way in
	   and a name that does not survive that round trip would never be found. */
	Q_snprintf(out, out_size, "%s#%08x", base_name, instance_hash);
}

bool MAT_KindSpecified(const pbr_material_t* mat)
{
	return mat && MAT_SPECIFIED(mat, MAT_KIND);
}

/* ROUGHNESS_OVERRIDE IS THE ROUGHNESS where nothing else says how rough the
   surface is (Matt, 2026-11-16: "lets have roughness override set the
   roughness of the whole texture if a roughness map is not supplied"). Where
   something does - a texture_roughness, or the roughness the stock Quake II
   RTX textures pack into the base image's alpha, which get_material reads
   when there is a normal or metallic map - it stays the floor it always was,
   so the stock entries and every one tuned against them look as they did.
   With neither, the shader pinned roughness at 1 and max(1, override) threw
   the override away: q3dm0's main_q3abanner at 0.0005 never shone.
   Not the gloss apply_q3_shader gives a Quake III reflection map by default,
   which shows only where the map did (path_tracer_rgen.h) - unless the entry
   or 'mat roughness_override' states it.

   AN ENTRY THAT CLEARED ITS ROUGHNESS MAP and states a roughness_override
   means that number, normal or metallic map or not. Matt's q3dm12 instance of
   textures/base_wall/chrome_metal, 2026-11-17: texture_roughness cleared,
   roughness_override 0.005, and still fully rough - its metallic map (the
   chrome's M1 stand-in) sent get_material to the base image's alpha for the
   roughness, and the base is the opaque solid clone, so max(1, 0.005) = 1. */
bool MAT_RoughnessIsWhole(const pbr_material_t* mat)
{
	if (!mat || mat->roughness_override < 0.f)
		return false;
	if (mat->image_roughness)
		return false;
	if (MAT_CLEARED(mat, MAT_TEXTURE_ROUGHNESS, filename_roughness) && MAT_SPECIFIED(mat, MAT_ROUGHNESS_OVERRIDE))
		return true;
	if (mat->image_normals || mat->image_metallic)
		return false;
	return MAT_SPECIFIED(mat, MAT_ROUGHNESS_OVERRIDE) || !mat->q3_program;
}

pbr_material_t* MAT_FindInstance(const pbr_material_t* base, uint32_t instance_hash)
{
	if (!base || !instance_hash)
		return NULL;

	char instance_name[MAX_QPATH];
	MAT_InstanceName(base->name, instance_hash, instance_name, sizeof(instance_name));

	/* AN INSTANCE EXISTS ONLY BECAUSE A .mat SECTION DECLARES IT.

	   Without this test MAT_Find would cheerfully auto-generate a material for
	   every connected group in the map. That is not a small waste: the shader
	   carries the material index in 12 bits (MATERIAL_INDEX_MASK), so there are
	   4096 of them for the whole game, and a single level has far more surface
	   groups than that. Declared instances are rare by construction, which is
	   what keeps this affordable. */
	bool declared = find_material_sorted(instance_name, r_map_materials, num_map_materials) != NULL
		|| find_material_sorted(instance_name, r_global_materials, num_global_materials) != NULL;

	if (!declared)
		return NULL;

	/* REGISTRATION FLAGS, NOT THE BASE'S ACCUMULATED ONES.

	   base->image_flags is not what MAT_Find is normally handed. It is the
	   registration flags OR-ed with the IF_SRC_* bits recording which game
	   directory the base's definition came from, accumulated over the base's
	   own load. Passing that back in sets a source bit on every IMG_Find the
	   instance does, on top of the one load_material_image already derives from
	   the instance's OWN definition file - so an instance declared in the
	   rerelease would go looking for its textures in baseq2 as well.

	   Only IF_TURBULENT survives, because that is a property of the SURFACE
	   (SURF_WARP) that the instance shares with its base, and it is what
	   bsp_mesh_register_textures passes for the same texture. */
	imageflags_t instance_flags = base->image_flags & IF_TURBULENT;

	pbr_material_t* instance = MAT_Find(instance_name, base->image_type ? base->image_type : IT_WALL, instance_flags);

	/* The shader carries a material index in MATERIAL_INDEX_MASK bits, which is
	   fewer than r_materials has slots. A material allocated past that point
	   would have its index truncated and the surface would silently render as
	   some unrelated material - so fall back to the base, which is merely the
	   old behaviour, and say why. */
	if (instance && (uint32_t)(instance - r_materials) > MATERIAL_INDEX_MASK)
	{
		Com_WPrintf("Material instance '%s' does not fit in the renderer's material index "
		            "(%d slots); using '%s' instead.\n",
		            instance_name, MATERIAL_INDEX_MASK + 1, base->name);
		return NULL;
	}

	return instance;
}

void MAT_UpdateRegistration(pbr_material_t * mat)
{
	if (!mat)
		return;

	mat->registration_sequence = registration_sequence;
	if (mat->image_base) mat->image_base->registration_sequence = registration_sequence;
	if (mat->image_normals) mat->image_normals->registration_sequence = registration_sequence;
	if (mat->image_emissive) mat->image_emissive->registration_sequence = registration_sequence;
	if (mat->image_roughness) mat->image_roughness->registration_sequence = registration_sequence;
	if (mat->image_metallic) mat->image_metallic->registration_sequence = registration_sequence;
	if (mat->image_height) mat->image_height->registration_sequence = registration_sequence;
	if (mat->image_mask) mat->image_mask->registration_sequence = registration_sequence;
	vkpt_q3_touch_program(mat);
}

//
int MAT_FreeUnused()
{
	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; ++i)
	{
		pbr_material_t * mat = r_materials + i;

		if (mat->registration_sequence == registration_sequence)
			continue;

		if (!mat->registration_sequence)
			continue;

		if (mat->image_flags & IF_PERMANENT)
			continue;

		// delete the material from the hash table
		List_Remove(&mat->entry);

		MAT_Reset(mat);
	}

	return Q_ERR_SUCCESS;
}

int MAT_IndexOf(const pbr_material_t* mat)
{
	if (!mat || mat < r_materials || mat >= r_materials + MAX_PBR_MATERIALS)
		return -1;
	return (int)(mat - r_materials);
}

pbr_material_t* MAT_ForIndex(int index)
{
	if (index < 0 || index >= MAX_PBR_MATERIALS)
		return NULL;

	pbr_material_t* mat = r_materials + index;
	if (mat->registration_sequence == 0)
		return NULL;

	return mat;
}

pbr_material_t* MAT_ForSkin(image_t* image_base)
{
	// find the material
	pbr_material_t* mat = MAT_Find(image_base->name, IT_SKIN, IF_NONE);
	assert(mat);

	// if it's already using this skin, do nothing
	if (mat->image_base == image_base)
		return mat;

	/* A .MAT ENTRY'S OWN texture_base WINS OVER THE SKIN THE CLIENT REGISTERED.
	   Player bodies are the one model drawn through here: their skin is picked
	   per client (entity->skin), not from the model's skin list. Swapping in
	   that image unconditionally threw away the base of an entry such as
	   players/male/grunt pointing at a painted override, so player skins
	   ignored texture_base while every other model honoured it. A tuning-only
	   entry (no texture_base) still takes the registered skin below. */
	if (MAT_SPECIFIED(mat, MAT_TEXTURE_BASE) && mat->image_base)
		return mat;

	mat->image_base = image_base;

	// update registration sequence of material and its textures
	int rseq = image_base->registration_sequence;

	if (mat->image_normals)
		mat->image_normals->registration_sequence = rseq;

	if (mat->image_emissive)
		mat->image_emissive->registration_sequence = rseq;

	mat->registration_sequence = rseq;

	return mat;
}

// Where a .mat file a definition came from actually lives. The recorded name is
// game-relative, and in the rerelease that names two files - rerelease/'s
// materials/baseq2.mat shadows baseq2/'s - so say which folder it was read from,
// or that it came out of a pack. Found as the loader found it (FS_RealPath): the
// old guess called every file outside the game directory baseq2/'s, which
// quake3.mat, in baseq3/, is not.
static void material_source_location(const char* file_name, char* out, size_t size)
{
	char path[MAX_OSPATH];
	int where = material_file_real_path(file_name, path, sizeof(path));

	if (where == FS_REALPATH_DISK)
		Q_strlcpy(out, material_short_path(path), size);
	else if (where == FS_REALPATH_PACK)
		Q_concat(out, size, file_name, " (inside ", material_short_path(path), ")");
	else
		Q_concat(out, size, file_name, " (no longer on disk)");
}

//
// prints material properties on the console
//

void MAT_Print(pbr_material_t const * mat)
{
	Com_Printf("%s:\n", mat->name);

	/* A filename with no image behind it renders exactly as if the slot were
	   empty, and a plain listing cannot tell the two apart - which is the very
	   case someone is trying to diagnose when they run this. "(NOT LOADED)"
	   means the path was stated and the file was not found. */
#define PRINT_TEXTURE(key, file, image) \
	if (mat->file[0]) \
		Com_Printf("    " key " %s%s\n", mat->file, (mat->image) ? "" : "   (NOT LOADED)"); \
	else \
		Com_Printf("    " key "\n")

	PRINT_TEXTURE("texture_base", filename_base, image_base);
	PRINT_TEXTURE("texture_normals", filename_normals, image_normals);
	PRINT_TEXTURE("texture_emissive", filename_emissive, image_emissive);
	PRINT_TEXTURE("texture_mask", filename_mask, image_mask);
	PRINT_TEXTURE("texture_roughness", filename_roughness, image_roughness);
	PRINT_TEXTURE("texture_metallic", filename_metallic, image_metallic);
	PRINT_TEXTURE("texture_height", filename_height, image_height);
#undef PRINT_TEXTURE
	Com_Printf("    bump_scale %f\n", mat->bump_scale);
	Com_Printf("    displace_in %f\n", mat->displace_in);
	Com_Printf("    displace_out %f\n", mat->displace_out);
	Com_Printf("    displace_clamp %d\n", mat->displace_clamp ? 1 : 0);
	if (MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS) || mat->alpha_glass)
		Com_Printf("    alpha_glass %d%s\n", mat->alpha_glass ? 1 : 0,
			MAT_SPECIFIED(mat, MAT_ALPHA_GLASS) ? "" : " (auto)");
	if (MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS) || mat->glass_caustic > 0.f)
		Com_Printf("    caustic %g%s\n", mat->glass_caustic,
			mat->glass_caustic > 0.f && !MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS) ? " (inactive: not GLASS)" : "");
	if (MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS) || mat->caustic_ripple > 0.f)
		Com_Printf("    caustic_ripple %g%s\n", mat->caustic_ripple,
			mat->caustic_ripple > 0.f && !MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS) ? " (inactive: not GLASS)"
			: mat->caustic_ripple > 0.f ? "   (the light through it thrown in moving water ripples)" : "");
	Com_Printf("    roughness_override %f%s\n", mat->roughness_override,
		mat->roughness_override < 0.f ? "" : MAT_RoughnessIsWhole(mat)
			? "   (the roughness all over: no roughness map)" : "   (a floor under its maps)");
	Com_Printf("    metalness_factor %f\n", mat->metalness_factor);
	Com_Printf("    emissive_factor %f\n", mat->emissive_factor);
	Com_Printf("    light_factor %f   (the light it casts only)\n", mat->light_factor);
	Com_Printf("    specular_factor %f\n", mat->specular_factor);
	Com_Printf("    base_factor %f\n", mat->base_factor);
	if (mat->flags & MATERIAL_FLAG_CURVED_WATER)
		Com_Printf("    curved_water 1\n");
	if (mat->flags & MATERIAL_FLAG_DOUBLE_SIDED)
		Com_Printf("    double_sided 1\n");
	const char * kind = getMaterialKindName(mat->flags);
	// "kind GLASSCAUSTIC" / "kind ALPHA_GLASS" have no kind of their own (the
	// kind field is full): GLASS plus a switch, which is what this line shows
	const bool glass = MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS);
	const bool caustic = glass && mat->glass_caustic > 0.f;
	const bool alpha = glass && mat->alpha_glass;
	Com_Printf("    kind %s%s%s%s%s%s\n", kind ? kind : "",
		(caustic || alpha) ? "   (" : "",
		caustic ? "GLASSCAUSTIC = GLASS + caustic" : "",
		(caustic && alpha) ? "; " : "",
		alpha ? "ALPHA_GLASS = GLASS + alpha_glass" : "",
		(caustic || alpha) ? ")" : "");
	Com_Printf("    is_light %d\n", (mat->flags & MATERIAL_FLAG_LIGHT) != 0);
	Com_Printf("    light_styles %d\n", mat->light_styles ? 1 : 0);
	Com_Printf("    bsp_radiance %d\n", mat->bsp_radiance ? 1 : 0);
	Com_Printf("    default_radiance %f\n", mat->default_radiance);
	Com_Printf("    synth_emissive %d\n", mat->synth_emissive ? 1 : 0);
	Com_Printf("    emissive_threshold %d\n", mat->emissive_threshold);
	if (mat->volumetric_scale >= 0.f)
		Com_Printf("    volumetric_scale %f\n", mat->volumetric_scale);
	else
		Com_Printf("    volumetric_scale (class default)\n");
	if (mat->dlss_guide_field >= 0)
		Com_Printf("    dlss_guide_field %d\n", mat->dlss_guide_field);
	else
		Com_Printf("    dlss_guide_field (pt_dlss_guide_field)\n");
	if (mat->albedo_strength > 0.f)
		Com_Printf("    albedo_strength %f\n", mat->albedo_strength);
	else
		Com_Printf("    albedo_strength (pt_bounce_albedo_power)\n");

	// a Quake III shader drawn stage by stage: what each stage really draws
	if (mat->q3_program)
		vkpt_q3_print_program(mat);

	// Last, so it sits right above the prompt: the file and line to edit.
	char where[MAX_OSPATH];
	if (mat->source_matfile[0])
	{
		material_source_location(mat->source_matfile, where, sizeof(where));
		Com_Printf("    defined in: %s line %u\n", where, mat->source_line);
	}
	else
		Com_Printf("    defined in: no .mat entry - generated from the texture and its sidecars\n");

	// what 'mat save' would write back
	if (material_unsaved(mat))
	{
		char changed[512];
		material_console_field_names(mat, changed, sizeof(changed));
		char stage_key[64], stage_value[MAX_IMAGE_PATH];
		for (int k = 0; vkpt_q3_stage_unsaved_get(mat->name, k, stage_key, sizeof(stage_key),
			stage_value, sizeof(stage_value)); k++)
		{
			Q_strlcat(changed, " ", sizeof(changed));
			Q_strlcat(changed, stage_key, sizeof(changed));
		}
		Com_Printf("    changed, not saved:%s ('mat save')\n", changed);
	}

	// An md5 skin with no entry of its own takes the classic skin's tuning.
	if (mat->inherit_source[0])
	{
		const pbr_material_t* src = find_material_sorted(mat->inherit_source,
			r_global_materials, num_global_materials);
		if (src && src->source_matfile[0])
		{
			material_source_location(src->source_matfile, where, sizeof(where));
			Com_Printf("    inherits tuning from: %s, %s line %u\n", mat->inherit_source, where, src->source_line);
		}
	}
}

// Swap one of a live material's textures for the one the definition now names.
// A name that has not changed keeps the image that is already loaded, so a
// reload that only moves a number does no image work here - an edit to the
// file itself is picked up by material_refresh_images. Unless that name has no
// image: the name was recorded even when its file was missing, so a texture
// saved after the reload that first named it was never loaded at all.
static void material_reload_image(image_t** image, char* dest_name, const char* new_name,
                                  pbr_material_t* mat, imageflags_t extra)
{
	// no name: no image, whatever the slot held (an image under an empty name
	// would otherwise survive the name check below)
	if (!new_name[0]) {
		dest_name[0] = 0;
		*image = NULL;
		return;
	}

	if (!strcmp(dest_name, new_name) && *image)
		return;

	Q_strlcpy(dest_name, new_name, MAX_IMAGE_PATH);

	load_material_image(image, new_name, mat, mat->image_type, mat->image_flags | extra);

	/* IMG_Find matches an image that is already loaded by its name WITHOUT the
	   extension, IF_EXACT or not. So switching a map from foo_n.tga to
	   foo_n.png handed back the foo_n.tga still in memory, and the .png only
	   showed after a restart, where it is the first one asked for. Take the file
	   that was asked for as an image of its own (IMG_FindFile), or, failing that,
	   treat it as missing, as a cold load would. This used to read the new file
	   INTO the old image, which changed it for every material sharing it: the
	   other xceil1_39 lights' .jpg glow turned into the one entry's .png. */
	if (*image != R_NOTEXTURE && (*image)->filepath[0] && FS_pathcmp((*image)->filepath, new_name))
	{
		*image = IMG_FindFile(new_name, mat->image_type, (mat->image_flags | extra) & ~IF_SRC_MASK);
		if (*image != R_NOTEXTURE && (*image)->filepath[0] && FS_pathcmp((*image)->filepath, new_name))
			*image = R_NOTEXTURE;
	}

	if (*image == R_NOTEXTURE) {
		Com_WPrintf("Texture '%s' specified in material '%s' could not be found.\n", new_name, mat->name);
		*image = NULL;
	}
}

/*
=================
material_reapply_definition

Re-applies a freshly parsed .mat definition onto a LIVE material, in place.

The live material object must NOT be destroyed and re-created, which is what
this command used to do. Two things point straight into r_materials and survive
a reload:

  - maliasmesh_t::materials holds pbr_material_t* written when the model was
    parsed. MOD_Reference_RTX writes registration_sequence back through those
    pointers for every model that is already cached, and CL_PrepRefresh takes
    the cached path for all of them.
  - the material index is baked into geometry that has already been uploaded.

Resetting a live material therefore left every cached model pointing at a
zeroed slot - which is the visible corruption - and re-registering through that
pointer put registration_sequence back onto a material whose list node had just
been cleared by the memset in MAT_Reset. The next reload then found a material
that looked live but was not linked into r_materialsHash, called List_Remove on
a node whose next/prev were NULL, and crashed. Every time, on the second
reload, which is exactly what was reported.

So: identity is kept - the address, the hash linkage, the material index, and
any image whose name did not change. Only tuning and texture names are taken
from the definition.
=================
*/
static void material_reapply_definition(pbr_material_t* mat, const pbr_material_t* matdef)
{
	mat->bump_scale         = matdef->bump_scale;
	mat->displace_in        = matdef->displace_in;
	mat->displace_out       = matdef->displace_out;
	mat->displace_clamp     = matdef->displace_clamp;
	mat->alpha_glass        = matdef->alpha_glass;
	mat->glass_caustic      = matdef->glass_caustic;
	mat->caustic_ripple     = matdef->caustic_ripple;
	mat->roughness_override = matdef->roughness_override;
	mat->metalness_factor   = matdef->metalness_factor;
	mat->emissive_factor    = matdef->emissive_factor;
	mat->light_factor       = matdef->light_factor;
	mat->specular_factor    = matdef->specular_factor;
	mat->base_factor        = matdef->base_factor;
	mat->light_styles       = matdef->light_styles;
	mat->bsp_radiance       = matdef->bsp_radiance;
	mat->default_radiance   = matdef->default_radiance;
	mat->num_frames         = matdef->num_frames;
	mat->synth_emissive     = matdef->synth_emissive;
	mat->volumetric_scale   = matdef->volumetric_scale;
	mat->dlss_guide_field   = matdef->dlss_guide_field;
	mat->albedo_strength    = matdef->albedo_strength;
	// what the definition states ITSELF, which is what the auto-detection and
	// the inheritance below are gated on
	mat->specified_fields   = matdef->specified_fields;
	// a reload takes the file's values, so console changes not saved are gone
	mat->console_fields     = 0;

	// keep 'mat which' truthful about where the values came from
	Q_strlcpy(mat->source_matfile, matdef->source_matfile, sizeof(mat->source_matfile));
	mat->source_line = matdef->source_line;

	// everything except the index, which addresses geometry already uploaded.
	// The per-surface runtime bits (WARP, FLOWING, ...) are re-applied by
	// bsp_mesh_register_textures during the CL_PrepRefresh below.
	mat->flags = (matdef->flags & ~MATERIAL_INDEX_MASK) | (mat->flags & MATERIAL_INDEX_MASK);

	// an instance re-inherits from its base after its own definition is applied;
	// material_command_reload has already brought the base up to date
	const pbr_material_t* instance_base = find_instance_base(mat->name);

	// A synthesized emissive is cached under a derived '*E' name keyed on the
	// old threshold, so a changed threshold has to retire it - the same thing
	// 'mat emissive_threshold <n>' does. Not one an instance borrowed from its
	// base, though: retiring that would retire the base's.
	if (mat->emissive_threshold != matdef->emissive_threshold
		&& mat->image_emissive && strstr(mat->image_emissive->name, "*E")
		&& !(instance_base && mat->image_emissive == instance_base->image_emissive))
	{
		mat->image_emissive->name[0] = 0;
		mat->image_emissive = NULL;
	}
	mat->emissive_threshold = matdef->emissive_threshold;

	// Textures follow the same dichotomy MAT_Find draws. An entry that names a
	// texture_base describes EVERY texture, so a name it no longer carries has
	// to clear that slot. A TUNING-ONLY entry names none of them: its slots were
	// auto-detected from the sidecars beside the texture, and writing the
	// definition's empty strings over them threw away the base, normal, _rough,
	// _metallic and _glow maps - i.e. 'mat reload' rendered the model white for
	// exactly the reason MAT_Find used to, one layer up. That is why adding
	// 'specular_factor 3' to an md5 skin still looked broken after MAT_Find was
	// fixed: the model only goes white once the .mat has been reloaded, which is
	// how the value is tuned in the first place.
	const bool describes_textures = MAT_SPECIFIED(matdef, MAT_TEXTURE_BASE);

#define RELOAD_TEXTURE(attr, image, filename, extra) \
	if (describes_textures || MAT_SPECIFIED(matdef, attr)) \
		material_reload_image(&mat->image, mat->filename, matdef->filename, mat, extra)

	// a player skin's own map beats its model's shared one, as in MAT_Find -
	// unless the entry cleared the slot (MAT_CLEARED)
	char skin_map[MAX_IMAGE_PATH];
#define RELOAD_SKIN_TEXTURE(attr, image, filename, suffix, extra) \
	if (describes_textures || MAT_SPECIFIED(matdef, attr)) \
		material_reload_image(&mat->image, mat->filename, describes_textures && !MAT_CLEARED(matdef, attr, filename) \
			? player_skin_map(mat->name, matdef->filename_base, matdef->filename, suffix, skin_map, sizeof(skin_map)) \
			: matdef->filename, mat, extra)

	RELOAD_TEXTURE(MAT_TEXTURE_BASE,           image_base,      filename_base,                   IF_SRGB);
	RELOAD_SKIN_TEXTURE(MAT_TEXTURE_NORMALS,   image_normals,   filename_normals,   "_n",        IF_NONE);
	RELOAD_SKIN_TEXTURE(MAT_TEXTURE_ROUGHNESS, image_roughness, filename_roughness, "_rough",    IF_NONE);
	RELOAD_SKIN_TEXTURE(MAT_TEXTURE_METALLIC,  image_metallic,  filename_metallic,  "_metallic", IF_NONE);
	RELOAD_SKIN_TEXTURE(MAT_TEXTURE_HEIGHT,    image_height,    filename_height,    "_height",   IF_NONE);
	RELOAD_SKIN_TEXTURE(MAT_TEXTURE_EMISSIVE,  image_emissive,  filename_emissive,  "_light",    IF_SRGB);
	RELOAD_TEXTURE(MAT_TEXTURE_MASK,           image_mask,      filename_mask,                   IF_NONE);

#undef RELOAD_SKIN_TEXTURE
#undef RELOAD_TEXTURE

	if (instance_base)
		material_inherit_instance(mat, instance_base);
	else if (describes_textures)
	{
		// as in MAT_Find: a Quake III shader still decides what the entry does not
		const q3shader_t* q3 = q3_shader_for(mat->name, mat->image_type);
		if (q3)
			apply_q3_shader(mat, q3, mat->image_type, mat->image_flags & ~IF_SRC_MASK);
	}
	else
	{
		// re-detect the slots the definition left unsaid, as a cold load would
		autodetect_material_textures(mat, NULL, mat->name, mat->image_type,
		                             mat->image_flags & ~IF_SRC_MASK);

		// and what a Quake III shader says about the slots still empty
		const q3shader_t* q3 = q3_shader_for(mat->name, mat->image_type);
		if (q3)
			apply_q3_shader(mat, q3, mat->image_type, mat->image_flags & ~IF_SRC_MASK);

		// and re-apply the classic material's tuning, which no .mat file records -
		// without this a reload drops an md5 skin from the inherited base_factor
		// of 1.5-2.5 back to the 1.0 default and the model visibly darkens
		if (mat->inherit_source[0])
			MAT_InheritScalars(mat, mat->inherit_source);
	}

	if (mat->synth_emissive && !mat->image_emissive)
	{
		MAT_SynthesizeEmissive(mat);

		// MAT_SynthesizeEmissive does not always produce one
		if (mat->image_emissive)
			IMG_Load(mat->image_emissive, mat->image_emissive->pix_data);
	}

	if (mat->image_normals)
		mat->image_normals->flags |= IF_NORMAL_MAP;

	if (mat->image_height)
		mat->image_height->flags |= IF_HEIGHT_MAP;

	if (mat->image_emissive && !mat->image_emissive->processing_complete)
		vkpt_extract_emissive_texture_info(mat->image_emissive);

	MAT_UpdateRegistration(mat);
}

// Re-read every texture of a live material whose file was saved since it was
// loaded - the base, normal, roughness ... map edited in a paint program while
// the game runs. The images keep their slots, so nothing that points at them
// has to change. An image shared between materials is read once: after that
// its time stamp is current.
static void material_refresh_images(pbr_material_t* mat)
{
	image_t* images[] = {
		mat->image_base, mat->image_normals, mat->image_roughness, mat->image_metallic,
		mat->image_height, mat->image_emissive, mat->image_mask,
	};

	for (int i = 0; i < q_countof(images); i++)
		if (images[i])
			IMG_ReloadIfModified(images[i]);

	// a light's color and extent are measured from the pixels
	if (mat->image_emissive && !mat->image_emissive->processing_complete)
		vkpt_extract_emissive_texture_info(mat->image_emissive);
}

/* WHAT 'reload_textures <name>' SAYS ABOUT THE MATERIALS. Matt, 2026-11-18,
   painting the Quake III Doom player's doom_rough.tga with the game open:
   "I make changes do reload_textures and I always get zero textures new and I
   have to restart the game to see my changes". The logs show the file saved
   and the next 'mat reload' re-reading nothing, which the code as read cannot
   do for a live material's own map - so the report names, for each material
   with the name in it, the image each map really is (its slot, its file, and
   whether that slot is still a live image), and for each texture matched, the
   materials that draw it. One run of it tells which of the three it is: not
   the file that was edited, not a material that is drawn, or not seen as
   changed. */
static const char* const map_slot_names[] = {
	"base", "normals", "roughness", "metallic", "height", "emissive", "mask",
};

static void material_map_images(const pbr_material_t* mat, image_t* out[7], const char* names[7])
{
	out[0] = mat->image_base;      names[0] = mat->filename_base;
	out[1] = mat->image_normals;   names[1] = mat->filename_normals;
	out[2] = mat->image_roughness; names[2] = mat->filename_roughness;
	out[3] = mat->image_metallic;  names[3] = mat->filename_metallic;
	out[4] = mat->image_height;    names[4] = mat->filename_height;
	out[5] = mat->image_emissive;  names[5] = mat->filename_emissive;
	out[6] = mat->image_mask;      names[6] = mat->filename_mask;
}

int MAT_ImageUsers(const image_t* image, char* out, size_t size)
{
	int count = 0;
	out[0] = 0;

	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		const pbr_material_t* mat = r_materials + i;
		if (!mat->registration_sequence)
			continue;

		image_t* images[7];
		const char* names[7];
		material_map_images(mat, images, names);
		for (int k = 0; k < 7; k++)
		{
			if (images[k] != image)
				continue;
			if (count++ < 4)
			{
				if (out[0])
					Q_strlcat(out, ", ", size);
				Q_strlcat(out, mat->name, size);
				Q_strlcat(out, " (", size);
				Q_strlcat(out, map_slot_names[k], size);
				Q_strlcat(out, ")", size);
			}
			break;
		}
	}
	if (count > 4)
		Q_strlcat(out, va(" and %d more", count - 4), size);
	return count;
}

void MAT_ReportImages(const char* filter)
{
	int shown = 0;

	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		const pbr_material_t* mat = r_materials + i;
		if (!mat->registration_sequence || !Q_stristr(mat->name, filter))
			continue;

		if (!shown++)
			Com_Printf("Materials with '%s' in their name - the image each map is:\n", filter);
		Com_Printf("  %s (material %u%s)\n", mat->name, i,
		           mat->registration_sequence == registration_sequence ? "" : ", NOT registered for this level");

		image_t* images[7];
		const char* names[7];
		material_map_images(mat, images, names);
		for (int k = 0; k < 7; k++)
		{
			const image_t* img = images[k];
			if (!img && !names[k][0])
				continue;
			if (!img)
			{
				Com_Printf("      %-9s %s (NOT LOADED)\n", map_slot_names[k], names[k]);
				continue;
			}
			const char* file = img->filepath[0] ? img->filepath : img->name;
			Com_Printf("      %-9s #%d %s%s%s\n", map_slot_names[k], (int)(img - r_images),
			           file[0] ? file : "(a freed slot)",
			           !img->registration_sequence ? " - FREED IMAGE SLOT, still drawn from" : "",
			           names[k][0] && file[0] && FS_pathcmp(file, names[k]) ? va(" - the entry names %s", names[k]) : "");
		}
		if (mat->q3_program)
			Com_Printf("      and a Quake III stage program: its stage images are listed above if their paths match\n");
	}
}

// what follows images re-read in place (IMG_ReloadModified)
void MAT_ImagesReloaded(void)
{
	// a light's color and extent are measured from the pixels
	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		const pbr_material_t* mat = r_materials + i;
		if (mat->registration_sequence && mat->image_emissive && !mat->image_emissive->processing_complete)
			vkpt_extract_emissive_texture_info(mat->image_emissive);
	}
	// and the stage glows' CPU copies (q3stages.c light_image)
	vkpt_q3_light_cache_clear();
}

// Re-read every .mat file from disk and push the new values onto the materials
// that are already live, so brightness and other values can be tuned in a text
// editor without restarting. The definition tables (r_global_materials /
// r_map_materials) are safe to repopulate wholesale; the live materials in
// r_materials are updated in place - see material_reapply_definition for why
// they cannot simply be dropped and rebuilt.
static void material_command_reload(void)
{
	load_global_materials();

	// re-read this map's overrides too, if it has any
	if (current_map_name[0])
	{
		char map_name_no_ext[MAX_QPATH];
		truncate_extension(current_map_name, map_name_no_ext, sizeof(map_name_no_ext));
		char file_name[MAX_QPATH];
		Q_snprintf(file_name, sizeof(file_name), "%s.mat", map_name_no_ext);

		memset(r_map_materials, 0, sizeof(r_map_materials));
		vkpt_q3_stage_override_clear(Q3SS_SCOPE_MAP);
		num_map_materials = load_material_file(file_name, r_map_materials, MAX_PBR_MATERIALS);
		if (num_map_materials > 0) {
			sort_and_deduplicate_materials(r_map_materials, &num_map_materials);
			Com_Printf("Loaded %d materials from %s\n", num_map_materials, file_name);
		}
	}

	// instances in a second pass: each one re-inherits from its base, which
	// therefore has to have taken its own new definition first
	int updated = 0, generated = 0;

	/* WHAT THE RELOAD THROWS AWAY, SAID OUT LOUD. Re-applying the definition
	   drops what was typed and not saved (material_reapply_definition clears
	   console_fields), silently - and the slot it drops can be left printing
	   the typed path as "(NOT LOADED)". Matt typed texture_metallic for the
	   Anarki gun (anarki_g_metal.png), ran 'mat reload' to see it, and it was
	   gone; 'mat save' then said "Nothing to save", and the next save wrote
	   only the roughness typed after (2026-11-18, rerelease/logs/console.log). */
	int dropped = 0;
	char dropped_list[1024] = "";
	for (int pass = 0; pass < 2; pass++)
	for (uint32_t i = 0; i < MAX_PBR_MATERIALS; i++)
	{
		pbr_material_t* mat = r_materials + i;

		if (!mat->registration_sequence)
			continue;

		if ((instance_separator(mat->name) != NULL) != (pass == 1))
			continue;

		// a wall takes its map's override in preference to the global table,
		// the same order MAT_Find uses
		const pbr_material_t* matdef = NULL;
		if (mat->image_type == IT_WALL)
			matdef = find_material_sorted(mat->name, r_map_materials, num_map_materials);
		if (!matdef)
			matdef = find_material_sorted(mat->name, r_global_materials, num_global_materials);

		if (!matdef) {
			// nothing in any .mat names this one - it was generated from the
			// texture and its sidecars, and there is nothing to re-apply.
			// Except what it inherits: an md5 skin with no entry of its own
			// still takes the classic skin's tuning, and that entry is one of
			// the ones just re-read - without this an edit to it (base_factor,
			// albedo_strength, ...) reached the md5 model only on a restart.
			if (mat->inherit_source[0])
				MAT_InheritScalars(mat, mat->inherit_source);
			material_refresh_images(mat);
			++generated;
			continue;
		}

		if (mat->console_fields)
		{
			if (dropped < 4)
			{
				char fields[512];
				material_console_field_names(mat, fields, sizeof(fields));
				Q_strlcat(dropped_list, "\n    ", sizeof(dropped_list));
				Q_strlcat(dropped_list, mat->name, sizeof(dropped_list));
				Q_strlcat(dropped_list, ":", sizeof(dropped_list));
				Q_strlcat(dropped_list, fields, sizeof(dropped_list));
			}
			++dropped;
		}

		material_reapply_definition(mat, matdef);
		material_refresh_images(mat);
		++updated;
	}

	// and the pictures the Quake III stages draw from, which are not the
	// material's own maps (q3stages.c)
	int stage_images = vkpt_q3_refresh_stage_images();

	// and every other live texture saved since it was read - what
	// 'reload_textures' does, so one command is enough (a model's skin
	// registered as an image of its own, a sprite, an image no map names)
	int other_images = IMG_ReloadModified(NULL, false, false);

	// Some material changes reclassify a mesh as transparent or masked, which
	// affects the static model BLAS, and emissive changes have to reach the
	// light lists - both are rebuilt from here.
	vkpt_vertex_buffer_invalidate_static_model_vbos(-1);
	CL_PrepRefresh();

	Com_Printf("Reloaded material definitions (%d materials updated, %d auto-generated with no .mat entry",
	           updated, generated);
	if (stage_images)
		Com_Printf(", %d stage image%s re-read", stage_images, stage_images == 1 ? "" : "s");
	if (other_images)
		Com_Printf(", %d other texture%s re-read", other_images, other_images == 1 ? "" : "s");
	Com_Printf(").\n");

	if (dropped)
	{
		Com_WPrintf("Dropped unsaved console changes to %d material%s - 'mat save' before 'mat reload' keeps them:%s\n",
		            dropped, dropped == 1 ? "" : "s", dropped_list);
		if (dropped > 4)
			Com_WPrintf("    ... and %d more\n", dropped - 4);
	}
}

// The commented-out lines in a fresh instance stub: what the instance inherits
// from (mat) right now - see material_create_instance.
static void write_instance_stub_values(qhandle_t file, const pbr_material_t* mat)
{
	const bool synthesized = mat->image_emissive && strstr(mat->image_emissive->name, "*E");

#define STUB_TEXTURE(key, filename) \
	if (mat->filename[0]) \
		FS_FPrintf(file, "#\t" key " %s\n", mat->filename)

	STUB_TEXTURE("texture_base", filename_base);
	STUB_TEXTURE("texture_normals", filename_normals);
	STUB_TEXTURE("texture_roughness", filename_roughness);
	STUB_TEXTURE("texture_metallic", filename_metallic);
	STUB_TEXTURE("texture_height", filename_height);
	if (!synthesized)
		STUB_TEXTURE("texture_emissive", filename_emissive);
	STUB_TEXTURE("texture_mask", filename_mask);

#undef STUB_TEXTURE

	FS_FPrintf(file, "#\tbump_scale %g\n", mat->bump_scale);
	FS_FPrintf(file, "#\tdisplace_in %g\n", mat->displace_in);
	FS_FPrintf(file, "#\tdisplace_out %g\n", mat->displace_out);
	if (mat->displace_clamp)
		FS_FPrintf(file, "#\tdisplace_clamp 1\n");
	if (mat->alpha_glass)
		FS_FPrintf(file, "#\talpha_glass 1\n");
	if (mat->glass_caustic > 0.f)
		FS_FPrintf(file, "#\tcaustic %g\n", mat->glass_caustic);
	if (mat->caustic_ripple > 0.f)
		FS_FPrintf(file, "#\tcaustic_ripple %g\n", mat->caustic_ripple);
	// negative means "no override" here, and is not something to write back
	if (mat->roughness_override >= 0.f)
		FS_FPrintf(file, "#\troughness_override %g\n", mat->roughness_override);
	FS_FPrintf(file, "#\tmetalness_factor %g\n", mat->metalness_factor);
	FS_FPrintf(file, "#\tspecular_factor %g\n", mat->specular_factor);
	FS_FPrintf(file, "#\tbase_factor %g\n", mat->base_factor);
	FS_FPrintf(file, "#\temissive_factor %g\n", mat->emissive_factor);
	FS_FPrintf(file, "#\tlight_factor %g\n", mat->light_factor);
	if (mat->synth_emissive || synthesized) {
		FS_FPrintf(file, "#\tsynth_emissive 1\n");
		FS_FPrintf(file, "#\temissive_threshold %d\n", mat->emissive_threshold);
	}

	const char* kind = getMaterialKindName(mat->flags);
	if (kind)
		FS_FPrintf(file, "#\tkind %s\n", kind);
	FS_FPrintf(file, "#\tis_light %d\n", (mat->flags & MATERIAL_FLAG_LIGHT) ? 1 : 0);
	if (mat->flags & MATERIAL_FLAG_CURVED_WATER)
		FS_FPrintf(file, "#\tcurved_water 1\n");
	if (mat->flags & MATERIAL_FLAG_DOUBLE_SIDED)
		FS_FPrintf(file, "#\tdouble_sided 1\n");
	FS_FPrintf(file, "#\tlight_styles %d\n", mat->light_styles ? 1 : 0);
	FS_FPrintf(file, "#\tbsp_radiance %d\n", mat->bsp_radiance ? 1 : 0);
	FS_FPrintf(file, "#\tdefault_radiance %g\n", mat->default_radiance);

	// these three default to "not stated, the cvar decides" - only a stated
	// value is worth listing
	if (mat->volumetric_scale >= 0.f)
		FS_FPrintf(file, "#\tvolumetric_scale %g\n", mat->volumetric_scale);
	if (mat->dlss_guide_field >= 0)
		FS_FPrintf(file, "#\tdlss_guide_field %d\n", mat->dlss_guide_field);
	if (mat->albedo_strength > 0.f)
		FS_FPrintf(file, "#\talbedo_strength %g\n", mat->albedo_strength);
}

/* `mat create_instance` - give the surface under the crosshair a material of its
   own, so it can be tuned without dragging every other use of its texture along
   with it.

   The identity comes from the geometry, not from the texture: bsp_mesh.c hashes
   the connected group of BSP faces the crosshair is on, and that hash names a
   .mat section of its own. See the MATERIAL INSTANCE IDENTITY block there for
   what "connected group" means and why the hash is stable enough to write into
   a file by hand.

   A fresh instance is an exact copy of the texture's material as it stands -
   see material_inherit_instance - so splitting a surface off changes nothing
   until a line under its section says otherwise. The stub lists what it
   inherits as commented-out lines, so the usual edit is to uncomment one and
   change the value. Those lines are only a snapshot: the inheritance itself is
   live, and an edit to the base still reaches every line left commented. */
static void material_create_instance(const pbr_material_t* mat)
{
	if (!vkpt_refdef.fd)
		return;

	uint32_t instance_hash = vkpt_refdef.fd->feedback.view_prim_instance_hash;

	if (!instance_hash)
	{
		Com_Printf("No static world surface under the crosshair.\n");
		Com_Printf("Instances are keyed on BSP geometry, so they only apply to the level "
		           "itself - not to models, brush entities such as doors and platforms, or the sky.\n");
		return;
	}

	if (instance_separator(mat->name))
	{
		Com_Printf("'%s' is already an instance", mat->name);
		if (mat->source_matfile[0])
			Com_Printf(", defined in %s line %d", mat->source_matfile, mat->source_line);
		Com_Printf(".\n");
		return;
	}

	char instance_name[MAX_QPATH];
	MAT_InstanceName(mat->name, instance_hash, instance_name, sizeof(instance_name));

	/* The hash is only meaningful inside one BSP, so the stub belongs in a file
	   named after the map. materials/*.mat is already scanned wholesale at
	   startup, so no loader change is needed to pick it up, and FS_MODE_APPEND
	   writes into the active game directory - rerelease/ or baseq2/ - which is
	   where the rest of that game's materials live. */
	char map_base[MAX_QPATH];
	Q_strlcpy(map_base, current_map_name, sizeof(map_base));
	{
		char* slash = strrchr(map_base, '/');
		if (slash)
			memmove(map_base, slash + 1, strlen(slash + 1) + 1);
		char* dot = strrchr(map_base, '.');
		if (dot)
			*dot = 0;
	}

	if (!map_base[0])
	{
		Com_Printf("No map is loaded.\n");
		return;
	}

	// a Quake III map's into baseq3/materials, beside its data (files.c)
	char file_name[MAX_QPATH];
	Q_snprintf(file_name, sizeof(file_name), "%smaterials/%s.mat",
		vkpt_refdef.bsp_mesh_world_loaded && vkpt_refdef.bsp_mesh_world.q3 ? "baseq3/" : "", map_base);

	qhandle_t file = 0;
	int err = FS_OpenFile(file_name, &file, FS_MODE_APPEND);

	if (err < 0 || !file)
	{
		Com_WPrintf("Cannot open '%s' for writing: %s\n", file_name, Q_ErrorString(err));
		return;
	}

	/* '#' opens a comment only as the first non-whitespace character on a line,
	   which is also why a name containing '#' parses - see load_material_file. */
	FS_FPrintf(file, "\n# instance of %s\n", mat->name);
	FS_FPrintf(file, "# %s, created at (%.0f %.0f %.0f)\n", map_base,
	           vkpt_refdef.fd->vieworg[0], vkpt_refdef.fd->vieworg[1], vkpt_refdef.fd->vieworg[2]);
	FS_FPrintf(file, "# Inherits everything from %s; the commented lines are what\n", mat->name);
	FS_FPrintf(file, "# it inherits now. Uncomment one and change it, then 'mat reload'.\n");
	FS_FPrintf(file, "%s:\n", instance_name);
	write_instance_stub_values(file, mat);

	FS_CloseFile(file);

	Com_Printf("Created %s\n", instance_name);
	Com_Printf("  stub written to %s\n", file_name);
	Com_Printf("  the surface looks as before; uncomment and edit a line under that "
	           "section to change it, then 'mat reload'.\n");

	/* Re-read the material files and rebuild the map so the new section takes
	   effect now. Without the rebuild the faces keep the material index they
	   were given when the BSP mesh was built, and nothing appears to happen. */
	material_command_reload();
}

/* 'mat print' puts the material's albedo in the top-right corner of the screen,
   over the console, so the surface the crosshair picked can be checked to be the
   one meant before tuning it. 'mat print normals' (emissive, mask, roughness,
   metallic, height) shows that map instead, 'mat print <name> [map]' a named
   material's. The map is drawn as stored, its alpha ignored - an albedo whose
   alpha is a cutout, or zero everywhere, would otherwise draw as holes. It stays
   up while the console is open and for MAT_PREVIEW_MS after it closes. */

#define MAT_PREVIEW_MS 8000

enum { PREVIEW_ALBEDO, PREVIEW_NORMALS, PREVIEW_EMISSIVE, PREVIEW_MASK,
	PREVIEW_ROUGHNESS, PREVIEW_METALLIC, PREVIEW_HEIGHT };

static const struct {
	const char *word;
	int map;
} mat_preview_words[] = {
	{ "albedo", PREVIEW_ALBEDO }, { "base", PREVIEW_ALBEDO }, { "diffuse", PREVIEW_ALBEDO },
	{ "normals", PREVIEW_NORMALS }, { "normal", PREVIEW_NORMALS },
	{ "emissive", PREVIEW_EMISSIVE }, { "mask", PREVIEW_MASK },
	{ "roughness", PREVIEW_ROUGHNESS }, { "metallic", PREVIEW_METALLIC },
	{ "metalness", PREVIEW_METALLIC }, { "height", PREVIEW_HEIGHT },
};

static const char *const mat_preview_names[] = {
	"albedo", "normals", "emissive", "mask", "roughness", "metallic", "height"
};

static struct {
	bool active;
	qhandle_t pic;                  // the map's r_images index; 0 = none
	char label[48];                 // which map: "albedo", "stage2 roughness"
	int width, height;
	char material[MAX_QPATH];
	char file[MAX_IMAGE_PATH + 16]; // the image, or why there is none
	unsigned shown_ms;              // the print, or the last frame the console was open
} mat_preview;

// the PREVIEW_* a word after 'mat print' names, or -1
static int mat_preview_word(const char *word)
{
	for (int i = 0; i < (int)q_countof(mat_preview_words); i++)
		if (!Q_stricmp(word, mat_preview_words[i].word))
			return mat_preview_words[i].map;
	return -1;
}

static void mat_preview_set(pbr_material_t const *mat, int map)
{
	image_t *image = NULL;
	const char *file = "";
	switch (map)
	{
	case PREVIEW_ALBEDO:    image = mat->image_base;      file = mat->filename_base; break;
	case PREVIEW_NORMALS:   image = mat->image_normals;   file = mat->filename_normals; break;
	case PREVIEW_EMISSIVE:  image = mat->image_emissive;  file = mat->filename_emissive; break;
	case PREVIEW_MASK:      image = mat->image_mask;      file = mat->filename_mask; break;
	case PREVIEW_ROUGHNESS: image = mat->image_roughness; file = mat->filename_roughness; break;
	case PREVIEW_METALLIC:  image = mat->image_metallic;  file = mat->filename_metallic; break;
	case PREVIEW_HEIGHT:    image = mat->image_height;    file = mat->filename_height; break;
	}

	memset(&mat_preview, 0, sizeof(mat_preview));
	mat_preview.active = true;
	Q_strlcpy(mat_preview.label, mat_preview_names[map], sizeof(mat_preview.label));
	mat_preview.shown_ms = Sys_Milliseconds();
	Q_strlcpy(mat_preview.material, mat->name, sizeof(mat_preview.material));

	if (image && image > r_images && image < r_images + MAX_RIMAGES && image->registration_sequence)
	{
		mat_preview.pic = (qhandle_t)(image - r_images);
		mat_preview.width = image->width;
		mat_preview.height = image->height;
		Q_strlcpy(mat_preview.file, image->name, sizeof(mat_preview.file));
	}
	else if (file[0])
		Q_concat(mat_preview.file, sizeof(mat_preview.file), file, " (NOT LOADED)");
	else
		Q_concat(mat_preview.file, sizeof(mat_preview.file), "no ", mat_preview_names[map], " map");
}

/* 'mat view stage<N> [map]': what one stage of a Quake III program draws - its
   image as it shows now - or the roughness, metallic or normals map its
   stage<N>_* settings give it (q3stages.h STAGE SETTINGS). */
static void mat_preview_set_stage(pbr_material_t const *mat, int stage, int map)
{
	int which = 0;
	switch (map)
	{
	case PREVIEW_ROUGHNESS: which = 1; break;
	case PREVIEW_METALLIC:  which = 2; break;
	case PREVIEW_NORMALS:   which = 3; break;
	default:                which = 0; break;
	}

	const char *file = "";
	const image_t *image = vkpt_q3_stage_image(mat, stage, which, &file);

	memset(&mat_preview, 0, sizeof(mat_preview));
	mat_preview.active = true;
	Q_snprintf(mat_preview.label, sizeof(mat_preview.label), "stage%d %s", stage,
		which == 0 ? "image" : mat_preview_names[map]);
	mat_preview.shown_ms = Sys_Milliseconds();
	Q_strlcpy(mat_preview.material, mat->name, sizeof(mat_preview.material));

	if (image && image > r_images && image < r_images + MAX_RIMAGES && image->registration_sequence)
	{
		mat_preview.pic = (qhandle_t)(image - r_images);
		mat_preview.width = image->width;
		mat_preview.height = image->height;
	}
	Q_strlcpy(mat_preview.file, file, sizeof(mat_preview.file));
}

// a line of the preview's caption, cut from the left to `max_chars` if it is longer
static void mat_preview_line(int x, int y, int max_chars, int flags, const char *s, qhandle_t font)
{
	size_t len = strlen(s);
	if (max_chars > 3 && len > (size_t)max_chars)
	{
		char buf[MAX_IMAGE_PATH + 32];
		Q_concat(buf, sizeof(buf), "...", s + len - (max_chars - 3));
		R_DrawString_RTX(x, y, flags, max_chars, buf, font);
	}
	else
		R_DrawString_RTX(x, y, flags, max_chars, s, font);
}

// R_EndFrame_RTX and D3D12's R_EndFrame: last, over the console
void MAT_DrawPreview(void)
{
	if (!mat_preview.active)
		return;

	unsigned now = Sys_Milliseconds();
	if (Key_GetDest() & KEY_CONSOLE)
		mat_preview.shown_ms = now;
	else if (now - mat_preview.shown_ms > MAT_PREVIEW_MS)
	{
		mat_preview.active = false;
		return;
	}

	qhandle_t font = R_RegisterFont("conchars");

	// a virtual screen about 720 lines tall, the console text's size on most setups
	const float scale = 1.f / (float)max(1, (int)((float)r_config.height / 720.f + 0.5f));
	const int vw = (int)((float)r_config.width * scale + 0.5f);
	const int vh = (int)((float)r_config.height * scale + 0.5f);

	// the map fits a square 40% of the screen's height, its own shape kept
	const int box = max(64, vh * 2 / 5);
	int w = box, h = box;
	if (mat_preview.pic && mat_preview.width > 0 && mat_preview.height > 0)
	{
		if (mat_preview.width >= mat_preview.height)
			h = max(8, box * mat_preview.height / mat_preview.width);
		else
			w = max(8, box * mat_preview.width / mat_preview.height);
	}

	char size_line[64];
	if (mat_preview.pic)
		Q_snprintf(size_line, sizeof(size_line), "%s  %dx%d", mat_preview.label,
			mat_preview.width, mat_preview.height);
	else
		Q_strlcpy(size_line, mat_preview.label, sizeof(size_line));

	// wide enough for the longest caption line, up to half the screen
	const int pad = 4, margin = 8, line_h = 10;
	size_t longest = max(strlen(size_line), max(strlen(mat_preview.material), strlen(mat_preview.file)));
	int panel_w = max(w, (int)longest * CHAR_WIDTH) + 2 * pad;
	panel_w = min(panel_w, max(w + 2 * pad, vw / 2));
	const int max_chars = (panel_w - 2 * pad) / CHAR_WIDTH;
	const int panel_h = pad + h + pad + 3 * line_h + pad;
	const int px = vw - margin - panel_w, py = margin;

	vkpt_draw_push_state(scale);

	R_DrawFill32_RTX(px, py, panel_w, panel_h, MakeColor(0, 0, 0, 220));

	const int ix = px + (panel_w - w) / 2, iy = py + pad;
	if (mat_preview.pic)
		vkpt_draw_stretch_pic_opaque(ix, iy, w, h, mat_preview.pic);
	else
		R_DrawFill32_RTX(ix, iy, w, h, MakeColor(64, 0, 64, 255));

	if (font)
	{
		int ty = iy + h + pad + 1;
		mat_preview_line(px + pad, ty, max_chars, UI_ALTCOLOR, size_line, font);
		mat_preview_line(px + pad, ty + line_h, max_chars, 0, mat_preview.material, font);
		mat_preview_line(px + pad, ty + 2 * line_h, max_chars, 0, mat_preview.file, font);
	}

	vkpt_draw_pop_state();
}

// "stage2", or "stage" "2", at argument (*argi): the stage number, with (*argi)
// moved past it - else -1
static int parse_stage_arg(int* argi)
{
	if (*argi >= Cmd_Argc())
		return -1;
	const char* s = Cmd_Argv(*argi);
	if (Q_strncasecmp(s, "stage", 5))
		return -1;

	const char* num = s + 5;
	int used = 1;
	if (!*num && *argi + 1 < Cmd_Argc())
	{
		num = Cmd_Argv(*argi + 1);
		used = 2;
	}
	if (!Q_isdigit(num[0]) || num[1])
		return -1;

	*argi += used;
	return num[0] - '0';
}

/* 'mat stage<N> <setting> <value>' (q3stages.h STAGE SETTINGS): one stage of the
   crosshair's Quake III shader - its image, or a surface of its own where it
   shows. Kept as the console's until 'mat save' writes it into the material's
   .mat entry as stage<N>_<setting>. No value: what the stage has. */
static void material_command_stage(pbr_material_t* mat, int stage, const char* setting, int field, const char* value)
{
	const q3shader_t* q3 = q3_shader_of(mat);
	q3stagesettings_t s;
	char shown[MAX_IMAGE_PATH];

	if (!q3)
	{
		Com_Printf("%s is not a Quake III shader: it has no stages\n", mat->name);
		return;
	}
	if (stage >= q3->num_stages)
	{
		Com_Printf("%s has %d stages, stage0 to stage%d ('mat print' lists them)\n", mat->name,
			q3->num_stages, q3->num_stages - 1);
		return;
	}

	if (!setting || field < 0)
	{
		if (setting)
			Com_Printf("'%s' is not a stage setting.\n", setting);
		vkpt_q3_stage_settings(mat->name, stage, &s);
		Com_Printf("%s stage%d:%s\n", mat->name, stage, s.set ? "" : " nothing of its own");
		for (int f = 0; f < Q3SS_COUNT; f++)
		{
			if (!(s.set & (1u << f)))
				continue;
			vkpt_q3_stage_format_value(&s, f, shown, sizeof(shown));
			Com_Printf("    %s %s\n", vkpt_q3_stage_field_name(f), shown);
		}
		Com_Printf("usage: mat stage%d <setting> <value|none>, the setting one of: map (its image; "
			"'map off' leaves the stage out), roughness_override, metalness_factor, specular_factor, "
			"bump_scale, texture_roughness, texture_metallic, texture_normals, emissive_factor (its own "
			"glow, added to the material's)\n", stage);
		return;
	}

	if (!value)
	{
		vkpt_q3_stage_settings(mat->name, stage, &s);
		vkpt_q3_stage_format_value(&s, field, shown, sizeof(shown));
		Com_Printf("%s stage%d_%s %s\n", mat->name, stage, vkpt_q3_stage_field_name(field), shown);
		return;
	}

	char err[256];
	if (!vkpt_q3_stage_setting_add(Q3SS_SCOPE_CONSOLE, mat->name, stage, field, value, err, sizeof(err)))
	{
		Com_Printf("stage%d_%s %s: %s\n", stage, vkpt_q3_stage_field_name(field), value, err);
		return;
	}

	vkpt_q3_stage_settings(mat->name, stage, &s);
	vkpt_q3_stage_format_value(&s, field, shown, sizeof(shown));
	Com_Printf("%s stage%d_%s %s ('mat save' writes it into %s)\n", mat->name, stage,
		vkpt_q3_stage_field_name(field), shown,
		mat->source_matfile[0] ? mat->source_matfile : "nothing: no .mat entry - 'mat save <file>'");

	if (q3->stages[stage].lightmap && field != Q3SS_MAP)
		Com_Printf("  stage%d is the $lightmap - the light, not a surface: this does nothing\n", stage);

	if (!mat->q3_program)
	{
		if (field == Q3SS_MAP)
			Com_Printf("  %s draws still, its image taken when it loads: 'mat save', then 'mat reload'\n", mat->name);
		else
			Com_Printf("  %s draws still, from its own maps (no stage program): its stages have no surfaces "
				"of their own - set the material's (roughness_override, ...)\n", mat->name);
		return;
	}

	// a stage's own map typed in: the surface is no longer "basic diffuse"
	// (where the stage shows, what it does not state comes from the material)
	if ((field == Q3SS_TEXTURE_ROUGHNESS || field == Q3SS_TEXTURE_METALLIC || field == Q3SS_TEXTURE_NORMALS)
		&& (s.set & (1u << field)))
		material_revive_specular(mat, field == Q3SS_TEXTURE_METALLIC);

	// The program draws from what it was built with: rebuilt, as for 'mat
	// texture_emissive' - and for a new image the map too, which is what its
	// glow lights from.
	material_rebuild_program(mat, q3);
	if (field == Q3SS_MAP && vkpt_refdef.fd)
	{
		vkpt_vertex_buffer_invalidate_static_model_vbos(MAT_IndexOf(mat));
		CL_PrepRefresh();
	}
}

/*
=================
THE VIEW WEAPON'S MATERIALS ('mat view_weapon')

The crosshair cannot reach the gun in your hands: 'mat print' reads the
material under the middle of the screen, and the view weapon is off to one
side. Every frame notes the materials of what is drawn as the view weapon
(RF_WEAPONMODEL: the gun, a Quake III gun's barrel and flash, Quake II's flash
model), and keeps them while the same gun is up - so a flash that shows for a
frame or two can still be picked after the shot. A new gun starts the list
again. 'mat view_weapon [N] <command>' then runs any 'mat' command on number
N (0, the gun's own, if left out) instead of the crosshair's. Matt,
2026-11-18: "mat view_weapon print", "mat view_weapon roughness_override 1".
=================
*/
#define MAX_VIEW_WEAPON_MATERIALS 16

static struct {
	char material[MAX_QPATH];   // by name: a reload makes the materials anew
	char model[MAX_QPATH];      // what it is on
} view_weapon_materials[MAX_VIEW_WEAPON_MATERIALS];
static int num_view_weapon_materials;
static qhandle_t view_weapon_gun;   // the frame's first view weapon model

// while 'mat view_weapon' runs a command: its material, not the crosshair's
static pbr_material_t* mat_command_target;

static const pbr_material_t* view_weapon_mesh_material(const entity_t* e, const model_t* model,
	const maliasmesh_t* mesh)
{
	// as the renderer picks it (main.c get_mesh_material)
	if (e->skin)
		return MAT_ForSkin(IMG_ForHandle(e->skin));

	int wanted = e->skinnum + model->skin_base;
	if (e->skinnum >= 0 && wanted < (int)q_countof(mesh->materials) && mesh->materials[wanted])
		return mesh->materials[wanted];
	return mesh->materials[0];
}

void MAT_NoteViewWeapon(const refdef_t* fd)
{
	bool first = true;

	for (int i = 0; i < fd->num_entities; i++)
	{
		const entity_t* e = fd->entities + i;
		if (!(e->flags & RF_WEAPONMODEL) || !e->model || (e->model & 0x80000000))
			continue;

		const model_t* model = MOD_ForHandle(e->model);
		if (!model || !model->meshes)
			continue;

		if (first)
		{
			first = false;
			if (e->model != view_weapon_gun)
			{
				view_weapon_gun = e->model;
				num_view_weapon_materials = 0;
			}
		}

		for (int m = 0; m < model->nummeshes; m++)
		{
			const pbr_material_t* mat = view_weapon_mesh_material(e, model, model->meshes + m);
			if (!mat || !mat->name[0])
				continue;

			int k;
			for (k = 0; k < num_view_weapon_materials; k++)
				if (!strcmp(view_weapon_materials[k].material, mat->name))
					break;
			if (k < num_view_weapon_materials || k == MAX_VIEW_WEAPON_MATERIALS)
				continue;

			Q_strlcpy(view_weapon_materials[k].material, mat->name, sizeof(view_weapon_materials[k].material));
			Q_strlcpy(view_weapon_materials[k].model, model->name, sizeof(view_weapon_materials[k].model));
			num_view_weapon_materials++;
		}
	}
}

static bool view_weapon_is_number(const char* n)
{
	if (!n[0])
		return false;
	for (const char* c = n; *c; c++)
		if (!Q_isdigit(*c))
			return false;
	return true;
}

static void material_command_view_weapon(void)
{
	int argi = 2, index = 0, skip = -1;

	if (mat_command_target)
	{
		Com_Printf("'mat view_weapon' is already picking the material\n");
		return;
	}

	// a number first: which of them
	if (argi < Cmd_Argc() && view_weapon_is_number(Cmd_Argv(argi)))
	{
		index = atoi(Cmd_Argv(argi));
		argi++;
	}
	// ... or straight after the words that take no value: 'mat view_weapon
	// print 1' is 'mat view_weapon 1 print', not a material named "1"
	else if (argi + 1 < Cmd_Argc() && view_weapon_is_number(Cmd_Argv(argi + 1)) &&
		(!strcmp(Cmd_Argv(argi), "print") || !strcmp(Cmd_Argv(argi), "view") || !strcmp(Cmd_Argv(argi), "which")))
	{
		index = atoi(Cmd_Argv(argi + 1));
		skip = argi + 1;
	}

	if (!num_view_weapon_materials)
	{
		Com_Printf("No view weapon drawn yet: one has to be up and on screen (cl_gun 1, hand 0 or 1)\n");
		return;
	}

	if (argi >= Cmd_Argc() || !strcmp(Cmd_Argv(argi), "list"))
	{
		Com_Printf("The view weapon's materials - 'mat view_weapon [N] <command>', N 0 if left out:\n");
		for (int i = 0; i < num_view_weapon_materials; i++)
			Com_Printf("  %d: %s  (%s)\n", i, view_weapon_materials[i].material, view_weapon_materials[i].model);
		Com_Printf("e.g. 'mat view_weapon print', 'mat view_weapon 1 roughness_override 0.3', 'mat save'\n");
		return;
	}

	if (index >= num_view_weapon_materials)
	{
		Com_Printf("The view weapon has no material %d: 'mat view_weapon' lists them\n", index);
		return;
	}

	const char* name = view_weapon_materials[index].material;
	pbr_material_t* mat = find_material(name, Com_HashString(name, RMATERIALS_HASH), r_materials, MAX_PBR_MATERIALS);
	if (!mat)
	{
		Com_Printf("'%s' is not loaded now\n", name);
		return;
	}

	const char* sub = Cmd_Argv(argi);
	if (!strcmp(sub, "create_instance"))
	{
		Com_Printf("Instances are keyed on the level's own surfaces: a weapon's material has none\n");
		return;
	}
	if (!strcmp(sub, "view_weapon"))
		return;

	// the rest of the line, as a 'mat' command of its own
	char line[MAX_STRING_CHARS];
	if (skip < 0)
		Q_concat(line, sizeof(line), "mat ", Cmd_RawArgsFrom(argi));
	else
	{
		Q_strlcpy(line, "mat", sizeof(line));
		for (int a = argi; a < Cmd_Argc(); a++)
			if (a != skip)
				Q_strlcat(line, va(" \"%s\"", Cmd_Argv(a)), sizeof(line));
	}
	Cmd_TokenizeString(line, false);

	mat_command_target = mat;
	material_command();
	mat_command_target = NULL;
}

static void material_command_help(void)
{
	Com_Printf("mat command - interface to the material system\n");
	Com_Printf("usage: mat <command> <arguments...>\n");
	Com_Printf("available commands:\n");
	Com_Printf("    help: print this message\n");
	Com_Printf("    print [name] [map]: print the material at the crosshair, or the named one,\n");
	Com_Printf("        and show its albedo in the top-right corner - or the map named:\n");
	Com_Printf("        normals, emissive, mask, roughness, metallic, height ('mat print normals')\n");
	Com_Printf("    view [stage<N>] [map]: only show it - or what stage N of a Quake III shader draws,\n");
	Com_Printf("        or its own roughness, metallic or normals map ('mat view stage2 roughness')\n");
	Com_Printf("    stage<N> <setting> <value>: one stage of a Quake III shader - map (its image), or a\n");
	Com_Printf("        surface of its own where it shows: roughness_override, metalness_factor,\n");
	Com_Printf("        specular_factor, bump_scale, texture_roughness, texture_metallic, texture_normals\n");
	Com_Printf("        ('none' takes one away; 'map off' leaves the stage out of the drawing).\n");
	Com_Printf("        In a .mat file: stage2_roughness_override 0.8, stage1_map off\n");
	Com_Printf("    author [roughness|metallic|normals|emissive]: paint the crosshair's material's\n");
	Com_Printf("        maps with a brush, in photo mode (pauses the game). Hold the right mouse\n");
	Com_Printf("        button to look, WASD/QE to fly, left button paints. 'mat author save'\n");
	Com_Printf("        writes the painted maps; 'mat author off' (or Escape) leaves.\n");
	Com_Printf("        'mat author stage <N|mat>' paints a Quake III stage's own maps;\n");
	Com_Printf("        'mat author rect <length> <width>' picks a rectangle brush (texels)\n");
	Com_Printf("    which: tell where the current material is defined\n");
	Com_Printf("    view_weapon [N] <command>: any of these for the gun in your hands instead of\n");
	Com_Printf("        the crosshair's material ('mat view_weapon print', 'mat view_weapon\n");
	Com_Printf("        roughness_override 1'); alone, it lists the gun's materials and their N\n");
	Com_Printf("    create_instance: give the surface at the crosshair its own material,\n");
	Com_Printf("        so it can be tuned without affecting other uses of the same texture.\n");
	Com_Printf("        Writes an empty section to materials/<map>.mat and reloads.\n");
	Com_Printf("    reload: re-read all .mat files, and any texture saved since it was loaded, from disk and rebuild\n");
	Com_Printf("    save: write what 'mat <attribute> <value>' changed back into the .mat entries\n");
	Com_Printf("        those materials came from - only those lines (the old file is kept as .bak)\n");
	Com_Printf("    save <filename> <options>: save the active materials to a new file\n");
	Com_Printf("        option 'all': save all materials (otherwise only the undefined ones)\n");
	Com_Printf("        option 'force': overwrite the output file if it exists\n");
	Com_Printf("    <attribute> <value>: set an attribute of the current material\n");
	Com_Printf("        use 'mat print' to list the available attributes\n");
}

static void material_command(void)
{
	if (Cmd_Argc() < 2)
	{
		material_command_help();
		return;
	}

	const char* key = Cmd_Argv(1);

	if (strcmp(key, "help") == 0)
	{
		material_command_help();
		return;
	}

	if (strcmp(key, "view_weapon") == 0)
	{
		material_command_view_weapon();
		return;
	}

	// 'mat author': paint the crosshair's material's maps (mat_author.c). Its own
	// words ('save', 'off') need no material; 'mat view_weapon author' paints the gun.
	if (strcmp(key, "author") == 0)
	{
		pbr_material_t* target = mat_command_target;
		if (!target && vkpt_refdef.fd)
			target = MAT_ForIndex(vkpt_refdef.fd->feedback.view_material_index);
		MAT_AuthorCommand(target);
		return;
	}

	if (strcmp(key, "save") == 0)
	{
		// no file name: into the files the changed materials came from
		if (Cmd_Argc() < 3)
		{
			save_to_source_files();
			return;
		}

		const char* file_name = Cmd_Argv(2);

		bool save_all = false;
		bool force = false;
		for (int i = 3; i < Cmd_Argc(); i++)
		{
			if (strcmp(Cmd_Argv(i), "all") == 0)
				save_all = true;
			else if (strcmp(Cmd_Argv(i), "force") == 0)
				force = true;
			else {
				Com_Printf("unrecognized argument: %s\n", Cmd_Argv(i));
				return;
			}
		}

		save_materials(file_name, save_all, force);
		return;
	}

	// handled before the crosshair lookup: a reload does not need a material
	// under the crosshair, and after it the old index would be stale anyway
	if (strcmp(key, "reload") == 0)
	{
		material_command_reload();
		return;
	}

	// 'print <name>' addresses a live material directly. The crosshair cannot
	// reach a skin that is not on screen - a monster variant this map does not
	// spawn, say - and that is exactly when one needs checking, because a
	// tuning-only .mat entry is invisible in 'mat save' by design.
	/* Which of the material's maps 'mat print' shows on screen: the albedo, or
	   one named after it - 'mat print normals' (the crosshair's material) or
	   'mat print <name> normals'. */
	int preview_map = PREVIEW_ALBEDO;
	/* ... or one stage of a Quake III program: 'mat print stage2', 'mat view
	   stage 2 roughness' - what it draws, or its own maps (STAGE SETTINGS).
	   'mat view' only shows, with nothing printed (Matt, 2026-11-16). */
	const bool view = strcmp(key, "view") == 0;
	int preview_stage = -1;
	if ((view || strcmp(key, "print") == 0) && Cmd_Argc() >= 3)
	{
		int argi = 2;
		preview_stage = parse_stage_arg(&argi);
		if (preview_stage < 0)
			argi = 2;
		if (argi < Cmd_Argc() && (preview_stage >= 0 || view))
		{
			preview_map = mat_preview_word(Cmd_Argv(argi));
			if (preview_map < 0)
			{
				Com_Printf("'%s' is not a map: albedo, normals, roughness or metallic%s\n", Cmd_Argv(argi),
					preview_stage >= 0 ? "" : ", emissive, mask or height");
				preview_map = PREVIEW_ALBEDO;
			}
		}
	}

	if (view || preview_stage >= 0)
	{
		// the crosshair's material, below
	}
	else if (strcmp(key, "print") == 0 && Cmd_Argc() >= 3 && mat_preview_word(Cmd_Argv(2)) >= 0)
	{
		preview_map = mat_preview_word(Cmd_Argv(2));
	}
	else if (strcmp(key, "print") == 0 && Cmd_Argc() >= 3)
	{
		char name_no_ext[MAX_QPATH];
		truncate_extension(Cmd_Argv(2), name_no_ext, sizeof(name_no_ext));
		Q_strlwr(name_no_ext);

		pbr_material_t* named = find_material(name_no_ext,
			Com_HashString(name_no_ext, RMATERIALS_HASH), r_materials, MAX_PBR_MATERIALS);

		if (Cmd_Argc() >= 4)
		{
			preview_map = mat_preview_word(Cmd_Argv(3));
			if (preview_map < 0)
			{
				Com_Printf("'%s' is not a map: albedo, normals, emissive, mask, roughness, metallic or height\n", Cmd_Argv(3));
				preview_map = PREVIEW_ALBEDO;
			}
		}

		if (named)
		{
			MAT_Print(named);
			mat_preview_set(named, preview_map);
		}
		else
			Com_Printf("No material named '%s' is loaded\n", name_no_ext);
		return;
	}

	pbr_material_t* mat = mat_command_target;

	if (!mat && vkpt_refdef.fd)
		mat = MAT_ForIndex(vkpt_refdef.fd->feedback.view_material_index);

	if (!mat)
		return;

	if (view)
	{
		if (preview_stage >= 0)
			mat_preview_set_stage(mat, preview_stage, preview_map);
		else
			mat_preview_set(mat, preview_map);
		Com_Printf("%s: %s\n", mat_preview.label, mat_preview.file);
		return;
	}

	if (strcmp(key, "print") == 0)
	{
		MAT_Print(mat);
		if (preview_stage >= 0)
			mat_preview_set_stage(mat, preview_stage, preview_map);
		else
			mat_preview_set(mat, preview_map);

		/* The surface's own identity, so two uses of one texture can be told
		   apart BEFORE committing to splitting them - aim at each in turn and
		   compare. Nothing to report off the static world, where there is no
		   geometry hash. */
		if (!mat_command_target && vkpt_refdef.fd && vkpt_refdef.fd->feedback.view_prim_instance_hash)
		{
			uint32_t instance_hash = vkpt_refdef.fd->feedback.view_prim_instance_hash;
			char instance_name[MAX_QPATH];
			MAT_InstanceName(mat->name, instance_hash, instance_name, sizeof(instance_name));

			if (instance_separator(mat->name))
				Com_Printf("    surface: this IS an instance\n");
			else
				Com_Printf("    surface: %s ('mat create_instance' to split it off)\n", instance_name);
		}
		return;
	}
	
	if (strcmp(key, "create_instance") == 0)
	{
		material_create_instance(mat);
		return;
	}

	if (strcmp(key, "which") == 0)
	{
		Com_Printf("%s: ", mat->name);
		if (mat->source_matfile[0])
		{
			char where[MAX_OSPATH];
			material_source_location(mat->source_matfile, where, sizeof(where));
			Com_Printf("%s line %u\n", where, mat->source_line);
		}
		else
			Com_Printf("automatically generated\n");
		return;
	}

	// 'mat stage2 roughness_override 0.8', 'mat stage 2 ...', or the .mat
	// file's spelling, 'mat stage2_roughness_override 0.8'
	{
		int argi = 1, field = -1;
		const char* setting = NULL;
		int stage = vkpt_q3_stage_parse_key(key, &field);
		if (stage >= 0)
		{
			setting = key + 7;
			argi = 2;
		}
		else if ((stage = parse_stage_arg(&argi)) >= 0)
		{
			setting = argi < Cmd_Argc() ? Cmd_Argv(argi) : NULL;
			field = vkpt_q3_stage_field(setting);
			argi++;
		}
		if (stage >= 0)
		{
			material_command_stage(mat, stage, setting, field, argi < Cmd_Argc() ? Cmd_Argv(argi) : NULL);
			return;
		}
	}

	if (Cmd_Argc() < 3)
	{
		Com_Printf("expected value for attribute\n");
		return;
	}

	unsigned int reload_flags = 0;
	const struct MaterialAttribute* attr = find_attribute(key);
	const uint32_t specified_before = mat->specified_fields;
	if (set_material_attribute(mat, key, Cmd_Argv(2), NULL, 0, &reload_flags) == Q_ERR_SUCCESS && attr)
	{
		// what 'mat save' writes back (save_to_source_files) - with whatever
		// else it set: "kind ALPHA_GLASS" is also alpha_glass 1
		mat->console_fields |= (1u << attr->index) | (mat->specified_fields & ~specified_before);
		if (attr->index == MAT_KIND && !Q_stricmp(Cmd_Argv(2), "ALPHA_GLASS"))
			mat->console_fields |= 1u << MAT_ALPHA_GLASS;

		/* A QUAKE III PROGRAM DRAWS FROM THE IMAGES IT WAS BUILT WITH. Typed
		   here, texture_emissive / texture_base changed the material's image
		   and left its program drawing the old stages: firegorre.tga named as
		   the glow of q3dm1's skull arch (textures/gothic_door/skullarch_b)
		   never glowed - nor after a map reload, which keeps the live material
		   (Matt, 2026-11-16). Rebuilt as 'mat reload' would, and the map
		   reloaded so a glow that is new gets its light polys. */
		if ((attr->index == MAT_TEXTURE_EMISSIVE || attr->index == MAT_TEXTURE_BASE) && mat->q3_program)
		{
			// an instance's from its base's shader (q3_shader_of), and the
			// instances' of it with it
			const q3shader_t* q3 = q3_shader_of(mat);
			if (q3)
			{
				material_rebuild_program(mat, q3);
				reload_flags |= RELOAD_MAP;
			}
		}

		// a map typed in: the surface is no longer "basic diffuse"
		if ((attr->index == MAT_TEXTURE_ROUGHNESS && mat->image_roughness)
			|| (attr->index == MAT_TEXTURE_METALLIC && mat->image_metallic)
			|| (attr->index == MAT_TEXTURE_NORMALS && mat->image_normals))
			material_revive_specular(mat, attr->index == MAT_TEXTURE_METALLIC);
	}

	if ((reload_flags & RELOAD_EMISSIVE) != 0)
	{
		if(mat->image_emissive && strstr(mat->image_emissive->name, "*E"))
		{
			// Prevent previous image being reused to allow emissive_threshold changes
			mat->image_emissive->name[0] = 0;
			mat->image_emissive = NULL;
		}
		if(mat->synth_emissive && !mat->image_emissive)
		{
			// Regenerate emissive image
			MAT_SynthesizeEmissive(mat);

			// In some cases, MAT_SynthesizeEmissive might not create an emissive image - test for that
			if (mat->image_emissive)
			{
				// Make sure it's loaded by CL_PrepRefresh()
				IMG_Load(mat->image_emissive, mat->image_emissive->pix_data);
			}

			reload_flags |= RELOAD_MAP;
		}
	}

	if ((reload_flags & RELOAD_MAP) != 0)
	{
		// Trigger a re-upload and rebuild of the models that use this material.
		// Reason to rebuild: some material changes result in meshes being classified as
		// transparent or masked, which affects the static model BLAS.
		vkpt_vertex_buffer_invalidate_static_model_vbos(MAT_IndexOf(mat));

		// Reload the map and necessary models.
		CL_PrepRefresh();
	}
}

/* 'mat <args>' run on (mat), as if it were under the crosshair - how 'mat author'
   (mat_author.c) points a material at a map it made or saved, so the change goes
   through everything a typed one does (the image loaded, specular revived, the
   attribute marked for 'mat save'). Retokenizes: read your own arguments first. */
void MAT_CommandFor(pbr_material_t* mat, const char* args)
{
	char line[MAX_STRING_CHARS];
	Q_concat(line, sizeof(line), "mat ", args);
	Cmd_TokenizeString(line, false);

	pbr_material_t* saved = mat_command_target;
	mat_command_target = mat;
	material_command();
	mat_command_target = saved;
}

// 'mat save' with no file name: every changed material into its own .mat entry
void MAT_SaveToSourceFiles(void)
{
	save_to_source_files();
}

// the words of a 'mat' command from (first) on: 1 for 'mat ...', 2 or 3 after
// 'mat view_weapon [N]', which takes the same commands for the gun
static void material_complete_from(genctx_t* ctx, int argnum, int first)
{
	const bool weapon = first > 1;

	if (argnum == first) {
		// sub-commands
		
		Prompt_AddMatch(ctx, "print");
		Prompt_AddMatch(ctx, "view");
		Prompt_AddMatch(ctx, "save");
		Prompt_AddMatch(ctx, "which");
		Prompt_AddMatch(ctx, "author");
		if (!weapon) {
			Prompt_AddMatch(ctx, "reload");
			Prompt_AddMatch(ctx, "create_instance");
			Prompt_AddMatch(ctx, "view_weapon");
		}
		for (int i = 0; i < Q3P_MAX_STAGES; i++)
			Prompt_AddMatch(ctx, va("stage%d", i));

		for (int i = 0; i < c_NumAttributes; i++)
			Prompt_AddMatch(ctx, c_Attributes[i].name);
	}
	else if (argnum == first + 1)
	{
		const char* sub = Cmd_Argv(first);

		if(strcmp(sub, "save") == 0)
		{
			// extra arguments for 'save'
			Prompt_AddMatch(ctx, "all");
			Prompt_AddMatch(ctx, "force");
		}
		else if (strcmp(sub, "author") == 0)
		{
			Prompt_AddMatch(ctx, "save");
			Prompt_AddMatch(ctx, "off");
			Prompt_AddMatch(ctx, "roughness");
			Prompt_AddMatch(ctx, "metallic");
			Prompt_AddMatch(ctx, "normals");
			Prompt_AddMatch(ctx, "emissive");
			Prompt_AddMatch(ctx, "stage");
			Prompt_AddMatch(ctx, "rect");
		}
		else if (strcmp(sub, "print") == 0 || strcmp(sub, "view") == 0)
		{
			// the map to show on screen, or the stage
			for (int i = 0; i < (int)q_countof(mat_preview_names); i++)
				Prompt_AddMatch(ctx, mat_preview_names[i]);
			for (int i = 0; i < Q3P_MAX_STAGES; i++)
				Prompt_AddMatch(ctx, va("stage%d", i));
		}
		else if (!Q_strncasecmp(sub, "stage", 5) && Q_isdigit(sub[5]) && !sub[6])
		{
			// a stage's settings
			for (int f = 0; f < Q3SS_COUNT; f++)
				Prompt_AddMatch(ctx, vkpt_q3_stage_field_name(f));
		}
		else if((strcmp(sub, "which") == 0)
			|| (strcmp(sub, "create_instance") == 0))
		{
			// Nothing to complete for these
		}
		else
		{
			// Material property completion
			struct MaterialAttribute const* t = NULL;
			for (int i = 0; i < c_NumAttributes; ++i)
			{
				if (strcmp(sub, c_Attributes[i].name) == 0)
				{
					t = &c_Attributes[i];
					break;
				}
			}

			if(!t)
				return;

			// Attribute-specific completions
			switch(t->index)
			{
			case MAT_KIND:
				for (int i = 0; i < nMaterialKinds; ++i)
				{
					// Use lower-case for completion, that is what you'd typically type
					char kind[32];
					Q_strlcpy(kind, materialKinds[i].name, sizeof(kind));
					Q_strlwr(kind);
					Prompt_AddMatch(ctx, kind);
				}
				return;
			default:
				break;
			}

			// Type-specific completions
			if(t->type == ATTR_BOOL)
			{
				Prompt_AddMatch(ctx, "0");
				Prompt_AddMatch(ctx, "1");
				return;
			}
		}
	}
}

static void material_completer(genctx_t* ctx, int argnum)
{
	if (argnum >= 2 && !strcmp(Cmd_Argv(1), "view_weapon"))
	{
		// 'mat view_weapon [N] ...': the gun's materials by number, then
		// whatever 'mat' itself takes
		const char* n = Cmd_Argv(2);
		bool digits = argnum > 2 && n[0] != 0;
		for (const char* c = n; digits && *c; c++)
			digits = Q_isdigit(*c) != 0;

		if (argnum == 2)
		{
			Prompt_AddMatch(ctx, "list");
			for (int i = 0; i < num_view_weapon_materials; i++)
				Prompt_AddMatch(ctx, va("%d", i));
		}
		material_complete_from(ctx, argnum, digits ? 3 : 2);
		return;
	}
	material_complete_from(ctx, argnum, 1);
}

uint32_t MAT_SetKind(uint32_t material, uint32_t kind)
{
	return (material & ~MATERIAL_KIND_MASK) | kind;
}

bool MAT_IsKind(uint32_t material, uint32_t kind)
{
	return (material & MATERIAL_KIND_MASK) == kind;
}

static image_t* get_fake_emissive_image(image_t* diffuse, int bright_threshold_int)
{
	switch(cvar_pt_surface_lights_fake_emissive_algo->integer)
	{
	case 0:
		return diffuse;
	case 1:
		return vkpt_fake_emissive_texture(diffuse, bright_threshold_int);
	default:
		return NULL;
	}
}

void MAT_SynthesizeEmissive(pbr_material_t * mat)
{
	mat->flags |= MATERIAL_FLAG_LIGHT;

	if (!mat->image_emissive) {
		mat->image_emissive = get_fake_emissive_image(mat->image_base, mat->emissive_threshold);
		mat->synth_emissive = true;

		if (mat->image_emissive) {
			vkpt_extract_emissive_texture_info(mat->image_emissive);
		}
	}
}

bool MAT_IsTransparent(uint32_t material)
{
	return MAT_IsKind(material, MATERIAL_KIND_SLIME)
		|| MAT_IsKind(material, MATERIAL_KIND_WATER)
		|| MAT_IsKind(material, MATERIAL_KIND_GLASS)
		|| MAT_IsKind(material, MATERIAL_KIND_TRANSPARENT)
		|| MAT_IsKind(material, MATERIAL_KIND_TRANSP_MODEL);
}

// "Masked" here means "goes in the masked-model BLAS" - the one built with
// VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR - so it is also how a
// model becomes two-sided, and double_sided materials ride along. That costs
// them nothing else: pt_logic_masked accepts every hit on a material with no
// mask texture, and the hit shaders already flip a back face's normal.
bool MAT_IsMasked(uint32_t material)
{
	const pbr_material_t* mat = MAT_ForIndex((int)(material & MATERIAL_INDEX_MASK));

	return mat && (mat->image_mask || (mat->flags & MATERIAL_FLAG_DOUBLE_SIDED));
}
