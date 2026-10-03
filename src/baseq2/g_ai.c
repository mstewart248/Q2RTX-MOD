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

// ROGUE - the most a walking monster may strafe in one think. The rerelease
// clamps each 40 Hz tick to MAX_SIDESTEP / (gi.frame_time_ms / 10), and both of
// those are integers there: 8 / (25 / 10) = 8 / 2 = 4 units a tick, 16 over the
// four ticks of one of our frames.
#define MAX_SIDESTEP_FRAME  16.0f

//============================================================================

/*
=============
M_DistanceBetweenBoxes / M_BoxesIntersect / M_ClosestPointToBox

[rerelease] q_vec3.h's distance_between_boxes, boxes_intersect and
closest_point_to_box. The rerelease measures nearly every AI distance as the
gap between two bounding boxes.
=============
*/
float M_DistanceBetweenBoxes(const vec3_t amin, const vec3_t amax, const vec3_t bmin, const vec3_t bmax)
{
    float   len = 0, d;
    int     i;

    for (i = 0; i < 3; i++) {
        if (amax[i] < bmin[i]) {
            d = amax[i] - bmin[i];
            len += d * d;
        } else if (amin[i] > bmax[i]) {
            d = amin[i] - bmax[i];
            len += d * d;
        }
    }

    return sqrtf(len);
}

bool M_BoxesIntersect(const vec3_t amin, const vec3_t amax, const vec3_t bmin, const vec3_t bmax)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (amin[i] > bmax[i] || amax[i] < bmin[i])
            return false;
    }

    return true;
}

void M_ClosestPointToBox(const vec3_t p, const vec3_t bmin, const vec3_t bmax, vec3_t out)
{
    int i;

    for (i = 0; i < 3; i++)
        out[i] = (p[i] < bmin[i]) ? bmin[i] : (p[i] > bmax[i]) ? bmax[i] : p[i];
}

/*
=============
M_ClientInvisible

[rerelease] A player under the Cloak. The rerelease also has a short window
after activating it, and after firing, where a monster can still see you
(invisibility_fade_time) - that is set in the item and weapon code and is not
ported, so this is the "fade time already passed" case: not there at all.
=============
*/
static bool M_ClientInvisible(edict_t *ent)
{
    return ent && ent->client && ent->client->invisible_framenum > level.framenum;
}

// ROGUE tesla_mine - the rerelease never circle strafes one
static bool M_EnemyIsTesla(edict_t *self)
{
    return self->enemy && self->enemy->classname && !strcmp(self->enemy->classname, "tesla_mine");
}

/*
=================
AI_GetSightClient

[rerelease] For a given monster, check every active player to see who it can
see, and pick one of them at random. A player whose box touches ours is always
seen. Replaces the single level.sight_client that AI_SetSightClient cycles
through, which made coop monsters slow to notice anyone.
=================
*/
edict_t *AI_GetSightClient(edict_t *self)
{
    edict_t *visible_players[MAX_CLIENTS];
    int     num_visible = 0;
    int     i;

    if (level.intermission_framenum)
        return NULL;

    for (i = 1; i <= game.maxclients && num_visible < MAX_CLIENTS; i++) {
        edict_t *player = &g_edicts[i];

        if (!player->inuse || !player->client)
            continue;
        if (player->health <= 0 || player->deadflag || !player->solid)
            continue;
        if (player->flags & (FL_NOTARGET | FL_DISGUISED))
            continue;

        // if we're touching them, allow to pass through
        if (!M_BoxesIntersect(self->absmin, self->absmax, player->absmin, player->absmax)) {
            if ((!(self->monsterinfo.aiflags2 & AI2_THIRD_EYE) && !infront(self, player)) ||
                !visible(self, player))
                continue;
        }

        visible_players[num_visible++] = player;
    }

    if (!num_visible)
        return NULL;

    return visible_players[Q_rand() % num_visible];
}

/*
=================
AI_GetMonsterAlertedByPlayers

[rerelease] A monster that found a player is remembered on that player for a
frame (FoundTarget). Other monsters that can see IT get mad too, even when they
cannot see the player - which is how a whole room wakes up around a corner.
=================
*/
static edict_t *AI_GetMonsterAlertedByPlayers(edict_t *self)
{
    int i;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *player = &g_edicts[i];
        edict_t *alerted;

        if (!player->inuse || !player->client)
            continue;
        if (player->health <= 0 || player->deadflag || !player->solid)
            continue;

        // we didn't alert any other monster, or it wasn't recently. The upper
        // bound is ours: client structs outlive a level, so a stale frame
        // number from the last map must not match.
        alerted = player->client->sight_entity;
        if (!alerted || !alerted->inuse ||
            player->client->sight_entity_framenum < level.framenum - 1 ||
            player->client->sight_entity_framenum > level.framenum)
            continue;

        // if we can't see the monster, don't bother
        if (!visible(self, alerted))
            continue;

        return alerted;
    }

    return NULL;
}

/*
=================
AI_GetSoundClient

[rerelease] Per-player sounds: the closest noise any player made this frame or
last. `direct` is the player's own noise (jumping, firing - mynoise); the other
is where their shots landed (mynoise2). PlayerNoise stamps last_sound_framenum
on each noise entity, so this needs nothing new from the player code.
=================
*/
static edict_t *AI_GetSoundClient(edict_t *self, bool direct)
{
    edict_t *best_sound = NULL;
    float   best_distance = 0;
    int     i;

    for (i = 1; i <= game.maxclients; i++) {
        edict_t *player = &g_edicts[i];
        edict_t *sound;
        vec3_t  d;
        float   dist;

        if (!player->inuse || !player->client)
            continue;
        if (player->health <= 0 || player->deadflag || !player->solid)
            continue;

        sound = direct ? player->mynoise : player->mynoise2;
        if (!sound || !sound->inuse)
            continue;

        // too late
        if (sound->last_sound_framenum < level.framenum - 1 ||
            sound->last_sound_framenum > level.framenum)
            continue;

        // prefer the closest one we heard
        VectorSubtract(self->s.origin, sound->s.origin, d);
        dist = VectorLength(d);

        if (!best_sound || dist < best_distance) {
            best_distance = dist;
            best_sound = sound;
        }
    }

    return best_sound;
}

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
ai_stand_rerelease

