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

BOTS

Deathmatch opponents for the Bot Match menu, and `sv addbot` in any
deathmatch game.

The rerelease's source has no bot brain: src/rerelease/bots/ is a set of empty
hooks, the bots themselves lived in the closed KEX engine. What it does ship is
their data - bots/settings_PC.txt (the six skill levels), bots/weapons.txt
and navmeshes for every deathmatch map - so these bots are written here
against that data: the same skill table, the same weapon ranges and
priorities, the same meshes. A map with no mesh, which is every map in the
classic game, gets one built when it loads (Nav_Generate in g_nav.c). Each
bot wears a random stock skin and goes by its name (inc/shared/bot_roster.h).

A bot is a client with no connection. It takes a client slot from the top
down (people connecting take them from the bottom up; one that needs a bot's
slot gets it), goes through ClientConnect and ClientBegin like anyone else,
and each server frame thinks once and then moves through ClientThink in four
25 ms commands, exactly the way a player's own commands arrive - so it runs,
jumps, rides lifts, swims and fires through the same pmove and weapon code.
The engine never hears of it; the few imports that address a client by its
connection (gi.unicast, gi.cprintf, gi.centerprintf) skip it - see
Bot_WrapImports.

The settings file names its knobs but not what they do, so this reads them:

    aiming      a damped spring on the view (stiffness, damping), turning no
                faster than max_acceleration degrees a second; velocity_offset
                is how many seconds of the target's motion the aim trails by
                when it does not lead; the modifier is a wander on top -
                max_angle / 10 degrees, re-picked every apply_time seconds,
                `scalar` times larger for a second after a new target
    perception  a target in the field of view is noticed after sight_time;
                lost for forget_non_vis seconds and it is forgotten; a shot
                within sound_range turns the bot after sound_time
    weapons     sight_time before the first shot; fov_scalar widens how far
                off the aim may be and still fire

==============================================================================
*/

#include "g_local.h"
#include "shared/bot_roster.h"

#define BOT_MAX_ROUTE       1024
#define BOT_STEPS           4           // commands per server frame
#define BOT_STEP_MSEC       25
#define BOT_STEP_TIME       0.025f
#define BOT_RUN             400
#define BOT_WALK            200
#define BOT_MAX_NAME        32
#define BOT_AVOIDS          4

cvar_t  *teamplay;

static cvar_t   *bot_match;
static cvar_t   *bot_count;
static cvar_t   *bot_debug;
static cvar_t   *bot_friendly_fire;

/*
bot_q3_ai: how the bots think, the menu's "bot ai".
0 - Quake II's bots: the brain below, the rerelease's skill table.
1 - Quake III characters (the default): the brain below, with Quake III's bots
    playing by their own character files and Quake III's combat rules
    (Bot_Q3* below, g_botchar.c).
2 - Quake III's engine: Quake III's own bot AI and AAS navigation (q3bot/,
    g_q3bot.c) for every bot - its bots as themselves, Quake II's on its
    default character. Only on a map with an AAS (Quake III's maps); on
    others, 1's brain and rules, Quake II's bots on that default character
    too. Read as a bot joins a level.
*/
static cvar_t   *bot_q3_ai;

// the rerelease's six skills as Quake III's (fractions are blends, g_botchar.c)
static const float bot_q3_skill[BOT_NUM_SKILLS] = { 1, 2, 3, 4, 4.5f, 5 };

// ==========================================================================
// SKILLS - bots/settings_PC.txt, in order: practice, easy, medium, hard,
// expert, nightmare. Only the keys the brain below uses.

typedef struct {
    const char  *name;
    float   turn_speed;         // aiming.max_acceleration, degrees a second
    float   spring;             // aiming.spring_stiffness
    float   damping;            // aiming.damping
    bool    lead;               // aiming.lead_targets
    float   lag;                // aiming.velocity_offset
    float   wander_angle;       // aiming.modifier.max_angle
    float   wander_time;        // aiming.modifier.apply_time
    float   wander_scale;       // aiming.modifier.scalar
    bool    combat;             // combat.allow_combat
    bool    combat_items;       // combat.allow_grab_items
    float   min_health;         // combat.min_health_pct
    float   min_ammo;           // combat.min_ammo_pct
    float   min_armor;          // combat.min_armor_pct
    bool    melee;              // allow_melee
    bool    items;              // allow_grab_items
    bool    power_items;        // grab_power_items
    bool    dangers;            // react_to_dangers
    bool    defer_power;        // defer_power_items_to_humans
    float   respawn_min;        // respawn.min_time
    float   respawn_max;        // respawn.max_time
    bool    combat_jump;        // jumping_in_combat
    float   jump_chance;        // jump_chance, percent
    float   jump_cooldown;      // jump_cooldown
    bool    walk_only;          // walk_only (in a fight)
    float   sight_time;         // perception.sight_time
    float   invis_scale;        // perception.invisibility_scalar
    float   invis_dist;         // perception.max_invisibility_distance
    float   fov;                // perception.fov_angle
    float   forget;             // perception.forget_non_vis
    float   sound_range;        // perception.sound_range
    float   sound_time;         // perception.sound_time
    float   fire_sight_time;    // weapons.sight_time
    float   fire_fov_scale;     // weapons.fov_scalar
} bot_skill_t;

static const bot_skill_t bot_skills[BOT_NUM_SKILLS] = {
    { "Practice", 180,  40,  5, false, -0.25f, 60, 2.0f,  2.0f,  false, false, 0.95f, 0.75f, 0.95f,
      false, false, false, false, true,  1.5f, 2.5f, false,  0, 0, true,
      0.30f, 2.0f,  128,  90, 1.0f,  512, 0.5f,   0.3f,  4.0f },
    { "Easy",     225,  60, 10, false, -0.20f, 50, 1.5f,  2.0f,  false, false, 0.95f, 0.75f, 0.95f,
      true,  true,  false, false, true,  1.0f, 1.5f, false, 15, 2, true,
      0.25f, 2.0f,  256, 100, 1.5f,  640, 0.4f,   0.2f,  3.0f },
    { "Medium",   315,  80, 15, true,  -0.15f, 40, 1.0f,  1.1f,  true,  true,  0.95f, 0.75f, 0.95f,
      true,  true,  true,  true,  true,  0.9f, 1.5f, true,  25, 1, false,
      0.25f, 2.0f,  384, 120, 2.0f,  768, 0.3f,   0.15f, 2.0f },
    { "Hard",     360, 125, 20, true,  -0.10f, 30, 0.75f, 1.25f, true,  true,  0.80f, 0.60f, 0.80f,
      true,  true,  true,  true,  true,  0.9f, 1.5f, true,  35, 1, false,
      0.15f, 2.0f,  512, 160, 4.0f, 1024, 0.2f,   0.1f,  1.5f },
    { "Expert",   480, 250, 25, true,   0.00f, 20, 0.4f,  1.5f,  true,  true,  0.70f, 0.40f, 0.70f,
      true,  true,  true,  true,  true,  0.7f, 1.2f, true,  35, 1, false,
      0.10f, 1.5f,  768, 180, 5.0f, 1200, 0.125f, 0.1f,  2.0f },
    { "Nightmare", 720, 500, 30, true,  0.00f, 10, 0.2f,  2.0f,  true,  true,  0.60f, 0.30f, 0.60f,
      true,  true,  true,  true,  false, 0.7f, 1.0f, true,  50, 1, false,
      0.05f, 1.25f, 1024, 180, 6.0f, 1600, 0.1f,   0.1f,  2.0f },
};

// ==========================================================================
// WEAPONS - bots/weapons.txt. Thrown weapons (hand grenades, traps, teslas)
// are left out: they are hold-and-release, and never worth it to a bot.

enum { AIM_CENTER, AIM_HEAD, AIM_FEET };

typedef struct {
    const char  *classname;
    float   ideal_fov;      // degrees
    float   min_range, max_range;
    float   speed;          // projectile speed; 0 = hitscan
    int     priority;
    int     min_ammo;
    int     aim;
    bool    lob;            // thrown on an arc (fire_grenade adds 200 up)
    bool    splash;         // hurts whoever fires it at point blank
    bool    melee;
    float   lob_rise;       // a lob's launch: `speed` along the view and this
                            // much up; 0 = Quake II's 200
    int     index;          // itemlist index, -1 = not in this game
    int     ammo_index;     // its ammo's, -1 = none
} bot_weapon_t;

static bot_weapon_t bot_weapons[] = {
    { "weapon_blaster",         15,   0, 4096, 1500,  1,  0, AIM_CENTER },
    { "weapon_shotgun",         15,   0,  768,    0,  2,  1, AIM_CENTER },
    { "weapon_supershotgun",    15,   0,  768,    0,  4,  2, AIM_CENTER },
    { "weapon_machinegun",      15,   0, 4096,    0,  3,  1, AIM_CENTER },
    { "weapon_chaingun",        15,   0, 4096,    0,  4,  1, AIM_CENTER },
    { "weapon_grenadelauncher", 20, 192,  768,  600,  5,  1, AIM_HEAD, true, true },
    { "weapon_rocketlauncher",  15, 100, 1280,  650,  7,  1, AIM_FEET, false, true },
    { "weapon_hyperblaster",    30,  64, 1024, 1000,  6,  1, AIM_CENTER },
    { "weapon_railgun",         15, 256, 8192,    0,  6,  1, AIM_CENTER },
    { "weapon_bfg",             30, 128, 4096,  400,  8, 50, AIM_CENTER, false, true },
    { "weapon_chainfist",       30,   0,   64,    0,  1,  0, AIM_CENTER, false, false, true },
    { "weapon_plasmabeam",      15, 128, 8192,    0,  8,  2, AIM_CENTER },
    { "weapon_etf_rifle",       15,   0,  768,  750,  5,  1, AIM_CENTER },
    { "weapon_proxlauncher",    15,  96, 1048,  600,  5,  1, AIM_HEAD, true, true },
    { "weapon_boomer",          15,   0,  768,  500,  6,  1, AIM_CENTER },
    { "weapon_phalanx",         15,  80, 2048,  725,  7,  1, AIM_FEET, false, true },
    { "weapon_disintegrator",    5,   0,  512,  800, 10,  1, AIM_CENTER },
    // Quake III's guns, in a bot match with them (g_q3weap.c). Its grenade
    // leaves at 700 along the view tipped 0.2 up, ~686 + 137 up.
    { "weapon_q3_gauntlet",     30,   0,   64,    0,  1,  0, AIM_CENTER, false, false, true },
    { "weapon_q3_machinegun",   15,   0, 4096,    0,  3,  1, AIM_CENTER },
    { "weapon_q3_shotgun",      15,   0,  768,    0,  4,  1, AIM_CENTER },
    { "weapon_q3_grenadelauncher", 20, 192, 768,  686,  5,  1, AIM_HEAD, true, true, false, 137 },
    { "weapon_q3_rocketlauncher", 15, 100, 1280,  900,  7,  1, AIM_FEET, false, true },
    { "weapon_q3_lightning",    15,   0,  768,    0,  6,  1, AIM_CENTER },
    { "weapon_q3_railgun",      15, 256, 8192,    0,  6,  1, AIM_CENTER },
    { "weapon_q3_plasmagun",    30,  64, 1024, 2000,  6,  1, AIM_CENTER },
    { "weapon_q3_bfg",          30, 128, 4096, 2000,  8,  1, AIM_CENTER, false, true },
};

#define BOT_NUM_WEAPONS ((int)(sizeof(bot_weapons) / sizeof(bot_weapons[0])))

// ==========================================================================
// THE BOTS

typedef enum {
    GOAL_NONE,
    GOAL_ITEM,
    GOAL_ROAM,
    GOAL_CHASE,
    GOAL_CTF            // CTF: a flag stand, a carrier, a team-mate to escort
} bot_goal_t;

typedef struct {
    // who: kept across levels
    bool        active;
    bool        spawned;            // connected into the current level
    char        name[BOT_MAX_NAME];
    char        skin[MAX_QPATH];
    int         skill;
    int         team;

    // the view, driven by the aim spring
    vec3_t      view;
    vec3_t      turn;               // pitch and yaw velocity, degrees a second
    short       delta[3];           // delta_angles last seen - a change is a teleport
    vec2_t      wander;
    float       wander_time;
    float       acquired_time;

    // who it knows about
    float       aware[MAX_CLIENTS];
    edict_t     *enemy;
    float       enemy_seen;         // level.time it was last in sight
    float       enemy_visible_since;
    bool        enemy_visible;
    vec3_t      enemy_pos;
    vec3_t      heard_pos;
    float       heard_at;           // when it turns to look
    float       heard_until;

    // where it is going
    bot_goal_t  goal;
    edict_t     *goal_item;
    vec3_t      goal_pos;
    float       goal_time;          // when it set out
    float       next_goal_time;
    int         route[BOT_MAX_ROUTE];
    int         route_len, route_pos;
    float       next_shortcut_time;
    float       replan_time;
    edict_t     *avoid[BOT_AVOIDS];
    float       avoid_until[BOT_AVOIDS];
    vec3_t      leash;              // CTF: stay within leash_radius of here
    float       leash_radius;       // (0 = anywhere)

    // getting unstuck
    vec3_t      stuck_pos;
    float       stuck_time;
    int         stuck_count;
    float       unstick_until;
    float       unstick_dir;
    bool        waiting;            // for a lift; not stuck
    vec3_t      last_origin;

    // in the air off a Quake III jump pad (Bot_PadFlight)
    bool        pad_flight;
    edict_t     *pad;               // the pad pad_landing is for
    vec3_t      pad_landing;

    // fighting
    float       strafe;             // -1 left, 1 right
    float       strafe_until;
    float       next_jump_time;
    float       next_weapon_time;
    vec3_t      dodge;              // away from a grenade or rocket
    bool        attack;
    bool        jump;

    // dying
    bool        dead;
    float       respawn_time;

    // the Quake III campaign (g_sp.c): this arena's bot, joining join_delay
    // seconds after you are in, and announced as it does
    bool        campaign;
    float       join_delay;

    // Quake III's rules (Bot_Q3*), with a character to play them by. Named
    // after the bot_state_t fields of ioq3's ai_dmq3.c they stand for.
    bot_character_t ch;
    bool        crouch;             // this frame's commands crouch
    vec2_t      q3_turn;            // viewanglespeed, pitch and yaw
    bool        q3_turning;         // the view is on the over-reaction model
    vec3_t      q3_ideal;           // ideal_viewangles while it aims
    float       q3_sight_time;      // enemysight_time
    float       q3_enemypos_time;   // enemyposition_time
    vec3_t      q3_enemy_org;       // enemyorigin, every half second
    vec3_t      q3_enemy_vel;       // enemyvelocity, the same
    float       q3_throttle_wait;   // firethrottlewait_time
    float       q3_throttle_shoot;  // firethrottleshoot_time
    float       q3_strafe_time;     // attackstrafe_time
    bool        q3_strafe_right;    // BFL_STRAFERIGHT
    float       q3_crouch_until;    // attackcrouch_time
    float       q3_jump_time;       // attackjump_time
    bool        q3_hurt;            // health went down since it last looked

    // bot_q3_ai 2: Quake III's bot engine plays it this level (g_q3bot.c),
    // at this Quake III skill
    float       q3_skill;
    bool        q3_engine;
} bot_t;

static bot_t    *bots;              // one per client slot
static bool     bots_pending;       // some active bot is not in this level yet
static bool     roster_built;       // the menu's bots were added this game
static float    bot_human_since;    // level.time someone was first in to play, -1 not yet

static bool     bot_connecting;     // Bot_Spawn is inside ClientConnect
static int      bot_goal_budget;    // goal choices left this frame

#define BOT_GOALS_PER_FRAME 2

static bool Bot_WalkableLine(edict_t *self, const vec3_t to_floor);

static edict_t *Bot_Edict(int i)
{
    return g_edicts + 1 + i;
}

bool Bot_IsBot(const edict_t *ent)
{
    int i;

    if (!bots || !ent)
        return false;

    i = ent - g_edicts - 1;
    if (i < 0 || i >= game.maxclients)
        return false;

    return bots[i].active && bots[i].spawned;
}

static bool Bot_IsHuman(const edict_t *ent)
{
    return ent->inuse && ent->client && ent->client->pers.connected && !Bot_IsBot(ent);
}

static bool Bot_HumanInGame(void)
{
    int i;

    for (i = 0; i < game.maxclients; i++)
        if (Bot_IsHuman(Bot_Edict(i)))
            return true;

    return false;
}

static float Bot_Dist2D(const vec3_t a, const vec3_t b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1];

    return sqrtf(dx * dx + dy * dy);
}

