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

QUAKE 1 MAPS

The engine loads a Quake 1 map (common/bsp_q1.c) as an ordinary one, and
tells the game so through sv_map_q1, which SpawnEntities copies into
level.q1map. Then each map entity comes here instead of ED_CallSpawn, for the
bot match: Quake II's rules with Quake 1's map.

Much of Quake 1 is still Quake II's: the skill and deathmatch spawnflags are
the same bits, and so are trigger_push (speed x 10), func_plat (up while it
has a targetname), func_train (by its mins, starting at once without a
targetname), trigger_relay / _counter, path_corner, info_notnull, and
Threewave CTF's flags and team spawns (Quake II CTF's names). What differs:

  - items: Quake 1's names, put into Quake II's (q1_renames); every item
    stands with its origin on the floor, so it goes up 16 before Quake II's
    droptofloor; the ammo and health boxes also have it at a corner,
    32 x 32, so they move to the box's middle
  - func_train: 2 damage to what blocks it, not 100; Quake 1's sounds
  - trigger_multiple / _once / _secret: spawnflag 1 (NOTOUCH) means fired
    only by something else; a "health" makes it a box that fires when shot;
    trigger_hurt hurts once a second
  - func_door: Quake 1 links doors that touch into one, unless spawnflag 4
    (DONT_LINK) - Quake II needs a "team" for that, so touching doors get one
    (Q1_LinkDoors). Its 4 / 8 / 16 (don't link, gold key, silver key) are
    Quake II's crusher, no-monster and animated: gone - there are no keys in
    a deathmatch. Quake II doubles every door's speed in deathmatch; Quake 1
    does not, so it is halved first. Quake 1's sounds.
  - func_plat: speed 150 by default, no acceleration; func_button: wait 1
  - func_door_secret: Quake 1's flags in Quake II's places
  - trigger_teleport to info_teleport_destination, 27 units up as Quake 1
    puts it; one with a targetname works only just after it is fired
  - info_intermission: "mangle" is its angles
  - ambient_*, and a teleporter's hum: a looped sound
  - func_illusionary: seen, not solid
  - MG1's func_bob: a sine swing through "dest" (SP_q1_func_bob)
  - MG1's trigger_fog / trigger_fog_transition: dropped, as MG1 itself
    removes them in deathmatch and co-op (its fog is per player, sent with
    a "fog" console command; that is for the campaign)
  - the spawn spots: Quake 1 allows a player box to touch a wall or stand
    sunk a unit into the floor (its hull test counts that as outside).
    Quake II does not, so a spot that is in something moves up or aside to
    the first clear place, as Quake 1's own spawn does.

What is not here - the monsters, the spike shooters, the add-ons' own
entities - is left out, quietly: a bot match has no use for monsters, and
the rest is the progs' (the campaign runs them, g_q1vm*.c). The torches are
here (q1_torch), and the exploding boxes (SP_q1_explobox). Its callbacks are at the
end of the savegame pointer table (g_ptrs.c), for g_q1_progs 0's single
player.

==============================================================================
*/

#include "g_local.h"

// Quake II's own, which the Quake 1 versions here call after their changes
void SP_func_door(edict_t *ent);
void SP_func_plat(edict_t *ent);
void SP_func_train(edict_t *ent);
void SP_func_button(edict_t *ent);
void SP_func_door_secret(edict_t *ent);
void SP_trigger_multiple(edict_t *ent);
void SP_trigger_once(edict_t *ent);
void InitTrigger(edict_t *self);
void SP_target_speaker(edict_t *ent);

/*
=================
Q1_SoundIndex

Quake 1's own copy of a sound ("doors/drclos4.wav"), from the Quake 1 paks
alone (files.c, "id1/"): several share a name with Quake II's.
=================
*/
int Q1_SoundIndex(const char *name)
{
    char    path[MAX_QPATH];

    if (!name || !*name)
        return 0;
    if (Q_snprintf(path, sizeof(path), "#id1/sound/%s", name) >= sizeof(path))
        return 0;
    return gi.soundindex(path);
}

static void q1_moveinfo_sounds(edict_t *ent, const char *start, const char *mid, const char *end)
{
    ent->moveinfo.sound_start = start ? Q1_SoundIndex(start) : 0;
    ent->moveinfo.sound_middle = mid ? Q1_SoundIndex(mid) : 0;
    ent->moveinfo.sound_end = end ? Q1_SoundIndex(end) : 0;
}

/*
==============================================================================

ITEMS

==============================================================================
*/

static const struct {
    const char  *from, *to;
    bool        corner;     // origin at the box's corner (32 x 32)
} q1_renames[] = {
    // the weapons: Quake 1's in the place of the Quake II gun most like it
    { "weapon_supershotgun",            "weapon_supershotgun" },
    { "weapon_nailgun",                 "weapon_machinegun" },
    { "weapon_supernailgun",            "weapon_chaingun" },
    { "weapon_grenadelauncher",         "weapon_grenadelauncher" },
    { "weapon_rocketlauncher",          "weapon_rocketlauncher" },
    { "weapon_lightning",               "weapon_plasmabeam" },

    { "item_shells",                    "ammo_shells",      true },
    { "item_spikes",                    "ammo_bullets",     true },
    { "item_rockets",                   "ammo_rockets",     true },
    { "item_cells",                     "ammo_cells",       true },

    { "item_armor1",                    "item_armor_jacket" },
    { "item_armor2",                    "item_armor_combat" },
    { "item_armorInv",                  "item_armor_body" },

    { "item_artifact_super_damage",     "item_quad" },
    { "item_artifact_invulnerability",  "item_invulnerability" },
    { "item_artifact_invisibility",     "item_invisibility" },
    { "item_artifact_envirosuit",       "item_enviro" },

    // the add-ons' (hipdm1 has hipnotic's three guns): the Quake II gun
    // most like each - laser cannon, proximity mines, the hammer
    { "weapon_laser_gun",               "weapon_hyperblaster" },
    { "weapon_proximity_gun",           "weapon_proxlauncher" },
    { "weapon_mjolnir",                 "weapon_chainfist" },
    { "item_artifact_wetsuit",          "item_breather" },
    { "item_artifact_lavasuit",         "item_enviro" },
    { "item_lava_spikes",               "ammo_bullets",     true },
    { "item_multi_rockets",             "ammo_rockets",     true },
    { "item_plasma",                    "ammo_cells",       true },

    { "info_intermission",              "info_player_intermission" },
    // start.bsp: the boss gate stands until all four runes are in; nobody
    // carries a rune here (the episode gates go, below)
    { "func_bossgate",                  "func_wall" },
};

// nothing for a bot match: light is the renderer's, the rest is single
// player's or the progs'
static const char *const q1_dropped[] = {
    "light", "light_fluoro", "light_fluorospark", "light_globe",
    "info_null", "info_player_start2",
    "item_key1", "item_key2", "item_sigil",
    "misc_fireball", "misc_noisemaker",
    "trap_spikeshooter", "trap_shooter", "air_bubbles", "viewthing",
    "event_lightning", "misc_teleporttrain", "trigger_onlyregistered", "trigger_monsterjump",
    "noclass", "ambient_thunder", "func_episodegate",
    // the add-ons' names Quake II also has, for other things: hipnotic's
    // clock hand (Quake II's func_clock is a countdown that "uses" its
    // target as a display - a NULL call on hip2m2), mg3's .mdl display,
    // mg1's light ramp and fog volume
    "func_clock", "misc_model", "target_lightramp", "trigger_fog",
    // the fog is the client's, read from the map (client/mapfog.c)
    "info_fog", "trigger_fog_transition",
};

/*
==============================================================================

TELEPORTERS

==============================================================================
*/

static void Q1_TeleportPlayer(edict_t *player, edict_t *dest)
{
    gclient_t   *client = player->client;
    vec3_t      forward;
    char        snd[32];
    int         i;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_TELEPORT_EFFECT);
    gi.WritePosition(player->s.origin);
    gi.multicast(player->s.origin, MULTICAST_PVS);

    if (client)
        CTF_PlayerResetGrapple(player);

    gi.unlinkentity(player);

    VectorCopy(dest->s.origin, player->s.origin);
    VectorCopy(player->s.origin, player->s.old_origin);

    // out of the far side at Quake 1's 300
    AngleVectors(dest->s.angles, forward, NULL, NULL);
    VectorScale(forward, 300, player->velocity);

    // no hold on arrival: Quake 1 lets the player run straight on
    if (client) {
        VectorCopy(player->velocity, client->oldvelocity);
        for (i = 0; i < 3; i++)
            client->ps.pmove.delta_angles[i] = ANGLE2SHORT(dest->s.angles[i] - client->resp.cmd_angles[i]);
        VectorClear(client->ps.viewangles);
        VectorClear(client->v_angle);
    }
    VectorClear(player->s.angles);
    player->s.event = EV_OTHER_TELEPORT;    // no lerp from the old spot, no Quake II sound

    KillBox(player);
    gi.linkentity(player);

    // Quake 1's five teleport sounds, one at random, at the far end
    Q_snprintf(snd, sizeof(snd), "misc/r_tele%d.wav", 1 + (int)(random() * 5) % 5);
    gi.sound(player, CHAN_AUTO, Q1_SoundIndex(snd), 1, ATTN_NORM, 0);
}

