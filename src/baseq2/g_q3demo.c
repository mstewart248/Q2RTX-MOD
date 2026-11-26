/*
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2026 Q2RTX contributors

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

QUAKE III'S DEMOS (g_q3demo.c)

Team Arena's own demos - missionpack/demos/MPDEMO1.dm_48 (mpteam1) and
MPDEMO2.dm_48 (mpteam3), CTF matches its menu played - for the attract loop
(Matt, 2026-11-26: "team arena has demo. Can we include that in the demo
rotation for the attract mode"). The engine's "q3demo <path>"
(server/commands.c) reads the map out of the demo's serverinfo, starts it as
a deathmatch with g_q3_demo set, and this plays the demo in place of running
the level, as g_q1demo.c plays Quake 1's: the world spawns (its sky, music,
Quake III's guns and HUD - level.q3_weapons), none of the map's other
entities do, and what the recorded server messages describe goes onto the
edicts and the player's view the way this game sends its own Quake III
things - the players in their Team Arena uniforms, the guns' flashes, beams
and missiles, the items, the movers, the sounds, Team Arena's HUD.

THE FORMAT. Quake III 1.27's protocol 48 (the demos say 47 in their
serverinfo): blocks of [int sequence][int length][message], length -1 at the
end. A message is a bit stream, least significant bit first, not Huffman
coded (that came with protocol 66): the reliable acknowledge, then commands,
each starting on a byte - the gamestate (configstrings, then entity
baselines), server commands (text: "cs", "print", "tinfo"...), snapshots. A
snapshot is the server time, the snapshot it is a delta from, the area mask,
the player state as a delta, and the entities: a 10-bit number each, merged
with the old snapshot's sorted list, each a delta from its old self or its
baseline. An entity delta names its changed fields by one of 31 known masks
(5 bits) or an explicit 51-bit one - 1.27's own scheme, which UberDemoTools
(GPL) documents; a float is a 13-bit integer or the raw 32 bits. Worked out
and checked against both demos with a prototype parser before this was
written.

TIME. The server sent 20 snapshots a second; this game runs at 10. Each
frame the clock moves 100 ms, the messages up to it are read, every
snapshot's events fire (as cgame's CG_CheckEvents and
CG_CheckPlayerstateEvents did, an entity's event when it changes, an event
entity's once), and the entities and the view go out at the clock - players
interpolated between snapshots (cg_smoothClients 0), everything else on its
trajectory (BG_EvaluateTrajectory). The client lerps the rest of the way. The
clock starts when the player is in (ClientBegin).

NUMBERS. The recorded player (the playerstate's clientNum) is the viewer's
own edict, 1, and its slot in the player skins, 0. Quake III's entity n is
edict base + n, base being the first edict past the clients and whatever the
world spawned; client c's look is CS_PLAYERSKINS + c + 1.

The demo ends at its last block (or a message that can't be read): the
server's nextserver runs - the attract loop's next demo - or, with none, the
server is killed and the menu comes back.

==============================================================================
*/

#include "g_local.h"

// q3bot/qcommon/q_math.c, renamed for the game (q3_rename.h): Quake III's own
// direction table and shotgun pattern
void    Q3_ByteToDir(int b, float *dir);
void    Q3_PerpendicularVector(float *dst, const float *src);
float   Q3_Q_crandom(int *seed);

#define Q3D_GENTITYNUM_BITS     10
#define Q3D_MAX_GENTITIES       (1 << Q3D_GENTITYNUM_BITS)
#define Q3D_ENTITYNUM_NONE      (Q3D_MAX_GENTITIES - 1)
#define Q3D_ENTITYNUM_WORLD     (Q3D_MAX_GENTITIES - 2)
#define Q3D_MAX_CLIENTS         64
#define Q3D_MAX_CONFIGSTRINGS   1024
#define Q3D_PACKET_BACKUP       32
#define Q3D_PACKET_MASK         (Q3D_PACKET_BACKUP - 1)
#define Q3D_MAX_PARSE_ENTITIES  2048
#define Q3D_MAX_MSGLEN          16384
#define Q3D_BIG_STRING          8192
#define Q3D_FLOAT_INT_BITS      13
#define Q3D_FLOAT_INT_BIAS      (1 << (Q3D_FLOAT_INT_BITS - 1))
#define Q3D_FRAME_MSEC          100     // this game's frame
#define Q3D_EVENT_VALID_MSEC    300

// svc_ops_e
#define Q3D_SVC_BAD             0
#define Q3D_SVC_NOP             1
#define Q3D_SVC_GAMESTATE       2
#define Q3D_SVC_CONFIGSTRING    3
#define Q3D_SVC_BASELINE        4
#define Q3D_SVC_SERVERCOMMAND   5
#define Q3D_SVC_SNAPSHOT        7
#define Q3D_SVC_EOF             8

// configstrings (bg_public.h)
#define Q3D_CS_SERVERINFO       0
#define Q3D_CS_SCORES1          6
#define Q3D_CS_SCORES2          7
#define Q3D_CS_FLAGSTATUS       23
#define Q3D_CS_ITEMS            27
#define Q3D_CS_MODELS           32
#define Q3D_CS_SOUNDS           288
#define Q3D_CS_PLAYERS          544
#define Q3D_CS_LOCATIONS        608
#define Q3D_MAX_MODELS          256
#define Q3D_MAX_SOUNDS          256
#define Q3D_MAX_LOCATIONS       64

// entityType_t
#define Q3D_ET_GENERAL          0
#define Q3D_ET_PLAYER           1
#define Q3D_ET_ITEM             2
#define Q3D_ET_MISSILE          3
#define Q3D_ET_MOVER            4
#define Q3D_ET_SPEAKER          7
#define Q3D_ET_INVISIBLE        10
#define Q3D_ET_TEAM             12
#define Q3D_ET_EVENTS           13

// trType_t
#define Q3D_TR_STATIONARY       0
#define Q3D_TR_INTERPOLATE      1
#define Q3D_TR_LINEAR           2
#define Q3D_TR_LINEAR_STOP      3
#define Q3D_TR_SINE             4
#define Q3D_TR_GRAVITY          5
#define Q3D_DEFAULT_GRAVITY     800

// eFlags
#define Q3D_EF_DEAD             0x00000001
#define Q3D_EF_TELEPORT_BIT     0x00000004
#define Q3D_EF_PLAYER_EVENT     0x00000010
#define Q3D_EF_NODRAW           0x00000080
#define Q3D_EF_FIRING           0x00000100
#define Q3D_EV_EVENT_BITS       0x300

// pmtype_t
#define Q3D_PM_NORMAL           0
#define Q3D_PM_DEAD             3
#define Q3D_PM_INTERMISSION     5
#define Q3D_PM_SPINTERMISSION   6
#define Q3D_PMF_DUCKED          1

// statIndex_t, Team Arena's (MISSIONPACK: STAT_PERSISTANT_POWERUP is 2)
#define Q3D_STAT_HEALTH         0
#define Q3D_STAT_HOLDABLE_ITEM  1
#define Q3D_STAT_PERSISTANT     2
#define Q3D_STAT_ARMOR          4
#define Q3D_STAT_DEAD_YAW       5
#define Q3D_PERS_SCORE          0

// powerup_t
#define Q3D_PW_QUAD             1
#define Q3D_PW_BATTLESUIT       2
#define Q3D_PW_HASTE            3
#define Q3D_PW_INVIS            4
#define Q3D_PW_REGEN            5
#define Q3D_PW_FLIGHT           6
#define Q3D_PW_REDFLAG          7
#define Q3D_PW_BLUEFLAG         8
#define Q3D_PW_NEUTRALFLAG      9
#define Q3D_PW_SCOUT            10
#define Q3D_PW_AMMOREGEN        13
#define Q3D_PW_INVULNERABILITY  14
#define Q3D_PW_NUM              16

// animNumber_t
#define Q3D_BOTH_DEAD3          5
#define Q3D_TORSO_GESTURE       6
#define Q3D_LEGS_WALKCR         13
#define Q3D_LEGS_JUMP           18
#define Q3D_LEGS_JUMPB          20
#define Q3D_LEGS_IDLECR         23
#define Q3D_TORSO_GETFLAG       25
#define Q3D_TORSO_NEGATIVE      30
#define Q3D_ANIM_TOGGLEBIT      128

// weapon_t: the Q3W_* tags follow it
#define Q3D_WP_GRENADE          Q3W_GRENADE
#define Q3D_WP_ROCKET           Q3W_ROCKET
#define Q3D_WP_LIGHTNING        Q3W_LIGHTNING
#define Q3D_WP_PLASMA           Q3W_PLASMA
#define Q3D_WP_BFG              Q3W_BFG
#define Q3D_WP_NAILGUN          Q3W_NAILGUN
#define Q3D_WP_PROX             Q3W_PROX
#define Q3D_WP_CHAINGUN         Q3W_CHAINGUN

// gametype_t
#define Q3D_GT_TEAM             3
#define Q3D_GT_CTF              4
#define Q3D_GT_1FCTF            5
#define Q3D_GT_HARVESTER        7

// entity_event_t, 1.27 with MISSIONPACK
enum {
    EV3_NONE,
    EV3_FOOTSTEP, EV3_FOOTSTEP_METAL, EV3_FOOTSPLASH, EV3_FOOTWADE, EV3_SWIM,
    EV3_STEP_4, EV3_STEP_8, EV3_STEP_12, EV3_STEP_16,
    EV3_FALL_SHORT, EV3_FALL_MEDIUM, EV3_FALL_FAR,
    EV3_JUMP_PAD, EV3_JUMP,
    EV3_WATER_TOUCH, EV3_WATER_LEAVE, EV3_WATER_UNDER, EV3_WATER_CLEAR,
    EV3_ITEM_PICKUP, EV3_GLOBAL_ITEM_PICKUP,
    EV3_NOAMMO, EV3_CHANGE_WEAPON, EV3_FIRE_WEAPON,
    EV3_USE_ITEM0,
    EV3_ITEM_RESPAWN = EV3_USE_ITEM0 + 16, EV3_ITEM_POP,
    EV3_PLAYER_TELEPORT_IN, EV3_PLAYER_TELEPORT_OUT,
    EV3_GRENADE_BOUNCE,
    EV3_GENERAL_SOUND, EV3_GLOBAL_SOUND, EV3_GLOBAL_TEAM_SOUND,
    EV3_BULLET_HIT_FLESH, EV3_BULLET_HIT_WALL,
    EV3_MISSILE_HIT, EV3_MISSILE_MISS, EV3_MISSILE_MISS_METAL,
    EV3_RAILTRAIL, EV3_SHOTGUN, EV3_BULLET,
    EV3_PAIN, EV3_DEATH1, EV3_DEATH2, EV3_DEATH3, EV3_OBITUARY,
    EV3_POWERUP_QUAD, EV3_POWERUP_BATTLESUIT, EV3_POWERUP_REGEN,
    EV3_GIB_PLAYER, EV3_SCOREPLUM,
    EV3_PROXIMITY_MINE_STICK, EV3_PROXIMITY_MINE_TRIGGER, EV3_KAMIKAZE,
    EV3_OBELISKEXPLODE, EV3_OBELISKPAIN, EV3_INVUL_IMPACT, EV3_JUICED, EV3_LIGHTNINGBOLT,
    EV3_DEBUG_LINE, EV3_STOPLOOPINGSOUND, EV3_TAUNT
};

// global_team_sound_t
enum {
    GTS3_RED_CAPTURE, GTS3_BLUE_CAPTURE, GTS3_RED_RETURN, GTS3_BLUE_RETURN,
    GTS3_RED_TAKEN, GTS3_BLUE_TAKEN, GTS3_REDOBELISK_ATTACKED, GTS3_BLUEOBELISK_ATTACKED,
    GTS3_REDTEAM_SCORED, GTS3_BLUETEAM_SCORED, GTS3_REDTEAM_TOOK_LEAD,
    GTS3_BLUETEAM_TOOK_LEAD, GTS3_TEAMS_ARE_TIED, GTS3_KAMIKAZE
};

// meansOfDeath_t, 1.27 with MISSIONPACK
enum {
    MOD3_UNKNOWN, MOD3_SHOTGUN, MOD3_GAUNTLET, MOD3_MACHINEGUN, MOD3_GRENADE,
    MOD3_GRENADE_SPLASH, MOD3_ROCKET, MOD3_ROCKET_SPLASH, MOD3_PLASMA,
    MOD3_PLASMA_SPLASH, MOD3_RAILGUN, MOD3_LIGHTNING, MOD3_BFG, MOD3_BFG_SPLASH,
    MOD3_WATER, MOD3_SLIME, MOD3_LAVA, MOD3_CRUSH, MOD3_TELEFRAG, MOD3_FALLING,
    MOD3_SUICIDE, MOD3_TARGET_LASER, MOD3_TRIGGER_HURT, MOD3_NAIL, MOD3_CHAINGUN,
    MOD3_PROXIMITY_MINE, MOD3_KAMIKAZE, MOD3_JUICED, MOD3_GRAPPLE
};

/*
Team Arena's bg_itemlist, by the index an item entity's modelindex and a
pickup's eventParm carry: the classname (which Q3_DemoItem turns into this
game's item); the sound everyone hears a powerup picked up with
(EV_GLOBAL_ITEM_PICKUP); the one the player taking it hears (EV_ITEM_PICKUP:
the item's pickup_sound, n_health for the powerups and team items, a
persistent powerup's cgs.media.scoutSound and the rest); the powerup, for the
HUD's icons. The demos' CS_ITEMS strings are this long, their flags sit at the
bases as 34 and 35.
*/
#define Q3D_ARMOR       "misc/ar2_pkup.wav"
#define Q3D_GUN         "misc/w_pkup.wav"
#define Q3D_AMMO        "misc/am_pkup.wav"
#define Q3D_HOLDABLE    "items/holdable.wav"
#define Q3D_TAKEN       "items/n_health.wav"

