/*
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
// lightedit.c -- placing dynamic lights from inside the game
//
// A level-design tool rather than a game feature. The rerelease's own lighting
// is the dynamic_light entities baked into each BSP (dynamiclights.c); these
// are the same kind of light - an analytic sphere light handed to the path
// tracer - but placed by hand from the console and kept in a per-map text file
// instead of in the map. That is the whole point: a light can be placed,
// judged, retuned and deleted while looking at the room it lights, with no map
// compile in between.
//
//   light place <r> <g> <b> <brightness> [radius]   put one in front of the camera
//   light edit  <r> <g> <b> <brightness> [radius]   retarget the one under the crosshair
//   light print                                     the one under the crosshair, or all
//   light delete                                    remove the one under the crosshair
//   light debug_on / light debug_off                show the lights as colored spheres
//   light reload                                    re-read the file from disk
//
// r, g, b and brightness are all 0..255, as typed. radius is in world units
// and is the size of the emitter sphere, which is what softens the shadow
// edge; it also sets the size of the debug marker and of the crosshair pick
// target, so what is on screen in debug mode is exactly what gets picked.
//
// The file is <gamedir>/maps/lights/<mapname>.cfg, rewritten in full after
// every change - edit and delete need a rewrite anyway, so append would only
// be a second code path to get wrong. It is read back through the normal
// search path, so a finished set can be packed into a .pkz.
//
// These lights are deliberately NOT gated by cl_dynamic_lights. That cvar
// exists to turn the map's baked lighting off, and a hand-placed light has to
// stay lit while exactly that comparison is being made. light_enable is its
// own switch.
//

#include "client.h"

// The renderer's whole frame budget is MAX_DLIGHTS (256), shared with the
// map's own dynamic_lights, so there is no point in a larger array here.
#define MAX_EDIT_LIGHTS     256

typedef struct {
    vec3_t  origin;
    byte    rgb[3];         // 0..255, exactly as typed
    byte    brightness;     // 0..255, exactly as typed
    float   radius;         // world units - emitter size, marker size, pick size
} editlight_t;

static editlight_t  *le_lights;
static int          le_num_lights;
static bool         le_debug;

static cvar_t   *light_enable;
static cvar_t   *light_scale;
static cvar_t   *light_place_dist;
static cvar_t   *light_pick_size;
static cvar_t   *light_debug_brightness;

// Emitter size for a "light place" that does not name one. 10 is what
// V_AddLight uses for an ordinary point light.
#define LIGHT_DEFAULT_RADIUS    10.0f

// A typed 0..255 value. Out of range is clamped rather than rejected - a 256
// typed for brightness is obviously meant to be full brightness.
static byte LE_ParseByte(const char *s)
{
    int v = atoi(s);

    clamp(v, 0, 255);
    return v;
}

// A typed radius, falling back to the default for a missing or nonsense one.
static float LE_ParseRadius(const char *s)
{
    float r = s ? atof(s) : 0.0f;

    return r > 0.0f ? r : LIGHT_DEFAULT_RADIUS;
}

/*
=================
LE_Active

Every subcommand that touches the list needs a loaded map: the file name comes
from cl.mapname and the placement trace needs cl.bsp.
=================
*/
static bool LE_Active(void)
{
    if (cls.state != ca_active || !cl.bsp || !cl.mapname[0]) {
        Com_Printf("Must be in a level.\n");
        return false;
    }

    if (!le_lights)
        le_lights = Z_Mallocz(sizeof(editlight_t) * MAX_EDIT_LIGHTS);

    return true;
}

/*
=================================================================

  the file

=================================================================
*/

