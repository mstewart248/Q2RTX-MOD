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

BERSERK

==============================================================================
*/

#include "g_local.h"
#include "m_berserk.h"


static int sound_pain;
static int sound_die;
static int sound_idle;
static int sound_punch;
static int sound_sight;
static int sound_search;

// spawnflag 8: this berserk never leaps or jumps
#define SPAWNFLAG_BERSERK_NOJUMPING 8
static int sound_thud;
static int sound_jump;

void berserk_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void berserk_search(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}


void berserk_fidget(edict_t *self);
mframe_t berserk_frames_stand [] = {
    { ai_stand, 0, berserk_fidget },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t berserk_move_stand = {FRAME_stand1, FRAME_stand5, berserk_frames_stand, NULL};

void berserk_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &berserk_move_stand;
}

mframe_t berserk_frames_stand_fidget [] = {
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
mmove_t berserk_move_stand_fidget = {FRAME_standb1, FRAME_standb20, berserk_frames_stand_fidget, berserk_stand};

void berserk_fidget(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        return;
    if (random() > 0.15f)
        return;

    self->monsterinfo.currentmove = &berserk_move_stand_fidget;
    gi.sound(self, CHAN_WEAPON, sound_idle, 1, ATTN_IDLE, 0);
}


mframe_t berserk_frames_walk [] = {
    { ai_walk, 9.1, NULL },
    { ai_walk, 6.3, NULL },
    { ai_walk, 4.9, NULL },
    { ai_walk, 6.7, NULL },
    { ai_walk, 6.0, NULL },
    { ai_walk, 8.2, NULL },
    { ai_walk, 7.2, NULL },
    { ai_walk, 6.1, NULL },
    { ai_walk, 4.9, NULL },
    { ai_walk, 4.7, NULL },
    { ai_walk, 4.7, NULL },
    { ai_walk, 4.8, NULL }
};
mmove_t berserk_move_walk = {FRAME_walkc1, FRAME_walkc11, berserk_frames_walk, NULL};

void berserk_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &berserk_move_walk;
}

/*

  *****************************
  SKIPPED THIS FOR NOW!
  *****************************

   Running -> Arm raised in air

void()  berserk_runb1   =[  $r_att1 ,   berserk_runb2   ] {{ ai_run(21);};
void()  berserk_runb2   =[  $r_att2 ,   berserk_runb3   ] {{ ai_run(11);};
void()  berserk_runb3   =[  $r_att3 ,   berserk_runb4   ] {{ ai_run(21);};
void()  berserk_runb4   =[  $r_att4 ,   berserk_runb5   ] {{ ai_run(25);};
void()  berserk_runb5   =[  $r_att5 ,   berserk_runb6   ] {{ ai_run(18);};
void()  berserk_runb6   =[  $r_att6 ,   berserk_runb7   ] {{ ai_run(19);};
// running with arm in air : start loop
void()  berserk_runb7   =[  $r_att7 ,   berserk_runb8   ] {{ ai_run(21);};
void()  berserk_runb8   =[  $r_att8 ,   berserk_runb9   ] {{ ai_run(11);};
void()  berserk_runb9   =[  $r_att9 ,   berserk_runb10  ] {{ ai_run(21);};
void()  berserk_runb10  =[  $r_att10 ,  berserk_runb11  ] {{ ai_run(25);};
void()  berserk_runb11  =[  $r_att11 ,  berserk_runb12  ] {{ ai_run(18);};
void()  berserk_runb12  =[  $r_att12 ,  berserk_runb7   ] {{ ai_run(19);};
// running with arm in air : end loop
*/


mframe_t berserk_frames_run1 [] = {
    { ai_run, 21, NULL },
    { ai_run, 11, NULL },
    { ai_run, 21, NULL },
    { ai_run, 25, NULL },
    { ai_run, 18, NULL },
    { ai_run, 19, NULL }
};
mmove_t berserk_move_run1 = {FRAME_run1, FRAME_run6, berserk_frames_run1, NULL};

void berserk_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &berserk_move_stand;
    else
        self->monsterinfo.currentmove = &berserk_move_run1;
}


void berserk_attack_spike(edict_t *self)
{
    static  vec3_t  aim = {MELEE_DISTANCE, 0, -24};
    fire_hit(self, aim, (15 + (Q_rand() % 6)), 400);    //  Faster attack -- upwards and backwards
}


