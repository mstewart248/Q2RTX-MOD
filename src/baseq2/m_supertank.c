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

SUPERTANK

==============================================================================
*/

#include "g_local.h"
#include "m_supertank.h"

bool visible(edict_t *self, edict_t *other);

static int  sound_pain1;
static int  sound_pain2;
static int  sound_pain3;
static int  sound_death;
static int  sound_search1;
static int  sound_search2;

static  int tread_sound;

void BossExplode(edict_t *self);
void BossExplodeTick(edict_t *self);

void TreadSound(edict_t *self)
{
    // [rerelease] on the body channel, so the treads no longer cut off a
    // pain or search sound on the voice channel
    gi.sound(self, M_RereleaseGame() ? CHAN_BODY : CHAN_VOICE, tread_sound, 1, ATTN_NORM, 0);
}

void supertank_search(edict_t *self)
{
    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, sound_search1, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_search2, 1, ATTN_NORM, 0);
}


void supertank_dead(edict_t *self);
// RAFAEL - monster_boss5 is the supertank with a power shield. Rogue's own
// value; nothing else on the supertank uses bit 3.
#define SPAWNFLAG_SUPERTANK_POWERSHIELD     8
// [rerelease] n64: the death animation's last six frames loop (BossLoop) while
// the explosions keep going. SP_monster_supertank sets it on every q64/ map.
#define SPAWNFLAG_SUPERTANK_LONG_DEATH      16

void supertankRocket(edict_t *self);
void supertankMachineGun(edict_t *self);
void supertank_reattack1(edict_t *self);


//
// stand
//

mframe_t supertank_frames_stand [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t supertank_move_stand = {FRAME_stand_1, FRAME_stand_60, supertank_frames_stand, NULL};

void supertank_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &supertank_move_stand;
}


mframe_t supertank_frames_run [] = {
    { ai_run, 12, TreadSound },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL },
    { ai_run, 12, NULL }
};
mmove_t supertank_move_run = {FRAME_forwrd_1, FRAME_forwrd_18, supertank_frames_run, NULL};

//
// walk
//


mframe_t supertank_frames_forward [] = {
    { ai_walk, 4, TreadSound },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 4, NULL }
};
mmove_t supertank_move_forward = {FRAME_forwrd_1, FRAME_forwrd_18, supertank_frames_forward, NULL};

void supertank_forward(edict_t *self)
{
    self->monsterinfo.currentmove = &supertank_move_forward;
}

void supertank_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &supertank_move_forward;
}

void supertank_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &supertank_move_stand;
    else
        self->monsterinfo.currentmove = &supertank_move_run;
}

mframe_t supertank_frames_turn_right [] = {
    { ai_move,    0,  TreadSound },
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
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_turn_right = {FRAME_right_1, FRAME_right_18, supertank_frames_turn_right, supertank_run};

mframe_t supertank_frames_turn_left [] = {
    { ai_move,    0,  TreadSound },
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
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_turn_left = {FRAME_left_1, FRAME_left_18, supertank_frames_turn_left, supertank_run};


mframe_t supertank_frames_pain3 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_pain3 = {FRAME_pain3_9, FRAME_pain3_12, supertank_frames_pain3, supertank_run};

mframe_t supertank_frames_pain2 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_pain2 = {FRAME_pain2_5, FRAME_pain2_8, supertank_frames_pain2, supertank_run};

mframe_t supertank_frames_pain1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_pain1 = {FRAME_pain1_1, FRAME_pain1_4, supertank_frames_pain1, supertank_run};

/*
=================
BossLoop

[rerelease] SPAWNFLAG_SUPERTANK_LONG_DEATH: send the death animation back to
death_19 `count` more times before letting it finish.
=================
*/
static void BossLoop(edict_t *self)
{
    if (!(self->spawnflags & SPAWNFLAG_SUPERTANK_LONG_DEATH))
        return;

    if (self->count)
        self->count--;
    else
        self->spawnflags &= ~SPAWNFLAG_SUPERTANK_LONG_DEATH;

    self->monsterinfo.nextframe = FRAME_death_19;
}

/*
=================
supertank_death_last

The last death frame. Classic: the original BossExplode sequence takes over
the supertank's think. [rerelease] the explosions have been going since the
start of the animation (BossExplodeTick); keep them going, loop for the N64
long death, and let supertank_dead gib once the animation is done.
=================
*/
static void supertank_death_last(edict_t *self)
{
    if (!M_RereleaseGame()) {
        BossExplode(self);
        return;
    }

    BossExplodeTick(self);
    BossLoop(self);
}

// [rerelease] id's BossExplode runs on the FIRST frame and spawns a helper that
// pops explosions over the body every 50-200 ms; BossExplodeTick does the same
// from each frame here (a no-op in the classic game). The first frame is
// empty, matching the helper's 75-250 ms start delay.
mframe_t supertank_frames_death1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  BossExplodeTick },
    { ai_move,    0,  supertank_death_last }
};
mmove_t supertank_move_death = {FRAME_death_1, FRAME_death_24, supertank_frames_death1, supertank_dead};

