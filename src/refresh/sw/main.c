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

// r_main.c

#include "sw.h"
#include "common/cpuprof.h"

viddef_t    vid;

entity_t    r_worldentity;

refdef_t    r_newrefdef;
model_t     *currentmodel;

bsp_t       *r_worldmodel;

unsigned    r_time1;
int         r_numallocatededges;
int         r_outofsurfaces;
int         r_outofedges;

bool        r_dowarp;

int         c_surf;
int         r_cnumsurfs;
int         r_clipflags;

//
// view origin
//
vec3_t  vup, base_vup;
vec3_t  vpn, base_vpn;
vec3_t  vright, base_vright;
vec3_t  r_origin;

//
// screen size info
//
oldrefdef_t r_refdef;

cplane_t    screenedge[4];

//
// refresh flags
//
int     r_framecount = 1;   // so frame counts initialized to 0 don't match
int     r_visframecount;
int     r_polycount;
int     r_drawnpolycount;
int     r_wholepolycount;

int         *pfrustum_indexes[4];
int         r_frustum_indexes[4 * 6];

mleaf_t     *r_viewleaf;
int         r_viewcluster, r_oldviewcluster;

cvar_t  *sw_aliasstats;
cvar_t  *sw_clearcolor;
cvar_t  *sw_drawflat;
cvar_t  *sw_draworder;
cvar_t  *sw_maxedges;
cvar_t  *sw_maxsurfs;
cvar_t  *sw_reportedgeout;
cvar_t  *sw_reportsurfout;
cvar_t  *sw_surfcacheoverride;
cvar_t  *sw_waterwarp;
cvar_t  *sw_dynamic;
cvar_t  *sw_modulate;
cvar_t  *sw_lockpvs;
cvar_t  *sw_scale;
cvar_t  *sw_polyblend;
cvar_t  *sw_8bit;
cvar_t  *sw_fog;
cvar_t  *sw_glowmaps;

cvar_t  *r_drawworld;
cvar_t  *r_drawentities;
cvar_t  *r_fullbright;
cvar_t  *r_lerpmodels;
cvar_t  *r_novis;

cvar_t  *r_speeds;

cvar_t  *vid_gamma;

// all global and static refresh variables are collected in a contiguous block
// to avoid cache conflicts.

//-------------------------------------------------------
// global refresh variables
//-------------------------------------------------------

// FIXME: make into one big structure, like cl or sv
// FIXME: do separately for refresh engine and driver

float   d_sdivzstepu, d_tdivzstepu, d_zistepu;
float   d_sdivzstepv, d_tdivzstepv, d_zistepv;
float   d_sdivzorigin, d_tdivzorigin, d_ziorigin;

fixed16_t   sadjust, tadjust, bbextents, bbextentt;

pixel_t         *cacheblock;
int             cachewidth;

pixel_t     *d_viewbuffer;
int         d_screenrowbytes;
short       *d_pzbuffer;
int         d_zrowbytes;
int         d_zwidth;
byte        *d_spantable[MAXHEIGHT];
short       *d_zspantable[MAXHEIGHT];

int     sintable[CYCLE * 2];
int     intsintable[CYCLE * 2];
int     blanktable[CYCLE * 2];

// The 3D view, when it is not drawn straight into vid.buffer: below window
// resolution (sw_scale), or underwater, where D_WarpScreen resamples it.
static byte     *r_viewbuffer;
static size_t   r_viewbuffer_size;

bool    r_8bit;
int     r_pixbytes = VID_BYTES;
int     r_texbytes = TEX_BYTES;

/*
================
R_InitTurb
================
*/
void R_InitTurb(void)
{
    int     i;

    for (i = 0; i < CYCLE * 2; i++) {
        sintable[i] = AMP + sin(i * M_PI * 2 / CYCLE) * AMP;
        intsintable[i] = AMP2 + sin(i * M_PI * 2 / CYCLE) * AMP2; // AMP2, not 20
        blanktable[i] = 0;
    }
}

