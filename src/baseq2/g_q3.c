/*
Copyright (C) 2026 Matt Stewart

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

QUAKE III ARENA MAPS

The engine loads a Quake III map as an ordinary one (common/bsp_q3.c) and
tells the game so through sv_map_q3, which SpawnEntities copies into
level.q3map. Everything here runs only then. It does three things:

  - turns each Quake III entity into the Quake II one that plays its part:
    the weapons and items one for one (Matt's table, below), the CTF bases
    and spawns, the movers with Quake III's own defaults;

  - supplies the entities Quake II has no equivalent for - jump pads aimed
    at a target, teleporters that throw you out at speed, auto-repeating
    speakers, bobbing and swinging movers, target_give and friends;

  - plays by Quake III's rules where Matt asked for them: the Haste, Regen,
    Flight and Battle Suit powerups and the two holdables, with their own
    models and sounds; Quake III health and armor amounts as a menu option
    (g_q3_items); the spawn weapons as another (g_q3_kit); and Quake III's
    gentler falling damage.

Its sounds are always Quake III's own, asked for by the "#baseq3/" path the
filesystem gives baseq3's data (files.c): several of them share a name with
a Quake II sound that would otherwise win.

==============================================================================
*/

#include "g_local.h"

cvar_t  *g_q3_items;    // 1: Quake III health/armor amounts and the Battle Suit
cvar_t  *g_q3_kit;      // what a player spawns holding, see Q3_GiveSpawnKit
cvar_t  *g_q3_weapons;  // bot match: 0 Quake II's guns, 1 Quake III's, 2 by player model (g_q3weap.c)
cvar_t  *g_q3_physics;  // bot match: 1 Quake III's movement and knockback (pmove.c q3move)

// Quake III item spawnflag
#define Q3_ITEM_SUSPENDED   1

// Quake III target_speaker spawnflags
#define Q3_SPEAKER_LOOPED_ON    1
#define Q3_SPEAKER_LOOPED_OFF   2
#define Q3_SPEAKER_GLOBAL       4
#define Q3_SPEAKER_ACTIVATOR    8

// Quake III's powerup lengths and respawns, in seconds
#define Q3_POWERUP_TIME         30
#define Q3_FLIGHT_TIME          60
#define Q3_RESPAWN_POWERUP      120
#define Q3_RESPAWN_HOLDABLE     60
#define Q3_RESPAWN_HEALTH       35
#define Q3_RESPAWN_ARMOR        25

#define SECONDS(s)      ((int)((s) * BASE_FRAMERATE))

void Q3_InitGame(void)
{
    g_q3_items = gi.cvar("g_q3_items", "1", CVAR_ARCHIVE);
    g_q3_kit = gi.cvar("g_q3_kit", "0", CVAR_ARCHIVE);
    g_q3_weapons = gi.cvar("g_q3_weapons", "0", CVAR_ARCHIVE);
    g_q3_physics = gi.cvar("g_q3_physics", "0", CVAR_ARCHIVE);
}

/*
=================
Q3_LevelRules

The bot match's weapons and physics for the level about to spawn, into
level.q3_weapons / level.q3_physics, and on to the engine and the clients as
CS_Q3RULES (written by SP_worldspawn). Deathmatch only, and only with Quake
III's data (fs_quake3): without it the menu hides both rows, so a value left
in a config falls back to Quake II's weapons and movement.
=================
*/
void Q3_LevelRules(void)
{
    level.q3_weapons = 0;
    level.q1_weapons = false;
    level.q1_by_model = false;
    level.q3_physics = false;

    if (!deathmatch->value)
        return;

    if (gi.cvar("fs_quake3", "0", 0)->integer == 1) {
        if (g_q3_weapons)
            level.q3_weapons = (g_q3_weapons->integer >= 0 && g_q3_weapons->integer <= 2) ? g_q3_weapons->integer : 0;
        level.q3_physics = g_q3_physics && g_q3_physics->integer;
    }

    // 3: Quake's guns for everyone (g_q1weap.c), with Quake's data - and
    // whether or not Quake III's is there
    if (g_q3_weapons && g_q3_weapons->integer == 3 && Q1W_Available())
        level.q1_weapons = true;

    // a campaign arena is Quake III's game whatever the bot match says (g_sp.c)
    SP_LevelRules();

    // 2, by player model: Quake's Ranger plays with Quake's guns (g_q1weap.c
    // Q1W_ClientQ1), with Quake's data
    level.q1_by_model = level.q3_weapons == 2 && !level.q1_weapons && Q1W_Available();
}

// Quake III's health and armor amounts and its Battle Suit: the bot match's
// choice, and always in the campaign
bool Q3_ItemRules(void)
{
    return (g_q3_items && g_q3_items->integer) || SP_Active();
}

static bool Q3_Amounts(void)
{
    return level.q3map && Q3_ItemRules();
}

/*
=================
Q3_SoundIndex

Quake III's own copy of a sound. Takes it either way the data spells it:
"sound/world/jumppad.wav" from an entity key, or "world/jumppad.wav".
=================
*/
int Q3_SoundIndex(const char *name)
{
    char    path[MAX_QPATH];

    if (!name || !*name)
        return 0;
    if (*name == '*')
        return gi.soundindex(name);     // the player's own (sexed) sound
    if (!Q_strncasecmp(name, "sound/", 6))
        name += 6;
    if (Q_snprintf(path, sizeof(path), "#baseq3/sound/%s", name) >= sizeof(path))
        return 0;
    if (!strchr(path + 14, '.'))
        Q_strlcat(path, ".wav", sizeof(path));
    return gi.soundindex(path);
}

/*
==============================================================================

POWERUPS AND HOLDABLES

==============================================================================
*/

static int *Q3_PowerupTimer(gclient_t *client, const gitem_t *item)
{
    if (!item || !item->classname)
        return NULL;
    if (!strcmp(item->classname, "item_q3_haste"))
        return &client->q3_haste_framenum;
    if (!strcmp(item->classname, "item_q3_regen"))
        return &client->q3_regen_framenum;
    if (!strcmp(item->classname, "item_q3_flight"))
        return &client->q3_flight_framenum;
    if (!strcmp(item->classname, "item_q3_battlesuit"))
        return &client->q3_battlesuit_framenum;
    return NULL;
}

// Quake III powerups work the moment they are touched; there is no
// powerup inventory. Time adds to what is left.
bool Pickup_Q3Powerup(edict_t *ent, edict_t *other)
{
    int *timer = Q3_PowerupTimer(other->client, ent->item);
    int length;

    if (!timer)
        return false;

    length = ent->item->quantity ? ent->item->quantity : Q3_POWERUP_TIME;
    if (ent->count > 0)
        length = ent->count;

    if (*timer > level.framenum)
        *timer += SECONDS(length);
    else
        *timer = level.framenum + SECONDS(length);

    if (!(ent->spawnflags & DROPPED_ITEM) && deathmatch->value)
        SetRespawn(ent, Q3_RESPAWN_POWERUP);

    return true;
}

static gitem_t *Q3_HeldHoldable(edict_t *ent)
{
    gitem_t *item;
    int     i;

    for (i = 1, item = itemlist + 1; i < game.num_items; i++, item++) {
        if (item->pickup == Pickup_Q3Holdable && ent->client->pers.inventory[i] > 0)
            return item;
    }
    return NULL;
}

// one holdable at a time, as in Quake III
bool Pickup_Q3Holdable(edict_t *ent, edict_t *other)
{
    if (Q3_HeldHoldable(other))
        return false;

    other->client->pers.inventory[ITEM_INDEX(ent->item)] = 1;

    if (!(ent->spawnflags & DROPPED_ITEM) && deathmatch->value)
        SetRespawn(ent, Q3_RESPAWN_HOLDABLE);

    return true;
}

