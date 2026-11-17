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

//
// q3shader.c -- reading Quake III Arena's scripts/*.shader (see q3shader.h)
//
// The scripts are indexed when they are first needed - each shader name and
// where its body starts - and a shader is parsed the first time something
// looks it up. That keeps a Quake II session from paying anything for the
// ~2000 shaders Quake III ships when no Quake III map is ever loaded.
//
// When two scripts define the same name, the one read LAST wins, which is
// what Quake III itself does (ScanAndLoadShaderFiles concatenates the files in
// reverse so the later ones are found first). FS_ListFiles names each script
// once and FS_LoadFile reads the highest-priority copy, so a pak that replaces
// gfx.shader wholesale replaces it here too.
//

#include "shared/shared.h"
#include "common/common.h"
#include "common/cmd.h"
#include "common/files.h"
#include "common/utils.h"
#include "common/zone.h"
#include "common/q3shader.h"
#include "format/bsp3.h"

#define Q3S_HASH_SIZE   2048

typedef struct q3entry_s {
    struct q3entry_s    *hash_next;
    char                name[MAX_QPATH];
    int                 file;       // index into q3_files
    const char          *body;      // the '{' that opens the shader
    int                 line;
    q3shader_t          *parsed;    // NULL until first looked up
} q3entry_t;

typedef struct {
    char    path[MAX_QPATH];
    char    *text;
} q3file_t;

static q3file_t     *q3_files;
static int          q3_num_files;
static q3entry_t    *q3_hash[Q3S_HASH_SIZE];
static int          q3_num_shaders;
static bool         q3_loaded;

/*
===============================================================================

TOKENS

Quake III's COM_ParseExt: whitespace and both kinds of C comment are skipped, a
"quoted string" is one token, and when a line break is not allowed the token
comes back empty at the end of the line - which is how a directive with a
variable number of arguments knows it has run out of them.

===============================================================================
*/

static char q3_token[MAX_TOKEN_CHARS];
static int  q3_line;

static const char *skip_whitespace(const char *data, bool *newlines)
{
    int c;

    while ((c = *(const unsigned char *)data) <= ' ') {
        if (!c)
            return NULL;
        if (c == '\n') {
            q3_line++;
            *newlines = true;
        }
        data++;
    }

    return data;
}

static const char *q3_parse(const char **data_p, bool allow_line_breaks)
{
    const char  *data = *data_p;
    bool        newlines = false;
    int         len = 0, c;

    q3_token[0] = 0;

    if (!data) {
        *data_p = NULL;
        return q3_token;
    }

    while (1) {
        const char *line_end = data;    // where this stretch of whitespace starts

        data = skip_whitespace(data, &newlines);
        if (!data) {
            *data_p = NULL;
            return q3_token;
        }
        if (newlines && !allow_line_breaks) {
            /* STOP AT THE LINE BREAK, NOT PAST IT. Every directive is followed
               by skip_rest_of_line, which then ends THIS line. Left past the
               break, a directive that reads to the end of its line - animMap's
               frame list, above all - had skip_rest_of_line throw away the
               NEXT line: an animMap stage's blendFunc GL_ONE GL_ONE. Every
               animated flame then drew as an opaque card over its own black
               background, and q3dm1's computer disc as its "III" letters on
               black over the disc (Matt, 2026-11-16). */
            while (*line_end != '\n')
                line_end++;
            for (const char *p = line_end; p < data; p++) {
                if (*p == '\n')
                    q3_line--;  // skip_rest_of_line and the next token count them again
            }
            *data_p = line_end;
            return q3_token;
        }

        c = *data;
        if (c == '/' && data[1] == '/') {
            data += 2;
            while (*data && *data != '\n')
                data++;
        } else if (c == '/' && data[1] == '*') {
            data += 2;
            while (*data && !(*data == '*' && data[1] == '/')) {
                if (*data == '\n')
                    q3_line++;
                data++;
            }
            if (*data)
                data += 2;
        } else {
            break;
        }
    }

    if (c == '\"') {
        data++;
        while (1) {
            c = *data++;
            if (c == '\"' || !c)
                break;
            if (c == '\n')
                q3_line++;
            if (len < MAX_TOKEN_CHARS - 1)
                q3_token[len++] = c;
        }
        q3_token[len] = 0;
        *data_p = c ? data : data - 1;
        return q3_token;
    }

    do {
        if (len < MAX_TOKEN_CHARS - 1)
            q3_token[len++] = c;
        data++;
        c = *(const unsigned char *)data;
    } while (c > ' ');

    q3_token[len] = 0;
    *data_p = data;
    return q3_token;
}

// Skips what is left of the current line.
static void skip_rest_of_line(const char **data_p)
{
    const char *p = *data_p;

    if (!p)
        return;

    while (*p && *p != '\n')
        p++;
    if (*p) {
        p++;
        q3_line++;
    }
    *data_p = p;
}

// Skips a { } block whose '{' has already been read, nested blocks included.
static void skip_braced_section(const char **data_p, int depth)
{
    const char *tok;

    do {
        tok = q3_parse(data_p, true);
        if (!tok[0] && !*data_p)
            return;
        if (tok[1] == 0) {
            if (tok[0] == '{')
                depth++;
            else if (tok[0] == '}')
                depth--;
        }
    } while (depth > 0 && *data_p);
}

/*
===============================================================================

INDEX

===============================================================================
*/

