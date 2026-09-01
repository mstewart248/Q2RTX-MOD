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
// m_move.c -- monster movement

#include "g_local.h"

#define STEPSIZE    18

/*
=============
M_CheckBottom

Returns false if any part of the bottom of the entity is off an edge that
is not a staircase.

=============
*/
int c_yes, c_no;

bool M_CheckBottom(edict_t *ent)
{
    vec3_t  mins, maxs, start, stop;
    trace_t trace;
    int     x, y;
    float   mid, bottom;

    VectorAdd(ent->s.origin, ent->mins, mins);
    VectorAdd(ent->s.origin, ent->maxs, maxs);

// if all of the points under the corners are solid world, don't bother
// with the tougher checks
// the corners must be within 16 of the midpoint
    // ROGUE - for a ceiling walker the "bottom" is the top of the box
    if (ent->gravityVector[2] > 0)
        start[2] = maxs[2] + 1;
    else
        start[2] = mins[2] - 1;

    for (x = 0 ; x <= 1 ; x++)
        for (y = 0 ; y <= 1 ; y++) {
            start[0] = x ? maxs[0] : mins[0];
            start[1] = y ? maxs[1] : mins[1];
            if (gi.pointcontents(start) != CONTENTS_SOLID)
                goto realcheck;
        }

    c_yes++;
    return true;        // we got out easy

realcheck:
    c_no++;
//
// check it for real...
//
// the midpoint must be within 16 of the bottom
    start[0] = stop[0] = (mins[0] + maxs[0]) * 0.5f;
    start[1] = stop[1] = (mins[1] + maxs[1]) * 0.5f;

    if (ent->gravityVector[2] < 0) {
        start[2] = mins[2];
        stop[2] = start[2] - 2 * STEPSIZE;
    } else {
        start[2] = maxs[2];
        stop[2] = start[2] + 2 * STEPSIZE;
    }

    trace = gi.trace(start, vec3_origin, vec3_origin, stop, ent, MASK_MONSTERSOLID);

    if (trace.fraction == 1.0f)
        return false;
    mid = bottom = trace.endpos[2];

// the corners must be within 16 of the midpoint
    for (x = 0 ; x <= 1 ; x++)
        for (y = 0 ; y <= 1 ; y++) {
            start[0] = stop[0] = x ? maxs[0] : mins[0];
            start[1] = stop[1] = y ? maxs[1] : mins[1];

            trace = gi.trace(start, vec3_origin, vec3_origin, stop, ent, MASK_MONSTERSOLID);

            if (ent->gravityVector[2] > 0) {
                if (trace.fraction != 1.0f && trace.endpos[2] < bottom)
                    bottom = trace.endpos[2];
                if (trace.fraction == 1.0f || trace.endpos[2] - mid > STEPSIZE)
                    return false;
            } else {
                if (trace.fraction != 1.0f && trace.endpos[2] > bottom)
                    bottom = trace.endpos[2];
                if (trace.fraction == 1.0f || mid - trace.endpos[2] > STEPSIZE)
                    return false;
            }
        }

    c_yes++;
    return true;
}


/*
==============================================================================

THE ALTERNATE FLY SYSTEM  (rerelease SV_alternate_flystep, m_move.cpp:213)

id's flying monsters do not fly - SV_movestep below teleports the bbox to a new
origin each frame and clamps its height against the enemy's, so a flyer tracks
in on rails and a floater bobs on a string.  The rerelease gives them a real
velocity model instead: pick a point to hover at RELATIVE to the enemy, steer
towards it by SLERPING the current heading (so the turn rate is constant no
matter how far off it starts), accelerate up to fly_speed, and slow down on
arrival so it does not overshoot.  That, and nothing else, is why rerelease
flyers circle and bank.

It runs INSTEAD of the classic path, gated on AI_ALTERNATE_FLY, and it writes
ent->velocity rather than ent->s.origin - so SV_Physics_Step must leave that
velocity alone for these monsters, and ai_run/ai_charge/ai_walk must keep
calling it even on a frame whose dist is 0.  Both of those are done.

WHAT IS NOT PORTED, and why:
  - every AI_PATHING / nav_path branch.  Those read a NAV MESH the rerelease's
    engine builds and this one has no equivalent for.  The AI_COMBAT_POINT,
    AI_SOUND_TARGET and AI_LOST_SIGHT cases, which share those code paths, ARE
    kept - they only need a goal position.
  - AI_HINT_PATH, which does not exist in this tree.
  - their isnan() debug-break guards.  Kept as plain early-outs instead.

THE RATE SCALE.  fly_acceleration is a speed change PER CALL, and the rerelease
calls its aifunc every engine tick while this tree runs at 10hz.  Their engine
is 20 or 40hz - p_weapon.cpp:459 gates the quick weapon switch on
`gi.tick_rate == 20 || gi.tick_rate == 40`, and g_monster.cpp:596 divides frame
distances by `tick_rate / 10`, which is what pins the authored values to a 10hz
base.  monster_fly_setup() therefore scales the acceleration on the way in and
the per-monster numbers below stay literally comparable to the rerelease's.
fly_speed needs no scale: a speed is a speed.

==============================================================================
*/

