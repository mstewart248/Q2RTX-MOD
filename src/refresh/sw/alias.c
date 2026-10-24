/*
Copyright (C) 1997-2001 Id Software, Inc.

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
// r_alias.c: routines for setting up to draw alias models

/*
** use a real variable to control lerping
*/
#include "sw.h"

int             r_amodels_drawn;

affinetridesc_t r_affinetridesc;

int             r_alias_alpha;
int             r_alias_one_minus_alpha;
fixed8_t        r_aliasblendcolor[3];

static vec3_t   r_plightvec;

// 8-bit mode's model light, the original's
#define LIGHT_MIN   5   // lowest light value we'll allow, to avoid the
                        // need for inner-loop light clamping
static int      r_ambientlight;
static float    r_shadelight;
static vec3_t   r_lerp_frontv, r_lerp_backv, r_lerp_move;

static maliasframe_t    *r_thisframe, *r_lastframe;
static int              r_thisframenum, r_lastframenum;
static float            r_entscale;

// work vertices for one mesh
static finalvert_t      r_finalverts[MAX_ALIAS_VERTS];

static float    aliastransform[3][4];
static float    aliasworldtransform[3][4];
static float    aliasoldworldtransform[3][4];

static float    s_ziscale;
static vec3_t   s_alias_forward, s_alias_right, s_alias_up;

static void R_AliasSetupBlend(void);

#define BBOX_TRIVIAL_ACCEPT 0
#define BBOX_MUST_CLIP_XY   1
#define BBOX_MUST_CLIP_Z    2
#define BBOX_TRIVIAL_REJECT 8

/*
================
R_AliasTransformVector
================
*/
static void R_AliasTransformVector(vec3_t in, vec3_t out, float xf[3][4])
{
    out[0] = DotProduct(in, xf[0]) + xf[0][3];
    out[1] = DotProduct(in, xf[1]) + xf[1][3];
    out[2] = DotProduct(in, xf[2]) + xf[2][3];
}


/*
** R_AliasCheckFrameBBox
**
** Checks a specific alias frame bounding box
*/
static unsigned long R_AliasCheckFrameBBox(maliasframe_t *frame, float worldxf[3][4])
{
    unsigned long aggregate_and_clipcode = ~0U,
                  aggregate_or_clipcode = 0;
    int           i;
    vec3_t        mins, maxs;
    vec3_t        transformed_min, transformed_max;
    bool      zclipped = false, zfullyclipped = true;

    /*
    ** get the exact frame bounding box
    */
    for (i = 0; i < 3; i++) {
        mins[i] = frame->bounds[0][i] * r_entscale;
        maxs[i] = frame->bounds[1][i] * r_entscale;
    }

    /*
    ** transform the min and max values into view space
    */
    R_AliasTransformVector(mins, transformed_min, aliastransform);
    R_AliasTransformVector(maxs, transformed_max, aliastransform);

    if (transformed_min[2] >= ALIAS_Z_CLIP_PLANE)
        zfullyclipped = false;
    if (transformed_max[2] >= ALIAS_Z_CLIP_PLANE)
        zfullyclipped = false;

    if (zfullyclipped) {
        return BBOX_TRIVIAL_REJECT;
    }
    if (zclipped) {
        return (BBOX_MUST_CLIP_XY | BBOX_MUST_CLIP_Z);
    }

    /*
    ** build a transformed bounding box from the given min and max
    */
    for (i = 0; i < 8; i++) {
        int      j;
        vec3_t   tmp, transformed;
        unsigned long clipcode = 0;

        if (i & 1)
            tmp[0] = mins[0];
        else
            tmp[0] = maxs[0];

        if (i & 2)
            tmp[1] = mins[1];
        else
            tmp[1] = maxs[1];

        if (i & 4)
            tmp[2] = mins[2];
        else
            tmp[2] = maxs[2];

        R_AliasTransformVector(tmp, transformed, worldxf);

        for (j = 0; j < 4; j++) {
            float dp = DotProduct(transformed, view_clipplanes[j].normal);

            if ((dp - view_clipplanes[j].dist) < 0.0F)
                clipcode |= 1 << j;
        }

        aggregate_and_clipcode &= clipcode;
        aggregate_or_clipcode  |= clipcode;
    }

    if (aggregate_and_clipcode) {
        return BBOX_TRIVIAL_REJECT;
    }
    if (!aggregate_or_clipcode) {
        return BBOX_TRIVIAL_ACCEPT;
    }

    return BBOX_MUST_CLIP_XY;
}

