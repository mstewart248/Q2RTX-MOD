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
// r_edge.c

#include "sw.h"
#include "common/cpuprof.h"

/*
the complex cases add new polys on most lines, so dont optimize for keeping them the same
have multiple free span lists to try to get better coherence?
low depth complexity -- 1 to 3 or so

have a sentinal at both ends?
*/


edge_t  *r_edges, *edge_p, *edge_max;

surf_t  *surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack

edge_t  *newedges[MAXHEIGHT];
edge_t  *removeedges[MAXHEIGHT];

static espan_t  *span_p, *max_span_p;

// span pool, flushed through D_DrawSurfaces when less than a scan is left
#define MINSPANS    3000

static espan_t  *r_spanpool;
static int      r_numspans;

int     r_currentkey;

static int      current_iv;

static int      edge_head_u_shift20, edge_tail_u_shift20;

static void     (*pdrawfunc)(void);

static edge_t   edge_head;
static edge_t   edge_tail;
static edge_t   edge_aftertail;
static edge_t   edge_sentinel;

static float    fv;

static espan_t  *r_spanrows[MAXHEIGHT + 1];     // where each row's spans start
static bool     r_spangrow;     // a frame flushed its spans part way: grow them

static void R_GenerateSpans(void);
static void R_GenerateSpansBackward(void);


/*
===============================================================================

EDGE SCANNING

===============================================================================
*/

/*
==============
R_BeginEdgeFrame
==============
*/
void R_BeginEdgeFrame(void)
{
    int     v;

    edge_p = r_edges;
    edge_max = &r_edges[r_numallocatededges];

    surface_p = &surfaces[2];   // background is surface 1,
                                // surface 0 is a dummy
    surfaces[1].spans = NULL;   // no background spans yet
    surfaces[1].flags = DSURF_BACKGROUND;

// put the background behind everything in the world
    if (sw_draworder->integer) {
        pdrawfunc = R_GenerateSpansBackward;
        surfaces[1].key = 0;
        r_currentkey = 1;
    } else {
        pdrawfunc = R_GenerateSpans;
        surfaces[1].key = 0x7FFFFFFF;
        r_currentkey = 0;
    }

// FIXME: set with memset
    for (v = r_refdef.vrect.y; v < r_refdef.vrectbottom; v++) {
        newedges[v] = removeedges[v] = NULL;
    }
}


/*
==============
R_InsertNewEdges

Adds the edges in the linked list edgestoadd, adding them to the edges in the
linked list edgelist.  edgestoadd is assumed to be sorted on u, and non-empty
(this is actually newedges[v]).  edgelist is assumed to be sorted on u, with a
sentinel at the end (actually, this is the active edge table starting at
edge_head.next).
==============
*/
static void R_InsertNewEdges(edge_t *edgestoadd, edge_t *edgelist)
{
    edge_t  *next_edge;

    do {
        next_edge = edgestoadd->next;
edgesearch:
        if (edgelist->u >= edgestoadd->u)
            goto addedge;
        edgelist = edgelist->next;
        if (edgelist->u >= edgestoadd->u)
            goto addedge;
        edgelist = edgelist->next;
        if (edgelist->u >= edgestoadd->u)
            goto addedge;
        edgelist = edgelist->next;
        if (edgelist->u >= edgestoadd->u)
            goto addedge;
        edgelist = edgelist->next;
        goto edgesearch;

        // insert edgestoadd before edgelist
addedge:
        edgestoadd->next = edgelist;
        edgestoadd->prev = edgelist->prev;
        edgelist->prev->next = edgestoadd;
        edgelist->prev = edgestoadd;
    } while ((edgestoadd = next_edge) != NULL);
}

/*
==============
R_RemoveEdges
==============
*/
static void R_RemoveEdges(edge_t *pedge)
{

    do {
        pedge->next->prev = pedge->prev;
        pedge->prev->next = pedge->next;
    } while ((pedge = pedge->nextremove) != NULL);
}