static const struct {
    const char  *classname;
    const char  *global_sound;      // EV_GLOBAL_ITEM_PICKUP
    const char  *own_sound;         // EV_ITEM_PICKUP
    int         powerup;            // PW_* it gives, for the HUD's icons
} q3d_items[] = {
    { NULL },
    { "item_armor_shard",       NULL, "misc/ar1_pkup.wav" },
    { "item_armor_combat",      NULL, Q3D_ARMOR },
    { "item_armor_body",        NULL, Q3D_ARMOR },
    { "item_health_small",      NULL, "items/s_health.wav" },
    { "item_health",            NULL, "items/n_health.wav" },
    { "item_health_large",      NULL, "items/l_health.wav" },
    { "item_health_mega",       NULL, "items/m_health.wav" },
    { "weapon_gauntlet",        NULL, Q3D_GUN },
    { "weapon_shotgun",         NULL, Q3D_GUN },
    { "weapon_machinegun",      NULL, Q3D_GUN },
    { "weapon_grenadelauncher", NULL, Q3D_GUN },
    { "weapon_rocketlauncher",  NULL, Q3D_GUN },
    { "weapon_lightning",       NULL, Q3D_GUN },
    { "weapon_railgun",         NULL, Q3D_GUN },
    { "weapon_plasmagun",       NULL, Q3D_GUN },
    { "weapon_bfg",             NULL, Q3D_GUN },
    { "weapon_grapplinghook",   NULL, Q3D_GUN },
    { "ammo_shells",            NULL, Q3D_AMMO },
    { "ammo_bullets",           NULL, Q3D_AMMO },
    { "ammo_grenades",          NULL, Q3D_AMMO },
    { "ammo_cells",             NULL, Q3D_AMMO },
    { "ammo_lightning",         NULL, Q3D_AMMO },
    { "ammo_rockets",           NULL, Q3D_AMMO },
    { "ammo_slugs",             NULL, Q3D_AMMO },
    { "ammo_bfg",               NULL, Q3D_AMMO },
    { "holdable_teleporter",    NULL, Q3D_HOLDABLE },
    { "holdable_medkit",        NULL, Q3D_HOLDABLE },
    { "item_quad",      "items/quaddamage.wav",     Q3D_TAKEN, Q3D_PW_QUAD },
    { "item_enviro",    "items/protect.wav",        Q3D_TAKEN, Q3D_PW_BATTLESUIT },
    { "item_haste",     "items/haste.wav",          Q3D_TAKEN, Q3D_PW_HASTE },
    { "item_invis",     "items/invisibility.wav",   Q3D_TAKEN, Q3D_PW_INVIS },
    { "item_regen",     "items/regeneration.wav",   Q3D_TAKEN, Q3D_PW_REGEN },
    { "item_flight",    "items/flight.wav",         Q3D_TAKEN, Q3D_PW_FLIGHT },
    { "team_CTF_redflag",       NULL, Q3D_TAKEN, Q3D_PW_REDFLAG },
    { "team_CTF_blueflag",      NULL, Q3D_TAKEN, Q3D_PW_BLUEFLAG },
    { "holdable_kamikaze",      NULL, Q3D_HOLDABLE },
    { "holdable_portal",        NULL, Q3D_HOLDABLE },
    { "holdable_invulnerability", NULL, Q3D_HOLDABLE, Q3D_PW_INVULNERABILITY },
    { "ammo_nails",             NULL, Q3D_AMMO },
    { "ammo_mines",             NULL, Q3D_AMMO },
    { "ammo_belt",              NULL, Q3D_AMMO },
    { "item_scout",     NULL, "items/cl_scout.wav",     Q3D_PW_SCOUT },
    { "item_guard",     NULL, "items/cl_guard.wav",     Q3D_PW_SCOUT + 1 },
    { "item_doubler",   NULL, "items/cl_doubler.wav",   Q3D_PW_SCOUT + 2 },
    { "item_ammoregen", NULL, "items/cl_ammoregen.wav", Q3D_PW_AMMOREGEN },
    { "team_CTF_neutralflag",   NULL, Q3D_TAKEN, Q3D_PW_NEUTRALFLAG },
    { "item_redcube",           NULL, Q3D_TAKEN },
    { "item_bluecube",          NULL, Q3D_TAKEN },
    { "weapon_nailgun",         NULL, Q3D_GUN },
    { "weapon_prox_launcher",   NULL, Q3D_GUN },
    { "weapon_chaingun",        NULL, Q3D_GUN },
};

#define Q3D_NUM_ITEMS   q_countof(q3d_items)

// a powerup's item, for its icon
static int q3d_powerup_item(int pw)
{
    int i;

    for (i = 1; i < Q3D_NUM_ITEMS; i++)
        if (q3d_items[i].powerup == pw)
            return i;
    return 0;
}

/*
==============================================================================

THE STATES

==============================================================================
*/

typedef struct {
    int     trType, trTime, trDuration;
    float   trBase[3], trDelta[3];
} q3d_traj_t;

typedef struct {
    int         number;
    int         eType, eFlags;
    q3d_traj_t  pos, apos;
    int         time, time2;
    float       origin[3], origin2[3], angles[3], angles2[3];
    int         otherEntityNum, otherEntityNum2, groundEntityNum;
    int         loopSound, constantLight, modelindex, modelindex2, frame, clientNum, solid;
    int         event, eventParm, powerups, weapon, legsAnim, torsoAnim, generic1;
} q3d_es_t;

typedef struct {
    int     commandTime, pm_type, bobCycle, pm_flags, pm_time;
    float   origin[3], velocity[3];
    int     weaponTime, gravity, speed, delta_angles[3];
    int     groundEntityNum, legsTimer, torsoTimer, legsAnim, torsoAnim, movementDir;
    int     eFlags, eventSequence, events[2], eventParms[2];
    int     externalEvent, externalEventParm, clientNum, weapon, weaponstate;
    float   viewangles[3];
    int     viewheight, damageEvent, damageYaw, damagePitch, damageCount;
    float   grapplePoint[3];
    int     jumppad_ent, loopSound, generic1;
    int     stats[16], persistant[16], ammo[16], powerups[16];
} q3d_ps_t;

// a field of either, in the order the deltas send them; bits 0 is a float,
// negative a signed integer
typedef struct {
    size_t  ofs;
    int     bits;
} q3d_field_t;

#define ESF(f, b)   { offsetof(q3d_es_t, f), b }
static const q3d_field_t q3d_es_fields[] = {
    ESF(eType, 8), ESF(eFlags, 19),
    ESF(pos.trType, 8), ESF(pos.trTime, 32), ESF(pos.trDuration, 32),
    ESF(pos.trBase[0], 0), ESF(pos.trBase[1], 0), ESF(pos.trBase[2], 0),
    ESF(pos.trDelta[0], 0), ESF(pos.trDelta[1], 0), ESF(pos.trDelta[2], 0),
    ESF(apos.trType, 8), ESF(apos.trTime, 32), ESF(apos.trDuration, 32),
    ESF(apos.trBase[0], 0), ESF(apos.trBase[1], 0), ESF(apos.trBase[2], 0),
    ESF(apos.trDelta[0], 0), ESF(apos.trDelta[1], 0), ESF(apos.trDelta[2], 0),
    ESF(time, 32), ESF(time2, 32),
    ESF(origin[0], 0), ESF(origin[1], 0), ESF(origin[2], 0),
    ESF(origin2[0], 0), ESF(origin2[1], 0), ESF(origin2[2], 0),
    ESF(angles[0], 0), ESF(angles[1], 0), ESF(angles[2], 0),
    ESF(angles2[0], 0), ESF(angles2[1], 0), ESF(angles2[2], 0),
    ESF(otherEntityNum, Q3D_GENTITYNUM_BITS), ESF(otherEntityNum2, Q3D_GENTITYNUM_BITS),
    ESF(groundEntityNum, Q3D_GENTITYNUM_BITS),
    ESF(loopSound, 8), ESF(constantLight, 32), ESF(modelindex, 8), ESF(modelindex2, 8),
    ESF(frame, 16), ESF(clientNum, 8), ESF(solid, 24), ESF(event, 10), ESF(eventParm, 8),
    ESF(powerups, 16), ESF(weapon, 8), ESF(legsAnim, 8), ESF(torsoAnim, 8), ESF(generic1, 8),
};
#undef ESF

#define PSF(f, b)   { offsetof(q3d_ps_t, f), b }
static const q3d_field_t q3d_ps_fields[] = {
    PSF(commandTime, 32), PSF(pm_type, 8), PSF(bobCycle, 8), PSF(pm_flags, 16), PSF(pm_time, -16),
    PSF(origin[0], 0), PSF(origin[1], 0), PSF(origin[2], 0),
    PSF(velocity[0], 0), PSF(velocity[1], 0), PSF(velocity[2], 0),
    PSF(weaponTime, -16), PSF(gravity, 16), PSF(speed, 16),
    PSF(delta_angles[0], 16), PSF(delta_angles[1], 16), PSF(delta_angles[2], 16),
    PSF(groundEntityNum, Q3D_GENTITYNUM_BITS), PSF(legsTimer, 8), PSF(torsoTimer, 12),
    PSF(legsAnim, 8), PSF(torsoAnim, 8), PSF(movementDir, 4),
    PSF(eFlags, 16), PSF(eventSequence, 16), PSF(events[0], 8), PSF(events[1], 8),
    PSF(eventParms[0], 8), PSF(eventParms[1], 8),
    PSF(externalEvent, 10), PSF(externalEventParm, 8), PSF(clientNum, 8),
    PSF(weapon, 5), PSF(weaponstate, 4),
    PSF(viewangles[0], 0), PSF(viewangles[1], 0), PSF(viewangles[2], 0),
    PSF(viewheight, -8), PSF(damageEvent, 8), PSF(damageYaw, 8), PSF(damagePitch, 8), PSF(damageCount, 8),
    PSF(grapplePoint[0], 0), PSF(grapplePoint[1], 0), PSF(grapplePoint[2], 0),
    PSF(jumppad_ent, Q3D_GENTITYNUM_BITS), PSF(loopSound, 16), PSF(generic1, 8),
};
#undef PSF

// 1.27's entity delta masks: the fields (bit i of byte i / 8) an index in 5
// bits stands for; 31 sends the mask itself
static const byte q3d_known_masks[31][7] = {
    { 0x60, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0xE1, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00 },
    { 0x60, 0x80, 0x00, 0x00, 0x00, 0x10, 0x00 }, { 0xE0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xE0, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00 },
    { 0x40, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x20, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x60, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00 }, { 0xED, 0x07, 0x00, 0x00, 0x00, 0x80, 0x00 },
    { 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0xED, 0x07, 0x00, 0x00, 0x00, 0x30, 0x00 },
    { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0xE0, 0xC0, 0x00, 0x00, 0x00, 0x10, 0x00 }, { 0x60, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00 },
    { 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0xE1, 0x00, 0x00, 0x00, 0x04, 0x20, 0x00 },
    { 0xE1, 0x00, 0xC0, 0x01, 0x20, 0x20, 0x00 }, { 0xE0, 0xC0, 0x00, 0x00, 0x01, 0x00, 0x00 },
    { 0x60, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x40, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x60, 0xC0, 0x00, 0x00, 0x01, 0x00, 0x00 }, { 0x60, 0xC0, 0x00, 0x00, 0x00, 0x10, 0x00 },
    { 0x60, 0x80, 0x00, 0x00, 0x01, 0x00, 0x01 }, { 0x60, 0x80, 0x00, 0x00, 0x00, 0x30, 0x00 },
    { 0xE0, 0x80, 0x00, 0x00, 0x00, 0x10, 0x00 }, { 0x20, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x60, 0x80, 0x00, 0x00, 0x00, 0x00, 0x02 },
};

typedef struct {
    bool        valid;
    int         message_num;
    int         server_time;
    q3d_ps_t    ps;
    int         first_entity;   // in the parse ring
    int         num_entities;
} q3d_snap_t;

// an item's look, as Q3_DemoItem gives it
typedef struct {
    bool        resolved;
    gitem_t     *item;
    int         modelindex, modelindex2, effects, renderfx;
    float       scale;
} q3d_look_t;

// what is kept of one Quake III entity between frames
typedef struct {
    int         last_seen;      // the last snapshot it was in (server time), 0 never
    int         prev_event;     // its event there, with the toggle bits
    int         prev_etype;
    int         prev_eflags;    // as last drawn
    bool        shown;          // drawn last frame
    int         present_frame;  // q3d.frame it was in
    vec3_t      sent_origin;
    int         look;           // what the edict was last set up as (eType | modelindex << 8)
    int         death, death_frame;
    int         pain_until, pain_time;
    int         flash_frame;    // a muzzle flash went out this frame
    bool        lg_firing, lg_was_firing;   // the Lightning Gun's trigger, this frame and last
    int         shooter;        // a plasma ball's: the edict it is drawn from
} q3d_ent_t;

// a player's configstring, taken apart
typedef struct {
    bool        active;
    char        name[36];
    char        model[MAX_QPATH];
    int         team;           // Quake III's: 0 free, 1 red, 2 blue, 3 spectator
    int         task;
    char        sex;            // 'm', 'f', 'n'
} q3d_player_t;

typedef struct {
    int         event, parm;
    bool        ps;             // the recorded player's own
    int         time;           // its snapshot's
    q3d_es_t    es;             // the entity as it was then
} q3d_event_t;

#define Q3D_MAX_EVENTS  256

static struct {
    bool        active;         // this level plays a demo
    bool        broken;         // it couldn't be read: over at the first frame
    bool        started;        // the player is in: the clock runs
    bool        finished;       // the next thing has been asked for
    bool        loading;        // the first messages: nothing goes out
    bool        level_started;  // Q3Demo_LevelStart has run: configstrings go out as they change
    char        path[MAX_QPATH];

    byte        *data;          // the whole file (TAG_LEVEL)
    int         size, pos;

    // the message being read
    const byte  *msg;
    int         msglen, bit;
    bool        bad;
    int         message_num;

    char        *cs[Q3D_MAX_CONFIGSTRINGS];
    char        bcs[Q3D_BIG_STRING];    // a configstring coming in pieces (bcs0/1/2)
    q3d_es_t    *baselines;             // Q3D_MAX_GENTITIES (TAG_LEVEL)
    q3d_es_t    *parse_ents;            // Q3D_MAX_PARSE_ENTITIES
    int         parse_num;
    q3d_snap_t  snaps[Q3D_PACKET_BACKUP];
    q3d_snap_t  *cur, *prev;

    int         time;           // the demo's clock: its server's milliseconds
    int         frame;          // frames played

    // the game it was
    char        mapname[MAX_QPATH];
    int         client;         // the recorded player
    int         gametype, capturelimit, fraglimit;
    char        red_team[32], blue_team[32];
    q3d_player_t players[Q3D_MAX_CLIENTS];
    char        hud[MAX_QPATH];

    int         edict_base;
    q3d_ent_t   *ents;          // Q3D_MAX_GENTITIES (TAG_LEVEL)
    int         max_ent;        // the highest number seen
    q3d_look_t  looks[Q3D_NUM_ITEMS];
    int         sounds[Q3D_MAX_SOUNDS];     // CS_SOUNDS + i as this game's, 0 not yet, -1 none

    q3d_event_t events[Q3D_MAX_EVENTS];
    int         num_events;

    // the recorded player
    int         shots;          // Q3_GUNFRAME_SHOTS
    bool        lg_firing, lg_was_firing;
    int         pickup_item, pickup_until;
    vec3_t      view_origin;
} q3d;

static cvar_t *g_q3_demo;

bool Q3Demo_Active(void)
{
    return q3d.active;
}

/*
==============================================================================

READING

==============================================================================
*/

static int q3d_bits(int bits)
{
    bool        sgn = bits < 0;
    unsigned    v = 0;
    int         i;

    if (sgn)
        bits = -bits;
    for (i = 0; i < bits; i++) {
        if (q3d.bit >= q3d.msglen * 8) {
            q3d.bad = true;
            return 0;
        }
        v |= ((unsigned)(q3d.msg[q3d.bit >> 3] >> (q3d.bit & 7)) & 1u) << i;
        q3d.bit++;
    }
    if (sgn && bits < 32 && (v & (1u << (bits - 1))))
        v |= ~0u << bits;
    return (int)v;
}

static int q3d_byte(void)  { return q3d_bits(8); }
static int q3d_short(void) { return q3d_bits(-16); }
static int q3d_long(void)  { return q3d_bits(32); }

// MSG_ReadString / MSG_ReadBigString: to a 0 (or the message's end)
static const char *q3d_string(char *buf, size_t size)
{
    size_t n = 0;

    for (;;) {
        int c = q3d_byte();

        if (q3d.bad || !c)
            break;
        if (c == '%' || c > 127)
            c = '.';
        if (n + 1 < size)
            buf[n++] = c;
    }
    buf[n] = 0;
    return buf;
}

static void q3d_read_field(byte *base, const q3d_field_t *f)
{
    if (!f->bits) {
        float v;

        if (q3d_bits(1)) {
            int raw = q3d_long();
            memcpy(&v, &raw, sizeof(v));
        } else {
            v = (float)(q3d_bits(Q3D_FLOAT_INT_BITS) - Q3D_FLOAT_INT_BIAS);
        }
        memcpy(base + f->ofs, &v, sizeof(v));
    } else {
        int v = q3d_bits(f->bits);
        memcpy(base + f->ofs, &v, sizeof(v));
    }
}