static void strip_extension_lower(const char *in, char *out, size_t size)
{
    Q_strlcpy(out, in, size);
    Q_strlwr(out);

    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && (!slash || dot > slash))
        *dot = 0;

    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static q3entry_t *find_entry(const char *name)
{
    unsigned hash = Com_HashString(name, Q3S_HASH_SIZE);

    for (q3entry_t *e = q3_hash[hash]; e; e = e->hash_next)
        if (!strcmp(e->name, name))
            return e;

    return NULL;
}

static void index_file(int file_index)
{
    const char *p = q3_files[file_index].text;
    char name[MAX_QPATH];

    q3_line = 1;

    while (p) {
        const char *tok = q3_parse(&p, true);
        if (!tok[0])
            break;

        // a stray '{' or '}' between shaders: skip it rather than taking it
        // as a name, so one malformed script cannot shift every shader after it
        if (!strcmp(tok, "{")) {
            skip_braced_section(&p, 1);
            continue;
        }
        if (!strcmp(tok, "}"))
            continue;

        strip_extension_lower(tok, name, sizeof(name));
        int line = q3_line;

        const char *body = p;
        tok = q3_parse(&p, true);
        if (strcmp(tok, "{")) {
            Com_DPrintf("%s:%d: expected '{' after shader '%s', found '%s'\n",
                        q3_files[file_index].path, q3_line, name, tok);
            continue;
        }

        // the opening brace is what the parser starts from
        body = p - 1;
        while (body > q3_files[file_index].text && *body != '{')
            body--;

        skip_braced_section(&p, 1);

        q3entry_t *e = find_entry(name);
        if (!e) {
            unsigned hash = Com_HashString(name, Q3S_HASH_SIZE);
            e = Z_TagMallocz(sizeof(*e), TAG_GENERAL);
            Q_strlcpy(e->name, name, sizeof(e->name));
            e->hash_next = q3_hash[hash];
            q3_hash[hash] = e;
            q3_num_shaders++;
        }

        // a later script replaces an earlier definition
        e->file = file_index;
        e->body = body;
        e->line = line;
    }
}

void Q3Shader_Load(void)
{
    int num_files = 0;
    void **list;

    if (q3_loaded)
        return;
    q3_loaded = true;

    list = FS_ListFiles("scripts", ".shader", 0, &num_files);
    if (!list || !num_files) {
        if (list)
            FS_FreeList(list);
        return;
    }

    q3_files = Z_TagMallocz(sizeof(q3file_t) * num_files, TAG_GENERAL);

    for (int i = 0; i < num_files; i++) {
        q3file_t *f = &q3_files[q3_num_files];
        char *buf = NULL;

        Q_concat(f->path, sizeof(f->path), "scripts/", (const char *)list[i]);
        int len = FS_LoadFile(f->path, (void **)&buf);
        if (!buf || len <= 0)
            continue;

        // FS_LoadFile terminates the buffer
        f->text = buf;
        q3_num_files++;
    }

    FS_FreeList(list);

    for (int i = 0; i < q3_num_files; i++)
        index_file(i);

    Com_DPrintf("Indexed %d Quake III shaders from %d scripts\n", q3_num_shaders, q3_num_files);
}

static void free_shader(q3shader_t *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->num_stages; i++)
        if (s->stages[i].frames)
            Z_Free(s->stages[i].frames);
    Z_Free(s);
}

void Q3Shader_Shutdown(void)
{
    for (int i = 0; i < Q3S_HASH_SIZE; i++) {
        q3entry_t *e = q3_hash[i];
        while (e) {
            q3entry_t *next = e->hash_next;
            free_shader(e->parsed);
            Z_Free(e);
            e = next;
        }
        q3_hash[i] = NULL;
    }

    for (int i = 0; i < q3_num_files; i++)
        FS_FreeFile(q3_files[i].text);
    if (q3_files)
        Z_Free(q3_files);

    q3_files = NULL;
    q3_num_files = 0;
    q3_num_shaders = 0;
    q3_loaded = false;
}

bool Q3Shader_Available(void)
{
    return q3_num_shaders > 0;
}

/*
===============================================================================

PARSING

===============================================================================
*/

static int blend_factor(const char *s)
{
    static const struct { const char *name; int factor; } factors[] = {
        { "GL_ONE", Q3BF_ONE },
        { "GL_ZERO", Q3BF_ZERO },
        { "GL_DST_COLOR", Q3BF_DST_COLOR },
        { "GL_ONE_MINUS_DST_COLOR", Q3BF_ONE_MINUS_DST_COLOR },
        { "GL_SRC_ALPHA", Q3BF_SRC_ALPHA },
        { "GL_ONE_MINUS_SRC_ALPHA", Q3BF_ONE_MINUS_SRC_ALPHA },
        { "GL_DST_ALPHA", Q3BF_DST_ALPHA },
        { "GL_ONE_MINUS_DST_ALPHA", Q3BF_ONE_MINUS_DST_ALPHA },
        { "GL_SRC_COLOR", Q3BF_SRC_COLOR },
        { "GL_ONE_MINUS_SRC_COLOR", Q3BF_ONE_MINUS_SRC_COLOR },
        { "GL_SRC_ALPHA_SATURATE", Q3BF_SRC_ALPHA_SATURATE },
    };

    for (int i = 0; i < q_countof(factors); i++)
        if (!Q_stricmp(s, factors[i].name))
            return factors[i].factor;

    return Q3BF_NONE;
}

