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

GUNNER

==============================================================================
*/

#include "g_local.h"
#include "m_gunner.h"


static int  sound_pain;
static int  sound_pain2;
static int  sound_death;
static int  sound_idle;
static int  sound_open;
static int  sound_search;
static int  sound_sight;


void gunner_idlesound(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

void gunner_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void gunner_search(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}


bool visible(edict_t *self, edict_t *other);
void GunnerGrenade(edict_t *self);
void GunnerFire(edict_t *self);
void gunner_fire_chain(edict_t *self);
void gunner_refire_chain(edict_t *self);


void gunner_stand(edict_t *self);

/*
=================
gunner_shrink

[rerelease] Flatten the corpse partway through the death animation, and mark it
a dead monster there, instead of waiting for the animation to finish. A body
that falls in a doorway stops blocking it while the rest of the death plays.

Gated: this sits in a death table BOTH games play, and the original game keeps
its full-height corpse until the dead-frame handler runs.
=================
*/
static void gunner_shrink(edict_t *self)
{
    if (!M_RereleaseGame())
        return;

    self->maxs[2] = -4;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

mframe_t gunner_frames_fidget [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, gunner_idlesound },
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
mmove_t gunner_move_fidget = {FRAME_stand31, FRAME_stand70, gunner_frames_fidget, gunner_stand};

void gunner_fidget(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        return;
    if (random() <= 0.05f)
        self->monsterinfo.currentmove = &gunner_move_fidget;
}

mframe_t gunner_frames_stand [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, gunner_fidget },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, gunner_fidget },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, gunner_fidget }
};
mmove_t gunner_move_stand = {FRAME_stand01, FRAME_stand30, gunner_frames_stand, NULL};

void gunner_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &gunner_move_stand;
}


mframe_t gunner_frames_walk [] = {
    { ai_walk, 0, NULL },
    { ai_walk, 3, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 7, NULL },
    { ai_walk, 2, monster_footstep },
    { ai_walk, 6, NULL },
    { ai_walk, 4, NULL },
    { ai_walk, 2, NULL },
    { ai_walk, 7, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 7, NULL },
    { ai_walk, 4, monster_footstep }
};
mmove_t gunner_move_walk = {FRAME_walk07, FRAME_walk19, gunner_frames_walk, NULL};

void gunner_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &gunner_move_walk;
}

mframe_t gunner_frames_run [] = {
    { ai_run, 26, NULL },
    { ai_run, 9, monster_footstep },
    { ai_run, 9,  NULL },
    { ai_run, 9, monster_done_dodge },
    { ai_run, 15, NULL },
    { ai_run, 10, monster_footstep },
    { ai_run, 13, NULL },
    { ai_run, 6,  NULL }
};

mmove_t gunner_move_run = {FRAME_run01, FRAME_run08, gunner_frames_run, NULL};

void gunner_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &gunner_move_stand;
    else
        self->monsterinfo.currentmove = &gunner_move_run;
}

mframe_t gunner_frames_runandshoot [] = {
    { ai_run, 32, NULL },
    { ai_run, 15, NULL },
    { ai_run, 10, NULL },
    { ai_run, 18, NULL },
    { ai_run, 8,  NULL },
    { ai_run, 20, NULL }
};

mmove_t gunner_move_runandshoot = {FRAME_runs01, FRAME_runs06, gunner_frames_runandshoot, NULL};

void gunner_runandshoot(edict_t *self)
{
    self->monsterinfo.currentmove = &gunner_move_runandshoot;
}

mframe_t gunner_frames_pain3 [] = {
    { ai_move, -3, NULL },
    { ai_move, 1,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 1,  NULL }
};
mmove_t gunner_move_pain3 = {FRAME_pain301, FRAME_pain305, gunner_frames_pain3, gunner_run};

mframe_t gunner_frames_pain2 [] = {
    { ai_move, -2, NULL },
    { ai_move, 11, NULL },
    { ai_move, 6, monster_footstep },
    { ai_move, 2,  NULL },
    { ai_move, -1, NULL },
    { ai_move, -7, NULL },
    { ai_move, -2, NULL },
    { ai_move, -7, monster_footstep }
};
mmove_t gunner_move_pain2 = {FRAME_pain201, FRAME_pain208, gunner_frames_pain2, gunner_run};

mframe_t gunner_frames_pain1 [] = {
    { ai_move, 2,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, -5, NULL },
    { ai_move, 3,  NULL },
    { ai_move, -1, monster_footstep },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 2,  NULL },
    { ai_move, 1, monster_footstep },
    { ai_move, 0,  NULL },
    { ai_move, -2, NULL },
    { ai_move, -2, NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0, monster_footstep }
};
mmove_t gunner_move_pain1 = {FRAME_pain101, FRAME_pain118, gunner_frames_pain1, gunner_run};