// MSG_ReadDeltaEntity: false when the entity was removed
static bool q3d_read_delta_entity(const q3d_es_t *from, q3d_es_t *to, int number)
{
    byte    mask[7];
    int     i, index;

    if (q3d_bits(1))
        return false;
    *to = *from;
    to->number = number;
    if (!q3d_bits(1))
        return true;    // no change

    index = q3d_bits(5);
    if (index == 31) {
        for (i = 0; i < 6; i++)
            mask[i] = q3d_bits(8);
        mask[6] = q3d_bits(3);
    } else {
        memcpy(mask, q3d_known_masks[index], sizeof(mask));
    }
    for (i = 0; i < q_countof(q3d_es_fields); i++)
        if (mask[i >> 3] & (1 << (i & 7)))
            q3d_read_field((byte *)to, &q3d_es_fields[i]);
    return true;
}

// MSG_ReadDeltaPlayerstate
static void q3d_read_delta_ps(const q3d_ps_t *from, q3d_ps_t *to)
{
    int i, bits;

    *to = *from;
    for (i = 0; i < q_countof(q3d_ps_fields); i++)
        if (q3d_bits(1))
            q3d_read_field((byte *)to, &q3d_ps_fields[i]);

    if (q3d_bits(1)) {
        bits = q3d_bits(16);
        for (i = 0; i < 16; i++)
            if (bits & (1 << i))
                to->stats[i] = q3d_short();
    }
    if (q3d_bits(1)) {
        bits = q3d_bits(16);
        for (i = 0; i < 16; i++)
            if (bits & (1 << i))
                to->persistant[i] = q3d_short();
    }
    if (q3d_bits(1)) {
        bits = q3d_bits(16);
        for (i = 0; i < 16; i++)
            if (bits & (1 << i))
                to->ammo[i] = q3d_short();
    }
    if (q3d_bits(1)) {
        bits = q3d_bits(16);
        for (i = 0; i < 16; i++)
            if (bits & (1 << i))
                to->powerups[i] = q3d_long();
    }
}

static const char *q3d_cs(int index)
{
    if (index < 0 || index >= Q3D_MAX_CONFIGSTRINGS || !q3d.cs[index])
        return "";
    return q3d.cs[index];
}

static void q3d_cs_changed(int index);

static void q3d_set_cs(int index, const char *s)
{
    size_t len;

    if (index < 0 || index >= Q3D_MAX_CONFIGSTRINGS)
        return;
    if (q3d.cs[index])
        gi.TagFree(q3d.cs[index]);
    q3d.cs[index] = NULL;
    len = strlen(s);
    if (len) {
        q3d.cs[index] = gi.TagMalloc(len + 1, TAG_LEVEL);
        memcpy(q3d.cs[index], s, len + 1);
    }
    if (q3d.level_started)
        q3d_cs_changed(index);
}

static void q3d_parse_gamestate(void)
{
    static char buf[Q3D_BIG_STRING];

    q3d_long();     // the server command sequence
    for (;;) {
        int c = q3d_byte();

        if (q3d.bad || c == Q3D_SVC_EOF || c == Q3D_SVC_BAD)
            break;
        if (c == Q3D_SVC_CONFIGSTRING) {
            int index = q3d_short();
            q3d_set_cs(index, q3d_string(buf, sizeof(buf)));
        } else if (c == Q3D_SVC_BASELINE) {
            static const q3d_es_t null_state;
            int n = q3d_bits(Q3D_GENTITYNUM_BITS);

            if (!q3d_read_delta_entity(&null_state, &q3d.baselines[n], n))
                memset(&q3d.baselines[n], 0, sizeof(q3d.baselines[n]));
        } else {
            q3d.bad = true;
        }
    }
}

static void q3d_server_command(const char *text);

static void q3d_parse_command(void)
{
    static char buf[Q3D_BIG_STRING];

    q3d_long();     // its sequence
    q3d_server_command(q3d_string(buf, sizeof(buf)));
}

static void q3d_add_entity(q3d_snap_t *snap, const q3d_es_t *es)
{
    q3d.parse_ents[q3d.parse_num & (Q3D_MAX_PARSE_ENTITIES - 1)] = *es;
    q3d.parse_num++;
    snap->num_entities++;
}

static void q3d_delta_entity(q3d_snap_t *snap, int number, const q3d_es_t *from)
{
    q3d_es_t *to = &q3d.parse_ents[q3d.parse_num & (Q3D_MAX_PARSE_ENTITIES - 1)];

    if (!q3d_read_delta_entity(from, to, number))
        return;     // removed
    q3d.parse_num++;
    snap->num_entities++;
}

static const q3d_es_t *q3d_snap_entity(const q3d_snap_t *snap, int i)
{
    return &q3d.parse_ents[(snap->first_entity + i) & (Q3D_MAX_PARSE_ENTITIES - 1)];
}

// CL_ParsePacketEntities: the old snapshot's entities merged with the new numbers
static void q3d_parse_entities(const q3d_snap_t *old, q3d_snap_t *snap)
{
    const q3d_es_t  *oldstate = NULL;
    int             oldindex = 0, oldnum, newnum;

    snap->first_entity = q3d.parse_num;
    snap->num_entities = 0;

#define Q3D_OLDNEXT \
    do { \
        if (!old || oldindex >= old->num_entities) { \
            oldnum = 99999; \
        } else { \
            oldstate = q3d_snap_entity(old, oldindex); \
            oldnum = oldstate->number; \
        } \
    } while (0)

    Q3D_OLDNEXT;
    for (;;) {
        newnum = q3d_bits(Q3D_GENTITYNUM_BITS);
        if (q3d.bad || newnum == Q3D_ENTITYNUM_NONE)
            break;
        while (oldnum < newnum) {
            q3d_add_entity(snap, oldstate);
            oldindex++;
            Q3D_OLDNEXT;
        }
        if (oldnum == newnum) {
            q3d_delta_entity(snap, newnum, oldstate);
            oldindex++;
            Q3D_OLDNEXT;
        } else {
            q3d_delta_entity(snap, newnum, &q3d.baselines[newnum]);
        }
    }
    while (oldnum != 99999) {
        q3d_add_entity(snap, oldstate);
        oldindex++;
        Q3D_OLDNEXT;
    }
#undef Q3D_OLDNEXT
}

static void q3d_transition(q3d_snap_t *snap);

static void q3d_parse_snapshot(void)
{
    static const q3d_ps_t   null_ps;
    q3d_snap_t              snap, *old = NULL;
    int                     delta, len, i;

    memset(&snap, 0, sizeof(snap));
    snap.message_num = q3d.message_num;
    snap.server_time = q3d_long();
    delta = q3d_byte();
    snap.valid = true;
    if (delta) {
        int num = q3d.message_num - delta;

        old = &q3d.snaps[num & Q3D_PACKET_MASK];
        if (!old->valid || old->message_num != num ||
            q3d.parse_num - old->first_entity > Q3D_MAX_PARSE_ENTITIES - 128) {
            old = NULL;
            snap.valid = false;     // read, to get past it, and dropped
        }
    }
    q3d_byte();     // snapFlags
    len = q3d_byte();
    for (i = 0; i < len; i++)
        q3d_byte();     // the area mask

    q3d_read_delta_ps(old ? &old->ps : &null_ps, &snap.ps);
    q3d_parse_entities(old, &snap);
    if (q3d.bad || !snap.valid)
        return;

    q3d.snaps[snap.message_num & Q3D_PACKET_MASK] = snap;
    q3d_transition(&q3d.snaps[snap.message_num & Q3D_PACKET_MASK]);
}

// the next block: false at the end, or one that can't be read
static bool q3d_next_message(void)
{
    int seq, len;

    if (q3d.pos + 8 > q3d.size)
        return false;
    seq = LittleLong(*(const int32_t *)(q3d.data + q3d.pos));
    len = LittleLong(*(const int32_t *)(q3d.data + q3d.pos + 4));
    q3d.pos += 8;
    if (len < 0 || len > Q3D_MAX_MSGLEN || q3d.pos + len > q3d.size)
        return false;   // -1: the end

    q3d.msg = q3d.data + q3d.pos;
    q3d.msglen = len;
    q3d.bit = 0;
    q3d.bad = false;
    q3d.message_num = seq;
    q3d.pos += len;

    q3d_long();     // the reliable acknowledge
    while (!q3d.bad && q3d.msglen * 8 - q3d.bit >= 8) {
        int cmd = q3d_byte();

        if (cmd == Q3D_SVC_EOF || cmd == Q3D_SVC_BAD)
            break;
        switch (cmd) {
        case Q3D_SVC_NOP:
            break;
        case Q3D_SVC_GAMESTATE:
            q3d_parse_gamestate();
            break;
        case Q3D_SVC_SERVERCOMMAND:
            q3d_parse_command();
            break;
        case Q3D_SVC_SNAPSHOT:
            q3d_parse_snapshot();
            break;
        default:
            q3d.bad = true;
            break;
        }
        q3d.bit = (q3d.bit + 7) & ~7;   // the next command starts on a byte
    }
    return !q3d.bad;
}

/*
==============================================================================

NUMBERS, NAMES, SOUNDS

==============================================================================
*/

// Quake III's entity n as an edict, -1 for none
static int q3d_edict(int n)
{
    int num;

    if (n < 0 || n >= Q3D_ENTITYNUM_WORLD)
        return -1;
    if (n == q3d.client)
        return 1;
    num = q3d.edict_base + n;
    return num < game.maxentities ? num : -1;
}

// client c's CS_PLAYERSKINS slot
static int q3d_slot(int c)
{
    if (c == q3d.client)
        return 0;
    return c >= 0 && c < Q3D_MAX_CLIENTS ? c + 1 : 0;
}

static void q3d_strip(char *out, size_t size, const char *in)
{
    size_t n = 0;

    while (*in && n + 1 < size) {
        unsigned char c = *in;

        if (c == '^' && in[1] && in[1] != '^') {    // a colour code
            in += 2;
            continue;
        }
        in++;
        if (c == '\n' || (c >= 32 && c < 127))
            out[n++] = c;
    }
    out[n] = 0;
}

// CS_SOUNDS + index as this game's sound
static int q3d_sound_index(int index)
{
    if (index <= 0 || index >= Q3D_MAX_SOUNDS)
        return 0;
    if (!q3d.sounds[index]) {
        const char *s = q3d_cs(Q3D_CS_SOUNDS + index);
        int i = s[0] ? Q3_SoundIndex(s) : 0;

        q3d.sounds[index] = i ? i : -1;
    }
    return max(q3d.sounds[index], 0);
}

// CS_MODELS + index as this game's model
static int q3d_model_index(int index)
{
    const char *s;

    if (index <= 0 || index >= Q3D_MAX_MODELS)
        return 0;
    s = q3d_cs(Q3D_CS_MODELS + index);
    return s[0] ? gi.modelindex((char *)s) : 0;
}

// a sound from Quake III's entity n: on its edict, where it is now, if it
// was in this frame; else where the event was
static void q3d_sound(int n, const vec3_t pos, int chan, int index, float attn)
{
    int num = q3d_edict(n);

    if (!index)
        return;
    if (num == 1 || (num > 0 && n < Q3D_MAX_GENTITIES && q3d.ents[n].present_frame == q3d.frame))
        gi.sound(&g_edicts[num], chan, index, 1, attn, 0);
    else
        gi.positioned_sound((vec_t *)pos, g_edicts, CHAN_AUTO, index, 1, attn, 0);
}

static void q3d_local_sound(const char *name)
{
    gi.sound(&g_edicts[1], CHAN_AUTO, Q3_SoundIndex(name), 1, ATTN_NONE, 0);
}

// the sex in a Quake III model's animation.cfg, for the obituaries
static char q3d_model_sex(const char *model)
{
    char        *buf = NULL, *text;
    const char  *tok;
    char        sex = 'm';
    int         len;

    if (!model[0])
        return sex;
    len = gi.LoadFile(va("models/players/%s/animation.cfg", model), (void **)&buf);
    if (len <= 0 || !buf)
        return sex;
    text = gi.TagMalloc(len + 1, TAG_LEVEL);
    memcpy(text, buf, len);
    text[len] = 0;
    gi.FreeFile(buf);

    buf = text;
    while (buf) {
        tok = COM_Parse((const char **)&buf);
        if (!buf || !tok[0])
            break;
        if (!Q_stricmp(tok, "sex")) {
            tok = COM_Parse((const char **)&buf);
            if (tok[0] == 'f' || tok[0] == 'F')
                sex = 'f';
            else if (tok[0] == 'n' || tok[0] == 'N')
                sex = 'n';
            break;
        }
    }
    gi.TagFree(text);
    return sex;
}

// CS_PLAYERS + c: who they are, and their look into CS_PLAYERSKINS
static void q3d_player_info(int c)
{
    q3d_player_t    *p = &q3d.players[c];
    const char      *info = q3d_cs(Q3D_CS_PLAYERS + c);
    char            model[MAX_QPATH], *slash;
    const char      *skin;

    if (!info[0]) {
        p->active = false;
        if (q3d.level_started && c != q3d.client)
            gi.configstring(CS_PLAYERSKINS + q3d_slot(c), "");
        return;
    }

    p->active = true;
    // the whole name for the obituaries; Quake II's 15 in the skin's
    q3d_strip(p->name, sizeof(p->name), Info_ValueForKey(info, "n"));
    p->team = atoi(Info_ValueForKey(info, "t"));
    p->task = atoi(Info_ValueForKey(info, "tt"));

    Q_strlcpy(model, Info_ValueForKey(info, "model"), sizeof(model));
    if (!model[0])
        Q_strlcpy(model, "sarge/default", sizeof(model));
    skin = Team_DemoSkin(model, p->team, p->team == 2 ? q3d.blue_team : q3d.red_team);

    slash = strchr(model, '/');
    if (slash)
        *slash = 0;
    if (Q_stricmp(model, p->model)) {
        Q_strlcpy(p->model, model, sizeof(p->model));
        p->sex = q3d_model_sex(model);
    }

    if (q3d.level_started)
        gi.configstring(CS_PLAYERSKINS + q3d_slot(c), va("%.15s\\%s", p->name, skin));
}

static void q3d_serverinfo(void)
{
    const char *info = q3d_cs(Q3D_CS_SERVERINFO);

    Q_strlcpy(q3d.mapname, Info_ValueForKey(info, "mapname"), sizeof(q3d.mapname));
    q3d.gametype = atoi(Info_ValueForKey(info, "g_gametype"));
    q3d.capturelimit = atoi(Info_ValueForKey(info, "capturelimit"));
    q3d.fraglimit = atoi(Info_ValueForKey(info, "fraglimit"));
    Q_strlcpy(q3d.red_team, Info_ValueForKey(info, "g_redteam"), sizeof(q3d.red_team));
    Q_strlcpy(q3d.blue_team, Info_ValueForKey(info, "g_blueteam"), sizeof(q3d.blue_team));
}

