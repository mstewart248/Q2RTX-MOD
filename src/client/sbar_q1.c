/*
Copyright (C) 1996-1997 Id Software, Inc.
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
// sbar_q1.c -- Quake 1's status bar, under its own game code (CS_Q1VM)
//
// Quake's sbar.c: the bar, the inventory above it, the mission packs' versions
// of both (Scourge of Armagon's, Dissolution of Eternity's), the solo scoreboard
// in the bar's place when the player is dead, and the intermission and finale
// overlays. Drawn from what the game puts in STAT_Q1_* (g_q1vm_game.c
// q1_hud_stats) and the pictures of the episode's gfx.wad, which the image
// loader cuts out of it (refresh/images.c load_q1_pic).
//
// What Quake's client worked out for itself, this does too: when each item
// arrived (they flash for a second) and the pain face after a hit.

#include "client.h"

// Quake's items (quakedef.h): the bits of STAT_Q1_ITEMS
#define IT_SHOTGUN              1
#define IT_LIGHTNING            64
#define IT_SUPER_LIGHTNING      128
#define IT_SHELLS               256
#define IT_NAILS                512
#define IT_ROCKETS              1024
#define IT_CELLS                2048
#define IT_ARMOR1               8192
#define IT_ARMOR2               16384
#define IT_ARMOR3               32768
#define IT_KEY1                 131072
#define IT_KEY2                 262144
#define IT_INVISIBILITY         524288
#define IT_INVULNERABILITY      1048576
#define IT_QUAD                 4194304

// Dissolution of Eternity's, items2 mixed in at bit 23
#define RIT_SHELLS              128
#define RIT_NAILS               256
#define RIT_ROCKETS             512
#define RIT_CELLS               1024
#define RIT_LAVA_NAILGUN        4096
#define RIT_ARMOR1              8388608
#define RIT_ARMOR2              16777216
#define RIT_ARMOR3              33554432
#define RIT_LAVA_NAILS          67108864
#define RIT_PLASMA_AMMO         134217728
#define RIT_MULTI_ROCKETS       268435456

// Scourge of Armagon's
#define HIT_PROXIMITY_GUN_BIT   16
#define HIT_MJOLNIR_BIT         7
#define HIT_LASER_CANNON_BIT    23
#define HIT_PROXIMITY_GUN       (1 << HIT_PROXIMITY_GUN_BIT)

#define SBAR_HEIGHT             24

typedef enum {
    SB_ID1,
    SB_HIPNOTIC,
    SB_ROGUE
} sbar_kind_t;

static struct {
    char        episode[16];    // whose pictures these are; "" = none yet
    sbar_kind_t kind;

    qhandle_t   sbar, ibar, scorebar, disc, colon, slash;
    qhandle_t   nums[2][11];    // [red][digit, minus]
    qhandle_t   weapons[7][7];  // [flash][weapon]: inv_, inv2_, inva1_ .. inva5_
    qhandle_t   ammo[4], armor[3], itempics[6], sigil[4];
    qhandle_t   faces[5][2];    // [health / 20][pain]
    qhandle_t   face_invis, face_invuln, face_invis_invuln, face_quad;
    qhandle_t   hweapons[7][5], hitems[2];
    qhandle_t   rinvbar[2], rweapons[5], ritems[2], rammo[3];
    qhandle_t   conchars;
    qhandle_t   complete, inter, finale;

    // CL_ParseClientdata and V_ParseDamage's
    uint32_t    items;
    float       item_gettime[32];
    int         hits;           // STAT_Q1_HITS as last seen...
    int         hits_server;    // ...on this cl.servercount
    float       faceanimtime;
} sb;

static int sb_x, sb_y;      // Quake's (0, 0): the bar's left, its top

static qhandle_t sb_register(const char *name)
{
    return R_RegisterPic(va("/id1/%s/gfx/%s.lmp", sb.episode, name));
}

/*
===============
SCR_Q1RegisterMedia

Sbar_Init's pictures, for the episode CS_Q1VM names - at every level's
CL_PrepRefresh, and on the first frame after a renderer restart. A new level
starts with no items, so the ones carried in flash, as they did in Quake.
===============
*/
void SCR_Q1RegisterMedia(void)
{
    static const char *const weapons[7] = {
        "shotgun", "sshotgun", "nailgun", "snailgun", "rlaunch", "srlaunch", "lightng"
    };
    static const char *const hweapons[5] = {
        "laser", "mjolnir", "gren_prox", "prox_gren", "prox"
    };
    static const char *const flashes[7] = {
        "inv_", "inv2_", "inva1_", "inva2_", "inva3_", "inva4_", "inva5_"
    };
    static const char *const ammo[4] = { "sb_shells", "sb_nails", "sb_rocket", "sb_cells" };
    static const char *const items[6] = {
        "sb_key1", "sb_key2", "sb_invis", "sb_invuln", "sb_suit", "sb_quad"
    };
    static const char *const rweapons[5] = {
        "r_lava", "r_superlava", "r_gren", "r_multirock", "r_plasma"
    };
    int i, j;

    memset(&sb, 0, sizeof(sb));
    if (!cl.q1vm[0])
        return;

    Q_strlcpy(sb.episode, cl.q1vm, sizeof(sb.episode));
    if (!Q_stricmp(sb.episode, "hipnotic"))
        sb.kind = SB_HIPNOTIC;
    else if (!Q_stricmp(sb.episode, "rogue"))
        sb.kind = SB_ROGUE;
    else
        sb.kind = SB_ID1;

    for (i = 0; i < 10; i++) {
        sb.nums[0][i] = sb_register(va("num_%i", i));
        sb.nums[1][i] = sb_register(va("anum_%i", i));
    }
    sb.nums[0][10] = sb_register("num_minus");
    sb.nums[1][10] = sb_register("anum_minus");
    sb.colon = sb_register("num_colon");
    sb.slash = sb_register("num_slash");

    for (i = 0; i < 7; i++)
        for (j = 0; j < 7; j++)
            sb.weapons[i][j] = sb_register(va("%s%s", flashes[i], weapons[j]));

    for (i = 0; i < 4; i++) {
        sb.ammo[i] = sb_register(ammo[i]);
        sb.sigil[i] = sb_register(va("sb_sigil%i", i + 1));
    }
    for (i = 0; i < 3; i++)
        sb.armor[i] = sb_register(va("sb_armor%i", i + 1));
    for (i = 0; i < 6; i++)
        sb.itempics[i] = sb_register(items[i]);

    for (i = 0; i < 5; i++) {
        sb.faces[4 - i][0] = sb_register(va("face%i", i + 1));
        sb.faces[4 - i][1] = sb_register(va("face_p%i", i + 1));
    }
    sb.face_invis = sb_register("face_invis");
    sb.face_invuln = sb_register("face_invul2");
    sb.face_invis_invuln = sb_register("face_inv2");
    sb.face_quad = sb_register("face_quad");

    sb.sbar = sb_register("sbar");
    sb.ibar = sb_register("ibar");
    sb.scorebar = sb_register("scorebar");
    sb.disc = sb_register("disc");

    if (sb.kind == SB_HIPNOTIC) {
        for (i = 0; i < 7; i++)
            for (j = 0; j < 5; j++)
                sb.hweapons[i][j] = sb_register(va("%s%s", flashes[i], hweapons[j]));
        sb.hitems[0] = sb_register("sb_wsuit");
        sb.hitems[1] = sb_register("sb_eshld");
    }

    if (sb.kind == SB_ROGUE) {
        sb.rinvbar[0] = sb_register("r_invbar1");
        sb.rinvbar[1] = sb_register("r_invbar2");
        for (i = 0; i < 5; i++)
            sb.rweapons[i] = sb_register(rweapons[i]);
        sb.ritems[0] = sb_register("r_shield1");
        sb.ritems[1] = sb_register("r_agrav1");
        sb.rammo[0] = sb_register("r_ammolava");
        sb.rammo[1] = sb_register("r_ammomulti");
        sb.rammo[2] = sb_register("r_ammoplasma");
    }

    sb.conchars = R_RegisterFont(va("/id1/%s/gfx/conchars.lmp", sb.episode));
    sb.complete = sb_register("complete");
    sb.inter = sb_register("inter");
    sb.finale = sb_register("finale");
}

