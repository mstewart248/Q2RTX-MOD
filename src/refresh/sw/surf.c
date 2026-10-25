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
// r_surf.c: surface-related refresh code

#include "sw.h"

// A surface cache block being drawn, for the block drawers: on the stack of
// whichever thread draws it.
typedef struct {
    blocklight_t    *lightptr;      // signed, as the light is: the drawers
                                    // step it by differences that go negative
                                    // wherever light falls off (unsigned made
                                    // that ~2^28 a texel; id's 8-bit drawer only
                                    // read the low 16 bits, which came out right)
    int             lightwidth;
    int             numvblocks;
    int             sourcetstep;
    int             surfrowbytes;
    int             stepback;
    byte            *pbasesource, *sourcemax;
    byte            *prowdestbase;
    byte            *pbaseglow;     // the texture's glow map (sw_glowmaps)
} blockctx_t;

static void R_DrawSurfaceBlock8_mip0(blockctx_t *c);
static void R_DrawSurfaceBlock8_mip1(blockctx_t *c);
static void R_DrawSurfaceBlock8_mip2(blockctx_t *c);
static void R_DrawSurfaceBlock8_mip3(blockctx_t *c);

static void (*surfmiptable[4])(blockctx_t *c) = {
    R_DrawSurfaceBlock8_mip0,
    R_DrawSurfaceBlock8_mip1,
    R_DrawSurfaceBlock8_mip2,
    R_DrawSurfaceBlock8_mip3
};

static void R_DrawSurfaceBlockPal_mip0(blockctx_t *c);
static void R_DrawSurfaceBlockPal_mip1(blockctx_t *c);
static void R_DrawSurfaceBlockPal_mip2(blockctx_t *c);
static void R_DrawSurfaceBlockPal_mip3(blockctx_t *c);

static void (*surfmiptable8[4])(blockctx_t *c) = {
    R_DrawSurfaceBlockPal_mip0,
    R_DrawSurfaceBlockPal_mip1,
    R_DrawSurfaceBlockPal_mip2,
    R_DrawSurfaceBlockPal_mip3
};

static void R_DrawSurfaceBlockGlow_mip0(blockctx_t *c);
static void R_DrawSurfaceBlockGlow_mip1(blockctx_t *c);
static void R_DrawSurfaceBlockGlow_mip2(blockctx_t *c);
static void R_DrawSurfaceBlockGlow_mip3(blockctx_t *c);

static void (*surfmiptableglow[4])(blockctx_t *c) = {
    R_DrawSurfaceBlockGlow_mip0,
    R_DrawSurfaceBlockGlow_mip1,
    R_DrawSurfaceBlockGlow_mip2,
    R_DrawSurfaceBlockGlow_mip3
};

static int          sc_size;
static surfcache_t  *sc_rover, *sc_base;
static int          sc_scale = 1;   // over the size the resolution asks for

// While a batch of spans is being set up for the workers (D_DrawSurfaces),
// every block it needs carries its number; evicting one of those means the
// batch needs more than the whole cache, which has to grow.
int                 r_cachebatch;
bool                r_cacheoverflow;
bool                r_cachegrow;

/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
static image_t *R_TextureAnimation(mtexinfo_t *tex)
{
    int     c;

    if (!tex->next)
        return tex->image;

    c = currententity->frame % tex->numframes;
    while (c) {
        tex = tex->next;
        c--;
    }

    return tex->image;
}