mframe_t supertank_frames_backward[] = {
    { ai_walk, 0, TreadSound },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL },
    { ai_walk, 0, NULL }
};
mmove_t supertank_move_backward = {FRAME_backwd_1, FRAME_backwd_18, supertank_frames_backward, NULL};

/*
=================
supertankGrenade

[rerelease] The supertank's THIRD attack, and one this tree never had at all.
The attak4_* frames are in the 1997 md2 already, AND this tree already
carried supertank_frames_attack4 as a stub of six empty ai_move slots that
nothing ever selected - id defined the animation and never wrote the code.
So this needs no new animation and no new move, only the thinks and the two
shoulder muzzles (MZ2_SUPERTANK_GRENADE_1/2, appended to the MZ2 list).

It lobs, so it is the answer to an enemy standing ABOVE the supertank, which
neither the chaingun nor the rockets can reach.  The speed sweep asks for the
flattest arc between 500 and 900 that actually lands on the predicted point.
=================
*/
static void supertankGrenade(edict_t *self)
{
    vec3_t  forward, right, start, aim, aim_point;
    int     flash_number;
    float   speed;

    if (!self->enemy || !self->enemy->inuse)
        return;

    if (self->s.frame == FRAME_attak4_1)
        flash_number = MZ2_SUPERTANK_GRENADE_1;
    else
        flash_number = MZ2_SUPERTANK_GRENADE_2;

    AngleVectors(self->s.angles, forward, right, NULL);
    M_ProjectFlashSource(self, monster_flash_offset[flash_number], forward, right, start);

    PredictAimEx(self, self->enemy, start, 0, false, crandom() * 0.1f, forward, aim_point);

    for (speed = 500.0f; speed < 1000.0f; speed += 100.0f) {
        VectorCopy(forward, aim);
        if (!M_CalculatePitchToFire(self, aim_point, start, aim, speed, 2.5f, true, false))
            continue;

        // the solved arc, with none of fire_grenade's classic 200 up
        monster_fire_grenade_ex(self, start, aim, 50, speed, flash_number, 0.0f, 0.0f);
        break;
    }
}

mframe_t supertank_frames_attack4[] = {
    { ai_move,    0,  supertankGrenade },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  supertankGrenade },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_attack4 = {FRAME_attak4_1, FRAME_attak4_6, supertank_frames_attack4, supertank_run};

