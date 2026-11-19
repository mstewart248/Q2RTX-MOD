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

QUAKE III'S BOT ENGINE: THE QUAKE II SIDE (bot_q3_ai 2)

Quake III's own bots - botlib's AAS navigation and its goal, move, weapon,
chat and character code, and the bot AI from its game module (q3bot/, from
ioquake3) - playing in this game. This file is what they see of it and how
they act in it: the q2b_* functions q3bot/q3bot_iface.h asks for, and the
BotEngine_* calls g_bot.c makes.

What they see. Every frame the game is redrawn in Quake III's terms
(q2b_snapshot): players with Quake III's stats, weapons and powerups, items
by Quake III's item numbers, lifts and doors as its movers, grenades as its
grenades. A Quake II gun stands in for the Quake III gun it plays like - the
hyperblaster for the plasma gun, the super shotgun for the shotgun - and the
blaster you spawn with for the machine gun Quake III's players spawn with.

How they act. A bot's input is a Quake III usercmd, run as a Quake II one
through ClientThink; its weapon number switches to that slot's gun; its chat
goes out as "say", and everyone's chat comes back to it as Quake III's
"chat" lines (BotEngine_Chat, from g_bot.c's cprintf wrapper).

Where they go. Quake III's maps come with their AAS (maps/<map>.aas in
baseq3's paks), checked against the map's MD4 checksum as Quake III checked
it; on a map without one the bots play by Quake II's rules (g_bot.c).

==============================================================================
*/

#include "g_local.h"
#include "common/mdfour.h"
#include "q3bot/q3bot_iface.h"

#include <time.h>

static char     *be_entities;           // the map's entity lump, TAG_LEVEL
static bool     be_loaded;              // Q3Bot_LevelStart ran this level
static bool     be_ready;               // ...and found the map's AAS
static int      be_spawncount = -1;     // the level the caches below are for

// what Quake III's game tracked per player, which Quake II's does not
typedef struct {
    int     lasthurt_client;
    int     lasthurt_mod;
    int     attacker;
    int     hits;
    int     spawn_count;
    bool    was_dead;
    vec3_t  last_origin;
    bool    have_origin;
    int     oldbuttons;
} be_player_t;

static be_player_t  be_players[Q3B_MAX_CLIENTS];

// ==========================================================================
// WEAPONS AND ITEMS, AS QUAKE III'S

// each Quake III weapon slot's guns: Quake III's own, then the Quake II guns
// that play like it
#define BE_SLOT_GUNS    6

static const char *const be_slot_names[Q3B_WP_NUM_WEAPONS][BE_SLOT_GUNS] = {
    { NULL },
    { "weapon_q3_gauntlet", "weapon_chainfist" },
    { "weapon_q3_machinegun", "weapon_chaingun", "weapon_machinegun", "weapon_etf_rifle", "weapon_blaster" },
    { "weapon_q3_shotgun", "weapon_supershotgun", "weapon_shotgun" },
    { "weapon_q3_grenadelauncher", "weapon_grenadelauncher", "weapon_proxlauncher" },
    { "weapon_q3_rocketlauncher", "weapon_rocketlauncher", "weapon_phalanx" },
    { "weapon_q3_lightning", "weapon_plasmabeam" },
    { "weapon_q3_railgun", "weapon_railgun", "weapon_disintegrator" },
    { "weapon_q3_plasmagun", "weapon_hyperblaster", "weapon_boomer" },
    { "weapon_q3_bfg", "weapon_bfg" },
    { NULL },
};

static gitem_t  *be_slot_items[Q3B_WP_NUM_WEAPONS][BE_SLOT_GUNS];

static const int be_slot_pickup[Q3B_WP_NUM_WEAPONS] = {
    0, Q3B_IT_GAUNTLET, Q3B_IT_MACHINEGUN, Q3B_IT_SHOTGUN, Q3B_IT_GRENADELAUNCHER,
    Q3B_IT_ROCKETLAUNCHER, Q3B_IT_LIGHTNING, Q3B_IT_RAILGUN, Q3B_IT_PLASMAGUN, Q3B_IT_BFG10K,
    Q3B_IT_GRAPPLINGHOOK
};

// everything else on the ground, by classname (a health box's own: its item
// has none)
static const struct {
    const char  *classname;
    int         index;
} be_items[] = {
    { "item_armor_shard",       Q3B_IT_ARMORSHARD },
    { "item_armor_jacket",      Q3B_IT_ARMORCOMBAT },   // Quake III has no green
    { "item_armor_combat",      Q3B_IT_ARMORCOMBAT },
    { "item_armor_body",        Q3B_IT_ARMORBODY },
    { "item_health_small",      Q3B_IT_HEALTHSMALL },
    { "item_health",            Q3B_IT_HEALTH },
    { "item_health_large",      Q3B_IT_HEALTHLARGE },
    { "item_health_mega",       Q3B_IT_HEALTHMEGA },
    { "ammo_shells",            Q3B_IT_SHELLS },
    { "ammo_bullets",           Q3B_IT_BULLETS },
    { "ammo_flechettes",        Q3B_IT_BULLETS },
    { "ammo_grenades",          Q3B_IT_GRENADES },
    { "ammo_prox",              Q3B_IT_GRENADES },
    { "ammo_cells",             Q3B_IT_CELLS },
    { "ammo_q3_lightning",      Q3B_IT_LIGHTNINGAMMO },
    { "ammo_rockets",           Q3B_IT_ROCKETS },
    { "ammo_magslug",           Q3B_IT_ROCKETS },
    { "ammo_slugs",             Q3B_IT_SLUGS },
    { "ammo_disruptor",         Q3B_IT_SLUGS },
    { "ammo_q3_bfg",            Q3B_IT_BFGAMMO },
    { "item_q3_teleporter",     Q3B_IT_TELEPORTER },
    { "item_q3_medkit",         Q3B_IT_MEDKIT },
    { "item_quad",              Q3B_IT_QUAD },
    { "item_q3_battlesuit",     Q3B_IT_ENVIRONMENTSUIT },
    { "item_enviro",            Q3B_IT_ENVIRONMENTSUIT },
    { "item_q3_haste",          Q3B_IT_HASTE },
    { "item_invisibility",      Q3B_IT_INVISIBILITY },
    { "item_q3_regen",          Q3B_IT_REGEN },
    { "item_q3_flight",         Q3B_IT_FLIGHT },
    { "item_flag_team1",        Q3B_IT_REDFLAG },
    { "item_flag_team2",        Q3B_IT_BLUEFLAG },
};

static gitem_t  *be_medkit, *be_teleporter, *be_redflag, *be_blueflag;
static int      be_items_spawncount = -2;

static void BE_CacheItems(void)
{
    int w, i;

    if (be_items_spawncount == be_spawncount)
        return;
    be_items_spawncount = be_spawncount;

    for (w = 0; w < Q3B_WP_NUM_WEAPONS; w++)
        for (i = 0; i < BE_SLOT_GUNS; i++)
            be_slot_items[w][i] = be_slot_names[w][i] ? FindItemByClassname((char *)be_slot_names[w][i]) : NULL;

    be_medkit = FindItemByClassname("item_q3_medkit");
    be_teleporter = FindItemByClassname("item_q3_teleporter");
    be_redflag = FindItemByClassname("item_flag_team1");
    be_blueflag = FindItemByClassname("item_flag_team2");
}

// the slot a gun is in, Q3B_WP_NONE for none
static int BE_WeaponSlot(const gitem_t *item)
{
    int w, i;

    if (!item)
        return Q3B_WP_NONE;
    for (w = 1; w < Q3B_WP_NUM_WEAPONS; w++)
        for (i = 0; i < BE_SLOT_GUNS; i++)
            if (be_slot_items[w][i] == item)
                return w;
    return Q3B_WP_NONE;
}

// Quake III's guns are this player's: a Quake III weapon game, or one by
// player model where its model is Quake III's
static bool BE_Q3Guns(const edict_t *ent)
{
    return level.q3_weapons == 1 || (level.q3_weapons == 2 && Q3W_PlayerQ3(ent));
}

static gitem_t *BE_AmmoOf(const gitem_t *gun)
{
    return (gun && gun->ammo) ? FindItem(gun->ammo) : NULL;
}

static bool BE_HasAmmo(const edict_t *ent, const gitem_t *gun)
{
    const gitem_t *ammo = BE_AmmoOf(gun);

    if (!ammo || ((int)dmflags->value & DF_INFINITE_AMMO))
        return true;
    return ent->client->pers.inventory[ITEM_INDEX(ammo)] >= max(gun->quantity, 1);
}

/*
=================
BE_SlotGun

The gun a slot means for this player: one it holds with ammo for it, its own
game's first; failing that one it holds; NULL for none.
=================
*/
static gitem_t *BE_SlotGun(const edict_t *ent, int slot)
{
    const int   *inv = ent->client->pers.inventory;
    bool        q3 = BE_Q3Guns(ent);
    gitem_t     *held = NULL;
    int         pass, i;

    if (slot <= Q3B_WP_NONE || slot >= Q3B_WP_NUM_WEAPONS)
        return NULL;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < BE_SLOT_GUNS; i++) {
            gitem_t *it = be_slot_items[slot][i];

            if (!it || !inv[ITEM_INDEX(it)])
                continue;
            if (!!(it->flags & IT_Q3) != (q3 ^ pass))
                continue;
            if (BE_HasAmmo(ent, it))
                return it;
            if (!held)
                held = it;
        }
    }
    return held;
}