/*
==============
R_StepActiveU
==============
*/
static void R_StepActiveU(edge_t *pedge)
{
    edge_t      *pnext_edge, *pwedge;

    while (1) {
nextedge:
        pedge->u += pedge->u_step;
        if (pedge->u < pedge->prev->u)
            goto pushback;
        pedge = pedge->next;

        pedge->u += pedge->u_step;
        if (pedge->u < pedge->prev->u)
            goto pushback;
        pedge = pedge->next;

        pedge->u += pedge->u_step;
        if (pedge->u < pedge->prev->u)
            goto pushback;
        pedge = pedge->next;

        pedge->u += pedge->u_step;
        if (pedge->u < pedge->prev->u)
            goto pushback;
        pedge = pedge->next;

        goto nextedge;

pushback:
        if (pedge == &edge_aftertail)
            return;

        // push it back to keep it sorted
        pnext_edge = pedge->next;

        // pull the edge out of the edge list
        pedge->next->prev = pedge->prev;
        pedge->prev->next = pedge->next;

        // find out where the edge goes in the edge list
        pwedge = pedge->prev->prev;

        while (pwedge->u > pedge->u) {
            pwedge = pwedge->prev;
        }

        // put the edge back into the edge list
        pedge->next = pwedge->next;
        pedge->prev = pwedge;
        pedge->next->prev = pedge;
        pwedge->next = pedge;

        pedge = pnext_edge;
        if (pedge == &edge_tail)
            return;
    }
}


/*
==============
R_CleanupSpan
==============
*/
static void R_CleanupSpan(void)
{
    surf_t  *surf;
    int     iu;
    espan_t *span;

// now that we've reached the right edge of the screen, we're done with any
// unfinished surfaces, so emit a span for whatever's on top
    surf = surfaces[1].next;
    iu = edge_tail_u_shift20;
    if (iu > surf->last_u) {
        span = span_p++;
        span->u = surf->last_u;
        span->count = iu - span->u;
        span->v = current_iv;
        span->psurf = surf;
        span->pnext = surf->spans;
        surf->spans = span;
    }

// reset spanstate for all surfaces in the surface stack
    do {
        surf->spanstate = 0;
        surf = surf->next;
    } while (surf != &surfaces[1]);
}


/*
==============
R_LeadingEdgeBackwards
==============
*/
static void R_LeadingEdgeBackwards(edge_t *edge)
{
    espan_t         *span;
    surf_t          *surf, *surf2;
    int             iu;

// it's adding a new surface in, so find the correct place
    surf = &surfaces[edge->surfs[1]];

// don't start a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we've already seen the
// end edge)
    if (++surf->spanstate == 1) {
        surf2 = surfaces[1].next;

        if (surf->key > surf2->key)
            goto newtop;

        // if it's two surfaces on the same plane, the one that's already
        // active is in front, so keep going unless it's a bmodel
        if (surf->insubmodel && (surf->key == surf2->key)) {
            // must be two bmodels in the same leaf; don't care, because they'll
            // never be farthest anyway
            goto newtop;
        }

continue_search:

        do {
            surf2 = surf2->next;
        } while (surf->key < surf2->key);

        if (surf->key == surf2->key) {
            // if it's two surfaces on the same plane, the one that's already
            // active is in front, so keep going unless it's a bmodel
            if (!surf->insubmodel)
                goto continue_search;

            // must be two bmodels in the same leaf; don't care which is really
            // in front, because they'll never be farthest anyway
        }

        goto gotposition;

newtop:
        // emit a span (obscures current top)
        iu = edge->u >> 20;

        if (iu > surf2->last_u) {
            span = span_p++;
            span->u = surf2->last_u;
            span->count = iu - span->u;
            span->v = current_iv;
            span->psurf = surf2;
            span->pnext = surf2->spans;
            surf2->spans = span;
        }

        // set last_u on the new span
        surf->last_u = iu;

gotposition:
        // insert before surf2
        surf->next = surf2;
        surf->prev = surf2->prev;
        surf2->prev->next = surf;
        surf2->prev = surf;
    }
}


