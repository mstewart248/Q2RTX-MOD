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

QUAKE III BOT ENGINE <-> QUAKE II GAME

Quake III's bot engine - botlib (q3bot/botlib) and the game's bot AI
(q3bot/game/ai_*.c), both from ioquake3 - is built into this game DLL. It is
compiled against Quake III's own headers (q3bot/qcommon/q_shared.h, its
bg_public.h, and a cut-down g_local.h of its game), which clash everywhere
with Quake II's: trace_t, usercmd_t, the CONTENTS_ bits, vec3_origin, va...
So the two halves never include each other's headers. They meet only here,
in plain C:

  - q3b_* types: an entity, a player and a usercmd as Quake III sees them,
    filled in by the Quake II side in Quake III's terms (its entity types,
    weapon and item numbers, stats, powerups, means of death).

  - q2b_*: what the Quake III side asks of the Quake II game - traces, files,
    cvars, running a bot's command. Written in g_q3bot.c.

  - Q3Bot_*: what the Quake II game asks of the Quake III side - load the
    map's AAS, set a bot up, run a frame, hear a chat line or an obituary.
    Written in q3bot/game/ai_q2bridge.c.

The Q3B_ numbers are Quake III's; ai_q2bridge.c checks each against the real
definition, so they cannot drift.

==============================================================================
*/

#ifndef Q3BOT_IFACE_H
#define Q3BOT_IFACE_H

// Quake III's entity numbering: clients 0..63, world 1022, none 1023. A
// Quake II edict n is Quake III entity n - 1; its world (edict 0) is 1022.
#define Q3B_MAX_CLIENTS         64
#define Q3B_MAX_ENTITIES        1024
#define Q3B_ENTITYNUM_NONE      1023
#define Q3B_ENTITYNUM_WORLD     1022
// event entities made here (obituaries...) take the numbers from here up;
// a Quake II edict that would land on one is not shown to the bots
#define Q3B_EVENT_BASE          992
#define Q3B_EVENT_COUNT         30

// entityState_t.eType
#define Q3B_ET_GENERAL          0
#define Q3B_ET_PLAYER           1
#define Q3B_ET_ITEM             2
#define Q3B_ET_MISSILE          3
#define Q3B_ET_MOVER            4
#define Q3B_ET_PUSH_TRIGGER     8
#define Q3B_ET_TELEPORT_TRIGGER 9
#define Q3B_ET_INVISIBLE        10
#define Q3B_ET_EVENTS           13

// entityState_t.eFlags
#define Q3B_EF_DEAD             0x00000001
#define Q3B_EF_TELEPORT_BIT     0x00000004
#define Q3B_EF_NODRAW           0x00000080
#define Q3B_EF_FIRING           0x00000100
#define Q3B_EF_TALK             0x00001000

// entityShared_t.svFlags
#define Q3B_SVF_NOCLIENT        0x00000001
#define Q3B_SVF_BOT             0x00000008
#define Q3B_SVF_BROADCAST       0x00000020

// gentity_t.flags
#define Q3B_FL_NOTARGET         0x00000020

// weapon_t
#define Q3B_WP_NONE             0
#define Q3B_WP_GAUNTLET         1
#define Q3B_WP_MACHINEGUN       2
#define Q3B_WP_SHOTGUN          3
#define Q3B_WP_GRENADE_LAUNCHER 4
#define Q3B_WP_ROCKET_LAUNCHER  5
#define Q3B_WP_LIGHTNING        6
#define Q3B_WP_RAILGUN          7
#define Q3B_WP_PLASMAGUN        8
#define Q3B_WP_BFG              9
#define Q3B_WP_GRAPPLING_HOOK   10
#define Q3B_WP_NUM_WEAPONS      11

// powerup_t
#define Q3B_PW_QUAD             1
#define Q3B_PW_BATTLESUIT       2
#define Q3B_PW_HASTE            3
#define Q3B_PW_INVIS            4
#define Q3B_PW_REGEN            5
#define Q3B_PW_FLIGHT           6
#define Q3B_PW_REDFLAG          7
#define Q3B_PW_BLUEFLAG         8

