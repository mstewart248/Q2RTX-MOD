/*
Copyright (C) 1999-2005 Id Software, Inc.
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
// bsp_q3.c -- a Quake III Arena map (IBSP 46) loaded as a Quake II bsp_t
//
// Everything downstream - the server's collision, the client's prediction,
// both path tracers - reads a bsp_t, so a Quake III map is TRANSCODED into one
// here rather than taught to every consumer. The two formats are close where
// it matters: the same node/leaf tree with the same child encoding, brushes
// made of outward-facing planes, clusters with a PVS. The differences, and
// what is done about each:
//
//  * Drawing. Quake III surfaces are triangle lists with a UV per vertex, and
//    curved "patches" given as bezier control grids. Every triangle becomes
//    one mface_t with its own plane and its own UV mapping (mface_t.uv_axis -
//    an exact affine map through its three corners), patches are tessellated
//    first. The world's triangles are cut at leaf boundaries, as a Quake II
//    compiler cuts faces, so each lies in one cluster (clip_world_to_leaves).
//    Vertices are welded and edges shared, so faces still connect for
//    material instances (bsp_mesh.c build_face_instance_hashes). Per-corner
//    normals and tangents go in basisvectors, so curves shade smooth.
//
//  * Contents. Quake III leaves have none; detail brushes do not split the
//    tree. Collision tests a leaf's brushes (mleaf_t.pointtest, cmodel.c).
//    The flags are renumbered - most Quake III bits above 0x20 mean something
//    else in Quake II.
//
//  * Curves collide in Quake III. Each tessellated facet of a solid patch
//    becomes a thin brush (a slab behind the facet, bevelled for box traces)
//    listed in every leaf it overlaps.
//
//  * Inline models have no nodes of their own: each gets one leaf listing its
//    brushes, used as its headnode, which Quake II allows.
//
//  * Vis is stored uncompressed and with no PHS; it is compressed and a PHS
//    built (the union of the PVS of everything visible).
//
//  * Areas: flattened to one. Quake III doors close area portals with no
//    "style" for the Quake II game to drive, so they would only ever be
//    shut. Leaf 0 is a solid leaf of our own, as Quake II code expects.
//
// Lightmaps and the light grid are not used: the path tracer lights everything
// itself. Fog volumes become boxes of real fog (build_fog_volumes, vkpt fog.c).
//

#include "shared/shared.h"
#include "common/bsp.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/math.h"
#include "common/utils.h"
#include "common/zone.h"
#include "common/q3shader.h"
#include "format/bsp3.h"
#include "system/hunk.h"

#include <float.h>

extern mtexinfo_t nulltexinfo;

static cvar_t *map_q3_curve_error;
static cvar_t *map_q3_curve_clip_error;

#define Q3_CURVE_MAX_STEPS      24      // per bezier span, per direction
#define Q3_CURVE_MAX_CONTROL    129     // control points per patch side
#define Q3_FACET_THICKNESS      2.0f    // depth of a curve facet's collision slab
#define Q3_MAX_FACET_PLANES     32

/*
===============================================================================

SCRATCH

===============================================================================
*/

typedef struct {
    void    *data;
    int     count;
    int     alloc;
    size_t  elem;
} q3array_t;

static void *array_push(q3array_t *a)
{
    if (a->count == a->alloc) {
        a->alloc = a->alloc ? a->alloc * 2 : 1024;
        a->data = Z_Realloc(a->data, a->alloc * a->elem);
    }
    return (byte *)a->data + (size_t)(a->count++) * a->elem;
}

static void array_free(q3array_t *a)
{
    Z_Free(a->data);
    memset(a, 0, sizeof(*a));
}

#define ARRAY(type) { NULL, 0, 0, sizeof(type) }
#define ARRAY_AT(a, type, i) (((type *)(a).data)[i])

typedef struct {
    vec3_t  xyz;
    vec2_t  st;
    vec3_t  normal;
    float   alpha;      // the drawvert's colour alpha, 0..1: alphaGen vertex
} q3vtx_t;

typedef struct {
    q3vtx_t v[3];
    int     shader;
} q3tri_t;

// one collision brush made from a facet of a curve
typedef struct {
    int     firstplane, numplanes;  // into clipplanes
    int     shader;
    vec3_t  mins, maxs;
    vec3_t  pts[6];                 // its corners; the brush is exactly their hull
} q3clipbrush_t;

typedef struct {
    int     firsttri, numtris;      // render triangles
    int     firstclip, numclip;     // collision brushes
} q3surfrange_t;

typedef struct {
    // the file
    const byte              *buf;
    size_t                  filelen;
    const q3dheader_t       *header;

    const q3dshader_t       *shaders;       int numshaders;
    const q3dplane_t        *planes;        int numplanes;
    const q3dnode_t         *nodes;         int numnodes;
    const q3dleaf_t         *leafs;         int numleafs;
    const int32_t           *leafsurfaces;  int numleafsurfaces;
    const int32_t           *leafbrushes;   int numleafbrushes;
    const q3dmodel_t        *models;        int nummodels;
    const q3dbrush_t        *brushes;       int numbrushes;
    const q3dbrushside_t    *brushsides;    int numbrushsides;
    const q3drawvert_t      *verts;         int numverts;
    const int32_t           *indexes;       int numindexes;
    const q3dsurface_t      *surfaces;      int numsurfaces;
    const q3dfog_t          *fogs;          int numfogs;
    const byte              *vis;           int vislen;
    const char              *entities;      int entlen;

    // per shader
    int                     *surfflags;     // Quake II SURF_*
    int                     *surfvalue;
    int                     *contents;      // Quake II CONTENTS_*

    q3surfrange_t           *ranges;        // per surface

    // how finely each curve span is cut (plan_patch_steps): per surface, the
    // index of its first span (u spans, then v spans) or -1, and the steps
    int                     *patchfirst;
    int                     numpatchspans;
    int                     *clipsteps;     // for collision
    int                     *drawsteps;     // for drawing

    q3array_t               clipplanes;     // cplane_t
    q3array_t               clipbrushes;    // q3clipbrush_t
#if USE_REF
    q3array_t               tris;           // q3tri_t
#endif

    // leaf brush lists, in final leaf order
    int                     numoutleafs;
    int                     *leaffirst, *leafcount;
    q3array_t               leafbrushlist;  // int, final brush index

    // vis
    int                     numclusters;
    int                     visrowsize;
    byte                    *viscompressed; // dvis_t, ready to copy
    int                     viscompressedlen;

#if USE_REF
    // welded geometry
    vec3_t                  *weldverts;     int numweldverts;
    int                     (*edges)[2];    int numedges;
    int                     *surfedgeedge;  // per surfedge: edge index
    byte                    *surfedgevert;  // per surfedge: 0 or 1
#endif
} q3load_t;

static void free_load(q3load_t *L)
{
    Z_Free(L->surfflags);
    Z_Free(L->surfvalue);
    Z_Free(L->contents);
    Z_Free(L->ranges);
    Z_Free(L->patchfirst);
    Z_Free(L->clipsteps);
    Z_Free(L->drawsteps);
    array_free(&L->clipplanes);
    array_free(&L->clipbrushes);
#if USE_REF
    array_free(&L->tris);
    Z_Free(L->weldverts);
    Z_Free(L->edges);
    Z_Free(L->surfedgeedge);
    Z_Free(L->surfedgevert);
#endif
    Z_Free(L->leaffirst);
    Z_Free(L->leafcount);
    array_free(&L->leafbrushlist);
    Z_Free(L->viscompressed);
}

static const void *get_lump(q3load_t *L, int lump, size_t elem, int *count)
{
    uint32_t ofs = LittleLong(L->header->lumps[lump].fileofs);
    uint32_t len = LittleLong(L->header->lumps[lump].filelen);

    if ((uint64_t)ofs + len > L->filelen || (elem > 1 && len % elem) || (len && (ofs & 3))) {
        *count = -1;
        return NULL;
    }

    *count = elem ? (int)(len / elem) : (int)len;
    return L->buf + ofs;
}

/*
===============================================================================

FLAGS

===============================================================================
*/

static int translate_contents(int q3)
{
    int c = 0;

    if (q3 & Q3_CONTENTS_SOLID)         c |= CONTENTS_SOLID;
    if (q3 & Q3_CONTENTS_LAVA)          c |= CONTENTS_LAVA;
    if (q3 & Q3_CONTENTS_SLIME)         c |= CONTENTS_SLIME;
    if (q3 & Q3_CONTENTS_WATER)         c |= CONTENTS_WATER;
    if (q3 & Q3_CONTENTS_PLAYERCLIP)    c |= CONTENTS_PLAYERCLIP;
    if (q3 & Q3_CONTENTS_MONSTERCLIP)   c |= CONTENTS_MONSTERCLIP;
    if (q3 & Q3_CONTENTS_DETAIL)        c |= CONTENTS_DETAIL;
    if (q3 & Q3_CONTENTS_TRANSLUCENT)   c |= CONTENTS_TRANSLUCENT;
    if (q3 & Q3_CONTENTS_TRIGGER)       c |= CONTENTS_TRIGGER;  // jump pads' exact touch

    // dropped: FOG (volumes are not drawn as such), AREAPORTAL (areas are
    // flattened), TELEPORTER/JUMPPAD/CLUSTERPORTAL/DONOTENTER/BOTCLIP (Quake
    // III bot routing), NODROP (the client's void floor stands in for it,
    // CL_SetVoidFloor), STRUCTURAL, ORIGIN (never in a BSP)
    return c;
}

static void shader_texinfo_name(const char *shader, char *out, size_t size)
{
    char name[MAX_QPATH];

    Q_strlcpy(name, shader, sizeof(name));
    for (char *p = name; *p; p++)
        if (*p == '\\')
            *p = '/';

    char *dot = strrchr(name, '.');
    char *slash = strrchr(name, '/');
    if (dot && (!slash || dot > slash) &&
        (!Q_stricmp(dot, ".tga") || !Q_stricmp(dot, ".jpg") || !Q_stricmp(dot, ".png")))
        *dot = 0;

    // The renderers name a wall texture "textures/" + name + ".wal" and
    // normalize the path, so this names the shader itself either way.
    if (!Q_strncasecmp(name, "textures/", 9))
        Q_strlcpy(out, name + 9, size);
    else
        Q_concat(out, size, "../", name);
}

