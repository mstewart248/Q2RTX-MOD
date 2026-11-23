/*
Copyright (C) 1999-2005 Id Software, Inc.
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
// hud_q3.c -- Quake III Arena's HUD, for a Quake III character
//
// When the player in view wears a Quake III model (q3player.c), the status bar
// is Quake III's (cgame cg_draw.c, as it drew with cg_draw3dIcons 0): ammo,
// the head and health, armor along the bottom in its big amber numbers; the
// item just picked up at the lower left; the score boxes and the powerup timer
// at the lower right; the holdable item at the right; "following" over a
// chased player. Drawn from Quake II's stats, with Quake III's art from baseq3.
//
// It takes the place of what it shows in the CS_STATUSBAR program, which still
// runs for the rest (the spectator line, CTF's): screen.c skips those tokens.
// cl_q3_hud 0 turns it off, 2 gives it to every model.
//
// Quake III's 640x480 is scaled by the HUD's height and anchored as its pieces
// were - the bar in the middle of the bottom, the pickup at the left edge, the
// scores and powerups at the right - so a wide screen spreads them out rather
// than stretching them.

#include "client.h"

#define Q3_ICON_SIZE        48
#define Q3_CHAR_WIDTH       32
#define Q3_CHAR_HEIGHT      48
#define Q3_TEXT_ICON_SPACE  4
#define Q3_BIGCHAR          16
#define Q3_GIANT_WIDTH      32
#define Q3_GIANT_HEIGHT     48

#define Q3_DAMAGE_TIME      500     // the head swells after a hit
#define Q3_PULSE_TIME       200     // a new powerup's icon swells
#define Q3_PULSE_SCALE      1.5f
#define Q3_PICKUP_TIME      3000
#define Q3_FADE_TIME        200
#define Q3_POWERUP_BLINKS   5       // the last seconds of a powerup blink

static cvar_t   *cl_q3_hud;
static cvar_t   *cl_q3_hud_scale;
static cvar_t   *cl_q3_hud_3d;

// cg_draw.c CG_DrawStatusBar's
static const float q3_colors[4][4] = {
    { 1.0f, 0.69f, 0.0f, 1.0f },    // normal
    { 1.0f, 0.2f, 0.2f, 1.0f },     // low health
    { 0.5f, 0.5f, 0.5f, 1.0f },     // weapon firing
    { 1.0f, 1.0f, 1.0f, 1.0f },     // health > 100
};
static const float q3_white[4] = { 1, 1, 1, 1 };
static const float q3_black[4] = { 0, 0, 0, 1 };
static const float q3_powerup_red[4] = { 1.0f, 0.2f, 0.2f, 1.0f };

// a Quake III gun by its view model (CS_MODELS, which ps.gunindex names), and
// the ammo icon Quake III's bar shows for it - flat, or its ammo pickup's model
// (bg_misc.c's ammo_* world_model, as cg_weapons.c CG_RegisterWeapon takes it)
static const struct {
    const char  *dir;       // models/weapons2/<dir>/
    const char  *icon;
    const char  *model;
} q3_ammo_icons[] = {
    { "machinegun", "/icons/icona_machinegun.tga",  "models/powerups/ammo/machinegunam.md3" },
    { "shotgun",    "/icons/icona_shotgun.tga",     "models/powerups/ammo/shotgunam.md3" },
    { "grenadel",   "/icons/icona_grenade.tga",     "models/powerups/ammo/grenadeam.md3" },
    { "rocketl",    "/icons/icona_rocket.tga",      "models/powerups/ammo/rocketam.md3" },
    { "lightning",  "/icons/icona_lightning.tga",   "models/powerups/ammo/lightningam.md3" },
    { "railgun",    "/icons/icona_railgun.tga",     "models/powerups/ammo/railgunam.md3" },
    { "plasma",     "/icons/icona_plasma.tga",      "models/powerups/ammo/plasmaam.md3" },
    { "bfg",        "/icons/icona_bfg.tga",         "models/powerups/ammo/bfgam.md3" },
};

static struct {
    bool        registered;
    qhandle_t   numbers[11];        // 0-9, minus
    qhandle_t   bigchars;
    qhandle_t   select;
    qhandle_t   armor;
    qhandle_t   quad;
    qhandle_t   ammo[q_countof(q3_ammo_icons)];
    qhandle_t   ammo_model[q_countof(q3_ammo_icons)];
    qhandle_t   armor_model;

    char        head_for[MAX_QPATH * 2];    // the model/skin the head is
    qhandle_t   head;

    // the 3D head's idle turns, and its kick from a hit (CG_DrawStatusBarHead)
    float       head_start_yaw, head_end_yaw;
    float       head_start_pitch, head_end_pitch;
    int         head_start_time, head_end_time;

    int         pickup_icon;        // STAT_PICKUP_ICON and _STRING as last seen
    int         pickup_string;
    int         pickup_time;        // cl.time they came

    int         frame;              // the server frame last looked at
    int         damage_time;        // cl.time of the last hit, 0 = none
    float       damage_x;           // where from: -1 left .. 1 right
    float       hit_x;              // TE_DAMAGE_INDICATOR's, at hit_time
    int         hit_time;

    int         timer_icon;         // STAT_TIMER_ICON and STAT_TIMER as last seen
    int         timer_value;
    int         timer_tick;         // cl.time STAT_TIMER last changed
    int         powerup_time;       // cl.time a new powerup came, for its pulse
} q3h;

// Quake III's 640x480 to the HUD's: scale, and where its middle 640 starts
static float    q3_s, q3_cx, q3_w, q3_h;
static float    q3_alpha;

static float q3_clipf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int q3_clip(int v, int lo, int hi)        { return v < lo ? lo : v > hi ? hi : v; }

static float q3_mid(float x)    { return q3_cx + x * q3_s; }           // the bar
static float q3_left(float x)   { return x * q3_s; }                    // the left edge
static float q3_right(float x)  { return q3_w - (640 - x) * q3_s; }     // the right edge
static float q3_bottom(float y) { return q3_h - (480 - y) * q3_s; }
static float q3_top(float y)    { return y * q3_s; }
static float q3_vmid(float y)   { return q3_h * 0.5f + (y - 240) * q3_s; }

static void q3_color(const float *c, float alpha)
{
    float a = c[3] * alpha * q3_alpha;

    R_SetColor(MakeColor((int)(q3_clipf(c[0], 0, 1) * 255), (int)(q3_clipf(c[1], 0, 1) * 255),
                         (int)(q3_clipf(c[2], 0, 1) * 255), (int)(q3_clipf(a, 0, 1) * 255)));
}

// x, y, w, h in the HUD's own units
static void q3_pic(float x, float y, float w, float h, qhandle_t pic)
{
    if (pic)
        R_DrawStretchSubPic(x, y, w, h, 0, 0, 1, 1, pic);
}

/*
===============
Pictures
===============
*/