static void R_Register(void)
{
    sw_aliasstats = Cvar_Get("sw_polymodelstats", "0", 0);
    sw_clearcolor = Cvar_Get("sw_clearcolor", "2", 0);
    sw_drawflat = Cvar_Get("sw_drawflat", "0", CVAR_CHEAT);
    sw_draworder = Cvar_Get("sw_draworder", "0", CVAR_CHEAT);
    sw_maxedges = Cvar_Get("sw_maxedges", "32768", 0);
    sw_maxsurfs = Cvar_Get("sw_maxsurfs", "16384", 0);
    sw_mipcap = Cvar_Get("sw_mipcap", "0", 0);
    sw_mipscale = Cvar_Get("sw_mipscale", "1", 0);
    sw_reportedgeout = Cvar_Get("sw_reportedgeout", "0", 0);
    sw_reportsurfout = Cvar_Get("sw_reportsurfout", "0", 0);
    sw_waterwarp = Cvar_Get("sw_waterwarp", "1", CVAR_ARCHIVE);
    sw_dynamic = Cvar_Get("sw_dynamic", "1", CVAR_ARCHIVE);
    sw_modulate = Cvar_Get("sw_modulate", "1", 0);
    sw_lockpvs = Cvar_Get("sw_lockpvs", "0", 0);
    // the 3D view is drawn at 1/sw_scale of the window and blown up with
    // nearest-neighbour; the HUD and menus stay at full resolution
    sw_scale = Cvar_Get("sw_scale", "1", CVAR_ARCHIVE);
    sw_polyblend = Cvar_Get("sw_polyblend", "1", CVAR_ARCHIVE);
    // 1 = the original 256-colour look: the palette, colormap.pcx's light
    // and translucency, white dynamic lights; 0 = 32-bit colour
    sw_8bit = Cvar_Get("sw_8bit", "1", CVAR_ARCHIVE);
    // the rerelease's fog and glow maps, neither of which the original
    // had: 32-bit mode only, and off unless asked for
    sw_fog = Cvar_Get("sw_fog", "0", CVAR_ARCHIVE);
    sw_glowmaps = Cvar_Get("sw_glowmaps", "0", CVAR_ARCHIVE | CVAR_FILES);
    // threads for the per-pixel work: 0 = one per logical processor, 1 = the
    // original single-threaded renderer
    sw_threads = Cvar_Get("sw_threads", "0", CVAR_ARCHIVE);

    r_speeds = Cvar_Get("r_speeds", "0", 0);
    r_fullbright = Cvar_Get("r_fullbright", "0", CVAR_CHEAT);
    r_drawentities = Cvar_Get("r_drawentities", "1", 0);
    r_drawworld = Cvar_Get("r_drawworld", "1", CVAR_CHEAT);
    r_lerpmodels = Cvar_Get("r_lerpmodels", "1", 0);
    r_novis = Cvar_Get("r_novis", "0", 0);

    vid_gamma = Cvar_Get("vid_gamma", "1.0", CVAR_ARCHIVE | CVAR_FILES);

    sw_surfcacheoverride = Cvar_Get("sw_surfcacheoverride", "0", 0);

    Cmd_AddCommand("scdump", D_SCDump_f);
}

static void R_UnRegister(void)
{
    Cmd_RemoveCommand("scdump");
}

static void R_FreeBuffers(void)
{
    Z_Freep((void **)&vid.buffer);
    Z_Freep((void **)&d_pzbuffer);
    Z_Freep((void **)&r_viewbuffer);
    r_viewbuffer_size = 0;
    D_FreeSpans();
    R_FreeFog();
}

void R_ModeChanged_SW(int width, int height, int flags, int rowbytes, void *pixels)
{
    // the window buffer is ours; SDL is handed a copy of it every frame
    width = Q_clip(width, 1, MAXWIDTH);
    height = Q_clip(height, 1, MAXHEIGHT);

    D_FlushCaches();
    R_FreeCaches();
    R_FreeBuffers();

    vid.width = width;
    vid.height = height;
    vid.rowbytes = width * VID_BYTES;
    vid.buffer = R_Mallocz(vid.rowbytes * height);

    r_config.width = vid.width;
    r_config.height = vid.height;
    r_config.flags = flags;

    R_SetClipRect_SW(NULL);

    d_pzbuffer = R_Mallocz(vid.width * vid.height * sizeof(d_pzbuffer[0]));
    d_zrowbytes = vid.width * sizeof(d_pzbuffer[0]);
    d_zwidth = vid.width;

    R_InitCaches();
}