static q3blend_t classify_blend(int src, int dst)
{
    if (src == Q3BF_ONE && dst == Q3BF_ZERO)
        return Q3BLEND_OPAQUE;

    // anything whose destination term keeps all of what is behind it adds light
    if (dst == Q3BF_ONE)
        return Q3BLEND_ADD;

    if ((src == Q3BF_DST_COLOR || src == Q3BF_ZERO) &&
        (dst == Q3BF_ZERO || dst == Q3BF_SRC_COLOR || dst == Q3BF_SRC_ALPHA || dst == Q3BF_ONE_MINUS_DST_ALPHA))
        return Q3BLEND_FILTER;

    if ((src == Q3BF_SRC_ALPHA && dst == Q3BF_ONE_MINUS_SRC_ALPHA) ||
        (src == Q3BF_ONE_MINUS_SRC_ALPHA && dst == Q3BF_SRC_ALPHA) ||
        (src == Q3BF_ONE && dst == Q3BF_ONE_MINUS_SRC_ALPHA))
        return Q3BLEND_BLEND;

    return Q3BLEND_OTHER;
}

static void parse_blendfunc(q3stage_t *stage, const char **text)
{
    const char *tok = q3_parse(text, false);

    // the shorthands, spelled out the way the GL renderer does
    if (!Q_stricmp(tok, "add")) {
        stage->src_blend = Q3BF_ONE;
        stage->dst_blend = Q3BF_ONE;
    } else if (!Q_stricmp(tok, "filter")) {
        stage->src_blend = Q3BF_DST_COLOR;
        stage->dst_blend = Q3BF_ZERO;
    } else if (!Q_stricmp(tok, "blend")) {
        stage->src_blend = Q3BF_SRC_ALPHA;
        stage->dst_blend = Q3BF_ONE_MINUS_SRC_ALPHA;
    } else {
        stage->src_blend = blend_factor(tok);
        stage->dst_blend = blend_factor(q3_parse(text, false));
    }
    stage->blend = classify_blend(stage->src_blend, stage->dst_blend);
}

// A number; tolerates a parenthesis glued to it, "(0.5" or "0.5)".
static float parse_float(const char **text)
{
    const char *tok = q3_parse(text, false);
    while (*tok == '(')
        tok++;
    return tok[0] ? (float)atof(tok) : 0.f;
}

// "( a b c )", or the same without the parentheses.
static int parse_numbers(const char **text, float *out, int count)
{
    int n = 0;
    while (n < count) {
        const char *tok = q3_parse(text, false);
        if (!tok[0])
            break;
        while (*tok == '(')
            tok++;
        if (!tok[0] || *tok == ')')
            continue;
        out[n++] = (float)atof(tok);
    }
    return n;
}

static int wave_func(const char *s)
{
    if (!Q_stricmp(s, "sin"))             return Q3WAVE_SIN;
    if (!Q_stricmp(s, "triangle"))        return Q3WAVE_TRIANGLE;
    if (!Q_stricmp(s, "square"))          return Q3WAVE_SQUARE;
    if (!Q_stricmp(s, "sawtooth"))        return Q3WAVE_SAWTOOTH;
    if (!Q_stricmp(s, "inversesawtooth")) return Q3WAVE_INVSAWTOOTH;
    if (!Q_stricmp(s, "noise"))           return Q3WAVE_NOISE;
    return Q3WAVE_NONE;
}

// "func base amplitude phase frequency"
static void parse_wave(const char **text, q3wave_t *wave)
{
    wave->func = wave_func(q3_parse(text, false));
    wave->base = parse_float(text);
    wave->amp = parse_float(text);
    wave->phase = parse_float(text);
    wave->freq = parse_float(text);
}

/* "deformVertexes <kind> ...", read after the kind, as ParseDeform reads it:
     wave <div> <func> <base> <amp> <phase> <freq>
     bulge <width> <height> <speed>
     move <x> <y> <z> <func> <base> <amp> <phase> <freq>
   Only these move a surface over time; the rest leave nothing here. */
static void parse_deform(q3shader_t *s, const char *kind, const char **text)
{
    q3deform_t d = { 0 };

    if (!Q_stricmp(kind, "wave")) {
        float div = parse_float(text);
        d.type = Q3DEFORM_WAVE;
        d.spread = (div != 0.f) ? 1.f / div : 100.f;   // Quake III's answer to div 0
        parse_wave(text, &d.wave);
    } else if (!Q_stricmp(kind, "bulge")) {
        d.type = Q3DEFORM_BULGE;
        d.bulge_width = parse_float(text);
        d.bulge_height = parse_float(text);
        d.bulge_speed = parse_float(text);
    } else if (!Q_stricmp(kind, "move")) {
        d.type = Q3DEFORM_MOVE;
        d.move[0] = parse_float(text);
        d.move[1] = parse_float(text);
        d.move[2] = parse_float(text);
        parse_wave(text, &d.wave);
    } else {
        return;
    }

    if (s->num_deforms < Q3S_MAX_DEFORMS)
        s->deforms[s->num_deforms++] = d;
}

static void add_tcmod(q3stage_t *stage, const q3tcmod_t *mod)
{
    if (stage->num_tcmods < Q3S_MAX_TCMODS)
        stage->tcmods[stage->num_tcmods++] = *mod;
}

