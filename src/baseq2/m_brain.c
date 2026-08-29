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

brain

==============================================================================
*/

#include "g_local.h"
#include "m_brain.h"


static int  sound_chest_open;
static int  sound_tentacles_extend;
static int  sound_tentacles_retract;
static int  sound_death;
static int  sound_idle1;
static int  sound_idle2;
static int  sound_idle3;
static int  sound_pain1;
static int  sound_pain2;
static int  sound_sight;
static int  sound_search;
static int  sound_melee1;
static int  sound_melee2;
static int  sound_melee3;
static int  sound_laser_fly;


void brain_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void brain_search(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}


void brain_run(edict_t *self);
void brain_dead(edict_t *self);


//
// STAND
//

/*
=================
brain_shrink

[rerelease] Flatten the corpse partway through the death animation, and mark it
a dead monster there, instead of waiting for the animation to finish. A body
that falls in a doorway stops blocking it while the rest of the death plays.

Gated: this sits in a death table BOTH games play, and the original game keeps
its full-height corpse until the dead-frame handler runs.
=================
*/
static void brain_shrink(edict_t *self)
{
    if (!M_RereleaseGame())
        return;

    self->maxs[2] = 0;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

mframe_t brain_frames_stand [] = {
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },

    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },

    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL }
};
mmove_t brain_move_stand = {FRAME_stand01, FRAME_stand30, brain_frames_stand, NULL};

void brain_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &brain_move_stand;
}


//
// IDLE
//

mframe_t brain_frames_idle [] = {
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },

    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },

    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL },
    { ai_stand,   0,  NULL }
};
mmove_t brain_move_idle = {FRAME_stand31, FRAME_stand60, brain_frames_idle, brain_stand};

void brain_idle(edict_t *self)
{
    gi.sound(self, CHAN_AUTO, sound_idle3, 1, ATTN_IDLE, 0);
    self->monsterinfo.currentmove = &brain_move_idle;
}


//
// WALK
//
mframe_t brain_frames_walk1 [] = {
    { ai_walk,    7,  NULL },
    { ai_walk,    2,  NULL },
    { ai_walk,    3,  NULL },
    { ai_walk, 3, monster_footstep },
    { ai_walk,    1,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    9,  NULL },
    { ai_walk,    -4, NULL },
    { ai_walk, -1, monster_footstep },
    { ai_walk,    2,  NULL }
};
mmove_t brain_move_walk1 = {FRAME_walk101, FRAME_walk111, brain_frames_walk1, NULL};

// walk2 is FUBAR, do not use
#if 0
void brain_walk2_cycle(edict_t *self)
{
    if (random() > 0.1f)
        self->monsterinfo.nextframe = FRAME_walk220;
}