// a renderer restart freed them all
void SCR_Q1ClearMedia(void)
{
    sb.episode[0] = 0;
}

/*
===============================================================================

DRAWING, AS SBAR.C DID

Coordinates are Quake's: x across the 320 of the bar, y from the bar's top
(the inventory at -24).

===============================================================================
*/

static void sb_pic(int x, int y, qhandle_t pic)
{
    if (pic)
        R_DrawPic(sb_x + x, sb_y + y, pic);
}

static void sb_char(int x, int y, int ch)
{
    R_DrawChar(sb_x + x + 4, sb_y + y, 0, ch, sb.conchars);
}

static void sb_string(int x, int y, const char *s)
{
    for (; *s; s++, x += 8)
        R_DrawChar(sb_x + x, sb_y + y, 0, (byte)*s, sb.conchars);
}

// Sbar_DrawNum: right-aligned in (digits) places, the last ones if it has more
static void sb_num_at(int x, int y, int num, int digits, int color)
{
    char str[16];
    const char *ptr = str;
    int l = Q_snprintf(str, sizeof(str), "%i", num);

    if (l > digits)
        ptr += l - digits;
    if (l < digits)
        x += (digits - l) * 24;

    for (; *ptr; ptr++, x += 24) {
        const qhandle_t pic = sb.nums[color][*ptr == '-' ? 10 : *ptr - '0'];
        if (pic)
            R_DrawPic(x, y, pic);
    }
}