static void translate_shaders(q3load_t *L)
{
    L->surfflags = Z_Mallocz(sizeof(int) * max(L->numshaders, 1));
    L->surfvalue = Z_Mallocz(sizeof(int) * max(L->numshaders, 1));
    L->contents = Z_Mallocz(sizeof(int) * max(L->numshaders, 1));

    for (int i = 0; i < L->numshaders; i++) {
        const q3dshader_t *in = &L->shaders[i];
        int q3f = LittleLong(in->surfaceFlags);
        int q3c = LittleLong(in->contentFlags);
        int f = 0;

        if (q3f & Q3_SURF_SLICK)    f |= SURF_SLICK;
        if (q3f & Q3_SURF_SKY)      f |= SURF_SKY;
        if (q3f & Q3_SURF_NODRAW)   f |= SURF_NODRAW;
        if (q3c & Q3_CONTENTS_FOG)  f |= SURF_NODRAW;   // a fog volume's face

        L->contents[i] = translate_contents(q3c);

#if USE_REF
        char name[sizeof(in->shader) + 1];
        memcpy(name, in->shader, sizeof(in->shader));
        name[sizeof(in->shader)] = 0;

        const q3shader_t *sh = Q3Shader_Find(name);
        if (sh) {
            if (sh->no_draw && !(f & SURF_SKY))
                f |= SURF_NODRAW;

            if (sh->content_flags & (Q3_CONTENTS_WATER | Q3_CONTENTS_SLIME)) {
                f |= SURF_WARP | SURF_TRANS33;      // Quake II water and slime
            } else if (sh->content_flags & Q3_CONTENTS_LAVA) {
                f |= SURF_WARP;
            } else if (sh->translucent && !(f & SURF_SKY)) {
                // see-through: an additive effect reads as faint, a blended
                // surface as mostly there (bsp_mesh.c create_poly's alpha)
                f |= sh->additive ? SURF_TRANS33 : SURF_TRANS66;
            }

            if (sh->surface_light > 0 && !(f & SURF_SKY)) {
                f |= SURF_LIGHT;
                L->surfvalue[i] = (int)sh->surface_light;
            }
        }
#endif

        L->surfflags[i] = f;
    }
}

/*
===============================================================================

CURVES

===============================================================================
*/

static inline void bezier_weights(float t, float w[3])
{
    float s = 1.0f - t;
    w[0] = s * s;
    w[1] = 2.0f * s * t;
    w[2] = t * t;
}

static float span_deviation(const q3drawvert_t *a, const q3drawvert_t *b, const q3drawvert_t *c)
{
    vec3_t mid;
    VectorAdd(a->xyz, c->xyz, mid);
    VectorScale(mid, 0.5f, mid);
    VectorSubtract(b->xyz, mid, mid);
    return VectorLength(mid);
}

// Segments a quadratic span needs so the polyline stays within `tol` of the
// curve: a parabola's chord error falls with the square of the segment count,
// and the curve's own deviation from its chord is half the control point's.
static int span_steps(float deviation, float tol)
{
    if (tol < 0.01f)
        tol = 0.01f;
    int n = (int)ceilf(sqrtf(deviation * 0.5f / tol));
    return clamp(n, 1, Q3_CURVE_MAX_STEPS);
}

/*
CURVES THAT MEET ARE CUT THE SAME WAY ALONG THE EDGE THEY SHARE.

How finely a span is cut comes from how far the curve bends: the most any row of
the patch strays from its chord. Two patches that meet along an edge (an arch and
the strip that caps it, the halves of a pipe) each used to work that out from
their OWN rows. So one could cut the shared edge into 6 and the other into 4. The
edge then had points on one side that the other side lacked, and light came
through the slivers between them: the bright seams along curved edges (Matt,
2026-11-16).

So the whole map is planned first. Every span of every patch is a node. The two
patch edges a span ends on are looked up by their three control points, either
way round. Spans that meet across an edge are joined, and so is everything joined
to those. Each group takes the finest cut any member needs. A curve meeting a
flat brush face is not covered: a brush face has no curve to plan.
*/
typedef struct {
    int     key[9];     // the edge's three control points, in 1/8 units, one fixed way round
    int     node;       // the span it bounds
} q3edgekey_t;

static int edgekey_cmp(const void *a, const void *b)
{
    const int *ka = ((const q3edgekey_t *)a)->key, *kb = ((const q3edgekey_t *)b)->key;
    for (int i = 0; i < 9; i++)
        if (ka[i] != kb[i])
            return ka[i] < kb[i] ? -1 : 1;
    return 0;
}

// false for an edge pinched to a point (a cone's tip), which would join every
// patch that meets there
static bool edge_key(q3edgekey_t *k, const q3drawvert_t *a, const q3drawvert_t *b,
                     const q3drawvert_t *c, int node)
{
    const q3drawvert_t *p[3] = { a, b, c };
    int q[3][3];

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            q[i][j] = (int)floorf(p[i]->xyz[j] * 8.0f + 0.5f);

    int order = 0;
    for (int j = 0; j < 3 && !order; j++)
        order = (q[0][j] > q[2][j]) - (q[0][j] < q[2][j]);
    if (!order)
        return false;

    for (int i = 0; i < 3; i++) {
        const int *src = q[order > 0 ? 2 - i : i];
        for (int j = 0; j < 3; j++)
            k->key[i * 3 + j] = src[j];
    }
    k->node = node;
    return true;
}

static int span_root(int *parent, int n)
{
    while (parent[n] != n) {
        parent[n] = parent[parent[n]];
        n = parent[n];
    }
    return n;
}

static bool validate_patch(const q3dsurface_t *s, int numverts);

static bool patch_surface(const q3load_t *L, int i)
{
    const q3dsurface_t *s = &L->surfaces[i];
    int shader = LittleLong(s->shaderNum);

    return LittleLong(s->surfaceType) == Q3_MST_PATCH &&
           shader >= 0 && shader < L->numshaders &&
           validate_patch(s, L->numverts);
}

// The steps of every patch span (L->patchfirst) at tolerance `tol`. Caller frees.
static int *plan_patch_steps(const q3load_t *L, float tol)
{
    int total = L->numpatchspans;
    int *steps = Z_Malloc(sizeof(int) * max(total, 1));
    int *parent = Z_Malloc(sizeof(int) * max(total, 1));
    int *finest = Z_Mallocz(sizeof(int) * max(total, 1));
    q3edgekey_t *keys = Z_Malloc(sizeof(q3edgekey_t) * max(total * 2, 1));
    int numkeys = 0;

    for (int i = 0; i < L->numsurfaces; i++) {
        int base = L->patchfirst[i];
        if (base < 0)
            continue;

        const q3dsurface_t *s = &L->surfaces[i];
        const q3drawvert_t *ctrl = L->verts + LittleLong(s->firstVert);
        int w = LittleLong(s->patchWidth), h = LittleLong(s->patchHeight);
        int uspans = (w - 1) / 2, vspans = (h - 1) / 2;

        for (int su = 0; su < uspans; su++) {
            int node = base + su;
            float dev = 0;
            for (int r = 0; r < h; r++) {
                const q3drawvert_t *row = ctrl + r * w + su * 2;
                dev = max(dev, span_deviation(&row[0], &row[1], &row[2]));
            }
            steps[node] = span_steps(dev, tol);
            parent[node] = node;

            // the patch's first and last rows are the edges this span cuts
            for (int e = 0; e < 2; e++) {
                const q3drawvert_t *row = ctrl + (e ? h - 1 : 0) * w + su * 2;
                if (edge_key(&keys[numkeys], &row[0], &row[1], &row[2], node))
                    numkeys++;
            }
        }

        for (int sv = 0; sv < vspans; sv++) {
            int node = base + uspans + sv;
            float dev = 0;
            for (int c = 0; c < w; c++) {
                const q3drawvert_t *col = ctrl + sv * 2 * w + c;
                dev = max(dev, span_deviation(&col[0], &col[w], &col[w * 2]));
            }
            steps[node] = span_steps(dev, tol);
            parent[node] = node;

            for (int e = 0; e < 2; e++) {
                const q3drawvert_t *col = ctrl + sv * 2 * w + (e ? w - 1 : 0);
                if (edge_key(&keys[numkeys], &col[0], &col[w], &col[w * 2], node))
                    numkeys++;
            }
        }
    }

    qsort(keys, numkeys, sizeof(keys[0]), edgekey_cmp);
    for (int k = 1; k < numkeys; k++) {
        if (edgekey_cmp(&keys[k - 1], &keys[k]))
            continue;
        int a = span_root(parent, keys[k - 1].node), b = span_root(parent, keys[k].node);
        if (a != b)
            parent[a] = b;
    }

    for (int n = 0; n < total; n++) {
        int r = span_root(parent, n);
        finest[r] = max(finest[r], steps[n]);
    }
    for (int n = 0; n < total; n++)
        steps[n] = finest[span_root(parent, n)];

    Z_Free(keys);
    Z_Free(finest);
    Z_Free(parent);
    return steps;
}

// The grid of points a patch tessellates to, each span cut as planned
// (plan_patch_steps). Caller frees.
static q3vtx_t *tessellate_patch(const q3drawvert_t *ctrl, int width, int height,
                                 const int *usteps, const int *vsteps, int *cols_p, int *rows_p)
{
    int uspans = (width - 1) / 2, vspans = (height - 1) / 2;
    int cols = 1, rows = 1;

    for (int su = 0; su < uspans; su++)
        cols += usteps[su];
    for (int sv = 0; sv < vspans; sv++)
        rows += vsteps[sv];

    // the span and parameter of every grid column and row
    int *colspan = Z_Malloc(sizeof(int) * (cols + rows));
    int *rowspan = colspan + cols;
    float *colt = Z_Malloc(sizeof(float) * (cols + rows));
    float *rowt = colt + cols;

    int k = 0;
    for (int su = 0; su < uspans; su++)
        for (int i = 0; i < usteps[su]; i++, k++) {
            colspan[k] = su;
            colt[k] = (float)i / usteps[su];
        }
    colspan[k] = uspans - 1;
    colt[k] = 1.0f;

    k = 0;
    for (int sv = 0; sv < vspans; sv++)
        for (int i = 0; i < vsteps[sv]; i++, k++) {
            rowspan[k] = sv;
            rowt[k] = (float)i / vsteps[sv];
        }
    rowspan[k] = vspans - 1;
    rowt[k] = 1.0f;

    q3vtx_t *grid = Z_Mallocz(sizeof(q3vtx_t) * cols * rows);

    for (int r = 0; r < rows; r++) {
        float wv[3];
        bezier_weights(rowt[r], wv);

        for (int c = 0; c < cols; c++) {
            float wu[3];
            bezier_weights(colt[c], wu);

            q3vtx_t *out = &grid[r * cols + c];
            const q3drawvert_t *base = ctrl + rowspan[r] * 2 * width + colspan[c] * 2;

            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    const q3drawvert_t *cp = base + i * width + j;
                    float w = wv[i] * wu[j];
                    VectorMA(out->xyz, w, cp->xyz, out->xyz);
                    out->st[0] += w * cp->st[0];
                    out->st[1] += w * cp->st[1];
                    VectorMA(out->normal, w, cp->normal, out->normal);
                    out->alpha += w * cp->color[3] * (1.0f / 255);
                }
            }
            VectorNormalize(out->normal);
        }
    }

    Z_Free(colspan);
    Z_Free(colt);

    *cols_p = cols;
    *rows_p = rows;
    return grid;
}

