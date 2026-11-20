/*
Copyright (C) 2003-2006 Andrey Nazarov

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

#include "ui.h"
#include "client/input.h"
#include "../client.h"
#include "common/prompt.h"
#include "shared/bot_roster.h"
#include "format/bsp.h"
#include "format/bsp3.h"

uiStatic_t    uis;

LIST_DECL(ui_menus);

cvar_t    *ui_debug;
static cvar_t    *ui_open;
static cvar_t    *ui_scale;

// ===========================================================================

/*
=================
UI_PushMenu
=================
*/
void UI_PushMenu(menuFrameWork_t *menu)
{
    int i, j;

    if (!menu) {
        return;
    }

    // if this menu is already present, drop back to that level
    // to avoid stacking menus by hotkeys
    for (i = 0; i < uis.menuDepth; i++) {
        if (uis.layers[i] == menu) {
            break;
        }
    }

    if (i == uis.menuDepth) {
        if (uis.menuDepth >= MAX_MENU_DEPTH) {
            Com_EPrintf("UI_PushMenu: MAX_MENU_DEPTH exceeded\n");
            return;
        }
        uis.layers[uis.menuDepth++] = menu;
    } else {
        for (j = i; j < uis.menuDepth; j++) {
            UI_PopMenu();
        }
        uis.menuDepth = i + 1;
    }

    if (menu->push && !menu->push(menu)) {
        uis.menuDepth--;
        return;
    }

    Menu_Init(menu);

    Key_SetDest((Key_GetDest() & ~KEY_CONSOLE) | KEY_MENU);

    Con_Close(true);

    if (!uis.activeMenu) {
        // opening menu moves cursor to the nice location
        IN_WarpMouse(menu->mins[0] / uis.scale, menu->mins[1] / uis.scale);

        uis.mouseCoords[0] = menu->mins[0];
        uis.mouseCoords[1] = menu->mins[1];

        uis.entersound = true;
    }

    uis.activeMenu = menu;

    // No hit test here either (see UI_PopMenu): a menu keeps its highlight between
    // visits, and the pointer left over from the menu that opened it would move it.

    if (menu->expose) {
        menu->expose(menu);
    }
}

static void UI_Resize(void)
{
    int i;

    uis.scale = R_ClampScale(ui_scale);
    uis.width = r_config.width * uis.scale;
    uis.height = r_config.height * uis.scale;

    for (i = 0; i < uis.menuDepth; i++) {
        Menu_Init(uis.layers[i]);
    }

    //CL_WarpMouse(0, 0);
}


/*
=================
UI_ForceMenuOff
=================
*/
void UI_ForceMenuOff(void)
{
    menuFrameWork_t *menu;
    int i;

    for (i = 0; i < uis.menuDepth; i++) {
        menu = uis.layers[i];
        if (menu->pop) {
            menu->pop(menu);
        }
    }

    Key_SetDest(Key_GetDest() & ~KEY_MENU);
    uis.menuDepth = 0;
    uis.activeMenu = NULL;
    uis.mouseTracker = NULL;
    uis.transparent = false;
}

/*
=================
UI_PopMenu
=================
*/
void UI_PopMenu(void)
{
    menuFrameWork_t *menu;

    Q_assert(uis.menuDepth > 0);

    menu = uis.layers[--uis.menuDepth];
    if (menu->pop) {
        menu->pop(menu);
    }

    if (!uis.menuDepth) {
        UI_ForceMenuOff();

		// Save the config file if the user closes the menu while in-game
		if (cls.state >= ca_active) {
			CL_WriteConfig();
		}

        return;
    }

    uis.activeMenu = uis.layers[uis.menuDepth - 1];
    uis.mouseTracker = NULL;

    // No hit test here: the pointer is still wherever it was in the submenu, so
    // one would move the parent's highlight to whichever item happens to sit at
    // that spot instead of the one that opened the submenu. The next real mouse
    // move or click hit-tests as usual.
}

/*
=================
UI_IsTransparent
=================
*/
bool UI_IsTransparent(void)
{
    if (!(Key_GetDest() & KEY_MENU)) {
        return true;
    }

    if (!uis.activeMenu) {
        return true;
    }

    return uis.activeMenu->transparent;
}

