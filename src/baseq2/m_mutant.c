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

mutant

==============================================================================
*/

#include "g_local.h"
#include "m_mutant.h"


static int  sound_swing;
static int  sound_hit;
static int  sound_hit2;
static int  sound_death;
static int  sound_idle;
static int  sound_pain1;
static int  sound_pain2;
static int  sound_sight;
static int  sound_search;
static int  sound_step1;
static int  sound_step2;
static int  sound_step3;
static int  sound_thud;

//
// SOUNDS
//

#define SPAWNFLAG_MUTANT_NOJUMPING   8

void mutant_step(edict_t *self)
{
    int     n;
    n = (Q_rand() + 1) % 3;
    if (n == 0)
        gi.sound(self, CHAN_VOICE, sound_step1, 1, ATTN_NORM, 0);
    else if (n == 1)
        gi.sound(self, CHAN_VOICE, sound_step2, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_step3, 1, ATTN_NORM, 0);
}

void mutant_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void mutant_search(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}

void mutant_swing(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_swing, 1, ATTN_NORM, 0);
}


//
// STAND
//

mframe_t mutant_frames_stand [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },      // 10

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },      // 20

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },      // 30

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },      // 40

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },      // 50

    { ai_stand, 0, NULL }
};
mmove_t mutant_move_stand = {FRAME_stand101, FRAME_stand151, mutant_frames_stand, NULL};

void mutant_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_stand;
}


//
// IDLE
//

void mutant_idle_loop(edict_t *self)
{
    if (random() < 0.75f)
        self->monsterinfo.nextframe = FRAME_stand155;
}

mframe_t mutant_frames_idle [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },                  // scratch loop start
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, mutant_idle_loop },      // scratch loop end
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t mutant_move_idle = {FRAME_stand152, FRAME_stand164, mutant_frames_idle, mutant_stand};

void mutant_idle(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_idle;
    gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}


//
// WALK
//

void mutant_walk(edict_t *self);

mframe_t mutant_frames_walk [] = {
    { ai_walk,    3,      NULL },
    { ai_walk,    1,      NULL },
    { ai_walk,    5,      NULL },
    { ai_walk,    10,     NULL },
    { ai_walk,    13,     NULL },
    { ai_walk,    10,     NULL },
    { ai_walk,    0,      NULL },
    { ai_walk,    5,      NULL },
    { ai_walk,    6,      NULL },
    { ai_walk,    16,     NULL },
    { ai_walk,    15,     NULL },
    { ai_walk,    6,      NULL }
};
mmove_t mutant_move_walk = {FRAME_walk05, FRAME_walk16, mutant_frames_walk, NULL};

void mutant_walk_loop(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_walk;
}

mframe_t mutant_frames_start_walk [] = {
    { ai_walk,    5,      NULL },
    { ai_walk,    5,      NULL },
    { ai_walk,    -2,     NULL },
    { ai_walk,    1,      NULL }
};
mmove_t mutant_move_start_walk = {FRAME_walk01, FRAME_walk04, mutant_frames_start_walk, mutant_walk_loop};

void mutant_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_start_walk;
}


//
// RUN
//

mframe_t mutant_frames_run [] = {
    { ai_run, 40,     NULL },
    { ai_run, 40,     mutant_step },
    { ai_run, 24,     NULL },
    { ai_run, 5,      mutant_step },
    { ai_run, 17,     NULL },
    { ai_run, 10,     NULL }
};
mmove_t mutant_move_run = {FRAME_run03, FRAME_run08, mutant_frames_run, NULL};

void mutant_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &mutant_move_stand;
    else
        self->monsterinfo.currentmove = &mutant_move_run;
}


//
// MELEE
//

/*
=================
mutant_swipe_damage

[rerelease] 5..15 rather than the classic 10..14.  A miss also starts a 1.5
second melee debounce, which is the pivot the whole rerelease mutant turns on:
having whiffed, it stops trying to swipe and is allowed to jump away and come
back in, instead of standing in your face swinging.
=================
*/
static int mutant_swipe_damage(void)
{
    if (M_RereleaseGame())
        return 5 + (Q_rand() % 10);

    return 10 + (Q_rand() % 5);
}

