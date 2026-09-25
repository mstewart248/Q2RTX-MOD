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
// are the same kind of light - an analytic light handed to the path tracer -
// but placed by hand from the console and kept in a per-map text file instead
// of in the map. That is the whole point: a light can be placed, judged,
// retuned and deleted while looking at the room it lights, with no map compile
// in between.
//
//   light place   <r> <g> <b> <brightness> [radius] [volscale]
//                                    put a new light in front of the camera
//   light replace [<r> <g> <b> <brightness> [radius] [volscale]]
//                                    put a new light in front of the camera that
//                                    STANDS IN for the map light under the
//                                    crosshair. Anything not typed is inherited
//                                    from that entity.
//   light edit <attribute> <value>   change one attribute of the light under the
//                                    crosshair - see the list below
//   light edit <r> <g> <b> <brightness> [radius] [volscale]
//                                    the older all-at-once form, still works
//   light vol  <scale|default>       shorthand for "light edit vol"
//   light print                      the light under the crosshair in full, or
//                                    the whole list
//   light styles                     enumerate the lightstyles this map has
//   light delete                     remove the light under the crosshair
//   light debug_on / light debug_off show the lights as spheres
//   light reload                     re-read the file from disk
//   light probe [targetname]         same as probe_trigger, below
//
//   probe_trigger [targetname]       what the button / trigger under the
//                                    crosshair fires, followed down the whole
//                                    chain of relays with their delays, and
//                                    which of those names switch lights - the
//                                    names "light edit trigger_on" takes. Given
//                                    a name, starts the chain from that instead.
//
// r, g, b are 0..255, as typed. brightness is on the same 0..255 scale but is a
// real number, so a big, barely-there light can be "brightness .1". radius is in world units
// and is the size of the emitter sphere, which is what softens the shadow
// edge; it also sets the size of the debug marker and of the crosshair pick
// target, so what is on screen in debug mode is exactly what gets picked.
//
// volscale is the light's VOLUMETRIC SCALE - how much it scatters into the
// fog relative to how much it lights surfaces, which is RTX Remix's per-light
// volumetricRadianceScale. 1 is "the same as its surface lighting", 0 is "lights
// the room but makes no fog at all", and above 1 is a light that is mostly there
// for its beam. Omitting it - or "light vol default" - leaves the light on the
// pt_fog_scale_dynamic class default, and that is what the file stores: the
// column is only written for lights that were actually given one, so raising the
// cvar still moves every light that never had an opinion.
//
// ATTRIBUTES, for "light edit <attribute> <value>"
//
// One at a time, so nothing has to be retyped to change one number:
//
//   rgb <r> <g> <b>        colour, 0..255 each   (also "color", "colour")
//   red <n> / green <n> / blue <n>
//                          one channel of the colour
//   brightness <n>         0..255, fractions allowed (.5, .05)  (also "bright")
//   radius <n>             emitter size in world units   (also "size")
//   vol <n>                volumetric scale      (also "volume", "volscale")
//   cone <deg|off>         spot cone, a FULL angle; "off" makes it a point
//                          light                 (also "angle")
//   style <n>              lightstyle index - 0 is steady, see "light styles"
//                                                (also "lightstyle")
//   aim                    point it where the camera is looking
//                                                (also "direction")
//   origin                 move it to in front of the camera  (also "move")
//   x <n> / y <n> / z <n>  nudge one coordinate
//   trigger_on <name>      dark until the game fires <name>, then lit
//   trigger_off <name>     lit until the game fires <name>, then dark
//                          ("light edit trigger_on default" clears either)
//
// Every INHERITABLE attribute also takes "default", which unstates it and sends
// it back where it came from: the entity for a replacement light, or the plain
// default for a placed one. origin, x, y and z are the light's own and have no
// default. Setting one colour channel of an inherited colour materialises the
// other two from the entity first, so "light edit red 255" means what it looks
// like.
//
// Every light has all of these - a hand-placed light can be given a cone and a
// lightstyle too - and a light that has never been given one behaves exactly as
// it always did: a steady point light.
//
// THE MAP'S OWN LIGHTS
//
// The rerelease's dynamic_light entities no longer light anything themselves.
// They are DORMANT: with debug on they draw as BLACK spheres marking a position,
// and "light print" on one dumps everything the entity says. "light replace"
// then builds a real light that stands in for it - the entity stops emitting for
// good and the new light is drawn instead.
//
// The replacement is a NORMAL light in its own right, placed in front of the
// camera rather than at the entity, so its position is yours to choose. What it
// takes from the entity is every attribute you did not type: colour, brightness,
// cone, lightstyle, aim - and its SWITCHABILITY, so a light the game turns on and
// off goes on doing that. Anything the entity does not specify falls back to the
// ordinary default, which is what happens with the volumetric scale.
//
// A replaced entity keeps a half-size black marker while debug is on, so a glance
// at a room says which map lights have been dealt with. Deleting the replacement
// puts the entity back to dormant.
//
// A replacement is linked to its entity by the ENTITY's origin, stored as its own
// column: that is the one identity these entities have, it is readable, and it
// survives a recompile as long as the light has not moved. The replacement's own
// origin is separate, because it is allowed to be somewhere else entirely.
//
// Only debug mode makes a dormant map light pickable. With the markers off there
// is nothing on screen to aim at.
//
// LIGHTS THE GAME SWITCHES
//
// The rerelease switches its baked lighting with lightstyles: every "light"
// entity with a targetname gets a style of 32 or above, qrad bakes it into its
// own lightmap layer, and firing the targetname makes light_use flip
// CS_LIGHTS+style between "m" and "a". This renderer never draws lightmaps, so
// a switched room goes dark and then stays dark - mgu4m1's red emergency lights
// (rect_light_02, style 34) are exactly that. But the configstring still flips.
//
// So "trigger_on <name>" looks <name> up among the map's light entities, takes
// their style, and makes this light follow it - INVERTED when those lights start
// on, because "on when fired" of a light that starts lit means "on when it goes
// out". trigger_off is the same with the sense reversed. The name is what is
// stored, not the style, so it reads in the file and survives a recompile that
// renumbers styles. Only names that switch light entities work: nothing else the
// game fires is visible to the client. probe_trigger shows which ones those are.
//
// A switched light toggles each time its name fires, exactly like the map's
// own, and a light_use style_on flicker pattern carries through un-inverted.
//
// The file is <gamedir>/maps/lights/<mapname>.cfg, rewritten in full after
// every change - edit and delete need a rewrite anyway, so append would only
// be a second code path to get wrong. It is read back through the normal
// search path, so a finished set can be packed into a .pkz.
//
// These lights are deliberately NOT gated by cl_dynamic_lights. That cvar
// exists to turn the map's own lighting on for comparison, and a hand-placed
// light has to stay lit while exactly that comparison is being made.
// light_enable is its own switch.
//

#include "client.h"

// The renderer's whole frame budget is MAX_DLIGHTS (256), shared with the
// map's own dynamic_lights, so there is no point in a larger array here.
#define MAX_EDIT_LIGHTS     256

// Which attributes this light states for itself. Anything not flagged is
// inherited - from the entity for a replacement, from the plain default
// otherwise - and that is what makes "everything I did not type comes from the
// entity" work, and what keeps a light written before an attribute existed
// behaving the way it always did.
#define LE_HAVE_RGB         (1u << 0)
#define LE_HAVE_BRIGHT      (1u << 1)
#define LE_HAVE_RADIUS      (1u << 2)
#define LE_HAVE_VOL         (1u << 3)
#define LE_HAVE_CONE        (1u << 4)
#define LE_HAVE_STYLE       (1u << 5)
#define LE_HAVE_AIM         (1u << 6)
#define LE_HAVE_TRIGGER     (1u << 7)

// Longest targetname kept. The game's own are unbounded, but nothing in a
// shipped map comes near this.
#define LE_NAME_LEN         64

typedef struct {
    vec3_t      origin;
    byte        rgb[3];         // 0..255, exactly as typed
    float       brightness;     // 0..255 scale, fractions allowed, as typed
    float       radius;         // world units - emitter size, marker, pick size
    float       vol_scale;      // LIGHT_VOLUMETRIC_SCALE_UNSET = class default
    float       cone;           // FULL cone angle in degrees; 0 = point light
    int         style;          // lightstyle index; 0 = steady
    vec3_t      aim;            // unit vector, only meaningful for a spot

    unsigned    have;           // LE_HAVE_* actually stated by this light

    // >= 0 means this light STANDS IN for that dynamic_light entity: the entity
    // stops emitting and everything not in `have` is inherited from it.
    int         ent_index;
    vec3_t      ent_origin;     // the link key, as stored in the file

    // LE_HAVE_TRIGGER: the targetname this light is switched by. trigger_on
    // means dark until it fires; false is trigger_off, lit until it fires.
    char        trigger[LE_NAME_LEN];
    bool        trigger_on;

    // what `trigger` resolved to, cached by LE_BindTrigger so the per-frame path
    // never searches the lump: the lightstyle to follow (-1 = the name switches
    // no light, and the trigger is ignored), and whether to follow it inverted
    int         trigger_style;
    bool        trigger_invert;
} editlight_t;

static editlight_t  *le_lights;
static int          le_num_lights;
static bool         le_debug;

// The map's entity lump, cut down to what probe_trigger and the trigger lookup
// need. Parsed once per map in LE_LoadLights; the lump is never large enough
// (mgu4m1 is 1540 entities) for a linear search per command to matter, and the
// per-frame path only ever uses the style cached in the light's resolve.
typedef struct {
    char    classname[LE_NAME_LEN];
    char    targetname[LE_NAME_LEN];
    char    target[LE_NAME_LEN];
    char    killtarget[LE_NAME_LEN];
    char    pathtarget[LE_NAME_LEN];
    char    message[LE_NAME_LEN];
    vec3_t  origin;
    float   delay;
    float   wait;
    int     model;          // N of "*N", or -1 for a point entity
    int     style;
    int     spawnflags;
} leent_t;

static leent_t      *le_ents;
static int          le_num_ents;

static cvar_t   *light_enable;
static cvar_t   *light_scale;
static cvar_t   *light_place_dist;
static cvar_t   *light_pick_size;
static cvar_t   *light_debug_brightness;
static cvar_t   *light_marker_opacity;

// Emitter size for a light that does not name one. 10 is what V_AddLight uses
// for an ordinary point light.
#define LIGHT_DEFAULT_RADIUS    10.0f

// Marker size of a DORMANT map light. It has no emitter of its own, and this
// doubles as its crosshair pick size, so it wants to be a comfortable target
// rather than physically meaningful.
#define LE_DORMANT_MARKER_RADIUS    12.0f

// A replaced entity still gets a marker, at this fraction of the dormant size,
// so it reads as "dealt with" rather than as another light to place.
#define LE_REPLACED_MARKER_SCALE    0.5f