menuFrameWork_t *UI_FindMenu(const char *name)
{
    menuFrameWork_t *menu;

    LIST_FOR_EACH(menuFrameWork_t, menu, &ui_menus, entry) {
        if (!strcmp(menu->name, name)) {
            return menu;
        }
    }

    return NULL;
}

/*
=================
UI_OpenMenu
=================
*/
void UI_OpenMenu(uiMenu_t type)
{
    menuFrameWork_t *menu = NULL;

    if (!uis.initialized) {
        return;
    }

    // close any existing menus
    UI_ForceMenuOff();

    switch (type) {
    case UIMENU_DEFAULT:
        if (ui_open->integer) {
            menu = UI_FindMenu("main");
        }
        break;
    case UIMENU_MAIN:
        menu = UI_FindMenu("main");
        break;
    case UIMENU_GAME:
        menu = UI_FindMenu("game");
        if (!menu) {
            menu = UI_FindMenu("main");
        }
        break;
    case UIMENU_NONE:
        break;
    default:
        Q_assert(!"bad menu");
    }

    UI_PushMenu(menu);
}

//=============================================================================

/*
=================
UI_FormatColumns
=================
*/
void *UI_FormatColumns(int extrasize, ...)
{
    va_list argptr;
    char *buffer, *p;
    int i, j;
    size_t total = 0;
    char *strings[MAX_COLUMNS];
    size_t lengths[MAX_COLUMNS];

    va_start(argptr, extrasize);
    for (i = 0; i < MAX_COLUMNS; i++) {
        if ((p = va_arg(argptr, char *)) == NULL) {
            break;
        }
        strings[i] = p;
        total += lengths[i] = strlen(p) + 1;
    }
    va_end(argptr);

    buffer = UI_Malloc(extrasize + total + 1);
    p = buffer + extrasize;
    for (j = 0; j < i; j++) {
        memcpy(p, strings[j], lengths[j]);
        p += lengths[j];
    }
    *p = 0;

    return buffer;
}

char *UI_GetColumn(char *s, int n)
{
    int i;

    for (i = 0; i < n && *s; i++) {
        s += strlen(s) + 1;
    }

    return s;
}

/*
=================
UI_CursorInRect
=================
*/
/*
=================
UI_Levelshot

A map's picture for the menus: levelshots/<map>.jpg - Quake III's own, in its
paks, or one `levelshots` made (client/levelshot.c); the image loader tries
the other formats too. With none, and `unknown` set, the game's own "unknown
map" picture: Quake III's for a Quake III map (IBSP 46), when it is
installed, and for every other map Quake II's logo on black
(baseq2/pics/unknownmap_q2.jpg, made from the rerelease's vault/logo.png -
Matt, 11-20: "show the quake2 logo instead of the quake 3 logo"); 0 when
that picture is not there. Misses are remembered with which game the map is
from, so a menu drawn every frame does not go to the disk for them;
UI_LevelshotForget clears them as a menu opens, so new levelshots show up.
=================
*/
#define UI_LEVELSHOT_MISSES 64

static struct {
    char    map[MAX_QPATH];
    bool    q3;
} ui_levelshot_misses[UI_LEVELSHOT_MISSES];
static int  ui_levelshot_nummisses;

void UI_LevelshotForget(void)
{
    ui_levelshot_nummisses = 0;
}

// maps/<map>.bsp is a Quake III map, told from its header (IBSP 46)
static bool UI_MapIsQuake3(const char *map)
{
    char        path[MAX_QPATH];
    qhandle_t   f = 0;
    uint32_t    header[2] = { 0, 0 };
    int64_t     len;
    bool        q3;

    if (Q_snprintf(path, sizeof(path), "maps/%s.bsp", map) >= sizeof(path))
        return false;
    len = FS_OpenFile(path, &f, FS_MODE_READ);
    if (!f)
        return false;
    q3 = len >= (int64_t)sizeof(header) &&
         FS_Read(header, sizeof(header), f) == (int)sizeof(header) &&
         LittleLong(header[0]) == IDBSPHEADER && LittleLong(header[1]) == Q3_BSPVERSION;
    FS_CloseFile(f);
    return q3;
}