// How far out to hover, and in which direction.  A zero vector means "go right
// for the centre" - used when there is nothing to orbit.
static void G_IdealHoverPosition(edict_t *ent, vec3_t out)
{
    float   theta, phi, dist;

    if ((!ent->enemy && !(ent->monsterinfo.aiflags & AI_MEDIC)) ||
        (ent->monsterinfo.aiflags & (AI_COMBAT_POINT | AI_SOUND_TARGET))) {
        VectorClear(out);
        return;
    }

    // pick a random direction on a sphere; phi decides which band of it
    theta = random() * 2 * M_PI;

    if (ent->monsterinfo.fly_above)
        phi = acosf(0.7f + random() * 0.3f);            // top cap only
    else if (ent->monsterinfo.fly_buzzard || (ent->monsterinfo.aiflags & AI_MEDIC))
        phi = acosf(random());                          // whole upper half
    else
        phi = acosf(crandom() * 0.06f);                 // a level band

    out[0] = sinf(phi) * cosf(theta);
    out[1] = sinf(phi) * sinf(theta);
    out[2] = cosf(phi);

    dist = ent->monsterinfo.fly_min_distance +
           random() * (ent->monsterinfo.fly_max_distance - ent->monsterinfo.fly_min_distance);
    VectorScale(out, dist, out);
}

// Can the monster see `end` from `start`, AND fit through the box sweep from
// `starta` to `startb`?  Used to decide which way to shift around a block.
static bool SV_flystep_testvisposition(vec3_t start, vec3_t end, vec3_t starta, vec3_t startb, edict_t *ent)
{
    trace_t tr;

    tr = gi.trace(start, NULL, NULL, end, ent, MASK_SOLID | CONTENTS_MONSTERCLIP);
    if (tr.fraction != 1.0f)
        return false;

    tr = gi.trace(starta, ent->mins, ent->maxs, startb, ent, MASK_SOLID | CONTENTS_MONSTERCLIP);
    return tr.fraction == 1.0f;
}

