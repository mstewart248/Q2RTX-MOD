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
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"
#include "refresh/images.h"

#define STAT_PICS       11
#define STAT_MINUS      (STAT_PICS - 1)  // num frame for '-' stats digit

static struct {
    bool        initialized;        // ready to draw

    qhandle_t   crosshair_pic;
    int         crosshair_width, crosshair_height;
    color_t     crosshair_color;

    qhandle_t   pause_pic;
    int         pause_width, pause_height;

    qhandle_t   loading_pic;
    int         loading_width, loading_height;
    bool        draw_loading;

    qhandle_t   sb_pics[2][STAT_PICS];
    qhandle_t   inven_pic;
    qhandle_t   field_pic;

    qhandle_t   backtile_pic;

    qhandle_t   net_pic;
    qhandle_t   font_pic;

    int         hud_width, hud_height;
    float       hud_scale;
    float       hud_alpha;
} scr;

cvar_t   *scr_viewsize;
static cvar_t   *scr_centertime;
static cvar_t   *scr_showpause;
#if USE_DEBUG
static cvar_t   *scr_showstats;
static cvar_t   *scr_showpmove;
#endif
static cvar_t   *scr_showturtle;
static cvar_t   *scr_showitemname;
static cvar_t   *scr_health_bars;

static cvar_t   *cl_weaponbar;
static cvar_t   *cl_weaponbar_time;
static cvar_t   *cl_weaponbar_hold;
static cvar_t   *cl_weaponbar_grace;

static cvar_t   *scr_draw2d;
static cvar_t   *scr_lag_x;
static cvar_t   *scr_lag_y;
static cvar_t   *scr_lag_draw;
static cvar_t   *scr_lag_min;
static cvar_t   *scr_lag_max;
static cvar_t   *scr_alpha;
static cvar_t   *scr_fps;

static cvar_t   *scr_demobar;
static cvar_t   *scr_font;
static cvar_t   *scr_scale;

static cvar_t   *scr_crosshair;

static cvar_t   *scr_chathud;
static cvar_t   *scr_chathud_lines;
static cvar_t   *scr_chathud_time;
static cvar_t   *scr_chathud_x;
static cvar_t   *scr_chathud_y;

static cvar_t   *ch_health;
static cvar_t   *ch_red;
static cvar_t   *ch_green;
static cvar_t   *ch_blue;
static cvar_t   *ch_alpha;

static cvar_t   *ch_scale;
static cvar_t   *ch_x;
static cvar_t   *ch_y;

vrect_t     scr_vrect;      // position of render window on screen

static const char *const sb_nums[2][STAT_PICS] = {
    {
        "num_0", "num_1", "num_2", "num_3", "num_4", "num_5",
        "num_6", "num_7", "num_8", "num_9", "num_minus"
    },
    {
        "anum_0", "anum_1", "anum_2", "anum_3", "anum_4", "anum_5",
        "anum_6", "anum_7", "anum_8", "anum_9", "anum_minus"
    }
};

const uint32_t colorTable[8] = {
    U32_BLACK, U32_RED, U32_GREEN, U32_YELLOW,
    U32_BLUE, U32_CYAN, U32_MAGENTA, U32_WHITE
};

/*
===============================================================================

UTILS

===============================================================================
*/

#define SCR_DrawString(x, y, flags, string) \
    SCR_DrawStringEx(x, y, flags, MAX_STRING_CHARS, string, scr.font_pic)

/*
==============
SCR_DrawStringEx
==============
*/
int SCR_DrawStringEx(int x, int y, int flags, size_t maxlen,
                     const char *s, qhandle_t font)
{
    size_t len = strlen(s);

    if (len > maxlen) {
        len = maxlen;
    }

    if ((flags & UI_CENTER) == UI_CENTER) {
        x -= len * CHAR_WIDTH / 2;
    } else if (flags & UI_RIGHT) {
        x -= len * CHAR_WIDTH;
    }

    return R_DrawString(x, y, flags, maxlen, s, font);
}


/*
==============
SCR_DrawStringMulti
==============
*/
void SCR_DrawStringMulti(int x, int y, int flags, size_t maxlen,
                         const char *s, qhandle_t font)
{
    char    *p;
    size_t  len;

    while (*s) {
        p = strchr(s, '\n');
        if (!p) {
            SCR_DrawStringEx(x, y, flags, maxlen, s, font);
            break;
        }

        len = p - s;
        if (len > maxlen) {
            len = maxlen;
        }
        SCR_DrawStringEx(x, y, flags, len, s, font);

        y += CHAR_HEIGHT;
        s = p + 1;
    }
}


/*
=================
SCR_FadeAlpha
=================
*/
float SCR_FadeAlpha(unsigned startTime, unsigned visTime, unsigned fadeTime)
{
    float alpha;
    unsigned timeLeft, delta = cls.realtime - startTime;

    if (delta >= visTime) {
        return 0;
    }

    if (fadeTime > visTime) {
        fadeTime = visTime;
    }

    alpha = 1;
    timeLeft = visTime - delta;
    if (timeLeft < fadeTime) {
        alpha = (float)timeLeft / fadeTime;
    }

    return alpha;
}

bool SCR_ParseColor(const char *s, color_t *color)
{
    int i;
    int c[8];

    // parse generic color
    if (*s == '#') {
        s++;
        for (i = 0; s[i]; i++) {
            if (i == 8) {
                return false;
            }
            c[i] = Q_charhex(s[i]);
            if (c[i] == -1) {
                return false;
            }
        }

        switch (i) {
        case 3:
            color->u8[0] = c[0] | (c[0] << 4);
            color->u8[1] = c[1] | (c[1] << 4);
            color->u8[2] = c[2] | (c[2] << 4);
            color->u8[3] = 255;
            break;
        case 6:
            color->u8[0] = c[1] | (c[0] << 4);
            color->u8[1] = c[3] | (c[2] << 4);
            color->u8[2] = c[5] | (c[4] << 4);
            color->u8[3] = 255;
            break;
        case 8:
            color->u8[0] = c[1] | (c[0] << 4);
            color->u8[1] = c[3] | (c[2] << 4);
            color->u8[2] = c[5] | (c[4] << 4);
            color->u8[3] = c[7] | (c[6] << 4);
            break;
        default:
            return false;
        }

        return true;
    }

    // parse name or index
    i = Com_ParseColor(s, COLOR_WHITE);
    if (i == COLOR_NONE) {
        return false;
    }

    color->u32 = colorTable[i];
    return true;
}

/*
===============================================================================

BAR GRAPHS

===============================================================================
*/

static void draw_percent_bar(int percent, bool paused, int framenum)
{
    char buffer[16];
    int x, w;
    size_t len;

    scr.hud_height -= CHAR_HEIGHT;

    w = scr.hud_width * percent / 100;

    R_DrawFill8(0, scr.hud_height, w, CHAR_HEIGHT, 4);
    R_DrawFill8(w, scr.hud_height, scr.hud_width - w, CHAR_HEIGHT, 0);

    len = Q_scnprintf(buffer, sizeof(buffer), "%d%%", percent);
    x = (scr.hud_width - len * CHAR_WIDTH) / 2;
    R_DrawString(x, scr.hud_height, 0, MAX_STRING_CHARS, buffer, scr.font_pic);

    if (scr_demobar->integer > 1) {
        int sec = framenum / 10;
        int min = sec / 60; sec %= 60;

        Q_scnprintf(buffer, sizeof(buffer), "%d:%02d.%d", min, sec, framenum % 10);
        R_DrawString(0, scr.hud_height, 0, MAX_STRING_CHARS, buffer, scr.font_pic);
    }

    if (paused) {
        SCR_DrawString(scr.hud_width, scr.hud_height, UI_RIGHT, "[PAUSED]");
    }
}

static void SCR_DrawDemo(void)
{
#if USE_MVD_CLIENT
    int percent;
    bool paused;
    int framenum;
#endif

    if (!scr_demobar->integer) {
        return;
    }

    if (cls.demo.playback) {
        if (cls.demo.file_size) {
            draw_percent_bar(
                cls.demo.file_percent,
                sv_paused->integer &&
                cl_paused->integer &&
                scr_showpause->integer == 2,
                cls.demo.frames_read);
        }
        return;
    }

#if USE_MVD_CLIENT
    if (sv_running->integer != ss_broadcast) {
        return;
    }

    if ((percent = MVD_GetDemoPercent(&paused, &framenum)) == -1) {
        return;
    }

    if (sv_paused->integer && cl_paused->integer && scr_showpause->integer == 2) {
        paused = true;
    }

    draw_percent_bar(percent, paused, framenum);
#endif
}

/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

static char     scr_centerstring[MAX_STRING_CHARS];
static unsigned scr_centertime_start;   // for slow victory printing
static int      scr_center_lines;

/*
==============
SCR_KeyForCommand

Writes the display name of the first key bound to `command`. False if it is
unbound.
==============
*/
static bool SCR_KeyForCommand(const char *command, char *out, size_t outsize)
{
    // a couple of the rerelease's commands have no equivalent here; the
    // equipment wheel prompt is really about opening the inventory
    static const char *const aliases[][2] = {
        { "+wheel",  "inven" },
        { "+wheel1", "inven" },
        { "+wheel2", "inven" },
    };
    const char *key;
    int i;

    for (i = 0; i < q_countof(aliases); i++) {
        if (!Q_stricmp(command, aliases[i][0])) {
            command = aliases[i][1];
            break;
        }
    }

    key = Key_GetBinding(command);
    if (!*key)
        return false;

    // the K_* names are already upper case; a plain ascii key is not
    Q_strlcpy(out, key, outsize);
    for (i = 0; out[i]; i++)
        out[i] = Q_toupper(out[i]);

    return true;
}

