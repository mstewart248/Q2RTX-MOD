/*
Copyright (C) 1999-2005 Id Software, Inc.

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

/*
==============================================================================

QUAKE III TEAM ARENA

The expansion's maps played as its own game (Matt, 11-25): a map out of
baseq3/missionpack's paks is level.ta_map (the server's sv_map_q3 2), and
runs as a Quake III map (g_q3.c) with what Team Arena adds:

  - its game types, by each entity's "gametype" key - "ffa", "team", "ctf",
    "oneflag", "obelisk", "harvester" (ioq3 g_spawn.c gametypeNames) - and
    its "notta" key. A CTF game's mode is `ctf` 1-4 (level.ta_mode):
      1  Capture the Flag
      2  One Flag CTF: the white flag in the middle, taken to the other
         team's flag stand (g_ctf.c)
      3  Overload: each team's obelisk, 2500 health, healed 15 a second;
         bringing the other team's down scores, and it stands again 10 s on
      4  Harvester: each death throws a skull of the victim's colour out of
         the obelisk in the middle; the other colour's skulls, taken to the
         other team's obelisk, score one each
    The techs are off on its maps: the persistent powerups take their place.
  - the Kamikaze and the Invulnerability, held with Quake III's holdables
  - the persistent powerups - Scout, Guard, Doubler, Ammo Regen - one at a
    time, kept until death, when it goes back to its spot
  - three guns, the Nailgun, Prox Launcher and Chaingun (g_q3weap.c)

The numbers are ioq3's (code/game built with MISSIONPACK): g_team.c,
g_items.c, g_active.c, g_weapon.c, g_missile.c. The scores are CTF's here
(15 for a capture, 10 for each teammate), as Quake III's maps play CTF.

==============================================================================
*/

#include "g_local.h"

#define TA_OBELISK_HEALTH       2500    // g_obeliskHealth
#define TA_OBELISK_REGEN        15      // g_obeliskRegenAmount, a second
#define TA_OBELISK_RESPAWN      10      // g_obeliskRespawnDelay, seconds
#define TA_OBELISK_HEIGHT       87
#define TA_ATTACK_SOUND_TIME    20      // OVERLOAD_ATTACK_BASE_SOUND_TIME, seconds
#define TA_CUBE_TIMEOUT         30      // g_cubeTimeout
#define TA_INVUL_TIME           10      // seconds
#define TA_CAPTURE_BONUS        15      // CTF's (g_ctf.c CTF_CAPTURE_BONUS)

// bg_public.h's kamikaze, in ms from the start
#define KAMI_SHOCKWAVE_STARTTIME    0
#define KAMI_SHOCKWAVE_ENDTIME      2000
#define KAMI_EXPLODE_STARTTIME      250
#define KAMI_IMPLODE_STARTTIME      2000
#define KAMI_SHOCKWAVE_MAXRADIUS    1320
#define KAMI_BOOMSPHERE_MAXRADIUS   720

static struct {
    edict_t *obelisk[3];            // [team]; [TEAM_NONE] is Harvester's middle one
    float   attacked_time[3];       // the "base under attack" call, by team
    gitem_t *cube[3];               // [TEAM_RED] item_ta_redcube, [TEAM_BLUE] blue
    gitem_t *persistent[TA_PERSISTENT_NUM];
    gitem_t *kamikaze;
    gitem_t *invulnerability;
    bool    skirmish_over;          // the skirmish has sent you back to its menu

    // Team Arena's HUD (TA_RunFrame)
    vec3_t  location[MAX_TA_LOCATIONS];     // the map's target_locations, CS_TA_LOCATIONS order
    int     num_locations;
    char    hud[MAX_QPATH];         // CS_TA_HUD as last sent
    float   tinfo_time;             // the next "ta_tinfo"

    // its voice chats (TA_Voice): the bots' calls, by client slot
    struct {
        int     enterframe;         // resp.enterframe the rest is for
        float   wish_time;          // "let me go on offense", 0 = said
        float   status_time;        // "I'm on offense", 0 = said
        int     task;               // the job it called for
    } bot_voice[MAX_CLIENTS];
    float   enemy_flag_time[3];     // "the enemy has our flag", by team
    float   voice_time[MAX_CLIENTS];    // a player's last vsay, for the flood guard
} ta;

static int TA_OtherTeam(int team)
{
    return team == TEAM_RED ? TEAM_BLUE : team == TEAM_BLUE ? TEAM_RED : TEAM_NONE;
}

static int TA_Team(const edict_t *ent)
{
    return ent && ent->client ? ent->client->resp.team : TEAM_NONE;
}

// what only this player hears (a bot hears nothing)
static void TA_LocalSound(edict_t *ent, const char *sound)
{
    if (!ent || !ent->inuse || !ent->client || Bot_IsBot(ent))
        return;
    gi.WriteByte(svc_stufftext);
    gi.WriteString(va("play #baseq3/sound/%s\n", sound));
    gi.unicast(ent, true);
}

// ... every player on a team (TEAM_NONE: everyone)
static void TA_TeamSound(int team, const char *sound)
{
    int i;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || !e->client || (team != TEAM_NONE && e->client->resp.team != team))
            continue;
        TA_LocalSound(e, sound);
    }
}

static void TA_BaseAttacked(int team);

// g_team.c AddTeamScore's call: who scored, who leads
static void TA_ScoreSound(int team)
{
    TA_TeamSound(TEAM_NONE, team == TEAM_RED ? "teamplay/voc_red_scores.wav" : "teamplay/voc_blue_scores.wav");
}

/*
==============================================================================

GAME TYPES

==============================================================================
*/

/*
=================
TA_GameMode

SpawnEntities, before the map's entities: which of Team Arena's games `ctf`
asks for. Its own maps only - the others have no obelisks or white flag - and
plain CTF everywhere else.
=================
*/
int TA_GameMode(void)
{
    int mode;

    if (!G_CTF())
        return 0;
    mode = (int)ctf->value;
    if (!level.ta_map || mode < TA_MODE_CTF || mode > TA_MODE_HARVESTER)
        return TA_MODE_CTF;
    return mode;
}

// ioq3 gametypeNames, for this game
const char *TA_GametypeName(void)
{
    if (!deathmatch->value || SP_Active())
        return "single";
    // a skirmish's Tournament: deathmatch against one bot
    if (TA_SkirmishActive() && gi.cvar("ta_gametype", "0", 0)->integer == 1)
        return "tournament";
    if (G_CTF()) {
        switch (level.ta_mode) {
        case TA_MODE_ONEFLAG:   return "oneflag";
        case TA_MODE_OVERLOAD:  return "obelisk";
        case TA_MODE_HARVESTER: return "harvester";
        default:                return "ctf";
        }
    }
    if (G_TeamPlay())
        return "team";
    return "ffa";
}

/*
=================
TA_SpawnFiltered

Q3_SpawnEntity, after "notfree" and "notteam": true when this entity is not
in this game (G_SpawnGEntityFromSpawnVars). "gametype" lists the games it is
in - a substring test, as Quake III's is - and Team Arena's own maps leave
out what says "notta".
=================
*/
bool TA_SpawnFiltered(void)
{
    if (st.gametype && st.gametype[0] && !strstr(st.gametype, TA_GametypeName()))
        return true;
    if (level.ta_map && st.notta)
        return true;
    return false;
}

/*
==============================================================================

OBELISKS

team_redobelisk and team_blueobelisk stand at the bases, team_neutralobelisk
in the middle. They are the bases' looks in CTF and One Flag (the flag stands,
cg_ents.c CG_TeamBase), the targets in Overload, and where the skulls come
from and go in Harvester (g_team.c SpawnObelisk).

==============================================================================
*/

static void TA_ObeliskLooks(edict_t *ob, bool alive)
{
    if (level.ta_mode != TA_MODE_OVERLOAD)
        return;
    // the base always; the lights and the target while it stands
    ob->s.modelindex2 = alive ? gi.modelindex("models/powerups/overload_lights.md3") : 0;
    if (ob->target_ent) {
        if (alive)
            ob->target_ent->svflags &= ~SVF_NOCLIENT;
        else
            ob->target_ent->svflags |= SVF_NOCLIENT;
    }
}

