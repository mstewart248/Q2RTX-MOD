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
// g_ai.c

#include "g_local.h"

bool FindTarget(edict_t *self);
extern cvar_t   *maxclients;

bool ai_checkattack(edict_t *self, float dist);

bool        enemy_vis;
int         enemy_range;
float       enemy_yaw;

//============================================================================


/*
=================
AI_SetSightClient

Called once each frame to set level.sight_client to the
player to be checked for in findtarget.

If all clients are either dead or in notarget, sight_client
will be null.

In coop games, sight_client will cycle between the clients.
=================
*/
void AI_SetSightClient(void)
{
    edict_t *ent;
    int     start, check;

    if (level.sight_client == NULL)
        start = 1;
    else
        start = level.sight_client - g_edicts;

    check = start;
    while (1) {
        check++;
        if (check > game.maxclients)
            check = 1;
        ent = &g_edicts[check];
        if (ent->inuse
            && ent->health > 0
            && !(ent->flags & FL_NOTARGET)) {
            level.sight_client = ent;
            return;     // got one
        }
        if (check == start) {
            level.sight_client = NULL;
            return;     // nobody to see
        }
    }
}

//============================================================================

/*
=============
ai_move

Move the specified distance at current facing.
This replaces the QC functions: ai_forward, ai_back, ai_pain, and ai_painforward
==============
*/
void ai_move(edict_t *self, float dist)
{
    M_walkmove(self, self->s.angles[YAW], dist);
}


/*
=============
ai_stand

Used for standing around and looking for players
Distance is for slight position adjustments needed by the animations
==============
*/
void ai_stand(edict_t *self, float dist)
{
    vec3_t  v;

    if (dist)
        M_walkmove(self, self->s.angles[YAW], dist);

    if (self->monsterinfo.aiflags & AI_STAND_GROUND) {
        if (self->enemy) {
            VectorSubtract(self->enemy->s.origin, self->s.origin, v);
            self->ideal_yaw = vectoyaw(v);
            if (self->s.angles[YAW] != self->ideal_yaw && self->monsterinfo.aiflags & AI_TEMP_STAND_GROUND) {
                self->monsterinfo.aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
                self->monsterinfo.run(self);
            }
            M_ChangeYaw(self);
            ai_checkattack(self, 0);
        } else
            FindTarget(self);
        return;
    }

    if (FindTarget(self))
        return;

    if (level.framenum > self->monsterinfo.pause_framenum) {
        self->monsterinfo.walk(self);
        return;
    }

    if (!(self->spawnflags & 1) && (self->monsterinfo.idle) && (level.framenum > self->monsterinfo.idle_framenum)) {
        if (self->monsterinfo.idle_framenum) {
            self->monsterinfo.idle(self);
            self->monsterinfo.idle_framenum = level.framenum + (1 + random()) * 15 * BASE_FRAMERATE;
        } else {
            self->monsterinfo.idle_framenum = level.framenum + random() * 15 * BASE_FRAMERATE;
        }
    }
}


/*
=============
ai_walk

The monster is walking it's beat
=============
*/
void ai_walk(edict_t *self, float dist)
{
    M_MoveToGoal(self, dist);

    // check for noticing a player
    if (FindTarget(self))
        return;

    if ((self->monsterinfo.search) && (level.framenum > self->monsterinfo.idle_framenum)) {
        if (self->monsterinfo.idle_framenum) {
            self->monsterinfo.search(self);
            self->monsterinfo.idle_framenum = level.framenum + (1 + random()) * 15 * BASE_FRAMERATE;
        } else {
            self->monsterinfo.idle_framenum = level.framenum + random() * 15 * BASE_FRAMERATE;
        }
    }
}


/*
=============
ai_charge

Turns towards target and advances
Use this call with a distnace of 0 to replace ai_face
==============
*/
void ai_charge(edict_t *self, float dist)
{
    vec3_t  v;

    VectorSubtract(self->enemy->s.origin, self->s.origin, v);
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);

    if (dist)
        M_walkmove(self, self->s.angles[YAW], dist);
}


/*
=============
ai_turn

don't move, but turn towards ideal_yaw
Distance is for slight position adjustments needed by the animations
=============
*/
void ai_turn(edict_t *self, float dist)
{
    if (dist)
        M_walkmove(self, self->s.angles[YAW], dist);

    if (FindTarget(self))
        return;

    M_ChangeYaw(self);
}


/*

.enemy
Will be world if not currently angry at anyone.

.movetarget
The next path spot to walk toward.  If .enemy, ignore .movetarget.
When an enemy is killed, the monster will try to return to it's path.

.hunt_time
Set to time + something when the player is in sight, but movement straight for
him is blocked.  This causes the monster to use wall following code for
movement direction instead of sighting on the player.

.ideal_yaw
A yaw angle of the intended direction, which will be turned towards at up
to 45 deg / state.  If the enemy is in view and hunt_time is not active,
this will be the exact line towards the enemy.

.pausetime
A monster will leave it's stand state and head towards it's .movetarget when
time > .pausetime.

walkmove(angle, speed) primitive is all or nothing
*/

