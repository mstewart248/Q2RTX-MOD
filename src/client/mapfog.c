/*
Copyright (C) 2026 Q2RTX rerelease port

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
// mapfog.c -- the rerelease's per-map atmospheric fog
//
// 67 of the 76 in-scope rerelease maps set fog on WORLDSPAWN, and 49 also set a
// plain distance fog. That - not trigger_fog - is the fog system: worldspawn
// establishes the map's baseline and trigger_fog only modifies it as the player
// moves. Without this the maps render with no atmosphere at all.
//
// Read on the CLIENT out of the BSP entity lump, exactly like dynamiclights.c,
// because worldspawn keys are static for the level and nothing needs to go over
// the wire. trigger_fog's values come out of the lump the same way; only which
// trigger applied last is game state, and that arrives in two configstrings -
// see "TRIGGER_FOG" below.
//
// WHAT THE RERELEASE DOES WITH THESE, AND WHAT WE DO INSTEAD
//
// KEX renders this as an analytic per-pixel tint computed from depth and world
// height - a filter over the framebuffer. It cannot be lit and casts nothing.
// Matt's call was that that will always look wrong on top of path-traced
// geometry, so these values instead drive the DENSITY of the volumetric medium
// the god-ray pass already marches. Same numbers, real single scattering.
//
// The keys, measured across every shipped BSP rather than assumed:
//
//   heightfog_density      67 maps   the altitude-banded fog, 0.0001 .. 0.0024
//   heightfog_start_dist   67 maps   world Z of the TOP of the band
//   heightfog_end_dist     67 maps   world Z of the BOTTOM of the band
//   heightfog_start_color  67 maps   linear rgb at the top
//   heightfog_end_color    65 maps   linear rgb at the bottom
//   heightfog_falloff      65 maps   exponential rate with height, ~0.01 .. 0.04
//   fog_color              65 maps   linear rgb of the plain distance fog
//   heightfog_density is the dominant one; see fog_density below
//   fog_density            49 maps   plain distance fog, 0.01 .. 0.05
//   fog_sky_factor         42 maps   how much the SKY is fogged - not used here,
//                                    the path tracer's sky is a real environment
//
// NOTE ON "dist": heightfog_start_dist / _end_dist are named "dist" in id's own
// spawn documentation but they are world Z HEIGHTS, not distances, and start is
// ABOVE end (base1 runs -256 down to -768; mgu1m2 +357 down to -433). Do not
// treat them as a near/far pair.
//
// THE COLOUR DOES NOT FOLLOW THE BAND. KEX's shader (reproduced, bug and all,
// in Paril's q2repro, src/refresh/shader.c write_height_fog: "this is very
// broken. but that's how it is in re-release") measures the height from start
// TWICE, so start_color holds down to z = 2*start and the ramp only reaches
// end_color at z = start + end. On base1 that is -512 .. -1024, below the whole
// map, so the rerelease shows start_color everywhere. Lerping the colour across
// the band instead put end_color under every band floor, and mgu6m1's triggers
// set end_color 1 0 0: the lower half of that map went pure red where the
// rerelease shows orange. See mapfog_finish.
//
// AND NO FALLOFF MEANS NO HEIGHT FOG. q2repro only draws height fog when the
// falloff is above 0 (its formula divides by it). mgu6m3 and train leave the key
// out on worldspawn, and three of mgu6m1's triggers leave it out too.
//
// BOTH QUIRKS, THE TWO-COLOUR LAYERING AND trigger_fog ITSELF SIT BEHIND
// cl_fog_rerelease, DEFAULT 0. Matt's verdict on mgu6m1 with all of it: closer
// to the rerelease, but too thick, and the map looked better before. With it at
// 0 the fog is worldspawn's alone, coloured across the band as it always was.
//
// THE DENSITY SCALE IS NOT DERIVABLE. KEX's density-to-extinction constant lives
// in a closed renderer, so the authored numbers cannot be taken literally. They
// used to be, times a cl_fog_scale that every map then needed its own value of
// (300 on the id maps, 4 on the MGU ones, 1000 left in a config), and the same
// menu settings looked completely different from one map to the next and from
// the classic campaign to the rerelease.
//
// So the authored values now give the fog its SHAPE and nothing else: the
// densities are normalised so the thickest point of the medium is 1 - exactly
// the flat medium the classic campaign's god rays have always marched. How
// bright the fog is comes only from the five pt_fog_scale_* knobs (sun, skybox,
// emissive, dynamic, model) plus each light's own volscale.
//
// THE MAP CFGS WERE CONVERTED, NOT RETUNED. rerelease/maps/*.cfg used to carry
// cl_fog_scale S and cl_volumetric_fog_density V. With P the old peak density of
// the map's own medium (hf_density / 0.025 + fog_density / 2), the old renderer
// drew the sun at density P*S times gr_intensity (2) times the fog colour, and
// the local lights at density P*V. So each cfg now sets, for the same picture:
//
//   pt_fog_scale_sun        P * S * 2 * luminance(fog colour)
//                           (the colour itself is dropped under the physical sky)
//   pt_fog_scale_<source>   P * V / FOG_LOCAL_DENSITY (0.05) * its old scale
//   pt_fog_scale_skybox     the same, times its old pt_fog_sky_scale
//
// mapcvar still restores the player's own values on the next map, so these are
// per-map overrides of the menu knobs, not new defaults.

#include "client.h"

typedef struct {
    bool    valid;

    float   density;            // plain distance fog
    vec3_t  color;
    float   sky_factor;         // how far the sky is tinted to fog_color (KEX)

    float   hf_density;         // altitude-banded fog
    float   hf_falloff;
    float   hf_start_z;
    float   hf_end_z;
    vec3_t  hf_start_color;
    vec3_t  hf_end_color;
    float   hf_color_start_z;   // all hf_start_color here, all hf_end_color at
    float   hf_color_end_z;     // hf_color_end_z - KEX's ramp, see mapfog_finish
    float   hf_ref_z;           // heightfog_start_dist as authored, never swapped:
                                // the height KEX's density integral starts from
} mapfog_t;

static mapfog_t cl_mapfog;
static mapfog_t cl_mapfog_kex;  // the same with KEX's falloff rule, for cl_fog_rerelease

static void mapfog_finish(mapfog_t *f);

static cvar_t   *cl_fog;
static cvar_t   *cl_fog_rerelease;

/* The normalisation constants. These MUST match FOG_HEIGHTFOG_REFERENCE and
   FOG_DISTANCEFOG_REFERENCE in fog_medium.glsl, which divides the values handed
   to it by them: the shader's density at the floor of the band is
   hf_density / HF_REF + density / DIST_REF, and that is what is scaled to 1. */