// CS_TA_HUD from the scores and the flags (g_ta.c TA_UpdateHud's format)
static void q3d_update_hud(void)
{
    const char  *f = q3d_cs(Q3D_CS_FLAGSTATUS);
    char        s[MAX_QPATH];
    int         gt = q3d.gametype, red = 0, blue = 0, rf = 0, bf = 0, nf = 0;
    int         limit = gt >= Q3D_GT_CTF ? q3d.capturelimit : q3d.fraglimit;

    if (!level.ta_map)
        return;
    if (gt >= Q3D_GT_TEAM) {
        red = atoi(q3d_cs(Q3D_CS_SCORES1));
        blue = atoi(q3d_cs(Q3D_CS_SCORES2));
    }
    if (gt == Q3D_GT_CTF && f[0] && f[1]) {
        rf = f[0] - '0';
        bf = f[1] - '0';
    } else if (gt == Q3D_GT_1FCTF && f[0]) {
        nf = f[0] - '0';
    }
    clamp(rf, 0, 2);
    clamp(bf, 0, 2);
    clamp(nf, 0, 4);

    Q_snprintf(s, sizeof(s), "%d %d %d %d %d %d %d", gt, limit, red, blue, rf, bf, nf);
    if (strcmp(s, q3d.hud)) {
        Q_strlcpy(q3d.hud, s, sizeof(q3d.hud));
        gi.configstring(CS_TA_HUD, s);
    }
}

static void q3d_cs_changed(int index)
{
    if (index == Q3D_CS_SERVERINFO) {
        q3d_serverinfo();
        q3d_update_hud();
    } else if (index == Q3D_CS_SCORES1 || index == Q3D_CS_SCORES2 || index == Q3D_CS_FLAGSTATUS) {
        q3d_update_hud();
    } else if (index >= Q3D_CS_SOUNDS && index < Q3D_CS_SOUNDS + Q3D_MAX_SOUNDS) {
        q3d.sounds[index - Q3D_CS_SOUNDS] = 0;
    } else if (index >= Q3D_CS_PLAYERS && index < Q3D_CS_PLAYERS + Q3D_MAX_CLIENTS) {
        q3d_player_info(index - Q3D_CS_PLAYERS);
    } else if (index > Q3D_CS_LOCATIONS && index <= Q3D_CS_LOCATIONS + MAX_TA_LOCATIONS &&
               index < Q3D_CS_LOCATIONS + Q3D_MAX_LOCATIONS) {
        // Quake III's location n is the HUD's CS_TA_LOCATIONS + n - 1
        gi.configstring(CS_TA_LOCATIONS + index - Q3D_CS_LOCATIONS - 1, (char *)q3d_cs(index));
    }
}

/*
==============================================================================

SERVER COMMANDS

==============================================================================
*/

static void q3d_stufftext(const char *text)
{
    gi.WriteByte(svc_stufftext);
    gi.WriteString((char *)text);
    gi.unicast(&g_edicts[1], true);
}

// an icon as an image index, 0 for none
static int q3d_icon(const gitem_t *it)
{
    return it && it->icon ? gi.imageindex(it->icon) : 0;
}

static const q3d_look_t *q3d_item_look(int index);

static int q3d_item_icon(int index)
{
    const q3d_look_t *look = q3d_item_look(index);

    return look ? q3d_icon(look->item) : 0;
}

static int q3d_gun_icon(int weapon)
{
    return q3d_icon(Q3W_DemoGun(weapon, NULL, NULL, NULL, NULL));
}

// the first timed powerup in a bitmask, as an icon
static int q3d_powerup_icon(int powerups)
{
    static const int order[] = {
        Q3D_PW_QUAD, Q3D_PW_BATTLESUIT, Q3D_PW_HASTE, Q3D_PW_INVIS, Q3D_PW_REGEN,
        Q3D_PW_FLIGHT, Q3D_PW_INVULNERABILITY
    };
    int i;

    for (i = 0; i < q_countof(order); i++)
        if (powerups & (1 << order[i]))
            return q3d_item_icon(q3d_powerup_item(order[i]));
    return 0;
}

static int q3d_persistent_icon(int powerups)
{
    int pw;

    for (pw = Q3D_PW_SCOUT; pw <= Q3D_PW_AMMOREGEN; pw++)
        if (powerups & (1 << pw))
            return q3d_item_icon(q3d_powerup_item(pw));
    return 0;
}

static int q3d_flag_of(int powerups)
{
    if (powerups & (1 << Q3D_PW_REDFLAG))
        return 1;
    if (powerups & (1 << Q3D_PW_BLUEFLAG))
        return 2;
    if (powerups & (1 << Q3D_PW_NEUTRALFLAG))
        return 3;
    return 0;
}

/*
"tinfo <n> [<client> <location> <health> <armor> <weapon> <powerups>]...",
cgame's CG_ParseTeamInfo, as this game's "ta_tinfo" (g_ta.c TA_SendTeamInfo):
the team, then the icons for the gun and the powerups, the flag, the job.
*/
static void q3d_tinfo(const char *args)
{
    char    msg[1024], entry[96];
    int     i, n, team, v[6];

    n = atoi(COM_Parse(&args));
    n = min(n, 16);
    team = q3d.players[q3d.client].team == 2 ? 2 : 1;
    msg[0] = 0;
    for (i = 0; i < n; i++) {
        int j, c;

        for (j = 0; j < 6; j++)
            v[j] = atoi(COM_Parse(&args));
        c = v[0];
        if (c < 0 || c >= Q3D_MAX_CLIENTS)
            continue;
        Q_snprintf(entry, sizeof(entry), " %d %d %d %d %d %d %d %d %d", q3d_slot(c), v[1],
                   max(v[2], 0), max(v[3], 0), q3d_gun_icon(v[4]), q3d_powerup_icon(v[5]),
                   q3d_flag_of(v[5]), q3d_persistent_icon(v[5]), q3d.players[c].task);
        if (strlen(msg) + strlen(entry) >= sizeof(msg) - 32)
            break;
        Q_strlcat(msg, entry, sizeof(msg));
    }
    q3d_stufftext(va("ta_tinfo %d %d%s\n", team, n, msg));
}

// "vchat <voice only> <client> <colour> <id>", and vtchat and vtell: the
// HUD's "ta_vchat <mode> <voice only> <client> <id>" (g_ta.c TA_VoiceTo)
static void q3d_vchat(int mode, const char *args)
{
    int     voice_only = atoi(COM_Parse(&args));
    int     c = atoi(COM_Parse(&args));
    char    id[64];

    COM_Parse(&args);   // the colour
    Q_strlcpy(id, COM_Parse(&args), sizeof(id));
    if (c < 0 || c >= Q3D_MAX_CLIENTS || !id[0])
        return;
    q3d_stufftext(va("ta_vchat %d %d %d %s\n", mode, voice_only ? 1 : 0, q3d_slot(c), id));
}

static void q3d_server_command(const char *text)
{
    char        cmd[32], out[1024];
    const char  *args = text, *s;

    Q_strlcpy(cmd, COM_Parse(&args), sizeof(cmd));
    if (!cmd[0])
        return;

    // the configstrings change while loading too: the gamestate's are
    // followed by those that changed before the first snapshot
    if (!strcmp(cmd, "cs")) {
        int index = atoi(COM_Parse(&args));
        q3d_set_cs(index, COM_Parse(&args));
        return;
    }
    if (!strcmp(cmd, "bcs0") || !strcmp(cmd, "bcs1") || !strcmp(cmd, "bcs2")) {
        int index = atoi(COM_Parse(&args));

        if (cmd[3] == '0')
            q3d.bcs[0] = 0;
        Q_strlcat(q3d.bcs, COM_Parse(&args), sizeof(q3d.bcs));
        if (cmd[3] == '2')
            q3d_set_cs(index, q3d.bcs);
        return;
    }
    if (q3d.loading)
        return;

    if (!strcmp(cmd, "print")) {
        q3d_strip(out, sizeof(out), COM_Parse(&args));
        gi.cprintf(&g_edicts[1], PRINT_HIGH, "%s", out);
    } else if (!strcmp(cmd, "cp")) {
        q3d_strip(out, sizeof(out), COM_Parse(&args));
        gi.centerprintf(&g_edicts[1], "%s", out);
    } else if (!strcmp(cmd, "chat") || !strcmp(cmd, "tchat")) {
        s = COM_Parse(&args);
        q3d_strip(out, sizeof(out), s);
        gi.cprintf(&g_edicts[1], PRINT_CHAT, "%s\n", out);
    } else if (!strcmp(cmd, "tinfo")) {
        if (level.ta_map)
            q3d_tinfo(args);
    } else if (!strcmp(cmd, "vchat")) {
        if (level.ta_map)
            q3d_vchat(0, args);
    } else if (!strcmp(cmd, "vtchat")) {
        if (level.ta_map)
            q3d_vchat(1, args);
    } else if (!strcmp(cmd, "vtell")) {
        if (level.ta_map)
            q3d_vchat(2, args);
    }
}

/*
==============================================================================

LOADING

==============================================================================
*/

// the snapshots' own bookkeeping, and their events into the queue
static void q3d_queue(int event, int parm, const q3d_es_t *es, bool ps, int time)
{
    q3d_event_t *ev;

    if (q3d.loading || q3d.num_events >= Q3D_MAX_EVENTS || !event)
        return;
    ev = &q3d.events[q3d.num_events++];
    ev->event = event;
    ev->parm = parm;
    ev->es = *es;
    ev->ps = ps;
    ev->time = time;
}

// a stand-in entity for the recorded player's own events
static void q3d_ps_entity(const q3d_ps_t *ps, q3d_es_t *es, int parm)
{
    memset(es, 0, sizeof(*es));
    es->number = es->clientNum = q3d.client;
    es->eType = Q3D_ET_PLAYER;
    es->weapon = ps->weapon;
    es->eventParm = parm;
    es->eFlags = ps->eFlags;
    VectorCopy(ps->origin, es->pos.trBase);
    VectorCopy(ps->viewangles, es->apos.trBase);
    for (int i = 0; i < Q3D_PW_NUM; i++)
        if (ps->powerups[i])
            es->powerups |= 1 << i;
}

// CG_CheckPlayerstateEvents
static void q3d_ps_events(const q3d_ps_t *ops, const q3d_ps_t *ps, int time)
{
    q3d_es_t    es;
    int         i;

    if (ps->externalEvent && ps->externalEvent != ops->externalEvent) {
        q3d_ps_entity(ps, &es, ps->externalEventParm);
        q3d_queue(ps->externalEvent & ~Q3D_EV_EVENT_BITS, ps->externalEventParm, &es, true, time);
    }
    for (i = ps->eventSequence - 2; i < ps->eventSequence; i++) {
        if (i < 0)
            continue;
        if (i >= ops->eventSequence ||
            (i > ops->eventSequence - 2 && ps->events[i & 1] != ops->events[i & 1])) {
            q3d_ps_entity(ps, &es, ps->eventParms[i & 1]);
            q3d_queue(ps->events[i & 1] & ~Q3D_EV_EVENT_BITS, ps->eventParms[i & 1], &es, true, time);
        }
    }
}

// CG_CheckEvents: an event entity's event once, anyone else's when it changes
static void q3d_entity_events(const q3d_es_t *es, int time)
{
    q3d_ent_t   *qe = &q3d.ents[es->number];
    bool        recent = qe->last_seen && time - qe->last_seen <= Q3D_EVENT_VALID_MSEC;

    if (es->eType >= Q3D_ET_EVENTS) {
        if (!recent || qe->prev_etype != es->eType) {
            // a player's second event of a frame, sent on its own
            // (SendPendingPredictableEvents): it is the player's
            if (es->eFlags & Q3D_EF_PLAYER_EVENT) {
                q3d_es_t pe = *es;

                pe.number = es->otherEntityNum;
                q3d_queue(es->eType - Q3D_ET_EVENTS, es->eventParm, &pe, false, time);
            } else {
                q3d_queue(es->eType - Q3D_ET_EVENTS, es->eventParm, es, false, time);
            }
        }
        qe->prev_event = 0;
    } else {
        int prev = recent ? qe->prev_event : 0;

        if (es->event != prev && (es->event & ~Q3D_EV_EVENT_BITS))
            q3d_queue(es->event & ~Q3D_EV_EVENT_BITS, es->eventParm, es, false, time);
        qe->prev_event = es->event;
    }
    qe->prev_etype = es->eType;
    qe->last_seen = time;
    if (es->number > q3d.max_ent)
        q3d.max_ent = es->number;
}

static void q3d_transition(q3d_snap_t *snap)
{
    int i;

    if (q3d.cur)
        q3d_ps_events(&q3d.cur->ps, &snap->ps, snap->server_time);
    for (i = 0; i < snap->num_entities; i++)
        q3d_entity_events(q3d_snap_entity(snap, i), snap->server_time);

    q3d.prev = q3d.cur ? q3d.cur : snap;
    q3d.cur = snap;
}

static bool q3d_load(const char *mapname)
{
    void    *buf;
    int     len;

    len = gi.LoadFile(q3d.path, &buf);
    if (len <= 0 || !buf) {
        gi.dprintf("Quake III demo %s: not found\n", q3d.path);
        return false;
    }
    q3d.data = gi.TagMalloc(len, TAG_LEVEL);
    memcpy(q3d.data, buf, len);
    gi.FreeFile(buf);
    q3d.size = len;

    q3d.baselines = gi.TagMalloc(sizeof(q3d_es_t) * Q3D_MAX_GENTITIES, TAG_LEVEL);
    memset(q3d.baselines, 0, sizeof(q3d_es_t) * Q3D_MAX_GENTITIES);
    q3d.parse_ents = gi.TagMalloc(sizeof(q3d_es_t) * Q3D_MAX_PARSE_ENTITIES, TAG_LEVEL);
    memset(q3d.parse_ents, 0, sizeof(q3d_es_t) * Q3D_MAX_PARSE_ENTITIES);
    q3d.ents = gi.TagMalloc(sizeof(q3d_ent_t) * Q3D_MAX_GENTITIES, TAG_LEVEL);
    memset(q3d.ents, 0, sizeof(q3d_ent_t) * Q3D_MAX_GENTITIES);

    // the gamestate, up to the first snapshot
    q3d.loading = true;
    while (!q3d.cur) {
        if (!q3d_next_message()) {
            gi.dprintf("Quake III demo %s: ends before it begins\n", q3d.path);
            return false;
        }
    }
    q3d.loading = false;

    q3d_serverinfo();
    if (Q_stricmp(q3d.mapname, COM_SkipPath(mapname))) {
        gi.dprintf("Quake III demo %s: recorded on %s, not %s\n", q3d.path, q3d.mapname, mapname);
        return false;
    }
    q3d.client = q3d.cur->ps.clientNum;
    if (q3d.client < 0 || q3d.client >= Q3D_MAX_CLIENTS)
        q3d.client = 0;
    q3d.time = q3d.cur->server_time;
    return true;
}

/*
A Quake III map started for a demo (g_q3_demo), before its entities spawn:
the demo's gamestate read, and Quake III's guns for the world to register
(SP_worldspawn's vweps and CS_Q3RULES), whatever the bot match says. The
parse loop stops after the world (Q3Demo_LevelStart). Every map comes here:
anything else just clears the last demo.
*/
void Q3Demo_SpawnEntities(const char *mapname)
{
    memset(&q3d, 0, sizeof(q3d));

    if (!g_q3_demo)
        g_q3_demo = gi.cvar("g_q3_demo", "", 0);
    if (!level.q3map || !g_q3_demo->string[0])
        return;

    // one level's worth: the next map is a game again
    Q_strlcpy(q3d.path, g_q3_demo->string, sizeof(q3d.path));
    gi.cvar_forceset("g_q3_demo", "");

    q3d.active = true;
    level.q3_weapons = 1;
    level.q1_weapons = false;
    level.q1_by_model = false;
    if (!q3d_load(mapname))
        q3d.broken = true;  // over at the first frame: the attract loop goes on
}

/*
==============================================================================

THE LOOKS

==============================================================================
*/