/*
================
R_AliasCheckBBox
================
*/
static bool R_AliasCheckBBox(void)
{
    unsigned long ccodes[2] = { 0, 0 };

    ccodes[0] = R_AliasCheckFrameBBox(r_thisframe, aliasworldtransform);

    /*
    ** non-lerping model
    */
    if (currententity->backlerp == 0) {
        if (ccodes[0] == BBOX_TRIVIAL_ACCEPT)
            return BBOX_TRIVIAL_ACCEPT;
        else if (ccodes[0] & BBOX_TRIVIAL_REJECT)
            return BBOX_TRIVIAL_REJECT;
        else
            return (ccodes[0] & ~BBOX_TRIVIAL_REJECT);
    }

    ccodes[1] = R_AliasCheckFrameBBox(r_lastframe, aliasoldworldtransform);

    if ((ccodes[0] | ccodes[1]) == BBOX_TRIVIAL_ACCEPT)
        return BBOX_TRIVIAL_ACCEPT;
    else if ((ccodes[0] & ccodes[1]) & BBOX_TRIVIAL_REJECT)
        return BBOX_TRIVIAL_REJECT;
    else
        return (ccodes[0] | ccodes[1]) & ~BBOX_TRIVIAL_REJECT;
}


/*
================
R_AliasProjectAndClipTestFinalVert
================
*/
void R_AliasProjectAndClipTestFinalVert(finalvert_t *fv)
{
    float   zi;
    float   x, y, z;

    // project points
    x = fv->xyz[0];
    y = fv->xyz[1];
    z = fv->xyz[2];
    zi = 1.0 / z;

    fv->zi = zi * s_ziscale;

    fv->u = (x * r_refdef.xscale * zi) + r_refdef.xcenter;
    fv->v = (y * r_refdef.yscale * zi) + r_refdef.ycenter;

    if (fv->u < r_refdef.vrect.x)
        fv->flags |= ALIAS_LEFT_CLIP;
    if (fv->v < r_refdef.vrect.y)
        fv->flags |= ALIAS_TOP_CLIP;
    if (fv->u > r_refdef.vrectright)
        fv->flags |= ALIAS_RIGHT_CLIP;
    if (fv->v > r_refdef.vrectbottom)
        fv->flags |= ALIAS_BOTTOM_CLIP;
}

/*
================
R_AliasTransformFinalVerts
================
*/
static void R_AliasTransformFinalVerts(int numpoints, finalvert_t *fv, maliasvert_t *oldv, maliasvert_t *newv)
{
    int i;

    for (i = 0; i < numpoints; i++, fv++, oldv++, newv++) {
        float       lightcos;
        const vec_t *plightnormal;
        vec3_t      lerped_vert;

        lerped_vert[0] = r_lerp_move[0] + oldv->v[0] * r_lerp_backv[0] + newv->v[0] * r_lerp_frontv[0];
        lerped_vert[1] = r_lerp_move[1] + oldv->v[1] * r_lerp_backv[1] + newv->v[1] * r_lerp_frontv[1];
        lerped_vert[2] = r_lerp_move[2] + oldv->v[2] * r_lerp_backv[2] + newv->v[2] * r_lerp_frontv[2];

        plightnormal = bytedirs[newv->lightnormalindex];

        if (currententity->flags & RF_SHELL_MASK) {
            float   scale;

            scale = (currententity->flags & RF_WEAPONMODEL) ?
                WEAPONSHELL_SCALE : POWERSUIT_SCALE;

            lerped_vert[0] += plightnormal[0] * scale;
            lerped_vert[1] += plightnormal[1] * scale;
            lerped_vert[2] += plightnormal[2] * scale;
        }

        fv->xyz[0] = DotProduct(lerped_vert, aliastransform[0]) + aliastransform[0][3];
        fv->xyz[1] = DotProduct(lerped_vert, aliastransform[1]) + aliastransform[1][3];
        fv->xyz[2] = DotProduct(lerped_vert, aliastransform[2]) + aliastransform[2][3];

        fv->flags = 0;

        // lighting
        lightcos = DotProduct(plightnormal, r_plightvec);
        if (r_8bit) {
            // a colormap row << 8: smaller is brighter
            int temp = r_ambientlight;

            if (lightcos < 0) {
                temp += (int)(r_shadelight * lightcos);

                // clamp; because we limited the minimum ambient and shading light, we
                // don't have to clamp low light, just bright
                if (temp < 0)
                    temp = 0;
            }

            fv->l = temp;
        } else {
            if (lightcos < 0)
                lightcos *= 0.3f;

            fv->l = 0x8000 + (int)(0x7fff * lightcos);
        }

        if (fv->xyz[2] < ALIAS_Z_CLIP_PLANE) {
            fv->flags |= ALIAS_Z_CLIP;
        } else {
            R_AliasProjectAndClipTestFinalVert(fv);
        }
    }
}