static float Bot_AngleDelta(float a)
{
    a = fmodf(a + 180.0f, 360.0f);
    if (a < 0)
        a += 360.0f;
    return a - 180.0f;
}

static float Bot_Rand(float lo, float hi)
{
    return lo + (hi - lo) * random();
}

static int Bot_Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// vectoangles, without its rounding to whole degrees - a degree is 17 units
// at a thousand, half a player
static void Bot_VecToAngles(const vec3_t v, vec3_t angles)
{
    float flat = sqrtf(v[0] * v[0] + v[1] * v[1]);

    angles[YAW] = (v[0] || v[1]) ? RAD2DEG(atan2f(v[1], v[0])) : 0;
    angles[PITCH] = -RAD2DEG(atan2f(v[2], flat));
    angles[ROLL] = 0;
}

// ==========================================================================
// IMPORTS THAT ADDRESS A CONNECTION
//
// The engine looks a client's connection up by its slot; a bot has none, so
// it warns and drops the message. Skip the warning - and for gi.unicast, still
// clear the message the game wrote, which a NULL entity does silently.

static void (*real_unicast)(edict_t *ent, qboolean reliable);
static void (*real_cprintf)(edict_t *ent, int printlevel, const char *fmt, ...);
static void (*real_centerprintf)(edict_t *ent, const char *fmt, ...);
static void (*real_bprintf)(int printlevel, const char *fmt, ...);

static void Bot_Unicast(edict_t *ent, qboolean reliable)
{
    real_unicast(Bot_IsBot(ent) ? NULL : ent, reliable);
}

static void Bot_Cprintf(edict_t *ent, int printlevel, const char *fmt, ...)
{
    char    msg[MAX_STRING_CHARS];
    va_list argptr;

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    // a line to one of Quake III's own bots is its to read: chat to answer,
    // prints as its server sent them
    if (Bot_IsBot(ent)) {
        BotEngine_Print(ent, printlevel, msg);
        return;
    }

    real_cprintf(ent, printlevel, "%s", msg);
}

// to everyone, and so to Quake III's bots too (CTF's "got the flag" lines)
static void Bot_Bprintf(int printlevel, const char *fmt, ...)
{
    char    msg[MAX_STRING_CHARS];
    va_list argptr;

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    real_bprintf(printlevel, "%s", msg);
    BotEngine_Print(NULL, printlevel, msg);
}

static void Bot_Centerprintf(edict_t *ent, const char *fmt, ...)
{
    char    msg[MAX_STRING_CHARS];
    va_list argptr;

    if (Bot_IsBot(ent))
        return;

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    real_centerprintf(ent, "%s", msg);
}

void Bot_WrapImports(void)
{
    real_unicast = gi.unicast;
    real_cprintf = gi.cprintf;
    real_centerprintf = gi.centerprintf;
    real_bprintf = gi.bprintf;
    gi.unicast = Bot_Unicast;
    gi.cprintf = Bot_Cprintf;
    gi.centerprintf = Bot_Centerprintf;
    gi.bprintf = Bot_Bprintf;
}

// ==========================================================================
// TEAMS
//
// teamplay 1: two teams, red and blue, balanced as people join, scored
// together. Friendly fire: Team_NoFriendlyFire.

// CTF plays in the same two teams
bool G_TeamPlay(void)
{
    return deathmatch->value && ((teamplay && teamplay->value) || G_CTF());
}

/*
Whether teammates' shots leave each other unhurt (knockback still happens).
dmflags' DF_NO_FRIENDLY_FIRE says so anywhere. A bot match also asks
bot_friendly_fire, its menu's "friendly fire", saved and off unless turned on -
Quake III's g_friendlyFire default. Its bots don't look out for a teammate
standing near what they shoot (ioq3 BotCheckAttack: "FIXME: check if a
teammate gets radial damage"), and the row used to write dmflags, which is
never saved, so every launch played with friendly fire on: Matt, 2026-11-19,
"my own team members were attacking and killing me".
*/
bool Team_NoFriendlyFire(void)
{
    if ((int)dmflags->value & DF_NO_FRIENDLY_FIRE)
        return true;
    return deathmatch->value && bot_match && bot_match->integer &&
           bot_friendly_fire && !bot_friendly_fire->integer;
}

const char *Team_Name(int team)
{
    if (team == TEAM_RED)
        return "Red";
    if (team == TEAM_BLUE)
        return "Blue";
    return "None";
}

static int Team_Count(int team, const edict_t *skip)
{
    int i, n = 0;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = Bot_Edict(i);

        if (e == skip || !e->inuse || !e->client || e->client->resp.spectator)
            continue;
        if (e->client->resp.team == team)
            n++;
    }

    return n;
}

int Team_Score(int team)
{
    int i, score = 0;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = Bot_Edict(i);

        if (e->inuse && e->client && e->client->resp.team == team)
            score += e->client->resp.score;
    }

    return score;
}

// a bot match's player: the side Bot_PlanTeams gave them, until they join
static int  bot_player_plan;

// the smaller team, or the one behind on frags, or either
void Team_Assign(edict_t *ent)
{
    int red, blue;

    if (!G_TeamPlay()) {
        ent->client->resp.team = TEAM_NONE;
        return;
    }
    if (ent->client->resp.team == TEAM_RED || ent->client->resp.team == TEAM_BLUE)
        return;

    // the first person into a bot match: the side the menu picked, or the
    // one that evens the teams up with the bots already planned
    if (bot_player_plan && !Bot_IsBot(ent)) {
        ent->client->resp.team = bot_player_plan;
        bot_player_plan = TEAM_NONE;
        return;
    }

    red = Team_Count(TEAM_RED, ent);
    blue = Team_Count(TEAM_BLUE, ent);
    if (red != blue)
        ent->client->resp.team = red < blue ? TEAM_RED : TEAM_BLUE;
    else if (Team_Score(TEAM_RED) != Team_Score(TEAM_BLUE))
        ent->client->resp.team = Team_Score(TEAM_RED) < Team_Score(TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
    else
        ent->client->resp.team = (Q_rand() & 1) ? TEAM_RED : TEAM_BLUE;
}

/*
The team uniforms: ctf_r and ctf_b. The rerelease ships both for all three of
id's models, male, female and cyborg (and their materials, in materialDev);
the classic game has them only with the CTF add-on. Which models have which,
looked up once a game.
*/
#define TEAM_SKIN_MODELS    32

static struct {
    char        model[MAX_QPATH];
    signed char has[2];             // red, blue: -1 not looked yet
} team_skin_cache[TEAM_SKIN_MODELS];
static int  team_skin_cached;

static bool Team_SkinExists(const char *model, char color)
{
    static const char *const exts[] = { "pcx", "png", "tga" };
    int i, c = color == 'r' ? 0 : 1;

    for (i = 0; i < team_skin_cached; i++)
        if (!Q_stricmp(team_skin_cache[i].model, model))
            break;
    if (i == team_skin_cached) {
        if (i == TEAM_SKIN_MODELS)
            i = TEAM_SKIN_MODELS - 1;   // full: the last entry turns over
        else
            team_skin_cached++;
        Q_strlcpy(team_skin_cache[i].model, model, sizeof(team_skin_cache[i].model));
        team_skin_cache[i].has[0] = team_skin_cache[i].has[1] = -1;
    }

    if (team_skin_cache[i].has[c] < 0) {
        int k;

        team_skin_cache[i].has[c] = 0;
        for (k = 0; k < 3 && gi.LoadFile; k++)
            if (gi.LoadFile(va("players/%s/ctf_%c.%s", model, color, exts[k]), NULL) >= 0) {
                team_skin_cache[i].has[c] = 1;
                break;
            }
    }
    return team_skin_cache[i].has[c] > 0;
}

/*
What a player wears in team play: the team's uniform, always - their own
model in it where it has one, or else id's male, female or cyborg in it (a
custom model, or a stock one without the uniforms). Only with no uniform
anywhere do they keep their own skin. Their userinfo keeps the skin they
chose, for when team play ends.
*/
static bool Bot_SkinExistsQ3(const char *skin);

const char *Team_Skin(edict_t *ent, const char *skin)
{
    static const char *const stock[] = { "male", "female", "cyborg" };
    static char out[MAX_QPATH];
    char        model[MAX_QPATH], color;
    const char  *slash;
    size_t      len;
    int         i;

    if (!G_TeamPlay() || !ent->client)
        return skin;
    if (ent->client->resp.team != TEAM_RED && ent->client->resp.team != TEAM_BLUE)
        return skin;
    color = ent->client->resp.team == TEAM_RED ? 'r' : 'b';

    slash = strchr(skin, '/');
    len = slash ? (size_t)(slash - skin) : 0;
    if (len >= sizeof(model))
        len = 0;
    memcpy(model, skin, len);
    model[len] = 0;

    // a Quake III model (baseq3) has its own: red and blue
    if (model[0]) {
        Q_snprintf(out, sizeof(out), "%s/%s", model, color == 'r' ? "red" : "blue");
        if (Bot_SkinExistsQ3(out))
            return out;
    }

    if (!model[0] || !Team_SkinExists(model, color)) {
        for (i = 0; i < 3; i++)
            if (Team_SkinExists(stock[i], color))
                break;
        if (i == 3)
            return skin;
        Q_strlcpy(model, stock[i], sizeof(model));
    }

    Q_snprintf(out, sizeof(out), "%s/ctf_%c", model, color);
    return out;
}

/*
=================
Team_Command

"team" says which team you are on; "team red" / "team blue" switches, which
costs a life like any other way of leaving the fight.
=================
*/
void Team_Command(edict_t *ent)
{
    const char  *arg = gi.argv(1);
    int         team;

    if (!G_TeamPlay()) {
        gi.cprintf(ent, PRINT_HIGH, "Not playing team deathmatch.\n");
        return;
    }

    if (!*arg) {
        gi.cprintf(ent, PRINT_HIGH, "You are on the %s team.\n", Team_Name(ent->client->resp.team));
        return;
    }

    if (!Q_stricmp(arg, "red"))
        team = TEAM_RED;
    else if (!Q_stricmp(arg, "blue"))
        team = TEAM_BLUE;
    else {
        gi.cprintf(ent, PRINT_HIGH, "Usage: team [red|blue]\n");
        return;
    }

    if (team == ent->client->resp.team) {
        gi.cprintf(ent, PRINT_HIGH, "You are already on the %s team.\n", Team_Name(team));
        return;
    }

    ent->client->resp.team = team;
    ent->client->resp.score = 0;
    {
        char userinfo[MAX_INFO_STRING];

        Q_strlcpy(userinfo, ent->client->pers.userinfo, sizeof(userinfo));
        ClientUserinfoChanged(ent, userinfo);   // into the team's skin
    }
    gi.bprintf(PRINT_HIGH, "%s joined the %s team.\n", ent->client->pers.netname, Team_Name(team));

    if (ent->health > 0 && !ent->client->resp.spectator) {
        vec3_t where;

        VectorCopy(ent->s.origin, where);
        ent->flags &= ~FL_GODMODE;
        ent->health = 0;
        meansOfDeath = MOD_SUICIDE;
        player_die(ent, ent, ent, 100000, where);
    }
}

// ==========================================================================
// ROSTER
//
// A bot is named after the skin it wears: male/grunt plays as Grunt. The menu
// rolls its line-up and hands the game the skins (bot_skin1..15); a bot added
// any other way draws from the same set - for now only the skins id shipped
// for male, female and cyborg (Bot_SkinUsable), less any this install lacks.

#define BOT_MAX_SKINS   128

static char     (*skin_pool)[MAX_QPATH];
static int      skin_pool_count;

// a Quake III model's skin (baseq3): models/players/<model>/lower_<skin>.skin,
// with no Quake II model of the name - the client's test (q3player.c)
static bool Bot_SkinExistsQ3(const char *skin)
{
    char        model[MAX_QPATH];
    const char  *slash = strchr(skin, '/');
    size_t      len = slash ? (size_t)(slash - skin) : 0;

    if (!len || len >= sizeof(model) || !gi.LoadFile)
        return false;
    memcpy(model, skin, len);
    model[len] = 0;

    if (gi.LoadFile(va("players/%s/tris.md2", model), NULL) >= 0)
        return false;
    return gi.LoadFile(va("models/players/%s/lower_%s.skin", model, slash + 1), NULL) >= 0;
}

static bool Bot_SkinExists(const char *skin)
{
    static const char *const exts[] = { "pcx", "png", "tga" };
    int i;

    for (i = 0; i < 3; i++)
        if (gi.LoadFile && gi.LoadFile(va("players/%s.%s", skin, exts[i]), NULL) >= 0)
            return true;

    return Bot_SkinExistsQ3(skin);
}

static void Bot_PoolAdd(const char *skin)
{
    int i;

    if (skin_pool_count >= BOT_MAX_SKINS || !strchr(skin, '/'))
        return;
    if (!Bot_SkinUsable(skin) && !Bot_SkinUsableQ3(skin))
        return;
    for (i = 0; i < skin_pool_count; i++)
        if (!Q_stricmp(skin_pool[i], skin))
            return;
    if (!Bot_SkinExists(skin))
        return;
    Q_strlcpy(skin_pool[skin_pool_count++], skin, MAX_QPATH);
}

/*
The skins a random bot wears: id's Quake II ones, Quake III's bots (baseq3,
bot_roster.h) or both, as bot_models says - looked again when it changes.
Quake III's asked for and none installed: Quake II's.
*/
static int  skin_pool_models = -1;

static void Bot_LoadSkins(void)
{
    int models = gi.cvar("bot_models", "2", 0)->integer;
    int i;

    if (skin_pool && models == skin_pool_models)
        return;

    if (!skin_pool)
        skin_pool = gi.TagMalloc(BOT_MAX_SKINS * sizeof(*skin_pool), TAG_GAME);
    skin_pool_count = 0;
    skin_pool_models = models;

    if (models != BOT_MODELS_Q3)
        for (i = 0; i < BOT_CLASSIC_ROSTER_SIZE; i++)
            Bot_PoolAdd(bot_classic_roster[i].skin);
    if (models != BOT_MODELS_Q2)
        for (i = 0; i < BOT_Q3_ROSTER_SIZE; i++)
            Bot_PoolAdd(bot_q3_roster[i].skin);
    if (!skin_pool_count)
        for (i = 0; i < BOT_CLASSIC_ROSTER_SIZE; i++)
            Bot_PoolAdd(bot_classic_roster[i].skin);

    gi.dprintf("Bots: %d skins to wear\n", skin_pool_count);
}

static bool Bot_NameInUse(const char *name)
{
    int i;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = Bot_Edict(i);

        if (bots[i].active && !Q_stricmp(bots[i].name, name))
            return true;
        if (e->inuse && e->client && !Q_stricmp(e->client->pers.netname, name))
            return true;
    }

    return false;
}

// `base`, or `base 2`, `base 3`... - whichever nobody has. Fits a netname.
static void Bot_UniqueName(const char *base, char *out, size_t size)
{
    char    trimmed[13];
    int     n;

    size = min(size, 16);
    Q_strlcpy(out, base, size);
    if (!Bot_NameInUse(out))
        return;

    Q_strlcpy(trimmed, base, sizeof(trimmed));
    for (n = 2; n < 100; n++) {
        Q_snprintf(out, size, "%s %d", trimmed, n);
        if (!Bot_NameInUse(out))
            return;
    }
}

// a skin whose name nobody playing has yet, if there is one
static const char *Bot_RandomSkin(void)
{
    char    name[BOT_MAX_NAME];
    int     i, start, tries;

    if (!skin_pool_count)
        return "male/grunt";

    start = Q_rand() % skin_pool_count;
    for (tries = 0; tries < skin_pool_count; tries++) {
        i = (start + tries) % skin_pool_count;
        Bot_SkinName(skin_pool[i], name, sizeof(name));
        if (!Bot_NameInUse(name))
            return skin_pool[i];
    }

    return skin_pool[start];
}

/*
What `sv addbot` and bot_skinN take: nothing or "random", a skin by the name
it gives ("grunt", "PS9000"), or a model/skin pair, which can be any skin the
install has - a custom model's too.
*/
static const char *Bot_ResolveSkin(const char *want)
{
    char    name[BOT_MAX_NAME];
    int     i;

    if (!want || !*want || !Q_stricmp(want, "random"))
        return Bot_RandomSkin();

    if (strchr(want, '/'))
        return Bot_SkinExists(want) ? want : NULL;

    for (i = 0; i < skin_pool_count; i++) {
        Bot_SkinName(skin_pool[i], name, sizeof(name));
        if (!Q_stricmp(name, want))
            return skin_pool[i];
    }
    return NULL;
}

