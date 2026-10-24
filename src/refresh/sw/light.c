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
// r_light.c

#include "sw.h"

int r_dlightframecount;


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights_r
=============
*/
static void R_MarkLights_r(mnode_t *node, dlight_t *light,
                           const vec3_t transformed, int bit)
{
    float       dist;
    mface_t     *surf;
    int         i;

    while (node->plane) {
        dist = PlaneDiffFast(transformed, node->plane);
        if (dist > light->intensity - DLIGHT_CUTOFF) {
            node = node->children[0];
            continue;
        }
        if (dist < -light->intensity + DLIGHT_CUTOFF) {
            node = node->children[1];
            continue;
        }

        // mark the polygons
        surf = node->firstface;
        for (i = 0; i < node->numfaces; i++, surf++) {
            if (surf->dlightframe != r_dlightframecount) {
                surf->dlightbits = 0;
                surf->dlightframe = r_dlightframecount;
            }
            surf->dlightbits |= bit;
        }

        R_MarkLights_r(node->children[0], light, transformed, bit);
        node = node->children[1];
    }
}

/*
=============
R_MarkLights
=============
*/
void R_MarkLights(mnode_t *headnode)
{
    dlight_t    *light;
    vec3_t      transformed;
    int         i;

    r_dlightframecount = r_framecount;
    for (i = 0, light = r_newrefdef.dlights; i < r_newrefdef.num_dlights; i++, light++) {
        if (light->light_type != DLIGHT_SPHERE)
            continue;
        if (insubmodel) {
            vec3_t      temp;

            VectorSubtract(light->origin, currententity->origin, temp);
            transformed[0] = DotProduct(temp, entity_rotation[0]);
            transformed[1] = DotProduct(temp, entity_rotation[1]);
            transformed[2] = DotProduct(temp, entity_rotation[2]);
        } else {
            VectorCopy(light->origin, transformed);
        }
        R_MarkLights_r(headnode, light, transformed, 1U << i);
    }
}

/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

static bool _R_LightPoint(const vec3_t start, vec3_t color)
{
    mface_t         *surf;
    int             s, t, maps;
    byte            *lightmap;
    byte            *b1, *b2, *b3, *b4;
    int             fracu, fracv;
    int             w1, w2, w3, w4;
    byte            temp[3];
    int             smax, tmax, size;
    float           scale;
    vec3_t          end;
    lightpoint_t    lightpoint;

    end[0] = start[0];
    end[1] = start[1];
    end[2] = start[2] - 2048;

    BSP_LightPoint(&lightpoint, start, end, r_worldmodel->nodes);

    surf = lightpoint.surf;
    if (!surf)
        return false;

    // The face's own lightmap, classic or DECOUPLED_LM: BSP_LightPoint
    // gives s and t in sixteenths of a luxel either way.
    smax = S_MAX(surf);
    tmax = T_MAX(surf);
    size = smax * tmax * LIGHTMAP_BYTES;

    s = lightpoint.s >> 4;
    t = lightpoint.t >> 4;
    fracu = lightpoint.s & 15;
    fracv = lightpoint.t & 15;

    // the far edge has no neighbour to blend with
    if (s >= smax - 1) {
        s = max(smax - 2, 0);
        fracu = smax > 1 ? 16 : 0;
    }
    if (t >= tmax - 1) {
        t = max(tmax - 2, 0);
        fracv = tmax > 1 ? 16 : 0;
    }

    // compute weights of lightmap blocks
    w1 = (16 - fracu) * (16 - fracv);
    w2 = fracu * (16 - fracv);
    w3 = fracu * fracv;
    w4 = (16 - fracu) * fracv;

    // add all the lightmaps with bilinear filtering
    lightmap = surf->lightmap;
    for (maps = 0; maps < surf->numstyles; maps++) {
        b1 = &lightmap[LIGHTMAP_BYTES * ((t + 0) * smax + (s + 0))];
        b2 = &lightmap[LIGHTMAP_BYTES * ((t + 0) * smax + (s + 1))];
        b3 = &lightmap[LIGHTMAP_BYTES * ((t + 1) * smax + (s + 1))];
        b4 = &lightmap[LIGHTMAP_BYTES * ((t + 1) * smax + (s + 0))];

        temp[0] = (w1 * b1[0] + w2 * b2[0] + w3 * b3[0] + w4 * b4[0]) >> 8;
        temp[1] = (w1 * b1[1] + w2 * b2[1] + w3 * b3[1] + w4 * b4[1]) >> 8;
        temp[2] = (w1 * b1[2] + w2 * b2[2] + w3 * b3[2] + w4 * b4[2]) >> 8;

        scale = r_newrefdef.lightstyles[surf->styles[maps]].white * (1.0f / 255);
        color[0] += temp[0] * scale;
        color[1] += temp[1] * scale;
        color[2] += temp[2] * scale;

        lightmap += size;
    }

    return true;
}