mframe_t brain_frames_walk2 [] = {
    { ai_walk,    3,  NULL },
    { ai_walk,    -2, NULL },
    { ai_walk,    -4, NULL },
    { ai_walk,    -3, NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    1,  NULL },
    { ai_walk,    12, NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    -3, NULL },
    { ai_walk,    0,  NULL },

    { ai_walk,    -2, NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    1,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    10, NULL },       // Cycle Start

    { ai_walk,    -1, NULL },
    { ai_walk,    7,  NULL },
    { ai_walk,    0,  NULL },
    { ai_walk,    3,  NULL },
    { ai_walk,    -3, NULL },
    { ai_walk,    2,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    -3, NULL },
    { ai_walk,    2,  NULL },
    { ai_walk,    0,  NULL },

    {
        ai_walk,    4,  brain_walk2_cycle,
        { ai_walk,    -1, NULL },
        { ai_walk,    -1, NULL },
        { ai_walk,    -8, NULL },
        { ai_walk,    0,  NULL },
        { ai_walk,    1,  NULL },
        { ai_walk,    5,  NULL },
        { ai_walk,    2,  NULL },
        { ai_walk,    -1, NULL },
        {
            ai_walk,    -5, NULL
        };
        mmove_t brain_move_walk2 = {FRAME_walk201, FRAME_walk240, brain_frames_walk2, NULL};
    }
#endif

void brain_walk(edict_t *self) {
//  if (random() <= 0.5)
    self->monsterinfo.currentmove = &brain_move_walk1;
//  else
//      self->monsterinfo.currentmove = &brain_move_walk2;
}



mframe_t brain_frames_defense [] =
{
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t brain_move_defense = {FRAME_defens01, FRAME_defens08, brain_frames_defense, NULL};

mframe_t brain_frames_pain3 [] =
{
    { ai_move,    -2, NULL },
    { ai_move,    2,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    -4, NULL }
};
mmove_t brain_move_pain3 = {FRAME_pain301, FRAME_pain306, brain_frames_pain3, brain_run};

mframe_t brain_frames_pain2 [] =
{
    { ai_move,    -2, NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    -2, NULL }
};
mmove_t brain_move_pain2 = {FRAME_pain201, FRAME_pain208, brain_frames_pain2, brain_run};

mframe_t brain_frames_pain1 [] =
{
    { ai_move,    -6, NULL },
    { ai_move,    -2, NULL },
    { ai_move, -6, monster_footstep },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    7,  NULL },
    { ai_move,    0,  NULL },
    { ai_move, 3, monster_footstep },
    { ai_move,    -1, NULL }
};
mmove_t brain_move_pain1 = {FRAME_pain101, FRAME_pain121, brain_frames_pain1, brain_run};


//
// DUCK
//




/*
=================
brain_duck_down

The brain is the one classic monster whose duck_down set no hold timer: id left
that to brain_dodge, which had already written (eta + 0.5) seconds.  The shared
monster_duck_down defaults to the flat one second the other five used, so put
the brain's own value back over it.
=================
*/
static void brain_duck_down(edict_t *self)
{
    int hold = self->monsterinfo.duck_wait_framenum;

    monster_duck_down(self);

    if (!M_RereleaseGame())
        self->monsterinfo.duck_wait_framenum = hold;
}

mframe_t brain_frames_duck [] =
{
    { ai_move,    0,  NULL },
    { ai_move,    -2, brain_duck_down },
    { ai_move,    17, monster_duck_hold },
    { ai_move,    -3, NULL },
    { ai_move,    -1, monster_duck_up },
    { ai_move,    -5, NULL },
    { ai_move,    -6, NULL },
    { ai_move,    -6, NULL }
};
mmove_t brain_move_duck = {FRAME_duck01, FRAME_duck08, brain_frames_duck, brain_run};

/*
=================
brain_duck

The ROGUE/rerelease dodge pair. Returning a bool is what lets
M_MonsterDodge fall back from a sidestep to a duck. Neither interrupts a
firing sequence - a monster that ducked mid-burst threw the shot away.
=================
*/
bool brain_duck(edict_t *self, float eta)
{
    self->monsterinfo.currentmove = &brain_move_duck;
    return true;
}

/*
=================
brain_dodge

monsterinfo.dodge for every brain, in both games.  The rerelease hands over to
M_MonsterDodge and its duck pair; the ORIGINAL game gets id's 1997
dodge back verbatim - a flat 25% chance of a plain crouch.

(This symbol also has to keep existing: g_ptrs_compat_v2.c is a frozen table
for version-2 saves and names it.)
=================
*/
void brain_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    if (M_RereleaseGame()) {
        M_MonsterDodge(self, attacker, eta, tr, gravity);
        return;
    }

    if (random() > 0.25f)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    // id wrote this to pause_framenum, which its own brain_duck_hold read.
    // The shared monster_duck_hold reads duck_wait_framenum, so set both.
    self->monsterinfo.pause_framenum = level.framenum + (eta + 0.5f) * BASE_FRAMERATE;
    self->monsterinfo.duck_wait_framenum = self->monsterinfo.pause_framenum;
    self->monsterinfo.currentmove = &brain_move_duck;
}


