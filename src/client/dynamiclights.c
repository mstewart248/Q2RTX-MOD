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
// dynamiclights.c -- the rerelease's dynamic_light entity
//
// 870 of these are placed across the 70 non-N64 maps and they are the actual
// lighting of the rerelease's levels - without them the maps are lit only by
// whatever emissive materials happen to exist, which is why they look dark.
//
// They are handled entirely on the client, out of the BSP entity lump, for two
// reasons: they are static (only 36 of the 870 even carry a targetname) and the
// path tracer already has exactly the light types they need. A dynamic_light is
// a spotlight when it targets an aim entity and a point light otherwise, which
// is V_AddSpotLight and V_AddSphereLight respectively. Nothing new is needed in
// the renderer and nothing goes over the wire.
//
// The keys, measured across every map rather than assumed:
//
//   shadowlightradius             99%  reach of the light, 256..2048
//   target                        87%  aim entity, an info_notnull 20-30 units
//                                      away - this is a DIRECTION marker, and
//                                      it is present on directional lights that
//                                      have no cone angle of their own
//   shadowlightintensity          87%  brightness multiplier, mostly 1..2
//   shadowlightconeangle          51%  full cone angle in degrees, 10..120
//   _color                        42%  linear rgb, already 0..1
//   shadowlightstyle               9%  classic light style index
//   shadowlightresolution         51%  shadow map size - meaningless to a path
//                                      tracer, ignored
//   shadowlightstart/endfadedistance    a renderer LOD, ignored (see below)
//

#include "client.h"

typedef struct {
    vec3_t  origin;
    vec3_t  color;
    vec3_t  direction;
    float   intensity;      // shadowlightintensity, already folded with radius
    float   cone_angle;     // full cone angle in degrees
    int     style;
    int     switch_index;   // bit in CS_DYNAMICLIGHTS, or -1 for always-on
    bool    is_spot;
} cdynamiclight_t;

static cdynamiclight_t  *cl_dynamiclights;
static int              cl_num_dynamiclights;

static cvar_t   *cl_dynamic_lights;
static cvar_t   *cl_dynamic_light_scale;
static cvar_t   *cl_dynamic_light_cone;

// the radius of the emitter itself, not of its reach; this is what softens the
// shadow edge. V_AddLight uses 10 for an ordinary point light, so do the same.
#define DLIGHT_EMITTER_RADIUS   10.0f

/*
=================================================================

  entity lump parsing

  Only the handful of keys that matter are kept. The lump is walked
  twice: once to find the aim entities that dynamic_lights target,
  and once to build the lights themselves.

=================================================================
*/

typedef struct {
    char    classname[32];
    char    targetname[64];
    char    target[64];
    vec3_t  origin;
    vec3_t  color;
    float   radius;
    float   intensity;
    float   cone_angle;
    float   angle;
    int     style;
    int     spawnflags;
    bool    has_origin;
    bool    has_color;
    bool    has_cone;
    bool    has_angle;
} entkeys_t;

typedef struct {
    char    name[64];
    vec3_t  origin;
} namedent_t;

// advances (data) past one { ... } block, filling (out). returns false at the
// end of the lump or on anything malformed.
static bool CL_ParseEntityBlock(const char **data, entkeys_t *out)
{
    char key[64];
    const char *token;

    memset(out, 0, sizeof(*out));
    out->intensity = 1.0f;
    VectorSet(out->color, 1.0f, 1.0f, 1.0f);

    token = COM_Parse(data);
    if (!*data && !*token)
        return false;
    if (strcmp(token, "{"))
        return false;

    while (1) {
        token = COM_Parse(data);
        if (!*data && !*token)
            return false;       // unterminated block
        if (!strcmp(token, "}"))
            break;

        Q_strlcpy(key, token, sizeof(key));

        token = COM_Parse(data);
        if (!*data && !*token)
            return false;

        if (!strcmp(key, "classname"))
            Q_strlcpy(out->classname, token, sizeof(out->classname));
        else if (!strcmp(key, "targetname"))
            Q_strlcpy(out->targetname, token, sizeof(out->targetname));
        else if (!strcmp(key, "target"))
            Q_strlcpy(out->target, token, sizeof(out->target));
        else if (!strcmp(key, "origin")) {
            if (sscanf(token, "%f %f %f", &out->origin[0], &out->origin[1], &out->origin[2]) == 3)
                out->has_origin = true;
        } else if (!strcmp(key, "_color")) {
            if (sscanf(token, "%f %f %f", &out->color[0], &out->color[1], &out->color[2]) == 3)
                out->has_color = true;
        } else if (!strcmp(key, "shadowlightradius"))
            out->radius = atof(token);
        else if (!strcmp(key, "shadowlightintensity"))
            out->intensity = atof(token);
        else if (!strcmp(key, "shadowlightconeangle")) {
            out->cone_angle = atof(token);
            out->has_cone = true;
        } else if (!strcmp(key, "shadowlightstyle"))
            out->style = atoi(token);
        else if (!strcmp(key, "spawnflags"))
            out->spawnflags = atoi(token);
        else if (!strcmp(key, "angle")) {
            out->angle = atof(token);
            out->has_angle = true;
        }
    }

    return true;
}