/*
================
R_AliasSetupSkin

Skin coordinates are 0..1 across, so a skin of any size fits.
================
*/
static byte *r_skinglow;     // this mesh's skin glow map, or NULL

static void R_AliasSetupSkin(const maliasmesh_t *mesh)
{
    int             skinnum;
    image_t         *pskindesc;

    r_skinglow = NULL;

    if (currententity->skin) {
        pskindesc = IMG_ForHandle(currententity->skin);
    } else if (mesh->numskins) {
        skinnum = currententity->skinnum;
        if ((skinnum >= mesh->numskins) || (skinnum < 0)) {
            Com_DPrintf("R_AliasSetupSkin %s: no such skin # %d\n",
                        currentmodel->name, skinnum);
            skinnum = 0;
        }
        pskindesc = mesh->skins[skinnum];
    } else {
        pskindesc = NULL;
    }

    if (pskindesc == NULL || !pskindesc->sw_pixels[0] || (r_8bit && !pskindesc->sw_pixels8[0]) ||
        pskindesc->upload_width < 1 || pskindesc->upload_height < 1) {
        memset(&r_affinetridesc, 0, sizeof(r_affinetridesc));
        return;
    }

    r_affinetridesc.pskin       = r_8bit ? pskindesc->sw_pixels8[0] : pskindesc->sw_pixels[0];
    r_affinetridesc.skinwidth   = pskindesc->upload_width * r_texbytes;

    if (!r_8bit && pskindesc->sw_glow[0])
        r_skinglow = pskindesc->sw_glow[0];
    r_affinetridesc.skinheight  = pskindesc->upload_height;
}

static inline void R_AliasSetST(finalvert_t *fv, const maliasst_t *st, int smax, int tmax)
{
    fv->s = Q_clip((int)(st->st[0] * smax), 0, smax - 1);
    fv->t = Q_clip((int)(st->st[1] * tmax), 0, tmax - 1);
}

/*
================
R_AliasPrepareMesh

General clipped case
================
*/
static void R_AliasPrepareMesh(const maliasmesh_t *mesh)
{
    int         i, smax, tmax;
    maliasst_t  *pstverts;
    maliastri_t *ptri;
    finalvert_t *pfv[3];
    finalvert_t *pfinalverts = r_finalverts;
    bool        reflect;

    if (mesh->numverts > MAX_ALIAS_VERTS)
        return;

    R_AliasSetupSkin(mesh);
    R_AliasSetupBlend();

    // in 16.16 skin texels
    smax = (r_affinetridesc.skinwidth / r_texbytes) << 16;
    tmax = r_affinetridesc.skinheight << 16;
    if (!r_affinetridesc.pskin)
        smax = tmax = 1;

    R_AliasTransformFinalVerts(mesh->numverts, pfinalverts,
                               mesh->verts + r_lastframenum * mesh->numverts,
                               mesh->verts + r_thisframenum * mesh->numverts);

// clip and draw all triangles
//
    pstverts = mesh->sts;
    ptri = mesh->tris;
    reflect = (currententity->flags & (RF_WEAPONMODEL | RF_LEFTHAND)) == (RF_WEAPONMODEL | RF_LEFTHAND);

    for (i = 0; i < mesh->numtris; i++, ptri++) {
        pfv[0] = &pfinalverts[ptri->index_xyz[0]];
        pfv[1] = &pfinalverts[ptri->index_xyz[1]];
        pfv[2] = &pfinalverts[ptri->index_xyz[2]];

        if (pfv[0]->flags & pfv[1]->flags & pfv[2]->flags)
            continue;       // completely clipped

        // insert s/t coordinates
        R_AliasSetST(pfv[0], &pstverts[ptri->index_st[0]], smax, tmax);
        R_AliasSetST(pfv[1], &pstverts[ptri->index_st[1]], smax, tmax);
        R_AliasSetST(pfv[2], &pstverts[ptri->index_st[2]], smax, tmax);

        if (reflect) {
            finalvert_t *tmp = pfv[0];
            pfv[0] = pfv[2];
            pfv[2] = tmp;
        }

        if (!(pfv[0]->flags | pfv[1]->flags | pfv[2]->flags)) {
            // totally unclipped
            aliastriangleparms.a = pfv[0];
            aliastriangleparms.b = pfv[1];
            aliastriangleparms.c = pfv[2];

            R_DrawTriangle();
        } else {
            // partially clipped
            R_AliasClipTriangle(pfv[0], pfv[1], pfv[2]);
        }
    }
}