// Orders (a, b, c) so the triangle faces the way its vertex normals do, in
// Quake's winding (front normal = cross(c - a, b - a)). False if degenerate.
static bool orient_triangle(const q3vtx_t **a, const q3vtx_t **b, const q3vtx_t **c)
{
    vec3_t e1, e2, n, vn;

    VectorSubtract((*b)->xyz, (*a)->xyz, e1);
    VectorSubtract((*c)->xyz, (*a)->xyz, e2);
    CrossProduct(e2, e1, n);

    if (VectorLength(n) < 0.001f)
        return false;

    VectorAdd((*a)->normal, (*b)->normal, vn);
    VectorAdd(vn, (*c)->normal, vn);
    if (DotProduct(vn, n) < 0) {
        const q3vtx_t *t = *b;
        *b = *c;
        *c = t;
    }
    return true;
}

static bool add_clip_plane(cplane_t *planes, int *count, const vec3_t normal, float dist)
{
    for (int i = 0; i < *count; i++) {
        if (DotProduct(planes[i].normal, normal) > 0.9999f && fabsf(planes[i].dist - dist) < 0.01f)
            return false;
    }
    if (*count >= Q3_MAX_FACET_PLANES)
        return false;

    cplane_t *p = &planes[(*count)++];
    memset(p, 0, sizeof(*p));
    VectorCopy(normal, p->normal);
    p->dist = dist;
    SetPlaneType(p);
    SetPlaneSignbits(p);
    return true;
}

static bool is_supporting(const vec3_t normal, float dist, const vec3_t *pts, int numpts)
{
    for (int i = 0; i < numpts; i++)
        if (DotProduct(pts[i], normal) > dist + 0.01f)
            return false;
    return true;
}

// A facet of a curve as a brush: the facet's own plane, a parallel one
// Q3_FACET_THICKNESS behind, its three edges, then the bevels that keep a box
// sweeping past an edge from catching on the plane's extension - the axial
// ones, and every edge crossed with every axis that supports the slab.
static void add_facet_brush(q3load_t *L, const vec3_t p0, const vec3_t p1, const vec3_t p2, int shader)
{
    const float *p[3] = { p0, p1, p2 };
    cplane_t planes[Q3_MAX_FACET_PLANES];
    vec3_t pts[6], n, e1, e2, tmp;
    int count = 0;

    VectorSubtract(p1, p0, e1);
    VectorSubtract(p2, p0, e2);
    CrossProduct(e2, e1, n);
    if (VectorNormalize(n) < 0.001f)
        return;

    float d = DotProduct(n, p0);

    for (int i = 0; i < 3; i++) {
        VectorCopy(p[i], pts[i]);
        VectorMA(p[i], -Q3_FACET_THICKNESS, n, pts[i + 3]);
    }

    add_clip_plane(planes, &count, n, d);
    VectorNegate(n, tmp);
    add_clip_plane(planes, &count, tmp, -(d - Q3_FACET_THICKNESS));

    for (int i = 0; i < 3; i++) {
        const float *a = p[i], *b = p[(i + 1) % 3], *c = p[(i + 2) % 3];
        vec3_t edge, en;
        VectorSubtract(b, a, edge);
        CrossProduct(edge, n, en);
        if (VectorNormalize(en) < 0.0001f)
            continue;
        VectorSubtract(c, a, tmp);
        if (DotProduct(en, tmp) > 0)
            VectorNegate(en, en);
        add_clip_plane(planes, &count, en, DotProduct(en, a));
    }

    // axial bevels
    for (int axis = 0; axis < 3; axis++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            vec3_t an = { 0, 0, 0 };
            float best = -FLT_MAX;
            an[axis] = (float)sign;
            for (int i = 0; i < 6; i++)
                best = max(best, DotProduct(pts[i], an));
            add_clip_plane(planes, &count, an, best);
        }
    }

    // edge bevels: the three facet edges and the slab's depth direction, each
    // crossed with each axis, through a point of that edge
    for (int e = 0; e < 4; e++) {
        vec3_t dir;
        if (e < 3)
            VectorSubtract(p[(e + 1) % 3], p[e], dir);
        else
            VectorCopy(n, dir);

        for (int axis = 0; axis < 3; axis++) {
            vec3_t a = { 0, 0, 0 }, bn;
            a[axis] = 1;
            CrossProduct(dir, a, bn);
            if (VectorNormalize(bn) < 0.0001f)
                continue;

            for (int sign = 0; sign < 2; sign++) {
                if (sign)
                    VectorNegate(bn, bn);
                // through any facet corner the bevel can rest on
                for (int v = 0; v < 3; v++) {
                    if (e < 3 && v != e)
                        continue;
                    float bd = DotProduct(bn, p[v]);
                    if (is_supporting(bn, bd, (const vec3_t *)pts, 6))
                        add_clip_plane(planes, &count, bn, bd);
                }
            }
        }
    }

    q3clipbrush_t *brush = array_push(&L->clipbrushes);
    brush->firstplane = L->clipplanes.count;
    brush->numplanes = count;
    brush->shader = shader;
    ClearBounds(brush->mins, brush->maxs);
    for (int i = 0; i < 6; i++) {
        AddPointToBounds(pts[i], brush->mins, brush->maxs);
        VectorCopy(pts[i], brush->pts[i]);
    }

    for (int i = 0; i < count; i++)
        *(cplane_t *)array_push(&L->clipplanes) = planes[i];
}

static bool validate_patch(const q3dsurface_t *s, int numverts)
{
    int w = LittleLong(s->patchWidth), h = LittleLong(s->patchHeight);
    int nv = LittleLong(s->numVerts), fv = LittleLong(s->firstVert);

    if (w < 3 || h < 3 || !(w & 1) || !(h & 1))
        return false;
    if (w > Q3_CURVE_MAX_CONTROL || h > Q3_CURVE_MAX_CONTROL)
        return false;
    if (w * h != nv || fv < 0 || fv + nv > numverts)
        return false;
    return true;
}

static void build_patch(q3load_t *L, int surfnum)
{
    const q3dsurface_t *s = &L->surfaces[surfnum];
    q3surfrange_t *range = &L->ranges[surfnum];
    int shader = LittleLong(s->shaderNum);
    const q3drawvert_t *ctrl = L->verts + LittleLong(s->firstVert);
    int width = LittleLong(s->patchWidth), height = LittleLong(s->patchHeight);
    int cols, rows;
    const int base = L->patchfirst[surfnum], uspans = (width - 1) / 2;

    // collision: solid, playerclip, liquids - whatever the shader's contents say
    bool solid = L->contents[shader] != 0 &&
                 !(LittleLong(L->shaders[shader].surfaceFlags) & Q3_SURF_NONSOLID);

    if (solid) {
        q3vtx_t *grid = tessellate_patch(ctrl, width, height, L->clipsteps + base,
                                         L->clipsteps + base + uspans, &cols, &rows);

        range->firstclip = L->clipbrushes.count;
        for (int r = 0; r < rows - 1; r++) {
            for (int c = 0; c < cols - 1; c++) {
                const q3vtx_t *q[4] = {
                    &grid[r * cols + c], &grid[r * cols + c + 1],
                    &grid[(r + 1) * cols + c + 1], &grid[(r + 1) * cols + c]
                };
                for (int t = 0; t < 2; t++) {
                    const q3vtx_t *a = q[0], *b = q[t + 1], *cc = q[t + 2];
                    if (orient_triangle(&a, &b, &cc))
                        add_facet_brush(L, a->xyz, b->xyz, cc->xyz, shader);
                }
            }
        }
        range->numclip = L->clipbrushes.count - range->firstclip;
        Z_Free(grid);
    }

#if USE_REF
    if (L->surfflags[shader] & SURF_NODRAW && !(L->surfflags[shader] & SURF_SKY))
        return;

    q3vtx_t *grid = tessellate_patch(ctrl, width, height, L->drawsteps + base,
                                     L->drawsteps + base + uspans, &cols, &rows);

    range->firsttri = L->tris.count;
    for (int r = 0; r < rows - 1; r++) {
        for (int c = 0; c < cols - 1; c++) {
            const q3vtx_t *q[4] = {
                &grid[r * cols + c], &grid[r * cols + c + 1],
                &grid[(r + 1) * cols + c + 1], &grid[(r + 1) * cols + c]
            };
            for (int t = 0; t < 2; t++) {
                const q3vtx_t *a = q[0], *b = q[t + 1], *cc = q[t + 2];
                if (!orient_triangle(&a, &b, &cc))
                    continue;
                q3tri_t *tri = array_push(&L->tris);
                tri->v[0] = *a;
                tri->v[1] = *b;
                tri->v[2] = *cc;
                tri->shader = shader;
            }
        }
    }
    range->numtris = L->tris.count - range->firsttri;
    Z_Free(grid);
#endif
}

#if USE_REF
static void vertex_from_draw(const q3drawvert_t *dv, q3vtx_t *out)
{
    VectorCopy(dv->xyz, out->xyz);
    out->st[0] = dv->st[0];
    out->st[1] = dv->st[1];
    VectorCopy(dv->normal, out->normal);
    out->alpha = dv->color[3] * (1.0f / 255);
}

static int build_trisurf(q3load_t *L, int surfnum)
{
    const q3dsurface_t *s = &L->surfaces[surfnum];
    q3surfrange_t *range = &L->ranges[surfnum];
    int shader = LittleLong(s->shaderNum);
    int fv = LittleLong(s->firstVert), nv = LittleLong(s->numVerts);
    int fi = LittleLong(s->firstIndex), ni = LittleLong(s->numIndexes);

    if (fv < 0 || nv < 0 || fv + nv > L->numverts)
        return Q_ERR_BAD_INDEX;
    if (fi < 0 || ni < 0 || fi + ni > L->numindexes || ni % 3)
        return Q_ERR_BAD_INDEX;

    if (L->surfflags[shader] & SURF_NODRAW && !(L->surfflags[shader] & SURF_SKY))
        return Q_ERR_SUCCESS;

    range->firsttri = L->tris.count;
    for (int i = 0; i < ni; i += 3) {
        q3vtx_t v[3];
        for (int k = 0; k < 3; k++) {
            int idx = LittleLong(L->indexes[fi + i + k]);
            if (idx < 0 || idx >= nv)
                return Q_ERR_BAD_INDEX;
            vertex_from_draw(&L->verts[fv + idx], &v[k]);
        }
        const q3vtx_t *a = &v[0], *b = &v[1], *c = &v[2];
        if (!orient_triangle(&a, &b, &c))
            continue;
        q3tri_t *tri = array_push(&L->tris);
        tri->v[0] = *a;
        tri->v[1] = *b;
        tri->v[2] = *c;
        tri->shader = shader;
    }
    range->numtris = L->tris.count - range->firsttri;
    return Q_ERR_SUCCESS;
}

