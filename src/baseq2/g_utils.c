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
// g_utils.c -- misc utility functions for game module

#include "g_local.h"


void G_ProjectSource(const vec3_t point, const vec3_t distance, const vec3_t forward, const vec3_t right, vec3_t result)
{
    result[0] = point[0] + forward[0] * distance[0] + right[0] * distance[1];
    result[1] = point[1] + forward[1] * distance[0] + right[1] * distance[1];
    result[2] = point[2] + forward[2] * distance[0] + right[2] * distance[1] + distance[2];
}


/*
=============
G_Find

Searches all active entities for the next one that holds
the matching string at fieldofs (use the FOFS() macro) in the structure.

Searches beginning at the edict after from, or the beginning if NULL
NULL will be returned if the end of the list is reached.

=============
*/
edict_t *G_Find(edict_t *from, int fieldofs, char *match)
{
    char    *s;

    if (!from)
        from = g_edicts;
    else
        from++;

    for (; from < &g_edicts[globals.num_edicts] ; from++) {
        if (!from->inuse)
            continue;
        s = *(char **)((byte *)from + fieldofs);
        if (!s)
            continue;
        if (!Q_stricmp(s, match))
            return from;
    }

    return NULL;
}


/*
=================
findradius

Returns entities that have origins within a spherical area

findradius (origin, radius)
=================
*/
edict_t *findradius(edict_t *from, vec3_t org, float rad)
{
    vec3_t  eorg;
    int     j;

    if (!from)
        from = g_edicts;
    else
        from++;
    for (; from < &g_edicts[globals.num_edicts]; from++) {
        if (!from->inuse)
            continue;
        if (from->solid == SOLID_NOT)
            continue;
        for (j = 0 ; j < 3 ; j++)
            eorg[j] = org[j] - (from->s.origin[j] + (from->mins[j] + from->maxs[j]) * 0.5f);
        if (VectorLength(eorg) > rad)
            continue;
        return from;
    }

    return NULL;
}


/*
=============
G_PickTarget

Searches all active entities for the next one that holds
the matching string at fieldofs (use the FOFS() macro) in the structure.

Searches beginning at the edict after from, or the beginning if NULL
NULL will be returned if the end of the list is reached.

=============
*/
#define MAXCHOICES  8

edict_t *G_PickTarget(char *targetname)
{
    edict_t *ent = NULL;
    int     num_choices = 0;
    edict_t *choice[MAXCHOICES];

    if (!targetname) {
        gi.dprintf("G_PickTarget called with NULL targetname\n");
        return NULL;
    }

    while (1) {
        ent = G_Find(ent, FOFS(targetname), targetname);
        if (!ent)
            break;
        choice[num_choices++] = ent;
        if (num_choices == MAXCHOICES)
            break;
    }

    if (!num_choices) {
        gi.dprintf("G_PickTarget: target %s not found\n", targetname);
        return NULL;
    }

    return choice[Q_rand_uniform(num_choices)];
}



void Think_Delay(edict_t *ent)
{
    G_UseTargets(ent, ent->activator);
    G_FreeEdict(ent);
}

/*
==============================
G_UseTargets

the global "activator" should be set to the entity that initiated the firing.

If self.delay is set, a DelayedUse entity will be created that will actually
do the SUB_UseTargets after that many seconds have passed.

Centerprints any self.message to the activator.

Search for (string)targetname in all entities that
match (string)self.target and call their .use function

==============================
*/
void G_UseTargets(edict_t *ent, edict_t *activator)
{
    edict_t     *t;

//
// check for a delay
//
    if (ent->delay) {
        // create a temp object to fire at a later time
        t = G_Spawn();
        t->classname = "DelayedUse";
        t->nextthink = level.framenum + ent->delay * BASE_FRAMERATE;
        t->think = Think_Delay;
        t->activator = activator;
        if (!activator)
            gi.dprintf("Think_Delay with no activator\n");
        t->message = ent->message;
        t->target = ent->target;
        t->killtarget = ent->killtarget;
        return;
    }


//
// print the message
//
    if ((ent->message) && !(activator->svflags & SVF_MONSTER)) {
        gi.centerprintf(activator, "%s", ent->message);
        if (ent->noise_index)
            gi.sound(activator, CHAN_AUTO, ent->noise_index, 1, ATTN_NORM, 0);
        else
            gi.sound(activator, CHAN_AUTO, gi.soundindex("misc/talk1.wav"), 1, ATTN_NORM, 0);
    }

//
// kill killtargets
//
    if (ent->killtarget) {
        t = NULL;
        while ((t = G_Find(t, FOFS(targetname), ent->killtarget))) {
            G_FreeEdict(t);
            if (!ent->inuse) {
                gi.dprintf("entity was removed while using killtargets\n");
                return;
            }
        }
    }

//
// fire targets
//
    if (ent->target) {
        t = NULL;
        while ((t = G_Find(t, FOFS(targetname), ent->target))) {
            // doors fire area portals in a specific way
            if (!Q_stricmp(t->classname, "func_areaportal") &&
                (!Q_stricmp(ent->classname, "func_door") || !Q_stricmp(ent->classname, "func_door_rotating") || !Q_stricmp(ent->classname, "func_water")))
                continue;

            if (t == ent) {
                gi.dprintf("WARNING: Entity used itself.\n");
            } else {
                if (t->use)
                    t->use(t, ent, activator);
            }
            if (!ent->inuse) {
                gi.dprintf("entity was removed while using targets\n");
                return;
            }
        }
    }
}


