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

/*
THE SOFTWARE RENDERER (vid_rtx 2).

id's span-based renderer as Q2PRO carried it to 32-bit colour in 2012-2013,
restored from this tree's history (1813ea32^:src/refresh/sw) and ported to the
current refresh API. The world is drawn the way the original was: an edge list
sorted in BSP order emits spans with zero overdraw, and every visible face is
textured from a lit, mipmapped surface cache. Models are affine-mapped
z-buffered triangles, translucency is per pixel.

What changed from the 2013 code:
  - no resolution cap. Edge u is 44.20 fixed point in an int64 (the int it
    was could not get past 2048 pixels), and the edge, surface and span
    pools are sized at run time and grow when a frame runs out;
  - the 2D API is the scaled one the GL renderer has (any font size,
    rotated pics, colour and alpha);
  - rerelease maps light from BSPX DECOUPLED_LM, which the surface cache
    cannot use as it stands; it is resampled onto the classic 16-texel grid
    at map load (mface_t::sw_lightmap);
  - the frame is presented through SDL_Renderer (VID_PresentSoftware).
*/

#include "shared/shared.h"
#include "common/bsp.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/math.h"
#include "client/client.h"
#include "client/video.h"
#include "refresh/refresh.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "system/system.h"

/*
====================================================

  CONSTANTS

====================================================
*/

#define DSURF_SKY           2
#define DSURF_TURB          4
#define DSURF_BACKGROUND    8

#define CACHE_SIZE      32

#define VID_BYTES       4   // the view and window buffers are BGRX
#define TEX_BYTES       4   // and so are textures and the surface cache

#define MAXVERTS        64              // max points in a surface polygon
#define MAXWORKINGVERTS (MAXVERTS + 4)  // max points in an intermediate
                                        // polygon (while processing)

// Static per-scanline tables are sized by these; nothing else is.
#define MAXHEIGHT       4320
#define MAXWIDTH        8192

#define INFINITE_DISTANCE       0x10000     // distance that's always guaranteed to
                                            // be farther away than anything in
                                            // the scene

#define PARTICLE_Z_CLIP 8.0

#define TRANSPARENT_COLOR       0xFF

#define TURB_SIZE               64  // base turbulent texture size
#define TURB_MASK               (TURB_SIZE - 1)

#define CYCLE                   128 // turbulent cycle size

#define DS_SPAN_LIST_END        -128

// Edge and surface pools. sw_maxedges/sw_maxsurfs set the starting size and
// a frame that runs out doubles the pool for the next one, up to these.
#define MINEDGES                4096
#define MAXEDGES                (1 << 20)
#define MINSURFACES             1024
#define MAXSURFACES             (1 << 18)

// flags in finalvert_t.flags
#define ALIAS_LEFT_CLIP             0x0001
#define ALIAS_TOP_CLIP              0x0002
#define ALIAS_RIGHT_CLIP            0x0004
#define ALIAS_BOTTOM_CLIP           0x0008
#define ALIAS_Z_CLIP                0x0010
#define ALIAS_XY_CLIP_MASK          0x000F

#define SURFCACHE_SIZE_AT_320X240   1024*768

#define BMODEL_FULLY_CLIPPED    0x10    // value returned by R_BmodelCheckBBox ()
                                        // if bbox is trivially rejected

#define CLIP_EPSILON            0.001

#define BACKFACE_EPSILON        0.01

#define NEAR_CLIP       0.01

#define ALIAS_Z_CLIP_PLANE      4

// turbulence stuff
#define AMP     8*0x10000
#define AMP2    3
#define SPEED   20

#define MIPSIZE(c)  ((c) * (256 + 64 + 16 + 4) / 256)

#define R_Malloc(size)      Z_TagMalloc(size, TAG_RENDERER)
#define R_Mallocz(size)     Z_TagMallocz(size, TAG_RENDERER)

#ifndef BIT
#define BIT(n)  (1u << (n))
#endif

static inline void Z_Freep(void **ptr)
{
    Z_Free(*ptr);
    *ptr = NULL;
}