/*
================
R_ViewBuffer

An off-screen buffer for a width x height view
================
*/
byte *R_ViewBuffer(int width, int height)
{
    size_t size = (size_t)width * height * VID_BYTES;

    if (size > r_viewbuffer_size) {
        Z_Free(r_viewbuffer);
        r_viewbuffer = R_Malloc(size);
        r_viewbuffer_size = size;
    }

    return r_viewbuffer;
}

/*
===============
R_Init
===============
*/
static ref_type_t R_Init_SW(bool total)
{
    Com_DPrintf("R_Init( %i )\n", total);

    if (!total) {
        R_InitImages();
        MOD_Init();
        return REF_TYPE_SW;
    }

    Com_Printf("------- R_Init (software) -------\n");

    R_Register();

    IMG_Init();

    MOD_Init();

    R_InitImages();

    R_InitDraw();

    R_InitSkyBox();

    view_clipplanes[0].leftedge = true;
    view_clipplanes[1].rightedge = true;
    view_clipplanes[1].leftedge =
    view_clipplanes[2].leftedge =
    view_clipplanes[3].leftedge = false;
    view_clipplanes[0].rightedge =
    view_clipplanes[2].rightedge =
    view_clipplanes[3].rightedge = false;

    R_InitTurb();

    R_InitThreads();

    // create the window last: it reports its size through R_ModeChanged,
    // which builds the buffers
    if (!VID_Init(GAPI_SOFTWARE)) {
        R_ShutdownThreads();
        R_UnRegister();
        R_ShutdownImages();
        MOD_Shutdown();
        IMG_Shutdown();
        return REF_TYPE_NONE;
    }

    Com_Printf("----------------------------------\n");

    return REF_TYPE_SW;
}

/*
===============
R_Shutdown
===============
*/
static void R_Shutdown_SW(bool total)
{
    Com_DPrintf("R_Shutdown( %i )\n", total);

    D_FlushCaches();

    MOD_Shutdown();

    R_ShutdownImages();

    R_FreeMap();

    if (!total) {
        return;
    }

    R_FreeCaches();

    R_FreeBuffers();

    R_ShutdownThreads();

    R_UnRegister();

    IMG_Shutdown();

    VID_Shutdown();

    memset(&vid, 0, sizeof(vid));
}

/*
===============
R_NewMap

Sizes the edge and surface pools. A frame that runs out of either doubles
it for the next frame (R_GrowPools), so these are only where they start.
===============
*/
static edge_t   *r_edgepool;
static surf_t   *r_surfpool;

static void R_AllocPools(int numedges, int numsurfs)
{
    Z_Free(r_edgepool);
    Z_Free(r_surfpool);

    r_numallocatededges = Q_clip(numedges, MINEDGES, MAXEDGES);
    r_cnumsurfs = Q_clip(numsurfs, MINSURFACES, MAXSURFACES);

    r_edgepool = R_Malloc(r_numallocatededges * sizeof(edge_t));
    // surface 0 doesn't really exist; it's just a dummy because index 0
    // is used to indicate no edge attached to surface
    r_surfpool = R_Malloc((r_cnumsurfs + 1) * sizeof(surf_t));
}

void R_NewMap(void)
{
    r_viewcluster = -1;

    R_AllocPools(sw_maxedges->integer, sw_maxsurfs->integer);
}

void R_FreeMap(void)
{
    Z_Freep((void **)&r_edgepool);
    Z_Freep((void **)&r_surfpool);
    r_numallocatededges = 0;
    r_cnumsurfs = 0;

    R_FreeWorld();

    if (r_worldmodel) {
        BSP_Free(r_worldmodel);
        r_worldmodel = NULL;
    }
}

static void R_GrowPools(void)
{
    int numedges = r_numallocatededges;
    int numsurfs = r_cnumsurfs;

    if (r_outofedges && numedges < MAXEDGES)
        numedges *= 2;
    if (r_outofsurfaces && numsurfs < MAXSURFACES)
        numsurfs *= 2;

    if (numedges != r_numallocatededges || numsurfs != r_cnumsurfs) {
        Com_DPrintf("%s: %d edges, %d surfaces\n", __func__, numedges, numsurfs);
        R_AllocPools(numedges, numsurfs);
    }
}