void berserk_swing(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_punch, 1, ATTN_NORM, 0);
}

mframe_t berserk_frames_attack_spike [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, berserk_swing },
    { ai_charge, 0, berserk_attack_spike },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t berserk_move_attack_spike = {FRAME_att_c1, FRAME_att_c8, berserk_frames_attack_spike, berserk_run};


void berserk_attack_club(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->mins[0], -4);
    fire_hit(self, aim, (5 + (Q_rand() % 6)), 400);     // Slower attack
}

mframe_t berserk_frames_attack_club [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, berserk_swing },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, berserk_attack_club },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t berserk_move_attack_club = {FRAME_att_c9, FRAME_att_c20, berserk_frames_attack_club, berserk_run};


void berserk_strike(edict_t *self)
{
    //FIXME play impact sound
}


mframe_t berserk_frames_attack_strike [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_swing },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_strike },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 9.7, NULL },
    { ai_move, 13.6, NULL }
};

mmove_t berserk_move_attack_strike = {FRAME_att_c21, FRAME_att_c34, berserk_frames_attack_strike, berserk_run};


extern mmove_t berserk_move_run_attack1;

void berserk_melee(edict_t *self)
{
    // rerelease: a swing that just landed keeps the berserk from immediately
    // starting another, and a run attack about to bring the club down is left
    // alone rather than being restarted as a standing swing
    if (M_RereleaseGame()) {
        if (self->monsterinfo.melee_debounce_framenum > level.framenum)
            return;
        if (self->monsterinfo.currentmove == &berserk_move_run_attack1 &&
            self->s.frame >= FRAME_r_att13) {
            self->monsterinfo.attack_state = AS_STRAIGHT;
            self->monsterinfo.attack_finished = 0;
            return;
        }
    }

    if ((Q_rand() % 2) == 0)
        self->monsterinfo.currentmove = &berserk_move_attack_spike;
    else
        self->monsterinfo.currentmove = &berserk_move_attack_club;
}

/*
=================
The rerelease berserk - dive-dodge and running attack

Two behaviours, both on frames the 1997 tris.md2 already has, so neither needs
M_RereleaseAnims() gating.  They do change how the berserk plays, so the hookups
in SP_monster_berserk sit behind M_RereleaseGame().

  duck2 (fall2-18)   the berserk's only dodge: a rare forward dive.
  run_attack1 (r_att1-18)  swings the club without breaking stride.

Note that id's berserk_move_duck (duck1-10) is defined in m_berserk.cpp and never
referenced - dead code, exactly like the hover start_attack2/end_attack2 pair.
It is deliberately not ported.

Dropped vs the rerelease: monster_footstep (no such sound here) and
monster_done_dodge (this tree has no AI_DODGING flag; attack_state does the
same job and is handled below).
=================
*/
// the rerelease's RANGE_NEAR is a real distance; ours is an enum tag
#define BERSERK_RANGE_NEAR  440.0f




mframe_t berserk_frames_duck2 [] = {
    { ai_move, 21, monster_duck_down },
    { ai_move, 28, NULL },
    { ai_move, 20, NULL },
    { ai_move, 12, NULL },
    { ai_move, 7,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  monster_duck_hold },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  monster_duck_up },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL }
};
mmove_t berserk_move_duck2 = {FRAME_fall2, FRAME_fall18, berserk_frames_duck2, berserk_run};

// Defined further down; the dodge pair tests against them.
extern mmove_t berserk_move_jump;
extern mmove_t berserk_move_jump2;
extern mmove_t berserk_move_attack_strike;
extern mmove_t berserk_move_pain2;
extern mmove_t berserk_move_run1;

/*
=================
berserk_duck / berserk_sidestep

The berserk's "duck" is the forward dive (berserk_move_duck2, on fall2-18), not
a crouch - id's berserk_move_duck on duck1-10 is dead code, never referenced.
Because it is a dive rather than a crouch it stays rare, keeping id's 5% roll.
=================
*/
bool berserk_duck(edict_t *self, float eta)
{
    // the berserk only dives forward, and very rarely
    if (random() >= 0.05f)
        return false;

    if (self->monsterinfo.currentmove == &berserk_move_jump ||
        self->monsterinfo.currentmove == &berserk_move_jump2)
        return false;

    self->monsterinfo.duck_wait_framenum = level.framenum + (eta + 0.5f) * BASE_FRAMERATE;
    self->monsterinfo.currentmove = &berserk_move_duck2;
    return true;
}