// in common/math.c, not in a header
void RotatePointAroundVector(vec3_t dst, const vec3_t dir, const vec3_t point, float degrees);

static inline int Q_clip(int a, int lo, int hi)
{
    return a < lo ? lo : a > hi ? hi : a;
}

static inline float Q_clipf(float a, float lo, float hi)
{
    return a < lo ? lo : a > hi ? hi : a;
}

/*
====================================================

TYPES

====================================================
*/

typedef unsigned char pixel_t;

// 44.20 fixed point, for the edge list's screen u
typedef int64_t fixed20_t;

typedef struct {
    pixel_t                 *buffer;                // window-sized back buffer, BGRX
    int                     rowbytes;
    int                     width;
    int                     height;
} viddef_t;

typedef struct {
    vrect_t         vrect;                          // subwindow in video for refresh
    int             vrectright, vrectbottom;        // right & bottom screen coords
    float           vrectrightedge;                 // rightmost right edge we care about,
                                                    // for use in edge list
    float           fvrectx, fvrecty;               // for floating-point compares
    float           fvrectx_adj, fvrecty_adj;       // left and top edges, for clamping
    fixed20_t       vrect_x_adj_shift20;            // (vrect.x + 0.5 - epsilon) << 20
    fixed20_t       vrectright_adj_shift20;         // (vrectright + 0.5 - epsilon) << 20
    float           fvrectright_adj, fvrectbottom_adj;  // right and bottom edges, for clamping
    float           fvrectright;                    // rightmost edge, for Alias clamping
    float           fvrectbottom;                   // bottommost edge, for Alias clamping

    // values for perspective projection
    float           xcenter, ycenter;
    float           xscale, yscale;
    float           xscaleinv, yscaleinv;
    float           xscaleshrink, yscaleshrink;
    float           scale_for_mip;

    // particle values
    int             vrectright_particle, vrectbottom_particle;
    int             pix_min, pix_max;
    float           pix_size;       // particle pixels at distance 1
} oldrefdef_t;

typedef struct {
    float   u, v;
    float   s, t;
    float   zi;
} emitpoint_t;

typedef struct finalvert_s {
    int     u, v, s, t;
    int     l;
    int     zi;
    int     flags;
    float   xyz[3];         // eye space
} finalvert_t;

typedef struct {
    void                *pskin;
    int                 skinwidth;
    int                 skinheight;
} affinetridesc_t;

typedef struct drawsurf_s {
    byte        *surfdat;       // destination for generated surface
    int         rowbytes;       // destination logical width in bytes
    mface_t     *surf;          // description for surface to generate
    fixed8_t    lightadj[MAX_LIGHTMAPS]; // adjust for lightmap levels for dynamic lighting
    image_t     *image;
    int         surfmip;        // mipmapped ratio of surface texels / world pixels
    int         surfwidth;      // in mipmapped texels
    int         surfheight;     // in mipmapped texels
} drawsurf_t;

// clipped bmodel edges
typedef struct bedge_s {
    mvertex_t       *v[2];
    struct bedge_s  *pnext;
} bedge_t;

typedef struct clipplane_s {
    vec3_t              normal;
    float               dist;
    struct clipplane_s  *next;
    byte                leftedge;
    byte                rightedge;
    byte                reserved[2];
} clipplane_t;

#define MAX_BLOCKLIGHTS (129 * 129)    // faces to 2048 texels, see R_FaceDrawable
#define LIGHTMAP_BYTES  3

// The surface cache's lightmap: the classic grid, a luxel every 16 texels
// from texturemins, whatever the map stores (see mface_t::sw_lightmap).
// S_MAX and T_MAX are the face's own lightmap, which can be decoupled.
#define SW_S_MAX(surf)  (((surf)->extents[0] >> 4) + 1)
#define SW_T_MAX(surf)  (((surf)->extents[1] >> 4) + 1)

typedef int blocklight_t;