// How close a stored entity origin has to be to a candidate's to count as the
// same light. These come out of the BSP with exact origins and the file is
// written at %.1f, so this only has to absorb that rounding; it must stay well
// under the spacing between neighbouring lights.
#define LE_LINK_EPSILON         4.0f

// "light print" lists every map light individually up to this many, and only
// the interesting ones above it.
#define LE_PRINT_ALL_LIMIT      24

/*
=================================================================

  value parsing

=================================================================
*/

// Whether a value means "put this attribute back where it came from".
static bool LE_IsDefaultWord(const char *s)
{
    return !s || !*s || !strcmp(s, "default") || !strcmp(s, "-") ||
           !strcmp(s, "entity");
}

// A typed 0..255 value. Out of range is clamped rather than rejected - a 256
// typed for brightness is obviously meant to be full brightness.
static byte LE_ParseByte(const char *s)
{
    int v = atoi(s);

    clamp(v, 0, 255);
    return v;
}

// A typed brightness. Same 0..255 scale as the colour, but a real number: at a
// small radius even 1 is plenty bright, and a big soft light that is barely
// there needs .5 or .1. Clamped like LE_ParseByte.
static float LE_ParseBrightness(const char *s)
{
    float v = atof(s);

    clamp(v, 0.0f, 255.0f);
    return v;
}

// A typed radius, falling back to the default for a missing or nonsense one.
static float LE_ParseRadius(const char *s)
{
    float r = s ? atof(s) : 0.0f;

    return r > 0.0f ? r : LIGHT_DEFAULT_RADIUS;
}

