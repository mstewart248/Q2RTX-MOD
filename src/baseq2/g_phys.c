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
// g_phys.c

#include "g_local.h"

/*


pushmove objects do not obey gravity, and do not interact with each other or trigger fields, but block normal movement and push normal objects when they move.

onground is set for toss objects when they come to a complete rest.  it is set for steping or walking objects

doors, plats, etc are SOLID_BSP, and MOVETYPE_PUSH
bonus items are SOLID_TRIGGER touch, and MOVETYPE_TOSS
corpses are SOLID_NOT and MOVETYPE_TOSS
crates are SOLID_BBOX and MOVETYPE_TOSS
walking monsters are SOLID_SLIDEBOX and MOVETYPE_STEP
flying/floating monsters are SOLID_SLIDEBOX and MOVETYPE_FLY

solid_edge items only clip against bsp models.

*/


/*
============
SV_TestEntityPosition

============
*/
edict_t *SV_TestEntityPosition(edict_t *ent)
{
    trace_t trace;
    int     mask;

    if (ent->clipmask)
        mask = ent->clipmask;
    else
        mask = MASK_SOLID;
    trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, ent->s.origin, ent, mask);

    if (trace.startsolid)
        return g_edicts;

    return NULL;
}


/*
================
SV_CheckVelocity
================
*/
void SV_CheckVelocity(edict_t *ent)
{
    int     i;

//
// bound velocity
//
    for (i = 0 ; i < 3 ; i++) {
        if (ent->velocity[i] > sv_maxvelocity->value)
            ent->velocity[i] = sv_maxvelocity->value;
        else if (ent->velocity[i] < -sv_maxvelocity->value)
            ent->velocity[i] = -sv_maxvelocity->value;
    }
}

/*
=============
SV_RunThink

Runs thinking code for this frame if necessary
=============
*/
bool SV_RunThink(edict_t *ent)
{
    int     thinktime;

    thinktime = ent->nextthink;
    if (thinktime <= 0)
        return true;
    if (thinktime > level.framenum)
        return true;

    ent->nextthink = 0;
    if (!ent->think)
        gi.error("NULL ent->think");
    ent->think(ent);

    return false;
}

/*
==================
SV_Impact

Two entities have touched, so run their touch functions
==================
*/
void SV_Impact(edict_t *e1, trace_t *trace)
{
    edict_t     *e2;
//  cplane_t    backplane;

    e2 = trace->ent;

    if (e1->touch && e1->solid != SOLID_NOT)
        e1->touch(e1, e2, &trace->plane, trace->surface);

    if (e2->touch && e2->solid != SOLID_NOT)
        e2->touch(e2, e1, NULL, NULL);
}


/*
==================
ClipVelocity

Slide off of the impacting object
returns the blocked flags (1 = floor, 2 = step / wall)
==================
*/
#define STOP_EPSILON    0.1f

int ClipVelocity(vec3_t in, vec3_t normal, vec3_t out, float overbounce)
{
    float   backoff;
    float   change;
    int     i, blocked;

    blocked = 0;
    if (normal[2] > 0)
        blocked |= 1;       // floor
    if (!normal[2])
        blocked |= 2;       // step

    backoff = DotProduct(in, normal) * overbounce;

    for (i = 0 ; i < 3 ; i++) {
        change = normal[i] * backoff;
        out[i] = in[i] - change;
        if (out[i] > -STOP_EPSILON && out[i] < STOP_EPSILON)
            out[i] = 0;
    }

    return blocked;
}