/*
================
R_ConcatTransforms
================
*/
static void R_ConcatTransforms(float in1[3][4], float in2[3][4], float out[3][4])
{
    out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
    out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
    out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
    out[0][3] = in1[0][0] * in2[0][3] + in1[0][1] * in2[1][3] + in1[0][2] * in2[2][3] + in1[0][3];
    out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
    out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
    out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
    out[1][3] = in1[1][0] * in2[0][3] + in1[1][1] * in2[1][3] + in1[1][2] * in2[2][3] + in1[1][3];
    out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
    out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
    out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
    out[2][3] = in1[2][0] * in2[0][3] + in1[2][1] * in2[1][3] + in1[2][2] * in2[2][3] + in1[2][3];
}


/*
================
R_AliasSetUpTransform
================
*/
static void R_AliasSetUpTransform(void)
{
    int             i;
    float           viewmatrix[3][4];
    vec3_t          angles;

// TODO: should really be stored with the entity instead of being reconstructed
// TODO: should use a look-up table
// TODO: could cache lazily, stored in the entity
//
    angles[ROLL] = currententity->angles[ROLL];
    angles[PITCH] = currententity->angles[PITCH];
    angles[YAW] = currententity->angles[YAW];
    AngleVectors(angles, s_alias_forward, s_alias_right, s_alias_up);

// TODO: can do this with simple matrix rearrangement

    memset(aliasworldtransform, 0, sizeof(aliasworldtransform));
    memset(aliasoldworldtransform, 0, sizeof(aliasoldworldtransform));

    for (i = 0; i < 3; i++) {
        aliasoldworldtransform[i][0] = aliasworldtransform[i][0] =  s_alias_forward[i];
        aliasoldworldtransform[i][1] = aliasworldtransform[i][1] = -s_alias_right[i];
        aliasoldworldtransform[i][2] = aliasworldtransform[i][2] =  s_alias_up[i];
    }

    aliasworldtransform[0][3] = currententity->origin[0] - r_origin[0];
    aliasworldtransform[1][3] = currententity->origin[1] - r_origin[1];
    aliasworldtransform[2][3] = currententity->origin[2] - r_origin[2];

    aliasoldworldtransform[0][3] = currententity->oldorigin[0] - r_origin[0];
    aliasoldworldtransform[1][3] = currententity->oldorigin[1] - r_origin[1];
    aliasoldworldtransform[2][3] = currententity->oldorigin[2] - r_origin[2];

// FIXME: can do more efficiently than full concatenation
//  memcpy(rotationmatrix, t2matrix, sizeof(rotationmatrix));

//  R_ConcatTransforms(t2matrix, tmatrix, rotationmatrix);

// TODO: should be global, set when vright, etc., set
    VectorCopy(vright, viewmatrix[0]);
    VectorCopy(vup, viewmatrix[1]);
    VectorNegate(viewmatrix[1], viewmatrix[1]);
    VectorCopy(vpn, viewmatrix[2]);

    viewmatrix[0][3] = 0;
    viewmatrix[1][3] = 0;
    viewmatrix[2][3] = 0;

//  memcpy(aliasworldtransform, rotationmatrix, sizeof(aliastransform));

    R_ConcatTransforms(viewmatrix, aliasworldtransform, aliastransform);

    aliasworldtransform[0][3] = currententity->origin[0];
    aliasworldtransform[1][3] = currententity->origin[1];
    aliasworldtransform[2][3] = currententity->origin[2];

    aliasoldworldtransform[0][3] = currententity->oldorigin[0];
    aliasoldworldtransform[1][3] = currententity->oldorigin[1];
    aliasoldworldtransform[2][3] = currententity->oldorigin[2];
}