// A typed volumetric scale. 0 is a real value here - "lights the room, makes no
// fog" - so it must not be confused with absent, which is why this is not just
// atof().
static float LE_ParseVolScale(const char *s)
{
    float v;

    if (LE_IsDefaultWord(s))
        return LIGHT_VOLUMETRIC_SCALE_UNSET;

    v = atof(s);

    return v >= 0.0f ? v : LIGHT_VOLUMETRIC_SCALE_UNSET;
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

// The 0..255 brightness -> engine light units mapping. The map's own lights come
// out around 128..2048 after cl_dynamic_light_scale, so full brightness here is
// comparable to a bright one of those.
static float LE_LightScale(void)
{
    return light_scale ? light_scale->value : 2000.0f;
}

/*
=================================================================

  the entity lump

  Everything the client can know about what the game fires. The
  targetname graph is static, so it can be read straight out of the
  BSP; what is live is only the lightstyle configstrings, and those
  are what a triggered light actually follows.

=================================================================
*/

// advances (data) past one { ... } block. false at the end of the lump or on
// anything malformed, the same contract as CL_ParseEntityBlock.
static bool LE_ParseEnt(const char **data, leent_t *out)
{
    char        key[LE_NAME_LEN];
    const char  *token;

    memset(out, 0, sizeof(*out));
    out->model = -1;

    token = COM_Parse(data);
    if (!*data && !*token)
        return false;
    if (strcmp(token, "{"))
        return false;

    while (1) {
        token = COM_Parse(data);
        if (!*data && !*token)
            return false;
        if (!strcmp(token, "}"))
            break;

        Q_strlcpy(key, token, sizeof(key));

        token = COM_Parse(data);
        if (!*data && !*token)
            return false;

        if (!strcmp(key, "classname"))
            Q_strlcpy(out->classname, token, sizeof(out->classname));
        else if (!strcmp(key, "targetname"))
            Q_strlcpy(out->targetname, token, sizeof(out->targetname));
        else if (!strcmp(key, "target"))
            Q_strlcpy(out->target, token, sizeof(out->target));
        else if (!strcmp(key, "killtarget"))
            Q_strlcpy(out->killtarget, token, sizeof(out->killtarget));
        else if (!strcmp(key, "pathtarget"))
            Q_strlcpy(out->pathtarget, token, sizeof(out->pathtarget));
        else if (!strcmp(key, "message"))
            Q_strlcpy(out->message, token, sizeof(out->message));
        else if (!strcmp(key, "origin"))
            sscanf(token, "%f %f %f", &out->origin[0], &out->origin[1], &out->origin[2]);
        else if (!strcmp(key, "model") && token[0] == '*')
            out->model = atoi(token + 1);
        else if (!strcmp(key, "delay"))
            out->delay = atof(token);
        else if (!strcmp(key, "wait"))
            out->wait = atof(token);
        else if (!strcmp(key, "style"))
            out->style = atoi(token);
        else if (!strcmp(key, "spawnflags"))
            out->spawnflags = atoi(token);
    }

    return true;
}

static void LE_FreeEnts(void)
{
    Z_Free(le_ents);
    le_ents = NULL;
    le_num_ents = 0;
}

static void LE_LoadEnts(void)
{
    const char  *data;
    leent_t     ent;
    int         count = 0;

    LE_FreeEnts();

    if (!cl.bsp || !cl.bsp->entitystring)
        return;

    data = cl.bsp->entitystring;
    while (LE_ParseEnt(&data, &ent))
        count++;

    if (!count)
        return;

    le_ents = Z_Mallocz(sizeof(leent_t) * count);

    data = cl.bsp->entitystring;
    while (le_num_ents < count && LE_ParseEnt(&data, &le_ents[le_num_ents]))
        le_num_ents++;
}

// What a targetname switches, as far as lighting goes.
typedef struct {
    int     style;          // -1 = no switchable light has that name
    int     count;          // how many light entities it switches
    bool    starts_on;
    bool    mixed;          // those lights disagree on style or start state
} leswitch_t;

// Whether this lump entry is a light the game can switch. SP_light only hooks
// light_use for a targetnamed light on style 32 or above - below that the style
// is a shared animation (flicker, pulse) that firing the name cannot change.
static bool LE_IsSwitchLight(const leent_t *e)
{
    return e->targetname[0] && e->style >= 32 && e->style < MAX_LIGHTSTYLES &&
           !strcmp(e->classname, "light");
}

static bool LE_FindSwitch(const char *name, leswitch_t *out)
{
    memset(out, 0, sizeof(*out));
    out->style = -1;

    if (!name || !*name)
        return false;

    for (int i = 0; i < le_num_ents; i++) {
        const leent_t *e = &le_ents[i];
        bool on = !(e->spawnflags & 1);     // START_OFF

        if (!LE_IsSwitchLight(e) || strcmp(e->targetname, name))
            continue;

        if (!out->count) {
            out->style = e->style;
            out->starts_on = on;
        } else if (e->style != out->style || on != out->starts_on) {
            out->mixed = true;
        }
        out->count++;
    }

    return out->count > 0;
}

/*
=================
LE_BindTrigger

Resolves a light's trigger name to the style it follows. Called whenever the name
or the entity lump changes - on load, and after every edit - never per frame.

"trigger_on" is dark until the name fires. If the lights with that name start
OFF, firing turns them on, so following their style directly is right; if they
start ON, firing turns them off, and this light has to be their inverse.
trigger_off is the same with the sense reversed.
=================
*/
static void LE_BindTrigger(editlight_t *l)
{
    leswitch_t sw;

    l->trigger_style = -1;
    l->trigger_invert = false;

    if (!(l->have & LE_HAVE_TRIGGER))
        return;

    if (!LE_FindSwitch(l->trigger, &sw))
        return;

    l->trigger_style = sw.style;
    l->trigger_invert = l->trigger_on ? sw.starts_on : !sw.starts_on;
}

// How lit a light's trigger has it right now, as a multiplier. 1 for a light
// with no trigger, or one whose name switches nothing.
static float LE_TriggerValue(const editlight_t *l)
{
    float v;

    if (!(l->have & LE_HAVE_TRIGGER) || l->trigger_style < 0)
        return 1.0f;

    // 'a' is 0. A non-inverted light keeps any style_on flicker the map gave
    // its switch; an inverted one can only be on or off.
    v = CL_LightStyleValue(l->trigger_style);
    if (l->trigger_invert)
        v = v > 0.0f ? 0.0f : 1.0f;

    return v;
}

/*
=================================================================

  inheritance

  A light states some attributes and inherits the rest. LE_Resolve is
  the single place that decides what a light actually is, so the
  renderer, the printout and the file all agree.

=================================================================
*/

// The entity a replacement light stands in for, or NULL for a plain one.
static cdynamiclight_t *LE_Entity(const editlight_t *l)
{
    if (l->ent_index < 0)
        return NULL;

    return CL_GetDynamicLight(l->ent_index);
}

/*
=================
LE_EntityBrightness

The entity's own brightness expressed on the editor's 0..255 scale.

This has to be converted, not copied. A dynamic_light's intensity is
shadowlightintensity * shadowlightradius and it reached the tracer multiplied by
cl_dynamic_light_scale, while a light here is 0..255 through LE_LightScale(). So
this is the brightness that reproduces what "cl_dynamic_lights 1" shows for that
entity, which makes replacing one a visually neutral act and every change after
it deliberate.

Note that a small light lands very low on this scale - a shadowlightradius of 100
comes out around 5 - which is faithful but leaves little room to tune downwards.
light_scale is the knob if that becomes annoying.
=================
*/
static float LE_EntityBrightness(const cdynamiclight_t *c)
{
    float scale = LE_LightScale();
    float bright = 0.0f;

    if (scale > 0.0f)
        bright = c->intensity * Cvar_VariableValue("cl_dynamic_light_scale")
                 / scale * 255.0f;

    // a light that resolves to 0 would be invisible and read as a bug, so it
    // gets a small floor - dim, but lit. Brightness is a float now, so a small
    // entity keeps its fraction instead of rounding up to a whole step.
    clamp(bright, 0.01f, 255.0f);
    return bright;
}

typedef struct {
    float   color[3];
    float   intensity;      // engine units, before the lightstyle
    float   radius;
    float   vol_scale;
    float   cone;           // FULL degrees; 0 with is_spot means "engine default"
    int     style;
    vec3_t  aim;
    bool    is_spot;
    bool    switched_on;    // the game may have the source entity turned off
} leresolved_t;

static void LE_Resolve(const editlight_t *l, leresolved_t *out)
{
    const cdynamiclight_t *c = LE_Entity(l);

    memset(out, 0, sizeof(*out));
    out->switched_on = true;

    // colour
    if (l->have & LE_HAVE_RGB) {
        for (int i = 0; i < 3; i++)
            out->color[i] = l->rgb[i] / 255.0f;
    } else if (c) {
        VectorCopy(c->color, out->color);
    } else {
        VectorSet(out->color, 1.0f, 1.0f, 1.0f);
    }

    // brightness
    if (l->have & LE_HAVE_BRIGHT)
        out->intensity = (l->brightness / 255.0f) * LE_LightScale();
    else if (c)
        out->intensity = (LE_EntityBrightness(c) / 255.0f) * LE_LightScale();
    else
        out->intensity = LE_LightScale();

    // emitter size. The entity has no emitter size of its own - its
    // shadowlightradius is reach, and that is already folded into its intensity
    // - so this falls straight through to the default for a replacement too.
    out->radius = (l->have & LE_HAVE_RADIUS) ? l->radius : LIGHT_DEFAULT_RADIUS;

    // volumetric scale. The entity may carry one; if it does not, the class
    // default is the answer, which is the sentinel either way.
    if (l->have & LE_HAVE_VOL)
        out->vol_scale = l->vol_scale;
    else if (c)
        out->vol_scale = c->vol_scale;
    else
        out->vol_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;

    // shape. A stated cone of 0 means "point light" - that is the whole reason
    // `have` exists rather than a sentinel value.
    if (l->have & LE_HAVE_CONE) {
        out->cone = l->cone;
        out->is_spot = l->cone > 0.0f;
    } else if (c) {
        out->cone = c->cone_angle;
        out->is_spot = c->is_spot;
    }

    // lightstyle
    if (l->have & LE_HAVE_STYLE)
        out->style = l->style;
    else if (c)
        out->style = c->style;

    // aim
    if (l->have & LE_HAVE_AIM)
        VectorCopy(l->aim, out->aim);
    else if (c)
        VectorCopy(c->direction, out->aim);
    else
        VectorClear(out->aim);

    // A POINT dynamic_light has no direction at all, so inheriting from one and
    // then giving the light a cone would hand V_AddSpotLight a zero vector.
    // Straight down is the fallback: it is where a ceiling fixture points, and
    // it is at least a direction. "light edit aim" is how to mean something else.
    if (out->is_spot && VectorLength(out->aim) < 0.001f)
        VectorSet(out->aim, 0.0f, 0.0f, -1.0f);

    // and whether the game currently has the source entity lit
    if (c)
        out->switched_on = CL_DynamicLightSwitchedOn(c);
}

/*
=================================================================

  the file

=================================================================
*/

// Applies one "<key> <value...>" attribute from a file line. Returns how many
// tokens it consumed, or 0 if the key is not one of ours.
static int LE_ApplyKeyedToken(editlight_t *l, int argc, int i)
{
    const char *key = Cmd_Argv(i);

#define NEED(n) do { if (i + (n) >= argc) return 0; } while (0)

    if (!strcmp(key, "rgb")) {
        NEED(3);
        for (int c = 0; c < 3; c++)
            l->rgb[c] = LE_ParseByte(Cmd_Argv(i + 1 + c));
        l->have |= LE_HAVE_RGB;
        return 4;
    }
    if (!strcmp(key, "bright")) {
        NEED(1);
        l->brightness = LE_ParseBrightness(Cmd_Argv(i + 1));
        l->have |= LE_HAVE_BRIGHT;
        return 2;
    }
    if (!strcmp(key, "radius")) {
        NEED(1);
        l->radius = LE_ParseRadius(Cmd_Argv(i + 1));
        l->have |= LE_HAVE_RADIUS;
        return 2;
    }
    if (!strcmp(key, "vol")) {
        NEED(1);
        if (!LE_IsDefaultWord(Cmd_Argv(i + 1))) {
            l->vol_scale = LE_ParseVolScale(Cmd_Argv(i + 1));
            l->have |= LE_HAVE_VOL;
        }
        return 2;
    }
    if (!strcmp(key, "cone")) {
        NEED(1);
        if (!LE_IsDefaultWord(Cmd_Argv(i + 1))) {
            l->cone = max(0.0f, (float)atof(Cmd_Argv(i + 1)));
            l->have |= LE_HAVE_CONE;
        }
        return 2;
    }
    if (!strcmp(key, "style")) {
        NEED(1);
        if (!LE_IsDefaultWord(Cmd_Argv(i + 1))) {
            l->style = atoi(Cmd_Argv(i + 1));
            clamp(l->style, 0, MAX_LIGHTSTYLES - 1);
            l->have |= LE_HAVE_STYLE;
        }
        return 2;
    }
    if (!strcmp(key, "aim")) {
        NEED(3);
        for (int c = 0; c < 3; c++)
            l->aim[c] = atof(Cmd_Argv(i + 1 + c));
        if (VectorNormalize(l->aim) > 0.0f)
            l->have |= LE_HAVE_AIM;
        return 4;
    }
    if (!strcmp(key, "trigger_on") || !strcmp(key, "trigger_off")) {
        NEED(1);
        Q_strlcpy(l->trigger, Cmd_Argv(i + 1), sizeof(l->trigger));
        l->trigger_on = !strcmp(key, "trigger_on");
        l->have |= LE_HAVE_TRIGGER;
        return 2;
    }

#undef NEED

    return 0;
}

/*
=================
LE_LoadLights

Called once per map load. A missing file is the normal case and is silent.

Two line shapes, and the older one has to keep working exactly as it did:

  x y z  r g b  brightness  [radius]  [volscale]  [<key> <value>...]
  maplight  entx enty entz  x y z  [<key> <value>...]
=================
*/
void LE_LoadLights(void)
{
    char    path[MAX_QPATH];
    char    *buffer, *s, *p;
    int     line = 0, ret, replacements = 0;

    LE_FreeLights();

    // before the file, so its trigger names can be resolved as they are read -
    // and regardless of whether there is a file, since probe_trigger needs it
    LE_LoadEnts();

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
        int         argc, i;

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

        if (!argc)
            goto next;

        if (le_num_lights == MAX_EDIT_LIGHTS) {
            Com_WPrintf("Too many lights in %s, stopping at %d\n",
                        path, MAX_EDIT_LIGHTS);
            break;
        }

        l = &le_lights[le_num_lights];
        memset(l, 0, sizeof(*l));
        l->ent_index = -1;
        l->vol_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
        l->radius = LIGHT_DEFAULT_RADIUS;
        l->trigger_style = -1;

        if (!strcmp(Cmd_Argv(0), "maplight")) {
            // maplight  entx enty entz  x y z  [key value...]
            if (argc < 7) {
                Com_WPrintf("Line %d is an incomplete maplight in %s\n", line, path);
                goto next;
            }

            for (i = 0; i < 3; i++)
                l->ent_origin[i] = atof(Cmd_Argv(1 + i));
            for (i = 0; i < 3; i++)
                l->origin[i] = atof(Cmd_Argv(4 + i));

            // -2 marks "this is a replacement whose entity is not resolved
            // yet". The link cannot be resolved inside this loop because
            // Cmd_TokenizeString is still holding the line.
            l->ent_index = -2;
            i = 7;
        } else {
            // x y z  r g b  brightness  [radius]  [volscale]  [key value...]
            if (argc < 7) {
                Com_WPrintf("Line %d is incomplete in %s\n", line, path);
                goto next;
            }

            for (i = 0; i < 3; i++)
                l->origin[i] = atof(Cmd_Argv(i));
            for (i = 0; i < 3; i++)
                l->rgb[i] = LE_ParseByte(Cmd_Argv(3 + i));

            l->brightness = LE_ParseBrightness(Cmd_Argv(6));
            l->have |= LE_HAVE_RGB | LE_HAVE_BRIGHT;

            i = 7;

            // the two legacy positional columns, each present only if the next
            // token is not one of the keyed attribute names
            if (i < argc && !LE_ApplyKeyedToken(l, argc, i)) {
                l->radius = LE_ParseRadius(Cmd_Argv(i));
                l->have |= LE_HAVE_RADIUS;
                i++;

                if (i < argc && !LE_ApplyKeyedToken(l, argc, i)) {
                    // "-" here is a light with no volumetric opinion that has
                    // keyed attributes after it, so the column is a placeholder
                    if (!LE_IsDefaultWord(Cmd_Argv(i))) {
                        l->vol_scale = LE_ParseVolScale(Cmd_Argv(i));
                        l->have |= LE_HAVE_VOL;
                    }
                    i++;
                }
            }
        }

        // whatever is left is keyed attributes
        while (i < argc) {
            int used = LE_ApplyKeyedToken(l, argc, i);

            if (!used) {
                Com_WPrintf("Line %d of %s: unknown attribute \"%s\"\n",
                            line, path, Cmd_Argv(i));
                break;
            }
            i += used;
        }

        LE_BindTrigger(l);
        if ((l->have & LE_HAVE_TRIGGER) && l->trigger_style < 0)
            Com_WPrintf("Line %d of %s: no switchable light is named \"%s\"; "
                        "that light ignores its trigger.\n", line, path, l->trigger);

        le_num_lights++;

next:
        if (!p)
            break;

        s = p + 1;
    }

    FS_FreeFile(buffer);

    // The entity links are resolved now rather than in the loop above, because
    // that loop is still holding a tokenized line. CL_LoadDynamicLights runs
    // before this - main.c depends on that order - so the entities exist.
    for (int i = 0; i < le_num_lights; i++) {
        editlight_t *l = &le_lights[i];
        int     count, best = -1;
        float   best_dist = LE_LINK_EPSILON;

        if (l->ent_index != -2)
            continue;               // a plain placed light

        count = CL_NumDynamicLights();
        for (int j = 0; j < count; j++) {
            const cdynamiclight_t *c = CL_GetDynamicLight(j);
            vec3_t  delta;
            float   dist;

            VectorSubtract(c->origin, l->ent_origin, delta);
            dist = VectorLength(delta);

            if (dist <= best_dist) {
                best_dist = dist;
                best = j;
            }
        }

        if (best < 0) {
            // the map's lights moved or went away under a file written against
            // an older compile. Say so - the light still works, it has just
            // stopped inheriting and stopped suppressing anything.
            Com_WPrintf("No dynamic_light at %.0f %.0f %.0f in %s; "
                        "that light keeps its own values.\n",
                        l->ent_origin[0], l->ent_origin[1], l->ent_origin[2], path);
        }

        l->ent_index = best;
    }

    // and now suppress every entity that has a stand-in
    replacements = 0;
    for (int i = 0; i < le_num_lights; i++) {
        cdynamiclight_t *c = LE_Entity(&le_lights[i]);

        if (c) {
            c->replaced = true;
            replacements++;
        }
    }

    Com_Printf("Loaded %d light%s from %s (%d replacing map lights)\n",
               le_num_lights, le_num_lights == 1 ? "" : "s", path, replacements);
}

void LE_FreeLights(void)
{
    int count = CL_NumDynamicLights();

    Z_Free(le_lights);
    le_lights = NULL;
    le_num_lights = 0;

    LE_FreeEnts();

    // The entities outlive this - they belong to dynamiclights.c and are only
    // reallocated on a map load - so "light reload" would otherwise keep every
    // suppression from the previous read of the file. Clearing them here is what
    // makes a reload actually mean re-read.
    for (int i = 0; i < count; i++)
        CL_GetDynamicLight(i)->replaced = false;
}