static bool SV_alternate_flystep(edict_t *ent, vec3_t move, bool relink)
{
    vec3_t      towards_origin, towards_velocity;
    vec3_t      dir, wanted_pos, wanted_dir, dest_diff, final_dir;
    vec3_t      aim_fwd, aim_rgt, aim_up, yaw_angles;
    vec3_t      probe, mins8, maxs8;
    float       current_speed, dist_to_wanted, turn_factor, speed_factor;
    float       accel, wanted_speed;
    bool        bad_movement_direction;
    trace_t     tr;

    // swimming monsters just follow their velocity while out of the water
    if ((ent->flags & FL_SWIM) && ent->waterlevel < 3)
        return true;

    // time to pick somewhere new to hover?  Also re-picked the moment a pinned
    // monster loses sight of its enemy, since a pinned position is absolute and
    // would otherwise hold it behind a wall.
    if (ent->monsterinfo.fly_position_time <= level.framenum ||
        (ent->enemy && ent->monsterinfo.fly_pinned && !visible(ent, ent->enemy))) {
        ent->monsterinfo.fly_pinned = false;
        ent->monsterinfo.fly_position_time =
            level.framenum + (3.0f + 7.0f * random()) * BASE_FRAMERATE;
        G_IdealHoverPosition(ent, ent->monsterinfo.fly_ideal_position);
    }

    VectorCopy(ent->velocity, dir);
    current_speed = VectorNormalize(dir);
    VectorClear(towards_velocity);

    if (ent->enemy && !(ent->monsterinfo.aiflags & (AI_COMBAT_POINT | AI_SOUND_TARGET | AI_LOST_SIGHT))) {
        VectorCopy(ent->enemy->s.origin, towards_origin);
        VectorCopy(ent->enemy->velocity, towards_velocity);
    } else if (ent->goalentity) {
        VectorCopy(ent->goalentity->s.origin, towards_origin);
    } else {
        // whatever we were going towards probably died.  Coast to a stop.
        if (current_speed) {
            current_speed = max(0.0f, current_speed - ent->monsterinfo.fly_acceleration);
            VectorScale(dir, current_speed, ent->velocity);
        }
        return true;
    }

    if (ent->monsterinfo.fly_pinned)
        VectorCopy(ent->monsterinfo.fly_ideal_position, wanted_pos);
    else if (ent->monsterinfo.aiflags & (AI_COMBAT_POINT | AI_SOUND_TARGET | AI_LOST_SIGHT))
        VectorCopy(towards_origin, wanted_pos);
    else {
        // lead the enemy by a quarter second, then offset by the hover point
        VectorMA(towards_origin, 0.25f, towards_velocity, wanted_pos);
        VectorAdd(wanted_pos, ent->monsterinfo.fly_ideal_position, wanted_pos);
    }

    // find a place we can fit in on the way out from the enemy
    VectorSet(mins8, -8, -8, -8);
    VectorSet(maxs8, 8, 8, 8);
    tr = gi.trace(towards_origin, mins8, maxs8, wanted_pos, ent, MASK_SOLID | CONTENTS_MONSTERCLIP);
    if (!tr.allsolid)
        VectorCopy(tr.endpos, wanted_pos);

    VectorSubtract(wanted_pos, ent->s.origin, dest_diff);

    // already at the right height within our own bbox: do not fight over it
    if (dest_diff[2] > ent->mins[2] && dest_diff[2] < ent->maxs[2])
        dest_diff[2] = 0;

    VectorCopy(dest_diff, wanted_dir);
    dist_to_wanted = VectorNormalize(wanted_dir);

    // face the enemy, not the hover point - that is what keeps it shooting at
    // you while it circles
    if (!(ent->monsterinfo.aiflags & AI_MANUAL_STEERING)) {
        VectorSubtract(towards_origin, ent->s.origin, probe);
        ent->ideal_yaw = vectoyaw(probe);
    }

    VectorSet(yaw_angles, 0, ent->s.angles[YAW], 0);
    AngleVectors(yaw_angles, aim_fwd, aim_rgt, aim_up);

    // blocked from moving that way from here?
    VectorMA(ent->s.origin, ent->monsterinfo.fly_acceleration, wanted_dir, probe);
    tr = gi.trace(ent->s.origin, ent->mins, ent->maxs, probe, ent, MASK_SOLID | CONTENTS_MONSTERCLIP);

    // a fairly close block, so shift more dramatically than the slerp would
    if (tr.fraction < 0.25f) {
        vec3_t  a, b;
        bool    bottom_visible, top_visible;

        VectorCopy(ent->s.origin, a);
        a[2] += ent->mins[2];
        VectorCopy(ent->s.origin, b);
        b[2] += ent->mins[2] - ent->monsterinfo.fly_acceleration;
        bottom_visible = SV_flystep_testvisposition(a, wanted_pos, ent->s.origin, b, ent);

        VectorCopy(ent->s.origin, a);
        a[2] += ent->maxs[2];
        VectorCopy(ent->s.origin, b);
        b[2] += ent->maxs[2] + ent->monsterinfo.fly_acceleration;
        top_visible = SV_flystep_testvisposition(a, wanted_pos, ent->s.origin, b, ent);

        if (bottom_visible == top_visible) {
            // up and down are the same, so try left and right
            bool    left_visible, right_visible;

            VectorMA(ent->s.origin, ent->maxs[0], aim_fwd, a);
            VectorMA(a, -ent->maxs[1], aim_rgt, b);
            left_visible = gi.trace(b, NULL, NULL, wanted_pos, ent, MASK_SOLID | CONTENTS_MONSTERCLIP).fraction == 1.0f;
            VectorMA(a, ent->maxs[1], aim_rgt, b);
            right_visible = gi.trace(b, NULL, NULL, wanted_pos, ent, MASK_SOLID | CONTENTS_MONSTERCLIP).fraction == 1.0f;

            if (left_visible != right_visible) {
                if (right_visible)
                    VectorAdd(wanted_dir, aim_rgt, wanted_dir);
                else
                    VectorSubtract(wanted_dir, aim_rgt, wanted_dir);
            } else {
                // probably stuck; push straight off the surface
                VectorCopy(tr.plane.normal, wanted_dir);
            }
        } else if (top_visible) {
            VectorAdd(wanted_dir, aim_up, wanted_dir);
        } else {
            VectorSubtract(wanted_dir, aim_up, wanted_dir);
        }

        VectorNormalize(wanted_dir);
    }

    // The closer to a standstill, the more the heading may change; pushed past
    // top speed it should not turn at all.  turn_factor is how much of the
    // OLD heading is kept, so bigger means a wider turn.
    if (((ent->monsterinfo.fly_thrusters && !ent->monsterinfo.fly_pinned) ||
         (ent->monsterinfo.aiflags & (AI_COMBAT_POINT | AI_LOST_SIGHT))) &&
        DotProduct(dir, wanted_dir) > 0.0f)
        turn_factor = 0.45f;
    else
        turn_factor = min(1.0f, 0.84f + (0.08f * (current_speed / ent->monsterinfo.fly_speed)));

    if (current_speed)
        VectorCopy(dir, final_dir);
    else
        VectorCopy(wanted_dir, final_dir);

    // swimming monsters don't leave water voluntarily and flying monsters
    // don't enter it, though both will try to get back out
    bad_movement_direction = false;
    VectorMA(ent->s.origin, current_speed, wanted_dir, probe);

    if (ent->flags & FL_SWIM)
        bad_movement_direction = !(gi.pointcontents(probe) & MASK_WATER);
    else if ((ent->flags & FL_FLY) && ent->waterlevel < 3)
        bad_movement_direction = (gi.pointcontents(probe) & MASK_WATER) != 0;

    if (bad_movement_direction) {
        if (ent->monsterinfo.fly_recovery_framenum < level.framenum) {
            VectorSet(ent->monsterinfo.fly_recovery_dir, crandom(), crandom(), crandom());
            VectorNormalize(ent->monsterinfo.fly_recovery_dir);
            ent->monsterinfo.fly_recovery_framenum = level.framenum + 1 * BASE_FRAMERATE;
        }

        VectorCopy(ent->monsterinfo.fly_recovery_dir, wanted_dir);
    }

    if (current_speed && turn_factor > 0)
        VectorSlerp(dir, wanted_dir, 1.0f - turn_factor, final_dir);

    // slow down on approach so we don't fly past the hover point
    if (!ent->enemy || (ent->monsterinfo.fly_thrusters && !ent->monsterinfo.fly_pinned) ||
        (ent->monsterinfo.aiflags & (AI_COMBAT_POINT | AI_LOST_SIGHT)))
        speed_factor = 1.0f;
    else if (DotProduct(aim_fwd, wanted_dir) < -0.25f && current_speed)
        speed_factor = 0.0f;
    else
        speed_factor = min(1.0f, dist_to_wanted / ent->monsterinfo.fly_speed);

    if (bad_movement_direction)
        speed_factor = -speed_factor;

    accel = ent->monsterinfo.fly_acceleration;

    // flying away from where we want to be: reverse thrusters
    if (DotProduct(final_dir, wanted_dir) < 0.25f)
        accel *= 2.0f;

    wanted_speed = ent->monsterinfo.fly_speed * speed_factor;

    // a blindfiring or otherwise manually steered monster holds still
    if (ent->monsterinfo.aiflags & AI_MANUAL_STEERING)
        wanted_speed = 0;

    if (current_speed > wanted_speed)
        current_speed = max(wanted_speed, current_speed - accel);
    else if (current_speed < wanted_speed)
        current_speed = min(wanted_speed, current_speed + accel);

    VectorScale(final_dir, current_speed, ent->velocity);

    // buzzards bank to face the way they are orbiting
    if (ent->enemy && (ent->monsterinfo.fly_buzzard || (ent->monsterinfo.aiflags & AI_MEDIC))) {
        vec3_t  d, ang;

        VectorSubtract(ent->s.origin, towards_origin, d);
        VectorNormalize(d);
        vectoangles(d, ang);
        ent->s.angles[PITCH] = LerpAngle(ent->s.angles[PITCH], -ang[PITCH], FRAMETIME * 4.0f);
    } else {
        ent->s.angles[PITCH] = 0;
    }

    if (relink) {
        gi.linkentity(ent);
        G_TouchTriggers(ent);
    }

    return true;
}