/*
===============
R_DrawSurface
===============
*/
static void R_DrawSurface(surfbuild_t *b)
{
    const drawsurf_t    *ds = &b->ds;
    blockctx_t  c;
    byte        *basetptr, *source, *glowsource = NULL;
    int         smax, tmax, twidth;
    int         u, numhblocks;
    int         soffset, toffset;
    int         horzblockstep;
    byte        *pcolumndest;
    void        (*pblockdrawer)(blockctx_t *c);
    image_t     *mt;
    int         blocksize;
    int         blockdivshift;

    c.surfrowbytes = ds->rowbytes;
    c.pbaseglow = NULL;

    mt = ds->image;

    if (r_8bit) {
        // an alpha tested face keeps its holes (255), which every colormap
        // row passes through for R_DrawSpanletOpaque8 to skip
        if (ds->surfmip == 0 && mt->sw_holes8 && SW_ALPHATEST(ds->surf->texinfo->c.flags))
            source = mt->sw_holes8;
        else
            source = mt->sw_pixels8[ds->surfmip];
        c.lightwidth = SW_S_MAX(ds->surf);
        pblockdrawer = surfmiptable8[ds->surfmip];
    } else {
        source = mt->sw_pixels[ds->surfmip];
        c.lightwidth = SW_S_MAX(ds->surf) * LIGHTMAP_BYTES;
        glowsource = mt->sw_glow[ds->surfmip];
        pblockdrawer = glowsource ? surfmiptableglow[ds->surfmip] : surfmiptable[ds->surfmip];
    }

    twidth = (mt->upload_width >> ds->surfmip) * r_texbytes;

    blocksize = 16 >> ds->surfmip;
    blockdivshift = 4 - ds->surfmip;

    numhblocks = ds->surfwidth >> blockdivshift;
    c.numvblocks = ds->surfheight >> blockdivshift;

// TODO: only needs to be set when there is a display settings change
    horzblockstep = blocksize * r_texbytes;

    smax = mt->upload_width >> ds->surfmip;
    tmax = mt->upload_height >> ds->surfmip;
    c.sourcetstep = twidth;
    c.stepback = tmax * twidth;

    c.sourcemax = source + tmax * smax * r_texbytes;

    soffset = ds->surf->texturemins[0];
    toffset = ds->surf->texturemins[1];

// << 16 components are to guarantee positive values for %
    soffset = ((soffset >> ds->surfmip) + (smax << 16)) % smax;
    toffset = ((toffset >> ds->surfmip) + (tmax << 16)) % tmax;

    basetptr = &source[toffset * twidth];

    pcolumndest = ds->surfdat;

    for (u = 0; u < numhblocks; u++) {
        c.lightptr = b->blocklights + u * (r_8bit ? 1 : LIGHTMAP_BYTES);

        c.prowdestbase = pcolumndest;

        c.pbasesource = basetptr + soffset * r_texbytes;
        if (glowsource)
            c.pbaseglow = glowsource + (c.pbasesource - source) / TEX_BYTES;

        (*pblockdrawer)(&c);

        soffset += blocksize;
        if (soffset >= smax)
            soffset = 0;

        pcolumndest += horzblockstep;
    }
}

//=============================================================================

#define BLOCK_FUNC R_DrawSurfaceBlock8_mip0
#define BLOCK_SHIFT 4
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlock8_mip1
#define BLOCK_SHIFT 3
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlock8_mip2
#define BLOCK_SHIFT 2
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlock8_mip3
#define BLOCK_SHIFT 1
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlockGlow_mip0
#define BLOCK_SHIFT 4
#define BLOCK_GLOW
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlockGlow_mip1
#define BLOCK_SHIFT 3
#define BLOCK_GLOW
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlockGlow_mip2
#define BLOCK_SHIFT 2
#define BLOCK_GLOW
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlockGlow_mip3
#define BLOCK_SHIFT 1
#define BLOCK_GLOW
#include "block.h"

#define BLOCK_FUNC R_DrawSurfaceBlockPal_mip0
#define BLOCK_SHIFT 4
#include "block8.h"

#define BLOCK_FUNC R_DrawSurfaceBlockPal_mip1
#define BLOCK_SHIFT 3
#include "block8.h"

#define BLOCK_FUNC R_DrawSurfaceBlockPal_mip2
#define BLOCK_SHIFT 2
#include "block8.h"

#define BLOCK_FUNC R_DrawSurfaceBlockPal_mip3
#define BLOCK_SHIFT 1
#include "block8.h"

//============================================================================


/*
================
R_InitCaches

================
*/
void R_InitCaches(void)
{
    int     size;
    int     pix;
    int     scale;

    // calculate size to allocate
    if (sw_surfcacheoverride->integer) {
        size = Cvar_ClampInteger(sw_surfcacheoverride,
                                 SURFCACHE_SIZE_AT_320X240,
                                 SURFCACHE_SIZE_AT_320X240 * 25);
    } else {
        size = SURFCACHE_SIZE_AT_320X240;

        pix = vid.width * vid.height;
        if (pix > 64000)
            size += (pix - 64000) * 3;
    }

    size *= TEX_BYTES;

    // grown for a view that needed more at once (R_CheckCaches)
    for (scale = 1; scale < sc_scale && size <= (1 << 29); scale *= 2)
        size *= 2;

    // round up to page size
    size = (size + 8191) & ~8191;

    Com_DPrintf("%ik surface cache\n", size / 1024);

    sc_size = size;
    sc_base = (surfcache_t *)R_Malloc(size);
    sc_rover = sc_base;

    sc_base->next = NULL;
    sc_base->owner = NULL;
    sc_base->size = sc_size;
    sc_base->batch = 0;
}