#define MAPFOG_HEIGHTFOG_REFERENCE   0.025f
#define MAPFOG_DISTANCEFOG_REFERENCE 2.0f

/*
=================
CL_ParseWorldspawnFog

Walks the first entity block of the lump. worldspawn is always entity 0, so
there is no need to scan the rest.
=================
*/
static bool CL_ParseWorldspawnFog(const char *data, mapfog_t *out)
{
    char        key[64];
    const char  *token;
    bool        is_world = false;
    bool        got_any = false;

    memset(out, 0, sizeof(*out));

    token = COM_Parse(&data);
    if (strcmp(token, "{"))
        return false;

    while (1) {
        token = COM_Parse(&data);
        if (!data && !*token)
            return false;       // unterminated block
        if (!strcmp(token, "}"))
            break;

        Q_strlcpy(key, token, sizeof(key));

        token = COM_Parse(&data);
        if (!data && !*token)
            return false;

        if (!strcmp(key, "classname")) {
            if (!strcmp(token, "worldspawn"))
                is_world = true;
        } else if (!strcmp(key, "fog_density")) {
            out->density = atof(token);
            got_any = true;
        } else if (!strcmp(key, "fog_color")) {
            sscanf(token, "%f %f %f", &out->color[0], &out->color[1], &out->color[2]);
        } else if (!strcmp(key, "fog_sky_factor")) {
            out->sky_factor = atof(token);
        } else if (!strcmp(key, "heightfog_density")) {
            out->hf_density = atof(token);
            got_any = true;
        } else if (!strcmp(key, "heightfog_falloff")) {
            // several maps carry a typo'd value such as "0..01"; atof stops at
            // the second dot and yields 0, which is a harmless "no falloff"
            out->hf_falloff = atof(token);
        } else if (!strcmp(key, "heightfog_start_dist")) {
            out->hf_start_z = atof(token);
        } else if (!strcmp(key, "heightfog_end_dist")) {
            out->hf_end_z = atof(token);
        } else if (!strcmp(key, "heightfog_start_color")) {
            sscanf(token, "%f %f %f", &out->hf_start_color[0], &out->hf_start_color[1], &out->hf_start_color[2]);
        } else if (!strcmp(key, "heightfog_end_color")) {
            sscanf(token, "%f %f %f", &out->hf_end_color[0], &out->hf_end_color[1], &out->hf_end_color[2]);
        }
    }

    mapfog_finish(out);

    out->valid = is_world && got_any;
    return out->valid;
}