/*
============
SV_FlyMove

The basic solid body movement clip that slides along multiple planes
Returns the clipflags if the velocity was modified (hit something solid)
1 = floor
2 = wall / step
4 = dead stop
============
*/
#define MAX_CLIP_PLANES 5
int SV_FlyMove(edict_t *ent, float time, int mask)
{
    edict_t     *hit;
    int         bumpcount, numbumps;
    vec3_t      dir;
    float       d;
    int         numplanes;
    vec3_t      planes[MAX_CLIP_PLANES];
    vec3_t      primal_velocity, original_velocity, new_velocity;
    int         i, j;
    trace_t     trace;
    vec3_t      end;
    float       time_left;
    int         blocked;

    numbumps = 4;

    blocked = 0;
    VectorCopy(ent->velocity, original_velocity);
    VectorCopy(ent->velocity, primal_velocity);
    numplanes = 0;

    time_left = time;

    ent->groundentity = NULL;
    for (bumpcount = 0 ; bumpcount < numbumps ; bumpcount++) {
        for (i = 0 ; i < 3 ; i++)
            end[i] = ent->s.origin[i] + time_left * ent->velocity[i];

        trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, end, ent, mask);

        if (trace.allsolid) {
            // entity is trapped in another solid
            VectorClear(ent->velocity);
            return 3;
        }

        if (trace.fraction > 0) {
            // actually covered some distance
            VectorCopy(trace.endpos, ent->s.origin);
            VectorCopy(ent->velocity, original_velocity);
            numplanes = 0;
        }

        if (trace.fraction == 1)
            break;     // moved the entire distance

        hit = trace.ent;

        if (trace.plane.normal[2] > 0.7f) {
            blocked |= 1;       // floor
            if (hit->solid == SOLID_BSP) {
                ent->groundentity = hit;
                ent->groundentity_linkcount = hit->linkcount;
            }
        }
        if (!trace.plane.normal[2]) {
            blocked |= 2;       // step
        }

//
// run the impact function
//
        SV_Impact(ent, &trace);
        if (!ent->inuse)
            break;      // removed by the impact function


        time_left -= time_left * trace.fraction;

        // cliped to another plane
        if (numplanes >= MAX_CLIP_PLANES) {
            // this shouldn't really happen
            VectorClear(ent->velocity);
            return 3;
        }

        VectorCopy(trace.plane.normal, planes[numplanes]);
        numplanes++;

//
// modify original_velocity so it parallels all of the clip planes
//
        for (i = 0 ; i < numplanes ; i++) {
            ClipVelocity(original_velocity, planes[i], new_velocity, 1);

            for (j = 0 ; j < numplanes ; j++)
                if ((j != i) && !VectorCompare(planes[i], planes[j])) {
                    if (DotProduct(new_velocity, planes[j]) < 0)
                        break;  // not ok
                }
            if (j == numplanes)
                break;
        }

        if (i != numplanes) {
            // go along this plane
            VectorCopy(new_velocity, ent->velocity);
        } else {
            // go along the crease
            if (numplanes != 2) {
//              gi.dprintf ("clip velocity, numplanes == %i\n",numplanes);
                VectorClear(ent->velocity);
                return 7;
            }
            CrossProduct(planes[0], planes[1], dir);
            d = DotProduct(dir, ent->velocity);
            VectorScale(dir, d, ent->velocity);
        }

//
// if original velocity is against the original velocity, stop dead
// to avoid tiny occilations in sloping corners
//
        if (DotProduct(ent->velocity, primal_velocity) <= 0) {
            VectorClear(ent->velocity);
            return blocked;
        }
    }

    return blocked;
}


/*
============
SV_AddGravity

============
*/
void SV_AddGravity(edict_t *ent)
{
	if (ent->movetype == MOVETYPE_EXPLODE) {
		ent->velocity[2] -= (ent->gravity * sv_gravity->value * FRAMETIME) / 3;
	}
	else if (ent->gravityVector[2] > 0) {
		// ROGUE - inverted gravity (a stalker on the ceiling). Pull along the
		// entity's own gravity vector instead of straight down.
		VectorMA(ent->velocity, ent->gravity * sv_gravity->value * FRAMETIME,
			ent->gravityVector, ent->velocity);
	}
	else {
		ent->velocity[2] -= ent->gravity * sv_gravity->value * FRAMETIME;
	}
}

/*
===============================================================================

PUSHMOVE

===============================================================================
*/

/*
============
SV_PushEntity

Does not change the entities velocity at all
============
*/
trace_t SV_PushEntity(edict_t *ent, vec3_t push)
{
    trace_t trace;
    vec3_t  start;
    vec3_t  end;
    int     mask;

    VectorCopy(ent->s.origin, start);
    VectorAdd(start, push, end);

retry:
    if (ent->clipmask)
        mask = ent->clipmask;
    else
        mask = MASK_SOLID;

    trace = gi.trace(start, ent->mins, ent->maxs, end, ent, mask);

    // An entity that begins the move completely buried in solid comes back
    // allsolid.  `map_allsolid_bug` defaults to 1 to reproduce 1997 Q2, and that
    // path deliberately leaves fraction at 1 with endpos == end - so taking the
    // result at face value TELEPORTS the entity the whole way, straight through
    // the world, and it will do it again next frame from further in.  Under
    // SV_Physics_Toss that is the corpse-falls-forever bug: nothing ever clips
    // it, gravity keeps stacking onto the velocity, and the origin wraps the
    // network encoding's +/-4096 range so the body appears to drop out of the
    // sky over and over for the rest of the map.
    //
    // SV_FlyMove has always tested allsolid (see SV_Physics_Step, which is why
    // this never bit a monster while it was still alive); the toss path never
    // did.  Refuse the move and let the caller decide what being stuck means.
    if (trace.allsolid)
        VectorCopy(start, ent->s.origin);
    else
        VectorCopy(trace.endpos, ent->s.origin);
    gi.linkentity(ent);

    if (!trace.allsolid && trace.fraction != 1.0f) {
        SV_Impact(ent, &trace);

        // if the pushed entity went away and the pusher is still there
        if (!trace.ent->inuse && ent->inuse) {
            // move the pusher back and try again
            VectorCopy(start, ent->s.origin);
            gi.linkentity(ent);
            goto retry;
        }
    }

    if (ent->inuse)
        G_TouchTriggers(ent);

    return trace;
}


