/*
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
==============================================================================

QUAKE III ARENA CAMPAIGN - the menus

Quake III's single player game as its own menus ran it (ioq3 code/q3_ui:
ui_splevel.c, ui_spskill.c, ui_sppostgame.c, ui_spreset.c, ui_gameinfo.c),
drawn with its own art, fonts and cursor in its 640x480 layout, centred on
the screen. The game half - the bots, the limits, the rewards, the podium -
is src/baseq2/g_sp.c.

  levelselect     CHOOSE LEVEL: the tiers of four arenas, the training arena
                  first ("Tier 0") and the final one last ("Tier 7"). A tier
                  opens when every arena of the one before it is won.
  (difficulty)    DIFFICULTY: the five skills, then FIGHT starts the arena.
  (postgame)      after the arena: the podium (the game puts it in front of
                  the camera), the medals one at a time, then MENU, REPLAY
                  and NEXT. Winning a whole tier plays its video once.
  sp_video        a Quake III video (roq.c), then maybe CHOOSE LEVEL again.
  sp_event        what the game sends while you play (g_sp.c SP_Event): the
                  reward medals and the announcer, and the result.

Progress is kept as Quake III kept it, in archived cvars of info strings:
sp_scores1..5 the best placing at each skill per arena ("l<num>"), sp_awards
the medal totals ("a0".."a5"), sp_videos the videos already shown, and
sp_selection the arena last chosen. sp_times adds the fastest win per arena,
shown on CHOOSE LEVEL with the best placing.

==============================================================================
*/

#include "ui.h"
#include "../client.h"
#include "client/sound/vorbis.h"
#include "shared/bot_roster.h"