bool berserk_sidestep(edict_t *self)
{
    if (self->monsterinfo.currentmove == &berserk_move_jump ||
        self->monsterinfo.currentmove == &berserk_move_jump2 ||
        self->monsterinfo.currentmove == &berserk_move_attack_strike ||
        self->monsterinfo.currentmove == &berserk_move_pain2)
        return false;

    if (self->monsterinfo.currentmove != &berserk_move_run1)
        self->monsterinfo.currentmove = &berserk_move_run1;

    return true;
}

/*
=================
berserk_dodge

KEPT ONLY FOR SAVEGAME COMPATIBILITY - g_ptrs_compat_v2.c names this symbol.
=================
*/
void berserk_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    M_MonsterDodge(self, attacker, eta, tr, gravity);
}

static void berserk_run_attack_speed(edict_t *self)
{
    // close enough to connect - jump straight to the swing
    if (self->enemy && realrange(self, self->enemy) < MELEE_DISTANCE)
        self->monsterinfo.nextframe = self->s.frame + 6;
}

static void berserk_run_swing(edict_t *self)
{
    berserk_swing(self);
    self->monsterinfo.melee_debounce_framenum = level.framenum + 0.6f * BASE_FRAMERATE;

    if (self->monsterinfo.attack_state == AS_SLIDING)
        self->monsterinfo.attack_state = AS_STRAIGHT;
}

mframe_t berserk_frames_run_attack1 [] = {
    { ai_run, 21, berserk_run_attack_speed },
    { ai_run, 11, berserk_run_attack_speed },
    { ai_run, 21, berserk_run_attack_speed },
    { ai_run, 25, berserk_run_attack_speed },
    { ai_run, 18, berserk_run_attack_speed },
    { ai_run, 19, berserk_run_attack_speed },
    { ai_run, 21, NULL },
    { ai_run, 11, NULL },
    { ai_run, 21, NULL },
    { ai_run, 25, NULL },
    { ai_run, 18, NULL },
    { ai_run, 19, NULL },
    { ai_run, 21, berserk_run_swing },
    { ai_run, 11, NULL },
    { ai_run, 21, NULL },
    { ai_run, 25, NULL },
    { ai_run, 18, NULL },
    { ai_run, 19, berserk_attack_club }
};
mmove_t berserk_move_run_attack1 = {FRAME_r_att1, FRAME_r_att18, berserk_frames_run_attack1, berserk_run};

extern mmove_t berserk_move_attack_slam;
extern mmove_t berserk_move_jump_slam;

void berserk_attack(edict_t *self)
{
    if (!self->enemy)
        return;

    if (self->monsterinfo.melee_debounce_framenum <= level.framenum &&
        realrange(self, self->enemy) < MELEE_DISTANCE) {
        berserk_melee(self);
    } else if (!(self->spawnflags & SPAWNFLAG_BERSERK_NOJUMPING) &&
               level.framenum > self->timestamp && (Q_rand() & 1) &&
               realrange(self, self->enemy) > 150.0f) {
        // the leaping ground slam.  Only worth doing from a distance, and on a
        // long cooldown - otherwise the berserk just pogos at you
        self->monsterinfo.currentmove = &berserk_move_attack_slam;
        self->timestamp = level.framenum + 5 * BASE_FRAMERATE;
    } else if (self->monsterinfo.currentmove == &berserk_move_run1 &&
               realrange(self, self->enemy) <= BERSERK_RANGE_NEAR) {
        // pick up the run attack at the same point in the stride, so the
        // switch from run1 to r_att1 does not pop
        self->monsterinfo.currentmove = &berserk_move_run_attack1;
        self->monsterinfo.nextframe = FRAME_r_att1 + (self->s.frame - FRAME_run1) + 1;
    }
}



