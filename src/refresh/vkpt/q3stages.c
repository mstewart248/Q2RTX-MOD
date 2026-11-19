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

// q3stages.c -- Quake III shader stages drawn by the path tracer; see q3stages.h

#include "vkpt.h"
#include "q3stages.h"
#include "conversion.h"
#include "shader/vertex_buffer.h"
#include "format/bsp3.h"

#include <assert.h>

// the GPU reads the blend factors with q3shader.h's numbering
static_assert(Q3P_BF_ONE == Q3BF_ONE && Q3P_BF_ZERO == Q3BF_ZERO && Q3P_BF_DST_COLOR == Q3BF_DST_COLOR &&
              Q3P_BF_ONE_MINUS_DST_COLOR == Q3BF_ONE_MINUS_DST_COLOR && Q3P_BF_SRC_ALPHA == Q3BF_SRC_ALPHA &&
              Q3P_BF_ONE_MINUS_SRC_ALPHA == Q3BF_ONE_MINUS_SRC_ALPHA && Q3P_BF_DST_ALPHA == Q3BF_DST_ALPHA &&
              Q3P_BF_ONE_MINUS_DST_ALPHA == Q3BF_ONE_MINUS_DST_ALPHA && Q3P_BF_SRC_COLOR == Q3BF_SRC_COLOR &&
              Q3P_BF_ONE_MINUS_SRC_COLOR == Q3BF_ONE_MINUS_SRC_COLOR &&
              Q3P_BF_SRC_ALPHA_SATURATE == Q3BF_SRC_ALPHA_SATURATE,
              "Q3P_BF_* (shader/constants.h) must match Q3BF_* (q3shader.h)");
static_assert(Q3ALPHA_GT0 == 1 && Q3ALPHA_LT128 == 2 && Q3ALPHA_GE128 == 3,
              "q3_stages.glsl reads the alpha test as 1 GT0, 2 LT128, 3 GE128");

#define MAX_Q3_PROGRAMS     512

typedef struct {
    image_t     *images[Q3S_MAX_ANIMFRAMES];
    int         num_images;
    float       anim_freq;
    int         src_blend, dst_blend;   // never Q3BF_NONE: no blendFunc is ONE, ZERO
    int         alpha_func;             // Q3ALPHA_*
    uint32_t    flags;                  // Q3P_STAGE_*
    int         rgbgen;
    vec3_t      rgb_const;
    q3wave_t    rgb_wave;
    int         alphagen;
    float       alpha_const;
    q3wave_t    alpha_wave;
    int         num_tcmods;
    q3tcmod_t   tcmods[Q3S_MAX_TCMODS];
    // its own surface, from the .mat's stage<N>_* settings (STAGE SETTINGS)
    uint32_t    prop_flags;             // Q3P_PROP_*
    image_t     *prop_images[3];        // roughness, metallic, normals
    float       prop_values[4];         // roughness_override, metalness_factor, specular_factor, bump_scale
    float       glow_factor;            // stage<N>_emissive_factor, 0 = none: Q3P_PROG_STAGE_EMIT
} q3pstage_t;

typedef struct {
    bool        in_use;
    int         owner;                  // index of the material that built it
    uint32_t    flags;                  // Q3P_PROG_*
    bool        has_env;                // a reflection-map stage, for pt_q3_env_shine
    bool        shine_metal;            // ... and the shader reads as metal (shine_is_metal)
    bool        has_props;              // a stage has prop_flags: Q3P_PROG_PROPS
    uint32_t    off;                    // 1 << stage: "stage<N>_map off", left out ('mat print')
    int         num_stages;
    q3pstage_t  stages[Q3P_MAX_STAGES];
    uint32_t    offset;                 // this frame's first vec4 + 1, 0 when not uploaded
    // for the glow as a light (vkpt_q3_light_glow):
    uint32_t    build;                  // which build of it this is, never 0 (vkpt_q3_program_key)
    bool        varies;                 // what it emits changes with time
    bool        glow_moves;             // ... moves over the surface, or is more than one glow
} q3program_t;

static q3program_t programs[MAX_Q3_PROGRAMS];
static uint32_t program_builds;

/*
===============================================================================

STAGE SETTINGS (.mat "stage<N>_<setting>", q3stages.h)

===============================================================================
*/

typedef struct {
    int         scope;                  // Q3SS_SCOPE_*
    int         stage;
    char        material[MAX_QPATH];
    uint32_t    set;                    // 1 << Q3SS_*: what this record states
    uint32_t    none;                   // ... as "there is none"
    uint32_t    unsaved;                // ... typed in the console, not in a file yet
    char        paths[Q3SS_NUM_PATHS][MAX_IMAGE_PATH];
    float       values[Q3SS_COUNT - Q3SS_NUM_PATHS];
} q3override_t;

static q3override_t *overrides;
static int num_overrides, max_overrides;

static const char *const stage_field_names[Q3SS_COUNT] = {
    "map", "texture_roughness", "texture_metallic", "texture_normals",
    "roughness_override", "metalness_factor", "specular_factor", "bump_scale", "emissive_factor",
};

int vkpt_q3_stage_field(const char *word)
{
    if (!word)
        return -1;
    if (!Q_stricmp(word, "texture_base"))
        return Q3SS_MAP;
    for (int f = 0; f < Q3SS_COUNT; f++)
        if (!Q_stricmp(word, stage_field_names[f]))
            return f;
    return -1;
}

const char *vkpt_q3_stage_field_name(int field)
{
    return field >= 0 && field < Q3SS_COUNT ? stage_field_names[field] : "?";
}

int vkpt_q3_stage_parse_key(const char *key, int *field)
{
    *field = -1;

    // "stage" <digit> "_" <setting>
    if (!key || Q_strncasecmp(key, "stage", 5))
        return -1;
    if (key[5] < '0' || key[5] > '9' || key[6] != '_')
        return -1;
    int stage = key[5] - '0';
    if (stage >= Q3P_MAX_STAGES)
        return -1;

    *field = vkpt_q3_stage_field(key + 7);
    return stage;
}

void vkpt_q3_stage_override_clear(int scope)
{
    int n = 0;
    for (int i = 0; i < num_overrides; i++)
        if (overrides[i].scope != scope)
            overrides[n++] = overrides[i];
    num_overrides = n;
}

static q3override_t *find_override(int scope, const char *material, int stage, bool create)
{
    for (int i = 0; i < num_overrides; i++) {
        q3override_t *o = &overrides[i];
        if (o->scope == scope && o->stage == stage && !Q_stricmp(o->material, material))
            return o;
    }
    if (!create)
        return NULL;

    if (num_overrides == max_overrides) {
        max_overrides = max_overrides ? max_overrides * 2 : 256;
        q3override_t *grown = Z_Malloc(sizeof(*grown) * max_overrides);
        if (overrides) {
            memcpy(grown, overrides, sizeof(*grown) * num_overrides);
            Z_Free(overrides);
        }
        overrides = grown;
    }

    q3override_t *o = &overrides[num_overrides++];
    memset(o, 0, sizeof(*o));
    o->scope = scope;
    o->stage = stage;
    Q_strlcpy(o->material, material, sizeof(o->material));
    return o;
}

bool vkpt_q3_stage_setting_add(int scope, const char *material, int stage, int field,
                               const char *value, char *err, size_t err_size)
{
    if (stage < 0 || stage >= Q3P_MAX_STAGES || !material || !value) {
        Q_snprintf(err, err_size, "stages go from 0 to %d", Q3P_MAX_STAGES - 1);
        return false;
    }
    if (field < 0 || field >= Q3SS_COUNT) {
        Q_strlcpy(err, "not a stage setting - map, texture_roughness, texture_metallic, texture_normals, "
                  "roughness_override, metalness_factor, specular_factor, bump_scale or emissive_factor", err_size);
        return false;
    }

    // nothing, or "" (a .mat line arrives with its quote marks), is none too -
    // as for the material's own texture_* (material.c MAT_TextureValueClears)
    bool none = !Q_stricmp(value, "none") || !strcmp(value, "-") || !value[0] || !strcmp(value, "\"\"");
    float number = 0;
    if (field < Q3SS_NUM_PATHS) {
        if (!strcmp(value, "0"))
            none = true;
        // as set_material_attribute: a path cut short names no file
        if (!none && strlen(value) >= MAX_IMAGE_PATH) {
            Q_snprintf(err, err_size, "the path is %u characters, and can be %d at most - shorten the file name",
                       (unsigned)strlen(value), MAX_IMAGE_PATH - 1);
            return false;
        }
    } else if (!none) {
        char *end;
        number = strtof(value, &end);
        if (end == value || *end) {
            Q_strlcpy(err, "not a number (or none)", err_size);
            return false;
        }
        if (field == Q3SS_ROUGHNESS_OVERRIDE && number < 0)
            none = true;
        // a half float on the GPU (write_stage)
        if (field == Q3SS_EMISSIVE_FACTOR && !(number >= 0.f && number <= 60000.f)) {
            Q_strlcpy(err, "from 0 up (0 or none: the stage glows only as the material does)", err_size);
            return false;
        }
    }

    q3override_t *o = find_override(scope, material, stage, true);
    const uint32_t bit = 1u << field;
    o->set |= bit;
    if (none) {
        o->none |= bit;
    } else {
        o->none &= ~bit;
        if (field < Q3SS_NUM_PATHS)
            Q_strlcpy(o->paths[field], value, sizeof(o->paths[field]));
        else
            o->values[field - Q3SS_NUM_PATHS] = number;
    }
    if (scope == Q3SS_SCOPE_CONSOLE)
        o->unsaved |= bit;
    return true;
}

// the material (name) is an instance of - "name#1a2b3c4d" -> "name" - or false
static bool instance_base_name(const char *name, char *out, size_t size)
{
    size_t len = strlen(name);
    if (len < 9 || name[len - 9] != '#')
        return false;
    for (size_t i = len - 8; i < len; i++) {
        const int c = Q_tolower(name[i]);
        if (!Q_isdigit(c) && (c < 'a' || c > 'f'))
            return false;
    }
    Q_strlcpy(out, name, min(size, len - 9 + 1));
    return true;
}

bool vkpt_q3_stage_settings(const char *material, int stage, q3stagesettings_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!material || stage < 0 || stage >= Q3P_MAX_STAGES || !num_overrides)
        return false;

    char base[MAX_QPATH];
    const bool instance = instance_base_name(material, base, sizeof(base));

    // most specific first: the material's own name, then the one it is an
    // instance of; within each, the console's, the map's, the global .mat's
    static const int scope_rank[3] = { 2, 1, 0 };   // by Q3SS_SCOPE_*
    const q3override_t *by_rank[6] = { NULL };
    for (int i = 0; i < num_overrides; i++) {
        const q3override_t *o = &overrides[i];
        if (o->stage != stage)
            continue;
        if (!Q_stricmp(o->material, material))
            by_rank[scope_rank[o->scope]] = o;
        else if (instance && !Q_stricmp(o->material, base))
            by_rank[3 + scope_rank[o->scope]] = o;
    }

    uint32_t decided = 0;
    for (int r = 0; r < 6; r++) {
        const q3override_t *o = by_rank[r];
        if (!o)
            continue;
        for (int f = 0; f < Q3SS_COUNT; f++) {
            const uint32_t bit = 1u << f;
            if (!(o->set & bit) || (decided & bit))
                continue;
            decided |= bit;
            if (o->none & bit)
                continue;
            out->set |= bit;
            if (f < Q3SS_NUM_PATHS)
                out->paths[f] = o->paths[f];
            else
                out->values[f - Q3SS_NUM_PATHS] = o->values[f - Q3SS_NUM_PATHS];
        }
    }
    return out->set != 0;
}

const char *vkpt_q3_stage_override(const char *material, int stage)
{
    q3stagesettings_t s;
    if (!vkpt_q3_stage_settings(material, stage, &s) || !(s.set & (1u << Q3SS_MAP)))
        return NULL;
    // "off" names no image: the stage is not drawn (vkpt_q3_stage_off)
    if (!Q_stricmp(s.paths[Q3SS_MAP], Q3SS_MAP_OFF))
        return NULL;
    return s.paths[Q3SS_MAP];
}

bool vkpt_q3_stage_off(const char *material, int stage)
{
    q3stagesettings_t s;
    return material && vkpt_q3_stage_settings(material, stage, &s) && (s.set & (1u << Q3SS_MAP))
        && !Q_stricmp(s.paths[Q3SS_MAP], Q3SS_MAP_OFF);
}