// ==========================================================================
// ADDING AND REMOVING

static void Bot_ResetBrain(bot_t *b)
{
    memset(b->aware, 0, sizeof(b->aware));
    VectorClear(b->turn);
    b->enemy = NULL;
    b->enemy_visible = false;
    b->heard_at = b->heard_until = 0;
    b->goal = GOAL_NONE;
    b->goal_item = NULL;
    b->route_len = b->route_pos = 0;
    b->next_goal_time = 0;
    b->stuck_count = 0;
    b->stuck_time = level.time + 1;
    b->unstick_until = 0;
    b->strafe = (Q_rand() & 1) ? 1 : -1;
    b->strafe_until = 0;
    b->next_weapon_time = 0;
    b->attack = b->jump = false;
    b->dead = false;
    b->wander_time = 0;
    memset(b->avoid, 0, sizeof(b->avoid));
    b->leash_radius = 0;

    b->crouch = false;
    b->q3_turn[0] = b->q3_turn[1] = 0;
    b->q3_turning = false;
    b->q3_sight_time = 0;
    b->q3_enemypos_time = 0;
    b->q3_throttle_wait = b->q3_throttle_shoot = 0;
    b->q3_strafe_time = 0;
    b->q3_strafe_right = Q_rand() & 1;
    b->q3_crouch_until = b->q3_jump_time = 0;
    b->q3_hurt = false;
}

// the Quake III bot this skin is, if any
static const char *Bot_Q3Character(const char *skin)
{
    int i;

    for (i = 0; i < BOT_Q3_ROSTER_SIZE; i++)
        if (!Q_stricmp(bot_q3_roster[i].skin, skin))
            return bot_q3_roster[i].name;
    return NULL;
}

/*
=================
Bot_SetCharacter

Quake III's bots play as themselves at Quake III skill `q3skill` (1..5): their
character file, Quake III's rules. Under bot_q3_ai 2 everyone else does too,
on Quake III's default character. The rest, and everyone when the data is
not there, play by the rerelease's skill table.
=================
*/
static void Bot_SetCharacter(bot_t *b, float q3skill)
{
    const char *name = Bot_Q3Character(b->skin);

    memset(&b->ch, 0, sizeof(b->ch));
    b->q3_skill = q3skill;
    // 2: Quake II's bots too, on Quake III's default character - what the
    // engine plays them as, and what they keep on a map with no AAS
    if (!bot_q3_ai->integer || (!name && bot_q3_ai->integer < 2))
        return;
    if (BotChar_Load(name, q3skill, &b->ch) && bot_debug->integer)
        gi.dprintf("Bots: %s plays as Quake III's %s, skill %.2f: aim %.2f/%.2f, attack %.2f, view %.2f, "
                   "reaction %.2f, throttle %.2f, walker %.2f\n", b->name, b->ch.name, b->ch.skill,
                   b->ch.c[BCH_AIM_SKILL], b->ch.c[BCH_AIM_ACCURACY], b->ch.c[BCH_ATTACK_SKILL],
                   b->ch.c[BCH_VIEW_FACTOR], b->ch.c[BCH_REACTIONTIME], b->ch.c[BCH_FIRETHROTTLE],
                   b->ch.c[BCH_WALKER]);
}

// the highest client slot nobody holds or is connecting into
static int Bot_FreeSlot(void)
{
    int i;

    for (i = game.maxclients - 1; i >= 0; i--) {
        edict_t *e = Bot_Edict(i);

        if (bots[i].active || e->inuse || game.clients[i].pers.connected)
            continue;
        return i;
    }

    return -1;
}

static bool Bot_Spawn(int i)
{
    bot_t   *b = &bots[i];
    edict_t *ent = Bot_Edict(i);
    char    userinfo[MAX_INFO_STRING];
    char    name[BOT_MAX_NAME];
    char    *p;

    // nothing that would end a userinfo value
    Q_strlcpy(name, b->name, sizeof(name));
    for (p = name; *p; p++)
        if (*p == '\\' || *p == '"' || *p == ';')
            *p = '_';

    userinfo[0] = 0;
    Info_SetValueForKey(userinfo, "name", name);
    Info_SetValueForKey(userinfo, "skin", b->skin);
    Info_SetValueForKey(userinfo, "hand", "2");
    Info_SetValueForKey(userinfo, "fov", "90");
    Info_SetValueForKey(userinfo, "spectator", "0");
    Info_SetValueForKey(userinfo, "ip", "bot");

    ent->inuse = false;
    bot_connecting = true;
    if (!ClientConnect(ent, userinfo)) {
        bot_connecting = false;
        gi.dprintf("Bots: %s was refused: %s\n", b->name, Info_ValueForKey(userinfo, "rejmsg"));
        b->active = false;
        game.clients[i].pers.connected = false;
        return false;
    }
    bot_connecting = false;

    b->spawned = true;
    ent->client->resp.team = b->team;
    ClientBegin(ent);
    b->team = ent->client->resp.team;
    ent->client->ping = 0;

    Bot_ResetBrain(b);
    VectorCopy(ent->client->v_angle, b->view);
    memcpy(b->delta, ent->client->ps.pmove.delta_angles, sizeof(b->delta));
    VectorCopy(ent->s.origin, b->last_origin);

    // Quake III's own bots, where the map has its AAS
    b->q3_engine = false;
    if (bot_q3_ai->integer >= 2) {
        if (BotEngine_Load()) {
            char charfile[MAX_QPATH];

            BotChar_EngineFile(Bot_Q3Character(b->skin), charfile, sizeof(charfile));
            b->q3_engine = BotEngine_Add(ent, charfile, b->q3_skill > 0 ? b->q3_skill : 3, b->name);
            if (!b->q3_engine)
                gi.dprintf("Bots: Quake III's engine couldn't take %s (%s), Quake II's brain plays it\n",
                           b->name, charfile);
            else if (bot_debug->integer)
                gi.dprintf("Bots: %s plays on Quake III's engine as %s, skill %.2f\n",
                           b->name, charfile, b->q3_skill);
        }
    }
    return true;
}

// q3skill: Quake III's skill for its bots' characters, 0 = skill's match
static int Bot_Add(const char *want, int skill, int team, float q3skill)
{
    const char  *skin;
    char        base[BOT_MAX_NAME], name[BOT_MAX_NAME];
    int         i;

    if (!deathmatch->value) {
        gi.cprintf(NULL, PRINT_HIGH, "Bots only play deathmatch.\n");
        return -1;
    }

    Bot_LoadSkins();

    skin = Bot_ResolveSkin(want);
    if (!skin) {
        gi.cprintf(NULL, PRINT_HIGH, "No skin called \"%s\".\n", want);
        return -1;
    }

    i = Bot_FreeSlot();
    if (i < 0) {
        gi.cprintf(NULL, PRINT_HIGH, "No room for another bot (maxclients %d).\n", game.maxclients);
        return -1;
    }

    // named before it joins the bots, or its own name would clash with it
    Bot_SkinName(skin, base, sizeof(base));
    Bot_UniqueName(base, name, sizeof(name));

    memset(&bots[i], 0, sizeof(bots[i]));
    bots[i].active = true;
    bots[i].skill = Bot_Clamp(skill, 0, BOT_NUM_SKILLS - 1);
    bots[i].team = team;
    Q_strlcpy(bots[i].skin, skin, sizeof(bots[i].skin));
    Q_strlcpy(bots[i].name, name, sizeof(bots[i].name));
    Bot_SetCharacter(&bots[i], q3skill > 0 ? q3skill : bot_q3_skill[bots[i].skill]);

    // reserve the slot now: a person connecting meanwhile goes elsewhere
    game.clients[i].pers.connected = true;
    bots_pending = true;
    return i;
}

static void Bot_Remove(int i, const char *why)
{
    bot_t   *b = &bots[i];
    edict_t *ent = Bot_Edict(i);

    if (!b->active)
        return;

    if (b->q3_engine) {
        BotEngine_Remove(ent);
        b->q3_engine = false;
    }

    if (b->spawned && ent->inuse) {
        if (why)
            gi.bprintf(PRINT_HIGH, "%s %s\n", b->name, why);
        ClientDisconnect(ent);
    } else {
        game.clients[i].pers.connected = false;
    }
    gi.configstring(CS_PLAYERSKINS + i, "");

    b->active = false;
    b->spawned = false;
}

// someone connecting needs this slot: the bot makes way
void Bot_ClientConnecting(edict_t *ent)
{
    int i;

    if (!bots || bot_connecting)
        return;

    i = ent - g_edicts - 1;
    if (i < 0 || i >= game.maxclients || !bots[i].active)
        return;

    Bot_Remove(i, "left to make room");
    ent->inuse = false;
}

/*
=================
Bot_Init

InitGame: a new game, so no bots yet. Also registers the cvars the menu sets.
=================
*/
void Bot_Init(void)
{
    int i;

    teamplay = gi.cvar("teamplay", "0", CVAR_SERVERINFO | CVAR_LATCH);
    bot_match = gi.cvar("bot_match", "0", 0);
    bot_count = gi.cvar("bot_count", "3", 0);
    bot_debug = gi.cvar("bot_debug", "0", 0);
    bot_q3_ai = gi.cvar("bot_q3_ai", "1", CVAR_ARCHIVE);
    // the menu's "team leader" (q3bot/game/ai_team.c FindHumanTeamLeader),
    // made here so it is there for the menu before Quake III's AI first runs
    gi.cvar("bot_q3_human_leader", "0", CVAR_ARCHIVE);
    // the menu's "friendly fire" for team bot matches (Team_NoFriendlyFire)
    bot_friendly_fire = gi.cvar("bot_friendly_fire", "0", CVAR_ARCHIVE);

    bots = gi.TagMalloc(game.maxclients * sizeof(*bots), TAG_GAME);
    memset(bots, 0, game.maxclients * sizeof(*bots));
    bots_pending = false;
    roster_built = false;
    bot_human_since = -1;
    skin_pool = NULL;
    skin_pool_count = 0;
    skin_pool_models = -1;
    team_skin_cached = 0;
    bot_player_plan = TEAM_NONE;

    for (i = 0; i < BOT_NUM_WEAPONS; i++) {
        gitem_t *item = FindItemByClassname((char *)bot_weapons[i].classname);
        gitem_t *ammo = (item && item->ammo) ? FindItem(item->ammo) : NULL;

        bot_weapons[i].index = item ? ITEM_INDEX(item) : -1;
        bot_weapons[i].ammo_index = ammo ? ITEM_INDEX(ammo) : -1;
    }
}

/*
Who plays on which side, settled as a bot match starts so the sides come out
even: the menu's picks first (bot_player_team, bot_team1..15: 1 red, 2 blue),
then everyone left on auto - the default - one at a time, in a random order,
onto the smaller side. The player is counted in, though they join after the
bots are added. teams[0] is the player's, teams[1..count] the bots'.
*/
static void Bot_PlanTeams(int count, int *teams, bool player)
{
    int order[BOT_MAX_MATCH + 1];
    int n = 0, red = 0, blue = 0, i, j, t;

    for (i = player ? 0 : 1; i <= count; i++) {
        int pick = i ? gi.cvar(va("bot_team%d", i), "0", 0)->integer
                     : gi.cvar("bot_player_team", "0", 0)->integer;

        if (pick == TEAM_RED || pick == TEAM_BLUE) {
            teams[i] = pick;
            if (pick == TEAM_RED)
                red++;
            else
                blue++;
        } else {
            order[n++] = i;
        }
    }

    for (i = n - 1; i > 0; i--) {
        j = Q_rand() % (i + 1);
        t = order[i];
        order[i] = order[j];
        order[j] = t;
    }

    for (i = 0; i < n; i++) {
        t = red < blue ? TEAM_RED : blue < red ? TEAM_BLUE : (Q_rand() & 1) ? TEAM_RED : TEAM_BLUE;
        teams[order[i]] = t;
        if (t == TEAM_RED)
            red++;
        else
            blue++;
    }
}

/*
=================
Bot_LevelStart

The end of SpawnEntities. The level's edicts are new, so every bot has to
join it again - once someone is here to play against. A bot match adds its
bots the first time through.
=================
*/
void Bot_LevelStart(void)
{
    int i;

    if (!bots)
        return;

    bot_human_since = -1;

    for (i = 0; i < game.maxclients; i++) {
        bots[i].spawned = false;
        bots[i].q3_engine = false;      // SpawnEntities dropped the last map's
        if (bots[i].active && !deathmatch->value)
            bots[i].active = false;
        if (bots[i].active)
            bots_pending = true;
    }

    if (!deathmatch->value)
        return;

    // A Quake III campaign arena: exactly its own bots, in its order, at the
    // campaign's skill - whoever was here before goes (g_sp.c)
    if (SP_Active()) {
        roster_built = true;
        for (i = 0; i < game.maxclients; i++)
            if (bots[i].active)
                Bot_Remove(i, NULL);
        bots_pending = false;

        for (i = 0; i < SP_NumBots(); i++) {
            const char  *skin = SP_BotSkin(i);
            int         slot;

            if (!skin) {
                gi.dprintf("Campaign: no bot of ours for this arena's bot %d\n", i + 1);
                continue;
            }
            slot = Bot_Add(skin, SP_BotSkillLevel(), TEAM_NONE, SP_Skill());
            if (slot < 0) {
                gi.dprintf("Campaign: couldn't add %s\n", skin);
                continue;
            }
            bots[slot].campaign = true;
            bots[slot].join_delay = SP_BotDelay(i);
        }
    }

    if (bot_match->value && !roster_built) {
        int count = Bot_Clamp(bot_count->integer, 0, min(game.maxclients - 1, BOT_MAX_MATCH));
        int teams[BOT_MAX_MATCH + 1] = { TEAM_NONE };
        bool player = !dedicated->value;

        roster_built = true;
        bot_player_plan = TEAM_NONE;
        if (G_TeamPlay()) {
            Bot_PlanTeams(count, teams, player);
            if (player)
                bot_player_plan = teams[0];
        }

        for (i = 1; i <= count; i++) {
            cvar_t  *skin = gi.cvar(va("bot_skin%d", i), "random", 0);
            cvar_t  *skill = gi.cvar(va("bot_skill%d", i), "2", 0);

            // a skin the menu rolled that this game cannot find: any other
            if (Bot_Add(skin->string, skill->integer, teams[i], 0) < 0)
                Bot_Add("random", skill->integer, teams[i], 0);
        }
        if (count && game.maxclients < 2)
            gi.dprintf("Bots: maxclients is 1, so there is no room for bots\n");
    }

    for (i = 0; i < game.maxclients; i++) {
        if (bots[i].active) {
            // map load is the time to spend building a navmesh, not the
            // first frame someone is playing - and loading Quake III's AAS
            Nav_PrepareForBots();
            if (bot_q3_ai->integer >= 2)
                BotEngine_Load();
            break;
        }
    }
}

// ==========================================================================
// SEEING

static void Bot_EyePos(const edict_t *ent, vec3_t out)
{
    VectorCopy(ent->s.origin, out);
    out[2] += ent->viewheight;
}

static bool Bot_ValidTarget(edict_t *self, edict_t *other)
{
    if (other == self || !other->inuse || !other->client)
        return false;
    if (other->health <= 0 || other->deadflag || other->client->resp.spectator)
        return false;
    if (other->movetype == MOVETYPE_NOCLIP || other->solid == SOLID_NOT)
        return false;
    if (OnSameTeam(self, other))
        return false;
    return true;
}

static bool Bot_Invisible(const edict_t *other)
{
    return other->client && other->client->invisible_framenum > level.framenum;
}

// a clear line from our eyes to their eyes or their middle
static bool Bot_LineOfSight(edict_t *self, edict_t *other)
{
    vec3_t  eye, spot;
    trace_t tr;

    Bot_EyePos(self, eye);

    Bot_EyePos(other, spot);
    tr = gi.trace(eye, NULL, NULL, spot, self, MASK_OPAQUE);
    if (tr.fraction == 1.0f || tr.ent == other)
        return true;

    VectorCopy(other->s.origin, spot);
    tr = gi.trace(eye, NULL, NULL, spot, self, MASK_OPAQUE);
    return tr.fraction == 1.0f || tr.ent == other;
}

static bool Bot_InView(edict_t *self, bot_t *b, const vec3_t point, float fov)
{
    vec3_t  forward, dir;

    if (fov >= 360)
        return true;

    AngleVectors(b->view, forward, NULL, NULL);
    VectorSubtract(point, self->s.origin, dir);
    VectorNormalize(dir);
    return DotProduct(forward, dir) >= cosf(DEG2RAD(fov * 0.5f));
}