void Use_Q3Medkit(edict_t *ent, gitem_t *item)
{
    // Quake III will not use it at or over the health it gives
    if (ent->health >= ent->max_health + 25)
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    ValidateSelectedItem(ent);

    ent->health = ent->max_health + 25;
    gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/use_medkit.wav"), 1, ATTN_NORM, 0);
}

void Use_Q3Teleporter(edict_t *ent, gitem_t *item)
{
    edict_t *spot;
    vec3_t  origin;

    spot = SelectRandomDeathmatchSpawnPoint();
    if (!spot)
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    ValidateSelectedItem(ent);

    VectorCopy(spot->s.origin, origin);
    origin[2] += 8;
    Q3_TeleportPlayer(ent, origin, spot->s.angles);
}

/*
=================
Q3_PickupHealth / Q3_PickupArmor

The Quake III amounts (g_q3_items 1), with the Quake II models: health 5 /
25 / 50 / 100, the 5 and the 100 able to go to double health; armor 5 / 50 /
100 up to double health in a single pool. That pool is kept as Quake II
combat armor, whose 60% protection is the nearest to Quake III's 66% - or as
body armor if that is what the player already wears.
=================
*/
bool Q3_PickupHealth(edict_t *ent, edict_t *other)
{
    int amount = ent->count;
    int max;

    max = (amount == 5 || amount == 100) ? other->max_health * 2 : other->max_health;
    if (other->health >= max)
        return false;

    other->health += amount;
    if (other->health > max)
        other->health = max;

    if (!(ent->spawnflags & DROPPED_ITEM) && deathmatch->value)
        SetRespawn(ent, Q3_RESPAWN_HEALTH);

    return true;
}

bool Q3_PickupArmor(edict_t *ent, edict_t *other)
{
    gitem_t *combat = FindItemByClassname("item_armor_combat");
    gitem_t *body = FindItemByClassname("item_armor_body");
    int     index = ArmorIndex(other);
    int     amount, count, max = other->max_health * 2;

    if (!strcmp(ent->item->classname, "item_armor_shard"))
        amount = 5;
    else if (!strcmp(ent->item->classname, "item_armor_body"))
        amount = 100;
    else
        amount = 50;

    count = index ? other->client->pers.inventory[index] : 0;
    if (count >= max)
        return false;

    // one pool: move whatever is worn over to the armor that holds it
    if (index && index != ITEM_INDEX(body)) {
        other->client->pers.inventory[index] = 0;
        index = ITEM_INDEX(combat);
    } else if (!index) {
        index = ITEM_INDEX(combat);
    }

    count += amount;
    if (count > max)
        count = max;
    other->client->pers.inventory[index] = count;

    if (!(ent->spawnflags & DROPPED_ITEM) && deathmatch->value)
        SetRespawn(ent, Q3_RESPAWN_ARMOR);

    return true;
}

/*
=================
Q3_GiveSpawnKit

g_q3_kit, from the bot match menu:
  0  Blaster and Machinegun with 100 bullets (Quake III's own kit)
  1  Chainfist and Machinegun with 100 bullets
  2  Blaster only
With the Quake III amounts a player also spawns at 125 health, which runs
down to 100 as Quake III's does.
=================
*/
void Q3_GiveSpawnKit(gclient_t *client)
{
    gitem_t *blaster = FindItemByClassname("weapon_blaster");
    gitem_t *mg = FindItemByClassname("weapon_machinegun");
    gitem_t *fist = FindItemByClassname("weapon_chainfist");
    gitem_t *bullets = FindItemByClassname("ammo_bullets");
    int     kit = g_q3_kit ? g_q3_kit->integer : 0;

    if (!level.q3map)
        return;

    if (kit != 2 && mg && bullets) {
        if (kit == 1 && fist) {
            client->pers.inventory[ITEM_INDEX(fist)] = 1;
            if (blaster)
                client->pers.inventory[ITEM_INDEX(blaster)] = 0;
        }
        client->pers.inventory[ITEM_INDEX(mg)] = 1;
        client->pers.inventory[ITEM_INDEX(bullets)] = 100;
        client->pers.weapon = mg;
        client->pers.selected_item = ITEM_INDEX(mg);
    }

    if (Q3_Amounts())
        client->pers.health = client->pers.max_health + 25;
}

/*
=================
Q3_PmoveGravity

Haste and Flight have to reach client prediction, and the gravity is the one
per-player value that does, exactly: see pmoveParams_t.q3 in common/pmove.h.
=================
*/
short Q3_PmoveGravity(edict_t *ent, short gravity)
{
    gclient_t *client = ent->client;

    if (!level.q3map)
        return gravity;

    gravity &= ~1;
    if (client->q3_flight_framenum > level.framenum)
        gravity = 0;
    if (client->q3_haste_framenum > level.framenum)
        gravity |= 1;
    return gravity;
}

// Haste fires 1.3 times as fast: one extra weapon think every ~3 frames
bool Q3_HasteExtraThink(edict_t *ent)
{
    gclient_t *client = ent->client;

    if (!level.q3map || client->q3_haste_framenum <= level.framenum)
        return false;

    client->q3_haste_accum += 0.3f;
    if (client->q3_haste_accum >= 1) {
        client->q3_haste_accum -= 1;
        return true;
    }
    return false;
}

/*
=================
Q3_BattleSuitDamage

Quake III's G_Damage: the suit takes all splash and falling damage and half
of anything else. Knockback has already been applied, so rocket jumps still
work. Returns the damage still to take.
=================
*/
int Q3_BattleSuitDamage(edict_t *targ, int damage, int dflags, int mod)
{
    gclient_t *client = targ->client;

    if (!client || client->q3_battlesuit_framenum <= level.framenum)
        return damage;
    if (dflags & DAMAGE_NO_PROTECTION)
        return damage;

    if (client->q3_suit_sound_framenum <= level.framenum) {
        gi.sound(targ, CHAN_ITEM, Q3_SoundIndex("items/protect3.wav"), 1, ATTN_NORM, 0);
        client->q3_suit_sound_framenum = level.framenum + SECONDS(0.5f);
    }

    if ((dflags & DAMAGE_RADIUS) || (mod & ~MOD_FRIENDLY_FIRE) == MOD_FALLING)
        return 0;

    damage /= 2;
    return damage;
}

bool Q3_BattleSuitActive(gclient_t *client)
{
    return level.q3map && client->q3_battlesuit_framenum > level.framenum;
}

/*
=================
Q3_FallingDamage

Quake III's PM_CrashLand: 10 for a long fall, 5 for a medium one, nothing
for less, from the same impact measure Quake II uses. Quake II's own scale
hurts a lot more, and Quake III's jump pads drop players from heights its
maps were built for.
=================
*/
/*
=================
Q3_Knockback

The bot match's Quake III physics (level.q3_physics): Quake III's push for a
hit player - g_knockback 1000 over its fixed mass of 200, a knockback of at
most 200, the same for your own rockets as anyone's - and the timer during
which the player has no ground friction and only air control, so the push
carries (pmove.c: PMF_TIME_LAND stands for PMF_TIME_KNOCKBACK in q3move).
The timer is set only when none runs, so another hit can't cut one short.
=================
*/
void Q3_Knockback(edict_t *targ, const vec3_t dir, int knockback)
{
    gclient_t   *client = targ->client;
    vec3_t      kvel;
    int         t;

    if (knockback > 200)
        knockback = 200;
    if (knockback <= 0)
        return;

    VectorNormalize2(dir, kvel);
    VectorMA(targ->velocity, 1000.0f * knockback / 200.0f, kvel, targ->velocity);

    if (!client || client->ps.pmove.pm_time)
        return;

    t = knockback * 2;      // milliseconds
    if (t < 50)
        t = 50;
    if (t > 200)
        t = 200;
    client->ps.pmove.pm_time = t >> 3;      // pmove's 8 ms units
    client->ps.pmove.pm_flags |= PMF_TIME_LAND;
}

