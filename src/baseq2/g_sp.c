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

QUAKE III ARENA CAMPAIGN - the game's half

Quake III's single player game, arena by arena, as its game code ran it
(ioq3 code/game: g_bot.c G_InitBots / G_SpawnBots, g_combat.c player_die,
g_weapon.c's accuracy counts, g_arenas.c's podium and UpdateTournamentInfo).
The menus that pick the arena and the skill and show the results are the
client's (client/ui/q3sp.c); it starts an arena with

    sp_campaign 1; sp_skill <1-5>; deathmatch 1; ... map <arena> force

and this side takes it from there:

  - The arena's line in Quake III's scripts/arenas.txt (or a scripts/*.arena)
    gives its bots, in order, and its frag and time limits; with neither, a
    fraglimit of 10. The menu's own fraglimit, timelimit, bot_count and the
    rest are left alone - they are the bot match's.
  - Quake III's weapons, movement and item amounts, whatever the bot match is
    set to (Q3_LevelRules, Q3_ItemRules).
  - The bots join as Quake III's did: the first two seconds after you are in
    (twelve in the training arena), one more every second and a half, each
    announced by name. They play at sp_skill, and below skill 4 with Quake
    III's handicap: 50, 70 or 90 health at most, and damage scaled to match.
  - Rewards: "excellent" for two frags inside three seconds, "impressive" for
    two railgun hits in a row, "gauntlet" for a gauntlet frag ("humiliation"
    for whoever it was), counted the way Quake III counted them, along with
    the shots and hits its accuracy medal is made from. The lead changes and
    the frags-left warnings. Each goes to the client as an "sp_event" line,
    which shows the medal and plays the voice (q3sp.c).
  - At the end: the first intermission camera, no scoreboard, the top three
    on Quake III's victory podium, and the result ("sp_event postgame ...")
    for the client's postgame screen. The level does not move on by itself;
    the screen's buttons do that.

==============================================================================
*/

#include "g_local.h"
#include "shared/bot_roster.h"

#define SP_MAX_ARENA_BOTS       8
#define SP_BOT_DELAY_BASE       2.0f        // BOT_BEGIN_DELAY_BASE, seconds
#define SP_BOT_DELAY_TRAINING   10.0f       // added in the training arena
#define SP_BOT_DELAY_STEP       1.5f        // BOT_BEGIN_DELAY_INCREMENT
#define SP_CARNAGE_TIME         3.0f        // CARNAGE_REWARD_TIME
#define SP_PODIUM_MODEL         "models/mapobjects/podium/podium4.md3"
#define SP_PODIUM_DIST          80          // g_podiumDist
#define SP_PODIUM_DROP          70          // g_podiumDrop
#define RANK_TIED_FLAG          0x4000

cvar_t  *sp_campaign;       // 1: this map is an arena of the campaign
cvar_t  *sp_skill;          // its skill, 1 "I Can Win" to 5 "NIGHTMARE!"

static struct {
    bool    active;                 // this level is a campaign arena
    bool    training, final;        // special "training" / "final"
    char    longname[64];
    char    bots[SP_MAX_ARENA_BOTS][32];
    int     numbots;
    int     fraglimit, timelimit;

    int     fragwarnings;           // 1, 2, 4: three, two, one frags left said
    int     timewarnings;           // 1, 2, 4: five minutes, one minute, sudden death
    int     rank[MAX_CLIENTS];      // each human's last rank, -1 not yet
    bool    rewarded[MAX_CLIENTS];  // a reward went out this frame
    bool    reported;

    edict_t *podium;
    edict_t *bodies[3];
    float   podium_time;
} sp;

/*
==============================================================================

ARENAS

==============================================================================
*/

// the value of `key` in the next arena block of `*data`, every key at once
typedef struct {
    char    map[MAX_QPATH];
    char    longname[64];
    char    bots[256];
    char    type[64];
    char    special[32];
    int     fraglimit, timelimit;
} sp_arena_t;

static bool SP_ParseArena(const char **data, sp_arena_t *a)
{
    char    key[MAX_TOKEN_CHARS];
    char    *tok;

    memset(a, 0, sizeof(*a));

    tok = COM_Parse(data);
    if (!*data || tok[0] != '{')
        return false;

    while (1) {
        tok = COM_Parse(data);
        if (!*data)
            return false;
        if (tok[0] == '}')
            return true;
        Q_strlcpy(key, tok, sizeof(key));
        tok = COM_Parse(data);
        if (!*data)
            return false;

        if (!Q_stricmp(key, "map"))
            Q_strlcpy(a->map, tok, sizeof(a->map));
        else if (!Q_stricmp(key, "longname"))
            Q_strlcpy(a->longname, tok, sizeof(a->longname));
        else if (!Q_stricmp(key, "bots"))
            Q_strlcpy(a->bots, tok, sizeof(a->bots));
        else if (!Q_stricmp(key, "type"))
            Q_strlcpy(a->type, tok, sizeof(a->type));
        else if (!Q_stricmp(key, "special"))
            Q_strlcpy(a->special, tok, sizeof(a->special));
        else if (!Q_stricmp(key, "fraglimit"))
            a->fraglimit = atoi(tok);
        else if (!Q_stricmp(key, "timelimit"))
            a->timelimit = atoi(tok);
    }
}

// a single player arena called `map` in this file's text
static bool SP_FindArenaIn(const char *path, const char *map, sp_arena_t *out)
{
    void        *buf = NULL;
    const char  *data;
    char        *text;
    int         len;
    bool        found = false;

    if (!gi.LoadFile)
        return false;
    len = gi.LoadFile(path, &buf);
    if (len <= 0 || !buf)
        return false;

    // the loader's buffer is not terminated
    text = gi.TagMalloc(len + 1, TAG_GAME);
    memcpy(text, buf, len);
    text[len] = 0;
    gi.FreeFile(buf);

    data = text;
    while (SP_ParseArena(&data, out)) {
        if (Q_stricmp(out->map, map))
            continue;
        if (!strstr(out->type, "single"))
            continue;
        found = true;
        break;
    }

    gi.TagFree(text);
    return found;
}

static bool SP_FindArena(const char *map, sp_arena_t *out)
{
    // Quake III's own copy first: "baseq3/" reads only from its paks
    return SP_FindArenaIn("baseq3/scripts/arenas.txt", map, out) ||
           SP_FindArenaIn("scripts/arenas.txt", map, out);
}

/*
=================
SP_LevelRules

From Q3_LevelRules, as the level starts: is it a campaign arena? If so,
Quake III's weapons and movement, and its bots and limits from arenas.txt.
=================
*/
void SP_LevelRules(void)
{
    sp_arena_t  a;
    const char  *s;
    char        *tok;
    int         i;

    memset(&sp, 0, sizeof(sp));
    for (i = 0; i < MAX_CLIENTS; i++)
        sp.rank[i] = -1;

    if (!sp_campaign || !sp_campaign->integer || !deathmatch->value || !level.q3map)
        return;
    if (gi.cvar("fs_quake3", "0", 0)->integer != 1)
        return;
    if (!SP_FindArena(level.mapname, &a)) {
        gi.dprintf("Campaign: %s is not one of Quake III's arenas\n", level.mapname);
        return;
    }

    sp.active = true;
    sp.training = !Q_stricmp(a.special, "training");
    sp.final = !Q_stricmp(a.special, "final");
    Q_strlcpy(sp.longname, a.longname, sizeof(sp.longname));
    sp.fraglimit = max(a.fraglimit, 0);
    sp.timelimit = max(a.timelimit, 0);
    if (!sp.fraglimit && !sp.timelimit)
        sp.fraglimit = 10;

    s = a.bots;
    while (sp.numbots < SP_MAX_ARENA_BOTS) {
        tok = COM_Parse(&s);
        if (!s && !*tok)
            break;
        if (!*tok)
            continue;
        Q_strlcpy(sp.bots[sp.numbots++], tok, sizeof(sp.bots[0]));
        if (!s)
            break;
    }

    level.q3_weapons = 1;
    level.q3_physics = true;

    gi.dprintf("Campaign: %s \"%s\", %d bot%s, fraglimit %d, timelimit %d, skill %d\n",
               level.mapname, sp.longname, sp.numbots, sp.numbots == 1 ? "" : "s",
               sp.fraglimit, sp.timelimit, SP_Skill());
}

bool SP_Active(void)
{
    return sp.active;
}

int SP_Skill(void)
{
    return sp_skill ? max(1, min(5, sp_skill->integer)) : 2;
}

float G_FragLimit(void)
{
    return sp.active ? sp.fraglimit : fraglimit->value;
}

float G_TimeLimit(void)
{
    return sp.active ? sp.timelimit : timelimit->value;
}

/*
==============================================================================

THE BOTS

==============================================================================
*/

int SP_NumBots(void)
{
    return sp.active ? sp.numbots : 0;
}

// Quake III's bot of this name ("daemia"), as a skin of ours ("major/daemia")
const char *SP_BotSkin(int i)
{
    int j;

    if (i < 0 || i >= sp.numbots)
        return NULL;
    for (j = 0; j < BOT_Q3_ROSTER_SIZE; j++)
        if (!Q_stricmp(bot_q3_roster[j].name, sp.bots[i]))
            return bot_q3_roster[j].skin;
    return NULL;
}

// seconds after you are in that bot i joins
float SP_BotDelay(int i)
{
    return SP_BOT_DELAY_BASE + (sp.training ? SP_BOT_DELAY_TRAINING : 0) + i * SP_BOT_DELAY_STEP;
}

/*
The bots' level (g_bot.c bot_skills, the rerelease's six: Practice, Easy,
Medium, Hard, Expert, Nightmare) for Quake III's five. They went straight
across, one up from where they belong - "Bring It On" played Medium bots and
nothing ever played Practice (Matt, 11-19: "slaughtered in under 2 minutes" on
q3dm0). Hurt Me Plenty, Quake III's own default, is Medium; Nightmare is
Nightmare, the cliff it was in Quake III, so Expert goes unused.
*/
int SP_BotSkillLevel(void)
{
    static const int level[5] = { 0, 1, 2, 3, 5 };

    return level[SP_Skill() - 1];
}

// G_AddBot: the handicap a bot of this skill plays with
int SP_BotHandicap(void)
{
    int skill = SP_Skill();

    return skill <= 1 ? 50 : skill == 2 ? 70 : skill == 3 ? 90 : 100;
}

static bool SP_IsHuman(const edict_t *ent)
{
    return ent && ent->inuse && ent->client && ent->client->pers.connected && !Bot_IsBot(ent);
}

// one "sp_event" line to every human (a bot has no client to run it)
static void SP_Event(edict_t *only, const char *fmt, ...)
{
    char    line[MAX_STRING_CHARS];
    va_list argptr;
    int     i;

    va_start(argptr, fmt);
    Q_vsnprintf(line, sizeof(line), fmt, argptr);
    va_end(argptr);

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = g_edicts + 1 + i;

        if (only && e != only)
            continue;
        if (!SP_IsHuman(e))
            continue;
        gi.WriteByte(svc_stufftext);
        gi.WriteString(va("sp_event %s\n", line));
        gi.unicast(e, true);
    }
}