void vkpt_q3_stage_format_value(const q3stagesettings_t *s, int field, char *out, size_t size)
{
    if (field < 0 || field >= Q3SS_COUNT || !(s->set & (1u << field)))
        Q_strlcpy(out, field < Q3SS_NUM_PATHS ? "0" : field == Q3SS_ROUGHNESS_OVERRIDE ? "-1" : "none", size);
    else if (field < Q3SS_NUM_PATHS)
        Q_strlcpy(out, s->paths[field], size);
    else
        Q_snprintf(out, size, "%g", s->values[field - Q3SS_NUM_PATHS]);
}

// one record's setting as a .mat value, "none" included
static void format_override_value(const q3override_t *o, int field, char *out, size_t size)
{
    q3stagesettings_t s;
    memset(&s, 0, sizeof(s));
    if (!(o->none & (1u << field))) {
        s.set = 1u << field;
        if (field < Q3SS_NUM_PATHS)
            s.paths[field] = o->paths[field];
        else
            s.values[field - Q3SS_NUM_PATHS] = o->values[field - Q3SS_NUM_PATHS];
    }
    vkpt_q3_stage_format_value(&s, field, out, size);
}

bool vkpt_q3_stage_unsaved(const char *material)
{
    for (int i = 0; i < num_overrides; i++) {
        const q3override_t *o = &overrides[i];
        if (o->scope == Q3SS_SCOPE_CONSOLE && o->unsaved && !Q_stricmp(o->material, material))
            return true;
    }
    return false;
}

bool vkpt_q3_stage_unsaved_get(const char *material, int index, char *key, size_t key_size,
                               char *value, size_t value_size)
{
    for (int stage = 0; stage < Q3P_MAX_STAGES; stage++) {
        const q3override_t *o = find_override(Q3SS_SCOPE_CONSOLE, material, stage, false);
        if (!o || !o->unsaved)
            continue;
        for (int f = 0; f < Q3SS_COUNT; f++) {
            if (!(o->unsaved & (1u << f)) || index-- > 0)
                continue;
            Q_snprintf(key, key_size, "stage%d_%s", stage, stage_field_names[f]);
            format_override_value(o, f, value, value_size);
            return true;
        }
    }
    return false;
}

void vkpt_q3_stage_saved(const char *material, bool map_scope)
{
    char err[128], value[MAX_IMAGE_PATH];

    for (int stage = 0; stage < Q3P_MAX_STAGES; stage++) {
        q3override_t *o = find_override(Q3SS_SCOPE_CONSOLE, material, stage, false);
        if (!o || !o->unsaved)
            continue;
        // into the file's own scope too, the way the next 'mat reload' reads it
        for (int f = 0; f < Q3SS_COUNT; f++) {
            if (!(o->unsaved & (1u << f)))
                continue;
            format_override_value(o, f, value, sizeof(value));
            vkpt_q3_stage_setting_add(map_scope ? Q3SS_SCOPE_MAP : Q3SS_SCOPE_GLOBAL, material, stage, f,
                                      value, err, sizeof(err));
        }
        // the add may have moved the table
        o = find_override(Q3SS_SCOPE_CONSOLE, material, stage, false);
        if (o)
            o->unsaved = 0;
    }
}

/*
===============================================================================

BUILDING

===============================================================================
*/

static bool has_lightmap_stage(const q3shader_t *q3)
{
    for (int i = 0; i < q3->num_stages; i++)
        if (q3->stages[i].lightmap)
            return true;
    return false;
}

// A $lightmap multiplied in somewhere after stage `after`.
static bool lightmap_filter_after(const q3shader_t *q3, int after)
{
    for (int i = after + 1; i < q3->num_stages; i++)
        if (q3->stages[i].lightmap && q3->stages[i].blend == Q3BLEND_FILTER)
            return true;
    return false;
}

/* An environment-mapped stage on a lightmapped surface that nothing lights:
   added on top after the light was applied, Quake III's stand-in for a shine.
   The path tracer reflects for real, so the stage is not drawn. Before a
   lightmap multiply it is part of the surface colour and stays. */
static bool env_is_highlight(const q3shader_t *q3, int i)
{
    const q3stage_t *st = &q3->stages[i];
    return st->env_map && !st->lightmap && has_lightmap_stage(q3) && !lightmap_filter_after(q3, i);
}

/* A reflection map whose image moves over time - it scrolls, spins, stretches
   or wobbles - is an effect, not Quake III's stand-in for a shine: the orb of
   lightning over q3dm0's console, the CTF banners' shimmer. pt_q3_env_shine
   leaves it drawn as it is. */
static bool env_stage_moves(const q3stage_t *st)
{
    for (int i = 0; i < st->num_tcmods; i++) {
        switch (st->tcmods[i].type) {
        case Q3TCMOD_SCROLL:
        case Q3TCMOD_ROTATE:
        case Q3TCMOD_STRETCH:
        case Q3TCMOD_TURB:
            return true;
        default:
            break;
        }
    }
    return false;
}

static bool name_has_any(const char *name, const char *const *words)
{
    for (int i = 0; words[i]; i++)
        if (Q_stristr(name, words[i]))
            return true;
    return false;
}

/* pt_q3_env_shine: is the shiny part of this shader metal? Quake III played metal
   footsteps on it (surfaceparm metalsteps), or the shader or its reflection map
   is named for a metal ("pewter_shiney", "chrome_env", "tinfx"). A screen, a
   banner, glass, cloth or blood is not, whatever it reflects. */
static bool shine_is_metal(const q3shader_t *q3)
{
    static const char *const not_metal[] = {
        "comp", "screen", "banner", "medal", "glass", "flag", "blood", "tesla", "concrete", NULL
    };
    static const char *const metal[] = {
        "metal", "chrome", "pewter", "tin", "zinc", "steel", "iron", "support", "grate", "grill",
        "door", "pipe", "clang", "brass", "copper", "bronze", "gold", NULL
    };

    if (name_has_any(q3->name, not_metal))
        return false;
    if (q3->surface_flags & Q3_SURF_METALSTEPS)
        return true;
    if (name_has_any(q3->name, metal))
        return true;
    for (int i = 0; i < q3->num_stages; i++) {
        const q3stage_t *st = &q3->stages[i];
        if (st->env_map && !st->lightmap && name_has_any(st->map, metal))
            return true;
    }
    return false;
}

bool vkpt_q3_wants_program(const q3shader_t *q3)
{
    if (!q3 || q3->sky || q3->portal || q3->no_draw || q3->chrome)
        return false;
    if (q3->content_flags & (Q3_CONTENTS_WATER | Q3_CONTENTS_SLIME))
        return false;
    if (q3->num_stages < 1)
        return false;

    // drawn from several images: a glow pulsing over a trim, text over a screen
    int images = 0;
    for (int i = 0; i < q3->num_stages; i++) {
        const q3stage_t *st = &q3->stages[i];
        if (st->lightmap || st->white || st->video || !st->map[0])
            continue;
        if (env_is_highlight(q3, i))
            continue;
        images++;
    }

    return q3->animated || images >= 2;
}

static image_t *load_stage_image(const char *path, imagetype_t type, imageflags_t flags)
{
    char resolved[MAX_IMAGE_PATH];

    if (!path || !path[0] || path[0] == '$' || path[0] == '*')
        return NULL;

    // an override names its file exactly; a script's ".tga" may be a .jpg
    if (strchr(path, '.') && FS_FileExists(path))
        Q_strlcpy(resolved, path, sizeof(resolved));
    else if (!Q3Shader_ResolveImage(path, resolved, sizeof(resolved)))
        return NULL;

    // the file itself, not an image already loaded from its .jpg/.png twin - so a
    // stage<N>_map switched to a .png beside id's .jpg shows on 'mat reload'
    image_t *image = IMG_FindFile(resolved, type, flags);
    return image == R_NOTEXTURE ? NULL : image;
}

/* Whether anything of the program is left with no light at the surface - C00 in
   q3_stages.glsl's terms - judged from the blends alone. A stage the light
   multiplies (the lightmap, or a lit colour) shows nothing then, and wipes out
   what is under it unless it keeps the destination; any other stage that brings
   its own colour leaves something. */
static bool program_emits(const q3program_t *p)
{
    bool emits = false;

    for (int s = 0; s < p->num_stages; s++) {
        const q3pstage_t *ps = &p->stages[s];
        if (ps->flags & (Q3P_STAGE_HIGHLIGHT | Q3P_STAGE_NO_IMAGE))
            continue;

        if (ps->flags & (Q3P_STAGE_LIGHTMAP | Q3P_STAGE_LIT)) {
            // black x anything, plus what was there x the destination factor
            if (ps->dst_blend == Q3BF_ZERO || ps->dst_blend == Q3BF_SRC_COLOR)
                emits = false;
        } else if (ps->src_blend != Q3BF_ZERO && ps->src_blend != Q3BF_DST_COLOR) {
            emits = true;
        }
    }

    return emits;
}

// the same picture: files of one name, in any directory, of any format -
// materialDev/textures/sfx/proto_zzztpink.jpg is textures/sfx/proto_zzztpink.tga
static bool same_picture(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0])
        return false;

    const char *na = COM_SkipPath(a), *nb = COM_SkipPath(b);
    const char *da = strrchr(na, '.'), *db = strrchr(nb, '.');
    const size_t la = da ? (size_t)(da - na) : strlen(na);
    const size_t lb = db ? (size_t)(db - nb) : strlen(nb);
    return la > 0 && la == lb && !Q_strncasecmp(na, nb, la);
}

/* The stage of `q3` whose own picture `image` is - its map, a frame of its
   animMap, or what a stage<N>_map line puts there - or -1. Not the lightmap,
   $whiteimage or a reflection map. */
static int stage_named_by(const pbr_material_t *mat, const q3shader_t *q3, const image_t *image)
{
    for (int i = 0; i < q3->num_stages && i < Q3P_MAX_STAGES; i++) {
        const q3stage_t *st = &q3->stages[i];
        if (st->lightmap || st->white || st->env_map || st->video)
            continue;
        if (same_picture(vkpt_q3_stage_override(mat->name, i), image->name) ||
            same_picture(st->map, image->name))
            return i;
        for (int f = 0; f < st->num_frames && st->frames; f++)
            if (same_picture(st->frames[f], image->name))
                return i;
    }
    return -1;
}

/* A stage's own surface (STAGE SETTINGS, q3stages.h): what the .mat's
   stage<N>_* lines give it, for q3_stages.glsl. Its maps are data, not colour. */
static void stage_surface(q3program_t *p, q3pstage_t *ps, const char *material, int index,
                          imagetype_t type, imageflags_t flags)
{
    static const uint32_t map_flags[3] = { Q3P_PROP_ROUGH_MAP, Q3P_PROP_METAL_MAP, Q3P_PROP_NORMAL_MAP };
    static const uint32_t value_flags[4] = { Q3P_PROP_ROUGH, Q3P_PROP_METAL, Q3P_PROP_SPEC, Q3P_PROP_BUMP };
    q3stagesettings_t s;

    if (!vkpt_q3_stage_settings(material, index, &s))
        return;

    // its own glow (q3_stages.glsl, vkpt_q3_light_glow)
    if ((s.set & (1u << Q3SS_EMISSIVE_FACTOR)) && s.values[Q3SS_EMISSIVE_FACTOR - Q3SS_NUM_PATHS] > 0.f &&
        !(ps->flags & (Q3P_STAGE_NO_IMAGE | Q3P_STAGE_HIGHLIGHT))) {
        ps->glow_factor = s.values[Q3SS_EMISSIVE_FACTOR - Q3SS_NUM_PATHS];
        p->flags |= Q3P_PROG_STAGE_EMIT;
    }

    for (int m = 0; m < 3; m++) {
        const int field = Q3SS_TEXTURE_ROUGHNESS + m;
        if (!(s.set & (1u << field)))
            continue;
        ps->prop_images[m] = load_stage_image(s.paths[field], type, flags & ~IF_SRGB);
        if (ps->prop_images[m])
            ps->prop_flags |= map_flags[m];
        else
            Com_WPrintf("%s: stage%d_%s %s: not found\n", material, index,
                        vkpt_q3_stage_field_name(field), s.paths[field]);
    }
    for (int v = 0; v < 4; v++) {
        if (s.set & (1u << (Q3SS_ROUGHNESS_OVERRIDE + v))) {
            ps->prop_flags |= value_flags[v];
            ps->prop_values[v] = s.values[v];
        }
    }
    // the bump scale alone says nothing about the surface
    if ((ps->prop_flags & ~Q3P_PROP_BUMP) == 0)
        ps->prop_flags = 0;

    if (ps->prop_flags)
        p->has_props = true;
}