void gunner_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (Q_rand() & 1)
        gi.sound(self, CHAN_VOICE, sound_pain, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if (damage <= 10)
        self->monsterinfo.currentmove = &gunner_move_pain3;
    else if (damage <= 25)
        self->monsterinfo.currentmove = &gunner_move_pain2;
    else
        self->monsterinfo.currentmove = &gunner_move_pain1;

    // [rerelease] being hit ends a blind volley - it also means the gunner now
    // has a much better idea where you are than the remembered position
    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
}

void gunner_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

mframe_t gunner_frames_death [] = {
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0, monster_footstep },
    { ai_move, -7, gunner_shrink },
    { ai_move, -3, NULL },
    { ai_move, -5, NULL },
    { ai_move, 8,  NULL },
    { ai_move, 6,  NULL },
    { ai_move, 0, monster_footstep },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL }
};
mmove_t gunner_move_death = {FRAME_death01, FRAME_death11, gunner_frames_death, gunner_dead};

void gunner_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
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
			
			for (n = 0; n < 16; n++) {
				if (n < 8) {
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
				}
				ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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

// regular death
    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.currentmove = &gunner_move_death;
}





/*
=================
gunner_duck_down

id's gunner lobbed a grenade on the way down, on skill 2+ only, from inside its
own gunner_duck_down.  The rerelease moved that to gunner_duck() and does it on
any skill, so only the classic path still needs it here - and only when the
duck is actually starting, which is what id's early-out on AI_DUCKED gave it.
=================
*/
static void gunner_duck_down(edict_t *self)
{
    if (!M_RereleaseGame() && !(self->monsterinfo.aiflags & AI_DUCKED) &&
        skill->value >= 2 && random() > 0.5f)
        GunnerGrenade(self);

    monster_duck_down(self);
}

mframe_t gunner_frames_duck [] = {
    { ai_move, 1,  gunner_duck_down },
    { ai_move, 1,  NULL },
    { ai_move, 1,  monster_duck_hold },
    { ai_move, 0,  NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, 0,  monster_duck_up },
    { ai_move, -1, NULL }
};
mmove_t gunner_move_duck = {FRAME_duck01, FRAME_duck08, gunner_frames_duck, gunner_run};

// Defined further down this file; the dodge pair below tests against them.
// Same forward-declaration need as the floater and soldier ports.
extern mmove_t gunner_move_jump;
extern mmove_t gunner_move_jump2;
extern mmove_t gunner_move_attack_chain;
extern mmove_t gunner_move_fire_chain;
extern mmove_t gunner_move_attack_grenade;
extern mmove_t gunner_move_attack_grenade2;

/*
=================
gunner_duck / gunner_sidestep

The ROGUE/rerelease dodge pair, replacing the classic one-shot gunner_dodge.
Both report whether they actually took the move, which is what lets
M_MonsterDodge fall back from a sidestep to a duck.

Neither will interrupt a jump or a firing sequence - a gunner that ducked
mid-burst used to abandon the shot.
=================
*/
bool gunner_duck(edict_t *self, float eta)
{
    if (self->monsterinfo.currentmove == &gunner_move_jump2 ||
        self->monsterinfo.currentmove == &gunner_move_jump)
        return false;

    if (self->monsterinfo.currentmove == &gunner_move_attack_chain ||
        self->monsterinfo.currentmove == &gunner_move_fire_chain ||
        self->monsterinfo.currentmove == &gunner_move_attack_grenade ||
        self->monsterinfo.currentmove == &gunner_move_attack_grenade2) {
        // already shooting - stand back up rather than half-duck
        monster_duck_up(self);
        return false;
    }

    // the rerelease keeps id's "lob one on the way down" on any skill; the
    // classic code gated it on skill 2 and does it from gunner_duck_down
    if (random() > 0.5f)
        GunnerGrenade(self);

    self->monsterinfo.currentmove = &gunner_move_duck;
    return true;
}

/*
=================
gunner_dodge

monsterinfo.dodge for every gunner, in both games.  The rerelease hands over to
M_MonsterDodge and its duck + sidestep pair; the ORIGINAL game gets id's 1997
dodge back verbatim - a flat 25% chance of a plain crouch.

(This symbol also has to keep existing: g_ptrs_compat_v2.c is a frozen table
for version-2 saves and names it.)
=================
*/
void gunner_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    if (M_RereleaseGame()) {
        M_MonsterDodge(self, attacker, eta, tr, gravity);
        return;
    }

    if (random() > 0.25f)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    self->monsterinfo.currentmove = &gunner_move_duck;
}