void q1_teleport_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    edict_t *dest;

    if (!other->client || other->health <= 0)
        return;
    // one with a targetname carries only just after it is fired
    if (self->targetname && level.framenum > self->timestamp)
        return;

    dest = G_PickTarget(self->target);
    if (!dest) {
        gi.dprintf("trigger_teleport at %s: no destination \"%s\"\n", vtos(self->absmin), self->target);
        return;
    }
    Q1_TeleportPlayer(other, dest);
}

void q1_teleport_use(edict_t *self, edict_t *other, edict_t *activator)
{
    self->timestamp = level.framenum + 0.2f * BASE_FRAMERATE;
}

static void SP_q1_trigger_teleport(edict_t *self)
{
    if (!self->target) {
        G_FreeEdict(self);
        return;
    }
    InitTrigger(self);
    self->touch = q1_teleport_touch;
    self->use = q1_teleport_use;
    gi.linkentity(self);

    for (int i = 1; i <= 5; i++)
        Q1_SoundIndex(va("misc/r_tele%d.wav", i));

    // its hum, unless SILENT (2): the trigger itself is never sent to the
    // clients, so the sound comes from a point at its middle
    if (!(self->spawnflags & 2)) {
        edict_t *hum = G_Spawn();

        hum->classname = "q1_teleport_hum";
        VectorAdd(self->absmin, self->absmax, hum->s.origin);
        VectorScale(hum->s.origin, 0.5f, hum->s.origin);
        hum->s.sound = Q1_SoundIndex("ambience/hum1.wav");
        gi.linkentity(hum);
    }
}