static void mapfog_order_band(mapfog_t *out)
{
    /* The renderer takes start_z as the TOP of the band and end_z as the BOTTOM,
       which is how most maps author it - but not all. mgu2m2 (-128 .. 580) and
       mgu2m3 (-864 .. -256) have start BELOW end. KEX keeps the fog thick at
       the LOW end either way, and the colours stay in their authored order:
       mgu2m2 in the rerelease is dense blue-grey down low fading into orange up
       high. (The colours ride their own ramp now, hf_color_*_z, which swaps
       with them.)
       Taken as authored, the band came out negative, getHeightFogDensity went
       flat and getFogColor pinned end_color everywhere - one solid orange.
       Swapping the heights WITH their colours keeps every colour where the
       mapper put it and hands the shader the top/bottom order it expects. */
    if (out->hf_start_z < out->hf_end_z) {
        vec3_t tmp;
        float z = out->hf_start_z;
        out->hf_start_z = out->hf_end_z;
        out->hf_end_z = z;
        VectorCopy(out->hf_start_color, tmp);
        VectorCopy(out->hf_end_color, out->hf_start_color);
        VectorCopy(tmp, out->hf_end_color);
        z = out->hf_color_start_z;
        out->hf_color_start_z = out->hf_color_end_z;
        out->hf_color_end_z = z;
    }
}

/*
=================
mapfog_finish

Once all of a set's keys are in. Works out KEX's colour heights - see the notes
at the top of the file - then puts the band in order. The colour heights come
from the keys as authored, before the swap, which carries them along with the
colours. Only cl_fog_rerelease uses them; without it CL_GetMapFog puts the
colours back on the band.
=================
*/
static void mapfog_finish(mapfog_t *f)
{
    f->hf_ref_z = f->hf_start_z;
    f->hf_color_start_z = 2 * f->hf_start_z;
    f->hf_color_end_z = f->hf_start_z + f->hf_end_z;

    mapfog_order_band(f);
}

// KEX's other quirk, for cl_fog_rerelease: no falloff, no height fog
static void mapfog_kex_falloff(mapfog_t *f)
{
    if (f->hf_falloff <= 0)
        f->hf_density = 0;
}

/*
=======================================================================

TRIGGER_FOG

A trigger_fog swaps the map's fog, its height fog or both for a set of its own
as the player moves through it - "on" values one way, "_off" values the other,
or a mix of the two by how far through the brush the player is. Its keys are
static and read here; the game runs the touch logic (src/baseq2/g_rerelease.c)
and publishes the result as "<brush model> <blend> <ms>" in CS_TRIGGER_FOG and
CS_TRIGGER_HEIGHTFOG, one per fog, because each trigger changes only the fogs
its spawnflags name. The engine saves configstrings with the level, so a
loaded game comes back in the fog it was saved in.

Each fog then fades from wherever it is to the new values over <ms>, as KEX
does. The result still goes through the same normalisation as worldspawn's
fog, against the WORLD's peak: a trigger that thickens the fog renders thicker,
one that clears it renders clear.

=======================================================================
*/

