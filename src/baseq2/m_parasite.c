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

parasite

==============================================================================
*/

#include "g_local.h"
#include "m_parasite.h"


static int  sound_pain1;
static int  sound_pain2;
static int  sound_die;
static int  sound_launch;
static int  sound_impact;
static int  sound_suck;
static int  sound_reelin;
static int  sound_sight;
static int  sound_tap;
static int  sound_scratch;
static int  sound_search;


void parasite_stand(edict_t *self);
void parasite_start_run(edict_t *self);
void parasite_run(edict_t *self);
void parasite_walk(edict_t *self);
void parasite_start_walk(edict_t *self);
void parasite_end_fidget(edict_t *self);
void parasite_do_fidget(edict_t *self);
void parasite_refidget(edict_t *self);


void parasite_launch(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_launch, 1, ATTN_NORM, 0);
}

void parasite_reel_in(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_reelin, 1, ATTN_NORM, 0);
}

void parasite_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_WEAPON, sound_sight, 1, ATTN_NORM, 0);
}

void parasite_tap(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_tap, 1, ATTN_IDLE, 0);
}

void parasite_scratch(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_scratch, 1, ATTN_IDLE, 0);
}

void parasite_search(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_search, 1, ATTN_IDLE, 0);
}


/*
=================
parasite_shrink

[rerelease] Flatten the corpse partway through the death animation, and mark it
a dead monster there, instead of waiting for the animation to finish. A body
that falls in a doorway stops blocking it while the rest of the death plays.

Gated: this sits in a death table BOTH games play, and the original game keeps
its full-height corpse until the dead-frame handler runs.
=================
*/
static void parasite_shrink(edict_t *self)
{
    if (!M_RereleaseGame())
        return;

    self->maxs[2] = 0;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

mframe_t parasite_frames_start_fidget [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t parasite_move_start_fidget = {FRAME_stand18, FRAME_stand21, parasite_frames_start_fidget, parasite_do_fidget};

mframe_t parasite_frames_fidget [] = {
    { ai_stand, 0, parasite_scratch },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_scratch },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t parasite_move_fidget = {FRAME_stand22, FRAME_stand27, parasite_frames_fidget, parasite_refidget};

mframe_t parasite_frames_end_fidget [] = {
    { ai_stand, 0, parasite_scratch },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t parasite_move_end_fidget = {FRAME_stand28, FRAME_stand35, parasite_frames_end_fidget, parasite_stand};

void parasite_end_fidget(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_end_fidget;
}

void parasite_do_fidget(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_fidget;
}

void parasite_refidget(edict_t *self)
{
    if (random() <= 0.8f)
        self->monsterinfo.currentmove = &parasite_move_fidget;
    else
        self->monsterinfo.currentmove = &parasite_move_end_fidget;
}

void parasite_idle(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_start_fidget;
}


mframe_t parasite_frames_stand [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap },
    { ai_stand, 0, NULL },
    { ai_stand, 0, parasite_tap }
};
mmove_t parasite_move_stand = {FRAME_stand01, FRAME_stand17, parasite_frames_stand, parasite_stand};

void parasite_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_stand;
}


mframe_t parasite_frames_run [] = {
    { ai_run, 30, NULL },
    { ai_run, 30, NULL },
    { ai_run, 22, monster_footstep },
    { ai_run, 19, monster_footstep },
    { ai_run, 24, NULL },
    { ai_run, 28, monster_footstep },
    { ai_run, 25, monster_footstep }
};
mmove_t parasite_move_run = {FRAME_run03, FRAME_run09, parasite_frames_run, NULL};

mframe_t parasite_frames_start_run [] = {
    { ai_run, 0,  NULL },
    { ai_run, 30, NULL },
};
mmove_t parasite_move_start_run = {FRAME_run01, FRAME_run02, parasite_frames_start_run, parasite_run};

mframe_t parasite_frames_stop_run [] = {
    { ai_run, 20, NULL },
    { ai_run, 20, NULL },
    { ai_run, 12, NULL },
    { ai_run, 10, NULL },
    { ai_run, 0,  NULL },
    { ai_run, 0,  NULL }
};
mmove_t parasite_move_stop_run = {FRAME_run10, FRAME_run15, parasite_frames_stop_run, NULL};

void parasite_start_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &parasite_move_stand;
    else
        self->monsterinfo.currentmove = &parasite_move_start_run;
}

