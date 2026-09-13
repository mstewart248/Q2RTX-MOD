/*
Copyright (C) 1997-2001 Id Software, Inc.
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

#ifndef REFRESH_H
#define REFRESH_H

#include "common/cvar.h"
#include "common/error.h"

// the rerelease's dynamic_light entities are submitted as dlights and the
// busiest map (mine1/mine2) has 69 of them, on top of the transient ones
#define MAX_DLIGHTS     256

#define MAX_ENTITIES    8192    // == MAX_PACKET_ENTITIES * 2
#define MAX_PARTICLES   1000000
#define MAX_LIGHTSTYLES 256

#define POWERSUIT_SCALE     4.0f
#define WEAPONSHELL_SCALE   0.5f

#define SHELL_RED_COLOR     0xF2
#define SHELL_GREEN_COLOR   0xD0
#define SHELL_BLUE_COLOR    0xF3

#define SHELL_RG_COLOR      0xDC
//#define SHELL_RB_COLOR        0x86
#define SHELL_RB_COLOR      0x68
#define SHELL_BG_COLOR      0x78

//ROGUE
#define SHELL_DOUBLE_COLOR  0xDF // 223
#define SHELL_HALF_DAM_COLOR    0x90
#define SHELL_CYAN_COLOR    0x72
//ROGUE

#define SHELL_WHITE_COLOR   0xD7

// NOTE: these flags are intentionally the same value
#define RF_LEFTHAND         RF_NOSHADOW

#define RF_SHELL_MASK       (RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | \
                             RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM)

#define DLIGHT_CUTOFF       64

typedef struct entity_s {
    qhandle_t           model;          // opaque type outside refresh
    vec3_t              angles;

    /*
    ** most recent data
    */
    vec3_t              origin;     // also used as RF_BEAM's "from"
    int                 frame;          // also used as RF_BEAM's diameter

    /*
    ** previous data for lerping
    */
    vec3_t              oldorigin;  // also used as RF_BEAM's "to"
    int                 oldframe;

    /*
    ** misc
    */
    float   backlerp;               // 0.0 = current, 1.0 = old
    int     skinnum;                // also used as RF_BEAM's palette index,
                                    // -1 => use rgba

    float   alpha;                  // ignore if RF_TRANSLUCENT isn't set
    color_t rgba;

    qhandle_t   skin;           // NULL for inline skin
    int         flags;

    int                 id;

	int tent_type;

	float scale;
} entity_t;

typedef enum dlight_type_e
{
    DLIGHT_SPHERE = 0,
    DLIGHT_SPOT
} dlight_type;

typedef enum dlight_spot_emission_profile_e
{
    DLIGHT_SPOT_EMISSION_PROFILE_FALLOFF = 0,
    DLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE
} dlight_spot_emission_profile;

// "no explicit volumetric scale", i.e. fall back to the per-class cvar default
// in copy_light(). Negative so that 0 keeps its literal meaning of "this light
// lights the room and adds no fog glow at all".
//
// Lives here rather than beside light_poly_t in models.h because both the
// client (view.c, lightedit.c, dynamiclights.c) and the renderer need it, and
// refresh.h is the header they have in common.
#define LIGHT_VOLUMETRIC_SCALE_UNSET (-1.f)

typedef struct dlight_s {
    vec3_t  origin;
#if USE_REF == REF_GL
    vec3_t  transformed;
#endif
    vec3_t  color;
    float   intensity;
	float   radius;

    // Per-light volumetric scale, carried through add_dlights() onto the
    // light_poly_t. LIGHT_VOLUMETRIC_SCALE_UNSET (-1) = use the class default;
    // V_AddSphereLight sets that, so a caller only touches this to override.
    float   volumetric_scale;

    // VKPT light types support
    dlight_type light_type;
    // Spotlight options
    struct {
        // Spotlight emission profile
        dlight_spot_emission_profile emission_profile;
        // Spotlight direction
        vec3_t  direction;
        union {
            // Options for DLIGHT_SPOT_EMISSION_PROFILE_FALLOFF
            struct {
                // Cosine of angle of spotlight cone width (no emission beyond that)
                float   cos_total_width;
                // Cosine of angle of start of falloff (full emission below that)
                float   cos_falloff_start;
            };
            // Options for DLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE
            struct {
                // Angle of spotlight cone width (no emission beyond that), in radians
                float   total_width;
                // Emission profile texture, indexed by 'angle / total_width'
                qhandle_t texture;
            };
        };
    } spot;
} dlight_t;