static const q3d_look_t *q3d_item_look(int index)
{
    static edict_t  scratch;
    q3d_look_t      *look;

    if (index <= 0 || index >= Q3D_NUM_ITEMS)
        return NULL;
    look = &q3d.looks[index];
    if (!look->resolved) {
        look->resolved = true;
        memset(&scratch, 0, sizeof(scratch));
        look->item = Q3_DemoItem(&scratch, q3d_items[index].classname);
        if (look->item) {
            look->modelindex = scratch.s.modelindex;
            look->modelindex2 = scratch.s.modelindex2;
            look->effects = scratch.s.effects | EF_ROTATE;
            look->renderfx = scratch.s.renderfx;
            look->scale = scratch.s.scale;
        }
    }
    return look->item ? look : NULL;
}

static const char *const q3d_precache_sounds[] = {
    "weapons/change.wav", "weapons/noammo.wav", "world/jumppad.wav",
    "world/telein.wav", "world/teleout.wav", "items/respawn1.wav", "items/poweruprespawn.wav",
    "items/damage3.wav", "items/protect3.wav", "items/regen.wav", "items/n_health.wav",
    "items/s_health.wav", "items/l_health.wav", "items/m_health.wav", Q3D_HOLDABLE,
    "misc/ar1_pkup.wav", Q3D_ARMOR, Q3D_GUN, Q3D_AMMO,
    "items/cl_scout.wav", "items/cl_guard.wav", "items/cl_doubler.wav", "items/cl_ammoregen.wav",
    "items/quaddamage.wav", "items/protect.wav", "items/haste.wav", "items/invisibility.wav",
    "items/regeneration.wav", "items/flight.wav", "items/invul_impact_01.wav",
    "weapons/grenade/hgrenb1a.wav", "weapons/grenade/hgrenb2a.wav",
    "weapons/rocket/rockfly.wav", "weapons/plasma/lasfly.wav",
    "weapons/proxmine/wstbimpd.wav", "weapons/proxmine/wstbactv.wav", "weapons/proxmine/wstbtick.wav",
    "teamplay/flagcapture_yourteam.wav", "teamplay/flagcapture_opponent.wav",
    "teamplay/flagreturn_yourteam.wav", "teamplay/flagreturn_opponent.wav",
    "teamplay/flagtaken_yourteam.wav", "teamplay/flagtaken_opponent.wav",
    "teamplay/voc_red_returned.wav", "teamplay/voc_blue_returned.wav",
    "teamplay/voc_enemy_flag.wav", "teamplay/voc_team_flag.wav", "teamplay/voc_you_flag.wav",
    "teamplay/voc_red_scores.wav", "teamplay/voc_blue_scores.wav",
    "feedback/redleads.wav", "feedback/blueleads.wav", "feedback/teamstied.wav",
    "player/watr_in.wav", "player/watr_out.wav", "player/watr_un.wav",
    // each player's own (Q3_SoundIndex passes these to the client as they are)
    "*jump1.wav", "*pain25_1.wav", "*pain50_1.wav", "*pain75_1.wav", "*pain100_1.wav",
    "*death1.wav", "*death2.wav", "*death3.wav", "*taunt.wav",
};

static const char *const q3d_flagbase_models[3] = {
    "models/mapobjects/flagbase/ntrl_base.md3",
    "models/mapobjects/flagbase/red_base.md3",
    "models/mapobjects/flagbase/blue_base.md3",
};

/*
The world has spawned (SP_worldspawn: the sky, the music, the status bar,
Quake III's guns): none of the map's other entities do. The demo's players,
scores and places go out, and what it will use is registered now rather
than mid-demo.
*/
void Q3Demo_LevelStart(void)
{
    const char  *items;
    int         i;

    q3d.edict_base = max(globals.num_edicts, game.maxclients + 1);
    q3d.level_started = true;
    if (q3d.broken)
        return;

    for (i = 0; i < Q3D_MAX_CLIENTS; i++)
        q3d_player_info(i);
    q3d_update_hud();
    for (i = 1; i < Q3D_MAX_LOCATIONS && i <= MAX_TA_LOCATIONS; i++)
        if (q3d_cs(Q3D_CS_LOCATIONS + i)[0])
            gi.configstring(CS_TA_LOCATIONS + i - 1, (char *)q3d_cs(Q3D_CS_LOCATIONS + i));

    for (i = 1; i < Q3D_MAX_SOUNDS; i++)
        q3d_sound_index(i);
    for (i = 0; i < q_countof(q3d_precache_sounds); i++)
        Q3_SoundIndex(q3d_precache_sounds[i]);
    for (i = 1; i < Q3D_MAX_MODELS; i++)
        q3d_model_index(i);

    // the items on the map (CS_ITEMS), and the flags anyone may carry
    items = q3d_cs(Q3D_CS_ITEMS);
    for (i = 1; i < Q3D_NUM_ITEMS && items[i]; i++)
        if (items[i] == '1')
            q3d_item_look(i);
    for (i = 0; i < 3; i++)
        gi.modelindex((char *)q3d_flagbase_models[i]);
    gi.modelindex("players/male/flag1.md2");
    gi.modelindex("players/male/flag2.md2");
    gi.modelindex("models/weaphits/nail.md3");
    gi.modelindex("models/weaphits/proxmine.md3");
    gi.imageindex("i_ctf1");
    gi.imageindex("i_ctf2");
    gi.imageindex("i_bodyarmor");
    for (i = 1; i < Q3W_Guns(); i++)
        q3d_gun_icon(i);

    gi.dprintf("Quake III demo: %s on %s, recorded as client %d (%s)\n", q3d.path, q3d.mapname,
               q3d.client, q3d.players[q3d.client].active ? q3d.players[q3d.client].name : "?");
}

/*
==============================================================================

THE PLAYER

==============================================================================
*/

void Q3Demo_ClientBegin(edict_t *ent)
{
    int         num = (int)(ent - g_edicts);
    gclient_t   *client = ent->client;
    short       fov = client->ps.fov;

    ent->inuse = true;
    ent->s.number = num;
    ent->classname = "player";
    ent->svflags = 0;
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    ent->s.modelindex = 0;
    memset(&client->ps, 0, sizeof(client->ps));
    client->ps.fov = fov ? fov : 90;
    client->ps.pmove.pm_type = PM_FREEZE;
    client->ps.pmove.pm_flags = PMF_NO_PREDICTION;
    memset(&client->old_pmove, 0, sizeof(client->old_pmove));

    // the recorded player is edict 1, and wears their look; anyone else
    // just watches
    if (num == 1) {
        q3d.started = true;
        if (q3d.level_started && !q3d.broken && q3d.players[q3d.client].active) {
            const q3d_player_t *p = &q3d.players[q3d.client];
            const char *skin = Info_ValueForKey(q3d_cs(Q3D_CS_PLAYERS + q3d.client), "model");

            skin = Team_DemoSkin(skin[0] ? skin : "sarge/default", p->team,
                                 p->team == 2 ? q3d.blue_team : q3d.red_team);
            gi.configstring(CS_PLAYERSKINS, va("%.15s\\%s", p->name, skin));
        }
    }
    gi.linkentity(ent);
}

void Q3Demo_ClientDisconnect(edict_t *ent)
{
    int num = (int)(ent - g_edicts);

    if (num == 1)
        q3d.started = false;

    gi.unlinkentity(ent);
    ent->s.modelindex = 0;
    ent->s.sound = 0;
    ent->s.event = 0;
    ent->s.effects = 0;
    ent->solid = SOLID_NOT;
    ent->inuse = false;
    ent->classname = "disconnected";
    ent->client->pers.connected = false;
}

/*
==============================================================================

THE FRAME

==============================================================================
*/

// BG_EvaluateTrajectory
static void q3d_eval(const q3d_traj_t *tr, int at, vec3_t out)
{
    float t;

    switch (tr->trType) {
    case Q3D_TR_LINEAR:
        t = (at - tr->trTime) * 0.001f;
        VectorMA(tr->trBase, t, tr->trDelta, out);
        break;
    case Q3D_TR_SINE:
        t = tr->trDuration ? (float)(at - tr->trTime) / tr->trDuration : 0;
        VectorMA(tr->trBase, sinf(t * M_PI * 2), tr->trDelta, out);
        break;
    case Q3D_TR_LINEAR_STOP:
        if (at > tr->trTime + tr->trDuration)
            at = tr->trTime + tr->trDuration;
        t = (at - tr->trTime) * 0.001f;
        if (t < 0)
            t = 0;
        VectorMA(tr->trBase, t, tr->trDelta, out);
        break;
    case Q3D_TR_GRAVITY:
        t = (at - tr->trTime) * 0.001f;
        VectorMA(tr->trBase, t, tr->trDelta, out);
        out[2] -= 0.5f * Q3D_DEFAULT_GRAVITY * t * t;
        break;
    default:    // TR_STATIONARY, TR_INTERPOLATE
        VectorCopy(tr->trBase, out);
        break;
    }
}

// BG_EvaluateTrajectoryDelta: how it moves
static void q3d_eval_delta(const q3d_traj_t *tr, int at, vec3_t out)
{
    switch (tr->trType) {
    case Q3D_TR_LINEAR:
        VectorCopy(tr->trDelta, out);
        break;
    case Q3D_TR_LINEAR_STOP:
        if (at > tr->trTime + tr->trDuration)
            VectorClear(out);
        else
            VectorCopy(tr->trDelta, out);
        break;
    case Q3D_TR_GRAVITY:
        VectorCopy(tr->trDelta, out);
        out[2] -= Q3D_DEFAULT_GRAVITY * (at - tr->trTime) * 0.001f;
        break;
    default:
        VectorClear(out);
        break;
    }
}

static float q3d_lerp_angle(float from, float to, float frac)
{
    float d = to - from;

    d -= 360.0f * floorf((d + 180.0f) / 360.0f);
    return from + frac * d;
}