// statIndex_t, persEnum_t
#define Q3B_STAT_HEALTH         0
#define Q3B_STAT_HOLDABLE_ITEM  1
#define Q3B_STAT_WEAPONS        2
#define Q3B_STAT_ARMOR          3
#define Q3B_STAT_DEAD_YAW       4
#define Q3B_STAT_CLIENTS_READY  5
#define Q3B_STAT_MAX_HEALTH     6
#define Q3B_PERS_SCORE          0
#define Q3B_PERS_HITS           1
#define Q3B_PERS_RANK           2
#define Q3B_PERS_TEAM           3
#define Q3B_PERS_SPAWN_COUNT    4
#define Q3B_PERS_ATTACKER       6
#define Q3B_PERS_KILLED         8

// pmtype_t, the pmove flags the AI reads
#define Q3B_PM_NORMAL           0
#define Q3B_PM_NOCLIP           1
#define Q3B_PM_SPECTATOR        2
#define Q3B_PM_DEAD             3
#define Q3B_PM_FREEZE           4
#define Q3B_PM_INTERMISSION     5
#define Q3B_PMF_DUCKED          1
#define Q3B_PMF_JUMP_HELD       2

// weaponstate_t
#define Q3B_WEAPON_READY        0
#define Q3B_WEAPON_RAISING      1
#define Q3B_WEAPON_DROPPING     2
#define Q3B_WEAPON_FIRING       3

// team_t, gametype_t, clientConnected_t
#define Q3B_TEAM_FREE           0
#define Q3B_TEAM_RED            1
#define Q3B_TEAM_BLUE           2
#define Q3B_TEAM_SPECTATOR      3
#define Q3B_GT_FFA              0
#define Q3B_GT_TOURNAMENT       1
#define Q3B_GT_SINGLE_PLAYER    2
#define Q3B_GT_TEAM             3
#define Q3B_GT_CTF              4
#define Q3B_CON_DISCONNECTED    0
#define Q3B_CON_CONNECTING      1
#define Q3B_CON_CONNECTED       2

// usercmd_t.buttons
#define Q3B_BUTTON_ATTACK       1
#define Q3B_BUTTON_TALK         2
#define Q3B_BUTTON_USE_HOLDABLE 4
#define Q3B_BUTTON_GESTURE      8
#define Q3B_BUTTON_WALKING      16

// meansOfDeath_t (no mission pack)
#define Q3B_MOD_UNKNOWN         0
#define Q3B_MOD_SHOTGUN         1
#define Q3B_MOD_GAUNTLET        2
#define Q3B_MOD_MACHINEGUN      3
#define Q3B_MOD_GRENADE         4
#define Q3B_MOD_GRENADE_SPLASH  5
#define Q3B_MOD_ROCKET          6
#define Q3B_MOD_ROCKET_SPLASH   7
#define Q3B_MOD_PLASMA          8
#define Q3B_MOD_PLASMA_SPLASH   9
#define Q3B_MOD_RAILGUN         10
#define Q3B_MOD_LIGHTNING       11
#define Q3B_MOD_BFG             12
#define Q3B_MOD_BFG_SPLASH      13
#define Q3B_MOD_WATER           14
#define Q3B_MOD_SLIME           15
#define Q3B_MOD_LAVA            16
#define Q3B_MOD_CRUSH           17
#define Q3B_MOD_TELEFRAG        18
#define Q3B_MOD_FALLING         19
#define Q3B_MOD_SUICIDE         20
#define Q3B_MOD_TARGET_LASER    21
#define Q3B_MOD_TRIGGER_HURT    22
#define Q3B_MOD_GRAPPLE         23

// global_team_sound_t (CTF)
#define Q3B_GTS_RED_CAPTURE     0
#define Q3B_GTS_BLUE_CAPTURE    1
#define Q3B_GTS_RED_RETURN      2
#define Q3B_GTS_BLUE_RETURN     3
#define Q3B_GTS_RED_TAKEN       4
#define Q3B_GTS_BLUE_TAKEN      5