// a client that just fired, the rerelease's "sound" sense
static bool Bot_Firing(const edict_t *other)
{
    return (other->client->buttons & BUTTON_ATTACK) && other->client->weaponstate == WEAPON_FIRING;
}

// Quake III's InFieldOfVision: the pitch and the yaw each within fov / 2
static bool Bot_Q3InFov(const vec3_t view, float fov, const vec3_t angles)
{
    int i;

    if (fov >= 360)
        return true;
    for (i = PITCH; i <= YAW; i++)
        if (fabsf(Bot_AngleDelta(angles[i] - view[i])) > fov * 0.5f)
            return false;
    return true;
}

/*
=================
Bot_Q3Sees

BotFindEnemy, for a bot with a character. The one it is fighting it follows
all round while in sight (AINode_Battle_Fight). Anyone else: within 900 +
4000 x alertness units, and in a field of view that is all round only with
no fight on and either its health just went down or they are shooting -
otherwise 90 degrees close up, widening to 180 at 810 units. No sight time:
Quake III makes it wait before it shoots instead (Bot_Q3CheckAttack).
=================
*/
static bool Bot_Q3Sees(edict_t *self, bot_t *b, edict_t *other, float dist, bool engaged)
{
    vec3_t  eye, mid, dir, angles;
    float   fov;

    if (other == b->enemy)
        return Bot_LineOfSight(self, other);
    if (Bot_Invisible(other) && !Bot_Firing(other))
        return false;
    if (dist > 900 + BotChar_BFloat(&b->ch, BCH_ALERTNESS, 0, 1) * 4000)
        return false;

    if (!engaged && (b->q3_hurt || Bot_Firing(other)))
        fov = 360;
    else
        fov = 90 + min(dist * dist, 810.0f * 810.0f) / (810 * 9);

    Bot_EyePos(self, eye);
    VectorAdd(other->absmin, other->absmax, mid);
    VectorScale(mid, 0.5f, mid);
    VectorSubtract(mid, eye, dir);
    Bot_VecToAngles(dir, angles);
    return Bot_Q3InFov(b->view, fov, angles) && Bot_LineOfSight(self, other);
}

static void Bot_Perceive(edict_t *self, bot_t *b)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    edict_t             *best = NULL;
    float               best_dist = 0;
    bool                engaged = b->enemy && b->enemy_visible;     // as of last frame
    int                 i;

    b->enemy_visible = false;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *other = Bot_Edict(i);
        float   dist, sight;
        bool    seen;

        if (!Bot_ValidTarget(self, other)) {
            b->aware[i] = 0;
            continue;
        }

        dist = Distance(self->s.origin, other->s.origin);
        sight = sk->sight_time;
        seen = false;

        if (b->ch.loaded) {
            seen = Bot_Q3Sees(self, b, other, dist, engaged);
            sight = 0;              // knows them at once
        } else if (!Bot_Invisible(other) || dist < sk->invis_dist) {
            if (Bot_Invisible(other))
                sight *= sk->invis_scale;
            // it fired at us, or it is in plain view
            if ((other == b->enemy || Bot_InView(self, b, other->s.origin, sk->fov)) &&
                Bot_LineOfSight(self, other))
                seen = true;
        }

        if (seen) {
            b->aware[i] += FRAMETIME / max(sight, 0.01f);
            if (b->aware[i] > 1)
                b->aware[i] = 1;
        } else if (other != b->enemy) {
            b->aware[i] = 0;
        }

        if (other == b->enemy && seen && b->aware[i] >= 1) {
            if (!b->enemy_visible && level.time - b->enemy_seen > 0.5f)
                b->enemy_visible_since = level.time;
            // back in sight after a think or more out of it: Quake III went
            // from chasing back to fighting, a new sighting (BotFindEnemy)
            if (level.time - b->enemy_seen > FRAMETIME * 1.5f)
                b->q3_sight_time = level.time;
            b->enemy_visible = true;
            b->enemy_seen = level.time;
            VectorCopy(other->s.origin, b->enemy_pos);
        }

        // CTF: the flag carrier first, as if four times nearer
        if (seen && b->aware[i] >= 1) {
            float rank = CTF_HasFlag(other) ? dist * 0.25f : dist;

            if (!best || rank < best_dist) {
                best = other;
                best_dist = rank;
            }
        }

        // a shot nearby: look that way, after a moment - unless already in a
        // fight with someone in sight. Quake III's BotFindEnemy turns to a
        // shooter anywhere round only when it has no enemy; a bot duelling
        // another did not feel you come up behind it (Matt, 11-19: "as soon
        // as you shoot the bot sees it from behind its back and moves")
        // (Quake III's bots hear nothing: a shooter in sight is seen all
        // round, Bot_Q3Sees, and one out of sight not at all)
        if (!seen && !b->ch.loaded && (!b->enemy || !b->enemy_visible) &&
            b->heard_until < level.time && dist < sk->sound_range && Bot_Firing(other)) {
            VectorCopy(other->s.origin, b->heard_pos);
            b->heard_at = level.time + sk->sound_time;
            b->heard_until = b->heard_at + 1.5f;
            if (b->aware[i] < 0.5f)
                b->aware[i] = 0.5f;
        }
    }

    // forget an enemy long out of sight
    if (b->enemy && (!Bot_ValidTarget(self, b->enemy) || level.time - b->enemy_seen > sk->forget)) {
        b->enemy = NULL;
        b->enemy_visible = false;
    }

    // a new enemy: nothing on, or what we are fighting is out of sight, or
    // this one is much closer
    if (best && best != b->enemy) {
        float cur = b->enemy ? Distance(self->s.origin, b->enemy->s.origin) : 0;

        if (b->enemy && CTF_HasFlag(b->enemy))
            cur *= 0.25f;

        // (Quake III's take anyone nearer at all)
        if (!b->enemy || !b->enemy_visible || best_dist < cur * (b->ch.loaded ? 1.0f : 0.5f)) {
            // Quake III: no wait to shoot one switched to mid-fight
            b->q3_sight_time = engaged ? level.time - 2 : level.time;
            b->enemy = best;
            b->enemy_visible = true;
            b->enemy_seen = level.time;
            b->enemy_visible_since = level.time;
            b->acquired_time = level.time;
            VectorCopy(best->s.origin, b->enemy_pos);
        }
    }

    b->q3_hurt = false;
}

/*
=================
Bot_Damaged

T_Damage: someone hurt this bot. It knows where they are now, wherever it
was looking (the rerelease's check_six, on at every skill).
=================
*/
void Bot_Damaged(edict_t *self, edict_t *attacker)
{
    bot_t   *b;
    int     i;

    // who hurt whom, for Quake III's bots (theirs or anyone's)
    BotEngine_Damaged(self, attacker);

    if (!Bot_IsBot(self) || !attacker || !Bot_ValidTarget(self, attacker))
        return;

    b = &bots[self - g_edicts - 1];
    if (b->q3_engine)
        return;

    // Quake III's: only that its health went down, which opens its view all
    // round next time it looks - if it has no fight on (Bot_Q3Sees). Hit
    // from behind mid-fight, it fights on.
    if (b->ch.loaded) {
        b->q3_hurt = true;
        return;
    }
    i = attacker - g_edicts - 1;
    if (i >= 0 && i < game.maxclients)
        b->aware[i] = 1;

    if (!b->enemy || !b->enemy_visible) {
        b->enemy = attacker;
        b->enemy_seen = level.time;
        b->acquired_time = level.time;
        VectorCopy(attacker->s.origin, b->enemy_pos);
    }
}

// ==========================================================================
// WEAPONS

static const bot_weapon_t *Bot_CurrentWeapon(const edict_t *ent)
{
    const gitem_t   *w = ent->client->pers.weapon;
    int             i;

    if (!w)
        return NULL;
    for (i = 0; i < BOT_NUM_WEAPONS; i++)
        if (bot_weapons[i].index == ITEM_INDEX(w))
            return &bot_weapons[i];
    return NULL;
}

static bool Bot_HasWeapon(const edict_t *ent, const bot_weapon_t *w)
{
    const int *inv = ent->client->pers.inventory;

    if (w->index < 0 || !inv[w->index])
        return false;
    if (w->ammo_index >= 0 && inv[w->ammo_index] < max(w->min_ammo, 1) &&
        !((int)dmflags->value & DF_INFINITE_AMMO))
        return false;
    return true;
}

static void Bot_ChooseWeapon(edict_t *self, bot_t *b)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    const bot_weapon_t  *best = NULL, *cur;
    float               dist = 512, best_score = -1e9f;
    gitem_t             *item;
    int                 i;

    if (self->client->newweapon)
        return;     // already changing

    cur = Bot_CurrentWeapon(self);
    if (cur && Bot_HasWeapon(self, cur) && level.time < b->next_weapon_time)
        return;

    if (b->enemy)
        dist = Distance(self->s.origin, b->enemy->s.origin);

    for (i = 0; i < BOT_NUM_WEAPONS; i++) {
        const bot_weapon_t  *w = &bot_weapons[i];
        float               score;

        if (!Bot_HasWeapon(self, w))
            continue;
        if (w->melee && !sk->melee)
            continue;

        score = w->priority * 10.0f;
        if (dist < w->min_range)
            score -= w->splash ? 200 : 30;
        else if (dist > w->max_range)
            score -= 60;
        if (w->melee && dist > w->max_range)
            score -= 100;

        // don't flick between near equals
        if (w == cur)
            score += 5;

        if (score > best_score) {
            best_score = score;
            best = w;
        }
    }

    b->next_weapon_time = level.time + 1.0f;

    if (!best || best == cur)
        return;

    item = &itemlist[best->index];
    if (item->use)
        item->use(self, item);
}

// ==========================================================================
// ITEMS

extern bool Pickup_Health(edict_t *ent, edict_t *other);

// health item style bits, as g_items.c defines them
#ifndef HEALTH_IGNORE_MAX
#define HEALTH_IGNORE_MAX   1
#endif

static int Bot_MaxAmmo(const gclient_t *cl, int tag)
{
    switch (tag) {
    case AMMO_BULLETS:      return cl->pers.max_bullets;
    case AMMO_SHELLS:       return cl->pers.max_shells;
    case AMMO_ROCKETS:      return cl->pers.max_rockets;
    case AMMO_GRENADES:     return cl->pers.max_grenades;
    case AMMO_CELLS:        return cl->pers.max_cells;
    case AMMO_SLUGS:        return cl->pers.max_slugs;
    case AMMO_MAGSLUG:      return cl->pers.max_magslug;
    case AMMO_TESLA:        return cl->pers.max_tesla;
    case AMMO_DISRUPTOR:    return cl->pers.max_disruptor;
    case AMMO_TRAP:         return cl->pers.max_trap;
    case AMMO_FLECHETTES:   return cl->pers.max_flechettes;
    case AMMO_PROX:         return cl->pers.max_prox;
    case AMMO_Q3_LIGHTNING: return cl->pers.max_q3_lightning;
    case AMMO_Q3_BFG:       return cl->pers.max_q3_bfg;
    }
    return 100;
}

static bool Bot_Avoiding(bot_t *b, const edict_t *it)
{
    int i;

    for (i = 0; i < BOT_AVOIDS; i++)
        if (b->avoid[i] == it && b->avoid_until[i] > level.time)
            return true;
    return false;
}

static void Bot_Avoid(bot_t *b, edict_t *it)
{
    int i, oldest = 0;

    if (!it)
        return;
    for (i = 1; i < BOT_AVOIDS; i++)
        if (b->avoid_until[i] < b->avoid_until[oldest])
            oldest = i;
    b->avoid[oldest] = it;
    b->avoid_until[oldest] = level.time + 10;
}

static bool Bot_ItemAvailable(const edict_t *it)
{
    return it->inuse && it->item && it->solid == SOLID_TRIGGER && !(it->svflags & SVF_NOCLIENT);
}

// is a person nearer this item than we are?
static bool Bot_HumanCloser(const edict_t *self, const edict_t *it)
{
    float   mine = Distance(self->s.origin, it->s.origin);
    int     i;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = Bot_Edict(i);

        if (Bot_IsHuman(e) && e->health > 0 && Distance(e->s.origin, it->s.origin) < min(mine, 768))
            return true;
    }
    return false;
}

static float Bot_AmmoFraction(const edict_t *self)
{
    const bot_weapon_t  *w = Bot_CurrentWeapon(self);
    const gclient_t     *cl = self->client;
    int                 have, most;

    if (!w || w->ammo_index < 0)
        return 1;
    have = cl->pers.inventory[w->ammo_index];
    most = Bot_MaxAmmo(cl, itemlist[w->ammo_index].tag);
    return most > 0 ? (float)have / most : 1;
}

static float Bot_ArmorFraction(const edict_t *self)
{
    int index = ArmorIndex((edict_t *)self);

    if (!index)
        return 0;
    return min(self->client->pers.inventory[index] / 100.0f, 1.0f);
}

/*
How much this bot wants the item `it`, roughly 0 (no use) to 2 (a powerup).
In a fight only what it is short of counts.
*/
static float Bot_ItemWant(edict_t *self, bot_t *b, edict_t *it, bool fighting)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    const gclient_t     *cl = self->client;
    // a pickup carrying both games' guns: the one this bot would take
    const gitem_t       *item = Q3W_ItemFor(it, self);
    const char          *cn = item->classname;
    float               health = (float)self->health / max(self->max_health, 1);
    int                 i;

    if (item->flags & IT_KEY)
        return 0;

    // CTF: a tech, when it has none; the flags are Bot_CTFGoal's
    if (item->flags & IT_CTF) {
        if (item->pickup != CTFPickup_Tech || fighting || CTF_HasTech(self))
            return 0;
        return 1.0f;
    }

    // health
    if (item->pickup == Pickup_Health) {
        if (it->style & HEALTH_IGNORE_MAX) {
            if (it->count >= 100) {     // megahealth
                if (!sk->power_items || (sk->defer_power && Bot_HumanCloser(self, it)))
                    return 0;
                return 1.6f;
            }
            return 0.15f;               // a stimpack
        }
        if (self->health >= self->max_health)
            return 0;
        if (fighting && health >= sk->min_health)
            return 0;
        return 0.3f + 1.5f * (1 - health) * min(it->count / 25.0f, 1.0f);
    }

    // armour
    if (item->flags & IT_ARMOR) {
        const gitem_armor_t *info = item->info;

        if (fighting && Bot_ArmorFraction(self) >= sk->min_armor)
            return 0;
        if (!info) {    // power screen / shield
            if (cl->pers.inventory[ITEM_INDEX(item)])
                return 0;
            if (!sk->power_items || (sk->defer_power && Bot_HumanCloser(self, it)))
                return 0;
            return 1.2f;
        }
        if (item->tag == ARMOR_SHARD)
            return 0.25f;
        {
            int index = ArmorIndex(self);
            int have = index ? cl->pers.inventory[index] : 0;

            if (have >= info->max_count)
                return 0;
            return 0.3f + (info->max_count - have) / 100.0f;
        }
    }

    // weapons
    if (item->flags & IT_WEAPON) {
        for (i = 0; i < BOT_NUM_WEAPONS; i++) {
            const bot_weapon_t *w = &bot_weapons[i];

            if (w->index != ITEM_INDEX(item))
                continue;
            if (!cl->pers.inventory[w->index])
                return fighting ? 0 : 0.5f + w->priority * 0.15f;
            // weapons stay: we can't take it again
            if (((int)dmflags->value & DF_WEAPONS_STAY) && !(it->spawnflags & DROPPED_ITEM))
                return 0;
            if (w->ammo_index >= 0 && cl->pers.inventory[w->ammo_index] <
                Bot_MaxAmmo(cl, itemlist[w->ammo_index].tag) / 2)
                return 0.4f;
            return 0;
        }
        return 0;
    }

    // ammo, for a weapon we hold
    if (item->flags & IT_AMMO) {
        int most = Bot_MaxAmmo(cl, item->tag);
        int have = cl->pers.inventory[ITEM_INDEX(item)];

        if (have >= most)
            return 0;
        for (i = 0; i < BOT_NUM_WEAPONS; i++) {
            const bot_weapon_t *w = &bot_weapons[i];

            if (w->ammo_index == ITEM_INDEX(item) && w->index >= 0 && cl->pers.inventory[w->index]) {
                float frac = (float)have / max(most, 1);

                if (fighting && frac >= sk->min_ammo)
                    return 0;
                return 0.2f + (1 - frac) * 0.8f;
            }
        }
        return fighting ? 0 : 0.05f;
    }

    if (fighting)
        return 0;

    // powerups
    if (!strcmp(cn, "item_quad") || !strcmp(cn, "item_invulnerability") ||
        !strcmp(cn, "item_double") || !strcmp(cn, "item_quadfire") ||
        !strcmp(cn, "item_invisibility")) {
        if (!sk->power_items || (sk->defer_power && Bot_HumanCloser(self, it)))
            return 0;
        return 2.0f;
    }
    if (!strcmp(cn, "item_adrenaline") || !strcmp(cn, "item_ancient_head") ||
        !strcmp(cn, "item_legacy_head"))
        return 0.8f;
    if (!strcmp(cn, "item_bandolier") || !strcmp(cn, "item_pack"))
        return 0.5f;
    if (!strncmp(cn, "item_sphere_", 12))
        return cl->owned_sphere ? 0 : 0.8f;

    return 0;
}