void parasite_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &parasite_move_stand;
    else
        self->monsterinfo.currentmove = &parasite_move_run;
}


mframe_t parasite_frames_walk [] = {
    { ai_walk, 30, NULL },
    { ai_walk, 30, NULL },
    { ai_walk, 22, monster_footstep },
    { ai_walk, 19, monster_footstep },
    { ai_walk, 24, NULL },
    { ai_walk, 28, monster_footstep },
    { ai_walk, 25, monster_footstep }
};
mmove_t parasite_move_walk = {FRAME_run03, FRAME_run09, parasite_frames_walk, parasite_walk};

mframe_t parasite_frames_start_walk [] = {
    { ai_walk, 0, NULL },
    { ai_walk, 30, parasite_walk }
};
mmove_t parasite_move_start_walk = {FRAME_run01, FRAME_run02, parasite_frames_start_walk, NULL};

mframe_t parasite_frames_stop_walk [] = {
    { ai_walk, 20, NULL },
    { ai_walk, 20,    NULL },
    { ai_walk, 12, NULL },
    { ai_walk, 10, NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL }
};
mmove_t parasite_move_stop_walk = {FRAME_run10, FRAME_run15, parasite_frames_stop_walk, NULL};

void parasite_start_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_start_walk;
}

void parasite_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &parasite_move_walk;
}


mframe_t parasite_frames_pain1 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 6, NULL },
    { ai_move, 16, NULL },
    { ai_move, -6, NULL },
    { ai_move, -7, NULL },
    { ai_move, 0, NULL }
};
mmove_t parasite_move_pain1 = {FRAME_pain101, FRAME_pain111, parasite_frames_pain1, parasite_start_run};

void parasite_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);

    self->monsterinfo.currentmove = &parasite_move_pain1;
}


static bool parasite_drain_attack_ok(vec3_t start, vec3_t end)
{
    vec3_t  dir, angles;

    // check for max distance
    VectorSubtract(start, end, dir);
    if (VectorLength(dir) > 256)
        return false;

    // check for min/max pitch
    vectoangles(dir, angles);
    if (angles[0] < -180)
        angles[0] += 360;
    if (fabsf(angles[0]) > 30)
        return false;

    return true;
}

void parasite_drain_attack(edict_t *self)
{
    vec3_t  offset, start, f, r, end, dir;
    trace_t tr;
    int damage;

    AngleVectors(self->s.angles, f, r, NULL);
    VectorSet(offset, 24, 0, 6);
    G_ProjectSource(self->s.origin, offset, f, r, start);

    VectorCopy(self->enemy->s.origin, end);
    if (!parasite_drain_attack_ok(start, end)) {
        end[2] = self->enemy->s.origin[2] + self->enemy->maxs[2] - 8;
        if (!parasite_drain_attack_ok(start, end)) {
            end[2] = self->enemy->s.origin[2] + self->enemy->mins[2] + 8;
            if (!parasite_drain_attack_ok(start, end))
                return;
        }
    }
    VectorCopy(self->enemy->s.origin, end);

    tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);
    if (tr.ent != self->enemy)
        return;

    if (self->s.frame == FRAME_drain03) {
        damage = 5;
        gi.sound(self->enemy, CHAN_AUTO, sound_impact, 1, ATTN_NORM, 0);
    } else {
        if (self->s.frame == FRAME_drain04)
            gi.sound(self, CHAN_WEAPON, sound_suck, 1, ATTN_NORM, 0);
        damage = 2;
    }

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_PARASITE_ATTACK);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(end);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    VectorSubtract(start, end, dir);
    T_Damage(self->enemy, self, self, dir, self->enemy->s.origin, vec3_origin, damage, 0, DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN);
}

