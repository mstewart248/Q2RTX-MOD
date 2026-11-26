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

#include "shared/shared.h"
#include "common/pmove.h"

#define STEPSIZE    18

// all of the locals will be zeroed before each
// pmove, just to make damn sure we don't have
// any differences when running on client or server

typedef struct {
    vec3_t      origin;         // full float precision
    vec3_t      velocity;       // full float precision

    vec3_t      forward, right, up;
    float       frametime;


    csurface_t  *groundsurface;
    cplane_t    groundplane;
    int         groundcontents;

    vec3_t      previous_origin;
    bool        ladder;

    // Quake III powerups (pmoveParams_t.q3)
    float       speedscale;     // 1.3 with Haste, else 1
    bool        q3_flight;

    // Quake III movement (pmoveParams_t.q3move): bg_pmove.c's pml.walking
    // (standing on a walkable floor) and pml.groundPlane (touching any floor,
    // walkable or too steep); the plane itself is groundplane above
    bool        walking;
    bool        onplane;
} pml_t;

static pmove_t      *pm;
static pml_t        pml;

static pmoveParams_t    *pmp;

// movement parameters
static const float  pm_stopspeed = 100;
static const float  pm_duckspeed = 100;
static const float  pm_accelerate = 10;
static const float  pm_wateraccelerate = 10;
static const float  pm_waterspeed = 400;

/*
  walking up a step should kill some velocity
*/

/*
==================
PM_ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define STOP_EPSILON    0.1f

static void PM_ClipVelocity(const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
    float   backoff;
    float   change;
    int     i;

    backoff = DotProduct(in, normal) * overbounce;

    for (i = 0; i < 3; i++) {
        change = normal[i] * backoff;
        out[i] = in[i] - change;
        if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
            out[i] = 0;
    }
}

/*
==================
PM_StepSlideMove

Each intersection will try to step over the obstruction instead of
sliding along it.

Returns a new origin, velocity, and contact entity
Does not modify any world state?
==================
*/
#define MIN_STEP_NORMAL 0.7f    // can't step up onto very steep slopes
#define MAX_CLIP_PLANES 5

static void PM_StepSlideMove_(void)
{
    int         bumpcount, numbumps;
    vec3_t      dir;
    float       d;
    int         numplanes;
    vec3_t      planes[MAX_CLIP_PLANES];
    vec3_t      primal_velocity;
    int         i, j;
    trace_t trace;
    vec3_t      end;
    float       time_left;

    numbumps = 4;

    VectorCopy(pml.velocity, primal_velocity);
    numplanes = 0;

    time_left = pml.frametime;

    for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
        for (i = 0; i < 3; i++)
            end[i] = pml.origin[i] + time_left * pml.velocity[i];

        trace = pm->trace(pml.origin, pm->mins, pm->maxs, end);

        if (trace.allsolid) {
            // entity is trapped in another solid
            pml.velocity[2] = 0;    // don't build up falling damage
            return;
        }

        if (trace.fraction > 0) {
            // actually covered some distance
            VectorCopy(trace.endpos, pml.origin);
            numplanes = 0;
        }

        if (trace.fraction == 1)
            break;     // moved the entire distance

        // save entity for contact
        if (pm->numtouch < MAXTOUCH && trace.ent) {
            pm->touchents[pm->numtouch] = trace.ent;
            pm->numtouch++;
        }

        time_left -= time_left * trace.fraction;

        // slide along this plane
        if (numplanes >= MAX_CLIP_PLANES) {
            // this shouldn't really happen
            VectorClear(pml.velocity);
            break;
        }

        // [rerelease] if this is the same plane we hit before, nudge the
        // origin out along it: the classic epsilon trap on non-axial planes,
        // where the box keeps re-hitting one slope and the crease logic below
        // zeroes the velocity - "stuck" against a surface that is not a corner.
        // The rerelease follows the nudge with G_FixStuckObject; here a nudge
        // that lands in solid is simply undone.
        if (pmp->rerelease) {
            for (i = 0; i < numplanes; i++) {
                if (DotProduct(trace.plane.normal, planes[i]) > 0.99f) {
                    vec3_t  nudged;
                    trace_t test;

                    VectorCopy(pml.origin, nudged);
                    nudged[0] += trace.plane.normal[0] * 0.01f;
                    nudged[1] += trace.plane.normal[1] * 0.01f;
                    test = pm->trace(nudged, pm->mins, pm->maxs, nudged);
                    if (!test.startsolid)
                        VectorCopy(nudged, pml.origin);
                    break;
                }
            }
            if (i < numplanes)
                continue;
        }

        VectorCopy(trace.plane.normal, planes[numplanes]);
        numplanes++;

//
// modify original_velocity so it parallels all of the clip planes
//
        for (i = 0; i < numplanes; i++) {
            PM_ClipVelocity(pml.velocity, planes[i], pml.velocity, 1.01f);
            for (j = 0; j < numplanes; j++)
                if (j != i) {
                    if (DotProduct(pml.velocity, planes[j]) < 0)
                        break;  // not ok
                }
            if (j == numplanes)
                break;
        }

        if (i != numplanes) {
            // go along this plane
        } else {
            // go along the crease
            if (numplanes != 2) {
                VectorClear(pml.velocity);
                break;
            }
            CrossProduct(planes[0], planes[1], dir);
            d = DotProduct(dir, pml.velocity);
            VectorScale(dir, d, pml.velocity);
        }

        //
        // if velocity is against the original velocity, stop dead
        // to avoid tiny occilations in sloping corners
        //
        if (DotProduct(pml.velocity, primal_velocity) <= 0) {
            VectorClear(pml.velocity);
            break;
        }
    }

    if (pm->s.pm_time)
        VectorCopy(primal_velocity, pml.velocity);
}

/*
==================
PM_StepSlideMove

==================
*/
static void PM_StepSlideMove(void)
{
    vec3_t      start_o, start_v;
    vec3_t      down_o, down_v;
    trace_t     trace;
    float       down_dist, up_dist, step_size;
    vec3_t      up, down;

    VectorCopy(pml.origin, start_o);
    VectorCopy(pml.velocity, start_v);

    PM_StepSlideMove_();

    VectorCopy(pml.origin, down_o);
    VectorCopy(pml.velocity, down_v);

    VectorCopy(start_o, up);
    up[2] += STEPSIZE;

    if (pmp->rerelease) {
        // [rerelease] sweep up rather than test the end point, and step by the
        // height actually reached: under a low or sloped ceiling the stock test
        // passes, the box is placed a full STEPSIZE up, and the push-down below
        // then has further to go than it came.
        trace = pm->trace(start_o, pm->mins, pm->maxs, up);
        if (trace.allsolid)
            return;     // can't step up
        step_size = trace.endpos[2] - start_o[2];
        VectorCopy(trace.endpos, up);
    } else {
        trace = pm->trace(up, pm->mins, pm->maxs, up);
        if (trace.allsolid)
            return;     // can't step up
        step_size = STEPSIZE;
    }

    // try sliding above
    VectorCopy(up, pml.origin);
    VectorCopy(start_v, pml.velocity);

    PM_StepSlideMove_();

    // push down the final amount
    VectorCopy(pml.origin, down);
    down[2] -= step_size;
    if (pmp->rerelease && start_o[2] < down[2]) {
        // [Update 1] jitspoe's stair clip fix: a box moving up (a jump into a
        // step's edge) ends the push-down above where it started, so the
        // short trace often finds no floor and the step-up is thrown away by
        // the normal check below. Probing to just under the start finds the
        // step's top, so a jump against a stair lands on it. The box itself
        // still stops at the normal push-down end.
        vec3_t  original_down;

        VectorCopy(down, original_down);
        down[2] = start_o[2] - 1;
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, down);
        if (!trace.allsolid) {
            trace_t real_trace = pm->trace(pml.origin, pm->mins, pm->maxs, original_down);
            VectorCopy(real_trace.endpos, pml.origin);
        }
    } else {
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, down);
        if (!trace.allsolid)
            VectorCopy(trace.endpos, pml.origin);
    }

    VectorCopy(pml.origin, up);

    // decide which one went farther
    down_dist = (down_o[0] - start_o[0]) * (down_o[0] - start_o[0])
                + (down_o[1] - start_o[1]) * (down_o[1] - start_o[1]);
    up_dist = (up[0] - start_o[0]) * (up[0] - start_o[0])
              + (up[1] - start_o[1]) * (up[1] - start_o[1]);

    if (down_dist > up_dist || trace.plane.normal[2] < MIN_STEP_NORMAL) {
        VectorCopy(down_o, pml.origin);
        VectorCopy(down_v, pml.velocity);
        if (!pmp->rerelease)
            return;
    } else {
        //!! Special case
        // if we were walking along a plane, then we need to copy the Z over.
        // [Update 1] the rerelease had this behind PMF_ON_GROUND, which kept
        // the stepped-up pass's Z in the air and broke ramp jumps; Update 1
        // dropped the condition again, which is also what stock Q2 does.
        pml.velocity[2] = down_v[2];
    }

    if (!pmp->rerelease)
        return;

    // [rerelease] step down stairs and slopes. Stock pmove moves horizontally,
    // ends up hovering over a descending slope, and the 0.25 ground probe in
    // PM_CategorizePosition misses - so every frame down a ramp is a short fall
    // that gravity accelerates, and each landing clips that fall into the slope
    // as extra speed. Snapping back onto the floor keeps the player walking.
    if ((pm->s.pm_flags & PMF_ON_GROUND) && !pml.ladder &&
        (pm->waterlevel < 2 || (pm->cmd.upmove < 10 && pml.velocity[2] <= 0))) {
        VectorCopy(pml.origin, down);
        down[2] -= STEPSIZE;
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, down);
        if (trace.fraction < 1.0f)
            VectorCopy(trace.endpos, pml.origin);
    }
}

