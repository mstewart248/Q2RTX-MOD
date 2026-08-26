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

INFANTRY

==============================================================================
*/

#include "g_local.h"
#include "m_infantry.h"

void InfantryMachineGun(edict_t *self);
void infantry_set_firetime(edict_t *self);
void infantry_skip_cock(edict_t *self);
extern mmove_t infantry_move_attack1;
extern mmove_t infantry_move_attack4;

// RANGE_NEAR * 0.75 in the rerelease, where their RANGE_NEAR is 440 units.  This
// tree's RANGE_NEAR is an enum tag rather than a distance, so spell it out.
#define RANGE_RUN_ATTACK    330.0f


static int  sound_pain1;
static int  sound_pain2;
static int  sound_die1;
static int  sound_die2;

static int  sound_gunshot;
static int  sound_weapon_cock;
static int  sound_punch_swing;
static int  sound_punch_hit;
static int  sound_sight;
static int  sound_search;
static int  sound_idle;


mframe_t infantry_frames_stand [] = {
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
mmove_t infantry_move_stand = {FRAME_stand50, FRAME_stand71, infantry_frames_stand, NULL};

void infantry_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &infantry_move_stand;
}


mframe_t infantry_frames_fidget [] = {
    { ai_stand, 1,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 3,  NULL },
    { ai_stand, 6,  NULL },
    { ai_stand, 3,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -1, NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -2, NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, -1, NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -1, NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -1, NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 1,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -1, NULL },
    { ai_stand, -1, NULL },
    { ai_stand, 0,  NULL },
    { ai_stand, -3, NULL },
    { ai_stand, -2, NULL },
    { ai_stand, -3, NULL },
    { ai_stand, -3, NULL },
    { ai_stand, -2, NULL }
};
mmove_t infantry_move_fidget = {FRAME_stand01, FRAME_stand49, infantry_frames_fidget, infantry_stand};

void infantry_fidget(edict_t *self)
{
    self->monsterinfo.currentmove = &infantry_move_fidget;
    gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

mframe_t infantry_frames_walk [] = {
    { ai_walk, 5,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 5,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 5,  NULL },
    { ai_walk, 6,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 4,  NULL },
    { ai_walk, 5,  NULL }
};
mmove_t infantry_move_walk = {FRAME_walk03, FRAME_walk14, infantry_frames_walk, NULL};

void infantry_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &infantry_move_walk;
}

mframe_t infantry_frames_run [] = {
    { ai_run, 10, NULL },
    { ai_run, 20, NULL },
    { ai_run, 5,  NULL },
    { ai_run, 7,  NULL },
    { ai_run, 30, NULL },
    { ai_run, 35, NULL },
    { ai_run, 2,  NULL },
    { ai_run, 6,  NULL }
};
mmove_t infantry_move_run = {FRAME_run01, FRAME_run08, infantry_frames_run, NULL};

void infantry_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &infantry_move_stand;
    else
        self->monsterinfo.currentmove = &infantry_move_run;
}


mframe_t infantry_frames_pain1 [] = {
    { ai_move, -3, NULL },
    { ai_move, -2, NULL },
    { ai_move, -1, NULL },
    { ai_move, -2, NULL },
    { ai_move, -1, NULL },
    { ai_move, 1,  NULL },
    { ai_move, -1, NULL },
    { ai_move, 1,  NULL },
    { ai_move, 6,  NULL },
    { ai_move, 2,  NULL }
};
mmove_t infantry_move_pain1 = {FRAME_pain101, FRAME_pain110, infantry_frames_pain1, infantry_run};

mframe_t infantry_frames_pain2 [] = {
    { ai_move, -3, NULL },
    { ai_move, -3, NULL },
    { ai_move, 0,  NULL },
    { ai_move, -1, NULL },
    { ai_move, -2, NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 2,  NULL },
    { ai_move, 5,  NULL },
    { ai_move, 2,  NULL }
};
mmove_t infantry_move_pain2 = {FRAME_pain201, FRAME_pain210, infantry_frames_pain2, infantry_run};