mframe_t supertank_frames_attack3[] = {
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
mmove_t supertank_move_attack3 = {FRAME_attak3_1, FRAME_attak3_27, supertank_frames_attack3, supertank_run};

/*
=================
supertank_ai_rocket

[rerelease] id keeps turning to face the enemy (ai_charge) through all three
rocket shots and the recovery after them, frames 1-21; the classic table only
did for the first eight.
=================
*/
static void supertank_ai_rocket(edict_t *self, float dist)
{
    if (M_RereleaseGame())
        ai_charge(self, dist);
    else
        ai_move(self, dist);
}

mframe_t supertank_frames_attack2[] = {
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  supertankRocket },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, supertankRocket },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, supertankRocket },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { supertank_ai_rocket, 0, NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t supertank_move_attack2 = {FRAME_attak2_1, FRAME_attak2_27, supertank_frames_attack2, supertank_run};

mframe_t supertank_frames_attack1[] = {
    { ai_charge,  0,  supertankMachineGun },
    { ai_charge,  0,  supertankMachineGun },
    { ai_charge,  0,  supertankMachineGun },
    { ai_charge,  0,  supertankMachineGun },
    { ai_charge,  0,  supertankMachineGun },
    { ai_charge,  0,  supertankMachineGun },

};
mmove_t supertank_move_attack1 = {FRAME_attak1_1, FRAME_attak1_6, supertank_frames_attack1, supertank_reattack1};

mframe_t supertank_frames_end_attack1[] = {
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
mmove_t supertank_move_end_attack1 = {FRAME_attak1_7, FRAME_attak1_20, supertank_frames_end_attack1, supertank_run};


void supertank_reattack1(edict_t *self)
{
    // [rerelease] the burst runs until the time supertank_attack picked
    // (1.5-2.7 s), then carries on 30% of the time
    if (M_RereleaseGame()) {
        if (visible(self, self->enemy) &&
            (self->timestamp >= level.framenum || random() < 0.3f))
            self->monsterinfo.currentmove = &supertank_move_attack1;
        else
            self->monsterinfo.currentmove = &supertank_move_end_attack1;
        return;
    }

    if (visible(self, self->enemy))
        if (random() < 0.9f)
            self->monsterinfo.currentmove = &supertank_move_attack1;
        else
            self->monsterinfo.currentmove = &supertank_move_end_attack1;
    else
        self->monsterinfo.currentmove = &supertank_move_end_attack1;
}

void supertank_pain(edict_t *self, edict_t *other, float kick, int damage)
{

    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    // [rerelease] no pain during the rockets on every skill, and the pain
    // sound plays even in nightmare, where there is no pain animation
    if (M_RereleaseGame()) {
        if (meansOfDeath != MOD_CHAINFIST) {
            // Lessen the chance of him going into his pain frames
            if (damage <= 25)
                if (random() < 0.2f)
                    return;

            // Don't go into pain if he's firing his rockets
            if ((self->s.frame >= FRAME_attak2_1) && (self->s.frame <= FRAME_attak2_14))
                return;
        }

        if (damage <= 10)
            gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        else if (damage <= 25)
            gi.sound(self, CHAN_VOICE, sound_pain3, 1, ATTN_NORM, 0);
        else
            gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);

        self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

        if (!M_ShouldReactToPain(self, meansOfDeath))
            return;     // no pain anims in nightmare

        if (damage <= 10)
            self->monsterinfo.currentmove = &supertank_move_pain1;
        else if (damage <= 25)
            self->monsterinfo.currentmove = &supertank_move_pain2;
        else
            self->monsterinfo.currentmove = &supertank_move_pain3;
        return;
    }

    // Lessen the chance of him going into his pain frames
    if (damage <= 25)
        if (random() < 0.2f)
            return;

    // Don't go into pain if he's firing his rockets
    if (skill->value >= 2)
        if ((self->s.frame >= FRAME_attak2_1) && (self->s.frame <= FRAME_attak2_14))
            return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if (damage <= 10) {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &supertank_move_pain1;
    } else if (damage <= 25) {
        gi.sound(self, CHAN_VOICE, sound_pain3, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &supertank_move_pain2;
    } else {
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &supertank_move_pain3;
    }
}


void supertankRocket(edict_t *self)
{
    vec3_t  forward, right;
    vec3_t  start;
    vec3_t  dir;
    vec3_t  vec;
    int     flash_number;

    if (self->s.frame == FRAME_attak2_8)
        flash_number = MZ2_SUPERTANK_ROCKET_1;
    else if (self->s.frame == FRAME_attak2_11)
        flash_number = MZ2_SUPERTANK_ROCKET_2;
    else // (self->s.frame == FRAME_attak2_14)
        flash_number = MZ2_SUPERTANK_ROCKET_3;

    AngleVectors(self->s.angles, forward, right, NULL);

    // [rerelease] scale-aware muzzle; plain rockets lead the target and fly
    // at 750 (boss5's heat seekers are unchanged)
    if (M_RereleaseGame()) {
        if (!self->enemy || !self->enemy->inuse)
            return;

        M_ProjectFlashSource(self, monster_flash_offset[flash_number], forward, right, start);

        if (self->spawnflags & SPAWNFLAG_SUPERTANK_POWERSHIELD) {
            VectorCopy(self->enemy->s.origin, vec);
            vec[2] += self->enemy->viewheight;
            VectorSubtract(vec, start, dir);
            VectorNormalize(dir);
            monster_fire_heat(self, start, dir, 40, 500, flash_number, 0.075f);
        } else {
            PredictAimEx(self, self->enemy, start, 750, false, 0.0f, forward, NULL);
            monster_fire_rocket(self, start, forward, 50, 750, flash_number);
        }
        return;
    }

    G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

    VectorCopy(self->enemy->s.origin, vec);
    vec[2] += self->enemy->viewheight;
    VectorSubtract(vec, start, dir);
    VectorNormalize(dir);

    // RAFAEL - the power-shielded supertank (monster_boss5) fires heat
    // seekers instead of dumb rockets
    if (self->spawnflags & SPAWNFLAG_SUPERTANK_POWERSHIELD)
        monster_fire_heat(self, start, dir, 40, 500, flash_number, 0.075f);
    else
        monster_fire_rocket(self, start, dir, 50, 500, flash_number);
}

void supertankMachineGun(edict_t *self)
{
    vec3_t  dir;
    vec3_t  vec;
    vec3_t  start;
    vec3_t  forward, right;
    int     flash_number;

    flash_number = MZ2_SUPERTANK_MACHINEGUN_1 + (self->s.frame - FRAME_attak1_1);

    //FIXME!!!
    dir[0] = 0;
    dir[1] = self->s.angles[1];
    dir[2] = 0;

    AngleVectors(dir, forward, right, NULL);

    // [rerelease] scale-aware muzzle, a 0.1 s lead at eye height, and three
    // times the spread
    if (M_RereleaseGame()) {
        if (!self->enemy || !self->enemy->inuse)
            return;

        M_ProjectFlashSource(self, monster_flash_offset[flash_number], forward, right, start);
        PredictAimEx(self, self->enemy, start, 0, true, -0.1f, forward, NULL);
        monster_fire_bullet(self, start, forward, 6, 4, DEFAULT_BULLET_HSPREAD * 3, DEFAULT_BULLET_VSPREAD * 3, flash_number);
        return;
    }

    G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

    if (self->enemy) {
        VectorCopy(self->enemy->s.origin, vec);
        VectorMA(vec, 0, self->enemy->velocity, vec);
        vec[2] += self->enemy->viewheight;
        VectorSubtract(vec, start, forward);
        VectorNormalize(forward);
    }

    monster_fire_bullet(self, start, forward, 6, 4, DEFAULT_BULLET_HSPREAD, DEFAULT_BULLET_VSPREAD, flash_number);
}


void supertank_attack(edict_t *self)
{
    vec3_t  vec;
    float   range;
    //float r;

    VectorSubtract(self->enemy->s.origin, self->s.origin, vec);
    range = VectorLength(vec);

    //r = random();

    // Attack 1 == Chaingun
    // Attack 2 == Rocket Launcher
    // Attack 3 == Grenade Launcher  [rerelease]

    if (M_RereleaseGame()) {
        vec3_t  scratch;
        bool    chaingun_good, rocket_good, grenade_good;

        // id's range_to: the gap between the two bounding boxes
        range = range_to(self, self->enemy);

        chaingun_good = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_MACHINEGUN_1], scratch);
        rocket_good   = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_ROCKET_1], scratch);
        grenade_good  = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_GRENADE_1], scratch);

        // the grenade is the lobbing answer to an enemy standing ABOVE us,
        // which is why vec[2] > 120 forces it over the flat-firing weapons
        if (chaingun_good && (!rocket_good || range <= 540 || random() < 0.3f)) {
            if (grenade_good && (range >= 350 || vec[2] > 120.0f || random() < 0.2f)) {
                self->monsterinfo.currentmove = &supertank_move_attack4;
            } else {
                self->monsterinfo.currentmove = &supertank_move_attack1;
                // how long the chaingun burst lasts - see supertank_reattack1
                self->timestamp = level.framenum + (1.5f + 1.2f * random()) * BASE_FRAMERATE;
            }
        } else if (rocket_good) {
            if (grenade_good && (vec[2] > 120.0f || random() < 0.2f))
                self->monsterinfo.currentmove = &supertank_move_attack4;
            else
                self->monsterinfo.currentmove = &supertank_move_attack2;
        } else if (grenade_good) {
            self->monsterinfo.currentmove = &supertank_move_attack4;
        }
        return;
    }

    if (range <= 160) {
        self->monsterinfo.currentmove = &supertank_move_attack1;
    } else {
        // fire rockets more often at distance
        if (random() < 0.3f)
            self->monsterinfo.currentmove = &supertank_move_attack1;
        else
            self->monsterinfo.currentmove = &supertank_move_attack2;
    }
}