/*
==============
R_TrailingEdge
==============
*/
static void R_TrailingEdge(surf_t *surf, edge_t *edge)
{
    espan_t         *span;
    int             iu;

// don't generate a span if this is an inverted span, with the end
// edge preceding the start edge (that is, we haven't seen the
// start edge yet)
    if (--surf->spanstate == 0) {
        if (surf == surfaces[1].next) {
            // emit a span (current top going away)
            iu = edge->u >> 20;
            if (iu > surf->last_u) {
                span = span_p++;
                span->u = surf->last_u;
                span->count = iu - span->u;
                span->v = current_iv;
                span->psurf = surf;
                span->pnext = surf->spans;
                surf->spans = span;
            }

            // set last_u on the surface below
            surf->next->last_u = iu;
        }

        surf->prev->next = surf->next;
        surf->next->prev = surf->prev;
    }
}


/*
==============
R_LeadingEdge
==============
*/
static void R_LeadingEdge(edge_t *edge)
{
    espan_t         *span;
    surf_t          *surf, *surf2;
    int             iu;
    float           fu, newzi, testzi, newzitop, newzibottom;

    if (edge->surfs[1]) {
        // it's adding a new surface in, so find the correct place
        surf = &surfaces[edge->surfs[1]];

        // don't start a span if this is an inverted span, with the end
        // edge preceding the start edge (that is, we've already seen the
        // end edge)
        if (++surf->spanstate == 1) {
            surf2 = surfaces[1].next;

            if (surf->key < surf2->key)
                goto newtop;

            // if it's two surfaces on the same plane, the one that's already
            // active is in front, so keep going unless it's a bmodel
            if (surf->insubmodel && (surf->key == surf2->key)) {
                // must be two bmodels in the same leaf; sort on 1/z
                fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
                newzi = surf->d_ziorigin + fv * surf->d_zistepv +
                        fu * surf->d_zistepu;
                newzibottom = newzi * 0.99;

                testzi = surf2->d_ziorigin + fv * surf2->d_zistepv +
                         fu * surf2->d_zistepu;

                if (newzibottom >= testzi) {
                    goto newtop;
                }

                newzitop = newzi * 1.01;
                if (newzitop >= testzi) {
                    if (surf->d_zistepu >= surf2->d_zistepu) {
                        goto newtop;
                    }
                }
            }

continue_search:

            do {
                surf2 = surf2->next;
            } while (surf->key > surf2->key);

            if (surf->key == surf2->key) {
                // if it's two surfaces on the same plane, the one that's already
                // active is in front, so keep going unless it's a bmodel
                if (!surf->insubmodel)
                    goto continue_search;

                // must be two bmodels in the same leaf; sort on 1/z
                fu = (float)(edge->u - 0xFFFFF) * (1.0 / 0x100000);
                newzi = surf->d_ziorigin + fv * surf->d_zistepv +
                        fu * surf->d_zistepu;
                newzibottom = newzi * 0.99;

                testzi = surf2->d_ziorigin + fv * surf2->d_zistepv +
                         fu * surf2->d_zistepu;

                if (newzibottom >= testzi) {
                    goto gotposition;
                }

                newzitop = newzi * 1.01;
                if (newzitop >= testzi) {
                    if (surf->d_zistepu >= surf2->d_zistepu) {
                        goto gotposition;
                    }
                }

                goto continue_search;
            }

            goto gotposition;

newtop:
            // emit a span (obscures current top)
            iu = edge->u >> 20;

            if (iu > surf2->last_u) {
                span = span_p++;
                span->u = surf2->last_u;
                span->count = iu - span->u;
                span->v = current_iv;
                span->psurf = surf2;
                span->pnext = surf2->spans;
                surf2->spans = span;
            }

            // set last_u on the new span
            surf->last_u = iu;

gotposition:
            // insert before surf2
            surf->next = surf2;
            surf->prev = surf2->prev;
            surf2->prev->next = surf;
            surf2->prev = surf;
        }
    }
}