// the entity numbered n in a snapshot, by bisection (they are in order)
static const q3d_es_t *q3d_find(const q3d_snap_t *snap, int n)
{
    int lo = 0, hi = snap ? snap->num_entities - 1 : -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const q3d_es_t *es = q3d_snap_entity(snap, mid);

        if (es->number == n)
            return es;
        if (es->number < n)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

// where it is at the clock: a player between the snapshots, as cgame drew
// them with cg_smoothClients 0, anything else on its trajectory
static void q3d_entity_place(const q3d_es_t *es, const q3d_es_t *old, float frac,
                             vec3_t origin, vec3_t angles)
{
    if (es->number < Q3D_MAX_CLIENTS || es->pos.trType == Q3D_TR_INTERPOLATE) {
        if (old && !((old->eFlags ^ es->eFlags) & Q3D_EF_TELEPORT_BIT)) {
            int i;

            for (i = 0; i < 3; i++) {
                origin[i] = old->pos.trBase[i] + frac * (es->pos.trBase[i] - old->pos.trBase[i]);
                angles[i] = q3d_lerp_angle(old->apos.trBase[i], es->apos.trBase[i], frac);
            }
        } else {
            VectorCopy(es->pos.trBase, origin);
            VectorCopy(es->apos.trBase, angles);
        }
        return;
    }
    q3d_eval(&es->pos, q3d.time, origin);
    q3d_eval(&es->apos, q3d.time, angles);
}

/*
A Quake III player's frame as the Quake II player frames the client turns
into Quake III's animations (q3player.c q2_frame_class): the deaths, the
crouch, the jump, a gesture, a moment's pain. The rest - the run, the legs,
the attack (svc_muzzleflash) - the client works out itself.
*/
static int q3d_player_frame(q3d_ent_t *qe, const q3d_es_t *es)
{
    static const int    first[3] = { 178, 184, 190 }, last[3] = { 183, 189, 197 };
    int                 legs = es->legsAnim & ~Q3D_ANIM_TOGGLEBIT;
    int                 torso = es->torsoAnim & ~Q3D_ANIM_TOGGLEBIT;
    bool                crouched = legs == Q3D_LEGS_WALKCR || legs == Q3D_LEGS_IDLECR;

    if ((es->eFlags & Q3D_EF_DEAD) || legs <= Q3D_BOTH_DEAD3) {
        int d = legs <= Q3D_BOTH_DEAD3 ? legs / 2 : 0;
        bool lying = legs <= Q3D_BOTH_DEAD3 && (legs & 1);

        if (qe->death != d + 1) {
            qe->death = d + 1;
            qe->death_frame = lying ? last[d] : first[d];
        } else if (qe->death_frame < last[d]) {
            qe->death_frame++;
        }
        return qe->death_frame;
    }
    qe->death = 0;

    if (q3d.time < qe->pain_until)
        return crouched ? 169 : 54;
    if (legs == Q3D_LEGS_WALKCR)
        return 154;
    if (legs == Q3D_LEGS_IDLECR)
        return 135;
    if (legs == Q3D_LEGS_JUMP || legs == Q3D_LEGS_JUMPB)
        return 66;
    if (torso == Q3D_TORSO_GESTURE || (torso >= Q3D_TORSO_GETFLAG && torso <= Q3D_TORSO_NEGATIVE))
        return 72;
    return 0;
}

// the powerups' looks on a player (g_q3.c Q3_ClientEffects, g_ctf.c CTF_ClientEffects)
static void q3d_powerup_effects(edict_t *e, int powerups, bool alive, int team)
{
    if (!alive)
        return;
    if (powerups & (1 << Q3D_PW_QUAD))
        e->s.effects |= EF_QUAD;
    if (powerups & (1 << Q3D_PW_BATTLESUIT)) {
        e->s.effects |= EF_COLOR_SHELL;
        e->s.renderfx |= RF_SHELL_RED | RF_SHELL_GREEN;
    }
    if ((powerups & (1 << Q3D_PW_REGEN)) && (q3d.frame & 4)) {
        e->s.effects |= EF_COLOR_SHELL;
        e->s.renderfx |= RF_SHELL_RED;
    }
    if (powerups & (1 << Q3D_PW_REDFLAG)) {
        e->s.effects |= EF_FLAG1;
        e->s.modelindex3 = gi.modelindex("players/male/flag1.md2");
    } else if (powerups & (1 << Q3D_PW_BLUEFLAG)) {
        e->s.effects |= EF_FLAG2;
        e->s.modelindex3 = gi.modelindex("players/male/flag2.md2");
    } else if (powerups & (1 << Q3D_PW_NEUTRALFLAG)) {
        // no white flag on a back here: the carrier's own colour (CTF_ClientEffects)
        e->s.effects |= team == 2 ? EF_FLAG2 : EF_FLAG1;
        e->s.modelindex3 = gi.modelindex(team == 2 ? "players/male/flag2.md2" : "players/male/flag1.md2");
    }
}

static void q3d_player(edict_t *e, q3d_ent_t *qe, const q3d_es_t *es, const vec3_t view)
{
    const char  *ready = NULL, *firing = NULL;
    gitem_t     *gun = Q3W_DemoGun(es->weapon, NULL, NULL, &ready, &firing);
    int         c = es->clientNum;
    bool        alive = !(es->eFlags & Q3D_EF_DEAD);
    float       pitch = view[PITCH];

    if (c < 0 || c >= Q3D_MAX_CLIENTS)
        c = es->number;
    e->s.modelindex = 255;
    e->s.modelindex2 = gun ? 255 : 0;
    e->s.skinnum = q3d_slot(c) | ((gun ? gun->weapmodel & 0xff : 0) << 8);
    e->s.frame = q3d_player_frame(qe, es);

    // the view's pitch over 3, as Quake II sends a player's
    pitch -= 360.0f * floorf((pitch + 180.0f) / 360.0f);
    e->s.angles[PITCH] = pitch / 3;
    e->s.angles[YAW] = view[YAW];
    e->s.angles[ROLL] = 0;

    q3d_powerup_effects(e, es->powerups, alive, c < Q3D_MAX_CLIENTS ? q3d.players[c].team : 0);

    // the gun's hum, or its whir with the trigger down (CG_AddPlayerWeapon)
    if (alive) {
        if ((es->eFlags & Q3D_EF_FIRING) && firing)
            e->s.sound = Q3_SoundIndex(firing);
        else if (ready)
            e->s.sound = Q3_SoundIndex(ready);
    }
}

// a missile's shooter, for a plasma ball drawn from the gun: the nearest
// player with a plasma gun where it was fired
static int q3d_missile_shooter(const q3d_es_t *es)
{
    vec3_t  from;
    float   best = 200 * 200;
    int     i, num = 0;

    q3d_eval(&es->pos, es->pos.trTime, from);
    if (q3d.cur->ps.weapon == es->weapon && DistanceSquared(from, q3d.cur->ps.origin) < best) {
        best = DistanceSquared(from, q3d.cur->ps.origin);
        num = 1;
    }
    for (i = 0; i < q3d.cur->num_entities; i++) {
        const q3d_es_t *p = q3d_snap_entity(q3d.cur, i);
        float d;

        if (p->eType != Q3D_ET_PLAYER || p->weapon != es->weapon)
            continue;
        d = DistanceSquared(from, p->pos.trBase);
        if (d < best) {
            best = d;
            num = q3d_edict(p->number);
        }
    }
    return max(num, 0);
}

static void q3d_missile(edict_t *e, q3d_ent_t *qe, const q3d_es_t *es, bool fresh)
{
    vec3_t dir;

    q3d_eval_delta(&es->pos, q3d.time, dir);
    if (VectorNormalize(dir) > 0)
        vectoangles(dir, e->s.angles);

    switch (es->weapon) {
    case Q3D_WP_GRENADE:
        e->s.modelindex = gi.modelindex("models/ammo/grenade1.md3");
        e->s.effects = EF_GRENADE;
        if (es->pos.trType != Q3D_TR_STATIONARY)
            e->s.angles[ROLL] = anglemod(q3d.time * 0.3f);
        break;
    case Q3D_WP_ROCKET:
        e->s.modelindex = gi.modelindex("models/ammo/rocket/rocket.md3");
        e->s.effects = EF_ROCKET;
        e->s.sound = Q3_SoundIndex("weapons/rocket/rockfly.wav");
        break;
    case Q3D_WP_PLASMA:
        // no model: the client draws Quake III's sprite, a new one from its
        // shooter's gun (g_q3weap.c Q3W_Missile)
        if (fresh)
            qe->shooter = q3d_missile_shooter(es);
        e->s.renderfx = RF_Q3_SPRITE;
        e->s.frame = Q3_SPRITE_PLASMA;
        e->s.skinnum = qe->shooter;
        e->s.sound = Q3_SoundIndex("weapons/plasma/lasfly.wav");
        break;
    case Q3D_WP_BFG:
        e->s.modelindex = gi.modelindex("sprites/s_bfg1.sp2");
        e->s.effects = EF_BFG | EF_ANIM_ALLFAST;
        e->s.scale = 0.5f;
        e->s.sound = Q3_SoundIndex("weapons/rocket/rockfly.wav");
        break;
    case Q3D_WP_NAILGUN:
        e->s.modelindex = gi.modelindex("models/weaphits/nail.md3");
        break;
    case Q3D_WP_PROX:
        e->s.modelindex = gi.modelindex("models/weaphits/proxmine.md3");
        if (es->pos.trType != Q3D_TR_STATIONARY)
            e->s.effects = EF_GRENADE;
        else
            VectorCopy(es->angles, e->s.angles);    // stuck where it hit
        break;
    default:
        break;
    }
}

/*
A shot against the players in the latest snapshot - their edicts aren't
solid, so a trace goes through them - as Quake III's boxes (16 out, 24 down,
32 up), alive ones only, not the shooter's own. The nearest along the shot,
nearer than *len: *len cut to it, and true.
*/
static bool q3d_shot_player(const vec3_t start, const vec3_t dir, float *len, int self)
{
    static const vec3_t mins = { -16, -16, -24 }, maxs = { 16, 16, 32 };
    bool                hit = false;
    int                 i, j;

    for (i = 0; i < q3d.cur->num_entities; i++) {
        const q3d_es_t  *p = q3d_snap_entity(q3d.cur, i);
        float           enter = 0, leave = *len;

        if (p->eType != Q3D_ET_PLAYER || p->number == self || (p->eFlags & Q3D_EF_DEAD))
            continue;
        // the slab test: where the line is inside all three of the box's pairs of planes
        for (j = 0; j < 3; j++) {
            float lo = p->pos.trBase[j] + mins[j] - start[j];
            float hi = p->pos.trBase[j] + maxs[j] - start[j];

            if (fabsf(dir[j]) < 1e-6f) {
                if (lo > 0 || hi < 0)
                    break;
                continue;
            }
            lo /= dir[j];
            hi /= dir[j];
            if (lo > hi) {
                float t = lo;
                lo = hi;
                hi = t;
            }
            enter = max(enter, lo);
            leave = min(leave, hi);
            if (enter > leave)
                break;
        }
        if (j < 3 || enter >= *len)
            continue;
        *len = enter;
        hit = true;
    }
    return hit;
}

// the shot of a player's Lightning Gun this frame, cut where it meets a
// wall or another player (Q3W_Lightning's message)
static void q3d_lightning(int num, int self, const vec3_t origin, const vec3_t view, int viewheight)
{
    vec3_t  forward, muzzle, end;
    trace_t tr;
    float   len;
    bool    hit;

    AngleVectors(view, forward, NULL, NULL);
    VectorCopy(origin, muzzle);
    muzzle[2] += viewheight;
    VectorMA(muzzle, 14, forward, muzzle);
    VectorMA(muzzle, 768, forward, end);
    tr = gi.trace(muzzle, NULL, NULL, end, &g_edicts[num], MASK_SOLID);
    hit = tr.fraction < 1.0f && !(tr.surface && (tr.surface->flags & SURF_SKY));
    len = 768 * tr.fraction;
    if (q3d_shot_player(muzzle, forward, &len, self))
        hit = true;
    VectorMA(muzzle, len, forward, end);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_LIGHTNING);
    gi.WriteShort(num);
    gi.WriteShort(hit ? 1 : 0);
    gi.WritePosition(muzzle);
    gi.WritePosition(end);
    gi.multicast(muzzle, MULTICAST_PVS);
}

// one Quake III entity onto its edict; false when it is not drawn
static void q3d_present_entity(const q3d_es_t *es, const q3d_es_t *old, float frac)
{
    q3d_ent_t   *qe = &q3d.ents[es->number];
    int         num = q3d_edict(es->number), look;
    edict_t     *e;
    vec3_t      origin, angles;
    bool        fresh, draw = true;

    if (num <= 1)
        return;     // none, or the view's own
    e = &g_edicts[num];
    if (globals.num_edicts <= num)
        globals.num_edicts = num + 1;
    fresh = !qe->shown;
    qe->present_frame = q3d.frame;

    q3d_entity_place(es, old, frac, origin, angles);

    // a new kind of thing on the edict: its box set up again
    look = es->eType | (es->modelindex << 8);
    if (!e->inuse || qe->look != look) {
        memset(e, 0, sizeof(*e));
        e->inuse = true;
        e->classname = "q3demo_entity";
        e->s.number = num;
        qe->look = look;
        fresh = true;
        if (es->eType == Q3D_ET_MOVER && es->modelindex > 0) {
            gi.setmodel(e, va("*%d", es->modelindex));
            e->solid = SOLID_BSP;
            e->movetype = MOVETYPE_PUSH;
        }
    }

    e->s.modelindex = e->s.modelindex2 = e->s.modelindex3 = e->s.modelindex4 = 0;
    e->s.effects = 0;
    e->s.renderfx = 0;
    e->s.sound = 0;
    e->s.frame = 0;
    e->s.skinnum = 0;
    e->s.scale = 0;
    e->s.event = 0;
    VectorCopy(angles, e->s.angles);

    switch (es->eType) {
    case Q3D_ET_PLAYER:
        if (es->eFlags & Q3D_EF_NODRAW)
            draw = false;
        else
            q3d_player(e, qe, es, angles);
        // its beam, every frame the trigger is down
        if (es->weapon == Q3D_WP_LIGHTNING && (es->eFlags & Q3D_EF_FIRING) && !(es->eFlags & Q3D_EF_DEAD))
            q3d_lightning(num, es->number, origin, angles, 26);
        break;
    case Q3D_ET_ITEM: {
        const q3d_look_t *l = q3d_item_look(es->modelindex);

        if (!l || (es->eFlags & Q3D_EF_NODRAW)) {
            draw = false;
            break;
        }
        e->s.modelindex = l->modelindex;
        e->s.modelindex2 = l->modelindex2;
        e->s.effects = l->effects;
        e->s.renderfx = l->renderfx;
        e->s.scale = l->scale;
        break;
    }
    case Q3D_ET_MISSILE:
        q3d_missile(e, qe, es, fresh);
        break;
    case Q3D_ET_MOVER:
        if (es->modelindex > 0)
            e->s.modelindex = gi.modelindex(va("*%d", es->modelindex));
        e->s.modelindex2 = q3d_model_index(es->modelindex2);
        break;
    case Q3D_ET_GENERAL:
        e->s.modelindex = q3d_model_index(es->modelindex);
        break;
    case Q3D_ET_TEAM:
        // CTF's flag stands (cg_ents.c CG_TeamBase; g_ta.c TA_FlagBase)
        if (q3d.gametype == Q3D_GT_CTF || q3d.gametype == Q3D_GT_1FCTF) {
            if (es->modelindex >= 0 && es->modelindex <= 2)
                e->s.modelindex = gi.modelindex((char *)q3d_flagbase_models[es->modelindex]);
        } else if (q3d.gametype == Q3D_GT_HARVESTER) {
            e->s.modelindex = gi.modelindex("models/powerups/harvester/harvester.md3");
            e->s.skinnum = es->modelindex == 2 ? 1 : 0;
        }
        break;
    default:    // speakers, triggers, the invisible, event entities
        break;
    }
    if (!draw) {
        e->s.modelindex = e->s.modelindex2 = e->s.modelindex3 = 0;
        e->s.effects = 0;
        e->s.renderfx = 0;
    }

    // a looping sound of its own: a speaker's, a stuck mine's tick
    if (es->loopSound && !e->s.sound && es->eType < Q3D_ET_EVENTS)
        e->s.sound = q3d_sound_index(es->loopSound);

    VectorCopy(origin, e->s.origin);
    // a new one starts where it is; a teleport is a jump, not a slide
    if (fresh)
        VectorCopy(origin, e->s.old_origin);
    else
        VectorCopy(qe->sent_origin, e->s.old_origin);
    if (!fresh && ((qe->prev_eflags ^ es->eFlags) & Q3D_EF_TELEPORT_BIT))
        e->s.event = EV_OTHER_TELEPORT;

    if (e->s.modelindex || e->s.sound || e->s.effects || (e->s.renderfx & RF_Q3_SPRITE))
        e->svflags &= ~SVF_NOCLIENT;
    else
        e->svflags |= SVF_NOCLIENT;
    gi.linkentity(e);

    VectorCopy(origin, qe->sent_origin);
    qe->prev_eflags = es->eFlags;
    qe->shown = !(e->svflags & SVF_NOCLIENT);
    qe->lg_was_firing = qe->lg_firing;
    qe->lg_firing = es->weapon == Q3D_WP_LIGHTNING && (es->eFlags & Q3D_EF_FIRING);
}

static void q3d_present_entities(float frac)
{
    int i, n;

    for (i = 0; i < q3d.cur->num_entities; i++) {
        const q3d_es_t *es = q3d_snap_entity(q3d.cur, i);

        q3d_present_entity(es, q3d.prev != q3d.cur ? q3d_find(q3d.prev, es->number) : NULL, frac);
    }

    // what the snapshot no longer has
    for (n = 0; n <= q3d.max_ent && n < Q3D_MAX_GENTITIES; n++) {
        q3d_ent_t *qe = &q3d.ents[n];
        int num;

        if (qe->present_frame == q3d.frame || (!qe->shown && !qe->present_frame))
            continue;
        num = q3d_edict(n);
        if (num > 1 && g_edicts[num].inuse) {
            g_edicts[num].svflags |= SVF_NOCLIENT;
            g_edicts[num].s.event = 0;
            g_edicts[num].s.sound = 0;
        }
        qe->shown = false;
        qe->present_frame = 0;
    }
}

/*
==============================================================================

THE EVENTS

cgame's CG_EntityEvent, as this game shows each: its temp entities, muzzle
flashes, Quake III's sounds, a footstep or a fall as Quake II's events.

==============================================================================
*/

static void q3d_te_pos(int type, const vec3_t pos, multicast_t to)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(type);
    gi.WritePosition((vec_t *)pos);
    gi.multicast((vec_t *)pos, to);
}

static void q3d_te_dir(int type, const vec3_t pos, const vec3_t dir)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(type);
    gi.WritePosition((vec_t *)pos);
    gi.WriteDir((vec_t *)dir);
    gi.multicast((vec_t *)pos, MULTICAST_PVS);
}

// a Quake II event on the entity's edict, if nothing has one this frame
static void q3d_q2_event(int n, int event)
{
    int num = q3d_edict(n);

    if (num > 0 && g_edicts[num].inuse && !g_edicts[num].s.event)
        g_edicts[num].s.event = event;
}

// a player's name for the obituaries, "noname" for none
static const char *q3d_name(int c)
{
    if (c < 0 || c >= Q3D_MAX_CLIENTS || !q3d.players[c].active)
        return NULL;
    return q3d.players[c].name;
}