/*
=================
BE_SlotAmmo

Quake III's ammo for a slot: the ammo of the gun it means, and with no gun,
of the first of this player's own game's guns there - Quake III kept ammo
for guns you did not have, which its bots go and fetch.
=================
*/
static int BE_SlotAmmo(const edict_t *ent, int slot)
{
    const int   *inv = ent->client->pers.inventory;
    gitem_t     *gun = BE_SlotGun(ent, slot), *ammo;
    bool        q3 = BE_Q3Guns(ent);
    int         i;

    if (!gun) {
        for (i = 0; i < BE_SLOT_GUNS && !gun; i++) {
            gitem_t *it = be_slot_items[slot][i];

            if (it && it->ammo && !!(it->flags & IT_Q3) == q3)
                gun = it;
        }
    }
    if (!gun)
        return 0;
    if (gun->flags & IT_MELEE)
        return -1;
    ammo = BE_AmmoOf(gun);
    if (!ammo)
        return 100;     // the blaster: never runs out
    return inv[ITEM_INDEX(ammo)];
}

static int BE_ItemIndex(const edict_t *ent)
{
    int slot, i;

    // health boxes go by their spawn classname
    for (i = 0; i < q_countof(be_items); i++) {
        if (ent->classname && !strcmp(ent->classname, be_items[i].classname))
            return be_items[i].index;
        if (ent->item && ent->item->classname && !strcmp(ent->item->classname, be_items[i].classname))
            return be_items[i].index;
    }
    slot = BE_WeaponSlot(ent->item);
    return slot ? be_slot_pickup[slot] : 0;
}

// ==========================================================================
// NUMBERS - Quake II edict n is Quake III entity n - 1

static int BE_Num(const edict_t *ent)
{
    int n;

    if (!ent)
        return Q3B_ENTITYNUM_NONE;
    if (ent == g_edicts)
        return Q3B_ENTITYNUM_WORLD;
    n = (int)(ent - g_edicts) - 1;
    if (n < 0 || n >= Q3B_EVENT_BASE)
        return Q3B_ENTITYNUM_WORLD;
    return n;
}

static edict_t *BE_Edict(int num)
{
    if (num < 0 || num >= Q3B_EVENT_BASE || num + 1 >= globals.num_edicts)
        return NULL;
    return g_edicts + num + 1;
}

static edict_t *BE_Client(int client)
{
    edict_t *ent;

    if (client < 0 || client >= game.maxclients || client >= Q3B_MAX_CLIENTS)
        return NULL;
    ent = g_edicts + 1 + client;
    return (ent->inuse && ent->client) ? ent : NULL;
}

// Quake III's content bits as Quake II's, for a trace's mask
static int BE_Q2Mask(int q3)
{
    int c = 0;

    if (q3 & Q3B_CONTENTS_SOLID)        c |= CONTENTS_SOLID | CONTENTS_WINDOW;
    if (q3 & Q3B_CONTENTS_LAVA)         c |= CONTENTS_LAVA;
    if (q3 & Q3B_CONTENTS_SLIME)        c |= CONTENTS_SLIME;
    if (q3 & Q3B_CONTENTS_WATER)        c |= CONTENTS_WATER;
    if (q3 & Q3B_CONTENTS_FOG)          c |= CONTENTS_MIST;
    if (q3 & Q3B_CONTENTS_PLAYERCLIP)   c |= CONTENTS_PLAYERCLIP;
    if (q3 & Q3B_CONTENTS_MONSTERCLIP)  c |= CONTENTS_MONSTERCLIP;
    if (q3 & Q3B_CONTENTS_BODY)         c |= CONTENTS_MONSTER;
    if (q3 & Q3B_CONTENTS_CORPSE)       c |= CONTENTS_DEADMONSTER;
    if (q3 & Q3B_CONTENTS_DETAIL)       c |= CONTENTS_DETAIL;
    if (q3 & Q3B_CONTENTS_TRANSLUCENT)  c |= CONTENTS_TRANSLUCENT;
    return c;
}