static q3program_t *alloc_program(int owner)
{
    for (int i = 0; i < MAX_Q3_PROGRAMS; i++) {
        q3program_t *p = &programs[i];
        if (p->in_use) {
            // still the program of the live material that built it?
            const pbr_material_t *m = MAT_ForIndex(p->owner);
            if (m && m->q3_program == i + 1)
                continue;
        }
        memset(p, 0, sizeof(*p));
        p->in_use = true;
        p->owner = owner;
        return p;
    }

    Com_WPrintf("Quake III shaders: more than %d animated or layered materials; the rest draw still.\n",
                MAX_Q3_PROGRAMS);
    return NULL;
}

void vkpt_q3_build_program(pbr_material_t *mat, const q3shader_t *q3, imagetype_t type, imageflags_t flags,
                           bool own_base, bool own_emissive)
{
    if (!vkpt_q3_wants_program(q3)) {
        mat->q3_program = 0;
        return;
    }

    // only the live materials have programs, never a definition being parsed
    const int index = MAT_IndexOf(mat);
    if (index < 0) {
        mat->q3_program = 0;
        return;
    }

    q3program_t *p = NULL;
    if (mat->q3_program && mat->q3_program <= MAX_Q3_PROGRAMS &&
        programs[mat->q3_program - 1].in_use && programs[mat->q3_program - 1].owner == index) {
        // rebuilding in place, for "mat reload"
        p = &programs[mat->q3_program - 1];
        memset(p, 0, sizeof(*p));
        p->in_use = true;
        p->owner = index;
    } else {
        p = alloc_program(index);
    }
    if (!p) {
        mat->q3_program = 0;
        return;
    }

    const bool lightmapped = has_lightmap_stage(q3);
    bool any_lighting = lightmapped;
    for (int i = 0; i < q3->num_stages; i++)
        if (q3->stages[i].rgbgen == Q3RGB_LIGHTING)
            any_lighting = true;

    if (q3->translucent)
        p->flags |= Q3P_PROG_TRANSLUCENT;
    // Nothing in the shader brings light in: Quake III drew it full bright,
    // which in a path tracer would make every pixel of it a lamp. Its colour
    // stages are lit like any surface instead; what it adds still glows.
    if (!any_lighting)
        p->flags |= Q3P_PROG_IMPLICIT_LIT;

    p->shine_metal = shine_is_metal(q3);

    // the stage the entry's texture_emissive is the picture of (THE ENTRY'S
    // GLOW, below), which it then never replaces
    const int named_stage = (own_emissive && mat->image_emissive)
        ? stage_named_by(mat, q3, mat->image_emissive) : -1;

    for (int i = 0; i < q3->num_stages && p->num_stages < Q3P_MAX_STAGES; i++) {
        const q3stage_t *st = &q3->stages[i];
        q3pstage_t *ps = &p->stages[p->num_stages++];

        /* "stage<N>_map off": not drawn at all. The slot stays, so the stages
           keep their numbers and their stage<N>_* settings. Matt's slamp2 lamp
           shade (models/mapobjects/slamp/slamp2, 2026-11-17): he wanted its
           reflection map (envmapdimb) gone so the glass reflects for real, and
           "stage1_map 0" only took the entry's image away - the script's came
           back. An env stage turned off is no shine either (has_env). */
        if (vkpt_q3_stage_off(mat->name, i)) {
            ps->src_blend = Q3BF_ONE;   // adds nothing; never read, NO_IMAGE skips it
            ps->dst_blend = Q3BF_ONE;
            ps->flags |= Q3P_STAGE_NO_IMAGE;
            p->off |= 1u << i;
            continue;
        }

        ps->src_blend = st->src_blend;
        ps->dst_blend = st->dst_blend;
        if (ps->src_blend == Q3BF_NONE || ps->dst_blend == Q3BF_NONE) {
            ps->src_blend = Q3BF_ONE;
            ps->dst_blend = Q3BF_ZERO;
        }
        const bool adds = (ps->dst_blend == Q3BF_ONE);

        ps->alpha_func = st->alpha_test ? st->alpha_func : Q3ALPHA_NONE;
        ps->rgbgen = st->rgbgen;
        VectorCopy(st->rgb_const, ps->rgb_const);
        ps->rgb_wave = st->rgb_wave;
        ps->alphagen = st->alphagen;
        ps->alpha_const = st->alpha_const;
        ps->alpha_wave = st->alpha_wave;
        ps->num_tcmods = st->num_tcmods;
        memcpy(ps->tcmods, st->tcmods, sizeof(ps->tcmods));
        ps->anim_freq = st->anim_freq;

        if (st->lightmap) {
            ps->flags |= Q3P_STAGE_LIGHTMAP;
            // the lightmap as a reflection (tcGen environment): Quake III
            // reflecting its own light, a shine
            if (st->env_map) {
                ps->flags |= Q3P_STAGE_ENV;
                if (env_stage_moves(st))
                    ps->flags |= Q3P_STAGE_ENV_EFFECT;
                else
                    p->has_env = true;
            }
            // the lightmap ADDED (as a reflection, or over a glow): a fake shine
            if (adds)
                ps->flags |= Q3P_STAGE_NO_IMAGE | (st->env_map ? Q3P_STAGE_HIGHLIGHT : 0);
            continue;
        }
        if (st->white) {
            ps->flags |= Q3P_STAGE_WHITE;
        } else {
            /* A stage<N>_map whose file is missing leaves the stage as it would
               be without the line: the script's own image. quake3.mat names
               copies in materialDev/ for every stage, and only the edited ones
               are in git - the rest are the pak's own pictures anyway. */
            const char *path = vkpt_q3_stage_override(mat->name, i);
            image_t *override = path ? load_stage_image(path, type, flags | IF_SRGB) : NULL;
            if (path && !override)
                Com_DPrintf("%s: stage%d_map %s not found, drawing the script's image\n",
                            mat->name, i, path);

            // the material's own base or glow, when its .mat names one, is the
            // image of the stage that supplied it
            if (override) {
                ps->images[0] = override;
                ps->num_images = 1;
            } else if (i == q3->diffuse_stage && own_base && mat->image_base) {
                ps->images[0] = mat->image_base;
                ps->num_images = 1;
            } else if (i == q3->emissive_stage && own_emissive && mat->image_emissive &&
                       (named_stage < 0 || named_stage == i)) {
                ps->images[0] = mat->image_emissive;
                ps->num_images = 1;
            } else if (st->num_frames > 1 && st->frames) {
                for (int f = 0; f < st->num_frames && f < Q3S_MAX_ANIMFRAMES; f++) {
                    ps->images[f] = load_stage_image(st->frames[f], type, flags | IF_SRGB);
                    ps->num_images = f + 1;
                }
            } else if (!st->video) {
                ps->images[0] = load_stage_image(st->map, type, flags | IF_SRGB);
                ps->num_images = ps->images[0] ? 1 : 0;
            }

            if (!ps->num_images || !ps->images[0])
                ps->flags |= Q3P_STAGE_NO_IMAGE;
        }

        if (st->env_map) {
            ps->flags |= Q3P_STAGE_ENV;
            if (env_stage_moves(st))
                ps->flags |= Q3P_STAGE_ENV_EFFECT;
            else
                p->has_env = true;
            // never drawn; under pt_q3_env_shine it still says where the gloss is
            if (env_is_highlight(q3, i))
                ps->flags |= Q3P_STAGE_HIGHLIGHT;
        }
        if (st->clamp)
            ps->flags |= Q3P_STAGE_CLAMP;

        // rgbGen vertex on a glow is how a misc_model's glow was coloured by
        // its baked vertex light; the glow itself is what matters here
        if (!adds) {
            if (st->rgbgen == Q3RGB_LIGHTING)
                ps->flags |= Q3P_STAGE_LIT;
            else if (st->rgbgen == Q3RGB_ONE_MINUS_LIGHTING)
                ps->flags |= Q3P_STAGE_ONE_MINUS_LIT;
            else if ((p->flags & Q3P_PROG_IMPLICIT_LIT) && !(ps->flags & Q3P_STAGE_WHITE))
                ps->flags |= Q3P_STAGE_LIT;
        }

        stage_surface(p, ps, mat->name, i, type, flags);
    }

    /* THE ENTRY'S GLOW: what the .mat entry's texture_emissive means for a
       program.
       - The picture of one of its stages (stage_named_by: the same file name, in
         any directory, any frame of an animMap): THAT stage glows - what shows
         of it through the layers over it, fully lit, is emitted, and moves as
         the stage does (Q3P_STAGE_GLOW, q3_stages.glsl).
         textures/base_floor/techfloor (q3dm0) is pink lightning scrolling under
         two metal layers, all of it under the lightmap; Matt named the
         lightning, proto_zzztpink. medal_impressive's glow stage is the
         scrolling text (comp3textd) over the medals; he wanted the MEDAL to be
         what glows (2026-11-16) - medal_impressive.jpg lights all six frames.
       - Any other picture replaces the shader's glow stage (the stage loop
         above: shinymetaldoor_outside3a2_light.png) - unless a stage<N>_map line
         already does - or, with no glow stage, is added over the whole program
         as a glow, the way a Quake II texture's _light map is. */
    if (named_stage >= 0 && named_stage != q3->emissive_stage && named_stage < p->num_stages) {
        q3pstage_t *ps = &p->stages[named_stage];
        if (!(ps->flags & Q3P_STAGE_NO_IMAGE)) {
            ps->flags |= Q3P_STAGE_GLOW;
            p->flags |= Q3P_PROG_GLOW;
        }
    } else if (own_emissive && mat->image_emissive && named_stage < 0 && q3->emissive_stage < 0) {
        if (p->num_stages < Q3P_MAX_STAGES) {
            q3pstage_t *ps = &p->stages[p->num_stages++];
            memset(ps, 0, sizeof(*ps));     // rgbGen and alphaGen identity, no tcMods
            ps->images[0] = mat->image_emissive;
            ps->num_images = 1;
            ps->src_blend = Q3BF_ONE;
            ps->dst_blend = Q3BF_ONE;
            ps->alpha_func = Q3ALPHA_NONE;
        } else {
            Com_WPrintf("%s: texture_emissive %s: the shader has %d stages, no room to add it as a glow\n",
                        mat->name, mat->image_emissive->name, Q3P_MAX_STAGES);
        }
    }

    /* A shader the author marked q3map_surfacelight - "this lights the room" -
       whose every stage the light multiplies: the medal screens (textures/
       base_wall/medal_impressive, a $lightmap filter over all of it), the
       computer screens and banners, and lava, which Quake III drew full bright
       with no lightmap at all. With no light at the surface it is black, so it
       emitted nothing: emissive_factor and default_radiance scaled zero, and
       its light polys were dark. Quake III showed it by the light baked onto
       it. It glows with its colour as fully lit instead, at default_radiance
       (q3_stages.glsl, vkpt_q3_light_glow) - unless the entry chose a stage to
       glow (THE ENTRY'S GLOW above): then only that stage does. */
    if (q3->surface_light > 0 && !program_emits(p) && !(p->flags & Q3P_PROG_GLOW))
        p->flags |= Q3P_PROG_SELF_LIT;

    /* For the glow as a light. A tcMod turb only wobbles the image in place,
       and the light leaves it out (vkpt_q3_light_glow), so it moves nothing. */
    int glows = 0;
    for (int s = 0; s < p->num_stages; s++) {
        const q3pstage_t *ps = &p->stages[s];
        if (ps->flags & (Q3P_STAGE_LIGHTMAP | Q3P_STAGE_HIGHLIGHT | Q3P_STAGE_NO_IMAGE))
            continue;

        // a reflection map counts as its average (vkpt_q3_light_glow), which
        // its tcMods do not move
        bool moves = ps->num_images > 1 && ps->anim_freq > 0;
        for (int t = 0; t < ps->num_tcmods && !(ps->flags & Q3P_STAGE_ENV); t++)
            if (ps->tcmods[t].type == Q3TCMOD_SCROLL || ps->tcmods[t].type == Q3TCMOD_ROTATE ||
                ps->tcmods[t].type == Q3TCMOD_STRETCH)
                moves = true;
        if (moves)
            p->glow_moves = true;
        if (moves || ps->rgbgen == Q3RGB_WAVE || ps->alphagen == Q3ALPHAGEN_WAVE)
            p->varies = true;
        if (ps->dst_blend == Q3BF_ONE && !(ps->flags & (Q3P_STAGE_ENV | Q3P_STAGE_WHITE)))
            glows++;
    }
    if (glows > 1)
        p->glow_moves = true;

    if (++program_builds == 0)
        program_builds = 1;
    p->build = program_builds;

    mat->q3_program = (uint16_t)(p - programs + 1);
}

