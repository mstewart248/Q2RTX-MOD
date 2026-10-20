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
// reverb.c -- room reverb for the OpenAL backend (EFX)
//
// The rerelease gives every space an echo that suits it: a tight corridor rings
// like a pipe, a big metal hall booms, outdoors is dry. Nothing in a map says
// which is which. The KEX engine works it out from the space around the
// listener and a table, sound/default.environments, that it ships as data:
//
//   { "environments": [
//       { "dimension": 500.0,
//         "reverbs": [ { "materials": [ "boot", "mech", "clank" ], "preset": "stone_room" },
//                      { "materials": "*", "preset": "living_room" } ] },
//       ... ] }
//
// An environment covers every space up to its "dimension"; within it, the first
// reverb whose "materials" (footstep material words - textures/<name>.mat, the
// same words footsteps.c uses) include what the space is made of wins, and "*"
// matches anything. "preset" names an EAX reverb preset.
//
// This reads that same file and the same preset names. What it cannot copy is
// how KEX measures a space, which is not documented anywhere, so:
//
//   size      rays go out from the listener along the 13 axes of a cube (faces,
//             edges, corners) in both directions; each pair is one chord of the
//             space through the listener, and the size is the MEDIAN chord. The
//             median rather than the mean so one long sightline down a corridor
//             does not make the corridor a hall. A ray that reaches sky or runs
//             out counts as open, so with sky overhead most chords that lean up
//             are long and the space reads as outdoors.
//   material  the footstep word most of those rays hit.
//
// Under water the "underwater" preset applies regardless of the table. Changes
// fade over s_reverb_lerptime; going in or out of water switches at once.
// Interface sounds, radio voices and the music stay dry - see AL_ReverbChannel.
//
// maps/<map>.environments, if present, REPLACES the default table for that map.
// KEX can also merge the two ("merge" / "additive"); no map ships one, so only
// replacement is done. Custom reverbs given as raw I3DL2 numbers, and the I3DL2
// presets (small_room, medium_hall, plate...), are not supported - only the
// names in rv_presets below, which cover every one default.environments uses.
//
// OpenAL only: the software mixer (s_enable 1) has no effects.
//

#include "sound.h"

#if USE_FIXED_LIBAL
#include "qal/fixed.h"
#else
#include "qal/dynamic.h"
#endif

#include <AL/efx.h>
#include <AL/efx-presets.h>

#define RV_MAX_ENVS         16
#define RV_MAX_REVERBS      8
#define RV_MAX_MATERIALS    8

#define RV_MAX_DIST         2048    // how far a ray looks before calling it open
#define RV_PROBE_MSEC       100

static cvar_t   *s_reverb;
static cvar_t   *s_reverb_lerptime;
static cvar_t   *s_reverb_debug;

static const struct {
    const char              *name;
    EFXEAXREVERBPROPERTIES  props;
} rv_presets[] = {
    { "generic",            EFX_REVERB_PRESET_GENERIC },
    { "padded_cell",        EFX_REVERB_PRESET_PADDEDCELL },
    { "room",               EFX_REVERB_PRESET_ROOM },
    { "bathroom",           EFX_REVERB_PRESET_BATHROOM },
    { "living_room",        EFX_REVERB_PRESET_LIVINGROOM },
    { "stone_room",         EFX_REVERB_PRESET_STONEROOM },
    { "auditorium",         EFX_REVERB_PRESET_AUDITORIUM },
    { "concert_hall",       EFX_REVERB_PRESET_CONCERTHALL },
    { "cave",               EFX_REVERB_PRESET_CAVE },
    { "arena",              EFX_REVERB_PRESET_ARENA },
    { "hangar",             EFX_REVERB_PRESET_HANGAR },
    { "carpeted_hallway",   EFX_REVERB_PRESET_CARPETEDHALLWAY },
    { "hallway",            EFX_REVERB_PRESET_HALLWAY },
    { "stone_corridor",     EFX_REVERB_PRESET_STONECORRIDOR },
    { "alley",              EFX_REVERB_PRESET_ALLEY },
    { "forest",             EFX_REVERB_PRESET_FOREST },
    { "city",               EFX_REVERB_PRESET_CITY },
    { "mountains",          EFX_REVERB_PRESET_MOUNTAINS },
    { "quarry",             EFX_REVERB_PRESET_QUARRY },
    { "plain",              EFX_REVERB_PRESET_PLAIN },
    { "parking_lot",        EFX_REVERB_PRESET_PARKINGLOT },
    { "sewer_pipe",         EFX_REVERB_PRESET_SEWERPIPE },
    { "underwater",         EFX_REVERB_PRESET_UNDERWATER },
};