// Quake III's bg_itemlist index of each item: the "modelindex" botfiles'
// items.c gives each (MODELINDEX_* in q3bot/game/inv.h), and what an item
// entity's modelindex is to the bots
#define Q3B_IT_ARMORSHARD       1
#define Q3B_IT_ARMORCOMBAT      2
#define Q3B_IT_ARMORBODY        3
#define Q3B_IT_HEALTHSMALL      4
#define Q3B_IT_HEALTH           5
#define Q3B_IT_HEALTHLARGE      6
#define Q3B_IT_HEALTHMEGA       7
#define Q3B_IT_GAUNTLET         8
#define Q3B_IT_SHOTGUN          9
#define Q3B_IT_MACHINEGUN       10
#define Q3B_IT_GRENADELAUNCHER  11
#define Q3B_IT_ROCKETLAUNCHER   12
#define Q3B_IT_LIGHTNING        13
#define Q3B_IT_RAILGUN          14
#define Q3B_IT_PLASMAGUN        15
#define Q3B_IT_BFG10K           16
#define Q3B_IT_GRAPPLINGHOOK    17
#define Q3B_IT_SHELLS           18
#define Q3B_IT_BULLETS          19
#define Q3B_IT_GRENADES         20
#define Q3B_IT_CELLS            21
#define Q3B_IT_LIGHTNINGAMMO    22
#define Q3B_IT_ROCKETS          23
#define Q3B_IT_SLUGS            24
#define Q3B_IT_BFGAMMO          25
#define Q3B_IT_TELEPORTER       26
#define Q3B_IT_MEDKIT           27
#define Q3B_IT_QUAD             28
#define Q3B_IT_ENVIRONMENTSUIT  29
#define Q3B_IT_HASTE            30
#define Q3B_IT_INVISIBILITY     31
#define Q3B_IT_REGEN            32
#define Q3B_IT_FLIGHT           33
#define Q3B_IT_REDFLAG          34
#define Q3B_IT_BLUEFLAG         35

// Quake III's CONTENTS_ bits that cross over (surfaceflags.h)
#define Q3B_CONTENTS_SOLID          0x00000001
#define Q3B_CONTENTS_LAVA           0x00000008
#define Q3B_CONTENTS_SLIME          0x00000010
#define Q3B_CONTENTS_WATER          0x00000020
#define Q3B_CONTENTS_FOG            0x00000040
#define Q3B_CONTENTS_PLAYERCLIP     0x00010000
#define Q3B_CONTENTS_MONSTERCLIP    0x00020000
#define Q3B_CONTENTS_TELEPORTER     0x00040000
#define Q3B_CONTENTS_JUMPPAD        0x00080000
#define Q3B_CONTENTS_DONOTENTER     0x00200000
#define Q3B_CONTENTS_MOVER          0x00800000
#define Q3B_CONTENTS_BODY           0x02000000
#define Q3B_CONTENTS_CORPSE         0x04000000
#define Q3B_CONTENTS_DETAIL         0x08000000
#define Q3B_CONTENTS_TRANSLUCENT    0x20000000
#define Q3B_CONTENTS_TRIGGER        0x40000000

// an entity, as Quake III's gentity_t / entityState_t / entityShared_t
// carry it - only what the bot code reads
typedef struct {
    int     inuse, linked;
    int     svflags;            // Q3B_SVF_*
    int     flags;              // Q3B_FL_*
    int     eType, eFlags;      // Q3B_ET_*, Q3B_EF_*
    float   origin[3];          // r.currentOrigin = s.pos.trBase = s.origin
    float   angles[3];          // r.currentAngles = s.apos.trBase
    float   old_origin[3];      // s.origin2
    float   mins[3], maxs[3];   // r.mins / r.maxs
    float   absmin[3], absmax[3];
    int     bmodel;             // a brush model; modelindex is then its *number
    int     contents;           // Q3B_CONTENTS_*
    int     modelindex, modelindex2;
    int     frame, solid;
    int     event, eventParm;
    int     otherEntityNum, otherEntityNum2;
    int     clientNum;
    int     powerups;           // 1 << Q3B_PW_*
    int     weapon;             // Q3B_WP_*
    int     legsAnim, torsoAnim;
    int     groundEntityNum;
    int     eventTime;          // level ms the event happened
} q3b_entity_t;

// a player, as gclient_t and its playerState_t carry it
typedef struct {
    int     connected;          // Q3B_CON_*
    int     team;               // Q3B_TEAM_*
    int     bot;                // one of ours
    char    netname[36];
    char    model[64];          // its skin, as "model/skin"
    int     lasthurt_client, lasthurt_mod;

    int     pm_type, pm_flags, pm_time;
    float   origin[3], velocity[3], viewangles[3];
    int     delta_angles[3];
    int     viewheight;
    int     groundEntityNum;
    int     weapon, weaponstate;
    int     eFlags;
    int     stats[16], persistant[16], powerups[16], ammo[16];
} q3b_client_t;