// The keyed tail shared by both line shapes: only the attributes this light
// actually states, so a light that has never been given a cone does not get a
// cone column, and raising a default still moves it.
static void LE_WriteKeyedTail(qhandle_t f, const editlight_t *l, unsigned skip)
{
    unsigned have = l->have & ~skip;

    if (have & LE_HAVE_RGB)
        FS_FPrintf(f, "  rgb %d %d %d", l->rgb[0], l->rgb[1], l->rgb[2]);
    if (have & LE_HAVE_BRIGHT)
        FS_FPrintf(f, "  bright %g", l->brightness);
    if (have & LE_HAVE_RADIUS)
        FS_FPrintf(f, "  radius %.1f", l->radius);
    if (have & LE_HAVE_VOL)
        FS_FPrintf(f, "  vol %.3f", l->vol_scale);
    if (have & LE_HAVE_CONE)
        FS_FPrintf(f, "  cone %.0f", l->cone);
    if (have & LE_HAVE_STYLE)
        FS_FPrintf(f, "  style %d", l->style);
    if (have & LE_HAVE_AIM)
        FS_FPrintf(f, "  aim %.4f %.4f %.4f", l->aim[0], l->aim[1], l->aim[2]);
    if (have & LE_HAVE_TRIGGER)
        FS_FPrintf(f, "  %s \"%s\"",
                   l->trigger_on ? "trigger_on" : "trigger_off", l->trigger);
}

/*
=================
LE_WriteLights

Rewrites the whole file, including when the list has just become empty -
otherwise deleting the last light would leave it behind on disk.

A plain light keeps the original column layout so a file that predates the
newer attributes round-trips unchanged; anything newer is appended as keyed
attributes.
=================
*/
static void LE_WriteLights(void)
{
    char        buffer[MAX_OSPATH];
    qhandle_t   f;
    int         placed = 0, replacing = 0;

    f = FS_EasyOpenFile(buffer, sizeof(buffer), FS_MODE_WRITE | FS_FLAG_TEXT,
                        "maps/lights/", cl.mapname, ".cfg");
    if (!f)
        return;

    FS_FPrintf(f, "// placed lights for %s - written by \"light place\"\n", cl.mapname);
    FS_FPrintf(f, "// x y z   r g b   brightness   radius   [volumetric scale]"
                  "   [cone d] [style n] [aim x y z] [trigger_on|trigger_off name]\n");

    for (int i = 0; i < le_num_lights; i++) {
        const editlight_t *l = &le_lights[i];
        unsigned tail;

        if (l->ent_index >= 0) {
            replacing++;
            continue;
        }
        placed++;

        FS_FPrintf(f, "%.1f %.1f %.1f   %d %d %d   %g   %.1f",
                   l->origin[0], l->origin[1], l->origin[2],
                   l->rgb[0], l->rgb[1], l->rgb[2],
                   l->brightness, l->radius);

        tail = LE_HAVE_RGB | LE_HAVE_BRIGHT | LE_HAVE_RADIUS;

        // the volumetric column stays positional, as it always was - but if
        // anything keyed follows it, it needs a placeholder so the reader does
        // not take that key for a number
        if (l->have & LE_HAVE_VOL) {
            FS_FPrintf(f, "   %.3f", l->vol_scale);
            tail |= LE_HAVE_VOL;
        } else if (l->have & ~tail) {
            FS_FPrintf(f, "   -");
        }

        LE_WriteKeyedTail(f, l, tail);
        FS_FPrintf(f, "\n");
    }

    if (replacing) {
        FS_FPrintf(f, "\n// lights that STAND IN for the map's own dynamic_light\n");
        FS_FPrintf(f, "// entities, which stop emitting. The first origin is the\n");
        FS_FPrintf(f, "// ENTITY's, and is the link; the second is this light's own.\n");
        FS_FPrintf(f, "// Attributes not listed are inherited from the entity.\n");
        FS_FPrintf(f, "// maplight <entity x y z>  <x y z>  [rgb r g b] [bright n]"
                      " [radius r] [vol v] [cone d] [style n] [aim x y z]"
                      " [trigger_on|trigger_off name]\n");

        for (int i = 0; i < le_num_lights; i++) {
            const editlight_t *l = &le_lights[i];

            if (l->ent_index < 0)
                continue;

            FS_FPrintf(f, "maplight %.1f %.1f %.1f   %.1f %.1f %.1f",
                       l->ent_origin[0], l->ent_origin[1], l->ent_origin[2],
                       l->origin[0], l->origin[1], l->origin[2]);

            LE_WriteKeyedTail(f, l, 0);
            FS_FPrintf(f, "\n");
        }
    }

    if (FS_CloseFile(f))
        Com_EPrintf("Error writing %s\n", buffer);
    else
        Com_Printf("Wrote %d placed and %d replacement light%s to %s\n",
                   placed, replacing,
                   (placed + replacing) == 1 ? "" : "s", buffer);
}

/*
=================================================================

  picking

  The crosshair is the view axis, so a light is "under the crosshair"
  when its marker sphere intersects the view ray. Nearest along the
  ray wins, which is what the eye expects when two overlap.

  NOTE the view sits about 22 units above the player origin, so a
  scripted pick has to aim at the marker, not at the player's own
  height. That is not obvious and has cost time.

=================================================================
*/

// Whether the view ray through the crosshair hits a marker sphere of `radius`
// at `origin`, and if so how far along the ray it is.
static bool LE_RayHitsMarker(const vec3_t origin, float radius, float *along_out)
{
    vec3_t  delta, perp;
    float   along, pick;

    VectorSubtract(origin, cl.refdef.vieworg, delta);

    along = DotProduct(delta, cl.v_forward);
    if (along <= 0.0f)
        return false;               // behind the camera

    // A small light is an unhittable target from across a room, so the pick
    // radius has a floor. This is the only place the marker size and the pick
    // size are allowed to disagree.
    pick = max(radius, light_pick_size->value);

    VectorMA(delta, -along, cl.v_forward, perp);
    if (VectorLength(perp) > pick)
        return false;

    *along_out = along;
    return true;
}

// The marker size of a light, which is its emitter size.
static float LE_MarkerRadius(const editlight_t *l)
{
    leresolved_t r;

    LE_Resolve(l, &r);
    return r.radius;
}

// The marker size of a map light entity: full while dormant, small once
// something stands in for it.
static float LE_EntityMarkerRadius(const cdynamiclight_t *c)
{
    return c->replaced ? LE_DORMANT_MARKER_RADIUS * LE_REPLACED_MARKER_SCALE
                       : LE_DORMANT_MARKER_RADIUS;
}

/*
=================
LE_Pick

What the crosshair is on: either one of this file's lights or one of the map's
dynamic_light entities. The commands act on whichever it is, so they have to be
picked from one ordered list rather than two.

Exactly one of the two fields is set on a hit; both are NULL on a miss.
=================
*/
typedef struct {
    editlight_t     *light;
    cdynamiclight_t *ent;
} lepick_t;

static lepick_t LE_Pick(void)
{
    lepick_t    best = { NULL, NULL };
    float       best_along = 0.0f;
    float       along;

    for (int i = 0; i < le_num_lights; i++) {
        editlight_t *l = &le_lights[i];

        if (!LE_RayHitsMarker(l->origin, LE_MarkerRadius(l), &along))
            continue;

        if ((!best.light && !best.ent) || along < best_along) {
            best.light = l;
            best.ent = NULL;
            best_along = along;
        }
    }

    // Map light entities are only pickable in debug mode. Off, there is nothing
    // on screen to aim at, and silently acting on an invisible one would be a
    // trap - so debug_on is what arms the map-light half of the tool.
    if (le_debug) {
        int count = CL_NumDynamicLights();

        for (int i = 0; i < count; i++) {
            cdynamiclight_t *c = CL_GetDynamicLight(i);

            if (!LE_RayHitsMarker(c->origin, LE_EntityMarkerRadius(c), &along))
                continue;

            if ((!best.light && !best.ent) || along < best_along) {
                best.light = NULL;
                best.ent = c;
                best_along = along;
            }
        }
    }

    return best;
}

/*
=================================================================

  printing

=================================================================
*/

// The live lightstyle string for an index, or NULL if the map never set one.
static const char *LE_StyleString(int style)
{
    const char *s;

    if (style < 0 || style >= MAX_LIGHTSTYLES)
        return NULL;

    s = cl.configstrings[CS_LIGHTS + style];

    return (s && *s) ? s : NULL;
}

// "60" / "point" / "60 (entity)" - the value plus where it came from.
#define LE_SRC(l, bit)  (((l)->have & (bit)) ? "" : \
                         ((l)->ent_index >= 0 ? " (entity)" : " (default)"))

static void LE_PrintLight(const editlight_t *l)
{
    leresolved_t    r;
    const char      *style_str;
    char            vol[32], shape[48];

    LE_Resolve(l, &r);

    if (r.vol_scale >= 0.0f)
        Q_snprintf(vol, sizeof(vol), "%.3f%s", r.vol_scale, LE_SRC(l, LE_HAVE_VOL));
    else
        Q_strlcpy(vol, "default", sizeof(vol));

    if (r.is_spot)
        Q_snprintf(shape, sizeof(shape), "cone %.0f%s", r.cone,
                   LE_SRC(l, LE_HAVE_CONE));
    else
        Q_snprintf(shape, sizeof(shape), "point%s", LE_SRC(l, LE_HAVE_CONE));

    Com_Printf("light %d%s: at %.0f %.0f %.0f\n",
               (int)(l - le_lights),
               l->ent_index >= 0 ? " (replaces a map light)" : "",
               l->origin[0], l->origin[1], l->origin[2]);

    Com_Printf("    rgb %d %d %d%s   brightness %g%s   radius %.1f%s\n",
               (int)(r.color[0] * 255.0f + 0.5f),
               (int)(r.color[1] * 255.0f + 0.5f),
               (int)(r.color[2] * 255.0f + 0.5f), LE_SRC(l, LE_HAVE_RGB),
               r.intensity / max(LE_LightScale(), 1.0f) * 255.0f,
               LE_SRC(l, LE_HAVE_BRIGHT),
               // radius is never inherited - a dynamic_light's own radius is
               // its REACH, already folded into its intensity, not an emitter
               // size - so this one is always "(default)" when unstated
               r.radius, (l->have & LE_HAVE_RADIUS) ? "" : " (default)");

    style_str = LE_StyleString(r.style);
    Com_Printf("    %s   style %d%s%s%s   vol %s\n",
               shape, r.style, LE_SRC(l, LE_HAVE_STYLE),
               style_str ? " = " : "", style_str ? style_str : "",
               vol);

    if (r.is_spot)
        Com_Printf("    aim %.3f %.3f %.3f%s\n",
                   r.aim[0], r.aim[1], r.aim[2], LE_SRC(l, LE_HAVE_AIM));

    if (l->have & LE_HAVE_TRIGGER) {
        if (l->trigger_style >= 0)
            Com_Printf("    trigger_%s \"%s\"   follows style %d%s   %s right now\n",
                       l->trigger_on ? "on" : "off", l->trigger, l->trigger_style,
                       l->trigger_invert ? " inverted" : "",
                       LE_TriggerValue(l) > 0.0f ? "LIT" : "DARK");
        else
            Com_Printf("    trigger_%s \"%s\"   NO switchable light has that name, "
                       "so this is ignored\n",
                       l->trigger_on ? "on" : "off", l->trigger);
    }

    if (l->ent_index >= 0) {
        Com_Printf("    entity at %.0f %.0f %.0f%s\n",
                   l->ent_origin[0], l->ent_origin[1], l->ent_origin[2],
                   r.switched_on ? "" : "   (switched OFF by the game right now)");
    }
}