// a name for a command line: no quotes, separators or expansions
static const char *SP_SafeName(const char *name)
{
    static char buf[4][32];
    static int  n;
    char        *out = buf[n++ & 3];
    int         i;

    for (i = 0; name && name[i] && i < 31; i++) {
        char c = name[i];
        out[i] = (c == '"' || c == ';' || c == '$' || c == '\\' || c < 32) ? '_' : c;
    }
    out[i] = 0;
    return out;
}

/*
PlayerIntroSound: the announcer's name for a model and skin - the skin's,
unless it is "default", then the model's ("major/daemia" daemia,
"sarge/default" sarge).
*/
static const char *SP_VoiceName(const char *skin)
{
    static char buf[MAX_QPATH];
    const char  *slash = strchr(skin, '/');

    if (!slash || !Q_stricmp(slash + 1, "default")) {
        Q_strlcpy(buf, skin, sizeof(buf));
        if ((slash = strchr(buf, '/')) != NULL)
            *(char *)slash = 0;
        return buf;
    }
    Q_strlcpy(buf, slash + 1, sizeof(buf));
    return buf;
}

// a campaign bot is in: the announcer says who
void SP_BotJoined(const char *skin)
{
    if (sp.active && skin)
        SP_Event(NULL, "announce %s", SP_VoiceName(skin));
}