static void TA_ObeliskRegen(edict_t *self)
{
    self->nextthink = level.framenum + BASE_FRAMERATE;
    if (self->health >= TA_OBELISK_HEALTH)
        return;

    gi.sound(self, CHAN_AUTO, Q3_SoundIndex("items/regen.wav"), 1, ATTN_NORM, 0);
    self->health += TA_OBELISK_REGEN;
    if (self->health > TA_OBELISK_HEALTH)
        self->health = TA_OBELISK_HEALTH;
    self->s.modelindex3 = 0;    // the hit glow off
}

static void TA_ObeliskRespawn(edict_t *self)
{
    self->takedamage = DAMAGE_YES;
    self->health = TA_OBELISK_HEALTH;
    self->think = TA_ObeliskRegen;
    self->nextthink = level.framenum + BASE_FRAMERATE;
    TA_ObeliskLooks(self, true);
}

// half way back: the respawn sound (cg_ents.c plays it when the target shows)
static void TA_ObeliskRising(edict_t *self)
{
    gi.sound(self, CHAN_AUTO, Q3_SoundIndex("items/obelisk_respawn.wav"), 1, ATTN_NORM, 0);
    self->think = TA_ObeliskRespawn;
    self->nextthink = level.framenum + (TA_OBELISK_RESPAWN - 5) * BASE_FRAMERATE;
}

static void TA_ObeliskPain(edict_t *self, edict_t *other, float kick, int damage)
{
    int points = damage / 10;

    if (points <= 0)
        points = 1;
    if (other && other->client)
        other->client->resp.score += points;

    // EV_OBELISKPAIN, once until the next heal
    if (!self->s.modelindex3) {
        static const char *const hits[3] = {
            "items/obelisk_hit_01.wav", "items/obelisk_hit_02.wav", "items/obelisk_hit_03.wav"
        };
        gi.sound(self, CHAN_AUTO, Q3_SoundIndex(hits[Q_rand() % 3]), 1, ATTN_NORM, 0);
    }
    self->s.modelindex3 = gi.modelindex("models/powerups/overload_energy.md3");
}

static void TA_ObeliskDie(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     other = TA_OtherTeam(self->ta_team);
    vec3_t  pos;

    CTF_AddTeamScore(other, 1);
    TA_ScoreSound(other);

    self->takedamage = DAMAGE_NO;
    self->health = 0;
    self->s.modelindex3 = 0;
    self->think = TA_ObeliskRising;
    self->nextthink = level.framenum + 5 * BASE_FRAMERATE;
    TA_ObeliskLooks(self, false);

    // EV_OBELISKEXPLODE: cg_effects.c CG_ObeliskExplode's blast, at the top
    VectorCopy(self->s.origin, pos);
    pos[2] += 56;
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_ROCKET_EXPLOSION);
    gi.WritePosition(pos);
    gi.multicast(pos, MULTICAST_PHS);

    if (attacker && attacker->client) {
        attacker->client->resp.score += TA_CAPTURE_BONUS;
        gi.bprintf(PRINT_HIGH, "%s destroyed the %s obelisk!\n", attacker->client->pers.netname,
                   Team_Name(self->ta_team));
    }
    ta.attacked_time[TEAM_RED] = ta.attacked_time[TEAM_BLUE] = 0;
}

// Harvester: the skulls you carry, brought into the other team's obelisk
// (g_team.c ObeliskTouch)
static void TA_ObeliskTouch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    int tokens, team;

    if (!other->client || other->health <= 0)
        return;
    team = other->client->resp.team;
    if (TA_OtherTeam(team) != self->ta_team)
        return;
    tokens = other->client->ta_skulls;
    if (tokens <= 0)
        return;

    gi.bprintf(PRINT_HIGH, "%s brought in %d skull%s.\n", other->client->pers.netname, tokens,
               tokens != 1 ? "s" : "");
    CTF_AddTeamScore(team, tokens);
    other->client->resp.score += TA_CAPTURE_BONUS * tokens;
    other->client->ta_skulls = 0;
    gi.sound(self, CHAN_RELIABLE | CHAN_NO_PHS_ADD | CHAN_ITEM, gi.soundindex("ctf/flagcap.wav"), 1, ATTN_NONE, 0);
    TA_ScoreSound(team);
}

// g_team.c SpawnObelisk: on the floor, a pixel up, unless "suspended"
static void TA_DropObelisk(edict_t *ob, bool suspended)
{
    vec3_t  dest;
    trace_t tr;

    if (suspended)
        return;

    ob->s.origin[2] += 1;
    VectorCopy(ob->s.origin, dest);
    dest[2] -= 4096;
    tr = gi.trace(ob->s.origin, ob->mins, ob->maxs, dest, ob, MASK_SOLID);
    if (tr.startsolid) {
        ob->s.origin[2] -= 1;
        gi.dprintf("SpawnObelisk: %s startsolid at %s\n", ob->classname, vtos(ob->s.origin));
    } else {
        VectorCopy(tr.endpos, ob->s.origin);
    }
}

static void TA_SpawnObelisk(edict_t *ent, int team)
{
    bool suspended = (ent->spawnflags & 1) != 0;

    ent->ta_team = team;
    ent->movetype = MOVETYPE_NONE;
    ent->flags |= FL_NO_KNOCKBACK;
    VectorSet(ent->mins, -15, -15, 0);
    VectorSet(ent->maxs, 15, 15, TA_OBELISK_HEIGHT);
    ent->spawnflags = 0;

    if (level.ta_mode == TA_MODE_OVERLOAD) {
        edict_t *target;

        ent->classname = "ta_obelisk";
        ent->solid = SOLID_BBOX;
        ent->takedamage = DAMAGE_YES;
        ent->health = ent->max_health = TA_OBELISK_HEALTH;
        ent->pain = TA_ObeliskPain;
        ent->die = TA_ObeliskDie;
        ent->think = TA_ObeliskRegen;
        ent->nextthink = level.framenum + BASE_FRAMERATE;
        ent->s.modelindex = gi.modelindex("models/powerups/overload_base.md3");
        gi.modelindex("models/powerups/overload_energy.md3");
        TA_DropObelisk(ent, suspended);

        // the target over it, 56 up (cg_ents.c CG_TeamBase)
        target = G_Spawn();
        target->classname = "ta_obelisk_target";
        VectorCopy(ent->s.origin, target->s.origin);
        VectorCopy(ent->s.angles, target->s.angles);
        target->s.origin[2] += 56;
        target->s.modelindex = gi.modelindex("models/powerups/overload_target.md3");
        gi.linkentity(target);
        ent->target_ent = target;
        TA_ObeliskLooks(ent, true);

        Q3_SoundIndex("items/obelisk_hit_01.wav");
        Q3_SoundIndex("items/obelisk_hit_02.wav");
        Q3_SoundIndex("items/obelisk_hit_03.wav");
        Q3_SoundIndex("items/obelisk_respawn.wav");
        Q3_SoundIndex("items/regen.wav");
    } else {
        // Harvester: walked through; the team ones take the skulls
        ent->classname = "ta_harvester";
        ent->solid = team == TEAM_NONE ? SOLID_NOT : SOLID_TRIGGER;
        if (team != TEAM_NONE)
            ent->touch = TA_ObeliskTouch;
        if (team == TEAM_NONE) {
            ent->s.modelindex = gi.modelindex("models/powerups/obelisk/obelisk.md3");
        } else {
            // red.skin is the model's own; the client gives it blue.skin as skin 1
            ent->s.modelindex = gi.modelindex("models/powerups/harvester/harvester.md3");
            ent->s.skinnum = team == TEAM_BLUE ? 1 : 0;
        }
        TA_DropObelisk(ent, suspended);
    }

    ta.obelisk[team] = ent;
    gi.linkentity(ent);
}

// CTF and One Flag: the flag stand's look (CG_TeamBase); nothing to touch
static void TA_FlagBase(edict_t *ent, int team)
{
    static const char *const models[3] = {
        "models/mapobjects/flagbase/ntrl_base.md3",
        "models/mapobjects/flagbase/red_base.md3",
        "models/mapobjects/flagbase/blue_base.md3",
    };

    ent->classname = "ta_flagbase";
    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_NOT;
    ent->s.modelindex = gi.modelindex(models[team]);
    gi.linkentity(ent);
}