/*
=================
LE_LoadLights

Called once per map load. A missing file is the normal case and is silent.
=================
*/
void LE_LoadLights(void)
{
    char    path[MAX_QPATH];
    char    *buffer, *s, *p;
    int     line = 0, ret;

    LE_FreeLights();

    if (!cl.mapname[0])
        return;

    Q_concat(path, sizeof(path), "maps/lights/", cl.mapname, ".cfg");

    ret = FS_LoadFile(path, (void **)&buffer);
    if (!buffer) {
        if (ret != Q_ERR(ENOENT))
            Com_EPrintf("Couldn't load %s: %s\n", path, Q_ErrorString(ret));
        return;
    }

    le_lights = Z_Mallocz(sizeof(editlight_t) * MAX_EDIT_LIGHTS);

    s = buffer;
    while (*s) {
        editlight_t *l;
        const char  *t;
        int         argc;

        p = strchr(s, '\n');
        if (p)
            *p = 0;

        line++;

        // Cmd_TokenizeString has no idea what a comment is - it would hand
        // back the words of the header this file is written with as a light -
        // so the // lines have to be dropped here.
        t = s + strspn(s, " \t\r");
        if (t[0] == '/' && t[1] == '/') {
            if (!p)
                break;
            s = p + 1;
            continue;
        }

        Cmd_TokenizeString(s, false);
        argc = Cmd_Argc();

        if (argc) {
            // x y z  r g b  brightness  [radius]
            if (argc < 7) {
                Com_WPrintf("Line %d is incomplete in %s\n", line, path);
            } else if (le_num_lights == MAX_EDIT_LIGHTS) {
                Com_WPrintf("Too many lights in %s, stopping at %d\n",
                            path, MAX_EDIT_LIGHTS);
                break;
            } else {
                l = &le_lights[le_num_lights++];

                for (int i = 0; i < 3; i++)
                    l->origin[i] = atof(Cmd_Argv(i));
                for (int i = 0; i < 3; i++)
                    l->rgb[i] = LE_ParseByte(Cmd_Argv(3 + i));

                l->brightness = LE_ParseByte(Cmd_Argv(6));
                l->radius = LE_ParseRadius(argc > 7 ? Cmd_Argv(7) : NULL);
            }
        }

        if (!p)
            break;

        s = p + 1;
    }

    Com_Printf("Loaded %d placed light%s from %s\n",
               le_num_lights, le_num_lights == 1 ? "" : "s", path);

    FS_FreeFile(buffer);
}

void LE_FreeLights(void)
{
    Z_Free(le_lights);
    le_lights = NULL;
    le_num_lights = 0;
}

/*
=================
LE_WriteLights

Rewrites the whole file, including when the list has just become empty -
otherwise deleting the last light would leave it behind on disk.
=================
*/
static void LE_WriteLights(void)
{
    char        buffer[MAX_OSPATH];
    qhandle_t   f;

    f = FS_EasyOpenFile(buffer, sizeof(buffer), FS_MODE_WRITE | FS_FLAG_TEXT,
                        "maps/lights/", cl.mapname, ".cfg");
    if (!f)
        return;

    FS_FPrintf(f, "// placed lights for %s - written by \"light place\"\n", cl.mapname);
    FS_FPrintf(f, "// x y z   r g b   brightness   radius\n");

    for (int i = 0; i < le_num_lights; i++) {
        const editlight_t *l = &le_lights[i];

        FS_FPrintf(f, "%.1f %.1f %.1f   %d %d %d   %d   %.1f\n",
                   l->origin[0], l->origin[1], l->origin[2],
                   l->rgb[0], l->rgb[1], l->rgb[2],
                   l->brightness, l->radius);
    }

    if (FS_CloseFile(f))
        Com_EPrintf("Error writing %s\n", buffer);
    else
        Com_Printf("Wrote %d light%s to %s\n",
                   le_num_lights, le_num_lights == 1 ? "" : "s", buffer);
}

/*
=================================================================

  picking

  The crosshair is the view axis, so a light is "under the crosshair"
  when its marker sphere intersects the view ray. Nearest along the
  ray wins, which is what the eye expects when two overlap.

=================================================================
*/
static editlight_t *LE_PickLight(void)
{
    editlight_t *best = NULL;
    float       best_along = 0.0f;

    for (int i = 0; i < le_num_lights; i++) {
        editlight_t *l = &le_lights[i];
        vec3_t      delta, perp;
        float       along, pick;

        VectorSubtract(l->origin, cl.refdef.vieworg, delta);

        along = DotProduct(delta, cl.v_forward);
        if (along <= 0.0f)
            continue;               // behind the camera

        // A small light is an unhittable target from across a room, so the
        // pick radius has a floor. This is the only place the marker size and
        // the pick size are allowed to disagree.
        pick = max(l->radius, light_pick_size->value);

        VectorMA(delta, -along, cl.v_forward, perp);
        if (VectorLength(perp) > pick)
            continue;

        if (!best || along < best_along) {
            best = l;
            best_along = along;
        }
    }

    return best;
}

static void LE_PrintLight(const editlight_t *l)
{
    Com_Printf("light %d: at %.0f %.0f %.0f   rgb %d %d %d   brightness %d   radius %.1f\n",
               (int)(l - le_lights), l->origin[0], l->origin[1], l->origin[2],
               l->rgb[0], l->rgb[1], l->rgb[2], l->brightness, l->radius);
}