/*
=================
SP_SpawnHealth

PutClientInServer, on a fresh spawn: a handicapped bot's health and its cap
(Quake III's STAT_MAX_HEALTH is the handicap).
=================
*/
void SP_SpawnHealth(edict_t *ent)
{
    gclient_t   *client = ent->client;
    int         handicap;

    if (!sp.active || !Bot_IsBot(ent))
        return;

    handicap = SP_BotHandicap();
    if (handicap >= 100)
        return;

    client->pers.health = max(1, client->pers.health * handicap / 100);
    client->pers.max_health = handicap;
}

// G_Damage: an attacker's damage scaled by its handicap
int SP_ScaleDamage(edict_t *targ, edict_t *attacker, int damage)
{
    int handicap;

    if (!sp.active || !attacker || attacker == targ || !attacker->client || !Bot_IsBot(attacker))
        return damage;

    handicap = SP_BotHandicap();
    if (handicap >= 100 || damage <= 0)
        return damage;
    return max(1, damage * handicap / 100);
}

/*
==============================================================================

REWARDS AND ACCURACY

==============================================================================
*/

// LogAccuracyHit: a hit on someone else, alive and playing
bool SP_AccuracyTarget(const edict_t *attacker, const edict_t *target)
{
    if (!sp.active || !attacker || !attacker->client || !target || target == attacker)
        return false;
    if (!target->client || !target->inuse || target->health <= 0 || target->deadflag)
        return false;
    if (level.intermission_framenum)
        return false;
    return true;
}