/*
================
R_CheckCaches

Before a frame: when the last one's spans needed more blocks at once than
the cache holds (and fell back to drawing on one thread), it doubles.
================
*/
void R_CheckCaches(void)
{
    if (!r_cachegrow)
        return;

    r_cachegrow = false;
    if (sc_scale >= 16 || sc_size > (1 << 29))
        return;

    sc_scale *= 2;
    D_FlushCaches();
    R_FreeCaches();
    R_InitCaches();
}

void R_FreeCaches(void)
{
    if (sc_base) {
        Z_Free(sc_base);
        sc_base = NULL;
    }

    sc_size = 0;
    sc_rover = NULL;
}

/*
==================
D_FlushCaches
==================
*/
void D_FlushCaches(void)
{
    surfcache_t     *c;

    if (!sc_base)
        return;

    for (c = sc_base; c; c = c->next) {
        if (c->owner) {
            *c->owner = NULL;
        }
    }

    sc_rover = sc_base;
    sc_base->next = NULL;
    sc_base->owner = NULL;
    sc_base->size = sc_size;
    sc_base->batch = 0;
}

// a block is going: if the batch being set up needs it, the cache is too small
// a block the batch being set up draws from: the rover steps over it
static inline bool D_SCPinned(const surfcache_t *c)
{
    return r_cachebatch && c->owner && c->batch == r_cachebatch;
}

static inline void D_SCFree(surfcache_t *c)
{
    if (c->owner) {
        if (D_SCPinned(c))
            r_cacheoverflow = true;
        *c->owner = NULL;
    }
}

/*
=================
D_SCAlloc

The rover takes the blocks after it, oldest first, as it goes round. While a
batch is set up for the workers it steps over the ones that batch draws from,
which may have been there since long before (a wall that has stayed in view):
only when every run is pinned does the batch need more than the cache.
=================
*/
static surfcache_t *D_SCAlloc(int width, int size)
{
    surfcache_t             *new, *c;
    int                     total, wraps;

    if ((width < 0) || (width > 256))
        Com_Error(ERR_FATAL, "D_SCAlloc: bad cache width %d\n", width);

    if ((size <= 0) || (size > 0x10000 * TEX_BYTES))
        Com_Error(ERR_FATAL, "D_SCAlloc: bad cache size %d\n", size);

    size += sizeof(surfcache_t) - 4;
    size = (size + 3) & ~3;
    if (size > sc_size)
        Com_Error(ERR_FATAL, "D_SCAlloc: %i > cache size of %i", size, sc_size);

    for (wraps = 0; ; ) {
        // if there is not size bytes after the rover, reset to the start
        if (!sc_rover || (byte *)sc_rover - (byte *)sc_base > sc_size - size) {
            sc_rover = sc_base;
            if (++wraps > 1)
                break;      // round twice: all of it is pinned
        }

        // enough unpinned bytes from here on? (the blocks tile the cache, so
        // the size bytes after the rover are all in it)
        for (c = sc_rover, total = 0; c && total < size; c = c->next) {
            if (D_SCPinned(c))
                break;
            total += c->size;
        }
        if (total >= size)
            break;
        if (!c)
            Com_Error(ERR_FATAL, "D_SCAlloc: hit the end of memory");
        sc_rover = c->next;
    }

// colect and free surfcache_t blocks until the rover block is large enough
// (evicting a pinned one marks the batch as too big for the cache)
    new = sc_rover;
    D_SCFree(sc_rover);

    while (new->size < size) {
        // free another
        sc_rover = sc_rover->next;
        if (!sc_rover)
            Com_Error(ERR_FATAL, "D_SCAlloc: hit the end of memory");
        D_SCFree(sc_rover);

        new->size += sc_rover->size;
        new->next = sc_rover->next;
    }

// create a fragment out of any leftovers
    if (new->size - size > 256) {
        sc_rover = (surfcache_t *)((byte *)new + size);
        sc_rover->size = new->size - size;
        sc_rover->next = new->next;
        sc_rover->width = 0;
        sc_rover->owner = NULL;
        sc_rover->batch = 0;
        new->next = sc_rover;
        new->size = size;
    } else
        sc_rover = new->next;

    new->width = width;
// DEBUG
    if (width > 0)
        new->height = (size - sizeof(*new) + sizeof(new->data)) / (width * r_texbytes);

    new->owner = NULL;              // should be set properly after return

    return new;
}


/*
=================
D_SCDump
=================
*/
void D_SCDump_f(void)
{
    surfcache_t             *test;

    for (test = sc_base; test; test = test->next) {
        if (test == sc_rover)
            Com_Printf("ROVER:\n");
        Com_Printf("%p : %i bytes     %i width\n", test, test->size,
                   test->width);
    }
}