/*QUAKED team_redobelisk (1 0 0) (-16 -16 0) (16 16 8)
Team Arena: the red base - its obelisk (Overload), its skull target
(Harvester), its flag stand's look (CTF).
*/
static void SP_team_obelisk(edict_t *ent, int team)
{
    switch (level.ta_mode) {
    case TA_MODE_OVERLOAD:
    case TA_MODE_HARVESTER:
        TA_SpawnObelisk(ent, team);
        return;
    case TA_MODE_CTF:
    case TA_MODE_ONEFLAG:
        if (team != TEAM_NONE || level.ta_mode == TA_MODE_ONEFLAG) {
            TA_FlagBase(ent, team);
            return;
        }
        break;
    }
    G_FreeEdict(ent);
}

// Q3_SpawnEntity: Team Arena's own entities; true when it took this one
bool TA_SpawnEntity(edict_t *ent)
{
    const char *cn = ent->classname;

    if (!Q_stricmp(cn, "team_redobelisk")) {
        SP_team_obelisk(ent, TEAM_RED);
        return true;
    }
    if (!Q_stricmp(cn, "team_blueobelisk")) {
        SP_team_obelisk(ent, TEAM_BLUE);
        return true;
    }
    if (!Q_stricmp(cn, "team_neutralobelisk")) {
        SP_team_obelisk(ent, TEAM_NONE);
        return true;
    }
    return false;
}

/*
==============================================================================

HARVESTER'S SKULLS

==============================================================================
*/

static edict_t *TA_LaunchSkull(gitem_t *item, const vec3_t origin, const vec3_t velocity)
{
    edict_t *skull = G_Spawn();

    skull->classname = item->classname;
    skull->item = item;
    skull->spawnflags = DROPPED_ITEM;
    skull->s.effects = item->world_model_flags;
    skull->s.renderfx = RF_GLOW;
    VectorSet(skull->mins, -15, -15, -15);
    VectorSet(skull->maxs, 15, 15, 15);
    gi.setmodel(skull, item->world_model);
    skull->solid = SOLID_TRIGGER;
    skull->movetype = MOVETYPE_BOUNCE;     // Quake III's items bounce (G_BounceItem)
    skull->touch = Touch_Item;
    VectorCopy(origin, skull->s.origin);
    VectorCopy(velocity, skull->velocity);
    skull->think = G_FreeEdict;
    skull->nextthink = level.framenum + TA_CUBE_TIMEOUT * BASE_FRAMERATE;
    gi.linkentity(skull);
    return skull;
}

// g_team.c TossClientCubes: a skull of the victim's colour out of the middle
static void TA_TossSkull(edict_t *self)
{
    int     team = TA_Team(self);
    vec3_t  angles, velocity, origin;

    self->client->ta_skulls = 0;    // what they carried is lost
    if ((team != TEAM_RED && team != TEAM_BLUE) || !ta.cube[team])
        return;

    VectorSet(angles, 0, (float)(((int)(level.time * 1000)) % 360), 0);
    AngleVectors(angles, velocity, NULL, NULL);
    VectorScale(velocity, 150, velocity);
    velocity[2] += 200 + crandom() * 50;

    if (ta.obelisk[TEAM_NONE]) {
        VectorCopy(ta.obelisk[TEAM_NONE]->s.origin, origin);
        origin[2] += 44;
    } else {
        VectorClear(origin);
    }
    TA_LaunchSkull(ta.cube[team], origin, velocity);
}

// a skull touched is gone; one of the other colour is yours to bring in
bool TA_PickupSkull(edict_t *ent, edict_t *other)
{
    int skull_team = ent->item == ta.cube[TEAM_RED] ? TEAM_RED : TEAM_BLUE;

    if (level.ta_mode != TA_MODE_HARVESTER)
        return false;
    if (skull_team != TA_Team(other))
        other->client->ta_skulls++;
    return true;
}

/*
For the bots (g_bot.c): a team's obelisk - [TEAM_NONE] Harvester's middle
one - and what a thing is to them: the other team's Overload obelisk,
standing, to shoot; a Harvester skull lying about, and whose colour.
*/
edict_t *TA_Obelisk(int team)
{
    return team >= TEAM_NONE && team <= TEAM_BLUE ? ta.obelisk[team] : NULL;
}

bool TA_EnemyObelisk(const edict_t *self, const edict_t *ent)
{
    int enemy = TA_OtherTeam(TA_Team(self));

    return level.ta_mode == TA_MODE_OVERLOAD && ent && ent->inuse && enemy != TEAM_NONE &&
           ent == ta.obelisk[enemy] && ent->takedamage && ent->health > 0;
}

int TA_SkullTeam(const edict_t *ent)
{
    if (level.ta_mode != TA_MODE_HARVESTER || !ent->item)
        return TEAM_NONE;
    if (ent->item == ta.cube[TEAM_RED])
        return TEAM_RED;
    if (ent->item == ta.cube[TEAM_BLUE])
        return TEAM_BLUE;
    return TEAM_NONE;
}

/*
==============================================================================

THE PERSISTENT POWERUPS

==============================================================================
*/

int TA_Persistent(const edict_t *ent)
{
    const gclient_t *cl = ent ? ent->client : NULL;
    int             i;

    if (!cl || !cl->ta_persistent || !cl->ta_persistent->item)
        return TA_NONE;
    for (i = 1; i < TA_PERSISTENT_NUM; i++)
        if (cl->ta_persistent->item == ta.persistent[i])
            return i;
    return TA_NONE;
}

// the powerup back on its spot (TossClientPersistantPowerups)
static void TA_ReturnPersistent(edict_t *player)
{
    edict_t *powerup = player->client->ta_persistent;

    player->client->ta_persistent = NULL;
    if (!powerup || !powerup->inuse)
        return;
    powerup->svflags &= ~SVF_NOCLIENT;
    powerup->solid = SOLID_TRIGGER;
    gi.linkentity(powerup);
}

/*
=================
TA_PickupPersistent

g_items.c Pickup_PersistantPowerup and BG_CanItemBeGrabbed: one at a time,
some for one team only (spawnflags 2 red, 4 blue - Q3_SpawnEntity keeps them
as ta_team). It never comes back on its own: it leaves the map with its
holder and is back on its spot when they die.
=================
*/
bool TA_PickupPersistent(edict_t *ent, edict_t *other)
{
    gclient_t   *cl = other->client;
    int         kind;

    if (cl->ta_persistent)
        return false;
    if (ent->ta_team && TA_Team(other) != ent->ta_team)
        return false;

    cl->ta_persistent = ent;
    kind = TA_Persistent(other);

    other->max_health = cl->pers.max_health = 100;
    switch (kind) {
    case TA_GUARD: {
        // 200 health, 200 max, 200 armor (Quake III's one pool: combat armor,
        // or body armor if that is what is worn - Q3_PickupArmor)
        gitem_t *combat = FindItemByClassname("item_armor_combat");
        gitem_t *body = FindItemByClassname("item_armor_body");
        int     index = ArmorIndex(other);

        other->max_health = cl->pers.max_health = 200;
        other->health = 200;
        if (index && body && index != ITEM_INDEX(body)) {
            cl->pers.inventory[index] = 0;
            index = 0;
        }
        if (!index && combat)
            index = ITEM_INDEX(combat);
        if (index)
            cl->pers.inventory[index] = 200;
        break;
    }
    case TA_SCOUT: {
        // no armor at all, and none to be picked up (TA_CanPickup)
        int index = ArmorIndex(other);

        if (index)
            cl->pers.inventory[index] = 0;
        break;
    }
    case TA_AMMOREGEN:
        memset(cl->ta_ammo_ms, 0, sizeof(cl->ta_ammo_ms));
        break;
    default:
        break;
    }

    // Touch_Item keeps a FL_RESPAWN item; nothing brings it back but a death
    ent->flags |= FL_RESPAWN;
    ent->svflags |= SVF_NOCLIENT;
    ent->solid = SOLID_NOT;
    ent->nextthink = 0;
    gi.linkentity(ent);
    return true;
}

// Touch_Item, before the pickup: what a persistent powerup forbids
bool TA_CanPickup(edict_t *ent, edict_t *other)
{
    if (!ent->item || !other->client)
        return true;
    // the Scout wears no armor (BG_CanItemBeGrabbed IT_ARMOR)
    if ((ent->item->flags & IT_ARMOR) && TA_HasPersistent(other, TA_SCOUT))
        return false;
    return true;
}

