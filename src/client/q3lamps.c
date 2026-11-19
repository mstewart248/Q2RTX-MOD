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
// q3lamps.c -- a real light on Quake III's flying skull lamps
//
// The skull lamps that hover in q3dm16, q3dm17, q3dm19 and q3tourney6 are map
// models baked into the BSP: skull (bot_lamp), pipe (bot_lamp2), wings
// (bot_wing), jets (bot_flare2) and a big lens-flare sprite in front of the
// pipe (bot_flare, autoSprite). Every one of those shaders carries the same two
// "deformVertexes move" waves, which is how the lamp bobs and drifts (0.1 and
// 0.05 Hz, about 15 units each way). vkpt q3deform.c draws the moving parts as
// a model; the flare sprite is a fake effect and is not drawn (q3shader.c
// q3_analyze). Quake III had no light there at all - only that glow card.
//
// In its place: a spot light at the flare sprite's centre, moved every frame
// by the flare shader's own move deforms on the clock q3deform.c animates the
// model with (refdef time), so it stays on the lamp. It points from the middle
// of the lamp body out through the flare - forward and down out of the pipe.
//
// The flare cards are not in cl.bsp (bsp_q3.c leaves out no-draw surfaces), so
// they are read from the map's own draw surfaces at load.
//
// Cvars (all live, archived):
//   cl_q3_lamp_light       1 = on
//   cl_q3_lamp_intensity   brightness, in the same units as the light editor's
//                          light_scale (2000 = its brightness 255)
//   cl_q3_lamp_color       "r g b", 0..1 - default the flare image's own pink
//   cl_q3_lamp_cone        full cone angle, degrees
//

#include "client.h"
#include "common/q3shader.h"
#include "format/bsp3.h"

#define MAX_Q3LAMPS     64
#define LAMP_REACH      128.0f  // lamp body faces this close to its flare are its own

typedef struct {
    vec3_t      origin;         // the flare's centre at rest
    vec3_t      dir;            // the spot's axis
    int         num_deforms;
    q3deform_t  deforms[Q3S_MAX_DEFORMS];   // the flare shader's, for its move
} q3lamp_t;

static q3lamp_t q3lamps[MAX_Q3LAMPS];
static int      num_q3lamps;

static cvar_t   *cl_q3_lamp_light;
static cvar_t   *cl_q3_lamp_intensity;
static cvar_t   *cl_q3_lamp_color;
static cvar_t   *cl_q3_lamp_cone;

// The flare sprite of a moving lamp: a fake (not drawn) autoSprite that moves.
static bool lamp_flare_shader(const q3shader_t *sh)
{
    return sh && sh->fake_effect && sh->deform_autosprite && Q3Shader_Travels(sh);
}

// Whether two shaders' move deforms are the same - the parts of one lamp.
static bool same_moves(const q3shader_t *a, const q3shader_t *b)
{
    int ia = 0, ib = 0;

    for (;;) {
        while (ia < a->num_deforms && a->deforms[ia].type != Q3DEFORM_MOVE)
            ia++;
        while (ib < b->num_deforms && b->deforms[ib].type != Q3DEFORM_MOVE)
            ib++;
        if (ia == a->num_deforms || ib == b->num_deforms)
            return ia == a->num_deforms && ib == b->num_deforms;

        const q3deform_t *da = &a->deforms[ia++], *db = &b->deforms[ib++];
        if (!VectorCompare(da->move, db->move) || da->wave.func != db->wave.func ||
            da->wave.base != db->wave.base || da->wave.amp != db->wave.amp ||
            da->wave.phase != db->wave.phase || da->wave.freq != db->wave.freq)
            return false;
    }
}

static const void *get_lump(const byte *data, int len, int lump, size_t elem, int *count)
{
    const q3dheader_t *h = (const q3dheader_t *)data;
    uint32_t ofs = LittleLong(h->lumps[lump].fileofs);
    uint32_t size = LittleLong(h->lumps[lump].filelen);

    *count = 0;
    if (ofs > (uint32_t)len || size > (uint32_t)len - ofs || size % elem)
        return NULL;
    *count = (int)(size / elem);
    return data + ofs;
}

