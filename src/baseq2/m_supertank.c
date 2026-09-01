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

void TreadSound(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, tread_sound, 1, ATTN_NORM, 0);
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

mframe_t supertank_frames_death1 [] = {
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
    { ai_move,    0,  BossExplode }
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

    PredictAim(self->enemy, start, 0, false, crandom() * 0.1f, forward, aim_point);

    for (speed = 500.0f; speed < 1000.0f; speed += 100.0f) {
        VectorCopy(forward, aim);
        if (!M_CalculatePitchToFire(self, aim_point, start, aim, speed, 2.5f, true, false))
            continue;

        monster_fire_grenade(self, start, aim, 50, speed, flash_number);
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

mframe_t supertank_frames_attack2[] = {
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  NULL },
    { ai_charge,  0,  supertankRocket },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  supertankRocket },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  supertankRocket },
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
        bool    chaingun_good = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_MACHINEGUN_1], scratch);
        bool    rocket_good   = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_ROCKET_1], scratch);
        bool    grenade_good  = M_CheckClearShot(self, monster_flash_offset[MZ2_SUPERTANK_GRENADE_1], scratch);

        // the grenade is the lobbing answer to an enemy standing ABOVE us,
        // which is why vec[2] > 120 forces it over the flat-firing weapons
        if (chaingun_good && (!rocket_good || range <= 540 || random() < 0.3f)) {
            if (grenade_good && (range >= 350 || vec[2] > 120.0f || random() < 0.2f))
                self->monsterinfo.currentmove = &supertank_move_attack4;
            else
                self->monsterinfo.currentmove = &supertank_move_attack1;
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

void supertank_dead(edict_t *self)
{
    VectorSet(self->mins, -60, -60, 0);
    VectorSet(self->maxs, 60, 60, 72);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}


void BossExplode(edict_t *self)
{
    vec3_t  org;
    int     n;

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
    if (self->spawnflags & SPAWNFLAG_SUPERTANK_POWERSHIELD) {
        if (!self->monsterinfo.power_armor_type)
            self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;
        if (!self->monsterinfo.power_armor_power)
            self->monsterinfo.power_armor_power = 400;
    }

    // [rerelease] id replaced the hardcoded "they spray too much" classname
    // list in T_Damage with this flag, so these have to carry it or they would
    // start infighting the moment the flag test goes live.  Harmless when the
    // game is not rerelease: the reader in g_combat.c is gated.
    self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

    walkmonster_start(self);
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