mframe_t brain_frames_death2 [] =
{
    { ai_move,    0,  NULL },
    { ai_move, 0, monster_footstep },
    { ai_move, 0, brain_shrink },
    { ai_move,    9,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t brain_move_death2 = {FRAME_death201, FRAME_death205, brain_frames_death2, brain_dead};

mframe_t brain_frames_death1 [] =
{
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    -2, NULL },
    { ai_move,    9,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t brain_move_death1 = {FRAME_death101, FRAME_death118, brain_frames_death1, brain_dead};


//
// MELEE
//

void brain_swing_right(edict_t *self) {
    gi.sound(self, CHAN_BODY, sound_melee1, 1, ATTN_NORM, 0);
}

void brain_hit_right(edict_t *self) {
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->maxs[0], 8);
    if (fire_hit(self, aim, (15 + (Q_rand() % 5)), 40))
        gi.sound(self, CHAN_WEAPON, sound_melee3, 1, ATTN_NORM, 0);
}

void brain_swing_left(edict_t *self) {
    gi.sound(self, CHAN_BODY, sound_melee2, 1, ATTN_NORM, 0);
}

void brain_hit_left(edict_t *self) {
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->mins[0], 8);
    if (fire_hit(self, aim, (15 + (Q_rand() % 5)), 40))
        gi.sound(self, CHAN_WEAPON, sound_melee3, 1, ATTN_NORM, 0);
}

mframe_t brain_frames_attack1 [] =
{
    { ai_charge,  8,  NULL },
    { ai_charge,  3,  NULL },
    { ai_charge,  5,  NULL },
    { ai_charge, 0, monster_footstep },
    { ai_charge,  -3, brain_swing_right },
    { ai_charge,  0,  NULL },
    { ai_charge,  -5, NULL },
    { ai_charge,  -7, brain_hit_right },
    { ai_charge,  0,  NULL },
    { ai_charge,  6,  brain_swing_left },
    { ai_charge,  1,  NULL },
    { ai_charge,  2,  brain_hit_left },
    { ai_charge,  -3, NULL },
    { ai_charge,  6,  NULL },
    { ai_charge,  -1, NULL },
    { ai_charge,  -3, NULL },
    { ai_charge,  2,  NULL },
    { ai_charge, -11, monster_footstep }
};
mmove_t brain_move_attack1 = {FRAME_attak101, FRAME_attak118, brain_frames_attack1, brain_run};

void brain_chest_open(edict_t *self) {
    self->spawnflags &= ~65536;
    self->monsterinfo.power_armor_type = POWER_ARMOR_NONE;
    gi.sound(self, CHAN_BODY, sound_chest_open, 1, ATTN_NORM, 0);
}

void brain_tentacle_attack(edict_t *self) {
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, 0, 8);
    if (fire_hit(self, aim, (10 + (Q_rand() % 5)), -600) && skill->value > 0)
        self->spawnflags |= 65536;
    gi.sound(self, CHAN_WEAPON, sound_tentacles_retract, 1, ATTN_NORM, 0);
}

void brain_chest_closed(edict_t *self) {
    self->monsterinfo.power_armor_type = POWER_ARMOR_SCREEN;
    if (self->spawnflags & 65536) {
        self->spawnflags &= ~65536;
        self->monsterinfo.currentmove = &brain_move_attack1;
    }
}

mframe_t brain_frames_attack2 [] =
{
    { ai_charge,  5,  NULL },
    { ai_charge,  -4, NULL },
    { ai_charge,  -4, NULL },
    { ai_charge,  -3, NULL },
    { ai_charge,  0,  brain_chest_open },
    { ai_charge,  0,  NULL },
    { ai_charge,  13, brain_tentacle_attack },
    { ai_charge,  0,  NULL },
    { ai_charge,  2,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  -9, brain_chest_closed },
    { ai_charge,  0,  NULL },
    { ai_charge,  4,  NULL },
    { ai_charge,  3,  NULL },
    { ai_charge,  2,  NULL },
    { ai_charge,  -3, NULL },
    { ai_charge,  -6, NULL }
};
mmove_t brain_move_attack2 = {FRAME_attak201, FRAME_attak217, brain_frames_attack2, brain_run};