// The lump's name is 64 chars with no room promised for the terminator.
static const q3shader_t *find_shader(const q3dshader_t *ds)
{
    char name[sizeof(ds->shader) + 1];

    memcpy(name, ds->shader, sizeof(ds->shader));
    name[sizeof(ds->shader)] = 0;
    return Q3Shader_Find(name);
}

static void draw_vert(const q3drawvert_t *v, vec3_t out)
{
    out[0] = LittleFloat(v->xyz[0]);
    out[1] = LittleFloat(v->xyz[1]);
    out[2] = LittleFloat(v->xyz[2]);
}

// Points the spot from the middle of the lamp's body (the opaque surfaces near
// the flare that move with it) through the flare's centre.
static void aim_lamp(q3lamp_t *lamp, const q3shader_t *flare,
                     const q3dsurface_t *surfs, int num_surfs,
                     const q3drawvert_t *verts, int num_verts,
                     const q3dshader_t *shaders, int num_shaders)
{
    vec3_t mins, maxs, p;
    bool any = false;

    ClearBounds(mins, maxs);
    for (int i = 0; i < num_surfs; i++) {
        const q3dsurface_t *s = &surfs[i];
        int sn = LittleLong(s->shaderNum);
        int fv = LittleLong(s->firstVert), nv = LittleLong(s->numVerts);

        if (sn < 0 || sn >= num_shaders || fv < 0 || nv <= 0 || fv + nv > num_verts)
            continue;

        const q3shader_t *sh = find_shader(&shaders[sn]);
        if (!sh || sh == flare || sh->additive || sh->no_draw || !same_moves(sh, flare))
            continue;

        for (int v = 0; v < nv; v++) {
            draw_vert(&verts[fv + v], p);
            if (Distance(p, lamp->origin) > LAMP_REACH)
                continue;
            AddPointToBounds(p, mins, maxs);
            any = true;
        }
    }

    VectorSet(lamp->dir, 0.0f, 0.0f, -1.0f);
    if (any) {
        vec3_t mid, dir;
        VectorAvg(mins, maxs, mid);
        VectorSubtract(lamp->origin, mid, dir);
        if (VectorNormalize(dir) > 1.0f)
            VectorCopy(dir, lamp->dir);
    }
}

static void add_lamp(const vec3_t origin, const q3shader_t *flare,
                     const q3dsurface_t *surfs, int num_surfs,
                     const q3drawvert_t *verts, int num_verts,
                     const q3dshader_t *shaders, int num_shaders)
{
    if (num_q3lamps >= MAX_Q3LAMPS)
        return;

    q3lamp_t *lamp = &q3lamps[num_q3lamps++];
    VectorCopy(origin, lamp->origin);
    lamp->num_deforms = flare->num_deforms;
    memcpy(lamp->deforms, flare->deforms, sizeof(lamp->deforms));
    aim_lamp(lamp, flare, surfs, num_surfs, verts, num_verts, shaders, num_shaders);
}