static void sb_num(int x, int y, int num, int digits, int color)
{
    sb_num_at(sb_x + x, sb_y + y, num, digits, color);
}

// 0 to 6: inv_, inv2_ (the one in hand) or a flash of a new one
static int sb_flash(int bit, bool active, float time)
{
    int flashon = (int)((time - sb.item_gettime[bit]) * 10);

    if (flashon >= 10)
        return active ? 1 : 0;
    if (flashon < 0)
        flashon = 0;
    return (flashon % 5) + 2;
}

static void sb_inventory(uint32_t items, uint32_t weapon, const short *stats, float time)
{
    int i;

    if (sb.kind == SB_ROGUE)
        sb_pic(0, -24, sb.rinvbar[weapon >= RIT_LAVA_NAILGUN ? 0 : 1]);
    else
        sb_pic(0, -24, sb.ibar);

    // weapons
    for (i = 0; i < 7; i++) {
        if (items & (IT_SHOTGUN << i))
            sb_pic(i * 24, -16, sb.weapons[sb_flash(i, weapon == (uint32_t)(IT_SHOTGUN << i), time)][i]);
    }

    // Scourge of Armagon's: the laser cannon and Mjolnir beside the lightning
    // gun, the proximity gun sharing the grenade launcher's place
    if (sb.kind == SB_HIPNOTIC) {
        static const int hipweapons[4] = {
            HIT_LASER_CANNON_BIT, HIT_MJOLNIR_BIT, 4, HIT_PROXIMITY_GUN_BIT
        };
        bool grenadeflashing = false;

        for (i = 0; i < 4; i++) {
            const int bit = hipweapons[i];
            if (!(items & (1u << bit)))
                continue;

            const int flashon = sb_flash(bit, weapon == (1u << bit), time);
            if (i == 2) {
                if ((items & HIT_PROXIMITY_GUN) && flashon) {
                    grenadeflashing = true;
                    sb_pic(96, -16, sb.hweapons[flashon][2]);
                }
            } else if (i == 3) {
                if (items & (IT_SHOTGUN << 4)) {
                    if (!grenadeflashing)
                        sb_pic(96, -16, sb.hweapons[flashon][3]);
                } else {
                    sb_pic(96, -16, sb.hweapons[flashon][4]);
                }
            } else {
                sb_pic(176 + i * 24, -16, sb.hweapons[flashon][i]);
            }
        }
    }

    // Dissolution of Eternity's powered-up weapons over the ones they replace
    if (sb.kind == SB_ROGUE && weapon >= RIT_LAVA_NAILGUN) {
        for (i = 0; i < 5; i++) {
            if (weapon == (uint32_t)(RIT_LAVA_NAILGUN << i))
                sb_pic((i + 2) * 24, -16, sb.rweapons[i]);
        }
    }

    // ammo counts, in the font's small numbers
    for (i = 0; i < 4; i++) {
        char num[8];
        int count = stats[STAT_Q1_SHELLS + i];

        clamp(count, 0, 999);
        Q_snprintf(num, sizeof(num), "%3i", count);
        for (int k = 0; k < 3; k++)
            if (num[k] != ' ')
                sb_char((6 * i + 1 + k) * 8 - 2, -24, 18 + num[k] - '0');
    }

    // items (Scourge of Armagon shows its keys on the bar instead)
    for (i = 0; i < 6; i++) {
        if ((items & (1u << (17 + i))) && (sb.kind != SB_HIPNOTIC || i > 1))
            sb_pic(192 + i * 16, -16, sb.itempics[i]);
    }

    if (sb.kind == SB_HIPNOTIC) {
        // the wetsuit and the empathy shields
        for (i = 0; i < 2; i++)
            if (items & (1u << (24 + i)))
                sb_pic(288 + i * 16, -16, sb.hitems[i]);
    }

    if (sb.kind == SB_ROGUE) {
        // the shield and the antigrav belt
        for (i = 0; i < 2; i++)
            if (items & (1u << (29 + i)))
                sb_pic(288 + i * 16, -16, sb.ritems[i]);
    } else {
        // the runes
        for (i = 0; i < 4; i++)
            if (items & (1u << (28 + i)))
                sb_pic(320 - 32 + i * 8, -16, sb.sigil[i]);
    }
}