void infantry_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    int     n;

    if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    n = Q_rand() % 2;
    if (n == 0) {
        self->monsterinfo.currentmove = &infantry_move_pain1;
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
    } else {
        self->monsterinfo.currentmove = &infantry_move_pain2;
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
    }
}


vec3_t  aimangles[] = {
    { 0.0, 5.0, 0.0 },
    { 10.0, 15.0, 0.0 },
    { 20.0, 25.0, 0.0 },
    { 25.0, 35.0, 0.0 },
    { 30.0, 40.0, 0.0 },
    { 30.0, 45.0, 0.0 },
    { 25.0, 50.0, 0.0 },
    { 20.0, 40.0, 0.0 },
    { 15.0, 35.0, 0.0 },
    { 40.0, 35.0, 0.0 },
    { 70.0, 35.0, 0.0 },
    { 90.0, 35.0, 0.0 }
};

void InfantryMachineGun(edict_t *self)
{
    vec3_t  start, target;
    vec3_t  forward, right;
    vec3_t  vec;
    int     flash_number;

    // attack1 (rerelease timing) fires at attak103, attack3 at attak311, and the
    // classic MD2 attack1 at attak111.  These never collide: the rerelease
    // attack1 skips attak108-113 entirely, and attack3 lives on attak3xx.
    bool is_run_attack = (self->s.frame >= FRAME_run201 && self->s.frame <= FRAME_run208);

    if (is_run_attack || self->s.frame == FRAME_attak103 || self->s.frame == FRAME_attak311 ||
        self->s.frame == FRAME_attak111 || self->s.frame == FRAME_attak416) {
        if (is_run_attack)
            // one flash per run frame.  NOTE: the rerelease writes this as
            // MZ2_INFANTRY_MACHINEGUN_14 + (frame - MZ2_INFANTRY_MACHINEGUN_14),
            // which only works because in THEIR enum that constant happens to
            // equal FRAME_run201 (both 232).  Ours does not, so index off the
            // frame explicitly.
            flash_number = MZ2_INFANTRY_MACHINEGUN_14 + (self->s.frame - FRAME_run201);
        else if (self->s.frame == FRAME_attak416)
            flash_number = MZ2_INFANTRY_MACHINEGUN_22;
        else
            flash_number = MZ2_INFANTRY_MACHINEGUN_1;
        AngleVectors(self->s.angles, forward, right, NULL);
        G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

        if (self->enemy) {
            VectorMA(self->enemy->s.origin, -0.2f, self->enemy->velocity, target);
            target[2] += self->enemy->viewheight;
            VectorSubtract(target, start, forward);
            VectorNormalize(forward);
        } else {
            AngleVectors(self->s.angles, forward, right, NULL);
        }
    } else {
        flash_number = MZ2_INFANTRY_MACHINEGUN_2 + (self->s.frame - FRAME_death211);

        AngleVectors(self->s.angles, forward, right, NULL);
        G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

        VectorSubtract(self->s.angles, aimangles[flash_number - MZ2_INFANTRY_MACHINEGUN_2], vec);
        AngleVectors(vec, forward, NULL, NULL);
    }

    monster_fire_bullet(self, start, forward, 3, 4, DEFAULT_BULLET_HSPREAD, DEFAULT_BULLET_VSPREAD, flash_number);
}

void infantry_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_BODY, sound_sight, 1, ATTN_NORM, 0);
}

void infantry_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);

    M_FlyCheck(self);
}

mframe_t infantry_frames_death1 [] = {
    { ai_move, -4, NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, -1, NULL },
    { ai_move, -4, NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, -1, NULL },
    { ai_move, 3,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, -2, NULL },
    { ai_move, 2,  NULL },
    { ai_move, 2,  NULL },
    { ai_move, 9,  NULL },
    { ai_move, 9,  NULL },
    { ai_move, 5,  NULL },
    { ai_move, -3, NULL },
    { ai_move, -3, NULL }
};
mmove_t infantry_move_death1 = {FRAME_death101, FRAME_death120, infantry_frames_death1, infantry_dead};