int Q3_FallingDamage(float delta)
{
    if (delta > 60)
        return 10;
    if (delta > 40)
        return 5;
    return 0;
}

/*
=================
Q3_ClientFrame

Once a second: Regen, and with the Quake III amounts the run-down of health
and armor over 100. In the last five seconds of a powerup, the wear-off tick.
=================
*/
void Q3_ClientFrame(edict_t *ent)
{
    gclient_t   *client = ent->client;
    int         *timers[5];
    int         i, index;

    if (!level.q3map || ent->health <= 0)
        return;

    timers[0] = &client->q3_haste_framenum;
    timers[1] = &client->q3_regen_framenum;
    timers[2] = &client->q3_flight_framenum;
    timers[3] = &client->q3_battlesuit_framenum;
    timers[4] = &client->quad_framenum;     // Quake II's quad, Quake III's tick (p_view.c)

    for (i = 0; i < q_countof(timers); i++) {
        int left = *timers[i] - level.framenum;

        if (left > 0 && left <= SECONDS(5) && !(left % BASE_FRAMERATE))
            gi.sound(ent, CHAN_ITEM, Q3_SoundIndex("items/wearoff.wav"), 1, ATTN_NORM, 0);
    }

    if (client->q3_tick_framenum > level.framenum)
        return;
    client->q3_tick_framenum = level.framenum + BASE_FRAMERATE;

    if (client->q3_regen_framenum > level.framenum) {
        int max = ent->max_health;

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
    } else if (Q3_Amounts() && ent->health > ent->max_health) {
        ent->health--;
    }

    if (Q3_Amounts()) {
        index = ArmorIndex(ent);
        if (index && client->pers.inventory[index] > ent->max_health)
            client->pers.inventory[index]--;
    }
}

// the Battle Suit's gold shell and Regen's red one
void Q3_ClientEffects(edict_t *ent)
{
    gclient_t *client = ent->client;

    if (!level.q3map || ent->health <= 0)
        return;

    if (client->q3_battlesuit_framenum > level.framenum) {
        ent->s.effects |= EF_COLOR_SHELL;
        ent->s.renderfx |= RF_SHELL_RED | RF_SHELL_GREEN;
    }
    if (client->q3_regen_framenum > level.framenum && (level.framenum & 4)) {
        ent->s.effects |= EF_COLOR_SHELL;
        ent->s.renderfx |= RF_SHELL_RED;
    }
}

// Flight hums while it lasts
void Q3_ClientSound(edict_t *ent)
{
    if (!level.q3map || ent->health <= 0)
        return;
    if (ent->client->q3_flight_framenum > level.framenum)
        ent->s.sound = Q3_SoundIndex("items/flight.wav");
}

// the HUD powerup timer: false if none of these is running
bool Q3_PowerupTimerStat(edict_t *ent)
{
    static const struct {
        const char  *icon;
        size_t      ofs;
    } timed[] = {
        { "/icons/haste.tga",       CLOFS(q3_haste_framenum) },
        { "/icons/regen.tga",       CLOFS(q3_regen_framenum) },
        { "/icons/flight.tga",      CLOFS(q3_flight_framenum) },
        { "/icons/envirosuit.tga",  CLOFS(q3_battlesuit_framenum) },
    };
    gclient_t   *client = ent->client;
    const char  *icon = NULL;
    int         i, best = 0;

    if (!level.q3map)
        return false;

    for (i = 0; i < q_countof(timed); i++) {
        int end = *(int *)((byte *)client + timed[i].ofs);

        if (end > level.framenum && (!icon || end < best)) {
            icon = timed[i].icon;
            best = end;
        }
    }
    if (!icon)
        return false;

    client->ps.stats[STAT_TIMER_ICON] = gi.imageindex(icon);
    client->ps.stats[STAT_TIMER] = (best - level.framenum + BASE_FRAMERATE - 1) / BASE_FRAMERATE;
    return true;
}

void Q3_ClearPowerups(gclient_t *client)
{
    client->q3_haste_framenum = 0;
    client->q3_regen_framenum = 0;
    client->q3_flight_framenum = 0;
    client->q3_battlesuit_framenum = 0;
}

/*
==============================================================================

TELEPORTERS

Quake III throws the player out of a teleporter at 400 in the direction the
destination faces, where Quake II stops them dead, and its maps are laid out
for that. Its arrival and departure have their own sounds.

==============================================================================
*/

/*
==============================================================================

GIBS

Quake III's GibEntity (g_combat.c): the body vanishes and the clients throw
the pieces themselves (EV_GIB_PLAYER; here TE_Q3_GIBS, q3weapons.c), eleven of
them, further than Quake II's five. It gibs at -40 or below where Quake II
needs less than -40, and the kill command always gibs.

==============================================================================
*/

// Quake III's game is on for this victim: its map, its campaign, its movement
// or its guns (g_q3_weapons 2: the player's own model decides). A body in the
// queue has no client; its skinnum still names the player it was.
bool Q3_GibStyle(const edict_t *victim)
{
    const edict_t   *player = victim;
    int             n;

    if (level.q3map || SP_Active())
        return true;
    if (!deathmatch->value)
        return false;
    if (level.q3_physics || level.q3_weapons == 1)
        return true;
    if (level.q3_weapons != 2 || !victim)
        return false;

    if (!player->client) {
        n = victim->s.skinnum & 0xff;
        if (n >= game.maxclients)
            return false;
        player = g_edicts + 1 + n;
    }
    return Q3W_PlayerQ3(player);
}

// player_die's test, Quake III's way (meansOfDeath is set by now)
bool Q3_GibDeath(int health)
{
    return health <= -40 || (meansOfDeath & ~MOD_FRIENDLY_FIRE) == MOD_SUICIDE;
}

void Q3_GibEntity(edict_t *self)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q3_GIBS);
    gi.WritePosition(self->s.origin);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    // the body goes: nothing drawn, nothing to hit. A player's own view drops
    // to the floor (PM_GIB), where Quake III's dead camera sits.
    self->s.modelindex = 0;
    self->s.modelindex2 = 0;
    self->s.modelindex3 = 0;
    self->s.modelindex4 = 0;
    self->s.effects = 0;
    self->s.sound = 0;
    self->takedamage = DAMAGE_NO;
    self->solid = SOLID_NOT;
    if (self->client) {
        self->client->anim_priority = ANIM_DEATH;
        self->client->anim_end = self->s.frame;
    }
    gi.linkentity(self);
}

void Q3_TeleportPlayer(edict_t *player, const vec3_t origin, const vec3_t angles)
{
    gclient_t   *client = player->client;
    vec3_t      forward;
    int         i;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_TELEPORT_EFFECT);
    gi.WritePosition(player->s.origin);
    gi.multicast(player->s.origin, MULTICAST_PVS);
    gi.positioned_sound(player->s.origin, g_edicts, CHAN_AUTO, Q3_SoundIndex("world/teleout.wav"), 1, ATTN_NORM, 0);

    // CTF: the hook stays behind, as at a Quake II teleporter
    if (client)
        CTF_PlayerResetGrapple(player);

    // unlinked, so KillBox does not find the arriving player
    gi.unlinkentity(player);

    VectorCopy(origin, player->s.origin);
    player->s.origin[2] += 1;
    VectorCopy(player->s.origin, player->s.old_origin);

    AngleVectors(angles, forward, NULL, NULL);
    VectorScale(forward, 400, player->velocity);

    if (client) {
        VectorCopy(player->velocity, client->oldvelocity);
        for (i = 0; i < 3; i++)
            client->ps.pmove.delta_angles[i] = ANGLE2SHORT(angles[i] - client->resp.cmd_angles[i]);
        VectorClear(client->ps.viewangles);
        VectorClear(client->v_angle);
        client->q3_jumppad = NULL;
    }
    VectorClear(player->s.angles);

    player->s.event = EV_OTHER_TELEPORT;    // no lerp from the old spot

    KillBox(player);
    gi.linkentity(player);

    gi.sound(player, CHAN_AUTO, Q3_SoundIndex("world/telein.wav"), 1, ATTN_NORM, 0);
}