/*
=============
range

returns the range catagorization of an entity reletive to self
0   melee range, will become hostile even if back is turned
1   visibility and infront, or visibility and show hostile
2   infront and show hostile
3   only triggered by damage
=============
*/
/*
=============
M_RangeBetween

How far apart two entities are, for the purposes of AI range checks.

The rerelease measures between BOUNDING BOXES (distance_between_boxes); 1997
Quake II measures between ORIGINS. For a stock-sized monster the two only differ
by a few dozen units, and switching wholesale would move every monster's
engagement distance in the game - so the origin measure stays.

A SCALED monster is a different matter. mgu6m3's Modir is a monster_shambler at
5.5, which makes its bounding box 176 units wide: a player standing against it
is ~192 units from its origin, well past MELEE_DISTANCE (80), so on the origin
measure it could never once reach melee range and would only ever cast
lightning. For those, fall back to the rerelease's box distance.
=============
*/
float M_RangeBetween(edict_t *self, edict_t *other)
{
    vec3_t  v;
    int     i;

    if (self->s.scale > 1.f || other->s.scale > 1.f) {
        // gap along each axis, 0 where the boxes overlap
        for (i = 0; i < 3; i++) {
            if (self->absmin[i] > other->absmax[i])
                v[i] = self->absmin[i] - other->absmax[i];
            else if (other->absmin[i] > self->absmax[i])
                v[i] = other->absmin[i] - self->absmax[i];
            else
                v[i] = 0;
        }
        return VectorLength(v);
    }

    VectorSubtract(self->s.origin, other->s.origin, v);
    return VectorLength(v);
}

int range(edict_t *self, edict_t *other)
{
    float   len;

    len = M_RangeBetween(self, other);
    if (len < MELEE_DISTANCE)
        return RANGE_MELEE;
    if (len < 500)
        return RANGE_NEAR;
    if (len < 1000)
        return RANGE_MID;
    return RANGE_FAR;
}

/*
=============
visible

returns 1 if the entity is visible to self, even if not infront ()
=============
*/
bool visible(edict_t *self, edict_t *other)
{
    vec3_t  spot1;
    vec3_t  spot2;
    trace_t trace;

    VectorCopy(self->s.origin, spot1);
    spot1[2] += self->viewheight;
    VectorCopy(other->s.origin, spot2);
    spot2[2] += other->viewheight;
    trace = gi.trace(spot1, vec3_origin, vec3_origin, spot2, self, MASK_OPAQUE);

    if (trace.fraction == 1.0f)
        return true;
    return false;
}


/*
=============
infront

returns 1 if the entity is in front (in sight) of self
=============
*/

/*
=============
inback / below / realrange / PredictAim

ROGUE helpers. The carrier is the first monster here that cares which side of
itself a player is on - it will not fire its rail gun or rockets at someone
behind or underneath it, and in coop it deliberately lobs a rocket at whoever
it is NOT currently fighting.
=============
*/
bool inback(edict_t *self, edict_t *other)
{
    vec3_t  vec, forward;

    AngleVectors(self->s.angles, forward, NULL, NULL);
    VectorSubtract(other->s.origin, self->s.origin, vec);
    VectorNormalize(vec);

    return DotProduct(vec, forward) < -0.3f;
}

bool below(edict_t *self, edict_t *other)
{
    vec3_t  vec;
    static const vec3_t down = { 0, 0, -1 };

    VectorSubtract(other->s.origin, self->s.origin, vec);
    VectorNormalize(vec);

    // an 18 degree cone straight down
    return DotProduct(vec, down) > 0.95f;
}

/*
=================
ai_check_move

Would the monster be able to step `dist` units straight ahead?  Used by the
rerelease's run-and-gun attacks to stop a monster charging off a ledge while it
is firing.  Ported from ai_check_move() in src/rerelease/m_move.cpp: it performs
a real trial SV_movestep and then puts the origin back.
=================
*/
bool ai_check_move(edict_t *self, float dist)
{
    float   yaw;
    vec3_t  move;
    vec3_t  old_origin;

    yaw = self->s.angles[YAW] * M_PI * 2 / 360;

    move[0] = cosf(yaw) * dist;
    move[1] = sinf(yaw) * dist;
    move[2] = 0;

    VectorCopy(self->s.origin, old_origin);

    if (!SV_movestep(self, move, false))
        return false;

    VectorCopy(old_origin, self->s.origin);
    gi.linkentity(self);
    return true;
}

float realrange(edict_t *self, edict_t *other)
{
    return M_RangeBetween(self, other);
}

/*
=============
PredictAim

Where to shoot so the shot and the target arrive together. `offset` shaves that
much off the flight time, which the carrier uses (-0.3) to lead a little further
than the straight solution.
=============
*/
void PredictAim(edict_t *target, vec3_t start, float bolt_speed, bool eye_height,
                float offset, vec3_t aimdir, vec3_t aimpoint)
{
    vec3_t  dir, vec, ndir, nvec;
    float   dist, time;

    if (!target || !target->inuse) {
        if (aimdir)
            VectorClear(aimdir);
        return;
    }

    VectorSubtract(target->s.origin, start, dir);
    if (eye_height)
        dir[2] += target->viewheight;

    dist = VectorLength(dir);

    // A hitscan attack passes bolt_speed 0 - it has no travel time to lead.
    // Dividing anyway gives time == +infinity, and the VectorMA below then
    // produces an infinite (or, against a stationary target, NaN) aim point:
    // the shot goes off in a meaningless direction and hits nothing. The
    // rerelease guards this in its own PredictAim; this copy predates that.
    if (bolt_speed)
        time = dist / bolt_speed;
    else
        time = 0;

    VectorMA(target->s.origin, time - offset, target->velocity, vec);
    if (eye_height)
        vec[2] += target->viewheight;

    // "went backwards..." (the rerelease's own name for it): a fast enough
    // target can lead the prediction to a point behind the shooter. Aim
    // straight at them instead of firing away from them.
    VectorCopy(dir, ndir);
    VectorNormalize(ndir);
    VectorSubtract(vec, start, nvec);
    VectorNormalize(nvec);
    if (DotProduct(ndir, nvec) < 0) {
        VectorCopy(target->s.origin, vec);
        if (eye_height)
            vec[2] += target->viewheight;
    }

    if (aimdir) {
        VectorSubtract(vec, start, aimdir);
        VectorNormalize(aimdir);
    }

    if (aimpoint)
        VectorCopy(vec, aimpoint);
}