static void parse_tcmod(q3stage_t *stage, const char **text)
{
    const char *tok = q3_parse(text, false);
    q3tcmod_t mod = { 0 };

    if (!Q_stricmp(tok, "scroll")) {
        mod.type = Q3TCMOD_SCROLL;
        mod.v[0] = stage->scroll[0] = parse_float(text);
        mod.v[1] = stage->scroll[1] = parse_float(text);
    } else if (!Q_stricmp(tok, "scale")) {
        mod.type = Q3TCMOD_SCALE;
        mod.v[0] = stage->scale[0] = parse_float(text);
        mod.v[1] = stage->scale[1] = parse_float(text);
    } else if (!Q_stricmp(tok, "rotate")) {
        mod.type = Q3TCMOD_ROTATE;
        mod.v[0] = stage->rotate = parse_float(text);
    } else if (!Q_stricmp(tok, "stretch")) {
        mod.type = Q3TCMOD_STRETCH;
        parse_wave(text, &mod.wave);
    } else if (!Q_stricmp(tok, "transform")) {
        mod.type = Q3TCMOD_TRANSFORM;
        for (int i = 0; i < 6; i++)
            mod.v[i] = parse_float(text);
    } else if (!Q_stricmp(tok, "turb")) {
        // "turb base amp phase freq", base unused; some scripts leave it out
        float v[4];
        int n = parse_numbers(text, v, 4);
        mod.type = Q3TCMOD_TURB;
        mod.wave.func = Q3WAVE_SIN;
        if (n == 4) {
            mod.wave.base = v[0];
            mod.wave.amp = v[1];
            mod.wave.phase = v[2];
            mod.wave.freq = v[3];
        } else if (n == 3) {
            mod.wave.amp = v[0];
            mod.wave.phase = v[1];
            mod.wave.freq = v[2];
        }
        stage->turb = true;
    } else {
        return;     // entityTranslate and the like: nothing a map surface uses
    }

    add_tcmod(stage, &mod);
}

static void parse_rgbgen(q3stage_t *stage, const char **text)
{
    const char *tok = q3_parse(text, false);

    if (!Q_stricmp(tok, "wave")) {
        stage->rgbgen = Q3RGB_WAVE;
        stage->rgbgen_wave = true;
        parse_wave(text, &stage->rgb_wave);
    } else if (!Q_stricmp(tok, "const") || !Q_stricmp(tok, "constant")) {
        stage->rgbgen = Q3RGB_CONST;
        parse_numbers(text, stage->rgb_const, 3);
    } else if (!Q_stricmp(tok, "identity") || !Q_stricmp(tok, "identityLighting")) {
        stage->rgbgen = Q3RGB_IDENTITY;
    } else if (!Q_stricmp(tok, "vertex") || !Q_stricmp(tok, "exactVertex") ||
               !Q_stricmp(tok, "lightingDiffuse")) {
        stage->rgbgen = Q3RGB_LIGHTING;
    } else if (!Q_stricmp(tok, "oneMinusVertex")) {
        stage->rgbgen = Q3RGB_ONE_MINUS_LIGHTING;
    }
    // entity / oneMinusEntity: white on a map surface, the default
}

static void parse_alphagen(q3stage_t *stage, const char **text)
{
    const char *tok = q3_parse(text, false);

    if (!Q_stricmp(tok, "wave")) {
        stage->alphagen = Q3ALPHAGEN_WAVE;
        parse_wave(text, &stage->alpha_wave);
    } else if (!Q_stricmp(tok, "const") || !Q_stricmp(tok, "constant")) {
        stage->alphagen = Q3ALPHAGEN_CONST;
        stage->alpha_const = parse_float(text);
    } else if (!Q_stricmp(tok, "lightingSpecular")) {
        stage->alphagen = Q3ALPHAGEN_ZERO;
    }
    // portal (alpha by distance), vertex, entity: left as the texture's alpha
}

static void parse_stage(q3shader_t *s, q3stage_t *stage, const char **text)
{
    stage->scale[0] = stage->scale[1] = 1.f;
    stage->blend = Q3BLEND_OPAQUE;
    stage->src_blend = Q3BF_NONE;
    stage->dst_blend = Q3BF_NONE;

    while (1) {
        const char *tok = q3_parse(text, true);
        if (!tok[0]) {
            Com_DPrintf("%s: no matching '}' in a stage\n", s->name);
            return;
        }
        if (!strcmp(tok, "}"))
            return;

        if (!Q_stricmp(tok, "map") || !Q_stricmp(tok, "clampmap")) {
            if (!Q_stricmp(tok, "clampmap"))
                stage->clamp = true;
            tok = q3_parse(text, false);
            if (!Q_stricmp(tok, "$lightmap"))
                stage->lightmap = true;
            else if (!Q_stricmp(tok, "$whiteimage"))
                stage->white = true;
            Q_strlcpy(stage->map, tok, sizeof(stage->map));
        } else if (!Q_stricmp(tok, "animmap")) {
            stage->anim_freq = parse_float(text);
            stage->frames = Z_TagMallocz(sizeof(*stage->frames) * Q3S_MAX_ANIMFRAMES, TAG_GENERAL);
            while (1) {
                tok = q3_parse(text, false);
                if (!tok[0])
                    break;
                if (stage->num_frames < Q3S_MAX_ANIMFRAMES)
                    Q_strlcpy(stage->frames[stage->num_frames++], tok, MAX_QPATH);
            }
            if (stage->num_frames)
                Q_strlcpy(stage->map, stage->frames[0], sizeof(stage->map));
        } else if (!Q_stricmp(tok, "videomap")) {
            stage->video = true;
            tok = q3_parse(text, false);
            Q_strlcpy(stage->map, tok, sizeof(stage->map));
        } else if (!Q_stricmp(tok, "blendfunc")) {
            parse_blendfunc(stage, text);
        } else if (!Q_stricmp(tok, "alphafunc")) {
            tok = q3_parse(text, false);
            stage->alpha_test = true;
            if (!Q_stricmp(tok, "GT0"))
                stage->alpha_func = Q3ALPHA_GT0;
            else if (!Q_stricmp(tok, "LT128"))
                stage->alpha_func = Q3ALPHA_LT128;
            else
                stage->alpha_func = Q3ALPHA_GE128;
        } else if (!Q_stricmp(tok, "tcgen") || !Q_stricmp(tok, "texgen")) {
            tok = q3_parse(text, false);
            if (!Q_stricmp(tok, "environment")) {
                stage->env_map = true;
                stage->tcgen = Q3TCGEN_ENVIRONMENT;
            } else if (!Q_stricmp(tok, "lightmap")) {
                stage->tcgen = Q3TCGEN_LIGHTMAP;
            } else if (!Q_stricmp(tok, "vector")) {
                stage->tcgen = Q3TCGEN_VECTOR;
            }
        } else if (!Q_stricmp(tok, "rgbgen")) {
            parse_rgbgen(stage, text);
        } else if (!Q_stricmp(tok, "alphagen")) {
            parse_alphagen(stage, text);
        } else if (!Q_stricmp(tok, "tcmod")) {
            parse_tcmod(stage, text);
        } else if (!Q_stricmp(tok, "detail")) {
            stage->detail = true;
        }

        // whatever is left of the line - wave parameters and so on
        skip_rest_of_line(text);
    }
}