// every member up to iDecayHFLimit is a float, and those are what fade
#define RV_NUM_FLOATS   (offsetof(EFXEAXREVERBPROPERTIES, iDecayHFLimit) / sizeof(float))

typedef struct {
    int     preset;                                 // index into rv_presets
    int     num_materials;                          // 0 = "*", anything
    char    materials[RV_MAX_MATERIALS][16];
} rv_reverb_t;

typedef struct {
    float       dimension;
    int         num_reverbs;
    rv_reverb_t reverbs[RV_MAX_REVERBS];
} rv_env_t;

static struct {
    LPALGENEFFECTS                  GenEffects;
    LPALDELETEEFFECTS               DeleteEffects;
    LPALEFFECTI                     Effecti;
    LPALEFFECTF                     Effectf;
    LPALEFFECTFV                    Effectfv;
    LPALGENAUXILIARYEFFECTSLOTS     GenAuxiliaryEffectSlots;
    LPALDELETEAUXILIARYEFFECTSLOTS  DeleteAuxiliaryEffectSlots;
    LPALAUXILIARYEFFECTSLOTI        AuxiliaryEffectSloti;
    LPALAUXILIARYEFFECTSLOTF        AuxiliaryEffectSlotf;
    LPALGENFILTERS                  GenFilters;
    LPALDELETEFILTERS               DeleteFilters;
    LPALFILTERI                     Filteri;
    LPALFILTERF                     Filterf;
} efx;

static struct {
    bool        available;
    ALuint      effect;
    ALuint      slot;
    ALuint      mute;                   // a filter that passes nothing, 0 = none
    float       gain;                   // slot gain last set, -1 = not yet

    char        mapname[MAX_QPATH];     // whose table is loaded
    int         num_envs;
    rv_env_t    envs[RV_MAX_ENVS];      // sorted by dimension

    int         target;                 // preset fading towards, -1 = none yet
    bool        fading;
    unsigned    fade_start;
    EFXEAXREVERBPROPERTIES  from, current;

    unsigned    next_probe;
} rv;

static int RV_FindPreset(const char *name)
{
    for (int i = 0; i < (int)q_countof(rv_presets); i++)
        if (!Q_stricmp(rv_presets[i].name, name))
            return i;
    return -1;
}

/*
==============================================================================

JSON

The smallest reader that takes the .environments files: values parse into a
tree in a fixed pool, strings are unescaped and terminated in place in the
loaded buffer. Nothing else in the engine reads JSON.

==============================================================================
*/

typedef enum {
    JS_LITERAL,     // true, false, null - parsed, never read
    JS_NUMBER,
    JS_STRING,
    JS_ARRAY,
    JS_OBJECT
} jstype_t;

typedef struct jsnode_s {
    jstype_t            type;
    const char          *key;       // member name, inside an object
    const char          *string;
    float               number;
    struct jsnode_s     *child;     // first element or member
    struct jsnode_s     *next;
} jsnode_t;

#define JS_MAX_NODES    1024
#define JS_MAX_DEPTH    16

static struct {
    char        *p;
    const char  *error;
    int         num_nodes;
    jsnode_t    nodes[JS_MAX_NODES];
} js;

static void JS_SkipSpace(void)
{
    while (*js.p && (unsigned char)*js.p <= ' ')
        js.p++;
}