/*
=================
LE_PrintEntity

Everything one of the map's dynamic_light entities says, which is the point of
hovering a black sphere: it is the sheet you tune a replacement against.
=================
*/
static void LE_PrintEntity(const cdynamiclight_t *c)
{
    const char *style_str = LE_StyleString(c->style);

    Com_Printf("map light at %.0f %.0f %.0f: %s\n",
               c->origin[0], c->origin[1], c->origin[2],
               c->replaced ? "REPLACED - a light stands in for it"
                           : "DORMANT - \"light replace\" to light it");

    Com_Printf("    rgb %d %d %d   brightness %g (converted from intensity %.0f)\n",
               (int)(c->color[0] * 255.0f + 0.5f),
               (int)(c->color[1] * 255.0f + 0.5f),
               (int)(c->color[2] * 255.0f + 0.5f),
               LE_EntityBrightness(c), c->intensity);

    if (c->is_spot)
        Com_Printf("    spot, cone %.0f%s   aim %.3f %.3f %.3f\n",
                   c->cone_angle,
                   c->cone_angle > 0.0f ? "" : " (engine default)",
                   c->direction[0], c->direction[1], c->direction[2]);
    else
        Com_Printf("    point light\n");

    Com_Printf("    style %d%s%s\n", c->style,
               style_str ? " = " : "", style_str ? style_str : "");

    if (c->switch_index >= 0)
        Com_Printf("    switchable by the game (bit %d, currently %s)\n",
                   c->switch_index, CL_DynamicLightSwitchedOn(c) ? "ON" : "OFF");
    else
        Com_Printf("    always on\n");

    if (c->vol_scale >= 0.0f)
        Com_Printf("    volumetric scale %.3f\n", c->vol_scale);
    else
        Com_Printf("    volumetric scale: not set, uses the default\n");
}

/*
=================================================================

  probe_trigger

  What a button or trigger fires, read out of the entity lump and
  followed down the chain. The chain matters more than the first
  link: mgu4m1's light switch fires rect_light_01, which puts the
  white lights out, and a trigger_relay on that same name fires
  rect_light_02 - the red ones - five seconds later. The button's own
  target is only half the story.

=================================================================
*/

#define LE_PROBE_MAX_DEPTH      8
#define LE_PROBE_MAX_SEEN       64
#define LE_PROBE_RANGE          8192.0f

static const char   *le_probe_seen[LE_PROBE_MAX_SEEN];
static int          le_probe_num_seen;

// Classes whose "target" is somewhere to go rather than something to fire: a
// monster's is the path_corner it walks to, a train's is its first stop.
static bool LE_TargetIsPath(const leent_t *e)
{
    return !strncmp(e->classname, "path_", 5) ||
           !strncmp(e->classname, "monster_", 8) ||
           !strncmp(e->classname, "misc_", 5) ||
           !strcmp(e->classname, "func_train") ||
           !strcmp(e->classname, "target_actor");
}

static void LE_Indent(int depth)
{
    for (int i = 0; i < depth; i++)
        Com_Printf("  ");
}

// The line that matters: this name switches lights, here is how to follow it.
static void LE_ProbeSwitch(const char *name, int depth)
{
    leswitch_t  sw;
    bool        lit;

    if (!LE_FindSwitch(name, &sw))
        return;

    lit = CL_LightStyleValue(sw.style) > 0.0f;

    LE_Indent(depth);
    Com_Printf("* SWITCHES %d light%s on style %d - they start %s, %s now%s\n",
               sw.count, sw.count == 1 ? "" : "s", sw.style,
               sw.starts_on ? "ON" : "OFF", lit ? "ON" : "OFF",
               sw.mixed ? " (not all agree)" : "");
    LE_Indent(depth);
    Com_Printf("    light edit trigger_on %s    - dark until this fires\n", name);
    LE_Indent(depth);
    Com_Printf("    light edit trigger_off %s   - lit until this fires\n", name);
}

static void LE_ProbeName(const char *name, float at, int depth)
{
    char    also[256];
    struct {
        const char  *classname;
        int         count;
    } groups[24];
    int     num_groups = 0, found = 0;

    LE_Indent(depth);
    if (at > 0.0f)
        Com_Printf("fires \"%s\"   at +%gs\n", name, at);
    else
        Com_Printf("fires \"%s\"\n", name);

    // relays can loop, and a func_timer chain always does
    for (int i = 0; i < le_probe_num_seen; i++) {
        if (!strcmp(le_probe_seen[i], name)) {
            LE_Indent(depth + 1);
            Com_Printf("(already shown above)\n");
            return;
        }
    }
    if (le_probe_num_seen < LE_PROBE_MAX_SEEN)
        le_probe_seen[le_probe_num_seen++] = name;

    LE_ProbeSwitch(name, depth + 1);

    for (int i = 0; i < le_num_ents; i++) {
        const leent_t *e = &le_ents[i];
        bool chains;

        if (strcmp(e->targetname, name))
            continue;
        found++;

        // the switch line above already said everything about these
        if (LE_IsSwitchLight(e))
            continue;

        chains = (e->target[0] || e->killtarget[0]) && !LE_TargetIsPath(e);

        if (chains && depth < LE_PROBE_MAX_DEPTH) {
            LE_Indent(depth + 1);
            Com_Printf("%s", e->classname);
            if (e->model >= 0)
                Com_Printf(" *%d", e->model);
            else
                Com_Printf(" at %.0f %.0f %.0f", e->origin[0], e->origin[1], e->origin[2]);
            if (e->delay > 0.0f)
                Com_Printf("   delay %g", e->delay);
            Com_Printf("\n");

            if (e->target[0])
                LE_ProbeName(e->target, at + e->delay, depth + 2);
            if (e->killtarget[0]) {
                LE_Indent(depth + 2);
                Com_Printf("removes \"%s\"   at +%gs\n", e->killtarget, at + e->delay);
            }
            continue;
        }

        // everything else is summarised by class - seven target_speakers are
        // one line, not seven
        {
            int g;

            for (g = 0; g < num_groups; g++)
                if (!strcmp(groups[g].classname, e->classname))
                    break;
            if (g == num_groups && num_groups < q_countof(groups)) {
                groups[g].classname = e->classname;
                groups[g].count = 0;
                num_groups++;
            }
            if (g < num_groups)
                groups[g].count++;
        }
    }

    if (!found) {
        LE_Indent(depth + 1);
        Com_Printf("(nothing in the map has that name)\n");
        return;
    }

    if (num_groups) {
        also[0] = 0;
        for (int g = 0; g < num_groups; g++) {
            char one[LE_NAME_LEN + 16];

            Q_snprintf(one, sizeof(one), "%s%d %s", g ? ", " : "",
                       groups[g].count, groups[g].classname);
            Q_strlcat(also, one, sizeof(also));
        }
        LE_Indent(depth + 1);
        Com_Printf("also: %s\n", also);
    }
}

/*
=================
LE_PickBrushEnt

The brush entity under the crosshair: a button, a door, a trigger volume. Traced
against each inline model at wherever the game has it now, so a door that has
opened is picked where it is drawn.

Trigger volumes are invisible and often sit in front of the thing they belong
to - a trigger_multiple filling a doorway - so a visible entity along the ray
wins over a nearer trigger. A trigger is picked only when there is nothing
else, and one the eye is standing inside is reported separately.
=================
*/
static bool LE_IsTriggerVolume(const leent_t *e)
{
    return !strncmp(e->classname, "trigger_", 8) ||
           !strcmp(e->classname, "func_areaportal");
}

static const leent_t *LE_PickBrushEnt(const leent_t **inside_out)
{
    const leent_t   *best_solid = NULL, *best_trigger = NULL, *inside = NULL;
    float           solid_frac = 1.0f, trigger_frac = 1.0f;
    vec3_t          start, end;
    trace_t         tr;
    float           world_frac;

    VectorCopy(cl.refdef.vieworg, start);
    VectorMA(start, LE_PROBE_RANGE, cl.v_forward, end);

    CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);
    // a button is flush with the wall it is set in, so allow a couple of units
    world_frac = tr.fraction + 2.0f / LE_PROBE_RANGE;

    for (int i = 0; i < le_num_ents; i++) {
        const leent_t   *e = &le_ents[i];
        const mmodel_t  *cmodel;
        const vec_t     *origin = vec3_origin, *angles = vec3_origin;

        if (e->model <= 0 || e->model >= cl.bsp->nummodels)
            continue;

        cmodel = &cl.bsp->models[e->model];

        // where the game has it now, if it is sending it. A trigger never is,
        // and never moves, so the compiled position is right for those.
        for (int j = 0; j < cl.numSolidEntities; j++) {
            const centity_t *cent = cl.solidEntities[j];

            if (cent->current.solid == PACKED_BSP &&
                cl.model_clip[cent->current.modelindex] == cmodel) {
                origin = cent->current.origin;
                angles = cent->current.angles;
                break;
            }
        }

        CM_TransformedBoxTrace(&tr, start, end, vec3_origin, vec3_origin,
                               cmodel->headnode, MASK_ALL, origin, angles);

        if (tr.startsolid) {
            if (LE_IsTriggerVolume(e) && !inside)
                inside = e;
            continue;
        }
        if (tr.fraction >= 1.0f || tr.fraction > world_frac)
            continue;

        if (LE_IsTriggerVolume(e)) {
            if (tr.fraction < trigger_frac) {
                trigger_frac = tr.fraction;
                best_trigger = e;
            }
        } else if (tr.fraction < solid_frac) {
            solid_frac = tr.fraction;
            best_solid = e;
        }
    }

    if (inside_out)
        *inside_out = inside;

    return best_solid ? best_solid : best_trigger;
}