static void sb_face(uint32_t items, int health, float time)
{
    int f;

    if ((items & (IT_INVISIBILITY | IT_INVULNERABILITY)) == (IT_INVISIBILITY | IT_INVULNERABILITY)) {
        sb_pic(112, 0, sb.face_invis_invuln);
        return;
    }
    if (items & IT_QUAD) {
        sb_pic(112, 0, sb.face_quad);
        return;
    }
    if (items & IT_INVISIBILITY) {
        sb_pic(112, 0, sb.face_invis);
        return;
    }
    if (items & IT_INVULNERABILITY) {
        sb_pic(112, 0, sb.face_invuln);
        return;
    }

    f = health >= 100 ? 4 : health / 20;
    clamp(f, 0, 4);
    sb_pic(112, 0, sb.faces[f][time <= sb.faceanimtime ? 1 : 0]);
}

static void sb_solo_scoreboard(const short *stats)
{
    char str[80];
    const char *name = cl.configstrings[CS_NAME];
    const int t = stats[STAT_Q1_TIME];
    const int minutes = t / 60, seconds = t - 60 * minutes;

    Q_snprintf(str, sizeof(str), "Monsters:%3i /%3i", stats[STAT_Q1_MONSTERS], stats[STAT_Q1_TOTALMONSTERS]);
    sb_string(8, 4, str);
    Q_snprintf(str, sizeof(str), "Secrets :%3i /%3i", stats[STAT_Q1_SECRETS], stats[STAT_Q1_TOTALSECRETS]);
    sb_string(8, 12, str);
    Q_snprintf(str, sizeof(str), "Time :%3i:%i%i", minutes, seconds / 10, seconds % 10);
    sb_string(184, 4, str);
    sb_string(232 - (int)strlen(name) * 4, 12, name);
}