static void mutant_swipe_missed(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_swing, 1, ATTN_NORM, 0);

    if (M_RereleaseGame())
        self->monsterinfo.melee_debounce_framenum = level.framenum + 1.5f * BASE_FRAMERATE;
}

void mutant_hit_left(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->mins[0], 8);
    if (fire_hit(self, aim, mutant_swipe_damage(), 100))
        gi.sound(self, CHAN_WEAPON, sound_hit, 1, ATTN_NORM, 0);
    else
        mutant_swipe_missed(self);
}

void mutant_hit_right(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->maxs[0], 8);
    if (fire_hit(self, aim, mutant_swipe_damage(), 100))
        gi.sound(self, CHAN_WEAPON, sound_hit2, 1, ATTN_NORM, 0);
    else
        mutant_swipe_missed(self);
}

void mutant_check_refire(edict_t *self)
{
    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0)
        return;

    // [rerelease] refires on any skill at 50%, not only on nightmare, but only
    // while it has not just whiffed - the debounce is what lets it disengage
    if (M_RereleaseGame()) {
        if (self->monsterinfo.melee_debounce_framenum <= level.framenum
            && (random() < 0.5f || range(self, self->enemy) == RANGE_MELEE))
            self->monsterinfo.nextframe = FRAME_attack09;
        return;
    }

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attack09;
}

mframe_t mutant_frames_attack [] = {
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  mutant_hit_left },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  mutant_hit_right },
    { ai_charge,  0,  mutant_check_refire }
};
mmove_t mutant_move_attack = {FRAME_attack09, FRAME_attack15, mutant_frames_attack, mutant_run};

void mutant_melee(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_attack;
}


//
// ATTACK
//

void mutant_jump_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (self->health <= 0) {
        self->touch = NULL;
        return;
    }

    // [rerelease] self->style is a one-shot latch armed at takeoff, so a single
    // leap can only body-slam once.  Ours used to be able to damage on every
    // touch for the whole flight.
    if (other->takedamage && (!M_RereleaseGame() || self->style == 1)) {
        if (VectorLength(self->velocity) > 400) {
            vec3_t  point;
            vec3_t  normal;
            int     damage;

            VectorCopy(self->velocity, normal);
            VectorNormalize(normal);
            VectorMA(self->s.origin, self->maxs[0], normal, point);
            damage = 40 + 10 * random();
            T_Damage(other, self, self, self->velocity, point, normal, damage, damage, 0, MOD_UNKNOWN);
            self->style = 0;
        }
    }

    if (!M_CheckBottom(self)) {
        if (self->groundentity) {
            self->monsterinfo.nextframe = FRAME_attack02;
            self->touch = NULL;
        }
        return;
    }

    self->touch = NULL;
}

void mutant_jump_takeoff(edict_t *self)
{
    vec3_t  forward;

    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
    AngleVectors(self->s.angles, forward, NULL, NULL);
    self->s.origin[2] += 1;

    // [rerelease] a shorter, flatter pounce - 400/150 against the classic
    // 600/250.  Paired with the 265 unit cap in mutant_check_jump this turns
    // the leap into a gap-closer instead of a room-crossing charge.
    if (M_RereleaseGame()) {
        VectorScale(forward, 400, self->velocity);
        self->velocity[2] = 150;
        self->style = 1;
    } else {
        VectorScale(forward, 600, self->velocity);
        self->velocity[2] = 250;
    }

    self->groundentity = NULL;
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->monsterinfo.attack_finished = level.framenum + 3 * BASE_FRAMERATE;
    self->touch = mutant_jump_touch;
}