static int surfaceparm_flags(const char *name, int *contents, bool *trans)
{
    static const struct { const char *name; int surface; int contents; } parms[] = {
        { "water",          0,                      Q3_CONTENTS_WATER },
        { "slime",          0,                      Q3_CONTENTS_SLIME },
        { "lava",           0,                      Q3_CONTENTS_LAVA },
        { "playerclip",     0,                      Q3_CONTENTS_PLAYERCLIP },
        { "monsterclip",    0,                      Q3_CONTENTS_MONSTERCLIP },
        { "nodrop",         0,                      (int)Q3_CONTENTS_NODROP },
        { "nonsolid",       Q3_SURF_NONSOLID,       0 },
        { "origin",         0,                      Q3_CONTENTS_ORIGIN },
        { "trans",          0,                      Q3_CONTENTS_TRANSLUCENT },
        { "detail",         0,                      Q3_CONTENTS_DETAIL },
        { "structural",     0,                      Q3_CONTENTS_STRUCTURAL },
        { "areaportal",     0,                      Q3_CONTENTS_AREAPORTAL },
        { "clusterportal",  0,                      Q3_CONTENTS_CLUSTERPORTAL },
        { "donotenter",     0,                      Q3_CONTENTS_DONOTENTER },
        { "fog",            0,                      Q3_CONTENTS_FOG },
        { "sky",            Q3_SURF_SKY,            0 },
        { "lightfilter",    Q3_SURF_LIGHTFILTER,    0 },
        { "alphashadow",    Q3_SURF_ALPHASHADOW,    0 },
        { "hint",           Q3_SURF_HINT,           0 },
        { "slick",          Q3_SURF_SLICK,          0 },
        { "noimpact",       Q3_SURF_NOIMPACT,       0 },
        { "nomarks",        Q3_SURF_NOMARKS,        0 },
        { "ladder",         Q3_SURF_LADDER,         0 },
        { "nodamage",       Q3_SURF_NODAMAGE,       0 },
        { "metalsteps",     Q3_SURF_METALSTEPS,     0 },
        { "flesh",          Q3_SURF_FLESH,          0 },
        { "nosteps",        Q3_SURF_NOSTEPS,        0 },
        { "nodraw",         Q3_SURF_NODRAW,         0 },
        { "pointlight",     Q3_SURF_POINTLIGHT,     0 },
        { "nolightmap",     Q3_SURF_NOLIGHTMAP,     0 },
        { "nodlight",       Q3_SURF_NODLIGHT,       0 },
        { "dust",           Q3_SURF_DUST,           0 },
    };

    for (int i = 0; i < q_countof(parms); i++) {
        if (!Q_stricmp(name, parms[i].name)) {
            *contents |= parms[i].contents;
            if (!Q_stricmp(name, "trans"))
                *trans = true;
            return parms[i].surface;
        }
    }

    return 0;
}

static int parse_sort(const char *tok)
{
    if (!Q_stricmp(tok, "portal"))      return 1;
    if (!Q_stricmp(tok, "sky"))         return 2;
    if (!Q_stricmp(tok, "opaque"))      return 3;
    if (!Q_stricmp(tok, "decal"))       return 4;
    if (!Q_stricmp(tok, "seeThrough"))  return 5;
    if (!Q_stricmp(tok, "banner"))      return 6;
    if (!Q_stricmp(tok, "underwater"))  return 8;
    if (!Q_stricmp(tok, "additive"))    return 9;
    if (!Q_stricmp(tok, "nearest"))     return 16;
    return atoi(tok);
}