mframe_t parasite_frames_drain [] = {
    { ai_charge, 0,   parasite_launch },
    { ai_charge, 0,   NULL },
    { ai_charge, 15,  parasite_drain_attack },          // Target hits
    { ai_charge, 0,   parasite_drain_attack },          // drain
    { ai_charge, 0,   parasite_drain_attack },          // drain
    { ai_charge, 0,   parasite_drain_attack },          // drain
    { ai_charge, 0,   parasite_drain_attack },          // drain
    { ai_charge, -2,  parasite_drain_attack },          // drain
    { ai_charge, -2,  parasite_drain_attack },          // drain
    { ai_charge, -3,  parasite_drain_attack },          // drain
    { ai_charge, -2,  parasite_drain_attack },          // drain
    { ai_charge, 0,   parasite_drain_attack },          // drain
    { ai_charge, -1,  parasite_drain_attack },          // drain
    { ai_charge, 0,   parasite_reel_in },               // let go
    { ai_charge, -2,  NULL },
    { ai_charge, -2,  NULL },
    { ai_charge, -3,  NULL },
    { ai_charge, 0,   NULL }
};
mmove_t parasite_move_drain = {FRAME_drain01, FRAME_drain18, parasite_frames_drain, parasite_start_run};

/*
===
Break Stuff Ends
===
*/

/*
=================
The rerelease parasite's PROBOSCIS - a real projectile on a tether

The classic parasite's drain is a hitscan: parasite_drain_attack traces to the
enemy every frame and draws a beam if it connects.  The rerelease replaced it
with a physical barb that flies out, sticks in whatever it hits, drains health
back down the tether, and reels in - and if it hits a wall instead, the parasite
plays a whole animation ripping itself free.

Ported from src/rerelease/m_parasite.cpp.  It runs on FRAME_drain01-drain18 and
FRAME_break01-break32, all of which exist in the CLASSIC parasite model, so no
M_RereleaseAnims() gating is needed - but it changes the parasite's behaviour
completely, so parasite_attack only picks it under M_RereleaseGame().

TWO ADAPTATIONS, both deliberate:

 1. The tether is drawn with a per-frame TE_PARASITE_ATTACK rather than the
    rerelease's persistent RF_BEAM segment entity.  Our client discards the
    model on an RF_BEAM entity outright (`ent.model = 0` in CL_AddPacketEntities)
    and draws the palette cylinder instead, whereas TE_PARASITE_ATTACK already
    routes through CL_ParseBeam with cl_mod_parasite_segment - i.e. the real
    segment model, stretched.  The classic drain already sent that TE every
    frame, so this is the proven path in this tree and it needs no second entity.

 2. `tip->style` carries the state machine, exactly as the rerelease does:
      0 = flying, 1 = stuck in something, 2 = retracting, 3 = spent.
=================
*/

// how fast the barb flies, and how much harder it is yanked back
#define PROBOSCIS_SPEED         1250.0f
#define PROBOSCIS_RETRACT_MUL   2.0f

void parasite_reel_in(edict_t *self);
void proboscis_reset(edict_t *self);
static void proboscis_retract(edict_t *self);
extern mmove_t parasite_move_fire_proboscis;
extern mmove_t parasite_move_break;

// muzzle offsets, one per frame of break01.. and drain01.., so the tether
// leaves the mouth rather than the model origin
static const vec3_t parasite_break_offsets[] = {
    {  7.0f,   0.0f,  7.0f }, {  6.3f,  14.5f,  4.0f }, {  8.5f,   0.0f,  5.6f },
    {  5.0f, -15.25f, 4.0f }, {  9.5f,  -1.8f,  5.9f }, {  6.2f,  14.0f,  4.0f },
    { 12.25f,  7.5f,  1.4f }, { 13.8f,   0.0f, -2.4f }, { 13.8f,   0.0f, -4.0f },
    {  0.1f,   0.0f, -0.7f }, {  5.0f,   0.0f,  3.7f }, { 11.0f,   0.0f,  4.0f },
    { 13.5f,   0.0f, -4.0f }, { 13.5f,   0.0f, -4.0f }, {  0.2f,   0.0f, -0.7f },
    {  3.9f,   0.0f,  3.6f }, {  8.5f,   0.0f,  5.0f }, { 14.0f,   0.0f, -4.0f },
    { 14.0f,   0.0f, -4.0f }, {  0.1f,   0.0f, -0.5f }
};