/*
=============
TempVector

This is just a convenience function
for making temporary vectors for function calls
=============
*/
float   *tv(float x, float y, float z)
{
    static  int     index;
    static  vec3_t  vecs[8];
    float   *v;

    // use an array so that multiple tempvectors won't collide
    // for a while
    v = vecs[index];
    index = (index + 1) & 7;

    v[0] = x;
    v[1] = y;
    v[2] = z;

    return v;
}


vec3_t VEC_UP       = {0, -1, 0};
vec3_t MOVEDIR_UP   = {0, 0, 1};
vec3_t VEC_DOWN     = {0, -2, 0};
vec3_t MOVEDIR_DOWN = {0, 0, -1};

void G_SetMovedir(vec3_t angles, vec3_t movedir)
{
    if (VectorCompare(angles, VEC_UP)) {
        VectorCopy(MOVEDIR_UP, movedir);
    } else if (VectorCompare(angles, VEC_DOWN)) {
        VectorCopy(MOVEDIR_DOWN, movedir);
    } else {
        AngleVectors(angles, movedir, NULL, NULL);
    }

    VectorClear(angles);
}


/*
=============
vectoyaw2

ROGUE's yaw-only vectoangles. Unlike vectoyaw() below it does not truncate, and
it works off the raw x/y rather than normalizing first - the carrier aims its
spawn hatch with it, where whole-degree rounding is visible.
=============
*/
float vectoyaw2(vec3_t vec)
{
    float yaw;

    if (vec[PITCH] == 0) {
        if (vec[YAW] == 0)
            yaw = 0;
        else if (vec[YAW] > 0)
            yaw = 90;
        else
            yaw = 270;
    } else {
        yaw = RAD2DEG(atan2f(vec[YAW], vec[PITCH]));
        if (yaw < 0)
            yaw += 360;
    }

    return yaw;
}

float vectoyaw(vec3_t vec)
{
    float   yaw;

    if (/*vec[YAW] == 0 &&*/ vec[PITCH] == 0) {
        yaw = 0;
        if (vec[YAW] > 0)
            yaw = 90;
        else if (vec[YAW] < 0)
            yaw = -90;
    } else {
        yaw = (int)RAD2DEG(atan2(vec[YAW], vec[PITCH]));
        if (yaw < 0)
            yaw += 360;
    }

    return yaw;
}


void vectoangles(vec3_t value1, vec3_t angles)
{
    float   forward;
    float   yaw, pitch;

    if (value1[1] == 0 && value1[0] == 0) {
        yaw = 0;
        if (value1[2] > 0)
            pitch = 90;
        else
            pitch = 270;
    } else {
        if (value1[0])
            yaw = (int)RAD2DEG(atan2(value1[1], value1[0]));
        else if (value1[1] > 0)
            yaw = 90;
        else
            yaw = -90;
        if (yaw < 0)
            yaw += 360;

        forward = sqrtf(value1[0] * value1[0] + value1[1] * value1[1]);
        pitch = (int)RAD2DEG(atan2(value1[2], forward));
        if (pitch < 0)
            pitch += 360;
    }

    angles[PITCH] = -pitch;
    angles[YAW] = yaw;
    angles[ROLL] = 0;
}

char *G_CopyString(char *in)
{
    char    *out;

    out = gi.TagMalloc(strlen(in) + 1, TAG_LEVEL);
    strcpy(out, in);
    return out;
}