qhandle_t UI_Levelshot(const char *map, bool unknown)
{
    char        path[MAX_QPATH];
    qhandle_t   pic = 0;
    bool        q3 = false;
    int         i;

    if (map && *map) {
        for (i = 0; i < ui_levelshot_nummisses; i++)
            if (!Q_stricmp(ui_levelshot_misses[i].map, map))
                break;
        if (i < ui_levelshot_nummisses) {
            q3 = ui_levelshot_misses[i].q3;
        } else if (Q_snprintf(path, sizeof(path), "/levelshots/%s.jpg", map) < sizeof(path)) {
            // filtered (IF_BILERP): a 640x480 or Quake III's 256x256 shot,
            // stretched, shows every texel when sampled nearest
            pic = R_RegisterImage(path, IT_PIC, IF_SRGB | IF_BILERP, NULL);
            if (!pic) {
                q3 = UI_MapIsQuake3(map);
                if (ui_levelshot_nummisses < UI_LEVELSHOT_MISSES) {
                    Q_strlcpy(ui_levelshot_misses[ui_levelshot_nummisses].map, map, MAX_QPATH);
                    ui_levelshot_misses[ui_levelshot_nummisses++].q3 = q3;
                }
            }
        }
    }

    if (!pic && unknown)
        pic = R_RegisterImage(q3 ? "/menu/art/unknownmap.jpg" : "/pics/unknownmap_q2.jpg",
                              IT_PIC, IF_SRGB | IF_BILERP, NULL);
    return pic;
}

bool UI_CursorInRect(vrect_t *rect)
{
    if (uis.mouseCoords[0] < rect->x) {
        return false;
    }
    if (uis.mouseCoords[0] >= rect->x + rect->width) {
        return false;
    }
    if (uis.mouseCoords[1] < rect->y) {
        return false;
    }
    if (uis.mouseCoords[1] >= rect->y + rect->height) {
        return false;
    }
    return true;
}

void UI_DrawString(int x, int y, int flags, const char *string)
{
    if ((flags & UI_CENTER) == UI_CENTER) {
        x -= strlen(string) * CHAR_WIDTH / 2;
    } else if (flags & UI_RIGHT) {
        x -= strlen(string) * CHAR_WIDTH;
    }

    R_DrawString(x, y, flags, MAX_STRING_CHARS, string, uis.fontHandle);
}

void UI_DrawChar(int x, int y, int flags, int ch)
{
    R_DrawChar(x, y, flags, ch, uis.fontHandle);
}

void UI_StringDimensions(vrect_t *rc, int flags, const char *string)
{
    rc->height = CHAR_HEIGHT;
    rc->width = CHAR_WIDTH * strlen(string);

    if ((flags & UI_CENTER) == UI_CENTER) {
        rc->x -= rc->width / 2;
    } else if (flags & UI_RIGHT) {
        rc->x -= rc->width;
    }
}

void UI_DrawRect8(const vrect_t *rc, int border, int c)
{
    R_DrawFill8(rc->x, rc->y, border, rc->height, c);   // left
    R_DrawFill8(rc->x + rc->width - border, rc->y, border, rc->height, c);   // right
    R_DrawFill8(rc->x + border, rc->y, rc->width - border * 2, border, c);   // top
    R_DrawFill8(rc->x + border, rc->y + rc->height - border, rc->width - border * 2, border, c);   // bottom
}

#if 0
void UI_DrawRect32(const vrect_t *rc, int border, uint32_t color)
{
    R_DrawFill32(rc->x, rc->y, border, rc->height, color);   // left
    R_DrawFill32(rc->x + rc->width - border, rc->y, border, rc->height, color);   // right
    R_DrawFill32(rc->x + border, rc->y, rc->width - border * 2, border, color);   // top
    R_DrawFill32(rc->x + border, rc->y + rc->height - border, rc->width - border * 2, border, color);   // bottom
}
#endif

//=============================================================================
/* Menu Subsystem */