/*
=================
LE_ParseValues

The shared argument list of "place" and "edit": four 0..255 bytes and an
optional radius.
=================
*/
static bool LE_ParseValues(editlight_t *out, int first)
{
    if (Cmd_Argc() < first + 4) {
        Com_Printf("Usage: %s %s <r> <g> <b> <brightness> [radius]\n",
                   Cmd_Argv(0), Cmd_Argv(1));
        return false;
    }

    for (int i = 0; i < 3; i++)
        out->rgb[i] = LE_ParseByte(Cmd_Argv(first + i));

    out->brightness = LE_ParseByte(Cmd_Argv(first + 3));
    out->radius = LE_ParseRadius(Cmd_Argc() > first + 4 ? Cmd_Argv(first + 4) : NULL);

    return true;
}

/*
=================================================================

  the subcommands

=================================================================
*/

static void LE_Place_f(void)
{
    editlight_t *l, values;
    vec3_t      end;
    trace_t     trace;

    if (le_num_lights == MAX_EDIT_LIGHTS) {
        Com_Printf("Already at the %d light limit.\n", MAX_EDIT_LIGHTS);
        return;
    }

    if (!LE_ParseValues(&values, 2))
        return;

    l = &le_lights[le_num_lights];
    *l = values;

    // light_place_dist in front of the eye, or against whatever is nearer
    // than that. Landing on a surface is the common case when lighting a wall
    // fixture, so back the emitter off the plane by its own radius - a sphere
    // light buried in a wall lights nothing.
    VectorMA(cl.refdef.vieworg, light_place_dist->value, cl.v_forward, end);
    CM_BoxTrace(&trace, cl.refdef.vieworg, end, vec3_origin, vec3_origin,
                cl.bsp->nodes, MASK_SOLID);

    // startsolid leaves no plane to push off of - the camera is already inside
    // something, which happens with noclip, so just take the eye position
    VectorCopy(trace.endpos, l->origin);
    if (trace.fraction < 1.0f && !trace.startsolid)
        VectorMA(l->origin, l->radius + 1.0f, trace.plane.normal, l->origin);

    le_num_lights++;

    LE_PrintLight(l);
    LE_WriteLights();
}

static void LE_Edit_f(void)
{
    editlight_t *l, values;

    l = LE_PickLight();
    if (!l) {
        Com_Printf("No light under the crosshair.\n");
        return;
    }

    if (!LE_ParseValues(&values, 2))
        return;

    // the origin is not part of the edit - it is set by where the light was
    // placed, and there is no way to express a new one from here
    memcpy(l->rgb, values.rgb, sizeof(l->rgb));
    l->brightness = values.brightness;
    l->radius = values.radius;

    LE_PrintLight(l);
    LE_WriteLights();
}

static void LE_Print_f(void)
{
    const editlight_t *l;

    if (!le_num_lights) {
        Com_Printf("No placed lights in %s.\n", cl.mapname);
        return;
    }

    // the one being aimed at if there is one, since that is what edit and
    // delete would act on; the whole list otherwise
    l = LE_PickLight();
    if (l) {
        LE_PrintLight(l);
        return;
    }

    Com_Printf("%d placed light%s in %s:\n",
               le_num_lights, le_num_lights == 1 ? "" : "s", cl.mapname);

    for (int i = 0; i < le_num_lights; i++)
        LE_PrintLight(&le_lights[i]);
}

static void LE_Delete_f(void)
{
    editlight_t *l = LE_PickLight();
    int         index;

    if (!l) {
        Com_Printf("No light under the crosshair.\n");
        return;
    }

    index = (int)(l - le_lights);
    Com_Printf("Deleted light %d.\n", index);

    memmove(l, l + 1, sizeof(editlight_t) * (le_num_lights - index - 1));
    le_num_lights--;

    LE_WriteLights();
}