static void sb_bar(uint32_t items, const short *stats, float time)
{
    const int armor = stats[STAT_ARMOR], health = stats[STAT_HEALTH], ammo = stats[STAT_AMMO];

    sb_pic(0, 0, sb.sbar);

    // Scourge of Armagon's keys
    if (sb.kind == SB_HIPNOTIC) {
        if (items & IT_KEY1)
            sb_pic(209, 3, sb.itempics[0]);
        if (items & IT_KEY2)
            sb_pic(209, 12, sb.itempics[1]);
    }

    // armor
    if (items & IT_INVULNERABILITY) {
        sb_num(24, 0, 666, 3, 1);
        sb_pic(0, 0, sb.disc);
    } else {
        const uint32_t a1 = sb.kind == SB_ROGUE ? RIT_ARMOR1 : IT_ARMOR1;
        const uint32_t a2 = sb.kind == SB_ROGUE ? RIT_ARMOR2 : IT_ARMOR2;
        const uint32_t a3 = sb.kind == SB_ROGUE ? RIT_ARMOR3 : IT_ARMOR3;

        sb_num(24, 0, armor, 3, armor <= 25);
        if (items & a3)
            sb_pic(0, 0, sb.armor[2]);
        else if (items & a2)
            sb_pic(0, 0, sb.armor[1]);
        else if (items & a1)
            sb_pic(0, 0, sb.armor[0]);
    }

    sb_face(items, health, time);
    sb_num(136, 0, health, 3, health <= 25);

    // ammo
    if (sb.kind == SB_ROGUE) {
        if (items & RIT_SHELLS)
            sb_pic(224, 0, sb.ammo[0]);
        else if (items & RIT_NAILS)
            sb_pic(224, 0, sb.ammo[1]);
        else if (items & RIT_ROCKETS)
            sb_pic(224, 0, sb.ammo[2]);
        else if (items & RIT_CELLS)
            sb_pic(224, 0, sb.ammo[3]);
        else if (items & RIT_LAVA_NAILS)
            sb_pic(224, 0, sb.rammo[0]);
        else if (items & RIT_PLASMA_AMMO)
            sb_pic(224, 0, sb.rammo[1]);
        else if (items & RIT_MULTI_ROCKETS)
            sb_pic(224, 0, sb.rammo[2]);
    } else {
        if (items & IT_SHELLS)
            sb_pic(224, 0, sb.ammo[0]);
        else if (items & IT_NAILS)
            sb_pic(224, 0, sb.ammo[1]);
        else if (items & IT_ROCKETS)
            sb_pic(224, 0, sb.ammo[2]);
        else if (items & IT_CELLS)
            sb_pic(224, 0, sb.ammo[3]);
    }
    sb_num(248, 0, ammo, 3, ammo <= 10);
}