/*
==============
R_GenerateSpans
==============
*/
static void R_GenerateSpans(void)
{
    edge_t          *edge;
    surf_t          *surf;

// clear active surfaces to just the background surface
    surfaces[1].next = surfaces[1].prev = &surfaces[1];
    surfaces[1].last_u = edge_head_u_shift20;

// generate spans
    for (edge = edge_head.next; edge != &edge_tail; edge = edge->next) {
        if (edge->surfs[0]) {
            // it has a left surface, so a surface is going away for this span
            surf = &surfaces[edge->surfs[0]];

            R_TrailingEdge(surf, edge);

            if (!edge->surfs[1])
                continue;
        }

        R_LeadingEdge(edge);
    }

    R_CleanupSpan();
}


/*
==============
R_GenerateSpansBackward
==============
*/
static void R_GenerateSpansBackward(void)
{
    edge_t          *edge;

// clear active surfaces to just the background surface
    surfaces[1].next = surfaces[1].prev = &surfaces[1];
    surfaces[1].last_u = edge_head_u_shift20;

// generate spans
    for (edge = edge_head.next; edge != &edge_tail; edge = edge->next) {
        if (edge->surfs[0])
            R_TrailingEdge(&surfaces[edge->surfs[0]], edge);

        if (edge->surfs[1])
            R_LeadingEdgeBackwards(edge);
    }

    R_CleanupSpan();
}


/*
==============
R_ScanEdges

Input:
newedges[] array
    this has links to edges, which have links to surfaces

Output:
Each surface has a linked list of its visible spans
==============
*/
void R_ScanEdges(void)
{
    int     iv, bottom, need, v0;
    espan_t *basespan_p;
    surf_t  *s;

    // a scan makes at most one span per pixel; keep room for several, and
    // for a whole frame once one has needed more: each flush is a pass of
    // the workers that waits on its slowest band
    need = max(MINSPANS, r_refdef.vrect.width * 8);
    if (r_spangrow && r_numspans < r_refdef.vrect.width * r_refdef.vrect.height / 4)
        need = max(need, r_numspans * 2);
    r_spangrow = false;
    if (need > r_numspans) {
        Z_Free(r_spanpool);
        r_spanpool = R_Malloc(need * sizeof(espan_t));
        r_numspans = need;
    }

    basespan_p = r_spanpool;
    max_span_p = &basespan_p[r_numspans - r_refdef.vrect.width];

    span_p = basespan_p;

// clear active edges to just the background edges around the whole screen
// FIXME: most of this only needs to be set up once
    edge_head.u = (fixed20_t)r_refdef.vrect.x << 20;
    edge_head_u_shift20 = edge_head.u >> 20;
    edge_head.u_step = 0;
    edge_head.prev = NULL;
    edge_head.next = &edge_tail;
    edge_head.surfs[0] = 0;
    edge_head.surfs[1] = 1;

    edge_tail.u = ((fixed20_t)r_refdef.vrectright << 20) + 0xFFFFF;
    edge_tail_u_shift20 = edge_tail.u >> 20;
    edge_tail.u_step = 0;
    edge_tail.prev = &edge_head;
    edge_tail.next = &edge_aftertail;
    edge_tail.surfs[0] = 1;
    edge_tail.surfs[1] = 0;

    edge_aftertail.u = -1;      // force a move
    edge_aftertail.u_step = 0;
    edge_aftertail.next = &edge_sentinel;
    edge_aftertail.prev = &edge_tail;

// FIXME: do we need this now that we clamp x in r_draw.c?
    edge_sentinel.u = (fixed20_t)(MAXWIDTH * 2) << 20;   // make sure nothing sorts past this
    edge_sentinel.prev = &edge_aftertail;

//
// process all scan lines
//
    bottom = r_refdef.vrectbottom - 1;
    v0 = r_refdef.vrect.y;

    for (iv = r_refdef.vrect.y; iv < bottom; iv++) {
        current_iv = iv;
        fv = (float)iv;
        r_spanrows[iv] = span_p;

        // mark that the head (background start) span is pre-included
        surfaces[1].spanstate = 1;

        if (newedges[iv]) {
            R_InsertNewEdges(newedges[iv], edge_head.next);
        }

        (*pdrawfunc)();

        // flush the span list if we can't be sure we have enough spans left for
        // the next scan
        if (span_p > max_span_p) {
            D_DrawSurfaces(v0, iv + 1);

            // clear the surface span pointers
            for (s = &surfaces[1]; s < surface_p; s++)
                s->spans = NULL;

            span_p = basespan_p;
            v0 = iv + 1;
            r_spangrow = true;
        }

        if (removeedges[iv])
            R_RemoveEdges(removeedges[iv]);

        if (edge_head.next != &edge_tail)
            R_StepActiveU(edge_head.next);
    }

// do the last scan (no need to step or sort or remove on the last scan)

    current_iv = iv;
    fv = (float)iv;
    r_spanrows[iv] = span_p;

// mark that the head (background start) span is pre-included
    surfaces[1].spanstate = 1;

    if (newedges[iv])
        R_InsertNewEdges(newedges[iv], edge_head.next);

    (*pdrawfunc)();

// draw whatever's left in the span list
    D_DrawSurfaces(v0, iv + 1);
}