/*
==================
PM_Friction

Handles both ground friction and water friction
==================
*/
static void PM_Friction(void)
{
    float   *vel;
    float   speed, newspeed, control;
    float   friction;
    float   drop;

    vel = pml.velocity;

    speed = VectorLength(vel);
    if (speed < 1) {
        vel[0] = 0;
        vel[1] = 0;
        return;
    }

    drop = 0;

// apply ground friction
    if ((pm->groundentity && pml.groundsurface && !(pml.groundsurface->flags & SURF_SLICK)) || (pml.ladder)) {
        friction = pmp->friction;
        control = speed < pm_stopspeed ? pm_stopspeed : speed;
        drop += control * friction * pml.frametime;
    }

// apply water friction
    if (pm->waterlevel && !pml.ladder)
        drop += speed * pmp->waterfriction * pm->waterlevel * pml.frametime;

// scale the velocity
    newspeed = speed - drop;
    if (newspeed < 0)
        newspeed = 0;
    newspeed /= speed;

    vel[0] = vel[0] * newspeed;
    vel[1] = vel[1] * newspeed;
    vel[2] = vel[2] * newspeed;
}

/*
==============
PM_Accelerate

Handles user intended acceleration
==============
*/
static void PM_Accelerate(const vec3_t wishdir, float wishspeed, float accel)
{
    int         i;
    float       addspeed, accelspeed, currentspeed;

    currentspeed = DotProduct(pml.velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0)
        return;
    accelspeed = accel * pml.frametime * wishspeed;
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    for (i = 0; i < 3; i++)
        pml.velocity[i] += accelspeed * wishdir[i];
}

static void PM_AirAccelerate(const vec3_t wishdir, float wishspeed, float accel)
{
    int         i;
    float       addspeed, accelspeed, currentspeed, wishspd = wishspeed;

    if (wishspd > 30)
        wishspd = 30;
    currentspeed = DotProduct(pml.velocity, wishdir);
    addspeed = wishspd - currentspeed;
    if (addspeed <= 0)
        return;
    accelspeed = accel * wishspeed * pml.frametime;
    if (accelspeed > addspeed)
        accelspeed = addspeed;

    for (i = 0; i < 3; i++)
        pml.velocity[i] += accelspeed * wishdir[i];
}

/*
=============
PM_AddCurrents
=============
*/
static void PM_AddCurrents(vec3_t wishvel)
{
    vec3_t  v;
    float   s;

    //
    // account for ladders
    //

    if (pml.ladder && fabsf(pml.velocity[2]) <= 200) {
        if ((pm->viewangles[PITCH] <= -15) && (pm->cmd.forwardmove > 0))
            wishvel[2] = 200;
        else if ((pm->viewangles[PITCH] >= 15) && (pm->cmd.forwardmove > 0))
            wishvel[2] = -200;
        else if (pm->cmd.upmove > 0)
            wishvel[2] = 200;
        else if (pm->cmd.upmove < 0)
            wishvel[2] = -200;
        else
            wishvel[2] = 0;

        // limit horizontal speed when on a ladder
        clamp(wishvel[0], -25, 25);
        clamp(wishvel[1], -25, 25);
    }

    //
    // add water currents
    //

    if (pm->watertype & MASK_CURRENT) {
        VectorClear(v);

        if (pm->watertype & CONTENTS_CURRENT_0)
            v[0] += 1;
        if (pm->watertype & CONTENTS_CURRENT_90)
            v[1] += 1;
        if (pm->watertype & CONTENTS_CURRENT_180)
            v[0] -= 1;
        if (pm->watertype & CONTENTS_CURRENT_270)
            v[1] -= 1;
        if (pm->watertype & CONTENTS_CURRENT_UP)
            v[2] += 1;
        if (pm->watertype & CONTENTS_CURRENT_DOWN)
            v[2] -= 1;

        s = pm_waterspeed;
        if ((pm->waterlevel == 1) && (pm->groundentity))
            s /= 2;

        VectorMA(wishvel, s, v, wishvel);
    }

    //
    // add conveyor belt velocities
    //

    if (pm->groundentity) {
        VectorClear(v);

        if (pml.groundcontents & CONTENTS_CURRENT_0)
            v[0] += 1;
        if (pml.groundcontents & CONTENTS_CURRENT_90)
            v[1] += 1;
        if (pml.groundcontents & CONTENTS_CURRENT_180)
            v[0] -= 1;
        if (pml.groundcontents & CONTENTS_CURRENT_270)
            v[1] -= 1;
        if (pml.groundcontents & CONTENTS_CURRENT_UP)
            v[2] += 1;
        if (pml.groundcontents & CONTENTS_CURRENT_DOWN)
            v[2] -= 1;

        VectorMA(wishvel, 100 /* pm->groundentity->speed */, v, wishvel);
    }
}

/*
===================
PM_WaterMove

===================
*/
static void PM_WaterMove(void)
{
    int     i;
    vec3_t  wishvel;
    float   wishspeed;
    vec3_t  wishdir;

//
// user intentions
//
    for (i = 0; i < 3; i++)
        wishvel[i] = pml.forward[i] * pm->cmd.forwardmove + pml.right[i] * pm->cmd.sidemove;

    if (!pm->cmd.forwardmove && !pm->cmd.sidemove && !pm->cmd.upmove)
        wishvel[2] -= 60;       // drift towards bottom
    else
        wishvel[2] += pm->cmd.upmove;

    PM_AddCurrents(wishvel);

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    if (wishspeed > pmp->maxspeed * pml.speedscale) {
        VectorScale(wishvel, pmp->maxspeed * pml.speedscale / wishspeed, wishvel);
        wishspeed = pmp->maxspeed * pml.speedscale;
    }
    wishspeed *= pmp->watermult;

    PM_Accelerate(wishdir, wishspeed, pm_wateraccelerate);

    PM_StepSlideMove();
}