/*
==============================================================================

MOVERS

==============================================================================
*/

// the doors Q1_LinkDoors groups, and which of them said DONT_LINK
#define Q1_MAX_DOORS    1024
static int      q1_doors[Q1_MAX_DOORS];
static bool     q1_door_nolink[Q1_MAX_DOORS];
static int      q1_num_doors;

static void SP_q1_func_door(edict_t *ent)
{
    // Quake 1's sounds, by "sounds": none, stone, machine, stone chain,
    // screechy metal - the moving sound at the start, the other at the end
    static const char *const q1_door_sounds[5][2] = {
        { NULL, NULL },
        { "doors/doormv1.wav", "doors/drclos4.wav" },
        { "doors/hydro1.wav",  "doors/hydro2.wav" },
        { "doors/stndr1.wav",  "doors/stndr2.wav" },
        { "doors/ddoor1.wav",  "doors/ddoor2.wav" },
    };
    const int q1flags = ent->spawnflags;
    const int snd = (ent->sounds >= 0 && ent->sounds < 5) ? ent->sounds : 0;

    // start open (1) and toggle (32) are Quake II's too
    ent->spawnflags = q1flags & (1 | 32);
    if (!ent->speed)
        ent->speed = 100;
    if (deathmatch->value)
        ent->speed *= 0.5f;     // SP_func_door doubles it

    SP_func_door(ent);
    q1_moveinfo_sounds(ent, q1_door_sounds[snd][0], NULL, q1_door_sounds[snd][1]);

    if (ent->inuse && q1_num_doors < Q1_MAX_DOORS && !ent->team) {
        q1_door_nolink[q1_num_doors] = (q1flags & 4) != 0;
        q1_doors[q1_num_doors++] = ent - g_edicts;
    }
}

static bool q1_touching(const edict_t *a, const edict_t *b)
{
    for (int i = 0; i < 3; i++)
        if (a->absmin[i] > b->absmax[i] || a->absmax[i] < b->absmin[i])
            return false;
    return true;
}

/*
=================
Q1_LinkDoors

Quake 1's LinkDoors: doors that touch move as one. Each group gets a Quake II
"team" before G_FindTeams, which makes the first its master; the master's
trigger then covers the whole group (Think_SpawnDoorTrigger).
=================
*/
void Q1_LinkDoors(void)
{
    int     group = 0;
    bool    done[Q1_MAX_DOORS] = { false };
    int     stack[Q1_MAX_DOORS];

    if (!level.q1map)
        return;

    for (int i = 0; i < q1_num_doors; i++) {
        int n = 0, members = 0;
        char *team;

        if (done[i] || q1_door_nolink[i])
            continue;

        team = G_CopyString(va("q1door%d", group));
        stack[n++] = i;
        done[i] = true;
        while (n) {
            int k = stack[--n];
            edict_t *a = &g_edicts[q1_doors[k]];

            a->team = team;
            members++;
            for (int j = 0; j < q1_num_doors; j++) {
                if (done[j] || q1_door_nolink[j])
                    continue;
                if (q1_touching(a, &g_edicts[q1_doors[j]])) {
                    done[j] = true;
                    stack[n++] = j;
                }
            }
        }
        if (members == 1)
            g_edicts[q1_doors[i]].team = NULL;  // alone: no team needed
        else
            group++;
    }
    q1_num_doors = 0;
}

static void SP_q1_func_plat(edict_t *ent)
{
    // Quake 1's speed in units a second (SP_func_plat takes a tenth of it,
    // as Quake II's maps give it), and no acceleration
    if (!ent->speed)
        ent->speed = 150;
    ent->accel = ent->decel = ent->speed;
    ent->spawnflags &= 1;   // PLAT_LOW_TRIGGER
    SP_func_plat(ent);
    // 1 the base plat, otherwise (0 too) the medieval one
    if (ent->sounds == 1)
        q1_moveinfo_sounds(ent, "plats/plat1.wav", NULL, "plats/plat2.wav");
    else
        q1_moveinfo_sounds(ent, "plats/medplat1.wav", NULL, "plats/medplat2.wav");
}

