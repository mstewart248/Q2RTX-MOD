/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2026 Q2RTX contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// g_q1vm_phys.c - Quake's world and physics for the progs: traces, linking
// and touching, the movetypes, and the monsters' stepping. A port of Quake's
// world.c, sv_phys.c and sv_move.c.
//
// The engine's own trace already clips Quake 1's hulls (bsp_q1.c) for the
// world and the brush entities; the boxes - monsters, players, missiles - are
// clipped here by Quake's rules, which Quake II's would get wrong (owners,
// the missile box against monsters, corpses). Every entity is also linked into
// the engine's area grid, so the players' pmove and the clients' prediction
// see the same world.

#include "g_q1vm.h"

#define Q1_DIST_EPSILON     0.03125f
#define Q1_STEPSIZE         18
#define Q1_STOP_EPSILON     0.1f
#define Q1_MAX_CLIP_PLANES  5

static inline bool q1_free(int num)
{
    return num < 0 || num >= qvm.num_edicts || qvm.ed[num].free;
}

static float q1_anglemod(float a)
{
    return (360.0f / 65536) * ((int)(a * (65536 / 360.0f)) & 65535);
}

/*
==============================================================================

POINT CONTENTS

==============================================================================
*/

int Q1_PointContents(const vec3_t p)
{
    int c = gi.pointcontents(p);

    if (c & (CONTENTS_SOLID | CONTENTS_WINDOW))
        return Q1_CONTENTS_SOLID;
    if (c & CONTENTS_LAVA)
        return Q1_CONTENTS_LAVA;
    if (c & CONTENTS_SLIME)
        return Q1_CONTENTS_SLIME;
    if (c & CONTENTS_WATER)
        return Q1_CONTENTS_WATER;
    return Q1_CONTENTS_EMPTY;
}

/*
==============================================================================

LINKING

==============================================================================
*/

// Quake's solid as the engine's: a brush entity only when it is one of the
// map's own models (the engine clips SOLID_BSP by its inline model's hull)
static solid_t q1_engine_solid(int num)
{
    qc_entvars_t *ev = QC_EV(num);

    switch ((int)ev->solid) {
    case Q1_SOLID_TRIGGER:
        return SOLID_TRIGGER;
    case Q1_SOLID_BBOX:
    case Q1_SOLID_SLIDEBOX:
    case Q1_SOLID_CORPSE:
        return SOLID_BBOX;
    case Q1_SOLID_BSP: {
        const char *m = Q1_ModelName((int)ev->modelindex);
        return m[0] == '*' ? SOLID_BSP : SOLID_BBOX;
    }
    default:
        return SOLID_NOT;
    }
}

// the engine's copy: what the area grid, pmove and the clients' prediction see
static void q1_link_engine(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    edict_t *e = &g_edicts[num];
    int owner = QC_FromProg(ev->owner);

    e->inuse = true;
    e->s.number = num;
    VectorCopy(ev->origin, e->s.origin);
    VectorCopy(ev->mins, e->mins);
    VectorCopy(ev->maxs, e->maxs);

    // items are easier to pick up, as Quake had them (the abs box below)
    if ((int)ev->flags & Q1_FL_ITEM) {
        e->mins[0] -= 14;
        e->mins[1] -= 14;
        e->maxs[0] += 14;
        e->maxs[1] += 14;
    }

    e->solid = q1_engine_solid(num);
    if (e->solid == SOLID_BSP)
        e->s.modelindex = (int)ev->modelindex;  // the engine clips by it

    e->svflags &= ~(SVF_DEADMONSTER | SVF_MONSTER);
    if ((int)ev->solid == Q1_SOLID_CORPSE || (e->solid == SOLID_BBOX && !ev->size[0]))
        // players walk through corpses, and through points (missiles), as
        // Quake's sized boxes did ("points never interact")
        e->svflags |= SVF_DEADMONSTER;
    else if ((int)ev->flags & Q1_FL_MONSTER)
        e->svflags |= SVF_MONSTER;

    e->owner = (owner > 0 && owner < qvm.num_edicts) ? &g_edicts[owner] : NULL;

    gi.linkentity(e);
}

void Q1_UnlinkEdict(int num)
{
    if (num <= 0 || num >= qvm.max_edicts)
        return;
    gi.unlinkentity(&g_edicts[num]);
    qvm.ed[num].linked = false;
}

/*
Runs the touch functions of every trigger the entity's box is in. The list is
taken first: a touch function may free or move anything.
*/
static void q1_touch_links(int num)
{
    edict_t *list[256];
    qc_entvars_t *ev = QC_EV(num);
    int n = gi.BoxEdicts(ev->absmin, ev->absmax, list, q_countof(list), AREA_TRIGGERS);

    for (int i = 0; i < n; i++) {
        int t = (int)(list[i] - g_edicts);
        if (t == num || q1_free(t))
            continue;
        if (qvm.ed[num].free)
            return;

        qc_entvars_t *tv = QC_EV(t);
        if (!tv->touch || (int)tv->solid != Q1_SOLID_TRIGGER)
            continue;
        if (ev->absmin[0] > tv->absmax[0] || ev->absmin[1] > tv->absmax[1] || ev->absmin[2] > tv->absmax[2] ||
            ev->absmax[0] < tv->absmin[0] || ev->absmax[1] < tv->absmin[1] || ev->absmax[2] < tv->absmin[2])
            continue;

        int old_self = qvm.g->self;
        int old_other = qvm.g->other;

        qvm.g->self = QC_ToProg(t);
        qvm.g->other = QC_ToProg(num);
        qvm.g->time = qvm.time;
        QC_Execute(tv->touch);

        qvm.g->self = old_self;
        qvm.g->other = old_other;
    }
}