typedef struct {
    edict_t *ent;
    vec3_t  origin;
    vec3_t  angles;
#if USE_SMOOTH_DELTA_ANGLES
    int     deltayaw;
#endif
} pushed_t;
pushed_t    pushed[MAX_EDICTS], *pushed_p;

edict_t *obstacle;

/*
============
SV_Push

Objects need to be moved back on a failed push,
otherwise riders would continue to slide.
============
*/
bool SV_Push(edict_t *pusher, vec3_t move, vec3_t amove)
{
    int         i, e;
    edict_t     *check, *block;
    vec3_t      mins, maxs;
    pushed_t    *p;
    vec3_t      org, org2, move2, forward, right, up;

    // clamp the move to 1/8 units, so the position will
    // be accurate for client side prediction
    for (i = 0 ; i < 3 ; i++) {
        float   temp;
        temp = move[i] * 8.0f;
        if (temp > 0.0f)
            temp += 0.5f;
        else
            temp -= 0.5f;
        move[i] = 0.125f * (int)temp;
    }

    // find the bounding box
    for (i = 0 ; i < 3 ; i++) {
        mins[i] = pusher->absmin[i] + move[i];
        maxs[i] = pusher->absmax[i] + move[i];
    }

// we need this for pushing things later
    VectorNegate(amove, org);
    AngleVectors(org, forward, right, up);

// save the pusher's original position
    pushed_p->ent = pusher;
    VectorCopy(pusher->s.origin, pushed_p->origin);
    VectorCopy(pusher->s.angles, pushed_p->angles);
#if USE_SMOOTH_DELTA_ANGLES
    if (pusher->client)
        pushed_p->deltayaw = pusher->client->ps.pmove.delta_angles[YAW];
#endif
    pushed_p++;

// move the pusher to it's final position
    VectorAdd(pusher->s.origin, move, pusher->s.origin);
    VectorAdd(pusher->s.angles, amove, pusher->s.angles);
    gi.linkentity(pusher);

// see if any solid entities are inside the final position
    check = g_edicts + 1;
    for (e = 1; e < globals.num_edicts; e++, check++) {
        if (!check->inuse)
            continue;
        if (check->movetype == MOVETYPE_PUSH
            || check->movetype == MOVETYPE_STOP
            || check->movetype == MOVETYPE_NONE
            || check->movetype == MOVETYPE_NOCLIP)
            continue;

        if (!check->area.prev)
            continue;       // not linked in anywhere

        // if the entity is standing on the pusher, it will definitely be moved
        if (check->groundentity != pusher) {
            // see if the ent needs to be tested
            if (check->absmin[0] >= maxs[0]
                || check->absmin[1] >= maxs[1]
                || check->absmin[2] >= maxs[2]
                || check->absmax[0] <= mins[0]
                || check->absmax[1] <= mins[1]
                || check->absmax[2] <= mins[2])
                continue;

            // see if the ent's bbox is inside the pusher's final position
            if (!SV_TestEntityPosition(check))
                continue;
        }

        if ((pusher->movetype == MOVETYPE_PUSH) || (check->groundentity == pusher)) {
            // move this entity
            pushed_p->ent = check;
            VectorCopy(check->s.origin, pushed_p->origin);
            VectorCopy(check->s.angles, pushed_p->angles);
#if USE_SMOOTH_DELTA_ANGLES
            if (check->client)
                pushed_p->deltayaw = check->client->ps.pmove.delta_angles[YAW];
#endif
            pushed_p++;

            // try moving the contacted entity
            VectorAdd(check->s.origin, move, check->s.origin);
#if USE_SMOOTH_DELTA_ANGLES
            if (check->client) {
                // FIXME: doesn't rotate monsters?
                // FIXME: skuller: needs client side interpolation
                check->client->ps.pmove.delta_angles[YAW] += ANGLE2SHORT(amove[YAW]);
            }
#endif

            // figure movement due to the pusher's amove
            VectorSubtract(check->s.origin, pusher->s.origin, org);
            org2[0] = DotProduct(org, forward);
            org2[1] = -DotProduct(org, right);
            org2[2] = DotProduct(org, up);
            VectorSubtract(org2, org, move2);
            VectorAdd(check->s.origin, move2, check->s.origin);

            // may have pushed them off an edge
            if (check->groundentity != pusher)
                check->groundentity = NULL;

            block = SV_TestEntityPosition(check);

            if (Q_strcasecmp(pusher->classname, "func_water") == 0) {
                block = NULL;
            }

            if (!block) {
                // pushed ok
                gi.linkentity(check);
                // impact?
                continue;
            }

            // if it is ok to leave in the old position, do it
            // this is only relevent for riding entities, not pushed
            // FIXME: this doesn't acount for rotation
            VectorSubtract(check->s.origin, move, check->s.origin);
            block = SV_TestEntityPosition(check);
            if (!block) {
                pushed_p--;
                continue;
            }
        }

        // save off the obstacle so we can call the block function
        obstacle = check;

        // move back any entities we already moved
        // go backwards, so if the same entity was pushed
        // twice, it goes back to the original position
        for (p = pushed_p - 1 ; p >= pushed ; p--) {
            VectorCopy(p->origin, p->ent->s.origin);
            VectorCopy(p->angles, p->ent->s.angles);
#if USE_SMOOTH_DELTA_ANGLES
            if (p->ent->client) {
                p->ent->client->ps.pmove.delta_angles[YAW] = p->deltayaw;
            }
#endif
            gi.linkentity(p->ent);
        }
        return false;
    }

//FIXME: is there a better way to handle this?
    // see if anything we moved has touched a trigger
    for (p = pushed_p - 1 ; p >= pushed ; p--)
        G_TouchTriggers(p->ent);

    return true;
}