#define MAX_TRIGGER_FOGS    64

typedef struct {
    int         model;          // N of the trigger's "*N" brush model
    mapfog_t    on, off;
} trigfog_t;

// one of the two fogs a trigger can drive, fading between two values
typedef struct {
    char        cs[MAX_QPATH];  // the configstring this state was built from
    bool        active;         // false: the map's own worldspawn fog
    mapfog_t    from, to;
    int         start, duration;
} fogfade_t;

enum { FADE_FOG, FADE_HEIGHTFOG, NUM_FADES };

static trigfog_t    cl_trigfogs[MAX_TRIGGER_FOGS];
static int          cl_numtrigfogs;
static float        cl_trigfog_peak;    // the reference when worldspawn has no fog
static fogfade_t    cl_fogfades[NUM_FADES];
static int          cl_fogfade_instant_until;  // -1 until the first poll on a map

static float mapfog_peak(const mapfog_t *f)
{
    return f->hf_density / MAPFOG_HEIGHTFOG_REFERENCE + f->density / MAPFOG_DISTANCEFOG_REFERENCE;
}

/*
=================
mapfog_parse_key

One fog key of a trigger_fog or of the entity it targets. Each has an "_off"
twin. Keys a trigger leaves out stay 0, as they do in the rerelease: a trigger
that names only height fog keys but sets AFFECT_FOG clears the distance fog.
=================
*/
static bool mapfog_parse_key(const char *key, const char *value, mapfog_t *on, mapfog_t *off)
{
    char        name[64];
    size_t      len = strlen(key);
    mapfog_t    *f = on;

    if (len > 4 && !strcmp(key + len - 4, "_off")) {
        if (len - 4 >= sizeof(name))
            return false;
        memcpy(name, key, len - 4);
        name[len - 4] = 0;
        key = name;
        f = off;
    }

    if (!strcmp(key, "fog_density"))
        f->density = atof(value);
    else if (!strcmp(key, "fog_color"))
        sscanf(value, "%f %f %f", &f->color[0], &f->color[1], &f->color[2]);
    else if (!strcmp(key, "fog_sky_factor"))
        f->sky_factor = atof(value);
    else if (!strcmp(key, "heightfog_density"))
        f->hf_density = atof(value);
    else if (!strcmp(key, "heightfog_falloff"))
        f->hf_falloff = atof(value);
    else if (!strcmp(key, "heightfog_start_dist"))
        f->hf_start_z = atof(value);
    else if (!strcmp(key, "heightfog_end_dist"))
        f->hf_end_z = atof(value);
    else if (!strcmp(key, "heightfog_start_color"))
        sscanf(value, "%f %f %f", &f->hf_start_color[0], &f->hf_start_color[1], &f->hf_start_color[2]);
    else if (!strcmp(key, "heightfog_end_color"))
        sscanf(value, "%f %f %f", &f->hf_end_color[0], &f->hf_end_color[1], &f->hf_end_color[2]);
    else
        return false;

    return true;
}

/*
=================
CL_ParseTriggerFogs

Every trigger_fog in the lump, with its values or, when it has a "target",
those of the entity it targets. The game resolves that target with
G_PickTarget at spawn time, which can only find entities spawned before it, so
only earlier entities are searched here too (rlava2's info_notnull precedes
its three triggers).
=================
*/
#define MAX_FOG_HOLDERS     64

typedef struct {
    char        targetname[MAX_QPATH];
    mapfog_t    on, off;
} fogholder_t;