bool gunner_sidestep(edict_t *self)
{
    if (self->monsterinfo.currentmove == &gunner_move_jump2 ||
        self->monsterinfo.currentmove == &gunner_move_jump ||
        self->monsterinfo.currentmove == &gunner_move_pain1)
        return false;

    if (self->monsterinfo.currentmove == &gunner_move_attack_chain ||
        self->monsterinfo.currentmove == &gunner_move_fire_chain ||
        self->monsterinfo.currentmove == &gunner_move_attack_grenade ||
        self->monsterinfo.currentmove == &gunner_move_attack_grenade2)
        return false;

    // strafing happens on the run move; AS_SLIDING is what makes ai_run
    // sidestep rather than close
    if (self->monsterinfo.currentmove != &gunner_move_run)
        self->monsterinfo.currentmove = &gunner_move_run;

    return true;
}


void gunner_opengun(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_open, 1, ATTN_IDLE, 0);
}

void GunnerFire(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  target;
    vec3_t  aim;
    int     flash_number;

    flash_number = MZ2_GUNNER_MACHINEGUN_1 + (self->s.frame - FRAME_attak216);

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

    // project enemy back a bit and target there
    VectorCopy(self->enemy->s.origin, target);
    VectorMA(target, -0.2f, self->enemy->velocity, target);
    target[2] += self->enemy->viewheight;

    VectorSubtract(target, start, aim);
    VectorNormalize(aim);
    monster_fire_bullet(self, start, aim, 3, 4, DEFAULT_BULLET_HSPREAD, DEFAULT_BULLET_VSPREAD, flash_number);
}

/*
=================
gunner_blind_check

[rerelease] Runs on the first frame of either grenade animation.  AI_MANUAL_STEERING
is how gunner_attack signals "this volley is a blind one"; the generic AI has
been told to leave ideal_yaw alone while it is set, so this is what actually
points the gunner at the remembered position.
=================
*/
void gunner_blind_check(edict_t *self)
{
    vec3_t  aim;

    if (self->monsterinfo.aiflags & AI_MANUAL_STEERING) {
        VectorSubtract(self->monsterinfo.blind_fire_target, self->s.origin, aim);
        self->ideal_yaw = vectoyaw(aim);
    }
}

void GunnerGrenade(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  aim;
    int     flash_number;
    float   spread;
    bool    blindfire = (self->monsterinfo.aiflags & AI_MANUAL_STEERING) != 0;

    // attak105/108/111/114 are the classic throw; attak309/312/315/318 are the
    // same four shots in the rerelease's second, front-on throwing animation
    if (self->s.frame == FRAME_attak105 || self->s.frame == FRAME_attak309) {
        spread = -0.10f;
        flash_number = MZ2_GUNNER_GRENADE_1;
    } else if (self->s.frame == FRAME_attak108 || self->s.frame == FRAME_attak312) {
        spread = -0.05f;
        flash_number = MZ2_GUNNER_GRENADE_2;
    } else if (self->s.frame == FRAME_attak111 || self->s.frame == FRAME_attak315) {
        spread = 0.05f;
        flash_number = MZ2_GUNNER_GRENADE_3;
    } else { // attak114, or attak318
        // the last grenade of the volley ends the blind volley with it
        self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
        spread = 0.10f;
        flash_number = MZ2_GUNNER_GRENADE_4;
    }

    // the second animation holds the launcher differently, so it has its own
    // muzzle points - and id maps them in reverse order
    if (self->s.frame >= FRAME_attak301 && self->s.frame <= FRAME_attak324)
        flash_number = MZ2_GUNNER_GRENADE2_1 + (MZ2_GUNNER_GRENADE_4 - flash_number);

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

    VectorCopy(forward, aim);

    // [rerelease] a blind volley lobs at the remembered position rather than
    // straight ahead - the yaw is already there via gunner_blind_check, but the
    // pitch has to come from the target or every blind grenade flies level
    if (blindfire && !visible(self, self->enemy)) {
        vec3_t  blind_aim;

        if (VectorEmpty(self->monsterinfo.blind_fire_target))
            return;

        VectorSubtract(self->monsterinfo.blind_fire_target, start, blind_aim);
        if (VectorNormalize(blind_aim) > 0.0f)
            VectorCopy(blind_aim, aim);
    }
    // the classic code never implemented the fan its own comment asked for
    // ("FIXME: do a spread ... around forward"); the rerelease does
    if (M_RereleaseGame())
        VectorMA(aim, spread, right, aim);

    monster_fire_grenade(self, start, aim, 50, 600, flash_number);
}

