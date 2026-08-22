/*
Copyright (C) 1997-2001 Id Software, Inc.

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

RUNTIME MONSTER SPAWNING                                    (rogue / rerelease)

Ported from rogue's g_spawn.c + g_newai.c. Nothing in vanilla Quake II creates
a monster after level load, so all of this is new: finding a spot a body will
actually fit in, confirming there is flat, non-lethal ground under it, building
the monster, and playing the spawn effect over it.

The medic commander is the first user. monster_carrier and the widow use exactly
the same entry points, so keep this file generic - anything commander-specific
belongs in m_medic.c.

==============================================================================
*/

#include "g_local.h"

#define STEPSIZE            18

// how long a spawngro effect lives, in seconds
#define SPAWNGROW_LIFESPAN  1

/*
=================
DetermineBBox

Spawn the monster, note how big it is, then throw it away. There is no table of
monster bounding boxes anywhere, and hardcoding one drifts out of date the
moment a spawn function is edited - the rerelease does exactly this too.
=================
*/
void DetermineBBox(char *classname, vec3_t mins, vec3_t maxs)
{
    edict_t         *newEnt;
    spawn_temp_t    saved_st;

    VectorClear(mins);
    VectorClear(maxs);

    if (!classname)
        return;

    // `st` is a single global that the spawn functions read. This runs from
    // inside another entity's spawn function, so the throwaway would otherwise
    // inherit that entity's keys (a commander carrying `item ammo_cells` would
    // hand one to every probe) and could leave st changed under the caller.
    saved_st = st;
    memset(&st, 0, sizeof(st));

    newEnt = G_Spawn();

    VectorCopy(vec3_origin, newEnt->s.origin);
    newEnt->classname = ED_NewString(classname);
    // keep the throwaway out of the level's monster tally
    newEnt->monsterinfo.aiflags |= AI_DO_NOT_COUNT;

    ED_CallSpawn(newEnt);

    VectorCopy(newEnt->mins, mins);
    VectorCopy(newEnt->maxs, maxs);

    G_FreeEdict(newEnt);

    st = saved_st;
}

/*
=================
CreateMonster
=================
*/
edict_t *CreateMonster(vec3_t origin, vec3_t angles, char *classname)
{
    edict_t *newEnt;

    if (!classname)
        return NULL;

    newEnt = G_Spawn();

    VectorCopy(origin, newEnt->s.origin);
    VectorCopy(angles, newEnt->s.angles);
    newEnt->classname = ED_NewString(classname);
    newEnt->monsterinfo.aiflags |= AI_DO_NOT_COUNT;

    ED_CallSpawn(newEnt);

    // summoned monsters read as targets through the IR goggles
    newEnt->s.renderfx |= RF_IR_VISIBLE;

    return newEnt;
}

/*
=================
CheckSpawnPoint

Is there room for a body of this size at this exact spot?
=================
*/
bool CheckSpawnPoint(vec3_t origin, vec3_t mins, vec3_t maxs)
{
    trace_t tr;

    if (!mins || !maxs || VectorCompare(mins, vec3_origin) || VectorCompare(maxs, vec3_origin))
        return false;

    tr = gi.trace(origin, mins, maxs, origin, NULL, MASK_MONSTERSOLID);
    if (tr.startsolid || tr.allsolid)
        return false;

    if (tr.ent != world)
        return false;

    return true;
}

/*
=================
CheckGroundSpawnPoint

As CheckSpawnPoint, plus: there is ground within `height` below, it is flat
enough to stand on (within one step across the whole footprint), and it is not
lava or slime.
=================
*/
bool CheckGroundSpawnPoint(vec3_t origin, vec3_t entMins, vec3_t entMaxs, float height, float gravity)
{
    trace_t tr;
    vec3_t  start, stop;
    vec3_t  mins, maxs;
    int     x, y;
    float   mid, bottom;

    if (!CheckSpawnPoint(origin, entMins, entMaxs))
        return false;

    VectorCopy(origin, stop);
    stop[2] = origin[2] + entMins[2] - height;

    tr = gi.trace(origin, entMins, entMaxs, stop, NULL, MASK_MONSTERSOLID | MASK_WATER);

    if (tr.fraction < 1 && (tr.contents & MASK_MONSTERSOLID)) {
        VectorAdd(tr.endpos, entMins, mins);
        VectorAdd(tr.endpos, entMaxs, maxs);

        // the cheap test first: are all four corners over solid?
        if (gravity > 0)
            start[2] = maxs[2] + 1;
        else
            start[2] = mins[2] - 1;

        for (x = 0; x <= 1; x++) {
            for (y = 0; y <= 1; y++) {
                start[0] = x ? maxs[0] : mins[0];
                start[1] = y ? maxs[1] : mins[1];
                if (gi.pointcontents(start) != CONTENTS_SOLID)
                    goto realcheck;
            }
        }

        return true;    // passed all four, done

realcheck:
        // trace the midpoint down for real
        start[0] = stop[0] = (mins[0] + maxs[0]) * 0.5f;
        start[1] = stop[1] = (mins[1] + maxs[1]) * 0.5f;
        start[2] = mins[2];

        tr = gi.trace(start, vec3_origin, vec3_origin, stop, NULL, MASK_MONSTERSOLID);
        if (tr.fraction == 1.0f)
            return false;

        if (gravity < 0) {
            start[2] = mins[2];
            stop[2] = start[2] - STEPSIZE - STEPSIZE;
            mid = bottom = tr.endpos[2] + entMins[2];
        } else {
            start[2] = maxs[2];
            stop[2] = start[2] + STEPSIZE + STEPSIZE;
            mid = bottom = tr.endpos[2] - entMaxs[2];
        }

        for (x = 0; x <= 1; x++) {
            for (y = 0; y <= 1; y++) {
                start[0] = stop[0] = x ? maxs[0] : mins[0];
                start[1] = stop[1] = y ? maxs[1] : mins[1];

                tr = gi.trace(start, vec3_origin, vec3_origin, stop, NULL, MASK_MONSTERSOLID);

                if (gravity > 0) {
                    if (tr.fraction != 1.0f && tr.endpos[2] < bottom)
                        bottom = tr.endpos[2];
                    if (tr.fraction == 1.0f || tr.endpos[2] - mid > STEPSIZE)
                        return false;
                } else {
                    if (tr.fraction != 1.0f && tr.endpos[2] > bottom)
                        bottom = tr.endpos[2];
                    if (tr.fraction == 1.0f || mid - tr.endpos[2] > STEPSIZE)
                        return false;
                }
            }
        }

        return true;    // it's clear
    }

    // it either didn't hit anything, or hit water/lava rather than solid
    return false;
}