/*
=========================================================================

SURFACE FILLING

=========================================================================
*/

// A batch of spans (D_DrawSurfaces) is drawn in three steps. On the main
// thread, every surface with spans is given what drawing them takes: its
// gradients and either a surface cache block (allocated, maybe not drawn
// yet) or a colour (D_PrepareSurface). Then the blocks that need drawing are
// drawn, a surface a job. Then the spans are, a band of rows a job, from the
// span pool in the order the edge scan made them: no pixel is in two spans,
// so the bands never touch. With one thread each surface is set up and drawn
// in turn, as it always was.

typedef enum {
    SD_SOLID,       // from its surface cache block
    SD_TURB,        // warping water, slime and lava
    SD_SKY,
    SD_FLAT         // one colour: background, drawflat, a face too big to cache
} surfkind_t;

typedef struct {
    surfkind_t  kind;
    int         color;          // SD_FLAT: palette index
    int         *warptable;     // SD_TURB
    spangrad_t  grad;
    float       zistepu, zistepv, ziorigin;     // what goes in the z buffer
} surfdraw_t;

typedef struct {
    int         v0, rows, jobs;
} bandjob_t;

static vec3_t       transformed_modelorg;
static vec3_t       world_transformed_modelorg;

drawstats_t         r_drawstats;

// r_speeds' clock, or 0 when it is off
static inline uint64_t D_Clock(void)
{
    return r_stagetime[0] ? Sys_Microseconds() : 0;
}

static surfdraw_t   *r_surfdraw;            // a surface's, while a batch is drawn
static surfbuild_t  *r_surfbuilds;          // the blocks a batch has to draw
static int          r_numsurfdraw;

/*
=============
D_MipLevelForScale
=============
*/
static int D_MipLevelForScale(float scale)
{
    int     lmiplevel;

    if (scale >= d_scalemip[0])
        lmiplevel = 0;
    else if (scale >= d_scalemip[1])
        lmiplevel = 1;
    else if (scale >= d_scalemip[2])
        lmiplevel = 2;
    else
        lmiplevel = 3;

    if (lmiplevel < d_minmip)
        lmiplevel = d_minmip;

    return lmiplevel;
}


/*
==============
D_FlatFillSpans

Simple single color fill with no texture mapping
==============
*/
static void D_FlatFillSpans(espan_t *span, int index)
{
    uint32_t    *pdest, color;
    int         i;

    index &= 255;
    color = d_8to24table[index];
    color = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);

    for (; span; span = span->pnext) {
        if (r_8bit) {
            memset(d_spantable[span->v] + span->u, index, span->count);
            continue;
        }
        pdest = (uint32_t *)(d_spantable[span->v] + span->u * VID_BYTES);
        for (i = 0; i < span->count; i++)
            pdest[i] = color;
    }
}


