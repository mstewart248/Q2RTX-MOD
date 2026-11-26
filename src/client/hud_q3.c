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

// Team Arena's HUD, below
static void ta_new_level(void);
static void ta_clear_media(void);

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
    ta_new_level();

    if (cl_q3_hud && cl_q3_hud->integer && CL_Q3PlayersEnabled())
        q3_register_media();
}

// a renderer restart freed them all
void SCR_Q3HudClearMedia(void)
{
    q3h.registered = false;
    q3h.head_for[0] = 0;
    q3h.head = 0;
    ta_clear_media();
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
        path[0] = '/';
        Q3P_IconPath(ci->model_name, ci->skin_name, path + 1, sizeof(path) - 1);
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
==============================================================================

TEAM ARENA'S HUD

On Team Arena's maps (the game sets CS_TA_HUD there, baseq2 g_ta.c) its own
HUD takes the place of Quake III's (Matt, 11-26: "it looks like we are not
using the team arena hud"): missionpack ui/hud.menu, as its cgame drew it
(cg_newdraw.c's ownerdraws, ui_shared.c's windows) -

  - the bar along the bottom, ui/assets/statusbar_full.tga over five panels:
    the flag and items carried, ammo, health and the head, armor, and the
    scores - team colour behind each (style 4), its scanlines over it
    (menuscreen2), a panel flashing (hudalert) when health is low, when you
    carry a flag, and over a team's score while the other has its flag
  - the powerup timers at the right
  - in a team game: the teammate picked out at the lower left (its
    "SelectedPlayer": head, name, where they are, health, armor, gun, job,
    powerup - nextteammember / prevteammember pick another), and where you
    are at the lower right ("location")
  - a voice chat's speaker, their head at the top left for 2.5 s
    (CG_ShowResponseHead), the console's notify lines moved right of it

Its text is its own fonts' (fonts/fontImage_<12|16|20>.dat and their glyph
sheets, CG_Text_Paint): a textscale up to 0.25 the 12 point, over 0.4 the 20.
What the game knows that Quake II's stats don't carry comes as CS_TA_HUD
(scores, limit, flags), CS_TA_LOCATIONS, and the "ta_tinfo" and "ta_vchat"
lines below. menuscreen2 was added light; the 2D renderers only blend, so it
is laid on thin, under the numbers rather than over them.

cl_q3_hud_ta 1 (default): on Team Arena's maps; 0: Quake III's HUD there
too; 2: on every map Quake III's HUD would draw.

==============================================================================
*/

static cvar_t   *cl_q3_hud_ta;

// Quake II's stats as baseq2 sends them (g_local.h): CTF's, and Team Arena's
// in the same slots
#define TA_STAT_SKULLS          18      // Harvester: the skulls carried
#define TA_STAT_TEAM1_PIC       24
#define TA_STAT_TEAM2_PIC       26
#define TA_STAT_FLAG_PIC        28      // CTF: the flag carried, blinking
#define TA_STAT_PERSISTENT      28      // ... outside CTF: the persistent powerup
#define TA_STAT_JOINED_TEAM1    29
#define TA_STAT_JOINED_TEAM2    30
#define TA_STAT_TECH            31      // CTF: the persistent powerup

// Quake III's game types (bg_public.h gametype_t)
#define TA_GT_FFA           0
#define TA_GT_TOURNAMENT    1
#define TA_GT_TEAM          3
#define TA_GT_CTF           4
#define TA_GT_1FCTF         5
#define TA_GT_OBELISK       6
#define TA_GT_HARVESTER     7

#define TA_FONT_SMALL       0       // fonts/fontImage_12.dat
#define TA_FONT_TEXT        1       // 16
#define TA_FONT_BIG         2       // 20
#define TA_STYLE_SHADOWED   3       // ITEM_TEXTSTYLE_SHADOWED

#define TA_VOICE_TIME       2500    // the speaker's head stays up
#define TA_VOICE_GAP        1000    // CG_PlayBufferedVoiceChats: one a second
#define TA_VOICE_QUEUE      8
#define TA_MAX_TINFO        16
#define TA_FLAG_HOLD        700     // STAT_TA_FLAG_PIC's blink, held over

typedef struct {
    float       s, t, s2, t2;
    float       top, xskip, w, h;
    qhandle_t   pic;
} ta_glyph_t;

typedef struct {
    bool        loaded;
    float       scale;              // glyphScale: 48 / the point size
    ta_glyph_t  glyph[256];
} ta_font_t;

typedef struct {
    int     client, location, health, armor, weapon, powerup, flag, persistent, task;
} ta_tinfo_t;

typedef struct {
    int     client;
    int     mode;                   // 0 everyone, 1 team, 2 told
    bool    voice_only;
    char    sound[MAX_QPATH];
    char    text[MAX_STRING_CHARS];
} ta_vchat_t;

static struct {
    bool        registered;
    bool        drawn;              // SCR_Q3DrawHud drew it this frame
    ta_font_t   font[3];

    qhandle_t   bar, screen, voice_window, selected_pic, location_pic, sel_armor;
    qhandle_t   chip[3];            // [TEAM_RED], [TEAM_BLUE]
    qhandle_t   skull[3];
    qhandle_t   flag_status[3];     // at its stand, carried, missing
    qhandle_t   flag_icon[4];       // 1 red, 2 blue, 3 white
    qhandle_t   flag_model[4];
    qhandle_t   orb_model[3];
    qhandle_t   task[8];            // teamTask's icons
    qhandle_t   armor;

    // ta_tinfo: your team and everyone on it
    int         team;               // 1 red, 2 blue
    int         num_tinfo;
    ta_tinfo_t  tinfo[TA_MAX_TINFO];
    int         tinfo_time;         // cls.realtime it came
    int         selected;           // the client SelectedPlayer shows, -1 the first other

    // the flag carried: its last picture and when
    int         flag;               // 1 red, 2 blue, 3 white
    int         flag_time;

    // voice chats
    ta_vchat_t  queue[TA_VOICE_QUEUE];
    int         queue_in, queue_out;
    int         voice_next;         // cls.realtime the next may start
    int         voice_client;       // whose head is up
    int         voice_time;         // cls.realtime it came up, 0 none

    // each player's icon, by model/skin
    char        icon_for[MAX_CLIENTS][MAX_QPATH * 2];
    qhandle_t   icon[MAX_CLIENTS];
} tah;

// a new level: what the last one told is gone, Team Arena's pictures
// registered again when first drawn
static void ta_new_level(void)
{
    tah.registered = false;
    tah.drawn = false;
    tah.num_tinfo = 0;
    tah.tinfo_time = 0;
    tah.selected = -1;
    tah.flag = 0;
    tah.flag_time = 0;
    tah.queue_in = tah.queue_out = 0;
    tah.voice_next = 0;
    tah.voice_time = 0;
    Con_SetNotifyIndent(0);
}