typedef struct particle_s {
    vec3_t  origin;
    int     color;              // -1 => use rgba
    float   alpha;
    color_t rgba;
	float   brightness;
	float   radius;
} particle_t;

// A blood droplet drawn as real, shaded SPHERE GEOMETRY in the path tracer's
// geometry TLAS, instead of as a camera-facing quad in the effects pass.
//
// The difference is not cosmetic. Ordinary particles go through
// pt_logic_particle(), which is not a shading pass at all - it fetches a colour,
// applies a radial falloff and alpha-composites the result over the traced
// image. Nothing there has a normal or a BRDF, so a particle can never be lit,
// never casts a shadow and never appears in a reflection. These do all three.
//
// Behind cl_blood_spheres; built in src/refresh/vkpt/blood.c.
// A CEILING, not the budget. The budget is cl_blood_max, which is what sizes the
// staging memory (see vkpt_blood_slot_capacity) - so raising this alone costs
// nothing at all.
//
// It has to be a ceiling rather than the budget because g_no_janitor changed what
// the budget means. While splats faded after cl_blood_splat_life, 512 was a
// ROLLING WINDOW that emptied itself, and a big fight could paint all of it. With
// permanence nothing ever expires, so the same number became a lifetime total:
// the floor fills once, stays full, and from then on there is only ever room for
// the most recent monster. Permanence did not add blood, it froze the budget.
#define MAX_BLOOD_SPHERES 4096

typedef struct blood_sphere_s {
    // WHERE THIS DROPLET'S GEOMETRY LIVES, and it is stable for the droplet's
    // whole life. The renderer writes every droplet at slot * stride in the blood
    // section, so a droplet that has not changed still finds its own vertices
    // exactly where it left them and can skip regenerating entirely.
    //
    // A packed array cannot do this. CL_AllocParticle PREPENDS, so every new
    // droplet shifted every existing one by one array position, moved all their
    // geometry, and invalidated the whole cache - which is why the cost appeared
    // precisely while blood was landing and vanished the moment it stopped.
    int     slot;

    vec3_t  origin;
    vec3_t  prev_origin;        // last frame's origin - this IS the motion vector
    float   radius;
    int     color;              // palette index, -1 => use rgba
    color_t rgba;
    float   seed;               // per-droplet phase for the animated surface ripple

    // Splat shape.  A droplet in flight is a sphere (flatten 1, normal unused);
    // one that has hit something is an ellipsoid squashed along the surface
    // normal, which is what makes it read as stuck to the wall rather than
    // resting against it.  The renderer derives the tangential spread from
    // flatten, so a splat keeps roughly the volume the droplet had.
    vec3_t  normal;
    float   flatten;            // 1 = sphere; smaller = flatter against `normal`

    // Directionality. `tangent` is the impact direction projected into the
    // surface, and `stretch` elongates the splat along it - which is what turns
    // a round dot into a mark that shows which way the blood was travelling.
    // stretch 1 (or a zero tangent) is the round splat.
    vec3_t  tangent;
    float   stretch;

    // The half-extent ACROSS `tangent`, as a multiple of the same spread that
    // `stretch` multiplies.  1 is the droplet's own width.
    //
    // THIS USED TO BE DERIVED (`stretch - stretch_trail`, the impact's share of
    // the elongation) AND IT COULD NOT STAY THAT WAY.  A derived cross axis is
    // never more than the droplet's own width, so the mark could never be WIDER
    // across its travel than along it - and that is exactly the shape a
    // horizontal smear has the moment it starts running down a wall.  With the
    // two extents independent the slide can DEFORM the mark, drawing it out
    // downhill while it narrows across; before, all it could do was pivot a
    // fixed ellipse onto the new axis, which reads as a needle swinging round
    // rather than as blood running.
    //
    // See CL_BloodReshapeSlide: the re-expression at each committed turn is
    // AREA-PRESERVING, so a 2:1 bar turned ninety degrees becomes a round mark
    // of the same size rather than a 2:1 bar facing the other way.
    float   cross;

    // How much of `stretch` the pool picked up by SLIDING rather than by landing,
    // in the same units.  The renderer shifts the mesh back along `tangent` by
    // exactly that much, so the length a pool gains while running downhill trails
    // BEHIND it instead of growing forward as well - a smear being drawn rather
    // than a stretched blob being carried.
    //
    // The impact smear is deliberately not counted here: it belongs to the moment
    // of contact and stays centred on it.  0 for everything that never slid,
    // which is the great majority of splats, and they are byte-identical to
    // before this field existed.
    float   stretch_trail;

    // Rim clipping - eight nibbles giving how far the surface reaches in eight
    // directions around the splat, in tenths of the nominal rim radius.  See
    // cparticle_t::blood_rim for how it is measured; here it only trims the
    // outline, so a splat that landed at the edge of a ledge stops AT the edge
    // instead of hanging its far half out over the drop.
    //
    // 0xffffffff (BLOOD_RIM_FULL) takes the untouched path, so every splat away
    // from an edge generates byte-identical geometry to before this existed.  It
    // is quantized for the same reason the fade is: it feeds the geometry cache,
    // and anything that varies continuously there rebuilds the mesh every frame.
    uint32_t rim_support;
} blood_sphere_t;