mframe_t gunner_frames_attack_chain [] = {
    /*
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    */
    { ai_charge, 0, gunner_opengun },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t gunner_move_attack_chain = {FRAME_attak209, FRAME_attak215, gunner_frames_attack_chain, gunner_fire_chain};

mframe_t gunner_frames_fire_chain [] = {
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire },
    { ai_charge,   0, GunnerFire }
};
mmove_t gunner_move_fire_chain = {FRAME_attak216, FRAME_attak223, gunner_frames_fire_chain, gunner_refire_chain};

mframe_t gunner_frames_endfire_chain [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, monster_footstep }
};
mmove_t gunner_move_endfire_chain = {FRAME_attak224, FRAME_attak230, gunner_frames_endfire_chain, gunner_run};

mframe_t gunner_frames_attack_grenade [] = {
    { ai_charge, 0, gunner_blind_check },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t gunner_move_attack_grenade = {FRAME_attak101, FRAME_attak121, gunner_frames_attack_grenade, gunner_run};

// The rerelease's second grenade throw, on the APPENDED attak305-324 frames.
// Same four-shot cadence as attack_grenade, thrown from a front-on pose.
// Only reachable behind M_RereleaseAnims() - the classic model has no such frames.
mframe_t gunner_frames_attack_grenade2 [] = {
    { ai_charge, 0, gunner_blind_check },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, GunnerGrenade },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
};
mmove_t gunner_move_attack_grenade2 = {FRAME_attak305, FRAME_attak324, gunner_frames_attack_grenade2, gunner_run};

void gunner_attack(edict_t *self)
{
    // [rerelease] Blind fire: M_CheckAttack put us in AS_BLIND because the
    // enemy is out of sight but recently seen.  The chance ladder falls off
    // with the accumulated delay, so the first blind shot after losing someone
    // is a certainty and the fifth is a long shot.
    if (M_RereleaseGame() && self->monsterinfo.attack_state == AS_BLIND) {
        float   chance;

        if (self->timestamp > level.framenum)
            return;

        if (self->monsterinfo.blind_fire_delay < 1.0f * BASE_FRAMERATE)
            chance = 1.0f;
        else if (self->monsterinfo.blind_fire_delay < 7.5f * BASE_FRAMERATE)
            chance = 0.4f;
        else
            chance = 0.1f;

        // minimum of 4.1 seconds, plus 0-3, before the next attempt
        self->monsterinfo.blind_fire_delay += (4.1f + 3.0f * random()) * BASE_FRAMERATE;

        // never shoot at the world origin
        if (VectorEmpty(self->monsterinfo.blind_fire_target))
            return;

        if (random() > chance)
            return;

        // doubles as the "this volley is blind" signal to GunnerGrenade
        self->monsterinfo.aiflags |= AI_MANUAL_STEERING;

        if (M_RereleaseAnims() && random() < 0.5f)
            self->monsterinfo.currentmove = &gunner_move_attack_grenade2;
        else
            self->monsterinfo.currentmove = &gunner_move_attack_grenade;

        self->monsterinfo.attack_finished = level.framenum + 2.0f * random() * BASE_FRAMERATE;
        self->timestamp = level.framenum + (2.0f + random()) * BASE_FRAMERATE;
        return;
    }

    if (range(self, self->enemy) == RANGE_MELEE) {
        self->monsterinfo.currentmove = &gunner_move_attack_chain;
    } else {
        if (random() <= 0.5f) {
            // the rerelease picks between its two throwing animations 50/50
            if (M_RereleaseAnims() && random() < 0.5f)
                self->monsterinfo.currentmove = &gunner_move_attack_grenade2;
            else
                self->monsterinfo.currentmove = &gunner_move_attack_grenade;
        } else {
            self->monsterinfo.currentmove = &gunner_move_attack_chain;
        }
    }
}

void gunner_fire_chain(edict_t *self)
{
    self->monsterinfo.currentmove = &gunner_move_fire_chain;
}

void gunner_refire_chain(edict_t *self)
{
    if (self->enemy->health > 0)
        if (visible(self, self->enemy))
            if (random() <= 0.5f) {
                self->monsterinfo.currentmove = &gunner_move_fire_chain;
                return;
            }
    self->monsterinfo.currentmove = &gunner_move_endfire_chain;
}