/*
===============
R_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current
cluster
===============
*/
static void R_MarkLeaves(void)
{
    byte    vis[VIS_MAX_BYTES];
    mnode_t *node;
    int     i;
    mleaf_t *leaf;
    int     cluster;

    if (r_oldviewcluster == r_viewcluster && !r_novis->integer &&
        r_viewcluster != -1) {
        return;
    }

    // development aid to let you run around and see exactly where
    // the pvs ends
    if (sw_lockpvs->integer)
        return;

    r_visframecount++;
    r_oldviewcluster = r_viewcluster;

    if (r_novis->integer || r_viewcluster == -1 || !r_worldmodel->vis) {
        // mark everything
        for (i = 0; i < r_worldmodel->numleafs; i++)
            r_worldmodel->leafs[i].visframe = r_visframecount;
        for (i = 0; i < r_worldmodel->numnodes; i++)
            r_worldmodel->nodes[i].visframe = r_visframecount;
        return;
    }

    BSP_ClusterVis(r_worldmodel, vis, r_viewcluster, DVIS_PVS);

    for (i = 0, leaf = r_worldmodel->leafs; i < r_worldmodel->numleafs; i++, leaf++) {
        cluster = leaf->cluster;
        if (cluster == -1)
            continue;
        if (Q_IsBitSet(vis, cluster)) {
            node = (mnode_t *)leaf;
            do {
                if (node->visframe == r_visframecount)
                    break;
                node->visframe = r_visframecount;
                node = node->parent;
            } while (node);
        }
    }

}

static int R_DrawEntities(int translucent)
{
    int         i;
    int         translucent_entities = 0;

    // all bmodels have already been drawn by the edge list
    for (i = 0; i < r_newrefdef.num_entities; i++) {
        currententity = &r_newrefdef.entities[i];

        // mirror-only: your own body, the world copy of your beam and the
        // like, which the path tracer shows in reflections - there are none
        if (currententity->flags & (RF_VIEWERMODEL | RF_REFLECTION_FX))
            continue;

        if ((currententity->flags & RF_TRANSLUCENT) == translucent) {
            translucent_entities++;
            continue;
        }

        if (currententity->flags & RF_BEAM) {
            modelorg[0] = -r_origin[0];
            modelorg[1] = -r_origin[1];
            modelorg[2] = -r_origin[2];
            VectorCopy(vec3_origin, r_entorigin);
            R_DrawBeam(currententity);
        } else {
            if (currententity->model & BIT(31)) {
                continue;
            }
            currentmodel = MOD_ForHandle(currententity->model);
            if (!currentmodel) {
                continue;
            }
            VectorCopy(currententity->origin, r_entorigin);
            VectorSubtract(r_origin, r_entorigin, modelorg);

            switch (currentmodel->type) {
            case MOD_ALIAS:
                R_AliasDrawModel();
                break;
            case MOD_SPRITE:
                R_DrawSprite();
                break;
            case MOD_EMPTY:
                break;
            default:
                Com_Error(ERR_FATAL, "%s: bad model type", __func__);
            }
        }
    }
    return translucent_entities;
}

/*
=============
R_DrawEntitiesOnList
=============
*/
static void R_DrawEntitiesOnList(void)
{
    int         translucent_entities;

    if (!r_drawentities->integer)
        return;

    translucent_entities = R_DrawEntities(RF_TRANSLUCENT);
    if (translucent_entities) {
        R_DrawEntities(0);
    }
}


