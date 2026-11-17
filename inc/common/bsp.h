/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2008 Andrey Nazarov
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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

#ifndef BSP_H
#define BSP_H

#include "shared/list.h"
#include "common/error.h"
#include "system/hunk.h"
#include "format/bsp.h"

#if REF_SOFT
// mip levels the software renderer keeps of every wall texture and surface
#define MIPLEVELS   4
#endif

// maximum size of a PVS row, in bytes
#define VIS_MAX_BYTES   (MAX_MAP_LEAFS >> 3)

// take advantage of 64-bit systems
#define VIS_FAST_LONGS(bsp) \
    (((bsp)->visrowsize + sizeof(size_t) - 1) / sizeof(size_t))

typedef struct mtexinfo_s {  // used internally due to name len probs //ZOID
    csurface_t          c;
    // MAX_QPATH rather than the on-disk MAX_TEXNAME: a Quake III map names its
    // surfaces by shader path ("base_wall/c_met5_2" with "textures/" taken
    // off, or "../models/mapobjects/..."), and those run past 32 characters.
    char                name[MAX_QPATH];

#if USE_REF
    int                 radiance;
    vec3_t              axis[2];
    vec2_t              offset;
#if REF_VKPT
	struct pbr_material_s *material;
#endif
#if REF_GL || REF_SOFT
	struct image_s      *image; // used for texturing
#endif
#if REF_SOFT
    vec_t               mipadjust;  // texels per world unit, for mip selection
#endif
    int                 numframes;
    struct mtexinfo_s   *next; // used for animation
#endif
} mtexinfo_t;

#if USE_REF
typedef struct {
    vec3_t      point;
} mvertex_t;

typedef struct {
    // indices into the bsp->basisvectors array
    uint32_t normal;
    uint32_t tangent;
    uint32_t bitangent;
} mbasis_t;

// A Quake III fog brush: its box, and its shader's "fogparms ( r g b ) distance"
typedef struct {
    vec3_t      mins, maxs;
    vec3_t      color;
    float       distance;       // how far in it turns opaque
} mfogvolume_t;

typedef struct {
    mvertex_t   *v[2];
#if REF_SOFT
    uintptr_t   cachededgeoffset;   // software renderer: edge emitted this frame
#endif
} medge_t;

typedef struct {
    medge_t     *edge;
    int         vert;
} msurfedge_t;

#define SURF_TRANS_MASK (SURF_TRANS33 | SURF_TRANS66)
#define SURF_COLOR_MASK (SURF_TRANS_MASK | SURF_WARP)
#define SURF_NOLM_MASK  (SURF_COLOR_MASK | SURF_FLOWING | SURF_SKY)

#define DSURF_PLANEBACK     1

// for lightmap block calculation
#define S_MAX(surf) ((surf)->lm_width ? (surf)->lm_width : ((surf)->extents[0] >> 4) + 1)
#define T_MAX(surf) ((surf)->lm_height ? (surf)->lm_height : ((surf)->extents[1] >> 4) + 1)

typedef struct mface_s {
    msurfedge_t     *firstsurfedge;
    int             numsurfedges;

    cplane_t        *plane;
    int             drawflags; // DSURF_PLANEBACK, etc

    byte            *lightmap;
    byte            styles[MAX_LIGHTMAPS];
    int             numstyles;

    // BSPX DECOUPLED_LM, which is where the rerelease's maps keep all their
    // lighting: the lightmap's own size, and u = DotProduct(point, lm_axis[0])
    // + lm_axis[0][3] in luxels, one sample at each whole u from 0 to
    // lm_width - 1 (v likewise). lm_width 0 is the classic mapping, a luxel
    // per 16 texinfo units from texturemins.
    int             lm_width, lm_height;
    float           lm_axis[2][4];

    mtexinfo_t      *texinfo;
    int             texturemins[2];
    int             extents[2];

    // Quake III maps (bsp_q3.c): every face is one triangle carrying its own
    // texture mapping, because Quake III stores a UV per vertex instead of
    // texinfo axes. u = DotProduct(point, uv_axis[0]) + uv_axis[0][3], in
    // 0..1 units of the image (v likewise) - not texels, so the renderer does
    // not divide by the image size. texinfo->axis is unused when uv_mapped.
    bool            uv_mapped;
    float           uv_axis[2][4];

#if REF_GL
    int             texnum[2];
    int             statebits;
    int             firstvert;
    int             light_s, light_t;
    float           stylecache[MAX_LIGHTMAPS];
#endif
#if REF_SOFT
    struct surfcache_s    *cachespots[MIPLEVELS]; // surface generation data
    // The lightmap on the classic 16-texel grid from texturemins, which is
    // the only layout the surface cache builds from. Same as `lightmap` for
    // a classic face; resampled from it for a DECOUPLED_LM one.
    byte            *sw_lightmap;
#endif

    int             firstbasis;

    int             drawframe;

    int             dlightframe;
    int             dlightbits;
    struct mface_s  *next;
} mface_t;
#endif

typedef struct mnode_s {
    /* ======> */
    cplane_t            *plane;     // never NULL to differentiate from leafs
#if USE_REF
    vec3_t              mins;
    vec3_t              maxs;

    int                 visframe;
#endif
    struct mnode_s      *parent;
    /* <====== */

    struct mnode_s      *children[2];

#if USE_REF
    int                 numfaces;
    mface_t             *firstface;
#endif
} mnode_t;

typedef struct {
    cplane_t            *plane;
    mtexinfo_t          *texinfo;
} mbrushside_t;