// the health a pickup may top up to: the Guard's max and no more (g_items.c
// Pickup_Health), or NULL's 0 to leave the rule as it is
int TA_HealthCap(const edict_t *other)
{
    return TA_HasPersistent(other, TA_GUARD) ? other->max_health : 0;
}

// the armor a pickup may top up to (Pickup_Armor's upperBound)
int TA_ArmorCap(const edict_t *other)
{
    return TA_HasPersistent(other, TA_GUARD) ? other->max_health : 0;
}

// g_weapon.c FireWeapon: the Doubler doubles s_quadFactor
int TA_DamageScale(const edict_t *attacker)
{
    return TA_HasPersistent(attacker, TA_DOUBLER) ? 2 : 1;
}

/*
=================
TA_FireScale

bg_pmove.c PM_Weapon: the Scout fires 1.5 times as fast, the Ammo Regen 1.3,
and either takes the place of Haste's 1.3. 0 when neither is held.
=================
*/
float TA_FireScale(const edict_t *ent)
{
    switch (TA_Persistent(ent)) {
    case TA_SCOUT:      return 1.5f;
    case TA_AMMOREGEN:  return 1.3f;
    default:            return 0;
    }
}

/*
g_active.c ClientTimerActions: each ammo comes back a few at a time up to a
little, gun or no gun. Quake III's ammo here, by the item that holds it; the
Quake II guns' own (flechettes, prox) as the Team Arena gun they stand beside.
*/
static const struct {
    const char  *ammo;
    int         max, inc, ms;
} ta_ammo_regen[] = {
    { "ammo_bullets",       50, 4, 1000 },
    { "ammo_shells",        10, 1, 1500 },
    { "ammo_grenades",      10, 1, 2000 },
    { "ammo_rockets",       10, 1, 1750 },
    { "ammo_q3_lightning",  50, 5, 1500 },
    { "ammo_slugs",         10, 1, 1750 },
    { "ammo_cells",         50, 5, 1500 },
    { "ammo_q3_bfg",        10, 1, 4000 },
    { "ammo_q3_nails",      10, 1, 1250 },
    { "ammo_q3_mines",       5, 1, 2000 },
    { "ammo_q3_belt",      100, 5, 1000 },
    { "ammo_flechettes",    10, 1, 1250 },
    { "ammo_prox",           5, 1, 2000 },
};

static void TA_AmmoRegen(edict_t *ent, int msec)
{
    gclient_t   *cl = ent->client;
    int         i;

    for (i = 0; i < q_countof(ta_ammo_regen) && i < q_countof(cl->ta_ammo_ms); i++) {
        gitem_t *item = FindItemByClassname((char *)ta_ammo_regen[i].ammo);
        int     *have, t = ta_ammo_regen[i].ms;

        if (!item)
            continue;
        have = &cl->pers.inventory[ITEM_INDEX(item)];
        cl->ta_ammo_ms[i] += msec;
        if (*have >= ta_ammo_regen[i].max)
            cl->ta_ammo_ms[i] = 0;
        if (cl->ta_ammo_ms[i] >= t) {
            while (cl->ta_ammo_ms[i] >= t)
                cl->ta_ammo_ms[i] -= t;
            *have += ta_ammo_regen[i].inc;
            if (*have > ta_ammo_regen[i].max)
                *have = ta_ammo_regen[i].max;
        }
    }
}

/*
==============================================================================

THE HOLDABLES

==============================================================================
*/

bool TA_Invulnerable(const edict_t *ent)
{
    return ent && ent->client && ent->client->ta_invul_framenum > level.framenum;
}

// g_active.c EV_USE_ITEM5: ten seconds in the sphere, held where you are
void Use_TAInvulnerability(edict_t *ent, gitem_t *item)
{
    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    ValidateSelectedItem(ent);

    ent->client->ta_invul_framenum = level.framenum + TA_INVUL_TIME * BASE_FRAMERATE;
    gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/invul_activate.wav"), 1, ATTN_NORM, 0);
}

// the kamikaze's hits, in g_weapon.c KamikazeRadiusDamage / KamikazeShockWave:
// through walls (their CanDamage is commented out), teammates too, and each
// one once in three seconds
static void TA_KamikazeHit(edict_t *self, float damage, float push, float radius, bool shock)
{
    edict_t *ent = NULL;
    vec3_t  v, dir;
    float   dist;
    int     i;

    if (radius < 1)
        radius = 1;

    while ((ent = findradius(ent, self->s.origin, radius + 48)) != NULL) {
        int *last;

        if (!ent->takedamage)
            continue;
        last = shock ? &ent->ta_kami_shock_framenum : &ent->ta_kami_framenum;
        if (*last > level.framenum)
            continue;

        for (i = 0; i < 3; i++) {
            if (self->s.origin[i] < ent->absmin[i])
                v[i] = ent->absmin[i] - self->s.origin[i];
            else if (self->s.origin[i] > ent->absmax[i])
                v[i] = self->s.origin[i] - ent->absmax[i];
            else
                v[i] = 0;
        }
        dist = VectorLength(v);
        if (dist >= radius)
            continue;

        VectorSubtract(ent->s.origin, self->s.origin, dir);
        dir[2] += 24;
        T_Damage(ent, self, self->activator ? self->activator : self, dir, self->s.origin, vec3_origin,
                 (int)damage, (int)damage, DAMAGE_RADIUS | DAMAGE_NO_TEAM_PROTECTION, MOD_TA_KAMIKAZE);

        if (shock && ent->client) {
            dir[2] = 0;
            VectorNormalize(dir);
            ent->velocity[0] = dir[0] * push;
            ent->velocity[1] = dir[1] * push;
            ent->velocity[2] = 100;
        }
        *last = level.framenum + 3 * BASE_FRAMERATE;
    }
}

static void TA_KamikazeThink(edict_t *self)
{
    float   t;
    int     i;

    self->count += 100;
    if (self->count >= KAMI_SHOCKWAVE_STARTTIME) {
        t = self->count - KAMI_SHOCKWAVE_STARTTIME;
        TA_KamikazeHit(self, 25, 400, t * KAMI_SHOCKWAVE_MAXRADIUS /
                       (KAMI_SHOCKWAVE_ENDTIME - KAMI_SHOCKWAVE_STARTTIME), true);
    }
    if (self->count >= KAMI_EXPLODE_STARTTIME) {
        t = self->count - KAMI_EXPLODE_STARTTIME;
        TA_KamikazeHit(self, 400, 0, t * KAMI_BOOMSPHERE_MAXRADIUS /
                       (KAMI_IMPLODE_STARTTIME - KAMI_EXPLODE_STARTTIME), false);
    }

    if (self->count >= KAMI_SHOCKWAVE_ENDTIME) {
        G_FreeEdict(self);
        return;
    }
    self->nextthink = level.framenum + 1;

    // the earthquake: everyone on the ground jolted, every view shaken
    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || !e->client)
            continue;
        if (e->groundentity) {
            e->velocity[0] += crandom() * 120;
            e->velocity[1] += crandom() * 120;
            e->velocity[2] = 30 + random() * 25;
        }
        e->client->kick_angles[0] += crandom() * 2;
        e->client->kick_angles[1] += crandom() * 2;
    }
}

// g_weapon.c G_StartKamikaze: the blast at `origin`, its kills `activator`'s
static void TA_StartKamikaze(const vec3_t origin, edict_t *activator)
{
    edict_t *k = G_Spawn();

    k->classname = "ta_kamikaze";
    VectorCopy(origin, k->s.origin);
    k->svflags |= SVF_NOCLIENT;
    k->activator = activator;
    k->count = 0;
    k->think = TA_KamikazeThink;
    k->nextthink = level.framenum + 1;

    // the sphere and its waves are the client's (q3weapons.c), everywhere
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_TA_KAMIKAZE);
    gi.WritePosition(k->s.origin);
    gi.multicast(k->s.origin, MULTICAST_ALL);

    gi.positioned_sound(k->s.origin, g_edicts, CHAN_AUTO, Q3_SoundIndex("items/kam_explode.wav"), 1, ATTN_NORM, 0);
    // GTS_KAMIKAZE: the far boom for everyone
    TA_TeamSound(TEAM_NONE, "items/kam_explode_far.wav");
}