// The rim encoding, shared because the client measures it and the renderer
// consumes it. Eight directions around the splat, each a reach in TENTHS of the
// nominal rim radius - so 10 is exactly the rim, 15 covers the outward lobes
// pt_blood_wobble adds, and 0 is "no surface at all this way".
#define BLOOD_RIM_SAMPLES   8
#define BLOOD_RIM_FULL      0xffffffffu
#define BLOOD_RIM_SCALE     10.0f

typedef struct lightstyle_s {
    float           white;          // highest of RGB
    vec3_t          rgb;            // 0.0 - 2.0
} lightstyle_t;

#ifdef USE_SMALL_GPU
#define MAX_DECALS 2
#else
#define MAX_DECALS 50
#endif
typedef struct decal_s {
    vec3_t pos;
    vec3_t dir;
    float spread;
    float length;
    float dummy;
} decal_t;

// passes information back from the RTX renderer to the engine for various development maros
typedef struct ref_feedback_s {
	int         viewcluster;
	int         lookatcluster;
	int         num_light_polys;
	int         resolution_scale;

	/* Presents performed for the last rendered frame: 1 normally, or the frame
	   generation multiplier (2..6) when DLSS-G is running. The FPS readout counts
	   RENDERED frames, so it needs this to report what reached the display. */
	int         presented_frames;

	char        view_material[MAX_QPATH];
	char        view_material_override[MAX_QPATH];
    int         view_material_index;

	vec3_t      hdr_color;
	float       adapted_luminance;
} ref_feedback_t;

// the rerelease's per-map atmospheric fog, read off worldspawn by the client in
// src/client/mapfog.c. These are the map's own authored values; how thick they
// actually render is cl_fog_scale, because KEX's density-to-extinction constant
// is not derivable from anything we have.
typedef struct {
    int     mode;               // cl_fog: 1 = sun-lit medium, 2 = lit by local lights

    float   density;            // plain distance fog
    vec3_t  color;

    float   hf_density;         // altitude-banded fog
    float   hf_falloff;
    float   hf_start_z;         // world Z of the TOP of the band
    float   hf_end_z;           // world Z of the BOTTOM; always below start
    vec3_t  hf_start_color;
    vec3_t  hf_end_color;

    // How much denser (or thinner) the VOLUMETRIC half is than the sky/sun half.
    // The densities above are scaled by cl_fog_scale and drive the sun term in
    // EVERY mode; this ratio is applied on top of them, to the local-light term
    // only, so cl_fog 3 can carry its own density without disturbing the sky
    // fog. 1.0 means the two match, which is what cl_volumetric_fog_density -1
    // (its default) produces.
    float   vol_density_ratio;
} mapfog_params_t;