//=============================================================================

/*
================
D_CacheSurfaceBegin

The part of D_CacheSurface that has to happen on the main thread: whether
the face's block at this mip still holds the right pixels, and if not, the
block they go into (allocating it can evict others). Fills *b for
D_BuildSurface and sets b->cache when the block has to be drawn, or leaves
it NULL. The block reads as stale until it is drawn.
================
*/
surfcache_t *D_CacheSurfaceBegin(mface_t *surface, int miplevel, surfbuild_t *b)
{
    surfcache_t     *cache;
    drawsurf_t      *ds = &b->ds;
    int             i;

    b->cache = NULL;

//
// if the surface is animating or flashing, flush the cache
//
    ds->image = R_TextureAnimation(surface->texinfo);
    ds->lightadj[0] = r_newrefdef.lightstyles[surface->styles[0]].white * sw_modulate->value * 256;
    ds->lightadj[1] = r_newrefdef.lightstyles[surface->styles[1]].white * sw_modulate->value * 256;
    ds->lightadj[2] = r_newrefdef.lightstyles[surface->styles[2]].white * sw_modulate->value * 256;
    ds->lightadj[3] = r_newrefdef.lightstyles[surface->styles[3]].white * sw_modulate->value * 256;

//
// see if the cache holds apropriate data
//
    cache = surface->cachespots[miplevel];

    // already to be drawn in this batch (a bmodel drawn twice)
    if (cache && r_cachebatch && cache->batch == r_cachebatch && !cache->image)
        return cache;

    if (cache && cache->image && !cache->dlight && surface->dlightframe != r_framecount
        && cache->image == ds->image
        && cache->lightadj[0] == ds->lightadj[0]
        && cache->lightadj[1] == ds->lightadj[1]
        && cache->lightadj[2] == ds->lightadj[2]
        && cache->lightadj[3] == ds->lightadj[3]) {
        cache->batch = r_cachebatch;
        return cache;
    }

    if (SW_S_MAX(surface) * SW_T_MAX(surface) > MAX_BLOCKLIGHTS)
        Com_Error(ERR_DROP, "D_CacheSurface: surface blocklights size %i > %i",
                  SW_S_MAX(surface) * SW_T_MAX(surface), MAX_BLOCKLIGHTS);

//
// determine shape of surface
//
    ds->surfmip = miplevel;
    ds->surfwidth = surface->extents[0] >> miplevel;
    ds->rowbytes = ds->surfwidth * r_texbytes;
    ds->surfheight = surface->extents[1] >> miplevel;

//
// allocate memory if needed
//
    if (!cache) {   // if a texture just animated, don't reallocate it
        cache = D_SCAlloc(ds->surfwidth, ds->surfwidth * ds->surfheight * r_texbytes);
        surface->cachespots[miplevel] = cache;
        cache->owner = &surface->cachespots[miplevel];
        cache->mipscale = 1.0f / (1 << miplevel);
    }

    cache->batch = r_cachebatch;
    cache->image = NULL;
    cache->dlight = surface->dlightframe == r_framecount;

    ds->surfdat = (pixel_t *)cache->data;
    ds->surf = surface;

    // a bmodel's dynamic lights are in its own frame
    b->entity = currententity;
    if (currententity && currententity != &r_worldentity)
        for (i = 0; i < 3; i++)
            VectorCopy(entity_rotation[i], b->rotation[i]);

    b->cache = cache;

    c_surf++;

    return cache;
}

/*
================
D_BuildSurface

Lights the face's texture into its block. Any thread, with blocklights of
its own.
================
*/
void D_BuildSurface(surfbuild_t *b)
{
    surfcache_t *cache = b->cache;

    // calculate the lightings
    R_BuildLightMap(b);

    // rasterize the surface into the cache
    R_DrawSurface(b);

    cache->lightadj[0] = b->ds.lightadj[0];
    cache->lightadj[1] = b->ds.lightadj[1];
    cache->lightadj[2] = b->ds.lightadj[2];
    cache->lightadj[3] = b->ds.lightadj[3];
    cache->image = b->ds.image;
}

/*
================
D_CacheSurface

The face's block at this mip, drawn now if it needs to be.
================
*/
surfcache_t *D_CacheSurface(mface_t *surface, int miplevel)
{
    surfcache_t     *cache;
    surfbuild_t     b;

    cache = D_CacheSurfaceBegin(surface, miplevel, &b);
    if (b.cache) {
        b.blocklights = R_ThreadBlocklights(0);
        D_BuildSurface(&b);
    }

    return cache;
}