typedef struct surfcache_s {
    struct surfcache_s      *next;
    struct surfcache_s      **owner;                // NULL is an empty chunk of memory
    int                     lightadj[MAX_LIGHTMAPS]; // checked for strobe flush
    int                     dlight;
    int                     size;           // including header
    unsigned                width;
    unsigned                height;         // DEBUG only needed for debug
    float                   mipscale;
    image_t                 *image;         // NULL until its pixels are drawn
    int                     batch;          // last span batch that needed it
    byte                    data[4];        // width*height elements
} surfcache_t;

typedef struct espan_s {
    int             u, v, count;
    struct espan_s  *pnext;
    struct surf_s   *psurf;         // whose span, for drawing in row order
} espan_t;

// used by the polygon drawer (R_POLY.C) and sprite setup code (R_SPRITE.C)
typedef struct {
    int         nump;
    emitpoint_t *pverts;
    byte        *pixels;                // image
    int         pixel_width;            // image width
    int         pixel_height;           // image height
    vec3_t      vup, vright, vpn;       // in worldspace, for plane eq
    float       dist;
    float       s_offset, t_offset;
    float       viewer_position[3];
    void        (*drawspanlet)(struct spanletvars_s *sv);
    int         alpha;
    int         one_minus_alpha;
} polydesc_t;

// FIXME: compress, make a union if that will help
// insubmodel is only 1, flags is fewer than 32, spanstate could be a byte
typedef struct surf_s {
    struct surf_s   *next;          // active surface stack in r_edge.c
    struct surf_s   *prev;          // used in r_edge.c for active surf stack
    struct espan_s  *spans;         // pointer to linked list of spans to draw
    int             key;            // sorting key (BSP order)
    int             last_u;         // set during tracing
    int             spanstate;      // 0 = not in span
                                    // 1 = in span
                                    // -1 = in inverted span (end before start)
    int             flags;          // currentface flags
    mface_t         *msurf;
    entity_t        *entity;
    float           nearzi;         // nearest 1/z on surface, for mipmapping
    bool            insubmodel;
    float           d_ziorigin, d_zistepu, d_zistepv;
} surf_t;

typedef struct edge_s {
    fixed20_t       u;
    fixed20_t       u_step;
    struct edge_s   *prev, *next;
    uint32_t        surfs[2];
    struct edge_s   *nextremove;
    float           nearzi;
    int             vert;           // which way the face that emitted it walks it
    medge_t         *owner;
} edge_t;

/*
ALIAS MODELS.

One mesh per md2, one per md3 surface. Vertex positions are kept the way md2
stores them - small integers scaled and offset per frame - which md3's 1/64
fixed point fits as well. Texture coordinates are normalised to the skin, so
a skin of any size maps without the model knowing.
*/
typedef struct {
    int16_t         v[3];
    uint8_t         lightnormalindex;
    uint8_t         pad;
} maliasvert_t;

typedef struct {
    float           st[2];      // 0..1 across the skin
} maliasst_t;

typedef struct {
    uint16_t        index_xyz[3];
    uint16_t        index_st[3];
} maliastri_t;

typedef struct maliasframe_s {
    vec3_t          scale;
    vec3_t          translate;
    vec3_t          bounds[2];
    vec_t           radius;
} maliasframe_t;

typedef struct maliasmesh_s {
    int             numverts;   // per frame
    int             numtris;
    int             numsts;
    maliasvert_t    *verts;     // numverts * model->numframes
    maliastri_t     *tris;
    maliasst_t      *sts;
    int             numskins;
    image_t         *skins[MAX_ALIAS_SKINS];
} maliasmesh_t;

typedef struct {
    finalvert_t *a, *b, *c;
} aliastriangleparms_t;


/*
====================================================

VARS

====================================================
*/

extern int      r_framecount;       // sequence # of current frame since Quake started
extern bool     r_dowarp;

extern affinetridesc_t  r_affinetridesc;

void D_DrawSurfaces(int v0, int v1);
void D_WarpScreen(void);

//=======================================================================//

// A world surface's texture gradients and texels, as its spans are drawn
// (D_CalcGradients). Each surface gets its own, so that spans of different
// surfaces can be drawn at once.
typedef struct {
    float       sdivzstepu, tdivzstepu, zistepu;
    float       sdivzstepv, tdivzstepv, zistepv;
    float       sdivzorigin, tdivzorigin, ziorigin;
    fixed16_t   sadjust, tadjust;
    fixed16_t   bbextents, bbextentt;
    byte        *cacheblock;
    int         cachewidth;     // in bytes
} spangrad_t;

