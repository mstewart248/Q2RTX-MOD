/*
Copyright (C) 1996-2001 Id Software, Inc.
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
// bsp_q1.c -- a Quake 1 map (BSP 29, BSP2, 2PSB) loaded as a Quake II bsp_t
//
// As with bsp_q3.c, a Quake 1 map is TRANSCODED into the bsp_t everything
// downstream reads. Quake II grew out of Quake, so most of it maps across one
// for one: the same planes, vertices, edges, faces with texinfo axes in
// texels, a node/leaf tree with leaf face lists, inline models, run-length
// coded vis. The differences, and what is done about each:
//
//  * Collision. Quake 1 has no brushes. It clips against HULLS: the draw tree
//    for a point, and two clipnode trees the compiler built with every brush
//    already grown by a player-sized box (hull 1) and a shambler-sized one
//    (hull 2). "clip" brushes exist only in those. The hulls are kept as they
//    are, and a box trace on a Quake 1 model runs through the hull Quake would
//    pick for that box (BSP_Q1BoxTrace, from cmodel.c). That is what Quake's
//    own game code expects, monsters and stairs and all.
//
//  * Contents are negative numbers; leaves get Quake II flags. Sky volumes
//    count as solid with a SURF_SKY surface, which is how a Quake II rocket
//    knows to vanish into the sky.
//
//  * Every solid region of the draw tree points at one shared leaf 0. Quake
//    II needs each leaf to have one parent, so each reference gets its own
//    copy of the solid leaf, appended after the map's leaves.
//
//  * Vis is per leaf: leaf N is cluster N-1, leaf 0 has none, and the rows
//    are the same coding. The big add-on maps have far more leaves than the
//    renderer wants clusters, so runs of neighbouring leaves share one there
//    (Q1_MAX_CLUSTERS). A PHS is built from the rows (the union of the PVS of
//    everything visible) unless that would take too long, when everything
//    can hear everything.
//
//  * Textures are inside the BSP, 8-bit, in Quake's palette. Their names
//    become "q1/<name>" (lower case, '*' spelled '#', as Quake ports name
//    external textures), so the renderers look for "textures/q1/<name>" - an
//    override or material - and the image loader falls back to the pixels
//    kept here (BSP_Q1FindMiptex). "+0name".."+9name" animate at Quake's 5
//    frames a second; each animation gets a ring of extra texinfos for the
//    renderer's frame links.
//
//  * Areas: one, as Quake has no area portals.
//
// Lightmaps are not kept: the path tracer lights everything itself.
//
#include "shared/shared.h"
#include "common/bsp.h"
#include "common/common.h"
#include "common/math.h"
#include "common/utils.h"
#include "common/zone.h"
#include "system/hunk.h"

#include <float.h>

extern mtexinfo_t nulltexinfo;

#define Q1_BSPVERSION       29
#define Q1_BSP2_IDENT       (('2'<<24)+('P'<<16)+('S'<<8)+'B')  // "BSP2"
#define Q1_2PSB_IDENT       (('B'<<24)+('S'<<16)+('P'<<8)+'2')  // "2PSB"

#define Q1_LUMP_ENTITIES        0
#define Q1_LUMP_PLANES          1
#define Q1_LUMP_TEXTURES        2
#define Q1_LUMP_VERTEXES        3
#define Q1_LUMP_VISIBILITY      4
#define Q1_LUMP_NODES           5
#define Q1_LUMP_TEXINFO         6
#define Q1_LUMP_FACES           7
#define Q1_LUMP_LIGHTING        8
#define Q1_LUMP_CLIPNODES       9
#define Q1_LUMP_LEAFS           10
#define Q1_LUMP_MARKSURFACES    11
#define Q1_LUMP_EDGES           12
#define Q1_LUMP_SURFEDGES       13
#define Q1_LUMP_MODELS          14
#define Q1_HEADER_LUMPS         15

// Most clusters a map gets. Quake 1 makes every leaf a cluster, and the big
// add-on maps have up to 65,000 of them: rows of 8 KB, half a gigabyte of PVS
// matrix, minutes of PHS. The renderer's light lists and second-order PVS were
// sized for Quake II maps, which have a few thousand. Neighbouring leaves are
// merged into clusters to stay under this.
#define Q1_MAX_CLUSTERS         4096

// most work (64-bit ORs) building a PHS may take, about a quarter of a second
#define Q1_MAX_PHS_COST         400000000ULL

/*
===============================================================================

READING

Everything is read into one plain layout first, whatever the format's field
widths, so the building below is written once.

===============================================================================
*/

enum { Q1F_29, Q1F_BSP2, Q1F_2PSB };

typedef struct {
    float   normal[3], dist;
} q1plane_t;

typedef struct {
    float   vecs[2][4];
    int     miptex;
    int     flags;
} q1texinfo_t;

typedef struct {
    int         planenum, side;
    int         firstedge, numedges;
    int         texinfo;
    byte        styles[4];
    int         lightofs;       // its lightmaps, one a style (q1_own_styles)
} q1face_t;

typedef struct {
    int         planenum;
    int         children[2];    // >= 0 a node, < 0 -(leaf + 1)
    float       mins[3], maxs[3];
    unsigned    firstface, numfaces;
} q1node_t;

typedef struct {
    int         contents;
    int         visofs;
    float       mins[3], maxs[3];
    unsigned    firstmark, nummark;
} q1leaf_t;

typedef struct {
    float       mins[3], maxs[3], origin[3];
    int         headnode[Q1_MAX_HULLS];
    int         visleafs;
    int         firstface, numfaces;
} q1model_t;

typedef struct {
    const byte  *buf;
    size_t      filelen;
    int         format;
    int         version;

    const byte  *lump[Q1_HEADER_LUMPS];
    int         lumplen[Q1_HEADER_LUMPS];

    int         numplanes;      q1plane_t       *planes;
    int         numvertexes;    vec3_t          *vertexes;
    int         numedges;       unsigned        (*edges)[2];
    int         numsurfedges;   int             *surfedges;
    int         numtexinfo;     q1texinfo_t     *texinfo;
    int         numfaces;       q1face_t        *faces;
    int         numnodes;       q1node_t        *nodes;
    int         numclipnodes;   mclipnode_t     *clipnodes;
    int         numleafs;       q1leaf_t        *leafs;
    int         nummarks;       unsigned        *marks;
    int         nummodels;      q1model_t       *models;
    int         nummiptex;      mq1miptex_t     *miptex;
} q1load_t;

static inline int32_t rl(const byte *p)   { int32_t v; memcpy(&v, p, 4); return LittleLong(v); }
static inline uint32_t rul(const byte *p) { uint32_t v; memcpy(&v, p, 4); return LittleLong(v); }
static inline int rs(const byte *p)       { int16_t v; memcpy(&v, p, 2); return LittleShort(v); }
static inline int rus(const byte *p)      { uint16_t v; memcpy(&v, p, 2); return (uint16_t)LittleShort(v); }
static inline float rf(const byte *p)     { float v; memcpy(&v, p, 4); return LittleFloat(v); }

static void free_load(q1load_t *L)
{
    Z_Free(L->planes);
    Z_Free(L->vertexes);
    Z_Free(L->edges);
    Z_Free(L->surfedges);
    Z_Free(L->texinfo);
    Z_Free(L->faces);
    Z_Free(L->nodes);
    Z_Free(L->clipnodes);
    Z_Free(L->leafs);
    Z_Free(L->marks);
    Z_Free(L->models);
    Z_Free(L->miptex);
}

// how many records of `size` the lump holds, or -1 when it is not a whole number
static int lump_count(const q1load_t *L, int lump, int size)
{
    if (L->lumplen[lump] % size)
        return -1;
    return L->lumplen[lump] / size;
}

#define GROW(n, type) Z_Mallocz(sizeof(type) * max((size_t)(n), 1))