void q3_teleport_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    edict_t *dest;

    if (!other->client || other->health <= 0)
        return;

    dest = G_PickTarget(self->target);
    if (!dest) {
        gi.dprintf("trigger_teleport at %s: no destination \"%s\"\n", vtos(self->absmin), self->target);
        return;
    }

    Q3_TeleportPlayer(other, dest->s.origin, dest->s.angles);
}

static void SP_q3_trigger_teleport(edict_t *self)
{
    // spawnflags 1: spectators only. Quake II spectators never touch
    // triggers, so such a teleporter has nobody to carry.
    if (!self->target || (self->spawnflags & 1)) {
        G_FreeEdict(self);
        return;
    }

    InitTrigger(self);
    self->touch = q3_teleport_touch;
    gi.linkentity(self);

    Q3_SoundIndex("world/telein.wav");
    Q3_SoundIndex("world/teleout.wav");
}

/*
==============================================================================

JUMP PADS

A Quake III trigger_push names a target_position, and the push is worked out
(AimAtTarget, g_trigger.c) so the player's arc peaks exactly there. The
velocity replaces the player's own, and the pad thumps once per jump.

==============================================================================
*/

static bool q3_jumppad_aim(edict_t *self)
{
    edict_t *target;
    vec3_t  origin, vel;
    float   height, gravity, time, dist;

    if (self->count)
        return self->count > 0;

    self->count = -1;   // tried

    target = G_PickTarget(self->target);
    if (!target) {
        gi.dprintf("trigger_push at %s: no target \"%s\"\n", vtos(self->absmin), self->target);
        return false;
    }

    VectorAdd(self->absmin, self->absmax, origin);
    VectorScale(origin, 0.5f, origin);

    height = target->s.origin[2] - origin[2];
    gravity = sv_gravity->value;
    if (height <= 0 || gravity <= 0) {
        gi.dprintf("trigger_push at %s: target is not above it\n", vtos(origin));
        return false;
    }
    time = sqrtf(height / (0.5f * gravity));

    VectorSubtract(target->s.origin, origin, vel);
    vel[2] = 0;
    dist = VectorNormalize(vel);
    VectorScale(vel, dist / time, vel);
    vel[2] = time * gravity;

    VectorCopy(vel, self->movedir);
    self->count = 1;
    return true;
}

/*
IN THE PAD, NOT ITS BOX. Quake III's G_TouchTriggers asks every trigger
whether the player is really in its brushes (trap_EntityContact); Quake II's
touch is the trigger's bounding box. For a sloped pad that box is far more
than the pad: The Longest Yard's armor pads slope up toward the armor, and
their boxes run some 60 units above the slab at its low end. The pad sets the
velocity again on every frame it is touched, so a player still "on" it well
into the climb was launched from there, on an arc that peaks at the armor
from the pad's middle - and went over it. Matt: "I am very rarely ever able
to get the yellow armor on q3dm17".

The pad's brushes carry CONTENTS_TRIGGER (bsp_q3.c), which no trace asks
for, and a trigger is not in the solid list traces look through. So it is
made solid for one position test and put back. A pad whose brushes cannot be
found that way (the test of its own box fails: no trigger contents, or no
inline model) keeps the box. self->sounds: 0 untested, 1 brushes, -1 box.
*/
static bool q3_trigger_overlap(edict_t *self, const vec3_t origin,
                               const vec3_t mins, const vec3_t maxs, edict_t *pass)
{
    trace_t tr;

    self->solid = SOLID_BSP;
    gi.linkentity(self);
    tr = gi.trace(origin, mins, maxs, origin, pass, CONTENTS_TRIGGER);
    self->solid = SOLID_TRIGGER;
    gi.linkentity(self);

    return tr.startsolid && tr.ent == self;
}

static bool q3_trigger_contact(edict_t *self, edict_t *other)
{
    if (!self->sounds) {
        self->sounds = -1;
        if (self->model && self->model[0] == '*' && self->s.modelindex > 1 &&
            q3_trigger_overlap(self, vec3_origin, self->absmin, self->absmax, NULL))
            self->sounds = 1;
        else
            gi.dprintf("trigger_push at %s: no trigger brushes, touching its box\n",
                       vtos(self->absmin));
    }
    if (self->sounds < 0)
        return true;

    return q3_trigger_overlap(self, other->s.origin, other->mins, other->maxs, other);
}

void q3_jumppad_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    gclient_t *client = other->client;

    if (!client || other->health <= 0 || other->movetype == MOVETYPE_NOCLIP)
        return;
    if (client->q3_flight_framenum > level.framenum)
        return;     // fliers do not bounce
    if (!q3_jumppad_aim(self))
        return;
    if (!q3_trigger_contact(self, other))
        return;

    if (client->q3_jumppad != self || client->q3_jumppad_framenum < level.framenum - 1)
        gi.sound(other, CHAN_AUTO, self->noise_index, 1, ATTN_NORM, 0);
    client->q3_jumppad = self;
    client->q3_jumppad_framenum = level.framenum;

    VectorCopy(self->movedir, other->velocity);
    VectorCopy(other->velocity, client->oldvelocity);  // no landing damage from the launch
    other->groundentity = NULL;
}

static void SP_q3_trigger_push(edict_t *self)
{
    InitTrigger(self);
    self->touch = q3_jumppad_touch;
    self->noise_index = Q3_SoundIndex("world/jumppad.wav");
    self->count = 0;
    self->sounds = 0;   // brushes untested - q3_trigger_contact
    gi.linkentity(self);
}

/*
Where a pad's player comes down, as a standing player's origin (for the bots:
the navmesh's link, and where they lean in the air). The target is the top of
the arc, not its end - the player carries on past it at the same speed across
- so the flight is followed, a player's box from the pad on the pad's
velocity under gravity, sliding off walls as movement does, to the first
floor it lands on. Under the target, if that finds none.
*/
#define PAD_SIM_STEP    0.025f
#define PAD_SIM_STEPS   320     // 8 seconds
#define PAD_SIM_MASK    (MASK_PLAYERSOLID & ~CONTENTS_MONSTER)   // the map, not who is in it

static bool q3_jumppad_fly(edict_t *pad, vec3_t out)
{
    static const vec3_t mins = { -16, -16, -24 }, maxs = { 16, 16, 32 };
    vec3_t  pos, vel, end;
    trace_t tr;
    float   gravity = sv_gravity->value;
    int     i;

    if (!q3_jumppad_aim(pad) || gravity <= 0)
        return false;

    // standing on the pad, under the middle of its trigger
    VectorAdd(pad->absmin, pad->absmax, pos);
    VectorScale(pos, 0.5f, pos);
    VectorSet(end, pos[0], pos[1], pos[2] - 512);
    tr = gi.trace(pos, NULL, NULL, end, NULL, PAD_SIM_MASK);
    if (tr.startsolid || tr.fraction == 1.0f)
        return false;
    VectorCopy(tr.endpos, pos);
    pos[2] -= mins[2] - 1;
    tr = gi.trace(pos, mins, maxs, pos, NULL, PAD_SIM_MASK);
    if (tr.startsolid)
        return false;

    VectorCopy(pad->movedir, vel);
    for (i = 0; i < PAD_SIM_STEPS; i++) {
        vel[2] -= gravity * PAD_SIM_STEP;
        VectorMA(pos, PAD_SIM_STEP, vel, end);
        tr = gi.trace(pos, mins, maxs, end, NULL, PAD_SIM_MASK);
        if (tr.allsolid)
            return false;
        VectorCopy(tr.endpos, pos);
        if (tr.fraction == 1.0f)
            continue;

        if (tr.plane.normal[2] >= 0.7f && vel[2] <= 0) {
            VectorCopy(pos, out);
            return true;
        }

        // a wall or a ceiling: along it
        {
            float back = DotProduct(vel, tr.plane.normal) * 1.001f;

            VectorMA(vel, -back, tr.plane.normal, vel);
        }
    }

    return false;
}