/*
===============
R_LightPoint
===============
*/
void R_LightPoint_SW(const vec3_t point, vec3_t color)
{
    int         lnum;
    dlight_t    *dl;
    float       add;

    if (!r_worldmodel || !r_worldmodel->lightmap || r_fullbright->integer) {
        VectorSet(color, 1, 1, 1);
        return;
    }

    VectorClear(color);

    if (!_R_LightPoint(point, color))
        VectorSet(color, 1, 1, 1);

    //
    // add dynamic lights
    //
    for (lnum = 0; lnum < r_newrefdef.num_dlights; lnum++) {
        dl = &r_newrefdef.dlights[lnum];
        if (dl->light_type != DLIGHT_SPHERE)
            continue;
        add = dl->intensity - DLIGHT_CUTOFF - Distance(point, dl->origin);
        if (add > 0) {
            add *= (1.0f / 255);
            VectorMA(color, add, dl->color, color);
        }
    }

    VectorScale(color, sw_modulate->value, color);
}

//===================================================================

/*
===============
R_AddDynamicLights
===============
*/
static void R_AddDynamicLights(surfbuild_t *b)
{
    mface_t         *surf;
    dlight_t        *light;
    mtexinfo_t      *tex;
    vec3_t          transformed, impact;
    int             local[2];
    vec_t           dist, rad, minlight, scale, frac;
    blocklight_t    *block;
    int             i, smax, tmax, s, t, sd, td;
    int             step = r_8bit ? 1 : LIGHTMAP_BYTES;

    surf = b->ds.surf;
    smax = SW_S_MAX(surf);
    tmax = SW_T_MAX(surf);
    tex = surf->texinfo;

    for (i = 0; i < r_newrefdef.num_dlights; i++) {
        if (!(surf->dlightbits & (1U << i)))
            continue;

        light = &r_newrefdef.dlights[i];

        if (b->entity && b->entity != &r_worldentity) {
            vec3_t      temp;

            VectorSubtract(light->origin, b->entity->origin, temp);
            transformed[0] = DotProduct(temp, b->rotation[0]);
            transformed[1] = DotProduct(temp, b->rotation[1]);
            transformed[2] = DotProduct(temp, b->rotation[2]);
        } else {
            VectorCopy(light->origin, transformed);
        }

        dist = PlaneDiffFast(transformed, surf->plane);
        rad = light->intensity - fabs(dist);
        if (rad < DLIGHT_CUTOFF)
            continue;
        minlight = rad - DLIGHT_CUTOFF * 0.8f;
        scale = rad / minlight;

        VectorMA(transformed, -dist, surf->plane->normal, impact);

        local[0] = DotProduct(impact, tex->axis[0]) + tex->offset[0];
        local[1] = DotProduct(impact, tex->axis[1]) + tex->offset[1];

        local[0] -= surf->texturemins[0];
        local[1] -= surf->texturemins[1];

        block = b->blocklights;
        for (t = 0; t < tmax; t++) {
            td = abs(local[1] - (t << 4));
            for (s = 0; s < smax; s++) {
                sd = abs(local[0] - (s << 4));
                if (sd > td)
                    dist = sd + (td >> 1);
                else
                    dist = td + (sd >> 1);
                if (dist < minlight) {
                    frac = (rad - dist * scale) * sw_modulate->value * 256;
                    if (r_8bit) {
                        // white, as the original's were
                        block[0] += frac;
                    } else {
                        block[0] += light->color[0] * frac;
                        block[1] += light->color[1] * frac;
                        block[2] += light->color[2] * frac;
                    }
                }
                block += step;
            }
        }
    }
}