void brain_melee(edict_t *self) {
    if (random() <= 0.5f)
        self->monsterinfo.currentmove = &brain_move_attack1;
    else
        self->monsterinfo.currentmove = &brain_move_attack2;
}


/*
=================
The rerelease's ranged brain - RAFAEL's code, identical in xatrix

attack3 is the old tentacle swipe (attak201-217) with a tongue grab woven into
it; attack4 fires a laser from each eye across the walk cycle (walk101-111).
Both animations exist in the 1997 tris.md2, so neither needs M_RereleaseAnims()
gating - but giving every brain a ranged attack changes how plain baseq2 plays,
so monsterinfo.attack is only hooked up under M_RereleaseGame().

Spawnflag 8 suppresses the lasers, matching SPAWNFLAG_BRAIN_NO_LASERS.
=================
*/
#define SPAWNFLAG_BRAIN_NO_LASERS   8

// the rerelease's RANGE_NEAR is a real distance; ours is an enum tag
#define BRAIN_RANGE_NEAR    440.0f

static bool brain_tounge_attack_ok(const vec3_t start, const vec3_t end)
{
    vec3_t  dir, angles;

    // check for max distance
    VectorSubtract(start, end, dir);
    if (VectorLength(dir) > 512)
        return false;

    // check for min/max pitch
    vectoangles(dir, angles);
    if (angles[0] < -180)
        angles[0] += 360;
    if (fabsf(angles[0]) > 30)
        return false;

    return true;
}

void brain_tounge_attack(edict_t *self)
{
    vec3_t  offset, start, f, r, end, dir;
    trace_t tr;
    int     damage;

    if (!self->enemy)
        return;

    AngleVectors(self->s.angles, f, r, NULL);
    VectorSet(offset, 24, 0, 16);
    G_ProjectSource(self->s.origin, offset, f, r, start);

    // try the enemy's centre, then the top of its head, then its feet
    VectorCopy(self->enemy->s.origin, end);
    if (!brain_tounge_attack_ok(start, end)) {
        end[2] = self->enemy->s.origin[2] + self->enemy->maxs[2] - 8;
        if (!brain_tounge_attack_ok(start, end)) {
            end[2] = self->enemy->s.origin[2] + self->enemy->mins[2] + 8;
            if (!brain_tounge_attack_ok(start, end))
                return;
        }
    }
    VectorCopy(self->enemy->s.origin, end);

    tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);
    if (tr.ent != self->enemy)
        return;

    damage = 5;
    gi.sound(self, CHAN_WEAPON, sound_tentacles_retract, 1, ATTN_NORM, 0);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_PARASITE_ATTACK);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(end);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    VectorSubtract(start, end, dir);
    T_Damage(self->enemy, self, self, dir, self->enemy->s.origin, vec3_origin,
             damage, 0, DAMAGE_NO_KNOCKBACK, MOD_BRAINTENTACLE);

    // pull the enemy in
    self->s.origin[2] += 1;
    AngleVectors(self->s.angles, f, NULL, NULL);
    VectorScale(f, -1200, self->enemy->velocity);
}

// brain right eye centre, one entry per walk101-111 frame
static const vec3_t brain_reye[11] = {
    {   0.746700f,  0.238370f, 34.167690f },
    {  -1.076390f,  0.238370f, 33.386372f },
    {  -1.335500f,  5.334300f, 32.177170f },
    {  -0.175360f,  8.846370f, 30.635479f },
    {  -2.757590f,  7.804610f, 30.150860f },
    {  -5.575090f,  5.152840f, 30.056160f },
    {  -7.017550f,  3.262470f, 30.552521f },
    {  -7.915740f,  0.638800f, 33.176189f },
    {  -3.915390f,  8.285730f, 33.976349f },
    {  -0.913540f, 10.933030f, 34.141811f },
    {  -0.369900f,  8.923900f, 34.189079f }
};