/*
================
R_AliasSetupLighting
================
*/
static void R_AliasSetupLighting(void)
{
    float           cp, cy, sp, sy;
    vec_t           yaw;
    vec3_t          light;
    int             i;

    // all components of light should be identical in software
    if (currententity->flags & RF_FULLBRIGHT) {
        VectorSet(light, 1, 1, 1);
    } else {
        R_LightPoint(currententity->origin, light);
    }

    if (currententity->flags & RF_MINLIGHT) {
        for (i = 0; i < 3; i++)
            if (light[i] < 0.1)
                light[i] = 0.1;
    }

    if (currententity->flags & RF_GLOW) {
        // bonus items will pulse with time
        float   scale;
        float   min;

        scale = 0.1 * sin(r_newrefdef.time * 7);
        for (i = 0; i < 3; i++) {
            min = light[i] * 0.8;
            light[i] += scale;
            if (light[i] < min)
                light[i] = min;
        }
    }

    for (i = 0; i < 3; i++)
        clamp(light[i], 0, 1);

    r_aliasblendcolor[0] = light[0] * 255;
    r_aliasblendcolor[1] = light[1] * 255;
    r_aliasblendcolor[2] = light[2] * 255;

    if (r_8bit) {
        static const vec3_t lightvec = { -1, 0, 0 };
        int ambientlight, shadelight, j;

        j = (light[0] + light[1] + light[2]) * 0.3333f * 255;

        ambientlight = j;
        shadelight = j;

    // clamp lighting so it doesn't overbright as much
        if (ambientlight > 128)
            ambientlight = 128;
        if (ambientlight + shadelight > 192)
            shadelight = 192 - ambientlight;

    // guarantee that no vertex will ever be lit below LIGHT_MIN, so we don't have
    // to clamp off the bottom
        r_ambientlight = ambientlight;

        if (r_ambientlight < LIGHT_MIN)
            r_ambientlight = LIGHT_MIN;

        r_ambientlight = (255 - r_ambientlight) << VID_CBITS;

        if (r_ambientlight < LIGHT_MIN)
            r_ambientlight = LIGHT_MIN;

        r_shadelight = max(shadelight, 0) * VID_GRADES;

    // rotate the lighting vector into the model's frame of reference
        r_plightvec[0] =  DotProduct(lightvec, s_alias_forward);
        r_plightvec[1] = -DotProduct(lightvec, s_alias_right);
        r_plightvec[2] =  DotProduct(lightvec, s_alias_up);
        return;
    }

// rotate the lighting vector into the model's frame of reference
    yaw = -DEG2RAD(currententity->angles[YAW]);
    cy = cos(yaw);
    sy = sin(yaw);
    cp = cos(-M_PI / 4);
    sp = sin(-M_PI / 4);
    r_plightvec[0] = cp * cy;
    r_plightvec[1] = cp * sy;
    r_plightvec[2] = -sp;
}


/*
=================
R_AliasSetupFrames

=================
*/
static void R_AliasSetupFrames(void)
{
    int thisframe = currententity->frame;
    int lastframe = currententity->oldframe;

    if (thisframe >= currentmodel->numframes || thisframe < 0) {
        Com_DPrintf("%s: %s: no such thisframe %d\n",
                    __func__, currentmodel->name, thisframe);
        thisframe = 0;
    }
    if (lastframe >= currentmodel->numframes || lastframe < 0) {
        Com_DPrintf("%s: %s: no such lastframe %d\n",
                    __func__, currentmodel->name, lastframe);
        lastframe = 0;
    }

    r_thisframenum = thisframe;
    r_lastframenum = lastframe;
    r_thisframe = &currentmodel->frames[thisframe];
    r_lastframe = &currentmodel->frames[lastframe];
}