/*
void()  berserk_atke1   =[  $r_attb1,   berserk_atke2   ] {{ ai_run(9);};
void()  berserk_atke2   =[  $r_attb2,   berserk_atke3   ] {{ ai_run(6);};
void()  berserk_atke3   =[  $r_attb3,   berserk_atke4   ] {{ ai_run(18.4);};
void()  berserk_atke4   =[  $r_attb4,   berserk_atke5   ] {{ ai_run(25);};
void()  berserk_atke5   =[  $r_attb5,   berserk_atke6   ] {{ ai_run(14);};
void()  berserk_atke6   =[  $r_attb6,   berserk_atke7   ] {{ ai_run(20);};
void()  berserk_atke7   =[  $r_attb7,   berserk_atke8   ] {{ ai_run(8.5);};
void()  berserk_atke8   =[  $r_attb8,   berserk_atke9   ] {{ ai_run(3);};
void()  berserk_atke9   =[  $r_attb9,   berserk_atke10  ] {{ ai_run(17.5);};
void()  berserk_atke10  =[  $r_attb10,  berserk_atke11  ] {{ ai_run(17);};
void()  berserk_atke11  =[  $r_attb11,  berserk_atke12  ] {{ ai_run(9);};
void()  berserk_atke12  =[  $r_attb12,  berserk_atke13  ] {{ ai_run(25);};
void()  berserk_atke13  =[  $r_attb13,  berserk_atke14  ] {{ ai_run(3.7);};
void()  berserk_atke14  =[  $r_attb14,  berserk_atke15  ] {{ ai_run(2.6);};
void()  berserk_atke15  =[  $r_attb15,  berserk_atke16  ] {{ ai_run(19);};
void()  berserk_atke16  =[  $r_attb16,  berserk_atke17  ] {{ ai_run(25);};
void()  berserk_atke17  =[  $r_attb17,  berserk_atke18  ] {{ ai_run(19.6);};
void()  berserk_atke18  =[  $r_attb18,  berserk_run1    ] {{ ai_run(7.8);};
*/


mframe_t berserk_frames_pain1 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t berserk_move_pain1 = {FRAME_painc1, FRAME_painc4, berserk_frames_pain1, berserk_run};


mframe_t berserk_frames_pain2 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t berserk_move_pain2 = {FRAME_painb1, FRAME_painb20, berserk_frames_pain2, berserk_run};

void berserk_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
    gi.sound(self, CHAN_VOICE, sound_pain, 1, ATTN_NORM, 0);

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if ((damage < 20) || (random() < 0.5f))
        self->monsterinfo.currentmove = &berserk_move_pain1;
    else
        self->monsterinfo.currentmove = &berserk_move_pain2;
}


void berserk_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}


mframe_t berserk_frames_death1 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }

};
mmove_t berserk_move_death1 = {FRAME_death1, FRAME_death13, berserk_frames_death1, berserk_dead};


mframe_t berserk_frames_death2 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t berserk_move_death2 = {FRAME_deathc1, FRAME_deathc8, berserk_frames_death2, berserk_dead};


void berserk_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
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
				if (n < 8) {
					if (n < 4) {
						ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					}
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
				}
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			}
			ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
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

    gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

    if (damage >= 50)
        self->monsterinfo.currentmove = &berserk_move_death1;
    else
        self->monsterinfo.currentmove = &berserk_move_death2;
}


/*QUAKED monster_berserk (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/

/*
=================
The rerelease berserk's LEAPING GROUND SLAM

The rerelease replaced the berserk's old standing strike with a real jump: it
launches itself at the player, falls under boosted gravity, and slams the ground
where it lands, throwing everything nearby into the air.

It runs on slam1-slam23, which id shipped in the ORIGINAL 1997 tris.md2 and never
used - the same story as the soldier's runt frames - so this needs no
M_RereleaseAnims() gating, only M_RereleaseGame().  Our classic
berserk_move_attack_strike (att_c21-att_c34) is left untouched for plain baseq2.

Dropped vs the rerelease: FL_KILL_VELOCITY (no such flag here; the slam zeroes
velocity directly) and monsterinfo.unduck (monster_duck_up does that job now).
=================
*/

// how far the slam reaches, and how hard it throws
#define BERSERK_SLAM_DAMAGE     35
#define BERSERK_SLAM_KICK       150.0f
#define BERSERK_SLAM_RADIUS     275.0f

void berserk_jump_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf);