// and back, for what a trace or a point found
static int BE_Q3Contents(int q2)
{
    int c = 0;

    if (q2 & (CONTENTS_SOLID | CONTENTS_WINDOW))    c |= Q3B_CONTENTS_SOLID;
    if (q2 & CONTENTS_LAVA)         c |= Q3B_CONTENTS_LAVA;
    if (q2 & CONTENTS_SLIME)        c |= Q3B_CONTENTS_SLIME;
    if (q2 & CONTENTS_WATER)        c |= Q3B_CONTENTS_WATER;
    if (q2 & CONTENTS_MIST)         c |= Q3B_CONTENTS_FOG;
    if (q2 & CONTENTS_PLAYERCLIP)   c |= Q3B_CONTENTS_PLAYERCLIP;
    if (q2 & CONTENTS_MONSTERCLIP)  c |= Q3B_CONTENTS_MONSTERCLIP;
    if (q2 & CONTENTS_MONSTER)      c |= Q3B_CONTENTS_BODY;
    if (q2 & CONTENTS_DEADMONSTER)  c |= Q3B_CONTENTS_CORPSE;
    if (q2 & CONTENTS_DETAIL)       c |= Q3B_CONTENTS_DETAIL;
    if (q2 & CONTENTS_TRANSLUCENT)  c |= Q3B_CONTENTS_TRANSLUCENT;
    return c;
}

// ==========================================================================
// q2b_*: WHAT THE BOT ENGINE ASKS OF THE GAME

void q2b_print(int level, const char *msg)
{
    switch (level) {
    case 0:     gi.dprintf("%s", msg); break;
    case 1:     gi.dprintf("Quake III bots: %s", msg); break;
    default:    gi.dprintf("Quake III bots: ERROR: %s", msg); break;
    }
}

void q2b_error(const char *msg)
{
    gi.error("Quake III bots: %s", msg);
}

int q2b_milliseconds(void)
{
    return (int)(clock() * 1000.0 / CLOCKS_PER_SEC);
}

static void BE_FillTrace(q3b_trace_t *out, const trace_t *tr)
{
    memset(out, 0, sizeof(*out));
    out->allsolid = tr->allsolid;
    out->startsolid = tr->startsolid;
    out->fraction = tr->fraction;
    VectorCopy(tr->endpos, out->endpos);
    VectorCopy(tr->plane.normal, out->normal);
    out->dist = tr->plane.dist;
    out->surfaceflags = tr->surface ? (tr->surface->flags & (SURF_SLICK | SURF_SKY)) : 0;
    out->contents = BE_Q3Contents(tr->contents);
    out->entityNum = tr->fraction < 1 || tr->startsolid ? BE_Num(tr->ent) : Q3B_ENTITYNUM_NONE;
}

void q2b_trace(q3b_trace_t *out, const float *start, const float *mins, const float *maxs,
               const float *end, int passent, int q3mask)
{
    static const vec3_t zero = { 0, 0, 0 };
    trace_t tr;

    tr = gi.trace(start, mins ? mins : zero, maxs ? maxs : zero, end, BE_Edict(passent), BE_Q2Mask(q3mask));
    BE_FillTrace(out, &tr);
}

/*
=================
q2b_entity_trace

A box swept against one entity alone. There is no such trace in this game's
imports, so it is against the entity's bounding box - what botlib asks it
for is whether a mover is in the way.
=================
*/
void q2b_entity_trace(q3b_trace_t *out, const float *start, const float *mins, const float *maxs,
                      const float *end, int entnum, int q3mask)
{
    edict_t *ent = BE_Edict(entnum);
    vec3_t  bmin, bmax;
    float   enter = -1, leave = 1;
    int     i, axis = -1;
    bool    neg = false;

    memset(out, 0, sizeof(*out));
    out->fraction = 1;
    VectorCopy(end, out->endpos);
    out->entityNum = Q3B_ENTITYNUM_NONE;

    if (!ent || !ent->inuse || ent->solid == SOLID_NOT || ent->solid == SOLID_TRIGGER ||
        !(q3mask & (Q3B_CONTENTS_SOLID | Q3B_CONTENTS_PLAYERCLIP | Q3B_CONTENTS_BODY)))
        return;

    for (i = 0; i < 3; i++) {
        bmin[i] = ent->absmin[i] - (maxs ? maxs[i] : 0);
        bmax[i] = ent->absmax[i] - (mins ? mins[i] : 0);
    }

    for (i = 0; i < 3; i++) {
        float d = end[i] - start[i];
        float t0, t1;

        if (d == 0) {
            if (start[i] < bmin[i] || start[i] > bmax[i])
                return;
            continue;
        }
        t0 = (bmin[i] - start[i]) / d;
        t1 = (bmax[i] - start[i]) / d;
        if (t0 > t1) {
            float t = t0;
            t0 = t1;
            t1 = t;
        }
        if (t0 > enter) {
            enter = t0;
            axis = i;
            neg = d > 0;
        }
        if (t1 < leave)
            leave = t1;
        if (enter > leave)
            return;
    }

    if (axis < 0 || enter < 0) {
        // started inside it
        out->startsolid = 1;
        out->allsolid = leave >= 1;
        out->fraction = 0;
        VectorCopy(start, out->endpos);
        out->entityNum = entnum;
        out->contents = Q3B_CONTENTS_SOLID;
        return;
    }
    if (enter > 1)
        return;

    out->fraction = max(0.0f, enter - 0.001f);
    for (i = 0; i < 3; i++)
        out->endpos[i] = start[i] + out->fraction * (end[i] - start[i]);
    out->normal[axis] = neg ? -1 : 1;
    out->dist = neg ? -bmin[axis] : bmax[axis];
    out->entityNum = entnum;
    out->contents = Q3B_CONTENTS_SOLID;
}

int q2b_point_contents(const float *p)
{
    return BE_Q3Contents(gi.pointcontents(p));
}

int q2b_in_pvs(const float *a, const float *b)
{
    return gi.inPVS(a, b);
}

const char *q2b_entity_string(void)
{
    return be_entities ? be_entities : "";
}

/*
=================
q2b_model_bounds

A brush model's bounds, "*N": the entity that carries it got them from
gi.setmodel. Movers ask every update, so they are looked up once a level.
=================
*/
#define BE_MAX_BMODELS  1024