/*
==============
SCR_ExpandBinds

The rerelease prefixes some center prints with one or more
"%bind:<command>:<purpose>%" groups - a prompt naming the key the player
should press. The game library has already turned the purpose into display
text; only the client knows the binding, so the key goes in here.

The groups are stripped off the front and re-emitted underneath the message as
"[KEY] purpose", which is where the rerelease HUD draws them. An unbound
command keeps its purpose line without a key rather than losing the hint.
==============
*/
static void SCR_ExpandBinds(const char *in, char *out, size_t outsize)
{
    char    hints[MAX_STRING_CHARS];
    size_t  n = 0;

    hints[0] = 0;

    while (!strncmp(in, "%bind:", 6)) {
        const char  *body = in + 6;
        const char  *end = strchr(body, '%');
        const char  *colon, *purpose;
        char        command[64], key[32], one[MAX_STRING_CHARS];
        bool        bound;
        size_t      len;

        if (!end)
            break;

        colon = memchr(body, ':', end - body);
        len = (colon ? colon : end) - body;
        if (len >= sizeof(command))
            len = sizeof(command) - 1;
        memcpy(command, body, len);
        command[len] = 0;

        purpose = colon ? colon + 1 : end;
        bound = SCR_KeyForCommand(command, key, sizeof(key));

        one[0] = 0;
        if (bound && purpose < end)
            Q_snprintf(one, sizeof(one), "\n[%s] %.*s", key, (int)(end - purpose), purpose);
        else if (bound)
            Q_snprintf(one, sizeof(one), "\nPress [%s]", key);
        else if (purpose < end)
            Q_snprintf(one, sizeof(one), "\n%.*s", (int)(end - purpose), purpose);

        if (one[0] && n < sizeof(hints) - 1)
            n += Q_strlcpy(hints + n, one, sizeof(hints) - n);
        if (n >= sizeof(hints))
            n = sizeof(hints) - 1;

        in = end + 1;
    }

    Q_strlcpy(out, in, outsize);
    if (hints[0]) {
        Q_strlcat(out, "\n", outsize);      // blank line between text and hints
        Q_strlcat(out, hints, outsize);
    }
}

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void SCR_CenterPrint(const char *str)
{
    char        expanded[MAX_STRING_CHARS];
    const char  *s;

    SCR_ExpandBinds(str, expanded, sizeof(expanded));
    str = expanded;

    scr_centertime_start = cls.realtime;
    if (!strcmp(scr_centerstring, str)) {
        return;
    }

    Q_strlcpy(scr_centerstring, str, sizeof(scr_centerstring));

    // count the number of lines for centering
    scr_center_lines = 1;
    s = str;
    while (*s) {
        if (*s == '\n')
            scr_center_lines++;
        s++;
    }

    // echo it to the console
    Com_Printf("%s\n", scr_centerstring);
    Con_ClearNotify_f();
}

static void SCR_DrawCenterString(void)
{
    int y;
    float alpha;

    Cvar_ClampValue(scr_centertime, 0.3f, 10.0f);

    alpha = SCR_FadeAlpha(scr_centertime_start, scr_centertime->value * 1000, 300);
    if (!alpha) {
        return;
    }

    R_SetAlpha(alpha * scr_alpha->value);

    y = scr.hud_height / 4 - scr_center_lines * 8 / 2;

    SCR_DrawStringMulti(scr.hud_width / 2, y, UI_CENTER,
                        MAX_STRING_CHARS, scr_centerstring, scr.font_pic);

    R_SetAlpha(scr_alpha->value);
}

/*
===============================================================================

LAGOMETER

===============================================================================
*/

#define LAG_WIDTH   48
#define LAG_HEIGHT  48

#define LAG_CRIT_BIT    (1U << 31)
#define LAG_WARN_BIT    (1U << 30)

#define LAG_BASE    0xD5
#define LAG_WARN    0xDC
#define LAG_CRIT    0xF2

static struct {
    unsigned samples[LAG_WIDTH];
    unsigned head;
} lag;

void SCR_LagClear(void)
{
    lag.head = 0;
}

void SCR_LagSample(void)
{
    int i = cls.netchan->incoming_acknowledged & CMD_MASK;
    client_history_t *h = &cl.history[i];
    unsigned ping;

    h->rcvd = cls.realtime;
    if (!h->cmdNumber || h->rcvd < h->sent) {
        return;
    }

    ping = h->rcvd - h->sent;
    for (i = 0; i < cls.netchan->dropped; i++) {
        lag.samples[lag.head % LAG_WIDTH] = ping | LAG_CRIT_BIT;
        lag.head++;
    }

    if (cl.frameflags & FF_SUPPRESSED) {
        ping |= LAG_WARN_BIT;
    }
    lag.samples[lag.head % LAG_WIDTH] = ping;
    lag.head++;
}

static void SCR_LagDraw(int x, int y)
{
    int i, j, v, c, v_min, v_max, v_range;

    v_min = Cvar_ClampInteger(scr_lag_min, 0, LAG_HEIGHT * 10);
    v_max = Cvar_ClampInteger(scr_lag_max, 0, LAG_HEIGHT * 10);

    v_range = v_max - v_min;
    if (v_range < 1)
        return;

    for (i = 0; i < LAG_WIDTH; i++) {
        j = lag.head - i - 1;
        if (j < 0) {
            break;
        }

        v = lag.samples[j % LAG_WIDTH];

        if (v & LAG_CRIT_BIT) {
            c = LAG_CRIT;
        } else if (v & LAG_WARN_BIT) {
            c = LAG_WARN;
        } else {
            c = LAG_BASE;
        }

        v &= ~(LAG_WARN_BIT | LAG_CRIT_BIT);
        v = (v - v_min) * LAG_HEIGHT / v_range;
        clamp(v, 0, LAG_HEIGHT);

        R_DrawFill8(x + LAG_WIDTH - i - 1, y + LAG_HEIGHT - v, 1, v, c);
    }
}

static void SCR_DrawNet(void)
{
    int x = scr_lag_x->integer;
    int y = scr_lag_y->integer;

    if (x < 0) {
        x += scr.hud_width - LAG_WIDTH + 1;
    }
    if (y < 0) {
        y += scr.hud_height - LAG_HEIGHT + 1;
    }

    // draw ping graph
    if (scr_lag_draw->integer) {
        if (scr_lag_draw->integer > 1) {
            R_DrawFill8(x, y, LAG_WIDTH, LAG_HEIGHT, 4);
        }
        SCR_LagDraw(x, y);
    }

    // draw phone jack
    if (cls.netchan && cls.netchan->outgoing_sequence - cls.netchan->incoming_acknowledged >= CMD_BACKUP) {
        if ((cls.realtime >> 8) & 3) {
            R_DrawStretchPic(x, y, LAG_WIDTH, LAG_HEIGHT, scr.net_pic);
        }
    }
}


/*
===============================================================================

DRAW OBJECTS

===============================================================================
*/

typedef struct {
    list_t          entry;
    int             x, y;
    cvar_t          *cvar;
    cmd_macro_t     *macro;
    int             flags;
    color_t         color;
} drawobj_t;

#define FOR_EACH_DRAWOBJ(obj) \
    LIST_FOR_EACH(drawobj_t, obj, &scr_objects, entry)
#define FOR_EACH_DRAWOBJ_SAFE(obj, next) \
    LIST_FOR_EACH_SAFE(drawobj_t, obj, next, &scr_objects, entry)

static LIST_DECL(scr_objects);

static void SCR_Color_g(genctx_t *ctx)
{
    int color;

    for (color = 0; color < 10; color++)
        Prompt_AddMatch(ctx, colorNames[color]);
}

static void SCR_Draw_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Cvar_Variable_g(ctx);
        Cmd_Macro_g(ctx);
    } else if (argnum == 4) {
        SCR_Color_g(ctx);
    }
}

// draw cl_fps -1 80
static void SCR_Draw_f(void)
{
    int x, y;
    const char *s, *c;
    drawobj_t *obj;
    cmd_macro_t *macro;
    cvar_t *cvar;
    color_t color;
    int flags;
    int argc = Cmd_Argc();

    if (argc == 1) {
        if (LIST_EMPTY(&scr_objects)) {
            Com_Printf("No draw strings registered.\n");
            return;
        }
        Com_Printf("Name               X    Y\n"
                   "--------------- ---- ----\n");
        FOR_EACH_DRAWOBJ(obj) {
            s = obj->macro ? obj->macro->name : obj->cvar->name;
            Com_Printf("%-15s %4d %4d\n", s, obj->x, obj->y);
        }
        return;
    }

    if (argc < 4) {
        Com_Printf("Usage: %s <name> <x> <y> [color]\n", Cmd_Argv(0));
        return;
    }

    color.u32 = U32_BLACK;
    flags = UI_IGNORECOLOR;

    s = Cmd_Argv(1);
    x = atoi(Cmd_Argv(2));
    y = atoi(Cmd_Argv(3));

    if (x < 0) {
        flags |= UI_RIGHT;
    }

    if (argc > 4) {
        c = Cmd_Argv(4);
        if (!strcmp(c, "alt")) {
            flags |= UI_ALTCOLOR;
        } else if (strcmp(c, "none")) {
            if (!SCR_ParseColor(c, &color)) {
                Com_Printf("Unknown color '%s'\n", c);
                return;
            }
            flags &= ~UI_IGNORECOLOR;
        }
    }

    cvar = NULL;
    macro = Cmd_FindMacro(s);
    if (!macro) {
        cvar = Cvar_WeakGet(s);
    }

    FOR_EACH_DRAWOBJ(obj) {
        if (obj->macro == macro && obj->cvar == cvar) {
            obj->x = x;
            obj->y = y;
            obj->flags = flags;
            obj->color.u32 = color.u32;
            return;
        }
    }

    obj = Z_Malloc(sizeof(*obj));
    obj->x = x;
    obj->y = y;
    obj->cvar = cvar;
    obj->macro = macro;
    obj->flags = flags;
    obj->color.u32 = color.u32;

    List_Append(&scr_objects, &obj->entry);
}

static void SCR_Draw_g(genctx_t *ctx)
{
    drawobj_t *obj;
    const char *s;

    if (LIST_EMPTY(&scr_objects)) {
        return;
    }

    Prompt_AddMatch(ctx, "all");

    FOR_EACH_DRAWOBJ(obj) {
        s = obj->macro ? obj->macro->name : obj->cvar->name;
        Prompt_AddMatch(ctx, s);
    }
}

static void SCR_UnDraw_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        SCR_Draw_g(ctx);
    }
}

static void SCR_UnDraw_f(void)
{
    char *s;
    drawobj_t *obj, *next;
    cmd_macro_t *macro;
    cvar_t *cvar;

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: %s <name>\n", Cmd_Argv(0));
        return;
    }

    if (LIST_EMPTY(&scr_objects)) {
        Com_Printf("No draw strings registered.\n");
        return;
    }

    s = Cmd_Argv(1);
    if (!strcmp(s, "all")) {
        FOR_EACH_DRAWOBJ_SAFE(obj, next) {
            Z_Free(obj);
        }
        List_Init(&scr_objects);
        Com_Printf("Deleted all draw strings.\n");
        return;
    }

    cvar = NULL;
    macro = Cmd_FindMacro(s);
    if (!macro) {
        cvar = Cvar_WeakGet(s);
    }

    FOR_EACH_DRAWOBJ_SAFE(obj, next) {
        if (obj->macro == macro && obj->cvar == cvar) {
            List_Remove(&obj->entry);
            Z_Free(obj);
            return;
        }
    }

    Com_Printf("Draw string '%s' not found.\n", s);
}