/*
==============
D_CalcGradients
==============
*/
static void D_CalcGradients(mface_t *pface, int miplevel, spangrad_t *g)
{
    float       mipscale;
    vec3_t      p_temp1;
    vec3_t      p_saxis, p_taxis;
    float       t;

    mipscale = 1.0 / (float)(1 << miplevel);

    R_TransformVector(pface->texinfo->axis[0], p_saxis);
    R_TransformVector(pface->texinfo->axis[1], p_taxis);

    t = r_refdef.xscaleinv * mipscale;
    g->sdivzstepu = p_saxis[0] * t;
    g->tdivzstepu = p_taxis[0] * t;

    t = r_refdef.yscaleinv * mipscale;
    g->sdivzstepv = -p_saxis[1] * t;
    g->tdivzstepv = -p_taxis[1] * t;

    g->sdivzorigin = p_saxis[2] * mipscale -
                     r_refdef.xcenter * g->sdivzstepu -
                     r_refdef.ycenter * g->sdivzstepv;
    g->tdivzorigin = p_taxis[2] * mipscale -
                     r_refdef.xcenter * g->tdivzstepu -
                     r_refdef.ycenter * g->tdivzstepv;

    VectorScale(transformed_modelorg, mipscale, p_temp1);

    t = 0x10000 * mipscale;
    g->sadjust = ((fixed16_t)(DotProduct(p_temp1, p_saxis) * 0x10000 + 0.5)) -
                 ((pface->texturemins[0] << 16) >> miplevel)
                 + pface->texinfo->offset[0] * t;
    g->tadjust = ((fixed16_t)(DotProduct(p_temp1, p_taxis) * 0x10000 + 0.5)) -
                 ((pface->texturemins[1] << 16) >> miplevel)
                 + pface->texinfo->offset[1] * t;

    if (pface->texinfo->c.flags & SURF_FLOWING) {
        if (pface->texinfo->c.flags & SURF_WARP)
            g->sadjust += 0x10000 * (-128 * ((r_newrefdef.time * 0.25) - (int)(r_newrefdef.time * 0.25)));
        else
            g->sadjust += 0x10000 * (-128 * ((r_newrefdef.time * 0.77) - (int)(r_newrefdef.time * 0.77)));
    }

//
// -1 (-epsilon) so we never wander off the edge of the texture
//
    g->bbextents = ((pface->extents[0] << 16) >> miplevel) - 1;
    g->bbextentt = ((pface->extents[1] << 16) >> miplevel) - 1;
}