static void ta_clear_media(void)
{
    tah.registered = false;
    memset(tah.icon_for, 0, sizeof(tah.icon_for));
    memset(tah.icon, 0, sizeof(tah.icon));
}

// the panel being drawn: how its 640x480 places come to the HUD's
static float (*ta_ax)(float x);
static float (*ta_ay)(float y);

static void ta_anchor(float (*ax)(float), float (*ay)(float))
{
    ta_ax = ax;
    ta_ay = ay;
}

/*
===============
Fonts
===============
*/

static int32_t ta_int(const byte *p)
{
    int32_t v;

    memcpy(&v, p, 4);
    return (int32_t)LittleLong(v);
}

static float ta_float(const byte *p)
{
    float v;

    memcpy(&v, p, 4);
    return LittleFloat(v);
}

/*
RE_RegisterFont's prepared font: 256 glyphInfo_t of 80 bytes - height, top,
bottom, pitch, xSkip, imageWidth, imageHeight; s, t, s2, t2; the handle;
the glyph sheet's name, 32 chars - then glyphScale and the font's name.
*/
static void ta_load_font(ta_font_t *f, int points)
{
    char        last[33] = "";
    qhandle_t   pic = 0;
    byte        *data;
    int         len, i;

    memset(f, 0, sizeof(*f));
    len = FS_LoadFile(va("missionpack/fonts/fontImage_%d.dat", points), (void **)&data);
    if (!data)
        return;
    if (len < 256 * 80 + 4) {
        FS_FreeFile(data);
        return;
    }

    for (i = 0; i < 256; i++) {
        const byte  *g = data + i * 80;
        ta_glyph_t  *out = &f->glyph[i];
        char        name[33];

        out->top = (float)ta_int(g + 4);
        out->xskip = (float)ta_int(g + 16);
        out->w = (float)ta_int(g + 20);
        out->h = (float)ta_int(g + 24);
        out->s = ta_float(g + 28);
        out->t = ta_float(g + 32);
        out->s2 = ta_float(g + 36);
        out->t2 = ta_float(g + 40);

        memcpy(name, g + 48, 32);
        name[32] = 0;
        if (!name[0])
            continue;
        if (strcmp(name, last)) {
            Q_strlcpy(last, name, sizeof(last));
            pic = q3_register(va("/%s", name), IT_PIC);
        }
        out->pic = pic;
    }
    f->scale = ta_float(data + 256 * 80);
    if (f->scale <= 0)
        f->scale = 48.0f / points;
    f->loaded = true;
    FS_FreeFile(data);
}

// CG_Text_Paint's choice: cg_smallFont 0.25, cg_bigFont 0.4
static const ta_font_t *ta_font_for(float scale)
{
    if (scale <= 0.25f)
        return &tah.font[TA_FONT_SMALL];
    if (scale > 0.4f)
        return &tah.font[TA_FONT_BIG];
    return &tah.font[TA_FONT_TEXT];
}

static bool ta_color_code(const char *s)
{
    return s[0] == '^' && s[1] && s[1] != '^';
}

// CG_Text_Width, in 640x480 units
static float ta_text_width(const char *s, float scale)
{
    const ta_font_t *f = ta_font_for(scale);
    float           out = 0;

    for (; *s; s++) {
        if (ta_color_code(s)) {
            s++;
            continue;
        }
        out += f->glyph[*s & 255].xskip;
    }
    return (float)(int)(out * scale * f->scale);
}

// Quake III's colours for ^0-^7 (g_color_table)
static const float ta_color_table[8][3] = {
    { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 },
    { 0, 0, 1 }, { 0, 1, 1 }, { 1, 0, 1 }, { 1, 1, 1 },
};

// CG_Text_Paint: (x, y) the start of the baseline, in the panel's 640x480
static void ta_text(float x, float y, float scale, const float *color, const char *s, int style)
{
    const ta_font_t *f = ta_font_for(scale);
    const float     use = scale * f->scale;
    float           c[4];

    if (!f->loaded)
        return;
    Vector4Copy(color, c);

    for (; *s; s++) {
        const ta_glyph_t *g;

        if (ta_color_code(s)) {
            VectorCopy(ta_color_table[(s[1] - '0') & 7], c);
            s++;
            continue;
        }
        g = &f->glyph[*s & 255];
        if (g->pic && g->w > 0 && g->h > 0) {
            const float top = y - use * g->top;

            if (style == TA_STYLE_SHADOWED) {
                const float black[4] = { 0, 0, 0, c[3] };

                q3_color(black, 1);
                R_DrawStretchSubPic(ta_ax(x + 1), ta_ay(top + 1), g->w * use * q3_s, g->h * use * q3_s,
                                    g->s, g->t, g->s2, g->t2, g->pic);
            }
            q3_color(c, 1);
            R_DrawStretchSubPic(ta_ax(x), ta_ay(top), g->w * use * q3_s, g->h * use * q3_s,
                                g->s, g->t, g->s2, g->t2, g->pic);
        }
        x += g->xskip * use;
    }
}

/*
===============
Windows
===============
*/

static void ta_pic(float x, float y, float w, float h, qhandle_t pic, const float *color)
{
    if (!pic)
        return;
    q3_color(color ? color : q3_white, 1);
    q3_pic(ta_ax(x), ta_ay(y), w * q3_s, h * q3_s, pic);
}

static void ta_fill(float x, float y, float w, float h, float r, float g, float b, float a)
{
    float x0 = ta_ax(x), y0 = ta_ay(y), x1 = ta_ax(x + w), y1 = ta_ay(y + h);

    R_DrawFill32((int)x0, (int)y0, (int)(x1 + 0.5f) - (int)x0, (int)(y1 + 0.5f) - (int)y0,
                 MakeColor((int)(q3_clipf(r, 0, 1) * 255), (int)(q3_clipf(g, 0, 1) * 255),
                           (int)(q3_clipf(b, 0, 1) * 255), (int)(q3_clipf(a * q3_alpha, 0, 1) * 255)));
}

static int ta_gametype(void);

// your team: 1 red, 2 blue, 0 none
static int ta_my_team(void)
{
    const short *stats = cl.frame.ps.stats;

    if (ta_gametype() >= TA_GT_CTF) {
        if (stats[TA_STAT_JOINED_TEAM1])
            return 1;
        if (stats[TA_STAT_JOINED_TEAM2])
            return 2;
    }
    if (ta_gametype() >= TA_GT_TEAM && tah.tinfo_time && cls.realtime - tah.tinfo_time < 3000)
        return tah.team;
    return 0;
}

// style 4, WINDOW_STYLE_TEAMCOLOR: CG_GetTeamColor
static void ta_team_fill(float x, float y, float w, float h)
{
    switch (ta_my_team()) {
    case 1:     ta_fill(x, y, w, h, 1, 0, 0, 0.25f); break;
    case 2:     ta_fill(x, y, w, h, 0, 0, 1, 0.25f); break;
    default:    ta_fill(x, y, w, h, 0, 0.17f, 0, 0.25f); break;
    }
}