bool infront(edict_t *self, edict_t *other)
{
    vec3_t  vec;
    float   dot;
    vec3_t  forward;

    AngleVectors(self->s.angles, forward, NULL, NULL);
    VectorSubtract(other->s.origin, self->s.origin, vec);
    VectorNormalize(vec);
    dot = DotProduct(vec, forward);

    if (dot > 0.3f)
        return true;
    return false;
}


//============================================================================

void HuntTarget(edict_t *self)
{
    vec3_t  vec;

    self->goalentity = self->enemy;
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.stand(self);
    else
        self->monsterinfo.run(self);
    VectorSubtract(self->enemy->s.origin, self->s.origin, vec);
    self->ideal_yaw = vectoyaw(vec);
    // wait a while before first attack
    if (!(self->monsterinfo.aiflags & AI_STAND_GROUND))
        AttackFinished(self, 1);
}

void FoundTarget(edict_t *self)
{
    // let other monsters see this monster for a while
    if (self->enemy->client) {
        level.sight_entity = self;
        level.sight_entity_framenum = level.framenum;
        level.sight_entity->light_level = 128;
    }

    self->show_hostile = level.framenum + 1 * BASE_FRAMERATE;   // wake up other monsters

    VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
    M_UpdateBlindFireTarget(self);
    self->monsterinfo.trail_framenum = level.framenum;

    if (!self->combattarget) {
        HuntTarget(self);
        return;
    }

    self->goalentity = self->movetarget = G_PickTarget(self->combattarget);
    if (!self->movetarget) {
        self->goalentity = self->movetarget = self->enemy;
        HuntTarget(self);
        gi.dprintf("%s at %s, combattarget %s not found\n", self->classname, vtos(self->s.origin), self->combattarget);
        return;
    }

    // clear out our combattarget, these are a one shot deal
    self->combattarget = NULL;
    self->monsterinfo.aiflags |= AI_COMBAT_POINT;

    // clear the targetname, that point is ours!
    self->movetarget->targetname = NULL;
    self->monsterinfo.pause_framenum = 0;

    // run for it
    self->monsterinfo.run(self);
}


/*
===========
FindTarget

Self is currently not attacking anything, so try to find a target

Returns TRUE if an enemy was sighted

When a player fires a missile, the point of impact becomes a fakeplayer so
that monsters that see the impact will respond as if they had seen the
player.

To avoid spending too much time, only a single client (or fakeclient) is
checked each frame.  This means multi player games will have slightly
slower noticing monsters.
============
*/
bool FindTarget(edict_t *self)
{
    edict_t     *client;
    bool        heardit;
    int         r;

    if (self->monsterinfo.aiflags & AI_GOOD_GUY) {
        if (self->goalentity && self->goalentity->inuse && self->goalentity->classname) {
            if (strcmp(self->goalentity->classname, "target_actor") == 0)
                return false;
        }

        //FIXME look for monsters?
        return false;
    }

    // if we're going to a combat point, just proceed
    if (self->monsterinfo.aiflags & AI_COMBAT_POINT)
        return false;

// if the first spawnflag bit is set, the monster will only wake up on
// really seeing the player, not another monster getting angry or hearing
// something

// revised behavior so they will wake up if they "see" a player make a noise
// but not weapon impact/explosion noises

    heardit = false;
    if ((level.sight_entity_framenum >= (level.framenum - 1)) && !(self->spawnflags & 1)) {
        client = level.sight_entity;
        if (client->enemy == self->enemy) {
            return false;
        }
    } else if (level.sound_entity_framenum >= (level.framenum - 1)) {
        client = level.sound_entity;
        heardit = true;
    } else if (!(self->enemy) && (level.sound2_entity_framenum >= (level.framenum - 1)) && !(self->spawnflags & 1)) {
        client = level.sound2_entity;
        heardit = true;
    } else {
        client = level.sight_client;
        if (!client)
            return false;   // no clients to get mad at
    }

    // if the entity went away, forget it
    if (!client->inuse)
        return false;

    if (client == self->enemy)
        return true;    // JDC false;

    if (client->client) {
        if (client->flags & FL_NOTARGET)
            return false;
    } else if (client->svflags & SVF_MONSTER) {
        if (!client->enemy)
            return false;
        if (client->enemy->flags & FL_NOTARGET)
            return false;
    } else if (heardit) {
        if (client->owner->flags & FL_NOTARGET)
            return false;
    } else
        return false;

    if (!heardit) {
        r = range(self, client);

        if (r == RANGE_FAR)
            return false;

// this is where we would check invisibility

        // is client in an spot too dark to be seen?
        if (client->light_level <= 5)
            return false;

        if (!visible(self, client)) {
            return false;
        }

        if (r == RANGE_NEAR) {
            if (client->show_hostile < level.framenum && !infront(self, client)) {
                return false;
            }
        } else if (r == RANGE_MID) {
            if (!infront(self, client)) {
                return false;
            }
        }

        self->enemy = client;

        if (strcmp(self->enemy->classname, "player_noise") != 0) {
            self->monsterinfo.aiflags &= ~AI_SOUND_TARGET;

            if (!self->enemy->client) {
                self->enemy = self->enemy->enemy;
                if (!self->enemy->client) {
                    self->enemy = NULL;
                    return false;
                }
            }
        }
    } else { // heardit
        vec3_t  temp;

        if (self->spawnflags & 1) {
            if (!visible(self, client))
                return false;
        } else {
            if (!gi.inPHS(self->s.origin, client->s.origin))
                return false;
        }

        VectorSubtract(client->s.origin, self->s.origin, temp);

        if (VectorLength(temp) > 1000) { // too far to hear
            return false;
        }

        // check area portals - if they are different and not connected then we can't hear it
        if (client->areanum != self->areanum)
            if (!gi.AreasConnected(self->areanum, client->areanum))
                return false;

        self->ideal_yaw = vectoyaw(temp);
        M_ChangeYaw(self);

        // hunt the sound for a bit; hopefully find the real player
        self->monsterinfo.aiflags |= AI_SOUND_TARGET;
        self->enemy = client;
    }

//
// got one
//
    FoundTarget(self);

    if (!(self->monsterinfo.aiflags & AI_SOUND_TARGET) && (self->monsterinfo.sight))
        self->monsterinfo.sight(self, self->enemy);

    return true;
}