/*
=================
UI_DoHitTest
=================
*/
bool UI_DoHitTest(void)
{
    menuCommon_t *item;

    if (!uis.activeMenu) {
        return false;
    }

    if (uis.mouseTracker) {
        item = uis.mouseTracker;
    } else {
        if (!(item = Menu_HitTest(uis.activeMenu))) {
            return false;
        }
    }

    if (!UI_IsItemSelectable(item)) {
        return false;
    }

    Menu_MouseMove(item);

    if (item->flags & QMF_HASFOCUS) {
        return false;
    }

    Menu_SetFocus(item);

    return true;
}

/*
=================
UI_MouseEvent
=================
*/
void UI_MouseEvent(int x, int y)
{
    clamp(x, 0, r_config.width - 1);
    clamp(y, 0, r_config.height - 1);

    uis.mouseCoords[0] = Q_rint(x * uis.scale);
    uis.mouseCoords[1] = Q_rint(y * uis.scale);

    UI_DoHitTest();
}

/*
=================
UI_Draw
=================
*/
void UI_Draw(int realtime)
{
    int i;

    uis.realtime = realtime;

    if (!(Key_GetDest() & KEY_MENU)) {
        return;
    }

    if (!uis.activeMenu) {
        return;
    }

    R_ClearColor();
    R_SetScale(uis.scale);

    if (1) {
        // draw top menu
        if (uis.activeMenu->draw) {
            uis.activeMenu->draw(uis.activeMenu);
        } else {
            Menu_Draw(uis.activeMenu);
        }
    } else {
        // draw all layers
        for (i = 0; i < uis.menuDepth; i++) {
            if (uis.layers[i]->draw) {
                uis.layers[i]->draw(uis.layers[i]);
            } else {
                Menu_Draw(uis.layers[i]);
            }
        }
    }

    // a page's draw can close the menus - the postgame's does, as a tier
    // done hands over to its video (q3sp.c SP_Post_TierVideo) - and the
    // cursor below read the menu that was gone (fullscreen only)
    if (!uis.activeMenu) {
        R_ClearColor();
        R_SetScale(1.0f);
        return;
    }

    // draw custom cursor in fullscreen mode - Quake III's on its pages (q3sp.c)
    if (r_config.flags & QVF_FULLSCREEN) {
        if (uis.activeMenu->q3_cursor) {
            UI_SP_DrawCursor();
        } else {
            R_DrawPic(uis.mouseCoords[0] - uis.cursorWidth / 2,
                      uis.mouseCoords[1] - uis.cursorHeight / 2, uis.cursorHandle);
        }
    }

    if (ui_debug->integer) {
        UI_DrawString(uis.width - 4, 4, UI_RIGHT,
                      va("%3i %3i", uis.mouseCoords[0], uis.mouseCoords[1]));
    }

    // delay playing the enter sound until after the
    // menu has been drawn, to avoid delay while
    // caching images
    if (uis.entersound) {
        uis.entersound = false;
        S_StartLocalSound("misc/menu1.wav");
    }

    R_ClearColor();
    R_SetScale(1.0f);
}

void UI_StartSound(menuSound_t sound)
{
    switch (sound) {
    case QMS_IN:
        S_StartLocalSound("misc/menu1.wav");
        break;
    case QMS_MOVE:
        S_StartLocalSound("misc/menu2.wav");
        break;
    case QMS_OUT:
        S_StartLocalSound("misc/menu3.wav");
        break;
    case QMS_BEEP:
        S_StartLocalSound("misc/talk1.wav");
        break;
    default:
        break;
    }
}

/*
=================
UI_KeyEvent
=================
*/
void UI_KeyEvent(int key, bool down)
{
    menuSound_t sound;

    if (!uis.activeMenu) {
        return;
    }

    if (!down) {
        if (key == K_MOUSE1) {
            uis.mouseTracker = NULL;
        }
        return;
    }

    // Menu_Keydown only acts on a click if the item under the pointer already has
    // the highlight. Since opening or closing a menu no longer hit-tests, the
    // pointer may not have moved since, so hit-test now.
    if (key == K_MOUSE1 || key == K_MOUSE3) {
        UI_DoHitTest();
    }

    sound = Menu_Keydown(uis.activeMenu, key);

    UI_StartSound(sound);
}