//
// death
//

/* [rerelease] id's gib list for this monster (ThrowGibs in the rerelease
   source). Thrown in the rerelease game only; the head entry, if any, is
   last because it turns the monster itself into that gib. */
const gib_def_t supertank_rerelease_gibs[] = {
    { 2, "models/objects/gibs/sm_meat/tris.md2", GIB_ORGANIC, 1.0f },
    { 2, "models/objects/gibs/sm_metal/tris.md2", GIB_METALLIC, 1.0f },
    { 1, "models/monsters/boss1/gibs/cgun.md2", GIB_SKINNED | GIB_METALLIC, 1.0f },
    { 1, "models/monsters/boss1/gibs/chest.md2", GIB_SKINNED, 1.0f },
    { 1, "models/monsters/boss1/gibs/core.md2", GIB_SKINNED, 1.0f },
    { 1, "models/monsters/boss1/gibs/ltread.md2", GIB_SKINNED | GIB_UPRIGHT, 1.0f },
    { 1, "models/monsters/boss1/gibs/rgun.md2", GIB_SKINNED | GIB_UPRIGHT, 1.0f },
    { 1, "models/monsters/boss1/gibs/rtread.md2", GIB_SKINNED | GIB_UPRIGHT, 1.0f },
    { 1, "models/monsters/boss1/gibs/tube.md2", GIB_SKINNED | GIB_UPRIGHT, 1.0f },
    { 1, "models/monsters/boss1/gibs/head.md2", GIB_SKINNED | GIB_METALLIC | GIB_HEAD, 1.0f },
};
const int supertank_num_rerelease_gibs = (int)(sizeof(supertank_rerelease_gibs) / sizeof(supertank_rerelease_gibs[0]));