static int read_lumps(q1load_t *L)
{
    const bool wide = L->format != Q1F_29;    // 32-bit indices
    int i, n;

    // planes
    if ((n = lump_count(L, Q1_LUMP_PLANES, 20)) < 0) return Q_ERR_ODD_SIZE;
    L->numplanes = n;
    L->planes = GROW(n, q1plane_t);
    for (i = 0; i < n; i++) {
        const byte *p = L->lump[Q1_LUMP_PLANES] + i * 20;
        for (int k = 0; k < 3; k++)
            L->planes[i].normal[k] = rf(p + k * 4);
        L->planes[i].dist = rf(p + 12);
    }

    // vertexes
    if ((n = lump_count(L, Q1_LUMP_VERTEXES, 12)) < 0) return Q_ERR_ODD_SIZE;
    L->numvertexes = n;
    L->vertexes = GROW(n, vec3_t);
    for (i = 0; i < n; i++)
        for (int k = 0; k < 3; k++)
            L->vertexes[i][k] = rf(L->lump[Q1_LUMP_VERTEXES] + i * 12 + k * 4);

    // edges
    if ((n = lump_count(L, Q1_LUMP_EDGES, wide ? 8 : 4)) < 0) return Q_ERR_ODD_SIZE;
    L->numedges = n;
    L->edges = GROW(n, unsigned[2]);
    for (i = 0; i < n; i++) {
        const byte *p = L->lump[Q1_LUMP_EDGES] + i * (wide ? 8 : 4);
        L->edges[i][0] = wide ? rul(p) : (unsigned)rus(p);
        L->edges[i][1] = wide ? rul(p + 4) : (unsigned)rus(p + 2);
        if (L->edges[i][0] >= (unsigned)L->numvertexes || L->edges[i][1] >= (unsigned)L->numvertexes)
            return Q_ERR_BAD_INDEX;
    }

    // surfedges
    if ((n = lump_count(L, Q1_LUMP_SURFEDGES, 4)) < 0) return Q_ERR_ODD_SIZE;
    L->numsurfedges = n;
    L->surfedges = GROW(n, int);
    for (i = 0; i < n; i++) {
        int e = rl(L->lump[Q1_LUMP_SURFEDGES] + i * 4);
        if (abs(e) >= L->numedges)
            return Q_ERR_BAD_INDEX;
        L->surfedges[i] = e;
    }

    // texinfo
    if ((n = lump_count(L, Q1_LUMP_TEXINFO, 40)) < 0) return Q_ERR_ODD_SIZE;
    L->numtexinfo = n;
    L->texinfo = GROW(n, q1texinfo_t);
    for (i = 0; i < n; i++) {
        const byte *p = L->lump[Q1_LUMP_TEXINFO] + i * 40;
        for (int k = 0; k < 8; k++)
            L->texinfo[i].vecs[k >> 2][k & 3] = rf(p + k * 4);
        L->texinfo[i].miptex = rl(p + 32);
        L->texinfo[i].flags = rl(p + 36);
    }

    // faces
    {
        const int size = wide ? 28 : 20;
        if ((n = lump_count(L, Q1_LUMP_FACES, size)) < 0) return Q_ERR_ODD_SIZE;
        L->numfaces = n;
        L->faces = GROW(n, q1face_t);
        for (i = 0; i < n; i++) {
            const byte *p = L->lump[Q1_LUMP_FACES] + i * size;
            q1face_t *f = &L->faces[i];
            if (wide) {
                f->planenum = rl(p);
                f->side = rl(p + 4);
                f->firstedge = rl(p + 8);
                f->numedges = rl(p + 12);
                f->texinfo = rl(p + 16);
                memcpy(f->styles, p + 20, 4);
                f->lightofs = rl(p + 24);
            } else {
                f->planenum = rus(p);
                f->side = rs(p + 2);
                f->firstedge = rl(p + 4);
                f->numedges = rs(p + 8);
                f->texinfo = rs(p + 10);
                memcpy(f->styles, p + 12, 4);
                f->lightofs = rl(p + 16);
            }
            if (f->planenum < 0 || f->planenum >= L->numplanes ||
                f->texinfo < 0 || f->texinfo >= L->numtexinfo ||
                f->numedges < 3 || f->numedges > 4096 || f->firstedge < 0 ||
                (int64_t)f->firstedge + f->numedges > L->numsurfedges)
                return Q_ERR_BAD_INDEX;
        }
    }

    // leafs
    {
        const int size = L->format == Q1F_BSP2 ? 44 : L->format == Q1F_2PSB ? 32 : 28;
        if ((n = lump_count(L, Q1_LUMP_LEAFS, size)) < 1) return Q_ERR_TOO_FEW;
        L->numleafs = n;
        L->leafs = GROW(n, q1leaf_t);
        for (i = 0; i < n; i++) {
            const byte *p = L->lump[Q1_LUMP_LEAFS] + i * size;
            q1leaf_t *l = &L->leafs[i];
            l->contents = rl(p);
            l->visofs = rl(p + 4);
            if (L->format == Q1F_BSP2) {
                for (int k = 0; k < 3; k++) {
                    l->mins[k] = rf(p + 8 + k * 4);
                    l->maxs[k] = rf(p + 20 + k * 4);
                }
                l->firstmark = rul(p + 32);
                l->nummark = rul(p + 36);
            } else {
                for (int k = 0; k < 3; k++) {
                    l->mins[k] = rs(p + 8 + k * 2);
                    l->maxs[k] = rs(p + 14 + k * 2);
                }
                if (L->format == Q1F_2PSB) {
                    l->firstmark = rul(p + 20);
                    l->nummark = rul(p + 24);
                } else {
                    l->firstmark = rus(p + 20);
                    l->nummark = rus(p + 22);
                }
            }
        }
    }

    // nodes
    {
        const int size = L->format == Q1F_BSP2 ? 44 : L->format == Q1F_2PSB ? 32 : 24;
        if ((n = lump_count(L, Q1_LUMP_NODES, size)) < 1) return Q_ERR_TOO_FEW;
        L->numnodes = n;
        L->nodes = GROW(n, q1node_t);
        for (i = 0; i < n; i++) {
            const byte *p = L->lump[Q1_LUMP_NODES] + i * size;
            q1node_t *nd = &L->nodes[i];
            nd->planenum = rl(p);
            if (nd->planenum < 0 || nd->planenum >= L->numplanes)
                return Q_ERR_BAD_INDEX;
            if (L->format == Q1F_29) {
                for (int j = 0; j < 2; j++) {
                    // unsigned, so maps past 32767 nodes still work: what
                    // is not a node counts down from 65535 as a leaf
                    int c = rus(p + 4 + j * 2);
                    nd->children[j] = c < n ? c : -(65535 - c) - 1;
                }
                for (int k = 0; k < 3; k++) {
                    nd->mins[k] = rs(p + 8 + k * 2);
                    nd->maxs[k] = rs(p + 14 + k * 2);
                }
                nd->firstface = rus(p + 20);
                nd->numfaces = rus(p + 22);
            } else {
                nd->children[0] = rl(p + 4);
                nd->children[1] = rl(p + 8);
                if (L->format == Q1F_BSP2) {
                    for (int k = 0; k < 3; k++) {
                        nd->mins[k] = rf(p + 12 + k * 4);
                        nd->maxs[k] = rf(p + 24 + k * 4);
                    }
                    nd->firstface = rul(p + 36);
                    nd->numfaces = rul(p + 40);
                } else {
                    for (int k = 0; k < 3; k++) {
                        nd->mins[k] = rs(p + 12 + k * 2);
                        nd->maxs[k] = rs(p + 18 + k * 2);
                    }
                    nd->firstface = rul(p + 24);
                    nd->numfaces = rul(p + 28);
                }
            }
            for (int j = 0; j < 2; j++) {
                int c = nd->children[j];
                if (c >= n || (c < 0 && -(c + 1) >= L->numleafs))
                    return Q_ERR_BAD_INDEX;
            }
            if ((uint64_t)nd->firstface + nd->numfaces > (uint64_t)L->numfaces)
                return Q_ERR_BAD_INDEX;
        }
    }

    // clipnodes
    {
        const int size = wide ? 12 : 8;
        if ((n = lump_count(L, Q1_LUMP_CLIPNODES, size)) < 0) return Q_ERR_ODD_SIZE;
        L->numclipnodes = n;
        L->clipnodes = GROW(n, mclipnode_t);
        for (i = 0; i < n; i++) {
            const byte *p = L->lump[Q1_LUMP_CLIPNODES] + i * size;
            mclipnode_t *c = &L->clipnodes[i];
            c->planenum = rl(p);
            if (c->planenum < 0 || c->planenum >= L->numplanes)
                return Q_ERR_BAD_INDEX;
            for (int j = 0; j < 2; j++) {
                int v;
                if (wide) {
                    v = rl(p + 4 + j * 4);
                } else {
                    // unsigned as well; contents are the top 15 values
                    v = rus(p + 4 + j * 2);
                    if (v > 0xfff0)
                        v -= 0x10000;
                }
                if (v >= n)
                    return Q_ERR_BAD_INDEX;
                c->children[j] = v;
            }
        }
    }

    // marksurfaces
    if ((n = lump_count(L, Q1_LUMP_MARKSURFACES, wide ? 4 : 2)) < 0) return Q_ERR_ODD_SIZE;
    L->nummarks = n;
    L->marks = GROW(n, unsigned);
    for (i = 0; i < n; i++) {
        const byte *p = L->lump[Q1_LUMP_MARKSURFACES] + i * (wide ? 4 : 2);
        L->marks[i] = wide ? rul(p) : (unsigned)rus(p);
        if (L->marks[i] >= (unsigned)L->numfaces)
            return Q_ERR_BAD_INDEX;
    }
    for (i = 0; i < L->numleafs; i++)
        if ((uint64_t)L->leafs[i].firstmark + L->leafs[i].nummark > (uint64_t)L->nummarks)
            return Q_ERR_BAD_INDEX;

    // models
    if ((n = lump_count(L, Q1_LUMP_MODELS, 64)) < 1) return Q_ERR_TOO_FEW;
    L->nummodels = n;
    L->models = GROW(n, q1model_t);
    for (i = 0; i < n; i++) {
        const byte *p = L->lump[Q1_LUMP_MODELS] + i * 64;
        q1model_t *m = &L->models[i];
        for (int k = 0; k < 3; k++) {
            m->mins[k] = rf(p + k * 4);
            m->maxs[k] = rf(p + 12 + k * 4);
            m->origin[k] = rf(p + 24 + k * 4);
        }
        for (int h = 0; h < Q1_MAX_HULLS; h++)
            m->headnode[h] = rl(p + 36 + h * 4);
        m->visleafs = rl(p + 52);
        m->firstface = rl(p + 56);
        m->numfaces = rl(p + 60);
        if (m->firstface < 0 || m->numfaces < 0 || (int64_t)m->firstface + m->numfaces > L->numfaces)
            return Q_ERR_BAD_INDEX;
        if (m->headnode[0] < 0 || m->headnode[0] >= L->numnodes)
            return Q_ERR_BAD_INDEX;
        for (int h = 1; h < 3; h++)
            if (m->headnode[h] >= L->numclipnodes)
                return Q_ERR_BAD_INDEX;
    }

    return Q_ERR_SUCCESS;
}