/*
================
SV_Physics_Pusher

Bmodel objects don't interact with each other, but
push all box objects
================
*/
void SV_Physics_Pusher(edict_t *ent)
{
    vec3_t      move, amove;
    edict_t     *part, *mv;

    // if not a team captain, so movement will be handled elsewhere
    if (ent->flags & FL_TEAMSLAVE)
        return;

    // make sure all team slaves can move before commiting
    // any moves or calling any think functions
    // if the move is blocked, all moved objects will be backed out
//retry:
    pushed_p = pushed;
    for (part = ent ; part ; part = part->teamchain) {
        if (!VectorEmpty(part->velocity) || !VectorEmpty(part->avelocity) ) {
            // object is moving
            VectorScale(part->velocity, FRAMETIME, move);
            VectorScale(part->avelocity, FRAMETIME, amove);

            if (!SV_Push(part, move, amove))
                break;  // move was blocked
        }
    }
    if (pushed_p > &pushed[MAX_EDICTS])
        gi.error("pushed_p > &pushed[MAX_EDICTS], memory corrupted");

    if (part) {
        // the move failed, bump all nextthink times and back out moves
        for (mv = ent ; mv ; mv = mv->teamchain) {
            if (mv->nextthink > 0)
                mv->nextthink++;
        }

        // if the pusher has a "blocked" function, call it
        // otherwise, just stay in place until the obstacle is gone
        if (part->blocked)
            part->blocked(part, obstacle);
#if 0
        // if the pushed entity went away and the pusher is still there
        if (!obstacle->inuse && part->inuse)
            goto retry;
#endif
    } else {
        // the move succeeded, so call all think functions
        for (part = ent ; part ; part = part->teamchain) {
            SV_RunThink(part);
        }
    }
}

//==================================================================

/*
=============
SV_Physics_None

Non moving objects can only think
=============
*/
void SV_Physics_None(edict_t *ent)
{
// regular thinking
    SV_RunThink(ent);
}

/*
=============
SV_Physics_Noclip

A moving object that doesn't obey physics
=============
*/
void SV_Physics_Noclip(edict_t *ent)
{
// regular thinking
    if (!SV_RunThink(ent))
        return;
    if (!ent->inuse)
        return;

    VectorMA(ent->s.angles, FRAMETIME, ent->avelocity, ent->s.angles);
    VectorMA(ent->s.origin, FRAMETIME, ent->velocity, ent->s.origin);

    gi.linkentity(ent);
}