/*
=================
T_SlamRadiusDamage

Like T_RadiusDamage, but it measures from the closest point on each victim's box
rather than its centre, and it drives the damage from the impact POINT while
keeping the push origin at the victim's feet - which is what launches them
upward instead of sideways.
=================
*/
static void closest_point_to_box(const vec3_t from, const vec3_t mins, const vec3_t maxs, vec3_t out)
{
    int i;

    for (i = 0; i < 3; i++)
        out[i] = (from[i] < mins[i]) ? mins[i] : (from[i] > maxs[i]) ? maxs[i] : from[i];
}

void T_SlamRadiusDamage(vec3_t point, edict_t *inflictor, edict_t *attacker,
                        float damage, float kick, edict_t *ignore, float radius, int mod)
{
    float    points;
    edict_t *ent = NULL;
    vec3_t   boxmins, boxmaxs, closest, v, dir, hit_point;

    while ((ent = findradius(ent, inflictor->s.origin, radius)) != NULL) {
        if (ent == ignore)
            continue;
        if (!ent->takedamage)
            continue;
        if (!CanDamage(ent, inflictor))
            continue;

        VectorAdd(ent->s.origin, ent->mins, boxmins);
        VectorAdd(ent->s.origin, ent->maxs, boxmaxs);
        closest_point_to_box(point, boxmins, boxmaxs, closest);

        VectorSubtract(closest, point, v);
        points = damage - 0.5f * VectorLength(v);
        if (ent == attacker)
            points = points * 0.5f;
        if (points < 1)
            points = 1;

        VectorSubtract(ent->s.origin, point, dir);
        VectorNormalize(dir);

        // keep the push origin at their feet so they always get knocked UP
        VectorCopy(point, hit_point);
        hit_point[2] = ent->absmin[2];

        T_Damage(ent, inflictor, attacker, dir, hit_point, dir,
                 (int)points, (int)kick, DAMAGE_RADIUS, mod);

        if (ent->client && ent->velocity[2] < 270)
            ent->velocity[2] = 270;
    }
}

/*
=================
berserk_high_gravity

Heavy on the way down, much heavier on the way up, so the leap is a short
punchy arc instead of a floaty one.  Scaled against sv_gravity so the arc
survives a map with non-standard gravity.
=================
*/
static void berserk_high_gravity(edict_t *self)
{
    float g = sv_gravity->value > 0 ? sv_gravity->value : 800.0f;

    if (self->velocity[2] < 0)
        self->gravity = 2.25f * (800.0f / g);
    else
        self->gravity = 5.25f * (800.0f / g);
}

static void berserk_attack_slam(edict_t *self)
{
    vec3_t  f, r, offset, start;
    trace_t tr;

    gi.sound(self, CHAN_WEAPON, sound_thud, 1, ATTN_NORM, 0);

    // find the ground just under the leading fist
    AngleVectors(self->s.angles, f, r, NULL);
    VectorSet(offset, 20.0f, -14.3f, -21.0f);
    G_ProjectSource(self->s.origin, offset, f, r, start);
    tr = gi.trace(self->s.origin, NULL, NULL, start, self, MASK_SOLID);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_BERSERK_SLAM);
    gi.WritePosition(tr.endpos);
    gi.WriteDir(vec3_origin);       // straight up
    gi.multicast(tr.endpos, MULTICAST_PHS);

    self->gravity = 1.0f;
    VectorClear(self->velocity);

    T_SlamRadiusDamage(tr.endpos, self, self, BERSERK_SLAM_DAMAGE, BERSERK_SLAM_KICK,
                       self, BERSERK_SLAM_RADIUS, MOD_UNKNOWN);
}

void berserk_jump_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (self->health <= 0) {
        self->touch = NULL;
        return;
    }

    // The takeoff frame is still scraping the floor it launched from, so a
    // bare groundentity test slams instantly and the leap never happens.
    // Only a berserk that is on its way DOWN has actually landed.
    if (self->velocity[2] > 0)
        return;

    if (self->groundentity) {
        berserk_attack_slam(self);
        self->touch = NULL;

        // This fires for the ledge drop too, where currentmove is the JUMP move -
        // poking s.frame alone would leave the frame outside the running move's
        // range, so switch to the slam and let it play its impact half.
        self->monsterinfo.aiflags &= ~AI_DUCKED;
        self->monsterinfo.currentmove = &berserk_move_attack_slam;
        self->monsterinfo.nextframe = FRAME_slam18;
    }
}

