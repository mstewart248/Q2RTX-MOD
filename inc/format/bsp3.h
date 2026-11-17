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

#ifndef FORMAT_BSP3_H
#define FORMAT_BSP3_H

/*
==============================================================================

Quake III Arena .BSP file format (IBSP version 46)

Only what common/bsp_q3.c needs to turn one into a bsp_t. Laid out exactly as
on disk - every field is a 32-bit int or float, so there is no padding.

==============================================================================
*/

#define Q3_BSPVERSION       46

#define Q3_LUMP_ENTITIES        0
#define Q3_LUMP_SHADERS         1
#define Q3_LUMP_PLANES          2
#define Q3_LUMP_NODES           3
#define Q3_LUMP_LEAFS           4
#define Q3_LUMP_LEAFSURFACES    5
#define Q3_LUMP_LEAFBRUSHES     6
#define Q3_LUMP_MODELS          7
#define Q3_LUMP_BRUSHES         8
#define Q3_LUMP_BRUSHSIDES      9
#define Q3_LUMP_DRAWVERTS       10
#define Q3_LUMP_DRAWINDEXES     11
#define Q3_LUMP_FOGS            12
#define Q3_LUMP_SURFACES        13
#define Q3_LUMP_LIGHTMAPS       14
#define Q3_LUMP_LIGHTGRID       15
#define Q3_LUMP_VISIBILITY      16
#define Q3_HEADER_LUMPS         17

typedef struct {
    uint32_t    fileofs, filelen;
} q3lump_t;

typedef struct {
    uint32_t    ident;
    uint32_t    version;
    q3lump_t    lumps[Q3_HEADER_LUMPS];
} q3dheader_t;

typedef struct {
    float       mins[3], maxs[3];
    int32_t     firstSurface, numSurfaces;
    int32_t     firstBrush, numBrushes;
} q3dmodel_t;

typedef struct {
    char        shader[64];
    int32_t     surfaceFlags;
    int32_t     contentFlags;
} q3dshader_t;

typedef struct {
    float       normal[3];
    float       dist;
} q3dplane_t;

typedef struct {
    int32_t     planeNum;
    int32_t     children[2];    // negative numbers are -(leafs+1), not nodes
    int32_t     mins[3];
    int32_t     maxs[3];
} q3dnode_t;

typedef struct {
    int32_t     cluster;        // -1 = opaque cluster (do I still store these?)
    int32_t     area;
    int32_t     mins[3];
    int32_t     maxs[3];
    int32_t     firstLeafSurface, numLeafSurfaces;
    int32_t     firstLeafBrush, numLeafBrushes;
} q3dleaf_t;

typedef struct {
    int32_t     planeNum;       // positive plane side faces out of the leaf
    int32_t     shaderNum;
} q3dbrushside_t;

typedef struct {
    int32_t     firstSide;
    int32_t     numSides;
    int32_t     shaderNum;      // the shader that determines the contents flags
} q3dbrush_t;

typedef struct {
    char        shader[64];
    int32_t     brushNum;
    int32_t     visibleSide;    // the brush side that ray tests need to clip against (-1 == none)
} q3dfog_t;

typedef struct {
    float       xyz[3];
    float       st[2];
    float       lightmap[2];
    float       normal[3];
    uint8_t     color[4];
} q3drawvert_t;

enum {
    Q3_MST_BAD,
    Q3_MST_PLANAR,
    Q3_MST_PATCH,
    Q3_MST_TRIANGLE_SOUP,
    Q3_MST_FLARE
};

typedef struct {
    int32_t     shaderNum;
    int32_t     fogNum;
    int32_t     surfaceType;

    int32_t     firstVert;
    int32_t     numVerts;

    int32_t     firstIndex;
    int32_t     numIndexes;

    int32_t     lightmapNum;
    int32_t     lightmapX, lightmapY;
    int32_t     lightmapWidth, lightmapHeight;

    float       lightmapOrigin[3];
    float       lightmapVecs[3][3]; // for patches, [0] and [1] are lodbounds

    int32_t     patchWidth;
    int32_t     patchHeight;
} q3dsurface_t;

// Quake III surface flags (q_shared.h / surfaceflags.h). These are NOT the
// Quake II SURF_* bits - only SLICK, SKY and NODRAW happen to share a value.
#define Q3_SURF_NODAMAGE        0x1
#define Q3_SURF_SLICK           0x2
#define Q3_SURF_SKY             0x4
#define Q3_SURF_LADDER          0x8
#define Q3_SURF_NOIMPACT        0x10
#define Q3_SURF_NOMARKS         0x20
#define Q3_SURF_FLESH           0x40
#define Q3_SURF_NODRAW          0x80
#define Q3_SURF_HINT            0x100
#define Q3_SURF_SKIP            0x200
#define Q3_SURF_NOLIGHTMAP      0x400
#define Q3_SURF_POINTLIGHT      0x800
#define Q3_SURF_METALSTEPS      0x1000
#define Q3_SURF_NOSTEPS         0x2000
#define Q3_SURF_NONSOLID        0x4000
#define Q3_SURF_LIGHTFILTER     0x8000
#define Q3_SURF_ALPHASHADOW     0x10000
#define Q3_SURF_NODLIGHT        0x20000
#define Q3_SURF_DUST            0x40000

// Quake III content flags. Again not Quake II's: the low liquid bits agree,
// everything from 0x40 up means something else.
#define Q3_CONTENTS_SOLID           0x1
#define Q3_CONTENTS_LAVA            0x8
#define Q3_CONTENTS_SLIME           0x10
#define Q3_CONTENTS_WATER           0x20
#define Q3_CONTENTS_FOG             0x40
#define Q3_CONTENTS_NOTTEAM1        0x80
#define Q3_CONTENTS_NOTTEAM2        0x100
#define Q3_CONTENTS_NOBOTCLIP       0x200
#define Q3_CONTENTS_AREAPORTAL      0x8000
#define Q3_CONTENTS_PLAYERCLIP      0x10000
#define Q3_CONTENTS_MONSTERCLIP     0x20000
#define Q3_CONTENTS_TELEPORTER      0x40000
#define Q3_CONTENTS_JUMPPAD         0x80000
#define Q3_CONTENTS_CLUSTERPORTAL   0x100000
#define Q3_CONTENTS_DONOTENTER      0x200000
#define Q3_CONTENTS_BOTCLIP         0x400000
#define Q3_CONTENTS_MOVER           0x800000
#define Q3_CONTENTS_ORIGIN          0x1000000
#define Q3_CONTENTS_BODY            0x2000000
#define Q3_CONTENTS_CORPSE          0x4000000
#define Q3_CONTENTS_DETAIL          0x8000000
#define Q3_CONTENTS_STRUCTURAL      0x10000000
#define Q3_CONTENTS_TRANSLUCENT     0x20000000
#define Q3_CONTENTS_TRIGGER         0x40000000
#define Q3_CONTENTS_NODROP          0x80000000

#endif // FORMAT_BSP3_H