static void SP_q1_func_train(edict_t *ent)
{
    // Quake 1 hurts what blocks a train 2 at a time, not Quake II's 100
    ent->spawnflags = 0;
    if (!ent->dmg)
        ent->dmg = 2;
    SP_func_train(ent);
    if (ent->sounds == 1)
        q1_moveinfo_sounds(ent, "plats/train1.wav", NULL, "plats/train2.wav");
}

/*
==============================================================================

TRIGGERS

==============================================================================
*/

// not static: a single-player save writes it (g_ptrs.c), as the bob's below
void q1_trigger_killed(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    // shot: fire, and take the next shot once it has re-armed
    self->health = self->max_health;
    if (self->use)
        self->use(self, inflictor, attacker);
}

/*
=================
SP_q1_trigger

trigger_multiple / _once / _secret. Quake 1's spawnflag 1 (NOTOUCH) means
only something else fires it; a "health" makes it a box that fires when shot,
which blocks like a wall as Quake 1's does. Quake II's flags are other things.
=================
*/
static void SP_q1_trigger(edict_t *ent, bool once, bool secret)
{
    static const char *const q1_trigger_sounds[3] = {
        "misc/secret.wav", "misc/talk.wav", "misc/trigger1.wav"
    };
    const int q1flags = ent->spawnflags;

    ent->spawnflags = 0;
    if (secret) {
        if (!ent->message)
            ent->message = "You found a secret area!";
        if (!ent->sounds)
            ent->sounds = 1;
    }
    if (once)
        SP_trigger_once(ent);
    else
        SP_trigger_multiple(ent);
    if (!ent->inuse)
        return;

    if (ent->sounds >= 1 && ent->sounds <= 3)
        ent->noise_index = Q1_SoundIndex(q1_trigger_sounds[ent->sounds - 1]);

    if (ent->health > 0) {
        ent->max_health = ent->health;
        ent->takedamage = DAMAGE_YES;
        ent->die = q1_trigger_killed;
        ent->solid = SOLID_BBOX;
        ent->touch = NULL;
        gi.linkentity(ent);
    } else if (q1flags & 1) {
        ent->touch = NULL;
    }
}

/*
=================
SP_q1_trigger_changelevel

A Quake 1 level's exit (changelevel.qc): the player walking into it goes on
to its "map", a name in the same episode - so the episode's folder goes in
front of it, from this map's own name (start's "e1m1" is q1/e1m1, a
hipnotic map's "start" is q1/hipnotic/start). Quake II does that as a trigger
firing a target_changelevel, which in single player leaves at once
(BeginIntermission), so the trigger becomes a trigger_multiple and its target
is spawned here: no new callbacks, nothing new for the save table. Quake 1's
NO_INTERMISSION (spawnflag 1) is how Quake II leaves in single player anyway;
Quake 1's stats at info_intermission are not shown. A bot match has no exits.
=================
*/
static void SP_q1_trigger_changelevel(edict_t *ent)
{
    static int  num_exits;
    const char  *slash = strrchr(level.mapname, '/');
    const int   folder = slash ? (int)(slash - level.mapname + 1) : 0;
    char        path[MAX_QPATH], name[32];
    edict_t     *target;

    if (deathmatch->value || !ent->map || !*ent->map) {
        G_FreeEdict(ent);
        return;
    }

    Q_snprintf(path, sizeof(path), "%.*s%s", folder, level.mapname, ent->map);
    Q_snprintf(name, sizeof(name), "q1_exit%d", num_exits++);

    target = G_Spawn();
    target->classname = "target_changelevel";
    target->map = ED_NewString(path);
    target->targetname = ED_NewString(name);
    ED_CallSpawn(target);

    ent->classname = "trigger_multiple";
    ent->target = ED_NewString(name);
    ent->spawnflags = 0;    // Quake 1's NO_INTERMISSION; Quake II's are other things
    ent->sounds = 0;
    SP_trigger_multiple(ent);
}

/*
=================
SP_q1_trigger_setskill

The start map's skill halls (misc.qc trigger_skill_touch): walking through
one sets "skill" to its "message", for the next map - the one its slipgate
loads. Force-set, as Quake II's skill is latched until a new game and a
slipgate is only a level change. A bot match has no use for them.
=================
*/
void q1_setskill_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (!other->client || !self->message)
        return;
    if (atoi(self->message) != skill->integer)
        gi.cvar_forceset("skill", self->message);
}

static void SP_q1_trigger_setskill(edict_t *ent)
{
    if (deathmatch->value || !ent->message || !*ent->message) {
        G_FreeEdict(ent);
        return;
    }
    InitTrigger(ent);
    ent->touch = q1_setskill_touch;
    gi.linkentity(ent);
}

static void SP_q1_func_button(edict_t *ent)
{
    static const char *const q1_button_sounds[4] = {
        "buttons/airbut1.wav", "buttons/switch21.wav", "buttons/switch02.wav", "buttons/switch04.wav"
    };
    const int snd = (ent->sounds >= 0 && ent->sounds < 4) ? ent->sounds : 0;

    if (!ent->wait)
        ent->wait = 1;
    ent->spawnflags = 0;
    SP_func_button(ent);
    q1_moveinfo_sounds(ent, q1_button_sounds[snd], NULL, NULL);
}