/*
=================
M_WaterLevelAt

M_CatagorizePosition only ever reads an entity's OWN origin, and the rerelease
ground step needs to know what it would be standing in at a position it has not
moved to yet.  Same probe heights, taken at an arbitrary point.
=================
*/
static void M_WaterLevelAt(edict_t *ent, const vec3_t org, int *level, int *type)
{
    vec3_t  point;
    int     cont;

    *level = 0;
    *type = 0;

    point[0] = org[0];
    point[1] = org[1];
    point[2] = org[2] + ent->mins[2] + 1;
    cont = gi.pointcontents(point);
    if (!(cont & MASK_WATER))
        return;

    *type = cont;
    *level = 1;

    point[2] += 26;
    if (!(gi.pointcontents(point) & MASK_WATER))
        return;

    *level = 2;

    point[2] += 22;
    if (gi.pointcontents(point) & MASK_WATER)
        *level = 3;
}

/*
=================
SV_movestep_rerelease

id rewrote the ground half of SV_movestep; this is m_move.cpp:613 onwards, minus
the tesla/bad-area block (rogue-only) and the nav-mesh pieces.  Four changes
matter, and all four show up as a monster that will not go where it obviously
should:

  1. TWO forward traces, not one - one from a step-height above the start (what
     the classic does) and one flat forward - and it takes whichever got
     further.  The classic single raised trace fails wherever something is
     overhead, a half-open door or a low frame, even though the monster would
     walk straight through at floor level.

  2. `stepsize += 0.75f`.  A stair of exactly STEPSIZE is a coin flip in float
     without it.

  3. THE BUMP-SLIDE RETRY.  If the step barely moved the monster, re-aim
     ideal_yaw along the plane it hit and take the frame anyway.  Without this a
     monster wedged on a corner keeps choosing the same blocked heading and
     grinds in place - the single biggest cause of "stuck where it shouldn't
     be".  Throttled by bump_framenum so it cannot fire every frame.

  4. WATER.  The classic refuses to enter water at all from dry land, so a
     monster balks at an ankle-deep puddle.  id wades to the waist and refuses
     only slime, lava, and anything deeper.

Deliberately NOT ported: `RF_STAIR_STEP` (this tree's client has no stair
smoothing to feed), `G_Impact`, and the CheckForBadArea tesla handling.
=================
*/
static bool SV_movestep_rerelease(edict_t *ent, vec3_t move, bool relink)
{
    vec3_t      oldorg, start_up, end_up, end_fwd, end, dir, forward, yawang;
    trace_t     up_trace, fwd_trace, trace;
    trace_t     *chosen;
    float       stepsize, new_yaw, moved, wanted;
    int         steps;
    int         end_level, end_type;

    VectorCopy(ent->s.origin, oldorg);

    // push down from a step height above the wished position
    if (!(ent->monsterinfo.aiflags & AI_NOSTEP))
        stepsize = STEPSIZE;
    else
        stepsize = 1;
    stepsize += 0.75f;

    // (1) the raised trace: lift by one stepsize against gravity, then forward
    VectorMA(oldorg, -stepsize, ent->gravityVector, start_up);
    up_trace = gi.trace(oldorg, ent->mins, ent->maxs, start_up, ent, MASK_MONSTERSOLID);
    VectorCopy(up_trace.endpos, start_up);

    VectorAdd(start_up, move, end_up);
    up_trace = gi.trace(start_up, ent->mins, ent->maxs, end_up, ent, MASK_MONSTERSOLID);

    if (up_trace.startsolid) {
        VectorMA(start_up, -stepsize, ent->gravityVector, start_up);
        up_trace = gi.trace(start_up, ent->mins, ent->maxs, end_up, ent, MASK_MONSTERSOLID);
    }

    // ...and the flat one from where we actually are
    VectorAdd(oldorg, move, end_fwd);
    fwd_trace = gi.trace(oldorg, ent->mins, ent->maxs, end_fwd, ent, MASK_MONSTERSOLID);
    // (id retries this one on startsolid, but its retry nudges `start_up` and
    // re-runs the identical trace, so it is a no-op.  Not reproduced.)

    // pick the one that went farther
    if (up_trace.fraction > fwd_trace.fraction) {
        chosen = &up_trace;
        steps = 2;
    } else {
        chosen = &fwd_trace;
        steps = 1;
    }

    if (chosen->startsolid || chosen->allsolid)
        return false;

    // step us back down
    VectorMA(chosen->endpos, steps * stepsize, ent->gravityVector, end);
    trace = gi.trace(chosen->endpos, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID);

    // (4) monsters are fine stepping into water up to the waist, but will not
    // walk into anything deeper, or into slime or lava
    if (ent->waterlevel <= 2) {
        M_WaterLevelAt(ent, trace.endpos, &end_level, &end_type);
        if ((end_type & (CONTENTS_SLIME | CONTENTS_LAVA)) || end_level > 2)
            return false;
    }

    if (trace.fraction == 1) {
        // if monster had the ground pulled out, go ahead and fall
        if (ent->flags & FL_PARTIALGROUND) {
            VectorAdd(ent->s.origin, move, ent->s.origin);
            if (relink) {
                gi.linkentity(ent);
                G_TouchTriggers(ent);
            }
            ent->groundentity = NULL;
            return true;
        }

        return false;       // walked off an edge
    }

    // (3) if we did not actually get anywhere, slide along whatever we hit
    // rather than reporting failure and letting SV_NewChaseDir mill about
    moved = Distance(trace.endpos, oldorg);
    wanted = VectorLength(move);

    if (moved < wanted * 0.05f) {
        ent->monsterinfo.bad_move_framenum = level.framenum + BASE_FRAMERATE;

        if (ent->monsterinfo.bump_framenum < level.framenum && chosen->fraction < 1.0f) {
            // adjust ideal_yaw to move against the object we hit and try again
            VectorSet(yawang, 0, ent->ideal_yaw, 0);
            AngleVectors(yawang, forward, NULL, NULL);
            ClipVelocity(forward, chosen->plane.normal, dir, 1.0f);
            new_yaw = vectoyaw2(dir);

            if (DotProduct(dir, dir) > 0.1f && ent->ideal_yaw != new_yaw) {
                ent->ideal_yaw = new_yaw;
                ent->monsterinfo.random_change_framenum = level.framenum + 1;
                ent->monsterinfo.bump_framenum = level.framenum + 2;
                return true;
            }
        }

        return false;
    }

// check point traces down for dangling corners
    VectorCopy(trace.endpos, ent->s.origin);

    if (!M_CheckBottom(ent)) {
        if (ent->flags & FL_PARTIALGROUND) {
            // entity had floor mostly pulled out from underneath it
            // and is trying to correct
            if (relink) {
                gi.linkentity(ent);
                G_TouchTriggers(ent);
            }
            return true;
        }
        VectorCopy(oldorg, ent->s.origin);
        return false;
    }

    if (ent->flags & FL_PARTIALGROUND)
        ent->flags &= ~FL_PARTIALGROUND;

    ent->groundentity = trace.ent;
    ent->groundentity_linkcount = trace.ent->linkcount;

// the move is ok
    if (relink) {
        gi.linkentity(ent);
        G_TouchTriggers(ent);
    }
    return true;
}