/*
==============
D_PrepareSurface

Everything drawing the surface's spans takes, worked out on the main thread
(a bmodel's turns the view vectors while it is). A solid face's block is
found or allocated, and b->cache is set when its pixels still have to be
drawn (D_BuildSurface).
==============
*/
static void D_PrepareSurface(surf_t *s, surfdraw_t *sd, surfbuild_t *b)
{
    mface_t     *pface = s->msurf;
    surfcache_t *cache;
    image_t     *image;
    int         miplevel;

    b->cache = NULL;

    sd->grad.zistepu = sd->zistepu = s->d_zistepu;
    sd->grad.zistepv = sd->zistepv = s->d_zistepv;
    sd->grad.ziorigin = sd->ziorigin = s->d_ziorigin;

    // to allow developers to see the polygon carving of the world: a stable
    // color for each surface from the low bits of the msurface pointer
    if (sw_drawflat->integer) {
        sd->kind = SD_FLAT;
        sd->color = (int)((intptr_t)s->msurf >> 4);
        return;
    }

    // the grey background filler seen when there is a hole in the map, and
    // the sky, both in the z buffer as at infinity
    if (s->flags & (DSURF_BACKGROUND | DSURF_SKY)) {
        sd->zistepu = 0;
        sd->zistepv = 0;
        sd->ziorigin = -0.9f;

        if (s->flags & DSURF_SKY)
            image = pface->texinfo->image;
        else
            image = NULL;

        if (!image) {
            sd->kind = SD_FLAT;
            sd->color = (s->flags & DSURF_SKY) ? 0 : sw_clearcolor->integer & 0xFF;
            return;
        }

        sd->kind = SD_SKY;
        sd->grad.cacheblock = r_8bit ? image->sw_pixels8[0] : image->sw_pixels[0];
        sd->grad.cachewidth = 256 * r_texbytes;
        D_CalcGradients(pface, 0, &sd->grad);
        return;
    }

    if (s->insubmodel) {
        vec3_t       local_modelorg;

        // FIXME: we don't want to do all this for every polygon!
        // TODO: store once at start of frame
        currententity = s->entity;  // FIXME: make this passed in to
                                    // R_RotateBmodel ()
        VectorSubtract(r_origin, currententity->origin, local_modelorg);
        R_TransformVector(local_modelorg, transformed_modelorg);

        R_RotateBmodel();   // FIXME: don't mess with the frustum,
                            // make entity passed in
    } else {
        currententity = &r_worldentity;
    }

    if (s->flags & DSURF_TURB) {
        image = pface->texinfo->image;

        sd->kind = SD_TURB;
        sd->grad.cacheblock = r_8bit ? image->sw_pixels8[0] : image->sw_pixels[0];
        sd->grad.cachewidth = TURB_SIZE * r_texbytes;
        // textures that aren't warping are just flowing. Use blanktable instead.
        sd->warptable = (pface->texinfo->c.flags & SURF_WARP) ? sintable : blanktable;
        D_CalcGradients(pface, 0, &sd->grad);
    } else {
        miplevel = D_MipLevelForScale(s->nearzi * r_refdef.scale_for_mip * pface->texinfo->mipadjust);

        // the cache holds at most 256 texels a side: build big faces coarser,
        // and fill the few that are too big even at the last mip
        while (miplevel < MIPLEVELS - 1 &&
               ((pface->extents[0] >> miplevel) > 256 || (pface->extents[1] >> miplevel) > 256))
            miplevel++;

        if ((pface->extents[0] >> miplevel) > 256 || (pface->extents[1] >> miplevel) > 256) {
            sd->kind = SD_FLAT;
            sd->color = R_Quantize(127, 127, 127);
        } else {
            cache = D_CacheSurfaceBegin(pface, miplevel, b);

            sd->kind = SD_SOLID;
            sd->grad.cacheblock = (pixel_t *)cache->data;
            sd->grad.cachewidth = cache->width * r_texbytes;
            D_CalcGradients(pface, miplevel, &sd->grad);
        }
    }

    if (s->insubmodel) {
        //
        // restore the old drawing state
        // FIXME: we don't want to do this every time!
        // TODO: speed up
        //
        VectorCopy(world_transformed_modelorg, transformed_modelorg);
        VectorCopy(base_vpn, vpn);
        VectorCopy(base_vup, vup);
        VectorCopy(base_vright, vright);
        R_TransformFrustum();
    }
    currententity = NULL;   //&r_worldentity;
}

/*
==============
D_DrawSurfaceSpans

Any thread.
==============
*/
static void D_DrawSurfaceSpans(const surfdraw_t *sd, espan_t *spans)
{
    switch (sd->kind) {
    case SD_SOLID:
    case SD_SKY:
        if (r_8bit)
            D_DrawSpans16_8(spans, &sd->grad);
        else
            D_DrawSpans16(spans, &sd->grad);
        break;
    case SD_TURB:
        if (r_8bit)
            D_DrawTurbulent16_8(spans, &sd->grad, sd->warptable);
        else
            D_DrawTurbulent16(spans, &sd->grad, sd->warptable);
        break;
    case SD_FLAT:
        D_FlatFillSpans(spans, sd->color);
        break;
    }

    D_DrawZSpans(spans, sd->zistepu, sd->zistepv, sd->ziorigin);
}

static void D_BuildJob(void *arg, int index, int thread)
{
    surfbuild_t *b = &r_surfbuilds[index];

    b->blocklights = R_ThreadBlocklights(thread);
    D_BuildSurface(b);
}

static void D_DrawBandJob(void *arg, int index, int thread)
{
    const bandjob_t *j = arg;
    int             r0 = j->v0 + j->rows * index / j->jobs;
    int             r1 = j->v0 + j->rows * (index + 1) / j->jobs;
    espan_t         *span, *end, one;

    end = r_spanrows[r1];
    for (span = r_spanrows[r0]; span < end; span++) {
        one = *span;
        one.pnext = NULL;
        D_DrawSurfaceSpans(&r_surfdraw[span->psurf - surfaces], &one);
    }
}