// Everything that drawing one face into its surface cache block needs, so
// that blocks can be drawn on any thread (D_BuildSurface).
typedef struct {
    drawsurf_t      ds;
    surfcache_t     *cache;
    entity_t        *entity;        // the bmodel's, for its dynamic lights
    vec3_t          rotation[3];    // and its entity_rotation
    blocklight_t    *blocklights;   // the thread's own
} surfbuild_t;

extern int              c_surf;

extern float    d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float    d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float    d_sdivzorigin, d_tdivzorigin, d_ziorigin;

extern fixed16_t    sadjust, tadjust;
extern fixed16_t    bbextents, bbextentt;

void D_DrawTurbulent16(espan_t *pspan, const spangrad_t *g, int *warptable);
void D_DrawSpans16(espan_t *pspans, const spangrad_t *g);
void D_DrawTurbulent16_8(espan_t *pspan, const spangrad_t *g, int *warptable);
void D_DrawSpans16_8(espan_t *pspans, const spangrad_t *g);
void D_DrawZSpans(espan_t *pspans, float zistepu, float zistepv, float ziorigin);

surfcache_t     *D_CacheSurface(mface_t *surface, int miplevel);
surfcache_t     *D_CacheSurfaceBegin(mface_t *surface, int miplevel, surfbuild_t *b);
void            D_BuildSurface(surfbuild_t *b);
extern int      r_cachebatch;       // stamps the blocks a batch of spans uses
extern bool     r_cacheoverflow;    // a batch needed more than the cache holds
extern bool     r_cachegrow;        // so it grows before the next frame
void            R_CheckCaches(void);

extern pixel_t  *d_viewbuffer;
extern int      d_screenrowbytes;
extern short    *d_pzbuffer;
extern int      d_zrowbytes;
extern int      d_zwidth;
extern byte     *d_spantable[MAXHEIGHT];
extern short    *d_zspantable[MAXHEIGHT];

extern int      d_minmip;
extern float    d_scalemip[3];

extern viddef_t         vid;
extern oldrefdef_t      r_refdef;

//===================================================================

extern int      cachewidth;
extern pixel_t  *cacheblock;

extern int      r_drawnpolycount;

extern int      sintable[CYCLE * 2];
extern int      intsintable[CYCLE * 2];
extern int      blanktable[CYCLE * 2];

extern vec3_t   vup, base_vup;
extern vec3_t   vpn, base_vpn;
extern vec3_t   vright, base_vright;

extern surf_t   *surfaces, *surface_p, *surf_max;

// surfaces are generated in back to front order by the bsp, so if a surf
// pointer is greater than another one, it should be drawn in front
// surfaces[1] is the background, and is used as the active surface stack.
// surfaces[0] is a dummy, because index 0 is used to indicate no surface
//  attached to an edge_t

//===================================================================

extern vec3_t   sxformaxis[4];  // s axis transformed into viewspace
extern vec3_t   txformaxis[4];  // t axis transformed into viewspac

void R_TransformVector(const vec3_t in, vec3_t out);

//===========================================================================

extern cvar_t   *sw_aliasstats;
extern cvar_t   *sw_clearcolor;
extern cvar_t   *sw_drawflat;
extern cvar_t   *sw_draworder;
extern cvar_t   *sw_maxedges;
extern cvar_t   *sw_maxsurfs;
extern cvar_t   *sw_mipcap;
extern cvar_t   *sw_mipscale;
extern cvar_t   *sw_reportsurfout;
extern cvar_t   *sw_reportedgeout;
extern cvar_t   *sw_surfcacheoverride;
extern cvar_t   *sw_waterwarp;
extern cvar_t   *sw_dynamic;
extern cvar_t   *sw_modulate;
extern cvar_t   *sw_scale;
extern cvar_t   *sw_8bit;
extern cvar_t   *sw_fog;
extern cvar_t   *sw_glowmaps;