/*
=============
SV_movestep

Called by monster program code.
The move will be adjusted for slopes and stairs, but if the move isn't
possible, no move is done, false is returned, and
pr_global_struct->trace_normal is set to the normal of the blocking wall
=============
*/
//FIXME since we need to test end position contents here, can we avoid doing
//it again later in catagorize position?
bool SV_movestep(edict_t *ent, vec3_t move, bool relink)
{
    float       dz;
    vec3_t      oldorg, neworg, end;
    trace_t     trace;
    int         i;
    float       stepsize;
    vec3_t      test;
    int         contents;

// try the move
    VectorCopy(ent->s.origin, oldorg);
    VectorAdd(ent->s.origin, move, neworg);

// flying monsters don't step up
    if (ent->flags & (FL_SWIM | FL_FLY)) {
        // [rerelease] the velocity-driven hover model, if this monster opted in
        if ((ent->monsterinfo.aiflags & AI_ALTERNATE_FLY) &&
            SV_alternate_flystep(ent, move, relink))
            return true;

        // try one move with vertical motion, then one without
        for (i = 0 ; i < 2 ; i++) {
            VectorAdd(ent->s.origin, move, neworg);
            if (i == 0 && ent->enemy) {
                if (!ent->goalentity)
                    ent->goalentity = ent->enemy;
                dz = ent->s.origin[2] - ent->goalentity->s.origin[2];
                if (ent->goalentity->client) {
                    if (dz > 40)
                        neworg[2] -= 8;
                    if (!((ent->flags & FL_SWIM) && (ent->waterlevel < 2)))
                        if (dz < 30)
                            neworg[2] += 8;
                } else {
                    if (dz > 8)
                        neworg[2] -= 8;
                    else if (dz > 0)
                        neworg[2] -= dz;
                    else if (dz < -8)
                        neworg[2] += 8;
                    else
                        neworg[2] += dz;
                }
            }
            trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, neworg, ent, MASK_MONSTERSOLID);

            // fly monsters don't enter water voluntarily
            if (ent->flags & FL_FLY) {
                if (!ent->waterlevel) {
                    test[0] = trace.endpos[0];
                    test[1] = trace.endpos[1];
                    test[2] = trace.endpos[2] + ent->mins[2] + 1;
                    contents = gi.pointcontents(test);
                    if (contents & MASK_WATER)
                        return false;
                }
            }

            // swim monsters don't exit water voluntarily
            if (ent->flags & FL_SWIM) {
                if (ent->waterlevel < 2) {
                    test[0] = trace.endpos[0];
                    test[1] = trace.endpos[1];
                    test[2] = trace.endpos[2] + ent->mins[2] + 1;
                    contents = gi.pointcontents(test);
                    if (!(contents & MASK_WATER))
                        return false;
                }
            }

            if (trace.fraction == 1) {
                VectorCopy(trace.endpos, ent->s.origin);
                if (relink) {
                    gi.linkentity(ent);
                    G_TouchTriggers(ent);
                }
                return true;
            }

            if (!ent->enemy)
                break;
        }

        return false;
    }

    // [rerelease] id rewrote the ground step; see SV_movestep_rerelease above.
    // Per the scope rule, classic Q2RTX keeps the 1997 body below.
    if (M_RereleaseGame())
        return SV_movestep_rerelease(ent, move, relink);