void Q1_LinkEdict(int num, bool touch_triggers)
{
    if (num <= 0 || q1_free(num))
        return;     // the world is never linked

    qc_entvars_t *ev = QC_EV(num);

    // set the abs box
    VectorAdd(ev->origin, ev->mins, ev->absmin);
    VectorAdd(ev->origin, ev->maxs, ev->absmax);

    // to make items easier to pick up and allow them to be grabbed off of
    // shelves, the abs sizes are expanded
    if ((int)ev->flags & Q1_FL_ITEM) {
        ev->absmin[0] -= 15;
        ev->absmin[1] -= 15;
        ev->absmax[0] += 15;
        ev->absmax[1] += 15;
    } else {
        // because movement is clipped an epsilon away from an actual edge,
        // we must fully check even when bounding boxes don't quite touch
        for (int i = 0; i < 3; i++) {
            ev->absmin[i] -= 1;
            ev->absmax[i] += 1;
        }
    }

    q1_link_engine(num);
    qvm.ed[num].linked = true;

    if (touch_triggers && (int)ev->solid != Q1_SOLID_NOT)
        q1_touch_links(num);
}

/*
The engine's copy brought up to date at the end of a frame, for what the progs
changed without relinking: a solid flag (a monster dying goes SOLID_NOT, and
pmove must stop bumping into it), an origin set directly. Quake's own clip
lists lagged the same way; its network did not, nor does this.
*/
void Q1_SyncLink(int num)
{
    edict_t *e = &g_edicts[num];
    qc_entvars_t *ev = QC_EV(num);

    if (!qvm.ed[num].linked || e->solid != q1_engine_solid(num) ||
        !VectorCompare(e->s.origin, ev->origin) ||
        !VectorCompare(e->mins, ev->mins) && !((int)ev->flags & Q1_FL_ITEM))
        Q1_LinkEdict(num, false);
}

/*
==============================================================================

TRACES

==============================================================================
*/

/*
A box entity, grown by the mover's box (Quake's SV_HullForBox), with the
mover's centre line traced through it: Quake's results, DIST_EPSILON short of
the face, and a start inside it "solid" with no impact.
*/
static void q1_clip_box(q1trace_t *tr, int touch, const vec3_t start, const vec3_t mins,
                        const vec3_t maxs, const vec3_t end)
{
    qc_entvars_t *t = QC_EV(touch);
    vec3_t bmin, bmax;
    float enterfrac = -1, leavefrac = 1;
    int enterface = -1;
    bool startout = false, getout = false;
    q1trace_t bt;

    for (int i = 0; i < 3; i++) {
        bmin[i] = t->origin[i] + t->mins[i] - maxs[i];
        bmax[i] = t->origin[i] + t->maxs[i] - mins[i];
    }

    for (int face = 0; face < 6; face++) {
        int axis = face >> 1;
        float d1, d2, f;

        if (face & 1) {     // the -axis face
            d1 = bmin[axis] - start[axis];
            d2 = bmin[axis] - end[axis];
        } else {            // the +axis face
            d1 = start[axis] - bmax[axis];
            d2 = end[axis] - bmax[axis];
        }

        // Quake's hull check puts a point ON a plane in front of it (t1 >= 0):
        // boxes that only touch - an item set down against a monster (e1m1's
        // rotten health and its grunt), two monsters side by side - are apart
        if (d2 >= 0)
            getout = true;
        if (d1 >= 0)
            startout = true;

        // in front of the face all the way
        if (d1 >= 0 && d2 >= 0)
            return;
        // behind it all the way
        if (d1 < 0 && d2 < 0)
            continue;

        if (d1 > d2) {  // enters
            f = (d1 - Q1_DIST_EPSILON) / (d1 - d2);
            if (f > enterfrac) {
                enterfrac = f;
                enterface = face;
            }
        } else {        // leaves
            f = (d1 + Q1_DIST_EPSILON) / (d1 - d2);
            if (f < leavefrac)
                leavefrac = f;
        }
    }

    memset(&bt, 0, sizeof(bt));
    bt.ent = touch;
    bt.fraction = 1;
    VectorCopy(end, bt.endpos);

    if (!startout) {
        // started inside: Quake's hull trace finds no plane to stop on
        bt.startsolid = true;
        bt.allsolid = !getout;
    } else {
        if (enterface < 0 || enterfrac >= leavefrac || enterfrac >= 1)
            return;     // misses
        if (enterfrac < 0)
            enterfrac = 0;
        int axis = enterface >> 1;
        bt.fraction = enterfrac;
        VectorClear(bt.plane.normal);
        if (enterface & 1) {
            bt.plane.normal[axis] = -1;
            bt.plane.dist = -bmin[axis];
        } else {
            bt.plane.normal[axis] = 1;
            bt.plane.dist = bmax[axis];
        }
        bt.plane.type = axis;
        LerpVector(start, end, enterfrac, bt.endpos);
    }

    // Quake's SV_ClipToLinks
    if (bt.allsolid || bt.startsolid || bt.fraction < tr->fraction) {
        if (tr->startsolid) {
            *tr = bt;
            tr->startsolid = true;
        } else {
            *tr = bt;
        }
    }
}