/*QUAKED monster_gunner (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/

/*
=================
gunner jumps - the rerelease/ROGUE blocked system

monsterinfo.blocked is called from SV_NewChaseDir when the gunner has run out
of step directions.  It jumps down off ledges and up onto them, and rides
func_plats.  All of this runs on the APPENDED jump frames, so blocked_checkjump
refuses unless M_RereleaseAnims() is on.

Dropped vs the rerelease: monster_done_dodge (no AI_DODGING flag in this tree).
=================
*/
#define SPAWNFLAG_GUNNER_NOJUMPING   8

static void gunner_jump_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

static void gunner_jump2_now(edict_t *self)
{
    vec3_t  forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 150, forward, self->velocity);
    VectorMA(self->velocity, 400, up, self->velocity);
}

static void gunner_jump_wait_land(edict_t *self)
{
    if (self->groundentity == NULL) {
        self->monsterinfo.nextframe = self->s.frame;

        if (monster_jump_finished(self))
            self->monsterinfo.nextframe = self->s.frame + 1;
    } else {
        self->monsterinfo.nextframe = self->s.frame + 1;
    }
}

mframe_t gunner_frames_jump [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, gunner_jump_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, gunner_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t gunner_move_jump = {FRAME_jump01, FRAME_jump10, gunner_frames_jump, gunner_run};

mframe_t gunner_frames_jump2 [] = {
    { ai_move, -8, NULL },
    { ai_move, -4, NULL },
    { ai_move, -4, NULL },
    { ai_move, 0, gunner_jump2_now },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, gunner_jump_wait_land },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
};
mmove_t gunner_move_jump2 = {FRAME_jump01, FRAME_jump10, gunner_frames_jump2, gunner_run};

void gunner_jump(edict_t *self, blocked_jump_result_t result)
{
    if (!self->enemy)
        return;

    if (result == JUMP_JUMP_UP)
        self->monsterinfo.currentmove = &gunner_move_jump2;
    else
        self->monsterinfo.currentmove = &gunner_move_jump;
}

bool gunner_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result;

    if (blocked_checkplat(self, dist))
        return true;

    result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            gunner_jump(self, result);
        return true;
    }

    return false;
}

void SP_monster_gunner(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_death = gi.soundindex("gunner/death1.wav");
    sound_pain = gi.soundindex("gunner/gunpain2.wav");
    sound_pain2 = gi.soundindex("gunner/gunpain1.wav");
    sound_idle = gi.soundindex("gunner/gunidle1.wav");
    sound_open = gi.soundindex("gunner/gunatck1.wav");
    sound_search = gi.soundindex("gunner/gunsrch1.wav");
    sound_sight = gi.soundindex("gunner/sight1.wav");

    gi.soundindex("gunner/gunatck2.wav");
    gi.soundindex("gunner/gunatck3.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/gunner/tris.md2");
    // [rerelease] 4 units TALLER - the one monster id grew rather than shrank
    if (M_RereleaseGame()) {
        VectorSet(self->mins, -16, -16, -24);
        VectorSet(self->maxs, 16, 16, 36);
    } else {
        VectorSet(self->mins, -16, -16, -24);
        VectorSet(self->maxs, 16, 16, 32);
    }

    self->health = 175;
    self->gib_health = -70;
    self->mass = 200;

    self->pain = gunner_pain;
    self->die = gunner_die;

    self->monsterinfo.stand = gunner_stand;
    self->monsterinfo.walk = gunner_walk;
    self->monsterinfo.run = gunner_run;
    // gunner_dodge is the classic dodge in baseq2 and forwards to M_MonsterDodge
    // in the rerelease.  duck/sidestep are what M_MonsterDodge drives, so the
    // original game must not advertise them at all.
    self->monsterinfo.dodge = gunner_dodge;
    if (M_RereleaseGame()) {
        self->monsterinfo.duck = gunner_duck;
        self->monsterinfo.unduck = monster_duck_up;
        self->monsterinfo.sidestep = gunner_sidestep;
    }
    self->monsterinfo.attack = gunner_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = gunner_sight;
    self->monsterinfo.search = gunner_search;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &gunner_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    // ROGUE/rerelease: let the gunner jump ledges and ride plats.  The jump
    // animations only exist on the rerelease model, so blocked_checkjump
    // gates itself on M_RereleaseAnims(); the plat half needs no frames.
    if (M_RereleaseGame()) {
        self->monsterinfo.blocked = gunner_blocked;
        self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_GUNNER_NOJUMPING);
        self->monsterinfo.drop_height = 192;
        self->monsterinfo.jump_height = 40;
    }

    // [rerelease] the gunner lobs grenades at your last known position rather
    // than waiting for you to step back into the doorway
    self->monsterinfo.blindfire = M_RereleaseGame();

    walkmonster_start(self);
}