/*
===================
PM_AirMove

===================
*/
static void PM_AirMove(void)
{
    int         i;
    vec3_t      wishvel;
    float       fmove, smove;
    vec3_t      wishdir;
    float       wishspeed;
    float       maxspeed;

    fmove = pm->cmd.forwardmove;
    smove = pm->cmd.sidemove;

    for (i = 0; i < 2; i++)
        wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
    wishvel[2] = 0;

    PM_AddCurrents(wishvel);

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

//
// clamp to server defined max speed
//
    maxspeed = (pm->s.pm_flags & PMF_DUCKED) ? pm_duckspeed : pmp->maxspeed;
    maxspeed *= pml.speedscale;

    if (wishspeed > maxspeed) {
        VectorScale(wishvel, maxspeed / wishspeed, wishvel);
        wishspeed = maxspeed;
    }

    if (pml.ladder) {
        PM_Accelerate(wishdir, wishspeed, pm_accelerate);
        if (!wishvel[2]) {
            if (pml.velocity[2] > 0) {
                pml.velocity[2] -= pm->s.gravity * pml.frametime;
                if (pml.velocity[2] < 0)
                    pml.velocity[2] = 0;
            } else {
                pml.velocity[2] += pm->s.gravity * pml.frametime;
                if (pml.velocity[2] > 0)
                    pml.velocity[2] = 0;
            }
        }
        PM_StepSlideMove();
    } else if (pm->groundentity) {
        // walking on ground
        pml.velocity[2] = 0; //!!! this is before the accel
        PM_Accelerate(wishdir, wishspeed, pm_accelerate);

// PGM  -- fix for negative trigger_gravity fields
//      pml.velocity[2] = 0;
        if (pm->s.gravity > 0)
            pml.velocity[2] = 0;
        else
            pml.velocity[2] -= pm->s.gravity * pml.frametime;
// PGM

        if (!pml.velocity[0] && !pml.velocity[1])
            return;
        PM_StepSlideMove();
    } else {
        // not on ground, so little effect on velocity
        if (pmp->airaccelerate)
            PM_AirAccelerate(wishdir, wishspeed, pm_accelerate);
        else
            PM_Accelerate(wishdir, wishspeed, 1);
        // add gravity
        pml.velocity[2] -= pm->s.gravity * pml.frametime;
        PM_StepSlideMove();
    }
}

/*
===================
PM_Q3FlightMove

Quake III's Flight powerup (bg_pmove.c PM_FlyMove): no gravity, steer in
three dimensions along the view with jump to rise and crouch to sink, a light
friction of its own (3) and acceleration of 8. Collides like walking.
===================
*/
static void PM_Q3FlightMove(void)
{
    vec3_t  wishvel, wishdir;
    float   wishspeed, maxspeed, speed;
    int     i;

    speed = VectorLength(pml.velocity);
    if (speed < 1) {
        VectorClear(pml.velocity);
    } else {
        float newspeed = speed - speed * 3 * pml.frametime;
        if (newspeed < 0)
            newspeed = 0;
        VectorScale(pml.velocity, newspeed / speed, pml.velocity);
    }

    AngleVectors(pm->viewangles, pml.forward, pml.right, pml.up);

    for (i = 0; i < 3; i++)
        wishvel[i] = pml.forward[i] * pm->cmd.forwardmove + pml.right[i] * pm->cmd.sidemove;
    wishvel[2] += pm->cmd.upmove;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    maxspeed = pmp->maxspeed * pml.speedscale;
    if (wishspeed > maxspeed)
        wishspeed = maxspeed;

    PM_Accelerate(wishdir, wishspeed, 8);
    PM_StepSlideMove();
}

/*
=============
PM_CategorizePosition
=============
*/
static void PM_SetWaterLevel(void);

static void PM_CategorizePosition(void)
{
    vec3_t      point;
    trace_t     trace;
    bool        slanted, rising;

// if the player hull point one unit down is solid, the player
// is on ground

// see if standing on something solid
    point[0] = pml.origin[0];
    point[1] = pml.origin[1];
    point[2] = pml.origin[2] - 0.25f;
    rising = pml.velocity[2] > 180; //!!ZOID changed from 100 to 180 (ramp accel)

    // A fast run up a steep ramp stores the ramp's own climb rate as Z (the
    // step-up copies it from the clipped pass), and on a 40 degree ramp that
    // is over 180 at about 350 ups - a diagonal run into a wall is enough.
    // Since Update 1 copies that Z in the air too, a single airborne frame
    // then repeats forever: no friction, the ramp hands the Z back each frame,
    // and the player glides up the ramp with no keys held. Only count Z as
    // leaving the ground when it actually moves away from the floor under us.
    if (rising && pmp->rerelease) {
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, point);
        if (trace.fraction < 1.0f && !trace.startsolid &&
            trace.plane.normal[2] >= 0.7f &&
            DotProduct(pml.velocity, trace.plane.normal) < 10)
            rising = false;
    }

    if (rising) {
        pm->s.pm_flags &= ~PMF_ON_GROUND;
        pm->groundentity = NULL;
    } else {
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, point);
        pml.groundplane = trace.plane;
        pml.groundsurface = trace.surface;
        pml.groundcontents = trace.contents;

        slanted = trace.plane.normal[2] < 0.7f && !trace.startsolid;

        // [rerelease] a player wedged between a too-steep slope and a wall can
        // neither stand on the slope nor slide off it, and stock pmove leaves
        // them there. Allow "standing" on the slope when the box is right up
        // against something along the slope's normal.
        if (pmp->rerelease && slanted && trace.fraction < 1.0f) {
            vec3_t  out;
            trace_t slant;

            VectorAdd(pml.origin, trace.plane.normal, out);
            slant = pm->trace(pml.origin, pm->mins, pm->maxs, out);
            if (slant.fraction < 1.0f && !slant.startsolid)
                slanted = false;
        }

        if (!trace.ent || slanted) {
            pm->groundentity = NULL;
            pm->s.pm_flags &= ~PMF_ON_GROUND;
        } else {
            pm->groundentity = trace.ent;

            // hitting solid ground will end a waterjump
            if (pm->s.pm_flags & PMF_TIME_WATERJUMP) {
                pm->s.pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND | PMF_TIME_TELEPORT);
                pm->s.pm_time = 0;
            }

            if (!(pm->s.pm_flags & PMF_ON_GROUND)) {
                // just hit the ground
                pm->s.pm_flags |= PMF_ON_GROUND;
                if (pmp->n64) {
                    // [rerelease] N64: every landing, however soft, holds the
                    // next jump off for 128 ms (KEX's pm_time 128, in our 8 ms
                    // units), so there is no bunny hopping
                    pm->s.pm_flags |= PMF_TIME_LAND;
                    pm->s.pm_time = 16;
                }
                // don't do landing time if we were just going down a slope
                else if (pml.velocity[2] < -200 && !pmp->strafehack) {
                    pm->s.pm_flags |= PMF_TIME_LAND;
                    // don't allow another jump for a little while
                    if (pml.velocity[2] < -400)
                        pm->s.pm_time = 25;
                    else
                        pm->s.pm_time = 18;
                }
            }
        }

        if (pm->numtouch < MAXTOUCH && trace.ent) {
            pm->touchents[pm->numtouch] = trace.ent;
            pm->numtouch++;
        }
    }

    PM_SetWaterLevel();
}