bool Q3_JumpPadLanding(edict_t *pad, vec3_t out)
{
    edict_t *target;
    trace_t tr;
    vec3_t  end;

    if (pad->touch != q3_jumppad_touch || !pad->target)
        return false;
    if (q3_jumppad_fly(pad, out))
        return true;

    target = G_PickTarget(pad->target);
    if (!target)
        return false;

    VectorCopy(target->s.origin, end);
    end[2] -= 2048;
    tr = gi.trace(target->s.origin, NULL, NULL, end, NULL, MASK_PLAYERSOLID);
    VectorCopy(tr.endpos, out);
    out[2] += 24;
    return true;
}

/*
==============================================================================

SPEAKERS

Quake III's target_speaker. Looped ones toggle like Quake II's; a one-shot
plays on use, at the activator (8) or everywhere (4); and one with a
"random" key repeats on its own every wait +- random seconds, which is how
Quake III maps do their occasional ambient sounds.

==============================================================================
*/

static void q3_speaker_play(edict_t *self, edict_t *activator)
{
    if ((self->spawnflags & Q3_SPEAKER_ACTIVATOR) && activator && activator->inuse)
        gi.sound(activator, CHAN_AUTO, self->noise_index, 1, ATTN_NORM, 0);
    else if (self->spawnflags & Q3_SPEAKER_GLOBAL)
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO | CHAN_RELIABLE, self->noise_index, 1, ATTN_NONE, 0);
    else
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, self->noise_index, 1, ATTN_NORM, 0);
}

static void q3_speaker_use(edict_t *self, edict_t *other, edict_t *activator)
{
    if (self->spawnflags & (Q3_SPEAKER_LOOPED_ON | Q3_SPEAKER_LOOPED_OFF)) {
        self->s.sound = self->s.sound ? 0 : self->noise_index;
        return;
    }
    q3_speaker_play(self, activator);
}

static void q3_speaker_think(edict_t *self)
{
    float delay;

    q3_speaker_play(self, NULL);

    delay = self->wait + crandom() * self->random;
    if (delay < 0.1f)
        delay = 0.1f;
    self->nextthink = level.framenum + SECONDS(delay);
}

static void SP_q3_target_speaker(edict_t *self)
{
    if (!st.noise || !st.noise[0]) {
        gi.dprintf("target_speaker with no noise at %s\n", vtos(self->s.origin));
        G_FreeEdict(self);
        return;
    }

    self->noise_index = Q3_SoundIndex(st.noise);
    self->use = q3_speaker_use;
    self->volume = 1;
    self->attenuation = (self->spawnflags & Q3_SPEAKER_GLOBAL) ? ATTN_NONE : ATTN_NORM;

    if (self->spawnflags & Q3_SPEAKER_LOOPED_ON)
        self->s.sound = self->noise_index;

    // cgame's CG_Speaker repeats only those with a random spread
    if (!(self->spawnflags & (Q3_SPEAKER_LOOPED_ON | Q3_SPEAKER_LOOPED_OFF)) && self->random > 0) {
        self->think = q3_speaker_think;
        self->nextthink = level.framenum + SECONDS(self->wait + random() * self->random) + 1;
    }

    gi.linkentity(self);
}

/*
==============================================================================

MOVERS

func_bobbing and func_pendulum move on a sine, which Quake II's pushers can
follow by being given each frame the velocity that lands them on the curve
next frame.

==============================================================================
*/

static void q3_mover_blocked(edict_t *self, edict_t *other)
{
    if (self->dmg)
        T_Damage(other, self, self, vec3_origin, other->s.origin, vec3_origin, self->dmg, 1, 0, MOD_CRUSH);
}

static float q3_sine_at(edict_t *self, int framenum)
{
    // self->wait: period in seconds; self->delay: phase in seconds
    float t = framenum * FRAMETIME + self->delay;
    return sinf(t / self->wait * 2 * M_PI);
}

static void q3_bobbing_think(edict_t *self)
{
    vec3_t  target;
    int     i;

    VectorMA(self->pos1, self->accel * q3_sine_at(self, level.framenum + 1), self->movedir, target);
    for (i = 0; i < 3; i++)
        self->velocity[i] = (target[i] - self->s.origin[i]) / FRAMETIME;
    self->nextthink = level.framenum + 1;
}

static void q3_pendulum_think(edict_t *self)
{
    float roll = self->pos2[ROLL] + self->accel * q3_sine_at(self, level.framenum + 1);

    self->avelocity[ROLL] = (roll - self->s.angles[ROLL]) / FRAMETIME;
    self->nextthink = level.framenum + 1;
}

static void q3_mover_common(edict_t *self)
{
    gi.setmodel(self, self->model);
    self->solid = SOLID_BSP;
    self->movetype = MOVETYPE_PUSH;
    self->blocked = q3_mover_blocked;
    if (!(st.keys_specified & SPAWNKEY_DMG))
        self->dmg = 2;
    if (st.model2 && st.model2[0])
        self->s.modelindex2 = gi.modelindex(st.model2);
    if (st.noise && st.noise[0])
        self->s.sound = Q3_SoundIndex(st.noise);
    self->nextthink = level.framenum + 1;
    gi.linkentity(self);
}

static void SP_q3_func_bobbing(edict_t *self)
{
    // "speed": seconds per cycle (4), "height": amplitude (32), "phase": 0..1
    self->wait = self->speed > 0 ? self->speed : 4;
    self->accel = st.height ? st.height : 32;
    self->delay = st.phase * self->wait;

    VectorClear(self->movedir);
    if (self->spawnflags & 1)
        self->movedir[0] = 1;
    else if (self->spawnflags & 2)
        self->movedir[1] = 1;
    else
        self->movedir[2] = 1;

    VectorCopy(self->s.origin, self->pos1);
    VectorClear(self->s.angles);
    self->think = q3_bobbing_think;
    q3_mover_common(self);
}

static void SP_q3_func_pendulum(edict_t *self)
{
    float length, freq;

    // "speed": swing in degrees (30). The period is a pendulum's, from the
    // length of the model below its origin.
    self->accel = self->speed ? self->speed : 30;

    gi.setmodel(self, self->model);
    length = fabsf(self->mins[2]);
    if (length < 8)
        length = 8;
    freq = 1 / (2 * M_PI) * sqrtf(sv_gravity->value / (3 * length));
    self->wait = 1 / freq;
    self->delay = st.phase * self->wait;

    VectorCopy(self->s.angles, self->pos2);
    self->think = q3_pendulum_think;
    q3_mover_common(self);
}

/*
==============================================================================

TARGETS

==============================================================================
*/

static void q3_delay_think(edict_t *self)
{
    G_UseTargets(self, self->activator);
}

static void q3_delay_use(edict_t *self, edict_t *other, edict_t *activator)
{
    float delay = self->wait + crandom() * self->random;

    self->activator = activator;
    self->think = q3_delay_think;
    self->nextthink = level.framenum + (delay > 0 ? SECONDS(delay) : 1);
}

static void SP_q3_target_delay(edict_t *self)
{
    // older maps spell "wait" as "delay"
    if (!self->wait)
        self->wait = self->delay ? self->delay : 1;
    self->delay = 0;
    self->use = q3_delay_use;
}