/*
=================
UI_CharEvent
=================
*/
void UI_CharEvent(int key)
{
    menuCommon_t *item;
    menuSound_t sound;

    if (!uis.activeMenu) {
        return;
    }

    if ((item = Menu_ItemAtCursor(uis.activeMenu)) == NULL ||
        (sound = Menu_CharEvent(item, key)) == QMS_NOTHANDLED) {
        return;
    }

    UI_StartSound(sound);
}

static void UI_Menu_g(genctx_t *ctx)
{
    menuFrameWork_t *menu;

    LIST_FOR_EACH(menuFrameWork_t, menu, &ui_menus, entry)
        Prompt_AddMatch(ctx, menu->name);
}

static void UI_PushMenu_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        UI_Menu_g(ctx);
    }
}

static void UI_PushMenu_f(void)
{
    menuFrameWork_t *menu;
    char *s;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s <menu>\n", Cmd_Argv(0));
        return;
    }
    s = Cmd_Argv(1);
    menu = UI_FindMenu(s);
    if (menu) {
        UI_PushMenu(menu);
    } else {
        Com_Printf("No such menu: %s\n", s);
    }
}

static void UI_PopMenu_f(void)
{
    if (uis.activeMenu) {
        UI_PopMenu();
    }
}


static void UI_BotLineup_f(void);

static const cmdreg_t c_ui[] = {
    { "forcemenuoff", UI_ForceMenuOff },
    { "pushmenu", UI_PushMenu_f, UI_PushMenu_c },
    { "popmenu", UI_PopMenu_f },
    { "bot_lineup", UI_BotLineup_f },

    { NULL, NULL }
};

static void ui_scale_changed(cvar_t *self)
{
    UI_Resize();
}

void UI_ModeChanged(void)
{
    ui_scale = Cvar_Get("ui_scale", "0", 0);
    ui_scale->changed = ui_scale_changed;
    UI_Resize();
}

static void UI_FreeMenus(void)
{
    menuFrameWork_t *menu, *next;

    LIST_FOR_EACH_SAFE(menuFrameWork_t, menu, next, &ui_menus, entry) {
        if (menu->free) {
            menu->free(menu);
        }
    }
    List_Init(&ui_menus);
}

/*
=================
UI_BotLineup_f

The Bot Match line-up. bot_skin1..15 are the skins the game puts its bots in
(src/baseq2/g_bot.c), and a bot goes by its skin's name: _ui_botname1..15
hold those names, which the menu shows as each bot's row. The skins are
rolled at random from the installed skins a bot may wear (for now id's own
male, female and cyborg skins - Bot_SkinUsable) - the first time the page
opens each session ("bot_lineup"), again on "bot_lineup shuffle", and for
any slot whose skin this game does not have. The bot picker
(playerconfig.c) sets one by hand, from every installed model.
=================
*/
#define UI_MAX_BOT_SKINS    256

static bool ui_lineup_rolled;

/*
_ui_botlabelN is what the page's row for bot N says: its name, and in a team
game the side it was put on, when it was (bot_teamN 1 red, 2 blue; 0, auto,
the default, leaves the game to even the teams out).
*/
static void UI_BotLabel(int n)
{
    char    label[48];
    int     team = Cvar_VariableInteger(va("bot_team%d", n));

    Cvar_VariableStringBuffer(va("_ui_botname%d", n), label, sizeof(label));
    // the team games only: co-op (3) is everyone on one side
    if (Cvar_VariableInteger("bot_mode") >= 1 && Cvar_VariableInteger("bot_mode") <= 2 &&
        (team == 1 || team == 2))
        Q_strlcat(label, team == 1 ? " (red)" : " (blue)", sizeof(label));
    Cvar_Set(va("_ui_botlabel%d", n), label);
}

static void UI_BotSkinChanged(cvar_t *var)
{
    char    name[32];
    int     n = atoi(var->name + 8);    // after "bot_skin"

    if (n < 1 || n > BOT_MAX_MATCH)
        return;
    Bot_SkinName(var->string, name, sizeof(name));
    Cvar_Set(va("_ui_botname%d", n), name);
    UI_BotLabel(n);
}