/*
===============================================================================

TEXTURES

===============================================================================
*/

// "*water1" -> "#water1", lower case: the texinfo name after "q1/"
static void texture_name(const char *in, char *out, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size && in[i]; i++) {
        char c = Q_tolower(in[i]);
        out[i] = (c == '*') ? '#' : c;
    }
    out[i] = 0;
}

static int read_textures(q1load_t *L)
{
    const byte *lump = L->lump[Q1_LUMP_TEXTURES];
    int len = L->lumplen[Q1_LUMP_TEXTURES];

    if (len < 4)
        return Q_ERR_SUCCESS;   // no textures at all: everything draws as notexture

    int n = rl(lump);
    if (n < 0 || n > 65536 || 4 + (int64_t)n * 4 > len)
        return Q_ERR_BAD_EXTENT;

    L->nummiptex = n;
    L->miptex = GROW(n, mq1miptex_t);
    for (int i = 0; i < n; i++) {
        mq1miptex_t *out = &L->miptex[i];
        int ofs = rl(lump + 4 + i * 4);

        if (ofs < 0 || (int64_t)ofs + 40 > len) {
            out->offset = -1;   // a texture the map lists but does not carry
            continue;
        }

        const byte *mt = lump + ofs;
        char raw[17];
        memcpy(raw, mt, 16);
        raw[16] = 0;
        texture_name(raw, out->name, sizeof(out->name));

        int w = rl(mt + 16), h = rl(mt + 20), pix = rl(mt + 24);
        if (w < 1 || h < 1 || w > 4096 || h > 4096 || pix < 40 ||
            (int64_t)ofs + pix + (int64_t)w * h > len) {
            // a name with no pixels (an external texture in a map built
            // for an engine that loads them): keep the name and the size
            out->width = max(w, 1);
            out->height = max(h, 1);
            out->offset = -1;
            continue;
        }
        out->width = w;
        out->height = h;
        out->offset = ofs + pix;   // into the lump, which is copied whole
    }

    return Q_ERR_SUCCESS;
}

// Quake's animation: "+<frame><name>", frames '0'-'9', alternates 'a'-'j'
// (Mod_LoadTextures folds 'A'-'J' to the same).
// Returns the frame (0-9) and sets *alt; -1 for a texture that does not animate.
static int anim_frame(const char *name, bool *alt)
{
    if (name[0] != '+' || !name[1] || !name[2])
        return -1;
    char c = (char)Q_tolower(name[1]);
    if (c >= '0' && c <= '9') {
        *alt = false;
        return c - '0';
    }
    if (c >= 'a' && c <= 'j') {
        *alt = true;
        return c - 'a';
    }
    return -1;
}

// the miptex index of frame `frame` of the animation `name` belongs to, or -1
static int find_frame(const q1load_t *L, const char *name, bool alt, int frame)
{
    const char want = (char)((alt ? 'a' : '0') + frame);
    for (int i = 0; i < L->nummiptex; i++) {
        const char *n = L->miptex[i].name;
        if (n[0] == '+' && Q_tolower(n[1]) == want && !strcmp(n + 2, name + 2))
            return i;
    }
    return -1;
}

// how many frames in a row, from 0, the animation of miptex `m` has
static int count_frames(const q1load_t *L, int m, bool *alt)
{
    if (m < 0 || m >= L->nummiptex)
        return 1;
    if (anim_frame(L->miptex[m].name, alt) < 0)
        return 1;
    int n = 0;
    while (n < 10 && find_frame(L, L->miptex[m].name, *alt, n) >= 0)
        n++;
    return max(n, 1);
}

/*
The two frame sets of the animation miptex m belongs to: its own ("+0".."+9",
or "+a".."+j" for a face that has an alternate frame) and the other one. Quake
draws the other set on a brush entity whose frame is not 0 (R_TextureAnimation's
alternate_anims): a button held in ("+0basebtn" blinking red -> "+abasebtn"
yellow), a func_wall that was used. Each set's frame 0, -1 if it has none.
*/
typedef struct {
    int first, n;               // the face's own set
    int other_first, other_n;   // the other set, n 0 when there is none
} q1anim_t;

static bool anim_sets(const q1load_t *L, int m, q1anim_t *a)
{
    bool alt, other_alt;
    if (m < 0 || m >= L->nummiptex || anim_frame(L->miptex[m].name, &alt) < 0)
        return false;
    const char *name = L->miptex[m].name;
    a->first = find_frame(L, name, alt, 0);
    a->n = count_frames(L, m, &alt);
    a->other_first = find_frame(L, name, !alt, 0);
    a->other_n = (a->other_first >= 0) ? count_frames(L, a->other_first, &other_alt) : 0;
    return a->first >= 0;
}

// whether a face of miptex m needs rings: an animation, or a frame with an
// alternate to switch to
static bool anim_rings(const q1load_t *L, int m, q1anim_t *a)
{
    return anim_sets(L, m, a) && (a->n > 1 || a->other_first >= 0);
}

/*
===============================================================================

LOAD

===============================================================================
*/

static void *q1_alloc(bsp_t *bsp, size_t size)
{
    void *p = Hunk_Alloc(&bsp->hunk, size);
    if (!p)
        Com_Error(ERR_DROP, "%s: %s needs more memory than was reserved", __func__, bsp->name);
    memset(p, 0, size);
    return p;
}

#define ALLOC(size) q1_alloc(bsp, size)
#define HUNK_SIZE(count, type) ALIGN((size_t)max((count), 1) * sizeof(type), 64)

int BSP_Q1ContentsToQ2(int q1)
{
    switch (q1) {
    case Q1_CONTENTS_EMPTY: return 0;
    case Q1_CONTENTS_WATER: return CONTENTS_WATER;
    case Q1_CONTENTS_SLIME: return CONTENTS_SLIME;
    case Q1_CONTENTS_LAVA:  return CONTENTS_LAVA;
    case Q1_CONTENTS_SOLID:
    case Q1_CONTENTS_SKY:   return CONTENTS_SOLID;
    case -9:  return CONTENTS_WATER | CONTENTS_CURRENT_0;
    case -10: return CONTENTS_WATER | CONTENTS_CURRENT_90;
    case -11: return CONTENTS_WATER | CONTENTS_CURRENT_180;
    case -12: return CONTENTS_WATER | CONTENTS_CURRENT_270;
    case -13: return CONTENTS_WATER | CONTENTS_CURRENT_UP;
    case -14: return CONTENTS_WATER | CONTENTS_CURRENT_DOWN;
    default:  return 0;
    }
}

bool BSP_IsQ1Header(const byte *buf, size_t filelen)
{
    if (filelen < 4 + Q1_HEADER_LUMPS * 8)
        return false;
    int v = rl(buf);
    return v == Q1_BSPVERSION || v == Q1_BSP2_IDENT || v == Q1_2PSB_IDENT;
}

// Run-length decode one Quake row (zero byte, then how many zero bytes)
static void decompress_row(const byte *in, const byte *end, byte *out, int rowbytes)
{
    byte *o = out, *oend = out + rowbytes;
    while (o < oend) {
        if (in >= end) {
            memset(o, 0, oend - o);
            return;
        }
        if (*in) {
            *o++ = *in++;
            continue;
        }
        if (in + 1 >= end) {
            memset(o, 0, oend - o);
            return;
        }
        int c = in[1];
        in += 2;
        while (c-- && o < oend)
            *o++ = 0;
    }
}