static inline int SP_Clip(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static inline float SP_Clipf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

#define SP_ARENAS_PER_TIER  4
#define SP_MAX_ARENAS       64
#define SP_MAX_BOTS         8
#define SP_NUM_SKILLS       5
#define SP_NUM_AWARDS       6

// Quake III's text styles (ui_local.h), ours to keep apart from UI_CENTER and co.
#define Q3S_LEFT        0x0000
#define Q3S_CENTER      0x0001
#define Q3S_RIGHT       0x0002
#define Q3S_FORMATMASK  0x0007
#define Q3S_SMALLFONT   0x0010
#define Q3S_DROPSHADOW  0x0800
#define Q3S_INVERSE     0x2000
#define Q3S_PULSE       0x4000

#define PROP_GAP_WIDTH      3
#define PROP_SPACE_WIDTH    8
#define PROP_HEIGHT         27
#define PROP_SMALL_SCALE    0.75f
#define PROPB_GAP_WIDTH     4
#define PROPB_SPACE_WIDTH   12
#define PROPB_HEIGHT        36
#define PULSE_DIVISOR       75

static const float c_white[4]   = { 1, 1, 1, 1 };
static const float c_black[4]   = { 0, 0, 0, 1 };
static const float c_red[4]     = { 1, 0, 0, 1 };
static const float c_orange[4]  = { 1, 0.43f, 0, 1 };
static const float c_yellow[4]  = { 1, 1, 0, 1 };

static const char *const sp_skill_names[SP_NUM_SKILLS] = {
    "I Can Win", "Bring It On", "Hurt Me Plenty", "Hardcore", "NIGHTMARE!"
};

// ui_atoms.c: the awards in their order, a0..a5
static const char *const sp_medal_names[SP_NUM_AWARDS] = {
    "Accuracy", "Impressive", "Excellent", "Gauntlet", "Frags", "Perfect"
};
static const char *const sp_medal_pics[SP_NUM_AWARDS] = {
    "menu/medals/medal_accuracy.tga", "menu/medals/medal_impressive.tga",
    "menu/medals/medal_excellent.tga", "menu/medals/medal_gauntlet.tga",
    "menu/medals/medal_frags.tga", "menu/medals/medal_victory.tga"
};
static const char *const sp_medal_sounds[SP_NUM_AWARDS] = {
    "feedback/accuracy.wav", "feedback/impressive_a.wav", "feedback/excellent_a.wav",
    "feedback/gauntlet.wav", "feedback/frags.wav", "feedback/perfect.wav"
};

enum { AWARD_ACCURACY, AWARD_IMPRESSIVE, AWARD_EXCELLENT, AWARD_GAUNTLET, AWARD_FRAGS, AWARD_PERFECT };

/*
==============================================================================

THE ARENAS

==============================================================================
*/

typedef struct {
    char    map[MAX_QPATH];
    char    longname[64];
    char    bots[SP_MAX_BOTS][32];
    int     numbots;
    int     fraglimit, timelimit;
    int     num;                    // its campaign number, as Quake III numbered it
} sp_arena_t;

static sp_arena_t   sp_arenas[SP_MAX_ARENAS];   // the singles in order, then the specials
static int          sp_numsingles;              // a whole number of tiers
static int          sp_training = -1, sp_final = -1;
static bool         sp_loaded;

static cvar_t   *sp_scores[SP_NUM_SKILLS];
static cvar_t   *sp_awards, *sp_videos, *sp_selection, *sp_times, *sp_skill, *sp_campaign;

static int SP_NumTiers(void)
{
    return sp_numsingles / SP_ARENAS_PER_TIER;
}

static void SP_ParseArenas(const char *text)
{
    char        key[MAX_TOKEN_CHARS], type[64], special[32], bots[256];
    const char  *data = text, *b;
    char        *tok;
    sp_arena_t  a;
    int         count = 0;

    while (1) {
        tok = COM_Parse(&data);
        if (!data || tok[0] != '{')
            break;

        memset(&a, 0, sizeof(a));
        type[0] = special[0] = bots[0] = 0;
        while (1) {
            tok = COM_Parse(&data);
            if (!data || tok[0] == '}')
                break;
            Q_strlcpy(key, tok, sizeof(key));
            tok = COM_Parse(&data);
            if (!data)
                break;
            if (!Q_stricmp(key, "map"))
                Q_strlcpy(a.map, tok, sizeof(a.map));
            else if (!Q_stricmp(key, "longname"))
                Q_strlcpy(a.longname, tok, sizeof(a.longname));
            else if (!Q_stricmp(key, "bots"))
                Q_strlcpy(bots, tok, sizeof(bots));
            else if (!Q_stricmp(key, "type"))
                Q_strlcpy(type, tok, sizeof(type));
            else if (!Q_stricmp(key, "special"))
                Q_strlcpy(special, tok, sizeof(special));
            else if (!Q_stricmp(key, "fraglimit"))
                a.fraglimit = atoi(tok);
            else if (!Q_stricmp(key, "timelimit"))
                a.timelimit = atoi(tok);
        }
        if (!data && tok[0] != '}')
            break;

        if (!a.map[0] || !strstr(type, "single"))
            continue;

        b = bots;
        while (a.numbots < SP_MAX_BOTS) {
            tok = COM_Parse(&b);
            if (!*tok)
                break;
            Q_strlcpy(a.bots[a.numbots++], tok, sizeof(a.bots[0]));
            if (!b)
                break;
        }
        if (!a.fraglimit && !a.timelimit)
            a.fraglimit = 10;

        if (count >= SP_MAX_ARENAS - 2)
            continue;

        if (!Q_stricmp(special, "training")) {
            if (sp_training < 0) {
                sp_arenas[SP_MAX_ARENAS - 2] = a;
                sp_training = SP_MAX_ARENAS - 2;
            }
        } else if (!Q_stricmp(special, "final")) {
            if (sp_final < 0) {
                sp_arenas[SP_MAX_ARENAS - 1] = a;
                sp_final = SP_MAX_ARENAS - 1;
            }
        } else if (!special[0]) {
            sp_arenas[count++] = a;
        }
    }

    // a whole number of tiers, as Quake III kept it
    sp_numsingles = count - count % SP_ARENAS_PER_TIER;
    for (count = 0; count < sp_numsingles; count++)
        sp_arenas[count].num = count;
    if (sp_training >= 0)
        sp_arenas[sp_training].num = sp_numsingles;
    if (sp_final >= 0)
        sp_arenas[sp_final].num = sp_numsingles + 1;
}

static bool SP_LoadArenas(void)
{
    void    *buf;
    char    *text;
    int     len;

    if (sp_loaded)
        return sp_numsingles > 0;

    sp_numsingles = 0;
    sp_training = sp_final = -1;

    if (!Cvar_VariableInteger("fs_quake3"))
        return false;

    len = FS_LoadFile("baseq3/scripts/arenas.txt", &buf);
    if (len <= 0)
        len = FS_LoadFile("scripts/arenas.txt", &buf);
    if (len <= 0 || !buf)
        return false;

    text = Z_Malloc(len + 1);
    memcpy(text, buf, len);
    text[len] = 0;
    FS_FreeFile(buf);

    SP_ParseArenas(text);
    Z_Free(text);

    sp_loaded = true;
    Com_DPrintf("Campaign: %d arenas in %d tiers%s%s\n", sp_numsingles, SP_NumTiers(),
                sp_training >= 0 ? ", training" : "", sp_final >= 0 ? ", final" : "");
    return sp_numsingles > 0;
}

static sp_arena_t *SP_ArenaByNum(int num)
{
    if (num >= 0 && num < sp_numsingles)
        return &sp_arenas[num];
    if (sp_training >= 0 && num == sp_arenas[sp_training].num)
        return &sp_arenas[sp_training];
    if (sp_final >= 0 && num == sp_arenas[sp_final].num)
        return &sp_arenas[sp_final];
    return NULL;
}

static sp_arena_t *SP_ArenaByMap(const char *map)
{
    int i;

    for (i = 0; i < sp_numsingles; i++)
        if (!Q_stricmp(sp_arenas[i].map, map))
            return &sp_arenas[i];
    if (sp_training >= 0 && !Q_stricmp(sp_arenas[sp_training].map, map))
        return &sp_arenas[sp_training];
    if (sp_final >= 0 && !Q_stricmp(sp_arenas[sp_final].map, map))
        return &sp_arenas[sp_final];
    return NULL;
}

static bool SP_IsTraining(int num)
{
    return sp_training >= 0 && num == sp_arenas[sp_training].num;
}

static bool SP_IsFinal(int num)
{
    return sp_final >= 0 && num == sp_arenas[sp_final].num;
}

// Quake III's bot of this name ("daemia"): its skin of ours, and the name it goes by
static const char *SP_BotSkin(const char *name, const char **display)
{
    int i;

    for (i = 0; i < BOT_Q3_ROSTER_SIZE; i++) {
        if (!Q_stricmp(bot_q3_roster[i].name, name)) {
            if (display)
                *display = bot_q3_roster[i].name;
            return bot_q3_roster[i].skin;
        }
    }
    if (display)
        *display = name;
    return NULL;
}

/*
==============================================================================

PROGRESS (ui_gameinfo.c)

==============================================================================
*/

static int SP_InfoInt(cvar_t *var, const char *key)
{
    return var ? atoi(Info_ValueForKey(var->string, key)) : 0;
}

static void SP_InfoSetInt(cvar_t *var, const char *key, int value)
{
    char info[MAX_INFO_STRING];

    if (!var)
        return;
    Q_strlcpy(info, var->string, sizeof(info));
    if (!Info_SetValueForKey(info, key, va("%d", value)))
        return;
    Cvar_SetByVar(var, info, FROM_CODE);
}

// UI_GetBestScore: the best placing at any skill, and the skill (the higher on a tie)
static int SP_BestScore(int num, int *skill)
{
    int best = 0, best_skill = 0, s, score;

    for (s = 1; s <= SP_NUM_SKILLS; s++) {
        score = SP_InfoInt(sp_scores[s - 1], va("l%d", num));
        if (!score)
            continue;
        if (!best || score <= best) {
            best = score;
            best_skill = s;
        }
    }
    if (skill)
        *skill = best_skill;
    return best;
}

// UI_SetBestScore
static void SP_SetBestScore(int num, int score, int skill)
{
    int old;

    if (score < 1 || score > 8 || skill < 1 || skill > SP_NUM_SKILLS)
        return;
    old = SP_InfoInt(sp_scores[skill - 1], va("l%d", num));
    if (old && old <= score)
        return;
    SP_InfoSetInt(sp_scores[skill - 1], va("l%d", num), score);
}

static int SP_AwardLevel(int award)
{
    return SP_InfoInt(sp_awards, va("a%d", award));
}

static void SP_LogAward(int award, int data)
{
    if (data <= 0)
        return;
    SP_InfoSetInt(sp_awards, va("a%d", award), SP_AwardLevel(award) + data);
}

// UI_GetCurrentGame: the arena to play next - training until it is won, then
// the first single not won, then the final
static int SP_CurrentGame(void)
{
    int n;

    if (sp_training >= 0 && SP_BestScore(sp_arenas[sp_training].num, NULL) != 1)
        return sp_arenas[sp_training].num;
    for (n = 0; n < sp_numsingles; n++)
        if (SP_BestScore(n, NULL) != 1)
            return n;
    if (sp_final >= 0)
        return sp_arenas[sp_final].num;
    return sp_numsingles - 1;
}

static int SP_SetOf(int num)
{
    if (SP_IsTraining(num))
        return -1;
    if (SP_IsFinal(num))
        return SP_NumTiers();
    return num / SP_ARENAS_PER_TIER;
}

// UI_TierCompleted: having just won this arena, the tier finished, or -1
static int SP_TierCompleted(int num)
{
    int tier = num / SP_ARENAS_PER_TIER, n;

    if (SP_IsTraining(num))
        return 0;
    if (SP_IsFinal(num))
        return SP_NumTiers() + 1;

    for (n = 0; n < SP_ARENAS_PER_TIER; n++)
        if (SP_BestScore(tier * SP_ARENAS_PER_TIER + n, NULL) != 1)
            return -1;
    return tier + 1;
}

// UI_ShowTierVideo: true the first time only
static bool SP_ShowVideoOnce(const char *key)
{
    if (SP_InfoInt(sp_videos, key))
        return false;
    SP_InfoSetInt(sp_videos, key, 1);
    return true;
}

static int SP_SelectionFor(int num)
{
    if (SP_IsTraining(num))
        return -SP_ARENAS_PER_TIER;
    if (SP_IsFinal(num))
        return SP_NumTiers() * SP_ARENAS_PER_TIER;
    return num;
}

// UI_NewGame: everything back to the start - but the intro, once seen, stays seen
static void SP_NewGame(void)
{
    bool    intro = SP_InfoInt(sp_videos, "intro") != 0;
    int     i;

    for (i = 0; i < SP_NUM_SKILLS; i++)
        Cvar_SetByVar(sp_scores[i], "", FROM_CODE);
    Cvar_SetByVar(sp_awards, "", FROM_CODE);
    Cvar_SetByVar(sp_videos, "", FROM_CODE);
    Cvar_SetByVar(sp_times, "", FROM_CODE);
    if (intro)
        SP_InfoSetInt(sp_videos, "intro", 1);
    Cvar_SetInteger(sp_selection, -SP_ARENAS_PER_TIER, FROM_CODE);
}

static void SP_StartArena(int num, int skill)
{
    const sp_arena_t *a = SP_ArenaByNum(num);

    if (!a)
        return;

    skill = SP_Clip(skill, 1, SP_NUM_SKILLS);
    Cvar_SetInteger(sp_selection, SP_SelectionFor(num), FROM_CODE);
    Cvar_SetInteger(sp_skill, skill, FROM_CODE);
    OGG_Stop();
    UI_ForceMenuOff();

    // Quake III's game whatever the bot match is set to - the game side
    // reads sp_campaign as the arena loads (src/baseq2/g_sp.c)
    Cbuf_AddText(&cmd_buffer, va("set sp_campaign 1; set sp_skill %d; set bot_match 0; set teamplay 0; "
                                 "set ctf 0; set cheats 0; deathmatch 1; coop 0; maxclients 8; map %s force\n",
                                 skill, a->map));
}

/*
==============================================================================

DRAWING IN QUAKE III'S 640x480

==============================================================================
*/

static float    sp_s, sp_x0, sp_y0;     // virtual to screen: x * sp_s + sp_x0

static void SP_Screen(float width, float height)
{
    sp_s = min(width / 640.0f, height / 480.0f);
    sp_x0 = (width - 640 * sp_s) * 0.5f;
    sp_y0 = (height - 480 * sp_s) * 0.5f;
}

static void SP_SetColor(const float *c, float alpha)
{
    R_SetColor(MakeColor((int)SP_Clipf(c[0] * 255, 0, 255), (int)SP_Clipf(c[1] * 255, 0, 255),
                         (int)SP_Clipf(c[2] * 255, 0, 255), (int)SP_Clipf(c[3] * alpha * 255, 0, 255)));
}

static float SP_Pulse(void)
{
    return 0.5f + 0.5f * sinf(uis.realtime / (float)PULSE_DIVISOR);
}

// pictures that are not there, so a page drawn every frame does not keep looking
#define SP_MAX_MISSES   64
static char sp_misses[SP_MAX_MISSES][MAX_QPATH];
static int  sp_nummisses;

/*
Quake III's menu art is drawn filtered, as Quake III drew it: it is 256x256 at
most, stretched to the screen, and Quake II's nearest sampling of pictures
showed every texel (Matt, 11-19: "grainy"). IF_BILERP is linear whatever
pt_bilerp_pics says. An image is found by name and type only, so the flag holds
only for a picture nothing registered first: the player icons are also the
scoreboard's (precache.c) and the model grid's, nearest, so they are taken as
IT_SPRITE - their own copy, which every renderer filters.
*/
static qhandle_t SP_PicAs(const char *name, imagetype_t type)
{
    char        path[MAX_QPATH];
    qhandle_t   h;
    int         i;

    if (!name || !*name)
        return 0;
    for (i = 0; i < sp_nummisses; i++)
        if (!Q_stricmp(sp_misses[i], name))
            return 0;

    if (Q_concat(path, sizeof(path), "/", name) >= sizeof(path))
        return 0;
    h = R_RegisterImage(path, type, IF_SRGB | IF_BILERP, NULL);
    if (!h && sp_nummisses < SP_MAX_MISSES)     // path, not name: a va() the loader may reuse
        Q_strlcpy(sp_misses[sp_nummisses++], path + 1, MAX_QPATH);
    return h;
}

static qhandle_t SP_Pic(const char *name)
{
    return SP_PicAs(name, IT_PIC);
}

// UI_DrawHandlePic: a negative width mirrors it
static void SP_DrawPic(float x, float y, float w, float h, qhandle_t pic)
{
    float s1 = 0, s2 = 1;

    if (!pic)
        return;
    if (w < 0) {
        w = -w;
        s1 = 1;
        s2 = 0;
    }
    R_DrawStretchSubPic(sp_x0 + x * sp_s, sp_y0 + y * sp_s, w * sp_s, h * sp_s, s1, 0, s2, 1, pic);
}

static void SP_DrawNamedPic(float x, float y, float w, float h, const char *name)
{
    SP_DrawPic(x, y, w, h, SP_Pic(name));
}

static void SP_FillRect(float x, float y, float w, float h, const float *c)
{
    R_DrawFill32((int)(sp_x0 + x * sp_s), (int)(sp_y0 + y * sp_s),
                 (int)ceilf(w * sp_s), (int)ceilf(h * sp_s),
                 MakeColor((int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), (int)(c[3] * 255)));
}

// the whole screen, not just the 640x480 in the middle
static void SP_FillScreen(const float *c)
{
    R_DrawFill32(0, 0, uis.width, uis.height,
                 MakeColor((int)(c[0] * 255), (int)(c[1] * 255), (int)(c[2] * 255), (int)(c[3] * 255)));
}

// ---- the proportional font, menu/art/font1_prop (ui_atoms.c propMap) ----

static const short sp_prop[65 + 4][3] = {
    { 0, 0, PROP_SPACE_WIDTH },     // space
    { 11, 122, 7 },     // !
    { 154, 181, 14 },   // "
    { 55, 122, 17 },    // #
    { 79, 122, 18 },    // $
    { 101, 122, 23 },   // %
    { 153, 122, 18 },   // &
    { 9, 93, 7 },       // '
    { 207, 122, 8 },    // (
    { 230, 122, 9 },    // )
    { 177, 122, 18 },   // *
    { 30, 152, 18 },    // +
    { 85, 181, 7 },     // ,
    { 34, 93, 11 },     // -
    { 110, 181, 6 },    // .
    { 130, 152, 14 },   // /
    { 22, 64, 17 },     // 0
    { 41, 64, 12 },     // 1
    { 58, 64, 17 },     // 2
    { 78, 64, 18 },     // 3
    { 98, 64, 19 },     // 4
    { 120, 64, 18 },    // 5
    { 141, 64, 18 },    // 6
    { 204, 64, 16 },    // 7
    { 162, 64, 17 },    // 8
    { 182, 64, 18 },    // 9
    { 59, 181, 7 },     // :
    { 35, 181, 7 },     // ;
    { 203, 152, 14 },   // <
    { 56, 93, 14 },     // =
    { 228, 152, 14 },   // >
    { 177, 181, 18 },   // ?
    { 28, 122, 22 },    // @
    { 5, 4, 18 },       // A
    { 27, 4, 18 },      // B
    { 48, 4, 18 },      // C
    { 69, 4, 17 },      // D
    { 90, 4, 13 },      // E
    { 106, 4, 13 },     // F
    { 121, 4, 18 },     // G
    { 143, 4, 17 },     // H
    { 164, 4, 8 },      // I
    { 175, 4, 16 },     // J
    { 195, 4, 18 },     // K
    { 216, 4, 12 },     // L
    { 230, 4, 23 },     // M
    { 6, 34, 18 },      // N
    { 27, 34, 18 },     // O
    { 48, 34, 18 },     // P
    { 68, 34, 18 },     // Q
    { 90, 34, 17 },     // R
    { 110, 34, 18 },    // S
    { 130, 34, 14 },    // T
    { 146, 34, 18 },    // U
    { 166, 34, 19 },    // V
    { 185, 34, 29 },    // W
    { 215, 34, 18 },    // X
    { 234, 34, 18 },    // Y
    { 5, 64, 14 },      // Z
    { 60, 152, 7 },     // [
    { 106, 151, 13 },   // backslash
    { 83, 152, 7 },     // ]
    { 128, 122, 17 },   // ^
    { 4, 152, 21 },     // _
    { 134, 181, 5 },    // `
    { 153, 152, 13 },   // {
    { 11, 181, 5 },     // |
    { 180, 152, 13 },   // }
    { 79, 93, 17 },     // ~
};

// the glyph for a character, or NULL; lower case is drawn as upper case
static const short *SP_PropGlyph(int ch)
{
    ch &= 127;
    if (ch >= 'a' && ch <= 'z')
        ch -= 'a' - 'A';
    if (ch >= ' ' && ch <= '`')
        return sp_prop[ch - ' '];
    if (ch >= '{' && ch <= '~')
        return sp_prop[65 + ch - '{'];
    return NULL;
}

static int SP_PropWidth(const char *s)
{
    int width = 0;

    for (; *s; s++) {
        const short *g = SP_PropGlyph(*s);
        if (g) {
            width += g[2] + PROP_GAP_WIDTH;
        }
    }
    return width ? width - PROP_GAP_WIDTH : 0;
}

static void SP_PropString2(float x, float y, const char *s, const float *color, float alpha,
                           float scale, qhandle_t font)
{
    float ax = x, aw;

    SP_SetColor(color, alpha);
    for (; *s; s++) {
        const short *g = SP_PropGlyph(*s);

        if (*s == ' ') {
            aw = PROP_SPACE_WIDTH * scale;
        } else if (g) {
            aw = g[2] * scale;
            R_DrawStretchSubPic(sp_x0 + ax * sp_s, sp_y0 + y * sp_s, aw * sp_s, PROP_HEIGHT * scale * sp_s,
                                g[0] / 256.0f, g[1] / 256.0f, (g[0] + g[2]) / 256.0f,
                                (g[1] + PROP_HEIGHT) / 256.0f, font);
        } else {
            aw = 0;
        }
        ax += aw + PROP_GAP_WIDTH * scale;
    }
    R_ClearColor();
}

// UI_DrawProportionalString
static void SP_PropString(float x, float y, const char *s, int style, const float *color, float alpha)
{
    qhandle_t   font = SP_Pic("menu/art/font1_prop.tga");
    float       scale = (style & Q3S_SMALLFONT) ? PROP_SMALL_SCALE : 1.0f;
    float       width = SP_PropWidth(s) * scale;
    float       dim[4];

    if (!font)
        return;

    switch (style & Q3S_FORMATMASK) {
    case Q3S_CENTER:
        x -= width / 2;
        break;
    case Q3S_RIGHT:
        x -= width;
        break;
    }

    if (style & Q3S_DROPSHADOW)
        SP_PropString2(x + 2, y + 2, s, c_black, alpha * color[3], scale, font);

    if (style & Q3S_INVERSE) {
        Vector4Set(dim, color[0] * 0.7f, color[1] * 0.7f, color[2] * 0.7f, color[3]);
        SP_PropString2(x, y, s, dim, alpha, scale, font);
        return;
    }

    // Quake III draws the plain text at full colour under the pulsing glow
    if (style & Q3S_PULSE) {
        qhandle_t glow = SP_Pic("menu/art/font1_prop_glo.tga");

        SP_PropString2(x, y, s, color, alpha, scale, font);
        if (glow)
            SP_PropString2(x, y, s, color, alpha * SP_Pulse(), scale, glow);
        return;
    }

    SP_PropString2(x, y, s, color, alpha, scale, font);
}

// ---- the banner font, menu/art/font2_prop (A-Z only) ----

static const short sp_propb[26][3] = {
    { 11, 12, 33 }, { 49, 12, 31 }, { 85, 12, 31 }, { 120, 12, 30 }, { 156, 12, 21 },
    { 183, 12, 21 }, { 207, 12, 32 },
    { 13, 55, 30 }, { 49, 55, 13 }, { 66, 55, 29 }, { 101, 55, 31 }, { 135, 55, 21 },
    { 158, 55, 40 }, { 204, 55, 32 },
    { 12, 97, 31 }, { 48, 97, 31 }, { 82, 97, 30 }, { 118, 97, 30 }, { 153, 97, 30 },
    { 185, 97, 25 }, { 213, 97, 30 },
    { 11, 139, 32 }, { 42, 139, 51 }, { 93, 139, 32 }, { 126, 139, 31 }, { 158, 139, 25 },
};

static void SP_BannerString2(float x, float y, const char *s, const float *color, float alpha, qhandle_t font)
{
    float ax = x;

    SP_SetColor(color, alpha);
    for (; *s; s++) {
        int ch = Q_toupper(*s & 127);

        if (ch == ' ') {
            ax += PROPB_SPACE_WIDTH + PROPB_GAP_WIDTH;
        } else if (ch >= 'A' && ch <= 'Z') {
            const short *g = sp_propb[ch - 'A'];

            R_DrawStretchSubPic(sp_x0 + ax * sp_s, sp_y0 + y * sp_s, g[2] * sp_s, PROPB_HEIGHT * sp_s,
                                g[0] / 256.0f, g[1] / 256.0f, (g[0] + g[2]) / 256.0f,
                                (g[1] + PROPB_HEIGHT) / 256.0f, font);
            ax += g[2] + PROPB_GAP_WIDTH;
        }
    }
    R_ClearColor();
}

// UI_DrawBannerString
static void SP_BannerString(float x, float y, const char *str, int style, const float *color)
{
    qhandle_t   font = SP_Pic("menu/art/font2_prop.tga");
    const char  *s;
    int         width = 0;

    if (!font)
        return;

    for (s = str; *s; s++) {
        int ch = Q_toupper(*s & 127);
        if (ch == ' ')
            width += PROPB_SPACE_WIDTH;
        else if (ch >= 'A' && ch <= 'Z')
            width += sp_propb[ch - 'A'][2] + PROPB_GAP_WIDTH;
    }
    width -= PROPB_GAP_WIDTH;

    switch (style & Q3S_FORMATMASK) {
    case Q3S_CENTER:
        x -= width / 2;
        break;
    case Q3S_RIGHT:
        x -= width;
        break;
    }

    if (style & Q3S_DROPSHADOW)
        SP_BannerString2(x + 2, y + 2, str, c_black, color[3], font);
    SP_BannerString2(x, y, str, color, 1, font);
}

// ---- gfx/2d/bigchars: 16x16 cells, drawn 16x16, or 8x16 small (UI_DrawString) ----

static void SP_CharString(float x, float y, const char *str, int style, const float *color, float alpha)
{
    qhandle_t   font = SP_Pic("gfx/2d/bigchars.tga");
    float       cw = (style & Q3S_SMALLFONT) ? 8 : 16, ch = 16;
    float       len = strlen(str) * cw;
    const char  *s;

    if (!font)
        return;

    switch (style & Q3S_FORMATMASK) {
    case Q3S_CENTER:
        x -= len / 2;
        break;
    case Q3S_RIGHT:
        x -= len;
        break;
    }

    if (style & Q3S_DROPSHADOW) {
        SP_SetColor(c_black, alpha * color[3]);
        for (s = str; *s; s++) {
            int c = *s & 255;
            float fx = x + 2 + (s - str) * cw;
            if (c != ' ')
                R_DrawStretchSubPic(sp_x0 + fx * sp_s, sp_y0 + (y + 2) * sp_s, cw * sp_s, ch * sp_s,
                                    (c & 15) * 0.0625f, (c >> 4) * 0.0625f,
                                    (c & 15) * 0.0625f + 0.0625f, (c >> 4) * 0.0625f + 0.0625f, font);
        }
    }

    SP_SetColor(color, alpha);
    for (s = str; *s; s++) {
        int c = *s & 255;
        float fx = x + (s - str) * cw;
        if (c != ' ')
            R_DrawStretchSubPic(sp_x0 + fx * sp_s, sp_y0 + y * sp_s, cw * sp_s, ch * sp_s,
                                (c & 15) * 0.0625f, (c >> 4) * 0.0625f,
                                (c & 15) * 0.0625f + 0.0625f, (c >> 4) * 0.0625f + 0.0625f, font);
    }
    R_ClearColor();
}

/*
==============================================================================

PAGES

What every page shares: its hot spots (built as it is drawn, in Quake III's
layout), the one with the focus - the mouse moves it, so do the arrow keys -
and the buttons' look: the plain picture, the lit one pulsing over it when
it has the focus (Bitmap_Draw with QMF_PULSEIFFOCUS).

==============================================================================
*/

#define SP_MAX_HOT  24

typedef struct {
    int     id;
    float   x, y, w, h;
} sp_hot_t;

typedef struct {
    menuFrameWork_t menu;
    sp_hot_t        hot[SP_MAX_HOT];
    int             numhot;
    int             focus;          // an id, -1 none
    int             mouse[2];       // where the pointer was last frame
    unsigned        opened;         // uis.realtime the page came up
} sp_page_t;

static void SP_SoundQ3(const char *rel);

static void SP_BeginHot(sp_page_t *p)
{
    p->numhot = 0;
}

static void SP_AddHot(sp_page_t *p, int id, float x, float y, float w, float h)
{
    if (p->numhot < SP_MAX_HOT) {
        sp_hot_t *t = &p->hot[p->numhot++];
        t->id = id;
        t->x = x;
        t->y = y;
        t->w = w;
        t->h = h;
    }
}

static bool SP_HasHot(const sp_page_t *p, int id)
{
    int i;

    for (i = 0; i < p->numhot; i++)
        if (p->hot[i].id == id)
            return true;
    return false;
}

// after the hot spots are in: the mouse, if it moved, takes the focus where it is
static void SP_TrackMouse(sp_page_t *p)
{
    float   mx, my;
    int     i;

    if (p->focus >= 0 && !SP_HasHot(p, p->focus))
        p->focus = -1;

    if (uis.mouseCoords[0] == p->mouse[0] && uis.mouseCoords[1] == p->mouse[1])
        return;
    p->mouse[0] = uis.mouseCoords[0];
    p->mouse[1] = uis.mouseCoords[1];

    mx = (uis.mouseCoords[0] - sp_x0) / sp_s;
    my = (uis.mouseCoords[1] - sp_y0) / sp_s;

    for (i = p->numhot - 1; i >= 0; i--) {
        const sp_hot_t *t = &p->hot[i];

        if (mx >= t->x && mx < t->x + t->w && my >= t->y && my < t->y + t->h) {
            if (p->focus != t->id) {
                p->focus = t->id;
                SP_SoundQ3("misc/menu2.wav");
            }
            return;
        }
    }
}

// the hot spot under the pointer, -1 none
static int SP_HotAtMouse(const sp_page_t *p)
{
    float   mx = (uis.mouseCoords[0] - sp_x0) / sp_s;
    float   my = (uis.mouseCoords[1] - sp_y0) / sp_s;
    int     i;

    for (i = p->numhot - 1; i >= 0; i--) {
        const sp_hot_t *t = &p->hot[i];
        if (mx >= t->x && mx < t->x + t->w && my >= t->y && my < t->y + t->h)
            return t->id;
    }
    return -1;
}

static void SP_MoveFocus(sp_page_t *p, int dir)
{
    int i, cur = -1;

    if (!p->numhot)
        return;
    for (i = 0; i < p->numhot; i++)
        if (p->hot[i].id == p->focus)
            cur = i;
    if (cur < 0)
        cur = dir > 0 ? -1 : 0;
    cur = (cur + dir + p->numhot) % p->numhot;
    p->focus = p->hot[cur].id;
    SP_SoundQ3("misc/menu2.wav");
}

// a button: its picture, and the lit one pulsing over it with the focus
static void SP_Button(sp_page_t *p, int id, float x, float y, float w, float h,
                      const char *pic, const char *focuspic)
{
    SP_DrawNamedPic(x, y, w, h, pic);
    if (p->focus == id) {
        float pulse[4] = { 1, 1, 1, 1 };
        SP_SetColor(pulse, SP_Pulse());
        SP_DrawNamedPic(x, y, w, h, focuspic);
        R_ClearColor();
    }
    // a negative width only mirrors the picture, which still covers x..x+|w|
    // (SP_DrawPic, Quake III's Bitmap_Init): the click goes there too
    SP_AddHot(p, id, x, y, fabsf(w), h);
}

/*
Keys every page takes alike: the arrows move the focus, Enter or a click is
the focused thing's, Escape or the right button is `back`. Returns the id to
act on, or -1.
*/
static int SP_PageKey(sp_page_t *p, int key, bool *back)
{
    *back = false;

    switch (key) {
    case K_UPARROW:
    case K_KP_UPARROW:
    case K_LEFTARROW:
    case K_KP_LEFTARROW:
    case K_MWHEELUP:
        SP_MoveFocus(p, -1);
        return -1;
    case K_DOWNARROW:
    case K_KP_DOWNARROW:
    case K_RIGHTARROW:
    case K_KP_RIGHTARROW:
    case K_TAB:
    case K_MWHEELDOWN:
        SP_MoveFocus(p, 1);
        return -1;
    case K_ENTER:
    case K_KP_ENTER:
        return p->focus;
    case K_MOUSE1:
        return SP_HotAtMouse(p);
    case K_ESCAPE:
    case K_MOUSE2:
        *back = true;
        return -1;
    }
    return -1;
}

static void SP_PageSize(menuFrameWork_t *m)
{
    // the layout is Quake III's, worked out as each page is drawn
}

static void SP_InitPage(sp_page_t *p, const char *name, bool transparent)
{
    memset(p, 0, sizeof(*p));
    p->menu.name = (char *)name;
    p->menu.size = SP_PageSize;
    p->menu.transparent = transparent;
    p->menu.q3_cursor = true;
    p->menu.color.u32 = MakeColor(0, 0, 0, 255);
    p->focus = -1;
    p->mouse[0] = p->mouse[1] = -1;
}

static void SP_PageOpened(sp_page_t *p)
{
    p->opened = uis.realtime;
    p->mouse[0] = p->mouse[1] = -1;     // the pointer takes the focus at once
    sp_nummisses = 0;                   // new art may have arrived
    UI_LevelshotForget();
}

void UI_SP_DrawCursor(void)
{
    qhandle_t cursor = SP_Pic("menu/art/3_cursor2.tga");

    if (!cursor) {
        R_DrawPic(uis.mouseCoords[0] - uis.cursorWidth / 2,
                  uis.mouseCoords[1] - uis.cursorHeight / 2, uis.cursorHandle);
        return;
    }
    SP_Screen(uis.width, uis.height);
    R_DrawStretchSubPic(uis.mouseCoords[0] - 16 * sp_s, uis.mouseCoords[1] - 16 * sp_s,
                        32 * sp_s, 32 * sp_s, 0, 0, 1, 1, cursor);
}

// ---- sound ----

// one of Quake III's sounds (under its sound/), if it has it
static void SP_SoundQ3(const char *rel)
{
    char path[MAX_QPATH];

    if (Q_concat(path, sizeof(path), "baseq3/sound/", rel) >= sizeof(path))
        return;
    if (!FS_FileExists(path))
        return;
    S_StartLocalSound(va("#%s", path));
}

/*
==============================================================================

THE VIDEO PAGE

==============================================================================
*/

typedef enum {
    SP_AFTER_NOTHING,
    SP_AFTER_LEVELSELECT
} sp_after_t;

static sp_page_t    m_video;
static sp_after_t   sp_video_after;
static sp_page_t    m_level;

static void SP_OpenLevelSelect(void);

static void SP_VideoFinished(void)
{
    sp_after_t after = sp_video_after;

    RoQ_Close();
    S_UnqueueRawSamples();
    sp_video_after = SP_AFTER_NOTHING;

    if (uis.activeMenu == &m_video.menu)
        UI_PopMenu();
    if (after == SP_AFTER_LEVELSELECT)
        SP_OpenLevelSelect();
}

static void SP_PlayVideo(const char *name, sp_after_t after)
{
    char path[MAX_QPATH];

    Q_concat(path, sizeof(path), "baseq3/video/", name, ".roq");
    sp_video_after = after;
    if (!RoQ_Open(path)) {
        SP_VideoFinished();
        return;
    }
    // the video's sound has the stream to itself, as a Quake II cinematic's
    // does (cin.c SCR_PlayCinematic) and Quake III's did: music fed into it
    // alongside, at another rate, is refused by OpenAL buffer by buffer
    OGG_Stop();
    S_UnqueueRawSamples();
    UI_PushMenu(&m_video.menu);
}

static void SP_Video_Draw(menuFrameWork_t *self)
{
    qhandle_t   pic;
    int         w, h;

    SP_FillScreen(c_black);
    SP_Screen(uis.width, uis.height);

    pic = RoQ_Image(&w, &h);
    if (pic)
        SP_DrawPic(0, 0, 640, 480, pic);    // a 4:3 picture, as Quake III showed them
}

static menuSound_t SP_Video_Key(menuFrameWork_t *self, int key)
{
    // any key ends it, as Quake III's did
    if (key == K_MOUSE1 || key == K_MOUSE2 || key == K_ESCAPE || key == K_ENTER ||
        key == K_KP_ENTER || key == K_SPACE || (key >= 32 && key < 127)) {
        SP_VideoFinished();
        return QMS_SILENT;
    }
    return QMS_SILENT;
}

static void SP_Video_Pop(menuFrameWork_t *self)
{
    if (RoQ_IsOpen()) {
        RoQ_Close();
        S_UnqueueRawSamples();
    }
}

/*
==============================================================================

CHOOSE LEVEL (ui_splevel.c)

==============================================================================
*/

enum {
    ID_LEFTARROW = 1, ID_MAP0, ID_MAP1, ID_MAP2, ID_MAP3, ID_RIGHTARROW,
    ID_PLAYER, ID_BACK, ID_RESET, ID_SKIRMISH, ID_FIGHT,
    ID_MEDAL0, ID_MEDAL5 = ID_MEDAL0 + SP_NUM_AWARDS - 1,
    ID_SKILL1, ID_SKILL5 = ID_SKILL1 + SP_NUM_SKILLS - 1,
    ID_YES, ID_NO, ID_MENU, ID_REPLAY, ID_NEXT
};

static struct {
    int     set, arena;         // what is selected: tier (-1 training) and arena in it
    int     current_set;        // the furthest tier open
    int     current_game;
} sp_level;

static sp_page_t m_skill, m_reset, m_post;

static int SP_MinTier(void)
{
    return sp_training >= 0 ? -1 : 0;
}

static int SP_MaxTier(void)
{
    return sp_final >= 0 ? SP_NumTiers() : SP_NumTiers() - 1;
}

static int SP_MapsInSet(int set)
{
    return (set < 0 || set >= SP_NumTiers()) ? 1 : SP_ARENAS_PER_TIER;
}

static sp_arena_t *SP_ArenaInSet(int set, int i)
{
    if (set < 0)
        return sp_training >= 0 ? &sp_arenas[sp_training] : NULL;
    if (set >= SP_NumTiers())
        return sp_final >= 0 ? &sp_arenas[sp_final] : NULL;
    return SP_ArenaByNum(set * SP_ARENAS_PER_TIER + i);
}

static void SP_SelectFromCvar(void)
{
    int sel;

    sp_level.current_game = SP_CurrentGame();
    sp_level.current_set = SP_SetOf(sp_level.current_game);

    if (!sp_selection->string[0]) {
        sel = SP_SelectionFor(sp_level.current_game);
    } else {
        sel = sp_selection->integer;
    }

    if (sel == -SP_ARENAS_PER_TIER && sp_training >= 0) {
        sp_level.set = -1;
        sp_level.arena = 0;
    } else if (sel >= 0 && sel < sp_numsingles) {
        sp_level.set = sel / SP_ARENAS_PER_TIER;
        sp_level.arena = sel % SP_ARENAS_PER_TIER;
    } else if (sel == SP_NumTiers() * SP_ARENAS_PER_TIER && sp_final >= 0) {
        sp_level.set = SP_NumTiers();
        sp_level.arena = 0;
    } else {
        sp_level.set = SP_SetOf(sp_level.current_game);
        sp_level.arena = sp_level.set >= 0 && sp_level.set < SP_NumTiers()
                         ? sp_level.current_game % SP_ARENAS_PER_TIER : 0;
    }
}

static void SP_StoreSelection(void)
{
    const sp_arena_t *a = SP_ArenaInSet(sp_level.set, sp_level.arena);

    if (a)
        Cvar_SetInteger(sp_selection, SP_SelectionFor(a->num), FROM_CODE);
}

static void SP_ChangeSet(int dir)
{
    int set = sp_level.set + dir;

    if (set < SP_MinTier() || set > SP_MaxTier())
        return;
    sp_level.set = set;
    sp_level.arena = 0;
    SP_StoreSelection();
}

// the player's head: their Quake III model's icon, or a Quake II skin's
static qhandle_t SP_PlayerIcon(const char *skin)
{
    char        model[MAX_QPATH];
    const char  *slash = strchr(skin, '/');
    qhandle_t   pic;

    if (!slash || slash == skin || (size_t)(slash - skin) >= sizeof(model))
        return 0;
    memcpy(model, skin, slash - skin);
    model[slash - skin] = 0;

    pic = SP_PicAs(va("models/players/%s/icon_%s.tga", model, slash + 1), IT_SPRITE);
    if (!pic)
        pic = SP_PicAs(va("models/players/%s/icon_default.tga", model), IT_SPRITE);
    if (!pic)
        pic = SP_PicAs(va("players/%s/%s_i.pcx", model, slash + 1), IT_SPRITE);
    return pic;
}

static const char *SP_Ordinal(int n)
{
    switch (n) {
    case 1:  return "1ST";
    case 2:  return "2ND";
    case 3:  return "3RD";
    default: return va("%dTH", n);
    }
}

static void SP_Level_Draw(menuFrameWork_t *self)
{
    sp_page_t   *p = &m_level;
    sp_arena_t  *sel;
    char        buf[128];
    float       pulse[4] = { 1, 1, 1, 1 };
    int         n, i, numMaps, x, y, count;
    bool        denied;

    SP_FillScreen(c_black);
    SP_Screen(uis.width, uis.height);
    SP_BeginHot(p);

    numMaps = SP_MapsInSet(sp_level.set);
    denied = sp_level.set > sp_level.current_set;
    sel = SP_ArenaInSet(sp_level.set, sp_level.arena);

    SP_BannerString(320, 16, "CHOOSE LEVEL", Q3S_CENTER, c_red);

    // the arrows to the tiers either side
    if (sp_level.set > SP_MinTier())
        SP_Button(p, ID_LEFTARROW, 18, 64, 16, 114, "menu/art/narrow_0.tga", "menu/art/narrow_1.tga");
    if (sp_level.set < SP_MaxTier())
        SP_Button(p, ID_RIGHTARROW, 640 - 18, 64, -16, 114, "menu/art/narrow_0.tga", "menu/art/narrow_1.tga");

    // the levelshots
    for (n = 0; n < numMaps; n++) {
        sp_arena_t *a = SP_ArenaInSet(sp_level.set, n);

        x = numMaps == 1 ? 256 : 46 + n * 140;
        y = 64;
        SP_DrawPic(x, y, 128, 96, a ? UI_Levelshot(a->map, true) : SP_Pic("menu/art/unknownmap.jpg"));
        if (!denied && a)
            SP_AddHot(p, ID_MAP0 + n, x, y, 128, 96);
    }

    // the player's medals, either side of the head, and their counts
    y = 340;
    for (i = 0, n = 0; n < SP_NUM_AWARDS; n++) {
        int level = SP_AwardLevel(n);

        if (level <= 0)
            continue;
        x = (i & 1) ? 224 - (i - 1) / 2 * (48 + 16) : 368 + i / 2 * (48 + 16);
        i++;
        SP_DrawNamedPic(x, y, 48, 48, sp_medal_pics[n]);
        SP_AddHot(p, ID_MEDAL0 + n, x, y, 48, 48);
        if (level == 1)
            continue;
        if (level >= 1000000)
            Q_snprintf(buf, sizeof(buf), "%dm", level / 1000000);
        else if (level >= 1000)
            Q_snprintf(buf, sizeof(buf), "%dk", level / 1000);
        else
            Q_snprintf(buf, sizeof(buf), "%d", level);
        SP_CharString(x + 24, y + 48, buf, Q3S_CENTER, c_yellow, 1);
    }

    // the player: name and head
    SP_PropString(320, 314, Cvar_VariableString("name"), Q3S_CENTER | Q3S_SMALLFONT, c_orange, 1);
    {
        qhandle_t head = SP_PlayerIcon(Cvar_VariableString("skin"));

        if (head)
            SP_DrawPic(288, 340, 64, 64, head);
        else
            SP_FillRect(288, 340, 64, 64, c_black);
        SP_AddHot(p, ID_PLAYER, 288, 340, 64, 64);
    }

    SP_PropString(18, 38, va("Tier %d", sp_level.set + 1), Q3S_LEFT | Q3S_SMALLFONT, c_orange, 1);

    // the name strips
    for (n = 0; n < numMaps; n++) {
        x = numMaps == 1 ? 256 : 46 + n * 140;
        SP_FillRect(x, 64 + 96, 128, 18, c_black);
    }

    // buttons
    SP_Button(p, ID_BACK, 0, 480 - 64, 128, 64, "menu/art/back_0.tga", "menu/art/back_1.tga");
    SP_Button(p, ID_RESET, 170, 480 - 64, 128, 64, "menu/art/reset_0.tga", "menu/art/reset_1.tga");
    SP_Button(p, ID_SKIRMISH, 342, 480 - 64, 128, 64, "menu/art/skirmish_0.tga", "menu/art/skirmish_1.tga");

    if (denied) {
        SP_PropString(320, 216, "ACCESS DENIED", Q3S_CENTER, c_red, 1);
        SP_TrackMouse(p);
        return;
    }

    SP_Button(p, ID_FIGHT, 640 - 128, 480 - 64, 128, 64, "menu/art/fight_0.tga", "menu/art/fight_1.tga");

    // names, completion and the selection
    for (n = 0; n < numMaps; n++) {
        sp_arena_t  *a = SP_ArenaInSet(sp_level.set, n);
        int         skill, best;

        if (!a)
            continue;
        x = numMaps == 1 ? 256 : 46 + n * 140;
        y = 64;

        Q_strlcpy(buf, a->map, 17);
        Q_strupr(buf);
        SP_CharString(x + 64, y + 96, buf, Q3S_CENTER | Q3S_SMALLFONT, c_orange, 1);

        best = SP_BestScore(a->num, &skill);
        if (best == 1 && skill >= 1)
            SP_DrawNamedPic(x, y, 128, 96, va("menu/art/level_complete%d.tga", skill));

        if (n == sp_level.arena) {
            if (p->focus == ID_MAP0 + n)
                SP_SetColor(pulse, SP_Pulse());
            SP_DrawNamedPic(x - 1, y - 1, 130, 130 - 14, "menu/art/maps_selected.tga");
            R_ClearColor();
        } else if (p->focus == ID_MAP0 + n) {
            SP_SetColor(pulse, SP_Pulse());
            SP_DrawNamedPic(x - 31, y - 30, 256, 256 - 27, "menu/art/maps_select.tga");
            R_ClearColor();
        }
    }

    if (sel) {
        int skill, best, secs;

        // the selected arena: name, frag limit, and the record
        Q_strlcpy(buf, sel->map, 20);
        Q_strupr(buf);
        SP_PropString(320, 192, va("%s: %s", buf, sel->longname), Q3S_CENTER | Q3S_SMALLFONT, c_orange, 1);
        if (sel->fraglimit)
            SP_CharString(18, 212, va("Frags %d", sel->fraglimit), Q3S_LEFT | Q3S_SMALLFONT, c_orange, 1);
        else if (sel->timelimit)
            SP_CharString(18, 212, va("Time %d", sel->timelimit), Q3S_LEFT | Q3S_SMALLFONT, c_orange, 1);

        best = SP_BestScore(sel->num, &skill);
        if (best && skill >= 1) {
            secs = SP_InfoInt(sp_times, va("t%d", sel->num));
            if (best == 1 && secs > 0)
                Q_snprintf(buf, sizeof(buf), "Best: %s on %s, %d:%02d", SP_Ordinal(best),
                           sp_skill_names[skill - 1], secs / 60, secs % 60);
            else
                Q_snprintf(buf, sizeof(buf), "Best: %s on %s", SP_Ordinal(best), sp_skill_names[skill - 1]);
            SP_CharString(622, 212, buf, Q3S_RIGHT | Q3S_SMALLFONT, c_orange, 1);
        }

        // the opponents
        y = 192 + 24;
        count = sel->numbots;
        for (n = 0; n < count; n++) {
            const char  *display;
            const char  *skin = SP_BotSkin(sel->bots[n], &display);
            qhandle_t   icon = skin ? SP_PlayerIcon(skin) : 0;
            float       pad = (7 - count) * (64 + 26) / 2.0f;

            x = 18 + pad + (64 + 26) * n;
            if (icon) {
                SP_DrawPic(x, y, 64, 64, icon);
            } else {
                SP_FillRect(x, y, 64, 64, c_black);
                SP_PropString(x + 22, y + 18, "?", Q3S_LEFT, c_orange, 1);
            }
            SP_CharString(x, y + 64, display, Q3S_SMALLFONT | Q3S_LEFT, c_orange, 1);
        }
    }

    SP_TrackMouse(p);
}

static void SP_OpenSkill(void);
static void SP_OpenReset(void);

static menuSound_t SP_Level_Key(menuFrameWork_t *self, int key)
{
    sp_page_t   *p = &m_level;
    bool        back;
    int         id;

    // the arrow keys step through the arenas and tiers, as Quake III's do
    if (key == K_LEFTARROW || key == K_KP_LEFTARROW || key == K_RIGHTARROW || key == K_KP_RIGHTARROW) {
        int dir = (key == K_LEFTARROW || key == K_KP_LEFTARROW) ? -1 : 1;
        int maps = SP_MapsInSet(sp_level.set);

        if (sp_level.set <= sp_level.current_set && sp_level.arena + dir >= 0 && sp_level.arena + dir < maps) {
            sp_level.arena += dir;
            SP_StoreSelection();
        } else if (sp_level.set + dir >= SP_MinTier() && sp_level.set + dir <= SP_MaxTier()) {
            SP_ChangeSet(dir);
            if (dir < 0 && sp_level.set <= sp_level.current_set)
                sp_level.arena = SP_MapsInSet(sp_level.set) - 1;
            SP_StoreSelection();
        } else {
            return QMS_BEEP;
        }
        p->focus = ID_MAP0 + sp_level.arena;
        SP_SoundQ3("misc/menu2.wav");
        return QMS_SILENT;
    }

    id = SP_PageKey(p, key, &back);
    if (back) {
        SP_SoundQ3("misc/menu3.wav");
        UI_PopMenu();
        return QMS_SILENT;
    }

    switch (id) {
    case ID_LEFTARROW:
        SP_ChangeSet(-1);
        break;
    case ID_RIGHTARROW:
        SP_ChangeSet(1);
        break;
    case ID_MAP0: case ID_MAP1: case ID_MAP2: case ID_MAP3:
        if (sp_level.arena == id - ID_MAP0 && key == K_ENTER) {
            SP_OpenSkill();
            return QMS_SILENT;
        }
        sp_level.arena = id - ID_MAP0;
        SP_StoreSelection();
        break;
    case ID_PLAYER: {
        menuFrameWork_t *m = UI_FindMenu("players");
        if (m)
            UI_PushMenu(m);
        break;
    }
    case ID_BACK:
        SP_SoundQ3("misc/menu3.wav");
        UI_PopMenu();
        return QMS_SILENT;
    case ID_RESET:
        SP_OpenReset();
        return QMS_SILENT;
    case ID_SKIRMISH:
        Cbuf_AddText(&cmd_buffer, "set _ui_botpack 0; bot_lineup; pushmenu botmatch\n");
        break;
    case ID_FIGHT:
        SP_OpenSkill();
        return QMS_SILENT;
    default:
        if (id >= ID_MEDAL0 && id <= ID_MEDAL5) {
            SP_SoundQ3(sp_medal_sounds[id - ID_MEDAL0]);
            return QMS_SILENT;
        }
        return QMS_NOTHANDLED;
    }

    SP_SoundQ3("misc/menu1.wav");
    return QMS_SILENT;
}

static bool SP_Level_Push(menuFrameWork_t *self)
{
    if (!SP_LoadArenas()) {
        Com_Printf("The campaign needs Quake III Arena's data (baseq3).\n");
        return false;
    }
    SP_SelectFromCvar();
    SP_PageOpened(&m_level);
    m_level.focus = ID_FIGHT;
    return true;
}

static void SP_Level_Expose(menuFrameWork_t *self)
{
    // back from the skill page or the player setup: the progress may have changed
    SP_SelectFromCvar();
}

static void SP_OpenLevelSelect(void)
{
    UI_PushMenu(&m_level.menu);
}

/*
==============================================================================

DIFFICULTY (ui_spskill.c)

==============================================================================
*/

static int sp_skill_choice;

static void SP_Skill_Draw(menuFrameWork_t *self)
{
    sp_page_t   *p = &m_skill;
    int         i;

    SP_FillScreen(c_black);
    SP_Screen(uis.width, uis.height);
    SP_BeginHot(p);

    SP_BannerString(320, 16, "DIFFICULTY", Q3S_CENTER, c_white);
    SP_DrawNamedPic(142, 118, 359, 256, "menu/art/cut_frame.tga");

    for (i = 0; i < SP_NUM_SKILLS; i++) {
        static const int ys[SP_NUM_SKILLS] = { 170, 198, 227, 255, 283 };
        const char  *name = sp_skill_names[i];
        float       w = SP_PropWidth(name);
        int         style = Q3S_CENTER;

        if (p->focus == ID_SKILL1 + i)
            style |= Q3S_PULSE;
        SP_PropString(320, ys[i], name, style, sp_skill_choice == i + 1 ? c_white : c_red, 1);
        SP_AddHot(p, ID_SKILL1 + i, 320 - w / 2, ys[i], w, PROP_HEIGHT);
    }

    SP_DrawNamedPic(320 - 64, 368, 128, 96, va("menu/art/skill%d.tga", sp_skill_choice));

    SP_Button(p, ID_BACK, 0, 480 - 64, 128, 64, "menu/art/back_0.tga", "menu/art/back_1.tga");
    SP_Button(p, ID_FIGHT, 640 - 128, 480 - 64, 128, 64, "menu/art/fight_0.tga", "menu/art/fight_1.tga");

    SP_TrackMouse(p);
}

static void SP_SetSkill(int skill)
{
    sp_skill_choice = SP_Clip(skill, 1, SP_NUM_SKILLS);
    Cvar_SetInteger(sp_skill, sp_skill_choice, FROM_CODE);
    if (sp_skill_choice == SP_NUM_SKILLS)
        SP_SoundQ3("misc/nightmare.wav");
}

static menuSound_t SP_Skill_Key(menuFrameWork_t *self, int key)
{
    sp_page_t   *p = &m_skill;
    sp_arena_t  *a;
    bool        back;
    int         id;

    id = SP_PageKey(p, key, &back);
    if (back) {
        SP_SoundQ3("misc/menu3.wav");
        UI_PopMenu();
        return QMS_SILENT;
    }

    switch (id) {
    case ID_BACK:
        SP_SoundQ3("misc/menu3.wav");
        UI_PopMenu();
        return QMS_SILENT;
    case ID_FIGHT:
        a = SP_ArenaInSet(sp_level.set, sp_level.arena);
        if (a)
            SP_StartArena(a->num, sp_skill_choice);
        return QMS_SILENT;
    default:
        if (id >= ID_SKILL1 && id <= ID_SKILL5) {
            if (sp_skill_choice == id - ID_SKILL1 + 1 && key == K_ENTER) {
                a = SP_ArenaInSet(sp_level.set, sp_level.arena);
                if (a)
                    SP_StartArena(a->num, sp_skill_choice);
                return QMS_SILENT;
            }
            SP_SetSkill(id - ID_SKILL1 + 1);
            if (sp_skill_choice != SP_NUM_SKILLS)
                SP_SoundQ3("misc/menu1.wav");
            return QMS_SILENT;
        }
        return QMS_NOTHANDLED;
    }
}

static bool SP_Skill_Push(menuFrameWork_t *self)
{
    sp_skill_choice = SP_Clip(sp_skill->integer, 1, SP_NUM_SKILLS);
    SP_PageOpened(&m_skill);
    m_skill.focus = ID_SKILL1 + sp_skill_choice - 1;
    return true;
}

static void SP_OpenSkill(void)
{
    SP_SoundQ3("misc/menu1.wav");
    UI_PushMenu(&m_skill.menu);
}

/*
==============================================================================

RESET GAME? (ui_spreset.c)

==============================================================================
*/

static void SP_Reset_Draw(menuFrameWork_t *self)
{
    sp_page_t   *p = &m_reset;
    float       l1, l2, l3;

    SP_FillScreen(c_black);
    SP_Screen(uis.width, uis.height);
    SP_BeginHot(p);

    SP_DrawNamedPic(142, 118, 359, 256, "menu/art/cut_frame.tga");
    SP_PropString(320, 194 + 10, "RESET GAME?", Q3S_CENTER | Q3S_INVERSE, c_red, 1);

    l1 = 320 - SP_PropWidth("YES/NO") / 2.0f;
    l2 = l1 + SP_PropWidth("YES") + PROP_GAP_WIDTH;
    l3 = l2 + SP_PropWidth("/") + PROP_GAP_WIDTH;
    SP_PropString(l1, 264, "YES", Q3S_LEFT | (p->focus == ID_YES ? Q3S_PULSE : 0), c_red, 1);
    SP_PropString(l2, 265, "/", Q3S_LEFT | Q3S_INVERSE, c_red, 1);
    SP_PropString(l3, 264, "NO", Q3S_LEFT | (p->focus == ID_NO ? Q3S_PULSE : 0), c_red, 1);
    SP_AddHot(p, ID_YES, l1, 264, SP_PropWidth("YES"), PROP_HEIGHT);
    SP_AddHot(p, ID_NO, l3, 264, SP_PropWidth("NO"), PROP_HEIGHT);

    SP_PropString(320, 356 + PROP_HEIGHT * 0, "WARNING: This resets all of the", Q3S_CENTER | Q3S_SMALLFONT, c_yellow, 1);
    SP_PropString(320, 356 + PROP_HEIGHT * 1, "single player game variables.", Q3S_CENTER | Q3S_SMALLFONT, c_yellow, 1);
    SP_PropString(320, 356 + PROP_HEIGHT * 2, "Do this only if you want to", Q3S_CENTER | Q3S_SMALLFONT, c_yellow, 1);
    SP_PropString(320, 356 + PROP_HEIGHT * 3, "start over from the beginning.", Q3S_CENTER | Q3S_SMALLFONT, c_yellow, 1);

    SP_TrackMouse(p);
}

static menuSound_t SP_Reset_Key(menuFrameWork_t *self, int key)
{
    sp_page_t   *p = &m_reset;
    bool        back;
    int         id;

    if (key == 'y' || key == 'Y') {
        id = ID_YES;
    } else if (key == 'n' || key == 'N') {
        id = ID_NO;
    } else {
        id = SP_PageKey(p, key, &back);
        if (back)
            id = ID_NO;
    }

    switch (id) {
    case ID_YES:
        SP_NewGame();
        UI_PopMenu();
        SP_SelectFromCvar();
        SP_SoundQ3("misc/menu1.wav");
        return QMS_SILENT;
    case ID_NO:
        UI_PopMenu();
        SP_SoundQ3("misc/menu3.wav");
        return QMS_SILENT;
    }
    return id < 0 ? QMS_SILENT : QMS_NOTHANDLED;
}

static bool SP_Reset_Push(menuFrameWork_t *self)
{
    SP_PageOpened(&m_reset);
    m_reset.focus = ID_NO;
    return true;
}

static void SP_OpenReset(void)
{
    SP_SoundQ3("misc/menu1.wav");
    UI_PushMenu(&m_reset.menu);
}

/*
==============================================================================

POSTGAME (ui_sppostgame.c)

==============================================================================
*/

#define AWARD_PRESENTATION_TIME 2000

static const int sp_medal_x[SP_NUM_AWARDS] = { 144, 448, 88, 504, 32, 560 };

static struct {
    int         num;            // the arena
    int         skill;
    int         rank;           // 1-based, ties stripped
    char        names[3][32];
    int         numclients;
    int         awards[SP_NUM_AWARDS];      // which, in the order earned
    int         levels[SP_NUM_AWARDS];
    bool        played[SP_NUM_AWARDS];
    int         numawards;
    int         won;            // the tier finished, or -1
    char        winner_sound[MAX_QPATH];
    int         phase;
    unsigned    start_time;
    unsigned    ignore_keys;
    bool        left;           // a video or the level select took over
} sp_post;

static bool sp_post_opening;    // inside UI_PushMenu of the postgame (SP_Postgame)
static bool     sp_post_photo;          // photo mode over it (UI_SP_PodiumPhoto*)
static unsigned sp_post_photo_time;     // since when

static void SP_Post_DrawMedals(int max)
{
    int n;

    for (n = 0; n < max && n < sp_post.numawards; n++) {
        int x = sp_medal_x[n], y = 64, amount = sp_post.levels[n];

        SP_DrawNamedPic(x, y, 48, 48, sp_medal_pics[sp_post.awards[n]]);
        if (sp_post.awards[n] == AWARD_ACCURACY)
            SP_PropString(x + 24, y + 52, va("%d%%", amount), Q3S_CENTER, c_yellow, 1);
        else if (amount != 1)
            SP_PropString(x + 24, y + 52, va("%d", amount), Q3S_CENTER, c_yellow, 1);
    }
}

// the end of the postgame: a tier done plays its video once, then CHOOSE LEVEL
static bool SP_Post_TierVideo(void)
{
    int last = SP_NumTiers() + 1;

    if (sp_post.won < 0 || !SP_ShowVideoOnce(va("tier%d", sp_post.won + 1)))
        return false;

    sp_post.left = true;
    OGG_Stop();
    UI_ForceMenuOff();
    if (sp_post.won == last) {
        Cbuf_AddText(&cmd_buffer, "disconnect\nsp_video end\n");
    } else {
        Cvar_SetInteger(sp_selection, sp_post.won * SP_ARENAS_PER_TIER, FROM_CODE);
        Cbuf_AddText(&cmd_buffer, va("disconnect\nsp_video tier%d levelselect\n", sp_post.won + 1));
    }
    return true;
}

static void SP_Post_Draw(menuFrameWork_t *self)
{
    sp_page_t   *p = &m_post;
    int         timer;

    SP_Screen(uis.width, uis.height);
    SP_BeginHot(p);

    if (sp_post.left)
        return;

    // the names under the podium
    if (sp_post.numclients > 2)
        SP_PropString(510, 480 - 64 - PROP_HEIGHT, sp_post.names[2], Q3S_CENTER, c_white, 1);
    if (sp_post.numclients > 1)
        SP_PropString(130, 480 - 64 - PROP_HEIGHT, sp_post.names[1], Q3S_CENTER, c_white, 1);
    SP_PropString(320, 480 - 64 - 2 * PROP_HEIGHT, sp_post.names[0], Q3S_CENTER, c_white, 1);

    if (sp_post.phase == 1) {
        timer = uis.realtime - sp_post.start_time;
        if (timer >= 1000 && sp_post.winner_sound[0]) {
            S_StartLocalSound(sp_post.winner_sound);
            sp_post.winner_sound[0] = 0;
        }
        if (timer < 5000)
            return;
        sp_post.phase = 2;
        sp_post.start_time = uis.realtime;
    }

    if (sp_post.phase == 2) {
        timer = uis.realtime - sp_post.start_time;
        if (timer >= sp_post.numawards * AWARD_PRESENTATION_TIME) {
            if (timer < 5000)
                return;
            sp_post.phase = 3;
            sp_post.start_time = uis.realtime;
        } else {
            int     n = timer / AWARD_PRESENTATION_TIME;
            float   fade = (float)(AWARD_PRESENTATION_TIME - timer % AWARD_PRESENTATION_TIME) /
                           AWARD_PRESENTATION_TIME;

            SP_PropString(320, 64, sp_medal_names[sp_post.awards[n]], Q3S_CENTER, c_white, fade);
            SP_Post_DrawMedals(n + 1);
            if (!sp_post.played[n]) {
                sp_post.played[n] = true;
                SP_SoundQ3(sp_medal_sounds[sp_post.awards[n]]);
            }
            return;
        }
    }

    // phase 3: a tier done goes to its video; else the buttons
    if (SP_Post_TierVideo())
        return;

    SP_Post_DrawMedals(sp_post.numawards);
    SP_Button(p, ID_MENU, 0, 480 - 64, 128, 64, "menu/art/menu_0.tga", "menu/art/menu_1.tga");
    SP_Button(p, ID_REPLAY, 320 - 64, 480 - 64, 128, 64, "menu/art/replay_0.tga", "menu/art/replay_1.tga");
    SP_Button(p, ID_NEXT, 640 - 128, 480 - 64, 128, 64, "menu/art/next_0.tga", "menu/art/next_1.tga");
    SP_TrackMouse(p);
}

static void SP_Post_Next(void)
{
    int level, set, current;

    // ui_sppostgame.c NextEvent: after the training arena, the first arena;
    // else the next, unless that is in a tier not open yet
    level = sp_post.won == 0 ? 0 : sp_post.num + 1;
    set = level / SP_ARENAS_PER_TIER;
    current = SP_CurrentGame();
    if (SP_IsTraining(sp_post.num) && sp_post.won != 0)
        level = current;
    else if (set > SP_SetOf(current) || set >= SP_NumTiers())
        level = current;
    SP_StartArena(level, sp_post.skill);
}

static menuSound_t SP_Post_Key(menuFrameWork_t *self, int key)
{
    sp_page_t   *p = &m_post;
    bool        back;
    int         id;

    if (uis.realtime < sp_post.ignore_keys || sp_post.left)
        return QMS_SILENT;

    // a key hurries the podium, then the medals, along
    if (sp_post.phase == 1 || sp_post.phase == 2) {
        sp_post.phase++;
        sp_post.start_time = uis.realtime;
        sp_post.ignore_keys = uis.realtime + 250;
        return QMS_SILENT;
    }

    id = SP_PageKey(p, key, &back);
    if (back)
        return QMS_SILENT;     // Quake III kept you here

    switch (id) {
    case ID_MENU:
        sp_post.left = true;
        OGG_Stop();
        UI_ForceMenuOff();
        Cbuf_AddText(&cmd_buffer, "disconnect\nlevelselect\n");
        return QMS_SILENT;
    case ID_REPLAY:
        sp_post.left = true;
        SP_StartArena(sp_post.num, sp_post.skill);
        return QMS_SILENT;
    case ID_NEXT:
        sp_post.left = true;
        SP_Post_Next();
        return QMS_SILENT;
    }
    return id < 0 ? QMS_SILENT : QMS_NOTHANDLED;
}

static void SP_Post_Pop(menuFrameWork_t *self)
{
    if (!sp_post.left)
        OGG_Stop();
}

/*
=================
SP_Postgame

"sp_event postgame <map> <place> <tied> <accuracy> <impressive> <excellent>
<gauntlet> <frags> <perfect> <seconds> <skill> <winner voice> <players>
<first> <second> <third>" (g_sp.c SP_Intermission): the arena's result, kept
(UI_SPPostgameMenu_f), then the postgame screen over the podium.
=================
*/
static void SP_Postgame(void)
{
    sp_arena_t  *a;
    int         accuracy, impressive, excellent, gauntlet, frags, perfect, secs, n, old_frags, new_frags;
    const char  *voice;

    if (Cmd_Argc() < 15 || !SP_LoadArenas())
        return;
    a = SP_ArenaByMap(Cmd_Argv(2));
    if (!a)
        return;

    memset(&sp_post, 0, sizeof(sp_post));
    sp_post_photo = false;
    sp_post.num = a->num;
    sp_post.rank = SP_Clip(atoi(Cmd_Argv(3)), 1, 8);
    accuracy = atoi(Cmd_Argv(5));
    impressive = atoi(Cmd_Argv(6));
    excellent = atoi(Cmd_Argv(7));
    gauntlet = atoi(Cmd_Argv(8));
    frags = atoi(Cmd_Argv(9));
    perfect = atoi(Cmd_Argv(10));
    secs = atoi(Cmd_Argv(11));
    sp_post.skill = SP_Clip(atoi(Cmd_Argv(12)), 1, SP_NUM_SKILLS);
    voice = Cmd_Argv(13);
    sp_post.numclients = atoi(Cmd_Argv(14));
    for (n = 0; n < 3; n++)
        Q_strlcpy(sp_post.names[n], Cmd_Argv(15 + n), sizeof(sp_post.names[n]));

    SP_SetBestScore(a->num, sp_post.rank, sp_post.skill);
    if (sp_post.rank == 1 && secs > 0) {
        int old = SP_InfoInt(sp_times, va("t%d", a->num));
        if (!old || secs < old)
            SP_InfoSetInt(sp_times, va("t%d", a->num), secs);
    }

    // the medals, logged to the totals and lined up for the presentation
    n = 0;
    if (accuracy >= 50) {
        SP_LogAward(AWARD_ACCURACY, 1);
        sp_post.awards[n] = AWARD_ACCURACY;
        sp_post.levels[n++] = accuracy;
    }
    if (impressive) {
        SP_LogAward(AWARD_IMPRESSIVE, impressive);
        sp_post.awards[n] = AWARD_IMPRESSIVE;
        sp_post.levels[n++] = impressive;
    }
    if (excellent) {
        SP_LogAward(AWARD_EXCELLENT, excellent);
        sp_post.awards[n] = AWARD_EXCELLENT;
        sp_post.levels[n++] = excellent;
    }
    if (gauntlet) {
        SP_LogAward(AWARD_GAUNTLET, gauntlet);
        sp_post.awards[n] = AWARD_GAUNTLET;
        sp_post.levels[n++] = gauntlet;
    }
    old_frags = SP_AwardLevel(AWARD_FRAGS) / 100;
    SP_LogAward(AWARD_FRAGS, frags);
    new_frags = SP_AwardLevel(AWARD_FRAGS) / 100;
    if (new_frags > old_frags) {
        sp_post.awards[n] = AWARD_FRAGS;
        sp_post.levels[n++] = new_frags * 100;
    }
    if (perfect) {
        SP_LogAward(AWARD_PERFECT, 1);
        sp_post.awards[n] = AWARD_PERFECT;
        sp_post.levels[n++] = 1;
    }
    sp_post.numawards = n;

    sp_post.won = sp_post.rank == 1 ? SP_TierCompleted(a->num) : -1;

    // the announcer names the winner, and the music says how it went
    if (sp_post.rank == 1 || !Q_stricmp(voice, "you"))
        Q_strlcpy(sp_post.winner_sound, "baseq3/sound/player/announce/youwin.wav", sizeof(sp_post.winner_sound));
    else
        Q_snprintf(sp_post.winner_sound, sizeof(sp_post.winner_sound),
                   "baseq3/sound/player/announce/%s_wins.wav", voice);
    if (FS_FileExists(sp_post.winner_sound)) {
        char path[MAX_QPATH];
        Q_concat(path, sizeof(path), "#", sp_post.winner_sound);
        Q_strlcpy(sp_post.winner_sound, path, sizeof(sp_post.winner_sound));
    } else {
        sp_post.winner_sound[0] = 0;
    }
    OGG_PlayMusicFiles(sp_post.rank == 1 ? "baseq3/music/win.wav" : "baseq3/music/loss.wav", NULL);

    sp_post.phase = 1;
    sp_post.start_time = uis.realtime ? uis.realtime : cls.realtime;
    sp_post.ignore_keys = sp_post.start_time + 1500;

    // UI_PushMenu turns the keys over to the menu (Key_SetDest, which asks
    // CL_CheckForPause at once) BEFORE it makes this the active menu: asked
    // then, UI_SP_KeepsGameRunning saw no postgame and the client paused. Alone
    // on its own server (the bots are not connections), the server paused with
    // it, no frame came again to ask a second time, and the podium stood still
    // with the last teleport burst hanging in the air (11-19).
    sp_post_opening = true;
    UI_PushMenu(&m_post.menu);
    sp_post_opening = false;
    SP_PageOpened(&m_post);
    m_post.focus = ID_NEXT;
    CL_CheckForPause();     // and should anything have paused it meanwhile
}

bool UI_SP_KeepsGameRunning(void)
{
    if (!uis.initialized || !(Key_GetDest() & KEY_MENU))
        return false;
    return sp_post_opening || uis.activeMenu == &m_post.menu;
}

/*
=================
UI_SP_PodiumPhoto*

Photo mode over the podium (11-19). The postgame screen holds the keys, so the
pause key - photo mode - only ever reached it as "hurry along" (and on a map
that finishes a tier, hurried it into the tier video). Now the key bound to
"pause" sets the screen aside: the menu gives up the keys but stays the active
menu, so photo mode runs exactly as it does in play - its free camera and
mouse, its own text from the renderer - while the screen is not drawn (UI_Draw
draws only with KEY_MENU), nor the HUD and notify lines (SCR_Draw2D,
Con_DrawNotify). Pause again, or Escape, ends it, and the screen comes back as
it was: its clock is held while it is away, so the medals pick up where they
were and no tier video starts behind the photo.
=================
*/
static bool SP_PostgameUp(void)
{
    return uis.initialized && uis.activeMenu == &m_post.menu && !sp_post.left && cls.state == ca_active;
}

// asked every frame by the HUD: checked against the screen itself, so a flag
// left behind by a level that went away mid-photo can never hide a later HUD
bool UI_SP_PodiumPhoto(void)
{
    return sp_post_photo && cl_paused->integer == 2 && SP_PostgameUp();
}

bool UI_SP_PodiumPhotoStart(void)
{
    if (UI_SP_PodiumPhoto() || !SP_PostgameUp())
        return false;

    // paused first: the key handover below asks CL_CheckForPause, which
    // would take a photo mode that has not begun yet as one that has ended
    Cvar_Set("cl_paused", "2");
    sp_post_photo = true;
    sp_post_photo_time = cls.realtime;
    Key_SetDest(Key_GetDest() & ~KEY_MENU);
    CL_CheckForPause();
    return true;
}

void UI_SP_PodiumPhotoEnd(void)
{
    if (!UI_SP_PodiumPhoto())
        return;
    Cvar_Set("cl_paused", "0");
    CL_CheckForPause();     // which brings the screen back
}

void UI_SP_PodiumPhotoCheck(void)
{
    unsigned away;

    if (!sp_post_photo || (cl_paused->integer == 2 && cls.state == ca_active))
        return;

    sp_post_photo = false;
    if (!SP_PostgameUp())
        return;     // the level went away meanwhile (a disconnect, a map)

    away = cls.realtime - sp_post_photo_time;
    sp_post.start_time += away;
    sp_post.ignore_keys += away;
    Key_SetDest(Key_GetDest() | KEY_MENU);  // an open console keeps the keys till it closes
}

/*
==============================================================================

IN THE ARENA: REWARDS AND THE ANNOUNCER (cg_draw.c CG_DrawReward,
cg_playerstate.c CG_CheckLocalSounds, cg_main.c CG_PlayBufferedSounds)

==============================================================================
*/

#define REWARD_TIME     3000
#define REWARD_FADE     200
#define ICON_SIZE       48
#define MAX_REWARDS     10
#define MAX_BUFFERED    8

typedef struct {
    char    sound[MAX_QPATH];
    char    medal[MAX_QPATH];
    int     count;
} sp_reward_t;

static struct {
    sp_reward_t stack[MAX_REWARDS];
    int         numstack;
    sp_reward_t shown;
    bool        showing;
    unsigned    time;

    char        buffered[MAX_BUFFERED][MAX_QPATH];
    int         in, out;
    unsigned    sound_time;
} sp_hud;

static void SP_BufferSound(const char *rel)
{
    int next = (sp_hud.in + 1) % MAX_BUFFERED;

    if (next == sp_hud.out)
        return;
    Q_strlcpy(sp_hud.buffered[sp_hud.in], rel, MAX_QPATH);
    sp_hud.in = next;
}

static void SP_PushReward(const char *sound, const char *medal, int count)
{
    sp_reward_t *r;

    if (sp_hud.numstack >= MAX_REWARDS)
        return;
    r = &sp_hud.stack[sp_hud.numstack++];
    Q_strlcpy(r->sound, sound, sizeof(r->sound));
    Q_strlcpy(r->medal, medal, sizeof(r->medal));
    r->count = count;
}

static void SP_ClearHUD(void)
{
    memset(&sp_hud, 0, sizeof(sp_hud));
}

void UI_SP_DrawHUD(void)
{
    float   alpha, x, y;
    int     i, t;

    if (cls.state != ca_active || !uis.initialized)
        return;

    if (!sp_hud.showing || cls.realtime - sp_hud.time >= REWARD_TIME) {
        sp_hud.showing = false;
        if (!sp_hud.numstack)
            return;
        sp_hud.shown = sp_hud.stack[0];
        memmove(sp_hud.stack, sp_hud.stack + 1, --sp_hud.numstack * sizeof(sp_hud.stack[0]));
        sp_hud.showing = true;
        sp_hud.time = cls.realtime;
        SP_SoundQ3(sp_hud.shown.sound);
    }

    t = cls.realtime - sp_hud.time;
    alpha = t > REWARD_TIME - REWARD_FADE ? (REWARD_TIME - t) / (float)REWARD_FADE : 1.0f;

    R_SetScale(1.0f);
    SP_Screen(r_config.width, r_config.height);
    {
        float white[4] = { 1, 1, 1, 1 };
        qhandle_t pic = SP_Pic(sp_hud.shown.medal);

        SP_SetColor(white, alpha);
        y = 56;
        if (sp_hud.shown.count >= 10) {
            x = 320 - ICON_SIZE / 2;
            SP_DrawPic(x, y, ICON_SIZE - 4, ICON_SIZE - 4, pic);
            SP_CharString(320, y + ICON_SIZE, va("%d", sp_hud.shown.count), Q3S_CENTER | Q3S_SMALLFONT,
                          white, alpha);
        } else {
            x = 320 - sp_hud.shown.count * ICON_SIZE / 2;
            for (i = 0; i < sp_hud.shown.count; i++, x += ICON_SIZE) {
                SP_SetColor(white, alpha);
                SP_DrawPic(x, y, ICON_SIZE - 4, ICON_SIZE - 4, pic);
            }
        }
        R_ClearColor();
    }
}

void UI_SP_Frame(void)
{
    // a level's rewards stay in it
    if (cls.state != ca_active && (sp_hud.numstack || sp_hud.showing || sp_hud.in != sp_hud.out))
        SP_ClearHUD();

    // nobody plays the campaign's rules out of the campaign: the game reads
    // sp_campaign as a map loads, and every start of an arena sets it again
    if (cls.state == ca_disconnected && sp_campaign && sp_campaign->integer)
        Cvar_SetByVar(sp_campaign, "0", FROM_CODE);

    // the announcer, one line at a time
    if (sp_hud.out != sp_hud.in && cls.realtime >= sp_hud.sound_time) {
        SP_SoundQ3(sp_hud.buffered[sp_hud.out]);
        sp_hud.out = (sp_hud.out + 1) % MAX_BUFFERED;
        sp_hud.sound_time = cls.realtime + 750;
    }

    if (RoQ_IsOpen() && uis.activeMenu == &m_video.menu) {
        if (!RoQ_Update())
            SP_VideoFinished();
    }
}

/*
=================
SP_Event_f

"sp_event <what> ..." - the game's side of the campaign talking (g_sp.c).
=================
*/
static void SP_Event_f(void)
{
    const char *what = Cmd_Argv(1);

    if (!Q_stricmp(what, "reward")) {
        const char  *award = Cmd_Argv(2);
        int         count = atoi(Cmd_Argv(3));

        if (!Q_stricmp(award, "excellent"))
            SP_PushReward("feedback/excellent.wav", "menu/medals/medal_excellent.tga", count);
        else if (!Q_stricmp(award, "impressive"))
            SP_PushReward("feedback/impressive.wav", "menu/medals/medal_impressive.tga", count);
        else if (!Q_stricmp(award, "gauntlet"))
            SP_PushReward("feedback/humiliation.wav", "menu/medals/medal_gauntlet.tga", count);
        return;
    }
    if (!Q_stricmp(what, "humiliation")) {
        SP_BufferSound("feedback/humiliation.wav");
        return;
    }
    if (!Q_stricmp(what, "announce")) {
        char rel[MAX_QPATH];
        Q_concat(rel, sizeof(rel), "player/announce/", Cmd_Argv(2), ".wav");
        SP_SoundQ3(rel);
        return;
    }
    if (!Q_stricmp(what, "lead")) {
        const char *how = Cmd_Argv(2);
        if (!Q_stricmp(how, "taken"))
            SP_BufferSound("feedback/takenlead.wav");
        else if (!Q_stricmp(how, "tied"))
            SP_BufferSound("feedback/tiedlead.wav");
        else if (!Q_stricmp(how, "lost"))
            SP_BufferSound("feedback/lostlead.wav");
        return;
    }
    if (!Q_stricmp(what, "fragsleft")) {
        int n = atoi(Cmd_Argv(2));
        if (n == 1)
            SP_BufferSound("feedback/1_frag.wav");
        else if (n == 2)
            SP_BufferSound("feedback/2_frags.wav");
        else if (n == 3)
            SP_BufferSound("feedback/3_frags.wav");
        return;
    }
    if (!Q_stricmp(what, "timeleft")) {
        int n = atoi(Cmd_Argv(2));
        SP_BufferSound(n == 1 ? "feedback/1_minute.wav" : "feedback/5_minute.wav");
        return;
    }
    if (!Q_stricmp(what, "postgame")) {
        SP_ClearHUD();
        SP_Postgame();
        return;
    }
}

/*
=================
SP_LevelSelect_f

"levelselect": CHOOSE LEVEL - the first time, Quake III's intro before it.
=================
*/
static void SP_LevelSelect_f(void)
{
    if (!SP_LoadArenas()) {
        Com_Printf("The campaign needs Quake III Arena's data (baseq3).\n");
        return;
    }
    if (!SP_InfoInt(sp_videos, "intro") && FS_FileExists("baseq3/video/intro.roq")) {
        SP_InfoSetInt(sp_videos, "intro", 1);
        SP_PlayVideo("intro", SP_AFTER_LEVELSELECT);
        return;
    }
    SP_OpenLevelSelect();
}

// "sp_video <name> [levelselect]": one of Quake III's videos (video/<name>.RoQ)
static void SP_Video_f(void)
{
    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s <intro|tier1..tier7|end> [levelselect]\n", Cmd_Argv(0));
        return;
    }
    SP_LoadArenas();
    SP_PlayVideo(Cmd_Argv(1), !Q_stricmp(Cmd_Argv(2), "levelselect") ? SP_AFTER_LEVELSELECT : SP_AFTER_NOTHING);
}