static const vec3_t parasite_drain_offsets[] = {
    { -1.7f, 0.0f,  1.2f }, { -2.2f, 0.0f, -0.6f }, {  7.7f, 0.0f,  7.2f },
    {  7.2f, 0.0f,  5.7f }, {  6.2f, 0.0f,  7.8f }, {  4.7f, 0.0f,  6.7f },
    {  5.0f, 0.0f,  9.0f }, {  5.0f, 0.0f,  7.0f }, {  5.0f, 0.0f, 10.5f },
    {  4.5f, 0.0f,  9.7f }, {  1.5f, 0.0f, 12.0f }, {  2.9f, 0.0f, 11.0f },
    {  2.1f, 0.0f,  7.6f }
};

static void parasite_get_proboscis_start(edict_t *self, vec3_t start)
{
    vec3_t f, r, offset;
    int    i;

    AngleVectors(self->s.angles, f, r, NULL);

    i = self->s.frame - FRAME_break01;
    if (i >= 0 && i < (int)q_countof(parasite_break_offsets)) {
        VectorCopy(parasite_break_offsets[i], offset);
    } else {
        i = self->s.frame - FRAME_drain01;
        if (i >= 0 && i < (int)q_countof(parasite_drain_offsets))
            VectorCopy(parasite_drain_offsets[i], offset);
        else
            VectorSet(offset, 8, 0, 6);
    }

    G_ProjectSource(self->s.origin, offset, f, r, start);
}

// Draw the tether.  cl_mod_parasite_segment is the real segment model, stretched
// between the two points by CL_ParseBeam.
static void proboscis_segment_draw(edict_t *parasite, vec3_t start, vec3_t end)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_PARASITE_ATTACK);
    gi.WriteShort(parasite - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(end);
    gi.multicast(parasite->s.origin, MULTICAST_PVS);
}

// hard reset; like we never existed
void proboscis_reset(edict_t *self)
{
    if (self->owner)
        self->owner->proboscus = NULL;
    G_FreeEdict(self);
}

void proboscis_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    // only a crusher can destroy the barb outright
    if (meansOfDeath == MOD_CRUSH)
        proboscis_reset(self);
}

static void proboscis_retract(edict_t *self)
{
    // tell the parasite to start reeling, if it is still in the drain animation
    if (self->owner && self->owner->monsterinfo.currentmove == &parasite_move_fire_proboscis)
        self->owner->monsterinfo.nextframe = FRAME_drain12;

    self->movetype = MOVETYPE_NONE;
    self->solid = SOLID_NOT;

    // comes back harder than it went out
    if (self->style != 2)
        self->speed *= PROBOSCIS_RETRACT_MUL;
    self->style = 2;
    gi.linkentity(self);
}