static void Bot_PlanTo(edict_t *self, bot_t *b, const vec3_t goal)
{
    float length;

    VectorCopy(goal, b->goal_pos);
    b->route_len = Nav_RouteFor(self, goal, b->route, BOT_MAX_ROUTE, &length);
    b->route_pos = 0;
    b->replan_time = level.time + 4;
    b->goal_time = level.time;

    // start from the second node if we are already past the first
    if (b->route_len > 1) {
        vec3_t p;

        if (Nav_NodeOrigin(b->route[1], p) && Bot_WalkableLine(self, p))
            b->route_pos = 1;
    }
}

/*
=================
Bot_PickGoal

The best thing to go and get - what it is worth over how far the walk is - or
somewhere to wander to.
=================
*/
#define BOT_CANDIDATES  6

static void Bot_PickGoal(edict_t *self, bot_t *b, bool fighting)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    edict_t             *cand[BOT_CANDIDATES];
    float               cscore[BOT_CANDIDATES], cwant[BOT_CANDIDATES];
    int                 ncand = 0, i, j;
    edict_t             *best = NULL;
    float               best_score = 0;
    int                 route[BOT_MAX_ROUTE];

    b->next_goal_time = level.time + Bot_Rand(2.0f, 3.0f);

    if (sk->items || fighting) {
        for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
            edict_t *it = g_edicts + i;
            float   want, dist, score;

            if (!Bot_ItemAvailable(it) || Bot_Avoiding(b, it))
                continue;
            if (b->leash_radius > 0 && Distance(it->s.origin, b->leash) > b->leash_radius)
                continue;

            dist = Distance(self->s.origin, it->s.origin);
            if (fighting && dist > 560)     // combat.max_item_dist
                continue;
            if (dist > 4000)
                continue;

            want = Bot_ItemWant(self, b, it, fighting);
            if (want <= 0)
                continue;

            score = want / (dist + 128);
            if (ncand == BOT_CANDIDATES && score <= cscore[ncand - 1])
                continue;
            if (ncand < BOT_CANDIDATES)
                ncand++;
            for (j = ncand - 1; j > 0 && cscore[j - 1] < score; j--) {
                cand[j] = cand[j - 1];
                cscore[j] = cscore[j - 1];
                cwant[j] = cwant[j - 1];
            }
            cand[j] = it;
            cscore[j] = score;
            cwant[j] = want;
        }

        // now by the length of the walk
        for (i = 0; i < ncand; i++) {
            float   length, score;
            int     n = Nav_RouteFor(self, cand[i]->s.origin, route, BOT_MAX_ROUTE, &length);

            if (!n) {
                if (Nav_NumNodes() > 0 && !Bot_LineOfSight(self, cand[i]))
                    continue;   // no way there
                length = Distance(self->s.origin, cand[i]->s.origin);
            }
            if (fighting && length > 560)
                continue;

            score = cwant[i] / (length + 128);
            if (score > best_score) {
                best_score = score;
                best = cand[i];
            }
        }
    }

    // keep going for what we were after unless this is clearly better
    if (b->goal == GOAL_ITEM && b->goal_item && Bot_ItemAvailable(b->goal_item) &&
        best && best != b->goal_item) {
        float stay = Bot_ItemWant(self, b, b->goal_item, fighting) /
                     (Distance(self->s.origin, b->goal_item->s.origin) + 128);

        if (stay * 1.3f >= best_score)
            return;
    }

    if (best) {
        if (best != b->goal_item || b->goal != GOAL_ITEM) {
            b->goal = GOAL_ITEM;
            b->goal_item = best;
            Bot_PlanTo(self, b, best->s.origin);
        }
        return;
    }

    if (fighting)
        return;

    // nothing worth having: wander to an item or a spawn point somewhere else
    if (b->goal == GOAL_ROAM && b->route_pos < b->route_len)
        return;

    for (j = 0; j < 16; j++) {
        edict_t *spot = g_edicts + game.maxclients + 1 + Q_rand() % max(globals.num_edicts - game.maxclients - 1, 1);

        if (!spot->inuse || (!spot->item && (!spot->classname ||
            Q_strncasecmp(spot->classname, "info_player_", 12))))
            continue;
        if (Distance(spot->s.origin, self->s.origin) < 384)
            continue;
        if (b->leash_radius > 0 && Distance(spot->s.origin, b->leash) > b->leash_radius)
            continue;

        b->goal = GOAL_ROAM;
        b->goal_item = NULL;
        Bot_PlanTo(self, b, spot->s.origin);
        if (b->route_len || !Nav_NumNodes())
            return;
    }
}

// ==========================================================================
// CAPTURE THE FLAG
//
// What the team needs of a bot comes before what it wants for itself. Its
// slot among its team's bots gives it a job: every third one (the second,
// the fifth...) defends, the rest attack. Whatever the job, a bot carrying
// their flag runs it home, one near our dropped flag returns it, and when
// they have our flag the defenders - and any attacker close by - go after the
// carrier. Bots know where the flags and carriers are, as a player does from
// the HUD and the announcements; they still have to see a carrier to shoot.

static bool Bot_CTFDefender(const edict_t *self)
{
    int i, rank = 0;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = Bot_Edict(i);

        if (e == self)
            break;
        if (Bot_IsBot(e) && e->client->resp.team == self->client->resp.team)
            rank++;
    }
    return rank % 3 == 1;
}

// something better than the blaster, with the ammo to fire it
static bool Bot_Armed(const edict_t *self)
{
    const gclient_t *cl = self->client;
    int             i;

    for (i = 0; i < BOT_NUM_WEAPONS; i++) {
        const bot_weapon_t *w = &bot_weapons[i];

        if (w->priority < 2 || w->index < 0 || !cl->pers.inventory[w->index])
            continue;
        if (w->ammo_index >= 0 && cl->pers.inventory[w->ammo_index] < max(w->min_ammo, 1))
            continue;
        return true;
    }
    return false;
}

/*
Head for a flag (an item: touching it is the point) or a place. Arrived at a
place, there is nothing more to do there: false, and the bot does what it
otherwise would, inside its leash.
*/
static bool Bot_CTFObjective(edict_t *self, bot_t *b, edict_t *item, const vec3_t pos)
{
    b->next_goal_time = level.time + 1;

    if (item) {
        if (b->goal != GOAL_ITEM || b->goal_item != item) {
            b->goal = GOAL_ITEM;
            b->goal_item = item;
            Bot_PlanTo(self, b, item->s.origin);
        }
        return true;
    }

    if (Bot_Dist2D(self->s.origin, pos) < 96 && fabsf(self->s.origin[2] - pos[2]) < 64)
        return false;

    if (b->goal != GOAL_CTF || Distance(b->goal_pos, pos) > 128) {
        b->goal = GOAL_CTF;
        b->goal_item = NULL;
        Bot_PlanTo(self, b, pos);
    }
    return true;
}

static bool Bot_CTFGoal(edict_t *self, bot_t *b, bool fighting)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    int                 team = self->client->resp.team, enemy;
    edict_t             *our_stand, *our_dropped, *thief;
    edict_t             *their_stand, *their_dropped, *runner;
    bool                defender;

    b->leash_radius = 0;
    if (team != TEAM_RED && team != TEAM_BLUE)
        return false;
    enemy = team == TEAM_RED ? TEAM_BLUE : TEAM_RED;

    our_stand = CTF_BaseFlag(team);         // our flag's stand, flag there or not
    our_dropped = CTF_DroppedFlag(team);    // our flag, lying out there
    thief = CTF_Carrier(team);              // one of them, with our flag
    their_stand = CTF_BaseFlag(enemy);
    their_dropped = CTF_DroppedFlag(enemy);
    runner = CTF_Carrier(enemy);            // one of us, with theirs

    // carrying their flag: home. Touching ours on its stand scores; with it
    // gone, wait by the stand for the team to win it back
    if (runner == self) {
        if (!our_stand)
            return false;
        if (Bot_ItemAvailable(our_stand))
            return Bot_CTFObjective(self, b, our_stand, NULL);
        VectorCopy(our_stand->s.origin, b->leash);
        b->leash_radius = 512;
        return Bot_CTFObjective(self, b, NULL, our_stand->s.origin);
    }

    defender = Bot_CTFDefender(self);

    // ours, dropped: whoever is near takes it back
    if (our_dropped && (defender || Distance(self->s.origin, our_dropped->s.origin) < 1200))
        return Bot_CTFObjective(self, b, our_dropped, NULL);

    // they have it: after the carrier
    if (thief && (defender || Distance(self->s.origin, thief->s.origin) < 1500))
        return Bot_CTFObjective(self, b, NULL, thief->s.origin);

    if (defender) {
        if (!our_stand)
            return false;
        VectorCopy(our_stand->s.origin, b->leash);
        b->leash_radius = 768;
        if (Distance(self->s.origin, our_stand->s.origin) > b->leash_radius)
            return Bot_CTFObjective(self, b, NULL, our_stand->s.origin);
        return false;   // close to home: fights, and what is lying about
    }

    // attack - their flag lying close by first, at any cost
    if (their_dropped && Distance(self->s.origin, their_dropped->s.origin) < 800)
        return Bot_CTFObjective(self, b, their_dropped, NULL);

    // not with a blaster and a scratch: a gun and some health first
    if (!fighting && sk->items &&
        (!Bot_Armed(self) || self->health < self->max_health * 0.35f))
        return false;

    if (their_dropped)
        return Bot_CTFObjective(self, b, their_dropped, NULL);
    if (their_stand && Bot_ItemAvailable(their_stand))
        return Bot_CTFObjective(self, b, their_stand, NULL);

    // one of us has it: run with them
    if (runner)
        return Bot_CTFObjective(self, b, NULL, runner->s.origin);
    return false;
}

// ==========================================================================
// MOVING

// stand at this floor point? not over a pit, not in lava or slime
static bool Bot_SafeFloor(edict_t *self, const vec3_t at)
{
    vec3_t  start, end;
    trace_t tr;

    VectorSet(start, at[0], at[1], self->s.origin[2]);
    VectorSet(end, at[0], at[1], self->s.origin[2] + self->mins[2] - 72);
    tr = gi.trace(start, NULL, NULL, end, self, MASK_PLAYERSOLID | MASK_WATER);
    if (tr.fraction == 1.0f)
        return false;
    if (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME))
        return false;
    return true;
}

// straight there on foot: nothing in the way and floor all along it
static bool Bot_WalkableLine(edict_t *self, const vec3_t to_floor)
{
    vec3_t  target, step;
    float   len;
    int     i, n;

    if (fabsf(to_floor[2] - (self->s.origin[2] + self->mins[2])) > 20)
        return false;
    if (!Nav_BodyCanReach(self, to_floor))
        return false;

    VectorSet(target, to_floor[0], to_floor[1], self->s.origin[2]);
    VectorSubtract(target, self->s.origin, step);
    len = VectorNormalize(step);
    n = (int)(len / 32);
    for (i = 1; i <= n; i++) {
        vec3_t p;

        VectorMA(self->s.origin, i * 32.0f, step, p);
        if (!Bot_SafeFloor(self, p))
            return false;
    }
    return true;
}

static bool Bot_Arrived(const edict_t *self, const vec3_t floor_point, float radius)
{
    float feet = self->s.origin[2] + self->mins[2];
    float dz = floor_point[2] - feet;

    if (dz > 40 || dz < -64)
        return false;
    return Bot_Dist2D(self->s.origin, floor_point) <= radius;
}

static void Bot_Steer(const edict_t *self, const vec3_t to, vec3_t wish)
{
    VectorSubtract(to, self->s.origin, wish);
    wish[2] = 0;
    VectorNormalize(wish);
}

// the plat, door or train standing over or under this spot
static edict_t *Bot_LiftAt(const vec3_t point)
{
    int i;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || e->solid != SOLID_BSP || e->movetype != MOVETYPE_PUSH)
            continue;
        if (point[0] < e->absmin[0] - 16 || point[0] > e->absmax[0] + 16 ||
            point[1] < e->absmin[1] - 16 || point[1] > e->absmax[1] + 16)
            continue;
        return e;
    }
    return NULL;
}

/*
=================
Bot_PadFlight

In the air off a Quake III jump pad (g_q3.c), with b->pad_landing where it
comes down. The flight is the pad's: the bot keeps its route and does not
replan from mid-air, fight-strafe or dodge, and only leans toward the
landing - Quake II's bit of air control spent steering back at the pad (the
route's step still says "walk onto it") would cut the jump short.
=================
*/
#define BOT_PAD_FLIGHT_FRAMES   80  // 8 seconds: longer is not a pad's doing

static bool Bot_PadFlight(edict_t *self, bot_t *b)
{
    gclient_t   *client = self->client;
    edict_t     *pad = client->q3_jumppad;

    if (!pad || self->groundentity || self->waterlevel >= 2 || self->movetype == MOVETYPE_NOCLIP)
        return false;
    if (level.framenum - client->q3_jumppad_framenum > BOT_PAD_FLIGHT_FRAMES)
        return false;

    if (b->pad != pad) {
        if (!Q3_JumpPadLanding(pad, b->pad_landing))
            return false;
        b->pad = pad;
    }
    return true;
}

/*
=================
Bot_FollowRoute

Where to go next along the route: sets `wish` (a flat direction, or zero to
stand still) and asks for a jump or a climb. False when the route is used up.
=================
*/
static bool Bot_FollowRoute(edict_t *self, bot_t *b, vec3_t wish, bool *jump, bool *climb)
{
    float radius = Nav_IsGenerated() ? 20 : 32;

    b->waiting = false;

    // a jump pad has us: toward where it puts us down
    if (Bot_PadFlight(self, b)) {
        if (Bot_Dist2D(self->s.origin, b->pad_landing) > 16)
            Bot_Steer(self, b->pad_landing, wish);
        else
            VectorClear(wish);
        return true;
    }

    while (b->route_pos < b->route_len) {
        int         to = b->route[b->route_pos];
        int         from = b->route_pos > 0 ? b->route[b->route_pos - 1] : -1;
        vec3_t      start, end;
        nav_step_t  kind;
        float       feet = self->s.origin[2] + self->mins[2];

        if (from >= 0) {
            kind = Nav_StepKind(from, to, start, end);
        } else {
            kind = NAV_STEP_WALK;
            Nav_NodeOrigin(to, end);
            VectorCopy(end, start);
        }

        switch (kind) {
        case NAV_STEP_TELEPORT:
            // the pad sends us on; Bot_Think notices the jump and replans
            Bot_Steer(self, start, wish);
            return true;

        case NAV_STEP_JUMP:
            if (Bot_Arrived(self, end, radius)) {
                b->route_pos++;
                continue;
            }
            // to the take-off first, if we are on its floor and short of it
            if (self->groundentity && fabsf(feet - start[2]) < 24 &&
                Bot_Dist2D(self->s.origin, start) > 24 &&
                Bot_Dist2D(self->s.origin, end) > Bot_Dist2D(start, end)) {
                Bot_Steer(self, start, wish);
                return true;
            }
            Bot_Steer(self, end, wish);
            if (self->groundentity && (Bot_Dist2D(self->s.origin, start) < 32 ||
                                       Bot_Dist2D(self->s.origin, end) < 64))
                *jump = true;
            return true;

        case NAV_STEP_ELEVATOR: {
            edict_t *lift = Bot_LiftAt(start);

            if (feet >= end[2] - 20) {
                if (Bot_Arrived(self, end, radius)) {
                    b->route_pos++;
                    continue;
                }
                Bot_Steer(self, end, wish);
                return true;
            }

            if (!lift) {
                // a ladder: into it and up
                Bot_Steer(self, end, wish);
                *climb = true;
                return true;
            }

            if (self->groundentity == lift) {
                // riding: stay in the middle
                if (Bot_Dist2D(self->s.origin, start) > 16)
                    Bot_Steer(self, start, wish);
                else
                    VectorClear(wish);
                b->waiting = true;
                return true;
            }

            // up, or on its way: wait beside it rather than under it, where
            // standing would hold it up there
            if (lift->absmax[2] > feet + 24) {
                if (Bot_Dist2D(self->s.origin, start) > 96)
                    Bot_Steer(self, start, wish);
                else
                    VectorClear(wish);
                b->waiting = true;
                return true;
            }

            Bot_Steer(self, start, wish);
            return true;
        }

        case NAV_STEP_DROP:
        case NAV_STEP_WALK:
        default:
            if (Bot_Arrived(self, end, radius)) {
                b->route_pos++;
                continue;
            }
            Bot_Steer(self, end, wish);
            // in water, swim for the node
            if (self->waterlevel >= 2 && end[2] > feet + 8)
                *climb = true;
            return true;
        }
    }

    return false;
}