/*
=============
R_BmodelCheckBBox
=============
*/
static int R_BmodelCheckBBox(const float *minmaxs)
{
    int         i, *pindex, clipflags;
    vec3_t      acceptpt, rejectpt;
    float       d;

    clipflags = 0;

    for (i = 0; i < 4; i++) {
        // generate accept and reject points
        // FIXME: do with fast look-ups or integer tests based on the sign bit
        // of the floating point values

        pindex = pfrustum_indexes[i];

        rejectpt[0] = minmaxs[pindex[0]];
        rejectpt[1] = minmaxs[pindex[1]];
        rejectpt[2] = minmaxs[pindex[2]];

        d = DotProduct(rejectpt, view_clipplanes[i].normal);
        d -= view_clipplanes[i].dist;

        if (d <= 0)
            return BMODEL_FULLY_CLIPPED;

        acceptpt[0] = minmaxs[pindex[3 + 0]];
        acceptpt[1] = minmaxs[pindex[3 + 1]];
        acceptpt[2] = minmaxs[pindex[3 + 2]];

        d = DotProduct(acceptpt, view_clipplanes[i].normal);
        d -= view_clipplanes[i].dist;

        if (d <= 0)
            clipflags |= (1 << i);
    }

    return clipflags;
}


/*
===================
R_FindTopnode

Find the first node that splits the given box
===================
*/
static mnode_t *R_FindTopnode(const vec3_t mins, const vec3_t maxs)
{
    int         sides;
    mnode_t     *node;

    node = r_worldmodel->nodes;

    while (node->visframe == r_visframecount) {
        if (!node->plane) {
            if (((mleaf_t *)node)->contents != CONTENTS_SOLID)
                return node;    // we've reached a non-solid leaf, so it's
                                // visible and not BSP clipped
            return NULL;    // in solid, so not visible
        }

        sides = BoxOnPlaneSideFast(mins, maxs, node->plane);

        if (sides == BOX_INTERSECTS)
            return node;    // this is the splitter

        // not split yet; recurse down the contacted side
        if (sides & BOX_INFRONT)
            node = node->children[0];
        else
            node = node->children[1];
    }

    return NULL;        // not visible at all
}


/*
=============
RotatedBBox

Returns an axially aligned box that contains the input box at the given rotation
=============
*/
static void RotatedBBox(const vec3_t mins, const vec3_t maxs, const vec3_t angles,
                        vec3_t tmins, vec3_t tmaxs)
{
    vec3_t  tmp, v;
    int     i, j;
    vec3_t  forward, right, up;

    if (!angles[0] && !angles[1] && !angles[2]) {
        VectorCopy(mins, tmins);
        VectorCopy(maxs, tmaxs);
        return;
    }

    for (i = 0; i < 3; i++) {
        tmins[i] = 99999;
        tmaxs[i] = -99999;
    }

    AngleVectors(angles, forward, right, up);

    for (i = 0; i < 8; i++) {
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


        VectorScale(forward, tmp[0], v);
        VectorMA(v, -tmp[1], right, v);
        VectorMA(v, tmp[2], up, v);

        for (j = 0; j < 3; j++) {
            if (v[j] < tmins[j])
                tmins[j] = v[j];
            if (v[j] > tmaxs[j])
                tmaxs[j] = v[j];
        }
    }
}

/*
=============
R_DrawBEntitiesOnList
=============
*/
static void R_DrawBEntitiesOnList(void)
{
    int         i, index, clipflags;
    vec3_t      oldorigin;
    vec3_t      mins, maxs;
    float       minmaxs[6];
    mnode_t     *topnode;
    mmodel_t    *model;

    if (!r_drawentities->integer)
        return;

    VectorCopy(modelorg, oldorigin);
    insubmodel = true;
    r_dlightframecount = r_framecount;

    for (i = 0; i < r_newrefdef.num_entities; i++) {
        currententity = &r_newrefdef.entities[i];
        index = currententity->model;
        if (!(index & BIT(31))) {
            continue;
        }
        index = ~index;
        if (index < 1 || index >= r_worldmodel->nummodels) {
            Com_Error(ERR_DROP, "%s: inline model %d out of range",
                      __func__, index);
        }
        model = &r_worldmodel->models[index];
        if (model->numfaces == 0)
            continue;   // clip brush only
        if (currententity->flags & RF_BEAM)
            continue;
        // see if the bounding box lets us trivially reject, also sets
        // trivial accept status
        RotatedBBox(model->mins, model->maxs,
                    currententity->angles, mins, maxs);
        VectorAdd(mins, currententity->origin, minmaxs);
        VectorAdd(maxs, currententity->origin, (minmaxs + 3));

        clipflags = R_BmodelCheckBBox(minmaxs);
        if (clipflags == BMODEL_FULLY_CLIPPED)
            continue;   // off the edge of the screen

        topnode = R_FindTopnode(minmaxs, minmaxs + 3);
        if (!topnode)
            continue;   // no part in a visible leaf

        VectorCopy(currententity->origin, r_entorigin);
        VectorSubtract(r_origin, r_entorigin, modelorg);

        // FIXME: stop transforming twice
        R_RotateBmodel();

        // calculate dynamic lighting for bmodel
        R_MarkLights(model->headnode);

        if (topnode->plane) {
            // not a leaf; has to be clipped to the world BSP
            r_clipflags = clipflags;
            R_DrawSolidClippedSubmodelPolygons(model, topnode);
        } else {
            // falls entirely in one leaf, so we just put all the
            // edges in the edge list and let 1/z sorting handle
            // drawing order
            R_DrawSubmodelPolygons(model, clipflags, topnode);
        }

        // put back world rotation and frustum clipping
        // FIXME: R_RotateBmodel should just work off base_vxx
        VectorCopy(base_vpn, vpn);
        VectorCopy(base_vup, vup);
        VectorCopy(base_vright, vright);
        VectorCopy(oldorigin, modelorg);
        R_TransformFrustum();
    }

    insubmodel = false;
}