/*
** R_AliasSetUpLerpData
**
** Precomputes lerp coefficients used for the whole frame.
*/
static void R_AliasSetUpLerpData(float backlerp)
{
    float   frontlerp;
    vec3_t  translation, vectors[3];
    int     i;

    frontlerp = 1.0F - backlerp;

    /*
    ** convert entity's angles into discrete vectors for R, U, and F
    */
    AngleVectors(currententity->angles, vectors[0], vectors[1], vectors[2]);

    /*
    ** translation is the vector from last position to this position
    */
    VectorSubtract(currententity->oldorigin, currententity->origin, translation);

    /*
    ** move should be the delta back to the previous frame * backlerp
    */
    r_lerp_move[0] =  DotProduct(translation, vectors[0]);  // forward
    r_lerp_move[1] = -DotProduct(translation, vectors[1]);  // left
    r_lerp_move[2] =  DotProduct(translation, vectors[2]);  // up

    VectorMA(r_lerp_move, r_entscale, r_lastframe->translate, r_lerp_move);

    for (i = 0; i < 3; i++) {
        r_lerp_move[i] = backlerp * r_lerp_move[i] + frontlerp * r_entscale * r_thisframe->translate[i];
    }

    for (i = 0; i < 3; i++) {
        r_lerp_frontv[i] = frontlerp * r_entscale * r_thisframe->scale[i];
        r_lerp_backv[i]  = backlerp  * r_entscale * r_lastframe->scale[i];
    }
}

static void R_AliasSetupBlend(void)
{
    extern void (*d_pdrawspans)(void *);
    extern void R_PolysetDrawSpansConstant8_Blended(void *);
    extern void R_PolysetDrawSpans8_Blended(void *);
    extern void R_PolysetDrawSpans8_Opaque(void *);
    extern void R_PolysetDrawSpans8_OpaqueGlow(void *);
    extern void R_PolysetDrawSpansPal_Opaque(void *);
    extern void R_PolysetDrawSpansPal_33(void *);
    extern void R_PolysetDrawSpansPal_66(void *);
    extern void R_PolysetDrawSpansPalConstant_33(void *);
    extern void R_PolysetDrawSpansPalConstant_66(void *);

    int         mask;
    color_t     color;

    if (currententity->flags & RF_TRANSLUCENT) {
        r_alias_alpha = 255 * currententity->alpha;
        r_alias_one_minus_alpha = 255 - r_alias_alpha;
    } else {
        r_alias_alpha = 255;
        r_alias_one_minus_alpha = 0;
    }

    /*
    ** select the proper span routine based on translucency
    */
    mask = currententity->flags & RF_SHELL_MASK;
    if (r_8bit) {
        r_iractive = (r_newrefdef.rdflags & RDF_IRGOGGLES) && (currententity->flags & RF_IR_VISIBLE);

        // the original's choices
        if (mask || r_affinetridesc.pskin == NULL) {
            if (mask == RF_SHELL_RED)
                r_aliasblendcolor8 = SHELL_RED_COLOR;
            else if (mask == RF_SHELL_GREEN)
                r_aliasblendcolor8 = SHELL_GREEN_COLOR;
            else if (mask == RF_SHELL_BLUE)
                r_aliasblendcolor8 = SHELL_BLUE_COLOR;
            else if (mask == (RF_SHELL_RED | RF_SHELL_GREEN))
                r_aliasblendcolor8 = SHELL_RG_COLOR;
            else if (mask == (RF_SHELL_RED | RF_SHELL_BLUE))
                r_aliasblendcolor8 = SHELL_RB_COLOR;
            else if (mask == (RF_SHELL_BLUE | RF_SHELL_GREEN))
                r_aliasblendcolor8 = SHELL_BG_COLOR;
            else if (mask == RF_SHELL_DOUBLE)
                r_aliasblendcolor8 = SHELL_DOUBLE_COLOR;
            else if (mask == RF_SHELL_HALF_DAM)
                r_aliasblendcolor8 = SHELL_HALF_DAM_COLOR;
            else
                r_aliasblendcolor8 = SHELL_WHITE_COLOR;

            if (currententity->alpha > 0.33f)
                d_pdrawspans = R_PolysetDrawSpansPalConstant_66;
            else
                d_pdrawspans = R_PolysetDrawSpansPalConstant_33;
        } else if (currententity->flags & RF_TRANSLUCENT) {
            if (currententity->alpha > 0.66f)
                d_pdrawspans = R_PolysetDrawSpansPal_Opaque;
            else if (currententity->alpha > 0.33f)
                d_pdrawspans = R_PolysetDrawSpansPal_66;
            else
                d_pdrawspans = R_PolysetDrawSpansPal_33;
        } else {
            d_pdrawspans = R_PolysetDrawSpansPal_Opaque;
        }
        return;
    }

    if (mask || r_affinetridesc.pskin == NULL) {
        if (mask == RF_SHELL_RED)
            color.u32 = d_8to24table[SHELL_RED_COLOR];
        else if (mask == RF_SHELL_GREEN)
            color.u32 = d_8to24table[SHELL_GREEN_COLOR];
        else if (mask == RF_SHELL_BLUE)
            color.u32 = d_8to24table[SHELL_BLUE_COLOR];
        else if (mask == (RF_SHELL_RED | RF_SHELL_GREEN))
            color.u32 = d_8to24table[SHELL_RG_COLOR];
        else if (mask == (RF_SHELL_RED | RF_SHELL_BLUE))
            color.u32 = d_8to24table[SHELL_RB_COLOR];
        else if (mask == (RF_SHELL_BLUE | RF_SHELL_GREEN))
            color.u32 = d_8to24table[SHELL_BG_COLOR];
        else if (mask == RF_SHELL_DOUBLE)
            color.u32 = d_8to24table[SHELL_DOUBLE_COLOR];
        else if (mask == RF_SHELL_HALF_DAM)
            color.u32 = d_8to24table[SHELL_HALF_DAM_COLOR];
        else
            color.u32 = d_8to24table[SHELL_WHITE_COLOR];

        r_aliasblendcolor[0] = color.u8[0] * r_alias_alpha;
        r_aliasblendcolor[1] = color.u8[1] * r_alias_alpha;
        r_aliasblendcolor[2] = color.u8[2] * r_alias_alpha;

        d_pdrawspans = R_PolysetDrawSpansConstant8_Blended;
    } else if ((currententity->flags & RF_TRANSLUCENT) && r_alias_alpha != 255) {
        d_pdrawspans = R_PolysetDrawSpans8_Blended;
    } else if (r_skinglow) {
        r_skinglowdelta = r_skinglow - (byte *)r_affinetridesc.pskin;
        d_pdrawspans = R_PolysetDrawSpans8_OpaqueGlow;
    } else {
        d_pdrawspans = R_PolysetDrawSpans8_Opaque;
    }
}