// filtered, as Quake III drew its 2D art (ui/q3sp.c SP_PicAs says why the flag)
static qhandle_t q3_register(const char *name, imagetype_t type)
{
    return R_RegisterImage(name, type, IF_SRGB | IF_BILERP, NULL);
}

static void q3_register_media(void)
{
    static const char *const numbers[11] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "minus"
    };
    int i;

    for (i = 0; i < 11; i++)
        q3h.numbers[i] = q3_register(va("/gfx/2d/numbers/%s_32b.tga", numbers[i]), IT_PIC);
    q3h.bigchars = q3_register("/gfx/2d/bigchars.tga", IT_PIC);
    q3h.select = q3_register("/gfx/2d/select.tga", IT_PIC);
    q3h.armor = q3_register("/icons/iconr_yellow.tga", IT_PIC);
    q3h.quad = q3_register("/icons/quad.tga", IT_PIC);
    for (i = 0; i < q_countof(q3_ammo_icons); i++)
        q3h.ammo[i] = q3_register(q3_ammo_icons[i].icon, IT_PIC);

    // the 3D icons (cg_draw3dIcons): only for a renderer that draws them
    for (i = 0; i < q_countof(q3_ammo_icons); i++)
        q3h.ammo_model[i] = R_DrawModel2D ? R_RegisterModel(q3_ammo_icons[i].model) : 0;
    q3h.armor_model = R_DrawModel2D ? R_RegisterModel("models/powerups/armor/armor_yel.md3") : 0;

    q3h.head_for[0] = 0;
    q3h.head = 0;
    q3h.registered = true;
}

/*
===============
SCR_Q3HudRegisterMedia

At every level's CL_PrepRefresh, so the pictures are touched in its
registration and stay; when nobody can be a Quake III model, on the first
frame that needs them instead.
===============
*/
void SCR_Q3HudRegisterMedia(void)
{
    q3h.registered = false;
    q3h.head_for[0] = 0;
    q3h.head = 0;
    q3h.frame = 0;
    q3h.damage_time = 0;
    q3h.pickup_icon = q3h.pickup_string = 0;
    q3h.timer_icon = q3h.timer_value = 0;

    if (cl_q3_hud && cl_q3_hud->integer && CL_Q3PlayersEnabled())
        q3_register_media();
}

// a renderer restart freed them all
void SCR_Q3HudClearMedia(void)
{
    q3h.registered = false;
    q3h.head_for[0] = 0;
    q3h.head = 0;
}