// push down from a step height above the wished position
    if (!(ent->monsterinfo.aiflags & AI_NOSTEP))
        stepsize = STEPSIZE;
    else
        stepsize = 1;

    // ROGUE - step along the entity's own gravity vector: one stepsize
    // "up" (against gravity), then trace two stepsizes "down". For normal
    // gravity this is identical to the old +stepsize / -2*stepsize.
    VectorMA(neworg, -1 * stepsize, ent->gravityVector, neworg);
    VectorCopy(neworg, end);
    VectorMA(end, 2 * stepsize, ent->gravityVector, end);

    trace = gi.trace(neworg, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID);

    if (trace.allsolid)
        return false;

    if (trace.startsolid) {
        VectorMA(neworg, stepsize, ent->gravityVector, neworg);
        trace = gi.trace(neworg, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID);
        if (trace.allsolid || trace.startsolid)
            return false;
    }


    // don't go in to water
    if (ent->waterlevel == 0) {
        test[0] = trace.endpos[0];
        test[1] = trace.endpos[1];
        test[2] = trace.endpos[2] + ent->mins[2] + 1;
        contents = gi.pointcontents(test);

        if (contents & MASK_WATER)
            return false;
    }

    if (trace.fraction == 1) {
        // if monster had the ground pulled out, go ahead and fall
        if (ent->flags & FL_PARTIALGROUND) {
            VectorAdd(ent->s.origin, move, ent->s.origin);
            if (relink) {
                gi.linkentity(ent);
                G_TouchTriggers(ent);
            }
            ent->groundentity = NULL;
            return true;
        }

        return false;       // walked off an edge
    }