void CL_LoadQ3Lamps(void)
{
    void *buf = NULL;
    int len;

    num_q3lamps = 0;

    if (!cl.bsp || !cl.bsp->q3)
        return;

    len = FS_LoadFile(cl.bsp->name, &buf);
    if (!buf)
        return;

    const byte *data = buf;
    const q3dheader_t *h = buf;
    if (len < (int)sizeof(*h) || LittleLong(h->version) != Q3_BSPVERSION) {
        FS_FreeFile(buf);
        return;
    }

    int num_shaders, num_verts, num_surfs;
    const q3dshader_t *shaders = get_lump(data, len, Q3_LUMP_SHADERS, sizeof(q3dshader_t), &num_shaders);
    const q3drawvert_t *verts = get_lump(data, len, Q3_LUMP_DRAWVERTS, sizeof(q3drawvert_t), &num_verts);
    const q3dsurface_t *surfs = get_lump(data, len, Q3_LUMP_SURFACES, sizeof(q3dsurface_t), &num_surfs);

    if (shaders && verts && surfs) {
        Q3Shader_Load();

        for (int i = 0; i < num_surfs; i++) {
            const q3dsurface_t *s = &surfs[i];
            int sn = LittleLong(s->shaderNum);
            int fv = LittleLong(s->firstVert), nv = LittleLong(s->numVerts);

            if (sn < 0 || sn >= num_shaders || fv < 0 || nv <= 0 || fv + nv > num_verts)
                continue;

            const q3shader_t *sh = find_shader(&shaders[sn]);
            if (!lamp_flare_shader(sh))
                continue;

            // autoSprite: every 4 vertices are one sprite (RB_AutoSprite)
            int per = (nv % 4 == 0) ? 4 : nv;
            for (int first = 0; first < nv; first += per) {
                vec3_t mid = { 0, 0, 0 }, p;
                for (int v = 0; v < per; v++) {
                    draw_vert(&verts[fv + first + v], p);
                    VectorAdd(mid, p, mid);
                }
                VectorScale(mid, 1.0f / per, mid);
                add_lamp(mid, sh, surfs, num_surfs, verts, num_verts, shaders, num_shaders);
            }
        }
    }

    FS_FreeFile(buf);

    if (num_q3lamps)
        Com_DPrintf("Quake III lamps: %d moving flare light(s)\n", num_q3lamps);
}

void CL_FreeQ3Lamps(void)
{
    num_q3lamps = 0;
}

void CL_AddQ3LampsToScene(void)
{
    if (!num_q3lamps || !cl_q3_lamp_light->integer)
        return;

    float intensity = cl_q3_lamp_intensity->value;
    if (intensity <= 0.0f)
        return;

    vec3_t color = { 1.0f, 0.76f, 0.76f };
    if (sscanf(cl_q3_lamp_color->string, "%f %f %f", &color[0], &color[1], &color[2]) != 3)
        VectorSet(color, 1.0f, 0.76f, 0.76f);

    float half = cl_q3_lamp_cone->value * 0.5f;
    clamp(half, 1.0f, 89.0f);
    const float time = cl.time * 0.001f;   // = cl.refdef.time, q3deform.c's clock

    for (int i = 0; i < num_q3lamps; i++) {
        const q3lamp_t *lamp = &q3lamps[i];
        vec3_t offset, origin;

        Q3Shader_MoveOffset(lamp->deforms, lamp->num_deforms, time, offset);
        VectorAdd(lamp->origin, offset, origin);

        // V_AddSpotLight takes half angles from the axis; the inner one gives
        // the cone a soft edge
        V_AddSpotLight(origin, lamp->dir, intensity, color[0], color[1], color[2],
                       half, half * 0.6f);
        // one light all its life, though it moves (vkpt main.c add_dlights)
        V_SetLightId(0x51A40000u | (uint32_t)i);
    }
}

void CL_InitQ3Lamps(void)
{
    // 1 = a spot light in place of the flying skull lamps' flare sprites
    cl_q3_lamp_light = Cvar_Get("cl_q3_lamp_light", "1", CVAR_ARCHIVE);
    // brightness; 2000 = the light editor's brightness 255 (light_scale)
    cl_q3_lamp_intensity = Cvar_Get("cl_q3_lamp_intensity", "1500", CVAR_ARCHIVE);
    // the flare image's colour: bot_flare.jpg's bright pixels average 176 133 133
    cl_q3_lamp_color = Cvar_Get("cl_q3_lamp_color", "1 0.76 0.76", CVAR_ARCHIVE);
    // full cone, degrees
    cl_q3_lamp_cone = Cvar_Get("cl_q3_lamp_cone", "100", CVAR_ARCHIVE);
}