void Use_TAKamikaze(edict_t *ent, gitem_t *item)
{
    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    ValidateSelectedItem(ent);

    // the invulnerability is off, and the one who set it off goes with it
    ent->client->ta_invul_framenum = 0;
    TA_StartKamikaze(ent->s.origin, ent);
    T_Damage(ent, ent, ent, vec3_origin, ent->s.origin, vec3_origin, 100000, 0,
             DAMAGE_NO_PROTECTION | DAMAGE_NO_KNOCKBACK, MOD_TA_KAMIKAZE);
}

// g_combat.c Kamikaze_DeathTimer: one held at death goes off five seconds on
static void TA_KamikazeTimer(edict_t *self)
{
    TA_StartKamikaze(self->s.origin, self->activator);
    G_FreeEdict(self);
}

/*
==============================================================================

THE PLAYERS

==============================================================================
*/

/*
=================
TA_BlockDamage

T_Damage, first thing: true when nothing is to be taken. The Invulnerability
takes everything but the juiced prox (g_combat.c G_Damage), and an Overload
obelisk is not its own team's to hurt (CheckObeliskAttack), which also calls
the team to its base.
=================
*/
bool TA_BlockDamage(edict_t *targ, edict_t *attacker, int dflags, int mod)
{
    if (targ->client && TA_Invulnerable(targ) && (mod & ~MOD_FRIENDLY_FIRE) != MOD_TA_JUICED &&
        !(dflags & DAMAGE_NO_PROTECTION)) {
        static const char *const hits[3] = {
            "items/invul_impact_01.wav", "items/invul_impact_02.wav", "items/invul_impact_03.wav"
        };
        if (!(level.framenum & 1))
            gi.sound(targ, CHAN_AUTO, Q3_SoundIndex(hits[Q_rand() % 3]), 1, ATTN_NORM, 0);
        return true;
    }

    if (targ->die == TA_ObeliskDie) {
        int team = targ->ta_team;

        if (!attacker || !attacker->client)
            return false;
        if (team == attacker->client->resp.team)
            return true;
        if ((team == TEAM_RED || team == TEAM_BLUE) &&
            (!ta.attacked_time[team] || level.time - ta.attacked_time[team] > TA_ATTACK_SOUND_TIME)) {
            ta.attacked_time[team] = level.time;
            TA_TeamSound(team, "teamplay/voc_base_attack.wav");
            TA_BaseAttacked(team);
        }
    }
    return false;
}

/*
=================
TA_PlayerDied

player_die: the persistent powerup goes home, Harvester throws a skull, a
kamikaze still held goes off where they fell, and a prox mine stuck on them
is gone (g_combat.c player_die).
=================
*/
void TA_PlayerDied(edict_t *self)
{
    gclient_t *cl = self->client;

    if (!cl)
        return;

    TA_ReturnPersistent(self);
    cl->ta_invul_framenum = 0;

    if (cl->ta_stuck_mine) {
        if (cl->ta_stuck_mine->inuse)
            G_FreeEdict(cl->ta_stuck_mine);
        cl->ta_stuck_mine = NULL;
    }

    if (level.ta_mode == TA_MODE_HARVESTER)
        TA_TossSkull(self);
    cl->ta_skulls = 0;

    if (ta.kamikaze && cl->pers.inventory[ITEM_INDEX(ta.kamikaze)] > 0) {
        edict_t *timer = G_Spawn();

        cl->pers.inventory[ITEM_INDEX(ta.kamikaze)] = 0;
        timer->classname = "ta_kamikaze_timer";
        VectorCopy(self->s.origin, timer->s.origin);
        timer->svflags |= SVF_NOCLIENT;
        timer->activator = self;
        timer->think = TA_KamikazeTimer;
        timer->nextthink = level.framenum + 5 * BASE_FRAMERATE;
    }
}

// a gibbed body takes its kamikaze timer with it (g_combat.c GibEntity)
void TA_BodyGibbed(edict_t *self)
{
    edict_t *e = NULL;

    while ((e = G_Find(e, FOFS(classname), "ta_kamikaze_timer")) != NULL) {
        if (e->activator == self) {
            G_FreeEdict(e);
            return;
        }
    }
}

// ClientDisconnect, and a change of team: what they hold goes back in play
void TA_PlayerLeft(edict_t *ent)
{
    if (!ent->client)
        return;
    TA_ReturnPersistent(ent);
    ent->client->ta_skulls = 0;
    if (ent->client->ta_stuck_mine) {
        if (ent->client->ta_stuck_mine->inuse)
            G_FreeEdict(ent->client->ta_stuck_mine);
        ent->client->ta_stuck_mine = NULL;
    }
    ent->client->ta_invul_framenum = 0;
    if (ent->client->ta_invul_sphere) {
        if (ent->client->ta_invul_sphere->inuse && ent->client->ta_invul_sphere->owner == ent)
            G_FreeEdict(ent->client->ta_invul_sphere);
        ent->client->ta_invul_sphere = NULL;
    }
}

/*
=================
TA_TeleporterDrops

The personal teleporter (Use_Q3Teleporter), before the jump: Harvester's
skulls fall where they were carried from (g_active.c EV_USE_ITEM1).
=================
*/
void TA_TeleporterDrops(edict_t *ent)
{
    gclient_t   *cl = ent->client;
    int         team = TA_Team(ent), i;
    gitem_t     *item;

    if (level.ta_mode != TA_MODE_HARVESTER || cl->ta_skulls <= 0)
        return;
    item = ta.cube[TA_OtherTeam(team)];
    for (i = 0; item && i < cl->ta_skulls; i++) {
        vec3_t vel;

        VectorSet(vel, crandom() * 150, crandom() * 150, 200 + crandom() * 50);
        TA_LaunchSkull(item, ent->s.origin, vel);
    }
    cl->ta_skulls = 0;
}

/*
=================
TA_ClientFrame

ClientEndServerFrame, once a frame: the Guard's regeneration and the Ammo
Regen's ammo (g_active.c ClientTimerActions), and the Invulnerability's
last seconds.
=================
*/
/*
=================
TA_InvulSphere

The invulnerability's sphere (cg_players.c CG_Player: shield.md3 round the
player, always drawn, its owner too): an entity of its own on the player,
put where they are as each frame ends, gone when the powerup is.
=================
*/
static void TA_InvulSphere(edict_t *ent)
{
    gclient_t   *cl = ent->client;
    edict_t     *s = cl->ta_invul_sphere;
    bool        want = ent->inuse && ent->health > 0 && TA_Invulnerable(ent);

    // a new level freed it under us
    if (s && (!s->inuse || s->owner != ent || Q_strcasecmp(s->classname, "ta_invul_sphere")))
        s = cl->ta_invul_sphere = NULL;

    if (!want) {
        if (s)
            G_FreeEdict(s);
        cl->ta_invul_sphere = NULL;
        return;
    }
    if (!s) {
        s = G_Spawn();
        s->classname = "ta_invul_sphere";
        s->owner = ent;
        s->movetype = MOVETYPE_NONE;
        s->solid = SOLID_NOT;
        s->s.modelindex = gi.modelindex("models/powerups/shield/shield.md3");
        cl->ta_invul_sphere = s;
    }
    VectorCopy(ent->s.origin, s->s.origin);
    VectorCopy(ent->s.origin, s->s.old_origin);
    gi.linkentity(s);
}

void TA_ClientFrame(edict_t *ent)
{
    gclient_t   *cl = ent->client;
    int         kind;

    if (!level.ta_map || !cl)
        return;
    TA_InvulSphere(ent);
    if (ent->health <= 0)
        return;

    if (cl->ta_invul_framenum > level.framenum) {
        int left = cl->ta_invul_framenum - level.framenum;

        if (left <= 5 * BASE_FRAMERATE && !(left % BASE_FRAMERATE))
            gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/wearoff.wav"), 1, ATTN_NORM, 0);
    }

    kind = TA_Persistent(ent);
    if (kind == TA_AMMOREGEN)
        TA_AmmoRegen(ent, BASE_FRAMETIME);

    cl->ta_tick_ms += BASE_FRAMETIME;
    if (cl->ta_tick_ms < 1000)
        return;
    cl->ta_tick_ms -= 1000;

    if (kind == TA_GUARD) {
        int max = ent->max_health / 2;

        if (ent->health < max) {
            ent->health += 15;
            if (ent->health > max * 1.1f)
                ent->health = max * 1.1f;
            gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/regen.wav"), 1, ATTN_NORM, 0);
        } else if (ent->health < max * 2) {
            ent->health += 5;
            if (ent->health > max * 2)
                ent->health = max * 2;
            gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/regen.wav"), 1, ATTN_NORM, 0);
        }
    }
}