void proboscis_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t p, dir;

    // owner isn't trying to probe any more, don't touch anything
    if (!self->owner || self->owner->monsterinfo.currentmove != &parasite_move_fire_proboscis)
        return;

    if (other == self->owner)
        return;

    // hit what we want to succ
    if (other->client || other == self->owner->enemy) {
        VectorCopy(self->s.origin, p);

        self->owner->monsterinfo.nextframe = FRAME_drain06;
        self->movetype = MOVETYPE_NONE;
        self->solid = SOLID_NOT;
        self->style = 1;
        // stick to this guy - remember where on him we landed
        VectorSubtract(p, other->s.origin, self->move_origin);
        self->enemy = other;
        gi.sound(self, CHAN_WEAPON, sound_suck, 1, ATTN_NORM, 0);
    } else if (other->svflags & (SVF_MONSTER | SVF_DEADMONSTER)) {
        // another monster: a scratch, and pull straight back
        proboscis_retract(self);
    } else {
        // hit the world; stick to it and rip free with the break animation
        self->movetype = MOVETYPE_NONE;
        self->solid = SOLID_NOT;
        self->style = 1;
        self->owner->monsterinfo.currentmove = &parasite_move_break;
        self->owner->s.angles[YAW] = self->s.angles[YAW];
    }

    if (other->takedamage) {
        VectorCopy(self->velocity, dir);
        VectorNormalize(dir);
        T_Damage(other, self, self->owner, dir, self->s.origin, vec3_origin,
                 5, 0, 0, MOD_UNKNOWN);
    }

    gi.positioned_sound(self->s.origin, self->owner, CHAN_AUTO, sound_impact, 1, ATTN_NORM, 0);

    self->nextthink = level.framenum + 1;
    gi.linkentity(self);
}

void proboscis_think(edict_t *self)
{
    vec3_t start, dir;
    float  dist;

    self->nextthink = level.framenum + 1;

    if (!self->owner || !self->owner->inuse) {
        proboscis_reset(self);
        return;
    }

    parasite_get_proboscis_start(self->owner, start);

    // ---- retracting: keep pulling until we reach the parasite ----
    if (self->style == 2) {
        VectorSubtract(self->s.origin, start, dir);
        dist = VectorNormalize(dir);

        if (dist <= self->speed * 2 * FRAMETIME) {
            // home; let the parasite know and go away next frame
            self->style = 3;
            self->think = proboscis_reset;
            VectorCopy(start, self->s.origin);
            gi.linkentity(self);
            return;
        }

        VectorMA(self->s.origin, -(self->speed * FRAMETIME), dir, self->s.origin);
        gi.linkentity(self);
    }
    // ---- stuck: drain, and check the victim is still there ----
    else if (self->style == 1) {
        if (!self->enemy) {
            // stuck in a wall; nothing to do but wait for the break animation
        } else if (!self->enemy->inuse || self->enemy->health <= 0 || !self->enemy->takedamage) {
            proboscis_retract(self);
        } else {
            trace_t tr;

            // ride along with whatever we are stuck in
            VectorAdd(self->enemy->s.origin, self->move_origin, self->s.origin);

            VectorSubtract(self->s.origin, start, dir);
            VectorNormalize(dir);
            vectoangles(dir, self->s.angles);

            // did the world come between us?
            tr = gi.trace(start, NULL, NULL, self->s.origin, NULL, MASK_SOLID);
            if (tr.fraction != 1.0f) {
                proboscis_retract(self);
                VectorCopy(self->s.old_origin, self->s.origin);
            } else if (level.framenum >= self->timestamp) {
                // succ & drain
                T_Damage(self->enemy, self, self->owner, dir, self->s.origin, vec3_origin,
                         2, 0, DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN);
                self->owner->health = min(self->owner->max_health, self->owner->health + 2);
                self->timestamp = level.framenum + 1;
            }

            gi.linkentity(self);
        }
    }
    // ---- flying ----
    else if (self->style == 0) {
        edict_t *target = self->owner->enemy;

        if (!target || !target->inuse || target->health <= 0) {
            proboscis_retract(self);
            return;
        }

        // if we are well past the target and still going, give up and reel in
        // rather than sail off across the map
        VectorSubtract(self->s.origin, target->s.origin, dir);
        dist = VectorNormalize(dir);

        if (dist > (self->speed * 2) / 15.0f) {
            vec3_t from_owner;

            VectorSubtract(self->s.origin, self->owner->s.origin, from_owner);
            VectorNormalize(from_owner);

            if (DotProduct(dir, from_owner) > 0)
                proboscis_retract(self);
        }
    }

    // the tether follows the barb wherever it is
    if (self->inuse)
        proboscis_segment_draw(self->owner, start, self->s.origin);
}