// cg_event.c CG_Obituary
static void q3d_obituary(const q3d_es_t *es)
{
    int         target = es->otherEntityNum, attacker = es->otherEntityNum2, mod = es->eventParm;
    const char  *tname = q3d_name(target), *aname, *message = NULL, *message2 = "";
    char        sex;

    if (!tname)
        return;
    sex = q3d.players[target].sex;

    switch (mod) {
    case MOD3_SUICIDE:      message = "suicides"; break;
    case MOD3_FALLING:      message = "cratered"; break;
    case MOD3_CRUSH:        message = "was squished"; break;
    case MOD3_WATER:        message = "sank like a rock"; break;
    case MOD3_SLIME:        message = "melted"; break;
    case MOD3_LAVA:         message = "does a back flip into the lava"; break;
    case MOD3_TARGET_LASER: message = "saw the light"; break;
    case MOD3_TRIGGER_HURT: message = "was in the wrong place"; break;
    default: break;
    }
    if (attacker == target) {
        switch (mod) {
        case MOD3_KAMIKAZE:
            message = "goes out with a bang";
            break;
        case MOD3_GRENADE_SPLASH:
            message = sex == 'f' ? "tripped on her own grenade" : sex == 'n' ? "tripped on its own grenade" : "tripped on his own grenade";
            break;
        case MOD3_ROCKET_SPLASH:
            message = sex == 'f' ? "blew herself up" : sex == 'n' ? "blew itself up" : "blew himself up";
            break;
        case MOD3_PLASMA_SPLASH:
            message = sex == 'f' ? "melted herself" : sex == 'n' ? "melted itself" : "melted himself";
            break;
        case MOD3_BFG_SPLASH:
            message = "should have used a smaller gun";
            break;
        case MOD3_PROXIMITY_MINE:
            message = sex == 'f' ? "found her prox mine" : sex == 'n' ? "found its prox mine" : "found his prox mine";
            break;
        default:
            message = sex == 'f' ? "killed herself" : sex == 'n' ? "killed itself" : "killed himself";
            break;
        }
    }
    if (message) {
        gi.cprintf(&g_edicts[1], PRINT_MEDIUM, "%s %s.\n", tname, message);
        return;
    }

    aname = q3d_name(attacker);
    if (attacker == q3d.client && aname)
        gi.centerprintf(&g_edicts[1], "You fragged %s", tname);

    if (aname) {
        switch (mod) {
        case MOD3_GRAPPLE:          message = "was caught by"; break;
        case MOD3_GAUNTLET:         message = "was pummeled by"; break;
        case MOD3_MACHINEGUN:       message = "was machinegunned by"; break;
        case MOD3_SHOTGUN:          message = "was gunned down by"; break;
        case MOD3_GRENADE:          message = "ate"; message2 = "'s grenade"; break;
        case MOD3_GRENADE_SPLASH:   message = "was shredded by"; message2 = "'s shrapnel"; break;
        case MOD3_ROCKET:           message = "ate"; message2 = "'s rocket"; break;
        case MOD3_ROCKET_SPLASH:    message = "almost dodged"; message2 = "'s rocket"; break;
        case MOD3_PLASMA:
        case MOD3_PLASMA_SPLASH:    message = "was melted by"; message2 = "'s plasmagun"; break;
        case MOD3_RAILGUN:          message = "was railed by"; break;
        case MOD3_LIGHTNING:        message = "was electrocuted by"; break;
        case MOD3_BFG:
        case MOD3_BFG_SPLASH:       message = "was blasted by"; message2 = "'s BFG"; break;
        case MOD3_NAIL:             message = "was nailed by"; break;
        case MOD3_CHAINGUN:         message = "got lead poisoning from"; message2 = "'s Chaingun"; break;
        case MOD3_PROXIMITY_MINE:   message = "was too close to"; message2 = "'s Prox Mine"; break;
        case MOD3_KAMIKAZE:         message = "falls to"; message2 = "'s Kamikaze blast"; break;
        case MOD3_JUICED:           message = "was juiced by"; break;
        case MOD3_TELEFRAG:         message = "tried to invade"; message2 = "'s personal space"; break;
        default:                    message = "was killed by"; break;
        }
        gi.cprintf(&g_edicts[1], PRINT_MEDIUM, "%s %s %s%s\n", tname, message, aname, message2);
        return;
    }
    gi.cprintf(&g_edicts[1], PRINT_MEDIUM, "%s died.\n", tname);
}

// EV_GLOBAL_TEAM_SOUND, from the recorded player's side
static void q3d_team_sound(int which, bool carrying)
{
    int team = q3d.players[q3d.client].team;

    switch (which) {
    case GTS3_RED_CAPTURE:
    case GTS3_BLUE_CAPTURE:
        q3d_local_sound(team == (which == GTS3_RED_CAPTURE ? 1 : 2) ?
                        "teamplay/flagcapture_yourteam.wav" : "teamplay/flagcapture_opponent.wav");
        break;
    case GTS3_RED_RETURN:
    case GTS3_BLUE_RETURN:
        q3d_local_sound(team == (which == GTS3_RED_RETURN ? 1 : 2) ?
                        "teamplay/flagreturn_yourteam.wav" : "teamplay/flagreturn_opponent.wav");
        q3d_local_sound(which == GTS3_RED_RETURN ? "teamplay/voc_blue_returned.wav" : "teamplay/voc_red_returned.wav");
        break;
    case GTS3_RED_TAKEN:
    case GTS3_BLUE_TAKEN: {
        // the taking team: red for RED_TAKEN. The one who took it hears
        // "you have the flag" (CG_CheckLocalSounds) instead.
        bool ours = team == (which == GTS3_RED_TAKEN ? 1 : 2);

        if (carrying) {
            q3d_local_sound("teamplay/voc_you_flag.wav");
            break;
        }
        q3d_local_sound(ours ? "teamplay/flagtaken_yourteam.wav" : "teamplay/flagtaken_opponent.wav");
        q3d_local_sound(ours ? "teamplay/voc_team_flag.wav" : "teamplay/voc_enemy_flag.wav");
        break;
    }
    case GTS3_REDTEAM_SCORED:
        q3d_local_sound("teamplay/voc_red_scores.wav");
        break;
    case GTS3_BLUETEAM_SCORED:
        q3d_local_sound("teamplay/voc_blue_scores.wav");
        break;
    case GTS3_REDTEAM_TOOK_LEAD:
        q3d_local_sound("feedback/redleads.wav");
        break;
    case GTS3_BLUETEAM_TOOK_LEAD:
        q3d_local_sound("feedback/blueleads.wav");
        break;
    case GTS3_TEAMS_ARE_TIED:
        q3d_local_sound("feedback/teamstied.wav");
        break;
    default:
        break;
    }
}

/*
EV_SHOTGUN: the muzzle (pos), the aim (origin2) and the seed of the
server's spread (eventParm) - cgame's CG_ShotgunPattern drew the same eleven
pellets from them the server fired. Each one's mark where it meets a wall,
or blood on a player.
*/
static void q3d_shotgun(const q3d_es_t *es, int self)
{
    vec3_t  start, forward, right, up, end, dir, back;
    trace_t tr;
    float   len;
    int     seed = es->eventParm, i;

    VectorCopy(es->pos.trBase, start);
    VectorCopy(es->origin2, forward);
    if (VectorNormalize(forward) <= 0)
        return;
    Q3_PerpendicularVector(right, forward);
    CrossProduct(forward, right, up);

    for (i = 0; i < 11; i++) {
        float r = Q3_Q_crandom(&seed) * 700 * 16;
        float u = Q3_Q_crandom(&seed) * 700 * 16;

        // DEFAULT_SHOTGUN_SPREAD at 8192 * 16 out, as a direction
        VectorScale(forward, 8192 * 16, dir);
        VectorMA(dir, r, right, dir);
        VectorMA(dir, u, up, dir);
        VectorNormalize(dir);

        VectorMA(start, 8192, dir, end);
        tr = gi.trace(start, NULL, NULL, end, &g_edicts[1], MASK_SOLID);
        len = 8192 * tr.fraction;
        if (q3d_shot_player(start, dir, &len, self)) {
            VectorMA(start, len, dir, end);
            VectorNegate(dir, back);
            q3d_te_dir(TE_BLOOD, end, back);
            continue;
        }
        if (tr.fraction >= 1.0f || (tr.surface && (tr.surface->flags & SURF_SKY)))
            continue;
        q3d_te_dir(TE_Q3_SHOTGUN, tr.endpos, tr.plane.normal);
    }
}

static void q3d_fire_event(const q3d_event_t *ev)
{
    const q3d_es_t  *es = &ev->es;
    const int       n = es->number, parm = ev->parm;
    int             num = q3d_edict(n), i;
    q3d_ent_t       *qe = n < Q3D_MAX_GENTITIES ? &q3d.ents[n] : NULL;
    vec3_t          pos, dir;

    if (ev->ps)
        VectorCopy(q3d.view_origin, pos);
    else if (num > 0 && qe && qe->present_frame == q3d.frame)
        VectorCopy(g_edicts[num].s.origin, pos);
    else
        q3d_eval(&es->pos, ev->time, pos);

    switch (ev->event) {
    case EV3_FOOTSTEP:
    case EV3_FOOTSTEP_METAL:
    case EV3_FOOTSPLASH:
    case EV3_FOOTWADE:
    case EV3_SWIM:
        q3d_q2_event(n, EV_FOOTSTEP);
        break;
    case EV3_FALL_SHORT:
        q3d_q2_event(n, EV_FALLSHORT);
        break;
    case EV3_FALL_MEDIUM:
        q3d_q2_event(n, EV_FALL);
        break;
    case EV3_FALL_FAR:
        q3d_q2_event(n, EV_FALLFAR);
        break;
    case EV3_JUMP_PAD:
        gi.positioned_sound(pos, g_edicts, CHAN_AUTO, Q3_SoundIndex("world/jumppad.wav"), 1, ATTN_NORM, 0);
        q3d_sound(n, pos, CHAN_VOICE, Q3_SoundIndex("*jump1.wav"), ATTN_NORM);
        break;
    case EV3_JUMP:
        q3d_sound(n, pos, CHAN_VOICE, Q3_SoundIndex("*jump1.wav"), ATTN_NORM);
        break;
    case EV3_WATER_TOUCH:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("player/watr_in.wav"), ATTN_NORM);
        break;
    case EV3_WATER_LEAVE:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("player/watr_out.wav"), ATTN_NORM);
        break;
    case EV3_WATER_UNDER:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("player/watr_un.wav"), ATTN_NORM);
        break;

    case EV3_ITEM_PICKUP: {
        const q3d_look_t *look = q3d_item_look(parm);

        if (parm > 0 && parm < Q3D_NUM_ITEMS && q3d_items[parm].own_sound)
            q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex(q3d_items[parm].own_sound), ATTN_NORM);
        else if (look && look->item->pickup_sound)
            q3d_sound(n, pos, CHAN_AUTO, gi.soundindex(look->item->pickup_sound), ATTN_NORM);
        if (ev->ps && look) {
            q3d.pickup_item = parm;
            q3d.pickup_until = q3d.time + 3000;
        }
        break;
    }
    case EV3_GLOBAL_ITEM_PICKUP:
        if (parm > 0 && parm < Q3D_NUM_ITEMS && q3d_items[parm].global_sound)
            q3d_local_sound(q3d_items[parm].global_sound);
        break;

    case EV3_NOAMMO:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("weapons/noammo.wav"), ATTN_NORM);
        break;
    case EV3_CHANGE_WEAPON:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("weapons/change.wav"), ATTN_NORM);
        break;

    case EV3_FIRE_WEAPON: {
        const char  *shot = NULL;
        int         flash = 0;
        // the trigger already down last frame (cgame's pe.lightningFiring)
        bool        held = ev->ps ? q3d.lg_was_firing : (qe && qe->lg_was_firing);

        if (!Q3W_DemoGun(es->weapon, &flash, &shot, NULL, NULL))
            break;
        if (ev->ps)
            q3d.shots++;
        // the Lightning Gun's sounds only as the trigger goes down (CG_FireWeapon)
        if (es->weapon != Q3D_WP_LIGHTNING || !held) {
            if (shot)
                q3d_sound(n, pos, CHAN_WEAPON, Q3_SoundIndex(shot), ATTN_NORM);
            if (es->powerups & (1 << Q3D_PW_QUAD))
                q3d_sound(n, pos, CHAN_ITEM, Q3_SoundIndex("items/damage3.wav"), ATTN_NORM);
        }
        // one flash a frame (the Lightning Gun fires every 50 ms)
        if (num > 0 && qe && qe->flash_frame != q3d.frame) {
            qe->flash_frame = q3d.frame;
            gi.WriteByte(svc_muzzleflash);
            gi.WriteShort(num);
            gi.WriteByte(flash);
            gi.multicast(pos, MULTICAST_PVS);
        }
        break;
    }

    case EV3_ITEM_RESPAWN:
        q3d_q2_event(n, EV_ITEM_RESPAWN);
        break;
    case EV3_ITEM_POP:
        gi.positioned_sound(pos, g_edicts, CHAN_AUTO, Q3_SoundIndex("items/respawn1.wav"), 1, ATTN_NORM, 0);
        break;
    case EV3_PLAYER_TELEPORT_IN:
        q3d_te_pos(TE_TELEPORT_EFFECT, pos, MULTICAST_PVS);
        gi.positioned_sound(pos, g_edicts, CHAN_AUTO, Q3_SoundIndex("world/telein.wav"), 1, ATTN_NORM, 0);
        break;
    case EV3_PLAYER_TELEPORT_OUT:
        q3d_te_pos(TE_TELEPORT_EFFECT, pos, MULTICAST_PVS);
        gi.positioned_sound(pos, g_edicts, CHAN_AUTO, Q3_SoundIndex("world/teleout.wav"), 1, ATTN_NORM, 0);
        break;
    case EV3_GRENADE_BOUNCE:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex((rand() & 1) ? "weapons/grenade/hgrenb1a.wav" :
                                                   "weapons/grenade/hgrenb2a.wav"), ATTN_NORM);
        break;

    case EV3_GENERAL_SOUND:
        q3d_sound(n, pos, CHAN_VOICE, q3d_sound_index(parm), ATTN_NORM);
        break;
    case EV3_GLOBAL_SOUND:
        gi.sound(&g_edicts[1], CHAN_AUTO, q3d_sound_index(parm), 1, ATTN_NONE, 0);
        break;
    case EV3_GLOBAL_TEAM_SOUND: {
        const q3d_ps_t *ps = &q3d.cur->ps;

        q3d_team_sound(parm, ps->powerups[Q3D_PW_REDFLAG] || ps->powerups[Q3D_PW_BLUEFLAG] ||
                             ps->powerups[Q3D_PW_NEUTRALFLAG]);
        break;
    }

    case EV3_BULLET_HIT_FLESH:
        VectorSet(dir, 0, 0, 1);
        q3d_te_dir(TE_BLOOD, pos, dir);
        break;
    case EV3_BULLET_HIT_WALL: {
        // the chaingun's own hit sounds for its bullets: its shooter's gun
        int shooter = es->otherEntityNum, weapon = 0;
        const q3d_es_t *s = q3d_find(q3d.cur, shooter);

        if (shooter == q3d.client)
            weapon = q3d.cur->ps.weapon;
        else if (s)
            weapon = s->weapon;
        Q3_ByteToDir(parm, dir);
        q3d_te_dir(weapon == Q3D_WP_CHAINGUN ? TE_TA_CHAINGUN : TE_Q3_BULLET, pos, dir);
        break;
    }
    case EV3_MISSILE_HIT:
    case EV3_MISSILE_MISS:
    case EV3_MISSILE_MISS_METAL:
        Q3_ByteToDir(parm, dir);
        if (VectorLength(dir) < 0.5f)
            VectorSet(dir, 0, 0, 1);
        // a player hit bleeds; only the blasts go off on one as well
        // (CG_MissileHitPlayer) - a plasma ball or the BFG's just bleeds
        if (ev->event == EV3_MISSILE_HIT) {
            q3d_te_dir(TE_BLOOD, pos, dir);
            if (es->weapon != Q3D_WP_GRENADE && es->weapon != Q3D_WP_ROCKET &&
                es->weapon != Q3D_WP_NAILGUN && es->weapon != Q3D_WP_CHAINGUN &&
                es->weapon != Q3D_WP_PROX)
                break;
        }
        switch (es->weapon) {
        case Q3D_WP_GRENADE:
            q3d_te_pos(TE_Q3_GRENADE_EXPLOSION, pos, MULTICAST_PHS);
            break;
        case Q3D_WP_ROCKET:
            q3d_te_pos(TE_Q3_ROCKET_EXPLOSION, pos, MULTICAST_PHS);
            break;
        case Q3D_WP_PLASMA:
            VectorMA(pos, 1, dir, pos);
            q3d_te_dir(TE_Q3_PLASMA_EXPLOSION, pos, dir);
            break;
        case Q3D_WP_BFG:
            q3d_te_pos(TE_Q3_BFG_EXPLOSION, pos, MULTICAST_PHS);
            break;
        case Q3D_WP_NAILGUN:
            q3d_te_dir(TE_TA_NAIL, pos, dir);
            break;
        case Q3D_WP_PROX:
            q3d_te_pos(TE_TA_PROX_EXPLOSION, pos, MULTICAST_PHS);
            break;
        default:
            break;
        }
        break;

    case EV3_RAILTRAIL:
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_Q3_RAILTRAIL);
        gi.WritePosition((vec_t *)es->origin2);
        gi.WritePosition(pos);
        gi.multicast((vec_t *)es->origin2, MULTICAST_PHS);
        break;
    case EV3_SHOTGUN:
        q3d_shotgun(es, es->otherEntityNum);
        break;

    case EV3_PAIN:
        // one a half second (cg_event.c CG_PainEvent), and a flinch
        if (qe && q3d.time - qe->pain_time >= 500) {
            const char *s = parm < 25 ? "*pain25_1.wav" : parm < 50 ? "*pain50_1.wav" :
                            parm < 75 ? "*pain75_1.wav" : "*pain100_1.wav";

            qe->pain_time = q3d.time;
            q3d_sound(n, pos, CHAN_VOICE, Q3_SoundIndex(s), ATTN_NORM);
        }
        if (qe)
            qe->pain_until = q3d.time + Q3D_FRAME_MSEC;
        break;
    case EV3_DEATH1:
    case EV3_DEATH2:
    case EV3_DEATH3:
        q3d_sound(n, pos, CHAN_VOICE, Q3_SoundIndex(va("*death%d.wav", ev->event - EV3_DEATH1 + 1)), ATTN_NORM);
        break;
    case EV3_OBITUARY:
        q3d_obituary(es);
        break;

    case EV3_POWERUP_QUAD:
        q3d_sound(n, pos, CHAN_ITEM, Q3_SoundIndex("items/damage3.wav"), ATTN_NORM);
        break;
    case EV3_POWERUP_BATTLESUIT:
        q3d_sound(n, pos, CHAN_ITEM, Q3_SoundIndex("items/protect3.wav"), ATTN_NORM);
        break;
    case EV3_POWERUP_REGEN:
        q3d_sound(n, pos, CHAN_ITEM, Q3_SoundIndex("items/regen.wav"), ATTN_NORM);
        break;

    case EV3_GIB_PLAYER:
        // the client throws the pieces and plays the splat (q3weapons.c)
        q3d_te_pos(TE_Q3_GIBS, pos, MULTICAST_PVS);
        break;

    case EV3_PROXIMITY_MINE_STICK:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("weapons/proxmine/wstbimpd.wav"), ATTN_NORM);
        break;
    case EV3_PROXIMITY_MINE_TRIGGER:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("weapons/proxmine/wstbactv.wav"), ATTN_NORM);
        break;
    case EV3_KAMIKAZE:
        q3d_te_pos(TE_TA_KAMIKAZE, pos, MULTICAST_PHS);
        break;
    case EV3_INVUL_IMPACT:
        q3d_sound(n, pos, CHAN_AUTO, Q3_SoundIndex("items/invul_impact_01.wav"), ATTN_NORM);
        break;
    case EV3_JUICED:
        q3d_te_pos(TE_TA_JUICED, pos, MULTICAST_PHS);
        break;
    case EV3_TAUNT:
        q3d_sound(n, pos, CHAN_VOICE, Q3_SoundIndex("*taunt.wav"), ATTN_NORM);
        break;
    default:
        break;
    }
    (void)i;
}