// a shot (every gun but the gauntlet)
void SP_AccuracyShot(edict_t *ent)
{
    if (sp.active && ent->client)
        ent->client->resp.sp_shots++;
}

void SP_AccuracyHit(edict_t *ent)
{
    if (sp.active && ent && ent->client)
        ent->client->resp.sp_hits++;
}

static void SP_Reward(edict_t *ent, const char *award, int count)
{
    int i = ent - g_edicts - 1;

    if (i >= 0 && i < MAX_CLIENTS)
        sp.rewarded[i] = true;
    SP_Event(ent, "reward %s %d", award, count);
}

/*
=================
SP_RailHits

weapon_railgun_fire: how many it went through. None ends the streak; else
they add up, and every two are an "impressive".
=================
*/
void SP_RailHits(edict_t *ent, int hits)
{
    client_respawn_t *resp;

    if (!sp.active || !ent->client)
        return;

    resp = &ent->client->resp;
    if (!hits) {
        resp->sp_accurate = 0;
        return;
    }

    resp->sp_accurate += hits;
    if (resp->sp_accurate >= 2) {
        resp->sp_accurate -= 2;
        resp->sp_impressive++;
        SP_Reward(ent, "impressive", resp->sp_impressive);
    }
    resp->sp_hits++;
}

/*
=================
SP_PlayerKilled

player_die, after the obituary has scored it.
=================
*/
void SP_PlayerKilled(edict_t *self, edict_t *inflictor, edict_t *attacker)
{
    client_respawn_t    *resp;
    int                 mod = meansOfDeath & ~MOD_FRIENDLY_FIRE;

    if (!sp.active || level.intermission_framenum)
        return;

    if (self->client)
        self->client->resp.sp_deaths++;

    if (!attacker || attacker == self || !attacker->client)
        return;

    resp = &attacker->client->resp;

    if (mod == MOD_Q3_GAUNTLET) {
        resp->sp_gauntlet++;
        SP_Reward(attacker, "gauntlet", resp->sp_gauntlet);
        SP_Event(self, "humiliation");
    }

    if (resp->sp_last_kill && level.time - resp->sp_last_kill < SP_CARNAGE_TIME) {
        resp->sp_excellent++;
        SP_Reward(attacker, "excellent", resp->sp_excellent);
    }
    resp->sp_last_kill = level.time;
}