/*
=============
PM_SetWaterLevel

Waterlevel and watertype, accounting for ducking. Its own function so Quake
III's movement (which tests water before the ground) can call it too.
=============
*/
static void PM_SetWaterLevel(void)
{
    vec3_t      point;
    int         cont;
    int         sample1;
    int         sample2;

    pm->waterlevel = 0;
    pm->watertype = 0;

    sample2 = pm->viewheight - pm->mins[2];
    sample1 = sample2 / 2;

    point[0] = pml.origin[0];
    point[1] = pml.origin[1];
    point[2] = pml.origin[2] + pm->mins[2] + 1;
    cont = pm->pointcontents(point);

    if (cont & MASK_WATER) {
        pm->watertype = cont;
        pm->waterlevel = 1;
        point[2] = pml.origin[2] + pm->mins[2] + sample1;
        cont = pm->pointcontents(point);
        if (cont & MASK_WATER) {
            pm->waterlevel = 2;
            point[2] = pml.origin[2] + pm->mins[2] + sample2;
            cont = pm->pointcontents(point);
            if (cont & MASK_WATER)
                pm->waterlevel = 3;
        }
    }
}

/*
=============
PM_CheckJump
=============
*/
static void PM_CheckJump(void)
{
    if (pm->s.pm_flags & PMF_TIME_LAND) {
        // hasn't been long enough since landing to jump again
        return;
    }

    if (pm->cmd.upmove < 10) {
        // not holding jump
        pm->s.pm_flags &= ~PMF_JUMP_HELD;
        return;
    }

    // must wait for jump to be released
    if (pm->s.pm_flags & PMF_JUMP_HELD)
        return;

    if (pm->s.pm_type == PM_DEAD)
        return;

    if (pm->waterlevel >= 2) {
        // swimming, not jumping
        pm->groundentity = NULL;

        if (pmp->waterhack)
            return;

        if (pml.velocity[2] <= -300)
            return;

        // FIXME: makes velocity dependent on client FPS,
        // even causes prediction misses
        if (pm->watertype == CONTENTS_WATER)
            pml.velocity[2] = 100;
        else if (pm->watertype == CONTENTS_SLIME)
            pml.velocity[2] = 80;
        else
            pml.velocity[2] = 50;
        return;
    }

    if (pm->groundentity == NULL)
        return;     // in air, so no effect

    pm->s.pm_flags |= PMF_JUMP_HELD;

    pm->groundentity = NULL;
    pm->s.pm_flags &= ~PMF_ON_GROUND;
    pml.velocity[2] += 270;
    if (pml.velocity[2] < 270)
        pml.velocity[2] = 270;
}

/*
=============
PM_CheckSpecialMovement
=============
*/
static void PM_CheckSpecialMovement(void)
{
    vec3_t  spot;
    int     cont;
    vec3_t  flatforward;
    trace_t trace;

    if (pm->s.pm_time)
        return;

    pml.ladder = false;

    // check for ladder
    flatforward[0] = pml.forward[0];
    flatforward[1] = pml.forward[1];
    flatforward[2] = 0;
    VectorNormalize(flatforward);

    VectorMA(pml.origin, 1, flatforward, spot);
    trace = pm->trace(pml.origin, pm->mins, pm->maxs, spot);
    if ((trace.fraction < 1) && (trace.contents & CONTENTS_LADDER))
        pml.ladder = true;

    // check for water jump
    if (pm->waterlevel != 2)
        return;

    VectorMA(pml.origin, 30, flatforward, spot);
    spot[2] += 4;
    cont = pm->pointcontents(spot);
    if (!(cont & CONTENTS_SOLID))
        return;

    spot[2] += 16;
    cont = pm->pointcontents(spot);
    if (cont)
        return;
    // jump out of water
    VectorScale(flatforward, 50, pml.velocity);
    pml.velocity[2] = 350;

    pm->s.pm_flags |= PMF_TIME_WATERJUMP;
    pm->s.pm_time = 255;
}

/*
===============
PM_FlyMove
===============
*/
static void PM_FlyMove(void)
{
    float   speed, drop, friction, control, newspeed;
    float   currentspeed, addspeed, accelspeed;
    int         i;
    vec3_t      wishvel;
    float       fmove, smove;
    vec3_t      wishdir;
    float       wishspeed;

    pm->viewheight = 22;

    // friction
    speed = VectorLength(pml.velocity);
    if (speed < 1) {
        VectorClear(pml.velocity);
    } else {
        drop = 0;

        friction = pmp->flyfriction;
        control = speed < pm_stopspeed ? pm_stopspeed : speed;
        drop += control * friction * pml.frametime;

        // scale the velocity
        newspeed = speed - drop;
        if (newspeed < 0)
            newspeed = 0;
        newspeed /= speed;

        VectorScale(pml.velocity, newspeed, pml.velocity);
    }

    // accelerate
    fmove = pm->cmd.forwardmove;
    smove = pm->cmd.sidemove;

    VectorNormalize(pml.forward);
    VectorNormalize(pml.right);

    for (i = 0; i < 3; i++)
        wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
    wishvel[2] += pm->cmd.upmove;

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);

    //
    // clamp to server defined max speed
    //
    if (wishspeed > pmp->maxspeed) {
        VectorScale(wishvel, pmp->maxspeed / wishspeed, wishvel);
        wishspeed = pmp->maxspeed;
    }

    currentspeed = DotProduct(pml.velocity, wishdir);
    addspeed = wishspeed - currentspeed;
    if (addspeed <= 0) {
        if (!pmp->flyhack) {
            return; // original buggy behaviour
        }
    } else {
        accelspeed = pm_accelerate * pml.frametime * wishspeed;
        if (accelspeed > addspeed)
            accelspeed = addspeed;

        for (i = 0; i < 3; i++)
            pml.velocity[i] += accelspeed * wishdir[i];
    }

    // move
    VectorMA(pml.origin, pml.frametime, pml.velocity, pml.origin);
}

/*
==============
PM_CheckDuck

Sets mins, maxs, and pm->viewheight
==============
*/
static void PM_CheckDuck(void)
{
    trace_t trace;

    pm->mins[0] = -16;
    pm->mins[1] = -16;

    pm->maxs[0] = 16;
    pm->maxs[1] = 16;

    if (pm->s.pm_type == PM_GIB) {
        pm->mins[2] = 0;
        pm->maxs[2] = 16;
        pm->viewheight = 8;
        return;
    }

    pm->mins[2] = -24;

    if (pm->s.pm_type == PM_DEAD) {
        pm->s.pm_flags |= PMF_DUCKED;
    } else if (pm->cmd.upmove < 0 && ((pm->s.pm_flags & PMF_ON_GROUND) || pmp->q3move) && !pmp->n64) {
        // duck (never on the N64 maps, which were built for a player who
        // cannot - the game prints $g_n64_crouching instead, see ClientThink).
        // Quake III ducks in the air too: the crouch-jump onto a ledge.
        pm->s.pm_flags |= PMF_DUCKED;
    } else {
        // stand up if possible
        if (pm->s.pm_flags & PMF_DUCKED) {
            // try to stand up
            pm->maxs[2] = 32;
            trace = pm->trace(pml.origin, pm->mins, pm->maxs, pml.origin);
            if (!trace.allsolid)
                pm->s.pm_flags &= ~PMF_DUCKED;
        }
    }

    if (pm->s.pm_flags & PMF_DUCKED) {
        pm->maxs[2] = 4;
        pm->viewheight = -2;
    } else {
        pm->maxs[2] = 32;
        pm->viewheight = 22;
    }
}

/*
==============
PM_DeadMove
==============
*/
static void PM_DeadMove(void)
{
    float   forward;

    if (!pm->groundentity)
        return;

    // extra friction
    forward = VectorLength(pml.velocity);
    forward -= 20;
    if (forward <= 0) {
        VectorClear(pml.velocity);
    } else {
        VectorNormalize(pml.velocity);
        VectorScale(pml.velocity, forward, pml.velocity);
    }
}

static bool PM_GoodPosition(void)
{
    trace_t trace;
    vec3_t  origin, end;
    int     i;

    if (pm->s.pm_type == PM_SPECTATOR)
        return true;

    for (i = 0; i < 3; i++)
        origin[i] = end[i] = pm->s.origin[i] * 0.125f;
    trace = pm->trace(origin, pm->mins, pm->maxs, end);

    return !trace.allsolid;
}