static struct {
    int     spawncount;
    bool    found;
    vec3_t  mins, maxs;
} be_bmodels[BE_MAX_BMODELS];

int q2b_model_bounds(int modelnum, float *mins, float *maxs)
{
    edict_t *ent;
    int     i;

    if (modelnum <= 0 || modelnum >= BE_MAX_BMODELS)
        return 0;

    if (be_bmodels[modelnum].spawncount != be_spawncount || !be_bmodels[modelnum].found) {
        be_bmodels[modelnum].spawncount = be_spawncount;
        be_bmodels[modelnum].found = false;
        for (i = 1, ent = g_edicts + 1; i < globals.num_edicts; i++, ent++) {
            if (!ent->inuse || !ent->model || ent->model[0] != '*' || atoi(ent->model + 1) != modelnum)
                continue;
            VectorCopy(ent->mins, be_bmodels[modelnum].mins);
            VectorCopy(ent->maxs, be_bmodels[modelnum].maxs);
            be_bmodels[modelnum].found = true;
            break;
        }
    }
    if (!be_bmodels[modelnum].found)
        return 0;
    VectorCopy(be_bmodels[modelnum].mins, mins);
    VectorCopy(be_bmodels[modelnum].maxs, maxs);
    return 1;
}

int q2b_load_file(const char *path, void **buffer)
{
    *buffer = NULL;
    if (!gi.LoadFile)
        return -1;
    return gi.LoadFile(path, buffer);
}

void q2b_free_file(void *buffer)
{
    if (buffer)
        gi.FreeFile(buffer);
}

// --------------------------------------------------------------------------
// cvars

#define BE_MAX_CVARS    256

static cvar_t   *be_cvars[BE_MAX_CVARS];
static int      be_cvar_mods[BE_MAX_CVARS];
static int      be_numcvars;

int q2b_cvar_register(const char *name, const char *value, int flags)
{
    cvar_t  *cv;
    int     i;

    cv = gi.cvar(name, value ? value : "", flags ? CVAR_ARCHIVE : 0);
    if (!cv)
        return -1;
    for (i = 0; i < be_numcvars; i++)
        if (be_cvars[i] == cv)
            return i;
    if (be_numcvars == BE_MAX_CVARS)
        return -1;
    be_cvars[be_numcvars] = cv;
    be_cvar_mods[be_numcvars] = 1;
    return be_numcvars++;
}

void q2b_cvar_read(int handle, float *value, int *integer, char *string, int size, int *modcount)
{
    cvar_t *cv;

    if (handle < 0 || handle >= be_numcvars)
        return;
    cv = be_cvars[handle];
    if (cv->modified) {
        // Quake III's modificationCount: up by one for each change seen
        be_cvar_mods[handle]++;
    }
    *value = cv->value;
    *integer = cv->integer;
    Q_strlcpy(string, cv->string, size);
    *modcount = be_cvar_mods[handle];
}

void q2b_cvar_set(const char *name, const char *value)
{
    gi.cvar_set(name, value);
}

void q2b_cvar_string(const char *name, char *buffer, int size)
{
    cvar_t *cv = gi.cvar(name, NULL, 0);

    Q_strlcpy(buffer, cv ? cv->string : "", size);
}

void q2b_console_command(const char *text)
{
    // the AI asks for these only to breed bots (bot_interbreed*), which is
    // Quake III's own tooling and has no place here
    gi.dprintf("Quake III bots: ignored command \"%s\"\n", text);
}

int q2b_gametype(void)
{
    if (SP_Active())
        return Q3B_GT_SINGLE_PLAYER;
    if (G_CTF())
        return Q3B_GT_CTF;
    if (G_TeamPlay())
        return Q3B_GT_TEAM;
    return Q3B_GT_FFA;
}

int q2b_maxclients(void)
{
    return min(game.maxclients, Q3B_MAX_CLIENTS);
}

int q2b_level_ms(void)
{
    return level.framenum * 100;
}

int q2b_intermission(void)
{
    return level.intermission_framenum != 0;
}

// ==========================================================================
// THE SNAPSHOT

static int BE_Team(const edict_t *ent)
{
    if (ent->client->resp.spectator || ent->client->pers.spectator)
        return Q3B_TEAM_SPECTATOR;
    if (!G_TeamPlay())
        return Q3B_TEAM_FREE;
    if (ent->client->resp.team == TEAM_RED)
        return Q3B_TEAM_RED;
    if (ent->client->resp.team == TEAM_BLUE)
        return Q3B_TEAM_BLUE;
    return Q3B_TEAM_FREE;
}

// a powerup's end, in level ms, as Quake III kept it; 0 = none
static int BE_PowerupEnd(int framenum)
{
    return framenum > level.framenum ? framenum * 100 : 0;
}

static void BE_Powerups(const edict_t *ent, int *powerups)
{
    const gclient_t *cl = ent->client;
    const int       *inv = cl->pers.inventory;

    powerups[Q3B_PW_QUAD] = BE_PowerupEnd(cl->quad_framenum);
    powerups[Q3B_PW_BATTLESUIT] = BE_PowerupEnd(max(cl->q3_battlesuit_framenum, cl->enviro_framenum));
    powerups[Q3B_PW_HASTE] = BE_PowerupEnd(cl->q3_haste_framenum);
    powerups[Q3B_PW_INVIS] = BE_PowerupEnd(cl->invisible_framenum);
    powerups[Q3B_PW_REGEN] = BE_PowerupEnd(cl->q3_regen_framenum);
    powerups[Q3B_PW_FLIGHT] = BE_PowerupEnd(cl->q3_flight_framenum);
    if (be_redflag && inv[ITEM_INDEX(be_redflag)])
        powerups[Q3B_PW_REDFLAG] = 0x7fffffff;
    if (be_blueflag && inv[ITEM_INDEX(be_blueflag)])
        powerups[Q3B_PW_BLUEFLAG] = 0x7fffffff;
}

static void BE_Common(q3b_entity_t *q, const edict_t *ent)
{
    q->inuse = 1;
    q->linked = ent->area.prev != NULL;
    q->svflags = (ent->svflags & SVF_NOCLIENT) ? Q3B_SVF_NOCLIENT : 0;
    VectorCopy(ent->s.origin, q->origin);
    VectorCopy(ent->s.angles, q->angles);
    VectorCopy(ent->s.old_origin, q->old_origin);
    VectorCopy(ent->mins, q->mins);
    VectorCopy(ent->maxs, q->maxs);
    VectorCopy(ent->absmin, q->absmin);
    VectorCopy(ent->absmax, q->absmax);
    q->groundEntityNum = ent->groundentity ? BE_Num(ent->groundentity) : Q3B_ENTITYNUM_NONE;
    q->otherEntityNum = q->otherEntityNum2 = Q3B_ENTITYNUM_NONE;
}

