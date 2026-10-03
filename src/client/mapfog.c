/*
Copyright (C) 2026 Q2RTX rerelease port

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
// mapfog.c -- the rerelease's per-map atmospheric fog
//
// 67 of the 76 in-scope rerelease maps set fog on WORLDSPAWN, and 49 also set a
// plain distance fog. That - not trigger_fog - is the fog system: worldspawn
// establishes the map's baseline and trigger_fog only modifies it as the player
// moves. Without this the maps render with no atmosphere at all.
//
// Read on the CLIENT out of the BSP entity lump, exactly like dynamiclights.c,
// because worldspawn keys are static for the level and nothing needs to go over
// the wire. (trigger_fog is genuinely per-player server state and is a separate,
// later job.)
//
// WHAT THE RERELEASE DOES WITH THESE, AND WHAT WE DO INSTEAD
//
// KEX renders this as an analytic per-pixel tint computed from depth and world
// height - a filter over the framebuffer. It cannot be lit and casts nothing.
// Matt's call was that that will always look wrong on top of path-traced
// geometry, so these values instead drive the DENSITY of the volumetric medium
// the god-ray pass already marches. Same numbers, real single scattering.
//
// The keys, measured across every shipped BSP rather than assumed:
//
//   heightfog_density      67 maps   the altitude-banded fog, 0.0001 .. 0.0024
//   heightfog_start_dist   67 maps   world Z of the TOP of the band
//   heightfog_end_dist     67 maps   world Z of the BOTTOM of the band
//   heightfog_start_color  67 maps   linear rgb at the top
//   heightfog_end_color    65 maps   linear rgb at the bottom
//   heightfog_falloff      65 maps   exponential rate with height, ~0.01 .. 0.04
//   fog_color              65 maps   linear rgb of the plain distance fog
//   heightfog_density is the dominant one; see fog_density below
//   fog_density            49 maps   plain distance fog, 0.01 .. 0.05
//   fog_sky_factor         42 maps   how much the SKY is fogged - not used here,
//                                    the path tracer's sky is a real environment
//
// NOTE ON "dist": heightfog_start_dist / _end_dist are named "dist" in id's own
// spawn documentation but they are world Z HEIGHTS, not distances, and start is
// ABOVE end (base1 runs -256 down to -768; mgu1m2 +357 down to -433). Do not
// treat them as a near/far pair.
//
// THE DENSITY SCALE IS NOT DERIVABLE. KEX's density-to-extinction constant lives
// in a closed renderer, so the authored numbers cannot be taken literally. They
// used to be, times a cl_fog_scale that every map then needed its own value of
// (300 on the id maps, 4 on the MGU ones, 1000 left in a config), and the same
// menu settings looked completely different from one map to the next and from
// the classic campaign to the rerelease.
//
// So the authored values now give the fog its SHAPE and nothing else: the
// densities are normalised so the thickest point of the medium is 1 - exactly
// the flat medium the classic campaign's god rays have always marched. How
// bright the fog is comes only from the five pt_fog_scale_* knobs (sun, skybox,
// emissive, dynamic, model) plus each light's own volscale.
//
// THE MAP CFGS WERE CONVERTED, NOT RETUNED. rerelease/maps/*.cfg used to carry
// cl_fog_scale S and cl_volumetric_fog_density V. With P the old peak density of
// the map's own medium (hf_density / 0.025 + fog_density / 2), the old renderer
// drew the sun at density P*S times gr_intensity (2) times the fog colour, and
// the local lights at density P*V. So each cfg now sets, for the same picture:
//
//   pt_fog_scale_sun        P * S * 2 * luminance(fog colour)
//                           (the colour itself is dropped under the physical sky)
//   pt_fog_scale_<source>   P * V / FOG_LOCAL_DENSITY (0.05) * its old scale
//   pt_fog_scale_skybox     the same, times its old pt_fog_sky_scale
//
// mapcvar still restores the player's own values on the next map, so these are
// per-map overrides of the menu knobs, not new defaults.

#include "client.h"

typedef struct {
    bool    valid;

    float   density;            // plain distance fog
    vec3_t  color;

    float   hf_density;         // altitude-banded fog
    float   hf_falloff;
    float   hf_start_z;
    float   hf_end_z;
    vec3_t  hf_start_color;
    vec3_t  hf_end_color;
} mapfog_t;

static mapfog_t cl_mapfog;

static cvar_t   *cl_fog;

/* The normalisation constants. These MUST match FOG_HEIGHTFOG_REFERENCE and
   FOG_DISTANCEFOG_REFERENCE in fog_medium.glsl, which divides the values handed
   to it by them: the shader's density at the floor of the band is
   hf_density / HF_REF + density / DIST_REF, and that is what is scaled to 1. */