// The head on the bar: the model's icon in its skin, else its default - its
// own copy, filtered (ui/q3sp.c SP_PlayerIcon), not the scoreboard's
static qhandle_t q3_head(const clientinfo_t *ci)
{
    char key[sizeof(q3h.head_for)];
    char path[MAX_QPATH];

    Q_concat(key, sizeof(key), ci->model_name, "/", ci->skin_name);
    if (!strcmp(key, q3h.head_for))
        return q3h.head;
    Q_strlcpy(q3h.head_for, key, sizeof(q3h.head_for));

    q3h.head = 0;
    if (ci->q3.q3) {
        Q_concat(path, sizeof(path), "/models/players/", ci->model_name, "/icon_", ci->skin_name, ".tga");
        q3h.head = q3_register(path, IT_SPRITE);
        if (!q3h.head) {
            Q_concat(path, sizeof(path), "/models/players/", ci->model_name, "/icon_default.tga");
            q3h.head = q3_register(path, IT_SPRITE);
        }
    }
    if (!q3h.head)
        q3h.head = ci->icon ? ci->icon : cl.baseclientinfo.icon;
    return q3h.head;
}

static bool q3_model_icon(float x, float y, float w, float h, qhandle_t model, int skinnum,
                          const vec3_t origin, const vec3_t angles);

/*
The 3D head (CG_DrawStatusBarHead, CG_DrawHead): the model's head.md3 in its
skin, framed by its bounds so it nearly fills the box, nudged by its
animation.cfg headoffset. It turns idly - a new angle every 0.1-2.1 s, eased
there - and while the bar's head swells from a hit it is held turned away from
the side the hit came from. false: the flat icon instead.
*/
static bool q3_head_model(const clientinfo_t *ci, float x, float y, float size)
{
    vec3_t  mins, maxs, origin, offset, angles;
    float   len, frac;

    if (!ci->q3.q3 || !ci->q3.head || !R_DrawModel2D || !R_ModelBounds || !cl_q3_hud_3d->integer)
        return false;
    if (!R_ModelBounds(ci->q3.head, mins, maxs))
        return false;

    if (q3h.damage_time && cl.time - q3h.damage_time < Q3_DAMAGE_TIME) {
        q3h.head_start_yaw = 180 + q3h.damage_x * 45;
        q3h.head_end_yaw = 180 + 20 * cosf(crand() * M_PI);
        q3h.head_end_pitch = 5 * cosf(crand() * M_PI);
        q3h.head_start_time = cl.time;
        q3h.head_end_time = cl.time + 100 + (int)(frand() * 2000);
    } else if (cl.time >= q3h.head_end_time) {
        // a new angle to turn to; the first time, from facing straight out
        if (!q3h.head_end_time) {
            q3h.head_end_yaw = 180;
            q3h.head_end_pitch = 0;
            q3h.head_end_time = cl.time;
        }
        q3h.head_start_yaw = q3h.head_end_yaw;
        q3h.head_start_pitch = q3h.head_end_pitch;
        q3h.head_start_time = q3h.head_end_time;
        q3h.head_end_time = cl.time + 100 + (int)(frand() * 2000);
        q3h.head_end_yaw = 180 + 20 * cosf(crand() * M_PI);
        q3h.head_end_pitch = 5 * cosf(crand() * M_PI);
    }
    // a map change or a demo seek put the clock behind it
    if (q3h.head_start_time > cl.time)
        q3h.head_start_time = cl.time;
    if (q3h.head_end_time - q3h.head_start_time > 2200 || q3h.head_end_time - cl.time > 2200)
        q3h.head_end_time = q3h.head_start_time = cl.time;

    frac = q3h.head_end_time > q3h.head_start_time
        ? (float)(cl.time - q3h.head_start_time) / (q3h.head_end_time - q3h.head_start_time) : 1;
    frac = q3_clipf(frac, 0, 1);
    frac = frac * frac * (3 - 2 * frac);
    VectorSet(angles, q3h.head_start_pitch + (q3h.head_end_pitch - q3h.head_start_pitch) * frac,
              q3h.head_start_yaw + (q3h.head_end_yaw - q3h.head_start_yaw) * frac, 0);

    // y and z centre the head; x is far enough that it nearly fills the box -
    // heads are taller than wide - at the 30 degree view (tan 15 = 0.268)
    origin[2] = -0.5f * (mins[2] + maxs[2]);
    origin[1] = 0.5f * (mins[1] + maxs[1]);
    len = 0.7f * (maxs[2] - mins[2]);
    origin[0] = len / 0.268f;
    Q3P_HeadOffset(ci->q3.q3, offset);
    VectorAdd(origin, offset, origin);

    return q3_model_icon(x, y, size, size, ci->q3.head, ci->q3.skin, origin, angles);
}

static qhandle_t q3_stat_pic(int stat)
{
    int index = cl.frame.ps.stats[stat];

    if (index <= 0 || index >= MAX_IMAGES || !cl.configstrings[CS_IMAGES + index][0])
        return 0;
    return cl.image_precache[index];
}

static const char *q3_stat_pic_name(int stat)
{
    int index = cl.frame.ps.stats[stat];

    if (index <= 0 || index >= MAX_IMAGES)
        return "";
    return cl.configstrings[CS_IMAGES + index];
}

