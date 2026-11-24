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
// q1player.c -- Quake's own player as a player model
//
// "quake1/ranger" in the skin cvar: Quake's Ranger, progs/player.mdl from the
// Quake paks (or the rerelease's player.md5mesh, as cl_md5_models has it),
// for the player and for bots in the bot match. It is one model with the gun
// in his hands, so no weapon model is added; the frames the server sends are
// Quake II's (m_player.h) and are turned into Quake's (player.qc) here, his
// attack and the axe's whole set by the gun the server says he holds. His
// voice is Quake's player/ sounds.
//
// Quake's shirt and pants colours are not done: the skin is drawn as it is.

#include "client.h"

// player.qc's $frame list: where each of Quake's animations starts
#define Q1F_AXRUN       0       // 6
#define Q1F_ROCKRUN     6       // 6
#define Q1F_STAND       12      // 5
#define Q1F_AXSTND      17      // 12
#define Q1F_AXPAIN      29      // 6
#define Q1F_PAIN        35      // 6
#define Q1F_AXDETH      41      // 9
#define Q1F_DEATHA      50      // 11
#define Q1F_DEATHB      61      // 9
#define Q1F_DEATHC      70      // 15
#define Q1F_DEATHD      85      // 9
#define Q1F_NAILATT     103     // 2
#define Q1F_LIGHT       Q1P_FRAME_LIGHT     // 2
#define Q1F_ROCKATT     107     // 6
#define Q1F_SHOTATT     113     // 6
#define Q1F_AXATT       119     // 6

bool Q1P_Available(void)
{
    return FS_FileExists(Q1P_MODEL_PATH);
}

bool Q1P_ModelExists(const char *model)
{
    return model && !Q_stricmp(model, Q1P_MODEL_NAME) && Q1P_Available();
}

/*
===============
Q1P_Register

A client wearing "quake1/<anything>": the Ranger, in his own skin (Quake's
colours would have been the skin's shirt and pants; a team uniform is the
same skin), with no weapon models.
===============
*/
bool Q1P_Register(clientinfo_t *ci, const char *model, const char *skin)
{
    int i;

    if (!Q1P_ModelExists(model))
        return false;

    ci->model = R_RegisterModel(Q1P_MODEL_PATH);
    if (!ci->model)
        return false;

    ci->skin = 0;       // the model's own
    for (i = 0; i < MAX_CLIENTWEAPONMODELS; i++)
        ci->weaponmodel[i] = 0;
    ci->icon = R_RegisterPic2(Q1P_ICON_PATH);
    ci->q1 = true;

    Q_strlcpy(ci->model_name, Q1P_MODEL_NAME, sizeof(ci->model_name));
    Q_strlcpy(ci->skin_name, Q1P_SKIN_NAME, sizeof(ci->skin_name));
    return true;
}

/*
===============
Q1P_GunOfVWep

The gun a player holds, by the vwep model the server says it is ("w_shotgun.md2",
or a Quake III gun's "models/weapons2/<gun>/<gun>.md3"): which of Quake's
attacks he does with it. Quake's own guns are dealt as Quake II's vweps
(g_items.c): the axe is the chainfist's, the Thunderbolt the plasma beam's.
===============
*/
q1pgun_t Q1P_GunOfVWep(const char *vwep)
{
    char name[MAX_QPATH];

    if (!vwep || !*vwep)
        return Q1P_GUN_SHOTGUN;
    Q_strlcpy(name, vwep, sizeof(name));
    Q_strlwr(name);

    if (strstr(name, "chainfist") || strstr(name, "gauntlet") || strstr(name, "axe"))
        return Q1P_GUN_AXE;
    if (strstr(name, "weapons2/plasma"))        // Quake III's plasma gun: a stream of shots
        return Q1P_GUN_NAILS;
    if (strstr(name, "plasma") || strstr(name, "lightning") || strstr(name, "beam"))
        return Q1P_GUN_LIGHTNING;
    if (strstr(name, "launch") || strstr(name, "grenade") || strstr(name, "rocket")
        || strstr(name, "phallanx") || strstr(name, "bfg"))
        return Q1P_GUN_ROCKET;
    if (strstr(name, "machinegun") || strstr(name, "chaingun") || strstr(name, "hyperblaster")
        || strstr(name, "etfrifle") || strstr(name, "ripper") || strstr(name, "disrupt")
        || strstr(name, "nail"))
        return Q1P_GUN_NAILS;
    return Q1P_GUN_SHOTGUN;
}