/*
=================
CL_LoadDynamicLights

Called once per map, after the BSP is loaded.
=================
*/
void CL_LoadDynamicLights(void)
{
    const char  *data;
    entkeys_t   ent;
    namedent_t  *named = NULL;
    int         num_named = 0, max_named = 0;
    int         num_lights = 0, skipped_off = 0, unresolved = 0;
    int         num_switchable = 0;

    CL_FreeDynamicLights();

    if (!cl.bsp || !cl.bsp->entitystring)
        return;

    // pass 1: count, so both arrays can be sized exactly
    data = cl.bsp->entitystring;
    while (CL_ParseEntityBlock(&data, &ent)) {
        if (ent.targetname[0] && ent.has_origin)
            max_named++;
        if (!strcmp(ent.classname, "dynamic_light"))
            num_lights++;
    }

    if (!num_lights)
        return;

    cl_dynamiclights = Z_Mallocz(sizeof(cdynamiclight_t) * num_lights);
    if (max_named)
        named = Z_Mallocz(sizeof(namedent_t) * max_named);

    // pass 2a: collect the aim entities
    data = cl.bsp->entitystring;
    while (CL_ParseEntityBlock(&data, &ent)) {
        if (ent.targetname[0] && ent.has_origin && num_named < max_named) {
            Q_strlcpy(named[num_named].name, ent.targetname, sizeof(named[num_named].name));
            VectorCopy(ent.origin, named[num_named].origin);
            num_named++;
        }
    }

    // pass 2b: build the lights
    data = cl.bsp->entitystring;
    while (CL_ParseEntityBlock(&data, &ent)) {
        cdynamiclight_t *dl;
        int             switch_index;

        if (strcmp(ent.classname, "dynamic_light") || !ent.has_origin)
            continue;

        // A light with a targetname is switched by a trigger, and spawnflag 1
        // means it starts off. The game owns that state and publishes it as
        // CS_DYNAMICLIGHTS; all this side has to do is agree on the bit index,
        // which is position among targetnamed dynamic_lights in lump order.
        // Counted BEFORE the reach test below so the two sides cannot drift.
        switch_index = -1;
        if (ent.targetname[0]) {
            if (num_switchable < MAX_SWITCHABLE_DLIGHTS)
                switch_index = num_switchable;
            num_switchable++;
            if (ent.spawnflags & 1)
                skipped_off++;
        }

        // a light with no reach contributes nothing
        if (ent.radius <= 0.0f || ent.intensity <= 0.0f)
            continue;

        dl = &cl_dynamiclights[cl_num_dynamiclights];
        VectorCopy(ent.origin, dl->origin);
        VectorCopy(ent.color, dl->color);
        dl->style = ent.style;
        dl->switch_index = switch_index;

        // the maps are consistent about writing _color in 0..1, but a stray
        // 0..255 value would be blindingly wrong rather than subtly wrong
        if (dl->color[0] > 1.5f || dl->color[1] > 1.5f || dl->color[2] > 1.5f)
            VectorScale(dl->color, 1.0f / 255.0f, dl->color);

        // A KEX shadow light falls off to nothing at shadowlightradius, while
        // these lights are physical and fall off with the inverse square. The
        // radius is therefore what sets how bright the emitter has to be, and
        // the product is scaled by a cvar because there is no exact conversion.
        dl->intensity = ent.intensity * ent.radius;

        // direction comes from the aim entity when there is one
        if (ent.target[0]) {
            for (int i = 0; i < num_named; i++) {
                if (strcmp(named[i].name, ent.target))
                    continue;

                VectorSubtract(named[i].origin, dl->origin, dl->direction);
                if (VectorNormalize(dl->direction) > 0.0f)
                    dl->is_spot = true;
                break;
            }
            if (!dl->is_spot)
                unresolved++;
        }

        // ...and otherwise from "angle", using Quake's two magic values
        if (!dl->is_spot && ent.has_cone && ent.has_angle) {
            if (ent.angle == -1.0f)
                VectorSet(dl->direction, 0.0f, 0.0f, 1.0f);
            else if (ent.angle == -2.0f)
                VectorSet(dl->direction, 0.0f, 0.0f, -1.0f);
            else
                VectorSet(dl->direction, cosf(DEG2RAD(ent.angle)), sinf(DEG2RAD(ent.angle)), 0.0f);
            dl->is_spot = true;
        }

        // 321 of the 765 targeted lights set no cone angle of their own, but
        // their aim entity is still placed deliberately, so they are spots
        // with an engine default rather than point lights.
        dl->cone_angle = ent.has_cone ? ent.cone_angle : 0.0f;

        cl_num_dynamiclights++;
    }

    Z_Free(named);

    Com_DPrintf("%s: %d dynamic_light (%d switchable, %d off at spawn, %d unresolved targets)\n",
                cl.mapname, cl_num_dynamiclights, num_switchable, skipped_off, unresolved);
}