static void fire_proboscis(edict_t *self, vec3_t start, vec3_t dir, float speed)
{
    edict_t *tip;

    tip = G_Spawn();
    tip->classname = "proboscis";
    vectoangles(dir, tip->s.angles);
    tip->s.modelindex = gi.modelindex("models/monsters/parasite/tip/tris.md2");
    tip->movetype = MOVETYPE_FLYMISSILE;
    tip->owner = self;
    self->proboscus = tip;
    tip->clipmask = MASK_SHOT;
    VectorCopy(start, tip->s.origin);
    VectorCopy(start, tip->s.old_origin);
    tip->speed = speed;
    VectorScale(dir, speed, tip->velocity);
    tip->solid = SOLID_BBOX;
    VectorClear(tip->mins);
    VectorClear(tip->maxs);
    tip->takedamage = DAMAGE_YES;
    tip->health = 1000;       // only MOD_CRUSH is meant to kill it
    tip->flags |= FL_NO_KNOCKBACK;
    tip->die = proboscis_die;
    tip->touch = proboscis_touch;
    tip->think = proboscis_think;
    tip->nextthink = level.framenum + 1;
    tip->svflags |= SVF_DEADMONSTER;    // do not block other monsters' movement

    gi.linkentity(tip);
}

static void parasite_fire_proboscis(edict_t *self)
{
    vec3_t start, dir;

    if (!self->enemy)
        return;

    // a barb still out there is abandoned rather than left dangling
    if (self->proboscus && self->proboscus->style != 2)
        proboscis_reset(self->proboscus);

    parasite_get_proboscis_start(self, start);
    PredictAim(self->enemy, start, PROBOSCIS_SPEED, false, crandom() * 0.03f, dir, NULL);

    fire_proboscis(self, start, dir, PROBOSCIS_SPEED);
}

// hold on the drain frames while the barb is still out
static void parasite_proboscis_wait(edict_t *self)
{
    if (self->s.frame == FRAME_drain04)
        self->monsterinfo.nextframe = FRAME_drain05;
    else
        self->monsterinfo.nextframe = FRAME_drain04;
}

// hold on the reel-in frames until the barb is home
static void parasite_proboscis_pull_wait(edict_t *self)
{
    // barb gone?
    if (!self->proboscus || self->proboscus->style == 3) {
        self->monsterinfo.nextframe = FRAME_drain14;
        return;
    }

    if (self->s.frame == FRAME_drain12)
        self->monsterinfo.nextframe = FRAME_drain13;
    else
        self->monsterinfo.nextframe = FRAME_drain12;

    if (self->proboscus->style != 2)
        proboscis_retract(self->proboscus);
}

// ai_charge, but it also keeps the tether drawn while the barb is out
static void parasite_charge_proboscis(edict_t *self, float dist)
{
    if (self->s.frame >= FRAME_break01 && self->s.frame <= FRAME_break32)
        ai_move(self, dist);
    else
        ai_charge(self, dist);
}

mframe_t parasite_frames_fire_proboscis [] = {
    { parasite_charge_proboscis, 0,   parasite_launch },
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, 15,  parasite_fire_proboscis },   // target hits
    { parasite_charge_proboscis, 0,   parasite_proboscis_wait },   // drain
    { parasite_charge_proboscis, 0,   parasite_proboscis_wait },   // drain
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, -3,  NULL },
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, 0,   parasite_proboscis_pull_wait },
    { parasite_charge_proboscis, -1,  parasite_proboscis_pull_wait },
    { parasite_charge_proboscis, 0,   parasite_reel_in },          // let go
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, -3,  NULL },
    { parasite_charge_proboscis, 0,   NULL }
};
mmove_t parasite_move_fire_proboscis = {FRAME_drain01, FRAME_drain18, parasite_frames_fire_proboscis, parasite_start_run};

