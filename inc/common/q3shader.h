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

#ifndef Q3SHADER_H
#define Q3SHADER_H

//
// q3shader.c -- reading Quake III Arena's scripts/*.shader
//
// Quake III describes every surface with a "shader": a short script naming the
// images it is drawn from, how they blend, how they scroll, and what the
// compiler and the game should make of it (surfaceparm sky, q3map_surfacelight
// and so on). This renderer is a path tracer, so a shader is not executed. It
// is READ for the facts a material needs - which stage is the surface colour,
// which one glows, whether it is see-through, a liquid, a sky, a light - and
// those become the defaults of the material that a .mat entry can still
// override. See material.c (q3_material_definition) and bsp_q3.c.
//

#define Q3S_MAX_STAGES      8
#define Q3S_MAX_ANIMFRAMES  8

typedef enum {
    Q3BLEND_OPAQUE,     // no blendFunc, or GL_ONE GL_ZERO
    Q3BLEND_FILTER,     // multiplies what is behind it (lightmap style)
    Q3BLEND_BLEND,      // alpha-blends over what is behind it
    Q3BLEND_ADD,        // adds light to what is behind it
    Q3BLEND_OTHER
} q3blend_t;

enum {
    Q3ALPHA_NONE,
    Q3ALPHA_GT0,        // alpha > 0 is drawn
    Q3ALPHA_LT128,      // alpha < 0.5 is drawn
    Q3ALPHA_GE128       // alpha >= 0.5 is drawn
};

enum {
    Q3CULL_FRONT,       // the default: back faces culled
    Q3CULL_BACK,
    Q3CULL_NONE         // "cull none / disable / twosided"
};

// GL blend factors, as the scripts spell them (blendFunc)
enum {
    Q3BF_NONE,
    Q3BF_ONE,
    Q3BF_ZERO,
    Q3BF_DST_COLOR,
    Q3BF_ONE_MINUS_DST_COLOR,
    Q3BF_SRC_ALPHA,
    Q3BF_ONE_MINUS_SRC_ALPHA,
    Q3BF_DST_ALPHA,
    Q3BF_ONE_MINUS_DST_ALPHA,
    Q3BF_SRC_COLOR,
    Q3BF_ONE_MINUS_SRC_COLOR,
    Q3BF_SRC_ALPHA_SATURATE
};

// waveform functions (rgbGen wave, alphaGen wave, tcMod stretch)
enum {
    Q3WAVE_NONE,
    Q3WAVE_SIN,
    Q3WAVE_TRIANGLE,
    Q3WAVE_SQUARE,
    Q3WAVE_SAWTOOTH,
    Q3WAVE_INVSAWTOOTH,
    Q3WAVE_NOISE
};

typedef struct {
    int         func;               // Q3WAVE_*
    float       base, amp, phase, freq;
} q3wave_t;

// rgbGen
enum {
    Q3RGB_DEFAULT,      // not stated: identity (identityLighting is the same here)
    Q3RGB_IDENTITY,
    Q3RGB_CONST,
    Q3RGB_WAVE,
    Q3RGB_LIGHTING,     // vertex, exactVertex, lightingDiffuse: the light itself
    Q3RGB_ONE_MINUS_LIGHTING
};

// alphaGen
enum {
    Q3ALPHAGEN_DEFAULT, // 1, or the texture's own alpha
    Q3ALPHAGEN_CONST,
    Q3ALPHAGEN_WAVE,
    Q3ALPHAGEN_ZERO     // lightingSpecular: a fake highlight the path tracer makes for real
};

// tcGen
enum {
    Q3TCGEN_BASE,
    Q3TCGEN_LIGHTMAP,
    Q3TCGEN_ENVIRONMENT,
    Q3TCGEN_VECTOR
};

// tcMod, applied in the order the script lists them
enum {
    Q3TCMOD_SCROLL,     // v[0..1] units per second
    Q3TCMOD_SCALE,      // v[0..1]
    Q3TCMOD_ROTATE,     // v[0] degrees per second
    Q3TCMOD_STRETCH,    // wave
    Q3TCMOD_TRANSFORM,  // v[0..5]: m00 m01 m10 m11 t0 t1
    Q3TCMOD_TURB        // wave: base unused, amp, phase, freq
};

#define Q3S_MAX_TCMODS  4

typedef struct {
    int         type;               // Q3TCMOD_*
    float       v[6];
    q3wave_t    wave;
} q3tcmod_t;

typedef struct {
    char        map[MAX_QPATH];     // image, "$lightmap", "$whiteimage", or animMap frame 0
    int         num_frames;         // animMap frames, 0 if not animated
    char        (*frames)[MAX_QPATH];
    float       anim_freq;
    q3blend_t   blend;
    bool        lightmap;           // map $lightmap
    bool        white;              // map $whiteimage
    bool        video;              // videoMap
    bool        clamp;              // clampMap
    bool        env_map;            // tcGen environment
    bool        alpha_test;         // alphaFunc
    int         alpha_func;         // Q3ALPHA_*
    bool        detail;
    bool        rgbgen_wave;        // rgbGen wave: a flickering or pulsing layer
    float       scroll[2];          // tcMod scroll, texture widths per second
    float       scale[2];           // tcMod scale
    bool        turb;               // tcMod turb
    float       rotate;             // tcMod rotate, degrees per second

    // The whole stage, for drawing it (vkpt q3stages.c) rather than reading it.
    int         src_blend, dst_blend;   // Q3BF_*, Q3BF_NONE when no blendFunc
    int         rgbgen;             // Q3RGB_*
    vec3_t      rgb_const;
    q3wave_t    rgb_wave;
    int         alphagen;           // Q3ALPHAGEN_*
    float       alpha_const;
    q3wave_t    alpha_wave;
    int         tcgen;              // Q3TCGEN_*
    int         num_tcmods;
    q3tcmod_t   tcmods[Q3S_MAX_TCMODS];
} q3stage_t;