/*
CUTTING THE WORLD'S TRIANGLES AT LEAF BOUNDARIES.

A Quake II compiler splits every face by the tree, so each face lies in one
leaf, and bsp_mesh.c gives each triangle the cluster its centre is in - which
light list and light statistics it reads. Quake III's compiler does not split
draw surfaces: one floor polygon runs under walls and across a dozen clusters,
and the cluster at its centre can be one that sees none of the lights over the
rest of it - or no cluster at all, its centre buried under a wall. On q3dm0
that is the sharp, dark triangle on the open floor.

So the world's triangles are pushed down the tree here and cut where they
cross a node plane, the way the Quake II compilers do it. A piece lying ON a
plane goes to the side its own normal faces - the leaf it is seen from.
Inline models move, so their triangles are left alone.

A piece that ends up in an opaque leaf (no cluster) is KEPT. This was meant to
drop them, but the test read LittleLong (unsigned) as < 0 and never fired, so
everything Matt has seen keeps them. When it was fixed (2026-11-16) it would
have dropped 6-19% of the world's triangles, and an offline check found
hundreds per map with no solid brush in front of them - q3dm0's blue tube
(sfx/proto_zzztblu3), q3dm1's tongue decorations - so not all of them are
hidden. Quake III draws a surface whole whenever any leaf of it is seen.

A cut point is computed from the edge's two ends in a fixed order, so the two
triangles sharing an edge put the new vertex in exactly the same place.
*/

#define Q3_CLIP_MAX_VERTS   32
#define Q3_CLIP_EPSILON     0.01f

typedef struct {
    int     node;
    int     numverts;
    q3vtx_t v[Q3_CLIP_MAX_VERTS];
} q3clippoly_t;

static void lerp_vertex(const q3vtx_t *a, const q3vtx_t *b, float da, float db, q3vtx_t *out)
{
    // same order whichever way round the edge is walked
    if (memcmp(a->xyz, b->xyz, sizeof(vec3_t)) > 0) {
        const q3vtx_t *t = a; a = b; b = t;
        float tf = da; da = db; db = tf;
    }
    float f = da / (da - db);
    for (int k = 0; k < 3; k++) {
        out->xyz[k] = a->xyz[k] + f * (b->xyz[k] - a->xyz[k]);
        out->normal[k] = a->normal[k] + f * (b->normal[k] - a->normal[k]);
    }
    out->st[0] = a->st[0] + f * (b->st[0] - a->st[0]);
    out->st[1] = a->st[1] + f * (b->st[1] - a->st[1]);
    out->alpha = a->alpha + f * (b->alpha - a->alpha);
}

static void emit_clipped(q3array_t *out, const q3clippoly_t *p, int shader)
{
    for (int i = 1; i + 1 < p->numverts; i++) {
        vec3_t e1, e2, n;
        VectorSubtract(p->v[i].xyz, p->v[0].xyz, e1);
        VectorSubtract(p->v[i + 1].xyz, p->v[0].xyz, e2);
        CrossProduct(e2, e1, n);
        if (VectorLength(n) < 0.001f)
            continue;
        q3tri_t *tri = array_push(out);
        tri->v[0] = p->v[0];
        tri->v[1] = p->v[i];
        tri->v[2] = p->v[i + 1];
        tri->shader = shader;
    }
}

// returns how many pieces landed in opaque leaves (kept; see above)
static int clip_tri_to_leaves(const q3load_t *L, const q3tri_t *tri, q3array_t *out, q3array_t *stack, int *pieces)
{
    vec3_t e1, e2, facenormal;
    VectorSubtract(tri->v[1].xyz, tri->v[0].xyz, e1);
    VectorSubtract(tri->v[2].xyz, tri->v[0].xyz, e2);
    CrossProduct(e2, e1, facenormal);   // Quake winding (orient_triangle)

    int buried = 0;
    stack->count = 0;
    q3clippoly_t *first = array_push(stack);
    first->node = 0;
    first->numverts = 3;
    first->v[0] = tri->v[0];
    first->v[1] = tri->v[1];
    first->v[2] = tri->v[2];

    while (stack->count) {
        q3clippoly_t p = ARRAY_AT(*stack, q3clippoly_t, stack->count - 1);
        stack->count--;

        if (p.node < 0) {
            int leaf = -1 - p.node;
            // (LittleLong is unsigned: the cluster goes through an int, or -1 never tests < 0)
            if (leaf >= 0 && leaf < L->numleafs && (int)LittleLong(L->leafs[leaf].cluster) < 0 && L->numclusters > 0)
                buried++;
            emit_clipped(out, &p, tri->shader);
            (*pieces)++;
            continue;
        }
        if (p.node >= L->numnodes) {
            emit_clipped(out, &p, tri->shader);
            (*pieces)++;
            continue;
        }

        const q3dnode_t *node = &L->nodes[p.node];
        const q3dplane_t *plane = &L->planes[LittleLong(node->planeNum)];
        vec3_t pn = { LittleFloat(plane->normal[0]), LittleFloat(plane->normal[1]), LittleFloat(plane->normal[2]) };
        float pd = LittleFloat(plane->dist);
        int child[2] = { LittleLong(node->children[0]), LittleLong(node->children[1]) };

        float d[Q3_CLIP_MAX_VERTS];
        bool front = false, back = false;
        for (int i = 0; i < p.numverts; i++) {
            d[i] = DotProduct(p.v[i].xyz, pn) - pd;
            if (d[i] > Q3_CLIP_EPSILON) front = true;
            else if (d[i] < -Q3_CLIP_EPSILON) back = true;
        }

        int side = -1;
        if (!front && !back)
            side = DotProduct(facenormal, pn) >= 0 ? 0 : 1;     // on the plane
        else if (!back)
            side = 0;
        else if (!front)
            side = 1;
        else if (p.numverts + 2 > Q3_CLIP_MAX_VERTS)
            side = d[0] >= 0 ? 0 : 1;   // pathological; leave it whole

        if (side >= 0) {
            p.node = child[side];
            *(q3clippoly_t *)array_push(stack) = p;
            continue;
        }

        q3clippoly_t f, b;
        f.numverts = b.numverts = 0;
        f.node = child[0];
        b.node = child[1];
        for (int i = 0; i < p.numverts; i++) {
            int j = (i + 1) % p.numverts;
            float di = fabsf(d[i]) <= Q3_CLIP_EPSILON ? 0 : d[i];
            float dj = fabsf(d[j]) <= Q3_CLIP_EPSILON ? 0 : d[j];

            if (di >= 0) f.v[f.numverts++] = p.v[i];
            if (di <= 0) b.v[b.numverts++] = p.v[i];
            if ((di > 0 && dj < 0) || (di < 0 && dj > 0)) {
                q3vtx_t mid;
                lerp_vertex(&p.v[i], &p.v[j], d[i], d[j], &mid);
                f.v[f.numverts++] = mid;
                b.v[b.numverts++] = mid;
            }
        }
        if (f.numverts >= 3)
            *(q3clippoly_t *)array_push(stack) = f;
        if (b.numverts >= 3)
            *(q3clippoly_t *)array_push(stack) = b;
    }
    return buried;
}

static void clip_world_to_leaves(q3load_t *L)
{
    int fs = LittleLong(L->models[0].firstSurface);
    int ns = LittleLong(L->models[0].numSurfaces);
    q3array_t out = ARRAY(q3tri_t);
    q3array_t stack = ARRAY(q3clippoly_t);
    int before = L->tris.count, buried = 0, pieces = 0;

    for (int s = 0; s < L->numsurfaces; s++) {
        q3surfrange_t *r = &L->ranges[s];
        int first = r->firsttri, num = r->numtris;
        bool world = s >= fs && s < fs + ns;

        r->firsttri = out.count;
        for (int t = first; t < first + num; t++) {
            const q3tri_t *tri = &ARRAY_AT(L->tris, q3tri_t, t);
            if (world) {
                buried += clip_tri_to_leaves(L, tri, &out, &stack, &pieces);
            } else {
                *(q3tri_t *)array_push(&out) = *tri;
            }
        }
        r->numtris = out.count - r->firsttri;
        if (!r->numtris)
            r->firsttri = 0;
    }

    array_free(&L->tris);
    L->tris = out;
    array_free(&stack);
    Com_DPrintf("%s: %d triangles cut at leaf boundaries into %d (%d of them in opaque leaves, kept)\n",
                __func__, before, L->tris.count, buried);
}
#endif

#if USE_REF
/*
Quake III's fog volumes: a brush whose shader says "fogparms ( r g b ) d".
Every one in the retail maps is an axis-aligned box, so the box is what the
renderer gets (vkpt fog.c turns it into a block of real fog in the volumetric
medium). Returns how many there are; `out` NULL just counts.
*/
static int build_fog_volumes(const q3load_t *L, mfogvolume_t *out)
{
    int count = 0;

    for (int i = 0; i < L->numfogs; i++) {
        const q3dfog_t *fog = &L->fogs[i];
        char name[sizeof(fog->shader) + 1];
        int bn = LittleLong(fog->brushNum);

        if (bn < 0 || bn >= L->numbrushes)
            continue;

        memcpy(name, fog->shader, sizeof(fog->shader));
        name[sizeof(fog->shader)] = 0;
        const q3shader_t *sh = Q3Shader_Find(name);
        if (!sh || !sh->has_fog || sh->fog_distance <= 0)
            continue;

        const q3dbrush_t *brush = &L->brushes[bn];
        int fs = LittleLong(brush->firstSide), ns = LittleLong(brush->numSides);
        vec3_t mins = { -FLT_MAX, -FLT_MAX, -FLT_MAX }, maxs = { FLT_MAX, FLT_MAX, FLT_MAX };

        if (fs < 0 || ns < 0 || fs + ns > L->numbrushsides)
            continue;

        // the box is the brush's axial planes; a slanted side (none in the
        // retail maps) is left out, so the box can only be larger
        for (int k = 0; k < ns; k++) {
            int pn = LittleLong(L->brushsides[fs + k].planeNum);
            if (pn < 0 || pn >= L->numplanes)
                continue;
            const q3dplane_t *pl = &L->planes[pn];
            float d = LittleFloat(pl->dist);
            for (int ax = 0; ax < 3; ax++) {
                float c = LittleFloat(pl->normal[ax]);
                if (c > 0.9999f)
                    maxs[ax] = min(maxs[ax], d);
                else if (c < -0.9999f)
                    mins[ax] = max(mins[ax], -d);
            }
        }
        if (mins[0] >= maxs[0] || mins[1] >= maxs[1] || mins[2] >= maxs[2] ||
            maxs[0] == FLT_MAX || mins[0] == -FLT_MAX ||
            maxs[1] == FLT_MAX || mins[1] == -FLT_MAX ||
            maxs[2] == FLT_MAX || mins[2] == -FLT_MAX)
            continue;

        if (out) {
            mfogvolume_t *v = &out[count];
            VectorCopy(mins, v->mins);
            VectorCopy(maxs, v->maxs);
            VectorCopy(sh->fog_color, v->color);
            v->distance = sh->fog_distance;
        }
        count++;
    }
    return count;
}
#endif