//=============================================================================

/*
============
FacingIdeal

============
*/
bool FacingIdeal(edict_t *self)
{
    float   delta;

    delta = anglemod(self->s.angles[YAW] - self->ideal_yaw);
    if (delta > 45 && delta < 315)
        return false;
    return true;
}


/*
=============
M_UpdateBlindFireTarget

[rerelease] Remember where to shoot if the enemy goes out of sight.  id aims at
the last sighting nudged BACKWARDS along the enemy's velocity - a player who has
just broken line of sight is usually still moving the same way, so the point
they were last seen at is already behind them, and aiming there is the shot most
likely to catch them.  Seeing the enemy also clears the accumulated delay.
=============
*/
void M_UpdateBlindFireTarget(edict_t *self)
{
    if (!self->monsterinfo.blindfire || !self->enemy)
        return;

    VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity,
             self->monsterinfo.blind_fire_target);
    self->monsterinfo.blind_fire_delay = 0;
}

//=============================================================================

bool M_CheckAttack(edict_t *self)
{
    vec3_t  spot1, spot2;
    float   chance;
    trace_t tr;

    if (self->enemy->health > 0) {
        // see if any entities are in the way of the shot
        VectorCopy(self->s.origin, spot1);
        spot1[2] += self->viewheight;
        VectorCopy(self->enemy->s.origin, spot2);
        spot2[2] += self->enemy->viewheight;

        tr = gi.trace(spot1, NULL, NULL, spot2, self, CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_SLIME | CONTENTS_LAVA | CONTENTS_WINDOW);

        // do we have a clear shot?
        if (tr.ent != self->enemy) {
            // [rerelease] No clear shot.  A blindfire monster that has lost
            // sight of the enemy - and is not merely blocked by another
            // monster - shoots at where it last saw them instead of standing
            // there.  Everything else just gives up, as it always did.
            if (M_RereleaseGame() && self->monsterinfo.blindfire &&
                tr.ent && !(tr.ent->svflags & SVF_MONSTER) &&
                !visible(self, self->enemy) &&
                self->monsterinfo.blind_fire_delay <= 20 * BASE_FRAMERATE) {
                if (level.framenum < self->monsterinfo.attack_finished)
                    return false;

                // not yet time for the next attempt
                if (level.framenum < self->monsterinfo.trail_framenum +
                                     self->monsterinfo.blind_fire_delay)
                    return false;

                // never blind fire into another monster
                tr = gi.trace(spot1, NULL, NULL, self->monsterinfo.blind_fire_target,
                              self, CONTENTS_MONSTER);
                if (tr.allsolid || tr.startsolid ||
                    (tr.fraction < 1.0f && tr.ent != self->enemy))
                    return false;

                gi.dprintf("BLINDFIRE %s delay=%d%s", self->classname,
                           self->monsterinfo.blind_fire_delay, "\n");
                self->monsterinfo.attack_state = AS_BLIND;
                return true;
            }

            return false;
        }
    }

    // melee attack
    if (enemy_range == RANGE_MELEE) {
        // don't always melee in easy mode
        if (skill->value == 0 && (Q_rand() & 3))
            return false;
        if (self->monsterinfo.melee)
            self->monsterinfo.attack_state = AS_MELEE;
        else
            self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

// missile attack
    if (!self->monsterinfo.attack)
        return false;

    if (level.framenum < self->monsterinfo.attack_finished)
        return false;

    if (enemy_range == RANGE_FAR)
        return false;

    if (M_RereleaseGame()) {
        // The rerelease's own M_CheckAttack chances.  Its monsters open fire far
        // more readily than the original's, and several rerelease behaviours
        // depend on that - the infantry run-and-gun only exists in the band
        // beyond 330 units, which the original chances almost never fire in
        // because the monster closes to melee before it ever rolls an attack.
        //
        // Their range bounds are real distances (20 / 440 / 940), not this
        // tree's RANGE_* enum tags, so measure with realrange().  They also
        // apply no skill multiplier here.
        float d = realrange(self, self->enemy);

        if (d > 940.0f)
            return false;

        if (self->monsterinfo.aiflags & AI_STAND_GROUND)
            chance = 0.7f;
        else if (d <= 20.0f)
            chance = 0.4f;
        else if (d <= 440.0f)
            chance = 0.25f;
        else
            chance = 0.06f;

        if (random() < chance) {
            self->monsterinfo.attack_state = AS_MISSILE;
            // no cooldown - the original imposes up to 2 seconds here, which is
            // most of why its monsters feel so much less aggressive
            self->monsterinfo.attack_finished = level.framenum;
            return true;
        }
    } else {
        if (self->monsterinfo.aiflags & AI_STAND_GROUND) {
            chance = 0.4f;
        } else if (enemy_range == RANGE_MELEE) {
            chance = 0.2f;
        } else if (enemy_range == RANGE_NEAR) {
            chance = 0.1f;
        } else if (enemy_range == RANGE_MID) {
            chance = 0.02f;
        } else {
            return false;
        }

        if (skill->value == 0)
            chance *= 0.5f;
        else if (skill->value >= 2)
            chance *= 2;

        if (random() < chance) {
            self->monsterinfo.attack_state = AS_MISSILE;
            self->monsterinfo.attack_finished = level.framenum + 2 * random() * BASE_FRAMERATE;
            return true;
        }
    }

    if (self->flags & FL_FLY) {
        if (random() < 0.3f)
            self->monsterinfo.attack_state = AS_SLIDING;
        else
            self->monsterinfo.attack_state = AS_STRAIGHT;
    }

    return false;
}


/*
=============
ai_run_melee

Turn and close until within an angle to launch a melee attack
=============
*/
void ai_run_melee(edict_t *self)
{
    // [rerelease] AI_MANUAL_STEERING means the monster is aiming somewhere of
    // its own choosing - blindfire at a remembered position, most often - so
    // the generic "turn to face the enemy" must not overwrite ideal_yaw.
    if (!M_RereleaseGame() || !(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
        self->ideal_yaw = enemy_yaw;
    M_ChangeYaw(self);

    if (FacingIdeal(self)) {
        self->monsterinfo.melee(self);
        self->monsterinfo.attack_state = AS_STRAIGHT;
    }
}


/*
=============
ai_run_missile

Turn in place until within an angle to launch a missile attack
=============
*/
void ai_run_missile(edict_t *self)
{
    // [rerelease] AI_MANUAL_STEERING means the monster is aiming somewhere of
    // its own choosing - blindfire at a remembered position, most often - so
    // the generic "turn to face the enemy" must not overwrite ideal_yaw.
    if (!M_RereleaseGame() || !(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
        self->ideal_yaw = enemy_yaw;
    M_ChangeYaw(self);

    if (FacingIdeal(self)) {
        self->monsterinfo.attack(self);

        // ROGUE - AS_BLIND has to be cleared here too, or the monster never
        // leaves it
        if (self->monsterinfo.attack_state == AS_MISSILE ||
            self->monsterinfo.attack_state == AS_BLIND)
            self->monsterinfo.attack_state = AS_STRAIGHT;
    }
}


/*
=============
ai_run_slide

Strafe sideways, but stay at aproximately the same range
=============
*/
void ai_run_slide(edict_t *self, float distance)
{
    float   ofs;

    // [rerelease] AI_MANUAL_STEERING means the monster is aiming somewhere of
    // its own choosing - blindfire at a remembered position, most often - so
    // the generic "turn to face the enemy" must not overwrite ideal_yaw.
    if (!M_RereleaseGame() || !(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
        self->ideal_yaw = enemy_yaw;
    M_ChangeYaw(self);

    if (self->monsterinfo.lefty)
        ofs = 90;
    else
        ofs = -90;

    if (M_walkmove(self, self->ideal_yaw + ofs, distance))
        return;

    self->monsterinfo.lefty = 1 - self->monsterinfo.lefty;
    M_walkmove(self, self->ideal_yaw - ofs, distance);
}


/*
=============
ai_checkattack

Decides if we're going to attack or do something else
used by ai_run and ai_stand
=============
*/
bool ai_checkattack(edict_t *self, float dist)
{
    vec3_t      temp;
    bool        hesDeadJim;

// this causes monsters to run blindly to the combat point w/o firing
    if (self->goalentity) {
        if (self->monsterinfo.aiflags & AI_COMBAT_POINT)
            return false;

        if (self->monsterinfo.aiflags & AI_SOUND_TARGET) {
            if ((level.framenum - self->enemy->last_sound_framenum) > 5.0f * BASE_FRAMERATE) {
                if (self->goalentity == self->enemy) {
                    if (self->movetarget)
                        self->goalentity = self->movetarget;
                    else
                        self->goalentity = NULL;
                }
                self->monsterinfo.aiflags &= ~AI_SOUND_TARGET;
                if (self->monsterinfo.aiflags & AI_TEMP_STAND_GROUND)
                    self->monsterinfo.aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
            } else {
                self->show_hostile = level.framenum + 1 * BASE_FRAMERATE;
                return false;
            }
        }
    }

    enemy_vis = false;

// see if the enemy is dead
    hesDeadJim = false;
    if ((!self->enemy) || (!self->enemy->inuse)) {
        hesDeadJim = true;
    } else if (self->monsterinfo.aiflags & AI_MEDIC) {
        if (self->enemy->health > 0) {
            hesDeadJim = true;
            self->monsterinfo.aiflags &= ~AI_MEDIC;
        }
    } else {
        if (self->monsterinfo.aiflags & AI_BRUTAL) {
            if (self->enemy->health <= -80)
                hesDeadJim = true;
        } else {
            if (self->enemy->health <= 0)
                hesDeadJim = true;
        }
    }

    if (hesDeadJim) {
        self->enemy = NULL;
        // FIXME: look all around for other targets
        if (self->oldenemy && self->oldenemy->health > 0) {
            self->enemy = self->oldenemy;
            self->oldenemy = NULL;
            HuntTarget(self);
        } else {
            if (self->movetarget) {
                self->goalentity = self->movetarget;
                self->monsterinfo.walk(self);
            } else {
                // we need the pausetime otherwise the stand code
                // will just revert to walking with no target and
                // the monsters will wonder around aimlessly trying
                // to hunt the world entity
                self->monsterinfo.pause_framenum = INT_MAX;
                self->monsterinfo.stand(self);
            }
            return true;
        }
    }

    self->show_hostile = level.framenum + 1 * BASE_FRAMERATE;   // wake up other monsters

// check knowledge of enemy
    enemy_vis = visible(self, self->enemy);
    if (enemy_vis) {
        self->monsterinfo.search_framenum = level.framenum + 5 * BASE_FRAMERATE;
        VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
        M_UpdateBlindFireTarget(self);
    }

// look for other coop players here
//  if (coop && self->monsterinfo.search_framenum < level.framenum)
//  {
//      if (FindTarget (self))
//          return true;
//  }

    enemy_range = range(self, self->enemy);
    VectorSubtract(self->enemy->s.origin, self->s.origin, temp);
    enemy_yaw = vectoyaw(temp);


    // JDC self->ideal_yaw = enemy_yaw;

    if (self->monsterinfo.attack_state == AS_MISSILE) {
        ai_run_missile(self);
        return true;
    }
    if (self->monsterinfo.attack_state == AS_MELEE) {
        ai_run_melee(self);
        return true;
    }

    // ROGUE - shooting blind. The carrier sets this when it cannot see the
    // player and answers it by spawning flyers instead of shooting.
    if (self->monsterinfo.attack_state == AS_BLIND) {
        ai_run_missile(self);
        return true;
    }

    // if enemy is not currently visible, we will never attack
    if (!enemy_vis)
        return false;

    return self->monsterinfo.checkattack(self);
}


/*
=============
ai_run

The monster has an enemy it is trying to kill
=============
*/
void ai_run(edict_t *self, float dist)
{
    vec3_t      v;
    edict_t     *tempgoal;
    edict_t     *save;
    bool        new;
    edict_t     *marker;
    float       d1, d2;
    trace_t     tr;
    vec3_t      v_forward, v_right;
    float       left, center, right;
    vec3_t      left_target, right_target;

    // if we're going to a combat point, just proceed
    if (self->monsterinfo.aiflags & AI_COMBAT_POINT) {
        M_MoveToGoal(self, dist);
        return;
    }

    if (self->monsterinfo.aiflags & AI_SOUND_TARGET) {
        VectorSubtract(self->s.origin, self->enemy->s.origin, v);
        if (VectorLength(v) < 64) {
            self->monsterinfo.aiflags |= (AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
            self->monsterinfo.stand(self);
            return;
        }

        M_MoveToGoal(self, dist);

        if (!FindTarget(self))
            return;
    }

    if (ai_checkattack(self, dist))
        return;

    if (self->monsterinfo.attack_state == AS_SLIDING) {
        ai_run_slide(self, dist);
        return;
    }

    if (enemy_vis) {
//      if (self.aiflags & AI_LOST_SIGHT)
//          dprint("regained sight\n");
        M_MoveToGoal(self, dist);
        self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
        VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
        M_UpdateBlindFireTarget(self);
        self->monsterinfo.trail_framenum = level.framenum;
        return;
    }

    // coop will change to another enemy if visible
    if (coop->value) {
        // FIXME: insane guys get mad with this, which causes crashes!
        if (FindTarget(self))
            return;
    }

    if ((self->monsterinfo.search_framenum) && (level.framenum > (self->monsterinfo.search_framenum + 20 * BASE_FRAMERATE))) {
        M_MoveToGoal(self, dist);
        self->monsterinfo.search_framenum = 0;
//      dprint("search timeout\n");
        return;
    }

    save = self->goalentity;
    tempgoal = G_Spawn();
    self->goalentity = tempgoal;

    new = false;

    if (!(self->monsterinfo.aiflags & AI_LOST_SIGHT)) {
        // just lost sight of the player, decide where to go first
//      dprint("lost sight of player, last seen at "); dprint(vtos(self.last_sighting)); dprint("\n");
        self->monsterinfo.aiflags |= (AI_LOST_SIGHT | AI_PURSUIT_LAST_SEEN);
        self->monsterinfo.aiflags &= ~(AI_PURSUE_NEXT | AI_PURSUE_TEMP);
        new = true;
    }

    if (self->monsterinfo.aiflags & AI_PURSUE_NEXT) {
        self->monsterinfo.aiflags &= ~AI_PURSUE_NEXT;
//      dprint("reached current goal: "); dprint(vtos(self.origin)); dprint(" "); dprint(vtos(self.last_sighting)); dprint(" "); dprint(ftos(vlen(self.origin - self.last_sighting))); dprint("\n");

        // give ourself more time since we got this far
        self->monsterinfo.search_framenum = level.framenum + 5 * BASE_FRAMERATE;

        if (self->monsterinfo.aiflags & AI_PURSUE_TEMP) {
//          dprint("was temp goal; retrying original\n");
            self->monsterinfo.aiflags &= ~AI_PURSUE_TEMP;
            marker = NULL;
            VectorCopy(self->monsterinfo.saved_goal, self->monsterinfo.last_sighting);
            new = true;
        } else if (self->monsterinfo.aiflags & AI_PURSUIT_LAST_SEEN) {
            self->monsterinfo.aiflags &= ~AI_PURSUIT_LAST_SEEN;
            marker = PlayerTrail_PickFirst(self);
        } else {
            marker = PlayerTrail_PickNext(self);
        }

        if (marker) {
            VectorCopy(marker->s.origin, self->monsterinfo.last_sighting);
            self->monsterinfo.trail_framenum = marker->timestamp;
            self->s.angles[YAW] = self->ideal_yaw = marker->s.angles[YAW];
//          dprint("heading is "); dprint(ftos(self.ideal_yaw)); dprint("\n");

//          debug_drawline(self.origin, self.last_sighting, 52);
            new = true;
        }
    }

    VectorSubtract(self->s.origin, self->monsterinfo.last_sighting, v);
    d1 = VectorLength(v);
    if (d1 <= dist) {
        self->monsterinfo.aiflags |= AI_PURSUE_NEXT;
        dist = d1;
    }

    VectorCopy(self->monsterinfo.last_sighting, self->goalentity->s.origin);

    if (new) {
//      gi.dprintf("checking for course correction\n");

        tr = gi.trace(self->s.origin, self->mins, self->maxs, self->monsterinfo.last_sighting, self, MASK_PLAYERSOLID);
        if (tr.fraction < 1) {
            VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
            d1 = VectorLength(v);
            center = tr.fraction;
            d2 = d1 * ((center + 1) / 2);
            self->s.angles[YAW] = self->ideal_yaw = vectoyaw(v);
            AngleVectors(self->s.angles, v_forward, v_right, NULL);

            VectorSet(v, d2, -16, 0);
            G_ProjectSource(self->s.origin, v, v_forward, v_right, left_target);
            tr = gi.trace(self->s.origin, self->mins, self->maxs, left_target, self, MASK_PLAYERSOLID);
            left = tr.fraction;

            VectorSet(v, d2, 16, 0);
            G_ProjectSource(self->s.origin, v, v_forward, v_right, right_target);
            tr = gi.trace(self->s.origin, self->mins, self->maxs, right_target, self, MASK_PLAYERSOLID);
            right = tr.fraction;

            center = (d1 * center) / d2;
            if (left >= center && left > right) {
                if (left < 1) {
                    VectorSet(v, d2 * left * 0.5f, -16, 0);
                    G_ProjectSource(self->s.origin, v, v_forward, v_right, left_target);
//                  gi.dprintf("incomplete path, go part way and adjust again\n");
                }
                VectorCopy(self->monsterinfo.last_sighting, self->monsterinfo.saved_goal);
                self->monsterinfo.aiflags |= AI_PURSUE_TEMP;
                VectorCopy(left_target, self->goalentity->s.origin);
                VectorCopy(left_target, self->monsterinfo.last_sighting);
                VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
                self->s.angles[YAW] = self->ideal_yaw = vectoyaw(v);
//              gi.dprintf("adjusted left\n");
//              debug_drawline(self.origin, self.last_sighting, 152);
            } else if (right >= center && right > left) {
                if (right < 1) {
                    VectorSet(v, d2 * right * 0.5f, 16, 0);
                    G_ProjectSource(self->s.origin, v, v_forward, v_right, right_target);
//                  gi.dprintf("incomplete path, go part way and adjust again\n");
                }
                VectorCopy(self->monsterinfo.last_sighting, self->monsterinfo.saved_goal);
                self->monsterinfo.aiflags |= AI_PURSUE_TEMP;
                VectorCopy(right_target, self->goalentity->s.origin);
                VectorCopy(right_target, self->monsterinfo.last_sighting);
                VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
                self->s.angles[YAW] = self->ideal_yaw = vectoyaw(v);
//              gi.dprintf("adjusted right\n");
//              debug_drawline(self.origin, self.last_sighting, 152);
            }
        }
//      else gi.dprintf("course was fine\n");
    }

    M_MoveToGoal(self, dist);

    G_FreeEdict(tempgoal);

    if (self)
        self->goalentity = save;
}

/*
=============================================================================

THE BLOCKED HOOK - jumping down from ledges and riding plats

SV_NewChaseDir calls monsterinfo.blocked when a monster has run out of step
directions.  A monster that sets it can then jump the gap, or push the button
on a func_plat, instead of milling about at the edge.

Lifted from src/rerelease/rogue/g_rogue_newai.cpp.  Differences here:

  - the AI_PATHING / nav_path branches are dropped; this tree has no nav mesh.
  - the "how deep is the water I would land in" test uses gi.pointcontents at
    roughly waist height instead of the rerelease's M_CatagorizePosition, which
    in this tree only reads an entity's own origin.
  - jump timing is in frame numbers, not gtime_t.

Every jump ANIMATION lives in frames the rerelease appended to its models, so
blocked_checkjump refuses to jump unless M_RereleaseAnims() is on.  Plats are
not gated - riding a plat is animation-free.
=============================================================================
*/

/*
=================
monster_jump_start / monster_jump_finished

One timer serves two jobs: it stops a monster re-jumping every frame, and it
ends a jump that never lands.  monster_jump_finished also tops the monster's
forward speed back up, so a jump that scrapes a wall still clears the gap.
=================
*/
void monster_jump_start(edict_t *self)
{
    self->monsterinfo.jump_framenum = level.framenum + 3 * BASE_FRAMERATE;
}

bool monster_jump_finished(edict_t *self)
{
    vec3_t  forward;
    vec3_t  forward_velocity;

    // if we lost our forward velocity, give us more
    AngleVectors(self->s.angles, forward, NULL, NULL);

    forward_velocity[0] = self->velocity[0] * forward[0];
    forward_velocity[1] = self->velocity[1] * forward[1];
    forward_velocity[2] = self->velocity[2] * forward[2];

    if (VectorLength(forward_velocity) < 150.0f) {
        float z_velocity = self->velocity[2];
        VectorScale(forward, 150.0f, self->velocity);
        self->velocity[2] = z_velocity;
    }

    return self->monsterinfo.jump_framenum < level.framenum;
}

/*
=================
face_wall

Turn to face whatever is directly ahead, so a jump up goes straight at the
ledge rather than off at an angle.
=================
*/
bool face_wall(edict_t *self)
{
    vec3_t  pt;
    vec3_t  forward;
    vec3_t  ang;
    trace_t tr;

    AngleVectors(self->s.angles, forward, NULL, NULL);
    VectorMA(self->s.origin, 64, forward, pt);
    tr = gi.trace(self->s.origin, vec3_origin, vec3_origin, pt, self, MASK_MONSTERSOLID);

    if (tr.fraction < 1 && !tr.allsolid && !tr.startsolid) {
        vectoangles(tr.plane.normal, ang);
        self->ideal_yaw = ang[YAW] + 180;
        if (self->ideal_yaw > 360)
            self->ideal_yaw -= 360;

        M_ChangeYaw(self);
        return true;
    }

    return false;
}

/*
=================
blocked_checkplat

`dist` is how far the monster was trying to walk.  If there is a func_plat
under the monster or one step ahead of it, and it is parked at the wrong end
for where the enemy is, press it.
=================
*/
bool blocked_checkplat(edict_t *self, float dist)
{
    int      playerPosition;
    trace_t  trace;
    vec3_t   pt1, pt2;
    vec3_t   forward;
    edict_t *plat;

    if (!self->enemy)
        return false;

    // check player's relative altitude
    if (self->enemy->absmin[2] >= self->absmax[2])
        playerPosition = 1;
    else if (self->enemy->absmax[2] <= self->absmin[2])
        playerPosition = -1;
    else
        playerPosition = 0;

    // if we're close to the same position, don't bother trying plats.
    if (playerPosition == 0)
        return false;

    plat = NULL;

    // see if we're already standing on a plat.
    if (self->groundentity && self->groundentity != g_edicts) {
        if (self->groundentity->classname && !strncmp(self->groundentity->classname, "func_plat", 8))
            plat = self->groundentity;
    }

    // if we're not, check to see if we'll step onto one with this move
    if (!plat) {
        AngleVectors(self->s.angles, forward, NULL, NULL);
        VectorMA(self->s.origin, dist, forward, pt1);
        VectorCopy(pt1, pt2);
        pt2[2] -= 384;

        trace = gi.trace(pt1, NULL, NULL, pt2, self, MASK_MONSTERSOLID);
        if (trace.fraction < 1 && !trace.allsolid && !trace.startsolid) {
            if (trace.ent && trace.ent->classname && !strncmp(trace.ent->classname, "func_plat", 8))
                plat = trace.ent;
        }
    }

    // if we've found a plat, trigger it.
    if (plat && plat->use) {
        if (playerPosition == 1) {
            if ((self->groundentity == plat && plat->moveinfo.state == STATE_BOTTOM) ||
                (self->groundentity != plat && plat->moveinfo.state == STATE_TOP)) {
                plat->use(plat, self, self);
                return true;
            }
        } else if (playerPosition == -1) {
            if ((self->groundentity == plat && plat->moveinfo.state == STATE_TOP) ||
                (self->groundentity != plat && plat->moveinfo.state == STATE_BOTTOM)) {
                plat->use(plat, self, self);
                return true;
            }
        }
    }

    return false;
}

/*
=================
blocked_checkjump

`dist` is how far the monster was trying to walk.  monsterinfo.drop_height and
monsterinfo.jump_height say how far down or up this monster will accept a jump
for; 0 disables that direction entirely.

Returns JUMP_TURN when the monster still needs to turn to face the jump, in
which case the caller must NOT start a jump animation this frame.  This tree
never returns it - it only comes from the nav-mesh path, which is not ported -
but the callers keep the test so they stay line-for-line with id's.
=================
*/
#define STEPSIZE    18

blocked_jump_result_t blocked_checkjump(edict_t *self, float dist)
{
    int     playerPosition;
    trace_t trace;
    vec3_t  pt1, pt2;
    vec3_t  forward, up;

    // every jump animation lives in frames the rerelease appended to the model
    if (!M_RereleaseAnims())
        return NO_JUMP;

    // can't jump even if we physically can
    if (!self->monsterinfo.can_jump)
        return NO_JUMP;

    // no enemy to path to
    if (!self->enemy)
        return NO_JUMP;

    // we just jumped recently, don't try again
    if (self->monsterinfo.jump_framenum > level.framenum)
        return NO_JUMP;

    AngleVectors(self->s.angles, forward, NULL, up);

    if (self->enemy->absmin[2] > (self->absmin[2] + STEPSIZE))
        playerPosition = 1;
    else if (self->enemy->absmin[2] < (self->absmin[2] - STEPSIZE))
        playerPosition = -1;
    else
        playerPosition = 0;

    if (playerPosition == -1 && self->monsterinfo.drop_height) {
        // check to make sure we can even get to the spot we're going to "fall" from
        VectorMA(self->s.origin, 48, forward, pt1);
        trace = gi.trace(self->s.origin, self->mins, self->maxs, pt1, self, MASK_MONSTERSOLID);
        if (trace.fraction < 1)
            return NO_JUMP;

        VectorCopy(pt1, pt2);
        pt2[2] = self->absmin[2] - self->monsterinfo.drop_height - 1;

        trace = gi.trace(pt1, NULL, NULL, pt2, self, MASK_MONSTERSOLID | MASK_WATER);
        if (trace.fraction < 1 && !trace.allsolid && !trace.startsolid) {
            // check how deep the water is - never jump into something we would
            // have to swim out of
            if (trace.contents & CONTENTS_WATER) {
                trace_t deep;
                vec3_t  waist;

                deep = gi.trace(trace.endpos, NULL, NULL, pt2, self, MASK_MONSTERSOLID);
                VectorCopy(deep.endpos, waist);
                waist[2] += 24;
                if (gi.pointcontents(waist) & MASK_WATER)
                    return NO_JUMP;
            }

            if ((self->absmin[2] - trace.endpos[2]) >= 24 && (trace.contents & (MASK_SOLID | CONTENTS_WATER))) {
                // don't drop way past the enemy, and don't drop onto a slope
                if ((self->enemy->absmin[2] - trace.endpos[2]) > 32)
                    return NO_JUMP;
                if (trace.plane.normal[2] < 0.9f)
                    return NO_JUMP;

                monster_jump_start(self);
                return JUMP_JUMP_DOWN;
            }
        }
    } else if (playerPosition == 1 && self->monsterinfo.jump_height) {
        VectorMA(self->s.origin, 48, forward, pt1);
        VectorCopy(pt1, pt2);
        pt1[2] = self->absmax[2] + self->monsterinfo.jump_height;

        trace = gi.trace(pt1, NULL, NULL, pt2, self, MASK_MONSTERSOLID | MASK_WATER);
        if (trace.fraction < 1 && !trace.allsolid && !trace.startsolid) {
            if ((trace.endpos[2] - self->absmin[2]) <= self->monsterinfo.jump_height &&
                (trace.contents & (MASK_SOLID | CONTENTS_WATER))) {
                face_wall(self);
                monster_jump_start(self);
                return JUMP_JUMP_UP;
            }
        }
    }

    return NO_JUMP;
}