// hudalert, hudalert_good, hudalert_blue (ui_kc.shader): an 8x8 box of
// colour put in place of what is behind (GL_ONE GL_ZERO), rgbgen wave sin
// .25 .25 0 1
static void ta_alert(float x, float y, float w, float h, int which)
{
    static const float rgb[3][3] = { { 1, 0, 0 }, { 42 / 255.0f, 1, 0 }, { 6 / 255.0f, 0, 1 } };
    const float v = 0.25f + 0.25f * sinf(cl.time * 0.001f * 2 * (float)M_PI);

    ta_fill(x, y, w, h, rgb[which][0] * v, rgb[which][1] * v, rgb[which][2] * v, 1);
}

/*
menuscreen2 (ui_kc.shader): textures/sfx2/screen01, tcMod scroll 7 0.2 then
scale .4 .5, rgbgen wave square .75 .05 0 5, added. The window of the
texture it shows slides, so it is cut where it wraps.
*/
#define TA_SCREEN_ALPHA     0.22f

static void ta_screen(float x, float y, float w, float h)
{
    const float t = cl.time * 0.001f;
    const float s0 = 2.8f * t - floorf(2.8f * t);
    const float t0 = 0.1f * t - floorf(0.1f * t);
    const float v = (5 * t - floorf(5 * t)) < 0.5f ? 0.8f : 0.7f;
    const float c[4] = { v, v, v, TA_SCREEN_ALPHA };
    float       sx[2][2], ty[2][2], ws[2], ht[2];
    int         ns = 1, nt = 1, i, j;

    if (!tah.screen)
        return;

    // across: s0..s0+0.4 of the texture over the width, cut at 1
    sx[0][0] = s0;
    sx[0][1] = s0 + 0.4f;
    ws[0] = w;
    if (sx[0][1] > 1) {
        ws[0] = w * (1 - s0) / 0.4f;
        sx[0][1] = 1;
        sx[1][0] = 0;
        sx[1][1] = s0 + 0.4f - 1;
        ws[1] = w - ws[0];
        ns = 2;
    }
    // down: t0..t0+0.5
    ty[0][0] = t0;
    ty[0][1] = t0 + 0.5f;
    ht[0] = h;
    if (ty[0][1] > 1) {
        ht[0] = h * (1 - t0) / 0.5f;
        ty[0][1] = 1;
        ty[1][0] = 0;
        ty[1][1] = t0 + 0.5f - 1;
        ht[1] = h - ht[0];
        nt = 2;
    }

    q3_color(c, 1);
    for (j = 0; j < nt; j++) {
        float yy = y + (j ? ht[0] : 0);

        for (i = 0; i < ns; i++) {
            float xx = x + (i ? ws[0] : 0);

            if (ws[i] <= 0 || ht[j] <= 0)
                continue;
            R_DrawStretchSubPic(ta_ax(xx), ta_ay(yy), ws[i] * q3_s, ht[j] * q3_s,
                                sx[i][0], ty[j][0], sx[i][1], ty[j][1], tah.screen);
        }
    }
}

/*
===============
Pictures
===============
*/

static void ta_register_media(void)
{
    static const char *const tasks[8] = {
        "assault", "assault", "defend", "patrol", "follow", "retrieve", "escort", "camp"
    };
    int i;

    memset(tah.icon_for, 0, sizeof(tah.icon_for));
    memset(tah.icon, 0, sizeof(tah.icon));

    ta_load_font(&tah.font[TA_FONT_SMALL], 12);
    ta_load_font(&tah.font[TA_FONT_TEXT], 16);
    ta_load_font(&tah.font[TA_FONT_BIG], 20);

    tah.bar = q3_register("/ui/assets/statusbar_full.tga", IT_PIC);
    tah.screen = q3_register("/textures/sfx2/screen01.tga", IT_PIC);
    tah.voice_window = q3_register("/ui/assets/voice_window.tga", IT_PIC);
    tah.selected_pic = q3_register("/ui/assets/selectedstatus.tga", IT_PIC);
    tah.location_pic = q3_register("/ui/assets/locationstatus.tga", IT_PIC);
    tah.sel_armor = q3_register("/ui/assets/statusbar/selectedarmor.tga", IT_PIC);
    tah.chip[1] = q3_register("/ui/assets/redchip.tga", IT_PIC);
    tah.chip[2] = q3_register("/ui/assets/bluechip.tga", IT_PIC);
    tah.skull[1] = q3_register("/icons/skull_red.tga", IT_PIC);
    tah.skull[2] = q3_register("/icons/skull_blue.tga", IT_PIC);
    tah.flag_status[0] = q3_register("/ui/assets/statusbar/flag_in_base.tga", IT_PIC);
    tah.flag_status[1] = q3_register("/ui/assets/statusbar/flag_capture.tga", IT_PIC);
    tah.flag_status[2] = q3_register("/ui/assets/statusbar/flag_missing.tga", IT_PIC);
    tah.flag_icon[1] = q3_register("/icons/iconf_red1.tga", IT_PIC);
    tah.flag_icon[2] = q3_register("/icons/iconf_blu1.tga", IT_PIC);
    tah.flag_icon[3] = q3_register("/icons/iconf_neutral1.tga", IT_PIC);
    tah.armor = q3_register("/icons/iconr_yellow.tga", IT_PIC);
    for (i = 0; i < 8; i++)
        tah.task[i] = q3_register(va("/ui/assets/statusbar/%s.tga", tasks[i]), IT_PIC);

    // the 3D icons, for a renderer that draws them (cg_draw3dIcons)
    tah.flag_model[1] = R_DrawModel2D ? R_RegisterModel("models/flags/r_flag.md3") : 0;
    tah.flag_model[2] = R_DrawModel2D ? R_RegisterModel("models/flags/b_flag.md3") : 0;
    tah.flag_model[3] = R_DrawModel2D ? R_RegisterModel("models/flags/n_flag.md3") : 0;
    tah.orb_model[1] = R_DrawModel2D ? R_RegisterModel("models/powerups/orb/r_orb.md3") : 0;
    tah.orb_model[2] = R_DrawModel2D ? R_RegisterModel("models/powerups/orb/b_orb.md3") : 0;

    tah.registered = true;
}

// a player's icon (their model's icon_<skin>), for the selected player and
// the voice chat's speaker when the head can't be drawn
static qhandle_t ta_client_icon(int client)
{
    const clientinfo_t  *ci = &cl.clientinfo[client];
    char                key[MAX_QPATH * 2], path[MAX_QPATH];

    Q_concat(key, sizeof(key), ci->model_name, "/", ci->skin_name);
    if (strcmp(key, tah.icon_for[client])) {
        Q_strlcpy(tah.icon_for[client], key, sizeof(tah.icon_for[client]));
        tah.icon[client] = 0;
        if (ci->q3.q3) {
            path[0] = '/';
            Q3P_IconPath(ci->model_name, ci->skin_name, path + 1, sizeof(path) - 1);
            tah.icon[client] = q3_register(path, IT_SPRITE);
        }
    }
    if (tah.icon[client])
        return tah.icon[client];
    return ci->icon ? ci->icon : cl.baseclientinfo.icon;
}