void G_InitEdict(edict_t *e)
{
    e->inuse = true;
    e->classname = "noclass";
    e->gravity = 1.0f;
    e->s.number = e - g_edicts;

    // down is down unless a monster says otherwise
    VectorSet(e->gravityVector, 0.0f, 0.0f, -1.0f);
}

/*
=================
G_Spawn

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *G_Spawn(void)
{
    int         i;
    edict_t     *e;

    e = &g_edicts[game.maxclients + 1];
    for (i = game.maxclients + 1 ; i < globals.num_edicts ; i++, e++) {
        // the first couple seconds of server time can involve a lot of
        // freeing and allocating, so relax the replacement policy
        if (!e->inuse && (e->freetime < 2 || level.time - e->freetime > 0.5f)) {
            G_InitEdict(e);
            return e;
        }
    }

    if (i == game.maxentities)
        gi.error("ED_Alloc: no free edicts");

    globals.num_edicts++;
    G_InitEdict(e);
    return e;
}

/*
=================
G_FreeEdict

Marks the edict as free
=================
*/
void G_FreeEdict(edict_t *ed)
{
    gi.unlinkentity(ed);        // unlink from world

    if ((ed - g_edicts) <= (maxclients->value + BODY_QUEUE_SIZE)) {
//      gi.dprintf("tried to free special edict\n");
        return;
    }

    memset(ed, 0, sizeof(*ed));
    ed->classname = "freed";
    ed->freetime = level.time;
    ed->inuse = false;
}


/*
============
G_TouchTriggers

============
*/
void    G_TouchTriggers(edict_t *ent)
{
    int         i, num;
    edict_t     *touch[MAX_EDICTS], *hit;

    // dead things don't activate triggers!
    if ((ent->client || (ent->svflags & SVF_MONSTER)) && (ent->health <= 0))
        return;

    num = gi.BoxEdicts(ent->absmin, ent->absmax, touch
                       , MAX_EDICTS, AREA_TRIGGERS);

    // be careful, it is possible to have an entity in this
    // list removed before we get to it (killtriggered)
    for (i = 0 ; i < num ; i++) {
        hit = touch[i];
        if (!hit->inuse)
            continue;
        if (!hit->touch)
            continue;
        hit->touch(hit, ent, NULL, NULL);
    }
}

/*
============
G_TouchSolids

Call after linking a new trigger in during gameplay
to force all entities it covers to immediately touch it
============
*/
void    G_TouchSolids(edict_t *ent)
{
    int         i, num;
    edict_t     *touch[MAX_EDICTS], *hit;

    num = gi.BoxEdicts(ent->absmin, ent->absmax, touch
                       , MAX_EDICTS, AREA_SOLID);

    // be careful, it is possible to have an entity in this
    // list removed before we get to it (killtriggered)
    for (i = 0 ; i < num ; i++) {
        hit = touch[i];
        if (!hit->inuse)
            continue;
        if (ent->touch)
            ent->touch(hit, ent, NULL, NULL);
        if (!ent->inuse)
            break;
    }
}




/*
==============================================================================

Kill box

==============================================================================
*/

/*
=================
KillBox

Kills all entities that would touch the proposed new positioning
of ent.  Ent should be unlinked before calling this!
=================
*/
bool KillBox(edict_t *ent)
{
    trace_t     tr;

    while (1) {
        tr = gi.trace(ent->s.origin, ent->mins, ent->maxs, ent->s.origin, NULL, MASK_PLAYERSOLID);
        if (!tr.ent)
            break;

        // nail it
        T_Damage(tr.ent, ent, ent, vec3_origin, ent->s.origin, vec3_origin, 100000, 0, DAMAGE_NO_PROTECTION, MOD_TELEFRAG);

        // if we didn't kill it, fail
        if (tr.ent->solid)
            return false;
    }

    return true;        // all clear
}