/*
================
PM_SnapPosition

On exit, the origin will have a value that is pre-quantized to the 0.125
precision of the network channel and in a valid position.
================
*/
static void PM_SnapPosition(void)
{
    int     sign[3];
    int     i, j, bits;
    int     base[3];        // pmove origins are 32-bit (wide coordinates)
    // try all single bits first
    static const byte jitterbits[8] = {0, 4, 1, 2, 3, 5, 6, 7};

    // snap velocity to eigths
    for (i = 0; i < 3; i++)
        pm->s.velocity[i] = (int)(pml.velocity[i] * 8);

    for (i = 0; i < 3; i++) {
        if (pml.origin[i] >= 0)
            sign[i] = 1;
        else
            sign[i] = -1;
        pm->s.origin[i] = (int)(pml.origin[i] * 8);
        if (pm->s.origin[i] * 0.125f == pml.origin[i])
            sign[i] = 0;
    }
    VectorCopy(pm->s.origin, base);

    // try all combinations
    for (j = 0; j < 8; j++) {
        bits = jitterbits[j];
        VectorCopy(base, pm->s.origin);
        for (i = 0; i < 3; i++)
            if (bits & (1 << i))
                pm->s.origin[i] += sign[i];

        if (PM_GoodPosition())
            return;
    }

    // go back to the last position
    VectorCopy(pml.previous_origin, pm->s.origin);
}

/*
================
PM_InitialSnapPosition

================
*/
static void PM_InitialSnapPosition(void)
{
    int        x, y, z;
    int        base[3];
    static const short offset[3] = { 0, -1, 1 };

    VectorCopy(pm->s.origin, base);

    for (z = 0; z < 3; z++) {
        pm->s.origin[2] = base[2] + offset[z];
        for (y = 0; y < 3; y++) {
            pm->s.origin[1] = base[1] + offset[y];
            for (x = 0; x < 3; x++) {
                pm->s.origin[0] = base[0] + offset[x];
                if (PM_GoodPosition()) {
                    pml.origin[0] = pm->s.origin[0] * 0.125f;
                    pml.origin[1] = pm->s.origin[1] * 0.125f;
                    pml.origin[2] = pm->s.origin[2] * 0.125f;
                    VectorCopy(pm->s.origin, pml.previous_origin);
                    return;
                }
            }
        }
    }
}

/*
================
PM_ClampAngles

================
*/
static void PM_ClampAngles(void)
{
    short   temp;
    int     i;

    if (pm->s.pm_flags & PMF_TIME_TELEPORT) {
        pm->viewangles[YAW] = SHORT2ANGLE(pm->cmd.angles[YAW] + pm->s.delta_angles[YAW]);
        pm->viewangles[PITCH] = 0;
        pm->viewangles[ROLL] = 0;
    } else {
        // circularly clamp the angles with deltas
        for (i = 0; i < 3; i++) {
            temp = pm->cmd.angles[i] + pm->s.delta_angles[i];
            pm->viewangles[i] = SHORT2ANGLE(temp);
        }

        // don't let the player look up or down more than 90 degrees
        clamp(pm->viewangles[PITCH], -89, 89);
    }
    AngleVectors(pm->viewangles, pml.forward, pml.right, pml.up);
}

/*
================
PM_DropTimers

Counts pm_time down (8 ms units) and clears the timer flags when it runs out.
================
*/
static void PM_DropTimers(void)
{
    int     msec;

    if (!pm->s.pm_time)
        return;

    msec = pm->cmd.msec >> 3;
    if (!msec)
        msec = 1;
    if (msec >= pm->s.pm_time) {
        pm->s.pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND | PMF_TIME_TELEPORT);
        pm->s.pm_time = 0;
    } else
        pm->s.pm_time -= msec;
}

/*
================
PM_WaterJumpMove

Out of the water onto a ledge: no control, but falls.
================
*/
static void PM_WaterJumpMove(void)
{
    pml.velocity[2] -= pm->s.gravity * pml.frametime;
    if (pml.velocity[2] < 0) {
        // cancel as soon as we are falling down again
        pm->s.pm_flags &= ~(PMF_TIME_WATERJUMP | PMF_TIME_LAND | PMF_TIME_TELEPORT);
        pm->s.pm_time = 0;
    }

    PM_StepSlideMove();
}

/*
==============================================================================

QUAKE III ARENA MOVEMENT (pmoveParams_t.q3move)

bg_pmove.c and bg_slidemove.c, for the bot match's "physics: Quake 3": how
the player accelerates, slides, steps, jumps and lands. Quake III's numbers -
320 ups, ground acceleration 10 but air acceleration only 1 (so strafe- and
circle-jumping build speed), a quarter speed crouched and in the air too, half
speed swimming at acceleration 4, a jump of exactly 270 with no wait after
landing, and OVERCLIP so slopes and steps don't eat speed. While the game's
knockback timer runs (PMF_TIME_LAND here, see pmove.h) there is no ground
friction and only air control, so a rocket's push carries.

Quake II's player box and view heights stay, as do its ladders, water jump,
teleport pause, conveyors and water currents, and the Quake III map powerups.
==============================================================================
*/

#define PMF_TIME_KNOCKBACK  PMF_TIME_LAND

#define Q3_OVERCLIP         1.001f
#define Q3_SPEED            320.0f
#define Q3_DUCKSCALE        0.25f
#define Q3_SWIMSCALE        0.5f
#define Q3_ACCELERATE       10.0f
#define Q3_AIRACCELERATE    1.0f
#define Q3_WATERACCELERATE  4.0f
#define Q3_FRICTION         6.0f
#define Q3_WATERFRICTION    1.0f
#define Q3_JUMP_VELOCITY    270.0f
#define Q3_MIN_WALK_NORMAL  0.7f

// A full run in the move command: the client clamps each axis to this
// (input.c CL_ClampSpeed) and the bots send it. Quake III spells it 127.
#define Q3_CMD_FULL         400

static void PM_AddTouch(trace_t *trace)
{
    if (pm->numtouch < MAXTOUCH && trace->ent) {
        pm->touchents[pm->numtouch] = trace->ent;
        pm->numtouch++;
    }
}

/*
==================
PM_Q3ClipVelocity

Slide off an impacting surface. Unlike Quake II's, a move AWAY from the plane
is shrunk by the overbounce instead of grown, and there is no epsilon snap.
==================
*/
static void PM_Q3ClipVelocity(const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
    float   backoff;
    int     i;

    backoff = DotProduct(in, normal);
    if (backoff < 0)
        backoff *= overbounce;
    else
        backoff /= overbounce;

    for (i = 0; i < 3; i++)
        out[i] = in[i] - normal[i] * backoff;
}

/*
============
PM_Q3CmdScale

The scale that turns the move command into a velocity, so that diagonal moves
are no faster than straight ones and a full run is exactly the player's speed.
============
*/
static float PM_Q3CmdScale(void)
{
    float   f = pm->cmd.forwardmove;
    float   s = pm->cmd.sidemove;
    float   u = pm->cmd.upmove;
    float   max, total;

    max = fabsf(f);
    if (fabsf(s) > max)
        max = fabsf(s);
    if (fabsf(u) > max)
        max = fabsf(u);
    if (!max)
        return 0;
    if (max > Q3_CMD_FULL)
        max = Q3_CMD_FULL;

    total = sqrtf(f * f + s * s + u * u);
    return Q3_SPEED * pml.speedscale * max / (Q3_CMD_FULL * total);
}