/* THE RENDERER-SIDE ACCESSOR FOR THE ABOVE, DECLARED HERE ON PURPOSE.

   It is defined in src/client/mapfog.c and was declared ONLY in
   src/client/client.h, which the refresh modules do not include. god_rays.c
   calls it at three sites and therefore called it with NO PROTOTYPE, which MSVC
   reported as:

     god_rays.c(460): warning C4013: 'CL_GetMapFog' undefined;
                      assuming extern returning int

   That is not cosmetic. The function returns `bool`, which the Microsoft x64
   ABI passes in AL with THE UPPER 24 BITS OF EAX UNDEFINED. An unprototyped
   caller assumes `int` and tests the whole of EAX, so a `false` can read as
   true (or a `true` be corrupted) depending on whatever last used that
   register. The result is a BOOLEAN that flips for reasons unrelated to the
   fog - and vkpt_froxel_enabled() is exactly such a boolean, gating ONLY the
   froxel grid: when it reads false the grid's passes are skipped and
   god_rays_filter reads a stale/empty integrated volume, i.e. ALL the map fog
   disappears at once while the per-pixel march stays correct.

   Declaring it next to the struct it fills gives every refresh TU the real
   signature. client.h keeps its identical declaration; the two agree. */
bool CL_GetMapFog(mapfog_params_t *out);

typedef struct refdef_s {
    int         x, y, width, height;// in virtual screen coordinates
    float       fov_x, fov_y;
    vec3_t      vieworg;
    vec3_t      viewangles;
    vec4_t      blend;          // rgba 0-1 full screen blend
    float       time;               // time is uesed to auto animate
    int         rdflags;            // RDF_UNDERWATER, etc

    byte        *areabits;          // if not NULL, only areas with set bits will be drawn

    lightstyle_t    *lightstyles;   // [MAX_LIGHTSTYLES]

    int         num_entities;
    entity_t    *entities;

    int         num_dlights;
    dlight_t    *dlights;

    int         num_particles;
    particle_t  *particles;

    int             num_blood_spheres;
    blood_sphere_t  *blood_spheres;

    int         decal_beg;
    int         decal_end;
    decal_t     decal[MAX_DECALS];

	ref_feedback_t feedback;
} refdef_t;

typedef enum {
    QVF_ACCELERATED     = (1 << 0),
    QVF_GAMMARAMP       = (1 << 1),
    QVF_FULLSCREEN      = (1 << 2)
} vidFlags_t;

typedef struct {
    int         width;
    int         height;
    vidFlags_t  flags;
} refcfg_t;

extern refcfg_t r_config;

typedef struct {
    int left, right, top, bottom;
} clipRect_t;

typedef enum {
    IF_NONE         = 0,
    IF_PERMANENT    = (1 << 0),
    IF_TRANSPARENT  = (1 << 1),
    IF_PALETTED     = (1 << 2),
    IF_UPSCALED     = (1 << 3),
    IF_SCRAP        = (1 << 4),
    IF_TURBULENT    = (1 << 5),
    IF_REPEAT       = (1 << 6),
    IF_NEAREST      = (1 << 7),
    IF_OPAQUE       = (1 << 8),
    IF_SRGB         = (1 << 9),
    IF_FAKE_EMISSIVE= (1 << 10),
    IF_EXACT        = (1 << 11),
    IF_NORMAL_MAP   = (1 << 12),
    IF_BILERP       = (1 << 13), // always lerp, independent of bilerp_pics cvar
    IF_NO_MIPMAPS   = (1 << 14), // upload mip 0 only; for images drawn 1:1 and replaced every frame

    // Image source indicator/requirement flags
    IF_SRC_BASE     = (0x1 << 16),
    IF_SRC_GAME     = (0x2 << 16),
    IF_SRC_MASK     = (0x3 << 16),
} imageflags_t;

// Shift amount for storing fake emissive synthesis threshold
#define IF_FAKE_EMISSIVE_THRESH_SHIFT  20

typedef enum {
    IT_PIC,
    IT_FONT,
    IT_SKIN,
    IT_SPRITE,
    IT_WALL,
    IT_SKY,

    IT_MAX
} imagetype_t;