static void BE_Player(q3b_entity_t *q, q3b_client_t *c, edict_t *ent, int num)
{
    gclient_t   *cl = ent->client;
    be_player_t *bp = &be_players[num];
    const int   *inv = cl->pers.inventory;
    bool        dead = ent->deadflag || ent->health <= 0 || cl->ps.pmove.pm_type == PM_DEAD ||
                       cl->ps.pmove.pm_type == PM_GIB;
    int         team = BE_Team(ent);
    int         w, armor;
    const char  *skin;

    BE_Common(q, ent);
    VectorCopy(cl->v_angle, q->angles);
    q->eType = Q3B_ET_PLAYER;
    if (Bot_IsBot(ent))
        q->svflags |= Q3B_SVF_BOT;
    if (team == Q3B_TEAM_SPECTATOR || !cl->pers.connected)
        q->svflags |= Q3B_SVF_NOCLIENT;
    if (ent->flags & FL_NOTARGET)
        q->flags |= Q3B_FL_NOTARGET;
    if (dead)
        q->eFlags |= Q3B_EF_DEAD;
    if ((cl->buttons & BUTTON_ATTACK) && !dead)
        q->eFlags |= Q3B_EF_FIRING;
    q->contents = dead ? Q3B_CONTENTS_CORPSE : Q3B_CONTENTS_BODY;
    q->clientNum = num;
    q->weapon = BE_WeaponSlot(cl->pers.weapon);

    // a respawn, and a teleport - a jump no running or jump pad makes in one
    // frame - are Quake III's EV_PLAYER_TELEPORT_IN, which its bots follow
    // their enemy through
    if (dead) {
        bp->was_dead = true;
    } else {
        if (bp->was_dead) {
            bp->spawn_count++;
            bp->was_dead = false;
            bp->have_origin = false;
        }
        if (bp->have_origin && DistanceSquared(bp->last_origin, ent->s.origin) > 200 * 200)
            Q3Bot_Teleported(ent->s.origin);
        VectorCopy(ent->s.origin, bp->last_origin);
        bp->have_origin = true;
    }

    c->connected = cl->pers.connected ? Q3B_CON_CONNECTED : Q3B_CON_DISCONNECTED;
    c->team = team;
    c->bot = Bot_IsBot(ent);
    Q_strlcpy(c->netname, cl->pers.netname, sizeof(c->netname));
    skin = Info_ValueForKey(cl->pers.userinfo, "skin");
    Q_strlcpy(c->model, skin, sizeof(c->model));
    c->lasthurt_client = bp->lasthurt_client;
    c->lasthurt_mod = bp->lasthurt_mod;

    if (level.intermission_framenum)
        c->pm_type = Q3B_PM_INTERMISSION;
    else if (team == Q3B_TEAM_SPECTATOR || cl->ps.pmove.pm_type == PM_SPECTATOR)
        c->pm_type = Q3B_PM_SPECTATOR;
    else if (dead)
        c->pm_type = Q3B_PM_DEAD;
    else if (cl->ps.pmove.pm_type == PM_FREEZE)
        c->pm_type = Q3B_PM_FREEZE;
    else
        c->pm_type = Q3B_PM_NORMAL;
    c->pm_flags = cl->ps.pmove.pm_flags & (Q3B_PMF_DUCKED | Q3B_PMF_JUMP_HELD);
    c->pm_time = cl->ps.pmove.pm_time;

    VectorCopy(ent->s.origin, c->origin);
    VectorCopy(ent->velocity, c->velocity);
    VectorCopy(cl->v_angle, c->viewangles);
    for (w = 0; w < 3; w++)
        c->delta_angles[w] = cl->ps.pmove.delta_angles[w];
    c->viewheight = ent->viewheight;
    c->groundEntityNum = q->groundEntityNum;
    c->weapon = q->weapon;
    switch (cl->weaponstate) {
    case WEAPON_ACTIVATING: c->weaponstate = Q3B_WEAPON_RAISING; break;
    case WEAPON_DROPPING:   c->weaponstate = Q3B_WEAPON_DROPPING; break;
    case WEAPON_FIRING:     c->weaponstate = Q3B_WEAPON_FIRING; break;
    default:                c->weaponstate = Q3B_WEAPON_READY; break;
    }
    c->eFlags = q->eFlags;

    c->stats[Q3B_STAT_HEALTH] = ent->health;
    if (be_teleporter && inv[ITEM_INDEX(be_teleporter)])
        c->stats[Q3B_STAT_HOLDABLE_ITEM] = Q3B_IT_TELEPORTER;
    else if (be_medkit && inv[ITEM_INDEX(be_medkit)])
        c->stats[Q3B_STAT_HOLDABLE_ITEM] = Q3B_IT_MEDKIT;
    for (w = 1; w < Q3B_WP_NUM_WEAPONS; w++) {
        if (BE_SlotGun(ent, w))
            c->stats[Q3B_STAT_WEAPONS] |= 1 << w;
        c->ammo[w] = BE_SlotAmmo(ent, w);
    }
    armor = ArmorIndex(ent);
    c->stats[Q3B_STAT_ARMOR] = armor ? inv[armor] : 0;
    c->stats[Q3B_STAT_MAX_HEALTH] = cl->pers.max_health;

    c->persistant[Q3B_PERS_SCORE] = cl->resp.score;
    c->persistant[Q3B_PERS_HITS] = bp->hits;
    c->persistant[Q3B_PERS_TEAM] = team;
    c->persistant[Q3B_PERS_SPAWN_COUNT] = bp->spawn_count;
    c->persistant[Q3B_PERS_ATTACKER] = bp->attacker;

    BE_Powerups(ent, c->powerups);
    for (w = 0; w < 16; w++)
        if (c->powerups[w])
            q->powerups |= 1 << w;
}

static void BE_Item(q3b_entity_t *q, edict_t *ent)
{
    BE_Common(q, ent);
    q->modelindex = BE_ItemIndex(ent);
    q->eType = q->modelindex ? Q3B_ET_ITEM : Q3B_ET_GENERAL;
    q->contents = Q3B_CONTENTS_TRIGGER;
    // taken, waiting to come back: Quake III's item is EF_NODRAW and unsent
    if (ent->solid == SOLID_NOT) {
        q->eFlags |= Q3B_EF_NODRAW;
        q->svflags |= Q3B_SVF_NOCLIENT;
    }
}