// the Quake III gun in hand's row of q3_ammo_icons, or -1 for any other gun
static int q3_gun_ammo_index(void)
{
    const char  *model, *dir;
    int         i;
    size_t      len;

    if (cl.frame.ps.gunindex <= 0 || cl.frame.ps.gunindex >= MAX_MODELS)
        return -1;
    model = cl.configstrings[CS_MODELS + cl.frame.ps.gunindex];
    if (Q_strncasecmp(model, "models/weapons2/", 16))
        return -1;
    dir = model + 16;
    for (i = 0; i < q_countof(q3_ammo_icons); i++) {
        len = strlen(q3_ammo_icons[i].dir);
        if (!Q_strncasecmp(dir, q3_ammo_icons[i].dir, len) && dir[len] == '/')
            return i;
    }
    return -1;
}

/*
A model as a HUD icon (cg_draw.c CG_Draw3DModel under cg_draw3dIcons): in a
camera of its own looking down +X, 30 degrees each way across the box x, y,
w, h (the HUD's units), drawn over the view by the renderer (R_DrawModel2D).
false: not drawn - cl_q3_hud_3d 0, a renderer without it, no model - and the
caller draws the flat icon.
*/
static bool q3_model_icon(float x, float y, float w, float h, qhandle_t model, int skinnum,
                          const vec3_t origin, const vec3_t angles)
{
    entity_t ent;

    if (!model || !R_DrawModel2D || !cl_q3_hud_3d->integer)
        return false;

    memset(&ent, 0, sizeof(ent));
    ent.model = model;
    ent.skinnum = skinnum;
    VectorCopy(origin, ent.origin);
    VectorCopy(origin, ent.oldorigin);
    VectorCopy(angles, ent.angles);
    return R_DrawModel2D(x, y, w, h, 30, 30, &ent);
}

/*
===============
Drawing, as cg_draw.c did
===============
*/

// CG_DrawField: right-aligned in (width) places of the big numbers
static void q3_field(float x, float y, int width, int value)
{
    char        num[16];
    const char  *ptr;
    int         l;

    switch (width) {
    case 1: value = q3_clip(value, 0, 9); break;
    case 2: value = q3_clip(value, -9, 99); break;
    case 3: value = q3_clip(value, -99, 999); break;
    default: value = q3_clip(value, -999, 9999); break;
    }

    l = Q_snprintf(num, sizeof(num), "%i", value);
    if (l > width)
        l = width;
    x += (2 + Q3_CHAR_WIDTH * (width - l)) * q3_s;

    for (ptr = num; *ptr && l; ptr++, l--) {
        q3_pic(x, y, Q3_CHAR_WIDTH * q3_s, Q3_CHAR_HEIGHT * q3_s,
               q3h.numbers[*ptr == '-' ? 10 : *ptr - '0']);
        x += Q3_CHAR_WIDTH * q3_s;
    }
}

// characters a string draws, without Quake III's ^ colour codes
static int q3_strlen(const char *s)
{
    int n = 0;

    for (; *s; s++) {
        if (s[0] == '^' && s[1] && s[1] != '^') {
            s++;
            continue;
        }
        n++;
    }
    return n;
}

// CG_DrawStringExt in gfx/2d/bigchars, with its drop shadow; w and h are a
// character's size in 640x480
static void q3_string(float x, float y, const char *str, float w, float h, const float *color, float alpha)
{
    const float cw = w * q3_s, ch = h * q3_s, off = 2 * q3_s;
    const char  *s;
    float       fx;
    int         pass;

    if (!q3h.bigchars)
        return;

    for (pass = 0; pass < 2; pass++) {
        q3_color(pass ? color : q3_black, alpha);
        fx = x;
        for (s = str; *s; s++) {
            int c;

            if (s[0] == '^' && s[1] && s[1] != '^') {
                s++;
                continue;
            }
            c = *s & 127;
            if (c != ' ')
                R_DrawStretchSubPic(fx + (pass ? 0 : off), y + (pass ? 0 : off), cw, ch,
                                    (c & 15) * 0.0625f, (c >> 4) * 0.0625f,
                                    (c & 15) * 0.0625f + 0.0625f, (c >> 4) * 0.0625f + 0.0625f,
                                    q3h.bigchars);
            fx += cw;
        }
    }
}

// a player's name from a CS_PLAYERSKINS string ("name\model/skin")
static void q3_configstring_name(int index, char *out, size_t size)
{
    char *p;

    *out = 0;
    if (cl.old_configstrings)
        index = CL_RemapOldConfigstring(index);
    if (index < 0 || index >= MAX_CONFIGSTRINGS)
        return;
    Q_strlcpy(out, cl.configstrings[index], size);
    p = strchr(out, '\\');
    if (p)
        *p = 0;
}