static int build_surfaces(q3load_t *L)
{
    L->ranges = Z_Mallocz(sizeof(q3surfrange_t) * max(L->numsurfaces, 1));

    // every curve's spans, cut alike where curves meet (plan_patch_steps)
    L->patchfirst = Z_Malloc(sizeof(int) * max(L->numsurfaces, 1));
    L->numpatchspans = 0;
    for (int i = 0; i < L->numsurfaces; i++) {
        if (!patch_surface(L, i)) {
            L->patchfirst[i] = -1;
            continue;
        }
        const q3dsurface_t *s = &L->surfaces[i];
        L->patchfirst[i] = L->numpatchspans;
        L->numpatchspans += (LittleLong(s->patchWidth) - 1) / 2 + (LittleLong(s->patchHeight) - 1) / 2;
    }
    L->clipsteps = plan_patch_steps(L, map_q3_curve_clip_error->value);
#if USE_REF
    L->drawsteps = plan_patch_steps(L, map_q3_curve_error->value);
#endif

    for (int i = 0; i < L->numsurfaces; i++) {
        const q3dsurface_t *s = &L->surfaces[i];
        int shader = LittleLong(s->shaderNum);
        int type = LittleLong(s->surfaceType);

        if (shader < 0 || shader >= L->numshaders) {
            Com_DPrintf("%s: surface %d has bad shader %d\n", __func__, i, shader);
            continue;
        }

        switch (type) {
        case Q3_MST_PATCH:
            if (!validate_patch(s, L->numverts)) {
                Com_DPrintf("%s: surface %d is a bad patch\n", __func__, i);
                continue;
            }
            build_patch(L, i);
            break;
#if USE_REF
        case Q3_MST_PLANAR:
        case Q3_MST_TRIANGLE_SOUP: {
            int ret = build_trisurf(L, i);
            if (ret)
                return ret;
            break;
        }
#endif
        default:
            // flares, and on the server everything that only draws
            break;
        }
    }

    return Q_ERR_SUCCESS;
}

/*
===============================================================================

LEAVES

===============================================================================
*/

static bool boxes_overlap(const vec3_t amins, const vec3_t amaxs, const vec3_t bmins, const vec3_t bmaxs)
{
    for (int i = 0; i < 3; i++)
        if (amins[i] > bmaxs[i] || amaxs[i] < bmins[i])
            return false;
    return true;
}

/*
A CURVE'S SLABS GO IN EVERY LEAF THEY REACH.

A box trace tests only the brushes listed in the leaves it passes through. The
facets used to be listed by Quake III's own list of the leaves each curve is in,
cut down by bounding box. But a facet is a flat chord of the curve, and its slab
runs 2 units behind it, so on the inside of a bend the slab leaves the curve's
leaves. q3dm1's tower (km_arena1tower_short) had a third of its points with no
solid behind them, and the player sank into it there (Matt, 2026-11-16: "I am
clipping through curved surfaces"). The arches beside it were the same.

So each world facet is pushed down the tree by its six corners (the slab is
exactly their hull) into every leaf it touches, as a Quake II compiler places a
brush, with a unit of slack either side of each plane. Quake III's list still
adds its leaves too, and each facet is listed once per leaf.

Opaque leaves get them too. Only brushes stop a trace - a leaf's own contents
never do - and Quake III's opaque leaves include the sealed-off void outside
the map, which has no brushes: a curve flush against the outer hull has its
slab there. Skipping them brought hundreds of holes back on pro-q3dm6/dm13.
*/
#define Q3_FACET_LEAF_SLACK     1.0f

typedef struct {
    int     leaf, facet;
} q3leaffacet_t;

static void facet_leaves(const q3load_t *L, int facet, q3array_t *stack, q3array_t *out)
{
    const q3clipbrush_t *cb = &ARRAY_AT(L->clipbrushes, q3clipbrush_t, facet);

    stack->count = 0;
    *(int *)array_push(stack) = 0;      // the world's head node

    while (stack->count) {
        int n = ARRAY_AT(*stack, int, stack->count - 1);
        stack->count--;

        const q3dnode_t *node = &L->nodes[n];
        int pn = LittleLong(node->planeNum);
        if (pn < 0 || pn >= L->numplanes)
            continue;   // the load fails on it later
        const q3dplane_t *plane = &L->planes[pn];
        vec3_t normal = { LittleFloat(plane->normal[0]), LittleFloat(plane->normal[1]), LittleFloat(plane->normal[2]) };
        float dist = LittleFloat(plane->dist);

        bool side[2] = { false, false };
        for (int i = 0; i < 6; i++) {
            float d = DotProduct(cb->pts[i], normal) - dist;
            if (d > -Q3_FACET_LEAF_SLACK)
                side[0] = true;
            if (d < Q3_FACET_LEAF_SLACK)
                side[1] = true;
        }

        for (int j = 0; j < 2; j++) {
            if (!side[j])
                continue;
            int child = LittleLong(node->children[j]);
            if (child < 0) {
                int leaf = -1 - child;
                if (leaf < L->numleafs) {
                    q3leaffacet_t *lf = array_push(out);
                    lf->leaf = leaf;
                    lf->facet = facet;
                }
            } else if (child > n && child < L->numnodes) {
                // (Quake III writes a node before its children; anything else is
                // a broken tree, which BSP_ValidateTree turns away)
                *(int *)array_push(stack) = child;
            }
        }
    }
}

// Every leaf's brush list: its own brushes, then the curve facets that reach
// into it. Leaf 0 is our solid leaf with none; the inline models' leaves
// follow the map's.
static int build_leaf_brushes(q3load_t *L)
{
    int numq3brushes = L->numbrushes;
    int ret = Q_ERR_SUCCESS;

    L->numoutleafs = 1 + L->numleafs + max(L->nummodels - 1, 0);
    L->leaffirst = Z_Mallocz(sizeof(int) * L->numoutleafs);
    L->leafcount = Z_Mallocz(sizeof(int) * L->numoutleafs);

    // the world's facets down the tree (A CURVE'S SLABS GO IN EVERY LEAF THEY REACH)
    q3array_t stack = ARRAY(int), pairs = ARRAY(q3leaffacet_t);
    int numfacets = L->clipbrushes.count;
    int *treefirst = Z_Mallocz(sizeof(int) * (L->numleafs + 1));
    int *treelist = NULL;
    int *listed = Z_Malloc(sizeof(int) * max(numfacets, 1));    // the leaf a facet was last listed in

    int wfs = LittleLong(L->models[0].firstSurface), wns = LittleLong(L->models[0].numSurfaces);
    if (wfs < 0 || wns < 0 || wfs + wns > L->numsurfaces) {
        ret = Q_ERR_BAD_INDEX;
        goto done;
    }
    for (int sn = wfs; sn < wfs + wns; sn++) {
        const q3surfrange_t *r = &L->ranges[sn];
        for (int c = 0; c < r->numclip; c++)
            facet_leaves(L, r->firstclip + c, &stack, &pairs);
    }

    // grouped by leaf
    for (int k = 0; k < pairs.count; k++)
        treefirst[ARRAY_AT(pairs, q3leaffacet_t, k).leaf + 1]++;
    for (int i = 0; i < L->numleafs; i++)
        treefirst[i + 1] += treefirst[i];
    treelist = Z_Malloc(sizeof(int) * max(pairs.count, 1));
    {
        int *fill = Z_Malloc(sizeof(int) * max(L->numleafs, 1));
        memcpy(fill, treefirst, sizeof(int) * L->numleafs);
        for (int k = 0; k < pairs.count; k++) {
            const q3leaffacet_t *lf = &ARRAY_AT(pairs, q3leaffacet_t, k);
            treelist[fill[lf->leaf]++] = lf->facet;
        }
        Z_Free(fill);
    }
    for (int k = 0; k < numfacets; k++)
        listed[k] = -1;

    for (int i = 0; i < L->numleafs; i++) {
        const q3dleaf_t *in = &L->leafs[i];
        int out = i + 1;
        int flb = LittleLong(in->firstLeafBrush), nlb = LittleLong(in->numLeafBrushes);
        int fls = LittleLong(in->firstLeafSurface), nls = LittleLong(in->numLeafSurfaces);

        if (flb < 0 || nlb < 0 || flb + nlb > L->numleafbrushes ||
            fls < 0 || nls < 0 || fls + nls > L->numleafsurfaces) {
            ret = Q_ERR_BAD_INDEX;
            goto done;
        }

        L->leaffirst[out] = L->leafbrushlist.count;

        for (int k = 0; k < nlb; k++) {
            int b = LittleLong(L->leafbrushes[flb + k]);
            if (b < 0 || b >= numq3brushes) {
                ret = Q_ERR_BAD_INDEX;
                goto done;
            }
            *(int *)array_push(&L->leafbrushlist) = b;
        }

        // the curve facets in this leaf: those the tree put here...
        for (int k = treefirst[i]; k < treefirst[i + 1]; k++) {
            int f = treelist[k];
            if (listed[f] == i)
                continue;
            listed[f] = i;
            *(int *)array_push(&L->leafbrushlist) = numq3brushes + f;
        }

        // ...and those of the curves Quake III lists here, by bounding box
        vec3_t lmins, lmaxs;
        for (int j = 0; j < 3; j++) {
            lmins[j] = (float)LittleLong(in->mins[j]) - 2;
            lmaxs[j] = (float)LittleLong(in->maxs[j]) + 2;
        }

        for (int k = 0; k < nls; k++) {
            int sn = LittleLong(L->leafsurfaces[fls + k]);
            if (sn < 0 || sn >= L->numsurfaces)
                continue;
            const q3surfrange_t *r = &L->ranges[sn];
            for (int c = 0; c < r->numclip; c++) {
                int f = r->firstclip + c;
                const q3clipbrush_t *cb = &ARRAY_AT(L->clipbrushes, q3clipbrush_t, f);
                if (listed[f] == i || !boxes_overlap(cb->mins, cb->maxs, lmins, lmaxs))
                    continue;
                listed[f] = i;
                *(int *)array_push(&L->leafbrushlist) = numq3brushes + f;
            }
        }

        L->leafcount[out] = L->leafbrushlist.count - L->leaffirst[out];
    }

done:
    array_free(&stack);
    array_free(&pairs);
    Z_Free(treefirst);
    Z_Free(treelist);
    Z_Free(listed);
    if (ret)
        return ret;

    for (int m = 1; m < L->nummodels; m++) {
        const q3dmodel_t *in = &L->models[m];
        int out = L->numleafs + m;
        int fb = LittleLong(in->firstBrush), nb = LittleLong(in->numBrushes);
        int fs = LittleLong(in->firstSurface), ns = LittleLong(in->numSurfaces);

        if (fb < 0 || nb < 0 || fb + nb > numq3brushes)
            return Q_ERR_BAD_INDEX;
        if (fs < 0 || ns < 0 || fs + ns > L->numsurfaces)
            return Q_ERR_BAD_INDEX;

        L->leaffirst[out] = L->leafbrushlist.count;
        for (int k = 0; k < nb; k++)
            *(int *)array_push(&L->leafbrushlist) = fb + k;
        for (int k = 0; k < ns; k++) {
            const q3surfrange_t *r = &L->ranges[fs + k];
            for (int c = 0; c < r->numclip; c++)
                *(int *)array_push(&L->leafbrushlist) = numq3brushes + r->firstclip + c;
        }
        L->leafcount[out] = L->leafbrushlist.count - L->leaffirst[out];
    }

    return Q_ERR_SUCCESS;
}