static char *JS_ParseString(void)
{
    char    *start, *out;

    if (*js.p != '"') {
        js.error = "expected a string";
        return NULL;
    }

    start = out = ++js.p;
    while (*js.p != '"') {
        if (!*js.p) {
            js.error = "unterminated string";
            return NULL;
        }
        if (*js.p == '\\' && js.p[1]) {
            js.p++;
            // \n and \t aside, keep the escaped character itself - right for
            // \" \\ \/, and the files are plain ASCII, so \uXXXX never comes up
            *out++ = *js.p == 'n' ? '\n' : *js.p == 't' ? '\t' : *js.p;
            js.p++;
            continue;
        }
        *out++ = *js.p++;
    }

    js.p++;
    *out = 0;
    return start;
}

static jsnode_t *JS_ParseValue(int depth)
{
    jsnode_t    *node, *child, **tail;
    const char  *key;
    char        *end, close;
    bool        object;

    if (depth > JS_MAX_DEPTH) {
        js.error = "nested too deeply";
        return NULL;
    }
    if (js.num_nodes == JS_MAX_NODES) {
        js.error = "too many values";
        return NULL;
    }

    node = &js.nodes[js.num_nodes++];
    memset(node, 0, sizeof(*node));

    JS_SkipSpace();
    switch (*js.p) {
    case '{':
    case '[':
        object = *js.p == '{';
        close = object ? '}' : ']';
        node->type = object ? JS_OBJECT : JS_ARRAY;
        tail = &node->child;
        js.p++;

        JS_SkipSpace();
        if (*js.p == close) {
            js.p++;
            return node;
        }

        while (1) {
            key = NULL;
            if (object) {
                JS_SkipSpace();
                if (!(key = JS_ParseString()))
                    return NULL;
                JS_SkipSpace();
                if (*js.p != ':') {
                    js.error = "expected ':'";
                    return NULL;
                }
                js.p++;
            }

            if (!(child = JS_ParseValue(depth + 1)))
                return NULL;
            child->key = key;
            *tail = child;
            tail = &child->next;

            JS_SkipSpace();
            if (*js.p == ',') {
                js.p++;
                continue;
            }
            if (*js.p == close) {
                js.p++;
                return node;
            }
            js.error = object ? "expected ',' or '}'" : "expected ',' or ']'";
            return NULL;
        }

    case '"':
        node->type = JS_STRING;
        node->string = JS_ParseString();
        return node->string ? node : NULL;

    default:
        if (!strncmp(js.p, "true", 4) || !strncmp(js.p, "null", 4)) {
            node->type = JS_LITERAL;
            js.p += 4;
            return node;
        }
        if (!strncmp(js.p, "false", 5)) {
            node->type = JS_LITERAL;
            js.p += 5;
            return node;
        }

        node->type = JS_NUMBER;
        node->number = strtof(js.p, &end);
        if (end == js.p) {
            js.error = "unexpected character";
            return NULL;
        }
        js.p = end;
        return node;
    }
}

static const jsnode_t *JS_Member(const jsnode_t *object, const char *key)
{
    if (!object || object->type != JS_OBJECT)
        return NULL;

    for (const jsnode_t *n = object->child; n; n = n->next)
        if (!Q_stricmp(n->key, key))
            return n;

    return NULL;
}

/*
==============================================================================

ENVIRONMENTS

==============================================================================
*/

static void RV_AddMaterial(rv_reverb_t *reverb, const jsnode_t *word, const char *path)
{
    if (!word || word->type != JS_STRING)
        return;

    // "*" anywhere in the list makes the whole entry match anything
    if (!strcmp(word->string, "*")) {
        reverb->num_materials = -1;
        return;
    }

    if (reverb->num_materials < 0)
        return;

    if (reverb->num_materials == RV_MAX_MATERIALS) {
        Com_WPrintf("%s: more than %d materials in one reverb\n", path, RV_MAX_MATERIALS);
        return;
    }

    Q_strlcpy(reverb->materials[reverb->num_materials++], word->string, sizeof(reverb->materials[0]));
}

static int RV_CompareEnvs(const void *a, const void *b)
{
    float   da = ((const rv_env_t *)a)->dimension;
    float   db = ((const rv_env_t *)b)->dimension;

    return da < db ? -1 : da > db;
}