/*
=================
FindSpawnPoint

Nudge upward (by up to maxMoveUp) until a body of this size clears. Returns the
spot in `spawnpoint`.
=================
*/
bool FindSpawnPoint(vec3_t startpoint, vec3_t mins, vec3_t maxs, vec3_t spawnpoint, float maxMoveUp)
{
    trace_t tr;
    vec3_t  top;

    tr = gi.trace(startpoint, mins, maxs, startpoint, NULL, MASK_MONSTERSOLID | CONTENTS_PLAYERCLIP);

    if (tr.startsolid || tr.allsolid || tr.ent != world) {
        VectorCopy(startpoint, top);
        top[2] += maxMoveUp;

        tr = gi.trace(top, mins, maxs, startpoint, NULL, MASK_MONSTERSOLID);
        if (tr.startsolid || tr.allsolid)
            return false;

        VectorCopy(tr.endpos, spawnpoint);
        return true;
    }

    VectorCopy(startpoint, spawnpoint);
    return true;
}

/*
=================
CreateGroundMonster

FindSpawnPoint has already picked the spot; this confirms the ground and builds
the monster. Pass a zero bbox to have it worked out from the classname.
=================
*/
edict_t *CreateGroundMonster(vec3_t origin, vec3_t angles, vec3_t entMins, vec3_t entMaxs, char *classname, int height)
{
    vec3_t  mins, maxs;

    if (!classname)
        return NULL;

    if (!entMins || !entMaxs || VectorCompare(entMins, vec3_origin) || VectorCompare(entMaxs, vec3_origin))
        DetermineBBox(classname, mins, maxs);
    else {
        VectorCopy(entMins, mins);
        VectorCopy(entMaxs, maxs);
    }

    if (!CheckGroundSpawnPoint(origin, mins, maxs, height, -1))
        return NULL;

    return CreateMonster(origin, angles, classname);
}

/*
=================
SpawnGrow

The tumbling translucent shell that plays over a spot while a monster is being
summoned into it. The rerelease drives this with s.scale/s.alpha, neither of
which exists in this protocol, so this is rogue's frame-based version.
=================
*/
void spawngrow_think(edict_t *self)
{
    int i;

    for (i = 0; i < 2; i++) {
        self->s.angles[0] = Q_rand() % 360;
        self->s.angles[1] = Q_rand() % 360;
        self->s.angles[2] = Q_rand() % 360;
    }

    if (level.framenum < self->wait && self->s.frame < 2)
        self->s.frame++;

    if (level.framenum >= self->wait) {
        if (self->s.effects & EF_SPHERETRANS) {
            G_FreeEdict(self);
            return;
        } else if (self->s.frame > 0) {
            self->s.frame--;
        } else {
            G_FreeEdict(self);
            return;
        }
    }

    self->nextthink = level.framenum + 1;
}

void SpawnGrow_Spawn(vec3_t startpos, int size)
{
    edict_t *ent;
    int     i;

    ent = G_Spawn();
    VectorCopy(startpos, ent->s.origin);

    for (i = 0; i < 2; i++) {
        ent->s.angles[0] = Q_rand() % 360;
        ent->s.angles[1] = Q_rand() % 360;
        ent->s.angles[2] = Q_rand() % 360;
    }

    ent->solid = SOLID_NOT;
    ent->s.renderfx = RF_IR_VISIBLE;
    ent->movetype = MOVETYPE_NONE;
    ent->classname = "spawngro";

    // spawngro3 is a rerelease-only model and is not among the extracted
    // assets, so both sizes use spawngro2 - which is what rogue does as well
    if (size <= 1)
        ent->s.modelindex = gi.modelindex("models/items/spawngro2/tris.md2");
    else
        ent->s.modelindex = gi.modelindex("models/items/spawngro/tris.md2");

    ent->think = spawngrow_think;
    ent->wait = level.framenum + SPAWNGROW_LIFESPAN * BASE_FRAMERATE;
    ent->nextthink = level.framenum + 1;

    ent->s.effects |= EF_SPHERETRANS;

    gi.linkentity(ent);
}