/*
=================
berserk_jump_takeoff

THE ARC IS SOLVED HERE, NOT COPIED.  The rerelease uses
`fwd_speed = distance * 1.95` with a fixed 450 upward kick, but those constants
only work at its 40Hz monster tick.  This tree runs monster frames at 10Hz, and
one frame of the 5.25x RISING gravity removes 420 of that 450 straight away - so
the berserk barely leaves the floor, re-grounds on the very next frame, and slams
where it stood.  Measured: it covered 71 units of a 330 unit gap.

So instead of trusting the magic numbers, pick a flight time from the distance
and solve for the launch velocity that actually lands on the target.  Rising and
falling gravity differ, which makes the arc asymmetric:

    t_rise = vz / g_up                 apex = vz^2 / (2 * g_up)
    t_fall = sqrt(2 * apex / g_down)   = vz / sqrt(g_up * g_down)
    t_total = vz * (1/g_up + 1/sqrt(g_up * g_down))

berserk_high_gravity scales its multiplier by (800 / sv_gravity), so the EFFECTIVE
gravity is always 5.25*800 rising and 2.25*800 falling whatever the map sets.
=================
*/
#define BERSERK_SLAM_GRAV_UP    (5.25f * 800.0f)
#define BERSERK_SLAM_GRAV_DOWN  (2.25f * 800.0f)

static void berserk_jump_takeoff(edict_t *self)
{
    vec3_t forward, dir, aim_point, to_target;
    float  dist, flight_time, nominal_time, vz, fwd_speed, per_vz, apex, drop;

    if (!self->enemy)
        return;

    // aim the leap where the player is GOING to be, not where they are.  The
    // speed passed here only feeds the lead estimate; the real one is solved below.
    PredictAim(self->enemy, self->s.origin, 800.0f, false, 0.0f, dir, aim_point);

    self->s.angles[YAW] = vectoyaw(dir);
    AngleVectors(self->s.angles, forward, NULL, NULL);

    // horizontal gap to the spot we mean to come down on
    VectorSubtract(aim_point, self->s.origin, to_target);
    to_target[2] = 0;
    dist = VectorLength(to_target);

    // longer leaps get a bigger kick, so the arc reads the same at any range
    nominal_time = dist / 700.0f;
    if (nominal_time < 0.45f)
        nominal_time = 0.45f;
    else if (nominal_time > 0.9f)
        nominal_time = 0.9f;

    // seconds of hang time per unit of upward velocity, for a LEVEL landing
    per_vz = 1.0f / BERSERK_SLAM_GRAV_UP
           + 1.0f / sqrtf(BERSERK_SLAM_GRAV_UP * BERSERK_SLAM_GRAV_DOWN);

    vz = nominal_time / per_vz;

    // Now work out the REAL hang time.  Leaping off a ledge means falling the
    // apex height PLUS the drop, which buys a lot of extra airtime - ignoring
    // it made the berserk sail straight over a player standing below.
    apex = (vz * vz) / (2.0f * BERSERK_SLAM_GRAV_UP);
    drop = apex - (aim_point[2] - self->s.origin[2]);
    if (drop < 0.0f)
        drop = 0.0f;

    flight_time = vz / BERSERK_SLAM_GRAV_UP
                + sqrtf(2.0f * drop / BERSERK_SLAM_GRAV_DOWN);

    // Round UP to a whole monster frame: the solve is continuous, but the
    // server integrates in 0.1s steps, so a 4.7 frame arc lands at 4 and drops
    // the berserk short.
    flight_time = ceilf(flight_time / FRAMETIME) * FRAMETIME;

    fwd_speed = dist / flight_time;

    // A drop from a ledge needs enough horizontal push to actually CLEAR the
    // lip.  Solving purely for "land on the player" gives a very low forward
    // speed when they are mostly below - measured 114 for an 80 unit gap with
    // a 145 unit drop - and the berserk then arcs up and comes straight back
    // down onto the ledge it launched from, slamming where it stood.  Prefer
    // overshooting slightly to never leaving.
    if (aim_point[2] < self->s.origin[2] - 32.0f && fwd_speed < 300.0f)
        fwd_speed = 300.0f;

    self->s.origin[2] += 1;
    VectorScale(forward, fwd_speed, self->velocity);
    self->velocity[2] = vz;
    self->groundentity = NULL;

    // ducked while airborne so shots pass over the tucked body
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->monsterinfo.attack_finished = level.framenum + 3 * BASE_FRAMERATE;
    self->touch = berserk_jump_touch;

    gi.sound(self, CHAN_WEAPON, sound_jump, 1, ATTN_NORM, 0);
    berserk_high_gravity(self);
}