/*
=================
parasite_move_break - ripping the barb back out of a wall

id shipped these frames in the original model and never used them; the classic
code has the table sitting inside an #if 0.  The rerelease finally wires it up
as what happens when the proboscis sticks in the world instead of in you.
=================
*/
static void parasite_break_wait(edict_t *self)
{
    // barb gone?
    if (self->proboscus && self->proboscus->style != 3)
        self->monsterinfo.nextframe = FRAME_break19;
    else if (Q_rand() & 1) {
        // do not tear a chunk out of ourselves
        parasite_reel_in(self);
        self->monsterinfo.nextframe = FRAME_break31;
    }
}

static void parasite_break_retract(edict_t *self)
{
    if (self->proboscus)
        proboscis_retract(self->proboscus);
}

static void parasite_break_sound(edict_t *self)
{
    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
}

static void parasite_break_noise(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}

mframe_t parasite_frames_break [] = {
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, -3,  parasite_break_noise },
    { parasite_charge_proboscis, 1,   NULL },
    { parasite_charge_proboscis, 2,   NULL },
    { parasite_charge_proboscis, -3,  NULL },
    { parasite_charge_proboscis, 1,   NULL },
    { parasite_charge_proboscis, 1,   NULL },
    { parasite_charge_proboscis, 3,   NULL },
    { parasite_charge_proboscis, 0,   parasite_break_noise },
    { parasite_charge_proboscis, -18, NULL },
    { parasite_charge_proboscis, 3,   NULL },
    { parasite_charge_proboscis, 9,   NULL },
    { parasite_charge_proboscis, 6,   NULL },
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, -18, NULL },
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, 8,   parasite_break_retract },
    { parasite_charge_proboscis, 9,   NULL },
    { parasite_charge_proboscis, 0,   parasite_break_wait },
    { parasite_charge_proboscis, -18, parasite_break_sound },
    { parasite_charge_proboscis, 0,   NULL },
    { parasite_charge_proboscis, 0,   NULL },   // airborne
    { parasite_charge_proboscis, 0,   NULL },   // airborne
    { parasite_charge_proboscis, 0,   NULL },   // slides
    { parasite_charge_proboscis, 0,   NULL },   // slides
    { parasite_charge_proboscis, 0,   NULL },   // slides
    { parasite_charge_proboscis, 0,   NULL },   // slides
    { parasite_charge_proboscis, 4,   NULL },
    { parasite_charge_proboscis, 11,  NULL },
    { parasite_charge_proboscis, -2,  NULL },
    { parasite_charge_proboscis, -5,  NULL },
    { parasite_charge_proboscis, 1,   NULL }
};
mmove_t parasite_move_break = {FRAME_break01, FRAME_break32, parasite_frames_break, parasite_start_run};

void parasite_attack(edict_t *self)
{
    // the rerelease fires a physical barb on a tether; the classic drain is
    // a hitscan on the same frames
    if (M_RereleaseGame())
        self->monsterinfo.currentmove = &parasite_move_fire_proboscis;
    else
        self->monsterinfo.currentmove = &parasite_move_drain;
}



/*
===
Death Stuff Starts
===
*/

void parasite_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

mframe_t parasite_frames_death [] = {
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0, parasite_shrink },
    { ai_move, 0, monster_footstep },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL }
};
mmove_t parasite_move_death = {FRAME_death101, FRAME_death107, parasite_frames_death, parasite_dead};

void parasite_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

    // Kill the barb with its owner. proboscis_think only self-destructs when
    // the owner has left the world, and a dead parasite is still a linked
    // corpse edict - so without this the tip hangs in mid-air, keeps draining
    // the player and keeps healing the body it is attached to.
    // style 2 is already retracting; id lets that one reel home on its own.
    if (self->proboscus && self->proboscus->style != 2)
        proboscis_reset(self->proboscus);

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
				if (n < 6) {
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
				}
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
				ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
			}				

			ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
			VectorScale(self->size, 0.8, self->size);
		}
		else if (!Q_stricmp(inflictor->classname, "bolt")) {
			ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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
				for (n = 0; n < 8; n++) {
					ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
				}

				
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
    gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.currentmove = &parasite_move_death;
}

/*
===
End Death Stuff
===
*/