// Which stage is the colour, which glows, and what kind of surface that adds up
// to. Quake III draws the stages in order, each blended over the last; what a
// path tracer needs is the one image that is the surface's ALBEDO (what the
// lightmap multiplied) and the one that is LIGHT (what was added on top).
static void q3_analyze(q3shader_t *s)
{
    s->diffuse_stage = -1;
    s->emissive_stage = -1;

    // the first stage that is drawn at all decides whether the surface covers
    // what is behind it - with $lightmap first it always does
    const q3stage_t *first = s->num_stages ? &s->stages[0] : NULL;
    s->translucent = first && first->blend != Q3BLEND_OPAQUE;

    bool any_solid = false;
    for (int i = 0; i < s->num_stages; i++) {
        const q3stage_t *st = &s->stages[i];

        if (st->lightmap || st->white || st->video || !st->map[0])
            continue;

        if (st->blend == Q3BLEND_ADD) {
            // a reflection map is a highlight, not a light
            if (s->emissive_stage < 0 && !st->env_map)
                s->emissive_stage = i;
            continue;
        }

        if (st->env_map)
            continue;

        any_solid = true;

        if (s->diffuse_stage < 0) {
            s->diffuse_stage = i;
            continue;
        }

        // An opaque glow image drawn first, with the real surface blended over
        // it (base_light/runway): the glow is q3map_lightimage, so the colour
        // is the layer on top of it.
        const q3stage_t *cur = &s->stages[s->diffuse_stage];
        if (s->light_image[0] && !Q_stricmp(cur->map, s->light_image) &&
            (st->blend == Q3BLEND_BLEND || st->blend == Q3BLEND_FILTER)) {
            if (s->emissive_stage < 0)
                s->emissive_stage = s->diffuse_stage;
            s->diffuse_stage = i;
        }
    }

    s->additive = s->translucent && !any_solid && s->emissive_stage >= 0;

    if (s->diffuse_stage >= 0)
        s->alpha_test = s->stages[s->diffuse_stage].alpha_test;

    // What moves: anything that scrolls, turns, warps, flips frames or pulses.
    for (int i = 0; i < s->num_stages; i++) {
        const q3stage_t *st = &s->stages[i];
        if (st->num_tcmods > 0 || st->num_frames > 1 ||
            st->rgbgen == Q3RGB_WAVE || st->alphagen == Q3ALPHAGEN_WAVE)
            s->animated = true;
    }

    // Chrome: an opaque reflection map first, then nothing but light added on
    // top or the lightmap multiplied in (chrome_metal). The reflection map is
    // Quake III faking a mirror finish, which a path tracer has for real.
    if (first && first->env_map && !first->lightmap && first->blend == Q3BLEND_OPAQUE &&
        !s->sky && !s->portal) {
        bool rest_ok = true;
        for (int i = 1; i < s->num_stages; i++) {
            const q3stage_t *st = &s->stages[i];
            if (st->blend != Q3BLEND_ADD && st->blend != Q3BLEND_FILTER)
                rest_ok = false;
        }
        s->chrome = rest_ok;
    }

    // Fake effects, drawn for real by the path tracer and so not drawn at all:
    //  - a lens flare: an additive sprite ("flare" in its name or image) - the
    //    lamp it sits on is a light already;
    //  - a light shaft: additive, non-solid cards ("beam") standing in for the
    //    volumetric light the fog draws from the lamp itself.
    if (s->additive) {
        bool all_add = true;
        bool flare = Q_stristr(s->name, "flare") != NULL;
        bool beam = Q_stristr(s->name, "beam") != NULL;
        for (int i = 0; i < s->num_stages; i++) {
            const q3stage_t *st = &s->stages[i];
            if (st->blend != Q3BLEND_ADD)
                all_add = false;
            if (Q_stristr(st->map, "flare"))
                flare = true;
            if (Q_stristr(st->map, "beam"))
                beam = true;
        }
        if (all_add && (flare || (beam && (s->surface_flags & Q3_SURF_NONSOLID))))
            s->fake_effect = true;
    }

    // Nothing to draw: no colour, no glow - and no reflection map either. A
    // shader of reflection maps alone has neither a colour nor a glow stage
    // (q3_analyze counts a reflection as neither), but it is the whole of
    // Quake III's glass (glass01, the kmlamp_white bulb, the q3dm7 lamp
    // globe) and of the electric tube round q3dm0's tesla coil
    // (proto_zzztblu3). Dropping those left the lamps with no bulb and the
    // windows empty. vkpt material.c draws them: still ones as clear glass,
    // moving ones as a glow over what is behind them.
    bool any_env = false;
    for (int i = 0; i < s->num_stages; i++) {
        const q3stage_t *st = &s->stages[i];
        if (st->env_map && st->map[0] && !st->lightmap && !st->white && !st->video)
            any_env = true;
    }

    s->no_draw = (s->surface_flags & Q3_SURF_NODRAW) ||
                 (s->content_flags & Q3_CONTENTS_FOG) ||
                 s->fake_effect ||
                 (s->diffuse_stage < 0 && s->emissive_stage < 0 && !any_env && !s->sky);
}

/*
===============================================================================

WAVES

Quake III's tables (tr_init.c R_InitFunctionTables) are 1024 entries indexed by
(phase + time * frequency) * 1024, wrapped; the same values computed directly.

===============================================================================
*/

// smooth 1D value noise in -1..1, standing in for R_NoiseGet4f
static float wave_noise(float x)
{
    float i = floorf(x);
    float f = x - i;
    unsigned a = (unsigned)(int)i * 2654435761u;
    unsigned b = (unsigned)((int)i + 1) * 2654435761u;
    float va = (float)((a >> 8) & 0xffff) / 32767.5f - 1.f;
    float vb = (float)((b >> 8) & 0xffff) / 32767.5f - 1.f;
    f = f * f * (3.f - 2.f * f);
    return va + (vb - va) * f;
}