// cut the corner: on to a later node that is in a straight walk from here
static void Bot_Shortcut(edict_t *self, bot_t *b)
{
    int k, last;

    if (!self->groundentity || b->route_pos >= b->route_len - 1)
        return;
    if (level.time < b->next_shortcut_time)
        return;
    b->next_shortcut_time = level.time + 0.3f;

    // only along plain floor
    last = b->route_pos;
    for (k = b->route_pos + 1; k < b->route_len && k <= b->route_pos + 6; k++) {
        vec3_t s, e;

        if (Nav_StepKind(b->route[k - 1], b->route[k], s, e) != NAV_STEP_WALK)
            break;
        last = k;
    }

    for (k = last; k > b->route_pos; k--) {
        vec3_t p;

        Nav_NodeOrigin(b->route[k], p);
        if (Bot_WalkableLine(self, p)) {
            b->route_pos = k;
            return;
        }
    }
}

static bool Bot_GoalReached(edict_t *self, bot_t *b)
{
    if (b->goal == GOAL_ITEM)
        return !b->goal_item || !Bot_ItemAvailable(b->goal_item);
    if (b->goal == GOAL_NONE)
        return true;
    // somewhere to wander or someone to chase: the end of the route will do
    if (b->route_len && b->route_pos >= b->route_len)
        return true;
    return Bot_Dist2D(self->s.origin, b->goal_pos) < 32 &&
           fabsf(self->s.origin[2] - b->goal_pos[2]) < 64;
}

// ==========================================================================
// DANGER

/*
Only what the bot can see: a grenade or rocket in its field of view and in
line of sight. It dodged everything on course for it, behind it included, so a
rocket from behind never landed (Matt, 11-19). Quake III's own bots dodge no
missiles at all; its missiles (q3_missile, g_q3weap.c) are not looked at here.
*/
static bool Bot_SeesDanger(edict_t *self, bot_t *b, edict_t *e)
{
    return Bot_InView(self, b, e->s.origin, bot_skills[b->skill].fov) && Bot_LineOfSight(self, e);
}

// a grenade lying or bouncing about, Quake II's or Quake III's
static bool Bot_IsGrenade(const edict_t *e)
{
    return !strcmp(e->classname, "grenade") || !strcmp(e->classname, "hgrenade") ||
           (!strcmp(e->classname, "q3_missile") && e->style == Q3W_GRENADE);
}

static void Bot_Dangers(edict_t *self, bot_t *b)
{
    int i;

    VectorClear(b->dodge);
    if (!bot_skills[b->skill].dangers && !b->ch.loaded)
        return;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        edict_t *e = g_edicts + i;
        vec3_t  rel;
        float   dist;

        if (!e->inuse || !e->classname)
            continue;
        if (b->ch.loaded) {
            // Quake III's: grenades, wherever they are (BotCheckForGrenades
            // steers round every one in its snapshot); never a rocket
            if (!Bot_IsGrenade(e))
                continue;
        } else {
            if (!Bot_IsGrenade(e) && strcmp(e->classname, "rocket") && strcmp(e->classname, "bfg blast"))
                continue;
            if (!Bot_SeesDanger(self, b, e))
                continue;
        }

        if (Bot_IsGrenade(e)) {
            VectorSubtract(self->s.origin, e->s.origin, rel);
            rel[2] = 0;
            dist = VectorNormalize(rel);
            if (dist < max(e->dmg_radius, 120) + 48)
                VectorMA(b->dodge, 1, rel, b->dodge);
            continue;
        }

        if ((!strcmp(e->classname, "rocket") || !strcmp(e->classname, "bfg blast")) && e->owner != self) {
            float   speed2 = DotProduct(e->velocity, e->velocity);
            float   t;
            vec3_t  closest, side;

            if (speed2 < 1)
                continue;
            VectorSubtract(self->s.origin, e->s.origin, rel);
            t = DotProduct(rel, e->velocity) / speed2;
            if (t < 0 || t > 0.8f)
                continue;
            VectorMA(e->s.origin, t, e->velocity, closest);
            VectorSubtract(self->s.origin, closest, side);
            side[2] = 0;
            dist = VectorNormalize(side);
            if (dist > 96)
                continue;
            if (dist < 1) {     // dead on: pick a side
                VectorSet(side, -e->velocity[1], e->velocity[0], 0);
                VectorNormalize(side);
            }
            VectorMA(b->dodge, 1, side, b->dodge);
        }
    }

    VectorNormalize(b->dodge);
}

// ==========================================================================
// AIMING

/*
A grenade: the pitch whose arc comes down on target. fire_grenade launches at
600 along the view plus 200 along the view's up, which is ~632 units/s at
18.4 degrees above where we look. False, and *pitch untouched, out of reach.
*/
static bool Bot_LobPitch(const vec3_t eye, const bot_weapon_t *w, const vec3_t target, float *pitch)
{
    float   g = sv_gravity->value > 0 ? sv_gravity->value : 800;
    float   dx = Bot_Dist2D(eye, target);
    float   dz = target[2] - eye[2];
    float   rise = w->lob_rise ? w->lob_rise : 200;
    float   v = sqrtf(w->speed * w->speed + rise * rise);
    float   v2 = v * v;
    float   root = v2 * v2 - g * (g * dx * dx + 2 * dz * v2);
    float   elevation;

    if (root < 0 || dx <= 1)
        return false;
    elevation = RAD2DEG(atanf((v2 - sqrtf(root)) / (g * dx)));
    *pitch = -(elevation - RAD2DEG(atanf(rise / w->speed)));
    return true;
}

static void Bot_AimPoint(edict_t *self, bot_t *b, const bot_weapon_t *w, vec3_t out, float *lob_pitch)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    edict_t             *e = b->enemy;
    vec3_t              eye, target;
    float               speed = w ? w->speed : 0;
    int                 aim = w ? w->aim : AIM_CENTER;
    int                 i;

    *lob_pitch = 999;
    Bot_EyePos(self, eye);

    VectorCopy(e->s.origin, target);
    if (aim == AIM_HEAD)
        target[2] += e->viewheight - 4;
    else if (aim == AIM_FEET && e->groundentity)
        target[2] += e->mins[2] + 4;

    if (sk->lead && speed > 0) {
        vec3_t lead;

        VectorCopy(target, lead);
        for (i = 0; i < 2; i++) {
            float t = Distance(eye, lead) / speed;

            VectorMA(target, t, e->velocity, lead);
            if (e->groundentity)
                lead[2] = target[2];
        }
        VectorCopy(lead, target);
    } else {
        VectorMA(target, sk->lag, e->velocity, target);
    }

    if (w && w->lob)
        Bot_LobPitch(eye, w, target, lob_pitch);

    VectorCopy(target, out);
}

static void Bot_TurnToward(bot_t *b, const vec3_t want, float dt)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    int                 i;

    for (i = PITCH; i <= YAW; i++) {
        float err = Bot_AngleDelta(want[i] - b->view[i]);
        float acc = sk->spring * err - sk->damping * b->turn[i];

        b->turn[i] += acc * dt;
        if (b->turn[i] > sk->turn_speed)
            b->turn[i] = sk->turn_speed;
        else if (b->turn[i] < -sk->turn_speed)
            b->turn[i] = -sk->turn_speed;
        b->view[i] = Bot_AngleDelta(b->view[i] + b->turn[i] * dt);
    }

    if (b->view[PITCH] > 89)
        b->view[PITCH] = 89;
    else if (b->view[PITCH] < -89)
        b->view[PITCH] = -89;
    b->view[ROLL] = 0;
}

static float Bot_AimError(edict_t *self, bot_t *b, const vec3_t point)
{
    vec3_t  forward, dir, eye;
    float   d;

    Bot_EyePos(self, eye);
    AngleVectors(b->view, forward, NULL, NULL);
    VectorSubtract(point, eye, dir);
    VectorNormalize(dir);
    d = DotProduct(forward, dir);
    return RAD2DEG(acosf(max(-1.0f, min(1.0f, d))));
}

// a team-mate in the line of fire?
static bool Bot_FriendInWay(edict_t *self, const vec3_t point)
{
    vec3_t  eye;
    trace_t tr;

    if (!G_TeamPlay())
        return false;

    Bot_EyePos(self, eye);
    tr = gi.trace(eye, NULL, NULL, point, self, MASK_SHOT);
    return tr.ent && tr.ent->client && tr.ent != self && OnSameTeam(self, tr.ent);
}

// ==========================================================================
// QUAKE III'S RULES
//
// For a bot with a character (g_botchar.c): ioq3's ai_dmq3.c BotAimAtEnemy,
// BotCheckAttack and BotAttackMove, run once a think as Quake III ran them
// (bot_thinktime 100 ms, our frame), and ai_main.c's BotChangeViewAngles at
// Quake III's 20 Hz. What needs its AAS - prediction round corners, the move
// predictor - is done with traces; the rest is line for line.

#define BOT_Q3_ATTACKDIST   140     // IDEAL_ATTACKDIST

// which of Quake III's guns this is, or plays most like (Q3W_*)
static int Bot_Q3Weapon(const bot_weapon_t *w)
{
    const char *c;

    if (!w)
        return Q3W_NONE;
    if (w->melee)
        return Q3W_GAUNTLET;
    c = w->classname;
    if (strstr(c, "machinegun") || strstr(c, "chaingun"))
        return Q3W_MACHINEGUN;
    if (strstr(c, "shotgun"))
        return Q3W_SHOTGUN;
    if (strstr(c, "grenade") || strstr(c, "prox"))
        return Q3W_GRENADE;
    if (strstr(c, "rocket") || strstr(c, "phalanx"))
        return Q3W_ROCKET;
    if (strstr(c, "lightning") || strstr(c, "plasmabeam"))
        return Q3W_LIGHTNING;
    if (strstr(c, "railgun") || strstr(c, "disintegrator"))
        return Q3W_RAILGUN;
    if (strstr(c, "bfg"))
        return Q3W_BFG;
    return Q3W_PLASMA;      // blasters, the plasma gun, ETF rifle, ionripper
}

static bool Bot_Q3Hitscan(int q3w)
{
    return q3w == Q3W_MACHINEGUN || q3w == Q3W_SHOTGUN || q3w == Q3W_LIGHTNING || q3w == Q3W_RAILGUN;
}

// botfiles/weapons.c: the splash a gun's shot does, if it does
static bool Bot_Q3Radial(int q3w, float *damage, float *radius)
{
    switch (q3w) {
    case Q3W_GRENADE:   *damage = 120; *radius = 160; return true;
    case Q3W_ROCKET:    *damage = 100; *radius = 120; return true;
    case Q3W_PLASMA:    *damage = 20;  *radius = 20;  return true;
    case Q3W_BFG:       *damage = 40;  *radius = 100; return true;
    default:            return false;
    }
}

// the gun's own aim accuracy, and for the slow shots its own aim skill
static void Bot_Q3AimSkill(const bot_t *b, int q3w, float *skill, float *acc)
{
    const bot_character_t *ch = &b->ch;

    switch (q3w) {
    case Q3W_MACHINEGUN:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_MACHINEGUN, 0, 1);
        break;
    case Q3W_SHOTGUN:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_SHOTGUN, 0, 1);
        break;
    case Q3W_GRENADE:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_GRENADELAUNCHER, 0, 1);
        *skill = BotChar_BFloat(ch, BCH_AIM_SKILL_GRENADELAUNCHER, 0, 1);
        break;
    case Q3W_ROCKET:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_ROCKETLAUNCHER, 0, 1);
        *skill = BotChar_BFloat(ch, BCH_AIM_SKILL_ROCKETLAUNCHER, 0, 1);
        break;
    case Q3W_LIGHTNING:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_LIGHTNING, 0, 1);
        break;
    case Q3W_RAILGUN:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_RAILGUN, 0, 1);
        break;
    case Q3W_PLASMA:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_PLASMAGUN, 0, 1);
        *skill = BotChar_BFloat(ch, BCH_AIM_SKILL_PLASMAGUN, 0, 1);
        break;
    case Q3W_BFG:
        *acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY_BFG10K, 0, 1);
        *skill = BotChar_BFloat(ch, BCH_AIM_SKILL_BFG10K, 0, 1);
        break;
    default:
        break;
    }
}

// AAS_PredictClientMovement, near enough: where e will be when a shot at
// `speed` gets there - coasting, falling if in the air, stopped by walls
static void Bot_Q3Predict(edict_t *self, edict_t *e, float speed, vec3_t out)
{
    float   t = Distance(e->s.origin, self->s.origin) / speed;
    vec3_t  end;
    trace_t tr;

    VectorMA(e->s.origin, t, e->velocity, end);
    if (e->groundentity)
        end[2] = e->s.origin[2];
    else
        end[2] -= 0.5f * sv_gravity->value * t * t;
    tr = gi.trace(e->s.origin, e->mins, e->maxs, end, e, MASK_PLAYERSOLID);
    VectorCopy(tr.endpos, out);
}