/*
================
R_EdgeDrawing
================
*/
static void R_EdgeDrawing(void)
{
    if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
        return;

    r_edges = r_edgepool;
    surfaces = r_surfpool;
    surf_max = &surfaces[r_cnumsurfs + 1];

    R_BeginEdgeFrame();

    R_RenderWorld();

    R_DrawBEntitiesOnList();

    R_ScanEdges();
}

//=======================================================================

byte *IMG_ReadPixels_SW(int *width, int *height, int *rowbytes)
{
    byte *pixels;
    byte *src, *dst;
    int x, y;

    pixels = Z_Malloc(vid.width * vid.height * 3);

    // bottom row first, as glReadPixels hands them over
    src = vid.buffer + vid.rowbytes * (vid.height - 1);
    dst = pixels;

    for (y = 0; y < vid.height; y++, src -= vid.rowbytes) {
        for (x = 0; x < vid.width; x++) {
            dst[0] = src[x * VID_BYTES + 2];
            dst[1] = src[x * VID_BYTES + 1];
            dst[2] = src[x * VID_BYTES + 0];
            dst += 3;
        }
    }

    *width = vid.width;
    *height = vid.height;
    *rowbytes = vid.width * 3;

    return pixels;
}

//=======================================================================

// r_speeds: when each stage of the view finished, for R_PrintTimes
uint64_t    r_stagetime[R_NUMSTAGES];

static inline void R_StageTime(int stage)
{
    if (r_stagetime[0])
        r_stagetime[stage] = Sys_Microseconds();
}

/*
================
R_ScaleView

Blows the off-screen view up to the refdef rectangle, nearest neighbour.
================
*/
typedef struct {
    int         w, h, vh, jobs;
    const int   *column;
} scalepass_t;

static void R_ScaleRows(void *arg, int index, int thread)
{
    const scalepass_t   *p = arg;
    const int           *column = p->column;
    int                 x, y, sy, lasty;
    uint32_t            *src, *dst;
    byte                *src8;
    int                 w = p->w;
    int                 h = p->h;
    int                 vh = p->vh;
    int                 y0 = h * index / p->jobs;
    int                 y1 = h * (index + 1) / p->jobs;

    // a band's first row is always drawn: the one above is another band's
    lasty = -1;
    for (y = y0; y < y1; y++) {
        dst = (uint32_t *)(vid.buffer + (r_newrefdef.y + y) * vid.rowbytes) + r_newrefdef.x;
        sy = y * vh / h;
        if (sy == lasty) {
            // same source row as the one above it
            memcpy(dst, (byte *)dst - vid.rowbytes, w * VID_BYTES);
            continue;
        }
        if (r_8bit) {
            src8 = d_viewbuffer + sy * d_screenrowbytes;
            for (x = 0; x < w; x++)
                dst[x] = r_palette[src8[column[x]]];
        } else {
            src = (uint32_t *)(d_viewbuffer + sy * d_screenrowbytes);
            for (x = 0; x < w; x++)
                dst[x] = src[column[x]];
        }
        lasty = sy;
    }
}