// a bot's usercmd, Quake III's: angles relative to delta_angles, moves -127..127
typedef struct {
    int     angles[3];
    int     buttons;            // Q3B_BUTTON_*
    int     weapon;             // Q3B_WP_*
    int     forwardmove, rightmove, upmove;
} q3b_usercmd_t;

typedef struct {
    int     allsolid, startsolid;
    float   fraction;
    float   endpos[3];
    float   normal[3];
    float   dist;
    int     surfaceflags;
    int     contents;           // Q3B_CONTENTS_*
    int     entityNum;          // Quake III numbering
} q3b_trace_t;

// ---------------------------------------------------------------------------
// q2b_*: g_q3bot.c, the Quake II game

void    q2b_print(int level, const char *msg);          // level: 0 normal, 1 warning, 2 error
void    q2b_error(const char *msg);
int     q2b_milliseconds(void);

void    q2b_trace(q3b_trace_t *out, const float *start, const float *mins, const float *maxs,
                  const float *end, int passent, int q3mask);
void    q2b_entity_trace(q3b_trace_t *out, const float *start, const float *mins, const float *maxs,
                         const float *end, int entnum, int q3mask);
int     q2b_point_contents(const float *p);             // Q3B_CONTENTS_*
int     q2b_in_pvs(const float *a, const float *b);
const char *q2b_entity_string(void);                    // the map's own entity lump
int     q2b_model_bounds(int modelnum, float *mins, float *maxs);

int     q2b_load_file(const char *path, void **buffer); // "baseq3/..." paths too
void    q2b_free_file(void *buffer);

int     q2b_cvar_register(const char *name, const char *value, int flags);
void    q2b_cvar_read(int handle, float *value, int *integer, char *string, int size, int *modcount);
void    q2b_cvar_set(const char *name, const char *value);
void    q2b_cvar_string(const char *name, char *buffer, int size);
void    q2b_console_command(const char *text);

int     q2b_gametype(void);                             // Q3B_GT_*
int     q2b_maxclients(void);
int     q2b_level_ms(void);
int     q2b_intermission(void);

// fills the bots' view of the game: ents[Q3B_MAX_ENTITIES], clients[Q3B_MAX_CLIENTS]
void    q2b_snapshot(q3b_entity_t *ents, q3b_client_t *clients);

// a bot's command, from its say / say_team / tell / kill / use and so on
void    q2b_client_command(int client, const char *command);
// a bot's input for `msec` milliseconds
void    q2b_bot_usercmd(int client, const q3b_usercmd_t *cmd, int msec);
// its delta_angles now (a spawn or a teleporter turns it between commands)
void    q2b_client_delta_angles(int client, int *delta);

// ---------------------------------------------------------------------------
// Q3Bot_*: q3bot/game/ai_q2bridge.c, the Quake III bot engine

void    Q3Bot_Shutdown(void);
int     Q3Bot_LevelStart(const char *mapname, int checksum);    // 1 = the map's AAS is loaded
int     Q3Bot_Ready(void);                                      // AAS loaded and initialised
int     Q3Bot_AddClient(int client, const char *charfile, float skill, int team, const char *name);
void    Q3Bot_RemoveClient(int client);
int     Q3Bot_IsRunning(int client);
void    Q3Bot_Frame(int level_ms);
void    Q3Bot_ServerCommand(int client, const char *text);
void    Q3Bot_Obituary(int target, int attacker, int q3mod);
void    Q3Bot_TeamSound(int gts);       // Q3B_GTS_*: a flag taken, returned, captured
void    Q3Bot_Teleported(const float *origin);
void    Q3Bot_PowerupRespawned(void);   // "sound/items/poweruprespawn.wav": go and get it
// CTF: where the flags are, for a bot that joins with the game under way and
// so heard none of it (status 0 at its stand, 1 away); carrier = its team's
// flag carrier, -1 none
void    Q3Bot_FlagStatus(int client, int redstatus, int bluestatus, int carrier);

#endif // Q3BOT_IFACE_H