/*
==================
PM_Q3Friction
==================
*/
static void PM_Q3Friction(void)
{
    float   *vel = pml.velocity;
    vec3_t  vec;
    float   speed, newspeed, control, drop;

    VectorCopy(vel, vec);
    if (pml.walking)
        vec[2] = 0;     // ignore slope movement

    speed = VectorLength(vec);
    if (speed < 1) {
        vel[0] = 0;
        vel[1] = 0;     // allow sinking underwater
        return;
    }

    drop = 0;

    // ground friction, unless the ground is slick or a hit is still pushing us
    if (pm->waterlevel <= 1 && pml.walking &&
        !(pml.groundsurface && (pml.groundsurface->flags & SURF_SLICK)) &&
        !(pm->s.pm_flags & PMF_TIME_KNOCKBACK)) {
        control = speed < pm_stopspeed ? pm_stopspeed : speed;
        drop += control * Q3_FRICTION * pml.frametime;
    }

    // water friction, even when just wading
    if (pm->waterlevel)
        drop += speed * Q3_WATERFRICTION * pm->waterlevel * pml.frametime;

    newspeed = speed - drop;
    if (newspeed < 0)
        newspeed = 0;
    newspeed /= speed;

    VectorScale(vel, newspeed, vel);
}

/*
==================
PM_Q3SlideMove

bg_slidemove.c. Returns true if the velocity was clipped at all. With gravity,
the move uses the average of the start and end vertical speeds, which is what
makes Quake III's jump arcs exact whatever the frame rate.
==================
*/
static bool PM_Q3SlideMove(bool gravity)
{
    int         bumpcount, numbumps = 4;
    vec3_t      dir;
    float       d;
    int         numplanes;
    vec3_t      planes[MAX_CLIP_PLANES];
    vec3_t      primal_velocity;
    vec3_t      clipVelocity;
    int         i, j, k;
    trace_t     trace;
    vec3_t      end;
    float       time_left;
    float       into;
    vec3_t      endVelocity;
    vec3_t      endClipVelocity;

    VectorCopy(pml.velocity, primal_velocity);
    VectorCopy(pml.velocity, endVelocity);

    if (gravity) {
        endVelocity[2] -= pm->s.gravity * pml.frametime;
        pml.velocity[2] = (pml.velocity[2] + endVelocity[2]) * 0.5f;
        primal_velocity[2] = endVelocity[2];
        if (pml.onplane) {
            // slide along the ground plane
            PM_Q3ClipVelocity(pml.velocity, pml.groundplane.normal, pml.velocity, Q3_OVERCLIP);
        }
    }

    time_left = pml.frametime;

    // never turn against the ground plane
    if (pml.onplane) {
        numplanes = 1;
        VectorCopy(pml.groundplane.normal, planes[0]);
    } else {
        numplanes = 0;
    }

    // never turn against original velocity
    VectorNormalize2(pml.velocity, planes[numplanes]);
    numplanes++;

    for (bumpcount = 0; bumpcount < numbumps; bumpcount++) {
        // calculate position we are trying to move to
        VectorMA(pml.origin, time_left, pml.velocity, end);

        // see if we can make it there
        trace = pm->trace(pml.origin, pm->mins, pm->maxs, end);

        if (trace.allsolid) {
            // entity is completely trapped in another solid
            pml.velocity[2] = 0;    // don't build up falling damage, but allow sideways acceleration
            return true;
        }

        if (trace.fraction > 0) {
            // actually covered some distance
            VectorCopy(trace.endpos, pml.origin);
        }

        if (trace.fraction == 1)
            break;      // moved the entire distance

        // save entity for contact
        PM_AddTouch(&trace);

        time_left -= time_left * trace.fraction;

        if (numplanes >= MAX_CLIP_PLANES) {
            // this shouldn't really happen
            VectorClear(pml.velocity);
            return true;
        }

        // if this is the same plane we hit before, nudge velocity out along
        // it, which fixes some epsilon issues with non-axial planes
        for (i = 0; i < numplanes; i++) {
            if (DotProduct(trace.plane.normal, planes[i]) > 0.99f) {
                VectorAdd(trace.plane.normal, pml.velocity, pml.velocity);
                break;
            }
        }
        if (i < numplanes)
            continue;

        VectorCopy(trace.plane.normal, planes[numplanes]);
        numplanes++;

        // modify velocity so it parallels all of the clip planes

        // find a plane that it enters
        for (i = 0; i < numplanes; i++) {
            into = DotProduct(pml.velocity, planes[i]);
            if (into >= 0.1f)
                continue;       // move doesn't interact with the plane

            // slide along the plane
            PM_Q3ClipVelocity(pml.velocity, planes[i], clipVelocity, Q3_OVERCLIP);
            PM_Q3ClipVelocity(endVelocity, planes[i], endClipVelocity, Q3_OVERCLIP);

            // see if there is a second plane that the new move enters
            for (j = 0; j < numplanes; j++) {
                if (j == i)
                    continue;
                if (DotProduct(clipVelocity, planes[j]) >= 0.1f)
                    continue;   // move doesn't interact with the plane

                // try clipping the move to the plane
                PM_Q3ClipVelocity(clipVelocity, planes[j], clipVelocity, Q3_OVERCLIP);
                PM_Q3ClipVelocity(endClipVelocity, planes[j], endClipVelocity, Q3_OVERCLIP);

                // see if it goes back into the first clip plane
                if (DotProduct(clipVelocity, planes[i]) >= 0)
                    continue;

                // slide the original velocity along the crease
                CrossProduct(planes[i], planes[j], dir);
                VectorNormalize(dir);
                d = DotProduct(dir, pml.velocity);
                VectorScale(dir, d, clipVelocity);

                d = DotProduct(dir, endVelocity);
                VectorScale(dir, d, endClipVelocity);

                // see if there is a third plane the the new move enters
                for (k = 0; k < numplanes; k++) {
                    if (k == i || k == j)
                        continue;
                    if (DotProduct(clipVelocity, planes[k]) >= 0.1f)
                        continue;   // move doesn't interact with the plane

                    // stop dead at a triple plane interaction
                    VectorClear(pml.velocity);
                    return true;
                }
            }

            // if we have fixed all interactions, try another move
            VectorCopy(clipVelocity, pml.velocity);
            VectorCopy(endClipVelocity, endVelocity);
            break;
        }
    }

    if (gravity)
        VectorCopy(endVelocity, pml.velocity);

    // don't change velocity if in a timer: a knockback's push is kept whole
    if (pm->s.pm_time)
        VectorCopy(primal_velocity, pml.velocity);

    return bumpcount != 0;
}

/*
==================
PM_Q3StepSlideMove
==================
*/
static void PM_Q3StepSlideMove(bool gravity)
{
    vec3_t      start_o, start_v;
    vec3_t      up, down;
    trace_t     trace;
    float       stepSize;

    VectorCopy(pml.origin, start_o);
    VectorCopy(pml.velocity, start_v);

    if (!PM_Q3SlideMove(gravity))
        return;     // we got exactly where we wanted to go first try

    VectorCopy(start_o, down);
    down[2] -= STEPSIZE;
    trace = pm->trace(start_o, pm->mins, pm->maxs, down);
    // never step up when you still have up velocity
    if (pml.velocity[2] > 0 && (trace.fraction == 1.0f || trace.plane.normal[2] < 0.7f))
        return;

    VectorCopy(start_o, up);
    up[2] += STEPSIZE;

    // test the player position if they were a stepheight higher
    trace = pm->trace(start_o, pm->mins, pm->maxs, up);
    if (trace.allsolid)
        return;     // can't step up

    stepSize = trace.endpos[2] - start_o[2];

    // try slidemove from this position
    VectorCopy(trace.endpos, pml.origin);
    VectorCopy(start_v, pml.velocity);

    PM_Q3SlideMove(gravity);

    // push down the final amount
    VectorCopy(pml.origin, down);
    down[2] -= stepSize;
    trace = pm->trace(pml.origin, pm->mins, pm->maxs, down);
    if (!trace.allsolid)
        VectorCopy(trace.endpos, pml.origin);
    if (trace.fraction < 1.0f)
        PM_Q3ClipVelocity(pml.velocity, trace.plane.normal, pml.velocity, Q3_OVERCLIP);
}