// Quake II's run-length coding: a zero byte is followed by how many zero
// bytes it stands for (the same as Quake's, written with a 255 cap)
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

static int popcount64(uint64_t v)
{
    v = v - ((v >> 1) & 0x5555555555555555ULL);
    v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
    v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((v * 0x0101010101010101ULL) >> 56);
}

static int lowest_bit64(uint64_t v)
{
    int n = 0;
    while (!(v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
}

// dvis_t with PVS and PHS rows. Leaf i (1..numvisleafs) is in cluster
// (i - 1) / leafspercluster: one leaf a cluster when the map is small enough,
// runs of neighbouring leaves (the tree numbers them in walking order) when it
// is not, each cluster seeing whatever any of its leaves sees.
static byte *build_vis(const q1load_t *L, int numvisleafs, int leafspercluster, int numclusters, int *outlen)
{
    const byte *vis = L->lump[Q1_LUMP_VISIBILITY];
    const byte *visend = vis + L->lumplen[Q1_LUMP_VISIBILITY];
    const int leafrowsize = (numvisleafs + 7) >> 3;
    const int rowsize = (numclusters + 7) >> 3;
    const size_t longs = (rowsize + sizeof(uint64_t) - 1) / sizeof(uint64_t);
    const byte lastmask = (numclusters & 7) ? (1 << (numclusters & 7)) - 1 : 0xff;

    if (!L->lumplen[Q1_LUMP_VISIBILITY] || numclusters <= 0)
        return NULL;    // not vised: everything sees everything

    uint64_t *pvs = Z_Mallocz(sizeof(uint64_t) * longs * numclusters);
    byte *leafrow = Z_Malloc(leafrowsize + 1);

    for (int i = 0; i < numvisleafs; i++) {
        const int c = i / leafspercluster;
        byte *row = (byte *)(pvs + longs * c);
        int ofs = (i + 1 < L->numleafs) ? L->leafs[i + 1].visofs : -1;

        if (ofs < 0 || ofs >= L->lumplen[Q1_LUMP_VISIBILITY]) {
            memset(row, 0xff, rowsize);     // a leaf without vis sees all
        } else if (leafspercluster == 1) {
            decompress_row(vis + ofs, visend, leafrow, leafrowsize);
            for (int b = 0; b < rowsize; b++)
                row[b] |= leafrow[b];
        } else {
            decompress_row(vis + ofs, visend, leafrow, leafrowsize);
            for (int b = 0; b < leafrowsize; b++) {
                if (!leafrow[b])
                    continue;
                for (int bit = 0; bit < 8; bit++) {
                    if (!(leafrow[b] & (1 << bit)))
                        continue;
                    int j = b * 8 + bit;
                    if (j < numvisleafs)
                        Q_SetBit(row, j / leafspercluster);
                }
            }
        }
        row[rowsize - 1] &= lastmask;   // bits past the last cluster stay clear
        Q_SetBit(row, c);               // a cluster always sees itself
    }
    Z_Free(leafrow);

    // the PHS costs (clusters seen, summed) x (row length); past the limit,
    // every cluster hears every other, which only costs some network traffic
    uint64_t phscost = 0;
    for (size_t k = 0; k < longs * numclusters; k++)
        phscost += popcount64(pvs[k]);
    phscost *= longs;
    const bool phs_ok = phscost <= Q1_MAX_PHS_COST;

    uint64_t *phs = Z_Malloc(sizeof(uint64_t) * longs);
    size_t maxlen = 4 + 8 * (size_t)numclusters + ((size_t)numclusters * 2 + 1) * (rowsize * 2 + 2);
    byte *out = Z_Malloc(maxlen);
    dvis_t *dvis = (dvis_t *)out;
    byte *cursor = out + 4 + 8 * (size_t)numclusters;

    dvis->numclusters = numclusters;

    // the shared "everything" row when there is no PHS
    uint32_t allofs = 0;
    if (!phs_ok) {
        byte *allrow = Z_Malloc(rowsize);
        memset(allrow, 0xff, rowsize);
        allrow[rowsize - 1] = lastmask;
        allofs = (uint32_t)(cursor - out);
        cursor = compress_row(allrow, rowsize, cursor);
        Z_Free(allrow);
    }

    for (int i = 0; i < numclusters; i++) {
        const uint64_t *row = pvs + longs * i;

        dvis->bitofs[i][DVIS_PVS] = (uint32_t)(cursor - out);
        cursor = compress_row((const byte *)row, rowsize, cursor);

        if (!phs_ok) {
            dvis->bitofs[i][DVIS_PHS] = allofs;
            continue;
        }

        // potentially hearable: everything visible from anything visible
        memset(phs, 0, sizeof(uint64_t) * longs);
        for (size_t w = 0; w < longs; w++) {
            uint64_t bits = row[w];
            while (bits) {
                const int j = (int)(w * 64) + lowest_bit64(bits);
                bits &= bits - 1;
                const uint64_t *other = pvs + longs * j;
                for (size_t k = 0; k < longs; k++)
                    phs[k] |= other[k];
            }
        }

        dvis->bitofs[i][DVIS_PHS] = (uint32_t)(cursor - out);
        cursor = compress_row((const byte *)phs, rowsize, cursor);
    }

    Z_Free(pvs);
    Z_Free(phs);

    if (!phs_ok)
        Com_DPrintf("Quake 1 map: %d clusters see too much for a PHS; all hear all\n", numclusters);

    *outlen = (int)(cursor - out);
    return out;
}

#if USE_REF
/*
==============================================================================

WHICH OF A FACE'S LIGHT STYLES ITS LAMP FOLLOWS

light.exe gives a face every style whose light reaches it, and the path tracer's
lamps follow the first one that is not 0 (bsp_mesh.c get_surf_light_style). So a
steady lamp that a flickering light only spills onto flickered with it: e1m1's
big tlight02 fixture by the start is lit 75-83% by its own steady light 24 units
away and 17-26% by the computer cubby's style 10, and it blinked with the cubby
(Matt, 2026-11-21: "it's the same material but only one instance should
blink"). A style other than 0 is kept only where it is the face's own: the one
that puts the most light on it (its lightmaps, one a style), or the style of
the light entity nearest it - a lamp's own light sits just in front of it, and
e1m1's rising lamps get only 40-50% of their light from their style 33-36.
Across the id1 and add-on maps this drops a spilled style from 1064 of 1756
styled lamp faces; on e1m1 the cubby, the rising lamps and their buttons keep
theirs.

==============================================================================
*/

typedef struct {
    vec3_t  origin;
    int     style;      // 0: a steady light
} q1light_t;

// every light entity's origin and style
static q1light_t *q1_lights(const q1load_t *L, int *count)
{
    const int   len = L->lumplen[Q1_LUMP_ENTITIES];
    int         total = 0, n = 0;
    char        *text;
    const char  *data;
    q1light_t   *lights;

    *count = 0;
    if (len <= 0)
        return NULL;

    text = Z_Malloc(len + 1);
    memcpy(text, L->lump[Q1_LUMP_ENTITIES], len);
    text[len] = 0;
    for (data = text; *data; data++)
        if (*data == '{')
            total++;
    lights = Z_Malloc(sizeof(*lights) * max(total, 1));

    data = text;
    while (data && n < total) {
        char        classname[64] = "", key[64];
        const char  *tok = COM_Parse(&data);
        vec3_t      origin;
        bool        has_origin = false;
        int         style = 0;

        if (!data || tok[0] != '{')
            break;
        while (1) {
            Q_strlcpy(key, COM_Parse(&data), sizeof(key));
            if (!data || key[0] == '}')
                break;
            tok = COM_Parse(&data);
            if (!data)
                break;
            if (!strcmp(key, "classname"))
                Q_strlcpy(classname, tok, sizeof(classname));
            else if (!strcmp(key, "origin"))
                has_origin = sscanf(tok, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
            else if (!strcmp(key, "style"))
                style = atoi(tok);
        }
        if (!strncmp(classname, "light", 5) && has_origin) {
            VectorCopy(origin, lights[n].origin);
            lights[n].style = style;
            n++;
        }
    }

    Z_Free(text);
    *count = n;
    return lights;
}

// out's styles, extents and edges are set
static void q1_own_styles(mface_t *out, const q1face_t *in, const q1load_t *L,
                          const q1light_t *lights, int numlights)
{
    const int   size = ((out->extents[0] >> 4) + 1) * ((out->extents[1] >> 4) + 1);
    const byte  *lightdata = L->lump[Q1_LUMP_LIGHTING];
    unsigned    sum[MAX_LIGHTMAPS] = { 0 };
    byte        kept[MAX_LIGHTMAPS];
    int         numstyles = 0, best = 0, nearest = -1, numkept = 0, k, j;
    float       nearest_dist = FLT_MAX;
    vec3_t      mins, maxs;

    for (k = 0; k < MAX_LIGHTMAPS && out->styles[k] != 255; k++)
        numstyles++;

    // nothing to choose between, or nothing to choose by
    if (numstyles < 2 || in->lightofs < 0 || size <= 0 || !lightdata ||
        (int64_t)in->lightofs + (int64_t)numstyles * size > L->lumplen[Q1_LUMP_LIGHTING])
        return;

    for (k = 0; k < numstyles; k++) {
        const byte *p = lightdata + in->lightofs + k * size;
        for (j = 0; j < size; j++)
            sum[k] += p[j];
        if (sum[k] > sum[best])
            best = k;
    }

    ClearBounds(mins, maxs);
    for (k = 0; k < out->numsurfedges; k++)
        AddPointToBounds(out->firstsurfedge[k].edge->v[out->firstsurfedge[k].vert]->point, mins, maxs);
    for (k = 0; k < numlights; k++) {
        float d = 0;
        for (j = 0; j < 3; j++) {
            float e = max(max(mins[j] - lights[k].origin[j], lights[k].origin[j] - maxs[j]), 0.0f);
            d += e * e;
        }
        if (d < nearest_dist) {
            nearest_dist = d;
            nearest = lights[k].style;
        }
    }

    for (k = 0; k < numstyles; k++)
        if (out->styles[k] == 0 || k == best || out->styles[k] == nearest)
            kept[numkept++] = out->styles[k];
    for (k = 0; k < MAX_LIGHTMAPS; k++)
        out->styles[k] = k < numkept ? kept[k] : 255;
}
#endif

int BSP_LoadQ1(bsp_t *bsp, const byte *buf, size_t filelen)
{
    q1load_t L;
    int ret;

    memset(&L, 0, sizeof(L));
    if (!BSP_IsQ1Header(buf, filelen))
        return Q_ERR_UNKNOWN_FORMAT;

    L.buf = buf;
    L.filelen = filelen;
    L.version = rl(buf);
    L.format = L.version == Q1_BSP2_IDENT ? Q1F_BSP2 : L.version == Q1_2PSB_IDENT ? Q1F_2PSB : Q1F_29;

    for (int i = 0; i < Q1_HEADER_LUMPS; i++) {
        uint32_t ofs = rul(buf + 4 + i * 8);
        uint32_t len = rul(buf + 8 + i * 8);
        if ((uint64_t)ofs + len > filelen)
            return Q_ERR_BAD_EXTENT;
        L.lump[i] = buf + ofs;
        L.lumplen[i] = (int)len;
    }

    if ((ret = read_lumps(&L)) != 0)
        goto fail;
    if ((ret = read_textures(&L)) != 0)
        goto fail;

    //
    // what has to be counted before the hunk is sized
    //

    // a copy of the solid leaf for every node child pointing at leaf 0
    int numsolidrefs = 0;
    for (int i = 0; i < L.numnodes; i++)
        for (int j = 0; j < 2; j++)
            if (L.nodes[i].children[j] == -1)
                numsolidrefs++;
    const int numleafs = L.numleafs + numsolidrefs;

    // animation rings: one texinfo per frame of every animation a face uses,
    // and of its other set when it has one (anim_sets) - the alternates are
    // in the texture lump but no face's texinfo names them
    int numringtexinfo = 0;
    bool *ring_used = GROW(L.nummiptex, bool);
    for (int i = 0; i < L.numtexinfo; i++) {
        q1anim_t a;
        if (!anim_rings(&L, L.texinfo[i].miptex, &a))
            continue;
        if (!ring_used[a.first]) {
            ring_used[a.first] = true;
            numringtexinfo += a.n;
        }
        if (a.other_first >= 0 && !ring_used[a.other_first]) {
            ring_used[a.other_first] = true;
            numringtexinfo += a.other_n;
        }
    }
    const int numtexinfo = max(L.numtexinfo, 1) + numringtexinfo;

    int numvisleafs = L.models[0].visleafs;
    if (numvisleafs < 0 || numvisleafs >= L.numleafs)
        numvisleafs = L.numleafs - 1;
    const int leafspercluster = max((numvisleafs + Q1_MAX_CLUSTERS - 1) / Q1_MAX_CLUSTERS, 1);
    const int numclusters = (numvisleafs + leafspercluster - 1) / leafspercluster;

    int vislen = 0;
    byte *vis = build_vis(&L, numvisleafs, leafspercluster, numclusters, &vislen);

    const int numclip = L.numclipnodes + L.numnodes;
    const int texlen = L.lumplen[Q1_LUMP_TEXTURES];
    const int entlen = L.lumplen[Q1_LUMP_ENTITIES];

    size_t memsize = 4096 * 4;
    memsize += HUNK_SIZE(1, q1bsp_t);
    memsize += HUNK_SIZE(L.numplanes, cplane_t);
    memsize += HUNK_SIZE(numtexinfo, mtexinfo_t);
    memsize += HUNK_SIZE(numleafs, mleaf_t);
    memsize += HUNK_SIZE(numleafs, signed char);
    memsize += HUNK_SIZE(L.numnodes, mnode_t);
    memsize += HUNK_SIZE(L.numnodes, int);
    memsize += HUNK_SIZE(L.nummodels, mmodel_t);
    memsize += HUNK_SIZE(L.nummodels, mhull_t[Q1_MAX_HULLS]);
    memsize += HUNK_SIZE(numclip, mclipnode_t);
    memsize += HUNK_SIZE(2, marea_t);
    memsize += HUNK_SIZE(L.nummiptex, mq1miptex_t);
    memsize += ALIGN((size_t)texlen + 1, 64);
    memsize += ALIGN((size_t)vislen + 1, 64);
    memsize += ALIGN((size_t)entlen + 1, 64);
    memsize += HUNK_SIZE(1, mbrush_t *);
#if USE_REF
    memsize += HUNK_SIZE(L.numvertexes, mvertex_t);
    memsize += HUNK_SIZE(L.numedges, medge_t);
    memsize += HUNK_SIZE(L.numsurfedges, msurfedge_t);
    memsize += HUNK_SIZE(L.numfaces, mface_t);
    memsize += HUNK_SIZE(L.nummarks, mface_t *);
    memsize += HUNK_SIZE(1, mfogvolume_t);
#endif

    Hunk_Begin(&bsp->hunk, memsize);
    bsp->extended = true;
    bsp->q1 = true;

    q1bsp_t *q = bsp->q1d = ALLOC(sizeof(q1bsp_t));
    q->version = L.version;
    q->numvisleafs = numvisleafs;
    q->leafspercluster = leafspercluster;

    //
    // planes
    //
    bsp->numplanes = L.numplanes;
    bsp->planes = ALLOC(sizeof(cplane_t) * max(L.numplanes, 1));
    for (int i = 0; i < L.numplanes; i++) {
        cplane_t *out = &bsp->planes[i];
        VectorCopy(L.planes[i].normal, out->normal);
        out->dist = L.planes[i].dist;
        SetPlaneType(out);
        SetPlaneSignbits(out);
    }

    //
    // textures, kept whole for the image loader
    //
    q->nummiptex = L.nummiptex;
    q->miptex = ALLOC(sizeof(mq1miptex_t) * max(L.nummiptex, 1));
    if (L.nummiptex)
        memcpy(q->miptex, L.miptex, sizeof(mq1miptex_t) * L.nummiptex);
    q->texdatalen = texlen;
    q->texdata = ALLOC(texlen + 1);
    if (texlen)
        memcpy(q->texdata, L.lump[Q1_LUMP_TEXTURES], texlen);

    //
    // texinfo
    //
    bsp->numtexinfo = numtexinfo;
    bsp->texinfo = ALLOC(sizeof(mtexinfo_t) * numtexinfo);

    // name and surface flags of a texinfo drawing miptex m
    #define SET_TEXTURE(out, m) do { \
        const mq1miptex_t *mt_ = ((m) >= 0 && (m) < L.nummiptex) ? &L.miptex[(m)] : NULL; \
        const char *tn_ = (mt_ && mt_->name[0]) ? mt_->name : "notexture"; \
        Q_concat((out)->name, sizeof((out)->name), "q1/", tn_); \
        Q_strlcpy((out)->c.name, tn_, sizeof((out)->c.name)); \
        (out)->c.flags = 0; \
        if (!Q_strncasecmp(tn_, "sky", 3)) \
            (out)->c.flags |= SURF_SKY; \
        else if (tn_[0] == '#') { \
            (out)->c.flags |= SURF_WARP; \
            if (!Q_strncasecmp(tn_ + 1, "water", 5) || !Q_strncasecmp(tn_ + 1, "slime", 5) || \
                (strstr(tn_, "water") && !strstr(tn_, "lava"))) \
                (out)->c.flags |= SURF_TRANS33; \
        } \
    } while (0)

    for (int i = 0; i < L.numtexinfo; i++) {
        mtexinfo_t *out = &bsp->texinfo[i];
        const q1texinfo_t *in = &L.texinfo[i];

        SET_TEXTURE(out, in->miptex);
        out->c.value = 0;
#if USE_REF
        out->radiance = 0;
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 3; k++)
                out->axis[j][k] = in->vecs[j][k];
            out->offset[j] = in->vecs[j][3];
        }
        out->numframes = 1;
        out->next = NULL;
        out->alt = NULL;
#endif
    }
    if (!L.numtexinfo) {
        Q_strlcpy(bsp->texinfo[0].name, "q1/notexture", sizeof(bsp->texinfo[0].name));
#if USE_REF
        bsp->texinfo[0].numframes = 1;
#endif
    }

#if USE_REF
    // the animation rings: frame k's texinfo leads to frame k+1's; a face's own
    // texinfo leads into its texture's ring one frame on. Every frame of a set
    // with an other set (anim_sets) points at that set's frame 0, as Quake's
    // alternate_anims do; a one-frame set gets a ring of one for that.
    {
        int *ring_start = GROW(L.nummiptex, int);  // miptex of frame 0 -> first ring texinfo
        int next_ring = max(L.numtexinfo, 1);

        for (int i = 0; i < L.nummiptex; i++)
            ring_start[i] = -1;

        for (int i = 0; i < L.numtexinfo; i++) {
            int m = L.texinfo[i].miptex;
            q1anim_t a;
            if (!anim_rings(&L, m, &a))
                continue;

            const int firsts[2] = { a.first, a.other_first };
            const int counts[2] = { a.n, a.other_n };
            for (int s = 0; s < 2; s++) {
                if (firsts[s] < 0 || ring_start[firsts[s]] >= 0)
                    continue;
                const char *fname = L.miptex[firsts[s]].name;
                bool salt = false;
                anim_frame(fname, &salt);
                const int n = counts[s];
                ring_start[firsts[s]] = next_ring;
                for (int k = 0; k < n; k++) {
                    mtexinfo_t *r = &bsp->texinfo[next_ring + k];
                    SET_TEXTURE(r, find_frame(&L, fname, salt, k));
                    r->numframes = n;
                    r->next = (n > 1) ? &bsp->texinfo[next_ring + (k + 1) % n] : NULL;
                    r->alt = NULL;
                }
                next_ring += n;
            }

            mtexinfo_t *own = &bsp->texinfo[ring_start[a.first]];
            mtexinfo_t *other = (a.other_first >= 0) ? &bsp->texinfo[ring_start[a.other_first]] : NULL;
            if (other) {
                for (int k = 0; k < a.n; k++)
                    own[k].alt = other;
                for (int k = 0; k < a.other_n; k++)
                    other[k].alt = own;
            }

            bool dummy;
            int frame = anim_frame(L.miptex[m].name, &dummy);
            if (frame < 0 || frame >= a.n)
                frame = 0;
            bsp->texinfo[i].numframes = a.n;
            bsp->texinfo[i].next = (a.n > 1) ? &own[(frame + 1) % a.n] : NULL;
            bsp->texinfo[i].alt = other;
        }
        Z_Free(ring_start);
    }
#endif
    #undef SET_TEXTURE
    Z_Free(ring_used);

    //
    // leafs, then a solid copy per reference to leaf 0
    //
    bsp->numleafbrushes = 0;
    bsp->leafbrushes = ALLOC(sizeof(mbrush_t *));
    bsp->numleafs = numleafs;
    bsp->leafs = ALLOC(sizeof(mleaf_t) * numleafs);
    q->leafcontents = ALLOC(numleafs);
#if USE_REF
    bsp->numleaffaces = L.nummarks;
    bsp->leaffaces = ALLOC(sizeof(mface_t *) * max(L.nummarks, 1));
#endif

    for (int i = 0; i < numleafs; i++) {
        mleaf_t *out = &bsp->leafs[i];
        const q1leaf_t *in = (i < L.numleafs) ? &L.leafs[i] : &L.leafs[0];
        int contents = (i < L.numleafs && i > 0) ? in->contents : Q1_CONTENTS_SOLID;

        q->leafcontents[i] = (signed char)contents;
        out->contents = BSP_Q1ContentsToQ2(contents);
        out->firstleafbrush = bsp->leafbrushes;
        out->numleafbrushes = 0;
        out->brushcontents = 0;
        out->pointtest = false;

        if (i > 0 && i < L.numleafs && i - 1 < numvisleafs && contents != Q1_CONTENTS_SOLID) {
            out->cluster = vis ? (i - 1) / leafspercluster : 0;
            out->area = 1;
        } else if (i > 0 && i < L.numleafs && contents != Q1_CONTENTS_SOLID) {
            // a leaf of an inline model: no cluster, but not solid either
            out->cluster = -1;
            out->area = 0;
        } else {
            out->cluster = -1;
            out->area = 0;
        }

#if USE_REF
        VectorCopy(in->mins, out->mins);
        VectorCopy(in->maxs, out->maxs);
        out->visframe = -1;
        if (i > 0 && i < L.numleafs) {
            out->firstleafface = bsp->leaffaces + in->firstmark;
            out->numleaffaces = in->nummark;
        }
#endif
    }

    //
    // nodes
    //
    bsp->numnodes = L.numnodes;
    bsp->nodes = ALLOC(sizeof(mnode_t) * L.numnodes);
    {
        int nextsolid = L.numleafs;
        for (int i = 0; i < L.numnodes; i++) {
            const q1node_t *in = &L.nodes[i];
            mnode_t *out = &bsp->nodes[i];

            out->plane = bsp->planes + in->planenum;
            for (int j = 0; j < 2; j++) {
                int c = in->children[j];
                if (c >= 0) {
                    out->children[j] = bsp->nodes + c;
                } else {
                    int leaf = -(c + 1);
                    if (leaf == 0) {
                        mleaf_t *copy = &bsp->leafs[nextsolid++];
#if USE_REF
                        // the node's own box: the shared leaf's says nothing
                        VectorCopy(in->mins, copy->mins);
                        VectorCopy(in->maxs, copy->maxs);
#endif
                        out->children[j] = (mnode_t *)copy;
                    } else {
                        out->children[j] = (mnode_t *)(bsp->leafs + leaf);
                    }
                }
            }

#if USE_REF
            VectorCopy(in->mins, out->mins);
            VectorCopy(in->maxs, out->maxs);
            out->visframe = -1;
            out->numfaces = in->numfaces;
            out->firstface = NULL;  // set once the faces exist
#endif
        }
    }

    //
    // vis
    //
    if (vis) {
        bsp->numvisibility = vislen;
        bsp->vis = ALLOC(vislen + 1);
        memcpy(bsp->vis, vis, vislen);
        bsp->visrowsize = (numclusters + 7) >> 3;
        Z_Free(vis);
    }

    //
    // areas: one open area, area 0 for solid
    //
    bsp->numareas = 2;
    bsp->areas = ALLOC(sizeof(marea_t) * 2);
    bsp->numareaportals = 0;
    bsp->areaportals = NULL;
    bsp->lastareaportal = 0;

#if USE_REF
    //
    // vertices, edges, faces
    //
    bsp->numvertices = L.numvertexes;
    bsp->vertices = ALLOC(sizeof(mvertex_t) * max(L.numvertexes, 1));
    for (int i = 0; i < L.numvertexes; i++)
        VectorCopy(L.vertexes[i], bsp->vertices[i].point);

    bsp->numedges = L.numedges;
    bsp->edges = ALLOC(sizeof(medge_t) * max(L.numedges, 1));
    for (int i = 0; i < L.numedges; i++) {
        bsp->edges[i].v[0] = bsp->vertices + L.edges[i][0];
        bsp->edges[i].v[1] = bsp->vertices + L.edges[i][1];
    }

    bsp->numsurfedges = L.numsurfedges;
    bsp->surfedges = ALLOC(sizeof(msurfedge_t) * max(L.numsurfedges, 1));
    for (int i = 0; i < L.numsurfedges; i++) {
        int e = L.surfedges[i];
        bsp->surfedges[i].edge = bsp->edges + abs(e);
        bsp->surfedges[i].vert = e < 0;
    }

    bsp->numfaces = L.numfaces;
    bsp->faces = ALLOC(sizeof(mface_t) * max(L.numfaces, 1));
    int numlights;
    q1light_t *lights = q1_lights(&L, &numlights);     // q1_own_styles
    for (int i = 0; i < L.numfaces; i++) {
        const q1face_t *in = &L.faces[i];
        mface_t *out = &bsp->faces[i];

        out->firstsurfedge = bsp->surfedges + in->firstedge;
        out->numsurfedges = in->numedges;
        out->plane = bsp->planes + in->planenum;
        out->drawflags = in->side ? DSURF_PLANEBACK : 0;
        out->texinfo = bsp->texinfo + in->texinfo;
        out->lightmap = NULL;
        /* No lightmap, so numstyles stays 0 (the GL and software renderers walk
           that many lightmaps) - but the STYLES are kept: they are what the path
           tracer's lamps follow (bsp_mesh.c get_surf_light_style), a lamp face
           carrying its light's style. Without them every Quake lamp glowed and
           cast light from the start, e1m1's rising lamps (style 33-36, START_OFF
           until the trigger) and the buttons beside them included (Matt,
           2026-11-21), and no fluorescent flickered. */
        out->numstyles = 0;
        for (int k = 0; k < MAX_LIGHTMAPS; k++)
            out->styles[k] = 255;
        for (int k = 0; k < MAX_LIGHTMAPS && in->styles[k] != 255; k++)
            out->styles[k] = in->styles[k];
        out->firstbasis = 0;

        // texture extents, as Quake's CalcSurfaceExtents
        float mins[2] = { FLT_MAX, FLT_MAX }, maxs[2] = { -FLT_MAX, -FLT_MAX };
        const q1texinfo_t *tex = &L.texinfo[in->texinfo];
        for (int k = 0; k < in->numedges; k++) {
            int e = L.surfedges[in->firstedge + k];
            const float *v = L.vertexes[e >= 0 ? L.edges[e][0] : L.edges[-e][1]];
            for (int j = 0; j < 2; j++) {
                double val = (double)v[0] * tex->vecs[j][0] + (double)v[1] * tex->vecs[j][1] +
                             (double)v[2] * tex->vecs[j][2] + tex->vecs[j][3];
                if (val < mins[j]) mins[j] = (float)val;
                if (val > maxs[j]) maxs[j] = (float)val;
            }
        }
        for (int j = 0; j < 2; j++) {
            int bmin = (int)floorf(mins[j] / 16);
            int bmax = (int)ceilf(maxs[j] / 16);
            out->texturemins[j] = bmin * 16;
            out->extents[j] = (bmax - bmin) * 16;
        }

        // only the styles that are the face's own (see q1_own_styles)
        q1_own_styles(out, in, &L, lights, numlights);
    }
    Z_Free(lights);

    for (int i = 0; i < L.nummarks; i++)
        bsp->leaffaces[i] = bsp->faces + L.marks[i];

    for (int i = 0; i < L.numnodes; i++)
        bsp->nodes[i].firstface = bsp->faces + L.nodes[i].firstface;

    bsp->numfogvolumes = 0;
    bsp->fogvolumes = ALLOC(sizeof(mfogvolume_t));
#endif

    //
    // clip hulls: the map's clipnodes, then hull 0 made from the draw nodes
    // with each leaf child turned into its contents (Quake's Mod_MakeHull0)
    //
    q->numclipnodes = numclip;
    q->clipnodes = ALLOC(sizeof(mclipnode_t) * max(numclip, 1));
    if (L.numclipnodes)
        memcpy(q->clipnodes, L.clipnodes, sizeof(mclipnode_t) * L.numclipnodes);
    q->hull0 = L.numclipnodes;
    for (int i = 0; i < L.numnodes; i++) {
        mclipnode_t *out = &q->clipnodes[q->hull0 + i];
        out->planenum = L.nodes[i].planenum;
        for (int j = 0; j < 2; j++) {
            int c = L.nodes[i].children[j];
            if (c >= 0) {
                out->children[j] = q->hull0 + c;
            } else {
                int leaf = -(c + 1);
                out->children[j] = leaf ? L.leafs[leaf].contents : Q1_CONTENTS_SOLID;
                // anything the trace cannot read as contents is empty
                if (out->children[j] >= 0)
                    out->children[j] = Q1_CONTENTS_EMPTY;
            }
        }
    }

    //
    // models and their hulls
    //
    bsp->nummodels = L.nummodels;
    bsp->models = ALLOC(sizeof(mmodel_t) * L.nummodels);
    q->hulls = ALLOC(sizeof(mhull_t[Q1_MAX_HULLS]) * L.nummodels);
    q->node_model = ALLOC(sizeof(int) * max(L.numnodes, 1));
    for (int i = 0; i < L.numnodes; i++)
        q->node_model[i] = -1;

    static const vec3_t hull_mins[3] = { { 0, 0, 0 }, { -16, -16, -24 }, { -32, -32, -24 } };
    static const vec3_t hull_maxs[3] = { { 0, 0, 0 }, {  16,  16,  32 }, {  32,  32,  64 } };

    for (int m = 0; m < L.nummodels; m++) {
        const q1model_t *in = &L.models[m];
        mmodel_t *out = &bsp->models[m];

        for (int k = 0; k < 3; k++) {
            // spread the box by a pixel, as Quake does
            out->mins[k] = in->mins[k] - 1;
            out->maxs[k] = in->maxs[k] + 1;
            out->origin[k] = in->origin[k];
        }
        out->headnode = bsp->nodes + in->headnode[0];
        if (q->node_model[in->headnode[0]] < 0)
            q->node_model[in->headnode[0]] = m;

        mhull_t *h = q->hulls[m];
        h[0].clipnodes = q->clipnodes;
        h[0].firstclipnode = q->hull0 + in->headnode[0];
        h[0].lastclipnode = q->hull0 + L.numnodes - 1;
        for (int k = 1; k < 3; k++) {
            h[k].clipnodes = q->clipnodes;
            h[k].firstclipnode = in->headnode[k];
            h[k].lastclipnode = L.numclipnodes - 1;
            VectorCopy(hull_mins[k], h[k].clip_mins);
            VectorCopy(hull_maxs[k], h[k].clip_maxs);
        }
        // hull 3 is not used by Quake; point it at hull 1
        h[3] = h[1];

#if USE_REF
        out->radius = RadiusFromBounds(out->mins, out->maxs);
        // Model 0 is the world, and like Quake II's loader (bsp.c
        // BSP_LoadSubmodels) it owns no faces: the renderers count a face that
        // is some model's as that brush entity's, drawn only where an entity
        // uses the model. Listing the world's here took every world face out
        // of the path tracer's world mesh - Quake 1 maps drew nothing but
        // their entities against the sky (q1/dm3, 2026-11-20).
        if (m > 0) {
            out->firstface = bsp->faces + in->firstface;
            out->numfaces = in->numfaces;
        }
#endif
    }

    //
    // entities
    //
    bsp->numentitychars = entlen;
    bsp->entitystring = ALLOC(entlen + 1);
    if (entlen)
        memcpy(bsp->entitystring, L.lump[Q1_LUMP_ENTITIES], entlen);
    bsp->entitystring[entlen] = 0;

    Com_DPrintf("%s: Quake 1 map (%s), %d textures, %d nodes, %d leafs + %d solid, %d clusters (%d leafs each), %d clipnodes, %d faces, %d models\n",
                bsp->name, L.format == Q1F_BSP2 ? "BSP2" : L.format == Q1F_2PSB ? "2PSB" : "BSP29",
                L.nummiptex, L.numnodes, L.numleafs, numsolidrefs, numclusters, leafspercluster,
                L.numclipnodes, L.numfaces, L.nummodels);

    free_load(&L);
    return Q_ERR_SUCCESS;

fail:
    free_load(&L);
    return ret;
}