/*
=================
PickCoopTarget

Hand a summoned monster one of the coop players at random, so a commander's
reinforcements do not all pile onto whoever it is fighting.
=================
*/
edict_t *PickCoopTarget(edict_t *self)
{
    edict_t *targets[MAX_CLIENTS];
    int     num_targets = 0, targetID;
    edict_t *ent;
    int     player;

    // if we're not in coop, this is a noop
    if (!coop->value)
        return NULL;

    memset(targets, 0, sizeof(targets));

    for (player = 1; player <= game.maxclients; player++) {
        ent = &g_edicts[player];
        if (!ent->inuse)
            continue;
        if (!ent->client)
            continue;
        if (ent->health <= 0)
            continue;
        targets[num_targets++] = ent;
    }

    if (!num_targets)
        return NULL;

    if (num_targets == 1)
        return targets[0];

    targetID = Q_rand() % num_targets;
    return targets[targetID];
}

/*
=================
M_SetupReinforcements

Parse a "classname strength;classname strength;..." list into the monster's
reinforcement table, caching each entry's bounding box.

Rogue hardcoded a fixed seven-monster list indexed by strength. The rerelease
replaced that with this key, and every medic commander in the MGU maps sets it
(mgu3m3 asks for lasergun/hypergun soldiers, mgu1m4 for SS), so the key has to
be honoured or the summons come out as the wrong monsters entirely.
=================
*/
void M_SetupReinforcements(edict_t *self, const char *reinforcements)
{
    char            buffer[MAX_STRING_CHARS];
    const char      *p;
    char            *token;
    reinforcement_t *r;
    int             i;

    self->monsterinfo.num_reinforcements = 0;

    if (!reinforcements || !*reinforcements)
        return;

    // this tree's COM_Parse splits on whitespace only, while the key uses ';'
    // between entries - flatten the separators into spaces first
    Q_strlcpy(buffer, reinforcements, sizeof(buffer));
    for (i = 0; buffer[i]; i++)
        if (buffer[i] == ';')
            buffer[i] = ' ';

    p = buffer;

    while (self->monsterinfo.num_reinforcements < MAX_REINFORCEMENT_TYPES) {
        token = COM_Parse(&p);
        if (!*token)
            break;

        r = &self->monsterinfo.reinforcements[self->monsterinfo.num_reinforcements];
        r->classname = G_CopyString(token);

        token = COM_Parse(&p);
        r->strength = atoi(token);
        if (r->strength < 1)
            r->strength = 1;

        // cache the body size, and incidentally precache everything this
        // monster needs - a summon mid-level cannot precache safely
        DetermineBBox(r->classname, r->mins, r->maxs);

        self->monsterinfo.num_reinforcements++;
    }
}

/*
=================
M_SlotsLeft

How much summoning capacity a commander has left.
=================
*/
int M_SlotsLeft(edict_t *self)
{
    return self->monsterinfo.monster_slots - self->monsterinfo.monster_used;
}

/*
=================
M_PickReinforcements

Choose which reinforcements to summon this time, subject to the slots left.
Fills monsterinfo.chosen_reinforcements with indices and returns how many.
=================
*/
int M_PickReinforcements(edict_t *self, int max_slots)
{
    int available[MAX_REINFORCEMENT_TYPES];
    int num_available;
    int remaining, num_slots, num_chosen, i;

    for (i = 0; i < MAX_REINFORCEMENTS; i++)
        self->monsterinfo.chosen_reinforcements[i] = -1;

    // how many to try for. Rogue's log2 curve, so large groups stay rare.
    num_slots = 1 + (Q_rand() % MAX_REINFORCEMENTS);
    if (num_slots > 3)
        num_slots = 1 + (Q_rand() % 3);

    remaining = self->monsterinfo.monster_slots - self->monsterinfo.monster_used;

    for (num_chosen = 0; num_chosen < MAX_REINFORCEMENTS && num_chosen < num_slots; num_chosen++) {
        if ((max_slots && num_chosen >= max_slots) || remaining <= 0)
            break;

        // everything that still fits in the slots we have left
        num_available = 0;
        for (i = 0; i < self->monsterinfo.num_reinforcements; i++)
            if (self->monsterinfo.reinforcements[i].strength <= remaining)
                available[num_available++] = i;

        if (!num_available)
            break;

        self->monsterinfo.chosen_reinforcements[num_chosen] = available[Q_rand() % num_available];
        remaining -= self->monsterinfo.reinforcements[self->monsterinfo.chosen_reinforcements[num_chosen]].strength;
    }

    self->monsterinfo.num_chosen_reinforcements = num_chosen;
    return num_chosen;
}