/*
=================
Bot_Q3Aim

BotAimAtEnemy: sets q3_ideal, where the view wants to be, and the point the
shot is meant for. Its misses are the point - the target point jittered by
up to 20 units and the direction by up to 0.3 a component, both scaled by
(1 - accuracy); accuracy down by 0.7 when the enemy has turned about, and for
hitscan guns up close; a lead only from aim skill 0.4, an exact one from 0.8,
splash aimed at the feet from 0.6. False while a very skilled bot holds off,
half its reaction time.
=================
*/
static bool Bot_Q3Aim(edict_t *self, bot_t *b, const bot_weapon_t *w, vec3_t aimtarget, float *lob_pitch)
{
    static const vec3_t mins4 = { -4, -4, -4 }, maxs4 = { 4, 4, 4 };
    const bot_character_t   *ch = &b->ch;
    edict_t                 *e = b->enemy;
    int                     q3w = Bot_Q3Weapon(w), i;
    float                   aim_skill, acc, dist, damage, radius;
    float                   speed = w ? w->speed : 0;
    vec3_t                  eye, best, dir, vel, clean;
    trace_t                 tr;

    *lob_pitch = 999;
    Bot_EyePos(self, eye);

    aim_skill = BotChar_BFloat(ch, BCH_AIM_SKILL, 0, 1);
    acc = BotChar_BFloat(ch, BCH_AIM_ACCURACY, 0, 1);
    if (aim_skill > 0.95f) {
        float reaction = 0.5f * BotChar_BFloat(ch, BCH_REACTIONTIME, 0, 1);

        if (b->q3_sight_time > level.time - reaction)
            return false;
    }
    Bot_Q3AimSkill(b, q3w, &aim_skill, &acc);
    if (acc <= 0)
        acc = 0.0001f;
    if (Bot_Invisible(e) && random() > 0.1f)
        acc *= 0.4f;

    // where it was and how it moved, every half second
    VectorCopy(e->velocity, vel);
    if (b->q3_enemypos_time < level.time) {
        b->q3_enemypos_time = level.time + 0.5f;
        VectorCopy(vel, b->q3_enemy_vel);
        VectorCopy(e->s.origin, b->q3_enemy_org);
    }
    if (aim_skill < 0.9f) {
        VectorSubtract(e->s.origin, b->q3_enemy_org, dir);
        if (VectorLengthSquared(dir) > 48 * 48 && DotProduct(b->q3_enemy_vel, vel) < 0)
            acc *= 0.7f;
    }

    if (b->enemy_visible) {
        VectorCopy(e->s.origin, best);
        best[2] += 8;
        tr = gi.trace(eye, mins4, maxs4, best, self, MASK_SHOT);
        if (tr.ent != e)
            best[2] += 16;

        // a slow shot: lead it, unless far off and only stepping about
        if (speed > 0) {
            dist = Distance(best, self->s.origin);
            VectorSubtract(e->s.origin, b->q3_enemy_org, dir);
            if (!(dist > 100 && VectorLengthSquared(dir) < 32 * 32)) {
                if (aim_skill > 0.8f && self->client->weaponstate == WEAPON_READY) {
                    Bot_Q3Predict(self, e, speed, best);
                } else if (aim_skill > 0.4f) {
                    vec3_t  hor = { vel[0], vel[1], 0 };
                    float   hspeed = VectorNormalize(hor);

                    dist = Distance(e->s.origin, self->s.origin);
                    VectorMA(e->s.origin, dist / speed * hspeed, hor, best);
                }
            }
        }

        // splash: at the floor under it, if that is in reach and in its sight
        if (aim_skill > 0.6f && Bot_Q3Radial(q3w, &damage, &radius) &&
            e->s.origin[2] < self->s.origin[2] + 16) {
            vec3_t  end, ground;

            VectorCopy(e->s.origin, end);
            end[2] -= 64;
            tr = gi.trace(e->s.origin, NULL, NULL, end, e, MASK_SHOT);
            VectorCopy(best, ground);
            ground[2] = tr.startsolid ? e->s.origin[2] - 16 : tr.endpos[2] - 8;
            tr = gi.trace(eye, NULL, NULL, ground, self, MASK_SHOT);
            if (fabsf(tr.endpos[2] - ground[2]) < 50) {
                VectorSubtract(tr.endpos, ground, dir);
                if (VectorLengthSquared(dir) < 60 * 60) {
                    VectorSubtract(tr.endpos, eye, dir);
                    if (VectorLengthSquared(dir) > 100 * 100) {
                        VectorCopy(tr.endpos, end);
                        end[2] += 1;
                        tr = gi.trace(end, NULL, NULL, e->s.origin, e, MASK_SHOT);
                        if (tr.fraction >= 1)
                            VectorCopy(ground, best);
                    }
                }
            }
        }

        best[0] += 20 * crandom() * (1 - acc);
        best[1] += 20 * crandom() * (1 - acc);
        best[2] += 10 * crandom() * (1 - acc);

        tr = gi.trace(eye, NULL, NULL, best, self, MASK_SHOT);
        VectorCopy(tr.endpos, aimtarget);
    } else {
        // where it was last seen (Quake III's shots round corners need its AAS)
        VectorCopy(b->enemy_pos, best);
        best[2] += 8;
        VectorCopy(best, aimtarget);
    }

    VectorSubtract(best, eye, dir);
    if (Bot_Q3Hitscan(q3w)) {
        dist = min(VectorLength(dir), 150.0f);
        acc *= 0.6f + dist / 150 * 0.4f;
    }
    Bot_VecToAngles(dir, clean);
    if (acc < 0.8f) {
        VectorNormalize(dir);
        for (i = 0; i < 3; i++)
            dir[i] += 0.3f * crandom() * (1 - acc);
    }
    Bot_VecToAngles(dir, b->q3_ideal);

    // a grenade on its arc - Quake III's own bots throw them straight -
    // with the same miss on top
    if (w && w->lob && Bot_LobPitch(eye, w, best, lob_pitch))
        b->q3_ideal[PITCH] = *lob_pitch + Bot_AngleDelta(b->q3_ideal[PITCH] - clean[PITCH]);

    b->q3_ideal[PITCH] = Bot_AngleDelta(b->q3_ideal[PITCH]);
    b->q3_ideal[ROLL] = 0;
    return true;
}

/*
=================
Bot_Q3CheckAttack

BotCheckAttack: whether to hold the trigger this think. Not before its
reaction time (up to a second) has passed since the enemy came into sight;
in bursts by its fire throttle; only once the aim point is within 25 degrees
of where it looks (60 up close), so a slow turner fires on its way round; not
through a team-mate, nor at a wall near enough to splash itself.
=================
*/
static bool Bot_Q3CheckAttack(edict_t *self, bot_t *b, const bot_weapon_t *w, const vec3_t aimtarget,
                              float lob_pitch)
{
    static const vec3_t mins8 = { -8, -8, -8 }, maxs8 = { 8, 8, 8 };
    const bot_character_t   *ch = &b->ch;
    int                     q3w = Bot_Q3Weapon(w);
    float                   throttle, fov, damage, radius;
    vec3_t                  eye, dir, angles, forward, start, end;
    trace_t                 tr;

    if (b->q3_sight_time > level.time - BotChar_BFloat(ch, BCH_REACTIONTIME, 0, 1))
        return false;

    // the fire throttle: now and then a pause of `throttle` seconds, else
    // a burst of 1 - throttle. (A pause ending this very think counts as
    // still on; in Quake III, float time made that a coin toss.)
    if (b->q3_throttle_wait > level.time - 0.001f)
        return false;
    throttle = BotChar_BFloat(ch, BCH_FIRETHROTTLE, 0, 1);
    if (b->q3_throttle_shoot < level.time) {
        if (random() > throttle) {
            b->q3_throttle_wait = level.time + throttle;
            b->q3_throttle_shoot = 0;
        } else {
            b->q3_throttle_shoot = level.time + 1 - throttle;
            b->q3_throttle_wait = 0;
        }
    }

    Bot_EyePos(self, eye);
    VectorSubtract(aimtarget, eye, dir);
    if (q3w == Q3W_GAUNTLET && VectorLengthSquared(dir) > 60 * 60)
        return false;
    fov = VectorLengthSquared(dir) < 100 * 100 ? 120 : 50;
    Bot_VecToAngles(dir, angles);
    if (lob_pitch < 900)
        angles[PITCH] = lob_pitch;
    if (!Bot_Q3InFov(b->view, fov, angles))
        return false;

    tr = gi.trace(eye, NULL, NULL, aimtarget, self, CONTENTS_SOLID | CONTENTS_PLAYERCLIP);
    if (tr.fraction < 1 && tr.ent != b->enemy)
        return false;

    // along the view itself
    AngleVectors(b->view, forward, NULL, NULL);
    VectorMA(eye, 1000, forward, end);
    VectorMA(eye, -12, forward, start);
    tr = gi.trace(start, mins8, maxs8, end, self, MASK_SHOT);
    if (tr.ent && tr.ent != b->enemy && tr.ent->client && G_TeamPlay() && OnSameTeam(self, tr.ent))
        return false;
    if (tr.ent != b->enemy && Bot_Q3Radial(q3w, &damage, &radius)) {
        float d = tr.fraction * 1000;

        if (d < radius && (damage - 0.5f * d) * 0.5f > 0)
            return false;
    }
    return true;
}

// BotWalkInDirection's say, roughly: not into a wall (steps are fine), off a
// ledge or into lava
static bool Bot_Q3CanMove(edict_t *self, const vec3_t dir)
{
    vec3_t  flat = { dir[0], dir[1], 0 }, mins, end;
    trace_t tr;

    if (VectorNormalize(flat) < 0.001f)
        return false;
    VectorCopy(self->mins, mins);
    mins[2] += 18;      // a stair step
    VectorMA(self->s.origin, 48, flat, end);
    tr = gi.trace(self->s.origin, mins, self->maxs, end, self, MASK_PLAYERSOLID);
    if (tr.startsolid || tr.fraction < 0.5f)
        return false;
    return Bot_SafeFloor(self, tr.endpos);
}

/*
=================
Bot_Q3AttackMove

BotAttackMove, once a think. Below attack skill 0.2 the bot stands and
shoots; to 0.4 it only walks up to, or back from, 140 units off; above that
it strafes, flipping about every 0.4-0.6 s at a 6.5% chance a think, edging
in or out to 140 and now and then back. Jumps (one a second at most) and
crouches as its character says.
=================
*/
static bool Bot_Q3AttackMove(edict_t *self, bot_t *b, const bot_weapon_t *w, vec3_t wish, bool *jump, bool *crouch)
{
    const bot_character_t   *ch = &b->ch;
    float                   attack_skill = BotChar_BFloat(ch, BCH_ATTACK_SKILL, 0, 1);
    float                   jumper = BotChar_BFloat(ch, BCH_JUMPER, 0, 1);
    float                   croucher = BotChar_BFloat(ch, BCH_CROUCHER, 0, 1);
    float                   dist, attack_dist, attack_range, change;
    bool                    want_jump = false;
    vec3_t                  forward, backward, side, hordir;
    int                     i;

    VectorClear(wish);
    *jump = *crouch = false;
    if (attack_skill < 0.2f)
        return false;

    VectorSubtract(b->enemy->s.origin, self->s.origin, forward);
    dist = VectorNormalize(forward);
    VectorNegate(forward, backward);

    if (b->q3_crouch_until < level.time - 1) {
        if (random() < jumper)
            want_jump = true;
        else if (random() < croucher)       // wait a second between crouches
            b->q3_crouch_until = level.time + croucher * 5;
    }
    if (b->q3_crouch_until > level.time) {
        *crouch = true;
        want_jump = false;
    }
    if (want_jump) {
        if (b->q3_jump_time > level.time)
            want_jump = false;
        else
            b->q3_jump_time = level.time + 1;
    }

    if (w && w->melee) {
        attack_dist = 0;
        attack_range = 0;
    } else {
        attack_dist = BOT_Q3_ATTACKDIST;
        attack_range = 40;
    }

    // not much of a fighter: just to or away from it
    if (attack_skill <= 0.4f) {
        if (dist > attack_dist + attack_range && Bot_Q3CanMove(self, forward)) {
            VectorSet(wish, forward[0], forward[1], 0);
        } else if (dist < attack_dist - attack_range && Bot_Q3CanMove(self, backward)) {
            VectorSet(wish, backward[0], backward[1], 0);
        } else {
            return false;
        }
        VectorNormalize(wish);
        *jump = want_jump;
        return true;
    }

    b->q3_strafe_time += FRAMETIME;
    change = 0.4f + (1 - attack_skill) * 0.2f;
    if (attack_skill > 0.7f)
        change += crandom() * 0.2f;
    if (b->q3_strafe_time > change && random() > 0.935f) {
        b->q3_strafe_right = !b->q3_strafe_right;
        b->q3_strafe_time = 0;
    }

    for (i = 0; i < 2; i++) {
        VectorSet(hordir, forward[0], forward[1], 0);
        VectorNormalize(hordir);
        VectorSet(side, hordir[1], -hordir[0], 0);     // hordir x up
        if (b->q3_strafe_right)
            VectorNegate(side, side);
        if (random() > 0.9f)
            VectorAdd(side, backward, side);
        else if (dist > attack_dist + attack_range)
            VectorAdd(side, forward, side);
        else if (dist < attack_dist - attack_range)
            VectorAdd(side, backward, side);
        side[2] = 0;

        if (Bot_Q3CanMove(self, side)) {
            VectorNormalize(side);
            VectorCopy(side, wish);
            *jump = want_jump;
            return true;
        }
        // that way's blocked: the other
        b->q3_strafe_right = !b->q3_strafe_right;
        b->q3_strafe_time = 0;
    }
    return false;
}

/*
=================
Bot_Q3Turn

BotChangeViewAngles' "over-reaction" model (bot_challenge 0), a tick every
50 ms as on Quake III's 20 Hz server: each tick the turn speed doubles less
view_factor of the way still to go, is held to view_maxchange (240 at least)
degrees a second, and keeps 0.45 x (1 - factor) of itself for the next.
Factor 1 snaps onto the aim; 0.2 swings past it and back.
=================
*/
static void Bot_Q3Turn(bot_t *b, const vec3_t ideal, float dt)
{
    float   factor = BotChar_BFloat(&b->ch, BCH_VIEW_FACTOR, 0.01f, 1);
    float   maxchange = BotChar_BFloat(&b->ch, BCH_VIEW_MAXCHANGE, 1, 1800);
    int     i;

    if (maxchange < 240)
        maxchange = 240;
    maxchange *= dt;

    for (i = PITCH; i <= YAW; i++) {
        float diff = Bot_AngleDelta(b->view[i] - ideal[i]);
        float speed = b->q3_turn[i];

        speed += speed - diff * factor;
        if (speed > 180)
            speed = maxchange;
        if (speed < -180)
            speed = -maxchange;
        b->view[i] = Bot_AngleDelta(b->view[i] + max(-maxchange, min(maxchange, speed)));
        b->q3_turn[i] = speed * 0.45f * (1 - factor);
    }

    if (b->view[PITCH] > 89)
        b->view[PITCH] = 89;
    else if (b->view[PITCH] < -89)
        b->view[PITCH] = -89;
    b->view[ROLL] = 0;
}

// ==========================================================================
// THINKING

static void Bot_SendCommand(edict_t *self, bot_t *b, const vec3_t wish, int speed, bool jump, bool climb, bool attack)
{
    usercmd_t   cmd;
    float       yaw = DEG2RAD(b->view[YAW]);
    float       fx = cosf(yaw), fy = sinf(yaw);
    int         i;

    memset(&cmd, 0, sizeof(cmd));
    cmd.msec = BOT_STEP_MSEC;
    cmd.lightlevel = 128;

    for (i = 0; i < 3; i++)
        cmd.angles[i] = (short)(ANGLE2SHORT(b->view[i]) - self->client->ps.pmove.delta_angles[i]);

    cmd.forwardmove = (short)((wish[0] * fx + wish[1] * fy) * speed);
    cmd.sidemove = (short)((wish[0] * fy - wish[1] * fx) * speed);

    // Quake III's movement takes a command's speed from its LARGEST axis, as
    // keys give it (pmove.c PM_Q3CmdScale), so a bot's diagonal - a unit
    // vector's worth split over two axes - would run slow. Stretch it.
    if (level.q3_physics && speed) {
        float   f = fabsf(cmd.forwardmove), s = fabsf(cmd.sidemove);
        float   m = max(f, s);

        if (m > 1 && m < speed) {
            cmd.forwardmove = (short)(cmd.forwardmove * speed / m);
            cmd.sidemove = (short)(cmd.sidemove * speed / m);
        }
    }

    if (jump || climb)
        cmd.upmove = 400;
    else if (b->crouch)
        cmd.upmove = -400;

    if (attack)
        cmd.buttons |= BUTTON_ATTACK;

    ClientThink(self, &cmd);

    // a teleporter turned us to face its exit: look that way from here on
    if (memcmp(b->delta, self->client->ps.pmove.delta_angles, sizeof(b->delta))) {
        VectorCopy(self->client->v_angle, b->view);
        VectorClear(b->turn);
        memcpy(b->delta, self->client->ps.pmove.delta_angles, sizeof(b->delta));
    }
}

static void Bot_Dead(edict_t *self, bot_t *b)
{
    static const vec3_t still = { 0, 0, 0 };
    const bot_skill_t   *sk = &bot_skills[b->skill];
    int                 step;

    if (!b->dead) {
        b->dead = true;
        b->respawn_time = level.time + Bot_Rand(sk->respawn_min, sk->respawn_max);
        b->enemy = NULL;
    }
    b->crouch = false;

    // a press, then a release, so the respawn sees a new press
    for (step = 0; step < BOT_STEPS; step++)
        Bot_SendCommand(self, b, still, 0, false, false,
                        level.time >= b->respawn_time && !(step & 1));
}