typedef struct {
    int                 contents;
    int                 numsides;
    mbrushside_t        *firstbrushside;
    int                 checkcount;        // to avoid repeated testings
} mbrush_t;

typedef struct {
    /* ======> */
    cplane_t            *plane;     // always NULL to differentiate from nodes
#if USE_REF
    vec3_t              mins;
    vec3_t              maxs;

    int                 visframe;
#endif
    struct mnode_s      *parent;
    /* <====== */

    int             contents;
    int             cluster;
    int             area;
    mbrush_t        **firstleafbrush;
    int             numleafbrushes;

    // Quake III maps (bsp_q3.c). A Quake III leaf has no contents of its own -
    // detail brushes do not split the tree, so one leaf can be part water,
    // part solid and part air - and its point contents come from testing the
    // point against the leaf's brushes (pointtest). `contents` is then only
    // the best single answer for the whole leaf (solid for an opaque leaf, a
    // liquid when the leaf's centre is in one), which is what the renderer's
    // leaf-level checks want. brushcontents is every contents bit any listed
    // brush has, for the trace code's "can anything here stop me" early-out.
    // Zero for a Quake II map, where `contents` already answers both.
    int             brushcontents;
    bool            pointtest;
#if USE_REF
    mface_t         **firstleafface;
    int             numleaffaces;
#endif
#if REF_SOFT
    unsigned        key;    // software renderer: BSP order, for sorting bmodels
#endif
} mleaf_t;

typedef struct {
    unsigned    portalnum;
    unsigned    otherarea;
} mareaportal_t;

typedef struct {
    int             numareaportals;
    mareaportal_t   *firstareaportal;
    int             floodvalid;
} marea_t;

typedef struct mmodel_s {
#if USE_REF
    /* ======> */
    int             type;
    /* <====== */
#endif
    vec3_t          mins, maxs;
    vec3_t          origin;        // for sounds or lights
    mnode_t         *headnode;

#if USE_REF
    float           radius;

    int             numfaces;
    mface_t         *firstface;

#if USE_REF == REF_GL
    int             drawframe;
#endif
#endif
} mmodel_t;

typedef struct bsp_s {
    list_t      entry;
    int         refcount;

    unsigned    checksum;

    memhunk_t   hunk;

    int             numbrushsides;
    mbrushside_t    *brushsides;

    int             numtexinfo;
    mtexinfo_t      *texinfo;

    int             numplanes;
    cplane_t        *planes;

    int             numnodes;
    mnode_t         *nodes;

    int             numleafs;
    mleaf_t         *leafs;

    int             numleafbrushes;
    mbrush_t        **leafbrushes;

    int             nummodels;
    mmodel_t        *models;

    int             numbrushes;
    mbrush_t        *brushes;

    int             numvisibility;
    int             visrowsize;
    dvis_t          *vis;

    int             numentitychars;
    char            *entitystring;

    int             numareas;
    marea_t         *areas;

    int             lastareaportal; // largest portal number used
    int             numareaportals; // size of the array below
    mareaportal_t   *areaportals;

#if USE_REF
    int             numfaces;
    mface_t         *faces;

    int             numleaffaces;
    mface_t         **leaffaces;

    int             numlightmapbytes;
    byte            *lightmap;

    int             numvertices;
    mvertex_t       *vertices;

    int             numedges;
    medge_t         *edges;

    int             numsurfedges;
    msurfedge_t     *surfedges;

    int             numbasisvectors;
    vec3_t          *basisvectors;

    int             numbases;
    mbasis_t        *bases;

    // Quake III fog brushes (bsp_q3.c) - a box of the map's own coloured fog
    int             numfogvolumes;
    mfogvolume_t    *fogvolumes;
#endif

	byte            *pvs_matrix;
	byte            *pvs2_matrix;
	bool            pvs_patched;

    bool            extended;
    bool            q3;             // transcoded from a Quake III IBSP 46 (bsp_q3.c)

	// WARNING: the 'name' string is actually longer than this, and the bsp_t structure is allocated larger than sizeof(bsp_t) in BSP_Load
    char            name[1];
} bsp_t;

int BSP_Load(const char *name, bsp_t **bsp_p);
void BSP_Free(bsp_t *bsp);

// bsp_q3.c: fills `bsp` (name, refcount and checksum already set, hunk not yet
// begun) from a whole Quake III .bsp file. BSP_Load calls it for IBSP 46 and
// does the validation and PVS work that follows for either format.
int BSP_LoadQ3(bsp_t *bsp, const byte *buf, size_t filelen);
void BSP_Q3_Init(void);
const char *BSP_GetError(void);

#if USE_REF
typedef struct {
    mface_t     *surf;
    cplane_t    plane;
    int         s, t;
    float       fraction;
} lightpoint_t;

void BSP_LightPoint(lightpoint_t *point, const vec3_t start, const vec3_t end, mnode_t *headnode);
void BSP_TransformedLightPoint(lightpoint_t *point, const vec3_t start, const vec3_t end,
                               mnode_t *headnode, const vec3_t origin, const vec3_t angles);
#endif

byte *BSP_ClusterVis(bsp_t *bsp, byte *mask, int cluster, int vis);
mleaf_t *BSP_PointLeaf(mnode_t *node, const vec3_t p);
mmodel_t *BSP_InlineModel(bsp_t *bsp, const char *name);

byte* BSP_GetPvs(bsp_t *bsp, int cluster);
byte* BSP_GetPvs2(bsp_t *bsp, int cluster);

bool BSP_SavePatchedPVS(bsp_t *bsp);

void BSP_Init(void);

#endif // BSP_H