// Off with his head
mframe_t infantry_frames_death2 [] = {
    { ai_move, 0,   NULL },
    { ai_move, 1,   NULL },
    { ai_move, 5,   NULL },
    { ai_move, -1,  NULL },
    { ai_move, 0,   NULL },
    { ai_move, 1,   NULL },
    { ai_move, 1,   NULL },
    { ai_move, 4,   NULL },
    { ai_move, 3,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, -2,  InfantryMachineGun },
    { ai_move, -2,  InfantryMachineGun },
    { ai_move, -3,  InfantryMachineGun },
    { ai_move, -1,  InfantryMachineGun },
    { ai_move, -2,  InfantryMachineGun },
    { ai_move, 0,   InfantryMachineGun },
    { ai_move, 2,   InfantryMachineGun },
    { ai_move, 2,   InfantryMachineGun },
    { ai_move, 3,   InfantryMachineGun },
    { ai_move, -10, InfantryMachineGun },
    { ai_move, -7,  InfantryMachineGun },
    { ai_move, -8,  InfantryMachineGun },
    { ai_move, -6,  NULL },
    { ai_move, 4,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t infantry_move_death2 = {FRAME_death201, FRAME_death225, infantry_frames_death2, infantry_dead};

mframe_t infantry_frames_death3 [] = {
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, -6,  NULL },
    { ai_move, -11, NULL },
    { ai_move, -3,  NULL },
    { ai_move, -11, NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t infantry_move_death3 = {FRAME_death301, FRAME_death309, infantry_frames_death3, infantry_dead};


void infantry_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

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

			for (n = 0; n < 12; n++) {
				if (n < 8) {
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
				}
				else {
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
				}
				ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			}			
			
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
					if (n < 4) {
						ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					}
					ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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

// regular death
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

    n = Q_rand() % 3;
    if (n == 0) {
        self->monsterinfo.currentmove = &infantry_move_death1;
        gi.sound(self, CHAN_VOICE, sound_die2, 1, ATTN_NORM, 0);
    } else if (n == 1) {
        self->monsterinfo.currentmove = &infantry_move_death2;
        gi.sound(self, CHAN_VOICE, sound_die1, 1, ATTN_NORM, 0);
    } else {
        self->monsterinfo.currentmove = &infantry_move_death3;
        gi.sound(self, CHAN_VOICE, sound_die2, 1, ATTN_NORM, 0);
    }
}





mframe_t infantry_frames_duck [] = {
    { ai_move, -2, monster_duck_down },
    { ai_move, -5, monster_duck_hold },
    { ai_move, 3,  NULL },
    { ai_move, 4,  monster_duck_up },
    { ai_move, 0,  NULL }
};
mmove_t infantry_move_duck = {FRAME_duck01, FRAME_duck05, infantry_frames_duck, infantry_run};

// Defined further down; the dodge pair tests against them.
extern mmove_t infantry_move_attack1;
extern mmove_t infantry_move_attack2;
extern mmove_t infantry_move_jump;
extern mmove_t infantry_move_jump2;

/*
=================
infantry_duck / infantry_sidestep

The ROGUE/rerelease dodge pair. Returning a bool is what lets
M_MonsterDodge fall back from a sidestep to a duck. Neither interrupts a
firing sequence - a monster that ducked mid-burst threw the shot away.
=================
*/
bool infantry_duck(edict_t *self, float eta)
{
    if (self->monsterinfo.currentmove == &infantry_move_jump ||
        self->monsterinfo.currentmove == &infantry_move_jump2)
        return false;

    if (self->monsterinfo.currentmove == &infantry_move_attack1 ||
        self->monsterinfo.currentmove == &infantry_move_attack2) {
        // already shooting - stand back up rather than half-duck
        monster_duck_up(self);
        return false;
    }

    self->monsterinfo.currentmove = &infantry_move_duck;
    return true;
}

bool infantry_sidestep(edict_t *self)
{
    if (self->monsterinfo.currentmove == &infantry_move_jump ||
        self->monsterinfo.currentmove == &infantry_move_jump2)
        return false;

    // strafing happens on the run move; AS_SLIDING is what makes
    // ai_run sidestep rather than close
    if (self->monsterinfo.currentmove != &infantry_move_run)
        self->monsterinfo.currentmove = &infantry_move_run;

    return true;
}

/*
=================
infantry_dodge

KEPT ONLY FOR SAVEGAME COMPATIBILITY - g_ptrs_compat_v2.c is a frozen table
for version-2 saves and names this symbol, so it cannot be deleted.
=================
*/
void infantry_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    M_MonsterDodge(self, attacker, eta, tr, gravity);
}


void infantry_cock_gun(edict_t *self)
{
    int     n;

    gi.sound(self, CHAN_WEAPON, sound_weapon_cock, 1, ATTN_NORM, 0);
    n = (Q_rand() & 15) + 3 + 7;
    self->monsterinfo.pause_framenum = level.framenum + n;

    // gun cocked
    self->count = 1;
}

// The rerelease fires early and then jumps the gun-cocking frames, so the
// firing window has to be armed a frame BEFORE the shot rather than by
// infantry_cock_gun.  0.7-2.0s in the rerelease, which is 7-20 frames here.
void infantry_set_firetime(edict_t *self)
{
    self->monsterinfo.pause_framenum = level.framenum + 7 + (Q_rand() % 14);

    // If the enemy is far enough away and there is somewhere to advance to,
    // charge while firing instead of standing still.

    if (M_RereleaseAnims() &&
        !(self->monsterinfo.aiflags & AI_STAND_GROUND) && self->enemy &&
        realrange(self, self->enemy) >= RANGE_RUN_ATTACK &&
        ai_check_move(self, 8.0f)) {
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
        self->monsterinfo.currentmove = &infantry_move_attack4;
    }
}

// attak108-attak113 are the cock-the-gun frames.  The rerelease skips them in
// attack1 (it keeps the full cock+shoot version as its attack3) - playing them
// is what made the infantry appear to reload while it was shooting.
void infantry_skip_cock(edict_t *self)
{
    self->monsterinfo.nextframe = FRAME_attak114;
}

void infantry_fire(edict_t *self)
{
    InfantryMachineGun(self);

    // we fired, so we must cock again before firing
    self->count = 0;

    // The run-and-gun must NEVER hold its frame.  A standing attack holds on the
    // firing frame for the whole burst, which is what makes it keep shooting -
    // but doing that while charging freezes the run animation on its first frame.
    // The rerelease branches on the active move here for exactly this reason: in
    // attack4 the burst ends by decision instead of by holding.
    if (self->monsterinfo.currentmove == &infantry_move_attack4) {
        if (level.framenum >= self->monsterinfo.pause_framenum) {
            // ran out of firing time
            self->monsterinfo.currentmove = &infantry_move_attack1;
            self->monsterinfo.nextframe = FRAME_attak114;
        } else if ((self->monsterinfo.aiflags & AI_STAND_GROUND) ||
                   (self->enemy && (realrange(self, self->enemy) < RANGE_RUN_ATTACK ||
                                    !ai_check_move(self, 8.0f)))) {
            // got too close, or ran out of room to advance
            self->monsterinfo.currentmove = &infantry_move_attack1;
            self->monsterinfo.nextframe = FRAME_attak103;
            self->monsterinfo.attack_state = AS_STRAIGHT;
        }
        return;
    }

    if (level.framenum >= self->monsterinfo.pause_framenum) {
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;

        // attack5 holds on attak416 for the burst, then skips its recovery frames
        if (self->s.frame == FRAME_attak416)
            self->monsterinfo.nextframe = FRAME_attak420;
    } else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

mframe_t infantry_frames_attack1 [] = {
    { ai_charge, 0,  NULL },
    { ai_charge, 6,  infantry_set_firetime },
    { ai_charge, 0,  infantry_fire },
    { ai_charge, 0,  NULL },
    { ai_charge, 1,  NULL },
    { ai_charge, -7, NULL },
    { ai_charge, -6, infantry_skip_cock },
    // dead frames start - jumped by infantry_skip_cock above
    { ai_charge, -1, NULL },
    { ai_charge, 0,  infantry_cock_gun },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL },
    // dead frames end
    { ai_charge, -1, NULL },
    { ai_charge, -1, NULL }
};
mmove_t infantry_move_attack1 = {FRAME_attak101, FRAME_attak115, infantry_frames_attack1, infantry_run};

// The full cock-then-shoot pass.  This MUST live on the rerelease's appended
// attak301-315 and not on attak101-115: the rerelease RE-AUTHORED attak101-115,
// so in that model the shot is at attak103 and attak104-113 is the cocking.
// Running the classic table over attak1xx therefore fires in the middle of the
// cocking animation, which is exactly the bug this was meant to fix.
// Gated on M_RereleaseAnims() - the classic md2 has no attak3xx at all.
mframe_t infantry_frames_attack3 [] = {
    { ai_charge, 4,  NULL },
    { ai_charge, -1, NULL },
    { ai_charge, -1, NULL },
    { ai_charge, 0,  infantry_cock_gun },
    { ai_charge, -1, NULL },
    { ai_charge, 1,  NULL },
    { ai_charge, 1,  NULL },
    { ai_charge, 2,  NULL },
    { ai_charge, -2, NULL },
    { ai_charge, -3, infantry_set_firetime },
    { ai_charge, 1,  infantry_fire },
    { ai_charge, 5,  NULL },
    { ai_charge, -1, NULL },
    { ai_charge, -2, NULL },
    { ai_charge, -3, NULL }
};
mmove_t infantry_move_attack3 = {FRAME_attak301, FRAME_attak315, infantry_frames_attack3, infantry_run};

// The original, pre-retiming attack1: cock at attak104, fire at attak111.  This
// is the CORRECT reading of attak101-115 for the classic md2, whose animation
// at those indices the rerelease replaced.  Used when M_RereleaseAnims() is off.
mframe_t infantry_frames_attack1_classic [] = {
    { ai_charge, 4,  NULL },
    { ai_charge, -1, NULL },
    { ai_charge, -1, NULL },
    { ai_charge, 0,  infantry_cock_gun },
    { ai_charge, -1, NULL },
    { ai_charge, 1,  NULL },
    { ai_charge, 1,  NULL },
    { ai_charge, 2,  NULL },
    { ai_charge, -2, NULL },
    { ai_charge, -3, NULL },
    { ai_charge, 1,  infantry_fire },
    { ai_charge, 5,  NULL },
    { ai_charge, -1, NULL },
    { ai_charge, -2, NULL },
    { ai_charge, -3, NULL }
};
mmove_t infantry_move_attack1_classic = {FRAME_attak101, FRAME_attak115, infantry_frames_attack1_classic, infantry_run};

//
// RUN AND GUN  (rerelease infantry_move_attack4)
//
// The infantry keeps advancing while it fires, over the appended run201-208, and
// drops back to a standing attack when it runs out of firing time or reaches an
// edge.  Gated on M_RereleaseAnims() - the classic md2 has no run2xx frames.
//
// The rerelease's mmove_t carries a 4th field (0.5f) that halves the playback
// rate; this tree's mmove_t has no such field, so the run cycle plays at normal
// speed.

void infantry_attack4_refire(edict_t *self)
{
    // infantry_fire() above owns the decision to break off the charge, so all
    // this has to do is loop the run cycle when we are still in it.
    infantry_fire(self);

    if (self->monsterinfo.currentmove == &infantry_move_attack4)
        self->monsterinfo.nextframe = FRAME_run201;
}

mframe_t infantry_frames_attack4 [] = {
    { ai_charge, 16, infantry_fire },
    { ai_charge, 16, infantry_fire },
    { ai_charge, 13, infantry_fire },
    { ai_charge, 10, infantry_fire },
    { ai_charge, 16, infantry_fire },
    { ai_charge, 16, infantry_fire },
    { ai_charge, 16, infantry_fire },
    { ai_charge, 16, infantry_attack4_refire }
};
mmove_t infantry_move_attack4 = {FRAME_run201, FRAME_run208, infantry_frames_attack4, infantry_run};

// The infantry's SECOND standing firing pose (rerelease infantry_move_attack5).
// Runs over the appended attak401-423 and is entered at attak405 - the first
// four frames are deliberately skipped, which is why infantry_attack sets
// nextframe when it picks this move.  Fires once, at attak416.
//
// The rerelease has a think on attak411 doing nextframe = s.frame + 1, which is
// what M_MoveFrame does anyway, so it is not carried over as a no-op.
mframe_t infantry_frames_attack5 [] = {
    { ai_charge, 0, NULL },                     // attak401, skipped
    { ai_charge, 0, NULL },                     // attak402, skipped
    { ai_charge, 0, NULL },                     // attak403, skipped
    { ai_charge, 0, NULL },                     // attak404, skipped
    { ai_charge, 0, NULL },                     // attak405 - entry point
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, infantry_cock_gun },        // attak408
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, infantry_set_firetime },    // attak415
    { ai_charge, 0, infantry_fire },            // attak416
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }                      // attak423
};
mmove_t infantry_move_attack5 = {FRAME_attak401, FRAME_attak423, infantry_frames_attack5, infantry_run};