static void q3_relay_use(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *t, *pick = NULL;
    int     n = 0;

    if (activator && activator->client && G_CTF()) {
        if ((self->spawnflags & 1) && activator->client->resp.team != TEAM_RED)
            return;
        if ((self->spawnflags & 2) && activator->client->resp.team == TEAM_RED)
            return;
    }

    if (!(self->spawnflags & 4)) {
        G_UseTargets(self, activator);
        return;
    }

    // RANDOM: one of the targets
    for (t = NULL; (t = G_Find(t, FOFS(targetname), self->target)) != NULL; )
        if (t->use && Q_rand() % ++n == 0)
            pick = t;
    if (pick)
        pick->use(pick, self, activator);
}

static void SP_q3_target_relay(edict_t *self)
{
    self->use = q3_relay_use;
}

static void q3_give_use(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *t, *give;

    if (!activator || !activator->client)
        return;

    for (t = NULL; (t = G_Find(t, FOFS(targetname), self->target)) != NULL; ) {
        if (!t->item)
            continue;

        // hand over a copy: a dropped item is taken and gone, and the
        // original stays where it is for the next time
        give = G_Spawn();
        give->classname = t->classname;
        give->item = t->item;
        give->count = t->count;
        give->style = t->style;
        give->spawnflags = DROPPED_ITEM;
        VectorCopy(activator->s.origin, give->s.origin);
        Touch_Item(give, activator, NULL, NULL);
        if (give->inuse && give->item)
            G_FreeEdict(give);
    }
}

static void SP_q3_target_give(edict_t *self)
{
    self->use = q3_give_use;
}

static void q3_remove_powerups_use(edict_t *self, edict_t *other, edict_t *activator)
{
    gclient_t *client;

    if (!activator || !(client = activator->client))
        return;

    Q3_ClearPowerups(client);
    client->quad_framenum = 0;
    client->invisible_framenum = 0;
    client->enviro_framenum = 0;
}

static void SP_q3_target_remove_powerups(edict_t *self)
{
    self->use = q3_remove_powerups_use;
}

static void q3_print_use(edict_t *self, edict_t *other, edict_t *activator)
{
    int i;

    if (!self->message)
        return;

    if ((self->spawnflags & 4) && activator && activator->client) {
        gi.centerprintf(activator, "%s", self->message);
        return;
    }

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *e = g_edicts + i;

        if (!e->inuse || !e->client)
            continue;
        if ((self->spawnflags & 1) && e->client->resp.team != TEAM_RED)
            continue;
        if ((self->spawnflags & 2) && e->client->resp.team == TEAM_RED)
            continue;
        gi.centerprintf(e, "%s", self->message);
    }
}

static void SP_q3_target_print(edict_t *self)
{
    self->use = q3_print_use;
}

// shooter_grenade / _rocket / _plasma: fire along the angle, or at the
// target, with up to "random" degrees of spread
static void q3_shooter_use(edict_t *self, edict_t *other, edict_t *activator)
{
    vec3_t  dir, up, right;
    float   spread = self->random;

    if (self->enemy) {
        VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
        VectorNormalize(dir);
    } else {
        VectorCopy(self->movedir, dir);
    }

    if (spread > 0) {
        VectorSet(up, 0, 0, 1);
        if (fabsf(dir[2]) > 0.9f)
            VectorSet(up, 1, 0, 0);
        CrossProduct(dir, up, right);
        VectorNormalize(right);
        CrossProduct(right, dir, up);
        VectorMA(dir, crandom() * spread, up, dir);
        VectorMA(dir, crandom() * spread, right, dir);
        VectorNormalize(dir);
    }

    switch (self->style) {
    case 0:
        fire_grenade(self, self->s.origin, dir, 100, 700, 2.5f, 150);
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, gi.soundindex("weapons/grenlf1a.wav"), 1, ATTN_NORM, 0);
        break;
    case 1:
        fire_rocket(self, self->s.origin, dir, 100, 900, 120, 100);
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, gi.soundindex("weapons/rocklf1a.wav"), 1, ATTN_NORM, 0);
        break;
    default:
        fire_blaster(self, self->s.origin, dir, 20, 1000, EF_HYPERBLASTER, true);
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, gi.soundindex("weapons/hyprbf1a.wav"), 1, ATTN_NORM, 0);
        break;
    }
}

static void q3_shooter_aim(edict_t *self)
{
    if (self->target)
        self->enemy = G_PickTarget(self->target);
}

static void SP_q3_shooter(edict_t *self, int kind)
{
    G_SetMovedir(self->s.angles, self->movedir);
    self->style = kind;
    self->random = self->random > 0 ? sinf(M_PI * self->random / 180) : 0;
    self->use = q3_shooter_use;
    self->svflags |= SVF_NOCLIENT;
    self->think = q3_shooter_aim;
    self->nextthink = level.framenum + 2;
}

/*
==============================================================================

ENTITY TRANSLATION

==============================================================================
*/

// classnames that become a Quake II one and spawn as that
static const struct {
    const char *from;
    const char *to;
} q3_renames[] = {
    // Matt's weapon table: the rest keep their names
    { "weapon_gauntlet",        "weapon_chainfist" },
    { "weapon_shotgun",         "weapon_supershotgun" },
    { "weapon_plasmagun",       "weapon_hyperblaster" },
    { "weapon_lightning",       "weapon_plasmabeam" },
    { "ammo_lightning",         "ammo_cells" },
    { "ammo_bfg",               "ammo_cells" },

    { "item_invis",             "item_invisibility" },
    { "item_haste",             "item_q3_haste" },
    { "item_regen",             "item_q3_regen" },
    { "item_flight",            "item_q3_flight" },
    { "holdable_medkit",        "item_q3_medkit" },
    { "holdable_teleporter",    "item_q3_teleporter" },

    { "team_CTF_redflag",       "item_flag_team1" },
    { "team_CTF_blueflag",      "item_flag_team2" },
    { "team_CTF_redplayer",     "info_player_team1" },
    { "team_CTF_redspawn",      "info_player_team1" },
    { "team_CTF_blueplayer",    "info_player_team2" },
    { "team_CTF_bluespawn",     "info_player_team2" },

    { "target_position",        "info_notnull" },
    { "misc_teleporter_dest",   "info_notnull" },   // a point; no Quake II pad
    { "func_static",            "func_wall" },
};

// nothing for the game to do: lights and misc_models are compiled into
// the map, portals are the renderer's, the rest are bot or UI hints
static const char *const q3_dropped[] = {
    "light", "misc_model", "misc_portal_surface", "misc_portal_camera",
    "info_camp", "info_null", "info_firstplace", "info_secondplace",
    "info_thirdplace", "info_spectator_start", "target_location",
    "target_score", "func_group", "info_player_start",
};

static bool q3_is_item(const char *classname)
{
    return FindItemByClassname((char *)classname) != NULL ||
           !strncmp(classname, "item_health", 11);
}

static void q3_item_suspended(edict_t *ent)
{
    vec3_t origin;

    VectorCopy(ent->s.origin, origin);
    droptofloor(ent);
    if (!ent->inuse)
        return;
    VectorCopy(origin, ent->s.origin);
    ent->movetype = MOVETYPE_NONE;
    gi.linkentity(ent);
}

// Quake III powerups are first seen 30 to 60 seconds into the game
static void q3_powerup_first(edict_t *ent)
{
    droptofloor(ent);
    if (ent->inuse && !(ent->svflags & SVF_NOCLIENT))
        SetRespawn(ent, 45 + crandom() * 15);
}

// Quake III's health models by size (small, medium, large, mega): the cross,
// then the sphere round it
static const char *const q3_health_models[4][2] = {
    { "models/powerups/health/small_cross.md3",  "models/powerups/health/small_sphere.md3" },
    { "models/powerups/health/medium_cross.md3", "models/powerups/health/medium_sphere.md3" },
    { "models/powerups/health/large_cross.md3",  "models/powerups/health/large_sphere.md3" },
    { "models/powerups/health/mega_cross.md3",   "models/powerups/health/mega_sphere.md3" },
};