static void CL_ParseTriggerFogs(const char *data)
{
    static fogholder_t holders[MAX_FOG_HOLDERS];
    int         numholders = 0;
    char        key[64], classname[64], model[16], target[MAX_QPATH], targetname[MAX_QPATH];
    mapfog_t    on, off;
    bool        has_fog;
    const char  *token;
    int         i;

    while (1) {
        token = COM_Parse(&data);
        if (!data || strcmp(token, "{"))
            break;

        classname[0] = model[0] = target[0] = targetname[0] = 0;
        memset(&on, 0, sizeof(on));
        memset(&off, 0, sizeof(off));
        has_fog = false;

        while (1) {
            token = COM_Parse(&data);
            if (!data || !strcmp(token, "}"))
                break;
            Q_strlcpy(key, token, sizeof(key));

            token = COM_Parse(&data);
            if (!data)
                break;

            if (!strcmp(key, "classname"))
                Q_strlcpy(classname, token, sizeof(classname));
            else if (!strcmp(key, "model"))
                Q_strlcpy(model, token, sizeof(model));
            else if (!strcmp(key, "target"))
                Q_strlcpy(target, token, sizeof(target));
            else if (!strcmp(key, "targetname"))
                Q_strlcpy(targetname, token, sizeof(targetname));
            else if (mapfog_parse_key(key, token, &on, &off))
                has_fog = true;
        }

        if (!strcmp(classname, "trigger_fog") && model[0] == '*' &&
            cl_numtrigfogs < MAX_TRIGGER_FOGS) {
            trigfog_t *t = &cl_trigfogs[cl_numtrigfogs++];

            t->model = atoi(model + 1);
            t->on = on;
            t->off = off;

            if (target[0]) {
                for (i = 0; i < numholders; i++) {
                    if (!strcmp(holders[i].targetname, target)) {
                        t->on = holders[i].on;
                        t->off = holders[i].off;
                        break;
                    }
                }
            }

            // triggers only ever apply under cl_fog_rerelease, so they take
            // its falloff rule here and fade through it smoothly
            mapfog_finish(&t->on);
            mapfog_finish(&t->off);
            mapfog_kex_falloff(&t->on);
            mapfog_kex_falloff(&t->off);
            cl_trigfog_peak = max(cl_trigfog_peak, max(mapfog_peak(&t->on), mapfog_peak(&t->off)));
        }

        if (targetname[0] && has_fog && numholders < MAX_FOG_HOLDERS) {
            Q_strlcpy(holders[numholders].targetname, targetname, sizeof(holders[0].targetname));
            holders[numholders].on = on;
            holders[numholders].off = off;
            numholders++;
        }
    }
}

// copy one fade's own fields - the distance fog's or the height fog's
static void mapfog_copy_part(mapfog_t *dst, const mapfog_t *src, int part)
{
    if (part == FADE_FOG) {
        dst->density = src->density;
        dst->sky_factor = src->sky_factor;
        VectorCopy(src->color, dst->color);
    } else {
        dst->hf_density = src->hf_density;
        dst->hf_falloff = src->hf_falloff;
        dst->hf_start_z = src->hf_start_z;
        dst->hf_end_z = src->hf_end_z;
        VectorCopy(src->hf_start_color, dst->hf_start_color);
        VectorCopy(src->hf_end_color, dst->hf_end_color);
        dst->hf_color_start_z = src->hf_color_start_z;
        dst->hf_color_end_z = src->hf_color_end_z;
        dst->hf_ref_z = src->hf_ref_z;
    }
}

