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
// in a closed renderer. Taken raw, fog_density 0.03 would mean 50% visibility at
// 23 units - pea soup - so there is certainly a scale factor we cannot read.
// cl_fog_scale exists to calibrate it against retail screenshots by eye.

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
static cvar_t   *cl_fog_scale;

// [cl_fog 3] The density that mode 3 uses, separate from cl_fog_scale because
// the two modes need wildly different numbers for the same map. See the note at
// its registration in CL_InitMapFog.
static cvar_t   *cl_volumetric_fog_density;

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

    out->valid = is_world && got_any;
    return out->valid;
}

void CL_InitMapFog(void)
{
    // Registered at client init, not lazily at map load, because the video
    // menu binds to these by name when it is built.
    // DEFAULT OFF while the volumetric is still being finished - see
    // [[q2rtx-rerelease-fog]]. 1 = sun-lit medium, 2 = lit by the map's own
    // lights with traced sky visibility.
    cl_fog = Cvar_Get("cl_fog", "0", CVAR_ARCHIVE);

    // The local-light brightness knob for cl_fog 2. Lives in the UBO cvar list
    // (appended at the END - see the alignment memory) so the shader reads it.
    Cvar_Get("pt_fog_light_scale", "1.0", CVAR_ARCHIVE);

    // The calibration knob. 1.0 means "the map's authored value, taken raw".
    // KEX's density-to-extinction constant is inside a closed renderer and
    // cannot be derived, so this is matched against retail by eye.
    //
    // 2 rather than 1: Matt's calibration, 2026-08-31, once the sky brushes
    // stopped being counted as area lights in the fog. The authored densities
    // read thin on their own at the light levels these maps actually use.
    cl_fog_scale = Cvar_Get("cl_fog_scale", "2", CVAR_ARCHIVE);

    /* THE SAME MAP NEEDS A DIFFERENT DENSITY IN MODE 1 AND MODE 3, and the
       difference is not a matter of taste - it falls out of how each mode is lit.

       cl_fog 1's only term is the sun through a HARD shadow cliff
       (pt_fog_ambient defaults to 0), so most of the volume is multiplied by
       ZERO and the scale has to be enormous for the handful of lit steps to
       carry the look: mgu1m1 ships mapcvar cl_fog_scale 250. Switch that map to
       cl_fog 3 and nothing is zeroed any more - every step is lit by something -
       so the same 250 over-amplifies the whole volume, and faint sun leakage in
       dim interiors becomes visible haze. Matt hit exactly that on mgu1m1.

       -1 means "no opinion, use cl_fog_scale", which is what every map that has
       not been split yet wants. So this changes NOTHING until it is set, and a
       map cfg can then carry both numbers and switch modes without a retune. */
    cl_volumetric_fog_density = Cvar_Get("cl_volumetric_fog_density", "-1", CVAR_ARCHIVE);
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

Fills the renderer-side description. Returns false when this map has no fog or
the feature is switched off, in which case the caller leaves the medium alone.
=================
*/
bool CL_GetMapFog(mapfog_params_t *out)
{
    float scale;

    if (!cl_mapfog.valid || !cl_fog || !cl_fog->integer)
        return false;

    /* cl_fog_scale is the SKY/SUN density and it applies in EVERY mode - a
       first version of this switched the whole density over to
       cl_volumetric_fog_density in mode 3, which meant one knob drove both
       halves and the other did nothing. That is not the split: mode 3 renders
       BOTH the sun term (which is cl_fog 1's fog, and wants cl_fog_scale) and
       the local-light term (which wants its own, much lower number) at the same
       time, so the two densities have to coexist rather than take turns. */
    scale = cl_fog_scale ? cl_fog_scale->value : 1.0f;

    if (scale <= 0.0f)
        return false;

    /* The volumetric half is expressed as a RATIO against that, because the
       densities handed to the renderer carry the map's height-fog profile and
       only their magnitude should move. Negative means "no opinion, match the
       sky density", which is the default and leaves every existing map alone. */
    out->vol_density_ratio = 1.0f;
    if (cl_volumetric_fog_density && cl_volumetric_fog_density->value >= 0.0f)
        out->vol_density_ratio = cl_volumetric_fog_density->value / scale;

    out->mode       = cl_fog->integer;
    out->density    = cl_mapfog.density * scale;
    out->hf_density = cl_mapfog.hf_density * scale;
    out->hf_falloff = cl_mapfog.hf_falloff;
    out->hf_start_z = cl_mapfog.hf_start_z;
    out->hf_end_z   = cl_mapfog.hf_end_z;
    VectorCopy(cl_mapfog.color, out->color);
    VectorCopy(cl_mapfog.hf_start_color, out->hf_start_color);
    VectorCopy(cl_mapfog.hf_end_color, out->hf_end_color);

    return (out->density > 0.0f) || (out->hf_density > 0.0f);
}