q1trace_t Q1_Move(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int type, int passedict)
{
    static edict_t *list[MAX_EDICTS];
    q1trace_t tr;
    trace_t t;
    vec3_t mins2, maxs2, boxmins, boxmaxs;
    edict_t *pass = (passedict >= 0 && passedict < qvm.max_edicts) ? &g_edicts[passedict] : NULL;

    // the world and its brush entities: the engine's trace, through Quake's hulls
    t = gi.trace(start, mins, maxs, end, pass, MASK_SOLID);

    memset(&tr, 0, sizeof(tr));
    tr.ent = -1;
    tr.startsolid = t.startsolid;
    tr.allsolid = t.allsolid;
    tr.fraction = t.fraction;
    VectorCopy(t.endpos, tr.endpos);
    tr.plane = t.plane;
    if (t.fraction < 1 || t.startsolid)
        tr.ent = t.ent ? (int)(t.ent - g_edicts) : 0;
    if (t.allsolid) {
        // Quake's hull trace leaves an all-solid move where it was asked to end
        tr.fraction = 1;
        VectorCopy(end, tr.endpos);
    }
    tr.sky = t.fraction < 1 && tr.ent == 0 && t.surface && (t.surface->flags & SURF_SKY);

    // the boxes
    if (type != Q1_MOVE_NOMONSTERS && !tr.allsolid) {
        const qc_entvars_t *pv = QC_EV(passedict >= 0 ? passedict : 0);

        if (type == Q1_MOVE_MISSILE) {
            VectorSet(mins2, -15, -15, -15);
            VectorSet(maxs2, 15, 15, 15);
        } else {
            VectorCopy(mins, mins2);
            VectorCopy(maxs, maxs2);
        }

        // Quake's SV_MoveBounds
        for (int i = 0; i < 3; i++) {
            if (end[i] > start[i]) {
                boxmins[i] = start[i] + mins2[i] - 1;
                boxmaxs[i] = end[i] + maxs2[i] + 1;
            } else {
                boxmins[i] = end[i] + mins2[i] - 1;
                boxmaxs[i] = start[i] + maxs2[i] + 1;
            }
        }

        int n = gi.BoxEdicts(boxmins, boxmaxs, list, MAX_EDICTS, AREA_SOLID);
        for (int i = 0; i < n; i++) {
            int num = (int)(list[i] - g_edicts);
            if (num <= 0 || q1_free(num) || num == passedict)
                continue;

            qc_entvars_t *touch = QC_EV(num);
            int solid = (int)touch->solid;
            if (solid == Q1_SOLID_NOT || solid == Q1_SOLID_TRIGGER)
                continue;
            if (solid == Q1_SOLID_BSP && list[i]->solid == SOLID_BSP)
                continue;   // the engine's trace had it

            if (boxmins[0] > touch->absmax[0] || boxmins[1] > touch->absmax[1] || boxmins[2] > touch->absmax[2] ||
                boxmaxs[0] < touch->absmin[0] || boxmaxs[1] < touch->absmin[1] || boxmaxs[2] < touch->absmin[2])
                continue;

            if (passedict >= 0 && pv->size[0] && !touch->size[0])
                continue;   // points never interact

            // the rerelease's corpses
            if (passedict >= 0) {
                int ps = (int)pv->solid;
                if (ps == Q1_SOLID_SLIDEBOX && solid == Q1_SOLID_CORPSE)
                    continue;
                if (ps == Q1_SOLID_CORPSE && (solid == Q1_SOLID_SLIDEBOX || solid == Q1_SOLID_CORPSE))
                    continue;
            }

            if (tr.allsolid)
                break;

            if (passedict >= 0) {
                if (QC_FromProg(touch->owner) == passedict)
                    continue;   // don't clip against own missiles
                if (QC_FromProg(pv->owner) == num)
                    continue;   // don't clip against owner
            }

            // (a box that clips the move replaces the trace, sky flag and all)
            if ((int)touch->flags & Q1_FL_MONSTER)
                q1_clip_box(&tr, num, start, mins2, maxs2, end);
            else
                q1_clip_box(&tr, num, start, mins, maxs, end);
        }
    }

    // what the line crossed, for sight checks through water: Quake's hull
    // trace knew every leaf it passed; the two ends say nearly as much
    if (VectorCompare(mins, vec3_origin) && VectorCompare(maxs, vec3_origin)) {
        int c1 = Q1_PointContents(start);
        int c2 = Q1_PointContents(tr.endpos);
        tr.inopen = c1 == Q1_CONTENTS_EMPTY || c2 == Q1_CONTENTS_EMPTY;
        tr.inwater = (c1 <= Q1_CONTENTS_WATER && c1 >= Q1_CONTENTS_LAVA) ||
                     (c2 <= Q1_CONTENTS_WATER && c2 >= Q1_CONTENTS_LAVA);
    }

    return tr;
}

// the blocking entity, 0 for the world, or -1 when it is clear
int Q1_TestEntityPosition(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    q1trace_t t = Q1_Move(ev->origin, ev->mins, ev->maxs, ev->origin, Q1_MOVE_NORMAL, num);

    if (t.startsolid)
        return t.ent >= 0 ? t.ent : 0;
    return -1;
}

/*
==============================================================================

PHYSICS

==============================================================================
*/

static void q1_check_velocity(qc_entvars_t *ev)
{
    float max = sv_maxvelocity ? sv_maxvelocity->value : 2000;

    for (int i = 0; i < 3; i++) {
        if (isnan(ev->velocity[i]))
            ev->velocity[i] = 0;
        if (isnan(ev->origin[i]))
            ev->origin[i] = 0;
        if (ev->velocity[i] > max)
            ev->velocity[i] = max;
        else if (ev->velocity[i] < -max)
            ev->velocity[i] = -max;
    }
}

/*
Runs thinking code if time. There is some play in the exact time the think
function will be called, because it is called before any movement is done in
a frame. Not used for pushmove objects, because they must be exact. Returns
false if the entity removed itself.
*/
bool Q1_RunThink(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    float thinktime = ev->nextthink;

    if (thinktime <= 0 || thinktime > qvm.time + qvm.frametime)
        return true;

    if (thinktime < qvm.time)
        thinktime = qvm.time;   // don't let things stay in the past

    ev->nextthink = 0;
    if (!ev->think)
        return true;    // Quake stopped the server here

    qvm.g->time = thinktime;
    qvm.g->self = QC_ToProg(num);
    qvm.g->other = 0;
    QC_Execute(ev->think);

    return !qvm.ed[num].free;
}

/*
Two entities have touched, so run their touch functions.
*/
void Q1_Impact(int e1, int e2)
{
    int old_self = qvm.g->self;
    int old_other = qvm.g->other;
    qc_entvars_t *a = QC_EV(e1), *b = QC_EV(e2);

    qvm.g->time = qvm.time;

    if (a->touch && (int)a->solid != Q1_SOLID_NOT) {
        qvm.g->self = QC_ToProg(e1);
        qvm.g->other = QC_ToProg(e2);
        QC_Execute(a->touch);
    }

    if (!qvm.ed[e1].free && !qvm.ed[e2].free && b->touch && (int)b->solid != Q1_SOLID_NOT) {
        qvm.g->self = QC_ToProg(e2);
        qvm.g->other = QC_ToProg(e1);
        QC_Execute(b->touch);
    }

    qvm.g->self = old_self;
    qvm.g->other = old_other;
}