/* pt_q3_env_shine: a reflection map standing in for a shine - not on a
   see-through program, where the reflection map is the effect itself, drawn
   over what is behind it (q3dm0's floating logo is a sphere of two spinning
   reflection maps, which as gloss went flat cyan). */
static bool program_shines(const q3program_t *p)
{
    return p->has_env && !(p->flags & Q3P_PROG_TRANSLUCENT);
}

bool vkpt_q3_program_shines(const pbr_material_t *mat, bool *metal)
{
    *metal = false;
    if (!mat || !mat->q3_program || mat->q3_program > MAX_Q3_PROGRAMS)
        return false;

    const q3program_t *p = &programs[mat->q3_program - 1];
    if (!p->in_use || !program_shines(p))
        return false;

    *metal = p->shine_metal;
    return true;
}

void vkpt_q3_touch_program(const pbr_material_t *mat)
{
    if (!mat || !mat->q3_program || mat->q3_program > MAX_Q3_PROGRAMS)
        return;

    q3program_t *p = &programs[mat->q3_program - 1];
    if (!p->in_use)
        return;

    for (int s = 0; s < p->num_stages; s++) {
        for (int f = 0; f < p->stages[s].num_images; f++)
            if (p->stages[s].images[f])
                p->stages[s].images[f]->registration_sequence = registration_sequence;
        for (int m = 0; m < 3; m++)
            if (p->stages[s].prop_images[m])
                p->stages[s].prop_images[m]->registration_sequence = registration_sequence;
    }
}

/*
===============================================================================

EVALUATING

A texture-coordinate transform is kept as s' = a s + b t + e, t' = c s + d t + f,
stored { a, b, c, d, e, f }. Quake III applies its tcMods in the order the
script lists them; everything before a turb folds into the first matrix and
everything after it into the second, and the turb - which depends on where on
the surface the pixel is - runs between them on the GPU.

===============================================================================
*/

static void affine_identity(float m[6])
{
    m[0] = 1; m[1] = 0; m[2] = 0; m[3] = 1; m[4] = 0; m[5] = 0;
}

// m = n after m
static void affine_then(float m[6], const float n[6])
{
    float r[6];
    r[0] = n[0] * m[0] + n[1] * m[2];
    r[1] = n[0] * m[1] + n[1] * m[3];
    r[2] = n[2] * m[0] + n[3] * m[2];
    r[3] = n[2] * m[1] + n[3] * m[3];
    r[4] = n[0] * m[4] + n[1] * m[5] + n[4];
    r[5] = n[2] * m[4] + n[3] * m[5] + n[5];
    memcpy(m, r, sizeof(r));
}

static float clamp01(float x)
{
    return x < 0.f ? 0.f : x > 1.f ? 1.f : x;
}

static float frac(float x)
{
    return x - floorf(x);
}

static void eval_tcmods(const q3pstage_t *ps, float time, float m1[6], float m2[6],
                        float *turb_amp, float *turb_now)
{
    bool after_turb = false;

    affine_identity(m1);
    affine_identity(m2);
    *turb_amp = 0;
    *turb_now = 0;

    for (int i = 0; i < ps->num_tcmods; i++) {
        const q3tcmod_t *mod = &ps->tcmods[i];
        float n[6];
        affine_identity(n);

        switch (mod->type) {
        case Q3TCMOD_SCROLL:
            // wrapped, as Quake III does, so the numbers stay small
            n[4] = frac(mod->v[0] * time);
            n[5] = frac(mod->v[1] * time);
            break;
        case Q3TCMOD_SCALE:
            n[0] = mod->v[0];
            n[3] = mod->v[1];
            break;
        case Q3TCMOD_ROTATE: {
            // about the middle of the texture (RB_CalcRotateTexCoords)
            float degs = -mod->v[0] * time;
            degs -= 360.f * floorf(degs / 360.f);
            float rad = DEG2RAD(degs);
            float s = sinf(rad), c = cosf(rad);
            n[0] = c;  n[1] = -s;
            n[2] = s;  n[3] = c;
            n[4] = 0.5f - 0.5f * c + 0.5f * s;
            n[5] = 0.5f - 0.5f * s - 0.5f * c;
            break;
        }
        case Q3TCMOD_STRETCH: {
            float w = Q3Shader_EvalWave(&mod->wave, time);
            float p = fabsf(w) > 1e-4f ? 1.f / w : 1.f;
            n[0] = p;
            n[3] = p;
            n[4] = 0.5f - 0.5f * p;
            n[5] = 0.5f - 0.5f * p;
            break;
        }
        case Q3TCMOD_TRANSFORM:
            // "m00 m01 m10 m11 t0 t1": s' = s m00 + t m10 + t0, t' = s m01 + t m11 + t1
            n[0] = mod->v[0];
            n[1] = mod->v[2];
            n[2] = mod->v[1];
            n[3] = mod->v[3];
            n[4] = mod->v[4];
            n[5] = mod->v[5];
            break;
        case Q3TCMOD_TURB:
            if (!after_turb) {
                *turb_amp = mod->wave.amp;
                *turb_now = frac(mod->wave.phase + time * mod->wave.freq);
                after_turb = true;
            }
            continue;
        default:
            continue;
        }

        affine_then(after_turb ? m2 : m1, n);
    }
}

static uint32_t float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static void put_uvec4(vec4 *dst, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
    uint32_t v[4] = { x, y, z, w };
    memcpy(dst, v, sizeof(v));
}

static void put_vec4(vec4 *dst, float x, float y, float z, float w)
{
    float v[4] = { x, y, z, w };
    memcpy(dst, v, sizeof(v));
}

// the animMap frame showing at `time`, NULL when it did not load
static const image_t *stage_frame(const q3pstage_t *ps, float time)
{
    int frame = 0;

    if (ps->num_images <= 0)
        return NULL;
    if (ps->num_images > 1 && ps->anim_freq > 0) {
        frame = (int)floorf(time * ps->anim_freq) % ps->num_images;
        if (frame < 0)
            frame += ps->num_images;
    }
    return ps->images[frame];
}

// rgbGen and alphaGen at `time`; what they leave to the lighting is 1
static void stage_colour(const q3pstage_t *ps, float time, float out[4])
{
    vec3_t rgb = { 1, 1, 1 };
    switch (ps->rgbgen) {
    case Q3RGB_CONST:
        VectorCopy(ps->rgb_const, rgb);
        break;
    case Q3RGB_WAVE: {
        float g = Q3Shader_EvalWave(&ps->rgb_wave, time);
        g = clamp01(g);
        VectorSet(rgb, g, g, g);
        break;
    }
    default:
        break;
    }

    float alpha = 1.f;
    switch (ps->alphagen) {
    case Q3ALPHAGEN_CONST:
        alpha = ps->alpha_const;
        break;
    case Q3ALPHAGEN_WAVE:
        alpha = clamp01(Q3Shader_EvalWave(&ps->alpha_wave, time));
        break;
    case Q3ALPHAGEN_ZERO:
        alpha = 0.f;
        break;
    default:
        break;
    }

    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    out[3] = alpha;
}

static void write_stage(vec4 *out, const q3pstage_t *ps, float time)
{
    uint32_t flags = ps->flags;
    uint32_t image_index = 0;

    if (!(flags & (Q3P_STAGE_NO_IMAGE | Q3P_STAGE_LIGHTMAP | Q3P_STAGE_WHITE)) && ps->num_images > 0) {
        const image_t *image = stage_frame(ps, time);
        if (image)
            image_index = (uint32_t)(image - r_images);
        else
            flags |= Q3P_STAGE_NO_IMAGE;
    }

    float m1[6], m2[6], turb_amp, turb_now;
    eval_tcmods(ps, time, m1, m2, &turb_amp, &turb_now);
    if (turb_amp != 0)
        flags |= Q3P_STAGE_TURB;

    float colour[4];
    stage_colour(ps, time, colour);

    put_uvec4(&out[0], (image_index & 0xffff) | (flags << 16),
              (uint32_t)ps->src_blend | ((uint32_t)ps->dst_blend << 4) | ((uint32_t)ps->alpha_func << 8) |
              ((uint32_t)floatToHalf(ps->glow_factor) << 16),
              float_bits(turb_amp), float_bits(turb_now));
    put_vec4(&out[1], m1[0], m1[1], m1[2], m1[3]);
    put_vec4(&out[2], m1[4], m1[5], m2[0], m2[1]);
    put_vec4(&out[3], m2[2], m2[3], m2[4], m2[5]);
    put_vec4(&out[4], colour[0], colour[1], colour[2], colour[3]);
}

// a stage's own surface, Q3P_PROP_VEC4S after the stages (Q3P_PROG_PROPS)
static void write_stage_props(vec4 *out, const q3pstage_t *ps)
{
    uint32_t index[3];
    for (int m = 0; m < 3; m++)
        index[m] = ps->prop_images[m] ? (uint32_t)(ps->prop_images[m] - r_images) & 0xffff : 0;

    put_uvec4(&out[0], index[0] | (index[1] << 16), index[2] | (ps->prop_flags << 16), 0, 0);
    put_vec4(&out[1], ps->prop_values[0], ps->prop_values[1], ps->prop_values[2],
             (ps->prop_flags & Q3P_PROP_BUMP) ? ps->prop_values[3] : 1.f);
}

static cvar_t *cvar_pt_q3_env_shine;
// the clock the last vkpt_q3_upload drew at, for 'mat print' (vkpt_q3_print_program)
static float last_upload_time;

void vkpt_q3_upload(vec4 *dst, float time)
{
    uint32_t used = 0;
    static bool warned;

    // 1 = a reflection-map stage (tcGen environment: Quake III's painted-on
    // shine) is not drawn; where it showed, the surface is glossy - and metal,
    // when the shader says so - and reflects the real scene. 0 = drawn as
    // Quake III drew it, an image that follows the view. Live.
    if (!cvar_pt_q3_env_shine)
        cvar_pt_q3_env_shine = Cvar_Get("pt_q3_env_shine", "1", 0);
    const bool shine = cvar_pt_q3_env_shine->integer != 0;

    // Quake III's shader clock: the tables repeat, so only its phase matters,
    // but floats lose the fraction after a long session - keep it under a day
    time = fmodf(time, 86400.f);
    last_upload_time = time;

    for (int i = 0; i < MAX_Q3_PROGRAMS; i++) {
        q3program_t *p = &programs[i];
        p->offset = 0;

        if (!p->in_use)
            continue;
        const pbr_material_t *m = MAT_ForIndex(p->owner);
        if (!m || m->q3_program != i + 1) {
            p->in_use = false;
            continue;
        }

        const uint32_t stages_size = 1 + (uint32_t)p->num_stages * Q3P_STAGE_VEC4S;
        const uint32_t size = stages_size + (p->has_props ? (uint32_t)p->num_stages * Q3P_PROP_VEC4S : 0);
        if (used + size > MAX_Q3_STAGE_VEC4S) {
            if (!warned) {
                Com_WPrintf("Quake III shaders: the stage table is full (%d vec4s); some surfaces draw still.\n",
                            MAX_Q3_STAGE_VEC4S);
                warned = true;
            }
            continue;
        }

        vec4 *out = dst + used;
        uint32_t header = (uint32_t)p->num_stages | p->flags;
        if (shine && program_shines(p))
            header |= Q3P_PROG_SHINE | (p->shine_metal ? Q3P_PROG_SHINE_METAL : 0);
        if (p->has_props)
            header |= Q3P_PROG_PROPS;
        put_uvec4(&out[0], header, 0, 0, 0);
        for (int s = 0; s < p->num_stages; s++)
            write_stage(out + 1 + s * Q3P_STAGE_VEC4S, &p->stages[s], time);
        for (int s = 0; p->has_props && s < p->num_stages; s++)
            write_stage_props(out + stages_size + s * Q3P_PROP_VEC4S, &p->stages[s]);

        p->offset = used + 1;
        used += size;
    }
}