static void UI_BotTeamChanged(cvar_t *var)
{
    int n = atoi(var->name + 8);        // after "bot_team"

    if (n >= 1 && n <= BOT_MAX_MATCH)
        UI_BotLabel(n);
}

static void UI_BotModeChanged(cvar_t *var)
{
    int n;

    for (n = 1; n <= BOT_MAX_MATCH; n++)
        UI_BotLabel(n);
}

// every installed skin a bot may wear, as "model/skin": id's Quake II ones,
// Quake III's bots (baseq3) or both, as bot_models says
static int UI_BotSkinsFor(char (*out)[MAX_QPATH], int max, int models)
{
    int i, j, n = 0;

    if (!uis.numPlayerModels)
        PlayerModel_Load();

    for (i = 0; i < uis.numPlayerModels; i++) {
        const playerModelInfo_t *pmi = &uis.pmi[i];

        for (j = 0; j < pmi->nskins && n < max; j++) {
            Q_concat(out[n], MAX_QPATH, pmi->directory, "/", pmi->skindisplaynames[j]);
            if (Bot_SkinInPool(out[n], models))
                n++;
        }
    }

    return n;
}

static int UI_BotSkins(char (*out)[MAX_QPATH], int max)
{
    int n = UI_BotSkinsFor(out, max, Cvar_VariableInteger("bot_models"));

    // Quake III's asked for and not installed: Quake II's
    return n ? n : UI_BotSkinsFor(out, max, BOT_MODELS_Q2);
}

// any installed skin, stock or not - what the bot picker can set
static bool UI_BotSkinInstalled(const char *value)
{
    const char  *s = strchr(value, '/');
    int         i, j;

    if (!s)
        return false;

    for (i = 0; i < uis.numPlayerModels; i++) {
        const playerModelInfo_t *pmi = &uis.pmi[i];

        if (Q_strncasecmp(pmi->directory, value, s - value) || pmi->directory[s - value])
            continue;
        for (j = 0; j < pmi->nskins; j++)
            if (!Q_stricmp(pmi->skindisplaynames[j], s + 1))
                return true;
    }

    return false;
}

// how the line-up is rolled
#define LINEUP_KEEP     0   // keep what was rolled or picked, where it is installed
#define LINEUP_SHUFFLE  1   // all new faces
#define LINEUP_POOL     2   // bot_models changed: new faces for the bots outside it

static void UI_BotLineup(int how)
{
    static char skins[UI_MAX_BOT_SKINS][MAX_QPATH];
    bool        used[UI_MAX_BOT_SKINS] = { false };
    bool        keep[BOT_MAX_MATCH + 1] = { false };
    int         count = UI_BotSkins(skins, UI_MAX_BOT_SKINS);
    int         models = Cvar_VariableInteger("bot_models");
    int         i, k, start, tries, left = count;

    // keep what was rolled or picked, where this game has it
    if (how != LINEUP_SHUFFLE && ui_lineup_rolled) {
        for (i = 1; i <= BOT_MAX_MATCH; i++) {
            cvar_t *var = Cvar_Get(va("bot_skin%d", i), "random", 0);

            if (!UI_BotSkinInstalled(var->string))
                continue;
            if (how == LINEUP_POOL && !Bot_SkinInPool(var->string, models))
                continue;
            keep[i] = true;
            for (k = 0; k < count; k++) {
                if (!Q_stricmp(var->string, skins[k])) {
                    if (!used[k])
                        left--;
                    used[k] = true;
                    break;
                }
            }
        }
    }

    for (i = 1; i <= BOT_MAX_MATCH; i++) {
        cvar_t *var = Cvar_Get(va("bot_skin%d", i), "random", 0);

        if (!keep[i]) {
            if (!count) {
                Cvar_SetByVar(var, "random", FROM_CODE);
            } else {
                // a skin nobody has yet, while there are any
                if (!left) {
                    memset(used, 0, sizeof(used));
                    left = count;
                }
                start = k = Q_rand_uniform(count);
                for (tries = 0; tries < count; tries++) {
                    k = (start + tries) % count;
                    if (!used[k])
                        break;
                }
                used[k] = true;
                left--;
                Cvar_SetByVar(var, skins[k], FROM_CODE);
            }
        }
        UI_BotSkinChanged(var);
    }

    ui_lineup_rolled = true;

    // capture the flag is the rerelease's: its maps and media are in its pak
    if (Cvar_VariableInteger("bot_mode") == 2 && Cvar_VariableInteger("fs_rerelease") != 1)
        Cvar_Set("bot_mode", "0");

    // Quake III Arena's maps only while its paks are there (files.c)
    if (Cvar_VariableInteger("bot_mapset") == 1 && Cvar_VariableInteger("fs_quake3") != 1)
        Cvar_Set("bot_mapset", "0");
}