/*
=============
PM_Q3CheckJump
=============
*/
static bool PM_Q3CheckJump(void)
{
    if (pm->cmd.upmove < 10)
        return false;   // not holding jump

    // must wait for jump to be released
    if (pm->s.pm_flags & PMF_JUMP_HELD) {
        // clear upmove so cmdscale doesn't lower running speed
        pm->cmd.upmove = 0;
        return false;
    }

    if (pm->s.pm_type == PM_DEAD)
        return false;

    pml.onplane = false;    // jumping away
    pml.walking = false;
    pm->s.pm_flags |= PMF_JUMP_HELD;
    pm->s.pm_flags &= ~PMF_ON_GROUND;
    pm->groundentity = NULL;
    pml.velocity[2] = Q3_JUMP_VELOCITY;
    return true;
}

static void PM_Q3NotOnGround(bool onplane)
{
    pm->groundentity = NULL;
    pm->s.pm_flags &= ~PMF_ON_GROUND;
    pml.onplane = onplane;
    pml.walking = false;
}

/*
=============
PM_Q3CorrectAllSolid

The ground trace started in solid: look for a free spot a unit away and trace
the ground from the real origin again if there is one. As in Quake III, the
player is not moved.
=============
*/
static bool PM_Q3CorrectAllSolid(trace_t *trace)
{
    vec3_t  point;
    int     i, j, k;

    for (i = -1; i <= 1; i++) {
        for (j = -1; j <= 1; j++) {
            for (k = -1; k <= 1; k++) {
                VectorCopy(pml.origin, point);
                point[0] += i;
                point[1] += j;
                point[2] += k;
                *trace = pm->trace(point, pm->mins, pm->maxs, point);
                if (!trace->allsolid) {
                    VectorCopy(pml.origin, point);
                    point[2] -= 0.25f;
                    *trace = pm->trace(pml.origin, pm->mins, pm->maxs, point);
                    return true;
                }
            }
        }
    }

    PM_Q3NotOnGround(false);
    return false;
}

/*
=============
PM_Q3GroundTrace
=============
*/
static void PM_Q3GroundTrace(void)
{
    vec3_t      point;
    trace_t     trace;

    VectorCopy(pml.origin, point);
    point[2] -= 0.25f;
    trace = pm->trace(pml.origin, pm->mins, pm->maxs, point);

    // do something corrective if the trace starts in a solid
    if (trace.allsolid && !PM_Q3CorrectAllSolid(&trace))
        return;

    pml.groundplane = trace.plane;
    pml.groundsurface = trace.surface;
    pml.groundcontents = trace.contents;

    // if the trace didn't hit anything, we are in free fall
    if (trace.fraction == 1.0f || !trace.ent) {
        PM_Q3NotOnGround(false);
        return;
    }

    // check if getting thrown off the ground
    if (pml.velocity[2] > 0 && DotProduct(pml.velocity, trace.plane.normal) > 10) {
        PM_Q3NotOnGround(false);
        return;
    }

    // slopes that are too steep will not be considered onground
    if (trace.plane.normal[2] < Q3_MIN_WALK_NORMAL) {
        PM_Q3NotOnGround(true);
        return;
    }

    pml.onplane = true;
    pml.walking = true;

    // hitting solid ground will end a waterjump
    if (pm->s.pm_flags & PMF_TIME_WATERJUMP) {
        pm->s.pm_flags &= ~PMF_TIME_WATERJUMP;
        pm->s.pm_time = 0;
    }

    // no landing delay: Quake III jumps again the moment it lands
    pm->s.pm_flags |= PMF_ON_GROUND;
    pm->groundentity = trace.ent;

    PM_AddTouch(&trace);
}

static void PM_Q3WaterMove(void);
static void PM_Q3AirMove(void);

/*
===================
PM_Q3Wish

The command as a wish direction and speed, with Quake II's conveyors and water
currents added on top. `wishvel` comes in unscaled.
===================
*/
static float PM_Q3Wish(vec3_t wishvel, float scale, vec3_t wishdir)
{
    float   wishspeed, max = Q3_SPEED * pml.speedscale;

    VectorScale(wishvel, scale, wishvel);
    PM_AddCurrents(wishvel);

    VectorCopy(wishvel, wishdir);
    wishspeed = VectorNormalize(wishdir);
    if (wishspeed > max)
        wishspeed = max;
    return wishspeed;
}

/*
===================
PM_Q3WalkMove
===================
*/
static void PM_Q3WalkMove(void)
{
    vec3_t      wishvel, wishdir;
    float       wishspeed, scale, accelerate, vel, speed;
    bool        slick;
    int         i;

    if (pm->waterlevel > 2 && DotProduct(pml.forward, pml.groundplane.normal) > 0) {
        // begin swimming
        PM_Q3WaterMove();
        return;
    }

    if (PM_Q3CheckJump()) {
        // jumped away
        if (pm->waterlevel > 1)
            PM_Q3WaterMove();
        else
            PM_Q3AirMove();
        return;
    }

    PM_Q3Friction();

    scale = PM_Q3CmdScale();

    // project moves down to flat plane
    pml.forward[2] = 0;
    pml.right[2] = 0;

    // project the forward and right directions onto the ground plane
    PM_Q3ClipVelocity(pml.forward, pml.groundplane.normal, pml.forward, Q3_OVERCLIP);
    PM_Q3ClipVelocity(pml.right, pml.groundplane.normal, pml.right, Q3_OVERCLIP);
    VectorNormalize(pml.forward);
    VectorNormalize(pml.right);

    // when going up or down slopes the wish velocity should not be zero
    for (i = 0; i < 3; i++)
        wishvel[i] = pml.forward[i] * pm->cmd.forwardmove + pml.right[i] * pm->cmd.sidemove;
    wishspeed = PM_Q3Wish(wishvel, scale, wishdir);

    speed = Q3_SPEED * pml.speedscale;

    // clamp the speed lower if ducking
    if ((pm->s.pm_flags & PMF_DUCKED) && wishspeed > speed * Q3_DUCKSCALE)
        wishspeed = speed * Q3_DUCKSCALE;

    // clamp the speed lower if wading or walking on the bottom
    if (pm->waterlevel) {
        float   waterScale = 1.0f - (1.0f - Q3_SWIMSCALE) * pm->waterlevel / 3.0f;

        if (wishspeed > speed * waterScale)
            wishspeed = speed * waterScale;
    }

    // when a player gets hit, they temporarily lose full control, which
    // allows them to be moved a bit
    slick = pml.groundsurface && (pml.groundsurface->flags & SURF_SLICK);
    if (slick || (pm->s.pm_flags & PMF_TIME_KNOCKBACK))
        accelerate = Q3_AIRACCELERATE;
    else
        accelerate = Q3_ACCELERATE;

    PM_Accelerate(wishdir, wishspeed, accelerate);

    if (slick || (pm->s.pm_flags & PMF_TIME_KNOCKBACK))
        pml.velocity[2] -= pm->s.gravity * pml.frametime;

    vel = VectorLength(pml.velocity);

    // slide along the ground plane
    PM_Q3ClipVelocity(pml.velocity, pml.groundplane.normal, pml.velocity, Q3_OVERCLIP);

    // don't decrease velocity when going up or down a slope
    VectorNormalize(pml.velocity);
    VectorScale(pml.velocity, vel, pml.velocity);

    // don't do anything if standing still
    if (!pml.velocity[0] && !pml.velocity[1])
        return;

    PM_Q3StepSlideMove(false);
}