// Sbar_IntermissionOverlay, on a 320x200 screen in the middle
static void sb_intermission(int width, int height, const short *stats)
{
    const int ox = (width - 320) / 2, oy = (height - 200) / 2;
    const int t = stats[STAT_Q1_TIME];
    const int minutes = t / 60, seconds = t - minutes * 60;

    if (sb.complete)
        R_DrawPic(ox + 64, oy + 24, sb.complete);
    if (sb.inter)
        R_DrawPic(ox, oy + 56, sb.inter);

    sb_num_at(ox + 160, oy + 64, minutes, 3, 0);
    if (sb.colon)
        R_DrawPic(ox + 234, oy + 64, sb.colon);
    if (sb.nums[0][seconds / 10])
        R_DrawPic(ox + 246, oy + 64, sb.nums[0][seconds / 10]);
    if (sb.nums[0][seconds % 10])
        R_DrawPic(ox + 266, oy + 64, sb.nums[0][seconds % 10]);

    sb_num_at(ox + 160, oy + 104, stats[STAT_Q1_SECRETS], 3, 0);
    if (sb.slash)
        R_DrawPic(ox + 232, oy + 104, sb.slash);
    sb_num_at(ox + 240, oy + 104, stats[STAT_Q1_TOTALSECRETS], 3, 0);

    sb_num_at(ox + 160, oy + 144, stats[STAT_Q1_MONSTERS], 3, 0);
    if (sb.slash)
        R_DrawPic(ox + 232, oy + 144, sb.slash);
    sb_num_at(ox + 240, oy + 144, stats[STAT_Q1_TOTALMONSTERS], 3, 0);
}

// Sbar_FinaleOverlay: the game sends the text itself, as a layout
static void sb_finale(int width, int height)
{
    int w = 0, h = 0;

    if (!sb.finale)
        return;
    R_GetPicSize(&w, &h, sb.finale);
    R_DrawPic((width - w) / 2, (height - 200) / 2 + 16, sb.finale);
}

/*
===============
SCR_Q1DrawStatusBar

In place of the CS_STATUSBAR layout under CS_Q1VM: Sbar_Draw.
===============
*/
void SCR_Q1DrawStatusBar(int width, int height)
{
    const short *stats = cl.frame.ps.stats;
    const float time = cl.time * 0.001f;
    const uint32_t items = (uint16_t)stats[STAT_Q1_ITEMS_LO] | (uint32_t)(uint16_t)stats[STAT_Q1_ITEMS_HI] << 16;
    const uint32_t weapon = (uint16_t)stats[STAT_Q1_WEAPON_LO] | (uint32_t)(uint16_t)stats[STAT_Q1_WEAPON_HI] << 16;

    if (!cl.q1vm[0])
        return;
    if (strcmp(sb.episode, cl.q1vm))
        SCR_Q1RegisterMedia();

    // CL_ParseClientdata: when each item came, so a new one flashes
    if (items != sb.items) {
        for (int i = 0; i < 32; i++)
            if ((items & (1u << i)) && !(sb.items & (1u << i)))
                sb.item_gettime[i] = time;
        sb.items = items;
    }
    // V_ParseDamage: the pain face for a moment after a hit - a svc_damage,
    // which the server counts in STAT_Q1_HITS. Not any fall in health, as it
    // was: a megahealth rots a point a second without hurting anyone, and the
    // face winced at every one (Matt, 2026-11-21). A new map or a loaded game
    // starts the count again without a hit.
    const int hits = stats[STAT_Q1_INTERMISSION] & STAT_Q1_HITS;
    if (sb.hits_server == cl.servercount && hits != sb.hits)
        sb.faceanimtime = time + 0.2f;
    sb.hits = hits;
    sb.hits_server = cl.servercount;

    switch (stats[STAT_Q1_INTERMISSION] & STAT_Q1_STATE & ~STAT_Q1_SHOWSCORES) {
    case 1:
        sb_intermission(width, height, stats);
        return;
    case 2:
        sb_finale(width, height);
        return;
    case 3:
        return;     // a cutscene: its text alone
    }

    sb_x = (width - 320) / 2;
    sb_y = height - SBAR_HEIGHT;

    sb_inventory(items, weapon, stats, time);

    if (stats[STAT_HEALTH] <= 0 || (stats[STAT_Q1_INTERMISSION] & STAT_Q1_SHOWSCORES)) {
        // the dead (and Tab) see how the level went
        sb_pic(0, 0, sb.scorebar);
        sb_solo_scoreboard(stats);
    } else {
        sb_bar(items, stats, time);
    }
}