// an impact on the sky: the touch function asking pointcontents of the spot
// hears CONTENTS_SKY, as it did when Quake let the missile into the sky volume
static void q1_impact_trace(int num, const q1trace_t *tr)
{
    if (tr->ent < 0 || q1_free(num) || q1_free(tr->ent))
        return;
    if (tr->sky) {
        qvm.sky_impact = true;
        VectorCopy(QC_EV(num)->origin, qvm.sky_point);
    }
    Q1_Impact(num, tr->ent);
    qvm.sky_impact = false;
}

/*
Slide off of the impacting object. Returns the blocked flags (1 = floor,
2 = step / wall).
*/
static int q1_clip_velocity(const vec3_t in, const vec3_t normal, vec3_t out, float overbounce)
{
    int blocked = 0;

    if (normal[2] > 0)
        blocked |= 1;   // floor
    if (!normal[2])
        blocked |= 2;   // step

    float backoff = DotProduct(in, normal) * overbounce;

    for (int i = 0; i < 3; i++) {
        float change = normal[i] * backoff;
        out[i] = in[i] - change;
        if (out[i] > -Q1_STOP_EPSILON && out[i] < Q1_STOP_EPSILON)
            out[i] = 0;
    }
    return blocked;
}

/*
The basic solid body movement clip that slides along multiple planes. Returns
the clipflags if the velocity was modified (hit something solid):
1 = floor, 2 = wall / step, 4 = dead stop.
*/
static int q1_fly_move(int num, float time)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t planes[Q1_MAX_CLIP_PLANES];
    vec3_t primal_velocity, original_velocity, new_velocity, dir, end;
    int numplanes = 0, blocked = 0, i, j;
    float time_left = time;

    VectorCopy(ev->velocity, original_velocity);
    VectorCopy(ev->velocity, primal_velocity);

    for (int bumpcount = 0; bumpcount < 4; bumpcount++) {
        if (!ev->velocity[0] && !ev->velocity[1] && !ev->velocity[2])
            break;

        for (i = 0; i < 3; i++)
            end[i] = ev->origin[i] + time_left * ev->velocity[i];

        q1trace_t trace = Q1_Move(ev->origin, ev->mins, ev->maxs, end, Q1_MOVE_NORMAL, num);

        if (trace.allsolid) {
            // entity is trapped in another solid
            VectorClear(ev->velocity);
            return 3;
        }

        if (trace.fraction > 0) {
            // actually covered some distance
            VectorCopy(trace.endpos, ev->origin);
            VectorCopy(ev->velocity, original_velocity);
            numplanes = 0;
        }

        if (trace.fraction == 1)
            break;      // moved the entire distance

        if (trace.ent < 0)
            trace.ent = 0;

        if (trace.plane.normal[2] > 0.7f) {
            blocked |= 1;   // floor
            if ((int)QC_EV(trace.ent)->solid == Q1_SOLID_BSP) {
                ev->flags = (int)ev->flags | Q1_FL_ONGROUND;
                ev->groundentity = QC_ToProg(trace.ent);
            }
        }
        if (!trace.plane.normal[2])
            blocked |= 2;   // step

        // run the impact function
        q1_impact_trace(num, &trace);
        if (qvm.ed[num].free)
            break;      // removed by the impact function

        time_left -= time_left * trace.fraction;

        // clipped to another plane
        if (numplanes >= Q1_MAX_CLIP_PLANES) {
            // this shouldn't really happen
            VectorClear(ev->velocity);
            return 3;
        }

        VectorCopy(trace.plane.normal, planes[numplanes]);
        numplanes++;

        // modify original_velocity so it parallels all of the clip planes
        for (i = 0; i < numplanes; i++) {
            q1_clip_velocity(original_velocity, planes[i], new_velocity, 1);
            for (j = 0; j < numplanes; j++) {
                if (j != i && DotProduct(new_velocity, planes[j]) < 0)
                    break;  // not ok
            }
            if (j == numplanes)
                break;
        }

        if (i != numplanes) {
            // go along this plane
            VectorCopy(new_velocity, ev->velocity);
        } else {
            // go along the crease
            if (numplanes != 2) {
                VectorClear(ev->velocity);
                return 7;
            }
            CrossProduct(planes[0], planes[1], dir);
            float d = DotProduct(dir, ev->velocity);
            VectorScale(dir, d, ev->velocity);
        }

        // if original velocity is against the original velocity, stop dead
        // to avoid tiny oscillations in sloping corners
        if (DotProduct(ev->velocity, primal_velocity) <= 0) {
            VectorClear(ev->velocity);
            return blocked;
        }
    }

    return blocked;
}

static void q1_add_gravity(qc_entvars_t *ev)
{
    ev->velocity[2] -= (sv_gravity ? sv_gravity->value : 800) * qvm.frametime;
}

static q1trace_t q1_push_entity_move(int num, const vec3_t start, const vec3_t end)
{
    qc_entvars_t *ev = QC_EV(num);

    if ((int)ev->movetype == Q1_MOVETYPE_FLYMISSILE)
        return Q1_Move(start, ev->mins, ev->maxs, end, Q1_MOVE_MISSILE, num);
    if ((int)ev->solid == Q1_SOLID_TRIGGER || (int)ev->solid == Q1_SOLID_NOT)
        // only clip against bmodels
        return Q1_Move(start, ev->mins, ev->maxs, end, Q1_MOVE_NOMONSTERS, num);
    return Q1_Move(start, ev->mins, ev->maxs, end, Q1_MOVE_NORMAL, num);
}

/*
Does not change the entity's velocity at all.
*/
static q1trace_t q1_push_entity(int num, const vec3_t push)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t end;

    VectorAdd(ev->origin, push, end);

    q1trace_t trace = q1_push_entity_move(num, ev->origin, end);

    VectorCopy(trace.endpos, ev->origin);
    Q1_LinkEdict(num, true);

    if (!qvm.ed[num].free)
        q1_impact_trace(num, &trace);

    return trace;
}