// CG_DrawSelectedPlayerHead: the head facing out, still; else the icon
static void ta_still_head(int client, float x, float y, float w, float h)
{
    const clientinfo_t  *ci;
    vec3_t              mins, maxs, origin, offset, angles;

    if (client < 0 || client >= MAX_CLIENTS)
        return;
    ci = &cl.clientinfo[client];
    q3_color(q3_white, 1);
    if (ci->q3.q3 && ci->q3.head && R_ModelBounds && R_ModelBounds(ci->q3.head, mins, maxs)) {
        origin[2] = -0.5f * (mins[2] + maxs[2]);
        origin[1] = 0.5f * (mins[1] + maxs[1]);
        origin[0] = 0.7f * (maxs[2] - mins[2]) / 0.268f;
        Q3P_HeadOffset(ci->q3.q3, offset);
        VectorAdd(origin, offset, origin);
        VectorSet(angles, 0, 180, 0);
        if (q3_model_icon(ta_ax(x), ta_ay(y), w * q3_s, h * q3_s, ci->q3.head, ci->q3.skin, origin, angles))
            return;
    }
    q3_pic(ta_ax(x), ta_ay(y), w * q3_s, h * q3_s, ta_client_icon(client));
}

// CG_DrawFlagModel: the flag turning to and fro, else its icon
static void ta_flag(float x, float y, float w, float h, int flag)
{
    vec3_t mins, maxs, origin, angles;

    if (flag < 1 || flag > 3)
        return;
    q3_color(q3_white, 1);
    if (tah.flag_model[flag] && R_ModelBounds && R_ModelBounds(tah.flag_model[flag], mins, maxs)) {
        origin[2] = -0.5f * (mins[2] + maxs[2]);
        origin[1] = 0.5f * (mins[1] + maxs[1]);
        origin[0] = 0.5f * (maxs[2] - mins[2]) / 0.268f;
        VectorSet(angles, 0, 60 * sinf(cl.time / 2000.0f), 0);
        if (q3_model_icon(ta_ax(x + 2), ta_ay(y + 2), (w - 2) * q3_s, (h - 2) * q3_s,
                          tah.flag_model[flag], 0, origin, angles))
            return;
    }
    ta_pic(x, y, w, h, tah.flag_icon[flag], NULL);
}

/*
===============
What the game sent
===============
*/

// CS_TA_HUD's numbers
static struct {
    int gametype, limit, red, blue, redflag, blueflag, flagstatus;
} tas;

static void ta_parse_state(void)
{
    const char *s = cl.configstrings[CS_TA_HUD];

    memset(&tas, 0, sizeof(tas));
    if (!s[0]) {
        // cl_q3_hud_ta 2 off Team Arena's maps: CTF by its stats, or a free-for-all
        if (cl.frame.ps.stats[TA_STAT_JOINED_TEAM1] || cl.frame.ps.stats[TA_STAT_JOINED_TEAM2]) {
            if ((cl.frame.ps.stats[STAT_Q3_SCORES] & ~STAT_Q3_SCORES_OTHER) != STAT_Q3_SCORES_TAG) {
                tas.gametype = TA_GT_CTF;
                tas.red = cl.frame.ps.stats[TA_STAT_TEAM1_PIC + 1];
                tas.blue = cl.frame.ps.stats[TA_STAT_TEAM2_PIC + 1];
            }
        }
        return;
    }
    sscanf(s, "%d %d %d %d %d %d %d", &tas.gametype, &tas.limit, &tas.red, &tas.blue,
           &tas.redflag, &tas.blueflag, &tas.flagstatus);
}

static int ta_gametype(void)
{
    return tas.gametype;
}

static int ta_my_client(void)
{
    int num = cl.frame.clientNum;

    if (num < 0 || num >= CLIENTNUM_NONE)
        num = cl.clientNum;
    return num;
}

static const ta_tinfo_t *ta_tinfo_for(int client)
{
    int i;

    for (i = 0; i < tah.num_tinfo; i++)
        if (tah.tinfo[i].client == client)
            return &tah.tinfo[i];
    return NULL;
}

// the teammate picked out: the one picked, else the first who isn't you
static const ta_tinfo_t *ta_selected(void)
{
    const ta_tinfo_t    *t = ta_tinfo_for(tah.selected);
    int                 i, me = ta_my_client();

    if (t)
        return t;
    for (i = 0; i < tah.num_tinfo; i++)
        if (tah.tinfo[i].client != me)
            return &tah.tinfo[i];
    return tah.num_tinfo ? &tah.tinfo[0] : NULL;
}

// the location a ta_tinfo gives (its index + 1), "unknown" for none
static const char *ta_location_name(int location)
{
    const char *s;

    if (location < 1 || location > MAX_TA_LOCATIONS)
        return "unknown";
    s = cl.configstrings[CS_TA_LOCATIONS + location - 1];
    return s[0] ? s : "unknown";
}

// "ta_tinfo <team> <n> [<client> <location> <health> <armor> <gun icon>
// <powerup icon> <flag> <persistent icon> <job>]..." (g_ta.c TA_SendTeamInfo)
#define TA_TINFO_FIELDS     9

static void ta_tinfo_f(void)
{
    int i, n;

    tah.team = atoi(Cmd_Argv(1));
    n = atoi(Cmd_Argv(2));
    n = q3_clip(n, 0, TA_MAX_TINFO);
    if (Cmd_Argc() < 3 + n * TA_TINFO_FIELDS)
        n = (Cmd_Argc() - 3) / TA_TINFO_FIELDS;
    for (i = 0; i < n; i++) {
        ta_tinfo_t  *t = &tah.tinfo[i];
        int         a = 3 + i * TA_TINFO_FIELDS;

        t->client = q3_clip(atoi(Cmd_Argv(a)), 0, MAX_CLIENTS - 1);
        t->location = atoi(Cmd_Argv(a + 1));
        t->health = atoi(Cmd_Argv(a + 2));
        t->armor = atoi(Cmd_Argv(a + 3));
        t->weapon = atoi(Cmd_Argv(a + 4));
        t->powerup = atoi(Cmd_Argv(a + 5));
        t->flag = atoi(Cmd_Argv(a + 6));
        t->persistent = atoi(Cmd_Argv(a + 7));
        t->task = atoi(Cmd_Argv(a + 8));
    }
    tah.num_tinfo = n;
    tah.tinfo_time = cls.realtime;
}

// cg_consolecmds.c CG_NextTeamMember_f / CG_PrevTeamMember_f
static void ta_select_step(int step)
{
    const ta_tinfo_t    *t = ta_selected();
    int                 i;

    if (!t || !tah.num_tinfo)
        return;
    i = (int)(t - tah.tinfo);
    i = (i + step + tah.num_tinfo) % tah.num_tinfo;
    tah.selected = tah.tinfo[i].client;
}