void infantry_swing(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_punch_swing, 1, ATTN_NORM, 0);
}

void infantry_smack(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, 0, 0);
    if (fire_hit(self, aim, (5 + (Q_rand() % 5)), 50))
        gi.sound(self, CHAN_WEAPON, sound_punch_hit, 1, ATTN_NORM, 0);
}

mframe_t infantry_frames_attack2 [] = {
    { ai_charge, 3, NULL },
    { ai_charge, 6, NULL },
    { ai_charge, 0, infantry_swing },
    { ai_charge, 8, NULL },
    { ai_charge, 5, NULL },
    { ai_charge, 8, infantry_smack },
    { ai_charge, 6, NULL },
    { ai_charge, 3, NULL },
};
mmove_t infantry_move_attack2 = {FRAME_attak201, FRAME_attak208, infantry_frames_attack2, infantry_run};

void infantry_attack(edict_t *self)
{
    if (range(self, self->enemy) == RANGE_MELEE)
        self->monsterinfo.currentmove = &infantry_move_attack2;
    else if (!M_RereleaseAnims())
        // classic md2: attak101-115 is the old cock-then-shoot animation, and
        // attak3xx does not exist.  Behave exactly as the game always did.
        self->monsterinfo.currentmove = &infantry_move_attack1_classic;
    else if (self->count)
        // gun is still cocked from an attack that got interrupted - skip
        // straight to the shot, which is what the retimed attack1 does
        self->monsterinfo.currentmove = &infantry_move_attack1;
    else if (random() <= 0.1f) {
        // the second firing pose, which starts part-way into its animation
        self->monsterinfo.currentmove = &infantry_move_attack5;
        self->monsterinfo.nextframe = FRAME_attak405;
    } else
        self->monsterinfo.currentmove = &infantry_move_attack3;
}