static void UI_BotLineup_f(void)
{
    UI_BotLineup(!Q_stricmp(Cmd_Argv(1), "shuffle") ? LINEUP_SHUFFLE : LINEUP_KEEP);
}

static void UI_BotModelsChanged(cvar_t *var)
{
    if (ui_lineup_rolled)
        UI_BotLineup(LINEUP_POOL);
}

/*
=================
UI_BotInit

The Bot Match menu's settings, which the game reads when the match starts.
How many bots and each one's skill are archived; the line-up is not, so
each session starts with new faces.
=================
*/
static void UI_BotInit(void)
{
    cvar_t  *mode;
    int     i;

    // the game mode: 0 deathmatch, 1 team deathmatch, 2 capture the flag
    // (rerelease only), 3 co-op - the campaign's maps with the bots on your
    // side (g_bot.c CO-OP). The start action turns it into teamplay, ctf and
    // coop.
    mode = Cvar_Get("bot_mode", "0", CVAR_ARCHIVE);
    mode->changed = UI_BotModeChanged;

    // sides in a team game: 0 auto - the default, the game evens the teams
    // out - 1 red, 2 blue. Yours is kept; the bots' go with this session's
    // line-up, so they start each session on auto.
    Cvar_Get("bot_player_team", "0", CVAR_ARCHIVE);

    Cvar_Get("bot_count", "3", CVAR_ARCHIVE);

    // Start Server's: its own bot count, none unless asked for (g_bot.c,
    // bot_match 2), and its co-op choice - a menu copy, as "coop" is latched
    // and its old value is what a condition or $coop would read
    Cvar_Get("bot_server_count", "0", CVAR_ARCHIVE);
    Cvar_Get("_ui_server_coop", "0", 0);

    // whose models the bots wear - 0 Quake II's, 1 Quake III's, 2 both
    // (bot_roster.h); Quake III's only while baseq3 has them, so the menu
    // shows the choice only then. Both by default: with no Quake III data
    // that is Quake II's.
    Cvar_Get("bot_models", "2", CVAR_ARCHIVE)->changed = UI_BotModelsChanged;

    // "cheats" is latched and not archived, so the menu keeps its own and
    // each start action copies it over - the map ... force applies it
    Cvar_Get("bot_cheats", "0", CVAR_ARCHIVE);

    for (i = 1; i <= BOT_MAX_MATCH; i++) {
        cvar_t *skin = Cvar_Get(va("bot_skin%d", i), "random", 0);
        cvar_t *team = Cvar_Get(va("bot_team%d", i), "0", 0);

        Cvar_Get(va("bot_skill%d", i), "2", CVAR_ARCHIVE);
        Cvar_Get(va("_ui_botname%d", i), "", 0);
        Cvar_Get(va("_ui_botlabel%d", i), "", 0);
        skin->changed = UI_BotSkinChanged;
        team->changed = UI_BotTeamChanged;
        UI_BotSkinChanged(skin);
    }

    // the map for each set of maps the menu offers
    Cvar_Get("bot_map_q2", "q2dm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_q2sp", "base1", CVAR_ARCHIVE);
    // co-op's difficulty, as the campaign's skill menu sets it: 0 easy,
    // 1 medium, 2 hard, 3 nightmare
    Cvar_Get("bot_coop_skill", "1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_mg", "mgdm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_xdm", "xdm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_rdm", "rdm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_q64", "q64/dm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_any", "q2dm1", CVAR_ARCHIVE);
    Cvar_Get("bot_map_ctf", "q2ctf1", CVAR_ARCHIVE);
    // which of those the menu shows: a number, as the menu's ifeq compares
    // numbers - 0 quake ii deathmatch, 1 call of the machine, 2 the reckoning,
    // 3 ground zero, 4 quake ii 64, 5 any map, 6 quake ii campaign
    Cvar_Get("_ui_botpack", "0", 0);

    // Quake III Arena's maps, when baseq3 has them: 0 Quake II's, 1 Quake
    // III's (deathmatch and tourney, or CTF)
    Cvar_Get("bot_mapset", "0", CVAR_ARCHIVE);
    Cvar_Get("bot_map_q3", "q3dm17", CVAR_ARCHIVE);
    Cvar_Get("bot_map_q3ctf", "q3ctf1", CVAR_ARCHIVE);

    // the game's own, registered here too so the menu has a value to show
    // before any game has run
    Cvar_Get("teamplay", "0", 0);
    Cvar_Get("capturelimit", "8", CVAR_ARCHIVE);
    Cvar_Get("g_q3_items", "1", CVAR_ARCHIVE);     // g_q3.c
    Cvar_Get("g_q3_kit", "0", CVAR_ARCHIVE);
}