static void berserk_check_landing(edict_t *self)
{
    berserk_high_gravity(self);

    if (self->groundentity && self->velocity[2] <= 0) {
        self->monsterinfo.attack_finished = 0;
        self->monsterinfo.aiflags &= ~AI_DUCKED;
        self->s.frame = FRAME_slam18;
        if (self->touch) {
            berserk_attack_slam(self);
            self->touch = NULL;
        }
        return;
    }

    // still airborne: hold on the tucked frames until we land, and give up
    // after the 3 second watchdog so a leap into a pit cannot freeze the anim
    if (level.framenum > self->monsterinfo.attack_finished)
        self->monsterinfo.nextframe = FRAME_slam2;
    else
        self->monsterinfo.nextframe = FRAME_slam5;
}

mframe_t berserk_frames_attack_slam [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, berserk_jump_takeoff },
    { ai_move,   0, berserk_high_gravity },
    { ai_move,   0, berserk_high_gravity },
    { ai_move,   0, berserk_check_landing },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL },
    { ai_move,   0, NULL }
};
mmove_t berserk_move_attack_slam = {FRAME_slam1, FRAME_slam23, berserk_frames_attack_slam, berserk_run};

/*
=================
berserk jumps - the rerelease/ROGUE blocked system

monsterinfo.blocked is called from SV_NewChaseDir when the berserk has run out
of step directions.  It jumps down off ledges and up onto them, and rides
func_plats.  All of this runs on the APPENDED jump frames, so blocked_checkjump
refuses unless M_RereleaseAnims() is on.

Dropped vs the rerelease: monster_done_dodge (no AI_DODGING flag in this tree).
=================
*/
static void berserk_jump_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

static void berserk_jump2_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 150, forward, self->velocity);
    VectorMA(self->velocity, 400, up, self->velocity);
}

static void berserk_jump_wait_land(edict_t *self)
{
    if (self->groundentity == NULL) {
        self->monsterinfo.nextframe = self->s.frame;

        if (monster_jump_finished(self))
            self->monsterinfo.nextframe = self->s.frame + 1;
    } else {
        self->monsterinfo.nextframe = self->s.frame + 1;
    }
}