// check point traces down for dangling corners
    VectorCopy(trace.endpos, ent->s.origin);

    if (!M_CheckBottom(ent)) {
        if (ent->flags & FL_PARTIALGROUND) {
            // entity had floor mostly pulled out from underneath it
            // and is trying to correct
            if (relink) {
                gi.linkentity(ent);
                G_TouchTriggers(ent);
            }
            return true;
        }
        VectorCopy(oldorg, ent->s.origin);
        return false;
    }

    if (ent->flags & FL_PARTIALGROUND) {
        ent->flags &= ~FL_PARTIALGROUND;
    }
    ent->groundentity = trace.ent;
    ent->groundentity_linkcount = trace.ent->linkcount;

// the move is ok
    if (relink) {
        gi.linkentity(ent);
        G_TouchTriggers(ent);
    }
    return true;
}


//============================================================================

/*
===============
M_ChangeYaw

===============
*/
void M_ChangeYaw(edict_t *ent)
{
    float   ideal;
    float   current;
    float   move;
    float   speed;

    current = anglemod(ent->s.angles[YAW]);
    ideal = ent->ideal_yaw;

    if (current == ideal)
        return;

    move = ideal - current;
    speed = ent->yaw_speed;
    if (ideal > current) {
        if (move >= 180)
            move = move - 360;
    } else {
        if (move <= -180)
            move = move + 360;
    }
    if (move > 0) {
        if (move > speed)
            move = speed;
    } else {
        if (move < -speed)
            move = -speed;
    }

    ent->s.angles[YAW] = anglemod(current + move);
}


/*
======================
SV_StepDirection

Turns to the movement direction, and walks the current distance if
facing it.

======================
*/
bool SV_StepDirection(edict_t *ent, float yaw, float dist)
{
    vec3_t      move, oldorigin;
    float       delta;

    ent->ideal_yaw = yaw;
    M_ChangeYaw(ent);

    yaw = DEG2RAD(yaw);
    move[0] = cos(yaw) * dist;
    move[1] = sin(yaw) * dist;
    move[2] = 0;

    VectorCopy(ent->s.origin, oldorigin);
    if (SV_movestep(ent, move, false)) {
        delta = ent->s.angles[YAW] - ent->ideal_yaw;
        if (delta > 45 && delta < 315) {
            // not turned far enough, so don't take the step
            VectorCopy(oldorigin, ent->s.origin);
        }
        gi.linkentity(ent);
        G_TouchTriggers(ent);
        return true;
    }
    gi.linkentity(ent);
    G_TouchTriggers(ent);
    return false;
}

/*
======================
SV_FixCheckBottom

======================
*/
void SV_FixCheckBottom(edict_t *ent)
{
    ent->flags |= FL_PARTIALGROUND;
}