static void SP_q1_func_door_secret(edict_t *ent)
{
    // Quake 1: 1 open once, 2 first left, 4 first down, 8 never shoot,
    // 16 always shoot. Quake II: 1 always shoot, 2 first left, 4 first down
    const int q1flags = ent->spawnflags;

    ent->spawnflags = (q1flags & (2 | 4)) | ((q1flags & 16) ? 1 : 0);
    SP_func_door_secret(ent);
}

static void SP_q1_func_illusionary(edict_t *ent)
{
    // seen, not solid
    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_NOT;
    gi.setmodel(ent, ent->model);
    gi.linkentity(ent);
}

/*
MG1's func_bob (mg1, mg3), after quakec_mg1/func_bob.qc in id's
quake-rerelease-qc: a brush that swings through "dest" (default 0 0 64) on a
sine, and through "dest2" on the cosine (the faint wisps' loops), about where
it was built, once every "wait" seconds (default 10). "delay" is where in the
cycle it starts, as a fraction of one. Spawnflag 1: not solid (the wisps),
2: on from the start; a fire toggles it, and one that is off stays where it
was built. "dmg" (default 1) to what blocks it, at most twice a second.
mg3 dm1's lightning gun rides one: Quake 1 drops it onto the pillar.

The solid ones are Quake II pushers (MOVETYPE_PUSH): a velocity each frame
that lands them on the cycle's next point, so they carry what stands on them,
as MG1's do. The others are set where they belong each frame.
*/
#define Q1_BOB_NONSOLID     1
#define Q1_BOB_START_ON     2

// pos1: where it was built; pos2: dest; move_origin: dest2; speed: degrees a
// second; accel: where in the cycle it is (degrees); count: on
static void q1_bob_advance(edict_t *ent, vec3_t out)
{
    float a;

    ent->accel = fmodf(ent->accel + ent->speed * FRAMETIME, 360);
    a = DEG2RAD(ent->accel);
    VectorMA(ent->pos1, sinf(a), ent->pos2, out);
    VectorMA(out, cosf(a), ent->move_origin, out);
}

void q1_bob_think(edict_t *ent)
{
    vec3_t  next;

    q1_bob_advance(ent, next);
    if (ent->solid == SOLID_NOT) {
        VectorCopy(next, ent->s.origin);
        gi.linkentity(ent);
    } else {
        VectorSubtract(next, ent->s.origin, ent->velocity);
        VectorScale(ent->velocity, 1.0f / FRAMETIME, ent->velocity);
    }
    ent->nextthink = level.framenum + 1;
}

void q1_bob_use(edict_t *ent, edict_t *other, edict_t *activator)
{
    if (ent->count) {
        VectorClear(ent->velocity);
        ent->nextthink = 0;
    } else {
        q1_bob_think(ent);
    }
    ent->count = !ent->count;
}

void q1_bob_blocked(edict_t *ent, edict_t *other)
{
    if (level.framenum < ent->touch_debounce_framenum || !other->takedamage)
        return;
    T_Damage(other, ent, ent, vec3_origin, other->s.origin, vec3_origin, ent->dmg, 1, 0, MOD_CRUSH);
    ent->touch_debounce_framenum = level.framenum + BASE_FRAMERATE / 2;
}

static void SP_q1_func_bob(edict_t *ent)
{
    gi.setmodel(ent, ent->model);
    if (ent->spawnflags & Q1_BOB_NONSOLID) {
        ent->movetype = MOVETYPE_NONE;
        ent->solid = SOLID_NOT;
    } else {
        ent->movetype = MOVETYPE_PUSH;
        ent->solid = SOLID_BSP;
    }

    VectorCopy(ent->s.origin, ent->pos1);
    VectorCopy(st.dest, ent->pos2);
    if (VectorEmpty(ent->pos2))
        VectorSet(ent->pos2, 0, 0, 64);
    VectorCopy(st.dest2, ent->move_origin);
    if (!ent->wait)
        ent->wait = 10;
    if (!ent->dmg)
        ent->dmg = 1;
    ent->speed = 360 / ent->wait;
    ent->accel = 360 * ent->delay;
    ent->delay = 0;         // not a delay before firing targets
    ent->count = 0;
    ent->use = q1_bob_use;
    ent->blocked = q1_bob_blocked;
    ent->think = q1_bob_think;
    gi.linkentity(ent);

    if (ent->spawnflags & Q1_BOB_START_ON)
        q1_bob_use(ent, NULL, NULL);
}

/*
==============================================================================

TORCHES

==============================================================================
*/

/*
misc.qc's torches and flames: the flame model and the fire loop. Quake made
them static entities that the model's frame group animated by the clock; the
renderer builds every member of a group as a frame of its own (model_q1.c),
so flame.mdl's frame 0 is frames 0-5 and flame2.mdl's frame 0 is 0-5 and its
frame 1, the large flame, 6-16, each member a tenth of a second - a frame
here. The light is the renderer's (from the map's light entities).
*/
static const struct {
    const char  *classname, *model;
    int         first, count;
} q1_torches[] = {
    { "light_torch_small_walltorch",    "progs/flame.mdl",  0, 6 },
    { "light_flame_large_yellow",       "progs/flame2.mdl", 6, 11 },
    { "light_flame_small_yellow",       "progs/flame2.mdl", 0, 6 },
    { "light_flame_small_white",        "progs/flame2.mdl", 0, 6 },
};