/*
==============
D_DrawSurfacesThreaded

Rows v0 to v1 of spans on every thread. False when the surface cache can't
hold every block they need at once: they are left to the one thread way,
which draws each block as it goes, and the cache grows for the next frame
(R_CheckCaches).
==============
*/
static bool D_DrawSurfacesThreaded(int v0, int v1)
{
    static int  batchid;
    surf_t      *s;
    int         numsurfs = surface_p - surfaces;
    int         numbuilds = 0;
    int         polycount = r_drawnpolycount;
    bandjob_t   bands;
    uint64_t    t0, t1, t2;

    if (numsurfs > r_numsurfdraw) {
        Z_Free(r_surfdraw);
        Z_Free(r_surfbuilds);
        r_numsurfdraw = max(numsurfs, r_cnumsurfs + 1);
        r_surfdraw = R_Malloc(r_numsurfdraw * sizeof(r_surfdraw[0]));
        r_surfbuilds = R_Malloc(r_numsurfdraw * sizeof(r_surfbuilds[0]));
    }

    if (++batchid <= 0)
        batchid = 1;
    r_cachebatch = batchid;
    r_cacheoverflow = false;

    t0 = D_Clock();

    for (s = &surfaces[1]; s < surface_p; s++) {
        if (!s->spans)
            continue;

        r_drawnpolycount++;

        D_PrepareSurface(s, &r_surfdraw[s - surfaces], &r_surfbuilds[numbuilds]);
        if (r_cacheoverflow)
            break;
        if (r_surfbuilds[numbuilds].cache)
            numbuilds++;
    }

    r_cachebatch = 0;

    if (r_cacheoverflow) {
        r_cachegrow = true;
        r_cacheoverflow = false;
        r_drawnpolycount = polycount;   // the one thread way counts them again
        return false;
    }

    t1 = D_Clock();
    R_ParallelFor(numbuilds, D_BuildJob, NULL);
    t2 = D_Clock();

    r_spanrows[v1] = span_p;
    bands.v0 = v0;
    bands.rows = v1 - v0;
    bands.jobs = R_RowJobs(bands.rows);
    R_ParallelFor(bands.jobs, D_DrawBandJob, &bands);

    r_drawstats.prep += t1 - t0;
    r_drawstats.build += t2 - t1;
    r_drawstats.spans += D_Clock() - t2;

    return true;
}

/*
==============
D_DrawSurfaces

Rasterize all the span lists, rows v0 to v1.  Guaranteed zero overdraw.
May be called more than once a frame if the surf list overflows (higher res)
==============
*/
void D_DrawSurfaces(int v0, int v1)
{
    surf_t          *s;
    surfdraw_t      sd;
    surfbuild_t     b;
    uint64_t        t0 = D_Clock();

//  currententity = NULL;   //&r_worldentity;
    VectorSubtract(r_origin, vec3_origin, modelorg);
    R_TransformVector(modelorg, transformed_modelorg);
    VectorCopy(transformed_modelorg, world_transformed_modelorg);

    r_drawstats.batches++;

    if (r_numthreads > 1 && D_DrawSurfacesThreaded(v0, v1))
        goto done;

    r_drawstats.serial++;

    for (s = &surfaces[1]; s < surface_p; s++) {
        if (!s->spans)
            continue;

        r_drawnpolycount++;

        D_PrepareSurface(s, &sd, &b);
        if (b.cache) {
            b.blocklights = R_ThreadBlocklights(0);
            D_BuildSurface(&b);
        }
        D_DrawSurfaceSpans(&sd, s->spans);
    }

done:
    currententity = NULL;   //&r_worldentity;
    VectorSubtract(r_origin, vec3_origin, modelorg);
    R_TransformFrustum();

    r_drawstats.total += D_Clock() - t0;
}


void D_FreeSpans(void)
{
    Z_Freep((void **)&r_spanpool);
    r_numspans = 0;
    Z_Freep((void **)&r_surfdraw);
    Z_Freep((void **)&r_surfbuilds);
    r_numsurfdraw = 0;
}