static void SCR_DrawObjects(void)
{
    char buffer[MAX_QPATH];
    int x, y;
    drawobj_t *obj;

    FOR_EACH_DRAWOBJ(obj) {
        x = obj->x;
        y = obj->y;
        if (x < 0) {
            x += scr.hud_width + 1;
        }
        if (y < 0) {
            y += scr.hud_height - CHAR_HEIGHT + 1;
        }
        if (!(obj->flags & UI_IGNORECOLOR)) {
            R_SetColor(obj->color.u32);
        }
        if (obj->macro) {
            obj->macro->function(buffer, sizeof(buffer));
            SCR_DrawString(x, y, obj->flags, buffer);
        } else {
            SCR_DrawString(x, y, obj->flags, obj->cvar->string);
        }
        if (!(obj->flags & UI_IGNORECOLOR)) {
            R_ClearColor();
            R_SetAlpha(scr_alpha->value);
        }
    }
}

extern int CL_GetFps(void);
extern int CL_GetResolutionScale(void);
extern int CL_GetPresentedFramesPerRender(void);

static void SCR_DrawFPS(void)
{
	if (scr_fps->integer == 0)
		return;

	int fps = R_FPS;
	int scale = CL_GetResolutionScale();

	/* R_FPS counts RENDERED frames. With frame generation each of those becomes
	   several presents, so report both: what the engine renders, and what the display
	   actually receives. Without this the counter shows half (or a sixth) of the
	   presented rate and frame generation looks like it is doing nothing. */
	int per_render = CL_GetPresentedFramesPerRender();

	char buffer[MAX_QPATH];
	if (scr_fps->integer == 2 && cls.ref_type == REF_TYPE_VKPT)
		Q_snprintf(buffer, MAX_QPATH, "%d FPS at %3d%%", fps, scale);
	else
		Q_snprintf(buffer, MAX_QPATH, "%d FPS", fps);

	if (per_render > 1 && cls.ref_type == REF_TYPE_VKPT) {
		char fg_buffer[MAX_QPATH];
		Q_snprintf(fg_buffer, MAX_QPATH, "%s -> %d presented (%dx FG)",
			buffer, fps * per_render, per_render);
		memcpy(buffer, fg_buffer, sizeof(buffer));
	}

	int x = scr.hud_width - 2;
	int y = 1;

	R_SetColor(~0u);
	SCR_DrawString(x, y, UI_RIGHT, buffer);
}

/*
===============================================================================

CHAT HUD

===============================================================================
*/

#define MAX_CHAT_TEXT       150
#define MAX_CHAT_LINES      32
#define CHAT_LINE_MASK      (MAX_CHAT_LINES - 1)

typedef struct {
    char        text[MAX_CHAT_TEXT];
    unsigned    time;
} chatline_t;

static chatline_t   scr_chatlines[MAX_CHAT_LINES];
static unsigned     scr_chathead;

void SCR_ClearChatHUD_f(void)
{
    memset(scr_chatlines, 0, sizeof(scr_chatlines));
    scr_chathead = 0;
}

void SCR_AddToChatHUD(const char *text)
{
    chatline_t *line;
    char *p;

    line = &scr_chatlines[scr_chathead++ & CHAT_LINE_MASK];
    Q_strlcpy(line->text, text, sizeof(line->text));
    line->time = cls.realtime;

    p = strrchr(line->text, '\n');
    if (p)
        *p = 0;
}

static void SCR_DrawChatHUD(void)
{
    int x, y, i, lines, flags, step;
    float alpha;
    chatline_t *line;

    if (scr_chathud->integer == 0)
        return;

    x = scr_chathud_x->integer;
    y = scr_chathud_y->integer;

    if (scr_chathud->integer == 2)
        flags = UI_ALTCOLOR;
    else
        flags = 0;

    if (x < 0) {
        x += scr.hud_width + 1;
        flags |= UI_RIGHT;
    } else {
        flags |= UI_LEFT;
    }

    if (y < 0) {
        y += scr.hud_height - CHAR_HEIGHT + 1;
        step = -CHAR_HEIGHT;
    } else {
        step = CHAR_HEIGHT;
    }

    lines = scr_chathud_lines->integer;
    if (lines > scr_chathead)
        lines = scr_chathead;

    for (i = 0; i < lines; i++) {
        line = &scr_chatlines[(scr_chathead - i - 1) & CHAT_LINE_MASK];

        if (scr_chathud_time->integer) {
            alpha = SCR_FadeAlpha(line->time, scr_chathud_time->integer, 1000);
            if (!alpha)
                break;

            R_SetAlpha(alpha * scr_alpha->value);
            SCR_DrawString(x, y, flags, line->text);
            R_SetAlpha(scr_alpha->value);
        } else {
            SCR_DrawString(x, y, flags, line->text);
        }

        y += step;
    }
}

/*
===============================================================================

DEBUG STUFF

===============================================================================
*/