static void LE_ProbeEnt(const leent_t *e)
{
    int sources = 0;

    le_probe_num_seen = 0;

    Com_Printf("%s", e->classname);
    if (e->model >= 0)
        Com_Printf(" (model *%d)", e->model);
    if (e->targetname[0])
        Com_Printf("   named \"%s\"", e->targetname);
    Com_Printf("\n");

    if (e->message[0])
        Com_Printf("  message \"%s\"\n", e->message);
    if (e->wait < 0.0f)
        Com_Printf("  wait -1 - fires once\n");
    else if (e->wait > 0.0f)
        Com_Printf("  wait %g\n", e->wait);

    if (e->target[0]) {
        if (LE_TargetIsPath(e))
            Com_Printf("  target \"%s\" is a path to follow, not something it fires\n",
                       e->target);
        else
            LE_ProbeName(e->target, e->delay, 1);
    }
    if (e->killtarget[0])
        Com_Printf("  removes \"%s\"\n", e->killtarget);
    if (e->pathtarget[0])
        Com_Printf("  pathtarget \"%s\" - tells the train where to go\n", e->pathtarget);
    if (!e->target[0] && !e->killtarget[0])
        Com_Printf("  fires nothing itself\n");

    // and the other direction: a door or a light is more often the END of a
    // chain, and what the mapper wants to know is what sets it off
    if (e->targetname[0]) {
        LE_ProbeSwitch(e->targetname, 1);

        for (int i = 0; i < le_num_ents; i++) {
            const leent_t *s = &le_ents[i];

            if (strcmp(s->target, e->targetname))
                continue;

            if (!sources++)
                Com_Printf("  is fired by:\n");
            if (sources > 8) {
                Com_Printf("    ...\n");
                break;
            }

            Com_Printf("    %s", s->classname);
            if (s->model >= 0)
                Com_Printf(" *%d", s->model);
            else
                Com_Printf(" at %.0f %.0f %.0f", s->origin[0], s->origin[1], s->origin[2]);
            if (s->targetname[0])
                Com_Printf("   named \"%s\"", s->targetname);
            if (s->delay > 0.0f)
                Com_Printf("   delay %g", s->delay);
            Com_Printf("\n");
        }
    }
}

/*
=================
LE_Probe

probe_trigger with no argument probes what is under the crosshair; with one,
the chain starting from that name, which is how to look at a trigger_relay or
anything else with no brush to aim at.
=================
*/
static void LE_Probe(const char *name)
{
    const leent_t *e, *inside = NULL;

    if (!le_ents) {
        Com_Printf("This map has no entities to probe.\n");
        return;
    }

    if (name && *name) {
        int named = 0;

        le_probe_num_seen = 0;
        for (int i = 0; i < le_num_ents; i++)
            if (!strcmp(le_ents[i].targetname, name))
                named++;

        Com_Printf("%d entit%s named \"%s\"\n", named, named == 1 ? "y" : "ies", name);
        LE_ProbeName(name, 0.0f, 0);
        return;
    }

    e = LE_PickBrushEnt(&inside);

    if (!e && !inside) {
        Com_Printf("No button, door or trigger under the crosshair. "
                   "\"probe_trigger <targetname>\" probes by name.\n");
        return;
    }

    if (e)
        LE_ProbeEnt(e);

    if (inside && inside != e) {
        Com_Printf("\nYou are standing inside a %s (model *%d)%s%s%s\n",
                   inside->classname, inside->model,
                   inside->target[0] ? " that fires \"" : "",
                   inside->target, inside->target[0] ? "\"" : "");
    }
}

static void LE_ProbeTrigger_f(void)
{
    if (!LE_Active())
        return;

    LE_Probe(Cmd_Argv(1));
}

// Why a name cannot drive a light: either nothing has it, or what has it is not
// a light the game can switch.
static void LE_ExplainNoSwitch(const char *name)
{
    int         named = 0;
    const char  *first = NULL;

    for (int i = 0; i < le_num_ents; i++) {
        if (strcmp(le_ents[i].targetname, name))
            continue;
        if (!named++)
            first = le_ents[i].classname;
    }

    if (!named) {
        Com_Printf("Nothing in %s is named \"%s\". probe_trigger on the switch "
                   "shows the names it fires.\n", cl.mapname, name);
        return;
    }

    Com_Printf("\"%s\" names %d entit%s (%s%s) but no light the game switches, and "
               "only a switched light's state reaches the client. probe_trigger "
               "%s shows what it fires - one of those names may be a switch.\n",
               name, named, named == 1 ? "y" : "ies", first,
               named > 1 ? ", ..." : "", name);
}

/*
=================================================================

  the subcommands

=================================================================
*/

// Where a new light goes: light_place_dist in front of the eye, or against
// whatever is nearer than that. Landing on a surface is the common case when
// lighting a wall fixture, so back the emitter off the plane by its own radius -
// a sphere light buried in a wall lights nothing.
static void LE_PlacementPoint(vec3_t out, float radius)
{
    vec3_t  end;
    trace_t trace;

    VectorMA(cl.refdef.vieworg, light_place_dist->value, cl.v_forward, end);
    CM_BoxTrace(&trace, cl.refdef.vieworg, end, vec3_origin, vec3_origin,
                cl.bsp->nodes, MASK_SOLID);

    // startsolid leaves no plane to push off of - the camera is already inside
    // something, which happens with noclip, so just take the eye position
    VectorCopy(trace.endpos, out);
    if (trace.fraction < 1.0f && !trace.startsolid)
        VectorMA(out, radius + 1.0f, trace.plane.normal, out);
}

/*
=================
LE_ParseCreateArgs

The shared argument list of "place" and "replace": up to four 0..255 bytes, then
a radius and a volumetric scale. Every one is optional here - what is not typed
is simply not stated, and gets inherited or defaulted. "place" is the one caller
that insists on the first four.
=================
*/
static void LE_ParseCreateArgs(editlight_t *l, int first)
{
    int argc = Cmd_Argc();

    if (argc >= first + 3) {
        for (int i = 0; i < 3; i++)
            l->rgb[i] = LE_ParseByte(Cmd_Argv(first + i));
        l->have |= LE_HAVE_RGB;
    }

    if (argc >= first + 4) {
        l->brightness = LE_ParseBrightness(Cmd_Argv(first + 3));
        l->have |= LE_HAVE_BRIGHT;
    }

    if (argc >= first + 5 && !LE_IsDefaultWord(Cmd_Argv(first + 4))) {
        l->radius = LE_ParseRadius(Cmd_Argv(first + 4));
        l->have |= LE_HAVE_RADIUS;
    }

    if (argc >= first + 6 && !LE_IsDefaultWord(Cmd_Argv(first + 5))) {
        l->vol_scale = LE_ParseVolScale(Cmd_Argv(first + 5));
        l->have |= LE_HAVE_VOL;
    }
}

static editlight_t *LE_NewLight(void)
{
    editlight_t *l;

    if (le_num_lights == MAX_EDIT_LIGHTS) {
        Com_Printf("Already at the %d light limit.\n", MAX_EDIT_LIGHTS);
        return NULL;
    }

    l = &le_lights[le_num_lights];
    memset(l, 0, sizeof(*l));
    l->ent_index = -1;
    l->radius = LIGHT_DEFAULT_RADIUS;
    l->vol_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
    l->trigger_style = -1;

    return l;
}

static void LE_Place_f(void)
{
    editlight_t *l = LE_NewLight();

    if (!l)
        return;

    if (Cmd_Argc() < 6) {
        Com_Printf("Usage: %s place <r> <g> <b> <brightness> [radius] [volscale]\n",
                   Cmd_Argv(0));
        return;
    }

    LE_ParseCreateArgs(l, 2);
    LE_PlacementPoint(l->origin, LE_MarkerRadius(l));

    le_num_lights++;

    LE_PrintLight(l);
    LE_WriteLights();
}