static void mapfog_lerp(mapfog_t *out, const mapfog_t *a, const mapfog_t *b, float t)
{
    out->density    = a->density    + (b->density    - a->density)    * t;
    out->hf_density = a->hf_density + (b->hf_density - a->hf_density) * t;
    out->hf_falloff = a->hf_falloff + (b->hf_falloff - a->hf_falloff) * t;
    out->hf_start_z = a->hf_start_z + (b->hf_start_z - a->hf_start_z) * t;
    out->hf_end_z   = a->hf_end_z   + (b->hf_end_z   - a->hf_end_z)   * t;
    out->hf_color_start_z = a->hf_color_start_z + (b->hf_color_start_z - a->hf_color_start_z) * t;
    out->hf_color_end_z   = a->hf_color_end_z   + (b->hf_color_end_z   - a->hf_color_end_z)   * t;
    out->hf_ref_z   = a->hf_ref_z   + (b->hf_ref_z   - a->hf_ref_z)   * t;
    out->sky_factor = a->sky_factor + (b->sky_factor - a->sky_factor) * t;
    LerpVector(a->color, b->color, t, out->color);
    LerpVector(a->hf_start_color, b->hf_start_color, t, out->hf_start_color);
    LerpVector(a->hf_end_color, b->hf_end_color, t, out->hf_end_color);
}

// where a fade is right now; only its own part of `out` is written
static void mapfog_fade_value(const fogfade_t *f, int part, mapfog_t *out)
{
    mapfog_t v;

    // fades only apply under cl_fog_rerelease, so they start from and return
    // to the map's fog under its rules
    if (!f->active) {
        mapfog_copy_part(out, &cl_mapfog_kex, part);
        return;
    }

    if (f->duration <= 0 || cl.time - f->start >= f->duration) {
        mapfog_copy_part(out, &f->to, part);
        return;
    }

    if (cl.time <= f->start) {      // a demo seeked backwards
        mapfog_copy_part(out, &f->from, part);
        return;
    }

    mapfog_lerp(&v, &f->from, &f->to, (float)(cl.time - f->start) / f->duration);
    mapfog_copy_part(out, &v, part);
}

/*
=================
CL_UpdateTriggerFog

Polled from CL_GetMapFog. A new configstring starts a fade from wherever that
fog is now. Anything that arrives in the first half second on a map - the
state a saved game comes back with, or a trigger the player spawns inside, as
on rmine2 - is applied at once rather than faded in.
=================
*/
static void CL_UpdateTriggerFog(void)
{
    static const int cs_index[NUM_FADES] = { CS_TRIGGER_FOG, CS_TRIGGER_HEIGHTFOG };
    int part, i;

    if (!cl_numtrigfogs || !CL_GameOwnsExtraStats())
        return;

    if (cl_fogfade_instant_until < 0)
        cl_fogfade_instant_until = cl.time + 500;

    for (part = 0; part < NUM_FADES; part++) {
        fogfade_t   *f = &cl_fogfades[part];
        const char  *cs = cl.configstrings[cs_index[part]];
        const trigfog_t *t = NULL;
        int         model, ms;
        float       blend;
        mapfog_t    now, target;

        if (!strcmp(cs, f->cs))
            continue;
        Q_strlcpy(f->cs, cs, sizeof(f->cs));

        if (sscanf(cs, "%d %f %d", &model, &blend, &ms) == 3) {
            for (i = 0; i < cl_numtrigfogs; i++) {
                if (cl_trigfogs[i].model == model) {
                    t = &cl_trigfogs[i];
                    break;
                }
            }
        }

        // where this fog is right now, before the change
        now = cl_mapfog_kex;
        mapfog_fade_value(f, part, &now);

        if (t) {
            mapfog_lerp(&target, &t->off, &t->on, blend < 0 ? 0 : blend > 1 ? 1 : blend);
        } else {
            target = cl_mapfog_kex; // cleared, or a string from some other game
            ms = 0;
        }

        f->from = now;
        f->to = target;
        f->start = cl.time;
        f->duration = cl.time < cl_fogfade_instant_until ? 0 : ms;
        f->active = true;
    }
}

static void CL_ClearTriggerFog(void)
{
    cl_numtrigfogs = 0;
    cl_trigfog_peak = 0;
    memset(cl_fogfades, 0, sizeof(cl_fogfades));
    cl_fogfade_instant_until = -1;
}

// Fog is on or off. The old 1/2/3 modes are gone: 1 is what was mode 3, the
// froxel volumetric, and anything an old config left above 1 means "on".
static void cl_fog_changed(cvar_t *self)
{
    if (self->integer > 1)
        Cvar_SetInteger(self, 1, FROM_CODE);
    else if (self->integer < 0)
        Cvar_SetInteger(self, 0, FROM_CODE);
}