typedef enum ref_type_e
{
    REF_TYPE_NONE = 0,
    REF_TYPE_GL,
    REF_TYPE_VKPT
} ref_type_t;

// called when the library is loaded
extern ref_type_t  (*R_Init)(bool total);

// called before the library is unloaded
extern void        (*R_Shutdown)(bool total);

// All data that will be used in a level should be
// registered before rendering any frames to prevent disk hits,
// but they can still be registered at a later time
// if necessary.
//
// EndRegistration will free any remaining data that wasn't registered.
// Any model_s or skin_s pointers from before the BeginRegistration
// are no longer valid after EndRegistration.
//
// Skins and images need to be differentiated, because skins
// are flood filled to eliminate mip map edge errors, and pics have
// an implicit "pics/" prepended to the name. (a pic name that starts with a
// slash will not use the "pics/" prefix or the ".pcx" postfix)
extern void    (*R_BeginRegistration)(const char *map);
qhandle_t R_RegisterModel(const char *name);
qhandle_t R_RegisterImage(const char *name, imagetype_t type,
                          imageflags_t flags, int *err_p);
qhandle_t R_RegisterRawImage(const char *name, int width, int height, byte* pic, imagetype_t type,
                          imageflags_t flags);
void R_UnregisterImage(qhandle_t handle);

extern void    (*R_SetSky)(const char *name, float rotate, int autorotate, const vec3_t axis);
extern void    (*R_EndRegistration)(void);

#define R_RegisterPic(name)     R_RegisterImage(name, IT_PIC, IF_PERMANENT | IF_SRGB, NULL)
#define R_RegisterPic2(name)    R_RegisterImage(name, IT_PIC, IF_SRGB, NULL)
#define R_RegisterFont(name)    R_RegisterImage(name, IT_FONT, IF_PERMANENT | IF_SRGB, NULL)
#define R_RegisterSkin(name)    R_RegisterImage(name, IT_SKIN, IF_SRGB, NULL)

extern void    (*R_RenderFrame)(refdef_t *fd, int waterLevel);
extern void    (*R_LightPoint)(const vec3_t origin, vec3_t light);

extern void    (*R_ClearColor)(void);
extern void    (*R_SetAlpha)(float clpha);
extern void    (*R_SetAlphaScale)(float alpha);
extern void    (*R_SetColor)(uint32_t color);
extern void    (*R_SetClipRect)(const clipRect_t *clip);
float   R_ClampScale(cvar_t *var);
extern void    (*R_SetScale)(float scale);
extern void    (*R_DrawChar)(int x, int y, int flags, int ch, qhandle_t font);
extern int     (*R_DrawString)(int x, int y, int flags, size_t maxChars,
                     const char *string, qhandle_t font);  // returns advanced x coord
bool R_GetPicSize(int *w, int *h, qhandle_t pic);   // returns transparency bit
extern void    (*R_DrawPic)(int x, int y, qhandle_t pic);
extern void    (*R_DrawStretchPic)(int x, int y, int w, int h, qhandle_t pic);
extern void    (*R_TileClear)(int x, int y, int w, int h, qhandle_t pic);
extern void    (*R_DrawFill8)(int x, int y, int w, int h, int c);
extern void    (*R_DrawFill32)(int x, int y, int w, int h, uint32_t color);

// video mode and refresh state management entry points
extern void    (*R_BeginFrame)(void);
extern void    (*R_EndFrame)(void);
extern void    (*R_ModeChanged)(int width, int height, int flags, int rowbytes, void *pixels);

// add decal to ring buffer
extern void    (*R_AddDecal)(decal_t *d);

extern bool    (*R_InterceptKey)(unsigned key, bool down);
extern bool    (*R_IsHDR)(void);

// NVIDIA Reflex: blocks until the driver judges this frame should begin. Called at the
// top of the client frame, BEFORE input is sampled - that ordering is what turns it
// into a latency reduction rather than just a frame limiter. NULL on renderers that
// do not implement it.
extern void    (*R_LatencySleep)(void);

#if REF_GL
void R_RegisterFunctionsGL(void);
#endif
#if REF_VKPT
void R_RegisterFunctionsRTX(void);
#endif

#endif // REFRESH_H