static void q3d_run_events(void)
{
    int i;

    for (i = 0; i < q3d.num_events; i++)
        q3d_fire_event(&q3d.events[i]);
    q3d.num_events = 0;
}

/*
The recorded player's view and HUD, from the playerstate as cgame drew a
demo's: the eye interpolated between snapshots, the bob and the run's lean
(CG_OffsetFirstPersonView), Quake III's gun in hand (ps.gunframe, g_q3weap.c
Q3W_SetGunframe), and the stats the Team Arena HUD reads (hud_q3.c).
*/
static void q3d_view(float frac)
{
    edict_t         *ent = &g_edicts[1];
    gclient_t       *client;
    player_state_t  *ps;
    const q3d_ps_t  *qp = &q3d.cur->ps, *op = &q3d.prev->ps;
    const char      *ready = NULL, *firing = NULL;
    gitem_t         *gun;
    vec3_t          origin, angles, forward, right, velocity;
    bool            alive, normal, teleported;
    int             i, health;

    if (!ent->inuse || !ent->client)
        return;
    client = ent->client;
    ps = &client->ps;

    teleported = (qp->eFlags ^ op->eFlags) & Q3D_EF_TELEPORT_BIT;
    for (i = 0; i < 3; i++) {
        if (teleported || q3d.prev == q3d.cur) {
            origin[i] = qp->origin[i];
            angles[i] = qp->viewangles[i];
        } else {
            origin[i] = op->origin[i] + frac * (qp->origin[i] - op->origin[i]);
            angles[i] = q3d_lerp_angle(op->viewangles[i], qp->viewangles[i], frac);
        }
        velocity[i] = qp->velocity[i];
    }
    VectorCopy(origin, q3d.view_origin);

    health = qp->stats[Q3D_STAT_HEALTH];
    normal = qp->pm_type != Q3D_PM_INTERMISSION && qp->pm_type != Q3D_PM_SPINTERMISSION;
    alive = health > 0 && normal;

    ps->pmove.pm_type = PM_FREEZE;
    ps->pmove.pm_flags = PMF_NO_PREDICTION;
    for (i = 0; i < 3; i++) {
        ps->pmove.origin[i] = COORD2SHORT(origin[i]);
        ps->pmove.velocity[i] = 0;
        ps->pmove.delta_angles[i] = 0;
    }
    VectorCopy(origin, ent->s.origin);
    VectorCopy(origin, ent->s.old_origin);
    ent->s.modelindex = 0;
    ent->s.sound = 0;

    VectorClear(ps->kick_angles);
    VectorClear(ps->gunoffset);
    VectorClear(ps->gunangles);
    Vector4Clear(ps->blend);
    ps->rdflags = 0;
    ps->gunindex = 0;
    ps->gunframe = 0;

    if (!normal) {
        VectorCopy(angles, ps->viewangles);
        VectorClear(ps->viewoffset);
    } else if (health <= 0) {
        // dead: on the side, looking at the killer (CG_OffsetFirstPersonView)
        VectorSet(ps->viewangles, -15, qp->stats[Q3D_STAT_DEAD_YAW], 40);
        VectorSet(ps->viewoffset, 0, 0, qp->viewheight);
    } else {
        float xyspeed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
        float bobfracsin = fabsf(sinf((qp->bobCycle & 127) / 127.0f * M_PI));
        float speed = max(xyspeed, 200.0f), delta, bob;

        VectorCopy(angles, ps->viewangles);
        AngleVectors(angles, forward, right, NULL);

        // the run's lean and the bob (cg_runpitch, cg_runroll, cg_bobpitch, cg_bobroll)
        ps->kick_angles[PITCH] += DotProduct(velocity, forward) * 0.002f;
        ps->kick_angles[ROLL] -= DotProduct(velocity, right) * 0.005f;
        delta = bobfracsin * 0.002f * speed;
        if (qp->pm_flags & Q3D_PMF_DUCKED)
            delta *= 3;
        ps->kick_angles[PITCH] += delta;
        delta = bobfracsin * 0.002f * speed;
        if (qp->pm_flags & Q3D_PMF_DUCKED)
            delta *= 3;
        if (qp->bobCycle & 128)
            delta = -delta;
        ps->kick_angles[ROLL] += delta;

        bob = min(bobfracsin * xyspeed * 0.005f, 6.0f);
        VectorSet(ps->viewoffset, 0, 0, qp->viewheight + bob);
    }

    // the gun in hand, and its hum
    gun = Q3W_DemoGun(qp->weapon, NULL, NULL, &ready, &firing);
    if (alive && gun && gun->view_model) {
        int state;

        switch (qp->weaponstate) {
        case 1:  state = Q3_GUNFRAME_RAISING; break;
        case 2:  state = Q3_GUNFRAME_DROPPING; break;
        case 3:  state = Q3_GUNFRAME_FIRING; break;
        default: state = Q3_GUNFRAME_READY; break;
        }
        ps->gunindex = gi.modelindex(gun->view_model);
        ps->gunframe = state | (q3d.shots & Q3_GUNFRAME_SHOTS);
        if ((qp->eFlags & Q3D_EF_FIRING) && firing)
            ent->s.sound = Q3_SoundIndex(firing);
        else if (ready)
            ent->s.sound = Q3_SoundIndex(ready);
    }
    if (alive && qp->weapon == Q3D_WP_LIGHTNING && (qp->eFlags & Q3D_EF_FIRING))
        q3d_lightning(1, q3d.client, origin, angles, qp->viewheight);
    q3d.lg_was_firing = q3d.lg_firing;
    q3d.lg_firing = qp->weapon == Q3D_WP_LIGHTNING && (qp->eFlags & Q3D_EF_FIRING);

    if (normal && (gi.pointcontents(origin) & MASK_WATER)) {
        vec3_t eye;

        VectorAdd(origin, ps->viewoffset, eye);
        if (gi.pointcontents(eye) & MASK_WATER)
            ps->rdflags |= RDF_UNDERWATER;
    }

    // the HUD
    memset(ps->stats, 0, sizeof(ps->stats));
    if (!normal)
        return;     // the intermission: none
    ps->stats[STAT_HEALTH] = (short)clamp(health, -999, 999);
    ps->stats[STAT_ARMOR] = (short)qp->stats[Q3D_STAT_ARMOR];
    if (qp->stats[Q3D_STAT_ARMOR] > 0)
        ps->stats[STAT_ARMOR_ICON] = gi.imageindex("i_bodyarmor");
    if (gun && gun->ammo && qp->weapon >= 0 && qp->weapon < 16 && qp->ammo[qp->weapon] >= 0) {
        gitem_t *ammo = FindItem(gun->ammo);

        ps->stats[STAT_AMMO] = (short)qp->ammo[qp->weapon];
        ps->stats[STAT_AMMO_ICON] = ammo ? q3d_icon(ammo) : q3d_icon(gun);
    }
    ps->stats[STAT_FRAGS] = (short)qp->persistant[Q3D_PERS_SCORE];
    ps->stats[STAT_SELECTED_ICON] = q3d_item_icon(qp->stats[Q3D_STAT_HOLDABLE_ITEM]);

    // the timed powerup that runs out first
    {
        int best = 0, pw;

        for (pw = Q3D_PW_QUAD; pw <= Q3D_PW_FLIGHT; pw++) {
            int left = qp->powerups[pw] - q3d.cur->server_time;

            if (qp->powerups[pw] && left > 0 && (!best || left < qp->powerups[best] - q3d.cur->server_time))
                best = pw;
        }
        if (best) {
            ps->stats[STAT_TIMER_ICON] = q3d_item_icon(q3d_powerup_item(best));
            ps->stats[STAT_TIMER] = (short)((qp->powerups[best] - q3d.cur->server_time + 999) / 1000);
        }
    }

    if (q3d.pickup_item && q3d.time < q3d.pickup_until) {
        const q3d_look_t *look = q3d_item_look(q3d.pickup_item);

        if (look) {
            ps->stats[STAT_PICKUP_ICON] = q3d_icon(look->item);
            ps->stats[STAT_PICKUP_STRING] = CS_ITEMS + ITEM_INDEX(look->item);
        }
    }

    // CTF's (g_ctf.c CTF_SetStats): the team pics and captures, yours, the
    // flag you carry, the persistent powerup
    if (q3d.gametype >= Q3D_GT_TEAM) {
        int team = q3d.players[q3d.client].team;
        int pic1 = gi.imageindex("i_ctf1"), pic2 = gi.imageindex("i_ctf2");

        ps->stats[STAT_CTF_TEAM1_PIC] = pic1;
        ps->stats[STAT_CTF_TEAM1_CAPS] = (short)atoi(q3d_cs(Q3D_CS_SCORES1));
        ps->stats[STAT_CTF_TEAM2_PIC] = pic2;
        ps->stats[STAT_CTF_TEAM2_CAPS] = (short)atoi(q3d_cs(Q3D_CS_SCORES2));
        if (team == 1)
            ps->stats[STAT_CTF_JOINED_TEAM1_PIC] = pic1;
        else if (team == 2)
            ps->stats[STAT_CTF_JOINED_TEAM2_PIC] = pic2;
        if (qp->powerups[Q3D_PW_REDFLAG])
            ps->stats[STAT_CTF_FLAG_PIC] = pic1;
        else if (qp->powerups[Q3D_PW_BLUEFLAG])
            ps->stats[STAT_CTF_FLAG_PIC] = pic2;
        else if (qp->powerups[Q3D_PW_NEUTRALFLAG])
            ps->stats[STAT_CTF_FLAG_PIC] = gi.imageindex("/icons/iconf_neutral1.tga");
        ps->stats[STAT_CTF_TECH] = q3d_item_icon(qp->stats[Q3D_STAT_PERSISTANT]);
    } else {
        // a free-for-all's score boxes (p_hud.c G_SetQ3ScoreStats)
        int mine = qp->persistant[Q3D_PERS_SCORE], s1 = atoi(q3d_cs(Q3D_CS_SCORES1));
        int s2 = atoi(q3d_cs(Q3D_CS_SCORES2));

        ps->stats[STAT_Q3_SCORES] = STAT_Q3_SCORES_TAG | STAT_Q3_SCORES_OTHER;
        ps->stats[STAT_Q3_OTHER_SCORE] = (short)(mine == s1 ? s2 : s1);
        ps->stats[STAT_Q3_FRAGLIMIT] = (short)q3d.fraglimit;
    }

    // the player's own powerups: the quad's glow on the gun and the rest
    ent->s.effects = 0;
    ent->s.renderfx = 0;
    {
        int bits = 0;

        for (i = 0; i < Q3D_PW_NUM; i++)
            if (qp->powerups[i])
                bits |= 1 << i;
        q3d_powerup_effects(ent, bits, alive, q3d.players[q3d.client].team);
        ent->s.modelindex3 = 0;     // no flag on your own back in first person
    }
    gi.linkentity(ent);
}

// the end of the demo: the attract loop's next, or back to the menu
static void q3d_finish(void)
{
    cvar_t *next;

    if (q3d.finished)
        return;
    q3d.finished = true;

    next = gi.cvar("nextserver", "", 0);
    if (next->string[0]) {
        char cmd[MAX_QPATH * 2];

        Q_snprintf(cmd, sizeof(cmd), "%s\n", next->string);
        gi.cvar_set("nextserver", "");
        gi.AddCommandString(cmd);
    } else {
        gi.AddCommandString("killserver\n");
    }
}

void Q3Demo_RunFrame(void)
{
    bool    more = true;
    float   frac = 1;

    if (!q3d.started || q3d.finished)
        return;
    if (q3d.broken || !q3d.cur) {
        q3d_finish();   // it couldn't be read
        return;
    }

    // read until the newest snapshot is at or past the clock
    q3d.time += Q3D_FRAME_MSEC;
    q3d.frame++;
    while (q3d.cur->server_time < q3d.time) {
        if (!q3d_next_message()) {
            more = false;
            break;
        }
    }

    if (q3d.prev != q3d.cur && q3d.cur->server_time > q3d.prev->server_time) {
        frac = (float)(q3d.time - q3d.prev->server_time) / (q3d.cur->server_time - q3d.prev->server_time);
        clamp(frac, 0, 1);
    }

    q3d_present_entities(frac);
    q3d_view(frac);
    q3d_run_events();
    // the gun's flash count after the shots this frame
    if (g_edicts[1].inuse && g_edicts[1].client && g_edicts[1].client->ps.gunindex)
        g_edicts[1].client->ps.gunframe = (g_edicts[1].client->ps.gunframe & Q3_GUNFRAME_STATE) |
                                          (q3d.shots & Q3_GUNFRAME_SHOTS);

    if (!more)
        q3d_finish();
}