// (i) of (n) frames of Quake II's attack, as the Ranger's with (gun)
static int Q1P_Attack(int i, int n, q1pgun_t gun)
{
    switch (gun) {
    case Q1P_GUN_AXE:
        return Q1F_AXATT + i * 6 / n;
    case Q1P_GUN_NAILS:
        return Q1F_NAILATT + (i & 1);
    case Q1P_GUN_LIGHTNING:
        return Q1F_LIGHT + (i & 1);
    case Q1P_GUN_ROCKET:
        return Q1F_ROCKATT + i * 6 / n;
    default:
        return Q1F_SHOTATT + i * 6 / n;
    }
}

/*
===============
Q1P_Frame

Quake II's player frame (m_player.h) as one of player.mdl's. Quake has no
crouch, jump or gestures: they stand, or run. Its deaths are longer than
Quake II's and are stepped through faster.
===============
*/
int Q1P_Frame(int f, q1pgun_t gun)
{
    const bool axe = gun == Q1P_GUN_AXE;
    const int stand = axe ? Q1F_AXSTND : Q1F_STAND;
    const int stand_n = axe ? 12 : 5;
    const int run = axe ? Q1F_AXRUN : Q1F_ROCKRUN;
    const int pain = axe ? Q1F_AXPAIN : Q1F_PAIN;

    if (f < 0)
        return Q1F_STAND;
    if (f < 40)                     // stand01-40
        return stand + f % stand_n;
    if (f < 46)                     // run1-6
        return run + (f - 40);
    if (f < 54)                     // attack1-8
        return Q1P_Attack(f - 46, 8, gun);
    if (f < 66)                     // pain1-3, four frames each
        return pain + ((f - 54) % 4) * 3 / 2;
    if (f < 72)                     // jump1-6: in the air, running
        return run + (f - 66);
    if (f < 135)                    // flip, salute, taunt, wave, point
        return stand + (f - 72) % stand_n;
    if (f < 154)                    // crstnd01-19
        return stand + (f - 135) % stand_n;
    if (f < 160)                    // crwalk1-6
        return run + (f - 154);
    if (f < 169)                    // crattak1-9
        return Q1P_Attack(f - 160, 9, gun);
    if (f < 173)                    // crpain1-4
        return pain + (f - 169) * 3 / 2;
    if (f < 178)                    // crdeath1-5
        return axe ? Q1F_AXDETH + (f - 173) * 2 : Q1F_DEATHD + (f - 173) * 2;
    if (f < 184)                    // death101-06
        return axe ? Q1F_AXDETH + (f - 178) * 8 / 5 : Q1F_DEATHA + (f - 178) * 2;
    if (f < 190)                    // death201-06
        return axe ? Q1F_AXDETH + (f - 184) * 8 / 5 : Q1F_DEATHB + (f - 184) * 8 / 5;
    if (f < 198)                    // death301-08
        return axe ? Q1F_AXDETH + (f - 190) * 8 / 7 : Q1F_DEATHC + (f - 190) * 2;
    return Q1F_STAND;
}

/*
===============
Q1P_SexedSound

Quake II's "*<name>" player sounds for a Ranger: Quake's own (player.qc
PainSound, DeathSound, PlayerJump; client.qc's landings; WaterMove's drowning).
(out) is the sound's file, from id1's pak.
===============
*/
static const struct {
    const char  *q2;
    const char  *q1;
} q1p_sounds[] = {
    { "death1.wav",     "death1.wav" },
    { "death2.wav",     "death2.wav" },
    { "death3.wav",     "death3.wav" },
    { "death4.wav",     "death4.wav" },
    { "drown1.wav",     "h2odeath.wav" },
    { "fall1.wav",      "land.wav" },
    { "fall2.wav",      "land2.wav" },
    { "gurp1.wav",      "drown1.wav" },
    { "gurp2.wav",      "drown2.wav" },
    { "jump1.wav",      "plyrjmp8.wav" },
    { "pain25_1.wav",   "pain1.wav" },
    { "pain25_2.wav",   "pain2.wav" },
    { "pain50_1.wav",   "pain3.wav" },
    { "pain50_2.wav",   "pain4.wav" },
    { "pain75_1.wav",   "pain5.wav" },
    { "pain75_2.wav",   "pain6.wav" },
    { "pain100_1.wav",  "pain5.wav" },
    { "pain100_2.wav",  "pain6.wav" },
};

bool Q1P_SexedSound(const char *model, const char *base, char *out, size_t size)
{
    const char *name = base + (*base == '*');
    int i;

    if (!model || Q_stricmp(model, Q1P_MODEL_NAME))
        return false;

    for (i = 0; i < q_countof(q1p_sounds); i++)
        if (!Q_stricmp(name, q1p_sounds[i].q2))
            break;
    if (i == q_countof(q1p_sounds))
        return false;

    if (Q_concat(out, size, "id1/id1/sound/player/", q1p_sounds[i].q1) >= size)
        return false;
    return FS_FileExists(out);
}