/*
================
SV_NewChaseDir

================
*/
#define DI_NODIR    -1
void SV_NewChaseDir(edict_t *actor, edict_t *enemy, float dist)
{
    float   deltax, deltay;
    float   d[3];
    float   tdir, olddir, turnaround;

    //FIXME: how did we get here with no enemy
    if (!enemy)
        return;

    olddir = anglemod((int)(actor->ideal_yaw / 45) * 45);
    turnaround = anglemod(olddir - 180);

    deltax = enemy->s.origin[0] - actor->s.origin[0];
    deltay = enemy->s.origin[1] - actor->s.origin[1];
    if (deltax > 10)
        d[1] = 0;
    else if (deltax < -10)
        d[1] = 180;
    else
        d[1] = DI_NODIR;
    if (deltay < -10)
        d[2] = 270;
    else if (deltay > 10)
        d[2] = 90;
    else
        d[2] = DI_NODIR;

// try direct route
    if (d[1] != DI_NODIR && d[2] != DI_NODIR) {
        if (d[1] == 0)
            tdir = d[2] == 90 ? 45 : 315;
        else
            tdir = d[2] == 90 ? 135 : 215;

        if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
            return;
    }

// try other directions
    if (((Q_rand() & 3) & 1) || fabsf(deltay) > fabsf(deltax)) {
        tdir = d[1];
        d[1] = d[2];
        d[2] = tdir;
    }

    if (d[1] != DI_NODIR && d[1] != turnaround
        && SV_StepDirection(actor, d[1], dist))
        return;

    if (d[2] != DI_NODIR && d[2] != turnaround
        && SV_StepDirection(actor, d[2], dist))
        return;

    // ROGUE/rerelease: neither axis worked, so give the monster a chance to
    // deal with the block itself - jump the gap, or ride a plat.  If it says
    // it handled it, we must not move or turn it this frame.
    if (actor->monsterinfo.blocked && actor->inuse && actor->health > 0 &&
        !(actor->monsterinfo.aiflags & AI_TARGET_ANGER)) {
        if (actor->monsterinfo.blocked(actor, dist))
            return;
    }

    /* there is no direct path to the player, so pick another direction */

    if (olddir != DI_NODIR && SV_StepDirection(actor, olddir, dist))
        return;

    if (Q_rand() & 1) { /*randomly determine direction of search*/
        for (tdir = 0 ; tdir <= 315 ; tdir += 45)
            if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
                return;
    } else {
        for (tdir = 315 ; tdir >= 0 ; tdir -= 45)
            if (tdir != turnaround && SV_StepDirection(actor, tdir, dist))
                return;
    }

    if (turnaround != DI_NODIR && SV_StepDirection(actor, turnaround, dist))
        return;

    // [rerelease] Every direction failed.  The 1997 code parks ideal_yaw back
    // on `olddir` - the direction that just failed - so the next frame runs
    // this whole search again from the same heading and fails the same way,
    // forever.  That is the classic "monster jitters against a corner and
    // never leaves" deadlock.  id picks a random yaw instead, which costs one
    // wandering frame and breaks the cycle.
    if (M_RereleaseGame())
        actor->ideal_yaw = frand() * 360.0f;
    else
        actor->ideal_yaw = olddir;      // can't move

// if a bridge was pulled out from underneath a monster, it may not have
// a valid standing position at all

    if (!M_CheckBottom(actor))
        SV_FixCheckBottom(actor);
}

/*
======================
SV_CloseEnough

======================
*/
bool SV_CloseEnough(edict_t *ent, edict_t *goal, float dist)
{
    int     i;

    for (i = 0 ; i < 3 ; i++) {
        if (goal->absmin[i] > ent->absmax[i] + dist)
            return false;
        if (goal->absmax[i] < ent->absmin[i] - dist)
            return false;
    }
    return true;
}


/*
======================
M_MoveToGoal
======================
*/
void M_MoveToGoal(edict_t *ent, float dist)
{
    edict_t     *goal;

    goal = ent->goalentity;

    if (!ent->groundentity && !(ent->flags & (FL_FLY | FL_SWIM)))
        return;

// if the next step hits the enemy, return immediately
    if (ent->enemy &&  SV_CloseEnough(ent, ent->enemy, dist))
        return;

// bump around...
    if ((Q_rand() & 3) == 1 || !SV_StepDirection(ent, ent->ideal_yaw, dist)) {
        if (ent->inuse)
            SV_NewChaseDir(ent, goal, dist);
    }
}


/*
===============
M_walkmove
===============
*/
bool M_walkmove(edict_t *ent, float yaw, float dist)
{
    vec3_t  move;

    if (!ent->groundentity && !(ent->flags & (FL_FLY | FL_SWIM)))
        return false;

    yaw = DEG2RAD(yaw);
    move[0] = cos(yaw) * dist;
    move[1] = sin(yaw) * dist;
    move[2] = 0;

    return SV_movestep(ent, move, true);
}