static void R_ScaleView(void)
{
    static int  column[MAXWIDTH];
    scalepass_t p;
    int         x;

    p.w = r_newrefdef.width;
    p.h = r_newrefdef.height;
    p.vh = r_refdef.vrect.height;
    p.column = column;

    for (x = 0; x < p.w; x++)
        column[x] = x * r_refdef.vrect.width / p.w;

    p.jobs = R_RowJobs(p.h);
    R_ParallelFor(p.jobs, R_ScaleRows, &p);
}

/*
@@@@@@@@@@@@@@@@
R_RenderFrame

@@@@@@@@@@@@@@@@
*/
static void R_RenderFrame_SW(refdef_t *fd, int waterLevel)
{
    r_newrefdef = *fd;

    if (!r_worldmodel && !(r_newrefdef.rdflags & RDF_NOWORLDMODEL))
        Com_Error(ERR_FATAL, "R_RenderView: NULL worldmodel");

    // the view may not leave the window, whatever the client asks for
    r_newrefdef.x = Q_clip(r_newrefdef.x, 0, vid.width - 1);
    r_newrefdef.y = Q_clip(r_newrefdef.y, 0, vid.height - 1);
    r_newrefdef.width = Q_clip(r_newrefdef.width, 1, vid.width - r_newrefdef.x);
    r_newrefdef.height = Q_clip(r_newrefdef.height, 1, vid.height - r_newrefdef.y);

    // a face keeps the lights that reach it as bits in an int
    if (!sw_dynamic->integer)
        r_newrefdef.num_dlights = 0;
    else
        r_newrefdef.num_dlights = min(r_newrefdef.num_dlights, 32);

    if (r_speeds->integer)
        r_time1 = Sys_Milliseconds();
    r_stagetime[0] = r_speeds->integer ? Sys_Microseconds() : 0;

    if (r_worldmodel && !(r_newrefdef.rdflags & RDF_NOWORLDMODEL))
        R_GrowPools();

    r_outofsurfaces = 0;
    r_outofedges = 0;

    R_CheckThreads();
    R_CheckCaches();

    R_SetupFrame();

    R_SetupFog();

    if (!(r_newrefdef.rdflags & RDF_NOWORLDMODEL)) {
        R_MarkLeaves();     // done here so we know if we're in water

        R_MarkLights(r_worldmodel->nodes);
    }

    R_StageTime(1);

    R_EdgeDrawing();

    R_StageTime(2);

    R_DrawEntitiesOnList();

    R_DrawParticles();

    if (!(r_newrefdef.rdflags & RDF_NOWORLDMODEL))
        R_DrawAlphaSurfaces();

    R_StageTime(3);

    R_DrawFog();

    R_StageTime(4);

    // an 8-bit view takes the screen blend in its palette, as the
    // original did, on its way out to the window
    if (r_8bit)
        R_SetPalette8(sw_polyblend->integer ? r_newrefdef.blend : NULL);

    if (r_dowarp)
        D_WarpScreen();
    else if (d_viewbuffer != vid.buffer)
        R_ScaleView();

    if (!r_8bit && sw_polyblend->integer && r_newrefdef.blend[3] > 0)
        R_BlendScreen(r_newrefdef.blend);

    R_StageTime(5);

    if (sw_aliasstats->integer)
        R_PrintAliasStats();

    if (r_speeds->integer)
        R_PrintTimes();

    if (sw_reportsurfout->integer && r_outofsurfaces)
        Com_Printf("Short %d surfaces\n", r_outofsurfaces);

    if (sw_reportedgeout->integer && r_outofedges)
        Com_Printf("Short roughly %d edges\n", r_outofedges * 2 / 3);
}

/*
** R_BeginFrame
*/
static void R_BeginFrame_SW(void)
{
    R_SetClipRect_SW(NULL);
}

static void R_EndFrame_SW(void)
{
    VID_PresentSoftware(vid.buffer, vid.rowbytes, vid.width, vid.height);
}