/*
==============================================================================

TOSS / BOUNCE

==============================================================================
*/

/*
=============
SV_Physics_Toss

Toss, bounce, and fly movement.  When onground, do nothing.
=============
*/
void SV_Physics_Toss(edict_t *ent)
{
    trace_t     trace;
    vec3_t      move;
    float       backoff;
    edict_t     *slave;
    int         wasinwater;
    int         isinwater;
    vec3_t      old_origin;
	
	if (ent->movetype == MOVETYPE_EXPLODE) {
		float i;
		i = 0;
	}
// regular thinking
    SV_RunThink(ent);
    if (!ent->inuse)
        return;

    // if not a team captain, so movement will be handled elsewhere
    if (ent->flags & FL_TEAMSLAVE)
        return;

    if (ent->velocity[2] > 0)
        ent->groundentity = NULL;

// check for the groundentity going away
    if (ent->groundentity)
        if (!ent->groundentity->inuse)
            ent->groundentity = NULL;

// if onground, return without moving
    if (ent->groundentity)
        return;

    VectorCopy(ent->s.origin, old_origin);

    SV_CheckVelocity(ent);

// add gravity
    if (ent->movetype != MOVETYPE_FLY
        && ent->movetype != MOVETYPE_FLYMISSILE
        && ent->movetype != MOVETYPE_WALLBOUNCE)
        SV_AddGravity(ent);

// move angles
    

// move origin
    if (ent->movetype == MOVETYPE_TOSS) {
        // [rerelease] TOSS slides: up to five moves a frame along whatever it
        // hits, losing half the into-surface speed each time, with friction on
        // floors, and it only comes to rest once it is slower than 60 u/s.
        // The single stock Q2 move stopped a tossed thing dead the first time
        // it touched anything flat enough to stand on and could never get off
        // a slope steeper than that. mgu4m1's crate drop depends on the
        // rerelease behaviour: func_object *52 slides down its ramp, across the
        // ledge and into the water, and with the stock move it stayed on the
        // ledge and walled the player in.
        float   time_left = FRAMETIME;
        int     num_tries = 5;
        float   backoff_dot;
        int     i;

        memset(&trace, 0, sizeof(trace));
        trace.fraction = 1.0f;

        while (time_left > 0 && num_tries-- > 0) {
            VectorScale(ent->velocity, time_left, move);
            trace = SV_PushEntity(ent, move);
            if (!ent->inuse)
                return;
            if (trace.fraction == 1.0f || trace.allsolid)
                break;      // allsolid is handled just below

            time_left -= time_left * trace.fraction;

            // The rerelease removes only half the into-surface speed here
            // (SlideClipVelocity with 0.5), which relies on its engine's trace
            // backing off the surface. Against this engine's trace the next
            // try went straight back into the slope at fraction 0 - mgu4m1's
            // crate sat on its ramp building up speed and never moved - so
            // take all of it, as stock Q2's ClipVelocity does.
            backoff_dot = DotProduct(ent->velocity, trace.plane.normal);
            for (i = 0; i < 3; i++) {
                ent->velocity[i] -= trace.plane.normal[i] * backoff_dot;
                if (ent->velocity[i] > -STOP_EPSILON && ent->velocity[i] < STOP_EPSILON)
                    ent->velocity[i] = 0;
            }

            if (trace.plane.normal[2] > 0.7f) {
                if (VectorLength(ent->velocity) < 60.0f) {
                    ent->groundentity = trace.ent;
                    ent->groundentity_linkcount = trace.ent->linkcount;
                    VectorClear(ent->velocity);
                    VectorClear(ent->avelocity);
                    break;
                }

                // friction for tossing stuff (gibs, etc)
                VectorScale(ent->velocity, 0.75f, ent->velocity);
                VectorScale(ent->avelocity, 0.75f, ent->avelocity);
            }
        }
    } else {
        VectorScale(ent->velocity, FRAMETIME, move);
        trace = SV_PushEntity(ent, move);
    }

    // Buried in solid and going nowhere.  SV_PushEntity has already refused the
    // move; treat whatever we are inside as the ground so gravity stops piling
    // onto the velocity, exactly as the rerelease does ("don't build up velocity
    // if we're stuck").  Without this a corpse that ends its death animation
    // even slightly inside the floor sinks forever - see the note in
    // SV_PushEntity for why the trace does not stop it.
    if (trace.allsolid) {
        edict_t *ground = trace.ent;

        // Rest on what we are actually buried in. The move trace can name the
        // world even when the box is inside a brush entity - mgu4m1's crate
        // (func_object *192) spawns inside the clamp that holds it up
        // (func_explosive *190) - and resting on the world meant it never
        // fell when the clamp was blown away. Resting on the clamp, it drops
        // as soon as the clamp is freed, as in the rerelease.
        if (ground == world) {
            trace_t in = gi.trace(ent->s.origin, ent->mins, ent->maxs, ent->s.origin, ent,
                                  ent->clipmask ? ent->clipmask : MASK_SOLID);
            if (in.allsolid && in.ent && in.ent != world)
                ground = in.ent;
        }

        if (ground) {
            ent->groundentity = ground;
            ent->groundentity_linkcount = ground->linkcount;
        }
        VectorClear(ent->velocity);
        VectorClear(ent->avelocity);
        gi.linkentity(ent);
    }

    // Belt and braces for the other way a body can be lost: falling out through
    // a leak instead of into a brush.  A BSP cannot describe anything past
    // +/-4096, and the network origin is a 1/8-unit short, so once an entity is
    // below that it can never be clipped or drawn where it really is - it just
    // wraps around and appears to fall from the ceiling again.  Nothing the
    // player should ever see, so retire it.  Clients are never touched: a player
    // who falls out of the level is the map's own kill trigger's business.
    // Debris only - MOVETYPE_TOSS/BOUNCE/EXPLODE are the corpses, gibs and
    // dropped items.  Projectiles are left alone deliberately: several of them
    // are pointed AT by their owner (the parasite keeps self->proboscus), and
    // freeing one out from under that pointer would be a worse bug than the
    // one being fixed.
    if (ent->s.origin[2] < -4096 && !ent->client &&
        (ent->movetype == MOVETYPE_TOSS || ent->movetype == MOVETYPE_BOUNCE ||
         ent->movetype == MOVETYPE_EXPLODE)) {
        G_FreeEdict(ent);
        return;
    }
	
	vec3_t res;

	VectorSubtract(trace.endpos, ent->s.old_origin, res);

	if (res[0] != 0 && res[1] != 0 && res[2] != 0) {
		VectorMA(ent->s.angles, FRAMETIME, ent->avelocity, ent->s.angles);
		if (ent->s.oldEffects == EF_GIB) {
			ent->s.effects = EF_GIB;
		}
	}
	else {
		if (ent->s.effects == EF_GIB) {
			ent->s.oldEffects = EF_GIB;
			ent->s.effects = NULL;
		}
	}

    if (!ent->inuse)
        return;

    if (trace.fraction < 1 && !trace.allsolid && ent->movetype != MOVETYPE_TOSS) {
        if (ent->movetype == MOVETYPE_WALLBOUNCE)
            backoff = 2.0f;
        else if (ent->movetype == MOVETYPE_BOUNCE)
            backoff = 1.5f;
        else
            backoff = 1;

        ClipVelocity(ent->velocity, trace.plane.normal, ent->velocity, backoff);

        // a wallbounce projectile is a thrown blade - point it the way it now flies
        if (ent->movetype == MOVETYPE_WALLBOUNCE)
            vectoangles(ent->velocity, ent->s.angles);

        // stop if on ground (never for wallbounce - it keeps ricocheting until it
        // hits something damageable or its think frees it)
        if (trace.plane.normal[2] > 0.7f && ent->movetype != MOVETYPE_WALLBOUNCE) {
            if (ent->velocity[2] < 60 || ent->movetype != MOVETYPE_BOUNCE) {
                ent->groundentity = trace.ent;
                ent->groundentity_linkcount = trace.ent->linkcount;
                VectorClear(ent->velocity);
                VectorClear(ent->avelocity);
            }
        }

//      if (ent->touch)
//          ent->touch (ent, trace.ent, &trace.plane, trace.surface);
    }

// check for water transition
    wasinwater = (ent->watertype & MASK_WATER);
    ent->watertype = gi.pointcontents(ent->s.origin);
    isinwater = ent->watertype & MASK_WATER;

    if (isinwater)
        ent->waterlevel = 1;
    else
        ent->waterlevel = 0;

    if (!wasinwater && isinwater)
        gi.positioned_sound(old_origin, g_edicts, CHAN_AUTO, gi.soundindex("misc/h2ohit1.wav"), 1, 1, 0);
    else if (wasinwater && !isinwater)
        gi.positioned_sound(ent->s.origin, g_edicts, CHAN_AUTO, gi.soundindex("misc/h2ohit1.wav"), 1, 1, 0);

// move teamslaves
    for (slave = ent->teamchain; slave; slave = slave->teamchain) {
        VectorCopy(ent->s.origin, slave->s.origin);
        gi.linkentity(slave);
    }
}