/*
==============================================================================

THE MATCH

==============================================================================
*/

// CalculateRanks for one client: how many are ahead, and a tie
static int SP_Rank(const edict_t *ent, bool *tied)
{
    int i, ahead = 0;

    *tied = false;
    for (i = 0; i < game.maxclients; i++) {
        const edict_t *e = g_edicts + 1 + i;

        if (e == ent || !e->inuse || !e->client || !e->client->pers.connected)
            continue;
        if (e->client->pers.spectator)
            continue;
        if (e->client->resp.score > ent->client->resp.score)
            ahead++;
        else if (e->client->resp.score == ent->client->resp.score)
            *tied = true;
    }
    return ahead;
}

static int SP_HighScore(void)
{
    int     i, best = 0;
    bool    any = false;

    for (i = 0; i < game.maxclients; i++) {
        const edict_t *e = g_edicts + 1 + i;

        if (!e->inuse || !e->client || !e->client->pers.connected || e->client->pers.spectator)
            continue;
        if (!any || e->client->resp.score > best)
            best = e->client->resp.score;
        any = true;
    }
    return best;
}

/*
=================
SP_RunFrame

G_RunFrame, after the rules: the lead changes, and the frags-left and time
warnings (cg_event.c CG_CheckLocalSounds).
=================
*/
void SP_RunFrame(void)
{
    int i, high, left;

    if (!sp.active)
        return;

    if (sp.podium)
        SP_PodiumThink();

    if (level.intermission_framenum) {
        memset(sp.rewarded, 0, sizeof(sp.rewarded));
        return;
    }

    high = SP_HighScore();

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = g_edicts + 1 + i;
        bool    tied;
        int     rank;

        if (!SP_IsHuman(e) || e->client->pers.spectator) {
            sp.rank[i] = -1;
            continue;
        }

        rank = SP_Rank(e, &tied);
        rank |= tied ? RANK_TIED_FLAG : 0;

        // nobody has scored: everyone level at nothing is no lead to speak of
        if (high > 0 && sp.rank[i] >= 0 && rank != sp.rank[i] && !sp.rewarded[i]) {
            if (rank == 0)
                SP_Event(e, "lead taken");
            else if (rank == RANK_TIED_FLAG)
                SP_Event(e, "lead tied");
            else if ((sp.rank[i] & ~RANK_TIED_FLAG) == 0)
                SP_Event(e, "lead lost");
        }
        sp.rank[i] = rank;
    }
    memset(sp.rewarded, 0, sizeof(sp.rewarded));

    if (sp.fraglimit > 0) {
        left = sp.fraglimit - high;
        if (!(sp.fragwarnings & 4) && left == 1) {
            sp.fragwarnings |= 1 | 2 | 4;
            SP_Event(NULL, "fragsleft 1");
        } else if (sp.fraglimit > 2 && !(sp.fragwarnings & 2) && left == 2) {
            sp.fragwarnings |= 1 | 2;
            SP_Event(NULL, "fragsleft 2");
        } else if (sp.fraglimit > 3 && !(sp.fragwarnings & 1) && left == 3) {
            sp.fragwarnings |= 1;
            SP_Event(NULL, "fragsleft 3");
        }
    }

    // (no "sudden death": that is Quake III's overtime, and here the time just ends)
    if (sp.timelimit > 0) {
        float left_s = sp.timelimit * 60.0f - level.time;

        if (!(sp.timewarnings & 2) && left_s < 60) {
            sp.timewarnings |= 1 | 2;
            SP_Event(NULL, "timeleft 1");
        } else if (sp.timelimit > 5 && !(sp.timewarnings & 1) && left_s < 300) {
            sp.timewarnings |= 1;
            SP_Event(NULL, "timeleft 5");
        }
    }
}