/*
** R_DrawBeam
*/
void R_DrawBeam(entity_t *e)
{
#define NUM_BEAM_SEGS 6

    color_t color;
    int i;

    vec3_t perpvec, up;
    vec3_t direction, normalized_direction;
    vec3_t start_points[NUM_BEAM_SEGS], end_points[NUM_BEAM_SEGS];
    vec3_t oldorigin, origin;

    oldorigin[0] = e->oldorigin[0];
    oldorigin[1] = e->oldorigin[1];
    oldorigin[2] = e->oldorigin[2];

    origin[0] = e->origin[0];
    origin[1] = e->origin[1];
    origin[2] = e->origin[2];

    normalized_direction[0] = direction[0] = oldorigin[0] - origin[0];
    normalized_direction[1] = direction[1] = oldorigin[1] - origin[1];
    normalized_direction[2] = direction[2] = oldorigin[2] - origin[2];

    if (VectorNormalize(normalized_direction) == 0)
        return;

    MakeNormalVectors(normalized_direction, perpvec, up);
    VectorScale(perpvec, e->frame / 2, perpvec);

    for (i = 0; i < NUM_BEAM_SEGS; i++) {
        RotatePointAroundVector(start_points[i], normalized_direction,
                                perpvec, (360.0f / NUM_BEAM_SEGS) * i);
        VectorAdd(start_points[i], origin, start_points[i]);
        VectorAdd(start_points[i], direction, end_points[i]);
    }

    if (e->skinnum == -1)
        color.u32 = e->rgba.u32;
    else
        color.u32 = d_8to24table[e->skinnum & 0xFF];

    for (i = 0; i < NUM_BEAM_SEGS; i++) {
        R_IMFlatShadedQuad(start_points[i],
                           end_points[i],
                           end_points[(i + 1) % NUM_BEAM_SEGS],
                           start_points[(i + 1) % NUM_BEAM_SEGS],
                           color,
                           e->alpha);
    }
}

static void R_AddDecal_SW(decal_t *d)
{
}

static bool R_InterceptKey_SW(unsigned key, bool down)
{
    return false;
}

void R_RegisterFunctionsSW(void)
{
    R_Init = R_Init_SW;
    R_Shutdown = R_Shutdown_SW;
    R_BeginRegistration = R_BeginRegistration_SW;
    R_SetSky = R_SetSky_SW;
    R_EndRegistration = R_EndRegistration_SW;
    R_RenderFrame = R_RenderFrame_SW;
    R_LightPoint = R_LightPoint_SW;
    R_ClearColor = R_ClearColor_SW;
    R_SetAlpha = R_SetAlpha_SW;
    R_SetAlphaScale = R_SetAlphaScale_SW;
    R_SetColor = R_SetColor_SW;
    R_SetClipRect = R_SetClipRect_SW;
    R_SetScale = R_SetScale_SW;
    R_DrawChar = R_DrawChar_SW;
    R_DrawString = R_DrawString_SW;
    R_DrawPic = R_DrawPic_SW;
    R_DrawStretchPic = R_DrawStretchPic_SW;
    R_DrawRotatedPic = R_DrawRotatedPic_SW;
    R_TileClear = R_TileClear_SW;
    R_DrawFill8 = R_DrawFill8_SW;
    R_DrawFill32 = R_DrawFill32_SW;
    R_BeginFrame = R_BeginFrame_SW;
    R_EndFrame = R_EndFrame_SW;
    R_ModeChanged = R_ModeChanged_SW;
    R_AddDecal = R_AddDecal_SW;
    R_InterceptKey = R_InterceptKey_SW;
    // Every hook another renderer installs must be cleared here: switching
    // renderers re-runs this, and a hook left set calls into a renderer that
    // has been shut down.
    R_IsHDR = NULL;
    R_LatencySleep = NULL;
    R_SoundTrace = NULL;
    IMG_Load = IMG_Load_SW;
    IMG_Unload = IMG_Unload_SW;
    IMG_ReadPixels = IMG_ReadPixels_SW;
    IMG_ReadPixelsHDR = NULL;
    MOD_LoadMD2 = MOD_LoadMD2_SW;
    MOD_LoadMD3 = MOD_LoadMD3_SW;
    MOD_LoadIQM = NULL;
    MOD_LoadMD5 = NULL;
    MOD_Reference = MOD_Reference_SW;
    MOD_TraceMesh = NULL;
}