/*
===================
PM_Q3AirMove
===================
*/
static void PM_Q3AirMove(void)
{
    vec3_t      wishvel, wishdir;
    float       wishspeed, scale;
    int         i;

    PM_Q3Friction();

    scale = PM_Q3CmdScale();

    // project moves down to flat plane
    pml.forward[2] = 0;
    pml.right[2] = 0;
    VectorNormalize(pml.forward);
    VectorNormalize(pml.right);

    for (i = 0; i < 2; i++)
        wishvel[i] = pml.forward[i] * pm->cmd.forwardmove + pml.right[i] * pm->cmd.sidemove;
    wishvel[2] = 0;
    wishspeed = PM_Q3Wish(wishvel, scale, wishdir);

    // not on ground, so little effect on velocity
    PM_Accelerate(wishdir, wishspeed, Q3_AIRACCELERATE);

    // we may have a ground plane that is very steep, even though we don't
    // have a groundentity: slide along the steep plane
    if (pml.onplane)
        PM_Q3ClipVelocity(pml.velocity, pml.groundplane.normal, pml.velocity, Q3_OVERCLIP);

    PM_Q3StepSlideMove(true);
}

/*
===================
PM_Q3WaterMove
===================
*/
static void PM_Q3WaterMove(void)
{
    vec3_t      wishvel, wishdir;
    float       wishspeed, scale, vel;
    int         i;

    PM_Q3Friction();

    scale = PM_Q3CmdScale();

    if (!scale) {
        VectorSet(wishvel, 0, 0, -60);  // sink towards bottom
        scale = 1;
    } else {
        for (i = 0; i < 3; i++)
            wishvel[i] = pml.forward[i] * pm->cmd.forwardmove + pml.right[i] * pm->cmd.sidemove;
        wishvel[2] += pm->cmd.upmove;
    }
    wishspeed = PM_Q3Wish(wishvel, scale, wishdir);

    if (wishspeed > Q3_SPEED * pml.speedscale * Q3_SWIMSCALE)
        wishspeed = Q3_SPEED * pml.speedscale * Q3_SWIMSCALE;

    PM_Accelerate(wishdir, wishspeed, Q3_WATERACCELERATE);

    // make sure we can go up slopes easily under water
    if (pml.onplane && DotProduct(pml.velocity, pml.groundplane.normal) < 0) {
        vel = VectorLength(pml.velocity);
        // slide along the ground plane
        PM_Q3ClipVelocity(pml.velocity, pml.groundplane.normal, pml.velocity, Q3_OVERCLIP);
        VectorNormalize(pml.velocity);
        VectorScale(pml.velocity, vel, pml.velocity);
    }

    PM_Q3SlideMove(false);
}

/*
================
PM_Q3Pmove

PmoveSingle's order: duck, water, ground, then one move by what the player
is doing, then ground and water again for the final spot.
================
*/
static void PM_Q3Pmove(void)
{
    if (pm->cmd.upmove < 10)
        pm->s.pm_flags &= ~PMF_JUMP_HELD;   // not holding jump

    // set mins, maxs, and viewheight
    PM_CheckDuck();

    if (pm->snapinitial)
        PM_InitialSnapPosition();

    PM_SetWaterLevel();
    PM_Q3GroundTrace();

    if (pm->s.pm_type == PM_DEAD)
        PM_DeadMove();

    // Quake II's ladders and water jump
    PM_CheckSpecialMovement();

    PM_DropTimers();

    if (pm->s.pm_flags & PMF_TIME_TELEPORT) {
        // teleport pause stays exactly in place
    } else if (pml.q3_flight) {
        PM_Q3FlightMove();
    } else if (pm->s.pm_flags & PMF_TIME_WATERJUMP) {
        PM_WaterJumpMove();
    } else if (pml.ladder) {
        // Quake II's climbing
        PM_Friction();
        PM_AirMove();
    } else if (pm->waterlevel > 1) {
        // swimming
        PM_Q3WaterMove();
    } else if (pml.walking) {
        // walking on ground
        PM_Q3WalkMove();
    } else {
        // airborne
        PM_Q3AirMove();
    }

    // set groundentity, watertype, and waterlevel for final spot
    PM_Q3GroundTrace();
    PM_SetWaterLevel();
}

/*
================
Pmove

Can be called by either the server or the client
================
*/
void Pmove(pmove_t *pmove, pmoveParams_t *params)
{
    pm = pmove;
    pmp = params;

    // clear results
    pm->numtouch = 0;
    VectorClear(pm->viewangles);
    pm->viewheight = 0;
    pm->groundentity = NULL;
    pm->watertype = 0;
    pm->waterlevel = 0;

    // clear all pmove local vars
    memset(&pml, 0, sizeof(pml));

    // convert origin and velocity to float values
    VectorScale(pm->s.origin, 0.125f, pml.origin);
    VectorScale(pm->s.velocity, 0.125f, pml.velocity);

    // save old org in case we get stuck
    VectorCopy(pm->s.origin, pml.previous_origin);

    PM_ClampAngles();

    if (pm->s.pm_type == PM_SPECTATOR) {
        pml.frametime = pmp->speedmult * pm->cmd.msec * 0.001f;
        PM_FlyMove();
        PM_SnapPosition();
        return;
    }

    pml.frametime = pm->cmd.msec * 0.001f;

    // Quake III Haste and Flight, and Team Arena's Scout, carried in the
    // gravity (pmoveParams_t.q3): bit 0 Haste, bit 1 Scout (1.5, in place of
    // Haste's 1.3 - ClientThink_real's else-if), the rest 0 for Flight
    pml.speedscale = 1;
    if (pmp->q3 && pm->s.pm_type == PM_NORMAL) {
        if (pm->s.gravity & 2)
            pml.speedscale = 1.5f;
        else if (pm->s.gravity & 1)
            pml.speedscale = 1.3f;
        pml.q3_flight = (pm->s.gravity & ~3) == 0;
    }

    if (pm->s.pm_type >= PM_DEAD) {
        pm->cmd.forwardmove = 0;
        pm->cmd.sidemove = 0;
        pm->cmd.upmove = 0;
    }

    if (pm->s.pm_type == PM_FREEZE)
        return;     // no movement at all

    if (pmp->q3move) {
        PM_Q3Pmove();
        PM_SnapPosition();
        return;
    }

    // set mins, maxs, and viewheight
    PM_CheckDuck();

    if (pm->snapinitial)
        PM_InitialSnapPosition();

    // set groundentity, watertype, and waterlevel
    PM_CategorizePosition();

    if (pm->s.pm_type == PM_DEAD)
        PM_DeadMove();

    PM_CheckSpecialMovement();

    PM_DropTimers();

    if (pm->s.pm_flags & PMF_TIME_TELEPORT) {
        // teleport pause stays exactly in place
    } else if (pml.q3_flight) {
        PM_Q3FlightMove();
    } else if (pm->s.pm_flags & PMF_TIME_WATERJUMP) {
        PM_WaterJumpMove();
    } else {
        PM_CheckJump();

        PM_Friction();

        if (pm->waterlevel >= 2)
            PM_WaterMove();
        else {
            vec3_t  angles;

            VectorCopy(pm->viewangles, angles);
            if (angles[PITCH] > 180)
                angles[PITCH] = angles[PITCH] - 360;
            angles[PITCH] /= 3;

            AngleVectors(angles, pml.forward, pml.right, pml.up);

            PM_AirMove();
        }
    }

    // set groundentity, watertype, and waterlevel for final spot
    PM_CategorizePosition();

    PM_SnapPosition();
}

void PmoveInit(pmoveParams_t *pmp)
{
    // set up default pmove parameters
    memset(pmp, 0, sizeof(*pmp));

    pmp->speedmult = 1;
    pmp->watermult = 0.5f;
    pmp->maxspeed = 300;
    pmp->friction = 6;
    pmp->waterfriction = 1;
    pmp->flyfriction = 9;
}

void PmoveEnableQW(pmoveParams_t *pmp)
{
    pmp->qwmode = true;
    pmp->watermult = 0.7f;
    pmp->maxspeed = 320;
    //pmp->upspeed = (sv_qwmod->integer > 1) ? 310 : 350;
    pmp->friction = 4;
    pmp->waterfriction = 4;
    pmp->airaccelerate = true;
}