/*
A door, a plat, a train: moves, carrying (or blocked by) whatever is in the
way. The engine's copy of the pusher is made non-solid while the others move
out of its way, as Quake's own was.
*/
static void q1_push_move(int pusher, float movetime)
{
    static int moved_edict[MAX_EDICTS];
    static vec3_t moved_from[MAX_EDICTS];
    qc_entvars_t *pv = QC_EV(pusher);
    edict_t *pe = &g_edicts[pusher];
    vec3_t move, mins, maxs, pushorig, entorig;
    int num_moved = 0;

    if (!pv->velocity[0] && !pv->velocity[1] && !pv->velocity[2]) {
        pv->ltime += movetime;
        return;
    }

    for (int i = 0; i < 3; i++) {
        move[i] = pv->velocity[i] * movetime;
        mins[i] = pv->absmin[i] + move[i];
        maxs[i] = pv->absmax[i] + move[i];
    }

    VectorCopy(pv->origin, pushorig);

    // move the pusher to its final position
    VectorAdd(pv->origin, move, pv->origin);
    pv->ltime += movetime;
    Q1_LinkEdict(pusher, false);

    // see if any solid entities are inside the final position
    for (int e = 1; e < qvm.num_edicts; e++) {
        if (qvm.ed[e].free)
            continue;

        qc_entvars_t *check = QC_EV(e);
        int mt = (int)check->movetype;
        if (mt == Q1_MOVETYPE_PUSH || mt == Q1_MOVETYPE_NONE || mt == Q1_MOVETYPE_NOCLIP)
            continue;

        bool riding = ((int)check->flags & Q1_FL_ONGROUND) && QC_FromProg(check->groundentity) == pusher;

        // if the entity is standing on the pusher, it will definitely be moved
        if (!riding) {
            if (check->absmin[0] >= maxs[0] || check->absmin[1] >= maxs[1] || check->absmin[2] >= maxs[2] ||
                check->absmax[0] <= mins[0] || check->absmax[1] <= mins[1] || check->absmax[2] <= mins[2])
                continue;

            // see if the ent's bbox is inside the pusher's final position
            if (Q1_TestEntityPosition(e) < 0)
                continue;
        }

        // remove the onground flag for non-players
        if (mt != Q1_MOVETYPE_WALK)
            check->flags = (int)check->flags & ~Q1_FL_ONGROUND;

        VectorCopy(check->origin, entorig);
        VectorCopy(check->origin, moved_from[num_moved]);
        moved_edict[num_moved] = e;
        num_moved++;

        // try moving the contacted entity
        float solid_backup = pv->solid;
        solid_t engine_backup = pe->solid;
        int block = -1;

        if (solid_backup == Q1_SOLID_BSP || solid_backup == Q1_SOLID_BBOX || solid_backup == Q1_SOLID_SLIDEBOX) {
            pv->solid = Q1_SOLID_NOT;
            pe->solid = SOLID_NOT;      // the engine's trace skips it
            q1_push_entity(e, move);
            pv->solid = solid_backup;
            pe->solid = engine_backup;

            // if it is still inside the pusher, block
            block = Q1_TestEntityPosition(e);
        }

        if (block >= 0) {
            // fail the move
            if (check->mins[0] == check->maxs[0])
                continue;

            if ((int)check->solid == Q1_SOLID_NOT || (int)check->solid == Q1_SOLID_TRIGGER) {
                // corpse
                check->mins[0] = check->mins[1] = 0;
                VectorCopy(check->mins, check->maxs);
                continue;
            }

            // a rider pushed a hair into the pusher it stands on: lift it clear
            // (vkQuake's elevator fix, for every entity)
            if (riding && block == pusher) {
                check->origin[2] += Q1_DIST_EPSILON;
                if (Q1_TestEntityPosition(e) < 0) {
                    Q1_LinkEdict(e, false);
                    continue;
                }
            }

            VectorCopy(entorig, check->origin);
            Q1_LinkEdict(e, true);

            VectorCopy(pushorig, pv->origin);
            Q1_LinkEdict(pusher, false);
            pv->ltime -= movetime;

            // if the pusher has a "blocked" function, call it, otherwise just
            // stay in place until the obstacle is gone
            if (pv->blocked) {
                qvm.g->self = QC_ToProg(pusher);
                qvm.g->other = QC_ToProg(e);
                QC_Execute(pv->blocked);
            }

            // move back any entities we already moved
            for (int i = 0; i < num_moved; i++) {
                if (qvm.ed[moved_edict[i]].free)
                    continue;
                VectorCopy(moved_from[i], QC_EV(moved_edict[i])->origin);
                Q1_LinkEdict(moved_edict[i], false);
            }
            return;
        }
    }
}

static void q1_physics_pusher(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    float oldltime = ev->ltime;
    float thinktime = ev->nextthink;
    float movetime;

    if (thinktime < ev->ltime + qvm.frametime) {
        movetime = thinktime - ev->ltime;
        if (movetime < 0)
            movetime = 0;
    } else {
        movetime = qvm.frametime;
    }

    if (movetime)
        q1_push_move(num, movetime);   // advances ltime if not blocked

    if (qvm.ed[num].free)
        return;

    if (thinktime > oldltime && thinktime <= ev->ltime) {
        ev->nextthink = 0;
        if (ev->think) {
            qvm.g->time = qvm.time;
            qvm.g->self = QC_ToProg(num);
            qvm.g->other = 0;
            QC_Execute(ev->think);
        }
    }
}

/*
Non moving objects can only think.
*/
static void q1_physics_none(int num)
{
    Q1_RunThink(num);
}

/*
A moving object that doesn't obey physics.
*/
static void q1_physics_noclip(int num)
{
    qc_entvars_t *ev = QC_EV(num);

    if (!Q1_RunThink(num))
        return;

    VectorMA(ev->angles, qvm.frametime, ev->avelocity, ev->angles);
    VectorMA(ev->origin, qvm.frametime, ev->velocity, ev->origin);
    Q1_LinkEdict(num, false);
}

static void q1_check_water_transition(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    int cont = Q1_PointContents(ev->origin);

    if (!ev->watertype) {
        // just spawned here
        ev->watertype = cont;
        ev->waterlevel = 1;
        return;
    }

    if (cont <= Q1_CONTENTS_WATER) {
        if (ev->watertype == Q1_CONTENTS_EMPTY)    // just crossed into water
            Q1_StartSound(num, 0, "misc/h2ohit1.wav", 1, 1);
        ev->watertype = cont;
        ev->waterlevel = 1;
    } else {
        if (ev->watertype != Q1_CONTENTS_EMPTY)    // just crossed out of water
            Q1_StartSound(num, 0, "misc/h2ohit1.wav", 1, 1);
        ev->watertype = Q1_CONTENTS_EMPTY;
        ev->waterlevel = cont;     // (sic, Quake's)
    }
}