/*
=================
TA_SetStats / TA_StatusBar

The persistent powerup's icon where CTF shows its tech (the right edge over
the ammo): CTF_SetStats puts it in STAT_CTF_TECH in CTF's games, this in
STAT_TA_PERSISTENT in the rest - Team Arena's team deathmatch has them too,
and a free-for-all those its maps place for one. (Not STAT_CTF_TECH there: in
a free-for-all that is Quake III's HUD's fraglimit, G_SetQ3ScoreStats.)
TA_StatusBar adds the picture to deathmatch's status bar on its maps.
=================
*/
void TA_SetStats(edict_t *ent)
{
    gclient_t *cl = ent->client;

    if (!level.ta_map || G_CTF() || !cl)
        return;
    cl->ps.stats[STAT_TA_PERSISTENT] = (cl->ta_persistent && cl->ta_persistent->item) ?
                                       gi.imageindex(cl->ta_persistent->item->icon) : 0;
}

const char *TA_StatusBar(const char *dm_statusbar)
{
    static char sb[1536];

    Q_snprintf(sb, sizeof(sb), "%syb -129 if %d xr -26 pic %d endif ", dm_statusbar,
               STAT_TA_PERSISTENT, STAT_TA_PERSISTENT);
    return sb;
}

/*
==============================================================================

TEAM ARENA'S HUD: WHAT THE CLIENT IS TOLD

On its maps the client draws Team Arena's own HUD (client hud_q3.c,
cl_q3_hud_ta), whose cgame knew more than Quake II's stats carry:

  - CS_TA_HUD: the game type, its limit, the teams' scores and where the
    flags are (cgs.redflag / blueflag / flagStatus, from CS_FLAGSTATUS) -
    TA_UpdateHud, sent when it changes
  - CS_TA_LOCATIONS: the map's target_location names (Q3's CS_LOCATIONS)
  - once a second, everyone on a team is told where each of their team is,
    with health, armor, gun, powerup and job (g_team.c CheckTeamStatus /
    TeamplayInfoMessage's "tinfo") - "ta_tinfo", through the console as
    g_sp.c's "sp_event" goes
  - voice chats (g_cmds.c G_Voice's "vchat" / "vtchat"): "ta_vchat"; the
    client picks the line from the speaker's voice file, plays it, prints it
    and shows their face at the top left (cg_servercmds.c CG_VoiceChat)

==============================================================================
*/

// SP_target_location, from Q3_SpawnEntity (target_location_linkup): its
// name for the HUD; ta_tinfo gives each player's as index + 1, 0 = none
void TA_AddLocation(edict_t *ent)
{
    if (!level.ta_map || ta.num_locations >= MAX_TA_LOCATIONS || !ent->message || !ent->message[0])
        return;
    VectorCopy(ent->s.origin, ta.location[ta.num_locations]);
    gi.configstring(CS_TA_LOCATIONS + ta.num_locations, ent->message);
    ta.num_locations++;
}

// g_team.c Team_GetLocation: the nearest target_location in sight's PVS
static int TA_LocationOf(const edict_t *ent)
{
    float   best = 3 * 8192.0f * 8192.0f;
    int     i, found = 0;

    for (i = 0; i < ta.num_locations; i++) {
        vec3_t  d;
        float   len;

        VectorSubtract(ent->s.origin, ta.location[i], d);
        len = DotProduct(d, d);
        if (len > best || !gi.inPVS(ent->s.origin, ta.location[i]))
            continue;
        best = len;
        found = i + 1;
    }
    return found;
}

// cgs.redflag / blueflag: 0 at its stand, 1 carried, 2 lying about; the
// white flag (flagStatus): 0 at its stand, 2 red has it, 3 blue has it,
// 4 lying about
static int TA_FlagState(int team)
{
    edict_t *carrier = CTF_Carrier(team);

    if (team == TEAM_NONE) {
        if (carrier)
            return TA_Team(carrier) == TEAM_BLUE ? 3 : 2;
        return CTF_DroppedFlag(TEAM_NONE) ? 4 : 0;
    }
    if (carrier)
        return 1;
    return CTF_DroppedFlag(team) ? 2 : 0;
}

/*
CS_TA_HUD: "<gametype> <limit> <red score> <blue score> <red flag> <blue flag>
<white flag>", the game type Quake III's (GT_FFA 0, GT_TOURNAMENT 1, GT_TEAM
3, GT_CTF 4, GT_1FCTF 5, GT_OBELISK 6, GT_HARVESTER 7), the limit its
capturelimit in CTF's games and its fraglimit otherwise, 0 = none.
*/
static void TA_UpdateHud(void)
{
    char    s[MAX_QPATH];
    int     gametype, limit, red = 0, blue = 0, rf = 0, bf = 0, nf = 0;

    if (G_CTF()) {
        gametype = 4 + (level.ta_mode >= TA_MODE_CTF ? level.ta_mode - TA_MODE_CTF : 0);
        limit = (int)(TA_SkirmishActive() ? TA_SkirmishCaptureLimit() : gi.cvar("capturelimit", "0", 0)->value);
        red = CTF_Caps(TEAM_RED);
        blue = CTF_Caps(TEAM_BLUE);
        if (level.ta_mode == TA_MODE_ONEFLAG) {
            nf = TA_FlagState(TEAM_NONE);
        } else if (level.ta_mode == TA_MODE_CTF) {
            rf = TA_FlagState(TEAM_RED);
            bf = TA_FlagState(TEAM_BLUE);
        }
    } else if (G_TeamPlay()) {
        gametype = 3;
        limit = (int)G_FragLimit();
        red = Team_Score(TEAM_RED);
        blue = Team_Score(TEAM_BLUE);
    } else {
        gametype = !strcmp(TA_GametypeName(), "tournament") ? 1 : 0;
        limit = (int)G_FragLimit();
    }

    Q_snprintf(s, sizeof(s), "%d %d %d %d %d %d %d", gametype, limit, red, blue, rf, bf, nf);
    if (strcmp(s, ta.hud)) {
        Q_strlcpy(ta.hud, s, sizeof(ta.hud));
        gi.configstring(CS_TA_HUD, s);
    }
}

// a team game, red against blue
static bool TA_TeamGame(void)
{
    return G_CTF() || G_TeamPlay();
}

/*
"ta_tinfo <your team> <n> [<client> <location> <health> <armor> <gun icon>
<powerup icon> <flag> <persistent icon> <job>]...": everyone on the team in
client order, as TeamplayInfoMessage sends them. The gun and powerups are
image indices (the persistent one Guard, Scout...); the flag carried 1 red,
2 blue, 3 white; the job Quake III's teamTask (0 none, 1 offense, 2 defense -
Bot_TeamTask). hud_q3.c ta_tinfo_f reads it.
*/
#define TA_TINFO_MAX    16

static void TA_SendTeamInfo(int team)
{
    char    msg[1024], entry[96];
    int     i, n = 0;
    size_t  len;

    msg[0] = 0;
    for (i = 1; i <= game.maxclients && n < TA_TINFO_MAX; i++) {
        edict_t     *e = g_edicts + i;
        gclient_t   *cl = e->client;
        int         flag = 0, powerup = 0, persistent = 0;

        if (!e->inuse || !cl || cl->resp.team != team || cl->resp.spectator)
            continue;
        if (G_CTF()) {
            if (CTF_Carrier(TEAM_RED) == e)
                flag = 1;
            else if (CTF_Carrier(TEAM_BLUE) == e)
                flag = 2;
            else if (CTF_Carrier(TEAM_NONE) == e)
                flag = 3;
        }
        powerup = cl->ps.stats[STAT_TIMER_ICON];
        if (cl->ta_persistent && cl->ta_persistent->item && cl->ta_persistent->item->icon)
            persistent = gi.imageindex(cl->ta_persistent->item->icon);
        Q_snprintf(entry, sizeof(entry), " %d %d %d %d %d %d %d %d %d", i - 1, TA_LocationOf(e),
                   max(e->health, 0), max((int)cl->ps.stats[STAT_ARMOR], 0),
                   cl->pers.weapon && cl->pers.weapon->icon ? gi.imageindex(cl->pers.weapon->icon) : 0,
                   powerup, flag, persistent, Bot_TeamTask(e));
        len = strlen(msg);
        if (len + strlen(entry) >= sizeof(msg) - 32)
            break;
        Q_strlcpy(msg + len, entry, sizeof(msg) - len);
        n++;
    }

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || !e->client || e->client->resp.team != team || Bot_IsBot(e))
            continue;
        gi.WriteByte(svc_stufftext);
        gi.WriteString(va("ta_tinfo %d %d%s\n", team == TEAM_BLUE ? 2 : 1, n, msg));
        gi.unicast(e, false);
    }
}