/*
===============================================================================

STEPPING MOVEMENT

===============================================================================
*/

/*
=============
SV_Physics_Step

Monsters freefall when they don't have a ground entity, otherwise
all movement is done with discrete steps.

This is also used for objects that have become still on the ground, but
will fall if the floor is pulled out from under them.
FIXME: is this true?
=============
*/

//FIXME: hacked in for E3 demo
#define sv_stopspeed        100
#define sv_friction         6
#define sv_waterfriction    1

void SV_AddRotationalFriction(edict_t *ent)
{
    int     n;
    float   adjustment;

    VectorMA(ent->s.angles, FRAMETIME, ent->avelocity, ent->s.angles);
    adjustment = FRAMETIME * sv_stopspeed * sv_friction;
    for (n = 0; n < 3; n++) {
        if (ent->avelocity[n] > 0) {
            ent->avelocity[n] -= adjustment;
            if (ent->avelocity[n] < 0)
                ent->avelocity[n] = 0;
        } else {
            ent->avelocity[n] += adjustment;
            if (ent->avelocity[n] > 0)
                ent->avelocity[n] = 0;
        }
    }
}

void SV_Physics_Step(edict_t *ent)
{
    bool        wasonground;
    bool        hitsound = false;
    float       *vel;
    float       speed, newspeed, control;
    float       friction;
    edict_t     *groundentity;
    int         mask;

    // airborn monsters should always check for ground
    if (!ent->groundentity)
        M_CheckGround(ent);

    groundentity = ent->groundentity;

    SV_CheckVelocity(ent);

    if (groundentity)
        wasonground = true;
    else
        wasonground = false;

    if (!VectorEmpty(ent->avelocity))
        SV_AddRotationalFriction(ent);

    // add gravity except:
    //   flying monsters
    //   swimming monsters who are in the water
    if (! wasonground)
        if (!(ent->flags & FL_FLY))
            if (!((ent->flags & FL_SWIM) && (ent->waterlevel > 2))) {
                if (ent->velocity[2] < sv_gravity->value * -0.1f)
                    hitsound = true;
                if (ent->waterlevel == 0)
                    SV_AddGravity(ent);
            }

    // friction for flying monsters that have been given vertical velocity.
    // [rerelease] AI_ALTERNATE_FLY monsters drive their own velocity in
    // SV_alternate_flystep and must not have it damped back out from under
    // them - the same exemption their g_phys.cpp:785/798/812 makes.
    if ((ent->flags & FL_FLY) && (ent->velocity[2] != 0) &&
        !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY)) {
        speed = fabsf(ent->velocity[2]);
        control = speed < sv_stopspeed ? sv_stopspeed : speed;
        friction = sv_friction / 3;
        newspeed = speed - (FRAMETIME * control * friction);
        if (newspeed < 0)
            newspeed = 0;
        newspeed /= speed;
        ent->velocity[2] *= newspeed;
    }

    // friction for flying monsters that have been given vertical velocity
    if ((ent->flags & FL_SWIM) && (ent->velocity[2] != 0) &&
        !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY)) {
        speed = fabsf(ent->velocity[2]);
        control = speed < sv_stopspeed ? sv_stopspeed : speed;
        newspeed = speed - (FRAMETIME * control * sv_waterfriction * ent->waterlevel);
        if (newspeed < 0)
            newspeed = 0;
        newspeed /= speed;
        ent->velocity[2] *= newspeed;
    }

    if (ent->velocity[2] || ent->velocity[1] || ent->velocity[0]) {
        // apply friction
        // let dead monsters who aren't completely onground slide
        if (((wasonground) || (ent->flags & (FL_SWIM | FL_FLY))) &&
            !(ent->monsterinfo.aiflags & AI_ALTERNATE_FLY))
            if (!(ent->health <= 0.0f && !M_CheckBottom(ent))) {
                vel = ent->velocity;
                speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
                if (speed) {
                    friction = sv_friction;

                    control = speed < sv_stopspeed ? sv_stopspeed : speed;
                    newspeed = speed - FRAMETIME * control * friction;

                    if (newspeed < 0)
                        newspeed = 0;
                    newspeed /= speed;

                    vel[0] *= newspeed;
                    vel[1] *= newspeed;
                }
            }

        if (ent->svflags & SVF_MONSTER)
            mask = MASK_MONSTERSOLID;
        else
            mask = MASK_SOLID;
        SV_FlyMove(ent, FRAMETIME, mask);

        gi.linkentity(ent);
        G_TouchTriggers(ent);
        if (!ent->inuse)
            return;

        if (ent->groundentity)
            if (!wasonground)
                if (hitsound)
                    gi.sound(ent, 0, gi.soundindex("world/land.wav"), 1, 1, 0);
    }

    // [rerelease] ground contact changed - let a monster with its own gravity
    // put itself the right way up (the stalker, leaving a ceiling)
    if ((ent->svflags & SVF_MONSTER) && wasonground != !!ent->groundentity &&
        ent->monsterinfo.physics_change)
        ent->monsterinfo.physics_change(ent);