/*QUAKED monster_parasite (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/

/*
=================
parasite jumps - the rerelease/ROGUE blocked system

monsterinfo.blocked is called from SV_NewChaseDir when the parasite has run out
of step directions.  jump_up hops onto a ledge, jump_down drops off one, and
blocked_checkplat rides func_plats.  All of this runs on the APPENDED jump
frames, so blocked_checkjump refuses unless M_RereleaseAnims() is on.
=================
*/
#define SPAWNFLAG_PARASITE_NOJUMPING   8

static void parasite_jump_down(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

static void parasite_jump_up(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 200, forward, self->velocity);
    VectorMA(self->velocity, 450, up, self->velocity);
}

static void parasite_jump_wait_land(edict_t *self)
{
    if (self->groundentity == NULL) {
        self->monsterinfo.nextframe = self->s.frame;

        if (monster_jump_finished(self))
            self->monsterinfo.nextframe = self->s.frame + 1;
    } else {
        self->monsterinfo.nextframe = self->s.frame + 1;
    }
}

mframe_t parasite_frames_jump_up [] = {
    { ai_move, -8, NULL },
    { ai_move, -8, NULL },
    { ai_move, -8, NULL },
    { ai_move, -8, parasite_jump_up },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, parasite_jump_wait_land },
    { ai_move, 0, NULL },
};
mmove_t parasite_move_jump_up = {FRAME_jump01, FRAME_jump08, parasite_frames_jump_up, parasite_run};

mframe_t parasite_frames_jump_down [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, parasite_jump_down },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, parasite_jump_wait_land },
    { ai_move, 0, NULL },
};
mmove_t parasite_move_jump_down = {FRAME_jump01, FRAME_jump08, parasite_frames_jump_down, parasite_run};

void parasite_jump_updown(edict_t *self, blocked_jump_result_t result)
{
    if (!self->enemy)
        return;

    if (result == JUMP_JUMP_UP)
        self->monsterinfo.currentmove = &parasite_move_jump_up;
    else
        self->monsterinfo.currentmove = &parasite_move_jump_down;
}

bool parasite_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            parasite_jump_updown(self, result);
        return true;
    }

    if (blocked_checkplat(self, dist))
        return true;

    return false;
}

void SP_monster_parasite(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_pain1 = gi.soundindex("parasite/parpain1.wav");
    sound_pain2 = gi.soundindex("parasite/parpain2.wav");
    sound_die = gi.soundindex("parasite/pardeth1.wav");
    sound_launch = gi.soundindex("parasite/paratck1.wav");
    sound_impact = gi.soundindex("parasite/paratck2.wav");
    sound_suck = gi.soundindex("parasite/paratck3.wav");
    sound_reelin = gi.soundindex("parasite/paratck4.wav");
    sound_sight = gi.soundindex("parasite/parsght1.wav");
    sound_tap = gi.soundindex("parasite/paridle1.wav");
    sound_scratch = gi.soundindex("parasite/paridle2.wav");
    sound_search = gi.soundindex("parasite/parsrch1.wav");

    self->s.modelindex = gi.modelindex("models/monsters/parasite/tris.md2");
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 24);
    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    self->health = 175;
    self->gib_health = -50;
    self->mass = 250;

    self->pain = parasite_pain;
    self->die = parasite_die;

    self->monsterinfo.stand = parasite_stand;
    self->monsterinfo.walk = parasite_start_walk;
    self->monsterinfo.run = parasite_start_run;
    self->monsterinfo.attack = parasite_attack;
    self->monsterinfo.sight = parasite_sight;
    self->monsterinfo.idle = parasite_idle;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &parasite_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    // ROGUE/rerelease: let the parasite jump ledges and ride plats.  The jump
    // animations only exist on the rerelease model, so blocked_checkjump
    // gates itself on M_RereleaseAnims(); the plat half needs no frames.
    if (M_RereleaseGame()) {
        self->monsterinfo.blocked = parasite_blocked;
        self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_PARASITE_NOJUMPING);
        self->monsterinfo.drop_height = 256;
        self->monsterinfo.jump_height = 68;
    }

    walkmonster_start(self);
}