/*
=================
TA_Voice

A voice chat (g_cmds.c G_Voice / G_VoiceTo): <id> a line of the speaker's
voice file (missionpack scripts/*.voice - "wantonoffense", "ihaveflag"...).
To everyone, (to) NULL and !team; to the speaker's team; or told to one
player. voice_only: heard, not printed (vosay / BotVoiceChatOnly).
=================
*/
#define TA_VOICE_ALL    0
#define TA_VOICE_TEAM   1
#define TA_VOICE_TELL   2

static void TA_VoiceTo(edict_t *speaker, edict_t *to, int mode, bool voice_only, const char *id)
{
    if (!to || !to->inuse || !to->client || Bot_IsBot(to))
        return;
    if (mode == TA_VOICE_TEAM && TA_Team(to) != TA_Team(speaker))
        return;
    gi.WriteByte(svc_stufftext);
    gi.WriteString(va("ta_vchat %d %d %d %s\n", mode, voice_only ? 1 : 0, (int)(speaker - g_edicts) - 1, id));
    gi.unicast(to, true);
}

static void TA_Voice(edict_t *speaker, edict_t *to, int mode, bool voice_only, const char *id)
{
    int i;

    if (!level.ta_map || !speaker || !speaker->client)
        return;
    if (mode != TA_VOICE_TELL && !TA_TeamGame())
        mode = TA_VOICE_ALL;
    if (to) {
        TA_VoiceTo(speaker, to, mode, voice_only, id);
        if (to != speaker)
            TA_VoiceTo(speaker, speaker, mode, voice_only, id);
        return;
    }
    for (i = 1; i <= game.maxclients; i++)
        TA_VoiceTo(speaker, g_edicts + i, mode, voice_only, id);
}

/*
=================
TA_VoiceCommand

ClientCommand: Team Arena's voice commands (g_cmds.c Cmd_Voice_f,
Cmd_VoiceTell_f, Cmd_VoiceTaunt_f) - vsay, vsay_team, vtell <client>, and
vosay, vosay_team, votell <client> with no line printed; vtaunt. One a
second at most.
=================
*/
bool TA_VoiceCommand(edict_t *ent, const char *cmd)
{
    bool        only = false;
    int         mode, slot = (int)(ent - g_edicts) - 1;
    edict_t     *to = NULL;
    const char  *id;

    if (!Q_strncasecmp(cmd, "vo", 2) && Q_stricmp(cmd, "vtaunt")) {
        only = true;
        cmd += 2;
    } else if (cmd[0] == 'v' || cmd[0] == 'V') {
        cmd += 1;
    } else {
        return false;
    }

    if (!Q_stricmp(cmd, "say")) {
        mode = TA_VOICE_ALL;
        id = gi.argv(1);
    } else if (!Q_stricmp(cmd, "say_team")) {
        mode = TA_VOICE_TEAM;
        id = gi.argv(1);
    } else if (!Q_stricmp(cmd, "tell")) {
        int n = atoi(gi.argv(1));

        if (n < 0 || n >= game.maxclients)
            return true;
        to = g_edicts + 1 + n;
        if (!to->inuse || !to->client)
            return true;
        mode = TA_VOICE_TELL;
        id = gi.argv(2);
    } else if (!Q_stricmp(cmd, "taunt") && !only) {
        mode = TA_VOICE_ALL;
        id = "taunt";
    } else {
        return false;
    }

    if (!level.ta_map) {
        gi.cprintf(ent, PRINT_HIGH, "Voice chats are Team Arena's, on its maps.\n");
        return true;
    }
    if (!id[0] || slot < 0 || slot >= MAX_CLIENTS)
        return true;
    if (ta.voice_time[slot] && level.time - ta.voice_time[slot] < 1)
        return true;
    ta.voice_time[slot] = level.time;
    TA_Voice(ent, to, mode, only, id);
    return true;
}

/*
The bots' calls. Quake III's (bot_q3_ai 2) make their own in its ai_*.c;
these are the other bots', at the times Team Arena's made them:
  - a few seconds into the level, its job wished for, told to the humans on
    its team (ai_dmq3.c BotCheckItemPickup's VOICECHAT_WANTONOFFENSE /
    _WANTONDEFENSE, "Let me go on offense.") - and then, heard only, that it
    is on it (ai_dmnet.c's VOICECHAT_ONOFFENSE / _ONDEFENSE / _ONGETFLAG)
  - their flag taken (BotVoiceChat VOICECHAT_IHAVEFLAG)
  - ours taken: a defender calls it (VOICECHAT_ENEMYHASFLAG)
  - an obelisk under attack, Overload: a defender calls it (_BASEATTACK)
Only in CTF's games, where the bots have jobs (Bot_TeamTask).
*/
static bool TA_HumanOnTeam(int team)
{
    int i;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (e->inuse && e->client && !Bot_IsBot(e) && e->client->resp.team == team)
            return true;
    }
    return false;
}

// a bot of (team), its defenders first
static edict_t *TA_TeamBot(int team)
{
    edict_t *any = NULL;
    int     i;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || !e->client || !Bot_IsBot(e) || e->client->resp.team != team || e->health <= 0)
            continue;
        if (Bot_TeamTask(e) == 2)
            return e;
        if (!any)
            any = e;
    }
    return any;
}

static void TA_BotVoices(void)
{
    int i;

    if (!G_CTF())
        return;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t     *e = g_edicts + i;
        gclient_t   *cl = e->client;
        int         task, slot = i - 1;

        if (!e->inuse || !cl || !Bot_IsBot(e) || cl->resp.spectator)
            continue;
        if (cl->resp.team != TEAM_RED && cl->resp.team != TEAM_BLUE)
            continue;
        if (ta.bot_voice[slot].enterframe != cl->resp.enterframe) {
            ta.bot_voice[slot].enterframe = cl->resp.enterframe;
            ta.bot_voice[slot].wish_time = level.time + 2 + random() * 8;
            ta.bot_voice[slot].status_time = 0;
            ta.bot_voice[slot].task = 0;
        }

        task = Bot_TeamTask(e);
        if (ta.bot_voice[slot].wish_time && level.time >= ta.bot_voice[slot].wish_time) {
            ta.bot_voice[slot].wish_time = 0;
            ta.bot_voice[slot].task = task;
            if (TA_HumanOnTeam(cl->resp.team)) {
                int j;

                for (j = 1; j <= game.maxclients; j++) {
                    edict_t *h = g_edicts + j;

                    if (h->inuse && h->client && !Bot_IsBot(h) && h->client->resp.team == cl->resp.team)
                        TA_VoiceTo(e, h, TA_VOICE_TELL, false, task == 2 ? "wantondefense" : "wantonoffense");
                }
                ta.bot_voice[slot].status_time = level.time + 4 + random() * 6;
            }
        }
        if (ta.bot_voice[slot].status_time && level.time >= ta.bot_voice[slot].status_time) {
            const char *id = "onoffense";

            ta.bot_voice[slot].status_time = 0;
            if (task == 2)
                id = "ondefense";
            else if (level.ta_mode == TA_MODE_CTF || level.ta_mode == TA_MODE_ONEFLAG)
                id = "ongetflag";
            TA_Voice(e, NULL, TA_VOICE_TEAM, true, id);
        }
    }
}