static bool RV_LoadEnvironments(const char *path)
{
    const jsnode_t  *root, *envs, *e, *r, *m, *preset;
    char    *text;
    int     ret, i;

    ret = FS_LoadFile(path, (void **)&text);
    if (!text) {
        if (ret != Q_ERR(ENOENT))
            Com_WPrintf("Couldn't load %s: %s\n", path, Q_ErrorString(ret));
        return false;
    }

    js.p = text;
    js.error = NULL;
    js.num_nodes = 0;

    root = JS_ParseValue(0);
    if (!root) {
        Com_WPrintf("%s: %s at offset %d\n", path, js.error, (int)(js.p - text));
        FS_FreeFile(text);
        return false;
    }

    envs = JS_Member(root, "environments");
    if (!envs || envs->type != JS_ARRAY) {
        Com_WPrintf("%s: no \"environments\" list\n", path);
        FS_FreeFile(text);
        return false;
    }

    rv.num_envs = 0;

    for (e = envs->child; e; e = e->next) {
        const jsnode_t *dimension = JS_Member(e, "dimension");
        const jsnode_t *reverbs = JS_Member(e, "reverbs");
        rv_env_t *env;

        if (!dimension || dimension->type != JS_NUMBER || !reverbs || reverbs->type != JS_ARRAY) {
            Com_WPrintf("%s: environment without a \"dimension\" and \"reverbs\"\n", path);
            continue;
        }

        if (rv.num_envs == RV_MAX_ENVS) {
            Com_WPrintf("%s: more than %d environments\n", path, RV_MAX_ENVS);
            break;
        }

        env = &rv.envs[rv.num_envs];
        memset(env, 0, sizeof(*env));
        env->dimension = dimension->number;

        for (r = reverbs->child; r; r = r->next) {
            rv_reverb_t *reverb;

            preset = JS_Member(r, "preset");
            if (!preset || preset->type != JS_STRING) {
                Com_WPrintf("%s: reverb without a \"preset\" (custom reverbs are not supported)\n", path);
                continue;
            }

            i = RV_FindPreset(preset->string);
            if (i < 0) {
                Com_WPrintf("%s: unknown reverb preset \"%s\"\n", path, preset->string);
                continue;
            }

            if (env->num_reverbs == RV_MAX_REVERBS) {
                Com_WPrintf("%s: more than %d reverbs in one environment\n", path, RV_MAX_REVERBS);
                break;
            }

            reverb = &env->reverbs[env->num_reverbs++];
            reverb->preset = i;

            m = JS_Member(r, "materials");
            if (m && m->type == JS_ARRAY) {
                for (m = m->child; m; m = m->next)
                    RV_AddMaterial(reverb, m, path);
            } else {
                RV_AddMaterial(reverb, m, path);
            }

            // no materials at all, or a "*" among them: anything
            if (reverb->num_materials <= 0)
                reverb->num_materials = 0;
        }

        if (env->num_reverbs)
            rv.num_envs++;
    }

    FS_FreeFile(text);

    qsort(rv.envs, rv.num_envs, sizeof(rv.envs[0]), RV_CompareEnvs);

    Com_DPrintf("Loaded %d reverb environments from %s\n", rv.num_envs, path);
    return rv.num_envs > 0;
}

static void RV_LoadMap(void)
{
    char    path[MAX_QPATH];

    Q_strlcpy(rv.mapname, cl.mapname, sizeof(rv.mapname));
    rv.num_envs = 0;
    rv.target = -1;
    rv.fading = false;
    efx.AuxiliaryEffectSloti(rv.slot, AL_EFFECTSLOT_EFFECT, 0);    // no effect: dry

    if (Q_snprintf(path, sizeof(path), "maps/%s.environments", cl.mapname) < sizeof(path)
        && RV_LoadEnvironments(path))
        return;

    if (!RV_LoadEnvironments("sound/default.environments"))
        Com_DPrintf("No reverb environments for %s\n", cl.mapname);
}

/*
==============================================================================

MEASURING

==============================================================================
*/

#define RV_MAX_VOTES    (2 * 13)

typedef struct {
    const char  *word;
    int         count;
} rv_vote_t;