/*
Toss, bounce, and fly movement. When onground, do nothing.
*/
static void q1_physics_toss(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t move;

    // regular thinking
    if (!Q1_RunThink(num))
        return;

    // if onground, return without moving
    if ((int)ev->flags & Q1_FL_ONGROUND)
        return;

    q1_check_velocity(ev);

    int mt = (int)ev->movetype;

    // add gravity
    if (mt != Q1_MOVETYPE_FLY && mt != Q1_MOVETYPE_FLYMISSILE)
        q1_add_gravity(ev);

    // move angles
    VectorMA(ev->angles, qvm.frametime, ev->avelocity, ev->angles);

    // move origin
    VectorScale(ev->velocity, qvm.frametime, move);
    q1trace_t trace = q1_push_entity(num, move);

    if (trace.fraction == 1)
        return;
    if (qvm.ed[num].free)
        return;

    float backoff = (mt == Q1_MOVETYPE_BOUNCE) ? 1.5f : 1;

    q1_clip_velocity(ev->velocity, trace.plane.normal, ev->velocity, backoff);

    // stop if on ground (vkQuake's: a grenade on a slope settles as on the flat)
    if (trace.plane.normal[2] > 0.7f) {
        if (mt != Q1_MOVETYPE_BOUNCE || DotProduct(trace.plane.normal, ev->velocity) < 60) {
            ev->flags = (int)ev->flags | Q1_FL_ONGROUND;
            ev->groundentity = QC_ToProg(trace.ent >= 0 && !q1_free(trace.ent) ? trace.ent : 0);
            VectorClear(ev->velocity);
            VectorClear(ev->avelocity);
        }
    }

    // check for in water
    q1_check_water_transition(num);
}

/*
Monsters freefall when they don't have a ground entity, otherwise all movement
is done with discrete steps. This is also used for objects that have become
still on the ground, but will fall if the floor is pulled out from under them.
*/
static void q1_physics_step(int num)
{
    qc_entvars_t *ev = QC_EV(num);

    // freefall if not onground
    if (!((int)ev->flags & (Q1_FL_ONGROUND | Q1_FL_FLY | Q1_FL_SWIM))) {
        float gravity = sv_gravity ? sv_gravity->value : 800;
        bool hitsound = ev->velocity[2] < gravity * -0.1f;

        q1_add_gravity(ev);
        q1_check_velocity(ev);
        q1_fly_move(num, qvm.frametime);
        Q1_LinkEdict(num, true);

        if (qvm.ed[num].free)
            return;

        // just hit ground
        if (((int)ev->flags & Q1_FL_ONGROUND) && hitsound && Q1_SoundPrecached("demon/dland2.wav"))
            Q1_StartSound(num, 0, "demon/dland2.wav", 1, 1);
    }

    // regular thinking
    if (Q1_RunThink(num))
        q1_check_water_transition(num);
}

/*
Quake's SV_CheckWater: how deep the entity is, by its feet, its middle and its
eyes.
*/
void Q1_CheckWater(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t point;
    int cont;

    point[0] = ev->origin[0];
    point[1] = ev->origin[1];
    point[2] = ev->origin[2] + ev->mins[2] + 1;

    ev->waterlevel = 0;
    ev->watertype = Q1_CONTENTS_EMPTY;
    cont = Q1_PointContents(point);
    if (cont <= Q1_CONTENTS_WATER) {
        ev->watertype = cont;
        ev->waterlevel = 1;
        point[2] = ev->origin[2] + (ev->mins[2] + ev->maxs[2]) * 0.5f;
        cont = Q1_PointContents(point);
        if (cont <= Q1_CONTENTS_WATER) {
            ev->waterlevel = 2;
            point[2] = ev->origin[2] + ev->view_ofs[2];
            cont = Q1_PointContents(point);
            if (cont <= Q1_CONTENTS_WATER)
                ev->waterlevel = 3;
        }
    }
}

/*
A player's frame of the progs. Quake II's pmove has already moved them (in
ClientThink, as often as their commands came); what is left is what Quake did
around the move - the progs before and after, thinking, and falling as a body.
*/
static void q1_physics_client(int num)
{
    qc_entvars_t *ev = QC_EV(num);

    if (!Q1VM_ClientInGame(num))
        return;

    // call standard client pre-think
    qvm.g->time = qvm.time;
    qvm.g->self = QC_ToProg(num);
    QC_Execute(qvm.g->PlayerPreThink);

    q1_check_velocity(ev);

    switch ((int)ev->movetype) {
    case Q1_MOVETYPE_TOSS:
    case Q1_MOVETYPE_BOUNCE:
    case Q1_MOVETYPE_GIB:
        // a body (a dead player falls as one)
        q1_physics_toss(num);
        break;
    default:
        // moved by pmove
        if (!Q1_RunThink(num))
            return;
        break;
    }

    if (qvm.ed[num].free)
        return;

    // call standard player post-think
    Q1_LinkEdict(num, true);

    qvm.g->time = qvm.time;
    qvm.g->self = QC_ToProg(num);
    QC_Execute(qvm.g->PlayerPostThink);
}