static void ta_next_member_f(void) { ta_select_step(1); }
static void ta_prev_member_f(void) { ta_select_step(-1); }

static qhandle_t ta_image(int index)
{
    if (index <= 0 || index >= MAX_IMAGES || !cl.configstrings[CS_IMAGES + index][0])
        return 0;
    return cl.image_precache[index];
}

/*
===============
Voice chats

"ta_vchat <mode> <voice only> <client> <id>" (g_ta.c TA_Voice): the line
<id> of the speaker's voice file - its character's (missionpack
scripts/<model>.vc names one), else the first of its sex - a random one of
its sounds and what it says (cg_servercmds.c CG_VoiceChatListForClient,
CG_GetVoiceChat). Queued, one a second.
===============
*/

#define TA_VOICE_FILES  8
#define TA_VOICE_TEXT   256     // what a line says

static struct {
    char    name[MAX_QPATH];
    char    *text;
} ta_voice_files[TA_VOICE_FILES];

static const char *ta_voice_file_text(const char *name)
{
    char    *buf;
    int     i;

    for (i = 0; i < TA_VOICE_FILES && ta_voice_files[i].name[0]; i++)
        if (!Q_stricmp(ta_voice_files[i].name, name))
            return ta_voice_files[i].text;
    if (i == TA_VOICE_FILES)
        return NULL;

    FS_LoadFile(va("missionpack/%s", name), (void **)&buf);
    if (!buf)
        return NULL;
    Q_strlcpy(ta_voice_files[i].name, name, sizeof(ta_voice_files[i].name));
    ta_voice_files[i].text = Z_CopyString(buf);
    FS_FreeFile(buf);
    return ta_voice_files[i].text;
}

// the voice file named in a .vc, or false
static bool ta_voice_vc(const char *path, char *out, size_t size)
{
    char        *buf;
    const char  *p, *tok;

    FS_LoadFile(path, (void **)&buf);
    if (!buf)
        return false;
    p = buf;
    tok = COM_Parse(&p);
    Q_strlcpy(out, tok, size);
    FS_FreeFile(buf);
    return out[0] != 0;
}

static bool ta_voice_line(int client, const char *id, char *sound, size_t ssize, char *chat, size_t csize)
{
    const clientinfo_t  *ci = &cl.clientinfo[client];
    char                file[MAX_QPATH];
    static struct { char sound[MAX_QPATH], text[TA_VOICE_TEXT]; } lines[64];
    const char          *text, *p, *tok;
    int                 n = 0, pick;
    char                sex = 'm';

    file[0] = 0;
    if (ci->model_name[0] &&
        !ta_voice_vc(va("missionpack/scripts/%s/%s.vc", ci->model_name, ci->skin_name), file, sizeof(file)))
        ta_voice_vc(va("missionpack/scripts/%s.vc", ci->model_name), file, sizeof(file));
    if (!file[0]) {
        if (ci->q3.q3)
            sex = Q3P_Sex(ci->q3.q3);
        else if (Q_stristr(ci->model_name, "female"))
            sex = 'f';
        Q_strlcpy(file, sex == 'f' ? "scripts/female1.voice" : "scripts/male1.voice", sizeof(file));
    }

    text = ta_voice_file_text(file);
    if (!text)
        return false;

    // "<sex>", then "<id> { <sound> "<what it says>" ... }"...
    p = text;
    COM_Parse(&p);
    while (p) {
        bool match;

        tok = COM_Parse(&p);
        if (!tok[0])
            break;
        match = !Q_stricmp(tok, id);
        tok = COM_Parse(&p);
        if (strcmp(tok, "{"))
            break;
        while (p) {
            char snd[MAX_QPATH];

            tok = COM_Parse(&p);
            if (!tok[0] || !strcmp(tok, "}"))
                break;
            Q_strlcpy(snd, tok, sizeof(snd));
            tok = COM_Parse(&p);
            if (match && n < 64) {
                Q_strlcpy(lines[n].sound, snd, sizeof(lines[n].sound));
                Q_strlcpy(lines[n].text, tok, sizeof(lines[n].text));
                n++;
            }
        }
        if (match)
            break;
    }
    if (!n)
        return false;

    pick = (int)(frand() * n);
    pick = q3_clip(pick, 0, n - 1);
    Q_snprintf(sound, ssize, "#missionpack/%s", lines[pick].sound);
    Q_strlcpy(chat, lines[pick].text, csize);
    return true;
}

static void ta_vchat_f(void)
{
    ta_vchat_t  v;
    char        chat[TA_VOICE_TEXT];
    const char  *name;

    v.mode = atoi(Cmd_Argv(1));
    v.voice_only = atoi(Cmd_Argv(2)) != 0;
    v.client = atoi(Cmd_Argv(3));
    if (v.client < 0 || v.client >= MAX_CLIENTS || Cmd_Argc() < 5)
        return;
    if (!ta_voice_line(v.client, Cmd_Argv(4), v.sound, sizeof(v.sound), chat, sizeof(chat)))
        return;

    // G_VoiceTo / CG_VoiceChatLocal: "[name]: " told, "(name): " to the team
    name = cl.clientinfo[v.client].name;
    if (v.mode == 2)
        Q_snprintf(v.text, sizeof(v.text), "[%s]: %s", name, chat);
    else if (v.mode == 1)
        Q_snprintf(v.text, sizeof(v.text), "(%s): %s", name, chat);
    else
        Q_snprintf(v.text, sizeof(v.text), "%s: %s", name, chat);

    tah.queue[tah.queue_in % TA_VOICE_QUEUE] = v;
    tah.queue_in++;
    if (tah.queue_in - tah.queue_out > TA_VOICE_QUEUE)
        tah.queue_out = tah.queue_in - TA_VOICE_QUEUE;
}

// CG_PlayBufferedVoiceChats / CG_PlayVoiceChat, each frame
static void ta_run_voices(void)
{
    if (tah.voice_time && cls.realtime - tah.voice_time > TA_VOICE_TIME) {
        tah.voice_time = 0;
        Con_SetNotifyIndent(0);
    }
    if (tah.queue_out == tah.queue_in || cls.realtime < tah.voice_next)
        return;

    {
        const ta_vchat_t    *v = &tah.queue[tah.queue_out % TA_VOICE_QUEUE];
        static const color_index_t colors[3] = { COLOR_GREEN, COLOR_CYAN, COLOR_MAGENTA };

        tah.queue_out++;
        tah.voice_next = cls.realtime + TA_VOICE_GAP;

        S_StartSound(NULL, listener_entnum, CHAN_VOICE, S_RegisterSound(v->sound), 1, ATTN_NONE, 0);
        if (v->client != ta_my_client()) {
            tah.voice_client = v->client;
            tah.voice_time = cls.realtime;
            // cl_conXOffset 72: the notify lines start right of the head -
            // when Team Arena's HUD, which draws it, was up last frame
            if (tah.drawn && q3_w > 0)
                Con_SetNotifyIndent(q3_left(72) / q3_w);
        }
        if (!v->voice_only) {
            Com_SetColor(colors[q3_clip(v->mode, 0, 2)]);
            Com_Printf("%s\n", v->text);
            Com_SetColor(COLOR_NONE);
            SCR_AddToChatHUD(v->text);
        }
    }
}

