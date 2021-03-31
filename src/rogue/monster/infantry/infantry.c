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

#include "../../header/local.h"
#include "infantry.h"

void InfantryMachineGun(edict_t *self);


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

    if (level.time < self->pain_debounce_time)
        return;

    self->pain_debounce_time = level.time + 3;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    n = rand() % 2;
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

    if (self->s.frame == FRAME_attak111) {
        flash_number = MZ2_INFANTRY_MACHINEGUN_1;
        AngleVectors(self->s.angles, forward, right, NULL);
        G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

        if (self->enemy) {
            VectorMA(self->enemy->s.origin, -0.2, self->enemy->velocity, target);
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

    n = rand() % 3;
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


void infantry_duck_down(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_DUCKED)
        return;
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->maxs[2] -= 32;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.pausetime = level.time + 1;
    gi.linkentity(self);
}

void infantry_duck_hold(edict_t *self)
{
    if (level.time >= self->monsterinfo.pausetime)
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
    else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

void infantry_duck_up(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->maxs[2] += 32;
    self->takedamage = DAMAGE_AIM;
    gi.linkentity(self);
}

mframe_t infantry_frames_duck [] = {
    { ai_move, -2, infantry_duck_down },
    { ai_move, -5, infantry_duck_hold },
    { ai_move, 3,  NULL },
    { ai_move, 4,  infantry_duck_up },
    { ai_move, 0,  NULL }
};
mmove_t infantry_move_duck = {FRAME_duck01, FRAME_duck05, infantry_frames_duck, infantry_run};

void infantry_dodge(edict_t *self, edict_t *attacker, float eta)
{
    if (random() > 0.25)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    self->monsterinfo.currentmove = &infantry_move_duck;
}


void infantry_cock_gun(edict_t *self)
{
    int     n;

    gi.sound(self, CHAN_WEAPON, sound_weapon_cock, 1, ATTN_NORM, 0);
    n = (rand() & 15) + 3 + 7;
    self->monsterinfo.pausetime = level.time + n * FRAMETIME;
}

void infantry_fire(edict_t *self)
{
    InfantryMachineGun(self);

    if (level.time >= self->monsterinfo.pausetime)
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
    else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

void
infantry_fire_prep(edict_t* self)
{
    int n;

    if (!self)
    {
        return;
    }

    n = (rand() & 15) + 3 + 1;
    self->monsterinfo.pausetime = level.time + n * FRAMETIME;
}

mframe_t infantry_frames_attack1 [] = {
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
mmove_t infantry_move_attack1 = {FRAME_attak101, FRAME_attak115, infantry_frames_attack1, infantry_run};


void infantry_swing(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_punch_swing, 1, ATTN_NORM, 0);
}

void infantry_smack(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, 0, 0);
    if (fire_hit(self, aim, (5 + (rand() % 5)), 50))
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
    else
        self->monsterinfo.currentmove = &infantry_move_attack1;
}


void
infantry_jump_now(edict_t* self)
{
    vec3_t forward, up;

    if (!self)
    {
        return;
    }

    monster_jump_start(self);

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

void
infantry_jump2_now(edict_t* self)
{
    vec3_t forward, up;

    if (!self)
    {
        return;
    }

    monster_jump_start(self);

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 150, forward, self->velocity);
    VectorMA(self->velocity, 400, up, self->velocity);
}

void
infantry_jump_wait_land(edict_t* self)
{
    if (!self)
    {
        return;
    }

    if (self->groundentity == NULL)
    {
        self->monsterinfo.nextframe = self->s.frame;

        if (monster_jump_finished(self))
        {
            self->monsterinfo.nextframe = self->s.frame + 1;
        }
    }
    else
    {
        self->monsterinfo.nextframe = self->s.frame + 1;
    }
}

mframe_t infantry_frames_jump[] = {
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, infantry_jump_now},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, infantry_jump_wait_land},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};

mmove_t infantry_move_jump = {
    FRAME_jump01,
    FRAME_jump10,
    infantry_frames_jump,
    infantry_run
};

mframe_t infantry_frames_jump2[] = {
    {ai_move, -8, NULL},
    {ai_move, -4, NULL},
    {ai_move, -4, NULL},
    {ai_move, 0, infantry_jump_now},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, infantry_jump_wait_land},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};

mmove_t infantry_move_jump2 = {
    FRAME_jump01,
    FRAME_jump10,
    infantry_frames_jump2,
    infantry_run
};

void
infantry_jump(edict_t* self)
{
    if (!self)
    {
        return;
    }

    if (!self->enemy)
    {
        return;
    }

    monster_done_dodge(self);

    if (self->enemy->s.origin[2] > self->s.origin[2])
    {
        self->monsterinfo.currentmove = &infantry_move_jump2;
    }
    else
    {
        self->monsterinfo.currentmove = &infantry_move_jump;
    }
}

qboolean
infantry_blocked(edict_t* self, float dist)
{
    if (!self)
    {
        return false;
    }

    if (blocked_checkshot(self, 0.25 + (0.05 * skill->value)))
    {
        return true;
    }

    if (blocked_checkjump(self, dist, 192, 40))
    {
        infantry_jump(self);
        return true;
    }

    if (blocked_checkplat(self, dist))
    {
        return true;
    }

    return false;
}

void
infantry_duck(edict_t* self, float eta)
{
    if (!self)
    {
        return;
    }

    /* if we're jumping, don't dodge */
    if ((self->monsterinfo.currentmove == &infantry_move_jump) ||
        (self->monsterinfo.currentmove == &infantry_move_jump2))
    {
        return;
    }

    if ((self->monsterinfo.currentmove == &infantry_move_attack1) ||
        (self->monsterinfo.currentmove == &infantry_move_attack2))
    {
        /* if we're shooting, and not on easy, don't dodge */
        if (skill->value)
        {
            self->monsterinfo.aiflags &= ~AI_DUCKED;
            return;
        }
    }

    if (skill->value == 0)
    {
        /* stupid dodge */
        self->monsterinfo.duck_wait_time = level.time + eta + 1;
    }
    else
    {
        self->monsterinfo.duck_wait_time = level.time + eta + (0.1 * (3 - skill->value));
    }

    /* has to be done immediately otherwise he can get stuck */
    monster_duck_down(self);

    self->monsterinfo.nextframe = FRAME_duck01;
    self->monsterinfo.currentmove = &infantry_move_duck;
    return;
}

void
infantry_sidestep(edict_t* self)
{
    if (!self)
    {
        return;
    }

    /* if we're jumping, don't dodge */
    if ((self->monsterinfo.currentmove == &infantry_move_jump) ||
        (self->monsterinfo.currentmove == &infantry_move_jump2))
    {
        return;
    }

    if ((self->monsterinfo.currentmove == &infantry_move_attack1) ||
        (self->monsterinfo.currentmove == &infantry_move_attack2))
    {
        /* if we're shooting, and not on easy, don't dodge */
        if (skill->value)
        {
            self->monsterinfo.aiflags &= ~AI_DODGING;
            return;
        }
    }

    if (self->monsterinfo.currentmove != &infantry_move_run)
    {
        self->monsterinfo.currentmove = &infantry_move_run;
    }
}


/*QUAKED monster_infantry (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
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
    self->monsterinfo.dodge = infantry_dodge;
    self->monsterinfo.attack = infantry_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = infantry_sight;
    self->monsterinfo.idle = infantry_fidget;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &infantry_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    walkmonster_start(self);
}