extern cvar_t   *r_fullbright;
extern cvar_t   *r_drawentities;
extern cvar_t   *r_drawworld;
extern cvar_t   *r_lerpmodels;

extern cvar_t   *r_speeds;

extern cvar_t   *vid_gamma;


extern clipplane_t  view_clipplanes[4];
extern int          *pfrustum_indexes[4];


//=============================================================================

void R_RenderWorld(void);

//=============================================================================

extern cplane_t     screenedge[4];

extern vec3_t       r_origin;

extern entity_t     r_worldentity;
extern model_t      *currentmodel;
extern entity_t     *currententity;
extern vec3_t       modelorg;
extern vec3_t       r_entorigin;
extern vec3_t       entity_rotation[3];

extern int          r_visframecount;

extern mface_t      *r_alpha_surfaces;

//=============================================================================

//
// current entity info
//
extern bool         insubmodel;

void R_DrawAlphaSurfaces(void);

void R_DrawSprite(void);
void R_DrawBeam(entity_t *e);

void R_RenderFace(mface_t *fa, int clipflags);
void R_RenderBmodelFace(bedge_t *pedges, mface_t *psurf);
void R_TransformFrustum(void);

void R_DrawSubmodelPolygons(mmodel_t *pmodel, int clipflags, mnode_t *topnode);
void R_DrawSolidClippedSubmodelPolygons(mmodel_t *pmodel, mnode_t *topnode);

void R_AliasDrawModel(void);
void R_BeginEdgeFrame(void);
void R_ScanEdges(void);
void R_MarkLights(mnode_t *headnode);

void R_RotateBmodel(void);

extern int      c_faceclip;
extern int      r_polycount;
extern int      r_wholepolycount;

extern mvertex_t    *r_ptverts, *r_ptvertsmax;

extern int          r_currentkey;
extern int          r_currentbkey;

void R_DrawParticles(void);

extern int          r_amodels_drawn;
extern int          r_numallocatededges;
extern edge_t       *r_edges, *edge_p, *edge_max;

extern edge_t   *newedges[MAXHEIGHT];
extern edge_t   *removeedges[MAXHEIGHT];

extern fixed8_t r_aliasblendcolor[3];

extern int      r_alias_alpha;
extern int      r_alias_one_minus_alpha;
extern int      r_aliasblendcolor8;     // 8-bit shell colour index
extern bool     r_iractive;             // 8-bit IR goggles on this model
extern ptrdiff_t r_skinglowdelta;       // skin glow map - skin, sw_glowmaps

extern int      r_outofsurfaces;
extern int      r_outofedges;

extern aliastriangleparms_t aliastriangleparms;

void R_DrawTriangle(void);
void R_AliasClipTriangle(finalvert_t *index0, finalvert_t *index1, finalvert_t *index2);
void R_AliasProjectAndClipTestFinalVert(finalvert_t *fv);

extern unsigned r_time1;

// r_speeds: setup, world, models, particles, translucent surfaces, fog, and
// the scale/warp/blend out to the window
#define R_NUMSTAGES     8
extern uint64_t r_stagetime[R_NUMSTAGES];

// r_speeds: the frame's span batches (D_DrawSurfaces) and where their time
// went, in microseconds; serial counts batches drawn on one thread
typedef struct {
    int         batches, serial;
    uint64_t    total, prep, build, spans;
} drawstats_t;

extern drawstats_t  r_drawstats;
extern int      r_frustum_indexes[4 * 6];
extern int      r_cnumsurfs;

extern mleaf_t  *r_viewleaf;
extern int      r_viewcluster, r_oldviewcluster;

extern int      r_clipflags;
extern int      r_dlightframecount;

extern bsp_t    *r_worldmodel;


void R_PrintAliasStats(void);
void R_PrintTimes(void);
void R_LightPoint_SW(const vec3_t p, vec3_t color);
void R_SetupFrame(void);
void R_BuildLightMap(surfbuild_t *b);

// thread.c: the worker pool for the per-pixel stages (sw_threads)
#define MAX_SW_THREADS  64