/*
==============================================================================

THE END: PODIUM AND RESULT

==============================================================================
*/

// a ahead of b: the higher score; level, you ahead of a bot (a tie for first is your win)
static bool SP_Ahead(const edict_t *a, const edict_t *b)
{
    if (a->client->resp.score != b->client->resp.score)
        return a->client->resp.score > b->client->resp.score;
    return SP_IsHuman(a) && !SP_IsHuman(b);
}

// the players, best first; out holds MAX_CLIENTS
static int SP_SortedPlayers(edict_t **out)
{
    int i, j, n = 0;

    for (i = 0; i < game.maxclients && n < MAX_CLIENTS; i++) {
        edict_t *e = g_edicts + 1 + i;

        if (!e->inuse || !e->client || !e->client->pers.connected || e->client->pers.spectator)
            continue;
        for (j = n; j > 0 && SP_Ahead(e, out[j - 1]); j--)
            out[j] = out[j - 1];
        out[j] = e;
        n++;
    }
    return n;
}

static void SP_PlaceOnPodium(edict_t *player, int place)
{
    static const vec3_t offsets[3] = {
        { 0, 0, 74 },       // offsetFirst
        { -10, 60, 54 },    // offsetSecond
        { -19, -60, 45 },   // offsetThird
    };
    vec3_t  f, r, u, org, dir;
    edict_t *body;

    AngleVectors(sp.podium->s.angles, f, r, u);
    VectorMA(sp.podium->s.origin, offsets[place][0], f, org);
    VectorMA(org, offsets[place][1], r, org);
    VectorMA(org, offsets[place][2], u, org);

    body = G_Spawn();
    body->classname = "podium_player";
    body->movetype = MOVETYPE_NONE;
    body->solid = SOLID_NOT;
    // drawn as that player - their model, skin and the gun in hand
    body->s.modelindex = 255;
    body->s.modelindex2 = player->s.modelindex2 ? player->s.modelindex2 : 255;
    body->s.skinnum = player->s.skinnum;
    body->s.frame = 0;
    VectorCopy(org, body->s.origin);
    VectorCopy(org, body->s.old_origin);

    VectorSubtract(level.intermission_origin, org, dir);
    body->s.angles[YAW] = vectoyaw(dir);
    gi.linkentity(body);

    sp.bodies[place] = body;
}

/*
=================
SP_BeginIntermission

BeginIntermission, before the players are moved: Quake III's podium in front
of the camera, the first three on it facing it (SpawnModelsOnVictoryPads).
=================
*/
void SP_BeginIntermission(void)
{
    edict_t *sorted[MAX_CLIENTS];
    vec3_t  f, org, dir;
    int     i, n;

    if (!sp.active)
        return;

    AngleVectors(level.intermission_angle, f, NULL, NULL);
    VectorMA(level.intermission_origin, SP_PODIUM_DIST, f, org);
    org[2] -= SP_PODIUM_DROP;

    sp.podium = G_Spawn();
    sp.podium->classname = "podium";
    sp.podium->movetype = MOVETYPE_NONE;
    sp.podium->solid = SOLID_NOT;
    sp.podium->s.modelindex = gi.modelindex(SP_PODIUM_MODEL);
    VectorCopy(org, sp.podium->s.origin);
    VectorCopy(org, sp.podium->s.old_origin);
    VectorSubtract(level.intermission_origin, org, dir);
    sp.podium->s.angles[YAW] = vectoyaw(dir);
    gi.linkentity(sp.podium);
    sp.podium_time = level.time;

    n = SP_SortedPlayers(sorted);
    for (i = 0; i < n && i < 3; i++) {
        if (i == 2 && n <= 2)
            break;
        SP_PlaceOnPodium(sorted[i], i);
    }
}