float Q3Shader_EvalWave(const q3wave_t *wave, float time)
{
    if (!wave)
        return 0.f;

    if (wave->func == Q3WAVE_NOISE)
        return wave->base + wave_noise((time + wave->phase) * wave->freq) * wave->amp;

    float x = wave->phase + time * wave->freq;
    x -= floorf(x);     // 0..1, one period

    float v;
    switch (wave->func) {
    case Q3WAVE_SIN:
        v = sinf(x * 2.f * (float)M_PI);
        break;
    case Q3WAVE_TRIANGLE:
        // 0 -> 1 -> 0 -> -1 -> 0 over the period
        if (x < 0.25f)      v = x * 4.f;
        else if (x < 0.5f)  v = 1.f - (x - 0.25f) * 4.f;
        else if (x < 0.75f) v = -(x - 0.5f) * 4.f;
        else                v = -1.f + (x - 0.75f) * 4.f;
        break;
    case Q3WAVE_SQUARE:
        v = x < 0.5f ? 1.f : -1.f;
        break;
    case Q3WAVE_SAWTOOTH:
        v = x;
        break;
    case Q3WAVE_INVSAWTOOTH:
        v = 1.f - x;
        break;
    default:
        return wave->base;
    }

    return wave->base + v * wave->amp;
}

static q3shader_t *parse_shader(const q3entry_t *e)
{
    q3shader_t *s = Z_TagMallocz(sizeof(*s), TAG_GENERAL);
    const char *text = e->body;
    const char *tok;

    Q_strlcpy(s->name, e->name, sizeof(s->name));
    Q_strlcpy(s->file, q3_files[e->file].path, sizeof(s->file));
    s->line = e->line;
    s->cull = Q3CULL_FRONT;

    q3_line = e->line;

    tok = q3_parse(&text, true);
    if (strcmp(tok, "{"))
        return s;

    while (1) {
        tok = q3_parse(&text, true);
        if (!tok[0]) {
            Com_DPrintf("%s: no concluding '}'\n", s->name);
            break;
        }
        if (!strcmp(tok, "}"))
            break;

        if (!strcmp(tok, "{")) {
            if (s->num_stages < Q3S_MAX_STAGES) {
                parse_stage(s, &s->stages[s->num_stages], &text);
                s->num_stages++;
            } else {
                skip_braced_section(&text, 1);
            }
            continue;
        }

        if (!Q_stricmp(tok, "surfaceparm")) {
            tok = q3_parse(&text, false);
            s->surface_flags |= surfaceparm_flags(tok, &s->content_flags, &s->trans);
        } else if (!Q_stricmp(tok, "q3map_surfacelight")) {
            s->surface_light = parse_float(&text);
        } else if (!Q_stricmp(tok, "q3map_lightimage")) {
            tok = q3_parse(&text, false);
            Q_strlcpy(s->light_image, tok, sizeof(s->light_image));
        } else if (!Q_stricmp(tok, "q3map_sun")) {
            vec3_t c;
            c[0] = parse_float(&text);
            c[1] = parse_float(&text);
            c[2] = parse_float(&text);
            VectorNormalize(c);
            VectorCopy(c, s->sun_color);
            s->sun_intensity = parse_float(&text);
            s->sun_degrees = parse_float(&text);
            s->sun_elevation = parse_float(&text);
            s->has_sun = true;
        } else if (!Q_stricmp(tok, "skyparms")) {
            tok = q3_parse(&text, false);
            if (strcmp(tok, "-"))
                Q_strlcpy(s->sky_farbox, tok, sizeof(s->sky_farbox));
            tok = q3_parse(&text, false);
            s->sky_cloud_height = (tok[0] && strcmp(tok, "-")) ? (float)atof(tok) : 512.f;
            s->sky = true;
        } else if (!Q_stricmp(tok, "cull")) {
            tok = q3_parse(&text, false);
            if (!Q_stricmp(tok, "none") || !Q_stricmp(tok, "twosided") || !Q_stricmp(tok, "disable"))
                s->cull = Q3CULL_NONE;
            else if (!Q_stricmp(tok, "back") || !Q_stricmp(tok, "backside") || !Q_stricmp(tok, "backsided"))
                s->cull = Q3CULL_BACK;
        } else if (!Q_stricmp(tok, "portal")) {
            s->portal = true;
            s->sort = 1;
        } else if (!Q_stricmp(tok, "sort")) {
            s->sort = parse_sort(q3_parse(&text, false));
        } else if (!Q_stricmp(tok, "deformvertexes")) {
            tok = q3_parse(&text, false);
            if (!Q_stricmpn(tok, "autosprite", 10)) {
                s->deform_autosprite = true;
            } else {
                s->deform_wave = true;
                parse_deform(s, tok, &text);
            }
        } else if (!Q_stricmp(tok, "fogparms")) {
            // "( r g b ) distance", or the older form without the parentheses
            vec3_t c;
            int n = 0;
            while (n < 3) {
                tok = q3_parse(&text, false);
                if (!tok[0])
                    break;
                if (!strcmp(tok, "(") || !strcmp(tok, ")"))
                    continue;
                c[n++] = (float)atof(tok);
            }
            if (n == 3) {
                tok = q3_parse(&text, false);
                if (!strcmp(tok, ")"))
                    tok = q3_parse(&text, false);
                VectorCopy(c, s->fog_color);
                s->fog_distance = (float)atof(tok);
                s->has_fog = true;
            }
        }

        skip_rest_of_line(&text);
    }

    q3_analyze(s);
    return s;
}