// brain left eye centre
static const vec3_t brain_leye[11] = {
    {  -3.364710f,  0.327750f, 33.938381f },
    {  -5.140450f,  0.493480f, 32.659851f },
    {  -5.341980f,  5.646980f, 31.277901f },
    {  -4.134480f,  9.277440f, 29.925621f },
    {  -6.598340f,  6.815090f, 29.322620f },
    {  -8.610840f,  2.529650f, 29.251591f },
    {  -9.231360f,  0.093280f, 29.747959f },
    { -11.004110f,  1.936930f, 32.395260f },
    {  -7.878310f,  7.648190f, 33.148151f },
    {  -4.947370f, 11.430050f, 33.313610f },
    {  -4.332820f,  9.444570f, 33.526340f }
};

static void brain_eye_laser(edict_t *self, const vec3_t angles, const vec3_t eye)
{
    vec3_t   forward, right, up, start;
    edict_t *ent;

    AngleVectors(angles, forward, right, up);

    VectorCopy(self->s.origin, start);
    VectorMA(start, eye[0], right, start);
    VectorMA(start, eye[1], forward, start);
    VectorMA(start, eye[2], up, start);

    ent = G_Spawn();
    ent->classname = "brain_laserbeam";
    VectorCopy(angles, ent->s.angles);
    VectorCopy(start, ent->s.origin);
    ent->enemy = self->enemy;
    ent->owner = self;
    ent->dmg = 1;
    monster_dabeam(ent);
}

void brain_laserbeam(edict_t *self)
{
    vec3_t  dir, angles;
    int     i;

    if (!self->enemy)
        return;

    // the eye tables are indexed by walk-cycle frame; attack4 runs over exactly
    // those frames, but clamp anyway so a stray call cannot read off the end
    i = self->s.frame - FRAME_walk101;
    if (i < 0 || i >= 11)
        return;

    if (random() > 0.8f)
        gi.sound(self, CHAN_AUTO, sound_laser_fly, 1, ATTN_STATIC, 0);

    VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
    vectoangles(dir, angles);

    brain_eye_laser(self, angles, brain_reye[i]);
    brain_eye_laser(self, angles, brain_leye[i]);
}

void brain_laserbeam_reattack(edict_t *self)
{
    if (random() < 0.5f)
        if (visible(self, self->enemy))
            if (self->enemy->health > 0)
                self->s.frame = FRAME_walk101;
}

mframe_t brain_frames_attack3 [] =
{
    { ai_charge,  5,  NULL },
    { ai_charge,  -4, NULL },
    { ai_charge,  -4, NULL },
    { ai_charge,  -3, NULL },
    { ai_charge,  0,  brain_chest_open },
    { ai_charge,  0,  brain_tounge_attack },
    { ai_charge,  13, NULL },
    { ai_charge,  0,  brain_tentacle_attack },
    { ai_charge,  2,  NULL },
    { ai_charge,  0,  brain_tounge_attack },
    { ai_charge,  -9, brain_chest_closed },
    { ai_charge,  0,  NULL },
    { ai_charge,  4,  NULL },
    { ai_charge,  3,  NULL },
    { ai_charge,  2,  NULL },
    { ai_charge,  -3, NULL },
    { ai_charge,  -6, NULL }
};
mmove_t brain_move_attack3 = {FRAME_attak201, FRAME_attak217, brain_frames_attack3, brain_run};