mframe_t berserk_frames_jump [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_jump_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t berserk_move_jump = {FRAME_jump1, FRAME_jump9, berserk_frames_jump, berserk_run};

/*
=================
berserk_move_jump_slam - drop off a ledge, then slam on impact

Routing the ledge drop straight into berserk_move_attack_slam looked wrong: its
berserk_check_landing holds nextframe on FRAME_slam5 for the WHOLE descent, and
slam5 is a mid-slam pose, so the berserk falls frozen mid-swing.  The plain jump
holds on jump7 instead, which is an actual falling pose and reads correctly.

It also has to AIM: berserk_jump_now is a fixed little hop (forward*100, up*300),
so the berserk landed at the foot of the ledge and then needed a SECOND leap to
reach the player.  Use the ballistic berserk_jump_takeoff instead, which solves for
the player's position including the drop.

So fall on the jump frames, and hand over to the slam's impact half (slam18) the
moment we touch down.  Same frames as berserk_move_jump; only the landing think
differs, which is what makes the move its own identity - no extra state needed.
=================
*/
static void berserk_jump_slam_land(edict_t *self)
{
    berserk_high_gravity(self);

    if (self->groundentity == NULL) {
        self->monsterinfo.nextframe = self->s.frame;

        // the watchdog stops a drop into a pit holding the pose forever
        if (monster_jump_finished(self))
            self->monsterinfo.nextframe = self->s.frame + 1;
        return;
    }

    self->monsterinfo.attack_finished = 0;
    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->gravity = 1.0f;

    // berserk_jump_touch may have beaten us to it on contact
    if (self->touch) {
        berserk_attack_slam(self);
        self->touch = NULL;
    }

    self->monsterinfo.currentmove = &berserk_move_attack_slam;
    self->monsterinfo.nextframe = FRAME_slam18;
}

mframe_t berserk_frames_jump_slam [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_jump_takeoff },
    { ai_move, 0, berserk_high_gravity },
    { ai_move, 0, berserk_high_gravity },
    { ai_move, 0, berserk_jump_slam_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t berserk_move_jump_slam = {FRAME_jump1, FRAME_jump9, berserk_frames_jump_slam, berserk_run};

mframe_t berserk_frames_jump2 [] = {
    { ai_move, -8, NULL },
    { ai_move, -4, NULL },
    { ai_move, -4, NULL },
    { ai_move, 0, berserk_jump2_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, berserk_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t berserk_move_jump2 = {FRAME_jump1, FRAME_jump9, berserk_frames_jump2, berserk_run};

void berserk_jump(edict_t *self, blocked_jump_result_t result)
{
    if (!self->enemy)
        return;

    if (result == JUMP_JUMP_UP) {
        self->monsterinfo.currentmove = &berserk_move_jump2;
        return;
    }

    // Coming DOWN to the player: land it as a ground pound.  id has no explicit
    // wiring for this - in the rerelease it emerges because berserk_attack picks
    // the slam while the berserk is above you and the leap carries it down.
    // Measured here that almost never gets the chance: over one ledge encounter
    // berserk_attack ran twice, while blocked_checkjump fired 12 times, so the
    // blocked hook wins the race and you get a plain hop.
    //
    // Drop on the JUMP frames and slam on impact - NOT berserk_move_attack_slam,
    // which freezes mid-swing for the whole descent.
    if (level.framenum > self->timestamp &&
        !(self->spawnflags & SPAWNFLAG_BERSERK_NOJUMPING)) {
        self->monsterinfo.currentmove = &berserk_move_jump_slam;
        self->timestamp = level.framenum + 5 * BASE_FRAMERATE;
        return;
    }

    self->monsterinfo.currentmove = &berserk_move_jump;
}

bool berserk_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            berserk_jump(self, result);
        return true;
    }

    if (blocked_checkplat(self, dist))
        return true;

    return false;
}

void SP_monster_berserk(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    // pre-caches
    sound_pain  = gi.soundindex("berserk/berpain2.wav");
    sound_die   = gi.soundindex("berserk/berdeth2.wav");
    sound_idle  = gi.soundindex("berserk/beridle1.wav");
    sound_punch = gi.soundindex("berserk/attack.wav");
    sound_search = gi.soundindex("berserk/bersrch1.wav");
    // only the rerelease berserk leaps, and berserk/jump.wav ships only there
    if (M_RereleaseGame()) {
        sound_thud = gi.soundindex("mutant/thud1.wav");
        sound_jump = gi.soundindex("berserk/jump.wav");
    }
    sound_sight = gi.soundindex("berserk/sight.wav");

    self->s.modelindex = gi.modelindex("models/monsters/berserk/tris.md2");
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 32);
    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    self->health = 240;
    self->gib_health = -60;
    self->mass = 250;

    self->pain = berserk_pain;
    self->die = berserk_die;

    self->monsterinfo.stand = berserk_stand;
    self->monsterinfo.walk = berserk_walk;
    self->monsterinfo.run = berserk_run;
    self->monsterinfo.dodge = NULL;
    self->monsterinfo.attack = NULL;
    // the rerelease berserk dive-dodges and attacks on the run; the classic
    // one does neither
    if (M_RereleaseGame()) {
        self->monsterinfo.dodge = M_MonsterDodge;
        self->monsterinfo.duck = berserk_duck;
        self->monsterinfo.unduck = monster_duck_up;
        self->monsterinfo.sidestep = berserk_sidestep;
        self->monsterinfo.attack = berserk_attack;
    }
    self->monsterinfo.melee = berserk_melee;
    self->monsterinfo.sight = berserk_sight;
    self->monsterinfo.search = berserk_search;

    self->monsterinfo.currentmove = &berserk_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    gi.linkentity(self);

    // ROGUE/rerelease: let the berserk jump ledges and ride plats.  The jump
    // animations only exist on the rerelease model, so blocked_checkjump
    // gates itself on M_RereleaseAnims(); the plat half needs no frames.
    if (M_RereleaseGame()) {
        self->monsterinfo.blocked = berserk_blocked;
        self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_BERSERK_NOJUMPING);
        self->monsterinfo.drop_height = 256;
        self->monsterinfo.jump_height = 40;
    }

    walkmonster_start(self);
}