// regular thinking
    SV_RunThink(ent);
}

//============================================================================
// The maps author the animation rate in milliseconds; thinking happens on server
// frames. Round to nearest rather than truncating - 192 ms is 2 frames, not 1 -
// and never return 0, which would step the frame every single server tick.
static int BModelAnimFrames(int msec)
{
    if (msec <= 0)
        return 1;
    return max(1, (msec + (1000 / BASE_FRAMERATE) / 2) / (1000 / BASE_FRAMERATE));
}

/*
=============
G_RunBmodelAnimation

Steps a brush model's texture-animation frame. Ported from the function of the
same name in src/rerelease/g_phys.cpp. See bmodel_anim_t in g_local.h for what
the two frame ranges mean and why the rate is only accurate to 100 ms here.
=============
*/
static void G_RunBmodelAnimation(edict_t *ent)
{
    bmodel_anim_t *anim = &ent->bmodel_anim;
    int start, end, style, speed, nowrap;

    // switching sets restarts the timer, so a button responds on the next frame
    if (anim->currently_alternate != anim->alternate) {
        anim->currently_alternate = anim->alternate;
        anim->next_framenum = 0;
    }

    if (level.framenum < anim->next_framenum)
        return;

    start  = anim->alternate ? anim->alt_start : anim->start;
    end    = anim->alternate ? anim->alt_end : anim->end;
    style  = anim->alternate ? anim->alt_style : anim->style;
    speed  = anim->alternate ? anim->alt_speed : anim->speed;
    nowrap = anim->alternate ? anim->alt_nowrap : anim->nowrap;

    anim->next_framenum = level.framenum + BModelAnimFrames(speed);

    switch (style) {
    case BMODEL_ANIM_BACKWARDS:
        ent->s.frame += (end >= start) ? -1 : 1;
        break;
    case BMODEL_ANIM_RANDOM:
        if (end >= start)
            ent->s.frame = start + (Q_rand() % (end - start + 1));
        else
            ent->s.frame = end + (Q_rand() % (start - end + 1));
        break;
    case BMODEL_ANIM_FORWARDS:
    default:
        ent->s.frame += (end >= start) ? 1 : -1;
        break;
    }

    if (nowrap) {
        // cclamp orders the bounds itself, and clamp() ASSIGNS to its first
        // argument - hence no "frame =" here.
        cclamp(ent->s.frame, start, end);
    } else {
        if (ent->s.frame < start)
            ent->s.frame = end;
        else if (ent->s.frame > end)
            ent->s.frame = start;
    }
}

/*
================
G_RunEntity

================
*/
void G_RunEntity(edict_t *ent)
{
    if (ent->prethink)
        ent->prethink(ent);

    // bmodel animation runs first so a custom entity can override the frame
    if (ent->bmodel_anim.enabled)
        G_RunBmodelAnimation(ent);

    switch ((int)ent->movetype) {
    case MOVETYPE_PUSH:
    case MOVETYPE_STOP:
        SV_Physics_Pusher(ent);
        break;
    case MOVETYPE_NONE:
        SV_Physics_None(ent);
        break;
    case MOVETYPE_NOCLIP:
        SV_Physics_Noclip(ent);
        break;
    case MOVETYPE_STEP:
        SV_Physics_Step(ent);
        break;
    case MOVETYPE_TOSS:
    case MOVETYPE_BOUNCE:
    case MOVETYPE_FLY:
    case MOVETYPE_FLYMISSILE:
    case MOVETYPE_WALLBOUNCE:
	case MOVETYPE_EXPLODE:
        SV_Physics_Toss(ent);
        break;
    default:
        gi.error("SV_Physics: bad movetype %i", (int)ent->movetype);
    }
}