extern cvar_t   *sw_threads;
extern int      r_numthreads;       // the main thread and its workers

void R_InitThreads(void);
void R_ShutdownThreads(void);
void R_CheckThreads(void);
void R_ParallelFor(int count, void (*func)(void *arg, int index, int thread), void *arg);
int R_RowJobs(int rows);
blocklight_t *R_ThreadBlocklights(int thread);

extern refdef_t     r_newrefdef;

//====================================================================

void R_NewMap(void);
void R_FreeMap(void);
void R_FreeWorld(void);
byte *R_ViewBuffer(int width, int height);

void R_InitCaches(void);
void R_FreeCaches(void);
void D_FlushCaches(void);
void D_FreeSpans(void);
void D_SCDump_f(void);

void R_InitTurb(void);

void R_InitImages(void);
void R_ShutdownImages(void);

void R_BuildGammaTable(void);

void R_InitSkyBox(void);
void R_EmitSkyBox(void);
void R_SetSky_SW(const char *name, float rotate, int autorotate, const vec3_t axis);

void R_IMFlatShadedQuad(const vec3_t a, const vec3_t b, const vec3_t c, const vec3_t d, color_t color, float alpha);

// model.c
void R_BeginRegistration_SW(const char *name);
void R_EndRegistration_SW(void);
int MOD_LoadMD2_SW(model_t *model, const void *rawdata, size_t length, const char *mod_name);
int MOD_LoadMD3_SW(model_t *model, const void *rawdata, size_t length, const char *mod_name);
void MOD_Reference_SW(model_t *model);

// image.c
void IMG_Load_SW(image_t *image, byte *pic);
void IMG_Unload_SW(image_t *image);
byte *IMG_ReadPixels_SW(int *width, int *height, int *rowbytes);

/*
8-BIT MODE (sw_8bit 1)

The original renderer: textures are palette indices, light is a row of
colormap.pcx, translucency its 66/33 table. The view is drawn one byte a
pixel into the off-screen view buffer and put through r_palette - which
carries vid_gamma and the screen blend, as the 1997 palette did - on its
way to the window. The 2D layer is 32-bit in both modes.
*/
#define VID_CBITS   6
#define VID_GRADES  (1 << VID_CBITS)

extern bool     r_8bit;         // this view
extern int      r_pixbytes;     // view buffer bytes a pixel: 1 or VID_BYTES
extern int      r_texbytes;     // texture and surface cache bytes a texel: 1 or TEX_BYTES

extern byte     r_colormap[VID_GRADES * 256];   // (light row << 8) + index
extern byte     r_alphamap[256 * 256];          // 66% of the row + 33% of the column
extern uint32_t r_palette[256];                 // this frame's output colours, BGRX

// fog.c
extern int32_t  *d_pfogz;   // full precision 1/z (D_DrawZSpans), or NULL

void R_SetupFog(void);
void R_DrawFog(void);
void R_FreeFog(void);

void R_InitPalette8(void);
byte R_Quantize(int r, int g, int b);
void R_SetPalette8(const vec4_t blend);

// draw.c
void R_InitDraw(void);
void R_ClearColor_SW(void);
void R_SetAlpha_SW(float alpha);
void R_SetAlphaScale_SW(float alpha);
void R_SetColor_SW(uint32_t color);
void R_SetClipRect_SW(const clipRect_t *clip);
void R_SetScale_SW(float scale);
void R_DrawChar_SW(int x, int y, int flags, int ch, qhandle_t font);
int R_DrawString_SW(int x, int y, int flags, size_t maxChars, const char *string, qhandle_t font);
void R_DrawPic_SW(int x, int y, qhandle_t pic);
void R_DrawStretchPic_SW(int x, int y, int w, int h, qhandle_t pic);
void R_DrawRotatedPic_SW(float cx, float cy, float w, float h, float angle, qhandle_t pic);
void R_TileClear_SW(int x, int y, int w, int h, qhandle_t pic);
void R_DrawFill8_SW(int x, int y, int w, int h, int c);
void R_DrawFill32_SW(int x, int y, int w, int h, uint32_t color);
void R_BlendScreen(const vec4_t blend);