static void Bot_Think(edict_t *self, bot_t *b)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    const bot_weapon_t  *w;
    vec3_t              want_view, aim_at = { 0, 0, 0 };
    float               lob_pitch = 999, tolerance = 0;
    bool                fighting, aiming = false, flight;
    int                 step, speed;
    // Quake III's rules, for a bot with a character: this think's aim,
    // trigger and fighting move, held for all four commands as Quake III held
    // a think's input for its two server frames
    bool                q3 = b->ch.loaded, q3_attack = false;
    bool                q3_moving = false, q3_jump = false, q3_crouch = false;
    vec3_t              q3_wish = { 0, 0, 0 };
    float               q3_run = BOT_RUN;

    if (level.intermission_framenum)
        return;

    b->crouch = false;

    if (self->deadflag || self->health <= 0) {
        Bot_Dead(self, b);
        return;
    }

    // back from the dead (PutClientInServer set a new view): look where we
    // were put
    if (b->dead || memcmp(b->delta, self->client->ps.pmove.delta_angles, sizeof(b->delta))) {
        if (b->dead)
            Bot_ResetBrain(b);
        VectorCopy(self->client->v_angle, b->view);
        VectorClear(b->turn);
        memcpy(b->delta, self->client->ps.pmove.delta_angles, sizeof(b->delta));
    }
    // a jump pad's flight (Bot_PadFlight) is ridden out; down again, the
    // route goes on from wherever it landed
    flight = Bot_PadFlight(self, b);
    if (flight) {
        b->pad_flight = true;
    } else if (b->pad_flight) {
        b->pad_flight = false;
        b->replan_time = 0;
        if (b->goal == GOAL_NONE || !b->route_len)
            b->next_goal_time = 0;
    }

    if (!flight && Distance(self->s.origin, b->last_origin) > 160) {
        // a teleporter: the route was for the other side
        b->route_len = b->route_pos = 0;
        b->next_goal_time = 0;
    }

    Bot_Perceive(self, b);
    Bot_ChooseWeapon(self, b);
    Bot_Dangers(self, b);

    fighting = b->enemy && b->enemy_visible;

    // where to go. Choosing costs route searches, so only a couple of bots
    // choose in any one frame; the rest carry on and choose next frame.
    // Not in a pad's flight: from mid-air the nearest node is the floor under it.
    if (!flight && (Bot_GoalReached(self, b) || level.time >= b->next_goal_time) && bot_goal_budget > 0) {
        bot_goal_budget--;
        if (Bot_GoalReached(self, b))
            b->goal = GOAL_NONE;
        if (G_CTF() && Bot_CTFGoal(self, b, fighting))
            ;                                   // the team's errand comes first
        else if (fighting && (sk->combat_items || q3))
            Bot_PickGoal(self, b, true);        // only what it is short of, close by
        else if (fighting && sk->combat)
            b->next_goal_time = level.time + 1;
        else if (fighting)
            Bot_PickGoal(self, b, false);       // shoots, but keeps on its way
        else if (b->enemy && (sk->combat || q3) &&
                 self->health >= self->max_health * sk->min_health * 0.5f) {
            // gone round a corner: after it
            if (b->goal != GOAL_CHASE || Distance(b->goal_pos, b->enemy_pos) > 128) {
                b->goal = GOAL_CHASE;
                b->goal_item = NULL;
                Bot_PlanTo(self, b, b->enemy_pos);
            }
            b->next_goal_time = level.time + 1;
        } else {
            Bot_PickGoal(self, b, false);
        }
    }
    if (!flight && b->goal != GOAL_NONE && b->route_len && level.time >= b->replan_time)
        Bot_PlanTo(self, b, b->goal_pos);

    Bot_Shortcut(self, b);

    // stuck?
    if (!flight && level.time >= b->stuck_time) {
        if (!b->waiting && Distance(self->s.origin, b->stuck_pos) < 24 && b->goal != GOAL_NONE)
            b->stuck_count++;
        else
            b->stuck_count = 0;
        VectorCopy(self->s.origin, b->stuck_pos);
        b->stuck_time = level.time + 1;

        if (b->stuck_count == 1) {
            b->jump = true;
        } else if (b->stuck_count == 2) {
            b->unstick_until = level.time + 0.8f;
            b->unstick_dir = Bot_Rand(-180, 180);
        } else if (b->stuck_count >= 3) {
            Bot_Avoid(b, b->goal_item);
            b->goal = GOAL_NONE;
            b->goal_item = NULL;
            b->route_len = b->route_pos = 0;
            b->next_goal_time = 0;
            b->stuck_count = 0;
        }
    }

    // the weapon, and how far off the aim can be and still fire
    w = Bot_CurrentWeapon(self);
    if (q3) {
        // a walker walks wherever it is going - not when fighting, where
        // BotAttackMove always ran (BotTravel_Walk's MFL_WALK)
        if (BotChar_BFloat(&b->ch, BCH_WALKER, 0, 1) > 0.5f)
            q3_run = BOT_WALK;
        if (fighting) {
            aiming = true;
            if (!Bot_Q3Aim(self, b, w, aim_at, &lob_pitch))
                VectorCopy(b->view, b->q3_ideal);      // holding off: stays put
            else
                q3_attack = w && Bot_Q3CheckAttack(self, b, w, aim_at, lob_pitch);
            q3_moving = Bot_Q3AttackMove(self, b, w, q3_wish, &q3_jump, &q3_crouch);
        }
    } else if (fighting) {
        float dist = Distance(self->s.origin, b->enemy->s.origin);
        float fov = w ? w->ideal_fov : 15;

        Bot_AimPoint(self, b, w, aim_at, &lob_pitch);
        aiming = true;
        tolerance = min(fov * 0.5f, max(RAD2DEG(atanf(28.0f / max(dist, 1.0f))), 1.5f)) * sk->fire_fov_scale;
    }

    // the aim's wander
    if (level.time >= b->wander_time) {
        float scale = sk->wander_angle * 0.1f;

        if (level.time - b->acquired_time < 1)
            scale *= sk->wander_scale;
        b->wander[0] = crandom() * scale * 0.5f;
        b->wander[1] = crandom() * scale;
        b->wander_time = level.time + sk->wander_time;
    }

    for (step = 0; step < BOT_STEPS; step++) {
        vec3_t  wish = { 0, 0, 0 }, eye, dir;
        bool    jump = false, climb = false, attack = false;
        bool    moving, riding;

        moving = Bot_FollowRoute(self, b, wish, &jump, &climb);
        speed = (jump || climb) ? BOT_RUN : (int)q3_run;

        // a jump pad's flight: Bot_FollowRoute leans toward the landing, and
        // nothing below turns it - it still aims and shoots
        riding = Bot_PadFlight(self, b);

        // the last few units to an item off the end of the route
        if (!moving && b->goal == GOAL_ITEM && b->goal_item && Bot_ItemAvailable(b->goal_item) &&
            Bot_Dist2D(self->s.origin, b->goal_item->s.origin) < 160) {
            Bot_Steer(self, b->goal_item->s.origin, wish);
            moving = true;
        }
        // no mesh at all: straight at it
        if (!moving && b->goal != GOAL_NONE && !Nav_NumNodes()) {
            Bot_Steer(self, b->goal_pos, wish);
            moving = true;
        }

        // Quake III's fighting move, unless it is on its way to an item
        if (fighting && q3 && !riding) {
            if ((b->goal != GOAL_ITEM && b->goal != GOAL_CTF) || !moving) {
                VectorCopy(q3_wish, wish);
                moving = q3_moving;
                speed = BOT_RUN;
                b->crouch = q3_crouch;
                if (step == 0 && q3_jump && self->groundentity)
                    jump = true;
            }
        }
        // fighting: keep at the weapon's range and keep moving sideways
        else if (fighting && sk->combat && !riding) {
            vec3_t  to, side, test;
            float   dist, ideal;

            VectorSubtract(b->enemy->s.origin, self->s.origin, to);
            to[2] = 0;
            dist = VectorNormalize(to);
            VectorSet(side, to[1], -to[0], 0);     // to the right

            ideal = w ? w->min_range + (min(w->max_range, 1200) - w->min_range) * 0.3f : 400;
            if ((b->goal != GOAL_ITEM && b->goal != GOAL_CTF) || !moving) {
                VectorClear(wish);
                if (dist > ideal * 1.4f)
                    VectorAdd(wish, to, wish);
                else if (dist < ideal * 0.7f)
                    VectorSubtract(wish, to, wish);
            }

            if (level.time >= b->strafe_until) {
                b->strafe = (random() < 0.5f) ? -b->strafe : b->strafe;
                b->strafe_until = level.time + Bot_Rand(0.4f, 1.4f);
            }
            VectorMA(wish, b->strafe, side, wish);
            VectorNormalize(wish);

            // not off a ledge
            VectorMA(self->s.origin, 48, wish, test);
            if (!Bot_SafeFloor(self, test)) {
                b->strafe = -b->strafe;
                VectorClear(wish);
            }

            if (sk->walk_only)
                speed = BOT_WALK;

            if (sk->combat_jump && step == 0 && self->groundentity && level.time >= b->next_jump_time) {
                b->next_jump_time = level.time + max(sk->jump_cooldown, 0.5f);
                if (random() * 100 < sk->jump_chance)
                    jump = true;
            }
            moving = true;
        }

        // out of the way of a grenade or a rocket
        if (!VectorEmpty(b->dodge) && !riding) {
            VectorAdd(wish, b->dodge, wish);
            VectorAdd(wish, b->dodge, wish);
            VectorNormalize(wish);
            moving = true;
        }

        if (level.time < b->unstick_until && !riding) {
            VectorSet(wish, cosf(DEG2RAD(b->unstick_dir)), sinf(DEG2RAD(b->unstick_dir)), 0);
            moving = true;
        }

        if (b->jump && step == 0) {
            jump = true;
            b->jump = false;
        }
        // a jump is a press: let go between them
        if (self->client->ps.pmove.pm_flags & PMF_JUMP_HELD)
            jump = jump && self->waterlevel >= 2;

        // where to look
        if (aiming && q3) {
            VectorCopy(b->q3_ideal, want_view);
        } else if (aiming) {
            Bot_EyePos(self, eye);
            VectorSubtract(aim_at, eye, dir);
            Bot_VecToAngles(dir, want_view);
            if (lob_pitch < 900)
                want_view[PITCH] = lob_pitch;
            want_view[PITCH] += b->wander[0];
            want_view[YAW] += b->wander[1];
        } else if (b->enemy && level.time - b->enemy_seen < 1.5f) {
            Bot_EyePos(self, eye);
            VectorSubtract(b->enemy_pos, eye, dir);
            Bot_VecToAngles(dir, want_view);
        } else if (level.time >= b->heard_at && level.time < b->heard_until) {
            Bot_EyePos(self, eye);
            VectorSubtract(b->heard_pos, eye, dir);
            Bot_VecToAngles(dir, want_view);
        } else if (moving && !VectorEmpty(wish)) {
            want_view[YAW] = RAD2DEG(atan2f(wish[1], wish[0]));
            want_view[PITCH] = self->waterlevel >= 2 && climb ? -30 : 0;
        } else {
            VectorCopy(b->view, want_view);
        }
        want_view[PITCH] = Bot_AngleDelta(want_view[PITCH]);
        want_view[ROLL] = 0;

        // with an enemy, Quake III's view turns by its own model, ticking
        // every other command (20 Hz); otherwise the aim spring's
        if (q3 && b->enemy) {
            if (!b->q3_turning) {
                b->q3_turn[0] = b->q3_turn[1] = 0;
                b->q3_turning = true;
            }
            if (!(step & 1))
                Bot_Q3Turn(b, want_view, BOT_STEP_TIME * 2);
        } else {
            if (b->q3_turning) {
                VectorClear(b->turn);
                b->q3_turning = false;
            }
            Bot_TurnToward(b, want_view, BOT_STEP_TIME);
        }

        // fire?
        if (q3) {
            attack = aiming && q3_attack;
        } else if (aiming && w && level.time - b->enemy_visible_since >= sk->fire_sight_time) {
            vec3_t check;

            VectorCopy(aim_at, check);
            if (lob_pitch < 900) {
                // a lob: only the yaw has to be on, and the pitch near the arc
                vec3_t flat;

                Bot_EyePos(self, eye);
                VectorSubtract(aim_at, eye, flat);
                Bot_VecToAngles(flat, dir);
                attack = fabsf(Bot_AngleDelta(dir[YAW] - b->view[YAW])) <= tolerance &&
                         fabsf(Bot_AngleDelta(lob_pitch - b->view[PITCH])) <= tolerance;
            } else {
                attack = Bot_AimError(self, b, check) <= tolerance;
            }
            if (attack && Bot_FriendInWay(self, aim_at))
                attack = false;
        }

        if (!moving)
            VectorClear(wish);

        Bot_SendCommand(self, b, wish, speed, jump, climb, attack);
        if (!self->inuse || self->deadflag)
            break;
    }

    VectorCopy(self->s.origin, b->last_origin);

    if (bot_debug->integer && (level.framenum % 10) == 0)
        gi.dprintf("%s: goal %d item %s route %d/%d enemy %s%s\n", b->name, b->goal,
                   b->goal_item ? b->goal_item->classname : "-", b->route_pos, b->route_len,
                   b->enemy ? b->enemy->client->pers.netname : "-",
                   b->enemy_visible ? " (in sight)" : "");
}

/*
=================
Bot_RunFrame

G_RunFrame, before the entities: bring in any bot still waiting to join this
level, then let each one think and move.
=================
*/
void Bot_RunFrame(void)
{
    int i;

    if (!bots || !deathmatch->value)
        return;

    bot_goal_budget = BOT_GOALS_PER_FRAME;

    if (bots_pending && Bot_HumanInGame() && !level.intermission_framenum) {
        bool waiting = false;

        if (bot_human_since < 0)
            bot_human_since = level.time;

        bots_pending = false;
        for (i = 0; i < game.maxclients; i++) {
            if (bots[i].active && !bots[i].spawned) {
                // a campaign bot keeps to Quake III's schedule
                if (bots[i].campaign && level.time < bot_human_since + bots[i].join_delay) {
                    waiting = true;
                    continue;
                }
                // one a frame: each ClientBegin is a configstring and a spawn
                if (Bot_Spawn(i) && bots[i].campaign)
                    SP_BotJoined(bots[i].skin);
                bots_pending = true;
                break;
            }
        }
        if (waiting)
            bots_pending = true;
    }

    for (i = 0; i < game.maxclients; i++) {
        edict_t *ent = Bot_Edict(i);

        if (!bots[i].active || !bots[i].spawned || !ent->inuse || !ent->client)
            continue;
        // Quake III's engine plays its own, all at once, below - unless it
        // has let go of this one, when Quake II's brain takes over
        if (bots[i].q3_engine) {
            if (BotEngine_Running(ent))
                continue;
            bots[i].q3_engine = false;
            Bot_ResetBrain(&bots[i]);
            VectorCopy(ent->client->v_angle, bots[i].view);
            memcpy(bots[i].delta, ent->client->ps.pmove.delta_angles, sizeof(bots[i].delta));
        }
        Bot_Think(ent, &bots[i]);
    }

    BotEngine_Frame();
}

// ==========================================================================
// COMMANDS

static int Bot_SkillByName(const char *s)
{
    int i;

    if (!*s)
        return 2;
    for (i = 0; i < BOT_NUM_SKILLS; i++)
        if (!Q_stricmp(s, bot_skills[i].name))
            return i;
    return atoi(s);
}

static void Bot_List(void)
{
    int i, n = 0;

    for (i = 0; i < game.maxclients; i++) {
        if (!bots[i].active)
            continue;
        gi.cprintf(NULL, PRINT_HIGH, "%2d %-16s %-10s %-18s%s%s%s\n", i, bots[i].name,
                   bot_skills[bots[i].skill].name, bots[i].skin,
                   G_TeamPlay() ? Team_Name(Bot_Edict(i)->inuse ? Bot_Edict(i)->client->resp.team : bots[i].team) : "",
                   bots[i].q3_engine ? " (Quake III engine)" : bots[i].ch.loaded ? " (Quake III character)" : "",
                   bots[i].spawned ? "" : " (waiting)");
        n++;
    }
    if (!n)
        gi.cprintf(NULL, PRINT_HIGH, "No bots.\n");
}

/*
=================
Bot_ServerCommand

"sv addbot [skin name|model/skin|random] [skill 0-5 or name] [red|blue]"
"sv removebot <name|all>"
"sv listbots"
=================
*/
bool Bot_ServerCommand(const char *cmd)
{
    int i;

    if (!bots)
        return false;

    if (!Q_stricmp(cmd, "addbot")) {
        const char  *t = gi.argv(4);
        int         team = !Q_stricmp(t, "red") ? TEAM_RED : !Q_stricmp(t, "blue") ? TEAM_BLUE : TEAM_NONE;

        i = Bot_Add(gi.argc() > 2 ? gi.argv(2) : "random", Bot_SkillByName(gi.argv(3)), team, 0);
        if (i >= 0)
            gi.cprintf(NULL, PRINT_HIGH, "Added %s (%s).\n", bots[i].name, bot_skills[bots[i].skill].name);
        return true;
    }

    if (!Q_stricmp(cmd, "removebot")) {
        const char *who = gi.argv(2);
        bool        all = !Q_stricmp(who, "all");
        int         n = 0;

        if (!*who) {
            gi.cprintf(NULL, PRINT_HIGH, "Usage: sv removebot <name|all>\n");
            return true;
        }
        for (i = 0; i < game.maxclients; i++) {
            if (bots[i].active && (all || !Q_stricmp(bots[i].name, who))) {
                Bot_Remove(i, "was removed");
                n++;
            }
        }
        if (!n)
            gi.cprintf(NULL, PRINT_HIGH, "No bot called \"%s\".\n", who);
        return true;
    }

    if (!Q_stricmp(cmd, "listbots")) {
        Bot_List();
        return true;
    }

    return false;
}