// g_ctf.c CTFPickup_Flag: (player) took (team)'s flag (TEAM_NONE: the white one)
void TA_FlagTaken(edict_t *player, int team)
{
    edict_t *caller;
    int     victim;

    if (!level.ta_map || !player || !player->client)
        return;
    if (Bot_IsBot(player))
        TA_Voice(player, NULL, TA_VOICE_TEAM, false, "ihaveflag");

    // the team it was taken from, CTF's own flags only: a defender of
    // theirs calls it
    if (team != TEAM_RED && team != TEAM_BLUE)
        return;
    victim = team;
    if (ta.enemy_flag_time[victim] && level.time - ta.enemy_flag_time[victim] < 15)
        return;
    caller = TA_TeamBot(victim);
    if (!caller || !TA_HumanOnTeam(victim))
        return;
    ta.enemy_flag_time[victim] = level.time;
    TA_Voice(caller, NULL, TA_VOICE_TEAM, false, "enemyhasflag");
}

// TA_BlockDamage's "base under attack", Overload: a defender calls it too
static void TA_BaseAttacked(int team)
{
    edict_t *caller = TA_TeamBot(team);

    if (caller)
        TA_Voice(caller, NULL, TA_VOICE_TEAM, false, "baseattack");
}

/*
=================
TA_RunFrame

G_RunFrame, each server frame: Team Arena's HUD state, the once a second
team info, the bots' calls.
=================
*/
void TA_RunFrame(void)
{
    int team;

    if (!level.ta_map || !deathmatch->value || level.intermission_framenum)
        return;

    TA_UpdateHud();

    if (TA_TeamGame() && level.time >= ta.tinfo_time) {
        ta.tinfo_time = level.time + 1;
        for (team = TEAM_RED; team <= TEAM_BLUE; team++)
            TA_SendTeamInfo(team);
    }

    TA_BotVoices();
}

/*
==============================================================================

THE LEVEL

==============================================================================
*/

void TA_Init(void)
{
    ta.cube[TEAM_RED] = FindItemByClassname("item_ta_redcube");
    ta.cube[TEAM_BLUE] = FindItemByClassname("item_ta_bluecube");
    ta.persistent[TA_SCOUT] = FindItemByClassname("item_ta_scout");
    ta.persistent[TA_GUARD] = FindItemByClassname("item_ta_guard");
    ta.persistent[TA_DOUBLER] = FindItemByClassname("item_ta_doubler");
    ta.persistent[TA_AMMOREGEN] = FindItemByClassname("item_ta_ammoregen");
    ta.kamikaze = FindItemByClassname("item_ta_kamikaze");
    ta.invulnerability = FindItemByClassname("item_ta_invulnerability");
}

// SpawnEntities, before the map's entities
/*
=================
TA_SkirmishActive / TA_SkirmishBot

Team Arena's single player game, its skirmish (ui_main.c UI_StartSkirmish):
a map of its own in one of its game types, against a team of its characters,
with you and your own team's on red. Its menu (client/ui/q3sp.c) starts it
with ta_skirmish 1, the game type in deathmatch's teamplay and ctf (ctf 2-4
One Flag, Overload, Harvester), Quake III's skill in sp_skill (1-5, g_sp.c's
for its bots), the teams' names in g_redTeam / g_blueTeam (their uniforms,
g_bot.c Team_Skin), and its bots in ta_bot1..ta_bot<ta_bots>: "<skin> <team>
<name>" - a Team Arena character, 1 red or 2 blue, and the alias it goes by.
None of it archived: the bot match's own line-up is left as it was.
ta_skirmish is taken as the map loads (level.ta_skirmish, g_spawn.c).

Its limits are its own, as the campaign's are (g_sp.c G_FragLimit): 10
frags, no time limit, and 5 captures - 4 obelisks, 15 skulls. When it is
over you go back to its menu ("taskirmish") rather than on to a map.
=================
*/
bool TA_SkirmishActive(void)
{
    return deathmatch->value && level.ta_map && level.ta_skirmish;
}

float TA_SkirmishCaptureLimit(void)
{
    switch (level.ta_mode) {
    case TA_MODE_OVERLOAD:  return 4;
    case TA_MODE_HARVESTER: return 15;
    default:                return 5;
    }
}

// ExitLevel: the intermission is over - each human back to the skirmish's menu
bool TA_SkirmishExit(void)
{
    int i;

    if (!TA_SkirmishActive())
        return false;
    if (ta.skirmish_over)
        return true;
    ta.skirmish_over = true;

    for (i = 0; i < game.maxclients; i++) {
        edict_t *e = g_edicts + 1 + i;

        if (!e->inuse || !e->client || Bot_IsBot(e))
            continue;
        gi.WriteByte(svc_stufftext);
        gi.WriteString("disconnect\ntaskirmish\n");
        gi.unicast(e, true);
    }
    return true;
}

bool TA_SkirmishBot(int i, const char **skin, int *team, const char **name)
{
    static char s_skin[MAX_QPATH], s_name[32];
    const char  *p;
    char        *tok;

    if (i < 0 || i >= gi.cvar("ta_bots", "0", 0)->integer || i >= MAX_CLIENTS - 1)
        return false;
    p = gi.cvar(va("ta_bot%d", i + 1), "", 0)->string;

    tok = COM_Parse(&p);
    Q_strlcpy(s_skin, tok, sizeof(s_skin));
    tok = COM_Parse(&p);
    *team = atoi(tok) == 2 ? TEAM_BLUE : TEAM_RED;
    tok = COM_Parse(&p);
    Q_strlcpy(s_name, tok, sizeof(s_name));

    *skin = s_skin;
    *name = s_name;
    return s_skin[0] != 0;
}

void TA_LevelStart(void)
{
    memset(ta.obelisk, 0, sizeof(ta.obelisk));
    memset(ta.attacked_time, 0, sizeof(ta.attacked_time));
    ta.skirmish_over = false;
    ta.num_locations = 0;
    ta.hud[0] = 0;
    ta.tinfo_time = 0;
    memset(ta.bot_voice, 0, sizeof(ta.bot_voice));
    memset(ta.enemy_flag_time, 0, sizeof(ta.enemy_flag_time));
    memset(ta.voice_time, 0, sizeof(ta.voice_time));

    // a skirmish is Quake III's game whatever the bot match is set to, as the
    // campaign's arenas are (g_sp.c SP_LevelRules)
    if (TA_SkirmishActive()) {
        level.q3_weapons = 1;
        level.q1_weapons = false;
        level.q3_physics = true;
        gi.dprintf("Skirmish: %s, %s, %d bots, skill %d\n", level.mapname, TA_GametypeName(),
                   gi.cvar("ta_bots", "0", 0)->integer, SP_Skill());
    }
}

// SP_worldspawn: what a game of Team Arena's needs that no map entity names
void TA_Precache(void)
{
    if (!level.ta_map)
        return;

    if (level.ta_mode == TA_MODE_HARVESTER) {
        if (ta.cube[TEAM_RED])
            PrecacheItem(ta.cube[TEAM_RED]);
        if (ta.cube[TEAM_BLUE])
            PrecacheItem(ta.cube[TEAM_BLUE]);
    }
    if (level.ta_mode != TA_MODE_CTF && G_CTF()) {
        Q3_SoundIndex("teamplay/voc_red_scores.wav");
        Q3_SoundIndex("teamplay/voc_blue_scores.wav");
        Q3_SoundIndex("teamplay/voc_base_attack.wav");
    }
    Q3_SoundIndex("items/wearoff.wav");
    // the holdables' and the guns' mid-game media, so nothing loads on first use
    gi.modelindex("models/powerups/shield/shield.md3");
    gi.modelindex("models/weaphits/nail.md3");
    gi.modelindex("models/weaphits/proxmine.md3");
    Q3_SoundIndex("items/invul_activate.wav");
    Q3_SoundIndex("items/invul_juiced.wav");
    Q3_SoundIndex("items/invul_impact_01.wav");
    Q3_SoundIndex("items/invul_impact_02.wav");
    Q3_SoundIndex("items/invul_impact_03.wav");
    Q3_SoundIndex("items/kam_explode.wav");
    Q3_SoundIndex("items/kam_explode_far.wav");
    Q3_SoundIndex("weapons/nailgun/wnalimpl.wav");
    Q3_SoundIndex("weapons/proxmine/wstbtick.wav");
    Q3_SoundIndex("weapons/proxmine/wstbactv.wav");
    Q3_SoundIndex("weapons/proxmine/wstbimpl.wav");
    Q3_SoundIndex("weapons/proxmine/wstbimpd.wav");
}