static void BE_Mover(q3b_entity_t *q, edict_t *ent)
{
    BE_Common(q, ent);
    q->eType = Q3B_ET_MOVER;
    q->bmodel = 1;
    q->modelindex = atoi(ent->model + 1);
    q->solid = 0xffffff;    // SOLID_BMODEL
    q->contents = Q3B_CONTENTS_SOLID;
}

static int BE_MissileWeapon(const edict_t *ent)
{
    if (!ent->classname)
        return Q3B_WP_NONE;
    if (!strcmp(ent->classname, "q3_missile"))
        return ent->style;
    if (!strcmp(ent->classname, "grenade") || !strcmp(ent->classname, "hgrenade"))
        return Q3B_WP_GRENADE_LAUNCHER;
    if (!strcmp(ent->classname, "rocket"))
        return Q3B_WP_ROCKET_LAUNCHER;
    if (!strcmp(ent->classname, "bolt"))
        return Q3B_WP_PLASMAGUN;
    if (!strcmp(ent->classname, "bfg blast"))
        return Q3B_WP_BFG;
    return Q3B_WP_NONE;
}

void q2b_snapshot(q3b_entity_t *ents, q3b_client_t *clients)
{
    int     n, maxc = q2b_maxclients();
    edict_t *ent;

    BE_CacheItems();

    for (n = 1, ent = g_edicts + 1; n < globals.num_edicts && n - 1 < Q3B_EVENT_BASE; n++, ent++) {
        q3b_entity_t    *q = &ents[n - 1];
        int             w;

        if (!ent->inuse)
            continue;

        if (n <= game.maxclients) {
            if (n - 1 < maxc && ent->client)
                BE_Player(q, &clients[n - 1], ent, n - 1);
            continue;
        }

        if (ent->item) {
            BE_Item(q, ent);
        } else if (ent->solid == SOLID_BSP && ent->model && ent->model[0] == '*') {
            BE_Mover(q, ent);
        } else if ((w = BE_MissileWeapon(ent)) != Q3B_WP_NONE) {
            BE_Common(q, ent);
            q->eType = Q3B_ET_MISSILE;
            q->weapon = w;
        }
        // nothing else is any of the bots' business
    }
}

// ==========================================================================
// WHAT A BOT DOES

void q2b_client_delta_angles(int client, int *delta)
{
    edict_t *ent = BE_Client(client);
    int     i;

    for (i = 0; i < 3; i++)
        delta[i] = ent ? ent->client->ps.pmove.delta_angles[i] : 0;
}

/*
=================
q2b_bot_usercmd

Quake III's usercmd as Quake II's: moves -127..127 to Quake II's speeds
(Quake III's movement scaled the largest axis to the full speed, which
Quake II's clamps for itself), the weapon number to that slot's gun, and its
holdable item's button to the item's use.
=================
*/
void q2b_bot_usercmd(int client, const q3b_usercmd_t *in, int msec)
{
    edict_t     *ent = BE_Client(client);
    be_player_t *bp;
    usercmd_t   cmd;
    float       scale = 400.0f / 127;
    int         i;

    if (!ent || client >= Q3B_MAX_CLIENTS)
        return;
    bp = &be_players[client];

    memset(&cmd, 0, sizeof(cmd));
    cmd.msec = (byte)max(1, min(msec, 250));
    cmd.lightlevel = 128;
    for (i = 0; i < 3; i++)
        cmd.angles[i] = (short)in->angles[i];

    if (in->buttons & Q3B_BUTTON_WALKING)
        scale *= 0.5f;
    cmd.forwardmove = (short)max(-400.0f, min(in->forwardmove * scale, 400.0f));
    cmd.sidemove = (short)max(-400.0f, min(in->rightmove * scale, 400.0f));
    cmd.upmove = (short)max(-400.0f, min(in->upmove * scale, 400.0f));

    if (in->buttons & Q3B_BUTTON_ATTACK)
        cmd.buttons |= BUTTON_ATTACK;

    if (ent->health > 0 && !ent->deadflag) {
        // its weapon: the gun its slot means, unless it holds that already
        if (in->weapon > Q3B_WP_NONE && in->weapon != BE_WeaponSlot(ent->client->pers.weapon)) {
            gitem_t *gun = BE_SlotGun(ent, in->weapon);

            if (gun && gun->use && ent->client->newweapon != gun && BE_HasAmmo(ent, gun))
                gun->use(ent, gun);
        }

        // its holdable item, once a press
        if ((in->buttons & Q3B_BUTTON_USE_HOLDABLE) && !(bp->oldbuttons & Q3B_BUTTON_USE_HOLDABLE)) {
            const int *inv = ent->client->pers.inventory;

            if (be_teleporter && inv[ITEM_INDEX(be_teleporter)] && be_teleporter->use)
                be_teleporter->use(ent, be_teleporter);
            else if (be_medkit && inv[ITEM_INDEX(be_medkit)] && be_medkit->use)
                be_medkit->use(ent, be_medkit);
        }
    }
    bp->oldbuttons = in->buttons;

    ClientThink(ent, &cmd);
}

// --------------------------------------------------------------------------
// chat

static void BE_Say(edict_t *ent, bool team, const char *text)
{
    char    line[MAX_STRING_CHARS];
    int     j;

    if (!*text)
        return;
    if (team && !G_TeamPlay())
        team = false;

    if (team)
        Q_snprintf(line, sizeof(line), "(%s): %s\n", ent->client->pers.netname, text);
    else
        Q_snprintf(line, sizeof(line), "%s: %s\n", ent->client->pers.netname, text);

    if (dedicated->value)
        gi.cprintf(NULL, PRINT_CHAT, "%s", line);

    for (j = 1; j <= game.maxclients; j++) {
        edict_t *other = g_edicts + j;

        if (!other->inuse || !other->client || other == ent)
            continue;
        if (team && !OnSameTeam(ent, other))
            continue;
        gi.cprintf(other, PRINT_CHAT, "%s", line);
    }
}