#define MAPFOG_HEIGHTFOG_REFERENCE   0.025f
#define MAPFOG_DISTANCEFOG_REFERENCE 2.0f

/*
=================
CL_ParseWorldspawnFog

Walks the first entity block of the lump. worldspawn is always entity 0, so
there is no need to scan the rest.
=================
*/
static bool CL_ParseWorldspawnFog(const char *data, mapfog_t *out)
{
    char        key[64];
    const char  *token;
    bool        is_world = false;
    bool        got_any = false;

    memset(out, 0, sizeof(*out));

    token = COM_Parse(&data);
    if (strcmp(token, "{"))
        return false;

    while (1) {
        token = COM_Parse(&data);
        if (!data && !*token)
            return false;       // unterminated block
        if (!strcmp(token, "}"))
            break;

        Q_strlcpy(key, token, sizeof(key));

        token = COM_Parse(&data);
        if (!data && !*token)
            return false;

        if (!strcmp(key, "classname")) {
            if (!strcmp(token, "worldspawn"))
                is_world = true;
        } else if (!strcmp(key, "fog_density")) {
            out->density = atof(token);
            got_any = true;
        } else if (!strcmp(key, "fog_color")) {
            sscanf(token, "%f %f %f", &out->color[0], &out->color[1], &out->color[2]);
        } else if (!strcmp(key, "heightfog_density")) {
            out->hf_density = atof(token);
            got_any = true;
        } else if (!strcmp(key, "heightfog_falloff")) {
            // several maps carry a typo'd value such as "0..01"; atof stops at
            // the second dot and yields 0, which is a harmless "no falloff"
            out->hf_falloff = atof(token);
        } else if (!strcmp(key, "heightfog_start_dist")) {
            out->hf_start_z = atof(token);
        } else if (!strcmp(key, "heightfog_end_dist")) {
            out->hf_end_z = atof(token);
        } else if (!strcmp(key, "heightfog_start_color")) {
            sscanf(token, "%f %f %f", &out->hf_start_color[0], &out->hf_start_color[1], &out->hf_start_color[2]);
        } else if (!strcmp(key, "heightfog_end_color")) {
            sscanf(token, "%f %f %f", &out->hf_end_color[0], &out->hf_end_color[1], &out->hf_end_color[2]);
        }
    }

    /* The renderer takes start_z as the TOP of the band and end_z as the BOTTOM,
       which is how most maps author it - but not all. mgu2m2 (-128 .. 580) and
       mgu2m3 (-864 .. -256) have start BELOW end. KEX keeps each colour at its
       own key's height and the fog thick at the LOW end either way: mgu2m2 in
       the rerelease is dense blue-grey down low fading into orange up high.
       Taken as authored, the band came out negative, getHeightFogDensity went
       flat and getFogColor pinned end_color everywhere - one solid orange.
       Swapping the heights WITH their colours keeps every colour where the
       mapper put it and hands the shader the top/bottom order it expects. */
    if (out->hf_start_z < out->hf_end_z) {
        vec3_t tmp;
        float z = out->hf_start_z;
        out->hf_start_z = out->hf_end_z;
        out->hf_end_z = z;
        VectorCopy(out->hf_start_color, tmp);
        VectorCopy(out->hf_end_color, out->hf_start_color);
        VectorCopy(tmp, out->hf_end_color);
    }

    out->valid = is_world && got_any;
    return out->valid;
}

// Fog is on or off. The old 1/2/3 modes are gone: 1 is what was mode 3, the
// froxel volumetric, and anything an old config left above 1 means "on".
static void cl_fog_changed(cvar_t *self)
{
    if (self->integer > 1)
        Cvar_SetInteger(self, 1, FROM_CODE);
    else if (self->integer < 0)
        Cvar_SetInteger(self, 0, FROM_CODE);
}