void CL_InitMapFog(void)
{
    // Registered at client init, not lazily at map load, because the video
    // menu binds to these by name when it is built.
    cl_fog = Cvar_Get("cl_fog", "0", CVAR_ARCHIVE);
    cl_fog->changed = cl_fog_changed;
    cl_fog_changed(cl_fog);

    // The rerelease's own fog rules and its trigger_fogs - see the notes at
    // the top of the file. Read every frame, so it switches live.
    cl_fog_rerelease = Cvar_Get("cl_fog_rerelease", "0", CVAR_ARCHIVE);

    /* THE FIVE FOG KNOBS - the only things that set how bright the fog is.
       Registered here only to make them ARCHIVE; the renderer owns them (the
       first two are UBO cvars in global_ubo.h, the other three are read in
       vertex_buffer.c) and the defaults must match theirs. A map cfg may set
       any of them with mapcvar. */
    Cvar_Get("pt_fog_scale_sun",      "2.0", CVAR_ARCHIVE);   // physical sky
    Cvar_Get("pt_fog_scale_skybox",   "1.0", CVAR_ARCHIVE);   // the map's skybox
    Cvar_Get("pt_fog_scale_emissive", "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_scale_dynamic",  "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_scale_model",    "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_lava_scale",     "5.0", CVAR_ARCHIVE);   // lava, x10 in vertex_buffer.c

    // Not a brightness knob - the quality of the physical sky's fog, fast or
    // accurate. See its note in global_ubo.h; the default must match it.
    Cvar_Get("pt_fog_sky_sun_only",   "1",   CVAR_ARCHIVE);
}

/*
=================
CL_LoadMapFog

Called once per map, after the BSP is loaded.
=================
*/
void CL_LoadMapFog(void)
{
    memset(&cl_mapfog, 0, sizeof(cl_mapfog));
    memset(&cl_mapfog_kex, 0, sizeof(cl_mapfog_kex));
    CL_ClearTriggerFog();

    if (!cl.bsp || !cl.bsp->entitystring)
        return;

    CL_ParseTriggerFogs(cl.bsp->entitystring);
    if (cl_numtrigfogs)
        Com_DPrintf("map fog: %d trigger_fog\n", cl_numtrigfogs);

    if (!CL_ParseWorldspawnFog(cl.bsp->entitystring, &cl_mapfog))
        return;

    // a band with no thickness would divide by zero in the shader
    if (cl_mapfog.hf_start_z <= cl_mapfog.hf_end_z)
        cl_mapfog.hf_density = 0.0f;

    cl_mapfog_kex = cl_mapfog;
    mapfog_kex_falloff(&cl_mapfog_kex);

    Com_DPrintf("map fog: density %g, heightfog %g z %.0f..%.0f falloff %g\n",
                cl_mapfog.density, cl_mapfog.hf_density,
                cl_mapfog.hf_end_z, cl_mapfog.hf_start_z, cl_mapfog.hf_falloff);
}

void CL_FreeMapFog(void)
{
    memset(&cl_mapfog, 0, sizeof(cl_mapfog));
    memset(&cl_mapfog_kex, 0, sizeof(cl_mapfog_kex));
    CL_ClearTriggerFog();
}