/*
===============
The panels, as hud.menu lays them out
===============
*/

// "powerup area", 570 220: CG_DrawAreaPowerUp - Quake II's timer, the soonest
static void ta_powerups(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    const short *stats = cl.frame.ps.stats;
    const char  *name;
    qhandle_t   pic;
    char        num[16];
    int         ms;
    float       f, c[4];

    if (!stats[STAT_TIMER_ICON] || stats[STAT_TIMER] <= 0)
        return;
    name = q3_stat_pic_name(STAT_TIMER_ICON);
    pic = !Q_stricmp(name, "p_quad") && q3h.quad ? q3h.quad : q3_stat_pic(STAT_TIMER_ICON);

    ta_anchor(q3_right, q3_vmid);
    ms = stats[STAT_TIMER] * 1000 - (cl.time - q3h.timer_tick);
    if (ms < 0)
        ms = 0;
    if (ms >= Q3_POWERUP_BLINKS * 1000) {
        ta_pic(570, 220, 40 * 0.75f, 40, pic, NULL);
    } else {
        f = ms / 1000.0f;
        f -= (int)f;
        c[0] = c[1] = c[2] = c[3] = f;
        ta_pic(570, 220, 40 * 0.75f, 40, pic, c);
    }
    Q_snprintf(num, sizeof(num), "%i", stats[STAT_TIMER]);
    ta_text(570 + 40 * 0.75f + 3, 220 + 40, 0.55f, white, num, 0);
}

// "playerStatus", 0 416: the items and the flag carried, Harvester's skulls
static void ta_player_status(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    const short *stats = cl.frame.ps.stats;
    const int   gt = ta_gametype();
    int         flag = 0;

    // the flag you carry: STAT_CTF_FLAG_PIC blinks, so it is held over the gaps
    if (gt >= TA_GT_CTF && gt != TA_GT_HARVESTER && stats[TA_STAT_FLAG_PIC]) {
        const char *name = q3_stat_pic_name(TA_STAT_FLAG_PIC);

        if (!Q_stricmp(name, "i_ctf1"))
            flag = 1;
        else if (!Q_stricmp(name, "i_ctf2"))
            flag = 2;
        else if (Q_stristr(name, "iconf_neutral"))
            flag = 3;
        if (flag) {
            tah.flag = flag;
            tah.flag_time = cls.realtime;
        }
    }
    if (!flag && tah.flag && cls.realtime - tah.flag_time < TA_FLAG_HOLD && gt >= TA_GT_CTF)
        flag = tah.flag;

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(12, 429, 116, 48);
    if (flag)
        ta_alert(12, 429, 116, 48, 1);
    ta_screen(12, 429, 116, 48);

    // CG_PLAYER_ITEM: Quake II's selected item stands in for the holdable
    ta_pic(27, 441, 22, 22, q3_stat_pic(STAT_SELECTED_ICON), NULL);
    // CG_CTF_POWERUP: the persistent powerup
    ta_pic(54, 441, 22, 22, q3_stat_pic(gt >= TA_GT_CTF ? TA_STAT_TECH : TA_STAT_PERSISTENT), NULL);
    // CG_PLAYER_HASFLAG
    if (flag)
        ta_flag(85, 431, 40, 40, flag);

    // CG_HARVESTER_SKULLS: the count, and the other colour's skull
    if (gt == TA_GT_HARVESTER) {
        char    num[16];
        int     other = ta_my_team() == 2 ? 1 : 2;
        vec3_t  origin, angles;

        Q_snprintf(num, sizeof(num), "%i", q3_clip(stats[TA_STAT_SKULLS], 0, 99));
        ta_text(72 + 48 - ta_text_width(num, 0.333f), 426 + 34, 0.333f, white, num, TA_STYLE_SHADOWED);
        VectorSet(origin, 90, 0, -10);
        VectorSet(angles, 0, (cl.time & 2047) * 360 / 2048.0f, 0);
        q3_color(q3_white, 1);
        if (!q3_model_icon(ta_ax(72), ta_ay(426), 35 * q3_s, 35 * q3_s, tah.orb_model[other], 0, origin, angles))
            ta_pic(72, 426, 35, 35, tah.skull[other], NULL);
    }
}

// a number centred in (x, w), on the baseline y + h, by addColorRange
static void ta_value(float x, float y, float w, float h, int value, const float *color)
{
    char num[16];

    Q_snprintf(num, sizeof(num), "%i", value);
    ta_text(x + (w - ta_text_width(num, 0.75f)) / 2, y + h, 0.75f, color, num, TA_STYLE_SHADOWED);
}

// "Ammo", 128 416
static void ta_ammo(void)
{
    static const float red[4] = { 1, 0, 0, 1 }, amber[4] = { 1, 0.75f, 0, 1 };
    const short *stats = cl.frame.ps.stats;

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(131, 429, 122, 48);
    ta_screen(131, 429, 122, 48);
    if (stats[STAT_AMMO_ICON] && stats[STAT_AMMO] >= 0) {
        int         gun = q3_gun_ammo_index();
        qhandle_t   pic = gun >= 0 ? q3h.ammo[gun] : 0;

        ta_pic(213, 441, 30, 30, pic ? pic : q3_stat_pic(STAT_AMMO_ICON), NULL);
        ta_value(138, 459, 63, 12, stats[STAT_AMMO], stats[STAT_AMMO] <= 5 ? red : amber);
    }
}

// "Player", 256 416: health and the head
static void ta_health(const clientinfo_t *ci)
{
    static const float red[4] = { 1, 0, 0, 1 }, amber[4] = { 1, 0.75f, 0, 1 }, white[4] = { 1, 1, 1, 1 };
    const int   health = cl.frame.ps.stats[STAT_HEALTH];
    float       x = 326;

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(256, 418, 128, 60);
    if (health < 25)
        ta_alert(256, 418, 128, 60, 0);
    ta_screen(256, 418, 128, 60);
    ta_value(268, 458, 63, 12, health, health <= 25 ? red : health <= 100 ? amber : white);

    // CG_DrawPlayerHead: kicked from a hit (its swell sized, but the box drawn
    // at the item's own size)
    if (q3h.damage_time && cl.time - q3h.damage_time < Q3_DAMAGE_TIME) {
        float frac = (float)(cl.time - q3h.damage_time) / Q3_DAMAGE_TIME;
        float size = 45 * 1.25f * (1.5f - frac * 0.5f);
        float stretch = size - 45 * 1.25f;

        x -= stretch * 0.5f + q3h.damage_x * stretch * 0.5f;
    }
    q3_color(q3_white, 1);
    if (!q3_head_model(ci, ta_ax(x), ta_ay(426), 45 * q3_s))
        ta_pic(x, 426, 45, 45, q3_head(ci), NULL);
}