// style: the group's first frame; count: its members
void q1_torch_think(edict_t *ent)
{
    if (ent->count > 0)
        ent->s.frame = ent->style + (ent->s.frame - ent->style + 1) % ent->count;
    ent->nextthink = level.framenum + 1;
}

// the map's episode's copy of a Quake 1 file: "id1/<episode>/<name>"
static const char *q1_episode_path(const char *name)
{
    const char *m = level.mapname, *slash;

    if (!Q_strncasecmp(m, "q1/", 3) && (slash = strchr(m + 3, '/')) != NULL)
        return va("id1/%.*s/%s", (int)(slash - (m + 3)), m + 3, name);
    return va("id1/id1/%s", name);
}

static bool q1_torch(edict_t *ent)
{
    for (int i = 0; i < q_countof(q1_torches); i++) {
        if (Q_stricmp(ent->classname, q1_torches[i].classname))
            continue;
        ent->classname = "q1_torch";
        ent->movetype = MOVETYPE_NONE;
        ent->solid = SOLID_NOT;
        ent->s.modelindex = gi.modelindex(q1_episode_path(q1_torches[i].model));
        ent->style = q1_torches[i].first;
        ent->count = q1_torches[i].count;
        // each at its own point in the flicker, as Quake's ST_RAND
        ent->s.frame = ent->style + (rand() % ent->count);
        ent->s.sound = Q1_SoundIndex("ambience/fire1.wav");
        ent->targetname = NULL;     // not a switched light
        ent->think = q1_torch_think;
        ent->nextthink = level.framenum + 1;
        gi.linkentity(ent);
        return true;
    }
    return false;
}

/*
==============================================================================

EXPLODING BOXES

misc_explobox (b_explob.bsp, 32 x 32 x 64) and misc_explobox2 (b_exbox2.bsp,
32 x 32 x 32): Quake's own boxes, drawn as the campaign draws them
(model_q1.c turns the .bsp into an .md3). They were dropped from the bot
match - Quake II takes its barrels out of a deathmatch - until Matt, 11-21:
"Quake 1 explosive barrels were missing from the maps". Quake keeps them in
every game, so they stay.

As Quake stands one: its origin is the box's CORNER (the .bsp spans 0..32),
2 units up, then dropped to the floor. 20 health. Shot to nothing it goes off
with Quake's 160 damage out to 200 units (T_RadiusDamage's damage + 40, the
same falloff as Quake II's, half to whoever shot it) - from the box's middle
rather than the corner Quake measured from, which on the floor could start
the line-of-sight test in the floor. Quake II's rules for the rest, as for
its own barrel: the death is "blew up" whoever shot it, and a box another box
sets off goes a frame later, so a row of them ripples instead of recursing.

==============================================================================
*/

void q1_explobox_explode(edict_t *self)
{
    // the middle of the box: the blast's centre and the fireball's
    VectorMA(self->absmin, 0.5f, self->size, self->s.origin);
    T_RadiusDamage(self, self->activator ? self->activator : self, 160, NULL, 200, MOD_BARREL);
    BecomeExplosion1(self);
}

void q1_explobox_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    self->takedamage = DAMAGE_NO;
    self->activator = attacker;
    self->think = q1_explobox_explode;
    self->nextthink = level.framenum + 1;
}

static void SP_q1_explobox(edict_t *ent, bool small)
{
    vec3_t  end;
    trace_t tr;

    ent->classname = "q1_explobox";
    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_BBOX;
    ent->s.modelindex = gi.modelindex(q1_episode_path(small ? "maps/b_exbox2.bsp" : "maps/b_explob.bsp"));
    VectorClear(ent->mins);
    VectorSet(ent->maxs, 32, 32, small ? 32 : 64);
    ent->health = 20;
    ent->takedamage = DAMAGE_YES;
    ent->die = q1_explobox_die;
    ent->targetname = NULL;

    // Quake's droptofloor: 2 up, then down as far as 256 to what it stands on
    // (left where it is when it starts in something or finds nothing)
    ent->s.origin[2] += 2;
    VectorCopy(ent->s.origin, end);
    end[2] -= 256;
    tr = gi.trace(ent->s.origin, ent->mins, ent->maxs, end, ent, MASK_SOLID);
    if (!tr.startsolid && !tr.allsolid && tr.fraction < 1)
        VectorCopy(tr.endpos, ent->s.origin);
    gi.linkentity(ent);
}

/*
==============================================================================

AMBIENT SOUNDS

==============================================================================
*/