/*
===============================================================================

TEXTURES FOR THE IMAGE LOADER

===============================================================================
*/

const byte *BSP_Q1MiptexFrom(const bsp_t *bsp, const char *name, int *width, int *height)
{
    const q1bsp_t *q = bsp->q1d;
    if (!q)
        return NULL;
    for (int i = 0; i < q->nummiptex; i++) {
        const mq1miptex_t *mt = &q->miptex[i];
        if (mt->offset < 0 || Q_stricmp(mt->name, name))
            continue;
        if ((int64_t)mt->offset + (int64_t)mt->width * mt->height > q->texdatalen)
            return NULL;
        *width = mt->width;
        *height = mt->height;
        return q->texdata + mt->offset;
    }
    return NULL;
}

/*
===============================================================================

HULL TRACES

Quake's SV_RecursiveHullCheck in the form later Quake ports use (it never
backs the impact point up through solid). "Solid" is whatever the trace's
mask stops at: a hull leaf's Quake contents turned into Quake II flags.

===============================================================================
*/

// 1/32 epsilon to keep floating point happy
#define Q1_DIST_EPSILON     0.03125f

enum { RHT_SOLID, RHT_EMPTY, RHT_IMPACT };

typedef struct {
    const mclipnode_t   *clipnodes;
    const cplane_t      *planes;
    vec3_t              start, end;
    int                 mask;
    trace_t             *trace;
    int                 hitcontents;    // Quake 1 contents of what stopped it
} q1trace_t;