mframe_t brain_frames_attack4 [] =
{
    { ai_charge, 9,  brain_laserbeam },
    { ai_charge, 2,  brain_laserbeam },
    { ai_charge, 3,  brain_laserbeam },
    { ai_charge, 3,  brain_laserbeam },
    { ai_charge, 1,  brain_laserbeam },
    { ai_charge, 0,  brain_laserbeam },
    { ai_charge, 0,  brain_laserbeam },
    { ai_charge, 10, brain_laserbeam },
    { ai_charge, -4, brain_laserbeam },
    { ai_charge, -1, brain_laserbeam },
    { ai_charge, 2,  brain_laserbeam_reattack }
};
mmove_t brain_move_attack4 = {FRAME_walk101, FRAME_walk111, brain_frames_attack4, brain_run};

void brain_attack(edict_t *self)
{
    if (!self->enemy)
        return;

    if (realrange(self, self->enemy) <= BRAIN_RANGE_NEAR) {
        if (random() < 0.5f)
            self->monsterinfo.currentmove = &brain_move_attack3;
        else if (!(self->spawnflags & SPAWNFLAG_BRAIN_NO_LASERS))
            self->monsterinfo.currentmove = &brain_move_attack4;
    } else if (!(self->spawnflags & SPAWNFLAG_BRAIN_NO_LASERS)) {
        self->monsterinfo.currentmove = &brain_move_attack4;
    }
}

//
// RUN
//

mframe_t brain_frames_run [] =
{
    { ai_run, 9,  NULL },
    { ai_run, 2,  NULL },
    { ai_run, 3,  NULL },
    { ai_run, 3,  NULL },
    { ai_run, 1,  NULL },
    { ai_run, 0,  NULL },
    { ai_run, 0,  NULL },
    { ai_run, 10, NULL },
    { ai_run, -4, NULL },
    { ai_run, -1, NULL },
    { ai_run, 2,  NULL }
};
mmove_t brain_move_run = {FRAME_walk101, FRAME_walk111, brain_frames_run, NULL};

void brain_run(edict_t *self) {
    self->monsterinfo.power_armor_type = POWER_ARMOR_SCREEN;
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &brain_move_stand;
    else
        self->monsterinfo.currentmove = &brain_move_run;
}


void brain_pain(edict_t *self, edict_t *other, float kick, int damage) {
    float   r;

    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
    if (skill->value == 3)
        return;     // no pain anims in nightmare

    r = random();
    if (r < 0.33f) {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &brain_move_pain1;
    } else if (r < 0.66f) {
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &brain_move_pain2;
    } else {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &brain_move_pain3;
    }
}

void brain_dead(edict_t *self) {
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}



void brain_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point) {
    int     n;

    self->s.effects = 0;
    self->monsterinfo.power_armor_type = POWER_ARMOR_NONE;

// check for gib
    if (self->health <= self->gib_health) {
        // Stock Quake II: one burst of gibs and the body is gone.
        if (!LUDICROUS_GIBS()) {
            gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
            for (n = 0; n < 2; n++)
                ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
            for (n = 0; n < 4; n++)
                ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
            self->deadflag = DEAD_DEAD;
            return;
        }

        // LUDICROUS GIBS: the burst scales with what killed it, and the
        // corpse is left shootable so it can be torn down in stages.
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
		if (InflictorGibExplosion(inflictor, self)) {
			VectorScale(self->size, 1.2, self->size);

			for (n = 0; n < 16; n++) {
				if (n < 8) {
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
				}
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			}

			ThrowGib(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
			ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
			VectorScale(self->size, 0.8, self->size);
		}
		else if (!Q_stricmp(inflictor->classname, "bolt")) {
			ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			self->takedamage = DAMAGE_YES;
		}
		else if (inflictor->client == NULL) {
			if (self->takedamage != DAMAGE_MAYBE && self->takedamage != DAMAGE_NO) {
				for (n = 0; n < 8; n++)
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);

				self->takedamage = DAMAGE_MAYBE;
			}
			else {
				for (n = 0; n < 8; n++)
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);

				ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
				ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
				self->takedamage = DAMAGE_NO;
			}
		}
		else {
			if (!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_machinegun")) {
				ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
				self->takedamage = DAMAGE_YES;
			}
			else if (!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_supershotgun") ||
				!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_shotgun")) {
				if (self->death_count < 3) {
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					self->takedamage = DAMAGE_YES;
				}
				else {
					for (n = 0; n < 8; n++) {
						ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					}
					ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
					ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
					self->takedamage = DAMAGE_NO;
				}
			}
			else if (!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_chaingun")) {
				if (self->death_count < 3) {
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					self->takedamage = DAMAGE_YES;
				}
				else {
					for (n = 0; n < 8; n++) {
						ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
						ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					}
					ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
					ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
					self->takedamage = DAMAGE_NO;
				}
			}
			else if (!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_railgun")) {
				for (n = 0; n < 8; n++)
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);

				ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
				ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
			}
			else {
				if (self->takedamage != DAMAGE_MAYBE && self->takedamage != DAMAGE_NO) {
					for (n = 0; n < 8; n++)
						ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);

					self->takedamage = DAMAGE_MAYBE;
				}
				else {
					for (n = 0; n < 8; n++)
						ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);

					ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
					ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
					self->takedamage = DAMAGE_NO;
				}
			}
		}
        self->deadflag = DEAD_DEAD;
        return;
    }

    if (self->deadflag == DEAD_DEAD)
        return;