/*
===============================================================================

VIS

===============================================================================
*/

// Quake II's run-length coding: a zero byte is followed by how many zero
// bytes it stands for.
static byte *compress_row(const byte *in, int rowbytes, byte *out)
{
    for (int j = 0; j < rowbytes; j++) {
        *out++ = in[j];
        if (in[j])
            continue;

        int rep = 1;
        for (j++; j < rowbytes; j++) {
            if (in[j] || rep == 255)
                break;
            rep++;
        }
        *out++ = rep;
        j--;
    }
    return out;
}

static int build_vis(q3load_t *L)
{
    if (L->vislen < 8)
        return Q_ERR_SUCCESS;   // not vised: everything sees everything

    int numclusters = LittleLong(((const int32_t *)L->vis)[0]);
    int clusterbytes = LittleLong(((const int32_t *)L->vis)[1]);
    int rowsize = (numclusters + 7) >> 3;

    if (numclusters <= 0)
        return Q_ERR_SUCCESS;
    // the client keeps PVS rows in VIS_MAX_BYTES buffers
    if (numclusters > MAX_MAP_LEAFS || clusterbytes < rowsize)
        return Q_ERR_TOO_MANY;
    if (8 + (int64_t)numclusters * clusterbytes > L->vislen)
        return Q_ERR_TOO_FEW;

    const byte *rows = L->vis + 8;
    size_t longs = (rowsize + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    uint64_t *pvs = Z_Mallocz(sizeof(uint64_t) * longs * numclusters);
    uint64_t *phs = Z_Malloc(sizeof(uint64_t) * longs);

    for (int i = 0; i < numclusters; i++) {
        memcpy(pvs + longs * i, rows + (size_t)clusterbytes * i, rowsize);
        Q_SetBit((byte *)(pvs + longs * i), i);     // a cluster always sees itself
    }

    // header, then each row at worst doubled by the coding
    size_t maxlen = 4 + 8 * (size_t)numclusters + (size_t)numclusters * 2 * (rowsize * 2 + 2);
    byte *out = Z_Malloc(maxlen);
    dvis_t *dvis = (dvis_t *)out;
    byte *cursor = out + 4 + 8 * (size_t)numclusters;

    dvis->numclusters = numclusters;

    for (int i = 0; i < numclusters; i++) {
        const uint64_t *row = pvs + longs * i;

        dvis->bitofs[i][DVIS_PVS] = (uint32_t)(cursor - out);
        cursor = compress_row((const byte *)row, rowsize, cursor);

        // potentially hearable: everything visible from anything visible
        memset(phs, 0, sizeof(uint64_t) * longs);
        for (int j = 0; j < numclusters; j++) {
            if (!Q_IsBitSet((const byte *)row, j))
                continue;
            const uint64_t *other = pvs + longs * j;
            for (size_t k = 0; k < longs; k++)
                phs[k] |= other[k];
        }

        dvis->bitofs[i][DVIS_PHS] = (uint32_t)(cursor - out);
        cursor = compress_row((const byte *)phs, rowsize, cursor);
    }

    Z_Free(pvs);
    Z_Free(phs);

    L->numclusters = numclusters;
    L->visrowsize = rowsize;
    L->viscompressed = out;
    L->viscompressedlen = (int)(cursor - out);
    return Q_ERR_SUCCESS;
}

/*
===============================================================================

WELDING

===============================================================================
*/

#if USE_REF
static inline uint32_t hash_float_bits(uint32_t h, float f)
{
    union { float f; uint32_t u; } v;
    v.f = f + 0.0f;     // -0 and +0 alike
    h ^= v.u;
    h *= 0x9E3779B1u;
    return h ^ (h >> 15);
}

// Shares a vertex between every face that has one at the same place, and an
// edge between every face that has one between the same two vertices, so that
// faces connect the way they do in a Quake II map.
static void weld_geometry(q3load_t *L)
{
    int numtris = L->tris.count;
    int maxverts = numtris * 3;
    uint32_t vsize = 1, esize = 1;

    while (vsize < (uint32_t)maxverts * 2)
        vsize <<= 1;
    esize = vsize;

    int *vhash = Z_Mallocz(sizeof(int) * vsize);
    int *ehash = Z_Mallocz(sizeof(int) * esize);
    int *corner = Z_Malloc(sizeof(int) * max(maxverts, 1));

    L->weldverts = Z_Malloc(sizeof(vec3_t) * max(maxverts, 1));
    L->edges = Z_Malloc(sizeof(*L->edges) * max(maxverts, 1));
    L->surfedgeedge = Z_Malloc(sizeof(int) * max(maxverts, 1));
    L->surfedgevert = Z_Malloc(max(maxverts, 1));
    L->numweldverts = 0;
    L->numedges = 0;

    for (int t = 0; t < numtris; t++) {
        const q3tri_t *tri = &ARRAY_AT(L->tris, q3tri_t, t);
        for (int k = 0; k < 3; k++) {
            const float *p = tri->v[k].xyz;
            uint32_t h = hash_float_bits(hash_float_bits(hash_float_bits(2166136261u, p[0]), p[1]), p[2]);
            uint32_t slot = h & (vsize - 1);
            int found = -1;

            while (vhash[slot]) {
                int idx = vhash[slot] - 1;
                if (VectorCompare(L->weldverts[idx], p)) {
                    found = idx;
                    break;
                }
                slot = (slot + 1) & (vsize - 1);
            }
            if (found < 0) {
                found = L->numweldverts++;
                VectorCopy(p, L->weldverts[found]);
                vhash[slot] = found + 1;
            }
            corner[t * 3 + k] = found;
        }
    }

    for (int t = 0; t < numtris; t++) {
        for (int k = 0; k < 3; k++) {
            int a = corner[t * 3 + k], b = corner[t * 3 + (k + 1) % 3];
            int lo = min(a, b), hi = max(a, b);
            uint32_t h = ((uint32_t)lo * 0x9E3779B1u) ^ ((uint32_t)hi * 0x85EBCA77u);
            h ^= h >> 15;
            uint32_t slot = h & (esize - 1);
            int found = -1;

            while (ehash[slot]) {
                int idx = ehash[slot] - 1;
                int ea = L->edges[idx][0], eb = L->edges[idx][1];
                if (min(ea, eb) == lo && max(ea, eb) == hi) {
                    found = idx;
                    break;
                }
                slot = (slot + 1) & (esize - 1);
            }
            if (found < 0) {
                found = L->numedges++;
                L->edges[found][0] = a;
                L->edges[found][1] = b;
                ehash[slot] = found + 1;
            }

            // the surfedge's vertex is the corner it starts from
            L->surfedgeedge[t * 3 + k] = found;
            L->surfedgevert[t * 3 + k] = (L->edges[found][0] == a) ? 0 : 1;
        }
    }

    Z_Free(vhash);
    Z_Free(ehash);
    Z_Free(corner);
}

// The UV mapping that passes exactly through a triangle's three corners,
// taken in the triangle's own plane (no component along its normal).
static void triangle_uv_axis(const q3tri_t *tri, float axis[2][4])
{
    double e1[3], e2[3];
    for (int k = 0; k < 3; k++) {
        e1[k] = (double)tri->v[1].xyz[k] - tri->v[0].xyz[k];
        e2[k] = (double)tri->v[2].xyz[k] - tri->v[0].xyz[k];
    }
    double a11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
    double a12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
    double a22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
    double det = a11 * a22 - a12 * a12;
    bool ok = det > 1e-12 * a11 * a22 && det > 0;

    for (int c = 0; c < 2; c++) {
        double ax[3] = { 0, 0, 0 };
        if (ok) {
            double d1 = (double)tri->v[1].st[c] - tri->v[0].st[c];
            double d2 = (double)tri->v[2].st[c] - tri->v[0].st[c];
            double alpha = (a22 * d1 - a12 * d2) / det;
            double beta = (a11 * d2 - a12 * d1) / det;
            for (int k = 0; k < 3; k++)
                ax[k] = alpha * e1[k] + beta * e2[k];
        }
        double off = tri->v[0].st[c];
        for (int k = 0; k < 3; k++) {
            axis[c][k] = (float)ax[k];
            off -= ax[k] * tri->v[0].xyz[k];
        }
        axis[c][3] = (float)off;
    }
}

// dP/du of a triangle: where the texture's u runs across it.
static void triangle_sdir(const q3tri_t *tri, vec3_t sdir)
{
    vec3_t dp0, dp1;
    VectorSubtract(tri->v[1].xyz, tri->v[0].xyz, dp0);
    VectorSubtract(tri->v[2].xyz, tri->v[0].xyz, dp1);
    float dt0[2] = { tri->v[1].st[0] - tri->v[0].st[0], tri->v[1].st[1] - tri->v[0].st[1] };
    float dt1[2] = { tri->v[2].st[0] - tri->v[0].st[0], tri->v[2].st[1] - tri->v[0].st[1] };
    float den = dt0[0] * dt1[1] - dt1[0] * dt0[1];

    if (fabsf(den) < 1e-12f) {
        VectorCopy(dp0, sdir);
        return;
    }
    float r = 1.0f / den;
    for (int k = 0; k < 3; k++)
        sdir[k] = (dt1[1] * dp0[k] - dt0[1] * dp1[k]) * r;
}
#endif

/*
===============================================================================

LOAD

===============================================================================
*/

#define ALLOC(size) q3_alloc(bsp, size)

static void *q3_alloc(bsp_t *bsp, size_t size)
{
    void *p = Hunk_Alloc(&bsp->hunk, size);
    if (!p)
        Com_Error(ERR_DROP, "%s: %s needs more memory than was reserved", __func__, bsp->name);
    memset(p, 0, size);
    return p;
}

#define HUNK_SIZE(count, type) ALIGN((size_t)(count) * sizeof(type), 64)

static bool brush_contains_point(const q3load_t *L, int b, const vec3_t p)
{
    const q3dbrush_t *brush = &L->brushes[b];
    int fs = LittleLong(brush->firstSide), ns = LittleLong(brush->numSides);

    for (int i = 0; i < ns; i++) {
        const q3dplane_t *pl = &L->planes[LittleLong(L->brushsides[fs + i].planeNum)];
        if (DotProduct(p, pl->normal) > pl->dist)
            return false;
    }
    return ns > 0;
}

int BSP_LoadQ3(bsp_t *bsp, const byte *buf, size_t filelen)
{
    q3load_t L;
    int ret = Q_ERR_SUCCESS;
    int count;

    memset(&L, 0, sizeof(L));
    L.clipplanes = (q3array_t)ARRAY(cplane_t);
    L.clipbrushes = (q3array_t)ARRAY(q3clipbrush_t);
#if USE_REF
    L.tris = (q3array_t)ARRAY(q3tri_t);
#endif
    L.leafbrushlist = (q3array_t)ARRAY(int);

    if (filelen < sizeof(q3dheader_t))
        return Q_ERR_FILE_TOO_SMALL;

    L.buf = buf;
    L.filelen = filelen;
    L.header = (const q3dheader_t *)buf;

#define GET(field, lump, type) \
    L.field = get_lump(&L, lump, sizeof(type), &count); \
    if (count < 0) { ret = Q_ERR_BAD_EXTENT; goto fail; } \
    L.num##field = count;

    GET(shaders,        Q3_LUMP_SHADERS,        q3dshader_t);
    GET(planes,         Q3_LUMP_PLANES,         q3dplane_t);
    GET(nodes,          Q3_LUMP_NODES,          q3dnode_t);
    GET(leafs,          Q3_LUMP_LEAFS,          q3dleaf_t);
    GET(leafsurfaces,   Q3_LUMP_LEAFSURFACES,   int32_t);
    GET(leafbrushes,    Q3_LUMP_LEAFBRUSHES,    int32_t);
    GET(models,         Q3_LUMP_MODELS,         q3dmodel_t);
    GET(brushes,        Q3_LUMP_BRUSHES,        q3dbrush_t);
    GET(brushsides,     Q3_LUMP_BRUSHSIDES,     q3dbrushside_t);
    GET(verts,          Q3_LUMP_DRAWVERTS,      q3drawvert_t);
    GET(indexes,        Q3_LUMP_DRAWINDEXES,    int32_t);
    GET(surfaces,       Q3_LUMP_SURFACES,       q3dsurface_t);
    GET(fogs,           Q3_LUMP_FOGS,           q3dfog_t);
#undef GET

    L.vis = get_lump(&L, Q3_LUMP_VISIBILITY, 1, &count);
    if (count < 0) { ret = Q_ERR_BAD_EXTENT; goto fail; }
    L.vislen = count;

    L.entities = get_lump(&L, Q3_LUMP_ENTITIES, 1, &count);
    if (count < 0) { ret = Q_ERR_BAD_EXTENT; goto fail; }
    L.entlen = count;

    if (!L.numnodes || !L.numleafs || !L.nummodels || !L.numplanes) {
        ret = Q_ERR_TOO_FEW;
        goto fail;
    }

#if USE_REF
    Q3Shader_Load();
#endif

    translate_shaders(&L);

    if ((ret = build_surfaces(&L)) != 0)
        goto fail;
    if ((ret = build_leaf_brushes(&L)) != 0)
        goto fail;
    if ((ret = build_vis(&L)) != 0)
        goto fail;
#if USE_REF
    clip_world_to_leaves(&L);
    weld_geometry(&L);
#endif

    //
    // sizes
    //
    int numfaces = 0;
#if USE_REF
    int numfogvolumes = 0;
    numfaces = L.tris.count;
#endif
    int numplanes = L.numplanes + L.clipplanes.count + numfaces;
    int numbrushes = L.numbrushes + L.clipbrushes.count;
    int numbrushsides = L.numbrushsides + L.clipplanes.count;
    int numtexinfo = max(L.numshaders, 1);

    size_t memsize = 4096 * 4;
    memsize += HUNK_SIZE(numtexinfo, mtexinfo_t);
    memsize += HUNK_SIZE(numplanes, cplane_t);
    memsize += HUNK_SIZE(numbrushsides, mbrushside_t);
    memsize += HUNK_SIZE(numbrushes, mbrush_t);
    memsize += HUNK_SIZE(L.leafbrushlist.count + 1, mbrush_t *);
    memsize += HUNK_SIZE(L.numoutleafs, mleaf_t);
    memsize += HUNK_SIZE(L.numnodes, mnode_t);
    memsize += HUNK_SIZE(L.nummodels, mmodel_t);
    memsize += HUNK_SIZE(2, marea_t);
    memsize += ALIGN((size_t)L.viscompressedlen, 64);
    memsize += ALIGN((size_t)L.entlen + 1, 64);
#if USE_REF
    memsize += HUNK_SIZE(numfaces, mface_t);
    memsize += HUNK_SIZE(L.numweldverts, mvertex_t);
    memsize += HUNK_SIZE(L.numedges, medge_t);
    memsize += HUNK_SIZE(numfaces * 3, msurfedge_t);
    memsize += HUNK_SIZE(numfaces * 3 * 2, vec3_t);
    memsize += HUNK_SIZE(numfaces * 3, mbasis_t);
    memsize += HUNK_SIZE(numfaces * 3, byte);
    numfogvolumes = build_fog_volumes(&L, NULL);
    memsize += HUNK_SIZE(max(numfogvolumes, 1), mfogvolume_t);
#endif

    Hunk_Begin(&bsp->hunk, memsize);
    bsp->extended = true;
    bsp->q3 = true;

    //
    // texinfo: one per shader
    //
    bsp->numtexinfo = numtexinfo;
    bsp->texinfo = ALLOC(sizeof(mtexinfo_t) * numtexinfo);
    for (int i = 0; i < L.numshaders; i++) {
        mtexinfo_t *out = &bsp->texinfo[i];
        char raw[sizeof(L.shaders[i].shader) + 1];
        memcpy(raw, L.shaders[i].shader, sizeof(L.shaders[i].shader));
        raw[sizeof(L.shaders[i].shader)] = 0;

        shader_texinfo_name(raw, out->name, sizeof(out->name));

        // the game sees 15 characters: keep the end, which names the texture
        const char *tail = strrchr(raw, '/');
        Q_strlcpy(out->c.name, tail ? tail + 1 : raw, sizeof(out->c.name));
        out->c.flags = L.surfflags[i];
        out->c.value = L.surfvalue[i];
#if USE_REF
        out->radiance = L.surfvalue[i];
        out->numframes = 1;
        out->next = NULL;
        out->alt = NULL;
#endif
    }

    //
    // planes: the map's, the curve facets', then one per drawn triangle
    //
    bsp->numplanes = numplanes;
    bsp->planes = ALLOC(sizeof(cplane_t) * max(numplanes, 1));
    for (int i = 0; i < L.numplanes; i++) {
        cplane_t *out = &bsp->planes[i];
        for (int k = 0; k < 3; k++)
            out->normal[k] = LittleFloat(L.planes[i].normal[k]);
        out->dist = LittleFloat(L.planes[i].dist);
        SetPlaneType(out);
        SetPlaneSignbits(out);
    }
    if (L.clipplanes.count)
        memcpy(bsp->planes + L.numplanes, L.clipplanes.data, sizeof(cplane_t) * L.clipplanes.count);

    //
    // brush sides and brushes
    //
    bsp->numbrushsides = numbrushsides;
    bsp->brushsides = ALLOC(sizeof(mbrushside_t) * max(numbrushsides, 1));
    for (int i = 0; i < L.numbrushsides; i++) {
        mbrushside_t *out = &bsp->brushsides[i];
        int pn = LittleLong(L.brushsides[i].planeNum);
        int sn = LittleLong(L.brushsides[i].shaderNum);
        if (pn < 0 || pn >= L.numplanes) {
            ret = Q_ERR_BAD_INDEX;
            goto fail;
        }
        out->plane = bsp->planes + pn;
        out->texinfo = (sn >= 0 && sn < L.numshaders) ? bsp->texinfo + sn : &nulltexinfo;
    }
    for (int i = 0; i < L.clipplanes.count; i++) {
        mbrushside_t *out = &bsp->brushsides[L.numbrushsides + i];
        out->plane = bsp->planes + L.numplanes + i;
        out->texinfo = &nulltexinfo;    // set per brush below
    }

    bsp->numbrushes = numbrushes;
    bsp->brushes = ALLOC(sizeof(mbrush_t) * max(numbrushes, 1));
    for (int i = 0; i < L.numbrushes; i++) {
        mbrush_t *out = &bsp->brushes[i];
        int fs = LittleLong(L.brushes[i].firstSide), ns = LittleLong(L.brushes[i].numSides);
        int sn = LittleLong(L.brushes[i].shaderNum);
        if (fs < 0 || ns < 0 || fs + ns > L.numbrushsides) {
            ret = Q_ERR_BAD_INDEX;
            goto fail;
        }
        out->firstbrushside = bsp->brushsides + fs;
        out->numsides = ns;
        out->contents = (sn >= 0 && sn < L.numshaders) ? L.contents[sn] : CONTENTS_SOLID;

        // Quake III climbs a ladder by the side's surface flag, Quake II by
        // the brush's contents
        for (int k = 0; k < ns; k++) {
            int ss = LittleLong(L.brushsides[fs + k].shaderNum);
            if (ss >= 0 && ss < L.numshaders && (LittleLong(L.shaders[ss].surfaceFlags) & Q3_SURF_LADDER))
                out->contents |= CONTENTS_LADDER;
        }
    }
    for (int i = 0; i < L.clipbrushes.count; i++) {
        const q3clipbrush_t *cb = &ARRAY_AT(L.clipbrushes, q3clipbrush_t, i);
        mbrush_t *out = &bsp->brushes[L.numbrushes + i];
        out->firstbrushside = bsp->brushsides + L.numbrushsides + cb->firstplane;
        out->numsides = cb->numplanes;
        out->contents = L.contents[cb->shader];
        for (int k = 0; k < cb->numplanes; k++)
            out->firstbrushside[k].texinfo = bsp->texinfo + cb->shader;
    }

    bsp->numleafbrushes = L.leafbrushlist.count;
    bsp->leafbrushes = ALLOC(sizeof(mbrush_t *) * (L.leafbrushlist.count + 1));
    for (int i = 0; i < L.leafbrushlist.count; i++)
        bsp->leafbrushes[i] = bsp->brushes + ARRAY_AT(L.leafbrushlist, int, i);

    //
    // vis
    //
    if (L.viscompressed) {
        bsp->numvisibility = L.viscompressedlen;
        bsp->vis = ALLOC(L.viscompressedlen);
        memcpy(bsp->vis, L.viscompressed, L.viscompressedlen);
        bsp->visrowsize = L.visrowsize;
    }

    //
    // areas: one open area, area 0 for solid
    //
    bsp->numareas = 2;
    bsp->areas = ALLOC(sizeof(marea_t) * 2);
    bsp->numareaportals = 0;
    bsp->areaportals = NULL;
    bsp->lastareaportal = 0;

    //
    // leafs
    //
    bsp->numleafs = L.numoutleafs;
    bsp->leafs = ALLOC(sizeof(mleaf_t) * L.numoutleafs);
    {
        mleaf_t *solid = &bsp->leafs[0];
        solid->contents = CONTENTS_SOLID;
        solid->cluster = -1;
        solid->area = 0;
        solid->firstleafbrush = bsp->leafbrushes;
#if USE_REF
        solid->visframe = -1;
#endif
    }

    for (int i = 0; i < L.numleafs; i++) {
        const q3dleaf_t *in = &L.leafs[i];
        mleaf_t *out = &bsp->leafs[i + 1];
        int cluster = LittleLong(in->cluster);
        vec3_t mins, maxs, center;

        for (int k = 0; k < 3; k++) {
            mins[k] = (float)LittleLong(in->mins[k]);
            maxs[k] = (float)LittleLong(in->maxs[k]);
        }

        if (cluster < 0) {
            out->cluster = -1;
        } else if (!bsp->vis) {
            out->cluster = 0;
        } else {
            if (cluster >= L.numclusters) {
                ret = Q_ERR_BAD_INDEX;
                goto fail;
            }
            out->cluster = cluster;
        }
        out->area = (cluster < 0) ? 0 : 1;

        out->firstleafbrush = bsp->leafbrushes + L.leaffirst[i + 1];
        out->numleafbrushes = L.leafcount[i + 1];
        out->pointtest = true;

        // what the whole leaf counts as: solid when opaque, else the liquid
        // its middle is in
        VectorAvg(mins, maxs, center);
        out->contents = (cluster < 0) ? CONTENTS_SOLID : 0;
        out->brushcontents = out->contents;
        for (int k = 0; k < out->numleafbrushes; k++) {
            int b = (int)(out->firstleafbrush[k] - bsp->brushes);
            int bc = out->firstleafbrush[k]->contents;
            out->brushcontents |= bc;
            if (cluster >= 0 && b < L.numbrushes && (bc & (MASK_WATER | CONTENTS_MIST)) &&
                brush_contains_point(&L, b, center))
                out->contents |= bc & (MASK_WATER | CONTENTS_MIST);
        }

#if USE_REF
        VectorCopy(mins, out->mins);
        VectorCopy(maxs, out->maxs);
        out->visframe = -1;
#endif
    }

    for (int m = 1; m < L.nummodels; m++) {
        int idx = L.numleafs + m;
        mleaf_t *out = &bsp->leafs[idx];
        out->cluster = -1;
        out->area = 0;
        out->firstleafbrush = bsp->leafbrushes + L.leaffirst[idx];
        out->numleafbrushes = L.leafcount[idx];
        out->pointtest = true;
        for (int k = 0; k < out->numleafbrushes; k++)
            out->brushcontents |= out->firstleafbrush[k]->contents;
        out->contents = 0;
#if USE_REF
        for (int k = 0; k < 3; k++) {
            out->mins[k] = LittleFloat(L.models[m].mins[k]);
            out->maxs[k] = LittleFloat(L.models[m].maxs[k]);
        }
        out->visframe = -1;
#endif
    }

    //
    // nodes
    //
    bsp->numnodes = L.numnodes;
    bsp->nodes = ALLOC(sizeof(mnode_t) * L.numnodes);
    for (int i = 0; i < L.numnodes; i++) {
        const q3dnode_t *in = &L.nodes[i];
        mnode_t *out = &bsp->nodes[i];
        int pn = LittleLong(in->planeNum);

        if (pn < 0 || pn >= L.numplanes) {
            ret = Q_ERR_BAD_INDEX;
            goto fail;
        }
        out->plane = bsp->planes + pn;

        for (int j = 0; j < 2; j++) {
            int child = LittleLong(in->children[j]);
            if (child < 0) {
                int leaf = -(child + 1);
                if (leaf >= L.numleafs) {
                    ret = Q_ERR_BAD_INDEX;
                    goto fail;
                }
                out->children[j] = (mnode_t *)(bsp->leafs + leaf + 1);
            } else {
                if (child >= L.numnodes) {
                    ret = Q_ERR_BAD_INDEX;
                    goto fail;
                }
                out->children[j] = bsp->nodes + child;
            }
        }

#if USE_REF
        for (int k = 0; k < 3; k++) {
            out->mins[k] = (float)LittleLong(in->mins[k]);
            out->maxs[k] = (float)LittleLong(in->maxs[k]);
        }
        out->visframe = -1;
#endif
    }

#if USE_REF
    //
    // faces: one per drawn triangle
    //
    bsp->numvertices = L.numweldverts;
    bsp->vertices = ALLOC(sizeof(mvertex_t) * max(L.numweldverts, 1));
    for (int i = 0; i < L.numweldverts; i++)
        VectorCopy(L.weldverts[i], bsp->vertices[i].point);

    bsp->numedges = L.numedges;
    bsp->edges = ALLOC(sizeof(medge_t) * max(L.numedges, 1));
    for (int i = 0; i < L.numedges; i++) {
        bsp->edges[i].v[0] = bsp->vertices + L.edges[i][0];
        bsp->edges[i].v[1] = bsp->vertices + L.edges[i][1];
    }

    bsp->numsurfedges = numfaces * 3;
    bsp->surfedges = ALLOC(sizeof(msurfedge_t) * max(numfaces * 3, 1));
    for (int i = 0; i < numfaces * 3; i++) {
        bsp->surfedges[i].edge = bsp->edges + L.surfedgeedge[i];
        bsp->surfedges[i].vert = L.surfedgevert[i];
    }

    bsp->numbasisvectors = numfaces * 3 * 2;
    bsp->basisvectors = ALLOC(sizeof(vec3_t) * max(numfaces * 3 * 2, 1));
    bsp->numbases = numfaces * 3;
    bsp->bases = ALLOC(sizeof(mbasis_t) * max(numfaces * 3, 1));
    // each corner's vertex alpha, for alphaGen vertex (Team Arena's terrain
    // blends one rock into the next by it): indexed as the bases
    bsp->corner_alpha = ALLOC(max(numfaces * 3, 1));

    bsp->numfaces = numfaces;
    bsp->faces = ALLOC(sizeof(mface_t) * max(numfaces, 1));

    cplane_t *faceplanes = bsp->planes + L.numplanes + L.clipplanes.count;

    for (int i = 0; i < numfaces; i++) {
        const q3tri_t *tri = &ARRAY_AT(L.tris, q3tri_t, i);
        mface_t *out = &bsp->faces[i];
        cplane_t *plane = &faceplanes[i];
        vec3_t e1, e2, n, sdir;

        VectorSubtract(tri->v[1].xyz, tri->v[0].xyz, e1);
        VectorSubtract(tri->v[2].xyz, tri->v[0].xyz, e2);
        CrossProduct(e2, e1, n);
        VectorNormalize(n);
        VectorCopy(n, plane->normal);
        plane->dist = DotProduct(n, tri->v[0].xyz);
        SetPlaneType(plane);
        SetPlaneSignbits(plane);

        out->firstsurfedge = bsp->surfedges + i * 3;
        out->numsurfedges = 3;
        out->plane = plane;
        out->drawflags = 0;
        out->lightmap = NULL;
        for (int k = 0; k < MAX_LIGHTMAPS; k++)
            out->styles[k] = 255;
        out->numstyles = 0;
        out->texinfo = bsp->texinfo + tri->shader;
        out->uv_mapped = true;
        triangle_uv_axis(tri, out->uv_axis);
        out->firstbasis = i * 3;

        triangle_sdir(tri, sdir);

        for (int k = 0; k < 3; k++) {
            int s = i * 3 + k;
            float *vn = bsp->basisvectors[s * 2];
            float *vt = bsp->basisvectors[s * 2 + 1];

            VectorCopy(tri->v[k].normal, vn);
            if (VectorNormalize(vn) < 0.5f)
                VectorCopy(n, vn);

            // Gram-Schmidt the triangle's u direction against this corner's normal
            vec3_t t;
            VectorMA(sdir, -DotProduct(vn, sdir), vn, t);
            if (VectorNormalize(t) < 1e-6f) {
                vec3_t up = { 0, 0, 1 };
                if (fabsf(vn[2]) > 0.9f)
                    VectorSet(up, 1, 0, 0);
                CrossProduct(up, vn, t);
                VectorNormalize(t);
            }
            VectorCopy(t, vt);

            bsp->bases[s].normal = s * 2;
            bsp->bases[s].tangent = s * 2 + 1;
            bsp->bases[s].bitangent = s * 2 + 1;

            float a = min(max(tri->v[k].alpha, 0.0f), 1.0f) * 255;
            bsp->corner_alpha[s] = (byte)Q_rint(a);
        }
    }

    // no node or leaf face lists: the path tracers walk bsp->faces, and the
    // classic renderers do not draw Quake III maps
    bsp->numleaffaces = 0;
    bsp->leaffaces = NULL;

    bsp->numfogvolumes = numfogvolumes;
    bsp->fogvolumes = ALLOC(sizeof(mfogvolume_t) * max(numfogvolumes, 1));
    build_fog_volumes(&L, bsp->fogvolumes);
#endif

    //
    // models
    //
    bsp->nummodels = L.nummodels;
    bsp->models = ALLOC(sizeof(mmodel_t) * L.nummodels);
    for (int m = 0; m < L.nummodels; m++) {
        const q3dmodel_t *in = &L.models[m];
        mmodel_t *out = &bsp->models[m];

        for (int k = 0; k < 3; k++) {
            out->mins[k] = LittleFloat(in->mins[k]) - 1;
            out->maxs[k] = LittleFloat(in->maxs[k]) + 1;
            out->origin[k] = 0;
        }

        out->headnode = m ? (mnode_t *)(bsp->leafs + L.numleafs + m) : bsp->nodes;

#if USE_REF
        out->radius = RadiusFromBounds(out->mins, out->maxs);
        if (m == 0)
            continue;   // the world's faces are everything no model owns

        int fs = LittleLong(in->firstSurface), ns = LittleLong(in->numSurfaces);
        int first = -1, last = -1;
        for (int k = fs; k < fs + ns && k < L.numsurfaces; k++) {
            const q3surfrange_t *r = &L.ranges[k];
            if (!r->numtris)
                continue;
            if (first < 0)
                first = r->firsttri;
            last = r->firsttri + r->numtris;
        }
        if (first >= 0) {
            out->firstface = bsp->faces + first;
            out->numfaces = last - first;
        }
#endif
    }

    //
    // entities
    //
    bsp->numentitychars = L.entlen;
    bsp->entitystring = ALLOC(L.entlen + 1);
    if (L.entlen)
        memcpy(bsp->entitystring, L.entities, L.entlen);
    bsp->entitystring[L.entlen] = 0;

    Com_DPrintf("%s: Quake III map, %d shaders, %d brushes + %d curve facets, %d leafs, %d clusters, %d faces\n",
                bsp->name, L.numshaders, L.numbrushes, L.clipbrushes.count, L.numleafs, L.numclusters, numfaces);

    free_load(&L);
    return Q_ERR_SUCCESS;

fail:
    free_load(&L);
    return ret;
}

void BSP_Q3_Init(void)
{
    // How far, in world units, a curve may sag from its true shape when it is
    // cut into triangles - drawn, and for collision. Take effect on the next
    // map load that is not already cached. There is no distance LOD: the cut is
    // made once, at load. 0.25 is twice the triangles of the 0.5 this had (Matt,
    // 2026-11-16: "double it with no lod"); every quarter of the error doubles
    // them again. Curves that meet are cut alike whatever it is (plan_patch_steps).
    map_q3_curve_error = Cvar_Get("map_q3_curve_error", "0.25", 0);
    map_q3_curve_clip_error = Cvar_Get("map_q3_curve_clip_error", "2", 0);
}