void q2b_client_command(int client, const char *command)
{
    edict_t     *ent = BE_Client(client);
    char        word[32];
    const char  *args;
    int         i;

    if (!ent)
        return;

    for (i = 0; command[i] && command[i] != ' ' && i < (int)sizeof(word) - 1; i++)
        word[i] = command[i];
    word[i] = 0;
    args = command + i;
    while (*args == ' ')
        args++;

    if (!Q_stricmp(word, "say")) {
        BE_Say(ent, false, args);
    } else if (!Q_stricmp(word, "say_team")) {
        BE_Say(ent, true, args);
    } else if (!Q_stricmp(word, "tell")) {
        // "tell <client> <text>"
        edict_t *to = BE_Client(atoi(args));
        char    line[MAX_STRING_CHARS];

        while (*args && *args != ' ')
            args++;
        while (*args == ' ')
            args++;
        if (to && *args) {
            Q_snprintf(line, sizeof(line), "[%s]: %s\n", ent->client->pers.netname, args);
            gi.cprintf(to, PRINT_CHAT, "%s", line);
        }
    }
    // the rest - team, teamtask, vsay, taunt, follow... - Quake II has none of
}

/*
=================
BotEngine_Chat

A chat line someone sent this bot (g_bot.c's cprintf wrapper), as Quake III's
server sent it (g_cmds.c G_Say): "chat" / "tchat" and the line, its name
marked off by the escape character (\x19) the way its match templates look
for - "\x19(name\x19)\x19: " to the team, "\x19[name\x19]\x19: " told to one
player. The told kind carries a team's orders ("Doom, go get the flag"),
and every order template wants those marks.
=================
*/
void BotEngine_Chat(edict_t *to, const char *msg)
{
    char        line[MAX_STRING_CHARS], name[MAX_STRING_CHARS];
    const char  *colon;
    bool        team = false, tell = false;
    size_t      len;
    int         client = (int)(to - g_edicts) - 1;

    if (!be_ready || !Q3Bot_IsRunning(client))
        return;

    colon = strstr(msg, ": ");
    if (!colon)
        return;
    len = colon - msg;
    if (len >= sizeof(name))
        return;
    memcpy(name, msg, len);
    name[len] = 0;
    if (name[0] == '(' && len > 2 && name[len - 1] == ')') {
        team = true;
        memmove(name, name + 1, len - 2);
        name[len - 2] = 0;
    } else if (name[0] == '[' && len > 2 && name[len - 1] == ']') {
        tell = true;
        memmove(name, name + 1, len - 2);
        name[len - 2] = 0;
    }

    if (team)
        Q_snprintf(line, sizeof(line), "tchat \"\x19(%s\x19)\x19: %s", name, colon + 2);
    else if (tell)
        Q_snprintf(line, sizeof(line), "chat \"\x19[%s\x19]\x19: %s", name, colon + 2);
    else
        Q_snprintf(line, sizeof(line), "chat \"%s\x19: %s", name, colon + 2);
    len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
    Q_strlcat(line, "\"", sizeof(line));

    Q3Bot_ServerCommand(client, line);
}

// ==========================================================================
// BotEngine_*: FROM THE GAME

// SpawnEntities: the map's entities, for botlib (BSPEntityData)
void BotEngine_SaveEntities(const char *entities)
{
    size_t len = entities ? strlen(entities) : 0;

    // the last map's library and AAS go with it
    Q3Bot_Shutdown();

    be_entities = gi.TagMalloc(len + 1, TAG_LEVEL);
    memcpy(be_entities, entities ? entities : "", len);
    be_entities[len] = 0;

    be_loaded = be_ready = false;
    be_spawncount++;
    memset(be_players, 0, sizeof(be_players));
}

// Quake III's sv_mapChecksum: the MD4 of the map's BSP, as its AAS records it
static int BE_MapChecksum(void)
{
    void    *buf = NULL;
    int     len, sum;

    len = q2b_load_file(va("maps/%s.bsp", level.mapname), &buf);
    if (len <= 0 || !buf)
        return 0;
    sum = (int)Com_BlockChecksum(buf, len);
    q2b_free_file(buf);
    return sum;
}

/*
=================
BotEngine_Load

The map's AAS and the bot library, once a level, when a bot is about to
play by them. true = Quake III's bots can play this map.
=================
*/
bool BotEngine_Load(void)
{
    if (be_loaded)
        return be_ready;
    be_loaded = true;

    BE_CacheItems();
    be_ready = Q3Bot_LevelStart(level.mapname, BE_MapChecksum()) != 0;
    if (be_ready)
        gi.dprintf("Quake III bots: %s's AAS is in\n", level.mapname);
    return be_ready;
}

bool BotEngine_Ready(void)
{
    return be_ready && Q3Bot_Ready();
}

void BotEngine_Shutdown(void)
{
    Q3Bot_Shutdown();
    be_loaded = be_ready = false;
}

static void BE_TellFlags(edict_t *ent, int client);

bool BotEngine_Add(edict_t *ent, const char *charfile, float skill, const char *name)
{
    int client = (int)(ent - g_edicts) - 1;
    int team;

    if (!BotEngine_Ready() || client < 0 || client >= Q3B_MAX_CLIENTS)
        return false;

    memset(&be_players[client], 0, sizeof(be_players[client]));
    team = BE_Team(ent);
    if (!Q3Bot_AddClient(client, charfile, skill, team, name))
        return false;
    BE_TellFlags(ent, client);
    return true;
}

void BotEngine_Remove(edict_t *ent)
{
    Q3Bot_RemoveClient((int)(ent - g_edicts) - 1);
}

bool BotEngine_Running(const edict_t *ent)
{
    return be_ready && Q3Bot_IsRunning((int)(ent - g_edicts) - 1);
}

void BotEngine_Frame(void)
{
    if (be_ready)
        Q3Bot_Frame(q2b_level_ms());
}

// T_Damage, on a player: who hurt it and with what, as Quake III's game kept it
void BotEngine_Damaged(edict_t *targ, edict_t *attacker)
{
    int t = (int)(targ - g_edicts) - 1;
    int a;

    if (!be_ready || t < 0 || t >= Q3B_MAX_CLIENTS || !attacker)
        return;
    a = BE_Num(attacker);
    be_players[t].lasthurt_client = a;
    be_players[t].lasthurt_mod = BotEngine_MeansOfDeath(meansOfDeath);
    if (attacker->client && attacker != targ) {
        be_players[t].attacker = a;
        if (a >= 0 && a < Q3B_MAX_CLIENTS && !OnSameTeam(targ, attacker))
            be_players[a].hits++;
    }
}