void mutant_check_landing(edict_t *self)
{
    // [rerelease] clears the blocked-system jump state; ours never did, so a
    // pounce left jump_framenum armed
    if (M_RereleaseGame())
        monster_jump_finished(self);

    if (self->groundentity) {
        gi.sound(self, CHAN_WEAPON, sound_thud, 1, ATTN_NORM, 0);

        // [rerelease] a real cooldown after landing, and a swipe if the leap
        // actually arrived.  Ours zeroed attack_finished, which let the mutant
        // re-roll a jump on the very next think - this is the biggest single
        // reason ours pounces over and over where the rerelease's runs at you.
        if (M_RereleaseGame()) {
            self->monsterinfo.attack_finished = level.framenum +
                (0.5f + random()) * BASE_FRAMERATE;
            self->monsterinfo.aiflags &= ~AI_DUCKED;

            // id tests range_to <= RANGE_MELEE * 2, a 40 unit gap between
            // boxes.  MELEE_DISTANCE (80) is that same reach on this tree's
            // origin-to-origin measure - see M_RangeBetween in g_ai.c.
            if (self->enemy && realrange(self, self->enemy) <= MELEE_DISTANCE)
                self->monsterinfo.melee(self);

            return;
        }

        self->monsterinfo.attack_finished = 0;
        self->monsterinfo.aiflags &= ~AI_DUCKED;
        return;
    }

    if (level.framenum > self->monsterinfo.attack_finished)
        self->monsterinfo.nextframe = FRAME_attack02;
    else
        self->monsterinfo.nextframe = FRAME_attack05;
}

mframe_t mutant_frames_jump [] = {
    { ai_charge,   0, NULL },
    { ai_charge,  17, NULL },
    { ai_charge,  15, mutant_jump_takeoff },
    { ai_charge,  15, NULL },
    { ai_charge,  15, mutant_check_landing },
    { ai_charge,   0, NULL },
    { ai_charge,   3, NULL },
    { ai_charge,   0, NULL }
};
mmove_t mutant_move_jump = {FRAME_attack01, FRAME_attack08, mutant_frames_jump, mutant_run};

void mutant_jump(edict_t *self)
{
    self->monsterinfo.currentmove = &mutant_move_jump;
}


//
// CHECKATTACK
//

bool mutant_check_melee(edict_t *self)
{
    if (range(self, self->enemy) != RANGE_MELEE)
        return false;

    // [rerelease] a mutant that just whiffed does not keep swinging - it backs
    // off and lets mutant_check_jump take over
    if (M_RereleaseGame() && self->monsterinfo.melee_debounce_framenum > level.framenum)
        return false;

    return true;
}

/*
=================
mutant_check_jump

[rerelease] Rewritten by id, and the difference is the whole reason our mutants
pounce down a corridor where the rerelease's run down it.

Classic: any distance over 100 units, 10% per check, no cooldown at all.  With
mutant_check_landing zeroing attack_finished, a mutant that lands still out of
reach re-rolls immediately - so at any range it eventually leaps, over and over,
and a doorway it cannot fit through becomes a loop.

Rerelease: capped at 265 units ("only use it to close distance gaps"), 50% but
gated behind attack_finished, and the two 1997 height tests replaced by a single
"can we even reach standing height" check.  The under-100 bail is also now
conditional on the melee debounce, so a mutant that has just whiffed IS allowed
to jump back out of a fight it is losing.
=================
*/
bool mutant_check_jump(edict_t *self)
{
    vec3_t  v;
    float   distance;
    bool    rerelease = M_RereleaseGame();

    if (rerelease) {
        // no way we could reach standing height
        if (self->absmin[2] + 125 < self->enemy->absmin[2])
            return false;
    } else {
        if (self->absmin[2] > (self->enemy->absmin[2] + 0.75f * self->enemy->size[2]))
            return false;

        if (self->absmax[2] < (self->enemy->absmin[2] + 0.25f * self->enemy->size[2]))
            return false;
    }

    v[0] = self->s.origin[0] - self->enemy->s.origin[0];
    v[1] = self->s.origin[1] - self->enemy->s.origin[1];
    v[2] = 0;
    distance = VectorLength(v);

    if (rerelease) {
        // already on top of the enemy, and not trying to escape a melee
        if (distance < 100 && self->monsterinfo.melee_debounce_framenum <= level.framenum)
            return false;

        // only use it to close distance gaps
        if (distance > 265)
            return false;

        return self->monsterinfo.attack_finished < level.framenum && random() < 0.5f;
    }

    if (distance < 100)
        return false;
    if (distance > 100) {
        if (random() < 0.9f)
            return false;
    }

    return true;
}