/*
===============
What the client works out for itself: when each thing came
===============
*/
static void q3_track(void)
{
    const short *stats = cl.frame.ps.stats;

    // the item picked up: the game holds its stats for three seconds
    if (stats[STAT_PICKUP_ICON] != q3h.pickup_icon || stats[STAT_PICKUP_STRING] != q3h.pickup_string) {
        if (stats[STAT_PICKUP_ICON] || stats[STAT_PICKUP_STRING])
            q3h.pickup_time = cl.time;
        q3h.pickup_icon = stats[STAT_PICKUP_ICON];
        q3h.pickup_string = stats[STAT_PICKUP_STRING];
    }

    // a hit: the frame's damage flashes (p_view.c), the side it came from if
    // the game said (TE_DAMAGE_INDICATOR, with cl_rerelease_hud)
    if (cl.frame.valid && cl.frame.number != q3h.frame) {
        q3h.frame = cl.frame.number;
        if (stats[STAT_FLASHES] & 3) {
            q3h.damage_time = cl.time;
            q3h.damage_x = (q3h.hit_time && cl.time - q3h.hit_time < 200) ? q3h.hit_x : 0;
        }
    }

    // the powerup timer: a new one pulses, and the seconds count down between
    // the server's whole ones so the last few can blink
    if (stats[STAT_TIMER_ICON] != q3h.timer_icon) {
        if (stats[STAT_TIMER_ICON])
            q3h.powerup_time = cl.time;
        q3h.timer_icon = stats[STAT_TIMER_ICON];
        q3h.timer_tick = cl.time;
    }
    if (stats[STAT_TIMER] != q3h.timer_value) {
        q3h.timer_value = stats[STAT_TIMER];
        q3h.timer_tick = cl.time;
    }
}

/*
===============
SCR_Q3HudDamage

TE_DAMAGE_INDICATOR: dir points from the source to the player. Which side it
came from, across the view, for the head's kick (cg_view.c CG_DamageFeedback's
damageX).
===============
*/
void SCR_Q3HudDamage(const vec3_t dir)
{
    vec3_t  angles, right;

    VectorSet(angles, 0, cl.refdef.viewangles[YAW], 0);
    AngleVectors(angles, NULL, right, NULL);
    q3h.hit_x = q3_clipf(-DotProduct(dir, right), -1, 1);
    q3h.hit_time = cl.time;
}

/*
===============
The pieces
===============
*/

// CG_DrawStatusBar: ammo, head and health, armor
static void q3_status_bar(const clientinfo_t *ci)
{
    const short *stats = cl.frame.ps.stats;
    const float y = q3_bottom(432);
    const float icon = Q3_ICON_SIZE * q3_s;
    int         value, color;
    float       size, x;
    vec3_t      origin, angles;

    // ammo, for a gun that uses it: its ammo box turning to and fro, else the icon
    if (stats[STAT_AMMO_ICON] && stats[STAT_AMMO] >= 0) {
        int         gun = q3_gun_ammo_index();
        qhandle_t   pic = gun >= 0 ? q3h.ammo[gun] : 0;
        const float ix = q3_mid(Q3_CHAR_WIDTH * 3 + Q3_TEXT_ICON_SPACE);

        q3_color(q3_colors[0], 1);
        q3_field(q3_mid(0), y, 3, stats[STAT_AMMO]);
        q3_color(q3_white, 1);
        VectorSet(origin, 70, 0, 0);
        VectorSet(angles, 0, 90 + 20 * sinf(cl.time / 1000.0f), 0);
        if (gun < 0 || !q3_model_icon(ix, y, icon, icon, q3h.ammo_model[gun], 0, origin, angles)) {
            if (!pic)
                pic = q3_stat_pic(STAT_AMMO_ICON);
            q3_pic(ix, y, icon, icon, pic);
        }
    }

    // the head (CG_DrawStatusBarHead), which swells and kicks away from a hit
    x = 185 + Q3_CHAR_WIDTH * 3 + Q3_TEXT_ICON_SPACE;
    size = Q3_ICON_SIZE * 1.25f;
    if (q3h.damage_time && cl.time - q3h.damage_time < Q3_DAMAGE_TIME) {
        float frac = (float)(cl.time - q3h.damage_time) / Q3_DAMAGE_TIME;
        float stretch;

        size = Q3_ICON_SIZE * 1.25f * (1.5f - frac * 0.5f);
        stretch = size - Q3_ICON_SIZE * 1.25f;
        x -= stretch * 0.5f + q3h.damage_x * stretch * 0.5f;
    }
    q3_color(q3_white, 1);
    if (!q3_head_model(ci, q3_mid(x), q3_bottom(480 - size), size * q3_s))
        q3_pic(q3_mid(x), q3_bottom(480 - size), size * q3_s, size * q3_s, q3_head(ci));

    // health
    value = stats[STAT_HEALTH];
    if (value > 100)
        color = 3;
    else if (value > 25)
        color = 0;
    else if (value > 0)
        color = (cl.time >> 8) & 1;     // flash
    else
        color = 1;
    q3_color(q3_colors[color], 1);
    q3_field(q3_mid(185), y, 3, value);

    // armor: Quake III's one icon, or the power armor in use
    value = stats[STAT_ARMOR];
    if (stats[STAT_ARMOR_ICON] && value > 0) {
        const char  *name = q3_stat_pic_name(STAT_ARMOR_ICON);
        qhandle_t   pic = q3h.armor;

        const bool  power = !Q_stricmp(name, "i_powershield") || !Q_stricmp(name, "i_powerscreen");
        const float ix = q3_mid(370 + Q3_CHAR_WIDTH * 3 + Q3_TEXT_ICON_SPACE);

        if (power || !pic)
            pic = q3_stat_pic(STAT_ARMOR_ICON);
        q3_color(q3_colors[0], 1);
        q3_field(q3_mid(370), y, 3, value);
        q3_color(q3_white, 1);
        // Quake III's yellow armor spinning, once round in 2048 ms; the power
        // armor, Quake II's, keeps its icon
        VectorSet(origin, 90, 0, -10);
        VectorSet(angles, 0, (cl.time & 2047) * 360 / 2048.0f, 0);
        if (power || !q3_model_icon(ix, y, icon, icon, q3h.armor_model, 0, origin, angles))
            q3_pic(ix, y, icon, icon, pic);
    }
}

