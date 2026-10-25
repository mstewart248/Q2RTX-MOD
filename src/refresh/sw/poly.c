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
#include "sw.h"

#define AFFINE_SPANLET_SIZE      16
#define AFFINE_SPANLET_SIZE_BITS 4

// a translucent polygon this big has its rows drawn on every thread
#define POLY_THREAD_PIXELS      16384

// a span being drawn: on the stack of whichever thread draws it
typedef struct spanletvars_s {
    byte        *pbase, *pdest;
    short       *pz;
    fixed16_t   s, t;
    fixed16_t   sstep, tstep;
    int         izi, izistep;
    int         spancount;
    unsigned    u, v;
} spanletvars_t;

static fixed8_t r_polyblendcolor[3];
static int      r_polyblendcolor8;

static espan_t  *s_polygon_spans;

static polydesc_t  r_polydesc;

mface_t *r_alpha_surfaces;

static int *r_turb_turb;

static int      clip_current;
static vec5_t   r_clip_verts[2][MAXWORKINGVERTS + 2];

static int      s_minindex, s_maxindex;

static void R_DrawPoly(int iswater);

/*
32-bit spanlets. The span's state comes in through sv and goes back out at
the end (R_PolygonDrawSpans carries pdest, pz and izi on into the next
spanlet); in between it is all locals, which every byte the loop stores
could otherwise alias.
*/
#define SPANLET_BEGIN \
    byte            *pdest = sv->pdest; \
    short           *pz = sv->pz; \
    const byte      *pbase = sv->pbase; \
    fixed16_t       s = sv->s, t = sv->t; \
    const fixed16_t sstep = sv->sstep, tstep = sv->tstep; \
    int             izi = sv->izi; \
    const int       izistep = sv->izistep; \
    int             count = sv->spancount

#define SPANLET_STEP() \
    izi += izistep; \
    pdest += VID_BYTES; \
    pz++; \
    s += sstep; \
    t += tstep

#define SPANLET_END() \
    sv->pdest = pdest; \
    sv->pz = pz; \
    sv->izi = izi; \
    sv->s = s; \
    sv->t = t; \
    sv->spancount = 0

static void R_DrawSpanletOpaque(spanletvars_t *sv)
{
    SPANLET_BEGIN;
    const int   width = cachewidth;
    const byte  *ptex;

    // alpha under half is a hole, as 255 is in 8-bit (an alpha tested face;
    // anything else drawn opaque has none)
    do {
        ptex = pbase + (s >> 16) * TEX_BYTES + (t >> 16) * width;
        if ((ptex[3] & 0x80) && *pz <= (izi >> 16)) {
            pdest[0] = ptex[2];
            pdest[1] = ptex[1];
            pdest[2] = ptex[0];
            *pz = izi >> 16;
        }
        SPANLET_STEP();
    } while (--count > 0);

    SPANLET_END();
}