static const cmdreg_t c_q3sp[] = {
    { "levelselect", SP_LevelSelect_f },
    { "sp_event", SP_Event_f },
    { "sp_video", SP_Video_f },
    { NULL }
};

static void SP_Free(menuFrameWork_t *self)
{
    Cmd_Deregister(c_q3sp);
    RoQ_Close();
    SP_ClearHUD();
    sp_loaded = false;
}

void M_Menu_Q3SP(void)
{
    int i;

    for (i = 0; i < SP_NUM_SKILLS; i++)
        sp_scores[i] = Cvar_Get(va("sp_scores%d", i + 1), "", CVAR_ARCHIVE);
    sp_awards = Cvar_Get("sp_awards", "", CVAR_ARCHIVE);
    sp_videos = Cvar_Get("sp_videos", "", CVAR_ARCHIVE);
    sp_times = Cvar_Get("sp_times", "", CVAR_ARCHIVE);
    sp_selection = Cvar_Get("sp_selection", "", CVAR_ARCHIVE);
    sp_skill = Cvar_Get("sp_skill", "2", CVAR_ARCHIVE);
    sp_campaign = Cvar_Get("sp_campaign", "0", 0);

    SP_InitPage(&m_level, "levelselect", false);
    m_level.menu.draw = SP_Level_Draw;
    m_level.menu.keydown = SP_Level_Key;
    m_level.menu.push = SP_Level_Push;
    m_level.menu.expose = SP_Level_Expose;
    m_level.menu.free = SP_Free;
    List_Append(&ui_menus, &m_level.menu.entry);

    SP_InitPage(&m_skill, "spskill", false);
    m_skill.menu.draw = SP_Skill_Draw;
    m_skill.menu.keydown = SP_Skill_Key;
    m_skill.menu.push = SP_Skill_Push;
    List_Append(&ui_menus, &m_skill.menu.entry);

    SP_InitPage(&m_reset, "spreset", false);
    m_reset.menu.draw = SP_Reset_Draw;
    m_reset.menu.keydown = SP_Reset_Key;
    m_reset.menu.push = SP_Reset_Push;
    List_Append(&ui_menus, &m_reset.menu.entry);

    // over the podium, which goes on moving behind it
    SP_InitPage(&m_post, "postgame", true);
    m_post.menu.clear_view = true;
    m_post.menu.draw = SP_Post_Draw;
    m_post.menu.keydown = SP_Post_Key;
    m_post.menu.pop = SP_Post_Pop;
    List_Append(&ui_menus, &m_post.menu.entry);

    SP_InitPage(&m_video, "spvideo", false);
    m_video.menu.draw = SP_Video_Draw;
    m_video.menu.keydown = SP_Video_Key;
    m_video.menu.pop = SP_Video_Pop;
    List_Append(&ui_menus, &m_video.menu.entry);

    Cmd_Register(c_q3sp);
}