/*
=================
LE_Replace_f

Build a light that stands in for the map light under the crosshair. It goes in
front of the camera, not at the entity, so its placement is the mapper's; and
every attribute not typed is left unstated, which is what makes it inherit.
=================
*/
static void LE_Replace_f(void)
{
    lepick_t    pick = LE_Pick();
    editlight_t *l;
    int         index;

    if (!pick.ent) {
        if (pick.light)
            Com_Printf("That is one of your own lights, not one of the map's - "
                       "use \"light edit\".\n");
        else if (!le_debug)
            Com_Printf("Map lights are only pickable in debug mode - "
                       "\"light debug_on\" first.\n");
        else
            Com_Printf("No map light under the crosshair.\n");
        return;
    }

    if (pick.ent->replaced) {
        Com_Printf("Something already stands in for that map light. "
                   "Delete that light first, or edit it.\n");
        return;
    }

    // the index is the link, and it has to be found before the array grows
    index = -1;
    for (int i = 0; i < CL_NumDynamicLights(); i++) {
        if (CL_GetDynamicLight(i) == pick.ent) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;

    l = LE_NewLight();
    if (!l)
        return;

    LE_ParseCreateArgs(l, 2);

    l->ent_index = index;
    VectorCopy(pick.ent->origin, l->ent_origin);
    LE_PlacementPoint(l->origin, LE_MarkerRadius(l));

    le_num_lights++;
    pick.ent->replaced = true;

    LE_PrintLight(l);
    LE_WriteLights();
}

/*
=================================================================

  the attribute table

  One row per name that "light edit" accepts, aliases included, so
  every property can be changed one at a time without retyping the
  others. The three colour channels are individually settable but
  share LE_HAVE_RGB, because a colour is one attribute as far as
  inheritance goes.

  A row with no `bit` is not inheritable and has no "default":
  origin is the light's own, never the entity's.

=================================================================
*/
typedef enum {
    LEA_RGB,        // three bytes
    LEA_CHANNEL,    // one byte into rgb[chan]
    LEA_BRIGHT,
    LEA_RADIUS,
    LEA_VOL,
    LEA_CONE,
    LEA_STYLE,
    LEA_AIM,        // no value: the camera's direction
    LEA_ORIGIN,     // no value: re-place in front of the camera
    LEA_COORD,      // one float into origin[chan]
    LEA_TRIGGER,    // a targetname; chan 1 = trigger_on, 0 = trigger_off
} leattrkind_t;

typedef struct {
    const char      *name;
    leattrkind_t    kind;
    unsigned        bit;        // 0 = not inheritable, no "default"
    int             chan;
} leattrdef_t;

static const leattrdef_t le_attrs[] = {
    { "rgb",        LEA_RGB,     LE_HAVE_RGB,    0 },
    { "color",      LEA_RGB,     LE_HAVE_RGB,    0 },
    { "colour",     LEA_RGB,     LE_HAVE_RGB,    0 },
    { "red",        LEA_CHANNEL, LE_HAVE_RGB,    0 },
    { "green",      LEA_CHANNEL, LE_HAVE_RGB,    1 },
    { "blue",       LEA_CHANNEL, LE_HAVE_RGB,    2 },
    { "bright",     LEA_BRIGHT,  LE_HAVE_BRIGHT, 0 },
    { "brightness", LEA_BRIGHT,  LE_HAVE_BRIGHT, 0 },
    { "radius",     LEA_RADIUS,  LE_HAVE_RADIUS, 0 },
    { "size",       LEA_RADIUS,  LE_HAVE_RADIUS, 0 },
    { "vol",        LEA_VOL,     LE_HAVE_VOL,    0 },
    { "volume",     LEA_VOL,     LE_HAVE_VOL,    0 },
    { "volscale",   LEA_VOL,     LE_HAVE_VOL,    0 },
    { "cone",       LEA_CONE,    LE_HAVE_CONE,   0 },
    { "angle",      LEA_CONE,    LE_HAVE_CONE,   0 },
    { "style",      LEA_STYLE,   LE_HAVE_STYLE,  0 },
    { "lightstyle", LEA_STYLE,   LE_HAVE_STYLE,  0 },
    { "aim",        LEA_AIM,     LE_HAVE_AIM,    0 },
    { "direction",  LEA_AIM,     LE_HAVE_AIM,    0 },
    { "origin",     LEA_ORIGIN,  0,              0 },
    { "move",       LEA_ORIGIN,  0,              0 },
    { "x",          LEA_COORD,   0,              0 },
    { "y",          LEA_COORD,   0,              1 },
    { "z",          LEA_COORD,   0,              2 },
    { "trigger_on", LEA_TRIGGER, LE_HAVE_TRIGGER, 1 },
    { "trigger_off", LEA_TRIGGER, LE_HAVE_TRIGGER, 0 },
};

/*
=================
LE_Edit_f

Either "light edit <attribute> <value>" or the older all-at-once
"light edit <r> <g> <b> <brightness> [radius] [volscale]". They are told apart by
whether the first argument is an attribute name, which can never be confused
with a number.

Every inheritable attribute takes "default" to unstate it, which sends it back to
the entity for a replacement light or to the plain default for a placed one.
=================
*/
static void LE_Edit_f(void)
{
    lepick_t    pick = LE_Pick();
    const char  *attr = Cmd_Argv(2);
    editlight_t *l;

    if (pick.ent) {
        Com_Printf("That is a map light entity - \"light replace\" builds a "
                   "light for it, then this can edit that.\n");
        return;
    }

    l = pick.light;
    if (!l) {
        Com_Printf("No light under the crosshair.\n");
        return;
    }

    if (!*attr) {
        Com_Printf("Usage: %s edit <attribute> <value|default>\n", Cmd_Argv(0));
        Com_Printf("   or: %s edit <r> <g> <b> <brightness> [radius] [volscale]\n",
                   Cmd_Argv(0));
        Com_Printf("Attributes: rgb r g b | red | green | blue | brightness |\n"
                   "            radius | vol | cone <deg|off> | style | aim |\n"
                   "            origin | x | y | z | trigger_on <name> |\n"
                   "            trigger_off <name>   (names: probe_trigger)\n");
        LE_PrintLight(l);
        return;
    }

    for (int i = 0; i < q_countof(le_attrs); i++) {
        const leattrdef_t *a = &le_attrs[i];
        // "aim" and "origin" are the two whose value is the camera rather than
        // something typed, so a bare "light edit aim" is a complete command.
        // Everything else needs a value, and only an EXPLICIT "default"
        // unstates - an ABSENT argument must not be read as one, which is a bug
        // this had until it was caught in play.
        bool has_value = Cmd_Argc() > 3;
        bool wants_default = has_value && LE_IsDefaultWord(Cmd_Argv(3));

        if (Q_stricmp(attr, a->name))
            continue;

        if (wants_default) {
            if (!a->bit) {
                Com_Printf("\"%s\" has no default to go back to.\n", attr);
                return;
            }
            if (a->kind == LEA_CHANNEL)
                Com_Printf("Unsetting the whole colour, not just %s - "
                           "the three channels are one attribute.\n", attr);
            l->have &= ~a->bit;
        } else if (a->kind == LEA_AIM) {
            // There is no sane way to type a direction. Standing where the
            // light should shine and saying "that way" is the whole idea of
            // editing inside the level.
            VectorCopy(cl.v_forward, l->aim);
            if (VectorNormalize(l->aim) <= 0.0f) {
                Com_Printf("No view direction to aim along.\n");
                return;
            }
            l->have |= LE_HAVE_AIM;
        } else if (a->kind == LEA_ORIGIN) {
            LE_PlacementPoint(l->origin, LE_MarkerRadius(l));
        } else if (!has_value) {
            Com_Printf("Usage: %s edit %s <value%s>\n", Cmd_Argv(0), attr,
                       a->bit ? "|default" : "");
            LE_PrintLight(l);
            return;
        } else {
            switch (a->kind) {
            case LEA_RGB:
                if (Cmd_Argc() < 6) {
                    Com_Printf("Usage: %s edit %s <r> <g> <b>\n", Cmd_Argv(0), attr);
                    return;
                }
                for (int c = 0; c < 3; c++)
                    l->rgb[c] = LE_ParseByte(Cmd_Argv(3 + c));
                break;

            case LEA_CHANNEL:
                // Setting ONE channel of a colour that is currently inherited
                // has to start from the inherited colour, or the other two
                // channels would come out of an uninitialised record.
                if (!(l->have & LE_HAVE_RGB)) {
                    leresolved_t r;

                    LE_Resolve(l, &r);
                    for (int c = 0; c < 3; c++) {
                        // NOTE this tree's clamp() ASSIGNS to its first
                        // argument, so it cannot be used on an expression
                        float v = r.color[c] * 255.0f + 0.5f;

                        clamp(v, 0.0f, 255.0f);
                        l->rgb[c] = (byte)v;
                    }
                }
                l->rgb[a->chan] = LE_ParseByte(Cmd_Argv(3));
                break;

            case LEA_BRIGHT:
                l->brightness = LE_ParseBrightness(Cmd_Argv(3));
                break;

            case LEA_RADIUS:
                l->radius = LE_ParseRadius(Cmd_Argv(3));
                break;

            case LEA_VOL:
                l->vol_scale = LE_ParseVolScale(Cmd_Argv(3));
                break;

            case LEA_CONE:
                // a stated cone of 0 is "point light", which is a different
                // thing from having no opinion about the cone at all
                if (!Q_stricmp(Cmd_Argv(3), "off"))
                    l->cone = 0.0f;
                else
                    l->cone = max(0.0f, (float)atof(Cmd_Argv(3)));
                break;

            case LEA_STYLE:
                l->style = atoi(Cmd_Argv(3));
                clamp(l->style, 0, MAX_LIGHTSTYLES - 1);
                break;

            case LEA_COORD:
                l->origin[a->chan] = atof(Cmd_Argv(3));
                break;

            case LEA_TRIGGER: {
                leswitch_t sw;

                // refuse a name that switches no light rather than store a
                // trigger that silently does nothing
                if (!LE_FindSwitch(Cmd_Argv(3), &sw)) {
                    LE_ExplainNoSwitch(Cmd_Argv(3));
                    return;
                }
                if (sw.mixed)
                    Com_WPrintf("The lights named \"%s\" disagree on style or "
                                "start state; following the first one.\n",
                                Cmd_Argv(3));

                Q_strlcpy(l->trigger, Cmd_Argv(3), sizeof(l->trigger));
                l->trigger_on = a->chan;
                break;
            }

            default:
                break;
            }

            l->have |= a->bit;
        }

        LE_BindTrigger(l);
        LE_PrintLight(l);
        LE_WriteLights();
        return;
    }

    // not an attribute name, so the older positional form
    if (Cmd_Argc() < 6) {
        Com_Printf("Unknown attribute \"%s\". Try one of:\n", attr);
        for (int i = 0; i < q_countof(le_attrs); i++)
            Com_Printf("%s%s", i ? " " : "  ", le_attrs[i].name);
        Com_Printf("\n");
        return;
    }

    LE_ParseCreateArgs(l, 2);

    LE_PrintLight(l);
    LE_WriteLights();
}

static void LE_Print_f(void)
{
    lepick_t    pick = LE_Pick();
    int         count = CL_NumDynamicLights();
    bool        all;

    // the one being aimed at if there is one, since that is what edit and
    // delete would act on; the whole list otherwise
    if (pick.light) {
        LE_PrintLight(pick.light);
        return;
    }
    if (pick.ent) {
        LE_PrintEntity(pick.ent);
        return;
    }

    if (!le_num_lights && !count) {
        Com_Printf("No lights and no map lights in %s.\n", cl.mapname);
        return;
    }

    if (le_num_lights) {
        Com_Printf("%d light%s in %s:\n",
                   le_num_lights, le_num_lights == 1 ? "" : "s", cl.mapname);

        for (int i = 0; i < le_num_lights; i++)
            LE_PrintLight(&le_lights[i]);
    }

    if (count) {
        // Above this many, only the replaced ones are listed: a map like mine1
        // has 69 and a wall of DORMANT lines buries the useful half.
        all = count <= LE_PRINT_ALL_LIMIT;

        Com_Printf("%d map light%s%s:\n", count, count == 1 ? "" : "s",
                   all ? "" : " (replaced only, too many to list)");

        for (int i = 0; i < count; i++) {
            const cdynamiclight_t *c = CL_GetDynamicLight(i);

            if (all || c->replaced)
                LE_PrintEntity(c);
        }
    }
}

/*
=================
LE_Styles_f

The lightstyles this map actually has, read out of the live configstrings rather
than from a hardcoded list, so anything the game or the map added shows up too.

'a' is black and 'z' is double brightness, so the string is the animation at
10hz - "aaaaaaaazzzzzzzz" is a slow strobe, 0.8s off and 0.8s on.
=================
*/
static void LE_Styles_f(void)
{
    int shown = 0;

    Com_Printf("Lightstyles ('a' is off, 'm' is normal, 'z' is double; 10hz):\n");

    for (int i = 0; i < MAX_LIGHTSTYLES; i++) {
        const char *s = LE_StyleString(i);

        if (!s)
            continue;

        Com_Printf("  %3d  %s\n", i, s);
        shown++;
    }

    if (!shown)
        Com_Printf("  none - the map has not set any\n");
    else
        Com_Printf("%d style%s. \"light edit style <n>\" to use one.\n",
                   shown, shown == 1 ? "" : "s");
}

/*
=================
LE_Vol_f

Shorthand for "light edit vol": the value most likely to be swept back and forth
while looking at the room.
=================
*/
static void LE_Vol_f(void)
{
    lepick_t pick = LE_Pick();

    if (!pick.light) {
        Com_Printf("No light of yours under the crosshair.\n");
        return;
    }

    if (Cmd_Argc() < 3) {
        Com_Printf("Usage: %s vol <scale|default>\n", Cmd_Argv(0));
        LE_PrintLight(pick.light);
        return;
    }

    if (LE_IsDefaultWord(Cmd_Argv(2))) {
        pick.light->have &= ~LE_HAVE_VOL;
    } else {
        pick.light->vol_scale = LE_ParseVolScale(Cmd_Argv(2));
        pick.light->have |= LE_HAVE_VOL;
    }

    LE_PrintLight(pick.light);
    LE_WriteLights();
}

static void LE_Delete_f(void)
{
    lepick_t        pick = LE_Pick();
    cdynamiclight_t *ent;
    int             index;

    if (pick.ent) {
        // A map light entity is in the BSP and cannot be deleted. If something
        // stands in for it, deleting THAT is the way back to dormant.
        Com_Printf(pick.ent->replaced
                   ? "That is a map light entity. Delete the light that stands "
                     "in for it instead.\n"
                   : "That is a dormant map light entity - there is nothing to "
                     "delete.\n");
        return;
    }

    if (!pick.light) {
        Com_Printf("No light under the crosshair.\n");
        return;
    }

    // hand the entity back before the array shifts under us
    ent = LE_Entity(pick.light);
    if (ent) {
        ent->replaced = false;
        Com_Printf("Map light at %.0f %.0f %.0f is dormant again.\n",
                   ent->origin[0], ent->origin[1], ent->origin[2]);
    }

    index = (int)(pick.light - le_lights);
    Com_Printf("Deleted light %d.\n", index);

    memmove(pick.light, pick.light + 1,
            sizeof(editlight_t) * (le_num_lights - index - 1));
    le_num_lights--;

    LE_WriteLights();
}

static void LE_Light_f(void)
{
    const char *cmd = Cmd_Argv(1);

    if (!*cmd) {
        Com_Printf("Usage: %s <place|replace|edit|vol|print|styles|delete|"
                   "debug_on|debug_off|reload|probe>\n", Cmd_Argv(0));
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
    else if (!strcmp(cmd, "replace"))
        LE_Replace_f();
    else if (!strcmp(cmd, "edit"))
        LE_Edit_f();
    else if (!strcmp(cmd, "vol"))
        LE_Vol_f();
    else if (!strcmp(cmd, "print"))
        LE_Print_f();
    else if (!strcmp(cmd, "styles"))
        LE_Styles_f();
    else if (!strcmp(cmd, "delete"))
        LE_Delete_f();
    else if (!strcmp(cmd, "reload"))
        LE_LoadLights();
    else if (!strcmp(cmd, "probe"))
        LE_Probe(Cmd_Argv(2));
    else
        Com_Printf("Unknown subcommand \"%s\".\n", cmd);
}

// Every name that switches lights, once each - what trigger_on and trigger_off
// accept, so the console can complete them.
static void LE_AddSwitchMatches(genctx_t *ctx)
{
    for (int i = 0; i < le_num_ents; i++) {
        const leent_t *e = &le_ents[i];
        int j;

        if (!LE_IsSwitchLight(e))
            continue;

        for (j = 0; j < i; j++)
            if (LE_IsSwitchLight(&le_ents[j]) &&
                !strcmp(le_ents[j].targetname, e->targetname))
                break;

        if (j == i)
            Prompt_AddMatch(ctx, e->targetname);
    }
}

static void LE_Light_c(genctx_t *ctx, int argnum)
{
    static const char *const subcommands[] = {
        "place", "replace", "edit", "vol", "print", "styles", "delete",
        "debug_on", "debug_off", "reload", "probe", NULL
    };

    if (argnum == 1) {
        for (int i = 0; subcommands[i]; i++)
            Prompt_AddMatch(ctx, subcommands[i]);
        return;
    }

    // the attribute names come straight off the table, so a new one is
    // completable the moment it is added there
    if (argnum == 2 && !Q_stricmp(Cmd_Argv(1), "edit")) {
        for (int i = 0; i < q_countof(le_attrs); i++)
            Prompt_AddMatch(ctx, le_attrs[i].name);
        return;
    }

    if (argnum == 2 && !Q_stricmp(Cmd_Argv(1), "probe")) {
        LE_AddSwitchMatches(ctx);
        return;
    }

    // "default" is a valid value for every inheritable attribute
    if (argnum == 3 && !Q_stricmp(Cmd_Argv(1), "edit")) {
        if (!Q_stricmp(Cmd_Argv(2), "trigger_on") ||
            !Q_stricmp(Cmd_Argv(2), "trigger_off"))
            LE_AddSwitchMatches(ctx);

        for (int i = 0; i < q_countof(le_attrs); i++)
            if (le_attrs[i].bit && !Q_stricmp(Cmd_Argv(2), le_attrs[i].name)) {
                Prompt_AddMatch(ctx, "default");
                break;
            }
    }
}

static void LE_ProbeTrigger_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1)
        LE_AddSwitchMatches(ctx);
}