// deformVertexes that move a surface over time, as RB_DeformTessGeometry does
// (vkpt q3deform.c draws them). normal, autosprite, text and projectionShadow
// are not kept here.
enum {
    Q3DEFORM_WAVE = 1,  // along the normal by a wave whose phase runs with x+y+z
    Q3DEFORM_BULGE,     // along the normal by a sine running along the texture's s
    Q3DEFORM_MOVE       // the whole surface along a vector by a wave
};

#define Q3S_MAX_DEFORMS 3

typedef struct {
    int         type;               // Q3DEFORM_*
    float       spread;             // wave: 1 / div, phase per unit of x+y+z
    q3wave_t    wave;               // wave, move
    float       bulge_width;        // bulge: radians per unit of s
    float       bulge_height;       // bulge: units along the normal
    float       bulge_speed;        // bulge: radians per second
    vec3_t      move;               // move: the direction and scale
} q3deform_t;

typedef struct q3shader_s {
    char        name[MAX_QPATH];    // lower case, no extension
    char        file[MAX_QPATH];    // the script it came from, "" for an implicit shader
    int         line;               // line of the name in that script

    int         surface_flags;      // Q3_SURF_* from its surfaceparms
    int         content_flags;      // Q3_CONTENTS_* from its surfaceparms
    bool        trans;              // surfaceparm trans

    float       surface_light;      // q3map_surfacelight
    char        light_image[MAX_QPATH];

    bool        has_sun;            // q3map_sun
    vec3_t      sun_color;          // normalised
    float       sun_intensity;
    float       sun_degrees;        // 0 = from the east, 90 = from the north
    float       sun_elevation;      // 0 = horizon, 90 = straight down

    bool        sky;                // skyparms present
    char        sky_farbox[MAX_QPATH];  // "env/space1", or "" for "-"
    float       sky_cloud_height;

    int         cull;               // Q3CULL_*
    bool        portal;             // "portal" keyword (mirror or portal view)
    int         sort;               // 0 when not stated

    bool        has_fog;            // fogparms
    vec3_t      fog_color;
    float       fog_distance;

    bool        deform_autosprite;  // autosprite / autosprite2
    bool        deform_wave;        // wave / bulge / move / normal
    int         num_deforms;        // the moving ones, in script order
    q3deform_t  deforms[Q3S_MAX_DEFORMS];

    int         num_stages;
    q3stage_t   stages[Q3S_MAX_STAGES];

    // What the stages add up to - filled in by the parser, see q3_analyze.
    int         diffuse_stage;      // the surface colour, -1 if none
    int         emissive_stage;     // the glow, -1 if none
    bool        translucent;        // the first stage blends: see-through
    bool        additive;           // ONLY glow: an additive effect surface
    bool        alpha_test;         // the colour stage has alphaFunc
    bool        no_draw;            // nothing to draw (nodraw, fog, no stages)

    // a FAKE effect the path tracer does for real, so not drawn at all: a lens
    // flare sprite or a light beam/shaft made of additive cards (q3_analyze)
    bool        fake_effect;

    // An environment-mapped chrome: an opaque tcGen environment stage first,
    // and only glows / detail / the lightmap after it. Drawn as a real mirror
    // metal (vkpt material.c).
    bool        chrome;

    // anything that moves or changes over time: tcMod, animMap, wave colours
    bool        animated;
} q3shader_t;

// Quake III's waveform tables, for evaluating a q3wave_t at a time in seconds.
float Q3Shader_EvalWave(const q3wave_t *wave, float time);

// True if a "deformVertexes move" carries the whole surface somewhere over time
// (the flying skull lamps bob this way).
bool Q3Shader_Travels(const q3shader_t *s);

// An additive glow, not a sprite, that travels with its lamp (Q3Shader_Travels):
// bot_flare2, the lamp's jets. Drawn as a q3deform model with no light polys
// (they would stay where the map put them), and never taken for a fake flare.
bool Q3Shader_IsTravellingGlow(const q3shader_t *s);

// How far the shader's move deforms have carried a surface at `time` (seconds,
// the clock vkpt q3deform.c animates with: refdef time). Wave/bulge deforms,
// which bend a surface along its normal, are not part of it.
void Q3Shader_MoveOffset(const q3deform_t *deforms, int num_deforms, float time, vec3_t out);

// Registers the q3shader_* console commands.
void Q3Shader_Init(void);

// Reads every scripts/*.shader on the search path, once. Cheap after the first
// call: it only indexes the text, and a shader is parsed when it is looked up.
// Does nothing - and Q3Shader_Find always fails - if there are no scripts.
void Q3Shader_Load(void);

// Drops everything read, so the next Q3Shader_Load reads the scripts again.
void Q3Shader_Shutdown(void);

// True once Q3Shader_Load has found at least one script.
bool Q3Shader_Available(void);

// The shader of that name (any case, extension ignored), or NULL if no script
// defines one. An image with no script is an "implicit" shader in Quake III;
// this returns NULL for those and callers treat them as a plain opaque image.
const q3shader_t *Q3Shader_Find(const char *name);

// Quake III names images ".tga" in its scripts but ships many of them as .jpg,
// and the engine silently tried the other. Writes the path of the file that
// exists (.tga, .jpg or .png, in that order) to `out`; false if none does.
bool Q3Shader_ResolveImage(const char *path, char *out, size_t size);

#endif // Q3SHADER_H