uint32_t vkpt_q3_program_offset(const pbr_material_t *mat)
{
    if (!mat || !mat->q3_program || mat->q3_program > MAX_Q3_PROGRAMS)
        return 0;

    const q3program_t *p = &programs[mat->q3_program - 1];
    return p->in_use ? p->offset : 0;
}

/* 'mat print': the program as the GPU gets it - each stage's image (or that it
   has none and is skipped), its blend, its colour now and its flags. A surface
   that draws wrong is then read off the console rather than guessed at: a stage
   showing NO IMAGE is one the compositing leaves out, which for the colour stage
   is a black surface (Matt's q3dm1 disc and q3dm0 fixtures, 2026-11-16). */
void vkpt_q3_print_program(const pbr_material_t *mat)
{
    static const char *const blends[] = {
        "-", "ONE", "ZERO", "DST_COLOR", "1-DST_COLOR", "SRC_ALPHA", "1-SRC_ALPHA",
        "DST_ALPHA", "1-DST_ALPHA", "SRC_COLOR", "1-SRC_COLOR", "SRC_ALPHA_SAT",
    };
#define BLEND_NAME(b) ((b) >= 0 && (b) < (int)q_countof(blends) ? blends[b] : "?")

    if (!mat || !mat->q3_program) {
        Com_Printf("    q3 program: none (the images above are drawn as they are)\n");
        return;
    }
    if (mat->q3_program > MAX_Q3_PROGRAMS || !programs[mat->q3_program - 1].in_use) {
        Com_Printf("    q3 program: %u - NOT IN USE (drawn from the images above)\n", mat->q3_program);
        return;
    }

    const q3program_t *p = &programs[mat->q3_program - 1];
    Com_Printf("    q3 program: %d stages%s%s%s%s%s, %s\n", p->num_stages,
               (p->flags & Q3P_PROG_TRANSLUCENT) ? ", translucent" : "",
               (p->flags & Q3P_PROG_IMPLICIT_LIT) ? ", implicit lit" : "",
               (p->flags & Q3P_PROG_SELF_LIT) ? ", self lit" : "",
               (p->flags & Q3P_PROG_GLOW) ? ", entry glow" : "",
               (p->flags & Q3P_PROG_STAGE_EMIT) ? ", stage glow" : "",
               p->offset ? "in this frame's table" : "NOT IN THIS FRAME'S TABLE (drawn still)");

    for (int s = 0; s < p->num_stages; s++) {
        const q3pstage_t *ps = &p->stages[s];
        char what[MAX_IMAGE_PATH + 48];
        if (p->off & (1u << s)) {
            Com_Printf("      stage%d OFF (stage%d_map off) - not drawn\n", s, s);
            continue;
        }
        if (ps->flags & Q3P_STAGE_LIGHTMAP)
            Q_strlcpy(what, (ps->flags & Q3P_STAGE_NO_IMAGE) ? "$lightmap (added: not drawn)" : "$lightmap", sizeof(what));
        else if (ps->flags & Q3P_STAGE_WHITE)
            Q_strlcpy(what, "$whiteimage", sizeof(what));
        else if ((ps->flags & Q3P_STAGE_NO_IMAGE) || ps->num_images <= 0 || !ps->images[0])
            Q_strlcpy(what, "NO IMAGE - skipped", sizeof(what));
        else if (ps->num_images > 1)
            Q_snprintf(what, sizeof(what), "%s %dx%d (+%d frames)", ps->images[0]->name,
                       ps->images[0]->width, ps->images[0]->height, ps->num_images - 1);
        else
            Q_snprintf(what, sizeof(what), "%s %dx%d", ps->images[0]->name,
                       ps->images[0]->width, ps->images[0]->height);

        float colour[4];
        stage_colour(ps, last_upload_time, colour);

        Com_Printf("      stage%d %s\n", s, what);
        Com_Printf("        blend %s %s, colour %.2f %.2f %.2f a %.2f%s%s%s%s%s%s%s%s\n",
                   BLEND_NAME(ps->src_blend), BLEND_NAME(ps->dst_blend),
                   colour[0], colour[1], colour[2], colour[3],
                   ps->alpha_func ? ", alpha test" : "",
                   (ps->flags & Q3P_STAGE_LIT) ? ", lit" : "",
                   (ps->flags & Q3P_STAGE_ONE_MINUS_LIT) ? ", 1-lit" : "",
                   (ps->flags & Q3P_STAGE_ENV) ? ", reflection map" : "",
                   (ps->flags & Q3P_STAGE_HIGHLIGHT) ? " (highlight: not drawn)" : "",
                   (ps->flags & Q3P_STAGE_ENV_EFFECT) ? " (moving: drawn)" : "",
                   (ps->flags & Q3P_STAGE_CLAMP) ? ", clamped" : "",
                   (ps->flags & Q3P_STAGE_GLOW) ? ", the entry's glow" : "");

        // its own surface (stage<N>_* settings), where it covers the material's
        if (ps->prop_flags) {
            char line[MAX_IMAGE_PATH * 3 + 160] = "";
            static const char *const map_names[3] = { "texture_roughness", "texture_metallic", "texture_normals" };
            static const uint32_t map_flags[3] = { Q3P_PROP_ROUGH_MAP, Q3P_PROP_METAL_MAP, Q3P_PROP_NORMAL_MAP };
            static const char *const value_names[4] = { "roughness_override", "metalness_factor", "specular_factor", "bump_scale" };
            static const uint32_t value_flags[4] = { Q3P_PROP_ROUGH, Q3P_PROP_METAL, Q3P_PROP_SPEC, Q3P_PROP_BUMP };
            for (int v = 0; v < 4; v++) {
                if (!(ps->prop_flags & value_flags[v]))
                    continue;
                char item[64];
                Q_snprintf(item, sizeof(item), "%s%s %g", line[0] ? ", " : "", value_names[v], ps->prop_values[v]);
                Q_strlcat(line, item, sizeof(line));
            }
            for (int m = 0; m < 3; m++) {
                if (!(ps->prop_flags & map_flags[m]) || !ps->prop_images[m])
                    continue;
                Q_strlcat(line, line[0] ? ", " : "", sizeof(line));
                Q_strlcat(line, map_names[m], sizeof(line));
                Q_strlcat(line, " ", sizeof(line));
                Q_strlcat(line, ps->prop_images[m]->name, sizeof(line));
            }
            Com_Printf("        own surface: %s\n", line);
        }
        if (ps->glow_factor > 0.f)
            Com_Printf("        own glow: emissive_factor %g (added, not scaled by the material's)\n", ps->glow_factor);
        else if ((ps->flags & Q3P_STAGE_GLOW) && (p->flags & Q3P_PROG_STAGE_EMIT))
            Com_Printf("        (the entry's glow stage, but other stages have their own emissive_factor: "
                       "this one glows only by one of its own)\n");
    }
    if (p->has_props)
        Com_Printf("      (a stage's own surface replaces the material's values above where it shows)\n");
#undef BLEND_NAME
}

const image_t *vkpt_q3_stage_image(const pbr_material_t *mat, int stage, int which, const char **file)
{
    static char why[MAX_QPATH + 64];
    static const char *const map_names[4] = { "image", "texture_roughness", "texture_metallic", "texture_normals" };

    *file = "";
    if (!mat || !mat->q3_program || mat->q3_program > MAX_Q3_PROGRAMS || !programs[mat->q3_program - 1].in_use) {
        Q_strlcpy(why, "no stage program: the material's own maps are drawn", sizeof(why));
        *file = why;
        return NULL;
    }

    const q3program_t *p = &programs[mat->q3_program - 1];
    if (stage < 0 || stage >= p->num_stages || which < 0 || which > 3) {
        Q_snprintf(why, sizeof(why), "no stage%d: the shader has %d", stage, p->num_stages);
        *file = why;
        return NULL;
    }

    const q3pstage_t *ps = &p->stages[stage];
    const image_t *image = NULL;
    if (which == 0) {
        if (ps->flags & Q3P_STAGE_LIGHTMAP)
            Q_strlcpy(why, "$lightmap: the light, no image", sizeof(why));
        else if (ps->flags & Q3P_STAGE_WHITE)
            Q_strlcpy(why, "$whiteimage", sizeof(why));
        else if (!(image = stage_frame(ps, last_upload_time)))
            Q_strlcpy(why, "NO IMAGE - skipped", sizeof(why));
    } else if (!(image = ps->prop_images[which - 1])) {
        Q_snprintf(why, sizeof(why), "no stage%d_%s", stage, map_names[which]);
    }
    if (p->off & (1u << stage)) {
        Q_snprintf(why, sizeof(why), "stage%d_map off - not drawn", stage);
        image = NULL;
    }

    *file = image ? image->name : why;
    return image;
}

/*
===============================================================================

GLOWS AS LIGHTS

A light poly cut from a program's surface shines with one colour. Before, that
was the glow IMAGE's average, fixed at map load: a trim whose pulse sweeps past
(q3dm0's purple band, the jump pad wall's scrolling stripes) lit the walls the
same all the time, and dimmed by the image's dark rows, so raising it to the
pulse's level left it glowing while the surface was dark.

Now copy_light (vertex_buffer.c) asks here every frame: the program's emission -
C00 in q3_stages.glsl's terms, the same blend with no light and nothing behind -
averaged over the light's triangle, at the shader clock. The images are read from
the copy each keeps in memory (image_t pix_data), shrunk to LIGHT_IMAGE_SIZE and
mip-mapped, linear; a sample taken at the mip level that matches its share of
the triangle stands for that share's average. A reflection-map stage counts as
its image's average (the light shines every way), and a turb is left out.

===============================================================================
*/

#define LIGHT_IMAGE_SIZE    128
#define LIGHT_IMAGE_LEVELS  8       // 128 .. 1
#define LIGHT_MAX_SAMPLES   4       // per side of the triangle's sample grid

typedef struct {
    // what it was made from, to see the slot now holds something else
    char        name[MAX_IMAGE_PATH];
    const byte  *pix_data;
    int         src_width, src_height;
    uint64_t    last_modified;
    int         num_levels;
    int         width[LIGHT_IMAGE_LEVELS], height[LIGHT_IMAGE_LEVELS];
    float       *levels[LIGHT_IMAGE_LEVELS];    // RGBA, linear
} q3lightimage_t;

static q3lightimage_t *light_images[MAX_RIMAGES];
static float srgb_to_linear[256];

/* Counts free_light_image calls. A light program set up before the latest one
   may hold a freed image (set_up_light_program checks it). */
static uint32_t light_image_frees;

static void free_light_image(q3lightimage_t *li)
{
    light_image_frees++;
    for (int l = 0; l < li->num_levels; l++)
        Z_Free(li->levels[l]);
    Z_Free(li);
}

void vkpt_q3_light_cache_clear(void)
{
    for (int i = 0; i < MAX_RIMAGES; i++) {
        if (light_images[i]) {
            free_light_image(light_images[i]);
            light_images[i] = NULL;
        }
    }
}