/*
=================
G_FixStuckObject

[Paril-KEX] generic code to detect and fix an object spawned inside solid.
Ported from G_FixStuckObject_Generic in src/rerelease/p_move.cpp. The rerelease
takes a trace lambda; here the only caller needs the server trace against a
single edict, so the edict and clip mask are passed directly.

Returns STUCK_GOOD_POSITION if `check` was never solid, STUCK_FIXED (with
`check` moved to the nearest free spot) or STUCK_NO_GOOD_POSITION. The caller
decides whether to commit the new origin.
=================
*/
stuck_result_t G_FixStuckObject(edict_t *self, vec3_t check, int mask)
{
    // the six faces of our own box, and which of our own extents form the
    // flattened probe box for each one
    static const struct {
        int8_t normal[3];
        int8_t mins[3], maxs[3];
    } side_checks[6] = {
        { {  0,  0,  1 }, { -1, -1,  0 }, {  1,  1,  0 } },
        { {  0,  0, -1 }, { -1, -1,  0 }, {  1,  1,  0 } },
        { {  1,  0,  0 }, {  0, -1, -1 }, {  0,  1,  1 } },
        { { -1,  0,  0 }, {  0, -1, -1 }, {  0,  1,  1 } },
        { {  0,  1,  0 }, { -1,  0, -1 }, {  1,  0,  1 } },
        { {  0, -1,  0 }, { -1,  0, -1 }, {  1,  0,  1 } },
    };

    vec3_t  best_origin;
    float   best_distance = 0;
    bool    found = false;
    trace_t tr;
    int     sn, n, e;

    tr = gi.trace(check, self->mins, self->maxs, check, self, mask);
    if (!tr.startsolid)
        return STUCK_GOOD_POSITION;

    for (sn = 0; sn < 6; sn++) {
        const int8_t *normal = side_checks[sn].normal;
        vec3_t  start, mins, maxs;
        vec3_t  opposite_start, end, delta, new_origin;
        const int8_t *other_normal;
        int     needed_epsilon_fix = -1;
        int     needed_epsilon_dir = 0;
        float   dist;

        VectorCopy(check, start);
        VectorClear(mins);
        VectorClear(maxs);

        for (n = 0; n < 3; n++) {
            if (normal[n] < 0)
                start[n] += self->mins[n];
            else if (normal[n] > 0)
                start[n] += self->maxs[n];

            if (side_checks[sn].mins[n] == -1)
                mins[n] = self->mins[n];
            else if (side_checks[sn].mins[n] == 1)
                mins[n] = self->maxs[n];

            if (side_checks[sn].maxs[n] == -1)
                maxs[n] = self->mins[n];
            else if (side_checks[sn].maxs[n] == 1)
                maxs[n] = self->maxs[n];
        }

        tr = gi.trace(start, mins, maxs, start, self, mask);

        // the probe is flush against a plane; nudge it a unit along one of the
        // two axes it spans and try again
        if (tr.startsolid) {
            for (e = 0; e < 3; e++) {
                vec3_t ep_start;

                if (normal[e] != 0)
                    continue;

                VectorCopy(start, ep_start);
                ep_start[e] += 1;
                tr = gi.trace(ep_start, mins, maxs, ep_start, self, mask);
                if (!tr.startsolid) {
                    VectorCopy(ep_start, start);
                    needed_epsilon_fix = e;
                    needed_epsilon_dir = 1;
                    break;
                }

                ep_start[e] -= 2;
                tr = gi.trace(ep_start, mins, maxs, ep_start, self, mask);
                if (!tr.startsolid) {
                    VectorCopy(ep_start, start);
                    needed_epsilon_fix = e;
                    needed_epsilon_dir = -1;
                    break;
                }
            }
        }

        // no good
        if (tr.startsolid)
            continue;

        // this side is free; trace from it back to the opposite face to find
        // out how much clearance there is
        other_normal = side_checks[sn ^ 1].normal;
        VectorCopy(check, opposite_start);
        for (n = 0; n < 3; n++) {
            if (other_normal[n] < 0)
                opposite_start[n] += self->mins[n];
            else if (other_normal[n] > 0)
                opposite_start[n] += self->maxs[n];
        }

        if (needed_epsilon_fix >= 0)
            opposite_start[needed_epsilon_fix] += needed_epsilon_dir;

        tr = gi.trace(start, mins, maxs, opposite_start, self, mask);
        if (tr.startsolid)
            continue;

        VectorCopy(tr.endpos, end);

        // push very slightly away from the wall
        end[0] += normal[0] * 0.125f;
        end[1] += normal[1] * 0.125f;
        end[2] += normal[2] * 0.125f;

        VectorSubtract(end, opposite_start, delta);
        VectorAdd(check, delta, new_origin);

        if (needed_epsilon_fix >= 0)
            new_origin[needed_epsilon_fix] += needed_epsilon_dir;

        tr = gi.trace(new_origin, self->mins, self->maxs, new_origin, self, mask);
        if (tr.startsolid)
            continue;

        dist = DotProduct(delta, delta);
        if (!found || dist < best_distance) {
            VectorCopy(new_origin, best_origin);
            best_distance = dist;
            found = true;
        }
    }

    if (!found)
        return STUCK_NO_GOOD_POSITION;

    VectorCopy(best_origin, check);
    return STUCK_FIXED;
}