static const struct {
    const char  *classname, *sound;
    float       volume;
} q1_ambients[] = {
    { "ambient_drip",           "ambience/drip1.wav",   0.5f },
    { "ambient_comp_hum",       "ambience/comp1.wav",   1.0f },
    { "ambient_swamp1",         "ambience/swamp1.wav",  0.5f },
    { "ambient_swamp2",         "ambience/swamp2.wav",  0.5f },
    { "ambient_drone",          "ambience/drone6.wav",  0.5f },
    { "ambient_light_buzz",     "ambience/fl_hum1.wav", 0.5f },
    { "ambient_flouro_buzz",    "ambience/buzz1.wav",   1.0f },
    { "ambient_suck_wind",      "ambience/suck1.wav",   1.0f },
};

static bool q1_ambient(edict_t *ent)
{
    for (int i = 0; i < q_countof(q1_ambients); i++) {
        if (Q_stricmp(ent->classname, q1_ambients[i].classname))
            continue;
        // a looped target_speaker, at the static attenuation Quake 1 gives
        // its ambientsound
        ent->classname = "target_speaker";
        st.noise = (char *)va("#id1/sound/%s", q1_ambients[i].sound);
        ent->spawnflags = 1;    // looped on
        ent->volume = q1_ambients[i].volume;
        ent->attenuation = ATTN_STATIC;
        SP_target_speaker(ent);
        return true;
    }
    return false;
}

/*
==============================================================================

SPAWN SPOTS

==============================================================================
*/

// the first clear place for a player at (ent): up to 17 units up and a unit
// aside, as Quake 1 lets a spot touch a wall or sit a unit into the floor
static void q1_unstick(edict_t *ent)
{
    static const vec3_t mins = { -16, -16, -24 }, maxs = { 16, 16, 32 };
    vec3_t  o;
    trace_t tr;

    for (int dz = 0; dz <= 17; dz++) {
        for (int dx = 0; dx < 3; dx++) {
            for (int dy = 0; dy < 3; dy++) {
                o[0] = ent->s.origin[0] + (dx == 2 ? -1 : dx);
                o[1] = ent->s.origin[1] + (dy == 2 ? -1 : dy);
                o[2] = ent->s.origin[2] + dz;
                tr = gi.trace(o, mins, maxs, o, NULL, MASK_PLAYERSOLID);
                if (!tr.startsolid && !tr.allsolid) {
                    if (dx || dy || dz)
                        gi.dprintf("%s at %s moved to a clear place\n", ent->classname, vtos(ent->s.origin));
                    VectorCopy(o, ent->s.origin);
                    return;
                }
            }
        }
    }
    gi.dprintf("%s at %s: no clear place near it\n", ent->classname, vtos(ent->s.origin));
}