// one way of each of the cube's 13 axes - the opposite way is the other half
// of the chord
static const vec3_t rv_axes[13] = {
    { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
    { 1, 1, 0 }, { 1, -1, 0 }, { 1, 0, 1 }, { 1, 0, -1 }, { 0, 1, 1 }, { 0, 1, -1 },
    { 1, 1, 1 }, { 1, 1, -1 }, { 1, -1, 1 }, { 1, -1, -1 },
};

static float RV_Ray(const vec3_t start, const vec3_t dir, rv_vote_t *votes, int *num_votes)
{
    trace_t     trace;
    vec3_t      end;
    const char  *word;
    int         i;

    VectorMA(start, RV_MAX_DIST, dir, end);
    CM_BoxTrace(&trace, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);

    if (trace.fraction == 1.0f || !trace.surface || (trace.surface->flags & SURF_SKY))
        return RV_MAX_DIST;

    word = CL_SurfaceMaterial(trace.surface);
    if (word) {
        for (i = 0; i < *num_votes; i++)
            if (votes[i].word == word)
                break;
        if (i == *num_votes) {
            votes[i].word = word;
            votes[i].count = 0;
            (*num_votes)++;
        }
        votes[i].count++;
    }

    return trace.fraction * RV_MAX_DIST;
}

static int RV_Select(float size, const char *material)
{
    const rv_env_t      *env;
    const rv_reverb_t   *reverb;
    int     i, j;

    if (!rv.num_envs)
        return -1;

    // the first environment big enough; anything bigger than all of them
    // goes in the last
    for (i = 0; i < rv.num_envs - 1; i++)
        if (size <= rv.envs[i].dimension)
            break;
    env = &rv.envs[i];

    for (i = 0, reverb = env->reverbs; i < env->num_reverbs; i++, reverb++) {
        if (!reverb->num_materials)
            return reverb->preset;
        if (!material)
            continue;
        for (j = 0; j < reverb->num_materials; j++)
            if (!Q_stricmp(reverb->materials[j], material))
                return reverb->preset;
    }

    return -1;
}

static void RV_Apply(const EFXEAXREVERBPROPERTIES *p)
{
    efx.Effectf(rv.effect, AL_EAXREVERB_DENSITY, p->flDensity);
    efx.Effectf(rv.effect, AL_EAXREVERB_DIFFUSION, p->flDiffusion);
    efx.Effectf(rv.effect, AL_EAXREVERB_GAIN, p->flGain);
    efx.Effectf(rv.effect, AL_EAXREVERB_GAINHF, p->flGainHF);
    efx.Effectf(rv.effect, AL_EAXREVERB_GAINLF, p->flGainLF);
    efx.Effectf(rv.effect, AL_EAXREVERB_DECAY_TIME, p->flDecayTime);
    efx.Effectf(rv.effect, AL_EAXREVERB_DECAY_HFRATIO, p->flDecayHFRatio);
    efx.Effectf(rv.effect, AL_EAXREVERB_DECAY_LFRATIO, p->flDecayLFRatio);
    efx.Effectf(rv.effect, AL_EAXREVERB_REFLECTIONS_GAIN, p->flReflectionsGain);
    efx.Effectf(rv.effect, AL_EAXREVERB_REFLECTIONS_DELAY, p->flReflectionsDelay);
    efx.Effectfv(rv.effect, AL_EAXREVERB_REFLECTIONS_PAN, p->flReflectionsPan);
    efx.Effectf(rv.effect, AL_EAXREVERB_LATE_REVERB_GAIN, p->flLateReverbGain);
    efx.Effectf(rv.effect, AL_EAXREVERB_LATE_REVERB_DELAY, p->flLateReverbDelay);
    efx.Effectfv(rv.effect, AL_EAXREVERB_LATE_REVERB_PAN, p->flLateReverbPan);
    efx.Effectf(rv.effect, AL_EAXREVERB_ECHO_TIME, p->flEchoTime);
    efx.Effectf(rv.effect, AL_EAXREVERB_ECHO_DEPTH, p->flEchoDepth);
    efx.Effectf(rv.effect, AL_EAXREVERB_MODULATION_TIME, p->flModulationTime);
    efx.Effectf(rv.effect, AL_EAXREVERB_MODULATION_DEPTH, p->flModulationDepth);
    efx.Effectf(rv.effect, AL_EAXREVERB_AIR_ABSORPTION_GAINHF, p->flAirAbsorptionGainHF);
    efx.Effectf(rv.effect, AL_EAXREVERB_HFREFERENCE, p->flHFReference);
    efx.Effectf(rv.effect, AL_EAXREVERB_LFREFERENCE, p->flLFReference);
    efx.Effectf(rv.effect, AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, p->flRoomRolloffFactor);
    efx.Effecti(rv.effect, AL_EAXREVERB_DECAY_HFLIMIT, p->iDecayHFLimit);

    // a slot holds a copy of its effect: changing the effect does nothing to
    // the sound until it is loaded into the slot again
    efx.AuxiliaryEffectSloti(rv.slot, AL_EFFECTSLOT_EFFECT, rv.effect);
}

static void RV_SetTarget(int preset, bool snap, float size, const char *material)
{
    if (s_reverb_debug->integer)
        Com_Printf("reverb: %s (size %.0f, material %s)%s\n", rv_presets[preset].name,
                   size, material ? material : "-", snap ? "" : ", fading");

    rv.target = preset;

    if (snap) {
        rv.current = rv_presets[preset].props;
        rv.fading = false;
        RV_Apply(&rv.current);
        return;
    }

    rv.from = rv.current;
    rv.fade_start = cls.realtime;
    rv.fading = true;
}

static void RV_Probe(void)
{
    static int  underwater = -1;
    rv_vote_t   votes[RV_MAX_VOTES];
    int         num_votes = 0;
    float       chords[q_countof(rv_axes)], size, t;
    const char  *material = NULL;
    vec3_t      dir, back;
    int         contents, preset, i, j, best = 0;
    bool        snap;

    if (underwater < 0)
        underwater = RV_FindPreset("underwater");

    contents = CM_PointContents(listener_origin, cl.bsp->nodes);

    // noclipping through a wall: keep whatever was there
    if (contents & CONTENTS_SOLID)
        return;

    if (contents & MASK_WATER) {
        preset = underwater;
        size = 0;
    } else {
        for (i = 0; i < q_countof(rv_axes); i++) {
            VectorNormalize2(rv_axes[i], dir);
            VectorNegate(dir, back);
            chords[i] = RV_Ray(listener_origin, dir, votes, &num_votes) +
                        RV_Ray(listener_origin, back, votes, &num_votes);
        }

        // insertion sort, 13 values
        for (i = 1; i < q_countof(chords); i++) {
            t = chords[i];
            for (j = i - 1; j >= 0 && chords[j] > t; j--)
                chords[j + 1] = chords[j];
            chords[j + 1] = t;
        }
        size = chords[q_countof(chords) / 2];

        for (i = 0; i < num_votes; i++) {
            if (votes[i].count > best) {
                best = votes[i].count;
                material = votes[i].word;
            }
        }

        preset = RV_Select(size, material);
    }

    if (s_reverb_debug->integer > 1)
        Com_Printf("reverb probe: size %.0f, material %s -> %s\n", size,
                   material ? material : "-", preset < 0 ? "(no match)" : rv_presets[preset].name);

    if (preset < 0 || preset == rv.target)
        return;

    // the first since the map loaded, and the surface of the water, switch at
    // once; everything else fades
    snap = rv.target < 0 || preset == underwater || rv.target == underwater;
    RV_SetTarget(preset, snap, size, material);
}

/*
==============================================================================

ENTRY POINTS

==============================================================================
*/

static bool RV_LoadEFX(void)
{
#define RV_PROC(name, type) \
    if (!(efx.name = (type)qalGetProcAddress("al" #name))) \
        return false;

    RV_PROC(GenEffects, LPALGENEFFECTS)
    RV_PROC(DeleteEffects, LPALDELETEEFFECTS)
    RV_PROC(Effecti, LPALEFFECTI)
    RV_PROC(Effectf, LPALEFFECTF)
    RV_PROC(Effectfv, LPALEFFECTFV)
    RV_PROC(GenAuxiliaryEffectSlots, LPALGENAUXILIARYEFFECTSLOTS)
    RV_PROC(DeleteAuxiliaryEffectSlots, LPALDELETEAUXILIARYEFFECTSLOTS)
    RV_PROC(AuxiliaryEffectSloti, LPALAUXILIARYEFFECTSLOTI)
    RV_PROC(AuxiliaryEffectSlotf, LPALAUXILIARYEFFECTSLOTF)
    RV_PROC(GenFilters, LPALGENFILTERS)
    RV_PROC(DeleteFilters, LPALDELETEFILTERS)
    RV_PROC(Filteri, LPALFILTERI)
    RV_PROC(Filterf, LPALFILTERF)

#undef RV_PROC
    return true;
}

/*
=================
AL_InitReverb

Every effect source sends to the one reverb slot. The music stream does not:
it is not in the room.
=================
*/
void AL_InitReverb(const unsigned *sources, int count)
{
    int     i;

    s_reverb = Cvar_Get("s_reverb", "1", CVAR_ARCHIVE);
    s_reverb_lerptime = Cvar_Get("s_reverb_lerptime", "1000", 0);
    s_reverb_debug = Cvar_Get("s_reverb_debug", "0", 0);

    memset(&rv, 0, sizeof(rv));
    rv.target = -1;
    rv.gain = -1;

    if (!RV_LoadEFX()) {
        Com_Printf("OpenAL: no EFX, room reverb disabled.\n");
        return;
    }

    qalGetError();
    efx.GenEffects(1, &rv.effect);
    efx.Effecti(rv.effect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    efx.GenAuxiliaryEffectSlots(1, &rv.slot);
    if (qalGetError() != AL_NO_ERROR) {
        Com_Printf("OpenAL: couldn't create a reverb effect, room reverb disabled.\n");
        if (efx.DeleteEffects && rv.effect)
            efx.DeleteEffects(1, &rv.effect);
        return;
    }

    for (i = 0; i < count; i++)
        qalSource3i(sources[i], AL_AUXILIARY_SEND_FILTER, rv.slot, 0, AL_FILTER_NULL);

    if (qalGetError() != AL_NO_ERROR) {
        Com_Printf("OpenAL: couldn't attach the reverb send, room reverb disabled.\n");
        for (i = 0; i < count; i++)
            qalSource3i(sources[i], AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        efx.DeleteAuxiliaryEffectSlots(1, &rv.slot);
        efx.DeleteEffects(1, &rv.effect);
        return;
    }

    // for AL_ReverbFeed; the reverb works without it
    efx.GenFilters(1, &rv.mute);
    efx.Filteri(rv.mute, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    efx.Filterf(rv.mute, AL_LOWPASS_GAIN, 0);
    if (qalGetError() != AL_NO_ERROR) {
        if (rv.mute)
            efx.DeleteFilters(1, &rv.mute);
        rv.mute = 0;
    }

    rv.available = true;
}

/*
=================
AL_ReverbChannel

Whether a sound plays in the room, set every time a source starts because
sources are reused. Interface sounds stay dry: the menus, chat beeps and the
help marker (S_StartLocalSound - from the listener, with no attenuation; the
game itself never plays an unattenuated sound on the player), and the beeps
the GAME sends for the same purpose, which arrive as ordinary sounds on the
player entity and so have to go by name - the help computer's "computer
updated" and the message beep.

Radio voices stay dry too (AL_ChannelIsRadio): they come over a radio, not
across the room.
=================
*/
static const char *const rv_dry_sounds[] = {
    "misc/pc_up.wav",
    "misc/talk.wav",
    "misc/talk1.wav",
};

// By name, as the start of it. All but the chatter are cut below 300 Hz, the
// way a radio is: the marines' calls in base1 and the other units (voice*,
// v_*, unit*), the pilots, the radio sets.
static const char *const rv_radio_sounds[] = {
    "world/chatter",
    "world/pilot",
    "world/radio",
    "world/unit",
    "world/v_",
    "world/voice",
};

// A sound's name as the game gave it: S_RegisterSound keeps it under "sound/".
static const char *RV_SoundName(const channel_t *ch)
{
    const char *name = ch->sfx->name;

    if (!Q_strncasecmp(name, "sound/", 6))
        name += 6;
    return name;
}

bool AL_ChannelIsDry(const channel_t *ch)
{
    const char  *name;
    int         i;

    if ((ch->entnum == -1 || ch->entnum == listener_entnum) && !ch->dist_mult)
        return true;

    name = RV_SoundName(ch);
    for (i = 0; i < (int)q_countof(rv_dry_sounds); i++)
        if (!Q_stricmp(name, rv_dry_sounds[i]))
            return true;

    return false;
}

bool AL_ChannelIsRadio(const channel_t *ch)
{
    const char  *name = RV_SoundName(ch);
    int         i;

    for (i = 0; i < (int)q_countof(rv_radio_sounds); i++)
        if (!Q_strncasecmp(name, rv_radio_sounds[i], strlen(rv_radio_sounds[i])))
            return true;

    return false;
}

void AL_ReverbChannel(const channel_t *ch)
{
    bool    dry;

    if (!rv.available)
        return;

    dry = AL_ChannelIsDry(ch) || AL_ChannelIsRadio(ch);
    qalSource3i(ch->srcnum, AL_AUXILIARY_SEND_FILTER, dry ? AL_EFFECTSLOT_NULL : rv.slot, 0, AL_FILTER_NULL);
}

// A source heard only through the reverb, its direct path muted: Steam
// Audio's room feed (steamaudio.c), a mix of what each of its voices sends to
// the room. False if there is no reverb to feed.
bool AL_ReverbFeed(unsigned source)
{
    if (!rv.available || !rv.mute)
        return false;

    qalGetError();
    qalSourcei(source, AL_DIRECT_FILTER, rv.mute);
    qalSource3i(source, AL_AUXILIARY_SEND_FILTER, rv.slot, 0, AL_FILTER_NULL);
    return qalGetError() == AL_NO_ERROR;
}

// The decay time of the reverb playing now, 0 if there is none: how much
// OpenAL sends to the room from a source depends on it (SA_RoomGain).
float AL_ReverbDecayTime(void)
{
    if (!rv.available || rv.target < 0)
        return 0;

    return rv.current.flDecayTime;
}

// after the sources are gone - a slot still in use cannot be deleted
void AL_ShutdownReverb(void)
{
    if (!rv.available)
        return;

    if (rv.mute)
        efx.DeleteFilters(1, &rv.mute);
    efx.DeleteAuxiliaryEffectSlots(1, &rv.slot);
    efx.DeleteEffects(1, &rv.effect);
    rv.available = false;
}

void AL_UpdateReverb(void)
{
    float   gain, frac, *out;
    const float *a, *b;
    int     i;

    if (!rv.available)
        return;

    gain = (cls.state == ca_active && cl.bsp) ? Cvar_ClampValue(s_reverb, 0, 1) : 0;
    if (gain != rv.gain) {
        efx.AuxiliaryEffectSlotf(rv.slot, AL_EFFECTSLOT_GAIN, gain);
        rv.gain = gain;
    }

    if (!gain) {
        // whatever comes next starts from scratch rather than fading in from
        // the room the last map or the last setting left behind
        rv.target = -1;
        rv.fading = false;
        rv.mapname[0] = 0;
        return;
    }

    if (strcmp(rv.mapname, cl.mapname))
        RV_LoadMap();

    if (cls.realtime >= rv.next_probe) {
        rv.next_probe = cls.realtime + RV_PROBE_MSEC;
        RV_Probe();
    }

    if (!rv.fading)
        return;

    frac = (float)(cls.realtime - rv.fade_start) / max(1, s_reverb_lerptime->integer);
    if (frac >= 1) {
        rv.current = rv_presets[rv.target].props;
        rv.fading = false;
    } else {
        a = &rv.from.flDensity;
        b = &rv_presets[rv.target].props.flDensity;
        out = &rv.current.flDensity;
        for (i = 0; i < (int)RV_NUM_FLOATS; i++)
            out[i] = a[i] + (b[i] - a[i]) * frac;
        rv.current.iDecayHFLimit = rv_presets[rv.target].props.iDecayHFLimit;
    }

    RV_Apply(&rv.current);
}