static void R_DrawSpanletTurbulentBlended(spanletvars_t *sv)
{
    SPANLET_BEGIN;
    const int   *turb = r_turb_turb;
    const int   alpha = r_polydesc.alpha, one_minus_alpha = r_polydesc.one_minus_alpha;
    int         sturb, tturb;
    const byte  *ptex;

    do {
        if (*pz <= (izi >> 16)) {
            sturb = ((s + turb[(t >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;
            tturb = ((t + turb[(s >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;
            ptex = pbase + (sturb) * TEX_BYTES + (tturb) * TURB_SIZE * TEX_BYTES;
            pdest[0] = (pdest[0] * one_minus_alpha + ptex[2] * alpha) >> 8;
            pdest[1] = (pdest[1] * one_minus_alpha + ptex[1] * alpha) >> 8;
            pdest[2] = (pdest[2] * one_minus_alpha + ptex[0] * alpha) >> 8;
        }
        SPANLET_STEP();
    } while (--count > 0);

    SPANLET_END();
}

static void R_DrawSpanletConstantBlended(spanletvars_t *sv)
{
    SPANLET_BEGIN;
    const int   one_minus_alpha = r_polydesc.one_minus_alpha;
    const int   c0 = r_polyblendcolor[2], c1 = r_polyblendcolor[1], c2 = r_polyblendcolor[0];

    (void)pbase;
    do {
        if (*pz <= (izi >> 16)) {
            pdest[0] = (pdest[0] * one_minus_alpha + c0) >> 8;
            pdest[1] = (pdest[1] * one_minus_alpha + c1) >> 8;
            pdest[2] = (pdest[2] * one_minus_alpha + c2) >> 8;
        }
        SPANLET_STEP();
    } while (--count > 0);

    SPANLET_END();
}

static void R_DrawSpanletBlended(spanletvars_t *sv)
{
    SPANLET_BEGIN;
    const int   width = cachewidth;
    const int   alpha = r_polydesc.alpha, one_minus_alpha = r_polydesc.one_minus_alpha;
    const byte  *ptex;

    do {
        if (*pz <= (izi >> 16)) {
            ptex = pbase + (s >> 16) * TEX_BYTES + (t >> 16) * width;
            pdest[0] = (pdest[0] * one_minus_alpha + ptex[2] * alpha) >> 8;
            pdest[1] = (pdest[1] * one_minus_alpha + ptex[1] * alpha) >> 8;
            pdest[2] = (pdest[2] * one_minus_alpha + ptex[0] * alpha) >> 8;
        }
        SPANLET_STEP();
    } while (--count > 0);

    SPANLET_END();
}

static void R_DrawSpanletAlphaTestBlended(spanletvars_t *sv)
{
    SPANLET_BEGIN;
    const int   width = cachewidth;
    const int   polyalpha = r_polydesc.alpha;
    const byte  *ptex;

    do {
        ptex = pbase + (s >> 16) * TEX_BYTES + (t >> 16) * width;
        if (ptex[3] && *pz <= (izi >> 16)) {
            int alpha = (ptex[3] * polyalpha) >> 8;
            int one_minus_alpha = 255 - alpha;

            pdest[0] = (pdest[0] * one_minus_alpha + ptex[2] * alpha) >> 8;
            pdest[1] = (pdest[1] * one_minus_alpha + ptex[1] * alpha) >> 8;
            pdest[2] = (pdest[2] * one_minus_alpha + ptex[0] * alpha) >> 8;
        }
        SPANLET_STEP();
    } while (--count > 0);

    SPANLET_END();
}

#undef SPANLET_BEGIN
#undef SPANLET_STEP
#undef SPANLET_END

/*
8-bit spanlets, the original's: 255 is a hole, translucency is the
colormap's 66/33 table - alphamap[row * 256 + column] is 66% of the row
and 33% of the column.
*/
#define SPANLET8_STEP() \
    sv->izi += sv->izistep; \
    sv->pdest++; \
    sv->pz++; \
    sv->s += sv->sstep; \
    sv->t += sv->tstep

static void R_DrawSpanletOpaque8(spanletvars_t *sv)
{
    byte    btemp;

    do {
        btemp = sv->pbase[(sv->s >> 16) + (sv->t >> 16) * cachewidth];
        if (btemp != 255 && *sv->pz <= (sv->izi >> 16)) {
            *sv->pdest = btemp;
            *sv->pz = sv->izi >> 16;
        }
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

static void R_DrawSpanlet33_8(spanletvars_t *sv)
{
    byte    btemp;

    do {
        btemp = sv->pbase[(sv->s >> 16) + (sv->t >> 16) * cachewidth];
        if (btemp != 255 && *sv->pz <= (sv->izi >> 16))
            *sv->pdest = r_alphamap[btemp + *sv->pdest * 256];
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

static void R_DrawSpanlet66_8(spanletvars_t *sv)
{
    byte    btemp;

    do {
        btemp = sv->pbase[(sv->s >> 16) + (sv->t >> 16) * cachewidth];
        if (btemp != 255 && *sv->pz <= (sv->izi >> 16))
            *sv->pdest = r_alphamap[btemp * 256 + *sv->pdest];
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

static inline byte R_TurbTexel8(const spanletvars_t *sv)
{
    int sturb = ((sv->s + r_turb_turb[(sv->t >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;
    int tturb = ((sv->t + r_turb_turb[(sv->s >> 16) & (CYCLE - 1)]) >> 16) & TURB_MASK;

    return sv->pbase[sturb + tturb * TURB_SIZE];
}

static void R_DrawSpanletTurbulentBlended33_8(spanletvars_t *sv)
{
    byte    btemp;

    do {
        btemp = R_TurbTexel8(sv);
        if (*sv->pz <= (sv->izi >> 16))
            *sv->pdest = r_alphamap[btemp + *sv->pdest * 256];
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

static void R_DrawSpanletTurbulentBlended66_8(spanletvars_t *sv)
{
    byte    btemp;

    do {
        btemp = R_TurbTexel8(sv);
        if (*sv->pz <= (sv->izi >> 16))
            *sv->pdest = r_alphamap[btemp * 256 + *sv->pdest];
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

static void R_DrawSpanletConstant33_8(spanletvars_t *sv)
{
    do {
        if (*sv->pz <= (sv->izi >> 16))
            *sv->pdest = r_alphamap[r_polyblendcolor8 + *sv->pdest * 256];
        SPANLET8_STEP();
    } while (--sv->spancount > 0);
}

#undef SPANLET8_STEP

/*
** R_ClipPolyFace
**
** Clips the winding at clip_verts[clip_current] and changes clip_current
** Throws out the back side
*/
static int R_ClipPolyFace(int nump, clipplane_t *pclipplane)
{
    int     i, outcount;
    float   dists[MAXWORKINGVERTS + 3];
    float   frac, clipdist, *pclipnormal;
    float   *in, *instep, *outstep, *vert2;

    clipdist = pclipplane->dist;
    pclipnormal = pclipplane->normal;

// calc dists
    if (clip_current) {
        in = r_clip_verts[1][0];
        outstep = r_clip_verts[0][0];
        clip_current = 0;
    } else {
        in = r_clip_verts[0][0];
        outstep = r_clip_verts[1][0];
        clip_current = 1;
    }

    instep = in;
    for (i = 0; i < nump; i++, instep += sizeof(vec5_t) / sizeof(float)) {
        dists[i] = DotProduct(instep, pclipnormal) - clipdist;
    }

// handle wraparound case
    dists[nump] = dists[0];
    memcpy(instep, in, sizeof(vec5_t));


// clip the winding
    instep = in;
    outcount = 0;

    for (i = 0; i < nump; i++, instep += sizeof(vec5_t) / sizeof(float)) {
        if (dists[i] >= 0) {
            memcpy(outstep, instep, sizeof(vec5_t));
            outstep += sizeof(vec5_t) / sizeof(float);
            outcount++;
        }

        if (dists[i] == 0 || dists[i + 1] == 0)
            continue;

        if ((dists[i] > 0) == (dists[i + 1] > 0))
            continue;

        // split it into a new vertex
        frac = dists[i] / (dists[i] - dists[i + 1]);

        vert2 = instep + sizeof(vec5_t) / sizeof(float);

        outstep[0] = instep[0] + frac * (vert2[0] - instep[0]);
        outstep[1] = instep[1] + frac * (vert2[1] - instep[1]);
        outstep[2] = instep[2] + frac * (vert2[2] - instep[2]);
        outstep[3] = instep[3] + frac * (vert2[3] - instep[3]);
        outstep[4] = instep[4] + frac * (vert2[4] - instep[4]);

        outstep += sizeof(vec5_t) / sizeof(float);
        outcount++;
    }

    return outcount;
}

/*
** R_PolygonDrawSpans
*/
static void R_PolygonDrawSpans(espan_t *pspan, int numspans, int iswater)
{
    int         count;
    fixed16_t   snext, tnext;
    float       sdivz, tdivz, zi, z, du, dv, spancountminus1;
    float       sdivzspanletstepu, tdivzspanletstepu, zispanletstepu;
    spanletvars_t   spanlet = { 0 }, *sv = &spanlet;

    sv->pbase = cacheblock;

    sdivzspanletstepu = d_sdivzstepu * AFFINE_SPANLET_SIZE;
    tdivzspanletstepu = d_tdivzstepu * AFFINE_SPANLET_SIZE;
    zispanletstepu = d_zistepu * AFFINE_SPANLET_SIZE;

// we count on FP exceptions being turned off to avoid range problems
    sv->izistep = (int)(d_zistepu * 0x8000 * 0x10000);

    for (; numspans > 0; numspans--, pspan++) {
        sv->pdest   = d_spantable[pspan->v] + pspan->u * r_pixbytes;
        sv->pz      = d_zspantable[pspan->v] + pspan->u;
        sv->u       = pspan->u;
        sv->v       = pspan->v;

        count = pspan->count;

        if (count <= 0)
            continue;

        // calculate the initial s/z, t/z, 1/z, s, and t and clamp
        du = (float)pspan->u;
        dv = (float)pspan->v;

        sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
        tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;

        zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
        z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point
        // we count on FP exceptions being turned off to avoid range problems
        sv->izi = (int)(zi * 0x8000 * 0x10000);

        sv->s = (int)(sdivz * z) + sadjust;
        sv->t = (int)(tdivz * z) + tadjust;

        if (!iswater) {
            if (sv->s > bbextents)
                sv->s = bbextents;
            else if (sv->s < 0)
                sv->s = 0;

            if (sv->t > bbextentt)
                sv->t = bbextentt;
            else if (sv->t < 0)
                sv->t = 0;
        }

        do {
            // calculate s and t at the far end of the span
            if (count >= AFFINE_SPANLET_SIZE)
                sv->spancount = AFFINE_SPANLET_SIZE;
            else
                sv->spancount = count;

            count -= sv->spancount;

            if (count) {
                // calculate s/z, t/z, zi->fixed s and t at far end of span,
                // calculate s and t steps across span by shifting
                sdivz += sdivzspanletstepu;
                tdivz += tdivzspanletstepu;
                zi += zispanletstepu;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point

                snext = (int)(sdivz * z) + sadjust;
                tnext = (int)(tdivz * z) + tadjust;

                if (!iswater) {
                    if (snext > bbextents)
                        snext = bbextents;
                    else if (snext < AFFINE_SPANLET_SIZE)
                        snext = AFFINE_SPANLET_SIZE;    // prevent round-off error on <0 steps from
                                                        // from causing overstepping & running off the
                                                        // edge of the texture

                    if (tnext > bbextentt)
                        tnext = bbextentt;
                    else if (tnext < AFFINE_SPANLET_SIZE)
                        tnext = AFFINE_SPANLET_SIZE;    // guard against round-off error on <0 steps
                }

                sv->sstep = (snext - sv->s) >> AFFINE_SPANLET_SIZE_BITS;
                sv->tstep = (tnext - sv->t) >> AFFINE_SPANLET_SIZE_BITS;
            } else {
                // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
                // can't step off polygon), clamp, calculate s and t steps across
                // span by division, biasing steps low so we don't run off the
                // texture
                spancountminus1 = (float)(sv->spancount - 1);
                sdivz += d_sdivzstepu * spancountminus1;
                tdivz += d_tdivzstepu * spancountminus1;
                zi += d_zistepu * spancountminus1;
                z = (float)0x10000 / zi;    // prescale to 16.16 fixed-point
                snext = (int)(sdivz * z) + sadjust;
                tnext = (int)(tdivz * z) + tadjust;

                if (!iswater) {
                    if (snext > bbextents)
                        snext = bbextents;
                    else if (snext < AFFINE_SPANLET_SIZE)
                        snext = AFFINE_SPANLET_SIZE;    // prevent round-off error on <0 steps from
                                                        // from causing overstepping & running off the
                                                        // edge of the texture

                    if (tnext > bbextentt)
                        tnext = bbextentt;
                    else if (tnext < AFFINE_SPANLET_SIZE)
                        tnext = AFFINE_SPANLET_SIZE;    // guard against round-off error on <0 steps
                }

                if (sv->spancount > 1) {
                    sv->sstep = (snext - sv->s) / (sv->spancount - 1);
                    sv->tstep = (tnext - sv->t) / (sv->spancount - 1);
                }
            }

            if (iswater) {
                sv->s = sv->s & ((CYCLE << 16) - 1);
                sv->t = sv->t & ((CYCLE << 16) - 1);
            }

            r_polydesc.drawspanlet(sv);

            sv->s = snext;
            sv->t = tnext;

        } while (count > 0);
    }
}

// a big polygon's rows, a band of them a thread
typedef struct {
    espan_t     *spans;
    int         numspans, iswater, jobs;
} polyjob_t;

static void R_PolygonSpanJob(void *arg, int index, int thread)
{
    const polyjob_t *j = arg;
    int             s0 = j->numspans * index / j->jobs;
    int             s1 = j->numspans * (index + 1) / j->jobs;

    if (s1 > s0)
        R_PolygonDrawSpans(j->spans + s0, s1 - s0, j->iswater);
}

/*
**
** R_PolygonScanLeftEdge
**
** Goes through the polygon and scans the left edge, filling in
** screen coordinate data for the spans
*/
static void R_PolygonScanLeftEdge(void)
{
    int         i, v, itop, ibottom, lmaxindex;
    emitpoint_t *pvert, *pnext;
    espan_t     *pspan;
    float       du, dv, vtop, vbottom, slope;
    fixed16_t   u, u_step;

    pspan = s_polygon_spans;
    i = s_minindex;
    if (i == 0)
        i = r_polydesc.nump;

    lmaxindex = s_maxindex;
    if (lmaxindex == 0)
        lmaxindex = r_polydesc.nump;

    vtop = ceil(r_polydesc.pverts[i].v);

    do {
        pvert = &r_polydesc.pverts[i];
        pnext = pvert - 1;

        vbottom = ceil(pnext->v);

        if (vtop < vbottom) {
            du = pnext->u - pvert->u;
            dv = pnext->v - pvert->v;

            slope = du / dv;
            u_step = (int)(slope * 0x10000);
            // adjust u to ceil the integer portion
            u = (int)((pvert->u + (slope * (vtop - pvert->v))) * 0x10000) +
                (0x10000 - 1);
            itop = (int)vtop;
            ibottom = (int)vbottom;

            for (v = itop; v < ibottom; v++) {
                pspan->u = u >> 16;
                pspan->v = v;
                u += u_step;
                pspan++;
            }
        }

        vtop = vbottom;

        i--;
        if (i == 0)
            i = r_polydesc.nump;

    } while (i != lmaxindex);
}

/*
** R_PolygonScanRightEdge
**
** Goes through the polygon and scans the right edge, filling in
** count values.
*/
static int R_PolygonScanRightEdge(void)
{
    int         i, v, itop, ibottom;
    emitpoint_t *pvert, *pnext;
    espan_t     *pspan;
    float       du, dv, vtop, vbottom, slope, uvert, unext, vvert, vnext;
    fixed16_t   u, u_step;

    pspan = s_polygon_spans;
    i = s_minindex;

    vvert = r_polydesc.pverts[i].v;
    if (vvert < r_refdef.fvrecty_adj)
        vvert = r_refdef.fvrecty_adj;
    if (vvert > r_refdef.fvrectbottom_adj)
        vvert = r_refdef.fvrectbottom_adj;

    vtop = ceil(vvert);

    do {
        pvert = &r_polydesc.pverts[i];
        pnext = pvert + 1;

        vnext = pnext->v;
        if (vnext < r_refdef.fvrecty_adj)
            vnext = r_refdef.fvrecty_adj;
        if (vnext > r_refdef.fvrectbottom_adj)
            vnext = r_refdef.fvrectbottom_adj;

        vbottom = ceil(vnext);

        if (vtop < vbottom) {
            uvert = pvert->u;
            if (uvert < r_refdef.fvrectx_adj)
                uvert = r_refdef.fvrectx_adj;
            if (uvert > r_refdef.fvrectright_adj)
                uvert = r_refdef.fvrectright_adj;

            unext = pnext->u;
            if (unext < r_refdef.fvrectx_adj)
                unext = r_refdef.fvrectx_adj;
            if (unext > r_refdef.fvrectright_adj)
                unext = r_refdef.fvrectright_adj;

            du = unext - uvert;
            dv = vnext - vvert;
            slope = du / dv;
            u_step = (int)(slope * 0x10000);
            // adjust u to ceil the integer portion
            u = (int)((uvert + (slope * (vtop - vvert))) * 0x10000) +
                (0x10000 - 1);
            itop = (int)vtop;
            ibottom = (int)vbottom;

            for (v = itop; v < ibottom; v++) {
                pspan->count = (u >> 16) - pspan->u;
                u += u_step;
                pspan++;
            }
        }

        vtop = vbottom;
        vvert = vnext;

        i++;
        if (i == r_polydesc.nump)
            i = 0;

    } while (i != s_maxindex);

    pspan->count = DS_SPAN_LIST_END;    // mark the end of the span list

    return pspan - s_polygon_spans;
}

/*
** R_ClipAndDrawPoly
*/
static void R_ClipAndDrawPoly(float alpha, int isturbulent, int textured)
{
    emitpoint_t outverts[MAXWORKINGVERTS + 3], *pout;
    float       *pv;
    int         i, nump;
    float       scale;
    vec3_t      transformed, local;

    r_polydesc.alpha = 255 * alpha;
    r_polydesc.one_minus_alpha = 255 - r_polydesc.alpha;

    if (r_8bit) {
        // the original's choices; holes (255) are cut by every textured one
        if (textured == 0)
            r_polydesc.drawspanlet = R_DrawSpanletConstant33_8;
        else if (alpha == 1)
            r_polydesc.drawspanlet = R_DrawSpanletOpaque8;
        else if (isturbulent)
            r_polydesc.drawspanlet = alpha > 0.33f ? R_DrawSpanletTurbulentBlended66_8 : R_DrawSpanletTurbulentBlended33_8;
        else
            r_polydesc.drawspanlet = alpha > 0.33f ? R_DrawSpanlet66_8 : R_DrawSpanlet33_8;
    } else if (textured == 0) {
        r_polydesc.drawspanlet = R_DrawSpanletConstantBlended;
    } else if (textured == 2) {
        r_polydesc.drawspanlet = R_DrawSpanletAlphaTestBlended;
    } else if (alpha == 1) {
        // isturbulent is ignored because we know that turbulent surfaces
        // can't be opaque
        r_polydesc.drawspanlet = R_DrawSpanletOpaque;
    } else if (isturbulent) {
        r_polydesc.drawspanlet = R_DrawSpanletTurbulentBlended;
    } else {
        r_polydesc.drawspanlet = R_DrawSpanletBlended;
    }

    // clip to the frustum in worldspace
    nump = r_polydesc.nump;
    clip_current = 0;

    for (i = 0; i < 4; i++) {
        nump = R_ClipPolyFace(nump, &view_clipplanes[i]);
        if (nump < 3)
            return;
        if (nump > MAXWORKINGVERTS)
            Com_Error(ERR_DROP, "R_ClipAndDrawPoly: too many points: %d", nump);
    }

// transform vertices into viewspace and project
    pv = &r_clip_verts[clip_current][0][0];

    for (i = 0; i < nump; i++) {
        VectorSubtract(pv, r_origin, local);
        R_TransformVector(local, transformed);

        if (transformed[2] < NEAR_CLIP)
            transformed[2] = NEAR_CLIP;

        pout = &outverts[i];
        pout->zi = 1.0 / transformed[2];

        pout->s = pv[3];
        pout->t = pv[4];

        scale = r_refdef.xscale * pout->zi;
        pout->u = (r_refdef.xcenter + scale * transformed[0]);

        scale = r_refdef.yscale * pout->zi;
        pout->v = (r_refdef.ycenter - scale * transformed[1]);

        pv += sizeof(vec5_t) / sizeof(vec_t);
    }

// draw it
    r_polydesc.nump = nump;
    r_polydesc.pverts = outverts;

    R_DrawPoly(isturbulent);
}

/*
** R_BuildPolygonFromSurface
*/
static void R_BuildPolygonFromSurface(mface_t *fa)
{
    int         i, lnumverts;
    msurfedge_t *surfedge;
    float       *vec;
    vec5_t      *pverts;
    float       tmins[2] = { 0, 0 };

    r_polydesc.nump = 0;

    // reconstruct the polygon
    lnumverts = fa->numsurfedges;

    if (lnumverts > MAXWORKINGVERTS)
        Com_Error(ERR_DROP, "R_BuildPolygonFromSurface: too many points: %d", lnumverts);

    pverts = r_clip_verts[0];

    surfedge = fa->firstsurfedge;
    for (i = 0; i < lnumverts; i++, surfedge++) {
        vec = surfedge->edge->v[surfedge->vert]->point;
        VectorCopy(vec, pverts[i]);
    }

    VectorCopy(fa->texinfo->axis[0], r_polydesc.vright);
    VectorCopy(fa->texinfo->axis[1], r_polydesc.vup);
    VectorCopy(fa->plane->normal, r_polydesc.vpn);
    VectorCopy(r_origin, r_polydesc.viewer_position);

    if (fa->drawflags & DSURF_PLANEBACK) {
        VectorInverse(r_polydesc.vpn);
    }

    if (fa->texinfo->c.flags & (SURF_WARP | SURF_FLOWING)) {
        r_polydesc.pixels       = r_8bit ? fa->texinfo->image->sw_pixels8[0] : fa->texinfo->image->sw_pixels[0];
        r_polydesc.pixel_width  = fa->texinfo->image->upload_width;
        r_polydesc.pixel_height = fa->texinfo->image->upload_height;
    } else {
        surfcache_t *scache;

        scache = D_CacheSurface(fa, 0);

        r_polydesc.pixels       = scache->data;
        r_polydesc.pixel_width  = scache->width;
        r_polydesc.pixel_height = scache->height;

        tmins[0] = fa->texturemins[0];
        tmins[1] = fa->texturemins[1];
    }

    r_polydesc.dist = DotProduct(r_polydesc.vpn, pverts[0]);

    r_polydesc.s_offset = fa->texinfo->offset[0] - tmins[0];
    r_polydesc.t_offset = fa->texinfo->offset[1] - tmins[1];

    // scrolling texture addition
    if (fa->texinfo->c.flags & SURF_FLOWING) {
        r_polydesc.s_offset += -128 * ((r_newrefdef.time * 0.25) - (int)(r_newrefdef.time * 0.25));
    }

    r_polydesc.nump = lnumverts;
}

/*
** R_PolygonCalculateGradients
*/
static void R_PolygonCalculateGradients(void)
{
    vec3_t      p_normal, p_saxis, p_taxis;
    float       distinv;

    R_TransformVector(r_polydesc.vpn, p_normal);
    R_TransformVector(r_polydesc.vright, p_saxis);
    R_TransformVector(r_polydesc.vup, p_taxis);

    distinv = 1.0 / (-(DotProduct(r_polydesc.viewer_position, r_polydesc.vpn)) + r_polydesc.dist);

    d_sdivzstepu  =  p_saxis[0] * r_refdef.xscaleinv;
    d_sdivzstepv  = -p_saxis[1] * r_refdef.yscaleinv;
    d_sdivzorigin =  p_saxis[2] - r_refdef.xcenter * d_sdivzstepu - r_refdef.ycenter * d_sdivzstepv;

    d_tdivzstepu  =  p_taxis[0] * r_refdef.xscaleinv;
    d_tdivzstepv  = -p_taxis[1] * r_refdef.yscaleinv;
    d_tdivzorigin =  p_taxis[2] - r_refdef.xcenter * d_tdivzstepu - r_refdef.ycenter * d_tdivzstepv;

    d_zistepu =   p_normal[0] * r_refdef.xscaleinv * distinv;
    d_zistepv =  -p_normal[1] * r_refdef.yscaleinv * distinv;
    d_ziorigin =  p_normal[2] * distinv - r_refdef.xcenter * d_zistepu - r_refdef.ycenter * d_zistepv;

    sadjust = (fixed16_t)((DotProduct(r_polydesc.viewer_position, r_polydesc.vright) + r_polydesc.s_offset) * 0x10000);
    tadjust = (fixed16_t)((DotProduct(r_polydesc.viewer_position, r_polydesc.vup) + r_polydesc.t_offset) * 0x10000);

// -1 (-epsilon) so we never wander off the edge of the texture
    bbextents = (r_polydesc.pixel_width << 16) - 1;
    bbextentt = (r_polydesc.pixel_height << 16) - 1;
}

/*
** R_DrawPoly
**
** Polygon drawing function.  Uses the polygon described in r_polydesc
** to calculate edges and gradients, then renders the resultant spans.
**
** This should NOT be called externally since it doesn't do clipping!
*/
static void R_DrawPoly(int iswater)
{
    int         i, nump, numspans, pixels;
    float       ymin, ymax;
    emitpoint_t *pverts;
    espan_t spans[MAXHEIGHT + 1];

    s_polygon_spans = spans;

// find the top and bottom vertices, and make sure there's at least one scan to
// draw
    ymin = 999999.9;
    ymax = -999999.9;
    pverts = r_polydesc.pverts;

    for (i = 0; i < r_polydesc.nump; i++) {
        if (pverts->v < ymin) {
            ymin = pverts->v;
            s_minindex = i;
        }

        if (pverts->v > ymax) {
            ymax = pverts->v;
            s_maxindex = i;
        }

        pverts++;
    }

    ymin = ceil(ymin);
    ymax = ceil(ymax);

    if (ymin >= ymax)
        return;     // doesn't cross any scans at all

    cachewidth = r_polydesc.pixel_width * r_texbytes;
    cacheblock = r_polydesc.pixels;

// copy the first vertex to the last vertex, so we don't have to deal with
// wrapping
    nump = r_polydesc.nump;
    pverts = r_polydesc.pverts;
    pverts[nump] = pverts[0];

    if (iswater & SURF_WARP)
        r_turb_turb = sintable + ((int)(r_newrefdef.time * SPEED) & (CYCLE - 1));
    else if (iswater & SURF_FLOWING)
        r_turb_turb = blanktable;

    R_PolygonCalculateGradients();
    R_PolygonScanLeftEdge();
    numspans = R_PolygonScanRightEdge();

    // water across half the view is too much for one thread; a sprite or a
    // pane of glass isn't worth waking the others for
    for (i = 0, pixels = 0; i < numspans && pixels < POLY_THREAD_PIXELS; i++)
        pixels += max(spans[i].count, 0);

    if (r_numthreads > 1 && pixels >= POLY_THREAD_PIXELS && numspans >= 16) {
        polyjob_t   j;

        j.spans = spans;
        j.numspans = numspans;
        j.iswater = iswater;
        j.jobs = R_RowJobs(numspans);
        R_ParallelFor(j.jobs, R_PolygonSpanJob, &j);
    } else {
        R_PolygonDrawSpans(spans, numspans, iswater);
    }
}

/*
** R_DrawAlphaSurfaces
*/
void R_DrawAlphaSurfaces(void)
{
    mface_t *s;

    //currentmodel = r_worldmodel;

    // the faces are lit here, in world space: left as the last model drawn,
    // that model's origin and frame moved the dynamic lights and animation
    currententity = &r_worldentity;

    modelorg[0] = -r_origin[0];
    modelorg[1] = -r_origin[1];
    modelorg[2] = -r_origin[2];

    // alpha tested faces first: they write z, so translucent ones in front
    // still blend over them, and ones behind are still hidden
    for (s = r_alpha_surfaces; s; s = s->next) {
        if (!SW_ALPHATEST(s->texinfo->c.flags))
            continue;

        R_BuildPolygonFromSurface(s);
        R_ClipAndDrawPoly(1.0f, false, true);
    }

    for (s = r_alpha_surfaces; s; s = s->next) {
        if (SW_ALPHATEST(s->texinfo->c.flags))
            continue;

        R_BuildPolygonFromSurface(s);

        if (s->texinfo->c.flags & SURF_TRANS66)
            R_ClipAndDrawPoly(0.66f, (s->texinfo->c.flags & (SURF_WARP | SURF_FLOWING)), true);
        else
            R_ClipAndDrawPoly(0.33f, (s->texinfo->c.flags & (SURF_WARP | SURF_FLOWING)), true);
    }

    r_alpha_surfaces = NULL;
}

/*
** R_IMFlatShadedQuad
*/
void R_IMFlatShadedQuad(const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t d, color_t color, float alpha)
{
    vec3_t s0, s1;

    r_polydesc.nump = 4;
    VectorCopy(r_origin, r_polydesc.viewer_position);

    VectorCopy(a, r_clip_verts[0][0]);
    VectorCopy(b, r_clip_verts[0][1]);
    VectorCopy(c, r_clip_verts[0][2]);
    VectorCopy(d, r_clip_verts[0][3]);

    r_clip_verts[0][0][3] = 0;
    r_clip_verts[0][1][3] = 0;
    r_clip_verts[0][2][3] = 0;
    r_clip_verts[0][3][3] = 0;

    r_clip_verts[0][0][4] = 0;
    r_clip_verts[0][1][4] = 0;
    r_clip_verts[0][2][4] = 0;
    r_clip_verts[0][3][4] = 0;

    VectorSubtract(d, c, s0);
    VectorSubtract(c, b, s1);
    CrossProduct(s0, s1, r_polydesc.vpn);
    VectorNormalize(r_polydesc.vpn);

    r_polydesc.dist = DotProduct(r_polydesc.vpn, r_clip_verts[0][0]);

    r_polydesc.alpha = 255 * alpha;

    r_polyblendcolor[0] = color.u8[0] * r_polydesc.alpha;
    r_polyblendcolor[1] = color.u8[1] * r_polydesc.alpha;
    r_polyblendcolor[2] = color.u8[2] * r_polydesc.alpha;
    r_polyblendcolor8 = R_Quantize(color.u8[0], color.u8[1], color.u8[2]);

    R_ClipAndDrawPoly(alpha, false, false);
}

/*
** R_DrawSprite
**
** Draw currententity / currentmodel as a single texture
** mapped polygon
*/
void R_DrawSprite(void)
{
    vec5_t      *pverts;
    vec3_t      left, up, right, down;
    mspriteframe_t  *frame;
    int             textured;

    frame = &currentmodel->spriteframes[
                currententity->frame % currentmodel->numframes];

    r_polydesc.pixels       = r_8bit ? frame->image->sw_pixels8[0] : frame->image->sw_pixels[0];
    r_polydesc.pixel_width  = frame->image->upload_width;
    r_polydesc.pixel_height = frame->image->upload_height;
    r_polydesc.dist         = 0;

    if (!r_polydesc.pixels)
        return;

    // generate the sprite's axes, completely parallel to the viewplane.
    VectorCopy(vup, r_polydesc.vup);
    VectorCopy(vright, r_polydesc.vright);
    VectorCopy(vpn, r_polydesc.vpn);

// build the sprite poster in worldspace
    VectorScale(r_polydesc.vright,
                frame->width - frame->origin_x, right);
    VectorScale(r_polydesc.vup,
                frame->height - frame->origin_y, up);
    VectorScale(r_polydesc.vright,
                -frame->origin_x, left);
    VectorScale(r_polydesc.vup,
                -frame->origin_y, down);

    // invert UP vector for sprites
    VectorNegate(r_polydesc.vup, r_polydesc.vup);

    pverts = r_clip_verts[0];

    pverts[0][0] = r_entorigin[0] + up[0] + left[0];
    pverts[0][1] = r_entorigin[1] + up[1] + left[1];
    pverts[0][2] = r_entorigin[2] + up[2] + left[2];
    pverts[0][3] = 0;
    pverts[0][4] = 0;

    pverts[1][0] = r_entorigin[0] + up[0] + right[0];
    pverts[1][1] = r_entorigin[1] + up[1] + right[1];
    pverts[1][2] = r_entorigin[2] + up[2] + right[2];
    pverts[1][3] = frame->width;
    pverts[1][4] = 0;

    pverts[2][0] = r_entorigin[0] + down[0] + right[0];
    pverts[2][1] = r_entorigin[1] + down[1] + right[1];
    pverts[2][2] = r_entorigin[2] + down[2] + right[2];
    pverts[2][3] = frame->width;
    pverts[2][4] = frame->height;

    pverts[3][0] = r_entorigin[0] + down[0] + left[0];
    pverts[3][1] = r_entorigin[1] + down[1] + left[1];
    pverts[3][2] = r_entorigin[2] + down[2] + left[2];
    pverts[3][3] = 0;
    pverts[3][4] = frame->height;

    r_polydesc.nump = 4;
    r_polydesc.s_offset = (r_polydesc.pixel_width  >> 1);
    r_polydesc.t_offset = (r_polydesc.pixel_height >> 1);
    VectorCopy(modelorg, r_polydesc.viewer_position);

    if (frame->image->flags & IF_TRANSPARENT)
        textured = 2;
    else
        textured = 1;

    if (currententity->flags & RF_TRANSLUCENT)
        R_ClipAndDrawPoly(currententity->alpha, false, textured);
    else
        R_ClipAndDrawPoly(1.0F, false, textured);
}