bool mutant_checkattack(edict_t *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return false;

    if (mutant_check_melee(self)) {
        self->monsterinfo.attack_state = AS_MELEE;
        return true;
    }

    // [rerelease] the NoJumping spawnflag suppresses the pounce as well as the
    // blocked-system ledge jumps; ours only ever checked it for the latter
    if ((!M_RereleaseGame() || !(self->spawnflags & SPAWNFLAG_MUTANT_NOJUMPING))
        && mutant_check_jump(self)) {
        self->monsterinfo.attack_state = AS_MISSILE;
        // FIXME play a jump sound here
        return true;
    }

    return false;
}


//
// PAIN
//

mframe_t mutant_frames_pain1 [] = {
    { ai_move,    4,  NULL },
    { ai_move,    -3, NULL },
    { ai_move,    -8, NULL },
    { ai_move,    2,  NULL },
    { ai_move,    5,  NULL }
};
mmove_t mutant_move_pain1 = {FRAME_pain101, FRAME_pain105, mutant_frames_pain1, mutant_run};

mframe_t mutant_frames_pain2 [] = {
    { ai_move,    -24, NULL },
    { ai_move,    11, NULL },
    { ai_move,    5,  NULL },
    { ai_move,    -2, NULL },
    { ai_move,    6,  NULL },
    { ai_move,    4,  NULL }
};
mmove_t mutant_move_pain2 = {FRAME_pain201, FRAME_pain206, mutant_frames_pain2, mutant_run};

mframe_t mutant_frames_pain3 [] = {
    { ai_move,    -22, NULL },
    { ai_move,    3,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    6,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    1,  NULL }
};
mmove_t mutant_move_pain3 = {FRAME_pain301, FRAME_pain311, mutant_frames_pain3, mutant_run};

void mutant_pain(edict_t *self, edict_t *other, float kick, int damage)
{
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
        self->monsterinfo.currentmove = &mutant_move_pain1;
    } else if (r < 0.66f) {
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &mutant_move_pain2;
    } else {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &mutant_move_pain3;
    }
}


//
// DEATH
//

/*
=================
mutant_shrink

[rerelease] Flattens the corpse mid-death-animation and marks it a dead monster
right there, so the body stops blocking the doorway it fell in while the rest of
the animation plays out.  Ours only did this at the very end, in mutant_dead.
=================
*/
static void mutant_shrink(edict_t *self)
{
    self->maxs[2] = 0;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

void mutant_dead(edict_t *self)
{
    // [rerelease] a longer, flatter corpse box that matches the sprawled pose
    if (M_RereleaseGame()) {
        VectorSet(self->mins, 0, -48, -24);
        VectorSet(self->maxs, 64, 16, -8);
    } else {
        VectorSet(self->mins, -16, -16, -24);
        VectorSet(self->maxs, 16, 16, -8);
    }
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);

    M_FlyCheck(self);
}

mframe_t mutant_frames_death1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  mutant_shrink },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t mutant_move_death1 = {FRAME_death101, FRAME_death109, mutant_frames_death1, mutant_dead};

mframe_t mutant_frames_death2 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  mutant_shrink },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t mutant_move_death2 = {FRAME_death201, FRAME_death210, mutant_frames_death2, mutant_dead};

void mutant_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

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
				ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
				ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);				
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
				for (n = 0; n < 8; n++) {
					ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
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

    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->s.skinnum = 1;

    if (random() < 0.5f)
        self->monsterinfo.currentmove = &mutant_move_death1;
    else
        self->monsterinfo.currentmove = &mutant_move_death2;
}


//
// SPAWN
//

/*QUAKED monster_mutant (1 .5 0) (-32 -32 -24) (32 32 32) Ambush Trigger_Spawn Sight
*/

/*
=================
mutant jumps - the rerelease/ROGUE blocked system

monsterinfo.blocked is called from SV_NewChaseDir when the mutant has run out
of step directions.  jump_up hops onto a ledge, jump_down drops off one, and
blocked_checkplat rides func_plats.  All of this runs on the APPENDED jump
frames, so blocked_checkjump refuses unless M_RereleaseAnims() is on.
=================
*/

static void mutant_jump_down(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

static void mutant_jump_up(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 200, forward, self->velocity);
    VectorMA(self->velocity, 450, up, self->velocity);
}