// "Armor", 384 416
static void ta_armor(void)
{
    static const float red[4] = { 0.67f, 0, 0, 1 }, amber[4] = { 1, 0.75f, 0, 1 }, grey[4] = { 0.75f, 0.75f, 0.75f, 1 };
    const short *stats = cl.frame.ps.stats;
    const int   armor = stats[STAT_ARMOR];
    qhandle_t   pic = tah.armor;
    const char  *name = q3_stat_pic_name(STAT_ARMOR_ICON);

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(384, 429, 125, 48);
    ta_screen(384, 429, 125, 48);
    // Quake II's power armor keeps its own icon
    if (stats[STAT_ARMOR_ICON] && (!Q_stricmp(name, "i_powershield") || !Q_stricmp(name, "i_powerscreen")))
        pic = q3_stat_pic(STAT_ARMOR_ICON);
    ta_pic(394, 423, 30, 30, pic, NULL);
    ta_value(439, 459, 63, 12, armor, armor <= 25 ? red : armor <= 100 ? amber : grey);
}

// "PlayerScore", 512 416, in a team game: the scores and the flags
static void ta_team_scores(void)
{
    static const float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 }, blue[4] = { 0.25f, 0.25f, 1, 1 };
    static const float flag_red[4] = { 1, 0, 0, 1 }, flag_blue[4] = { 0, 0, 1, 1 };
    const int   gt = ta_gametype();
    char        num[16];

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(512, 429, 126, 48);
    // CG_SHOW_BLUE_TEAM_HAS_REDFLAG / _RED_TEAM_HAS_BLUEFLAG
    if (tas.redflag == 1 || tas.flagstatus == 2)
        ta_alert(512, 429, 63, 48, 2);
    if (tas.blueflag == 1 || tas.flagstatus == 3)
        ta_alert(575, 429, 63, 48, 0);
    ta_screen(512, 429, 126, 48);

    ta_pic(542, 438, 16, 16, tah.chip[2], NULL);
    ta_pic(589, 438, 16, 16, tah.chip[1], NULL);
    if (gt == TA_GT_HARVESTER) {
        ta_pic(542, 438, 16, 16, tah.skull[2], NULL);
        ta_pic(589, 438, 16, 16, tah.skull[1], NULL);
    }
    // CG_BLUE_FLAGSTATUS / CG_RED_FLAGSTATUS, CTF; CG_ONEFLAG_STATUS
    if (gt == TA_GT_CTF) {
        ta_pic(542, 438, 16, 16, tah.flag_status[q3_clip(tas.blueflag, 0, 2)], flag_blue);
        ta_pic(589, 438, 16, 16, tah.flag_status[q3_clip(tas.redflag, 0, 2)], flag_red);
    } else if (gt == TA_GT_1FCTF) {
        float   c[4] = { 1, 1, 1, 1 };
        int     index = 0;

        if (tas.flagstatus == 2) {
            c[1] = c[2] = 0;
            index = 1;
        } else if (tas.flagstatus == 3) {
            c[0] = c[1] = 0;
            index = 1;
        } else if (tas.flagstatus == 4) {
            index = 2;
        }
        ta_pic(542, 456, 16, 16, tah.flag_status[index], c);
    }

    // CG_BLUE_SCORE, CG_RED_SCORE: right-aligned
    Q_snprintf(num, sizeof(num), "%i", tas.blue);
    ta_text(547 + 30 - ta_text_width(num, 0.25f), 437 + 15, 0.25f, blue, num, TA_STYLE_SHADOWED);
    Q_snprintf(num, sizeof(num), "%i", tas.red);
    ta_text(599 + 30 - ta_text_width(num, 0.25f), 437 + 15, 0.25f, red, num, TA_STYLE_SHADOWED);

    // CG_CAPFRAGLIMIT, CG_PLAYER_SCORE
    Q_snprintf(num, sizeof(num), "%2i", tas.limit);
    ta_text(520, 452, 0.25f, white, num, TA_STYLE_SHADOWED);
    Q_snprintf(num, sizeof(num), "%i", cl.frame.ps.stats[STAT_FRAGS]);
    ta_text(572 + (65 - ta_text_width(num, 0.25f)) / 2, 459 + 12, 0.25f, white, num, TA_STYLE_SHADOWED);
}

// "PlayerScore" in a free-for-all: the limit, first and second, your score
static void ta_ffa_scores(void)
{
    static const float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 }, blue[4] = { 0.25f, 0.25f, 1, 1 };
    const short *stats = cl.frame.ps.stats;
    const int   score = stats[STAT_FRAGS];
    const bool  tagged = (stats[STAT_Q3_SCORES] & ~STAT_Q3_SCORES_OTHER) == STAT_Q3_SCORES_TAG;
    const bool  any = tagged && (stats[STAT_Q3_SCORES] & STAT_Q3_SCORES_OTHER);
    const int   other = stats[STAT_Q3_OTHER_SCORE];
    int         limit = tas.limit;
    int         s1 = score, s2 = 0;
    float       x;
    char        num[16];

    if (!cl.configstrings[CS_TA_HUD][0] && tagged)
        limit = stats[STAT_Q3_FRAGLIMIT];
    if (any) {
        s1 = max(score, other);
        s2 = min(score, other);
    }

    ta_anchor(q3_mid, q3_bottom);
    ta_team_fill(512, 429, 126, 48);
    ta_screen(512, 429, 126, 48);

    // an ownerdraw after its text: 8 on from it (Item_OwnerDraw_Paint)
    ta_text(520, 448, 0.25f, white, "FragLimit:", TA_STYLE_SHADOWED);
    x = 520 + ta_text_width("FragLimit:", 0.25f) + 8;
    Q_snprintf(num, sizeof(num), "%2i", limit);
    ta_text(x, 448, 0.25f, white, num, TA_STYLE_SHADOWED);

    ta_text(522, 461, 0.25f, blue, "1st:", TA_STYLE_SHADOWED);
    Q_snprintf(num, sizeof(num), "%2i", s1);
    ta_text(522 + ta_text_width("1st:", 0.25f) + 8, 461, 0.25f, blue, num, TA_STYLE_SHADOWED);

    ta_text(582, 461, 0.25f, red, "2nd:", TA_STYLE_SHADOWED);
    if (any) {
        Q_snprintf(num, sizeof(num), "%2i", s2);
        ta_text(582 + ta_text_width("2nd:", 0.25f) + 8, 461, 0.25f, red, num, TA_STYLE_SHADOWED);
    }

    ta_text(522, 474, 0.25f, white, "Score:", TA_STYLE_SHADOWED);
    x = 522 + ta_text_width("Score:", 0.25f) + 8;
    Q_snprintf(num, sizeof(num), "%i", score);
    ta_text(x + (40 - ta_text_width(num, 0.25f)) / 2, 474, 0.25f, white, num, TA_STYLE_SHADOWED);
}