// `image` as the lights read it, or NULL while its pixels are not in memory
static const q3lightimage_t *light_image(const image_t *image)
{
    if (!image)
        return NULL;
    const int index = (int)(image - r_images);
    if (index < 0 || index >= MAX_RIMAGES)
        return NULL;

    q3lightimage_t *li = light_images[index];
    if (li && li->pix_data == image->pix_data && li->src_width == image->upload_width &&
        li->src_height == image->upload_height && li->last_modified == image->last_modified &&
        !strcmp(li->name, image->name))
        return li;

    if (li) {
        free_light_image(li);
        light_images[index] = NULL;
    }

    const int sw = image->upload_width, sh = image->upload_height;
    if (!image->pix_data || image->pixel_format != PF_R8G8B8A8_UNORM || sw <= 0 || sh <= 0)
        return NULL;

    if (srgb_to_linear[255] == 0.f) {
        for (int i = 0; i < 256; i++) {
            float x = i / 255.f;
            srgb_to_linear[i] = x < 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
        }
    }

    li = Z_Mallocz(sizeof(*li));
    Q_strlcpy(li->name, image->name, sizeof(li->name));
    li->pix_data = image->pix_data;
    li->src_width = sw;
    li->src_height = sh;
    li->last_modified = image->last_modified;

    // level 0: the image boxed down to LIGHT_IMAGE_SIZE at most
    int w = sw, h = sh;
    while (w > LIGHT_IMAGE_SIZE || h > LIGHT_IMAGE_SIZE) {
        w = max(1, w / 2);
        h = max(1, h / 2);
    }
    float *dst = Z_Malloc(sizeof(float) * 4 * w * h);
    for (int y = 0; y < h; y++) {
        const int y0 = y * sh / h, y1 = max(y0 + 1, (y + 1) * sh / h);
        for (int x = 0; x < w; x++) {
            const int x0 = x * sw / w, x1 = max(x0 + 1, (x + 1) * sw / w);
            float sum[4] = { 0, 0, 0, 0 };
            for (int yy = y0; yy < y1; yy++) {
                const byte *px = image->pix_data + 4 * ((size_t)yy * sw + x0);
                for (int xx = x0; xx < x1; xx++, px += 4) {
                    if (image->is_srgb) {
                        sum[0] += srgb_to_linear[px[0]];
                        sum[1] += srgb_to_linear[px[1]];
                        sum[2] += srgb_to_linear[px[2]];
                    } else {
                        sum[0] += px[0] / 255.f;
                        sum[1] += px[1] / 255.f;
                        sum[2] += px[2] / 255.f;
                    }
                    sum[3] += px[3] / 255.f;
                }
            }
            const float n = 1.f / (float)((y1 - y0) * (x1 - x0));
            float *o = dst + 4 * (y * w + x);
            for (int c = 0; c < 4; c++)
                o[c] = sum[c] * n;
        }
    }
    li->levels[0] = dst;
    li->width[0] = w;
    li->height[0] = h;
    li->num_levels = 1;

    // the rest, two by two down to one texel
    while (li->num_levels < LIGHT_IMAGE_LEVELS && (w > 1 || h > 1)) {
        const float *src = li->levels[li->num_levels - 1];
        const int pw = w, ph = h;
        w = max(1, w / 2);
        h = max(1, h / 2);
        dst = Z_Malloc(sizeof(float) * 4 * w * h);
        for (int y = 0; y < h; y++) {
            const int ya = min(2 * y, ph - 1), yb = min(2 * y + 1, ph - 1);
            for (int x = 0; x < w; x++) {
                const int xa = min(2 * x, pw - 1), xb = min(2 * x + 1, pw - 1);
                for (int c = 0; c < 4; c++)
                    dst[4 * (y * w + x) + c] = 0.25f * (src[4 * (ya * pw + xa) + c] + src[4 * (ya * pw + xb) + c] +
                                                        src[4 * (yb * pw + xa) + c] + src[4 * (yb * pw + xb) + c]);
            }
        }
        li->levels[li->num_levels] = dst;
        li->width[li->num_levels] = w;
        li->height[li->num_levels] = h;
        li->num_levels++;
    }

    light_images[index] = li;
    return li;
}

static int wrap_texel(int i, int n)
{
    i %= n;
    return i < 0 ? i + n : i;
}

// bilinear, wrapped (or clamped to the edge, for clampMap)
static void sample_light_image(const q3lightimage_t *li, int level, float s, float t, bool clamp, float out[4])
{
    const int w = li->width[level], h = li->height[level];
    const float *texels = li->levels[level];

    if (!isfinite(s)) s = 0;
    if (!isfinite(t)) t = 0;
    if (!clamp) {
        // keeps the int conversion small; the image repeats every 1
        s -= floorf(s);
        t -= floorf(t);
    }

    const float x = s * w - 0.5f, y = t * h - 0.5f;
    const float fx0 = floorf(x), fy0 = floorf(y);
    const float fx = x - fx0, fy = y - fy0;
    int x0 = (int)fx0, y0 = (int)fy0, x1 = x0 + 1, y1 = y0 + 1;
    if (clamp) {
        x0 = max(0, min(w - 1, x0)); x1 = max(0, min(w - 1, x1));
        y0 = max(0, min(h - 1, y0)); y1 = max(0, min(h - 1, y1));
    } else {
        x0 = wrap_texel(x0, w); x1 = wrap_texel(x1, w);
        y0 = wrap_texel(y0, h); y1 = wrap_texel(y1, h);
    }

    const float *a = texels + 4 * (y0 * w + x0), *b = texels + 4 * (y0 * w + x1);
    const float *c = texels + 4 * (y1 * w + x0), *d = texels + 4 * (y1 * w + x1);
    for (int k = 0; k < 4; k++) {
        const float top = a[k] + (b[k] - a[k]) * fx;
        const float bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

// q3_blend.glsl's q3_blend_factor
static void blend_factor(int f, const float src[4], const float dst[4], float out[4])
{
    switch (f) {
    case Q3BF_ZERO:
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    case Q3BF_DST_COLOR:
        for (int k = 0; k < 4; k++) out[k] = dst[k];
        return;
    case Q3BF_ONE_MINUS_DST_COLOR:
        for (int k = 0; k < 4; k++) out[k] = 1 - dst[k];
        return;
    case Q3BF_SRC_ALPHA:
        out[0] = out[1] = out[2] = out[3] = src[3];
        return;
    case Q3BF_ONE_MINUS_SRC_ALPHA:
        out[0] = out[1] = out[2] = out[3] = 1 - src[3];
        return;
    case Q3BF_DST_ALPHA:
        out[0] = out[1] = out[2] = out[3] = dst[3];
        return;
    case Q3BF_ONE_MINUS_DST_ALPHA:
        out[0] = out[1] = out[2] = out[3] = 1 - dst[3];
        return;
    case Q3BF_SRC_COLOR:
        for (int k = 0; k < 4; k++) out[k] = src[k];
        return;
    case Q3BF_ONE_MINUS_SRC_COLOR:
        for (int k = 0; k < 4; k++) out[k] = 1 - src[k];
        return;
    case Q3BF_SRC_ALPHA_SATURATE: {
        const float s = min(src[3], 1 - dst[3]);
        out[0] = out[1] = out[2] = s;
        out[3] = 1;
        return;
    }
    default:
        out[0] = out[1] = out[2] = out[3] = 1;
        return;
    }
}

// A program's stages at one moment, for every light that uses it this frame.
typedef struct {
    const q3lightimage_t *image;        // NULL: no image to read (lightmap, white, none)
    float       m[6];                   // both tcMod matrices in one, the turb left out
    float       det;                    // |determinant| of its linear part
    float       colour[4];
    float       average[4];             // a reflection map: the image's average
    uint32_t    flags;
    int         src_blend, dst_blend, alpha_func;
    float       glow_factor;            // stage<N>_emissive_factor
} q3lightstage_t;

typedef struct {
    uint32_t    key;                    // vkpt_q3_program_key it was set up under
    float       time;
    bool        ready;                  // every image it draws is in memory
    uint32_t    frees;                  // light_image_frees when it was set up
    q3lightstage_t stages[Q3P_MAX_STAGES];
} q3lightprog_t;

static q3lightprog_t light_programs[MAX_Q3_PROGRAMS];

static bool shine_on(void)
{
    if (!cvar_pt_q3_env_shine)
        cvar_pt_q3_env_shine = Cvar_Get("pt_q3_env_shine", "1", 0);
    return cvar_pt_q3_env_shine->integer != 0;
}

static const q3program_t *program_of(const pbr_material_t *mat)
{
    if (!mat || !mat->q3_program || mat->q3_program > MAX_Q3_PROGRAMS)
        return NULL;
    const q3program_t *p = &programs[mat->q3_program - 1];
    return p->in_use ? p : NULL;
}

uint32_t vkpt_q3_program_key(const pbr_material_t *mat)
{
    const q3program_t *p = program_of(mat);
    // pt_q3_env_shine changes what a reflection map stands for
    return p ? p->build * 2 + (shine_on() ? 1 : 0) : 0;
}

bool vkpt_q3_program_glow_moves(const pbr_material_t *mat)
{
    const q3program_t *p = program_of(mat);
    return p && p->glow_moves;
}

static const q3lightprog_t *set_up_light_program(const q3program_t *p, uint32_t key, float time)
{
    q3lightprog_t *lp = &light_programs[p - programs];
    /* The stages point into light_images, so a free since the set-up makes it
       stale even at the same key and time. That happened on map changes: the
       programs outlive the map, a still one is set up once at the map's first
       frame (q3_glow_color keeps its colour), the next map's first frame comes
       at the same clock - and its glow read the freed image (w = h = 0 once the
       memory was reused: a divide by zero in wrap_texel). */
    if (lp->key == key && lp->time == time && lp->ready && lp->frees == light_image_frees)
        return lp;

    const bool shine = shine_on() && program_shines(p);

    lp->key = key;
    lp->time = time;
    lp->ready = true;

    for (int s = 0; s < p->num_stages; s++) {
        const q3pstage_t *ps = &p->stages[s];
        q3lightstage_t *ls = &lp->stages[s];

        ls->flags = ps->flags;
        ls->src_blend = ps->src_blend;
        ls->dst_blend = ps->dst_blend;
        ls->alpha_func = ps->alpha_func;
        ls->glow_factor = ps->glow_factor;
        ls->image = NULL;
        stage_colour(ps, time, ls->colour);

        if (ps->flags & (Q3P_STAGE_HIGHLIGHT | Q3P_STAGE_NO_IMAGE | Q3P_STAGE_LIGHTMAP | Q3P_STAGE_WHITE))
            continue;

        const image_t *frame = stage_frame(ps, time);
        if (!frame) {
            ls->flags |= Q3P_STAGE_NO_IMAGE;    // as write_stage
            continue;
        }
        ls->image = light_image(frame);
        if (!ls->image) {
            lp->ready = false;
            continue;
        }

        float m2[6], turb_amp, turb_now;
        eval_tcmods(ps, time, ls->m, m2, &turb_amp, &turb_now);
        affine_then(ls->m, m2);
        ls->det = fabsf(ls->m[0] * ls->m[3] - ls->m[1] * ls->m[2]);

        if (ps->flags & Q3P_STAGE_ENV) {
            const q3lightimage_t *li = ls->image;
            memcpy(ls->average, li->levels[li->num_levels - 1], sizeof(ls->average));
            // as q3_stages.glsl: a metal's shine is its hue at a bright steel's,
            // unless it is added on top of the surface
            if (shine && !(ps->flags & Q3P_STAGE_ENV_EFFECT) && p->shine_metal && ps->dst_blend != Q3BF_ONE) {
                const float lum = 0.2126f * ls->average[0] + 0.7152f * ls->average[1] + 0.0722f * ls->average[2];
                for (int k = 0; k < 3; k++)
                    ls->average[k] = lum > 1e-4f ? min(ls->average[k] * (float)Q3_SHINE_METAL_F0 / lum, 1.f)
                                                 : (float)Q3_SHINE_METAL_F0;
            }
        }
    }

    // after the loop: a free light_image did above replaced another slot's
    // image, never one of these stages'
    lp->frees = light_image_frees;
    return lp;
}

int vkpt_q3_light_glow(const pbr_material_t *mat, const float uv[6], int samples, float time, vec3_t out,
                       vec3_t out_stages)
{
    VectorClear(out);
    VectorClear(out_stages);

    const q3program_t *p = program_of(mat);
    if (!p)
        return 0;

    time = fmodf(time, 86400.f);    // as vkpt_q3_upload
    const q3lightprog_t *lp = set_up_light_program(p, vkpt_q3_program_key(mat), time);
    if (!lp->ready)
        return 0;

    const bool self_lit = (p->flags & Q3P_PROG_SELF_LIT) != 0;
    const bool stage_emit = (p->flags & Q3P_PROG_STAGE_EMIT) != 0;
    // as q3_stages.glsl: stage<N>_emissive_factor settings take the entry's
    // glow stage over (it glows by a factor of its own, or not)
    const bool glow = (p->flags & Q3P_PROG_GLOW) != 0 && !stage_emit;
    const int n = max(1, min(LIGHT_MAX_SAMPLES, samples));
    const float e1[2] = { uv[2] - uv[0], uv[3] - uv[1] };
    const float e2[2] = { uv[4] - uv[0], uv[5] - uv[1] };
    // each sample's share of the triangle, in texture repeats squared
    const float share = 0.5f * fabsf(e1[0] * e2[1] - e1[1] * e2[0]) / (float)(n * n);

    // the mip level a sample's share covers, per stage
    int level[Q3P_MAX_STAGES];
    for (int s = 0; s < p->num_stages; s++) {
        const q3lightimage_t *li = lp->stages[s].image;
        level[s] = 0;
        if (!li)
            continue;
        const float texels = lp->stages[s].det * share * (float)(li->width[0] * li->height[0]);
        if (texels > 1.f)
            level[s] = min(li->num_levels - 1, (int)(0.5f * log2f(texels) + 0.5f));
    }

    vec3_t sum = { 0, 0, 0 }, sum_stages = { 0, 0, 0 };

    /* The centres of the n x n triangles the light's triangle splits into, all
       the same size: n(n+1)/2 pointing like it, n(n-1)/2 the other way. */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n - i; j++) {
            for (int flip = 0; flip < 2; flip++) {
                if (flip && i + j >= n - 1)
                    break;
                const float a = (i + (flip ? 2.f : 1.f) / 3.f) / (float)n;
                const float b = (j + (flip ? 2.f : 1.f) / 3.f) / (float)n;
                const float u = uv[0] + a * e1[0] + b * e2[0];
                const float v = uv[1] + a * e1[1] + b * e2[1];

                float c00[4] = { 0, 0, 0, 1 };
                // Q3P_PROG_GLOW, as q3_stages.glsl: fully lit, and fully lit
                // with the Q3P_STAGE_GLOW stages black
                float c10[4] = { 0, 0, 0, 1 }, cg[4] = { 0, 0, 0, 1 };
                // Q3P_PROG_STAGE_EMIT: fully lit with each stage<N>_emissive_factor
                // stage's colour x (1 - its factor) - C10 - CS is what of them
                // shows, fully lit, x the factor
                float cs[4] = { 0, 0, 0, 1 };

                for (int s = 0; s < p->num_stages; s++) {
                    const q3lightstage_t *ls = &lp->stages[s];
                    const uint32_t flags = ls->flags;

                    if (flags & (Q3P_STAGE_HIGHLIGHT | Q3P_STAGE_NO_IMAGE))
                        continue;

                    float tex[4] = { 1, 1, 1, 1 };
                    if (ls->image) {
                        if (flags & Q3P_STAGE_ENV) {
                            memcpy(tex, ls->average, sizeof(tex));
                        } else {
                            float st = ls->m[0] * u + ls->m[1] * v + ls->m[4];
                            float tt = ls->m[2] * u + ls->m[3] * v + ls->m[5];
                            const bool clamp = (flags & Q3P_STAGE_CLAMP) != 0;
                            if (clamp) {
                                st = clamp01(st);
                                tt = clamp01(tt);
                            }
                            sample_light_image(ls->image, level[s], st, tt, clamp, tex);
                        }
                    }

                    float s0[4] = { tex[0] * ls->colour[0], tex[1] * ls->colour[1], tex[2] * ls->colour[2],
                                    tex[3] * ls->colour[3] };

                    if (ls->alpha_func == Q3ALPHA_GT0 && !(s0[3] > 0))
                        continue;
                    if (ls->alpha_func == Q3ALPHA_LT128 && !(s0[3] < 0.5f))
                        continue;
                    if (ls->alpha_func == Q3ALPHA_GE128 && !(s0[3] >= 0.5f))
                        continue;

                    if (glow || stage_emit) {
                        float s1[4], fs[4], fd[4];
                        memcpy(s1, s0, sizeof(s1));
                        if (flags & Q3P_STAGE_ONE_MINUS_LIT)
                            s1[0] = s1[1] = s1[2] = 0;

                        blend_factor(ls->src_blend, s1, c10, fs);
                        blend_factor(ls->dst_blend, s1, c10, fd);
                        for (int k = 0; k < 4; k++)
                            c10[k] = s1[k] * fs[k] + c10[k] * fd[k];

                        if (glow) {
                            float sg[4];
                            memcpy(sg, s1, sizeof(sg));
                            if (flags & Q3P_STAGE_GLOW)
                                sg[0] = sg[1] = sg[2] = 0;
                            blend_factor(ls->src_blend, sg, cg, fs);
                            blend_factor(ls->dst_blend, sg, cg, fd);
                            for (int k = 0; k < 4; k++)
                                cg[k] = sg[k] * fs[k] + cg[k] * fd[k];
                        }
                        if (stage_emit) {
                            float se[4];
                            memcpy(se, s1, sizeof(se));
                            for (int k = 0; k < 3; k++)
                                se[k] *= 1.f - ls->glow_factor;
                            blend_factor(ls->src_blend, se, cs, fs);
                            blend_factor(ls->dst_blend, se, cs, fd);
                            for (int k = 0; k < 4; k++)
                                cs[k] = se[k] * fs[k] + cs[k] * fd[k];
                        }
                    }

                    // with no light: what the light multiplies is not there -
                    // or, for Q3P_PROG_SELF_LIT, with all of it (C10): what
                    // only shows without light is not
                    if (self_lit) {
                        if (flags & Q3P_STAGE_ONE_MINUS_LIT)
                            s0[0] = s0[1] = s0[2] = 0;
                    } else if (flags & (Q3P_STAGE_LIGHTMAP | Q3P_STAGE_LIT)) {
                        s0[0] = s0[1] = s0[2] = 0;
                    }

                    float fs[4], fd[4];
                    blend_factor(ls->src_blend, s0, c00, fs);
                    blend_factor(ls->dst_blend, s0, c00, fd);
                    for (int k = 0; k < 4; k++)
                        c00[k] = s0[k] * fs[k] + c00[k] * fd[k];
                }

                // the .mat's glow stage: what shows of it, fully lit
                if (glow)
                    for (int k = 0; k < 3; k++)
                        c00[k] = max(max(c00[k], 0.f), c10[k] - cg[k]);

                // correct_emissive (path_tracer_rgen.h), as the surface shows it
                for (int k = 0; k < 3; k++)
                    sum[k] += max(0.f, c00[k] + (float)EMISSIVE_TRANSFORM_BIAS);
                if (stage_emit)
                    for (int k = 0; k < 3; k++)
                        sum_stages[k] += max(0.f, c10[k] - cs[k] + (float)EMISSIVE_TRANSFORM_BIAS);
            }
        }
    }

    VectorScale(sum, 1.f / (float)(n * n), out);
    VectorScale(sum_stages, 1.f / (float)(n * n), out_stages);
    return p->varies ? 1 : 2;
}