void CL_FreeDynamicLights(void)
{
    Z_Free(cl_dynamiclights);
    cl_dynamiclights = NULL;
    cl_num_dynamiclights = 0;
}

/*
=================
CL_AddDynamicLightsToScene

Called every frame from CL_AddEntities. The renderer culls these by
cluster once they are in the light list, so there is no visibility
work to do here.
=================
*/
void CL_AddDynamicLightsToScene(void)
{
    float scale;
    unsigned switched;

    if (!cl_num_dynamiclights || !cl_dynamic_lights->integer)
        return;

    // the analytic light types only exist in the path tracer
    if (cls.ref_type != REF_TYPE_VKPT)
        return;

    scale = max(0.0f, cl_dynamic_light_scale->value);
    if (scale == 0.0f)
        return;

    // which switchable lights the game currently has lit. SP_dynamic_light
    // publishes this at spawn, so it is only ever empty on a map with no
    // switchable lights at all - and then nothing carries a switch_index.
    switched = strtoul(cl.configstrings[CS_DYNAMICLIGHTS], NULL, 16);

    for (int i = 0; i < cl_num_dynamiclights; i++) {
        const cdynamiclight_t *dl = &cl_dynamiclights[i];
        float intensity = dl->intensity * scale;

        // switched off by the game right now?
        if (dl->switch_index >= 0 && !(switched & (1u << dl->switch_index)))
            continue;

        if (dl->style)
            intensity *= CL_LightStyleValue(dl->style);

        if (intensity <= 0.0f)
            continue;

        if (dl->is_spot) {
            float cone = dl->cone_angle > 0.0f ? dl->cone_angle : cl_dynamic_light_cone->value;
            float half = max(1.0f, cone * 0.5f);

            // V_AddSpotLight takes half angles from the axis; the inner one
            // gives the cone a soft edge instead of a hard circle
            V_AddSpotLight(dl->origin, dl->direction, intensity,
                           dl->color[0], dl->color[1], dl->color[2],
                           half, half * 0.75f);
        } else {
            V_AddSphereLight(dl->origin, intensity,
                             dl->color[0], dl->color[1], dl->color[2],
                             DLIGHT_EMITTER_RADIUS);
        }
    }
}

void CL_InitDynamicLights(void)
{
    // The rerelease's dynamic_light entities. These are what actually light
    // its maps, so this defaults on.
    cl_dynamic_lights = Cvar_Get("cl_dynamic_lights", "1", CVAR_ARCHIVE);

    // Converts shadowlightintensity * shadowlightradius into the engine's
    // light units. There is no exact conversion - a KEX shadow light is
    // clamped to its radius and these are inverse-square - so this is the
    // knob to turn if the maps come out too bright or too dark.
    cl_dynamic_light_scale = Cvar_Get("cl_dynamic_light_scale", "0.5", CVAR_ARCHIVE);

    // Full cone angle, in degrees, for a light that aims at something but
    // sets no shadowlightconeangle of its own.
    cl_dynamic_light_cone = Cvar_Get("cl_dynamic_light_cone", "45", CVAR_ARCHIVE);
}