/*
================
R_AliasDrawModel
================
*/
void R_AliasDrawModel(void)
{
    if (r_lerpmodels->integer == 0)
        currententity->backlerp = 0;

    if ((currententity->flags & (RF_WEAPONMODEL | RF_LEFTHAND)) == (RF_WEAPONMODEL | RF_LEFTHAND))
        r_refdef.xscale = -r_refdef.xscale;

    r_entscale = currententity->scale > 0 ? currententity->scale : 1;

    /*
    ** we have to set our frame pointers and transformations before
    ** doing any real work
    */
    R_AliasSetupFrames();
    R_AliasSetUpTransform();

    // see if the bounding box lets us trivially reject, also sets
    // trivial accept status
    if (R_AliasCheckBBox() == BBOX_TRIVIAL_REJECT)
        goto exit;

    r_amodels_drawn++;

    R_AliasSetupLighting();

    /*
    ** compute this_frame and old_frame addresses
    */
    R_AliasSetUpLerpData(currententity->backlerp);

    if (currententity->flags & RF_DEPTHHACK)
        s_ziscale = (float)0x8000 * (float)0x10000 * 3.0;
    else
        s_ziscale = (float)0x8000 * (float)0x10000;

    for (int i = 0; i < currentmodel->nummeshes; i++)
        R_AliasPrepareMesh(&currentmodel->meshes[i]);

exit:
    if ((currententity->flags & (RF_WEAPONMODEL | RF_LEFTHAND)) == (RF_WEAPONMODEL | RF_LEFTHAND))
        r_refdef.xscale = -r_refdef.xscale;
}