/*
===============================================================================

QUAKE III SKY

A Quake III sky shader's cloud layers, drawn per ray over the far box by
shader/q3_sky.glsl, and its q3map_sun. main.c's R_SetSky_RTX hands the shader
over once per map; the layers are evaluated like any program's stages - frame,
tcMods, rgbGen and alphaGen at the shader clock - into the UBO every frame.

The images are loaded UNORM (no IF_SRGB): the far box is a plain UNORM cube map
read as its gamma-encoded values, and the layers blend over it in that same
space, as Quake III blended them into its framebuffer.

===============================================================================
*/

static struct {
    int         num_layers;
    q3pstage_t  layers[Q3_SKY_MAX_LAYERS];
    float       cloud_height;
    bool        has_sun;
    vec3_t      sun_light;          // q3map_sun's normalised colour x its intensity
    float       sun_degrees;
    float       sun_elevation;
    bool        has_hue;
    vec3_t      sky_hue;            // the sky's average colour, brightest channel 1 (vkpt_q3_sky_tint)
} q3_sky;

static cvar_t *cvar_pt_q3_sky_clouds;
static cvar_t *cvar_pt_q3_sun;
static cvar_t *cvar_pt_q3_sun_scale;
static cvar_t *cvar_pt_q3_sun_sky_tint;
static cvar_t *cvar_pt_q3_sun_color;

static void q3_sky_cvars(void)
{
    if (cvar_pt_q3_sky_clouds)
        return;

    // 1 = a Quake III sky's cloud layers move, drawn per ray; 0 = baked still into
    // the cube map, as before. Read when a map's sky is set (the next map load).
    cvar_pt_q3_sky_clouds = Cvar_Get("pt_q3_sky_clouds", "1", 0);
    // 1 = a Quake III sky's q3map_sun lights the map under its own skybox; 0 = no
    // sun, the sky's own light only. Live. (Matt tried 0 and kept the sun,
    // 2026-11-16.)
    cvar_pt_q3_sun = Cvar_Get("pt_q3_sun", "1", 0);
    /* The sun's light on surfaces per unit of q3map_sun intensity. 0.1 put the usual
       100 at about the physical sky's noon sun; Matt, 2026-11-16, on q3dm1's
       near-white ground: "255 200 200 and half as bright" - 0.05. The haze keeps
       0.1's sun (vkpt_q3_sun_fog_ratio): he liked the fog as it was. q3dm1 sets
       its own in baseq3/maps/q3dm1.cfg (Matt's 0.0005). Live. */
    cvar_pt_q3_sun_scale = Cvar_Get("pt_q3_sun_scale", "0.05", 0);
    /* How far the sun's light takes on the sky's colour: 0 = q3map_sun's own, 1 = the
       sky's hue (tim_hell's 1 .10 .02, deep red). The brightest channel keeps its
       strength, the rest come down. Default 0: q3dm1's own 1 .77 .77 is the 255 200
       200 Matt asked for. (Decoding q3map_sun's colour from sRGB with a 0.25 tint
       was written next, then dropped untried: Matt tuned the scale instead.) Live. */
    cvar_pt_q3_sun_sky_tint = Cvar_Get("pt_q3_sun_sky_tint", "0", 0);
    /* The sun's COLOUR in place of q3map_sun's, as "r g b". The scale does not
       matter: 255 200 200 and 1 .78 .78 are the same colour. It is normalised as
       the shader parser normalises q3map_sun's (q3shader.c), so the same hue gives
       exactly the light the map had. The strength stays q3map_sun's intensity x
       pt_q3_sun_scale. Empty = the map's own. Per map: "mapcvar pt_q3_sun_color
       ..." in baseq3/maps/<map>.cfg. Live. (Matt, 2026-11-16: "set the RGB of the
       sun color on certain levels".) */
    cvar_pt_q3_sun_color = Cvar_Get("pt_q3_sun_color", "", 0);
}

// pt_q3_sun_color as a unit-length colour, false when it is unset or unusable
static bool q3_sun_color_override(vec3_t out)
{
    vec3_t c;

    if (sscanf(cvar_pt_q3_sun_color->string, "%f %f %f", &c[0], &c[1], &c[2]) != 3)
        return false;
    for (int i = 0; i < 3; i++)
        c[i] = max(0.f, c[i]);
    if (VectorNormalize(c) <= 0.f)
        return false;

    VectorCopy(c, out);
    return true;
}

void vkpt_q3_sky_register(void)
{
    q3_sky_cvars();
}

bool vkpt_q3_sky_set(const q3shader_t *sky)
{
    q3_sky_cvars();
    memset(&q3_sky, 0, sizeof(q3_sky));

    if (!sky)
        return false;

    q3_sky.cloud_height = sky->sky_cloud_height > 0 ? sky->sky_cloud_height : 512.f;

    if (sky->has_sun) {
        q3_sky.has_sun = true;
        VectorScale(sky->sun_color, sky->sun_intensity, q3_sky.sun_light);
        q3_sky.sun_degrees = sky->sun_degrees;
        q3_sky.sun_elevation = sky->sun_elevation;
    }

    if (!cvar_pt_q3_sky_clouds->integer)
        return false;

    for (int i = 0; i < sky->num_stages && q3_sky.num_layers < Q3_SKY_MAX_LAYERS; i++) {
        const q3stage_t *st = &sky->stages[i];
        q3pstage_t *ps = &q3_sky.layers[q3_sky.num_layers];

        // the same stages main.c's bake draws
        if (st->lightmap || st->white || st->video || !st->map[0])
            continue;

        memset(ps, 0, sizeof(*ps));
        if (st->num_frames > 1 && st->frames) {
            for (int f = 0; f < st->num_frames && f < Q3S_MAX_ANIMFRAMES; f++) {
                ps->images[f] = load_stage_image(st->frames[f], IT_WALL, IF_REPEAT);
                ps->num_images = f + 1;
            }
        } else {
            ps->images[0] = load_stage_image(st->map, IT_WALL, IF_REPEAT);
            ps->num_images = ps->images[0] ? 1 : 0;
        }
        if (!ps->num_images || !ps->images[0])
            continue;

        ps->anim_freq = st->anim_freq;
        ps->src_blend = st->src_blend;
        ps->dst_blend = st->dst_blend;
        if (ps->src_blend == Q3BF_NONE || ps->dst_blend == Q3BF_NONE) {
            ps->src_blend = Q3BF_ONE;
            ps->dst_blend = Q3BF_ZERO;
        }
        ps->rgbgen = st->rgbgen;
        VectorCopy(st->rgb_const, ps->rgb_const);
        ps->rgb_wave = st->rgb_wave;
        ps->alphagen = st->alphagen;
        ps->alpha_const = st->alpha_const;
        ps->alpha_wave = st->alpha_wave;
        ps->num_tcmods = st->num_tcmods;
        memcpy(ps->tcmods, st->tcmods, sizeof(ps->tcmods));

        q3_sky.num_layers++;
    }

    return q3_sky.num_layers > 0;
}