[rerelease] ai_stand from g_ai.cpp, after the animation step. A stand-ground
monster pushed off its hold point_combat walks back to it; one that loses
sight of its enemy goes looking for another target; and a monster that was
somehow given an enemy without ever being told to hunt it starts hunting.
=============
*/
static void ai_stand_rerelease(edict_t *self)
{
    vec3_t  v;
    bool    retval;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND) {
        // [Paril-KEX] check if we've been pushed out of our point_combat
        if (self->movetarget &&
            !M_BoxesIntersect(self->absmin, self->absmax, self->movetarget->absmin, self->movetarget->absmax)) {
            self->monsterinfo.aiflags &= ~AI_STAND_GROUND;
            self->monsterinfo.aiflags |= AI_COMBAT_POINT;
            self->goalentity = self->movetarget;
            self->monsterinfo.run(self);
            return;
        }

        if (self->enemy) {
            VectorSubtract(self->enemy->s.origin, self->s.origin, v);
            self->ideal_yaw = vectoyaw(v);
            if (!FacingIdeal(self) && (self->monsterinfo.aiflags & AI_TEMP_STAND_GROUND)) {
                self->monsterinfo.aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
                self->monsterinfo.run(self);
            }
            if (!(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
                M_ChangeYaw(self);

            // find out if we're going to be shooting
            retval = ai_checkattack(self, 0);

            // record sightings of player
            if (self->enemy && self->enemy->inuse) {
                if (visible(self, self->enemy)) {
                    self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
                    VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
                    VectorCopy(self->enemy->s.origin, self->monsterinfo.saved_goal);
                    VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity,
                             self->monsterinfo.blind_fire_target);
                    self->monsterinfo.trail_framenum = level.framenum;
                    self->monsterinfo.blind_fire_delay = 0;
                } else {
                    if (FindTarget(self))
                        return;

                    self->monsterinfo.aiflags |= AI_LOST_SIGHT;
                }

                // Paril: fixes rare cases of a stand ground monster being stuck
                // aiming at a sound target that they can still see
                if ((self->monsterinfo.aiflags & AI_SOUND_TARGET) && !retval) {
                    if (FindTarget(self))
                        return;
                }
            } else if (!retval) {
                // check retval to make sure we're not blindfiring
                FindTarget(self);
                return;
            }
        } else {
            FindTarget(self);
        }
        return;
    }

    // Paril: this fixes a bug somewhere else that sometimes causes
    // a monster to be given an enemy without ever calling HuntTarget.
    if (self->enemy && !(self->monsterinfo.aiflags & AI_SOUND_TARGET)) {
        HuntTarget(self);
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
ai_stand

Used for standing around and looking for players
Distance is for slight position adjustments needed by the animations
==============
*/
void ai_stand(edict_t *self, float dist)
{
    vec3_t  v;

    // [rerelease] AI_ALTERNATE_FLY drives velocity in SV_alternate_flystep
    // rather than stepping the origin, so it has to run even on a frame
    // whose dist is 0 - otherwise a hovering flyer freezes in mid-air.
    if (dist || (self->monsterinfo.aiflags & AI_ALTERNATE_FLY))
        M_walkmove(self, self->s.angles[YAW], dist);

    if (M_RereleaseGame()) {
        ai_stand_rerelease(self);
        return;
    }

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
    edict_t *temp_goal = NULL;

    // [rerelease] a good guy with nowhere to go walks straight ahead
    if (M_RereleaseGame() && !self->goalentity && (self->monsterinfo.aiflags & AI_GOOD_GUY)) {
        vec3_t fwd;

        AngleVectors(self->s.angles, fwd, NULL, NULL);
        temp_goal = G_Spawn();
        VectorMA(self->s.origin, 64, fwd, temp_goal->s.origin);
        self->goalentity = temp_goal;
    }

    M_MoveToGoal(self, dist);

    if (temp_goal) {
        G_FreeEdict(temp_goal);
        self->goalentity = NULL;
    }

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

    if (M_RereleaseGame()) {
        float ofs;

        // This is put in there so monsters won't move towards the origin
        // after killing a tesla.
        if (!self->enemy || !self->enemy->inuse)
            return;

        // PMM - save blindfire target
        if (visible(self, self->enemy))
            VectorMA(self->enemy->s.origin, -0.1f, self->enemy->velocity, self->monsterinfo.blind_fire_target);

        // PMM - AI_MANUAL_STEERING monsters turn, but do not set ideal_yaw
        if (!(self->monsterinfo.aiflags & AI_MANUAL_STEERING)) {
            VectorSubtract(self->enemy->s.origin, self->s.origin, v);
            self->ideal_yaw = vectoyaw(v);
        }
        M_ChangeYaw(self);

        if (dist || (self->monsterinfo.aiflags & AI_ALTERNATE_FLY)) {
            if (self->monsterinfo.aiflags & AI_CHARGING) {
                M_MoveToGoal(self, dist);
                return;
            }

            // circle strafe support
            if (self->monsterinfo.attack_state == AS_SLIDING) {
                // if we're fighting a tesla, NEVER circle strafe
                if (M_EnemyIsTesla(self))
                    ofs = 0;
                else if (self->monsterinfo.lefty)
                    ofs = 90;
                else
                    ofs = -90;

                // the current move's sidestep_scale; 0 (the default) for
                // everything but the few run-and-gun moves, so a monster
                // that is merely aiming while AS_SLIDING stays put
                dist *= self->monsterinfo.currentmove ? self->monsterinfo.currentmove->sidestep_scale : 0;

                // id returns on a good strafe, skipping the close-range turn
                if (M_walkmove(self, self->ideal_yaw + ofs, dist))
                    return;

                self->monsterinfo.lefty = !self->monsterinfo.lefty;
                M_walkmove(self, self->ideal_yaw - ofs, dist);
            } else {
                M_walkmove(self, self->s.angles[YAW], dist);
            }
        }

        // [Paril-KEX] if our enemy is literally right next to us, give
        // us more rotational speed so we don't get circled
        if (self->enemy && range_to(self, self->enemy) <= RR_RANGE_MELEE * 2.5f)
            M_ChangeYaw(self);
        return;
    }

    VectorSubtract(self->enemy->s.origin, self->s.origin, v);
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);

    if (dist || (self->monsterinfo.aiflags & AI_ALTERNATE_FLY))
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
    if (dist || (self->monsterinfo.aiflags & AI_ALTERNATE_FLY))
        M_walkmove(self, self->s.angles[YAW], dist);

    if (FindTarget(self))
        return;

    // ROGUE - a manually steered monster is turning itself
    if (M_RereleaseGame() && (self->monsterinfo.aiflags & AI_MANUAL_STEERING))
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
range_to / M_RangeBetween

How far apart two entities are, for the purposes of AI range checks.

The rerelease measures between BOUNDING BOXES (range_to, distance_between_boxes)
and tunes every threshold for that - RANGE_MELEE is 20, "boxes basically
touching". 1997 Quake II measures between ORIGINS with 80/500/1000. Monsters
are meant to behave exactly as in the official game when it is the rerelease
being played, so in rerelease mode everything goes by the box gap against the
rerelease's thresholds (RR_RANGE_*, RR_MELEE_DISTANCE); the classic game keeps
the origin measure and its own numbers. M_RangeBetween and range() both go
through range_to, so every caller follows the mode.

In the classic game a SCALED monster still falls back to the box gap.
mgu6m3's Modir is a monster_shambler at 5.5, which makes its bounding box 176
units wide: a player standing against it is ~192 units from its origin, well
past MELEE_DISTANCE (80), so on the origin measure it could never once reach
melee range.
=============
*/
float range_to(edict_t *self, edict_t *other)
{
    vec3_t  v;
    int     i;

    if (M_RereleaseGame())
        return M_DistanceBetweenBoxes(self->absmin, self->absmax, other->absmin, other->absmax);

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

float M_RangeBetween(edict_t *self, edict_t *other)
{
    return range_to(self, other);
}

int range(edict_t *self, edict_t *other)
{
    float   len;

    len = range_to(self, other);

    // [rerelease] the same categories on the rerelease's box-gap thresholds
    if (M_RereleaseGame()) {
        if (len <= RR_RANGE_MELEE)
            return RANGE_MELEE;
        if (len <= RR_RANGE_NEAR)
            return RANGE_NEAR;
        if (len <= RR_RANGE_MID)
            return RANGE_MID;
        return RANGE_FAR;
    }

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

[rerelease] visible(self, other, through_glass): FL_NOVISIBLE is never seen, a
cloaked player is not seen, nor a non-solid one (intermission), an N64
HACKFLAG_ATTACK_PLAYER monster always sees the player, windows block the view
unless through_glass, and a trace that stops ON the target still counts.
visible() is the rerelease's default, through_glass = true.
=============
*/
bool visible(edict_t *self, edict_t *other)
{
    return visible_ex(self, other, true);
}

bool visible_ex(edict_t *self, edict_t *other, bool through_glass)
{
    vec3_t  spot1;
    vec3_t  spot2;
    trace_t trace;

    if (M_RereleaseGame()) {
        int mask = MASK_OPAQUE;

        // never visible
        if (other->flags & FL_NOVISIBLE)
            return false;

        if (other->client) {
            // always visible in rtest
            if (self->hackflags & HACKFLAG_ATTACK_PLAYER)
                return self->inuse;

            // fix intermission
            if (!other->solid)
                return false;

            if (M_ClientInvisible(other))
                return false;
        }

        if (!through_glass)
            mask |= CONTENTS_WINDOW;

        VectorCopy(self->s.origin, spot1);
        spot1[2] += self->viewheight;
        VectorCopy(other->s.origin, spot2);
        spot2[2] += other->viewheight;
        trace = gi.trace(spot1, vec3_origin, vec3_origin, spot2, self, mask);

        return trace.fraction == 1.0f || trace.ent == other;
    }

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

/*
=============
realrange

ROGUE. The rerelease keeps this one an ORIGIN distance (g_rogue_newai.cpp), and
its callers' numbers (the medic commander's 150, the carrier's, ...) are tuned
for that, so in rerelease mode it is the plain origin distance; everything that
wants the box gap uses range_to. Classic keeps M_RangeBetween's measure.
=============
*/
float realrange(edict_t *self, edict_t *other)
{
    vec3_t  dir;

    if (!M_RereleaseGame())
        return M_RangeBetween(self, other);

    VectorSubtract(self->s.origin, other->s.origin, dir);
    return VectorLength(dir);
}

/*
=============
PredictAim

Where to shoot so the shot and the target arrive together. `offset` shaves that
much off the flight time, which the carrier uses (-0.3) to lead a little further
than the straight solution.
=============
*/
/*
=============
PredictAimEx

[rerelease] The rerelease's PredictAim (g_rogue_newai.cpp) takes the shooter:
if the straight line to the chosen point (eyes or origin) is blocked, it aims at
the other one, and a lead that would put the shot into a wall less than 90% of
the way out is dropped for a straight shot at the target. The blocked-line
test needs `self` to trace from, so it only runs when one is given - PredictAim
passes NULL and gets just the wall check.
=============
*/
void PredictAimEx(edict_t *self, edict_t *target, vec3_t start, float bolt_speed, bool eye_height,
                  float offset, vec3_t aimdir, vec3_t aimpoint)
{
    vec3_t  dir, vec, ndir, nvec, end;
    float   dist, time;
    trace_t tr;

    if (!M_RereleaseGame()) {
        PredictAim(target, start, bolt_speed, eye_height, offset, aimdir, aimpoint);
        return;
    }

    if (!target || !target->inuse) {
        if (aimdir)
            VectorClear(aimdir);
        return;
    }

    VectorSubtract(target->s.origin, start, dir);
    if (eye_height)
        dir[2] += target->viewheight;
    dist = VectorLength(dir);

    // [Paril-KEX] if our current attempt is blocked, try the opposite one
    if (self) {
        VectorAdd(start, dir, end);
        tr = gi.trace(start, NULL, NULL, end, self, MASK_PROJECTILE);

        if (tr.ent != target) {
            eye_height = !eye_height;
            VectorSubtract(target->s.origin, start, dir);
            if (eye_height)
                dir[2] += target->viewheight;
            dist = VectorLength(dir);
        }
    }

    // a hitscan attack passes bolt_speed 0 - it has no travel time to lead
    if (bolt_speed)
        time = dist / bolt_speed;
    else
        time = 0;

    VectorMA(target->s.origin, time - offset, target->velocity, vec);

    // went backwards...
    VectorCopy(dir, ndir);
    VectorNormalize(ndir);
    VectorSubtract(vec, start, nvec);
    VectorNormalize(nvec);
    if (DotProduct(ndir, nvec) < 0) {
        VectorCopy(target->s.origin, vec);
    } else {
        // if the shot is going to impact a nearby wall from our prediction,
        // just fire it straight.
        tr = gi.trace(start, NULL, NULL, vec, NULL, MASK_SOLID);
        if (tr.fraction < 0.9f)
            VectorCopy(target->s.origin, vec);
    }

    if (eye_height)
        vec[2] += target->viewheight;

    if (aimdir) {
        VectorSubtract(vec, start, aimdir);
        VectorNormalize(aimdir);
    }

    if (aimpoint)
        VectorCopy(vec, aimpoint);
}

void PredictAim(edict_t *target, vec3_t start, float bolt_speed, bool eye_height,
                float offset, vec3_t aimdir, vec3_t aimpoint)
{
    vec3_t  dir, vec, ndir, nvec;
    float   dist, time;

    if (M_RereleaseGame()) {
        PredictAimEx(NULL, target, start, bolt_speed, eye_height, offset, aimdir, aimpoint);
        return;
    }

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

    // [rerelease] a much wider cone - everything but the 145 degrees behind
    if (M_RereleaseGame())
        return dot > -0.30f;

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
    // wait a while before first attack. [rerelease] not any more: FoundTarget
    // gives the first-sighting grace period instead.
    if (!M_RereleaseGame() && !(self->monsterinfo.aiflags & AI_STAND_GROUND))
        AttackFinished(self, 1);
}

/*
=============
FoundTarget_rerelease

[rerelease] FoundTarget from g_ai.cpp. The monster is remembered on the player
it found (AI_GetMonsterAlertedByPlayers) and the PLAYER is marked hostile, the
first sighting ever costs 600 ms before the first shot (plus 400/200 ms on
easy/normal), a monster on its way to a combat point keeps going, and combat
points are no longer used up.
=============
*/
static void FoundTarget_rerelease(edict_t *self)
{
    if (self->enemy->client) {
        // ROGUE - being found blows a trigger_disguise
        self->enemy->flags &= ~FL_DISGUISED;

        self->enemy->client->sight_entity = self;
        self->enemy->client->sight_entity_framenum = level.framenum;

        self->enemy->show_hostile = level.framenum + 1 * BASE_FRAMERATE;   // wake up other monsters
    }

    // [Paril-KEX] the first time we spot something, give us a bit of a grace
    // period on firing
    if (!self->monsterinfo.trail_framenum)
        self->monsterinfo.attack_finished = level.framenum + (int)(0.6f * BASE_FRAMERATE);

    // give easy/medium a little more reaction time
    if (skill->value == 0)
        self->monsterinfo.attack_finished += (int)(0.4f * BASE_FRAMERATE);
    else if (skill->value == 1)
        self->monsterinfo.attack_finished += (int)(0.2f * BASE_FRAMERATE);

    VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
    VectorCopy(self->enemy->s.origin, self->monsterinfo.saved_goal);
    self->monsterinfo.trail_framenum = level.framenum;
    // ROGUE
    VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity, self->monsterinfo.blind_fire_target);
    self->monsterinfo.blind_fire_delay = 0;
    // [Paril-KEX] for alternate fly, pick a new position immediately
    self->monsterinfo.fly_position_time = 0;

    self->monsterinfo.aiflags2 &= ~AI2_THIRD_EYE;

    // Paril: if we're heading to a combat point/path corner, don't
    // hunt the new target yet.
    if (self->monsterinfo.aiflags & AI_COMBAT_POINT)
        return;

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
    // [Paril-KEX] not any more, we can re-use them
    self->monsterinfo.pause_framenum = 0;

    // run for it
    self->monsterinfo.run(self);
}

void FoundTarget(edict_t *self)
{
    if (M_RereleaseGame()) {
        FoundTarget_rerelease(self);
        return;
    }

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
/*
===========
FindTarget_rerelease

[rerelease] FindTarget from g_ai.cpp. A player the monster can see right now
wins over anything it heard, every player is checked (one picked at random
from those in view), a monster that can see another monster that just found a
player wakes up too, and per-player sounds replace the single level-wide one.
Up close a non-ambush monster wakes for a hostile player (one some monster is
already fighting) within 440 units of box gap even without line of sight. The
sight sound only plays the first time - close_sight_tripped - so a monster
that keeps re-finding the same player does not keep shouting.
===========
*/
static bool G_MonsterSourceVisible(edict_t *self, edict_t *client)
{
    // this is where we would check invisibility
    float r = range_to(self, client);

    if (r > RR_RANGE_MID)
        return false;

    // Paril: revised so that monsters can be woken up by players 'seen' and
    // attacked at by other monsters if they are close enough. they don't have
    // to be visible.
    return (r <= RR_RANGE_NEAR && client->show_hostile >= level.framenum && !(self->spawnflags & 1)) ||
           (visible(self, client) &&
            (r <= RR_RANGE_MELEE || (self->monsterinfo.aiflags2 & AI2_THIRD_EYE) || infront(self, client)));
}

static bool FindTarget_rerelease(edict_t *self)
{
    edict_t *client;
    bool    heardit = false;
    bool    ignore_sight_sound = false;

    // N64 cutscene behavior
    if (self->hackflags & HACKFLAG_END_CUTSCENE)
        return false;

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

    // Paril: revised so that monsters will first try to consider the current
    // sight client immediately if they can see it. this fixes them dancing in
    // front of you if you fire every frame.
    client = AI_GetSightClient(self);
    if (client && client == self->enemy)
        return false;

    // check indirect sources
    if (!client) {
        // check monsters that were alerted by players; we can only be alerted
        // if we can see them
        if (!(self->spawnflags & 1) && (client = AI_GetMonsterAlertedByPlayers(self)) != NULL) {
            if (client->enemy == self->enemy || !G_MonsterSourceVisible(self, client))
                client = NULL;
        }

        // (the rerelease checks level.disguise_violator here; trigger_disguise
        // in this tree has no violation tracking, so there is none to check)
        if (!client) {
            if ((client = AI_GetSoundClient(self, true)) != NULL)
                heardit = true;
            else if (!self->enemy && !(self->spawnflags & 1) &&
                     (client = AI_GetSoundClient(self, false)) != NULL)
                heardit = true;
        }
    }

    if (!client)
        return false;   // no clients to get mad at

    // if the entity went away, forget it
    if (!client->inuse)
        return false;

    if (client == self->enemy)
        return true;    // JDC false;

    // ROGUE - hintpath coop fix
    if ((self->monsterinfo.aiflags & AI_HINT_PATH) && coop->value)
        heardit = false;

    if (client->svflags & SVF_MONSTER) {
        if (!client->enemy)
            return false;
        if (client->enemy->flags & FL_NOTARGET)
            return false;
    } else if (heardit) {
        // pgm - a little more paranoia won't hurt....
        if (client->owner && (client->owner->flags & FL_NOTARGET))
            return false;
    } else if (!client->client) {
        return false;
    }

    if (!heardit) {
        if (!G_MonsterSourceVisible(self, client))
            return false;

        self->enemy = client;

        if (strcmp(self->enemy->classname, "player_noise") != 0) {
            self->monsterinfo.aiflags &= ~AI_SOUND_TARGET;

            if (!self->enemy->client) {
                self->enemy = self->enemy->enemy;
                if (!self->enemy || !self->enemy->client) {
                    self->enemy = NULL;
                    return false;
                }
            }
        }

        if (M_ClientInvisible(self->enemy)) {
            self->enemy = NULL;
            return false;
        }

        if (self->monsterinfo.close_sight_tripped)
            ignore_sight_sound = true;
        else
            self->monsterinfo.close_sight_tripped = true;
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

        if (VectorLength(temp) > 1000) // too far to hear
            return false;

        // check area portals - if they are different and not connected then we can't hear it
        if (client->areanum != self->areanum)
            if (!gi.AreasConnected(self->areanum, client->areanum))
                return false;

        self->ideal_yaw = vectoyaw(temp);
        if (!(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
            M_ChangeYaw(self);

        // hunt the sound for a bit; hopefully find the real player
        self->monsterinfo.aiflags |= AI_SOUND_TARGET;
        self->enemy = client;
    }

    //
    // got one
    //
    // ROGUE - if we got an enemy, we need to bail out of hint paths, so take over here
    if (self->monsterinfo.aiflags & AI_HINT_PATH)
        hintpath_stop(self);    // this calls foundtarget for us
    else
        FoundTarget(self);

    if (!(self->monsterinfo.aiflags & AI_SOUND_TARGET) && (self->monsterinfo.sight) &&
        // Paril: adjust to prevent monsters getting stuck in sight loops
        !ignore_sight_sound)
        self->monsterinfo.sight(self, self->enemy);

    return true;
}

bool FindTarget(edict_t *self)
{
    edict_t     *client;
    bool        heardit;
    int         r;

    if (M_RereleaseGame())
        return FindTarget_rerelease(self);

    // [rerelease] N64 cutscene behaviour: q64/command's closing procession
    // (HACKFLAG_END_CUTSCENE) marches its path and ignores the player
    if (self->hackflags & HACKFLAG_END_CUTSCENE)
        return false;

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

    // ROGUE - hintpath coop fix. In coop a monster on a hint path would be
    // yanked off it by every noise any player made, and never get anywhere.
    if ((self->monsterinfo.aiflags & AI_HINT_PATH) && coop->value)
        heardit = false;

    if (client->client) {
        if (client->flags & FL_NOTARGET)
            return false;
        // ROGUE cloak - you simply are not there as far as a monster is
        // concerned. The rerelease has a short fade window after you activate
        // it or fire; that is not ported, so this is straight on/off.
        if (client->client->invisible_framenum > level.framenum)
            return false;
        // ROGUE trigger_disguise - same idea, but set by a map trigger rather
        // than an item, and it lasts until another trigger clears it.
        if (client->flags & FL_DISGUISED)
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
    // ROGUE - if we got an enemy while walking a hint path we have to bail
    // out of it first; hintpath_stop calls FoundTarget for us.
    if (self->monsterinfo.aiflags & AI_HINT_PATH)
        hintpath_stop(self);
    else
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

    // [rerelease] a monster walking a navmesh route lines up properly before
    // it steps - 45 degrees off is enough to walk it into the door frame
    if (self->monsterinfo.aiflags & AI_PATHING)
        return !(delta > 5 && delta < 355);

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
    // [Paril-KEX] for alternate fly, pick a new hover position immediately
    self->monsterinfo.fly_position_time = 0;
}

//=============================================================================

/*
=============
M_CheckAttack_rerelease

[rerelease] M_CheckAttack from g_ai.cpp. Distances are box gaps (range_to)
against 20/440/940, there is no skill multiplier and no cooldown on a
successful roll. A cloaked or FL_NOVISIBLE enemy is never attacked; a player
in the way counts as a clear shot; blindfire needs the enemy to have been seen
at least once; an info_notnull target (non-client, SOLID_NOT) is always shot
at. Flyers pick strafe-or-straight and hold the choice for 1-3 seconds; every
other monster goes back to AS_STRAIGHT unless it is walking a navmesh route.
=============
*/
static bool M_CheckAttack_rerelease(edict_t *self)
{
    vec3_t  spot1, spot2;
    float   chance, enemy_dist;
    trace_t tr;

    if (self->enemy->flags & FL_NOVISIBLE)
        return false;

    if (self->enemy->health > 0) {
        // can't see us at all
        if (M_ClientInvisible(self->enemy))
            return false;

        VectorCopy(self->s.origin, spot1);
        spot1[2] += self->viewheight;

        // see if any entities are in the way of the shot
        if (!self->enemy->client || self->enemy->solid) {
            VectorCopy(self->enemy->s.origin, spot2);
            spot2[2] += self->enemy->viewheight;

            tr = gi.trace(spot1, NULL, NULL, spot2, self,
                          MASK_SOLID | CONTENTS_MONSTER | CONTENTS_SLIME | CONTENTS_LAVA);
        } else {
            memset(&tr, 0, sizeof(tr));
            tr.ent = world;
            tr.fraction = 0;
        }

        // do we have a clear shot? (players are CONTENTS_MONSTER here, so a
        // client in the way stands in for the rerelease's SVF_PLAYER test)
        if (!(self->hackflags & HACKFLAG_ATTACK_PLAYER) && tr.ent != self->enemy &&
            !(tr.ent && tr.ent->client)) {
            // ROGUE - we want them to go ahead and shoot at info_notnulls if they can.
            if (self->enemy->solid != SOLID_NOT || tr.fraction < 1.0f) {
                // PMM - if we can't see our target, and we're not blocked by a
                // monster, go into blind fire if available
                // Paril - *and* we have at least seen them once
                if (!(tr.ent && (tr.ent->svflags & SVF_MONSTER)) && !visible(self, self->enemy) &&
                    self->monsterinfo.had_visibility) {
                    if (self->monsterinfo.blindfire &&
                        self->monsterinfo.blind_fire_delay <= 20 * BASE_FRAMERATE) {
                        if (level.framenum < self->monsterinfo.attack_finished)
                            return false;

                        // wait for our time
                        if (level.framenum < self->monsterinfo.trail_framenum +
                                             self->monsterinfo.blind_fire_delay)
                            return false;

                        // make sure we're not going to shoot a monster
                        tr = gi.trace(spot1, NULL, NULL, self->monsterinfo.blind_fire_target,
                                      self, CONTENTS_MONSTER);
                        if (tr.allsolid || tr.startsolid ||
                            (tr.fraction < 1.0f && tr.ent != self->enemy))
                            return false;

                        self->monsterinfo.attack_state = AS_BLIND;
                        return true;
                    }
                }
                return false;
            }
        }
    }

    enemy_dist = range_to(self, self->enemy);

    // melee attack
    if (enemy_dist <= RR_RANGE_MELEE) {
        // a monster that just whiffed a swing is locked out of melee until
        // melee_debounce_framenum and shoots instead
        if (self->monsterinfo.melee &&
            self->monsterinfo.melee_debounce_framenum <= level.framenum)
            self->monsterinfo.attack_state = AS_MELEE;
        else
            self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    // if we were in melee just before this but we're too far away, get out of
    // melee state now
    if (self->monsterinfo.attack_state == AS_MELEE &&
        self->monsterinfo.melee_debounce_framenum > level.framenum)
        self->monsterinfo.attack_state = AS_MISSILE;

    // missile attack
    if (!self->monsterinfo.attack) {
        // ROGUE - fix for melee only monsters & strafing
        self->monsterinfo.attack_state = AS_STRAIGHT;
        return false;
    }

    if (level.framenum < self->monsterinfo.attack_finished)
        return false;

    if (enemy_dist > RR_RANGE_MID)
        return false;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        chance = 0.7f;
    else if (enemy_dist <= RR_RANGE_MELEE)
        chance = 0.4f;
    else if (enemy_dist <= RR_RANGE_NEAR)
        chance = 0.25f;
    else
        chance = 0.06f;

    // PGM - go ahead and shoot every time if it's a info_notnull
    if (random() < chance || (!self->enemy->client && self->enemy->solid == SOLID_NOT)) {
        self->monsterinfo.attack_state = AS_MISSILE;
        self->monsterinfo.attack_finished = level.framenum;
        return true;
    }

    // ROGUE - daedalus should strafe more
    if (self->flags & FL_FLY) {
        if (self->monsterinfo.strafe_check_framenum <= level.framenum) {
            // originally, just 0.3
            float   strafe_chance;
            int     new_state = AS_STRAIGHT;

            if (!strcmp(self->classname, "monster_daedalus"))
                strafe_chance = 0.8f;
            else
                strafe_chance = 0.6f;

            // if enemy is tesla, never strafe
            if (M_EnemyIsTesla(self))
                strafe_chance = 0;

            if (random() < strafe_chance)
                new_state = AS_SLIDING;

            if (new_state != self->monsterinfo.attack_state) {
                self->monsterinfo.strafe_check_framenum =
                    level.framenum + (int)((1.0f + 2.0f * random()) * BASE_FRAMERATE);
                self->monsterinfo.attack_state = new_state;
            }
        }
    }
    // do we want the monsters strafing?
    // [Paril-KEX] no, we don't
    // [Paril-KEX] if we're pathing, don't immediately reset us to straight;
    // this allows us to turn to fire and not jerk back and forth.
    else if (!(self->monsterinfo.aiflags & AI_PATHING)) {
        self->monsterinfo.attack_state = AS_STRAIGHT;
    }

    return false;
}

bool M_CheckAttack(edict_t *self)
{
    vec3_t  spot1, spot2;
    float   chance;
    trace_t tr;

    if (M_RereleaseGame())
        return M_CheckAttack_rerelease(self);

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

                self->monsterinfo.attack_state = AS_BLIND;
                return true;
            }

            return false;
        }
    }

    // melee attack
    if (enemy_range == RANGE_MELEE) {
        if (M_RereleaseGame()) {
            // [rerelease] a monster that just whiffed a swing is locked out of
            // melee until melee_debounce_framenum and shoots instead of
            // standing there flailing. The per-monster melee functions stamp
            // it on a miss; without this read the whole mechanism is dead,
            // which is what it was here. No easy-mode roll - the rerelease
            // does not have one.
            if (self->monsterinfo.melee &&
                self->monsterinfo.melee_debounce_framenum <= level.framenum)
                self->monsterinfo.attack_state = AS_MELEE;
            else
                self->monsterinfo.attack_state = AS_MISSILE;
            return true;
        }

        // don't always melee in easy mode
        if (skill->value == 0 && (Q_rand() & 3))
            return false;
        if (self->monsterinfo.melee)
            self->monsterinfo.attack_state = AS_MELEE;
        else
            self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    // [rerelease] we were in melee a moment ago but the enemy has moved out of
    // reach - leave the melee state rather than chasing in it
    if (M_RereleaseGame() &&
        self->monsterinfo.attack_state == AS_MELEE &&
        self->monsterinfo.melee_debounce_framenum > level.framenum)
        self->monsterinfo.attack_state = AS_MISSILE;

// missile attack
    if (!self->monsterinfo.attack) {
        // [rerelease/ROGUE] melee-only monsters must not be left in a state
        // that suppresses strafing
        if (M_RereleaseGame())
            self->monsterinfo.attack_state = AS_STRAIGHT;
        return false;
    }

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
    self->ideal_yaw = enemy_yaw;
    // ROGUE - an AI_MANUAL_STEERING monster is aiming somewhere of its own
    // choosing (blindfire, most often) and does the turning itself
    if (!M_RereleaseGame() || !(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
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
    self->ideal_yaw = enemy_yaw;
    // ROGUE - an AI_MANUAL_STEERING monster is aiming somewhere of its own
    // choosing (blindfire, most often) and does the turning itself
    if (!M_RereleaseGame() || !(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
        M_ChangeYaw(self);

    if (FacingIdeal(self)) {
        // [rerelease] a melee-only monster (the flipper) has no attack at all,
        // and M_CheckAttack can now hand it AS_MISSILE when its melee is
        // debounced out. Calling through a NULL attack would crash.
        if (self->monsterinfo.attack) {
            self->monsterinfo.attack(self);

            // [rerelease] EVERY attack costs 1-2 seconds before the monster
            // may pick another one. This is the pacing that makes a rerelease
            // monster alternate "attack" with a stretch of running at you
            // instead of re-rolling an attack on the next 10hz tick, and
            // several per-monster behaviours read it back - notably the gekk's
            // "keep running" guard, which has no window without this stamp.
            // It deliberately OVERWRITES whatever attack() just set.
            if (M_RereleaseGame())
                self->monsterinfo.attack_finished =
                    level.framenum + (1.0f + random()) * BASE_FRAMERATE;
        }

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

    // ROGUE/rerelease: the slide is also the dodge, and it reports a failed
    // move back to ai_run by dropping to AS_STRAIGHT
    if (M_RereleaseGame()) {
        self->ideal_yaw = enemy_yaw;

        ofs = self->monsterinfo.lefty ? 90 : -90;

        if (!(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
            M_ChangeYaw(self);

        // PMM - clamp maximum sideways move for non flyers to make them look less jerky
        if (!(self->flags & FL_FLY) && distance > MAX_SIDESTEP_FRAME)
            distance = MAX_SIDESTEP_FRAME;

        if (M_walkmove(self, self->ideal_yaw + ofs, distance))
            return;

        // PMM - if we're dodging, give up on it and go straight
        if (self->monsterinfo.aiflags & AI_DODGING) {
            monster_done_dodge(self);
            // by setting as_straight, caller will know to try straight move
            self->monsterinfo.attack_state = AS_STRAIGHT;
            return;
        }

        self->monsterinfo.lefty = !self->monsterinfo.lefty;
        if (M_walkmove(self, self->ideal_yaw - ofs, distance))
            return;

        // PMM - if we're dodging, give up on it and go straight
        if (self->monsterinfo.aiflags & AI_DODGING)
            monster_done_dodge(self);

        // PMM - the move failed, so signal the caller (ai_run) to try going straight
        self->monsterinfo.attack_state = AS_STRAIGHT;
        return;
    }

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
ai_checkattack_rerelease

[rerelease] ai_checkattack from g_ai.cpp. checkattack runs FIRST (throttled to
once per 100 ms - every frame at 10 Hz) whether or not the enemy is in sight,
so a monster can attack while strafing or charging and can blindfire, and the
attack it picks is dispatched the same frame. A monster heading for a combat
point fights anyone within 100 units on the way. A dead enemy also clears the
goal, and an AI_BRUTAL monster only stops at the corpse's gib_health.
=============
*/
static bool ai_checkattack_rerelease(edict_t *self, float dist)
{
    vec3_t  temp;
    bool    hesDeadJim;
    bool    retval;

    if (self->monsterinfo.aiflags & AI_TEMP_STAND_GROUND)
        self->monsterinfo.aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);

    // this causes monsters to run blindly to the combat point w/o firing
    if (self->goalentity) {
        if (self->monsterinfo.aiflags & AI_COMBAT_POINT) {
            if (self->enemy && range_to(self, self->enemy) > 100.f)
                return false;
        }

        if ((self->monsterinfo.aiflags & AI_SOUND_TARGET) && self->enemy) {
            // the noise entity's last_sound_framenum is the rerelease's
            // teleport_time: when it was last made
            if ((level.framenum - self->enemy->last_sound_framenum) > 5 * BASE_FRAMERATE) {
                if (self->goalentity == self->enemy) {
                    if (self->movetarget)
                        self->goalentity = self->movetarget;
                    else
                        self->goalentity = NULL;
                }
                self->monsterinfo.aiflags &= ~AI_SOUND_TARGET;
            } else {
                self->enemy->show_hostile = level.framenum + 1 * BASE_FRAMERATE;
                return false;
            }
        }
    }

    enemy_vis = false;

    // see if the enemy is dead
    hesDeadJim = false;
    if ((!self->enemy) || (!self->enemy->inuse)) {
        hesDeadJim = true;
    } else if (self->monsterinfo.aiflags2 & AI2_FORGET_ENEMY) {
        self->monsterinfo.aiflags2 &= ~AI2_FORGET_ENEMY;
        hesDeadJim = true;
    } else if (self->monsterinfo.aiflags & AI_MEDIC) {
        if (self->enemy->health > 0)
            hesDeadJim = true;
    } else {
        if (self->monsterinfo.aiflags & AI_BRUTAL) {
            if (self->enemy->health <= self->enemy->gib_health)
                hesDeadJim = true;
        } else {
            if (self->enemy->health <= 0)
                hesDeadJim = true;
        }

        // [Paril-KEX] if our enemy was invisible, lose sight now
        if (M_ClientInvisible(self->enemy) && (self->monsterinfo.aiflags & AI_PURSUE_NEXT))
            hesDeadJim = true;
    }

    if (hesDeadJim && !(self->hackflags & HACKFLAG_ATTACK_PLAYER)) {
        self->monsterinfo.aiflags &= ~AI_MEDIC;
        self->enemy = self->goalentity = NULL;
        self->monsterinfo.close_sight_tripped = false;
        // FIXME: look all around for other targets
        if (self->oldenemy && self->oldenemy->health > 0) {
            self->enemy = self->oldenemy;
            self->oldenemy = NULL;
            HuntTarget(self);
        }
        // ROGUE - multiple teslas make monsters lose track of the player.
        else if (self->monsterinfo.last_player_enemy && self->monsterinfo.last_player_enemy->inuse &&
                 self->monsterinfo.last_player_enemy->health > 0) {
            self->enemy = self->monsterinfo.last_player_enemy;
            self->oldenemy = NULL;
            self->monsterinfo.last_player_enemy = NULL;
            HuntTarget(self);
        } else {
            if (self->movetarget && !(self->monsterinfo.aiflags & AI_STAND_GROUND)) {
                self->goalentity = self->movetarget;
                self->monsterinfo.walk(self);
            } else {
                // we need the pausetime otherwise the stand code
                // will just revert to walking with no target and
                // the monsters will wonder around aimlessly trying
                // to hunt the world entity
                self->monsterinfo.pause_framenum = INT_MAX;
                self->monsterinfo.stand(self);

                if (self->monsterinfo.aiflags & AI_TEMP_STAND_GROUND)
                    self->monsterinfo.aiflags &= ~(AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
            }
            return true;
        }
    }

    // HACKFLAG_ATTACK_PLAYER keeps a dead or missing enemy; nothing to check
    if (!self->enemy || !self->enemy->inuse)
        return false;

    // check knowledge of enemy
    enemy_vis = visible(self, self->enemy);
    if (enemy_vis) {
        self->monsterinfo.had_visibility = true;
        self->enemy->show_hostile = level.framenum + 1 * BASE_FRAMERATE;   // wake up other monsters
        self->monsterinfo.search_framenum = level.framenum + 5 * BASE_FRAMERATE;
        VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
        VectorCopy(self->enemy->s.origin, self->monsterinfo.saved_goal);
        if (self->monsterinfo.aiflags & AI_LOST_SIGHT) {
            nav_monster_t *nm = Nav_MonsterState(self);

            self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
            if (nm->move_block_change_framenum < level.framenum)
                nm->temp_melee = false;
        }
        self->monsterinfo.trail_framenum = level.framenum;
        VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity, self->monsterinfo.blind_fire_target);
        self->monsterinfo.blind_fire_delay = 0;
    }

    // monster checkattack functions written against the classic ai_checkattack
    // still read the range category
    enemy_range = range(self, self->enemy);
    VectorSubtract(self->enemy->s.origin, self->s.origin, temp);
    enemy_yaw = vectoyaw(temp);

    // PMM -- reordered so the monster specific checkattack is called before the
    // run_missle/melee/checkvis stuff .. this allows for, among other things,
    // circle strafing and attacking while in ai_run
    retval = false;

    if (self->monsterinfo.checkattack_framenum <= level.framenum) {
        self->monsterinfo.checkattack_framenum = level.framenum + 1;   // 100 ms
        retval = self->monsterinfo.checkattack(self);
    }

    if (retval || self->monsterinfo.attack_state >= AS_MISSILE) {
        if (self->monsterinfo.attack_state == AS_MISSILE) {
            ai_run_missile(self);
            return true;
        }
        if (self->monsterinfo.attack_state == AS_MELEE) {
            ai_run_melee(self);
            return true;
        }
        // PMM -- added so monsters can shoot blind
        if (self->monsterinfo.attack_state == AS_BLIND) {
            ai_run_missile(self);
            return true;
        }

        // if enemy is not currently visible, we will never attack
        if (!enemy_vis)
            return false;
    }

    return retval;
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

    if (M_RereleaseGame())
        return ai_checkattack_rerelease(self, dist);

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
ai_run_rerelease

[rerelease] ai_run from g_ai.cpp. A monster on its way to a combat point
fights on the way and gives up a "hold" post it has been knocked well away from;
a sound target is reached by touching it rather than by an origin distance,
then faced; attacks are checked before moving, so a monster can shoot while it
strafes or charges and, when the attack goes off from AS_STRAIGHT, keeps moving
while it shoots; a dodge always slides, and a strafe without sight of the enemy
stops. The pursuit and navmesh parts are this tree's, as in the classic ai_run.
=============
*/
static void ai_run_rerelease(edict_t *self, float dist)
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
    bool        retval;
    bool        alreadyMoved = false;

    // if we're going to a combat point, just proceed
    if (self->monsterinfo.aiflags & AI_COMBAT_POINT) {
        ai_checkattack(self, dist);
        M_MoveToGoal(self, dist);

        if (!self->inuse)
            return;

        if (self->movetarget) {
            vec3_t  centroid, closest, small_mins = { -2, -2, -2 }, small_maxs = { 2, 2, 2 };

            // nb: this is done from the centroid and not viewheight on purpose;
            VectorAdd(self->absmax, self->absmin, centroid);
            VectorScale(centroid, 0.5f, centroid);
            tr = gi.trace(centroid, small_mins, small_maxs, self->movetarget->s.origin, self, CONTENTS_SOLID);

            // [Paril-KEX] special case: if we're stand ground & knocked way too
            // far away from our path_corner, or we can't see it any more,
            // assume all is lost.
            M_ClosestPointToBox(self->movetarget->s.origin, self->absmin, self->absmax, closest);
            VectorSubtract(closest, self->movetarget->s.origin, v);
            if ((self->monsterinfo.aiflags2 & AI2_REACHED_HOLD_COMBAT) &&
                (VectorLength(v) > 160.f ||
                 (tr.fraction < 1.0f && tr.plane.normal[2] <= 0.7f))) {    // if we hit a climbable, ignore this result
                self->monsterinfo.aiflags &= ~AI_COMBAT_POINT;
                self->movetarget = NULL;
                self->target = NULL;
                self->goalentity = self->enemy;
            } else {
                return;
            }
        } else {
            return;
        }
    }

    // PMM - see the classic ai_run for why a crouch must not outlive the dodge
    if ((self->monsterinfo.aiflags & AI_DUCKED) && self->monsterinfo.unduck)
        self->monsterinfo.unduck(self);

    // ROGUE - if we are currently walking a hint path, that is ALL we do
    if (self->monsterinfo.aiflags & AI_HINT_PATH) {
        edict_t *realEnemy;
        bool     gotcha = false;

        // determine direction to our destination hintpath.
        M_MoveToGoal(self, dist);
        if (!self->inuse)
            return;

        // first off, make sure we're looking for the player, not a noise he made
        if (!self->enemy || !self->enemy->inuse) {
            self->enemy = NULL;
            hintpath_stop(self);
            return;
        }

        if (strcmp(self->enemy->classname, "player_noise") != 0) {
            realEnemy = self->enemy;
        } else if (self->enemy->owner) {
            realEnemy = self->enemy->owner;
        } else {
            // uh oh, can't figure out enemy, bail
            self->enemy = NULL;
            hintpath_stop(self);
            return;
        }

        if (visible(self, realEnemy))
            gotcha = true;
        else if (coop->value)
            // let FindTarget bump us out of hint paths, if appropriate
            FindTarget(self);

        // if we see the player, stop following hintpaths and hunt normally
        if (gotcha)
            hintpath_stop(self);

        return;
    }

    if (self->monsterinfo.aiflags & AI_SOUND_TARGET) {
        // the rerelease tests SV_CloseEnough with dist * (tick_rate / 10) -
        // one 10 Hz frame's worth of movement, which is exactly `dist` here
        bool touching_noise = self->enemy && SV_CloseEnough(self, self->enemy, dist);

        if (!self->enemy || (touching_noise && FacingIdeal(self))) {
            self->monsterinfo.aiflags |= (AI_STAND_GROUND | AI_TEMP_STAND_GROUND);
            self->s.angles[YAW] = self->ideal_yaw;
            self->monsterinfo.stand(self);
            self->monsterinfo.close_sight_tripped = false;
            return;
        }

        // if we're close to the goal, just turn
        if (touching_noise)
            M_ChangeYaw(self);
        else
            M_MoveToGoal(self, dist);

        // ROGUE - prevent double moves for sound_targets
        alreadyMoved = true;

        if (!self->inuse)
            return;     // PGM - g_touchtrigger free problem

        if (!FindTarget(self))
            return;
    }

    // PMM -- moved ai_checkattack up here so the monsters can attack while
    // strafing or charging
    retval = ai_checkattack(self, dist);

    if (!self->inuse)
        return;

    // PMM - don't strafe if we can't see our enemy
    if (!enemy_vis && self->monsterinfo.attack_state == AS_SLIDING)
        self->monsterinfo.attack_state = AS_STRAIGHT;
    // unless we're dodging (dodging out of view looks smart)
    if (self->monsterinfo.aiflags & AI_DODGING)
        self->monsterinfo.attack_state = AS_SLIDING;

    if (self->monsterinfo.attack_state == AS_SLIDING) {
        // PMM - protect against double moves
        if (!alreadyMoved)
            ai_run_slide(self, dist);
        // we're using attack_state as the return value out of ai_run_slide to
        // indicate whether or not the move succeeded. If the move succeeded,
        // and we're still sliding, we're done in here (since we've had our
        // chance to shoot in ai_checkattack, and have moved). if the move
        // failed, our state is as_straight, and it will be taken care of below
        if (!retval && self->monsterinfo.attack_state == AS_SLIDING)
            return;
    } else if (self->monsterinfo.aiflags & AI_CHARGING) {
        self->ideal_yaw = enemy_yaw;
        if (!(self->monsterinfo.aiflags & AI_MANUAL_STEERING))
            M_ChangeYaw(self);
    }

    if (retval) {
        // PMM - is this useful?  Monsters attacking usually call the ai_charge
        // routine.. the only monster this affects should be the soldier
        if ((dist || (self->monsterinfo.aiflags & AI_ALTERNATE_FLY)) && !alreadyMoved &&
            self->monsterinfo.attack_state == AS_STRAIGHT &&
            !(self->monsterinfo.aiflags & AI_STAND_GROUND)) {
            M_MoveToGoal(self, dist);
            if (!self->inuse)
                return;
        }
        if (self->enemy && self->enemy->inuse && enemy_vis) {
            if (self->monsterinfo.aiflags & AI_LOST_SIGHT) {
                nav_monster_t *nm = Nav_MonsterState(self);

                self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
                if (nm->move_block_change_framenum < level.framenum)
                    nm->temp_melee = false;
            }
            VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
            VectorCopy(self->enemy->s.origin, self->monsterinfo.saved_goal);
            self->monsterinfo.trail_framenum = level.framenum;
            VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity, self->monsterinfo.blind_fire_target);
            self->monsterinfo.blind_fire_delay = 0;
        }
        return;
    }

    // PGM - added a little paranoia checking here... 9/22/98
    if (self->enemy && self->enemy->inuse && enemy_vis) {
        // PMM - check for alreadyMoved
        if (!alreadyMoved)
            M_MoveToGoal(self, dist);
        if (!self->inuse)
            return;     // PGM - g_touchtrigger free problem

        if (self->monsterinfo.aiflags & AI_LOST_SIGHT) {
            nav_monster_t *nm = Nav_MonsterState(self);

            self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
            if (nm->move_block_change_framenum < level.framenum)
                nm->temp_melee = false;
        }
        VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
        VectorCopy(self->enemy->s.origin, self->monsterinfo.saved_goal);
        self->monsterinfo.trail_framenum = level.framenum;
        VectorMA(self->monsterinfo.last_sighting, -0.1f, self->enemy->velocity, self->monsterinfo.blind_fire_target);
        self->monsterinfo.blind_fire_delay = 0;

        // [Paril-KEX] if our enemy is literally right next to us, give
        // us more rotational speed so we don't get circled
        if (range_to(self, self->enemy) <= RR_RANGE_MELEE * 2.5f)
            M_ChangeYaw(self);

        return;
    }

    // ROGUE - if we have been looking (unsuccessfully) for the player for
    // five seconds, and have not checked for a hint path in the last ten,
    // go and look for one.
    if (level.framenum >= self->monsterinfo.trail_framenum + 5 * BASE_FRAMERATE &&
        level.framenum >= self->monsterinfo.last_hint_framenum + 10 * BASE_FRAMERATE) {
        self->monsterinfo.last_hint_framenum = level.framenum;
        if (monsterlost_checkhint(self))
            return;
    }

    // PMM - moved down here to allow monsters to get on hint paths
    // coop will change to another enemy if visible
    if (coop->value)
        FindTarget(self);

    if (!self->enemy || !self->enemy->inuse)
        return;

    if ((self->monsterinfo.search_framenum) && (level.framenum > (self->monsterinfo.search_framenum + 20 * BASE_FRAMERATE))) {
        // PMM - double move protection
        if (!alreadyMoved)
            M_MoveToGoal(self, dist);
        self->monsterinfo.search_framenum = 0;
        return;
    }

    save = self->goalentity;
    tempgoal = G_Spawn();
    self->goalentity = tempgoal;

    new = false;

    if (!(self->monsterinfo.aiflags & AI_LOST_SIGHT)) {
        // just lost sight of the player, decide where to go first
        self->monsterinfo.aiflags |= (AI_LOST_SIGHT | AI_PURSUIT_LAST_SEEN);
        self->monsterinfo.aiflags &= ~(AI_PURSUE_NEXT | AI_PURSUE_TEMP);
        new = true;

        // immediately try paths
        Nav_MonsterState(self)->blocked_time = 0;
        Nav_MonsterState(self)->wait_framenum = 0;
    }

    if (self->monsterinfo.aiflags & AI_PURSUE_NEXT) {
        self->monsterinfo.aiflags &= ~AI_PURSUE_NEXT;

        // give ourself more time since we got this far
        self->monsterinfo.search_framenum = level.framenum + 5 * BASE_FRAMERATE;

        if (self->monsterinfo.aiflags & AI_PURSUE_TEMP) {
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
            new = true;
        }
    }

    // [rerelease] reached the pursuit point when it is inside our box, and not
    // while the navmesh is walking us (the breadcrumbs are not our goal then)
    if (!(self->monsterinfo.aiflags & AI_PATHING)) {
        vec3_t mins, maxs;

        VectorAdd(self->s.origin, self->mins, mins);
        VectorAdd(self->s.origin, self->maxs, maxs);
        if (M_BoxesIntersect(self->monsterinfo.last_sighting, self->monsterinfo.last_sighting, mins, maxs)) {
            self->monsterinfo.aiflags |= AI_PURSUE_NEXT;
            VectorSubtract(self->s.origin, self->monsterinfo.last_sighting, v);
            d1 = VectorLength(v);
            if (d1 < dist)
                dist = d1;
            // [Paril-KEX] this helps them navigate corners when two next
            // pursuits are really close together
            self->monsterinfo.random_change_framenum = level.framenum + 1;
        }
    }

    VectorCopy(self->monsterinfo.last_sighting, self->goalentity->s.origin);

    if (new) {
        tr = gi.trace(self->s.origin, self->mins, self->maxs, self->monsterinfo.last_sighting, self, MASK_PLAYERSOLID);
        if (tr.fraction < 1) {
            float backup_yaw = self->s.angles[YAW];

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
                }
                VectorCopy(self->monsterinfo.last_sighting, self->monsterinfo.saved_goal);
                self->monsterinfo.aiflags |= AI_PURSUE_TEMP;
                VectorCopy(left_target, self->goalentity->s.origin);
                VectorCopy(left_target, self->monsterinfo.last_sighting);
                VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
                self->ideal_yaw = vectoyaw(v);
            } else if (right >= center && right > left) {
                if (right < 1) {
                    VectorSet(v, d2 * right * 0.5f, 16, 0);
                    G_ProjectSource(self->s.origin, v, v_forward, v_right, right_target);
                }
                VectorCopy(self->monsterinfo.last_sighting, self->monsterinfo.saved_goal);
                self->monsterinfo.aiflags |= AI_PURSUE_TEMP;
                VectorCopy(right_target, self->goalentity->s.origin);
                VectorCopy(right_target, self->monsterinfo.last_sighting);
                VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
                self->ideal_yaw = vectoyaw(v);
            }
            // the rerelease only re-aims ideal_yaw here; the body turns at its
            // own rate
            self->s.angles[YAW] = backup_yaw;
        }
    }

    M_MoveToGoal(self, dist);

    G_FreeEdict(tempgoal);

    if (!self->inuse)
        return;     // PGM - g_touchtrigger free problem

    self->goalentity = save;
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

    if (M_RereleaseGame()) {
        ai_run_rerelease(self, dist);
        return;
    }

    // if we're going to a combat point, just proceed
    if (self->monsterinfo.aiflags & AI_COMBAT_POINT) {
        M_MoveToGoal(self, dist);
        return;
    }

    // PMM - a crouch that never reached its stand-up frame must not outlive the
    // dodge.  monster_duck_down shrinks maxs[2] by 32 and ONLY the monster_duck_up
    // at the tail of the crouch animation puts it back, so every path that
    // replaces currentmove mid-crouch - ai_run choosing an attack, the enemy
    // being lost and the monster going back to its run, a blocked/plat move, a
    // sidestep that declined - strands the monster at waist height for the rest
    // of the level.  Nothing about it looks wrong: the animation is whatever it
    // moved on to, so it walks, aims and fires normally while every shot at its
    // chest passes clean over the bounding box.  Only the one instance that was
    // interrupted is affected, which is what makes it look like a one-off.
    //
    // The rerelease unducks here, at the top of ai_run, for exactly this reason:
    // the crouch animations all sit on ai_move / ai_charge frames, so a monster
    // reaching ai_run at all has left the crouch behind.  See ai_run in
    // src/rerelease/g_ai.cpp.  This tree had no equivalent.
    if ((self->monsterinfo.aiflags & AI_DUCKED) && self->monsterinfo.unduck)
        self->monsterinfo.unduck(self);

    // ROGUE - if we are currently walking a hint path, that is ALL we do:
    // steer at the next node and watch for the enemy coming back into view.
    // g_rogue.c owns the chain itself; this is just the per-frame half.
    if (self->monsterinfo.aiflags & AI_HINT_PATH) {
        edict_t *realEnemy;
        bool     gotcha = false;

        // determine direction to our destination hintpath.
        M_MoveToGoal(self, dist);
        if (!self->inuse)
            return;

        // first off, make sure we're looking for the player, not a noise he made
        if (!self->enemy || !self->enemy->inuse) {
            self->enemy = NULL;
            hintpath_stop(self);
            return;
        }

        if (strcmp(self->enemy->classname, "player_noise") != 0) {
            realEnemy = self->enemy;
        } else if (self->enemy->owner) {
            realEnemy = self->enemy->owner;
        } else {
            // uh oh, can't figure out enemy, bail
            self->enemy = NULL;
            hintpath_stop(self);
            return;
        }

        if (visible(self, realEnemy))
            gotcha = true;
        else if (coop->value)
            // let FindTarget bump us out of hint paths, if appropriate
            FindTarget(self);

        // if we see the player, stop following hintpaths and hunt normally
        if (gotcha)
            hintpath_stop(self);

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
        if (!self->inuse)
            return;
        if (self->monsterinfo.aiflags & AI_LOST_SIGHT) {
            nav_monster_t *nm = Nav_MonsterState(self);

            self->monsterinfo.aiflags &= ~AI_LOST_SIGHT;
            if (nm->move_block_change_framenum < level.framenum)
                nm->temp_melee = false;
        }
        VectorCopy(self->enemy->s.origin, self->monsterinfo.last_sighting);
        M_UpdateBlindFireTarget(self);
        self->monsterinfo.trail_framenum = level.framenum;
        return;
    }

    // ROGUE - if we have been looking (unsuccessfully) for the player for
    // five seconds, and have not checked for a hint path in the last ten,
    // go and look for one. Both throttles matter: the search walks every
    // hint_path on the map and traces to each of them twice.
    if (level.framenum >= self->monsterinfo.trail_framenum + 5 * BASE_FRAMERATE &&
        level.framenum >= self->monsterinfo.last_hint_framenum + 10 * BASE_FRAMERATE) {
        self->monsterinfo.last_hint_framenum = level.framenum;
        if (monsterlost_checkhint(self))
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

        // [rerelease] immediately try paths
        Nav_MonsterState(self)->blocked_time = 0;
        Nav_MonsterState(self)->wait_framenum = 0;
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
    // [rerelease] While the navmesh is walking us to the enemy (AI_PATHING,
    // left set by M_MoveToGoal on the previous frame) the trail breadcrumbs
    // are not our goal, so arriving at one must not advance the trail.
    // last_sighting is still what M_MoveToGoal gets as the goal for the frame
    // the mesh gives up and the classic pursuit takes over.
    if (d1 <= dist && !(self->monsterinfo.aiflags & AI_PATHING)) {
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

  - the AI_PATHING branch reads the route from g_nav.c (Nav_MonsterState)
    rather than monsterinfo.nav_path.
  - the rerelease's "how deep is the water I would land in" test is not
    ported: it hands M_CatagorizePosition the FLOOR point as if it were an
    origin, so its feet probe lands inside the floor and never reads water -
    in practice the rerelease never refuses a drop for water. This tree used
    to refuse any drop into water deeper than 24 units, which kept monsters on
    the bank while the player swam away below them.
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

    // [rerelease] if we're pathing, the nodes will ensure we can reach the
    // destination: only jump where the route says the next leg IS a jump,
    // facing along it, and trust it for the landing.
    if (self->monsterinfo.aiflags & AI_PATHING) {
        nav_path_t  *path = &Nav_MonsterState(self)->path;
        vec3_t      dir;

        if (path->code != NAV_PATH_TRAVERSAL)
            return NO_JUMP;

        VectorSubtract(path->second_move_point, path->first_move_point, dir);
        self->ideal_yaw = vectoyaw(dir);

        if (!FacingIdeal(self)) {
            M_ChangeYaw(self);
            return JUMP_TURN;
        }

        monster_jump_start(self);

        // the jump is under way; re-query as soon as we are back on the ground
        Nav_MonsterState(self)->cache_framenum = 0;

        if (path->second_move_point[2] > path->first_move_point[2])
            return JUMP_JUMP_UP;
        return JUMP_JUMP_DOWN;
    }

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
            // (no water depth test - see the notes at the top of this section)
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