/*QUAKED monster_infantry (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/

/*
=================
infantry jumps - the rerelease/ROGUE blocked system

monsterinfo.blocked is called from SV_NewChaseDir when the infantry has run out
of step directions.  It jumps down off ledges and up onto them, and rides
func_plats.  All of this runs on the APPENDED jump frames, so blocked_checkjump
refuses unless M_RereleaseAnims() is on.

Dropped vs the rerelease: monster_done_dodge (no AI_DODGING flag in this tree).
=================
*/
#define SPAWNFLAG_INFANTRY_NOJUMPING   8

static void infantry_jump_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

static void infantry_jump2_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 150, forward, self->velocity);
    VectorMA(self->velocity, 400, up, self->velocity);
}

static void infantry_jump_wait_land(edict_t *self)
{
    if (self->groundentity == NULL) {
        self->monsterinfo.nextframe = self->s.frame;

        if (monster_jump_finished(self))
            self->monsterinfo.nextframe = self->s.frame + 1;
    } else {
        self->monsterinfo.nextframe = self->s.frame + 1;
    }
}

mframe_t infantry_frames_jump [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, infantry_jump_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, infantry_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t infantry_move_jump = {FRAME_jump01, FRAME_jump10, infantry_frames_jump, infantry_run};

mframe_t infantry_frames_jump2 [] = {
    { ai_move, -8, NULL },
    { ai_move, -4, NULL },
    { ai_move, -4, NULL },
    { ai_move, 0, infantry_jump2_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, infantry_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t infantry_move_jump2 = {FRAME_jump01, FRAME_jump10, infantry_frames_jump2, infantry_run};

void infantry_jump(edict_t *self, blocked_jump_result_t result)
{
    if (!self->enemy)
        return;

    if (result == JUMP_JUMP_UP)
        self->monsterinfo.currentmove = &infantry_move_jump2;
    else
        self->monsterinfo.currentmove = &infantry_move_jump;
}

bool infantry_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            infantry_jump(self, result);
        return true;
    }

    if (blocked_checkplat(self, dist))
        return true;

    return false;
}

void SP_monster_infantry(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_pain1 = gi.soundindex("infantry/infpain1.wav");
    sound_pain2 = gi.soundindex("infantry/infpain2.wav");
    sound_die1 = gi.soundindex("infantry/infdeth1.wav");
    sound_die2 = gi.soundindex("infantry/infdeth2.wav");

    sound_gunshot = gi.soundindex("infantry/infatck1.wav");
    sound_weapon_cock = gi.soundindex("infantry/infatck3.wav");
    sound_punch_swing = gi.soundindex("infantry/infatck2.wav");
    sound_punch_hit = gi.soundindex("infantry/melee2.wav");

    sound_sight = gi.soundindex("infantry/infsght1.wav");
    sound_search = gi.soundindex("infantry/infsrch1.wav");
    sound_idle = gi.soundindex("infantry/infidle1.wav");


    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/infantry/tris.md2");
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 32);

    self->health = 100;
    self->gib_health = -40;
    self->mass = 200;

    self->pain = infantry_pain;
    self->die = infantry_die;

    self->monsterinfo.stand = infantry_stand;
    self->monsterinfo.walk = infantry_walk;
    self->monsterinfo.run = infantry_run;
    self->monsterinfo.dodge = M_MonsterDodge;
    self->monsterinfo.duck = infantry_duck;
    self->monsterinfo.unduck = monster_duck_up;
    self->monsterinfo.sidestep = infantry_sidestep;
    self->monsterinfo.attack = infantry_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = infantry_sight;
    self->monsterinfo.idle = infantry_fidget;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &infantry_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    // ROGUE/rerelease: let the infantry jump ledges and ride plats.  The jump
    // animations only exist on the rerelease model, so blocked_checkjump
    // gates itself on M_RereleaseAnims(); the plat half needs no frames.
    if (M_RereleaseGame()) {
        self->monsterinfo.blocked = infantry_blocked;
        self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_INFANTRY_NOJUMPING);
        self->monsterinfo.drop_height = 192;
        self->monsterinfo.jump_height = 40;
    }

    walkmonster_start(self);
}