void vkpt_q3_sky_upload(QVKUniformBuffer_t *ubo, float time)
{
    int n = 0;

    time = fmodf(time, 86400.f);   // as vkpt_q3_upload

    for (int l = 0; l < q3_sky.num_layers; l++) {
        const q3pstage_t *ps = &q3_sky.layers[l];
        const image_t *image = stage_frame(ps, time);

        // freed under the sky (a renderer restart before the next R_SetSky)
        if (!image || !image->name[0] || !image->registration_sequence)
            continue;

        float m1[6], m2[6], turb_amp, turb_now, colour[4];
        eval_tcmods(ps, time, m1, m2, &turb_amp, &turb_now);
        affine_then(m1, m2);    // a turb between them is left out on the sky
        stage_colour(ps, time, colour);

        vec4 *out = &ubo->q3_sky_layers[n * Q3_SKY_LAYER_VEC4S];
        put_vec4(&out[0], m1[0], m1[1], m1[2], m1[3]);
        put_uvec4(&out[1], float_bits(m1[4]), float_bits(m1[5]), (uint32_t)(image - r_images),
                  (uint32_t)ps->src_blend | ((uint32_t)ps->dst_blend << 4));
        put_vec4(&out[2], colour[0], colour[1], colour[2], colour[3]);
        n++;
    }

    ubo->q3_sky_num_layers = n;
    ubo->q3_sky_cloud_height = q3_sky.cloud_height;
}

bool vkpt_q3_sky_sun(vec3_t color, float *degrees, float *elevation)
{
    q3_sky_cvars();

    if (!q3_sky.has_sun || !cvar_pt_q3_sun->integer)
        return false;

    VectorScale(q3_sky.sun_light, max(0.f, cvar_pt_q3_sun_scale->value), color);

    // pt_q3_sun_color: another colour at the same strength (sun_light is the
    // unit-length colour x the intensity, so its length is the intensity)
    vec3_t own;
    if (q3_sun_color_override(own))
        VectorScale(own, VectorLength(color), color);

    // the sky's colour imparted on it (pt_q3_sun_sky_tint)
    if (q3_sky.has_hue) {
        const float t = clamp01(cvar_pt_q3_sun_sky_tint->value);
        for (int i = 0; i < 3; i++)
            color[i] *= 1.f + (q3_sky.sky_hue[i] - 1.f) * t;
    }

    *degrees = q3_sky.sun_degrees;
    *elevation = q3_sky.sun_elevation;
    return true;
}

// The pt_q3_sun_scale the haze's sun stays at (see vkpt_q3_sun_fog_ratio).
#define Q3_SUN_FOG_SCALE 0.1f

float vkpt_q3_sun_fog_ratio(void)
{
    q3_sky_cvars();

    const float scale = cvar_pt_q3_sun_scale->value;
    if (!q3_sky.has_sun || !cvar_pt_q3_sun->integer || scale <= 0.f)
        return 1.f;

    return Q3_SUN_FOG_SCALE / scale;
}

void vkpt_q3_sky_tint(const vec3_t avg)
{
    const float peak = max(avg[0], max(avg[1], avg[2]));

    q3_sky.has_hue = peak > 1e-3f;
    if (q3_sky.has_hue)
        VectorScale(avg, 1.f / peak, q3_sky.sky_hue);
}

/*
===============================================================================

QUAKE III QUAD DAMAGE (TEXTURE_FLAG_Q3_QUAD)

Matt, 2026-11-17: on the Quake III maps the quad should not make the model glow
blue but wear Quake III's glowing lightning. That is its powerups/quad shader
(scripts/gfx.shader), which cgame drew as a second pass over the player model
and the gun:

    deformVertexes wave 100 sin 3 0 0 0
    { map textures/effects/quadmap2.tga; blendfunc GL_ONE GL_ONE;
      tcGen environment; tcmod rotate 30; tcmod scroll 1 .1 }

The client marks a quad entity on a Quake III map RF_Q3_QUAD, main.c gives its
model instances TEXTURE_FLAG_Q3_QUAD in place of MATERIAL_FLAG_SHELL_BLUE, and
the path tracer adds this stage as a glow on them (path_tracer_rgen.h
get_emissive_q3_quad). The stage is read from the script and goes into the UBO
like a sky cloud layer, its tcMods worked out here every frame. The deform,
which floated the pass 3 units out (half a unit on the gun), is left out: the
glow is on the model's own surface, where the blue shell was.

A "powerups/quad" entry in a .mat with a stage0_map line swaps the image, as on
any Quake III shader. pt_q3_quad_emissive (archived, live) is its brightness,
read like a material's emissive_factor: 1 = the first version's (what was
pt_q3_quad_glow 0.5), default 2 at Matt's asking (2026-11-17). It replaces
pt_q3_quad_glow, whose 0.5 was already saved in the config.

===============================================================================
*/

#define Q3_QUAD_SHADER  "powerups/quad"

static q3pstage_t q3_quad;          // num_images 0: none this map
static cvar_t *cvar_pt_q3_quad_emissive;

// the layer colour at pt_q3_quad_emissive 1
#define Q3_QUAD_EMISSIVE_UNIT   0.5f

void vkpt_q3_quad_register(bool quake3)
{
    // the last map's images are about to be freed (IMG_FreeUnused)
    memset(&q3_quad, 0, sizeof(q3_quad));

    if (!cvar_pt_q3_quad_emissive)
        cvar_pt_q3_quad_emissive = Cvar_Get("pt_q3_quad_emissive", "2", CVAR_ARCHIVE);

    if (!quake3)
        return;

    Q3Shader_Load();
    const q3shader_t *q3 = Q3Shader_Find(Q3_QUAD_SHADER);
    if (!q3) {
        Com_DPrintf("%s: no %s shader, the quad keeps its blue shell\n", __func__, Q3_QUAD_SHADER);
        return;
    }

    // its first stage with a picture: quadmap2
    for (int i = 0; i < q3->num_stages; i++) {
        const q3stage_t *st = &q3->stages[i];
        q3pstage_t *ps = &q3_quad;
        const char *path = vkpt_q3_stage_override(Q3_QUAD_SHADER, i);

        if (st->lightmap || st->white || st->video || (!st->map[0] && !path)
            || vkpt_q3_stage_off(Q3_QUAD_SHADER, i))
            continue;

        memset(ps, 0, sizeof(*ps));
        // a stage<N>_map whose file is missing: the script's image, as without it
        image_t *override = path ? load_stage_image(path, IT_SKIN, IF_REPEAT | IF_SRGB) : NULL;
        if (override) {
            ps->images[0] = override;
            ps->num_images = 1;
        } else if (st->num_frames > 1 && st->frames) {
            for (int f = 0; f < st->num_frames && f < Q3S_MAX_ANIMFRAMES; f++) {
                ps->images[f] = load_stage_image(st->frames[f], IT_SKIN, IF_REPEAT | IF_SRGB);
                ps->num_images = f + 1;
            }
        } else {
            ps->images[0] = load_stage_image(st->map, IT_SKIN, IF_REPEAT | IF_SRGB);
            ps->num_images = ps->images[0] ? 1 : 0;
        }
        if (!ps->num_images || !ps->images[0]) {
            memset(ps, 0, sizeof(*ps));
            continue;
        }

        ps->anim_freq = st->anim_freq;
        ps->rgbgen = st->rgbgen;
        VectorCopy(st->rgb_const, ps->rgb_const);
        ps->rgb_wave = st->rgb_wave;
        ps->alphagen = st->alphagen;
        ps->alpha_const = st->alpha_const;
        ps->alpha_wave = st->alpha_wave;
        ps->num_tcmods = st->num_tcmods;
        memcpy(ps->tcmods, st->tcmods, sizeof(ps->tcmods));
        return;
    }
}

bool vkpt_q3_quad_ready(void)
{
    const image_t *image = q3_quad.num_images > 0 ? q3_quad.images[0] : NULL;

    // freed under it (a renderer restart before the next registration)
    return image && image->name[0] && image->registration_sequence;
}

void vkpt_q3_quad_upload(QVKUniformBuffer_t *ubo, float time)
{
    ubo->q3_quad_active = 0;

    if (!vkpt_q3_quad_ready())
        return;

    time = fmodf(time, 86400.f);    // as vkpt_q3_upload
    const image_t *image = stage_frame(&q3_quad, time);
    if (!image || !image->name[0] || !image->registration_sequence)
        return;

    float m1[6], m2[6], turb_amp, turb_now, colour[4];
    eval_tcmods(&q3_quad, time, m1, m2, &turb_amp, &turb_now);
    affine_then(m1, m2);            // a turb between them is left out, as on the sky
    stage_colour(&q3_quad, time, colour);

    const float glow = Q3_QUAD_EMISSIVE_UNIT
        * (cvar_pt_q3_quad_emissive ? max(0.f, cvar_pt_q3_quad_emissive->value) : 2.f);

    vec4 *out = ubo->q3_quad_layer;
    put_vec4(&out[0], m1[0], m1[1], m1[2], m1[3]);
    put_uvec4(&out[1], float_bits(m1[4]), float_bits(m1[5]), (uint32_t)(image - r_images), 0);
    put_vec4(&out[2], colour[0] * glow, colour[1] * glow, colour[2] * glow, colour[3]);
    ubo->q3_quad_active = 1;
}

/* 'MAT RELOAD' AND THE STAGES' OWN IMAGES. A stage program loads its pictures
   by name (load_stage_image -> IMG_FindFile), so a rebuild finds the copy
   already in memory, and the material's own refresh (material_refresh_images)
   only looks at its seven maps. A stage texture edited in a paint program and
   saved - a frame, a glow, a stage<N>_map or stage<N>_texture_* file - was
   never read again until a restart (Matt, 11-18: "mat reload doesn't catch the
   texture stages"). The images reload in place, so the programs keep pointing
   at them; the lights' CPU copies of the pixels (light_image) are dropped so
   the glows are measured again. */
static int refresh_stage(q3pstage_t *ps)
{
    int reloaded = 0;

    for (int f = 0; f < ps->num_images && f < Q3S_MAX_ANIMFRAMES; f++)
        if (ps->images[f] && IMG_ReloadIfModified(ps->images[f]))
            reloaded++;
    for (int m = 0; m < 3; m++)
        if (ps->prop_images[m] && IMG_ReloadIfModified(ps->prop_images[m]))
            reloaded++;
    return reloaded;
}

int vkpt_q3_refresh_stage_images(void)
{
    int reloaded = 0;

    for (int i = 0; i < MAX_Q3_PROGRAMS; i++) {
        q3program_t *p = &programs[i];

        if (!p->in_use)
            continue;
        for (int s = 0; s < p->num_stages && s < Q3P_MAX_STAGES; s++)
            reloaded += refresh_stage(&p->stages[s]);
    }
    for (int i = 0; i < q3_sky.num_layers && i < Q3_SKY_MAX_LAYERS; i++)
        reloaded += refresh_stage(&q3_sky.layers[i]);
    reloaded += refresh_stage(&q3_quad);

    if (reloaded)
        vkpt_q3_light_cache_clear();
    return reloaded;
}