static const cmdreg_t c_lightedit[] = {
    { "light", LE_Light_f, LE_Light_c },
    { "probe_trigger", LE_ProbeTrigger_f, LE_ProbeTrigger_c },
    { NULL }
};

/*
=================
LE_AddLightsToScene

Called every frame from CL_AddEntities, after the view axis is set up.
=================
*/

// Debug markers for the map's own dynamic_lights. Dormant is BLACK and replaced
// is a smaller black, so a glance at a room says which have been dealt with.
//
// Black works because these particles composite with PREMULTIPLIED alpha
// (pt_logic_particle multiplies rgb by a, then alpha_blend_premultiplied): rgb 0
// with alpha 1 is an opaque black disc, correctly occluded, not an invisible
// additive one. Which also means brightness cannot mark the selected one, since
// anything times zero is still zero - a picked marker grows instead. Lifting it
// to grey was tried and is wrong: under an auto-exposing tone mapper, in the
// dark rooms this tool is for, any nonzero grey blows out to a white disc.
static void LE_AddEntityMarkers(const cdynamiclight_t *picked)
{
    int count = CL_NumDynamicLights();

    for (int i = 0; i < count; i++) {
        const cdynamiclight_t *c = CL_GetDynamicLight(i);
        particle_t  p;

        memset(&p, 0, sizeof(p));
        VectorCopy(c->origin, p.origin);
        p.color = -1;               // use rgba rather than a palette index
        p.rgba.u8[3] = 255;         // rgb stays 0: an opaque black disc
        // Above 1 on purpose. The particle shader's radial falloff only reaches
        // full opacity at the exact centre, so alpha 1 draws a soft translucent
        // blob - which is why these were nearly invisible. pt_logic_particle
        // clamps after applying the falloff, so this reads as a solid disc with
        // a soft rim. See the note there.
        p.alpha = max(0.0f, light_marker_opacity->value);
        p.radius = LE_EntityMarkerRadius(c);
        p.brightness = light_debug_brightness->value;

        if (c == picked)
            p.radius *= 1.4f;

        V_AddParticle(&p);
    }
}

void LE_AddLightsToScene(void)
{
    lepick_t    pick = { NULL, NULL };

    // The map's own lights get markers even with nothing placed, so this cannot
    // bail on an empty list.
    if (le_debug) {
        pick = LE_Pick();
        LE_AddEntityMarkers(pick.ent);
    }

    for (int i = 0; i < le_num_lights; i++) {
        const editlight_t *l = &le_lights[i];
        leresolved_t    r;
        float           intensity;

        LE_Resolve(l, &r);

        // The markers are ordinary particles, which the vkpt back end draws as
        // world-space camera-facing quads of exactly particle_t.radius - the
        // same thing cl_show_lights does for the map's lights. The lights
        // themselves are analytic and only exist in the path tracer.
        if (le_debug) {
            particle_t p;

            memset(&p, 0, sizeof(p));
            VectorCopy(l->origin, p.origin);
            p.color = -1;
            p.rgba.u8[0] = (byte)(r.color[0] * 255.0f);
            p.rgba.u8[1] = (byte)(r.color[1] * 255.0f);
            p.rgba.u8[2] = (byte)(r.color[2] * 255.0f);
            p.rgba.u8[3] = 255;
            p.alpha = 1.0f;
            p.radius = r.radius;    // world units, so the marker is the emitter

            // whichever one edit and delete would act on reads brighter, so
            // there is never a question of which light is selected
            p.brightness = light_debug_brightness->value *
                           (l == pick.light ? 4.0f : 1.0f);

            // a light its trigger has dark right now reads dim, so a room can
            // be judged in both states without losing track of what is where
            if (LE_TriggerValue(l) <= 0.0f)
                p.brightness *= 0.25f;

            V_AddParticle(&p);
        }

        if (!light_enable->integer)
            continue;

        if (cls.ref_type != REF_TYPE_VKPT)
            continue;

        // A replacement light inherits its entity's switchability, so a light
        // the game turns off goes dark with it.
        if (!r.switched_on)
            continue;

        intensity = r.intensity;

        // and its lightstyle, so a flickering map light goes on flickering
        // after it has been retuned - and a hand-placed light can be given one
        if (r.style)
            intensity *= CL_LightStyleValue(r.style);

        // and the switch it has been tied to with trigger_on / trigger_off
        intensity *= LE_TriggerValue(l);

        if (intensity <= 0.0f)
            continue;

        if (r.is_spot) {
            // a targeted dynamic_light with no cone of its own is still a spot,
            // just with the engine's default angle
            float full = r.cone > 0.0f ? r.cone
                                       : Cvar_VariableValue("cl_dynamic_light_cone");
            float half = max(1.0f, full * 0.5f);

            // V_AddSpotLight takes half angles from the axis; the inner one
            // gives the cone a soft edge instead of a hard circle
            V_AddSpotLight(l->origin, r.aim, intensity,
                           r.color[0], r.color[1], r.color[2],
                           half, half * 0.75f);
        } else {
            V_AddSphereLight(l->origin, intensity,
                             r.color[0], r.color[1], r.color[2], r.radius);
        }

        // has to follow the add - it retunes the light just pushed. A light
        // still on LIGHT_VOLUMETRIC_SCALE_UNSET goes back to the class default,
        // which is exactly what passing the sentinel straight through does.
        V_SetLightVolumetricScale(r.vol_scale);
    }
}

void LE_Init(void)
{
    // Hand-placed lights are a separate system from the map's own dynamic_light
    // set on purpose, so cl_dynamic_lights does not take them with it. This is
    // their own switch.
    light_enable = Cvar_Get("light_enable", "1", CVAR_ARCHIVE);

    // Maps "brightness 255" onto engine light units. The map's own lights come
    // out around 128..2048, so full brightness here is comparable to a bright
    // one of those.
    light_scale = Cvar_Get("light_scale", "2000", CVAR_ARCHIVE);

    // How far in front of the eye a new light goes when nothing is in the way.
    light_place_dist = Cvar_Get("light_place_dist", "64", CVAR_ARCHIVE);

    // Floor on the crosshair pick radius, so a small light is still a
    // hittable target from across a room.
    light_pick_size = Cvar_Get("light_pick_size", "8", CVAR_ARCHIVE);

    // Brightness of the debug markers. They are emissive particles, so this
    // is the knob for stopping them washing out the room being judged.
    light_debug_brightness = Cvar_Get("light_debug_brightness", "1", CVAR_ARCHIVE);

    // How solid the map lights' black markers read. This is an alpha, but it
    // is deliberately allowed ABOVE 1: pt_logic_particle applies a radial
    // falloff and then clamps, so 1 is a soft translucent blob and 4 is a
    // solid disc with a soft rim. Matt could barely see them at 1.
    light_marker_opacity = Cvar_Get("light_marker_opacity", "4", CVAR_ARCHIVE);

    Cmd_Register(c_lightedit);
}