static void mutant_jump_wait_land(edict_t *self)
{
    if (!monster_jump_finished(self) && self->groundentity == NULL)
        self->monsterinfo.nextframe = self->s.frame;
    else
        self->monsterinfo.nextframe = self->s.frame + 1;
}

mframe_t mutant_frames_jump_up [] = {
    { ai_move, -8, NULL },
    { ai_move, -8, mutant_jump_up },
    { ai_move, 0, mutant_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t mutant_move_jump_up = {FRAME_jump01, FRAME_jump05, mutant_frames_jump_up, mutant_run};

mframe_t mutant_frames_jump_down [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, mutant_jump_down },
    { ai_move, 0, mutant_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t mutant_move_jump_down = {FRAME_jump01, FRAME_jump05, mutant_frames_jump_down, mutant_run};

void mutant_jump_updown(edict_t *self, blocked_jump_result_t result)
{
    if (!self->enemy)
        return;

    if (result == JUMP_JUMP_UP)
        self->monsterinfo.currentmove = &mutant_move_jump_up;
    else
        self->monsterinfo.currentmove = &mutant_move_jump_down;
}

bool mutant_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            mutant_jump_updown(self, result);
        return true;
    }

    if (blocked_checkplat(self, dist))
        return true;

    return false;
}

void SP_monster_mutant(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_swing = gi.soundindex("mutant/mutatck1.wav");
    sound_hit = gi.soundindex("mutant/mutatck2.wav");
    sound_hit2 = gi.soundindex("mutant/mutatck3.wav");
    sound_death = gi.soundindex("mutant/mutdeth1.wav");
    sound_idle = gi.soundindex("mutant/mutidle1.wav");
    sound_pain1 = gi.soundindex("mutant/mutpain1.wav");
    sound_pain2 = gi.soundindex("mutant/mutpain2.wav");
    sound_sight = gi.soundindex("mutant/mutsght1.wav");
    sound_search = gi.soundindex("mutant/mutsrch1.wav");
    sound_step1 = gi.soundindex("mutant/step1.wav");
    sound_step2 = gi.soundindex("mutant/step2.wav");
    sound_step3 = gi.soundindex("mutant/step3.wav");
    sound_thud = gi.soundindex("mutant/thud1.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/mutant/tris.md2");
    // [rerelease] id shrank the mutant a lot: 36 units wide and 54 tall,
    // against the classic 64 wide and 72 tall.  This is why ours snags on a
    // half-open door where the rerelease's runs straight through - the classic
    // box simply does not fit through the gap.  The QUAKED comment in id's own
    // m_mutant.cpp still says the old size; the code is what ships.
    if (M_RereleaseGame()) {
        VectorSet(self->mins, -18, -18, -24);
        VectorSet(self->maxs, 18, 18, 30);
    } else {
        VectorSet(self->mins, -32, -32, -24);
        VectorSet(self->maxs, 32, 32, 48);
    }

    self->health = 300;
    self->gib_health = -120;
    self->mass = 300;

    self->pain = mutant_pain;
    self->die = mutant_die;

    self->monsterinfo.stand = mutant_stand;
    self->monsterinfo.walk = mutant_walk;
    self->monsterinfo.run = mutant_run;
    self->monsterinfo.dodge = NULL;
    self->monsterinfo.attack = mutant_jump;
    self->monsterinfo.melee = mutant_melee;
    self->monsterinfo.sight = mutant_sight;
    self->monsterinfo.search = mutant_search;
    self->monsterinfo.idle = mutant_idle;
    self->monsterinfo.checkattack = mutant_checkattack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &mutant_move_stand;

    self->monsterinfo.scale = MODEL_SCALE;
    // ROGUE/rerelease: let the mutant jump ledges and ride plats.  The jump
    // animations only exist on the rerelease model, so blocked_checkjump
    // gates itself on M_RereleaseAnims(); the plat half needs no frames.
    if (M_RereleaseGame()) {
        self->monsterinfo.blocked = mutant_blocked;
        self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_MUTANT_NOJUMPING);
        self->monsterinfo.drop_height = 256;
        self->monsterinfo.jump_height = 68;
    }

    walkmonster_start(self);
}