// a health item's size from its classname: 0 small, 1 medium, 2 large, 3 mega
static int q3_health_size(const char *classname)
{
    if (!strcmp(classname, "item_health_small"))
        return 0;
    if (!strcmp(classname, "item_health_large"))
        return 2;
    if (!strcmp(classname, "item_health_mega"))
        return 3;
    return 1;
}

/*
=================
Q3_HealthSound

A health item's pickup sound under the Quake III amounts: Quake III's own, one
per size as it had them. Quake II chooses by amount (2/10/25/100, Touch_Item),
which gave the 5 and the 50 the mega health's sound and the 25 the large one's
(Matt, 2026-11-16). 0 when Quake II's choice stands.
=================
*/
int Q3_HealthSound(const edict_t *ent)
{
    static const char *const sounds[4] = {
        "items/s_health.wav", "items/n_health.wav", "items/l_health.wav", "items/m_health.wav"
    };

    if (!Q3_Amounts() || !ent->classname || strncmp(ent->classname, "item_health", 11))
        return 0;
    return Q3_SoundIndex(sounds[q3_health_size(ent->classname)]);
}

// Quake III's quad damage pickup (Matt, 2026-11-17): Quake II's item and rules,
// Quake III's model and the ring round it. The quad's look on a player is the
// renderer's (client entities.c RF_Q3_QUAD).
#define Q3_QUAD_MODEL   "models/powerups/instant/quad.md3"

static const char *q3_powerup_ring(const char *classname)
{
    // Quake II's "Health" item has no classname of its own (the health
    // boxes spawn through SP_item_health) - every Quake III map has some
    if (!classname)
        return NULL;
    if (!strcmp(classname, "item_quad"))
        return "models/powerups/instant/quad_ring.md3";
    if (!strcmp(classname, "item_q3_haste"))
        return "models/powerups/instant/haste_ring.md3";
    if (!strcmp(classname, "item_q3_regen"))
        return "models/powerups/instant/regen_ring.md3";
    if (!strcmp(classname, "item_q3_flight"))
        return "models/powerups/instant/flight_ring.md3";
    if (!strcmp(classname, "item_q3_battlesuit"))
        return "models/powerups/instant/enviro_ring.md3";
    return NULL;
}

/*
=================
Q3_DroppedItem

An item thrown on a Quake III map (Drop_Item: a dying player's quad under
DF_QUAD_DROP, or what "drop" throws) looks like the one Q3_SpawnEntity placed.
=================
*/
void Q3_DroppedItem(edict_t *dropped)
{
    const char *ring;

    if (!level.q3map || !dropped->item || !dropped->item->classname)
        return;

    if (!strcmp(dropped->item->classname, "item_quad"))
        gi.setmodel(dropped, Q3_QUAD_MODEL);

    ring = q3_powerup_ring(dropped->item->classname);
    if (ring)
        dropped->s.modelindex2 = gi.modelindex(ring);
}

/*
==============================================================================

QUAKE III POWERUP SOUNDS (Matt, 2026-11-17)

On a Quake III map its powerups - the quad (Quake II's item_quad, which takes
effect at the pickup there: Pickup_Powerup) and its own four (Pickup_Q3Powerup:
haste, regen, flight, battle suit) - sound the way Quake III's did:

- picked up: the powerup's sound to everyone, as Quake III broadcast a powerup
  pickup (EV_GLOBAL_ITEM_PICKUP) - quaddamage.wav for the quad - and
  n_health.wav at the player, the sound cgame gave a powerup's own
  EV_ITEM_PICKUP
- back on its spot, the first time included: poweruprespawn.wav to everyone
  (RespawnItem's EV_GLOBAL_SOUND)

and the quad's own:

- switched on (Use_Quad - at the pickup on these maps, so the broadcast pickup
  sound on the same channel takes its place): quaddamage.wav at the player,
  for Quake II's damage.wav
- firing: Quake III's damage3.wav (cgs.media.quadSound) for Quake II's
- running out: wearoff.wav once a second for the last five (Q3_ClientFrame),
  for Quake II's damage2.wav at three (p_view.c), as the other four do

==============================================================================
*/

// The pickup sound of one of those powerups on a Quake III map; 0 for anything else
static int q3_powerup_pickup_sound(const gitem_t *item)
{
    if (!level.q3map || !item || !item->classname)
        return 0;
    if (!strcmp(item->classname, "item_quad"))
        return Q3_SoundIndex("items/quaddamage.wav");
    if (item->pickup == Pickup_Q3Powerup && item->pickup_sound)
        return gi.soundindex(item->pickup_sound);   // already "#baseq3/sound/..."
    return 0;
}

// Q3_SpawnEntity: everything a powerup will play, before the game starts
static void q3_powerup_precache(const gitem_t *item)
{
    q3_powerup_pickup_sound(item);
    Q3_SoundIndex("items/n_health.wav");
    Q3_SoundIndex("items/poweruprespawn.wav");

    if (!strcmp(item->classname, "item_quad")) {
        Q3_SoundIndex("items/damage3.wav");
        Q3_SoundIndex("items/wearoff.wav");
    }
}

// Touch_Item: true when the pickup sounds were Quake III's
bool Q3_PowerupPickupSound(edict_t *ent, edict_t *other)
{
    int sound = q3_powerup_pickup_sound(ent->item);

    if (!sound)
        return false;

    gi.sound(other, CHAN_AUTO, Q3_SoundIndex("items/n_health.wav"), 1, ATTN_NORM, 0);
    gi.sound(other, CHAN_ITEM, sound, 1, ATTN_NONE, 0);
    return true;
}

// Use_Quad's switch-on sound, Quake II's or Quake III's
int Q3_QuadUseSound(void)
{
    return level.q3map ? Q3_SoundIndex("items/quaddamage.wav") : gi.soundindex("items/damage.wav");
}

// a quad shot's sound, Quake II's or Quake III's (same file name, own copy)
int Q3_QuadFireSound(void)
{
    return level.q3map ? Q3_SoundIndex("items/damage3.wav") : gi.soundindex("items/damage3.wav");
}

// DoRespawn: an item is back on its spot
void Q3_ItemRespawned(edict_t *ent)
{
    if (q3_powerup_pickup_sound(ent->item)) {
        gi.positioned_sound(ent->s.origin, ent, CHAN_AUTO,
                            Q3_SoundIndex("items/poweruprespawn.wav"), 1, ATTN_NONE, 0);
        // Quake III's bots hear it too, and go for it (g_q3bot.c)
        BotEngine_PowerupRespawned();
    }
}