void BossGib(edict_t *self);

void supertank_dead(edict_t *self)
{
    // [rerelease] the explosions have run through the whole death animation;
    // now it comes apart, at once - unless it was placed as a corpse ("no
    // blowy on deady"), which stays a body
    if (M_RereleaseGame() && !(self->spawnflags & SPAWNFLAG_MONSTER_DEAD)) {
        BossGib(self);
        return;
    }

    VectorSet(self->mins, -60, -60, 0);
    VectorSet(self->maxs, 60, 60, 72);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}


extern const gib_def_t boss2_rerelease_gibs[], boss31_rerelease_gibs[], carrier_rerelease_gibs[];
extern const int boss2_num_rerelease_gibs, boss31_num_rerelease_gibs, carrier_num_rerelease_gibs;

/*
=================
BossExplodeTick

[rerelease] id's BossExplode (g_rogue_newai.cpp) is called on a boss's first
death frame and spawns a helper entity that pops an explosion at a random point
in the boss's box every 50-200 ms until the boss is gone. Here the boss's own
death frames call this once each - one explosion per 100 ms server frame - so
there is no extra entity or think to save. The supertank, hornet and Jorg use
it; the carrier and guardian still end on the classic BossExplode.

id makes two of every three explosions TE_EXPLOSION1_NL (no light); this
protocol has no such temp entity, so they are all TE_EXPLOSION1.

A no-op in the classic game, and for a corpse being laid out by M_SpawnDead.
=================
*/
void BossExplodeTick(edict_t *self)
{
    vec3_t  org;

    if (!M_RereleaseGame())
        return;

    // no blowy on deady
    if ((self->spawnflags & SPAWNFLAG_MONSTER_DEAD) || (self->monsterinfo.aiflags & AI_SPAWNED_DEAD))
        return;

    org[0] = self->s.origin[0] + self->mins[0] + random() * self->size[0];
    org[1] = self->s.origin[1] + self->mins[1] + random() * self->size[1];
    org[2] = self->s.origin[2] + self->mins[2] + random() * self->size[2];

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1);
    gi.WritePosition(org);
    gi.multicast(org, MULTICAST_PVS);
}