const q3shader_t *Q3Shader_Find(const char *name)
{
    char key[MAX_QPATH];

    if (!q3_num_shaders || !name || !name[0])
        return NULL;

    strip_extension_lower(name, key, sizeof(key));

    q3entry_t *e = find_entry(key);
    if (!e)
        return NULL;

    if (!e->parsed)
        e->parsed = parse_shader(e);

    return e->parsed;
}

bool Q3Shader_ResolveImage(const char *path, char *out, size_t size)
{
    static const char *const exts[] = { ".tga", ".jpg", ".png" };
    char base[MAX_OSPATH];   // a .mat stage<N>_map may be longer than MAX_QPATH

    if (!path || !path[0] || path[0] == '$' || path[0] == '*')
        return false;

    Q_strlcpy(base, path, sizeof(base));
    for (char *p = base; *p; p++)
        if (*p == '\\')
            *p = '/';

    char named[MAX_OSPATH];  // as written, extension and all
    Q_strlcpy(named, base, sizeof(named));

    char *dot = strrchr(base, '.');
    char *slash = strrchr(base, '/');
    if (dot && (!slash || dot > slash))
        *dot = 0;

    for (int i = 0; i < q_countof(exts); i++) {
        char candidate[MAX_OSPATH];
        if (Q_concat(candidate, sizeof(candidate), base, exts[i]) >= sizeof(candidate))
            continue;
        if (FS_FileExists(candidate)) {
            Q_strlcpy(out, candidate, size);
            return true;
        }
    }

    /* A .mat line into materialDev/ (quake3.mat's stage<N>_map) names a copy
       q3matdev.py extracted from the paks, at the pak's own path. Only the
       copies that were edited are in git, so on a checkout without the rest
       the pak file the copy came from is the same picture: its exact name
       first, then the other formats. */
    if (!Q_strncasecmp(named, "materialDev/", 12) && named[12]) {
        if (strchr(COM_SkipPath(named), '.') && FS_FileExists(named + 12)) {
            Q_strlcpy(out, named + 12, size);
            return true;
        }
        return Q3Shader_ResolveImage(named + 12, out, size);
    }

    return false;
}

/*
===============================================================================

CONSOLE

===============================================================================
*/

static const char *blend_name(q3blend_t b)
{
    switch (b) {
    case Q3BLEND_OPAQUE: return "opaque";
    case Q3BLEND_FILTER: return "filter";
    case Q3BLEND_BLEND:  return "blend";
    case Q3BLEND_ADD:    return "add";
    default:             return "other";
    }
}

// q3shader <name>: what this renderer made of a shader - for working out why a
// Quake III surface draws the way it does before writing a .mat entry for it.
static void Q3Shader_Print_f(void)
{
    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s <shader name>\n"
                   "%d shaders from %d scripts\n", Cmd_Argv(0), q3_num_shaders, q3_num_files);
        return;
    }

    Q3Shader_Load();

    const q3shader_t *s = Q3Shader_Find(Cmd_Argv(1));
    if (!s) {
        Com_Printf("No script defines '%s' - it would draw as a plain opaque image.\n", Cmd_Argv(1));
        return;
    }

    Com_Printf("%s  (%s:%d)\n", s->name, s->file, s->line);
    Com_Printf("  surface 0x%x  contents 0x%x  trans %d  cull %d  sort %d  portal %d\n",
               s->surface_flags, s->content_flags, s->trans, s->cull, s->sort, s->portal);
    if (s->surface_light)
        Com_Printf("  surfacelight %g  lightimage %s\n", s->surface_light, s->light_image);
    if (s->sky)
        Com_Printf("  sky farbox '%s' cloudheight %g\n", s->sky_farbox, s->sky_cloud_height);
    if (s->has_sun)
        Com_Printf("  sun (%g %g %g) x %g at %g deg, elevation %g\n", s->sun_color[0], s->sun_color[1],
                   s->sun_color[2], s->sun_intensity, s->sun_degrees, s->sun_elevation);
    if (s->has_fog)
        Com_Printf("  fog (%g %g %g) distance %g\n", s->fog_color[0], s->fog_color[1], s->fog_color[2], s->fog_distance);
    for (int i = 0; i < s->num_stages; i++) {
        const q3stage_t *st = &s->stages[i];
        Com_Printf("  stage %d: %s  blend %s%s%s%s%s  scroll %g %g  scale %g %g\n", i, st->map,
                   blend_name(st->blend), st->env_map ? " env" : "", st->alpha_test ? " alphatest" : "",
                   st->num_frames ? " anim" : "", st->rgbgen_wave ? " wave" : "",
                   st->scroll[0], st->scroll[1], st->scale[0], st->scale[1]);
    }
    Com_Printf("  -> diffuse stage %d, emissive stage %d%s%s%s%s%s%s%s\n", s->diffuse_stage, s->emissive_stage,
               s->translucent ? ", translucent" : "", s->additive ? ", additive" : "",
               s->alpha_test ? ", alpha tested" : "", s->animated ? ", animated" : "",
               s->chrome ? ", chrome" : "", s->fake_effect ? ", fake flare/beam" : "",
               s->no_draw ? ", NOT DRAWN" : "");
}

static void Q3Shader_Reload_f(void)
{
    Q3Shader_Shutdown();
    Q3Shader_Load();
    Com_Printf("%d Quake III shaders from %d scripts\n", q3_num_shaders, q3_num_files);
}

void Q3Shader_Init(void)
{
    Cmd_AddCommand("q3shader", Q3Shader_Print_f);
    Cmd_AddCommand("q3shader_reload", Q3Shader_Reload_f);
}