static inline bool q1_blocks(const q1trace_t *t, int contents)
{
    return (BSP_Q1ContentsToQ2(contents) & t->mask) != 0;
}

static int q1_hull_contents(const mclipnode_t *clipnodes, const cplane_t *planes, int num, const vec3_t p)
{
    while (num >= 0) {
        const mclipnode_t *node = clipnodes + num;
        const cplane_t *plane = planes + node->planenum;
        float d = (plane->type < 3) ? p[plane->type] - plane->dist
                                    : DotProduct(plane->normal, p) - plane->dist;
        num = node->children[d < 0];
    }
    return num;
}

static int q1_recursive_trace(q1trace_t *t, int num, float p1f, float p2f, const vec3_t p1, const vec3_t p2)
{
    const mclipnode_t *node;
    const cplane_t *plane;
    float t1, t2, midf;
    vec3_t mid;
    int side, rht;

reenter:
    if (num < 0) {
        if (q1_blocks(t, num)) {
            if (t->trace->allsolid)
                t->trace->startsolid = true;
            t->hitcontents = num;
            return RHT_SOLID;
        }
        t->trace->allsolid = false;
        return RHT_EMPTY;
    }

    node = t->clipnodes + num;
    plane = t->planes + node->planenum;

    if (plane->type < 3) {
        t1 = p1[plane->type] - plane->dist;
        t2 = p2[plane->type] - plane->dist;
    } else {
        t1 = DotProduct(plane->normal, p1) - plane->dist;
        t2 = DotProduct(plane->normal, p2) - plane->dist;
    }

    // all on one side: carry on down that side
    if (t1 >= 0 && t2 >= 0) {
        num = node->children[0];
        goto reenter;
    }
    if (t1 < 0 && t2 < 0) {
        num = node->children[1];
        goto reenter;
    }

    // where the whole trace crosses this plane, measured on the whole line so
    // the fractions stay exact however deep the split
    if (plane->type < 3) {
        t1 = t->start[plane->type] - plane->dist;
        t2 = t->end[plane->type] - plane->dist;
    } else {
        t1 = DotProduct(plane->normal, t->start) - plane->dist;
        t2 = DotProduct(plane->normal, t->end) - plane->dist;
    }

    side = t1 < 0;
    midf = (t1 == t2) ? p1f : t1 / (t1 - t2);
    if (midf < p1f) midf = p1f;
    if (midf > p2f) midf = p2f;
    LerpVector(t->start, t->end, midf, mid);

    rht = q1_recursive_trace(t, node->children[side], p1f, midf, p1, mid);
    if (rht != RHT_EMPTY && !t->trace->allsolid)
        return rht;
    rht = q1_recursive_trace(t, node->children[side ^ 1], midf, p2f, mid, p2);
    if (rht != RHT_SOLID)
        return rht;

    // the near side is open and the far side stops us: this is the impact
    if (side) {
        t->trace->plane.dist = -plane->dist;
        VectorNegate(plane->normal, t->trace->plane.normal);
    } else {
        t->trace->plane.dist = plane->dist;
        VectorCopy(plane->normal, t->trace->plane.normal);
    }

    t1 = DotProduct(t->trace->plane.normal, t->start) - t->trace->plane.dist;
    t2 = DotProduct(t->trace->plane.normal, t->end) - t->trace->plane.dist;
    midf = (t1 == t2) ? 0 : (t1 - Q1_DIST_EPSILON) / (t1 - t2);
    clamp(midf, 0, 1);
    t->trace->fraction = midf;
    LerpVector(t->start, t->end, midf, t->trace->endpos);

    return RHT_IMPACT;
}