static void LE_Light_f(void)
{
    const char *cmd = Cmd_Argv(1);

    if (!*cmd) {
        Com_Printf("Usage: %s <place|edit|print|delete|debug_on|debug_off|reload>\n",
                   Cmd_Argv(0));
        return;
    }

    // debug_on/off are the only ones that mean anything without a map
    if (!strcmp(cmd, "debug_on")) {
        le_debug = true;
        Com_Printf("Light debug markers on.\n");
        return;
    }

    if (!strcmp(cmd, "debug_off")) {
        le_debug = false;
        Com_Printf("Light debug markers off.\n");
        return;
    }

    if (!LE_Active())
        return;

    if (!strcmp(cmd, "place"))
        LE_Place_f();
    else if (!strcmp(cmd, "edit"))
        LE_Edit_f();
    else if (!strcmp(cmd, "print"))
        LE_Print_f();
    else if (!strcmp(cmd, "delete"))
        LE_Delete_f();
    else if (!strcmp(cmd, "reload"))
        LE_LoadLights();
    else
        Com_Printf("Unknown subcommand \"%s\".\n", cmd);
}

static void LE_Light_c(genctx_t *ctx, int argnum)
{
    static const char *const subcommands[] = {
        "place", "edit", "print", "delete", "debug_on", "debug_off", "reload", NULL
    };

    if (argnum != 1)
        return;

    for (int i = 0; subcommands[i]; i++)
        Prompt_AddMatch(ctx, subcommands[i]);
}

static const cmdreg_t c_lightedit[] = {
    { "light", LE_Light_f, LE_Light_c },

    { NULL }
};

/*
=================================================================

  rendering

=================================================================
*/

/*
=================
LE_AddLightsToScene

Called every frame from CL_AddEntities, after the view axis is set up.
=================
*/
void LE_AddLightsToScene(void)
{
    const editlight_t   *picked = NULL;
    float               scale;

    if (!le_num_lights)
        return;

    // The markers are ordinary particles, which the vkpt back end draws as
    // world-space camera-facing quads of exactly particle_t.radius - the same
    // thing cl_show_lights does for the map's lights. The lights themselves
    // are analytic and only exist in the path tracer.
    if (le_debug)
        picked = LE_PickLight();

    scale = light_scale->value;

    for (int i = 0; i < le_num_lights; i++) {
        const editlight_t *l = &le_lights[i];
        vec3_t  color;
        float   intensity;

        VectorSet(color, l->rgb[0] / 255.0f, l->rgb[1] / 255.0f, l->rgb[2] / 255.0f);

        if (le_debug) {
            particle_t p;

            memset(&p, 0, sizeof(p));
            VectorCopy(l->origin, p.origin);
            p.color = -1;               // use rgba rather than a palette index
            p.rgba.u8[0] = l->rgb[0];
            p.rgba.u8[1] = l->rgb[1];
            p.rgba.u8[2] = l->rgb[2];
            p.rgba.u8[3] = 255;
            p.alpha = 1.0f;
            p.radius = l->radius;       // world units, so the marker is the emitter

            // whichever one edit and delete would act on reads brighter, so
            // there is never a question of which light is selected
            p.brightness = light_debug_brightness->value * (l == picked ? 4.0f : 1.0f);

            V_AddParticle(&p);
        }

        if (!light_enable->integer)
            continue;

        if (cls.ref_type != REF_TYPE_VKPT)
            continue;

        // brightness is 0..255 as typed; light_scale is what maps that onto
        // the engine's light units, where the map's own dynamic_lights land
        // somewhere around 128..2048 after cl_dynamic_light_scale
        intensity = (l->brightness / 255.0f) * scale;
        if (intensity <= 0.0f)
            continue;

        V_AddSphereLight(l->origin, intensity, color[0], color[1], color[2], l->radius);
    }
}

void LE_Init(void)
{
    // Hand-placed lights are a separate system from the map's baked
    // dynamic_light set on purpose, so cl_dynamic_lights 0 does not take them
    // with it. This is their own switch.
    light_enable = Cvar_Get("light_enable", "1", CVAR_ARCHIVE);

    // Maps "brightness 255" onto engine light units. The map's own lights come
    // out around 128..2048, so full brightness here is comparable to a bright
    // one of those.
    light_scale = Cvar_Get("light_scale", "2000", CVAR_ARCHIVE);

    // How far in front of the eye "light place" puts one when nothing is in
    // the way.
    light_place_dist = Cvar_Get("light_place_dist", "64", CVAR_ARCHIVE);

    // Floor on the crosshair pick radius, so a small light is still a
    // hittable target from across a room.
    light_pick_size = Cvar_Get("light_pick_size", "8", CVAR_ARCHIVE);

    // Brightness of the debug markers. They are emissive particles, so this
    // is the knob for stopping them washing out the room being judged.
    light_debug_brightness = Cvar_Get("light_debug_brightness", "1", CVAR_ARCHIVE);

    Cmd_Register(c_lightedit);
}