static void SCR_DrawTurtle(void)
{
    int x, y;

    if (scr_showturtle->integer <= 0)
        return;

    if (!cl.frameflags)
        return;

    x = CHAR_WIDTH;
    y = scr.hud_height - 11 * CHAR_HEIGHT;

#define DF(f) \
    if (cl.frameflags & FF_##f) { \
        SCR_DrawString(x, y, UI_ALTCOLOR, #f); \
        y += CHAR_HEIGHT; \
    }

    if (scr_showturtle->integer > 1) {
        DF(SUPPRESSED)
    }
    DF(CLIENTPRED)
    if (scr_showturtle->integer > 1) {
        DF(CLIENTDROP)
        DF(SERVERDROP)
    }
    DF(BADFRAME)
    DF(OLDFRAME)
    DF(OLDENT)
    DF(NODELTA)

#undef DF
}

#if USE_DEBUG

static void SCR_DrawDebugStats(void)
{
    char buffer[MAX_QPATH];
    int i, j;
    int x, y;

    j = scr_showstats->integer;
    if (j <= 0)
        return;

    if (j > MAX_STATS)
        j = MAX_STATS;

    x = CHAR_WIDTH;
    y = (scr.hud_height - j * CHAR_HEIGHT) / 2;
    for (i = 0; i < j; i++) {
        Q_snprintf(buffer, sizeof(buffer), "%2d: %d", i, cl.frame.ps.stats[i]);
        if (cl.oldframe.ps.stats[i] != cl.frame.ps.stats[i]) {
            R_SetColor(U32_RED);
        }
        R_DrawString(x, y, 0, MAX_STRING_CHARS, buffer, scr.font_pic);
        R_ClearColor();
        y += CHAR_HEIGHT;
    }
}

static void SCR_DrawDebugPmove(void)
{
    static const char * const types[] = {
        "NORMAL", "SPECTATOR", "DEAD", "GIB", "FREEZE"
    };
    static const char * const flags[] = {
        "DUCKED", "JUMP_HELD", "ON_GROUND",
        "TIME_WATERJUMP", "TIME_LAND", "TIME_TELEPORT",
        "NO_PREDICTION", "TELEPORT_BIT"
    };
    unsigned i, j;
    int x, y;

    if (!scr_showpmove->integer)
        return;

    x = CHAR_WIDTH;
    y = (scr.hud_height - 2 * CHAR_HEIGHT) / 2;

    i = cl.frame.ps.pmove.pm_type;
    if (i > PM_FREEZE)
        i = PM_FREEZE;

    R_DrawString(x, y, 0, MAX_STRING_CHARS, types[i], scr.font_pic);
    y += CHAR_HEIGHT;

    j = cl.frame.ps.pmove.pm_flags;
    for (i = 0; i < 8; i++) {
        if (j & (1 << i)) {
            x = R_DrawString(x, y, 0, MAX_STRING_CHARS, flags[i], scr.font_pic);
            x += CHAR_WIDTH;
        }
    }
}

#endif

//============================================================================

// Sets scr_vrect, the coordinates of the rendered window
void SCR_CalcVrect(void)
{
    scr_vrect.width = scr.hud_width;
    scr_vrect.height = scr.hud_height;
    scr_vrect.x = 0;
    scr_vrect.y = 0;
}

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void SCR_SizeUp_f(void)
{
	int delta = (scr_viewsize->integer < 100) ? 5 : 10;
    Cvar_SetInteger(scr_viewsize, scr_viewsize->integer + delta, FROM_CONSOLE);
}

/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f(void)
{
	int delta = (scr_viewsize->integer <= 100) ? 5 : 10;
	Cvar_SetInteger(scr_viewsize, scr_viewsize->integer - delta, FROM_CONSOLE);
}

/*
=================
SCR_Sky_f

Set a specific sky and rotation speed. If empty sky name is provided, falls
back to server defaults.
=================
*/
static void SCR_Sky_f(void)
{
    char    *name;
    float   rotate;
    vec3_t  axis;
    int     argc = Cmd_Argc();

    if (argc < 2) {
        Com_Printf("Usage: sky <basename> [rotate] [axis x y z]\n");
        return;
    }

    if (cls.state != ca_active) {
        Com_Printf("No map loaded.\n");
        return;
    }

    name = Cmd_Argv(1);
    if (!*name) {
        CL_SetSky();
        return;
    }

    if (argc > 2)
        rotate = atof(Cmd_Argv(2));
    else
        rotate = 0;

    if (argc == 6) {
        axis[0] = atof(Cmd_Argv(3));
        axis[1] = atof(Cmd_Argv(4));
        axis[2] = atof(Cmd_Argv(5));
    } else
        VectorSet(axis, 0, 0, 1);

    R_SetSky(name, rotate, 1, axis);
}

/*
================
SCR_TimeRefresh_f
================
*/
static void SCR_TimeRefresh_f(void)
{
    int     i;
    unsigned    start, stop;
    float       time;

    if (cls.state != ca_active) {
        Com_Printf("No map loaded.\n");
        return;
    }

    start = Sys_Milliseconds();

    if (Cmd_Argc() == 2) {
        // run without page flipping
        R_BeginFrame();
        for (i = 0; i < 128; i++) {
            cl.refdef.viewangles[1] = i / 128.0f * 360.0f;
            R_RenderFrame(&cl.refdef, 0);
        }
        R_EndFrame();
    } else {
        for (i = 0; i < 128; i++) {
            cl.refdef.viewangles[1] = i / 128.0f * 360.0f;

            R_BeginFrame();
            R_RenderFrame(&cl.refdef, 0);
            R_EndFrame();
        }
    }

    stop = Sys_Milliseconds();
    time = (stop - start) * 0.001f;
    Com_Printf("%f seconds (%f fps)\n", time, 128.0f / time);
}


//============================================================================

static void scr_crosshair_changed(cvar_t *self)
{
    char buffer[16];
    int w, h;
    float scale;

    if (scr_crosshair->integer > 0) {
        Q_snprintf(buffer, sizeof(buffer), "ch%i", scr_crosshair->integer);
        scr.crosshair_pic = R_RegisterPic(buffer);
        R_GetPicSize(&w, &h, scr.crosshair_pic);

        // prescale
        scale = Cvar_ClampValue(ch_scale, 0.1f, 9.0f);
        scr.crosshair_width = w * scale;
        scr.crosshair_height = h * scale;
        if (scr.crosshair_width < 1)
            scr.crosshair_width = 1;
        if (scr.crosshair_height < 1)
            scr.crosshair_height = 1;

        if (ch_health->integer) {
            SCR_SetCrosshairColor();
        } else {
            scr.crosshair_color.u8[0] = Cvar_ClampValue(ch_red, 0, 1) * 255;
            scr.crosshair_color.u8[1] = Cvar_ClampValue(ch_green, 0, 1) * 255;
            scr.crosshair_color.u8[2] = Cvar_ClampValue(ch_blue, 0, 1) * 255;
        }
        scr.crosshair_color.u8[3] = Cvar_ClampValue(ch_alpha, 0, 1) * 255;
    } else {
        scr.crosshair_pic = 0;
    }
}

void SCR_SetCrosshairColor(void)
{
    int health;

    if (!ch_health->integer) {
        return;
    }

    health = cl.frame.ps.stats[STAT_HEALTH];
    if (health <= 0) {
        VectorSet(scr.crosshair_color.u8, 0, 0, 0);
        return;
    }

    // red
    scr.crosshair_color.u8[0] = 255;

    // green
    if (health >= 66) {
        scr.crosshair_color.u8[1] = 255;
    } else if (health < 33) {
        scr.crosshair_color.u8[1] = 0;
    } else {
        scr.crosshair_color.u8[1] = (255 * (health - 33)) / 33;
    }

    // blue
    if (health >= 99) {
        scr.crosshair_color.u8[2] = 255;
    } else if (health < 66) {
        scr.crosshair_color.u8[2] = 0;
    } else {
        scr.crosshair_color.u8[2] = (255 * (health - 66)) / 33;
    }
}

void SCR_ModeChanged(void)
{
    IN_Activate();
    Con_CheckResize();
    UI_ModeChanged();
    // video sync flag may have changed
    CL_UpdateFrameTimes();
    cls.disable_screen = 0;
    if (scr.initialized)
        scr.hud_scale = R_ClampScale(scr_scale);

    scr.hud_alpha = 1.f;
}

/*
==================
SCR_RegisterMedia
==================
*/
void SCR_RegisterMedia(void)
{
    int     i, j;

    for (i = 0; i < 2; i++)
        for (j = 0; j < STAT_PICS; j++)
            scr.sb_pics[i][j] = R_RegisterPic(sb_nums[i][j]);

    scr.inven_pic = R_RegisterPic("inventory");
    scr.field_pic = R_RegisterPic("field_3");

    scr.backtile_pic = R_RegisterImage("backtile", IT_PIC, IF_PERMANENT | IF_REPEAT, NULL);

    scr.pause_pic = R_RegisterPic("pause");
    R_GetPicSize(&scr.pause_width, &scr.pause_height, scr.pause_pic);

    scr.loading_pic = R_RegisterPic("loading");
    R_GetPicSize(&scr.loading_width, &scr.loading_height, scr.loading_pic);

    scr.net_pic = R_RegisterPic("net");
    scr.font_pic = R_RegisterFont(scr_font->string);

    scr_crosshair_changed(scr_crosshair);
}

static void scr_font_changed(cvar_t *self)
{
    scr.font_pic = R_RegisterFont(self->string);
}

static void scr_scale_changed(cvar_t *self)
{
    scr.hud_scale = R_ClampScale(self);
}

// the weapon bar itself is further down, next to the inventory screen
static void SCR_WeaponBarInit(void);
static void SCR_WeapNext_f(void);
static void SCR_WeapPrev_f(void);

static const cmdreg_t scr_cmds[] = {
    { "timerefresh", SCR_TimeRefresh_f },
    { "sizeup", SCR_SizeUp_f },
    { "sizedown", SCR_SizeDown_f },
    { "sky", SCR_Sky_f },
    { "draw", SCR_Draw_f, SCR_Draw_c },
    { "undraw", SCR_UnDraw_f, SCR_UnDraw_c },
    { "clearchathud", SCR_ClearChatHUD_f },
    { "weapnext", SCR_WeapNext_f },
    { "weapprev", SCR_WeapPrev_f },
    { NULL }
};

/*
==================
SCR_Init
==================
*/
void SCR_Init(void)
{
    scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
    scr_showpause = Cvar_Get("scr_showpause", "1", 0);
    scr_centertime = Cvar_Get("scr_centertime", "2.5", 0);
    scr_demobar = Cvar_Get("scr_demobar", "1", 0);
    scr_font = Cvar_Get("scr_font", "conchars", 0);
    scr_font->changed = scr_font_changed;
    scr_scale = Cvar_Get("scr_scale", "0", 0);
    scr_scale->changed = scr_scale_changed;
    scr_crosshair = Cvar_Get("crosshair", "0", CVAR_ARCHIVE);
    scr_crosshair->changed = scr_crosshair_changed;

    scr_chathud = Cvar_Get("scr_chathud", "0", 0);
    scr_chathud_lines = Cvar_Get("scr_chathud_lines", "4", 0);
    scr_chathud_time = Cvar_Get("scr_chathud_time", "0", 0);
    scr_chathud_time->changed = cl_timeout_changed;
    scr_chathud_time->changed(scr_chathud_time);
    scr_chathud_x = Cvar_Get("scr_chathud_x", "8", 0);
    scr_chathud_y = Cvar_Get("scr_chathud_y", "-64", 0);

    ch_health = Cvar_Get("ch_health", "0", 0);
    ch_health->changed = scr_crosshair_changed;
    ch_red = Cvar_Get("ch_red", "1", 0);
    ch_red->changed = scr_crosshair_changed;
    ch_green = Cvar_Get("ch_green", "1", 0);
    ch_green->changed = scr_crosshair_changed;
    ch_blue = Cvar_Get("ch_blue", "1", 0);
    ch_blue->changed = scr_crosshair_changed;
    ch_alpha = Cvar_Get("ch_alpha", "1", 0);
    ch_alpha->changed = scr_crosshair_changed;

    ch_scale = Cvar_Get("ch_scale", "1", 0);
    ch_scale->changed = scr_crosshair_changed;
    ch_x = Cvar_Get("ch_x", "0", 0);
    ch_y = Cvar_Get("ch_y", "0", 0);

    scr_draw2d = Cvar_Get("scr_draw2d", "2", 0);
    scr_showturtle = Cvar_Get("scr_showturtle", "1", 0);
    scr_showitemname = Cvar_Get("scr_showitemname", "1", CVAR_ARCHIVE);
    scr_health_bars = Cvar_Get("scr_health_bars", "1", CVAR_ARCHIVE);
    cl_weaponbar = Cvar_Get("cl_weaponbar", "1", CVAR_ARCHIVE);
    cl_weaponbar_time = Cvar_Get("cl_weaponbar_time", "0.4", CVAR_ARCHIVE);
    cl_weaponbar_hold = Cvar_Get("cl_weaponbar_hold", "0.8", CVAR_ARCHIVE);
    cl_weaponbar_grace = Cvar_Get("cl_weaponbar_grace", "0.5", CVAR_ARCHIVE);
    SCR_WeaponBarInit();
    scr_lag_x = Cvar_Get("scr_lag_x", "-1", 0);
    scr_lag_y = Cvar_Get("scr_lag_y", "-1", 0);
    scr_lag_draw = Cvar_Get("scr_lag_draw", "0", 0);
    scr_lag_min = Cvar_Get("scr_lag_min", "0", 0);
    scr_lag_max = Cvar_Get("scr_lag_max", "200", 0);
	scr_alpha = Cvar_Get("scr_alpha", "1", 0);
	scr_fps = Cvar_Get("scr_fps", "0", CVAR_ARCHIVE);
#ifdef USE_DEBUG
    scr_showstats = Cvar_Get("scr_showstats", "0", 0);
    scr_showpmove = Cvar_Get("scr_showpmove", "0", 0);
#endif

    Cmd_Register(scr_cmds);

    scr_scale_changed(scr_scale);

    scr.initialized = true;
}

void SCR_Shutdown(void)
{
    Cmd_Deregister(scr_cmds);
    scr.initialized = false;
}

/*
================
SCR_BeginLoadingPlaque
================
*/
void SCR_BeginLoadingPlaque(void)
{
    if (!cls.state) {
        return;
    }

    if (cls.disable_screen) {
        return;
    }

#if USE_DEBUG
    if (developer->integer) {
        return;
    }
#endif

    // if at console or menu, don't bring up the plaque
    if (cls.key_dest & (KEY_CONSOLE | KEY_MENU)) {
        return;
    }

    scr.draw_loading = true;
    SCR_UpdateScreen();

    cls.disable_screen = Sys_Milliseconds();
}

/*
================
SCR_EndLoadingPlaque
================
*/
void SCR_EndLoadingPlaque(void)
{
    if (!cls.state) {
        return;
    }
    cls.disable_screen = 0;
    Con_ClearNotify_f();
}

// Clear any parts of the tiled background that were drawn on last frame
static void SCR_TileClear(void)
{
}

/*
===============================================================================

STAT PROGRAMS

===============================================================================
*/

#define ICON_WIDTH  24
#define ICON_HEIGHT 24
#define DIGIT_WIDTH 16
#define ICON_SPACE  8

#define HUD_DrawString(x, y, string) \
    R_DrawString(x, y, 0, MAX_STRING_CHARS, string, scr.font_pic)

#define HUD_DrawAltString(x, y, string) \
    R_DrawString(x, y, UI_XORCOLOR, MAX_STRING_CHARS, string, scr.font_pic)

#define HUD_DrawCenterString(x, y, string) \
    SCR_DrawStringMulti(x, y, UI_CENTER, MAX_STRING_CHARS, string, scr.font_pic)

#define HUD_DrawAltCenterString(x, y, string) \
    SCR_DrawStringMulti(x, y, UI_CENTER | UI_XORCOLOR, MAX_STRING_CHARS, string, scr.font_pic)

static void HUD_DrawNumber(int x, int y, int color, int width, int value)
{
    char    num[16], *ptr;
    int     l;
    int     frame;

    if (width < 1)
        return;

    // draw number string
    if (width > 5)
        width = 5;

    color &= 1;

    l = Q_scnprintf(num, sizeof(num), "%i", value);
    if (l > width)
        l = width;
    x += 2 + DIGIT_WIDTH * (width - l);

    ptr = num;
    while (*ptr && l) {
        if (*ptr == '-')
            frame = STAT_MINUS;
        else
            frame = *ptr - '0';

        R_DrawPic(x, y, scr.sb_pics[color][frame]);
        x += DIGIT_WIDTH;
        ptr++;
        l--;
    }
}

/*
===============================================================================

WEAPON BAR

The rerelease's weapon bar.  The mouse wheel slides a highlight along the guns
you are carrying WITHOUT switching, and the one you land on is raised a moment
after you stop scrolling - so you can run the whole list without cycling
through every weapon on the way.

It lives entirely on the client, because the highlight has to follow the wheel
with no server round trip, and everything it needs is already on this side:
the counts arrive as svc_inventory, the item names as CS_ITEMS, and the gun in
your hands is ps.gunindex.  The switch itself is an ordinary "use <item>" sent
once the selection settles.

The slot order below is the rerelease's item_id_t order (src/rerelease/
g_local.h), which is what its bar is sorted by, and it is used ONLY in the
rerelease game - where g_items.c's rerelease_weapon_order[] walks the same
sequence, so the bar and a plain weapnext agree.  Every other game keeps this
tree's own itemlist order, wb_classic_order[] below, and the 1997 backwards
weapnext with it: the bar must never reorder the original game's weapon cycle.

A weapon this game does not have simply never resolves and is skipped.

===============================================================================
*/

typedef struct {
    const char  *item;      // CS_ITEMS pickup name
    const char  *icon;      // pic name, minus the extension
    const char  *view;      // CS_MODELS path, which is how ps.gunindex names it
    const char  *ammo;      // CS_ITEMS pickup name of its ammo, NULL for none
} wbslot_t;

static const wbslot_t wb_slots[] = {
    { "Blaster",          "w_blaster",       "models/weapons/v_blast/tris.md2",     NULL },
    { "Chainfist",        "w_chainfist",     "models/weapons/v_chainf/tris.md2",    NULL },
    { "Shotgun",          "w_shotgun",       "models/weapons/v_shotg/tris.md2",     "Shells" },
    { "Super Shotgun",    "w_sshotgun",      "models/weapons/v_shotg2/tris.md2",    "Shells" },
    { "Machinegun",       "w_machinegun",    "models/weapons/v_machn/tris.md2",     "Bullets" },
    { "ETF Rifle",        "w_etf_rifle",     "models/weapons/v_etf_rifle/tris.md2", "Flechettes" },
    { "Chaingun",         "w_chaingun",      "models/weapons/v_chain/tris.md2",     "Bullets" },
    { "Grenades",         "a_grenades",      "models/weapons/v_handgr/tris.md2",    "Grenades" },
    { "Trap",             "a_trap",          "models/weapons/v_trap/tris.md2",      "Trap" },
    { "Tesla",            "a_tesla",         "models/weapons/v_tesla/tris.md2",     "Tesla" },
    { "Grenade Launcher", "w_glauncher",     "models/weapons/v_launch/tris.md2",    "Grenades" },
    // no flare gun: it is not on the rerelease's bar, and its "w_flareg" icon
    // was never authored anywhere.  It is still an item, so "use Flare Gun"
    // and the inventory screen still reach it.
    { "Prox Launcher",    "w_proxlaunch",    "models/weapons/v_plaunch/tris.md2",   "Prox" },
    { "Rocket Launcher",  "w_rlauncher",     "models/weapons/v_rocket/tris.md2",    "Rockets" },
    { "HyperBlaster",     "w_hyperblaster",  "models/weapons/v_hyperb/tris.md2",    "Cells" },
    { "Ionripper",        "w_ripper",        "models/weapons/v_boomer/tris.md2",    "Cells" },
    { "Plasma Beam",      "w_heatbeam",      "models/weapons/v_beamer/tris.md2",    "Cells" },
    { "Railgun",          "w_railgun",       "models/weapons/v_rail/tris.md2",      "Slugs" },
    { "Phalanx",          "w_phallanx",      "models/weapons/v_shotx/tris.md2",     "Mag Slug" },
    { "BFG10K",           "w_bfg",           "models/weapons/v_bfg/tris.md2",       "Cells" },
    { "Disruptor",        "w_disintegrator", "models/weapons/v_dist/tris.md2",      "Rounds" },
};

// The same weapons in this tree's own itemlist order, which is the order a
// weapnext walks outside the rerelease game.  Listed by pickup name so it can
// only ever name a weapon wb_slots[] also has.
static const char *const wb_classic_order[] = {
    "Blaster", "Shotgun", "Super Shotgun", "Machinegun", "Chaingun",
    "Grenades", "Grenade Launcher", "Rocket Launcher", "HyperBlaster",
    "Railgun", "BFG10K", "Trap", "Disruptor", "Tesla", "Prox Launcher",
    "ETF Rifle", "Plasma Beam", "Chainfist", "Ionripper", "Phalanx",
};

#define WB_NUM_SLOTS    ((int)q_countof(wb_slots))

// the two lists have to hold the same weapons
typedef char wb_orders_agree[q_countof(wb_classic_order) == q_countof(wb_slots) ? 1 : -1];

// the w_* pics are 24x24; the bar draws them a little larger, the way the
// rerelease sits its own row over the status bar
#define WB_ICON_SIZE    32

static struct {
    int         item[WB_NUM_SLOTS];     // CS_ITEMS index, -1 if this game has no such item
    int         ammo[WB_NUM_SLOTS];     // CS_ITEMS index of its ammo, -1 for none
    qhandle_t   pic[WB_NUM_SLOTS];
    int         order[WB_NUM_SLOTS];    // wb_slots[] indices, in cycle order
    int         place[WB_NUM_SLOTS];    // wb_slots[] index -> where it sits in order[]
    int         dir;                    // which way along order[] weapnext runs
    int         selected;               // slot the highlight is on, -1 when idle
    unsigned    input_time;             // cls.realtime of the last wheel step
    bool        committed;              // the "use" has already gone out
    int         commit_slot;            // slot the "use" named, -1 if none yet
    int         last_slot;              // where the highlight was when the bar went idle
    unsigned    idle_time;              // cls.realtime the bar went idle
} wb;

static int SCR_WeaponBarFindItem(const char *name)
{
    int     i;

    if (!name)
        return -1;

    for (i = 0; i < MAX_ITEMS; i++)
        if (!strcmp(cl.configstrings[CS_ITEMS + i], name))
            return i;

    return -1;
}

// Resolved fresh every time the bar wakes up rather than cached against the
// map load, so it can never be left pointing at the previous level's item
// numbering.  Twenty-odd names against 256 configstrings, once per scroll.
static void SCR_WeaponBarResolve(void)
{
    int     i, j;

    for (i = 0; i < WB_NUM_SLOTS; i++) {
        wb.item[i] = SCR_WeaponBarFindItem(wb_slots[i].item);
        wb.ammo[i] = SCR_WeaponBarFindItem(wb_slots[i].ammo);
        wb.pic[i] = wb.item[i] >= 0 ? R_RegisterPic2(wb_slots[i].icon) : 0;
        wb.order[i] = i;

        // No icon, no slot.  baseq2 carries none of the rogue or xatrix art,
        // so a cheated-in Disruptor there would otherwise sit on the bar as an
        // empty cell with an ammo count floating over it.  Dropping the item
        // index takes the slot out of the draw, the step and the seed at once;
        // the weapon is still an item, so "use" and the inventory reach it.
        if (!wb.pic[i])
            wb.item[i] = -1;
    }

    // The rerelease sorts its bar by item_id_t and runs weapnext FORWARD along
    // it; the 1997 game walks its item list backwards, so "next" moves toward
    // the blaster.  Match whichever game we are connected to - this is the
    // same test M_RereleaseGame() makes on the other side.
    if (!Q_stricmp(cl.gamedir, "rerelease")) {
        wb.dir = 1;
    } else {
        wb.dir = -1;
        for (i = 0; i < WB_NUM_SLOTS; i++)
            for (j = 0; j < WB_NUM_SLOTS; j++)
                if (!strcmp(wb_slots[j].item, wb_classic_order[i])) {
                    wb.order[i] = j;
                    break;
                }
    }

    for (i = 0; i < WB_NUM_SLOTS; i++)
        wb.place[wb.order[i]] = i;
}

// The gun in our hands, named by its view model.  No stat needed: the server
// already tells us which model the player state is holding, and the view model
// is unique per weapon.
static int SCR_WeaponBarHeld(void)
{
    const char  *model;
    int         i;

    if (cl.frame.ps.gunindex <= 0)
        return -1;

    model = cl.configstrings[CS_MODELS + cl.frame.ps.gunindex];
    for (i = 0; i < WB_NUM_SLOTS; i++)
        if (!strcmp(model, wb_slots[i].view))
            return i;

    return -1;
}

static bool SCR_WeaponBarCarried(int slot)
{
    return wb.item[slot] >= 0 && cl.inventory[wb.item[slot]] > 0;
}

// Returns false when the bar cannot take the input - not in a game, dead, or
// holding something it does not know about - so the caller can hand the
// command to the server instead.
static bool SCR_WeaponBarStep(int dir)
{
    int     i, from;

    if (cls.state != ca_active || cls.demo.playback)
        return false;
    if (cl.frame.ps.stats[STAT_HEALTH] <= 0)
        return false;

    // A step while the bar is still up - through the settle AND the whole
    // fade - continues from the highlight the player can see, even once the
    // "use" for it has gone out.  Re-seeding from the weapon in our hands
    // there is what made a late correction jump: the switch costs a server
    // round trip plus the lowering animation, so ps.gunindex still names the
    // weapon you scrolled away from for long after the commit, and the
    // highlight would snap back to it.
    if (wb.selected < 0) {
        unsigned    grace = Cvar_ClampValue(cl_weaponbar_grace, 0, 5) * 1000;

        SCR_WeaponBarResolve();

        // Just missed it: for a moment after the bar has faded out a step
        // picks the selection back up where it left off rather than starting
        // over from the gun, so a correction that comes in a touch late still
        // lands where the player was aiming it.  A resume keeps commit_slot,
        // so settling back on the weapon already on its way to us does not
        // ask for it a second time.
        if (wb.last_slot >= 0 && SCR_WeaponBarCarried(wb.last_slot) &&
            cls.realtime - wb.idle_time <= grace) {
            wb.selected = wb.last_slot;
        } else {
            wb.selected = SCR_WeaponBarHeld();
            wb.commit_slot = -1;
        }

        // nothing the bar can highlight - leave it idle rather than selected
        // on a weapon it has no name or icon for
        if (wb.selected < 0 || wb.item[wb.selected] < 0) {
            wb.selected = -1;
            return false;
        }
    }

    // Back out of the committed state: a correction during the fade then
    // settles and sends like any other selection, and the refreshed
    // input_time below takes the bar back to full opacity, which is what
    // shows the player the late input was taken.
    wb.committed = false;

    dir *= wb.dir;
    from = wb.place[wb.selected];
    for (i = 1; i <= WB_NUM_SLOTS; i++) {
        int next = wb.order[((from + dir * i) % WB_NUM_SLOTS + WB_NUM_SLOTS) % WB_NUM_SLOTS];
        if (SCR_WeaponBarCarried(next)) {
            wb.selected = next;
            break;
        }
    }

    wb.input_time = cls.realtime;
    return true;
}

// Raise the highlighted weapon once the wheel has been still long enough, then
// leave the bar up a moment more so you can see what you landed on.
static void SCR_WeaponBarThink(void)
{
    unsigned    settle, hold;
    int         target;

    if (wb.selected < 0)
        return;

    if (cls.state != ca_active) {
        wb.selected = -1;
        wb.last_slot = -1;
        wb.commit_slot = -1;
        return;
    }

    settle = cl_weaponbar_time->value * 1000;
    hold = cl_weaponbar_hold->value * 1000;

    if (!wb.committed && cls.realtime - wb.input_time >= settle) {
        wb.committed = true;

        // What we are on our way to holding, which is not the same thing as
        // what we are holding: once a "use" has gone out the gun in our hands
        // takes a round trip plus the lowering animation to catch up.  Test
        // the correction against the request, so that scrolling back onto the
        // weapon we started from still cancels a switch that is in flight,
        // and so that landing again on the one already coming does not ask
        // for it twice.
        target = wb.commit_slot >= 0 ? wb.commit_slot : SCR_WeaponBarHeld();

        if (wb.selected != target)
            CL_ClientCommand(va("use %s", wb_slots[wb.selected].item));
        wb.commit_slot = wb.selected;
    }

    // keep the place across the fade-out, for the grace window in Step()
    if (cls.realtime - wb.input_time >= settle + hold) {
        wb.last_slot = wb.selected;
        wb.idle_time = cls.realtime;
        wb.selected = -1;
    }
}

// the HUD digits, scaled to sit above an icon three abreast
static void SCR_WeaponBarNumber(int cx, int y, int color, int value, int dw, int dh)
{
    char    num[8];
    int     i, l, x;

    l = Q_scnprintf(num, sizeof(num), "%i", value);
    if (l > 4)
        l = 4;

    x = cx - l * dw / 2;
    for (i = 0; i < l; i++) {
        R_DrawStretchPic(x, y, dw, dh, scr.sb_pics[color][num[i] - '0']);
        x += dw;
    }
}

static void SCR_WeaponBarOutline(int x, int y, int w, int h, uint32_t color)
{
    R_DrawFill32(x, y, w, 1, color);
    R_DrawFill32(x, y + h - 1, w, 1, color);
    R_DrawFill32(x, y + 1, 1, h - 2, color);
    R_DrawFill32(x + w - 1, y + 1, 1, h - 2, color);
}

static void SCR_DrawWeaponBar(void)
{
    int         list[WB_NUM_SLOTS];
    int         i, count, slot_w, icon_sz, digit_w, digit_h, x, icon_y, name_y;
    unsigned    settle, elapsed;
    float       alpha, base;

    if (wb.selected < 0)
        return;

    count = 0;
    for (i = 0; i < WB_NUM_SLOTS; i++)
        if (SCR_WeaponBarCarried(wb.order[i]))
            list[count++] = wb.order[i];
    if (!count)
        return;

    // fade out over the hold, once the weapon has actually been raised
    settle = cl_weaponbar_time->value * 1000;
    elapsed = cls.realtime - wb.input_time;
    alpha = 1.0f;
    if (elapsed > settle) {
        unsigned hold = cl_weaponbar_hold->value * 1000;
        if (hold)
            alpha = 1.0f - (float)(elapsed - settle) / hold;
    }
    alpha = max(0.0f, min(1.0f, alpha));
    if (alpha <= 0.0f)
        return;

    base = Cvar_ClampValue(scr_alpha, 0, 1);
    R_SetAlpha(alpha * base);

    // the icon sets the pitch and the count is scaled to fit three digits
    // across it; with every weapon carried the row still has to cross the HUD,
    // so give up icon size before it runs off the edge
    slot_w = WB_ICON_SIZE + 8;
    if (count * slot_w > scr.hud_width - 16)
        slot_w = (scr.hud_width - 16) / count;
    icon_sz = min(WB_ICON_SIZE, slot_w - 4);
    icon_sz = max(icon_sz, 8);
    digit_w = max(icon_sz * 5 / 16, 4);
    digit_h = max(icon_sz * 9 / 16, 6);

    icon_y = scr.hud_height - 84;
    name_y = icon_y - digit_h - CHAR_HEIGHT - 6;
    x = (scr.hud_width - count * slot_w) / 2;

    HUD_DrawCenterString(scr.hud_width / 2, name_y,
                         cl.configstrings[CS_ITEMS + wb.item[wb.selected]]);

    for (i = 0; i < count; i++, x += slot_w) {
        int slot = list[i];
        int cx = x + slot_w / 2;
        int ammo = wb.ammo[slot] >= 0 ? cl.inventory[wb.ammo[slot]] : -1;

        if (slot == wb.selected)
            SCR_WeaponBarOutline(cx - slot_w / 2, icon_y - 2, slot_w, icon_sz + 4,
                                 MakeColor(255, 255, 0, (int)(alpha * 255)));

        if (ammo >= 0)
            SCR_WeaponBarNumber(cx, icon_y - digit_h - 2, ammo ? 0 : 1, ammo,
                                digit_w, digit_h);

        if (!ammo)
            R_SetAlpha(alpha * 0.5f * base);

        R_DrawStretchPic(cx - icon_sz / 2, icon_y, icon_sz, icon_sz, wb.pic[slot]);

        if (!ammo)
            R_SetAlpha(alpha * base);
    }

    R_SetAlpha(base);
}

static void SCR_WeaponBarInit(void)
{
    wb.selected = -1;
    wb.last_slot = -1;
    wb.commit_slot = -1;
}

static void SCR_WeaponBarCmd(int dir)
{
    if (cl_weaponbar->integer && SCR_WeaponBarStep(dir))
        return;

    // the bar cannot take it - let the game cycle weapons the old way
    if (!CL_ForwardToServer())
        Com_Printf("Can't \"%s\", not connected\n", Cmd_Argv(0));
}

// Forward along the bar, which is the way the rerelease runs it.  The 1997
// game walks its item list backwards for weapnext; matching that here would
// send the highlight left when the wheel says next.
static void SCR_WeapNext_f(void)
{
    SCR_WeaponBarCmd(1);
}

static void SCR_WeapPrev_f(void)
{
    SCR_WeaponBarCmd(-1);
}

#define DISPLAY_ITEMS   17

static void SCR_DrawInventory(void)
{
    int     i;
    int     num, selected_num, item;
    int     index[MAX_ITEMS];
    char    string[MAX_STRING_CHARS];
    int     x, y;
    char    *bind;
    int     selected;
    int     top;

    if (!(cl.frame.ps.stats[STAT_LAYOUTS] & 2))
        return;

    selected = cl.frame.ps.stats[STAT_SELECTED_ITEM];

    num = 0;
    selected_num = 0;
    for (i = 0; i < MAX_ITEMS; i++) {
        if (i == selected) {
            selected_num = num;
        }
        if (cl.inventory[i]) {
            index[num++] = i;
        }
    }

    // determine scroll point
    top = selected_num - DISPLAY_ITEMS / 2;
    if (top > num - DISPLAY_ITEMS) {
        top = num - DISPLAY_ITEMS;
    }
    if (top < 0) {
        top = 0;
    }

    x = (scr.hud_width - 256) / 2;
    y = (scr.hud_height - 240) / 2;

    R_DrawPic(x, y + 8, scr.inven_pic);
    y += 24;
    x += 24;

    HUD_DrawString(x, y, "hotkey ### item");
    y += CHAR_HEIGHT;

    HUD_DrawString(x, y, "------ --- ----");
    y += CHAR_HEIGHT;

    for (i = top; i < num && i < top + DISPLAY_ITEMS; i++) {
        item = index[i];
        // search for a binding
        Q_concat(string, sizeof(string), "use ", cl.configstrings[CS_ITEMS + item]);
        bind = Key_GetBinding(string);

        Q_snprintf(string, sizeof(string), "%6s %3i %s",
                   bind, cl.inventory[item], cl.configstrings[CS_ITEMS + item]);

        if (item != selected) {
            HUD_DrawAltString(x, y, string);
        } else {    // draw a blinky cursor by the selected item
            HUD_DrawString(x, y, string);
            if ((cls.realtime >> 8) & 1) {
                R_DrawChar(x - CHAR_WIDTH, y, 0, 15, scr.font_pic);
            }
        }

        y += CHAR_HEIGHT;
    }
}

static void SCR_DrawSelectedItemName(int x, int y, int item)
{
    static int display_item = -1;
    static int display_start_time = 0;

    float duration = 0.f;
    if (display_item != item)
    {
        display_start_time = Sys_Milliseconds();
        display_item = item;
    }
    else
    {
        duration = (float)(Sys_Milliseconds() - display_start_time) * 0.001f;
    }

    float alpha;
    if (scr_showitemname->integer < 2)
        alpha = max(0.f, min(1.f, 5.f - 4.f * duration)); // show and hide
    else
        alpha = 1; // always show

    if (alpha > 0.f)
    {
        R_SetAlpha(alpha * scr_alpha->value);

        int index = CS_ITEMS + item;
        HUD_DrawString(x, y, cl.configstrings[index]);

        R_SetAlpha(scr_alpha->value);
    }
}

static void SCR_ExecuteLayoutString(const char *s)
{
    char    buffer[MAX_QPATH];
    int     x, y;
    int     value;
    char    *token;
    int     width;
    int     index;
    clientinfo_t    *ci;

    if (!s[0])
        return;

    x = 0;
    y = 0;

    while (s) {
        token = COM_Parse(&s);
        if (token[2] == 0) {
            if (token[0] == 'x') {
                if (token[1] == 'l') {
                    token = COM_Parse(&s);
                    x = atoi(token);
                    continue;
                }

                if (token[1] == 'r') {
                    token = COM_Parse(&s);
                    x = scr.hud_width + atoi(token);
                    continue;
                }

                if (token[1] == 'v') {
                    token = COM_Parse(&s);
                    x = scr.hud_width / 2 - 160 + atoi(token);
                    continue;
                }
            }

            if (token[0] == 'y') {
                if (token[1] == 't') {
                    token = COM_Parse(&s);
                    y = atoi(token);
                    continue;
                }

                if (token[1] == 'b') {
                    token = COM_Parse(&s);
                    y = scr.hud_height + atoi(token);
                    continue;
                }

                if (token[1] == 'v') {
                    token = COM_Parse(&s);
                    y = scr.hud_height / 2 - 120 + atoi(token);
                    continue;
                }
            }
        }

        if (!strcmp(token, "pic")) {
            // draw a pic from a stat number
            token = COM_Parse(&s);
            value = atoi(token);
            if (value < 0 || value >= MAX_STATS) {
                Com_Error(ERR_DROP, "%s: invalid stat index", __func__);
            }
            index = cl.frame.ps.stats[value];
            if (index < 0 || index >= MAX_IMAGES) {
                Com_Error(ERR_DROP, "%s: invalid pic index", __func__);
            }
            token = cl.configstrings[CS_IMAGES + index];
            if (token[0] && cl.image_precache[index]) {
                R_DrawPic(x, y, cl.image_precache[index]);
            }

            if (value == STAT_SELECTED_ICON && scr_showitemname->integer)
            {
                SCR_DrawSelectedItemName(x + 32, y + 8, cl.frame.ps.stats[STAT_SELECTED_ITEM]);
            }
            continue;
        }

        if (!strcmp(token, "client")) {
            // draw a deathmatch client block
            int     score, ping, time;

            token = COM_Parse(&s);
            x = scr.hud_width / 2 - 160 + atoi(token);
            token = COM_Parse(&s);
            y = scr.hud_height / 2 - 120 + atoi(token);

            token = COM_Parse(&s);
            value = atoi(token);
            if (value < 0 || value >= MAX_CLIENTS) {
                Com_Error(ERR_DROP, "%s: invalid client index", __func__);
            }
            ci = &cl.clientinfo[value];

            token = COM_Parse(&s);
            score = atoi(token);

            token = COM_Parse(&s);
            ping = atoi(token);

            token = COM_Parse(&s);
            time = atoi(token);

            HUD_DrawAltString(x + 32, y, ci->name);
            HUD_DrawString(x + 32, y + CHAR_HEIGHT, "Score: ");
            Q_snprintf(buffer, sizeof(buffer), "%i", score);
            HUD_DrawAltString(x + 32 + 7 * CHAR_WIDTH, y + CHAR_HEIGHT, buffer);
            Q_snprintf(buffer, sizeof(buffer), "Ping:  %i", ping);
            HUD_DrawString(x + 32, y + 2 * CHAR_HEIGHT, buffer);
            Q_snprintf(buffer, sizeof(buffer), "Time:  %i", time);
            HUD_DrawString(x + 32, y + 3 * CHAR_HEIGHT, buffer);

            if (!ci->icon) {
                ci = &cl.baseclientinfo;
            }
            R_DrawPic(x, y, ci->icon);
            continue;
        }

        if (!strcmp(token, "ctf")) {
            // draw a ctf client block
            int     score, ping;

            token = COM_Parse(&s);
            x = scr.hud_width / 2 - 160 + atoi(token);
            token = COM_Parse(&s);
            y = scr.hud_height / 2 - 120 + atoi(token);

            token = COM_Parse(&s);
            value = atoi(token);
            if (value < 0 || value >= MAX_CLIENTS) {
                Com_Error(ERR_DROP, "%s: invalid client index", __func__);
            }
            ci = &cl.clientinfo[value];

            token = COM_Parse(&s);
            score = atoi(token);

            token = COM_Parse(&s);
            ping = atoi(token);
            if (ping > 999)
                ping = 999;

            Q_snprintf(buffer, sizeof(buffer), "%3d %3d %-12.12s",
                       score, ping, ci->name);
            if (value == cl.frame.clientNum) {
                HUD_DrawAltString(x, y, buffer);
            } else {
                HUD_DrawString(x, y, buffer);
            }
            continue;
        }

        if (!strcmp(token, "picn")) {
            // draw a pic from a name
            token = COM_Parse(&s);
            R_DrawPic(x, y, R_RegisterPic2(token));
            continue;
        }

        if (!strcmp(token, "num")) {
            // draw a number
            token = COM_Parse(&s);
            width = atoi(token);
            token = COM_Parse(&s);
            value = atoi(token);
            if (value < 0 || value >= MAX_STATS) {
                Com_Error(ERR_DROP, "%s: invalid stat index", __func__);
            }
            value = cl.frame.ps.stats[value];
            HUD_DrawNumber(x, y, 0, width, value);
            continue;
        }

        if (!strcmp(token, "hnum")) {
            // health number
            int     color;

            width = 3;
            value = cl.frame.ps.stats[STAT_HEALTH];
            if (value > 25)
                color = 0;  // green
            else if (value > 0)
                color = ((cl.frame.number / CL_FRAMEDIV) >> 2) & 1;     // flash
            else
                color = 1;

            if (cl.frame.ps.stats[STAT_FLASHES] & 1)
                R_DrawPic(x, y, scr.field_pic);

            HUD_DrawNumber(x, y, color, width, value);
            continue;
        }

        if (!strcmp(token, "anum")) {
            // ammo number
            int     color;

            width = 3;
            value = cl.frame.ps.stats[STAT_AMMO];
            if (value > 5)
                color = 0;  // green
            else if (value >= 0)
                color = ((cl.frame.number / CL_FRAMEDIV) >> 2) & 1;     // flash
            else
                continue;   // negative number = don't show

            if (cl.frame.ps.stats[STAT_FLASHES] & 4)
                R_DrawPic(x, y, scr.field_pic);

            HUD_DrawNumber(x, y, color, width, value);
            continue;
        }

        if (!strcmp(token, "rnum")) {
            // armor number
            int     color;

            width = 3;
            value = cl.frame.ps.stats[STAT_ARMOR];
            if (value < 1)
                continue;

            color = 0;  // green

            if (cl.frame.ps.stats[STAT_FLASHES] & 2)
                R_DrawPic(x, y, scr.field_pic);

            HUD_DrawNumber(x, y, color, width, value);
            continue;
        }

        if (!strcmp(token, "stat_string")) {
            token = COM_Parse(&s);
            index = atoi(token);
            if (index < 0 || index >= MAX_STATS) {
                Com_Error(ERR_DROP, "%s: invalid stat index", __func__);
            }
            index = cl.frame.ps.stats[index];
            if (index < 0 || index >= MAX_CONFIGSTRINGS) {
                Com_Error(ERR_DROP, "%s: invalid string index", __func__);
            }
            HUD_DrawString(x, y, cl.configstrings[index]);
            continue;
        }

        if (!strcmp(token, "cstring")) {
            token = COM_Parse(&s);
            HUD_DrawCenterString(x + 320 / 2, y, token);
            continue;
        }

        if (!strcmp(token, "cstring2")) {
            token = COM_Parse(&s);
            HUD_DrawAltCenterString(x + 320 / 2, y, token);
            continue;
        }

        if (!strcmp(token, "string")) {
            token = COM_Parse(&s);
            HUD_DrawString(x, y, token);
            continue;
        }

        if (!strcmp(token, "string2")) {
            token = COM_Parse(&s);
            HUD_DrawAltString(x, y, token);
            continue;
        }

        if (!strcmp(token, "if")) {
            token = COM_Parse(&s);
            value = atoi(token);
            if (value < 0 || value >= MAX_STATS) {
                Com_Error(ERR_DROP, "%s: invalid stat index", __func__);
            }
            value = cl.frame.ps.stats[value];
            if (!value) {   // skip to endif
                while (strcmp(token, "endif")) {
                    token = COM_Parse(&s);
                    if (!s) {
                        break;
                    }
                }
            }
            continue;
        }

        if (!strcmp(token, "color")) {
            color_t     color;

            token = COM_Parse(&s);
            if (SCR_ParseColor(token, &color)) {
                color.u8[3] *= scr_alpha->value;
                R_SetColor(color.u32);
            }
            continue;
        }
    }

    R_ClearColor();
    R_SetAlpha(scr_alpha->value);
}

//=============================================================================

static void SCR_DrawPause(void)
{
    int x, y;

    if (!sv_paused->integer)
        return;
    if (!cl_paused->integer)
        return;
    if (scr_showpause->integer != 1)
        return;

    x = (scr.hud_width - scr.pause_width) / 2;
    y = (scr.hud_height - scr.pause_height) / 2;

    R_DrawPic(x, y, scr.pause_pic);
}

static void SCR_DrawLoading(void)
{
    int x, y;

    if (!scr.draw_loading)
        return;

    scr.draw_loading = false;

    R_SetScale(scr.hud_scale);

    x = (r_config.width * scr.hud_scale - scr.loading_width) / 2;
    y = (r_config.height * scr.hud_scale - scr.loading_height) / 2;

    R_DrawPic(x, y, scr.loading_pic);

    R_SetScale(1.0f);
}

/*
=================
SCR_ProjectPoint

World point -> hud coordinates. Returns false if the point is behind the
camera and `clamp_edge` is not set; with it, a point behind you is pushed hard
off the edge so the caller's clamp turns it into a direction indicator.
=================
*/
static bool SCR_ProjectPoint(const vec3_t world, bool clamp_edge,
                             float *out_x, float *out_y, float *out_dist)
{
    vec3_t  dir;
    float   fwd, right, up, tx, ty;

    VectorSubtract(world, cl.refdef.vieworg, dir);
    *out_dist = VectorLength(dir);
    if (*out_dist < 1)
        return false;

    fwd   = DotProduct(dir, cl.v_forward);
    right = DotProduct(dir, cl.v_right);
    up    = DotProduct(dir, cl.v_up);

    if (fwd > 1) {
        tx = right / fwd / tanf(DEG2RAD(cl.refdef.fov_x) * 0.5f);
        ty = -up / fwd / tanf(DEG2RAD(cl.refdef.fov_y) * 0.5f);
    } else {
        float len;

        if (!clamp_edge)
            return false;       // behind us and nobody wants an arrow

        len = sqrtf(right * right + up * up);
        if (len < 0.001f)
            return false;
        tx = (right / len) * 100.0f;
        ty = (-up / len) * 100.0f;
    }

    *out_x = scr.hud_width / 2 + tx * (scr.hud_width / 2);
    *out_y = scr.hud_height / 2 + ty * (scr.hud_height / 2);
    return true;
}

/*
=================
SCR_DrawCompassTrail

The breadcrumb path to the objective, drawn small and only where it is
actually in front of you - unlike the objective marker itself, a trail point
clamped to the screen edge would just be noise.
=================
*/
static void SCR_DrawCompassTrail(void)
{
    int i, w, h;

    if (!cl.poi_path_count || !cl.poi_pic)
        return;

    R_GetPicSize(&w, &h, cl.poi_pic);
    if (w <= 0 || h <= 0)
        return;

    // the trail markers are deliberately a third the size of the objective
    w = max(4, w / 3);
    h = max(4, h / 3);

    for (i = 0; i < cl.poi_path_count && i < MAX_POI_PATH; i++) {
        float sx, sy, dist;

        if (!SCR_ProjectPoint(cl.poi_path[i], false, &sx, &sy, &dist))
            continue;
        if (sx < 0 || sy < 0 || sx > scr.hud_width || sy > scr.hud_height)
            continue;

        R_DrawStretchPic((int)sx - w / 2, (int)sy - h / 2, w, h, cl.poi_pic);
    }
}

/*
=================
SCR_DrawCompassPOI

The rerelease compass marker. cl.poi_* is set by TE_POI when the player uses
item_compass, and lasts until cl.poi_time.

The marker is a world point drawn in screen space: project it through the view
basis, and when it is behind you or off the edge, clamp it to the border so it
still reads as "the objective is that way". Distance in metres-ish (Quake units
/ 32) goes under it, which is what tells you whether you are getting closer.

Deliberately NOT a depth-tested 3D sprite: the objective is usually through a
wall, and the whole point is to see it anyway.
=================
*/
static void SCR_DrawCompassPOI(void)
{
    float   sx, sy, dist;
    int     w, h, margin;
    char    buf[16];

    if (!cl.poi_time || cl.time >= cl.poi_time)
        return;

    if (!cl.poi_pic)
        return;

    if (!SCR_ProjectPoint(cl.poi_origin, true, &sx, &sy, &dist))
        return;

    R_GetPicSize(&w, &h, cl.poi_pic);
    if (w <= 0 || h <= 0) {
        w = 16;
        h = 16;
    }

    margin = max(w, h);

    // clamp() is the in-place macro in shared.h, not a returning function
    clamp(sx, (float)margin, (float)(scr.hud_width - margin));
    clamp(sy, (float)margin, (float)(scr.hud_height - margin));

    R_DrawStretchPic((int)sx - w / 2, (int)sy - h / 2, w, h, cl.poi_pic);

    Q_snprintf(buf, sizeof(buf), "%d", (int)(dist / 32.0f));
    SCR_DrawString((int)sx - (int)strlen(buf) * CHAR_WIDTH / 2,
                   (int)sy + h / 2 + 2, UI_DROPSHADOW, buf);
}

static void SCR_DrawCrosshair(void)
{
    int x, y;

    if (!scr_crosshair->integer)
        return;

    x = (scr.hud_width - scr.crosshair_width) / 2;
    y = (scr.hud_height - scr.crosshair_height) / 2;

    R_SetColor(scr.crosshair_color.u32);

    R_DrawStretchPic(x + ch_x->integer,
                     y + ch_y->integer,
                     scr.crosshair_width,
                     scr.crosshair_height,
                     scr.crosshair_pic);
}

// The status bar is a small layout program that is based on the stats array
static void SCR_DrawStats(void)
{
    if (scr_draw2d->integer <= 1)
        return;

    SCR_ExecuteLayoutString(cl.configstrings[CS_STATUSBAR]);
}

/*
==============
SCR_DrawHealthBars

[rerelease] target_healthbar. The game packs both bars into one short in
STAT_HEALTH_BARS, a byte each - bit 7 says the bar is showing and bits 0-6 are
the health remaining out of 127 - and puts the boss's (already localized) name
in CS_HEALTH_BAR_NAME. Nothing about it is in the statusbar program, so it is
drawn here rather than as a layout token, and scr_health_bars gates it.
==============
*/
#define HEALTH_BAR_HEIGHT   4

static void SCR_DrawHealthBars(void)
{
    int     stat, i, shown;
    int     x, y, w;
    const char *name;

    if (!scr_health_bars->integer)
        return;
    if (scr_draw2d->integer <= 1)
        return;

    stat = cl.frame.ps.stats[STAT_HEALTH_BARS];
    if (!stat)
        return;

    // nothing to draw unless at least one bar is flagged active
    shown = 0;
    for (i = 0; i < MAX_HEALTH_BARS; i++)
        if ((stat >> (i * 8)) & 0x80)
            shown++;
    if (!shown)
        return;

    w = scr.hud_width / 2;
    x = (scr.hud_width - w) / 2;
    y = scr.hud_height / 8;

    name = cl.configstrings[CS_HEALTH_BAR_NAME];
    if (name[0]) {
        SCR_DrawString(scr.hud_width / 2, y, UI_CENTER, name);
        y += CHAR_HEIGHT + 2;
    }

    for (i = 0; i < MAX_HEALTH_BARS; i++) {
        int  bar = (stat >> (i * 8)) & 0xff;
        int  filled;

        if (!(bar & 0x80))
            continue;

        filled = ((bar & 0x7f) * w) / 127;

        // one pixel of black around the whole bar, then red for what is left
        // and grey for what has been taken off
        R_DrawFill32(x - 1, y - 1, w + 2, HEALTH_BAR_HEIGHT + 2, U32_BLACK);
        if (filled > 0)
            R_DrawFill32(x, y, filled, HEALTH_BAR_HEIGHT, U32_RED);
        if (filled < w)
            R_DrawFill32(x + filled, y, w - filled, HEALTH_BAR_HEIGHT,
                         MakeColor(80, 80, 80, 255));

        y += HEALTH_BAR_HEIGHT * 3;
    }
}

static void SCR_DrawLayout(void)
{
    if (scr_draw2d->integer == 3 && !Key_IsDown(K_F1))
        return;     // turn off for GTV

    if (cls.demo.playback && Key_IsDown(K_F1))
        goto draw;

    if (!(cl.frame.ps.stats[STAT_LAYOUTS] & 1))
        return;

draw:
    SCR_ExecuteLayoutString(cl.layout);
}

static void SCR_Draw2D(void)
{
	if (scr_draw2d->integer <= 0)
		return;     // turn off for screenshots

	if (cls.key_dest & KEY_MENU)
		return;

	R_SetAlphaScale(scr.hud_alpha);

    R_SetScale(scr.hud_scale);

    scr.hud_height *= scr.hud_scale;
    scr.hud_width *= scr.hud_scale;

    // crosshair has its own color and alpha
    SCR_DrawCrosshair();

    // the rest of 2D elements share common alpha
    R_ClearColor();
    R_SetAlpha(Cvar_ClampValue(scr_alpha, 0, 1));

    SCR_DrawStats();

    SCR_DrawCompassTrail();
    SCR_DrawCompassPOI();

    SCR_DrawHealthBars();

    SCR_DrawLayout();

    SCR_DrawInventory();

    SCR_DrawWeaponBar();

    SCR_DrawCenterString();

    SCR_DrawNet();

    SCR_DrawObjects();

	SCR_DrawFPS();

    SCR_DrawChatHUD();

    SCR_DrawTurtle();

    SCR_DrawPause();

    // debug stats have no alpha
    R_ClearColor();

#if USE_DEBUG
    SCR_DrawDebugStats();
    SCR_DrawDebugPmove();
#endif

    R_SetScale(1.0f);
	R_SetAlphaScale(1.0f);
}

static void SCR_DrawActive(int waterLevel)
{
    // if full screen menu is up, do nothing at all
    if (!UI_IsTransparent())
        return;

    // draw black background if not active
    if (cls.state < ca_active) {
        R_DrawFill8(0, 0, r_config.width, r_config.height, 0);
        return;
    }

    if (cls.state == ca_cinematic) {
        if (cl.image_precache[0]) 
        {
            // scale the image to touch the screen from inside, keeping the aspect ratio

            image_t* image = IMG_ForHandle(cl.image_precache[0]);

            float zoomx = (float)r_config.width / (float)image->width;
            float zoomy = (float)r_config.height / (float)image->height;
            float zoom = min(zoomx, zoomy);

            int w = (int)(image->width * zoom);
            int h = (int)(image->height * zoom);
            int x = (r_config.width - w) / 2;
            int y = (r_config.height - h) / 2;

            R_DrawFill8(0, 0, r_config.width, r_config.height, 0);
            R_DrawStretchPic(x, y, w, h, cl.image_precache[0]);
        }
        return;
    }

    // start with full screen HUD
    scr.hud_height = r_config.height;
    scr.hud_width = r_config.width;

    SCR_DrawDemo();

    SCR_CalcVrect();

    // clear any dirty part of the background
    SCR_TileClear();

    // draw 3D game view
    V_RenderView(/*waterLevel*/);

    // draw all 2D elements
    SCR_Draw2D();
}

//=======================================================

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen(int waterLevel)
{
    static int recursive;

    if (!scr.initialized) {
        return;             // not initialized yet
    }

    // if the screen is disabled (loading plaque is up), do nothing at all
    if (cls.disable_screen) {
        unsigned delta = Sys_Milliseconds() - cls.disable_screen;

        if (delta < 120 * 1000) {
            return;
        }

        cls.disable_screen = 0;
        Com_Printf("Loading plaque timed out.\n");
    }

    if (recursive > 1) {
        Com_Error(ERR_FATAL, "%s: recursively called", __func__);
    }

    recursive++;

    SCR_WeaponBarThink();

    R_BeginFrame();

    // do 3D refresh drawing
    SCR_DrawActive(waterLevel);

    // draw main menu
    UI_Draw(cls.realtime);

    // draw console
    Con_DrawConsole();

    // draw loading plaque
    SCR_DrawLoading();

    R_EndFrame();

    recursive--;
}

qhandle_t SCR_GetFont(void)
{
	return scr.font_pic;
}

void SCR_SetHudAlpha(float alpha)
{
	scr.hud_alpha = alpha;
}