// Quake II's means of death as Quake III's, for its bots' death and kill chat
int BotEngine_MeansOfDeath(int mod)
{
    switch (mod & ~MOD_FRIENDLY_FIRE) {
    case MOD_BLASTER:
    case MOD_MACHINEGUN:
    case MOD_CHAINGUN:
    case MOD_ETF_RIFLE:
    case MOD_Q3_MACHINEGUN:     return Q3B_MOD_MACHINEGUN;
    case MOD_SHOTGUN:
    case MOD_SSHOTGUN:
    case MOD_Q3_SHOTGUN:        return Q3B_MOD_SHOTGUN;
    case MOD_CHAINFIST:
    case MOD_Q3_GAUNTLET:       return Q3B_MOD_GAUNTLET;
    case MOD_GRENADE:
    case MOD_HANDGRENADE:
    case MOD_PROX:
    case MOD_Q3_GRENADE:        return Q3B_MOD_GRENADE;
    case MOD_G_SPLASH:
    case MOD_HG_SPLASH:
    case MOD_HELD_GRENADE:
    case MOD_Q3_G_SPLASH:       return Q3B_MOD_GRENADE_SPLASH;
    case MOD_ROCKET:
    case MOD_PHALANX:
    case MOD_Q3_ROCKET:         return Q3B_MOD_ROCKET;
    case MOD_R_SPLASH:
    case MOD_Q3_R_SPLASH:       return Q3B_MOD_ROCKET_SPLASH;
    case MOD_HYPERBLASTER:
    case MOD_BLUEBLASTER:
    case MOD_Q3_PLASMA:         return Q3B_MOD_PLASMA;
    case MOD_Q3_P_SPLASH:       return Q3B_MOD_PLASMA_SPLASH;
    case MOD_RAILGUN:
    case MOD_DISINTEGRATOR:
    case MOD_Q3_RAILGUN:        return Q3B_MOD_RAILGUN;
    case MOD_HEATBEAM:
    case MOD_Q3_LIGHTNING:      return Q3B_MOD_LIGHTNING;
    case MOD_BFG_LASER:
    case MOD_BFG_BLAST:
    case MOD_Q3_BFG:            return Q3B_MOD_BFG;
    case MOD_BFG_EFFECT:
    case MOD_Q3_BFG_SPLASH:     return Q3B_MOD_BFG_SPLASH;
    case MOD_WATER:             return Q3B_MOD_WATER;
    case MOD_SLIME:             return Q3B_MOD_SLIME;
    case MOD_LAVA:              return Q3B_MOD_LAVA;
    case MOD_CRUSH:             return Q3B_MOD_CRUSH;
    case MOD_TELEFRAG:          return Q3B_MOD_TELEFRAG;
    case MOD_FALLING:           return Q3B_MOD_FALLING;
    case MOD_SUICIDE:           return Q3B_MOD_SUICIDE;
    case MOD_TARGET_LASER:      return Q3B_MOD_TARGET_LASER;
    case MOD_TRIGGER_HURT:      return Q3B_MOD_TRIGGER_HURT;
    case MOD_GRAPPLE:           return Q3B_MOD_GRAPPLE;
    default:                    return Q3B_MOD_UNKNOWN;
    }
}

// ClientObituary: Quake III's EV_OBITUARY
void BotEngine_Obituary(edict_t *self, edict_t *attacker, int mod)
{
    if (!be_ready)
        return;
    Q3Bot_Obituary(BE_Num(self), attacker && attacker->client ? BE_Num(attacker) : Q3B_ENTITYNUM_WORLD,
                   BotEngine_MeansOfDeath(mod));
}

// a Quake III powerup back on its spot: its bots hear it and go
void BotEngine_PowerupRespawned(void)
{
    if (be_ready)
        Q3Bot_PowerupRespawned();
}

/*
=================
BotEngine_Print

A print to one of Quake III's bots, or (to NULL) to everyone - g_bot.c's
bprintf and cprintf wrappers. Quake III's server sent its bots every print as
"print" and they read them: the CTF lines ("Doom got the RED flag!", "...
captured the ... flag!", "... returned the ... flag!", Threewave's words and
Quake II's) are how they know who carries a flag. Chat goes the chat way.
=================
*/
void BotEngine_Print(edict_t *to, int printlevel, const char *msg)
{
    char    line[MAX_STRING_CHARS];
    int     i;

    if (!be_ready || !msg || !*msg)
        return;

    if (printlevel == PRINT_CHAT) {
        if (to) {
            BotEngine_Chat(to, msg);
        } else {
            for (i = 0; i < Q3B_MAX_CLIENTS && i < game.maxclients; i++)
                if (Q3Bot_IsRunning(i))
                    BotEngine_Chat(g_edicts + 1 + i, msg);
        }
        return;
    }

    Q_snprintf(line, sizeof(line), "print \"%s\"", msg);
    if (to) {
        Q3Bot_ServerCommand((int)(to - g_edicts) - 1, line);
        return;
    }
    for (i = 0; i < Q3B_MAX_CLIENTS; i++)
        Q3Bot_ServerCommand(i, line);   // which skips all but its bots
}

/*
=================
BotEngine_FlagTaken / FlagReturned / FlagCaptured

CTF (g_ctf.c): Quake III's EV_GLOBAL_TEAM_SOUND, by which its bots keep the
flags' state. Its numbers go by who did it, as g_team.c sent them: the red
team taking the blue flag is GTS_RED_TAKEN, the blue flag going home
GTS_RED_RETURN, the red team scoring GTS_RED_CAPTURE.
=================
*/
void BotEngine_FlagTaken(int flagteam)
{
    if (be_ready)
        Q3Bot_TeamSound(flagteam == TEAM_BLUE ? Q3B_GTS_RED_TAKEN : Q3B_GTS_BLUE_TAKEN);
}

void BotEngine_FlagReturned(int flagteam)
{
    if (be_ready)
        Q3Bot_TeamSound(flagteam == TEAM_BLUE ? Q3B_GTS_RED_RETURN : Q3B_GTS_BLUE_RETURN);
}

void BotEngine_FlagCaptured(int team)
{
    if (be_ready)
        Q3Bot_TeamSound(team == TEAM_RED ? Q3B_GTS_RED_CAPTURE : Q3B_GTS_BLUE_CAPTURE);
}

// a flag off its stand - carried, or lying somewhere
static int BE_FlagAway(int team)
{
    edict_t *flag = CTF_BaseFlag(team);

    return flag && flag->solid == SOLID_NOT;
}

// a bot joining a CTF game under way: where the flags are, and which of its
// team has theirs
static void BE_TellFlags(edict_t *ent, int client)
{
    edict_t *carrier;
    int     team = ent->client->resp.team;

    if (!G_CTF() || (team != TEAM_RED && team != TEAM_BLUE))
        return;
    carrier = CTF_Carrier(team == TEAM_RED ? TEAM_BLUE : TEAM_RED);
    Q3Bot_FlagStatus(client, BE_FlagAway(TEAM_RED), BE_FlagAway(TEAM_BLUE),
                     carrier ? BE_Num(carrier) : -1);
}