/*
=================
CL_GetMapFog

Fills the renderer-side description. Returns false only when fog is switched
off, in which case the renderer falls back to the classic god rays. A map with
no fog of its own still gets a medium - a flat white one - so turning fog on
looks the same on the classic campaign as on the rerelease.
=================
*/
bool CL_GetMapFog(mapfog_params_t *out)
{
    const mapfog_t  *world;
    mapfog_t        fog;
    float           peak;
    bool            rerelease;

    if (!cl_fog || !cl_fog->integer)
        return false;

    memset(out, 0, sizeof(*out));
    out->mode = 3;

    // kept up to date either way, so turning cl_fog_rerelease on mid-level
    // shows the fog the last trigger set
    CL_UpdateTriggerFog();

    rerelease = cl_fog_rerelease && cl_fog_rerelease->integer > 0;
    world = rerelease ? &cl_mapfog_kex : &cl_mapfog;

    // The scale is set by the map's own fog and stays put, so a trigger_fog
    // that thickens or clears the fog renders thicker or clear. A map whose
    // only fog is in its triggers (rmine2) takes the thickest of those.
    peak = world->valid ? mapfog_peak(world) : 0.0f;
    if (peak <= 0.0f && rerelease &&
        (cl_fogfades[FADE_FOG].active || cl_fogfades[FADE_HEIGHTFOG].active))
        peak = cl_trigfog_peak;

    if (peak <= 0.0f) {
        out->density = MAPFOG_DISTANCEFOG_REFERENCE;    // density 1 everywhere
        VectorSet(out->color, 1, 1, 1);
        return true;
    }

    fog = *world;
    if (rerelease) {
        mapfog_fade_value(&cl_fogfades[FADE_FOG], FADE_FOG, &fog);
        mapfog_fade_value(&cl_fogfades[FADE_HEIGHTFOG], FADE_HEIGHTFOG, &fog);
    } else {
        // the colours ramp across the density band itself
        fog.hf_color_start_z = fog.hf_start_z;
        fog.hf_color_end_z = fog.hf_end_z;
    }

    // a band with no thickness would divide by zero in the shader
    if (fog.hf_start_z <= fog.hf_end_z)
        fog.hf_density = 0.0f;

    // Keep the shape, drop the magnitude: the band floor comes out at 1.
    out->density    = fog.density / peak;
    out->hf_density = fog.hf_density / peak;
    out->hf_falloff = fog.hf_falloff;
    out->hf_start_z = fog.hf_start_z;
    out->hf_end_z   = fog.hf_end_z;
    VectorCopy(fog.color, out->color);
    VectorCopy(fog.hf_start_color, out->hf_start_color);
    VectorCopy(fog.hf_end_color, out->hf_end_color);
    out->hf_color_start_z = fog.hf_color_start_z;
    out->hf_color_end_z   = fog.hf_color_end_z;
    out->color_layers     = rerelease;

    return true;
}

/*
=================
CL_GetMapFogKEX

The rerelease's fog as KEX draws it, for the software renderer's analytic
fog: the authored values, not normalised, under KEX's rules and with its
trigger_fog fades - whatever cl_fog and cl_fog_rerelease say, which are the
path tracer's. False when the map has no fog at all.
=================
*/
bool CL_GetMapFogKEX(mapfog_kex_t *out)
{
    mapfog_t    fog;

    memset(out, 0, sizeof(*out));

    CL_UpdateTriggerFog();

    fog = cl_mapfog_kex;
    mapfog_fade_value(&cl_fogfades[FADE_FOG], FADE_FOG, &fog);
    mapfog_fade_value(&cl_fogfades[FADE_HEIGHTFOG], FADE_HEIGHTFOG, &fog);

    // KEX draws height fog only with both a density and a falloff, and a band
    // with no thickness would divide by zero in its colour ramp
    if (fog.hf_falloff <= 0 || fog.hf_color_end_z == fog.hf_color_start_z)
        fog.hf_density = 0;

    if (fog.density <= 0 && fog.hf_density <= 0 && fog.sky_factor <= 0)
        return false;

    out->density = fog.density;
    VectorCopy(fog.color, out->color);
    out->sky_factor = fog.sky_factor;
    out->hf_density = fog.hf_density;
    out->hf_falloff = fog.hf_falloff;
    out->hf_ref_z = fog.hf_ref_z;
    VectorCopy(fog.hf_start_color, out->hf_start_color);
    VectorCopy(fog.hf_end_color, out->hf_end_color);
    out->hf_color_start_z = fog.hf_color_start_z;
    out->hf_color_end_z = fog.hf_color_end_z;

    return true;
}