// CG_DrawPickupItem, at the lower left
static void q3_pickup(void)
{
    const short *stats = cl.frame.ps.stats;
    const float y = 480 - Q3_ICON_SIZE * 2;
    int         elapsed = cl.time - q3h.pickup_time;
    int         index;
    float       alpha = 1;

    if (!stats[STAT_PICKUP_ICON] && !stats[STAT_PICKUP_STRING])
        return;
    if (elapsed < 0 || elapsed >= Q3_PICKUP_TIME)
        return;
    if (elapsed > Q3_PICKUP_TIME - Q3_FADE_TIME)
        alpha = (float)(Q3_PICKUP_TIME - elapsed) / Q3_FADE_TIME;

    q3_color(q3_white, alpha);
    q3_pic(q3_left(8), q3_bottom(y), Q3_ICON_SIZE * q3_s, Q3_ICON_SIZE * q3_s, q3_stat_pic(STAT_PICKUP_ICON));

    index = stats[STAT_PICKUP_STRING];
    if (cl.old_configstrings)
        index = CL_RemapOldConfigstring(index);
    if (index > 0 && index < MAX_CONFIGSTRINGS)
        q3_string(q3_left(Q3_ICON_SIZE + 16), q3_bottom(y + Q3_ICON_SIZE / 2 - Q3_BIGCHAR / 2),
                  cl.configstrings[index], Q3_BIGCHAR, Q3_BIGCHAR, q3_white, alpha);
}

// one of CG_DrawScores' boxes, right to left from *x
static void q3_score_box(float *x, float y, int score, const float *fill, bool select)
{
    char    s[16];
    float   w;

    Q_snprintf(s, sizeof(s), "%2i", score);
    w = q3_strlen(s) * Q3_BIGCHAR + 8;
    *x -= w;
    if (fill) {
        R_DrawFill32(q3_right(*x), q3_bottom(y - 4), w * q3_s, (Q3_BIGCHAR + 8) * q3_s,
                     MakeColor((int)(fill[0] * 255), (int)(fill[1] * 255), (int)(fill[2] * 255),
                               (int)(fill[3] * q3_alpha * 255)));
        if (select) {
            q3_color(q3_white, 1);
            q3_pic(q3_right(*x), q3_bottom(y - 4), w * q3_s, (Q3_BIGCHAR + 8) * q3_s, q3h.select);
        }
    }
    q3_string(q3_right(*x + 4), q3_bottom(y), s, Q3_BIGCHAR, Q3_BIGCHAR, q3_white, 1);
}

// CG_DrawScores in a free-for-all: the leader's box and the second's, the
// player's own picked out - blue in the lead, red behind - and the fraglimit
static bool q3_scores(float y)
{
    static const float blue[4] = { 0, 0, 1, 0.33f };
    static const float red[4] = { 1, 0, 0, 0.33f };
    static const float grey[4] = { 0.5f, 0.5f, 0.5f, 0.33f };
    const short *stats = cl.frame.ps.stats;
    const bool  spectator = stats[STAT_SPECTATOR] && !stats[STAT_CHASE];
    int         score = stats[STAT_FRAGS];
    int         other = stats[STAT_Q3_OTHER_SCORE];
    bool        any = stats[STAT_Q3_SCORES] & STAT_Q3_SCORES_OTHER;
    int         s1, s2;
    float       x = 640;

    if (!CL_GameOwnsExtraStats() || (stats[STAT_Q3_SCORES] & ~STAT_Q3_SCORES_OTHER) != STAT_Q3_SCORES_TAG)
        return false;

    // a spectator has no score of their own: the leader's alone
    if (spectator) {
        if (!any)
            return true;
        q3_score_box(&x, y, other, grey, false);
        if (stats[STAT_Q3_FRAGLIMIT] > 0)
            q3_score_box(&x, y, stats[STAT_Q3_FRAGLIMIT], NULL, false);
        return true;
    }

    // cgs.scores1 and scores2: first and second place, the player in one
    if (!any || score >= other) {
        s1 = score;
        s2 = other;
    } else {
        s1 = other;
        s2 = score;
    }

    if (any) {
        if (score == s2 && score != s1)
            q3_score_box(&x, y, s2, red, true);
        else
            q3_score_box(&x, y, s2, grey, false);
    }
    if (score == s1)
        q3_score_box(&x, y, s1, blue, true);
    else
        q3_score_box(&x, y, s1, grey, false);
    if (stats[STAT_Q3_FRAGLIMIT] > 0)
        q3_score_box(&x, y, stats[STAT_Q3_FRAGLIMIT], NULL, false);

    return true;
}