/*
=================
Q1_SpawnEntity

Takes the place of ED_CallSpawn for each map entity of a Quake 1 map.
=================
*/
void Q1_SpawnEntity(edict_t *ent)
{
    const char  *cn = ent->classname;
    int         i;
    int         big_box_flag = 0;   // the spawnflag of a big ammo box, if not 1

    if (!cn) {
        G_FreeEdict(ent);
        return;
    }
    if (ent == g_edicts) {
        q1_num_doors = 0;
        ED_CallSpawn(ent);
        return;
    }

    if (!Q_strncasecmp(cn, "monster_", 8)) {
        G_FreeEdict(ent);
        return;
    }
    if (q1_torch(ent))
        return;
    if (!Q_stricmp(cn, "misc_explobox") || !Q_stricmp(cn, "misc_explobox2")) {
        SP_q1_explobox(ent, cn[13] == '2');
        return;
    }
    // A switched light (a targetname; the light compiler gave it a style of 32
    // or more): Quake II's own light sets and toggles that lightstyle as
    // Quake 1's light_use did, and the light editor's copy of it follows the
    // style (client lightedit.c, LE_Q1EntityLights). The rest light nothing
    // here - they are the renderer's - and in deathmatch SP_light frees them all.
    if (!Q_strncasecmp(cn, "light", 5) && (!cn[5] || cn[5] == '_') && ent->targetname && !deathmatch->value) {
        ent->classname = "light";
        ED_CallSpawn(ent);
        return;
    }
    for (i = 0; i < q_countof(q1_dropped); i++) {
        if (!Q_stricmp(cn, q1_dropped[i])) {
            G_FreeEdict(ent);
            return;
        }
    }

    if (!Q_stricmp(cn, "item_weapon")) {
        // the first game's old ammo box (dm5, dm6, death32c): 1 shells,
        // 2 rockets, 4 nails, 8 big - corner-placed like the others
        big_box_flag = 8;
        cn = (ent->spawnflags & 2) ? "ammo_rockets" :
             (ent->spawnflags & 4) ? "ammo_bullets" : "ammo_shells";
        ent->classname = (char *)cn;
        ent->s.origin[0] += 16;
        ent->s.origin[1] += 16;
    } else if (!Q_stricmp(cn, "item_health")) {
        // 1 rotten (15), 2 megahealth, else 25 - a corner-placed box
        cn = (ent->spawnflags & 2) ? "item_health_mega" :
             (ent->spawnflags & 1) ? "item_health" : "item_health_large";
        ent->classname = (char *)cn;
        ent->s.origin[0] += 16;
        ent->s.origin[1] += 16;
    } else {
        for (i = 0; i < q_countof(q1_renames); i++) {
            if (!Q_stricmp(cn, q1_renames[i].from)) {
                cn = q1_renames[i].to;
                ent->classname = (char *)cn;
                if (q1_renames[i].corner) {
                    ent->s.origin[0] += 16;
                    ent->s.origin[1] += 16;
                }
                break;
            }
        }
    }

    if (!strncmp(cn, "ammo_", 5) || !strncmp(cn, "weapon_", 7) || !strncmp(cn, "item_", 5)) {
        // With Quake's guns (g_q1weap.c), a big box (items.qc WEAPON_BIG2:
        // flag 1 on item_shells, _spikes, _rockets and _cells; 8 on the old
        // item_weapon) holds twice the small one's - Q1W_PickupAmmo's
        if (level.q1_weapons && !strncmp(cn, "ammo_", 5) &&
            (ent->spawnflags & (big_box_flag ? big_box_flag : 1))) {
            if (!strcmp(cn, "ammo_shells"))
                ent->count = 40;
            else if (!strcmp(cn, "ammo_bullets"))
                ent->count = 50;
            else if (!strcmp(cn, "ammo_rockets"))
                ent->count = 10;
            else if (!strcmp(cn, "ammo_cells"))
                ent->count = 12;
        }
        // Quake 1's item flags (the big box, rotten) are other things to
        // Quake II's; and its items stand with their origin on the floor,
        // where Quake II's 30 unit box would start buried (droptofloor)
        ent->spawnflags = 0;
        ent->s.origin[2] += 16;
        ED_CallSpawn(ent);
        return;
    }

    if (!Q_stricmp(cn, "info_teleport_destination")) {
        // a target spot, 27 up as Quake 1 puts the player there; its "angle"
        // is the way out
        ent->classname = "info_notnull";
        ent->s.origin[2] += 27;
        ED_CallSpawn(ent);
        return;
    }
    if (!Q_stricmp(cn, "info_player_intermission")) {
        VectorCopy(st.mangle, ent->s.angles);
        ED_CallSpawn(ent);
        return;
    }
    // (Threewave CTF's team spawns are Quake II CTF's names too: its author
    // wrote both, and its flags are item_flag_team1 / 2 as Quake II's)
    if (!Q_stricmp(cn, "info_player_deathmatch") || !Q_stricmp(cn, "info_player_start") ||
        !Q_stricmp(cn, "info_player_coop") || !Q_stricmp(cn, "info_player_team1") ||
        !Q_stricmp(cn, "info_player_team2")) {
        ent->spawnflags = 0;
        q1_unstick(ent);
        ED_CallSpawn(ent);
        return;
    }

    if (!Q_stricmp(cn, "func_door")) {
        SP_q1_func_door(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_plat")) {
        SP_q1_func_plat(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_train")) {
        SP_q1_func_train(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_button")) {
        SP_q1_func_button(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_door_secret")) {
        SP_q1_func_door_secret(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_illusionary")) {
        SP_q1_func_illusionary(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_bob")) {
        SP_q1_func_bob(ent);
        return;
    }
    if (!Q_stricmp(cn, "trigger_teleport")) {
        SP_q1_trigger_teleport(ent);
        return;
    }
    if (!Q_stricmp(cn, "trigger_multiple")) {
        SP_q1_trigger(ent, false, false);
        return;
    }
    if (!Q_stricmp(cn, "trigger_once")) {
        SP_q1_trigger(ent, true, false);
        return;
    }
    if (!Q_stricmp(cn, "trigger_secret")) {
        SP_q1_trigger(ent, true, true);
        return;
    }
    if (!Q_stricmp(cn, "trigger_changelevel")) {
        SP_q1_trigger_changelevel(ent);
        return;
    }
    if (!Q_stricmp(cn, "trigger_setskill")) {
        SP_q1_trigger_setskill(ent);
        return;
    }
    if (!Q_strncasecmp(cn, "ambient_", 8)) {
        if (!q1_ambient(ent))
            G_FreeEdict(ent);
        return;
    }
    if (!Q_stricmp(cn, "func_wall")) {
        // Quake 1 gives it no flags; Quake II's are other things
        ent->spawnflags = 0;
    }
    if (!Q_stricmp(cn, "trigger_hurt")) {
        // Quake 1 hurts once a second: Quake II's SLOW (16)
        ent->spawnflags = 16;
    }
    if (!Q_stricmp(cn, "trigger_push")) {
        /* Quake 1's one flag is PUSH_ONCE (1), Quake II's too. Its SILENT (4)
           and START_OFF (8) are other things to a Quake 1 map: mg3/map2's two
           wind pushes carry 10, and a START_OFF push with no targetname is
           Quake II's inert solid brush - drawn, trigger texture and all
           (q1gamehost HOST_TRIGGERS, 11-22). */
        ent->spawnflags &= 1;
    }

    ED_CallSpawn(ent);
}