// what a trace stopped by Quake contents `c` reports as its surface
static csurface_t *q1_surface_for(int c)
{
    static csurface_t sky = { "sky", SURF_SKY, 0 };
    static csurface_t water = { "water", SURF_WARP, 0 };
    static csurface_t slime = { "slime", SURF_WARP, 0 };
    static csurface_t lava = { "lava", SURF_WARP, 0 };

    switch (c) {
    case Q1_CONTENTS_SKY:   return &sky;
    case Q1_CONTENTS_WATER: return &water;
    case Q1_CONTENTS_SLIME: return &slime;
    case Q1_CONTENTS_LAVA:  return &lava;
    default:                return &nulltexinfo.c;
    }
}

/*
==================
BSP_Q1BoxTrace

The hull for the box is Quake's choice: a point (narrower than 3 units) in
hull 0, up to 32 wide in hull 1, wider in hull 2. The box is lined up with the
hull's own by its bottom corner, as Quake's SV_HullForEntity does, so a box
shorter than the hull's touches ceilings early, just as it would in Quake.

A mask with nothing solid in it (a test for water, say) can only be answered
by hull 0, whose leaves still know their liquids; the box's centre goes
through it there.
==================
*/
void BSP_Q1BoxTrace(trace_t *trace, bsp_t *bsp, int model,
                    const vec3_t start, const vec3_t end,
                    const vec3_t mins, const vec3_t maxs, int brushmask)
{
    const q1bsp_t *q = bsp->q1d;
    const mhull_t *hull;
    vec3_t offset, s, e;
    q1trace_t t;

    memset(trace, 0, sizeof(*trace));
    trace->fraction = 1;
    trace->allsolid = true;
    trace->surface = &nulltexinfo.c;

    if (model < 0 || model >= bsp->nummodels) {
        trace->allsolid = false;
        VectorCopy(end, trace->endpos);
        return;
    }

    const float width = maxs[0] - mins[0];
    const bool solidmask = (brushmask & (CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP)) != 0;

    if (width < 3 || !solidmask) {
        hull = &q->hulls[model][0];
        if (width < 3)
            VectorClear(offset);
        else
            VectorAvg(mins, maxs, offset);  // the box's centre, through hull 0
        VectorNegate(offset, offset);
    } else {
        hull = &q->hulls[model][width <= 32 ? 1 : 2];
        // the hull's box bottom corner on the trace box's
        VectorSubtract(hull->clip_mins, mins, offset);
    }

    VectorSubtract(start, offset, s);
    VectorSubtract(end, offset, e);

    memset(&t, 0, sizeof(t));
    t.clipnodes = hull->clipnodes;
    t.planes = bsp->planes;
    t.mask = brushmask;
    t.trace = trace;
    t.hitcontents = 0;
    // hull 1/2 trees know only solid and empty: let solid in them stop any
    // mask that has something solid in it
    if (hull != &q->hulls[model][0])
        t.mask |= CONTENTS_SOLID;
    VectorCopy(s, t.start);
    VectorCopy(e, t.end);

    if (VectorCompare(s, e)) {
        // a position test: a point cannot cross a plane
        int c = q1_hull_contents(hull->clipnodes, bsp->planes, hull->firstclipnode, s);
        if (q1_blocks(&t, c)) {
            trace->startsolid = true;
            trace->fraction = 0;
            trace->contents = BSP_Q1ContentsToQ2(c);
            trace->surface = q1_surface_for(c);
        } else {
            trace->allsolid = false;
        }
        VectorCopy(start, trace->endpos);
        return;
    }

    q1_recursive_trace(&t, hull->firstclipnode, 0, 1, s, e);

    if (trace->allsolid) {
        // stuck: Quake II's traces say so with no movement at all
        trace->startsolid = true;
        trace->fraction = 0;
        trace->contents = BSP_Q1ContentsToQ2(t.hitcontents ? t.hitcontents : Q1_CONTENTS_SOLID);
        trace->surface = q1_surface_for(t.hitcontents);
        VectorCopy(start, trace->endpos);
        return;
    }

    if (trace->fraction == 1) {
        VectorCopy(end, trace->endpos);
    } else {
        // what stopped it: in hull 0 the leaf behind the impact
        int c = t.hitcontents;
        if (hull == &q->hulls[model][0]) {
            vec3_t behind;
            VectorMA(trace->endpos, -2 * Q1_DIST_EPSILON, trace->plane.normal, behind);
            int cb = q1_hull_contents(hull->clipnodes, bsp->planes, hull->firstclipnode, behind);
            if (q1_blocks(&t, cb))
                c = cb;
        }
        trace->contents = BSP_Q1ContentsToQ2(c ? c : Q1_CONTENTS_SOLID);
        trace->surface = q1_surface_for(c);
        VectorAdd(trace->endpos, offset, trace->endpos);
    }
}

int BSP_Q1PointContents(bsp_t *bsp, const vec3_t p)
{
    const q1bsp_t *q = bsp->q1d;
    if (!q || !bsp->nummodels)
        return Q1_CONTENTS_EMPTY;
    return q1_hull_contents(q->clipnodes, bsp->planes, q->hulls[0][0].firstclipnode, p);
}