// "SelectedPlayer", 2 363: the teammate picked out
static void ta_selected_player(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float range[3][4] = { { 1, 0, 0, 1 }, { 1, 0.75f, 0, 1 }, { 1, 1, 1, 1 } };
    const ta_tinfo_t    *t = ta_selected();
    const float         *c;
    char                num[16];
    qhandle_t           pic;

    if (!t || !tah.tinfo_time || cls.realtime - tah.tinfo_time > 3000)
        return;

    ta_anchor(q3_left, q3_bottom);
    ta_team_fill(4, 367, 170, 57);
    ta_screen(4, 367, 170, 57);

    ta_still_head(t->client, 6, 366, 32, 32);

    // health, then armor with its shirt
    c = range[t->health <= 25 ? 0 : t->health <= 100 ? 1 : 2];
    Q_snprintf(num, sizeof(num), "%i", t->health);
    ta_text(77 + (22 - ta_text_width(num, 0.25f)) / 2, 398 + 11, 0.25f, c, num, 0);
    if (t->armor > 0) {
        c = range[t->armor <= 25 ? 0 : t->armor <= 100 ? 1 : 2];
        ta_pic(52, 411, 22, 11, tah.sel_armor, NULL);
        ta_pic(52, 411, 22, 11, tah.sel_armor, c);
        Q_snprintf(num, sizeof(num), "%i", t->armor);
        ta_text(77 + (22 - ta_text_width(num, 0.25f)) / 2, 411 + 11, 0.25f, c, num, TA_STYLE_SHADOWED);
    }

    ta_text(37, 366 + 15, 0.25f, white, cl.clientinfo[t->client].name, TA_STYLE_SHADOWED);
    ta_text(37, 378 + 15, 0.25f, white, ta_location_name(t->location), TA_STYLE_SHADOWED);

    // their job, their powerup (a timed one, else the flag, else the
    // persistent one - bg_public.h's powerup order), their gun
    ta_pic(9, 403, 16, 16, tah.task[q3_clip(t->task, 0, 7)], NULL);
    pic = ta_image(t->powerup);
    if (!pic && t->flag >= 1 && t->flag <= 3)
        pic = tah.flag_icon[t->flag];
    if (!pic)
        pic = ta_image(t->persistent);
    ta_pic(32, 403, 16, 16, pic, NULL);
    ta_pic(102, 396, 24, 24, ta_image(t->weapon), NULL);

    ta_pic(2, 363, 256, 64, tah.selected_pic, NULL);
}

// "location", 463 395: your job and where you are
static void ta_location(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    const ta_tinfo_t *t = ta_tinfo_for(ta_my_client());

    if (!t || !tah.tinfo_time || cls.realtime - tah.tinfo_time > 3000)
        return;

    ta_anchor(q3_right, q3_bottom);
    ta_team_fill(465, 401, 170, 26);
    ta_pic(479, 405, 16, 16, tah.task[q3_clip(t->task, 0, 7)], NULL);
    ta_text(500, 402 + 16, 0.25f, white, ta_location_name(t->location), TA_STYLE_SHADOWED);
    ta_pic(463, 395, 256, 32, tah.location_pic, NULL);
}

// "voiceMenu", 5 5: whoever spoke last, for TA_VOICE_TIME
static void ta_voice_head(void)
{
    static const float white[4] = { 1, 1, 1, 1 };

    if (!tah.voice_time || cls.realtime - tah.voice_time > TA_VOICE_TIME)
        return;

    ta_anchor(q3_left, q3_top);
    ta_team_fill(7, 7, 50, 50);
    ta_still_head(tah.voice_client, 7, 7, 50, 50);
    ta_screen(7, 7, 50, 50);
    ta_pic(5, 5, 54, 54, tah.voice_window, NULL);
    ta_text(7, 60 + 12, 0.25f, white, cl.clientinfo[tah.voice_client].name, 0);
}

static bool ta_hud_wanted(void)
{
    if (!cl_q3_hud_ta || !cl_q3_hud_ta->integer)
        return false;
    return cl_q3_hud_ta->integer >= 2 || cl.configstrings[CS_TA_HUD][0];
}

// cg_draw.c CG_Draw2D under MISSIONPACK: its menus only for the living, and
// not over the scoreboard
static void ta_draw_hud(const clientinfo_t *ci)
{
    ta_parse_state();

    ta_powerups();
    if (ta_gametype() >= TA_GT_TEAM)
        ta_selected_player();
    ta_player_status();
    ta_ammo();
    ta_health(ci);
    ta_armor();
    if (ta_gametype() >= TA_GT_TEAM)
        ta_team_scores();
    else
        ta_ffa_scores();
    if (ta_gametype() >= TA_GT_TEAM)
        ta_location();

    // the frame over it all
    ta_anchor(q3_mid, q3_bottom);
    ta_pic(0, 416, 640, 64, tah.bar, NULL);

    ta_voice_head();
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
    // Team Arena's: CTF's team pics and caps, the flag, the joined pics, the
    // persistent powerup, Harvester's skulls
    if (tah.drawn && (stat == TA_STAT_SKULLS || (stat >= TA_STAT_TEAM1_PIC && stat <= TA_STAT_TECH)))
        return true;

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
    // Team Arena's maps have its HUD whoever you are
    if (cl.configstrings[CS_TA_HUD][0] && ta_hud_wanted())
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
    // voice chats play, and their speaker's head comes down, whatever HUD is up
    ta_run_voices();
    tah.drawn = false;
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

    // Team Arena's in place of it all (cg_draw.c CG_Draw2D, MISSIONPACK):
    // Quake III's "following" and the pickup still drawn as Team Arena did
    if (ta_hud_wanted()) {
        if (!tah.registered)
            ta_register_media();
        if (playing && alive && !scoreboard) {
            ta_draw_hud(ci);
            tah.drawn = true;
            drew |= Q3HUD_SCORES | Q3HUD_TA;
            *bar_top = (int)q3_bottom(416);
            q3_pickup();
        }
        q3_follow();
        R_ClearColor();
        R_SetAlpha(alpha);
        return drew;
    }

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
    // Team Arena's HUD: 1 on its maps, 0 never, 2 wherever Quake III's draws
    cl_q3_hud_ta = Cvar_Get("cl_q3_hud_ta", "1", CVAR_ARCHIVE);

    tah.selected = -1;
    Cmd_AddCommand("ta_vchat", ta_vchat_f);
    Cmd_AddCommand("ta_tinfo", ta_tinfo_f);
    Cmd_AddCommand("nextteammember", ta_next_member_f);
    Cmd_AddCommand("prevteammember", ta_prev_member_f);
}