/*
=================
BossGib

[rerelease] the end of a boss death (supertank_gib, boss2_gib, jorg's
BossExplode end): the big explosion and the boss's own parts, straight away.
That is exactly the last step of BossExplode, so go there directly.
=================
*/
void BossGib(edict_t *self)
{
    // a placed corpse that is shot apart gibs too; BossExplode would refuse
    // it, and the body is about to become its own head gib anyway
    self->spawnflags &= ~SPAWNFLAG_MONSTER_DEAD;
    self->count = 8;
    BossExplode(self);

    // called as the death animation's endfunc: M_MoveFrame stops on
    // SVF_DEADMONSTER, instead of stepping the head gib (now frame 0) back
    // onto the boss's death frames and running their thinks
    if (self->inuse)
        self->svflags |= SVF_DEADMONSTER;
}

void BossExplode(edict_t *self)
{
    vec3_t  org;
    int     n;

    // [rerelease] "no blowy on deady": a boss laid out as a corpse
    // (SPAWNFLAG_MONSTER_DEAD) reaches this death frame while M_SpawnDead
    // fast-forwards the animation, and must stay a body, not explode
    if (self->spawnflags & SPAWNFLAG_MONSTER_DEAD)
        return;

    self->think = BossExplode;
    VectorCopy(self->s.origin, org);
    org[2] += 24 + (Q_rand() & 15);
    switch (self->count++) {
    case 0:
        org[0] -= 24;
        org[1] -= 24;
        break;
    case 1:
        org[0] += 24;
        org[1] += 24;
        break;
    case 2:
        org[0] += 24;
        org[1] -= 24;
        break;
    case 3:
        org[0] -= 24;
        org[1] += 24;
        break;
    case 4:
        org[0] -= 48;
        org[1] -= 48;
        break;
    case 5:
        org[0] += 48;
        org[1] += 48;
        break;
    case 6:
        org[0] -= 48;
        org[1] += 48;
        break;
    case 7:
        org[0] += 48;
        org[1] -= 48;
        break;
    case 8:
        self->s.sound = 0;

        // [rerelease] each boss comes apart into its own parts - its gun,
        // treads, wings, head - rather than the generic meat and gear.
        // id throws these from each boss's own death function; here the
        // explosion sequence owns the boss's think, so it is done from here.
        if (M_RereleaseGame() && !LUDICROUS_GIBS()) {
            const gib_def_t *list = NULL;
            int num = 0;

            if (!Q_stricmp(self->classname, "monster_boss2")) {
                list = boss2_rerelease_gibs;
                num = boss2_num_rerelease_gibs;
            } else if (!Q_stricmp(self->classname, "monster_jorg")) {
                list = boss31_rerelease_gibs;
                num = boss31_num_rerelease_gibs;
            } else if (!Q_stricmp(self->classname, "monster_supertank") ||
                       !Q_stricmp(self->classname, "monster_boss5")) {
                list = supertank_rerelease_gibs;
                num = supertank_num_rerelease_gibs;
            } else if (!Q_stricmp(self->classname, "monster_carrier")) {
                list = carrier_rerelease_gibs;
                num = carrier_num_rerelease_gibs;
            }

            if (list) {
                gi.WriteByte(svc_temp_entity);
                gi.WriteByte(TE_EXPLOSION1_BIG);
                gi.WritePosition(self->s.origin);
                gi.multicast(self->s.origin, MULTICAST_PHS);

                // the boss may be flying (hornet, carrier); its parts fall
                VectorSet(self->gravityVector, 0, 0, -1);
                self->s.skinnum /= 2;
                ThrowGibs(self, 500, list, num);
                self->deadflag = DEAD_DEAD;
                return;
            }
        }
        if (!LUDICROUS_GIBS()) {
            for (n = 0; n < 4; n++)
                ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", 500, GIB_ORGANIC);
            for (n = 0; n < 8; n++)
                ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", 500, GIB_METALLIC);
        } else {
            for (n = 0; n < 16; n++) {
                if (n < 8) {
                    ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", 500, GIB_ORGANIC);
                    ThrowGib(self, "models/objects/gibs/bone/tris.md2", 500, GIB_ORGANIC);
                    ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", 500, GIB_METALLIC);
                }
                ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", 500, GIB_ORGANIC);
                ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", 500, GIB_ORGANIC);
                ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", 500, GIB_METALLIC);
            }
        }

        ThrowGib(self, "models/objects/gibs/chest/tris.md2", 500, GIB_ORGANIC);
        ThrowHead(self, "models/objects/gibs/gear/tris.md2", 500, GIB_METALLIC);
        self->deadflag = DEAD_DEAD;
        return;
    }

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1);
    gi.WritePosition(org);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    self->nextthink = level.framenum + 1;
}