// CG_DrawPowerups: the timer, its icon swelling as it comes and blinking for
// the last seconds. Quake II sends the soonest to run out.
static void q3_powerup(float y)
{
    const short *stats = cl.frame.ps.stats;
    const char  *name;
    qhandle_t   pic;
    float       size, left;
    int         ms;

    if (!stats[STAT_TIMER_ICON] || stats[STAT_TIMER] <= 0)
        return;

    name = q3_stat_pic_name(STAT_TIMER_ICON);
    pic = !Q_stricmp(name, "p_quad") && q3h.quad ? q3h.quad : q3_stat_pic(STAT_TIMER_ICON);

    y -= Q3_ICON_SIZE;
    q3_color(q3_powerup_red, 1);
    q3_field(q3_right(640 - Q3_ICON_SIZE - Q3_CHAR_WIDTH * 2), q3_bottom(y), 2, stats[STAT_TIMER]);

    // the time left, between the server's whole seconds
    ms = stats[STAT_TIMER] * 1000 - (cl.time - q3h.timer_tick);
    if (ms < 0)
        ms = 0;
    if (ms >= Q3_POWERUP_BLINKS * 1000) {
        q3_color(q3_white, 1);
    } else {
        float f = ms / 1000.0f;
        float modulate[4];

        f -= (int)f;
        modulate[0] = modulate[1] = modulate[2] = modulate[3] = f;
        q3_color(modulate, 1);
    }

    size = Q3_ICON_SIZE;
    if (cl.time - q3h.powerup_time < Q3_PULSE_TIME) {
        float f = 1.0f - (float)(cl.time - q3h.powerup_time) / Q3_PULSE_TIME;
        size = Q3_ICON_SIZE * (1.0f + (Q3_PULSE_SCALE - 1.0f) * f);
    }
    left = q3_w - size * q3_s;
    q3_pic(left, q3_bottom(y + Q3_ICON_SIZE / 2 - size / 2), size * q3_s, size * q3_s, pic);
}

// CG_DrawHoldableItem, at the middle of the right edge: Quake II's selected
// item. Quake II's held keys (cl_rerelease_hud) under it.
static void q3_holdable(void)
{
    const float icon = Q3_ICON_SIZE * q3_s;
    float       y = (480 - Q3_ICON_SIZE) / 2;
    int         i;

    q3_color(q3_white, 1);
    q3_pic(q3_right(640 - Q3_ICON_SIZE), q3_vmid(y), icon, icon, q3_stat_pic(STAT_SELECTED_ICON));

    y += Q3_ICON_SIZE + 8;
    for (i = STAT_KEY_A; i <= STAT_KEY_C; i++) {
        qhandle_t pic = CL_GameOwnsExtraStats() ? q3_stat_pic(i) : 0;

        if (!pic)
            continue;
        q3_pic(q3_right(640 - 40), q3_vmid(y), 32 * q3_s, 32 * q3_s, pic);
        y += 36;
    }
}

// Quake II's help icon, when there is news on the help computer: over the
// pickup at the lower left
static void q3_help(void)
{
    if (Q_stricmp(q3_stat_pic_name(STAT_HELPICON), "i_help"))
        return;
    q3_color(q3_white, 1);
    q3_pic(q3_left(8), q3_bottom(480 - Q3_ICON_SIZE * 3 - 8), Q3_ICON_SIZE * q3_s, Q3_ICON_SIZE * q3_s,
           q3_stat_pic(STAT_HELPICON));
}

// CG_DrawFollow: whom a spectator is watching
static void q3_follow(void)
{
    char    name[MAX_QPATH];
    float   x;

    if (!cl.frame.ps.stats[STAT_CHASE])
        return;
    q3_configstring_name(cl.frame.ps.stats[STAT_CHASE], name, sizeof(name));

    q3_string(q3_mid(320 - 9 * 8), q3_top(24), "following", Q3_BIGCHAR, Q3_BIGCHAR, q3_white, 1);
    x = 0.5f * (640 - Q3_GIANT_WIDTH * q3_strlen(name));
    q3_string(q3_mid(x), q3_top(40), name, Q3_GIANT_WIDTH, Q3_GIANT_HEIGHT, q3_white, 1);
}