/*
=================
SP_PodiumThink

Every frame of the intermission: the bodies stand (frames 0-39, Quake II's
stand, which a Quake III model plays as its idle); two seconds in, the
winner gestures (a taunt frame starts its gesture, q3player.c), then stands.
=================
*/
void SP_PodiumThink(void)
{
    float   t = level.time - sp.podium_time;
    int     i;

    for (i = 0; i < 3; i++) {
        edict_t *b = sp.bodies[i];

        if (!b || !b->inuse)
            continue;
        if (i == 0 && t >= 2.0f && t < 2.0f + 2.294f)
            b->s.frame = 95 + min((int)((t - 2.0f) * 10), 111 - 95);
        else
            b->s.frame = (int)(t * 10) % 40;
    }
}

/*
=================
SP_Intermission

BeginIntermission, after the players are at the camera: the result for the
postgame screen (UpdateTournamentInfo). For each human:

  sp_event postgame <map> <place> <tied> <accuracy> <impressive> <excellent>
                    <gauntlet> <frags> <perfect> <seconds> <skill> <winner voice>
                    <players> "<first>" "<second>" "<third>"
=================
*/
void SP_Intermission(void)
{
    edict_t *sorted[MAX_CLIENTS];
    int     i, n;

    if (!sp.active || sp.reported)
        return;
    sp.reported = true;

    n = SP_SortedPlayers(sorted);

    for (i = 0; i < game.maxclients; i++) {
        edict_t             *e = g_edicts + 1 + i;
        client_respawn_t    *resp;
        const char          *voice;
        bool                tied;
        int                 rank, accuracy, perfect;

        if (!SP_IsHuman(e) || e->client->pers.spectator)
            continue;

        resp = &e->client->resp;
        rank = SP_Rank(e, &tied);
        accuracy = resp->sp_shots ? resp->sp_hits * 100 / resp->sp_shots : 0;
        perfect = (rank == 0 && !tied && resp->sp_deaths == 0) ? 1 : 0;

        // the winner, as the announcer has it: you (a tie for first is a win), or that bot
        if (rank == 0 || (n && sorted[0] == e))
            voice = "you";
        else if (n && sorted[0]->client)
            voice = SP_VoiceName(Info_ValueForKey(sorted[0]->client->pers.userinfo, "skin"));
        else
            voice = "-";

        SP_Event(e, "postgame %s %d %d %d %d %d %d %d %d %d %d %s %d \"%s\" \"%s\" \"%s\"",
                 level.mapname, rank + 1, tied ? 1 : 0, accuracy, resp->sp_impressive,
                 resp->sp_excellent, resp->sp_gauntlet, resp->score, perfect,
                 (int)level.time, SP_Skill(), voice, n,
                 n > 0 ? SP_SafeName(sorted[0]->client->pers.netname) : "",
                 n > 1 ? SP_SafeName(sorted[1]->client->pers.netname) : "",
                 n > 2 ? SP_SafeName(sorted[2]->client->pers.netname) : "");
    }
}

void SP_Init(void)
{
    sp_campaign = gi.cvar("sp_campaign", "0", 0);
    sp_skill = gi.cvar("sp_skill", "2", 0);
    memset(&sp, 0, sizeof(sp));
}