void supertank_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    if (M_RereleaseGame()) {
        if (self->spawnflags & SPAWNFLAG_MONSTER_DEAD) {
            // a placed corpse can still be blown apart
            if (self->health <= self->gib_health) {
                BossGib(self);
                return;
            }

            if (self->deadflag == DEAD_DEAD)
                return;

            self->deadflag = DEAD_DEAD;
            self->takedamage = DAMAGE_YES;
        } else {
            gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
            self->deadflag = DEAD_DEAD;
            self->takedamage = DAMAGE_NO;
            // count is BossLoop's loop counter here, not BossExplode's step
        }

        self->monsterinfo.currentmove = &supertank_move_death;
        return;
    }

    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_NO;
    self->count = 0;
    self->monsterinfo.currentmove = &supertank_move_death;
}

//
// monster_supertank
//

/*
=================
supertank_blocked

[rerelease/ROGUE] monsterinfo.blocked, called from SV_NewChaseDir when the
supertank has run out of step directions.  Plats only - the supertank has no jump
animations, so blocked_checkjump has nothing to play and is not consulted.
Same shape as soldier_blocked.
=================
*/
bool supertank_blocked(edict_t *self, float dist)
{
    if (blocked_checkplat(self, dist))
        return true;

    return false;
}

/*QUAKED monster_supertank (1 .5 0) (-64 -64 0) (64 64 72) Ambush Trigger_Spawn Sight
*/
void SP_monster_supertank(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_pain1 = gi.soundindex("bosstank/btkpain1.wav");
    sound_pain2 = gi.soundindex("bosstank/btkpain2.wav");
    sound_pain3 = gi.soundindex("bosstank/btkpain3.wav");
    sound_death = gi.soundindex("bosstank/btkdeth1.wav");
    sound_search1 = gi.soundindex("bosstank/btkunqv1.wav");
    sound_search2 = gi.soundindex("bosstank/btkunqv2.wav");

//  self->s.sound = gi.soundindex ("bosstank/btkengn1.wav");
    tread_sound = gi.soundindex("bosstank/btkengn1.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/boss1/tris.md2");
    if (M_RereleaseGame())
        PrecacheGibs(supertank_rerelease_gibs, supertank_num_rerelease_gibs);
    VectorSet(self->mins, -64, -64, 0);
    VectorSet(self->maxs, 64, 64, 112);

    self->health = 1500;
    self->gib_health = -500;
    self->mass = 800;

    self->pain = supertank_pain;
    self->die = supertank_die;
    self->monsterinfo.stand = supertank_stand;
    self->monsterinfo.walk = supertank_walk;
    self->monsterinfo.run = supertank_run;
    self->monsterinfo.dodge = NULL;
    self->monsterinfo.attack = supertank_attack;
    self->monsterinfo.search = supertank_search;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = NULL;

    // [rerelease] let it ride func_plats instead of milling about
    if (M_RereleaseGame())
        self->monsterinfo.blocked = supertank_blocked;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &supertank_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    // RAFAEL - monster_boss5. The map may override either value with the
    // power_armor_type / power_armor_power keys, so only fill in what is unset.
    // [rerelease] keyed on whether the key was given, so an explicit 0 works
    if (self->spawnflags & SPAWNFLAG_SUPERTANK_POWERSHIELD) {
        if (!(st.keys_specified & SPAWNKEY_POWER_ARMOR_TYPE))
            self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;
        if (!(st.keys_specified & SPAWNKEY_POWER_ARMOR_POWER))
            self->monsterinfo.power_armor_power = 400;
    }

    // [rerelease] id replaced the hardcoded "they spray too much" classname
    // list in T_Damage with this flag, so these have to carry it or they would
    // start infighting the moment the flag test goes live.  Harmless when the
    // game is not rerelease: the reader in g_combat.c is gated.
    self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

    walkmonster_start(self);

    // [rerelease] every N64 supertank gets the long death: ten more loops of
    // the last six death frames
    if (M_RereleaseGame() && level.is_n64) {
        self->spawnflags |= SPAWNFLAG_SUPERTANK_LONG_DEATH;
        self->count = 10;
    }
}

/*QUAKED monster_boss5 (1 .5 0) (-64 -64 0) (64 64 72) Ambush Trigger_Spawn Sight
RAFAEL - the supertank with a power shield and heat-seeking rockets. Same
monster otherwise; the shield and the rocket type both hang off the spawnflag,
and skin 2 is the darker shielded look.
*/
void SP_monster_boss5(edict_t *self)
{
    self->spawnflags |= SPAWNFLAG_SUPERTANK_POWERSHIELD;

    SP_monster_supertank(self);

    if (!self->inuse)
        return;             // deathmatch: SP_monster_supertank freed it

    gi.soundindex("weapons/railgr1a.wav");
    self->s.skinnum = 2;
}