/*
===============
SCR_Q3HudDrawsStat

The CS_STATUSBAR program's "pic <stat>"s that Quake III's HUD draws in its
own places: health, ammo and armor icons, the selected item, the pickup, the
powerup timer, the help icon and the held keys.
===============
*/
bool SCR_Q3HudDrawsStat(int stat)
{
    switch (stat) {
    case STAT_HEALTH_ICON:
    case STAT_AMMO_ICON:
    case STAT_ARMOR_ICON:
    case STAT_SELECTED_ICON:
    case STAT_PICKUP_ICON:
    case STAT_TIMER_ICON:
    case STAT_HELPICON:
    case STAT_KEY_A:
    case STAT_KEY_B:
    case STAT_KEY_C:
        return true;
    }
    return false;
}

/*
===============
SCR_Q3HudActive

Whether the player in view gets Quake III's HUD: a Quake III model, or
everyone under cl_q3_hud 2. The game has to be one whose stats are Quake II's
own - this one's, or single player - and not Quake 1's VM.
===============
*/
bool SCR_Q3HudActive(void)
{
    int num;

    if (!cl_q3_hud || !cl_q3_hud->integer || cl.q1vm[0] || cl.kex_protocol)
        return false;
    if (!CL_GameOwnsExtraStats())
        return false;
    if (cl_q3_hud->integer >= 2)
        return true;

    num = cl.frame.clientNum;
    if (num < 0 || num >= CLIENTNUM_NONE)
        num = cl.clientNum;
    if (num < 0 || num >= MAX_CLIENTS)
        return false;
    return cl.clientinfo[num].q3.q3 != NULL;
}

/*
===============
SCR_Q3DrawHud

In SCR_DrawStats, before the CS_STATUSBAR program. Returns Q3HUD_* bits for
what it drew, which the program then leaves out (screen.c); *bar_top is the
top of the status bar, in the HUD's units, for the weapon bar to sit above,
or 0 when it is not up.
===============
*/
int SCR_Q3DrawHud(int width, int height, float alpha, int *bar_top)
{
    const short *stats = cl.frame.ps.stats;
    const clientinfo_t *ci;
    bool        alive, playing, scoreboard;
    float       y;
    int         num, drew = Q3HUD_ON;

    *bar_top = 0;
    if (!SCR_Q3HudActive())
        return 0;
    if (!q3h.registered)
        q3_register_media();

    q3_w = width;
    q3_h = height;
    q3_s = min(width / 640.0f, height / 480.0f) * Cvar_ClampValue(cl_q3_hud_scale, 0.25f, 2.0f);
    q3_cx = (width - 640 * q3_s) * 0.5f;
    q3_alpha = alpha;

    num = cl.frame.clientNum;
    if (num < 0 || num >= CLIENTNUM_NONE)
        num = cl.clientNum;
    ci = (num >= 0 && num < MAX_CLIENTS) ? &cl.clientinfo[num] : &cl.baseclientinfo;

    q3_track();

    // cg_draw.c CG_Draw2D: the bar and what goes with it only for the living,
    // and not under the scoreboard (or the help computer, or the intermission)
    playing = !(stats[STAT_SPECTATOR] && !stats[STAT_CHASE]);
    alive = stats[STAT_HEALTH] > 0;
    scoreboard = stats[STAT_LAYOUTS] & 1;

    if (playing && alive && !scoreboard) {
        q3_status_bar(ci);
        q3_holdable();
        *bar_top = (int)q3_bottom(480 - Q3_ICON_SIZE * 1.25f);
    }

    // CG_DrawLowerRight: the scores, the powerups over them
    y = 480 - Q3_ICON_SIZE;
    y -= Q3_BIGCHAR + 8;
    if (q3_scores(y))
        drew |= Q3HUD_SCORES;
    y -= 8;
    if (playing)
        q3_powerup(y);

    // CG_DrawLowerLeft
    if (playing && alive) {
        q3_pickup();
        q3_help();
    }

    q3_follow();

    R_ClearColor();
    R_SetAlpha(alpha);
    return drew;
}

void SCR_Q3HudInit(void)
{
    cl_q3_hud = Cvar_Get("cl_q3_hud", "1", CVAR_ARCHIVE);
    cl_q3_hud_scale = Cvar_Get("cl_q3_hud_scale", "1", CVAR_ARCHIVE);
    // cg_draw3dIcons: the head, ammo and armor as models (R_DrawModel2D); 0 = flat icons
    cl_q3_hud_3d = Cvar_Get("cl_q3_hud_3d", "1", CVAR_ARCHIVE);
}