/*
=================
Q3_SpawnEntity

Takes the place of ED_CallSpawn for each map entity of a Quake III map.
=================
*/
void Q3_SpawnEntity(edict_t *ent)
{
    const char  *cn = ent->classname;
    bool        item, suspended = false;
    int         i;

    if (!cn) {
        G_FreeEdict(ent);
        return;
    }

    if (ent == g_edicts) {
        ED_CallSpawn(ent);
        return;
    }

    // Quake III's game type filter: "notfree" leaves out of free for all,
    // "notteam" out of the team games
    if (deathmatch->value) {
        bool team = G_TeamPlay() || G_CTF();

        if ((team && st.notteam) || (!team && st.notfree)) {
            G_FreeEdict(ent);
            return;
        }
    }

    for (i = 0; i < q_countof(q3_dropped); i++) {
        if (!Q_stricmp(cn, q3_dropped[i])) {
            G_FreeEdict(ent);
            return;
        }
    }

    // the bot match's Quake III guns (g_q3weap.c): the map's own, keeping the
    // Quake II item each would be otherwise - the renames below - for Quake
    // II's players in a match by player model
    if (level.q3_weapons) {
        const char  *q2 = cn, *q3;

        for (i = 0; i < q_countof(q3_renames); i++) {
            if (!Q_stricmp(cn, q3_renames[i].from)) {
                q2 = q3_renames[i].to;
                break;
            }
        }
        q3 = Q3W_MapClassname(ent, cn, q2);
        if (q3 != cn) {
            cn = q3;
            ent->classname = (char *)cn;
        }
    }

    for (i = 0; i < q_countof(q3_renames); i++) {
        if (!Q_stricmp(cn, q3_renames[i].from)) {
            cn = q3_renames[i].to;
            ent->classname = (char *)cn;
            break;
        }
    }

    // the Battle Suit with the Quake III amounts, the Environment Suit without
    if (!Q_stricmp(cn, "item_enviro") && Q3_ItemRules()) {
        cn = "item_q3_battlesuit";
        ent->classname = (char *)cn;
    }

    // the Quake III entities with no Quake II counterpart
    if (!Q_stricmp(cn, "trigger_push") && ent->target) {
        SP_q3_trigger_push(ent);
        return;
    }
    if (!Q_stricmp(cn, "trigger_teleport")) {
        SP_q3_trigger_teleport(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_speaker")) {
        SP_q3_target_speaker(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_bobbing")) {
        SP_q3_func_bobbing(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_pendulum")) {
        SP_q3_func_pendulum(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_delay")) {
        SP_q3_target_delay(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_relay")) {
        SP_q3_target_relay(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_give")) {
        SP_q3_target_give(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_remove_powerups")) {
        SP_q3_target_remove_powerups(ent);
        return;
    }
    if (!Q_stricmp(cn, "target_print")) {
        SP_q3_target_print(ent);
        return;
    }
    if (!Q_stricmp(cn, "shooter_grenade")) {
        SP_q3_shooter(ent, 0);
        return;
    }
    if (!Q_stricmp(cn, "shooter_rocket")) {
        SP_q3_shooter(ent, 1);
        return;
    }
    if (!Q_stricmp(cn, "shooter_plasma")) {
        SP_q3_shooter(ent, 2);
        return;
    }
    if (!Q_stricmp(cn, "info_player_deathmatch")) {
        // a point to spawn at; Quake II's pad model is not Quake III's look.
        // Its flags are kept for SelectSpawnPoint (p_client.c): spawnflag 1
        // is the host's first spawn (q3dm0's intro room), and nobots /
        // nohumans keep one kind of player off it. Its target fires on
        // each spawn there (a target_give, mostly).
        if (!deathmatch->value) {
            G_FreeEdict(ent);
            return;
        }
        ent->spawnflags = ((ent->spawnflags & 1) ? Q3_SPOT_INITIAL : 0) |
                          (st.nobots ? Q3_SPOT_NOBOTS : 0) |
                          (st.nohumans ? Q3_SPOT_NOHUMANS : 0);
        return;
    }

    // Quake II spawn functions with Quake III's defaults
    if (!Q_stricmp(cn, "func_door")) {
        if (!ent->speed)
            ent->speed = 400;
        if (!ent->wait)
            ent->wait = 2;
        if (!st.noise_start)
            st.noise_start = "#baseq3/sound/movers/doors/dr1_strt.wav";
        if (!st.noise_middle)
            st.noise_middle = "";
        if (!st.noise_end)
            st.noise_end = "#baseq3/sound/movers/doors/dr1_end.wav";
        ent->spawnflags &= 1 | 4;   // START_OPEN, CRUSHER
    } else if (!Q_stricmp(cn, "func_button")) {
        if (!ent->wait)
            ent->wait = 1;
        if (!st.noise_start)
            st.noise_start = "#baseq3/sound/movers/switches/butn2.wav";
        ent->spawnflags = 0;
    } else if (!Q_stricmp(cn, "func_rotating")) {
        // always turning in Quake III; keeps its X / Y axis bits
        ent->spawnflags = (ent->spawnflags & (4 | 8)) | 1;
    } else if (!Q_stricmp(cn, "func_wall")) {
        ent->spawnflags = 0;
    } else if (!Q_stricmp(cn, "trigger_multiple")) {
        if (!ent->wait)
            ent->wait = 0.5f;
        ent->spawnflags = 0;    // Team Arena's team bits; Quake II's 2 is NOT_PLAYER
    }

    // Quake III item spawnflags: 1 is "suspended", not Quake II's
    // trigger-spawn, and nothing else is meaningful
    item = q3_is_item(cn);
    if (item) {
        suspended = (ent->spawnflags & Q3_ITEM_SUSPENDED) != 0;
        ent->spawnflags = 0;
    }

    ED_CallSpawn(ent);

    if (!ent->inuse)
        return;

    // a door, a func_static... with an md3 drawn at its origin
    if (st.model2 && st.model2[0] && !ent->s.modelindex2 && ent->solid == SOLID_BSP)
        ent->s.modelindex2 = gi.modelindex(st.model2);

    if (!item)
        return;

    // after the Quake II spawn function has set its amounts
    if (Q3_Amounts() && !strncmp(cn, "item_health", 11)) {
        static const int amounts[4] = { 5, 25, 50, 100 };
        int size = q3_health_size(cn);

        ent->count = amounts[size];
        ent->style = 0;
        Q3_HealthSound(ent);    // precache: picked up mid-game otherwise

        // and Quake III's look: the cross (droptofloor sets ent->model) inside
        // its sphere. Quake III held the sphere still and turned the cross;
        // here both turn with the item, which a round sphere does not show.
        // The renderer makes the sphere glass and the cross glow
        // (baseq3/materials/quake3.mat) and rounds the sphere (vkpt models.c).
        ent->model = (char *)q3_health_models[size][0];
        ent->s.modelindex2 = gi.modelindex(q3_health_models[size][1]);
        ent->s.effects |= EF_ROTATE;    // Quake II's health stands still
    }

    if (ent->item) {
        const char *ring = q3_powerup_ring(ent->item->classname);

        // droptofloor sets the model from ent->model, as for health above
        if (ent->item->classname && !strcmp(ent->item->classname, "item_quad")) {
            ent->model = (char *)Q3_QUAD_MODEL;
            gi.modelindex(Q3_QUAD_MODEL);   // now, not when it lands
        }

        // the powerups: Quake III's ring round them, and its sounds
        if (ring) {
            ent->s.modelindex2 = gi.modelindex(ring);
            q3_powerup_precache(ent->item);
        }

        if (ent->think == droptofloor) {
            if (suspended)
                ent->think = q3_item_suspended;
            else if ((ent->item->flags & IT_POWERUP) && deathmatch->value && !ent->team)
                ent->think = q3_powerup_first;
        }
    }
}

/*
=================
Q3_LevelStart

After every entity has spawned: aim the jump pads and intermission cameras,
and keep the items target_give hands out off the floor.
=================
*/
void Q3_LevelStart(void)
{
    edict_t *e, *t;
    int     i;

    if (!level.q3map)
        return;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        e = g_edicts + i;
        if (!e->inuse || !e->classname)
            continue;

        if (e->touch == q3_jumppad_touch) {
            q3_jumppad_aim(e);
            continue;
        }

        if (!strcmp(e->classname, "info_player_intermission") && e->target) {
            vec3_t dir;

            t = G_PickTarget(e->target);
            if (t) {
                VectorSubtract(t->s.origin, e->s.origin, dir);
                vectoangles(dir, e->s.angles);
            }
            continue;
        }

        if (e->use == q3_give_use && e->target) {
            for (t = NULL; (t = G_Find(t, FOFS(targetname), e->target)) != NULL; ) {
                if (!t->item)
                    continue;
                t->think = NULL;
                t->nextthink = 0;
                t->touch = NULL;
                t->solid = SOLID_NOT;
                t->svflags |= SVF_NOCLIENT;
                gi.unlinkentity(t);
            }
        }
    }
}