/*
===============
R_BuildLightMap8

8-bit mode: one channel, the brightest of the luxel's three as id's loader
kept it, bounded and inverted into colormap rows (light >> 8) with 1/256ths
between them for the block drawers to step through.
===============
*/
static void R_BuildLightMap8(surfbuild_t *b)
{
    int             i, maps, size, t;
    byte            *lightmap;
    mface_t         *surf;
    blocklight_t    *blocklights = b->blocklights;

    surf = b->ds.surf;
    size = SW_S_MAX(surf) * SW_T_MAX(surf);

    // fullbright is the colormap's brightest row, as it was; a face without
    // a lightmap shows its texture as it is, row 32, like 32-bit mode
    if (r_fullbright->integer || !surf->sw_lightmap) {
        t = r_fullbright->integer ? 0 : 32 << 8;
        for (i = 0; i < size; i++)
            blocklights[i] = t;
        return;
    }

// clear to no light
    memset(blocklights, 0, sizeof(blocklights[0]) * size);

// add all the lightmaps
    lightmap = surf->sw_lightmap;
    for (maps = 0; maps < surf->numstyles; maps++) {
        fixed8_t scale = b->ds.lightadj[maps];  // 8.8 fraction

        for (i = 0; i < size; i++, lightmap += LIGHTMAP_BYTES)
            blocklights[i] += max(lightmap[0], max(lightmap[1], lightmap[2])) * scale;
    }

// add all the dynamic lights
    if (surf->dlightframe == r_framecount)
        R_AddDynamicLights(b);

// bound, invert, and shift
    for (i = 0; i < size; i++) {
        t = (int)blocklights[i];
        if (t < 0)
            t = 0;
        t = (255 * 256 - t) >> (8 - VID_CBITS);

        if (t < (1 << 6))
            t = (1 << 6);

        blocklights[i] = t;
    }
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap(surfbuild_t *b)
{
    int             i, maps, smax, tmax, size;
    byte            *lightmap;
    mface_t         *surf;
    blocklight_t    *block, *blocklights = b->blocklights;

    surf = b->ds.surf;
    smax = SW_S_MAX(surf);
    tmax = SW_T_MAX(surf);
    size = smax * tmax;

    // D_CacheSurfaceBegin turned the face away already: this may be a worker,
    // which can't throw an error
    if (size > MAX_BLOCKLIGHTS)
        return;

    if (r_8bit) {
        R_BuildLightMap8(b);
        return;
    }

    if (r_fullbright->integer || !surf->sw_lightmap) {
        block = blocklights;
        for (i = 0; i < size; i++) {
            block[0] = block[1] = block[2] = 0x8000;   // the texture as it is
            block += LIGHTMAP_BYTES;
        }
        return;
    }

// clear to no light
    memset(blocklights, 0, sizeof(blocklights[0]) * size * LIGHTMAP_BYTES);

// add all the lightmaps
    lightmap = surf->sw_lightmap;
    for (maps = 0; maps < surf->numstyles; maps++) {
        fixed8_t        scale;

        block = blocklights;
        scale = b->ds.lightadj[maps];  // 8.8 fraction
        for (i = 0; i < size; i++) {
            block[0] += lightmap[0] * scale;
            block[1] += lightmap[1] * scale;
            block[2] += lightmap[2] * scale;

            lightmap += LIGHTMAP_BYTES;
            block += LIGHTMAP_BYTES;
        }
    }

// add all the dynamic lights
    if (surf->dlightframe == r_framecount)
        R_AddDynamicLights(b);

// bound
    block = blocklights;
    for (i = 0; i < size; i++) {
        blocklight_t    r, g, b, max;

        r = block[0];
        g = block[1];
        b = block[2];

        // catch negative lights
        if (r < 255)
            r = 255;
        if (g < 255)
            g = 255;
        if (b < 255)
            b = 255;

        // determine the brightest of the three color components
        max = g;
        if (r > max)
            max = r;
        if (b > max)
            max = b;

        // rescale all the color components if the intensity of the greatest
        // channel exceeds 1.0
        if (max > 65535) {
            float   y;

            y = 65535.0f / max;
            r *= y;
            g *= y;
            b *= y;
        }

        block[0] = r;
        block[1] = g;
        block[2] = b;

        block += LIGHTMAP_BYTES;
    }
}