void Q1_Physics(void)
{
    // let the progs know that a new frame has started
    qvm.g->self = 0;
    qvm.g->other = 0;
    qvm.g->time = qvm.time;
    if (qvm.g->StartFrame)
        QC_Execute(qvm.g->StartFrame);

    // treat each object in turn (num_edicts can grow as they run)
    for (int i = 0; i < qvm.num_edicts; i++) {
        if (qvm.ed[i].free)
            continue;

        qc_entvars_t *ev = QC_EV(i);

        if (qvm.g->force_retouch) {
            Q1_LinkEdict(i, true);  // force retouch even for stationary
            if (qvm.ed[i].free)
                continue;
        }

        if (i > 0 && i <= game.maxclients) {
            q1_physics_client(i);
            continue;
        }

        switch ((int)ev->movetype) {
        case Q1_MOVETYPE_PUSH:
            q1_physics_pusher(i);
            break;
        case Q1_MOVETYPE_NONE:
            q1_physics_none(i);
            break;
        case Q1_MOVETYPE_NOCLIP:
            q1_physics_noclip(i);
            break;
        case Q1_MOVETYPE_STEP:
            q1_physics_step(i);
            break;
        case Q1_MOVETYPE_TOSS:
        case Q1_MOVETYPE_BOUNCE:
        case Q1_MOVETYPE_GIB:
        case Q1_MOVETYPE_FLY:
        case Q1_MOVETYPE_FLYMISSILE:
            q1_physics_toss(i);
            break;
        case Q1_MOVETYPE_WALK:
            // a walking non-player (Quake errored); think at least
            q1_physics_none(i);
            break;
        default:
            QC_RunError("bad movetype %d on entity %d (%s)", (int)ev->movetype, i, QC_Str(ev->classname));
        }
    }

    if (qvm.g->force_retouch)
        qvm.g->force_retouch--;

    qvm.time += qvm.frametime;
}

/*
==============================================================================

MONSTER MOVEMENT

==============================================================================
*/

float Q1_DropToFloor(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t end;

    VectorCopy(ev->origin, end);
    end[2] -= 256;

    q1trace_t trace = Q1_Move(ev->origin, ev->mins, ev->maxs, end, Q1_MOVE_NORMAL, num);

    if (trace.fraction == 1 || trace.allsolid)
        return 0;

    VectorCopy(trace.endpos, ev->origin);
    Q1_LinkEdict(num, false);
    ev->flags = (int)ev->flags | Q1_FL_ONGROUND;
    ev->groundentity = QC_ToProg(trace.ent >= 0 ? trace.ent : 0);
    return 1;
}

void Q1_ChangeYaw(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    float current = q1_anglemod(ev->angles[1]);
    float ideal = ev->ideal_yaw;
    float speed = ev->yaw_speed;
    float move;

    if (current == ideal)
        return;

    move = ideal - current;
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

    ev->angles[1] = q1_anglemod(current + move);
}

/*
Returns false if any part of the bottom of the entity is off an edge that is
not a staircase.
*/
bool Q1_CheckBottom(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t mins, maxs, start, stop;
    float mid, bottom;
    int x, y;

    VectorAdd(ev->origin, ev->mins, mins);
    VectorAdd(ev->origin, ev->maxs, maxs);

    // if all of the points under the corners are solid world, don't bother
    // with the tougher checks
    start[2] = mins[2] - 1;
    for (x = 0; x <= 1; x++) {
        for (y = 0; y <= 1; y++) {
            start[0] = x ? maxs[0] : mins[0];
            start[1] = y ? maxs[1] : mins[1];
            if (Q1_PointContents(start) != Q1_CONTENTS_SOLID)
                goto realcheck;
        }
    }
    return true;    // we got out easy

realcheck:
    // check it for real...
    start[2] = mins[2];

    // the midpoint must be within 16 of the bottom
    start[0] = stop[0] = (mins[0] + maxs[0]) * 0.5f;
    start[1] = stop[1] = (mins[1] + maxs[1]) * 0.5f;
    stop[2] = start[2] - 2 * Q1_STEPSIZE;
    q1trace_t trace = Q1_Move(start, vec3_origin, vec3_origin, stop, Q1_MOVE_NOMONSTERS, num);

    if (trace.fraction == 1.0f)
        return false;
    mid = bottom = trace.endpos[2];

    // the corners must be within 16 of the midpoint
    for (x = 0; x <= 1; x++) {
        for (y = 0; y <= 1; y++) {
            start[0] = stop[0] = x ? maxs[0] : mins[0];
            start[1] = stop[1] = y ? maxs[1] : mins[1];

            trace = Q1_Move(start, vec3_origin, vec3_origin, stop, Q1_MOVE_NOMONSTERS, num);

            if (trace.fraction != 1.0f && trace.endpos[2] > bottom)
                bottom = trace.endpos[2];
            if (trace.fraction == 1.0f || mid - trace.endpos[2] > Q1_STEPSIZE)
                return false;
        }
    }

    return true;
}

/*
Called by monster program code. The move will be adjusted for slopes and
stairs, but if the move isn't possible, no move is done and false is returned.
*/
bool Q1_MoveStep(int num, const vec3_t move, bool relink)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t oldorg, neworg, end;
    q1trace_t trace;

    // try the move
    VectorCopy(ev->origin, oldorg);
    VectorAdd(ev->origin, move, neworg);

    // flying monsters don't step up
    if ((int)ev->flags & (Q1_FL_SWIM | Q1_FL_FLY)) {
        int enemy = QC_FromProg(ev->enemy);

        // try one move with vertical motion, then one without
        for (int i = 0; i < 2; i++) {
            VectorAdd(ev->origin, move, neworg);
            if (i == 0 && enemy != 0) {
                float dz = ev->origin[2] - QC_EV(enemy)->origin[2];
                if (dz > 40)
                    neworg[2] -= 8;
                if (dz < 30)
                    neworg[2] += 8;
            }
            trace = Q1_Move(ev->origin, ev->mins, ev->maxs, neworg, Q1_MOVE_NORMAL, num);

            if (trace.fraction == 1) {
                if (((int)ev->flags & Q1_FL_SWIM) && Q1_PointContents(trace.endpos) == Q1_CONTENTS_EMPTY)
                    return false;   // swim monster left water

                VectorCopy(trace.endpos, ev->origin);
                if (relink)
                    Q1_LinkEdict(num, true);
                return true;
            }

            if (enemy == 0)
                break;
        }

        return false;
    }

    // push down from a step height above the wished position
    neworg[2] += Q1_STEPSIZE;
    VectorCopy(neworg, end);
    end[2] -= Q1_STEPSIZE * 2;

    trace = Q1_Move(neworg, ev->mins, ev->maxs, end, Q1_MOVE_NORMAL, num);

    if (trace.allsolid)
        return false;

    if (trace.startsolid) {
        neworg[2] -= Q1_STEPSIZE;
        trace = Q1_Move(neworg, ev->mins, ev->maxs, end, Q1_MOVE_NORMAL, num);
        if (trace.allsolid || trace.startsolid)
            return false;
    }
    if (trace.fraction == 1) {
        // if monster had the ground pulled out, go ahead and fall
        if ((int)ev->flags & Q1_FL_PARTIALGROUND) {
            VectorAdd(ev->origin, move, ev->origin);
            if (relink)
                Q1_LinkEdict(num, true);
            ev->flags = (int)ev->flags & ~Q1_FL_ONGROUND;
            return true;
        }

        return false;   // walked off an edge
    }

    // check point traces down for dangling corners
    VectorCopy(trace.endpos, ev->origin);

    if (!Q1_CheckBottom(num)) {
        if ((int)ev->flags & Q1_FL_PARTIALGROUND) {
            // entity had floor mostly pulled out from underneath it and is
            // trying to correct
            if (relink)
                Q1_LinkEdict(num, true);
            return true;
        }
        VectorCopy(oldorg, ev->origin);
        return false;
    }

    if ((int)ev->flags & Q1_FL_PARTIALGROUND)
        ev->flags = (int)ev->flags & ~Q1_FL_PARTIALGROUND;

    if (trace.ent >= 0)
        ev->groundentity = QC_ToProg(trace.ent);

    // the move is ok
    if (relink)
        Q1_LinkEdict(num, true);
    return true;
}