void CL_InitMapFog(void)
{
    // Registered at client init, not lazily at map load, because the video
    // menu binds to these by name when it is built.
    cl_fog = Cvar_Get("cl_fog", "0", CVAR_ARCHIVE);
    cl_fog->changed = cl_fog_changed;
    cl_fog_changed(cl_fog);

    /* THE FIVE FOG KNOBS - the only things that set how bright the fog is.
       Registered here only to make them ARCHIVE; the renderer owns them (the
       first two are UBO cvars in global_ubo.h, the other three are read in
       vertex_buffer.c) and the defaults must match theirs. A map cfg may set
       any of them with mapcvar. */
    Cvar_Get("pt_fog_scale_sun",      "2.0", CVAR_ARCHIVE);   // physical sky
    Cvar_Get("pt_fog_scale_skybox",   "1.0", CVAR_ARCHIVE);   // the map's skybox
    Cvar_Get("pt_fog_scale_emissive", "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_scale_dynamic",  "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_scale_model",    "1.0", CVAR_ARCHIVE);
    Cvar_Get("pt_fog_lava_scale",     "5.0", CVAR_ARCHIVE);   // lava, x10 in vertex_buffer.c

    // Not a brightness knob - the quality of the physical sky's fog, fast or
    // accurate. See its note in global_ubo.h; the default must match it.
    Cvar_Get("pt_fog_sky_sun_only",   "1",   CVAR_ARCHIVE);
}

/*
=================
CL_LoadMapFog

Called once per map, after the BSP is loaded.
=================
*/
void CL_LoadMapFog(void)
{
    memset(&cl_mapfog, 0, sizeof(cl_mapfog));

    if (!cl.bsp || !cl.bsp->entitystring)
        return;

    if (!CL_ParseWorldspawnFog(cl.bsp->entitystring, &cl_mapfog))
        return;

    // a band with no thickness would divide by zero in the shader
    if (cl_mapfog.hf_start_z <= cl_mapfog.hf_end_z)
        cl_mapfog.hf_density = 0.0f;

    Com_DPrintf("map fog: density %g, heightfog %g z %.0f..%.0f falloff %g\n",
                cl_mapfog.density, cl_mapfog.hf_density,
                cl_mapfog.hf_end_z, cl_mapfog.hf_start_z, cl_mapfog.hf_falloff);
}

void CL_FreeMapFog(void)
{
    memset(&cl_mapfog, 0, sizeof(cl_mapfog));
}

/*
=================
CL_GetMapFog

Fills the renderer-side description. Returns false only when fog is switched
off, in which case the renderer falls back to the classic god rays. A map with
no fog of its own still gets a medium - a flat white one - so turning fog on
looks the same on the classic campaign as on the rerelease.
=================
*/
bool CL_GetMapFog(mapfog_params_t *out)
{
    float peak;

    if (!cl_fog || !cl_fog->integer)
        return false;

    memset(out, 0, sizeof(*out));
    out->mode = 3;

    peak = 0.0f;
    if (cl_mapfog.valid)
        peak = cl_mapfog.hf_density / MAPFOG_HEIGHTFOG_REFERENCE
             + cl_mapfog.density / MAPFOG_DISTANCEFOG_REFERENCE;

    if (peak <= 0.0f) {
        out->density = MAPFOG_DISTANCEFOG_REFERENCE;    // density 1 everywhere
        VectorSet(out->color, 1, 1, 1);
        return true;
    }

    // Keep the shape, drop the magnitude: the band floor comes out at 1.
    out->density    = cl_mapfog.density / peak;
    out->hf_density = cl_mapfog.hf_density / peak;
    out->hf_falloff = cl_mapfog.hf_falloff;
    out->hf_start_z = cl_mapfog.hf_start_z;
    out->hf_end_z   = cl_mapfog.hf_end_z;
    VectorCopy(cl_mapfog.color, out->color);
    VectorCopy(cl_mapfog.hf_start_color, out->hf_start_color);
    VectorCopy(cl_mapfog.hf_end_color, out->hf_end_color);

    return true;
}
