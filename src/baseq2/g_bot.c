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
} bot_t;

static bot_t    *bots;              // one per client slot
static bool     bots_pending;       // some active bot is not in this level yet
static bool     roster_built;       // the menu's bots were added this game

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

static void Bot_Unicast(edict_t *ent, qboolean reliable)
{
    real_unicast(Bot_IsBot(ent) ? NULL : ent, reliable);
}

static void Bot_Cprintf(edict_t *ent, int printlevel, const char *fmt, ...)
{
    char    msg[MAX_STRING_CHARS];
    va_list argptr;

    if (Bot_IsBot(ent))
        return;

    va_start(argptr, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, argptr);
    va_end(argptr);

    real_cprintf(ent, printlevel, "%s", msg);
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
    gi.unicast = Bot_Unicast;
    gi.cprintf = Bot_Cprintf;
    gi.centerprintf = Bot_Centerprintf;
}

// ==========================================================================
// TEAMS
//
// teamplay 1: two teams, red and blue, balanced as people join, scored
// together. Friendly fire follows dmflags' DF_NO_FRIENDLY_FIRE.

// CTF plays in the same two teams
bool G_TeamPlay(void)
{
    return deathmatch->value && ((teamplay && teamplay->value) || G_CTF());
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

static bool Bot_SkinExists(const char *skin)
{
    static const char *const exts[] = { "pcx", "png", "tga" };
    int i;

    for (i = 0; i < 3; i++)
        if (gi.LoadFile && gi.LoadFile(va("players/%s.%s", skin, exts[i]), NULL) >= 0)
            return true;

    return false;
}

static void Bot_PoolAdd(const char *skin)
{
    int i;

    if (skin_pool_count >= BOT_MAX_SKINS || !strchr(skin, '/') || !Bot_SkinUsable(skin))
        return;
    for (i = 0; i < skin_pool_count; i++)
        if (!Q_stricmp(skin_pool[i], skin))
            return;
    if (!Bot_SkinExists(skin))
        return;
    Q_strlcpy(skin_pool[skin_pool_count++], skin, MAX_QPATH);
}

static void Bot_LoadSkins(void)
{
    int i;

    if (skin_pool)
        return;

    skin_pool = gi.TagMalloc(BOT_MAX_SKINS * sizeof(*skin_pool), TAG_GAME);
    skin_pool_count = 0;

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
    return true;
}

static int Bot_Add(const char *want, int skill, int team)
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

    bots = gi.TagMalloc(game.maxclients * sizeof(*bots), TAG_GAME);
    memset(bots, 0, game.maxclients * sizeof(*bots));
    bots_pending = false;
    roster_built = false;
    skin_pool = NULL;
    skin_pool_count = 0;
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

    for (i = 0; i < game.maxclients; i++) {
        bots[i].spawned = false;
        if (bots[i].active && !deathmatch->value)
            bots[i].active = false;
        if (bots[i].active)
            bots_pending = true;
    }

    if (!deathmatch->value)
        return;

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
            if (Bot_Add(skin->string, skill->integer, teams[i]) < 0)
                Bot_Add("random", skill->integer, teams[i]);
        }
        if (count && game.maxclients < 2)
            gi.dprintf("Bots: maxclients is 1, so there is no room for bots\n");
    }

    for (i = 0; i < game.maxclients; i++) {
        if (bots[i].active) {
            // map load is the time to spend building a navmesh, not the
            // first frame someone is playing
            Nav_PrepareForBots();
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

static void Bot_Perceive(edict_t *self, bot_t *b)
{
    const bot_skill_t   *sk = &bot_skills[b->skill];
    edict_t             *best = NULL;
    float               best_dist = 0;
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

        if (!Bot_Invisible(other) || dist < sk->invis_dist) {
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

        // a shot nearby: look that way, after a moment
        if (!seen && b->heard_until < level.time && dist < sk->sound_range && Bot_Firing(other)) {
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

        if (!b->enemy || !b->enemy_visible || best_dist < cur * 0.5f) {
            b->enemy = best;
            b->enemy_visible = true;
            b->enemy_seen = level.time;
            b->enemy_visible_since = level.time;
            b->acquired_time = level.time;
            VectorCopy(best->s.origin, b->enemy_pos);
        }
    }
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

    if (!Bot_IsBot(self) || !attacker || !Bot_ValidTarget(self, attacker))
        return;

    b = &bots[self - g_edicts - 1];
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
    const gitem_t       *item = it->item;
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
Bot_FollowRoute

Where to go next along the route: sets `wish` (a flat direction, or zero to
stand still) and asks for a jump or a climb. False when the route is used up.
=================
*/
static bool Bot_FollowRoute(edict_t *self, bot_t *b, vec3_t wish, bool *jump, bool *climb)
{
    float radius = Nav_IsGenerated() ? 20 : 32;

    b->waiting = false;

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

static void Bot_Dangers(edict_t *self, bot_t *b)
{
    int i;

    VectorClear(b->dodge);
    if (!bot_skills[b->skill].dangers)
        return;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        edict_t *e = g_edicts + i;
        vec3_t  rel;
        float   dist;

        if (!e->inuse || !e->classname)
            continue;

        if (!strcmp(e->classname, "grenade") || !strcmp(e->classname, "hgrenade")) {
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

    // a grenade: aim above it so the arc comes down on it. fire_grenade
    // launches at 600 along the view plus 200 along the view's up, which is
    // ~632 units/s at 18.4 degrees above where we look.
    if (w && w->lob) {
        float   g = sv_gravity->value > 0 ? sv_gravity->value : 800;
        float   dx = Bot_Dist2D(eye, target);
        float   dz = target[2] - eye[2];
        float   v = sqrtf(w->speed * w->speed + 200 * 200);
        float   v2 = v * v;
        float   root = v2 * v2 - g * (g * dx * dx + 2 * dz * v2);

        if (root >= 0 && dx > 1) {
            float elevation = RAD2DEG(atanf((v2 - sqrtf(root)) / (g * dx)));

            *lob_pitch = -(elevation - RAD2DEG(atanf(200 / w->speed)));
        }
    }

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

    if (jump || climb)
        cmd.upmove = 400;

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
    bool                fighting, aiming = false;
    int                 step, speed;

    if (level.intermission_framenum)
        return;

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
    if (Distance(self->s.origin, b->last_origin) > 160) {
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
    if ((Bot_GoalReached(self, b) || level.time >= b->next_goal_time) && bot_goal_budget > 0) {
        bot_goal_budget--;
        if (Bot_GoalReached(self, b))
            b->goal = GOAL_NONE;
        if (G_CTF() && Bot_CTFGoal(self, b, fighting))
            ;                                   // the team's errand comes first
        else if (fighting && sk->combat_items)
            Bot_PickGoal(self, b, true);        // only what it is short of, close by
        else if (fighting && sk->combat)
            b->next_goal_time = level.time + 1;
        else if (fighting)
            Bot_PickGoal(self, b, false);       // shoots, but keeps on its way
        else if (b->enemy && sk->combat &&
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
    if (b->goal != GOAL_NONE && b->route_len && level.time >= b->replan_time)
        Bot_PlanTo(self, b, b->goal_pos);

    Bot_Shortcut(self, b);

    // stuck?
    if (level.time >= b->stuck_time) {
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
    if (fighting) {
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
        bool    moving;

        speed = BOT_RUN;
        moving = Bot_FollowRoute(self, b, wish, &jump, &climb);

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

        // fighting: keep at the weapon's range and keep moving sideways
        if (fighting && sk->combat) {
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
        if (!VectorEmpty(b->dodge)) {
            VectorAdd(wish, b->dodge, wish);
            VectorAdd(wish, b->dodge, wish);
            VectorNormalize(wish);
            moving = true;
        }

        if (level.time < b->unstick_until) {
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
        if (aiming) {
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

        Bot_TurnToward(b, want_view, BOT_STEP_TIME);

        // fire?
        if (aiming && w && level.time - b->enemy_visible_since >= sk->fire_sight_time) {
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
        bots_pending = false;
        for (i = 0; i < game.maxclients; i++) {
            if (bots[i].active && !bots[i].spawned) {
                // one a frame: each ClientBegin is a configstring and a spawn
                Bot_Spawn(i);
                bots_pending = true;
                break;
            }
        }
        if (i == game.maxclients)
            bots_pending = false;
    }

    for (i = 0; i < game.maxclients; i++) {
        edict_t *ent = Bot_Edict(i);

        if (!bots[i].active || !bots[i].spawned || !ent->inuse || !ent->client)
            continue;
        Bot_Think(ent, &bots[i]);
    }
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
        gi.cprintf(NULL, PRINT_HIGH, "%2d %-16s %-10s %-18s%s%s\n", i, bots[i].name,
                   bot_skills[bots[i].skill].name, bots[i].skin,
                   G_TeamPlay() ? Team_Name(Bot_Edict(i)->inuse ? Bot_Edict(i)->client->resp.team : bots[i].team) : "",
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

        i = Bot_Add(gi.argc() > 2 ? gi.argv(2) : "random", Bot_SkillByName(gi.argv(3)), team);
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