static void UI_BotShutdown(void)
{
    cvar_t  *var;
    int     i;

    for (i = 1; i <= BOT_MAX_MATCH; i++) {
        if ((var = Cvar_FindVar(va("bot_skin%d", i))) != NULL)
            var->changed = NULL;
        if ((var = Cvar_FindVar(va("bot_team%d", i))) != NULL)
            var->changed = NULL;
    }
    if ((var = Cvar_FindVar("bot_mode")) != NULL)
        var->changed = NULL;
    if ((var = Cvar_FindVar("bot_models")) != NULL)
        var->changed = NULL;
}


/*
=================
UI_Init
=================
*/
void UI_Init(void)
{
    char buffer[MAX_QPATH];
    int i;

    Cmd_Register(c_ui);

    ui_debug = Cvar_Get("ui_debug", "0", 0);
    ui_open = Cvar_Get("ui_open", "1", 0);

    UI_ModeChanged();

    uis.fontHandle = R_RegisterFont("conchars");
    uis.cursorHandle = R_RegisterPic("ch1");
    R_GetPicSize(&uis.cursorWidth, &uis.cursorHeight, uis.cursorHandle);

    for (i = 0; i < NUM_CURSOR_FRAMES; i++) {
        Q_snprintf(buffer, sizeof(buffer), "m_cursor%d", i);
        uis.bitmapCursors[i] = R_RegisterPic(buffer);
    }

    uis.color.background.u32    = MakeColor(0,   0,   0, 255);
    uis.color.normal.u32        = MakeColor(15, 128, 235, 100);
    uis.color.active.u32        = MakeColor(15, 128, 235, 100);
    uis.color.selection.u32     = MakeColor(15, 128, 235, 100);
    uis.color.disabled.u32      = MakeColor(127, 127, 127, 255);

    strcpy(uis.weaponModel, "w_railgun.md2");

    // bot match settings, which the menus use
    UI_BotInit();

    // load custom menus
    UI_LoadScript();

    // load built-in menus
    M_Menu_PlayerConfig();
    M_Menu_PlayerModelGrid();
    M_Menu_BotPick();
    M_Menu_Q3SP();
    M_Menu_Servers();
    M_Menu_Demos();

    Com_DPrintf("Registered %d menus.\n", List_Count(&ui_menus));

    uis.initialized = true;
}

/*
=================
UI_Shutdown
=================
*/
void UI_Shutdown(void)
{
    if (!uis.initialized) {
        return;
    }
    UI_ForceMenuOff();

    ui_scale->changed = NULL;
    UI_BotShutdown();

    PlayerModel_Free();

    UI_FreeMenus();

    Cmd_Deregister(c_ui);

    memset(&uis, 0, sizeof(uis));

    Z_LeakTest(TAG_UI);
}