// regular death
    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    if (random() <= 0.5f)
        self->monsterinfo.currentmove = &brain_move_death1;
    else
        self->monsterinfo.currentmove = &brain_move_death2;
}

/*QUAKED monster_brain (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_brain(edict_t *self) {
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_chest_open = gi.soundindex("brain/brnatck1.wav");
    sound_tentacles_extend = gi.soundindex("brain/brnatck2.wav");
    sound_tentacles_retract = gi.soundindex("brain/brnatck3.wav");
    sound_death = gi.soundindex("brain/brndeth1.wav");
    sound_idle1 = gi.soundindex("brain/brnidle1.wav");
    sound_idle2 = gi.soundindex("brain/brnidle2.wav");
    sound_idle3 = gi.soundindex("brain/brnlens1.wav");
    sound_pain1 = gi.soundindex("brain/brnpain1.wav");
    sound_pain2 = gi.soundindex("brain/brnpain2.wav");
    sound_sight = gi.soundindex("brain/brnsght1.wav");
    sound_search = gi.soundindex("brain/brnsrch1.wav");
    sound_melee1 = gi.soundindex("brain/melee1.wav");
    sound_melee2 = gi.soundindex("brain/melee2.wav");
    sound_melee3 = gi.soundindex("brain/melee3.wav");
    sound_laser_fly = gi.soundindex("misc/lasfly.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/brain/tris.md2");
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 32);

    self->health = 300;
    self->gib_health = -150;
    self->mass = 400;

    self->pain = brain_pain;
    self->die = brain_die;

    self->monsterinfo.stand = brain_stand;
    self->monsterinfo.walk = brain_walk;
    self->monsterinfo.run = brain_run;
    // brain_dodge is the classic dodge in baseq2 and forwards to M_MonsterDodge
    // in the rerelease.  duck/unduck are what M_MonsterDodge drives, so the
    // original game must not advertise them at all.
    self->monsterinfo.dodge = brain_dodge;
    if (M_RereleaseGame()) {
        self->monsterinfo.duck = brain_duck;
        self->monsterinfo.unduck = monster_duck_up;
    }
    // the rerelease brain is a ranged monster; the classic one is melee-only
    if (M_RereleaseGame())
        self->monsterinfo.attack = brain_attack;
    self->monsterinfo.melee = brain_melee;
    self->monsterinfo.sight = brain_sight;
    self->monsterinfo.search = brain_search;
    self->monsterinfo.idle = brain_idle;

    self->monsterinfo.power_armor_type = POWER_ARMOR_SCREEN;
    self->monsterinfo.power_armor_power = 100;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &brain_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    walkmonster_start(self);
}