bool Q1_WalkMove(int num, float yaw, float dist)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t move;

    if (!((int)ev->flags & (Q1_FL_ONGROUND | Q1_FL_FLY | Q1_FL_SWIM)))
        return false;

    yaw = yaw * M_PI * 2 / 360;
    move[0] = cos(yaw) * dist;
    move[1] = sin(yaw) * dist;
    move[2] = 0;

    return Q1_MoveStep(num, move, true);
}

/*
Turns to the movement direction, and walks the current distance if facing it.
*/
static bool q1_step_direction(int num, float yaw, float dist)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t move, oldorigin;

    ev->ideal_yaw = yaw;
    Q1_ChangeYaw(num);

    yaw = yaw * M_PI * 2 / 360;
    move[0] = cos(yaw) * dist;
    move[1] = sin(yaw) * dist;
    move[2] = 0;

    VectorCopy(ev->origin, oldorigin);
    if (Q1_MoveStep(num, move, false)) {
        float delta = ev->angles[YAW] - ev->ideal_yaw;
        if (delta > 45 && delta < 315) {
            // not turned far enough, so don't take the step
            VectorCopy(oldorigin, ev->origin);
        }
        Q1_LinkEdict(num, true);
        return true;
    }
    Q1_LinkEdict(num, true);
    return false;
}

#define Q1_DI_NODIR     -1

static void q1_new_chase_dir(int actor, int enemy, float dist)
{
    qc_entvars_t *av = QC_EV(actor), *ev = QC_EV(enemy);
    float d[3], tdir;
    float olddir = q1_anglemod((int)(av->ideal_yaw / 45) * 45);
    float turnaround = q1_anglemod(olddir - 180);

    float deltax = ev->origin[0] - av->origin[0];
    float deltay = ev->origin[1] - av->origin[1];
    if (deltax > 10)
        d[1] = 0;
    else if (deltax < -10)
        d[1] = 180;
    else
        d[1] = Q1_DI_NODIR;
    if (deltay < -10)
        d[2] = 270;
    else if (deltay > 10)
        d[2] = 90;
    else
        d[2] = Q1_DI_NODIR;

    // try direct route
    if (d[1] != Q1_DI_NODIR && d[2] != Q1_DI_NODIR) {
        if (d[1] == 0)
            tdir = d[2] == 90 ? 45 : 315;
        else
            tdir = d[2] == 90 ? 135 : 215;

        if (tdir != turnaround && q1_step_direction(actor, tdir, dist))
            return;
    }

    // try other directions
    if (((rand() & 3) & 1) || abs((int)deltay) > abs((int)deltax)) {
        tdir = d[1];
        d[1] = d[2];
        d[2] = tdir;
    }

    if (d[1] != Q1_DI_NODIR && d[1] != turnaround && q1_step_direction(actor, d[1], dist))
        return;

    if (d[2] != Q1_DI_NODIR && d[2] != turnaround && q1_step_direction(actor, d[2], dist))
        return;

    // there is no direct path to the player, so pick another direction

    if (olddir != Q1_DI_NODIR && q1_step_direction(actor, olddir, dist))
        return;

    if (rand() & 1) {
        // randomly determine direction of search
        for (tdir = 0; tdir <= 315; tdir += 45)
            if (tdir != turnaround && q1_step_direction(actor, tdir, dist))
                return;
    } else {
        for (tdir = 315; tdir >= 0; tdir -= 45)
            if (tdir != turnaround && q1_step_direction(actor, tdir, dist))
                return;
    }

    if (turnaround != Q1_DI_NODIR && q1_step_direction(actor, turnaround, dist))
        return;

    av->ideal_yaw = olddir;     // can't move

    // if a bridge was pulled out from underneath a monster, it may not have a
    // valid standing position at all
    if (!Q1_CheckBottom(actor))
        av->flags = (int)av->flags | Q1_FL_PARTIALGROUND;
}

static bool q1_close_enough(int ent, int goal, float dist)
{
    qc_entvars_t *ev = QC_EV(ent), *gv = QC_EV(goal);

    for (int i = 0; i < 3; i++) {
        if (gv->absmin[i] > ev->absmax[i] + dist)
            return false;
        if (gv->absmax[i] < ev->absmin[i] - dist)
            return false;
    }
    return true;
}

void Q1_MoveToGoal(int num, float dist)
{
    qc_entvars_t *ev = QC_EV(num);
    int goal = QC_FromProg(ev->goalentity);

    if (!((int)ev->flags & (Q1_FL_ONGROUND | Q1_FL_FLY | Q1_FL_SWIM))) {
        QC_RETURN_FLOAT(0);
        return;
    }

    // if the next step hits the enemy, return immediately
    if (QC_FromProg(ev->enemy) != 0 && q1_close_enough(num, goal, dist))
        return;

    // bump around...
    if ((rand() & 3) == 1 || !q1_step_direction(num, ev->ideal_yaw, dist))
        q1_new_chase_dir(num, goal, dist);
}
