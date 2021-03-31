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

MEDIC

==============================================================================
*/

#include "../../header/local.h"
#include "medic.h"

qboolean visible(edict_t *self, edict_t *other);


static int  sound_idle1;
static int  sound_pain1;
static int  sound_pain2;
static int  sound_die;
static int  sound_sight;
static int  sound_search;
static int  sound_hook_launch;
static int  sound_hook_hit;
static int  sound_hook_heal;
static int  sound_hook_retract;

/* commander sounds */
static int commander_sound_idle1;
static int commander_sound_pain1;
static int commander_sound_pain2;
static int commander_sound_die;
static int commander_sound_sight;
static int commander_sound_search;
static int commander_sound_hook_launch;
static int commander_sound_hook_hit;
static int commander_sound_hook_heal;
static int commander_sound_hook_retract;
static int commander_sound_spawn;

char* reinforcements[] = {
    "monster_soldier_light",  /* 0 */
    "monster_soldier",        /* 1 */
    "monster_soldier_ss",     /* 2 */
    "monster_infantry",       /* 3 */
    "monster_gunner",         /* 4 */
    "monster_medic",          /* 5 */
    "monster_gladiator"       /* 6 */
};

vec3_t reinforcement_mins[] = {
    {-16, -16, -24},
    {-16, -16, -24},
    {-16, -16, -24},
    {-16, -16, -24},
    {-16, -16, -24},
    {-16, -16, -24},
    {-32, -32, -24}
};

vec3_t reinforcement_maxs[] = {
    {16, 16, 32},
    {16, 16, 32},
    {16, 16, 32},
    {16, 16, 32},
    {16, 16, 32},
    {16, 16, 32},
    {32, 32, 64}
};

vec3_t reinforcement_position[] = {
    {80, 0, 0},
    {40, 60, 0},
    {40, -60, 0},
    {0, 80, 0},
    {0, -80, 0}
};



void
cleanupHeal(edict_t* self, qboolean change_frame)
{
    if (!self)
    {
        return;
    }

    /* clean up target, if we have one and it's legit */
    if (self->enemy && self->enemy->inuse)
    {
        self->enemy->monsterinfo.healer = NULL;
        self->enemy->monsterinfo.aiflags &= ~AI_RESURRECTING;
        self->enemy->takedamage = DAMAGE_YES;
        M_SetEffects(self->enemy);
    }

    if (change_frame)
    {
        self->monsterinfo.nextframe = FRAME_attack52;
    }
}

void
abortHeal(edict_t* self, qboolean change_frame, qboolean gib, qboolean mark)
{
    int hurt;
    static vec3_t pain_normal = { 0, 0, 1 };

    if (!self)
    {
        return;
    }

    /* clean up target */
    cleanupHeal(self, change_frame);

    /* gib em! */
    if ((mark) && (self->enemy) && (self->enemy->inuse))
    {
        if ((self->enemy->monsterinfo.badMedic1) &&
            (self->enemy->monsterinfo.badMedic1->inuse) &&
            (!strncmp(self->enemy->monsterinfo.badMedic1->classname, "monster_medic", 13)))
        {
            self->enemy->monsterinfo.badMedic2 = self;
        }
        else
        {
            self->enemy->monsterinfo.badMedic1 = self;
        }
    }

    if ((gib) && (self->enemy) && (self->enemy->inuse))
    {
        if (self->enemy->gib_health)
        {
            hurt = -self->enemy->gib_health;
        }
        else
        {
            hurt = 500;
        }

        T_Damage(self->enemy, self, self, vec3_origin, self->enemy->s.origin,
            pain_normal, hurt, 0, 0, MOD_UNKNOWN);
    }

    /* clean up self */
    self->monsterinfo.aiflags &= ~AI_MEDIC;

    if ((self->oldenemy) && (self->oldenemy->inuse))
    {
        self->enemy = self->oldenemy;
    }
    else
    {
        self->enemy = NULL;
    }

    self->monsterinfo.medicTries = 0;
}

qboolean
canReach(edict_t* self, edict_t* other)
{
    vec3_t spot1;
    vec3_t spot2;
    trace_t trace;

    if (!self || !other)
    {
        return false;
    }

    VectorCopy(self->s.origin, spot1);
    spot1[2] += self->viewheight;
    VectorCopy(other->s.origin, spot2);
    spot2[2] += other->viewheight;
    trace = gi.trace(spot1, vec3_origin, vec3_origin, spot2,
        self, MASK_SHOT | MASK_WATER);

    if ((trace.fraction == 1.0) || (trace.ent == other))
    {
        return true;
    }

    return false;
}


edict_t *medic_FindDeadMonster(edict_t *self)
{
    edict_t *ent = NULL;
    edict_t *best = NULL;

    while ((ent = findradius(ent, self->s.origin, 1024)) != NULL) {
        if (ent == self)
            continue;
        if (!(ent->svflags & SVF_MONSTER))
            continue;
        if (ent->monsterinfo.aiflags & AI_GOOD_GUY)
            continue;
        if (ent->owner)
            continue;
        if (ent->health > 0)
            continue;
        if (ent->nextthink)
            continue;
        if (!visible(self, ent))
            continue;
        if (!best) {
            best = ent;
            continue;
        }
        if (ent->max_health <= best->max_health)
            continue;
        best = ent;
    }

    return best;
}

void medic_idle(edict_t *self)
{
    edict_t *ent;

    gi.sound(self, CHAN_VOICE, sound_idle1, 1, ATTN_IDLE, 0);

    ent = medic_FindDeadMonster(self);
    if (ent) {
        self->enemy = ent;
        self->enemy->owner = self;
        self->monsterinfo.aiflags |= AI_MEDIC;
        FoundTarget(self);
    }
}

void medic_search(edict_t *self)
{
    edict_t *ent;

    gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_IDLE, 0);

    if (!self->oldenemy) {
        ent = medic_FindDeadMonster(self);
        if (ent) {
            self->oldenemy = self->enemy;
            self->enemy = ent;
            self->enemy->owner = self;
            self->monsterinfo.aiflags |= AI_MEDIC;
            FoundTarget(self);
        }
    }
}

void medic_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}


mframe_t medic_frames_stand [] = {
    { ai_stand, 0, medic_idle },
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

};
mmove_t medic_move_stand = {FRAME_wait1, FRAME_wait90, medic_frames_stand, NULL};

void medic_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &medic_move_stand;
}


mframe_t medic_frames_walk [] = {
    { ai_walk, 6.2,   NULL },
    { ai_walk, 18.1,  NULL },
    { ai_walk, 1,     NULL },
    { ai_walk, 9,     NULL },
    { ai_walk, 10,    NULL },
    { ai_walk, 9,     NULL },
    { ai_walk, 11,    NULL },
    { ai_walk, 11.6,  NULL },
    { ai_walk, 2,     NULL },
    { ai_walk, 9.9,   NULL },
    { ai_walk, 14,    NULL },
    { ai_walk, 9.3,   NULL }
};
mmove_t medic_move_walk = {FRAME_walk1, FRAME_walk12, medic_frames_walk, NULL};

void medic_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &medic_move_walk;
}


mframe_t medic_frames_run [] = {
    { ai_run, 18,     NULL },
    { ai_run, 22.5,   NULL },
    { ai_run, 25.4,   NULL },
    { ai_run, 23.4,   NULL },
    { ai_run, 24,     NULL },
    { ai_run, 35.6,   NULL }

};
mmove_t medic_move_run = {FRAME_run1, FRAME_run6, medic_frames_run, NULL};

void medic_run(edict_t *self)
{
    if (!(self->monsterinfo.aiflags & AI_MEDIC)) {
        edict_t *ent;

        ent = medic_FindDeadMonster(self);
        if (ent) {
            self->oldenemy = self->enemy;
            self->enemy = ent;
            self->enemy->owner = self;
            self->monsterinfo.aiflags |= AI_MEDIC;
            FoundTarget(self);
            return;
        }
    }

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &medic_move_stand;
    else
        self->monsterinfo.currentmove = &medic_move_run;
}


mframe_t medic_frames_pain1 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t medic_move_pain1 = {FRAME_paina1, FRAME_paina8, medic_frames_pain1, medic_run};

mframe_t medic_frames_pain2 [] = {
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
mmove_t medic_move_pain2 = {FRAME_painb1, FRAME_painb15, medic_frames_pain2, medic_run};

void medic_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;

    if (level.time < self->pain_debounce_time)
        return;

    self->pain_debounce_time = level.time + 3;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if (random() < 0.5) {
        self->monsterinfo.currentmove = &medic_move_pain1;
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
    } else {
        self->monsterinfo.currentmove = &medic_move_pain2;
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
    }
}

void medic_fire_blaster(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  end;
    vec3_t  dir;
    int     effect;

    if ((self->s.frame == FRAME_attack9) || (self->s.frame == FRAME_attack12))
		if (self->monsterFireHyperBlaster) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else if ((self->s.frame == FRAME_attack19) || (self->s.frame == FRAME_attack22) || (self->s.frame == FRAME_attack25) || (self->s.frame == FRAME_attack28))
		if (self->monsterFireHyperBlaster) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else
        effect = 0;

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[MZ2_MEDIC_BLASTER_1], forward, right, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);

	if (self->monsterFireHyperBlaster) {
		monster_fire_hyper_blaster(self, start, dir, 2, 1000, MZ2_MEDIC_BLASTER_1, effect);
	}
	else {
		monster_fire_blaster(self, start, dir, 2, 1000, MZ2_MEDIC_BLASTER_1, effect);
	}
}


void medic_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

mframe_t medic_frames_death [] = {
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
mmove_t medic_move_death = {FRAME_death1, FRAME_death30, medic_frames_death, medic_dead};

void medic_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

    // if we had a pending patient, free him up for another medic
    if ((self->enemy) && (self->enemy->owner == self))
        self->enemy->owner = NULL;

// check for gib
    if (self->health <= self->gib_health) {
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
		if (InflictorGibExplosion(inflictor, self)) {
			VectorScale(self->size, 1.2, self->size);

			for (n = 0; n < 16; n++) {
				if (n < 8) {
					ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
				}
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
				ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
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
    gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

    self->monsterinfo.currentmove = &medic_move_death;
}


void medic_duck_down(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_DUCKED)
        return;
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->maxs[2] -= 32;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.pausetime = level.time + 1;
    gi.linkentity(self);
}

void medic_duck_hold(edict_t *self)
{
    if (level.time >= self->monsterinfo.pausetime)
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
    else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

void medic_duck_up(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->maxs[2] += 32;
    self->takedamage = DAMAGE_AIM;
    gi.linkentity(self);
}

mframe_t medic_frames_duck [] = {
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    medic_duck_down },
    { ai_move, -1,    medic_duck_hold },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    medic_duck_up },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL }
};
mmove_t medic_move_duck = {FRAME_duck1, FRAME_duck16, medic_frames_duck, medic_run};

void medic_dodge(edict_t *self, edict_t *attacker, float eta)
{
    if (random() > 0.25)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    self->monsterinfo.currentmove = &medic_move_duck;
}

mframe_t medic_frames_attackHyperBlaster [] = {
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   medic_fire_blaster }
};
mmove_t medic_move_attackHyperBlaster = {FRAME_attack15, FRAME_attack30, medic_frames_attackHyperBlaster, medic_run};


void medic_continue(edict_t *self)
{
    if (visible(self, self->enemy))
        if (random() <= 0.95)
            self->monsterinfo.currentmove = &medic_move_attackHyperBlaster;
}


mframe_t medic_frames_attackBlaster [] = {
    { ai_charge, 0,   NULL },
    { ai_charge, 5,   NULL },
    { ai_charge, 5,   NULL },
    { ai_charge, 3,   NULL },
    { ai_charge, 2,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_continue }  // Change to medic_continue... Else, go to frame 32
};
mmove_t medic_move_attackBlaster = {FRAME_attack1, FRAME_attack14, medic_frames_attackBlaster, medic_run};


void medic_hook_launch(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_hook_launch, 1, ATTN_NORM, 0);
}

void ED_CallSpawn(edict_t *ent);

static vec3_t   medic_cable_offsets[] = {
    { 45.0,  -9.2, 15.5 },
    { 48.4,  -9.7, 15.2 },
    { 47.8,  -9.8, 15.8 },
    { 47.3,  -9.3, 14.3 },
    { 45.4, -10.1, 13.1 },
    { 41.9, -12.7, 12.0 },
    { 37.8, -15.8, 11.2 },
    { 34.3, -18.4, 10.7 },
    { 32.7, -19.7, 10.4 },
    { 32.7, -19.7, 10.4 }
};

void medic_cable_attack(edict_t *self)
{
    vec3_t  offset, start, end, f, r;
    trace_t tr;
    vec3_t  dir, angles;
    float   distance;

    if (!self->enemy->inuse)
        return;

    AngleVectors(self->s.angles, f, r, NULL);
    VectorCopy(medic_cable_offsets[self->s.frame - FRAME_attack42], offset);
    G_ProjectSource(self->s.origin, offset, f, r, start);

    // check for max distance
    VectorSubtract(start, self->enemy->s.origin, dir);
    distance = VectorLength(dir);
    if (distance > 256)
        return;

    // check for min/max pitch
    vectoangles(dir, angles);
    if (angles[0] < -180)
        angles[0] += 360;
    if (fabs(angles[0]) > 45)
        return;

    tr = gi.trace(start, NULL, NULL, self->enemy->s.origin, self, MASK_SHOT);
    if (tr.fraction != 1.0 && tr.ent != self->enemy)
        return;

    if (self->s.frame == FRAME_attack43) {
        gi.sound(self->enemy, CHAN_AUTO, sound_hook_hit, 1, ATTN_NORM, 0);
        self->enemy->monsterinfo.aiflags |= AI_RESURRECTING;
    } else if (self->s.frame == FRAME_attack50) {
        self->enemy->spawnflags = 0;
        self->enemy->monsterinfo.aiflags = 0;
        self->enemy->target = NULL;
        self->enemy->targetname = NULL;
        self->enemy->combattarget = NULL;
        self->enemy->deathtarget = NULL;
        self->enemy->owner = self;
        ED_CallSpawn(self->enemy);
        self->enemy->owner = NULL;
        if (self->enemy->think) {
            self->enemy->nextthink = level.time;
            self->enemy->think(self->enemy);
        }
        self->enemy->monsterinfo.aiflags |= AI_RESURRECTING;
        if (self->oldenemy && self->oldenemy->client) {
            self->enemy->enemy = self->oldenemy;
            FoundTarget(self->enemy);
        }
    } else {
        if (self->s.frame == FRAME_attack44)
            gi.sound(self, CHAN_WEAPON, sound_hook_heal, 1, ATTN_NORM, 0);
    }

    // adjust start for beam origin being in middle of a segment
    VectorMA(start, 8, f, start);

    // adjust end z for end spot since the monster is currently dead
    VectorCopy(self->enemy->s.origin, end);
    end[2] = self->enemy->absmin[2] + self->enemy->size[2] / 2;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_MEDIC_CABLE_ATTACK);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(end);
    gi.multicast(self->s.origin, MULTICAST_PVS);
}

void medic_hook_retract(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, sound_hook_retract, 1, ATTN_NORM, 0);
    self->enemy->monsterinfo.aiflags &= ~AI_RESURRECTING;
}

mframe_t medic_frames_attackCable [] = {
    { ai_move, 2,     NULL },
    { ai_move, 3,     NULL },
    { ai_move, 5,     NULL },
    { ai_move, 4.4,   NULL },
    { ai_charge, 4.7, NULL },
    { ai_charge, 5,   NULL },
    { ai_charge, 6,   NULL },
    { ai_charge, 4,   NULL },
    { ai_charge, 0,   NULL },
    { ai_move, 0,     medic_hook_launch },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, 0,     medic_cable_attack },
    { ai_move, -15,   medic_hook_retract },
    { ai_move, -1.5,  NULL },
    { ai_move, -1.2,  NULL },
    { ai_move, -3,    NULL },
    { ai_move, -2,    NULL },
    { ai_move, 0.3,   NULL },
    { ai_move, 0.7,   NULL },
    { ai_move, 1.2,   NULL },
    { ai_move, 1.3,   NULL }
};
mmove_t medic_move_attackCable = {FRAME_attack33, FRAME_attack60, medic_frames_attackCable, medic_run};

void
medic_start_spawn(edict_t* self)
{
    if (!self)
    {
        return;
    }

    gi.sound(self, CHAN_WEAPON, commander_sound_spawn, 1, ATTN_NORM, 0);
    self->monsterinfo.nextframe = FRAME_attack48;
}

void
medic_determine_spawn(edict_t* self)
{
    vec3_t f, r, offset, startpoint, spawnpoint;
    float lucky;
    int summonStr;
    int count;
    int inc;
    int num_summoned; /* should be 1, 3, or 5 */
    int num_success = 0;

    lucky = random();
    summonStr = skill->value;

    if (!self)
    {
        return;
    }

    if (lucky < 0.05)
    {
        summonStr -= 3;
    }
    else if (lucky < 0.15)
    {
        summonStr -= 2;
    }
    else if (lucky < 0.3)
    {
        summonStr -= 1;
    }
    else if (lucky > 0.95)
    {
        summonStr += 3;
    }
    else if (lucky > 0.85)
    {
        summonStr += 2;
    }
    else if (lucky > 0.7)
    {
        summonStr += 1;
    }

    if (summonStr < 0)
    {
        summonStr = 0;
    }

    self->plat2flags = summonStr;
    AngleVectors(self->s.angles, f, r, NULL);

    /* this yields either 1, 3, or 5 */
    if (summonStr)
    {
        num_summoned = (summonStr - 1) + (summonStr % 2);
    }
    else
    {
        num_summoned = 1;
    }

    for (count = 0; count < num_summoned; count++)
    {
        inc = count + (count % 2); /* 0, 2, 2, 4, 4 */
        VectorCopy(reinforcement_position[count], offset);

        G_ProjectSource(self->s.origin, offset, f, r, startpoint);
        startpoint[2] += 10;

        if (FindSpawnPoint(startpoint, reinforcement_mins[summonStr - inc],
            reinforcement_maxs[summonStr - inc], spawnpoint, 32))
        {
            if (CheckGroundSpawnPoint(spawnpoint, reinforcement_mins[summonStr - inc],
                reinforcement_maxs[summonStr - inc], 256, -1))
            {
                num_success++;
                /* we found a spot, we're done here */
                count = num_summoned;
            }
        }
    }

    if (num_success == 0)
    {
        for (count = 0; count < num_summoned; count++)
        {
            inc = count + (count % 2); /* 0, 2, 2, 4, 4 */
            VectorCopy(reinforcement_position[count], offset);

            /* check behind */
            offset[0] *= -1.0;
            offset[1] *= -1.0;
            G_ProjectSource(self->s.origin, offset, f, r, startpoint);
            /* a little off the ground */
            startpoint[2] += 10;

            if (FindSpawnPoint(startpoint, reinforcement_mins[summonStr - inc],
                reinforcement_maxs[summonStr - inc], spawnpoint, 32))
            {
                if (CheckGroundSpawnPoint(spawnpoint, reinforcement_mins[summonStr - inc],
                    reinforcement_maxs[summonStr - inc], 256, -1))
                {
                    num_success++;
                    /* we found a spot, we're done here */
                    count = num_summoned;
                }
            }
        }

        if (num_success)
        {
            self->monsterinfo.aiflags |= AI_MANUAL_STEERING;
            self->ideal_yaw = anglemod(self->s.angles[YAW]) + 180;

            if (self->ideal_yaw > 360.0)
            {
                self->ideal_yaw -= 360.0;
            }
        }
    }

    if (num_success == 0)
    {
        self->monsterinfo.nextframe = FRAME_attack53;
    }
}


void
medic_spawngrows(edict_t* self)
{
    vec3_t f, r, offset, startpoint, spawnpoint;
    int summonStr;
    int count;
    int inc;
    int num_summoned; /* should be 1, 3, or 5 */
    int num_success = 0;
    float current_yaw;

    if (!self)
    {
        return;
    }

    /* if we've been directed to turn around */
    if (self->monsterinfo.aiflags & AI_MANUAL_STEERING)
    {
        current_yaw = anglemod(self->s.angles[YAW]);

        if (fabs(current_yaw - self->ideal_yaw) > 0.1)
        {
            self->monsterinfo.aiflags |= AI_HOLD_FRAME;
            return;
        }

        /* done turning around */
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
        self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
    }

    summonStr = self->plat2flags;
    AngleVectors(self->s.angles, f, r, NULL);

    if (summonStr)
    {
        num_summoned = (summonStr - 1) + (summonStr % 2);
    }
    else
    {
        num_summoned = 1;
    }

    for (count = 0; count < num_summoned; count++)
    {
        inc = count + (count % 2); /* 0, 2, 2, 4, 4 */
        VectorCopy(reinforcement_position[count], offset);

        G_ProjectSource(self->s.origin, offset, f, r, startpoint);
        /* a little off the ground */
        startpoint[2] += 10;

        if (FindSpawnPoint(startpoint, reinforcement_mins[summonStr - inc],
            reinforcement_maxs[summonStr - inc], spawnpoint, 32))
        {
            if (CheckGroundSpawnPoint(spawnpoint, reinforcement_mins[summonStr - inc],
                reinforcement_maxs[summonStr - inc], 256, -1))
            {
                num_success++;

                if ((summonStr - inc) > 3)
                {
                    SpawnGrow_Spawn(spawnpoint, 1);         /* big monster */
                }
                else
                {
                    SpawnGrow_Spawn(spawnpoint, 0);         /* normal size */
                }
            }
        }
    }

    if (num_success == 0)
    {
        self->monsterinfo.nextframe = FRAME_attack53;
    }
}

void
medic_finish_spawn(edict_t* self)
{
    edict_t* ent;
    vec3_t f, r, offset, startpoint, spawnpoint;
    int summonStr;
    int count;
    int inc;
    int num_summoned; /* should be 1, 3, or 5 */
    edict_t* designated_enemy;

    if (!self)
    {
        return;
    }

    if (self->plat2flags < 0)
    {
        self->plat2flags *= -1;
    }

    summonStr = self->plat2flags;

    AngleVectors(self->s.angles, f, r, NULL);

    if (summonStr)
    {
        num_summoned = (summonStr - 1) + (summonStr % 2);
    }
    else
    {
        num_summoned = 1;
    }

    for (count = 0; count < num_summoned; count++)
    {
        inc = count + (count % 2); /* 0, 2, 2, 4, 4 */
        VectorCopy(reinforcement_position[count], offset);

        G_ProjectSource(self->s.origin, offset, f, r, startpoint);

        /* a little off the ground */
        startpoint[2] += 10;

        ent = NULL;

        if (FindSpawnPoint(startpoint, reinforcement_mins[summonStr - inc],
            reinforcement_maxs[summonStr - inc], spawnpoint, 32))
        {
            if (CheckSpawnPoint(spawnpoint, reinforcement_mins[summonStr - inc],
                reinforcement_maxs[summonStr - inc]))
            {
                ent = CreateGroundMonster(spawnpoint, self->s.angles,
                    reinforcement_mins[summonStr - inc],
                    reinforcement_maxs[summonStr - inc],
                    reinforcements[summonStr - inc], 256);
            }
        }

        if (!ent)
        {
            continue;
        }


        if (ent->think)
        {
            ent->nextthink = level.time;
            ent->think(ent);
        }

        ent->monsterinfo.aiflags |= AI_IGNORE_SHOTS | AI_DO_NOT_COUNT |
            AI_SPAWNED_MEDIC_C;
        ent->monsterinfo.commander = self;
        self->monsterinfo.monster_slots--;

        if (self->monsterinfo.aiflags & AI_MEDIC)
        {
            designated_enemy = self->oldenemy;
        }
        else
        {
            designated_enemy = self->enemy;
        }

        if (coop && coop->value)
        {
            designated_enemy = PickCoopTarget(ent);

            if (designated_enemy)
            {
                /* try to avoid using my enemy */
                if (designated_enemy == self->enemy)
                {
                    designated_enemy = PickCoopTarget(ent);

                    if (!designated_enemy)
                    {
                        designated_enemy = self->enemy;
                    }
                }
            }
            else
            {
                designated_enemy = self->enemy;
            }
        }

        if ((designated_enemy) && (designated_enemy->inuse) &&
            (designated_enemy->health > 0))
        {
            ent->enemy = designated_enemy;
            FoundTarget(ent);
        }
        else
        {
            ent->enemy = NULL;
            ent->monsterinfo.stand(ent);
        }
    }
}


mframe_t medic_frames_callReinforcements[] = {
    {ai_charge, 2, NULL},                     /* 33 */
    {ai_charge, 3, NULL},
    {ai_charge, 5, NULL},
    {ai_charge, 4.4, NULL},                   /* 36 */
    {ai_charge, 4.7, NULL},
    {ai_charge, 5, NULL},
    {ai_charge, 6, NULL},
    {ai_charge, 4, NULL},                     /* 40 */
    {ai_charge, 0, NULL},
    {ai_move, 0, medic_start_spawn},          /* 42 */
    {ai_move, 0, NULL},						  /* 43 -- 43 through 47 are skipped */
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, medic_determine_spawn},      /* 48 */
    {ai_charge, 0, medic_spawngrows},         /* 49 */
    {ai_move, 0, NULL},						  /* 50 */
    {ai_move, 0, NULL},						  /* 51 */
    {ai_move, -15, medic_finish_spawn},       /* 52 */
    {ai_move, -1.5, NULL},
    {ai_move, -1.2, NULL},
    {ai_move, -3, NULL},
    {ai_move, -2, NULL},
    {ai_move, 0.3, NULL},
    {ai_move, 0.7, NULL},
    {ai_move, 1.2, NULL},
    {ai_move, 1.3, NULL}                      /* 60 */
};

mmove_t medic_move_callReinforcements = {
    FRAME_attack33,
    FRAME_attack60,
    medic_frames_callReinforcements,
    medic_run
};

void medic_attack(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_MEDIC)
        self->monsterinfo.currentmove = &medic_move_attackCable;
    else
        self->monsterinfo.currentmove = &medic_move_attackBlaster;
}

qboolean medic_checkattack(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_MEDIC) {
        medic_attack(self);
        return qtrue;
    }

    return M_CheckAttack(self);
}

void
medic_duck(edict_t* self, float eta)
{
    if (!self)
    {
        return;
    }

    /*	don't dodge if you're healing */
    if (self->monsterinfo.aiflags & AI_MEDIC)
    {
        return;
    }

    if ((self->monsterinfo.currentmove == &medic_move_attackHyperBlaster) ||
        (self->monsterinfo.currentmove == &medic_move_attackCable) ||
        (self->monsterinfo.currentmove == &medic_move_attackBlaster) ||
        (self->monsterinfo.currentmove == &medic_move_callReinforcements))
    {
        /* he ignores skill */
        self->monsterinfo.aiflags &= ~AI_DUCKED;
        return;
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

    self->monsterinfo.nextframe = FRAME_duck1;
    self->monsterinfo.currentmove = &medic_move_duck;
    return;
}

void
medic_sidestep(edict_t* self)
{
    if (!self)
    {
        return;
    }

    if ((self->monsterinfo.currentmove == &medic_move_attackHyperBlaster) ||
        (self->monsterinfo.currentmove == &medic_move_attackCable) ||
        (self->monsterinfo.currentmove == &medic_move_attackBlaster) ||
        (self->monsterinfo.currentmove == &medic_move_callReinforcements))
    {
        /* if we're shooting, and not on easy, don't dodge */
        if (skill->value)
        {
            self->monsterinfo.aiflags &= ~AI_DODGING;
            return;
        }
    }

    if (self->monsterinfo.currentmove != &medic_move_run)
    {
        self->monsterinfo.currentmove = &medic_move_run;
    }
}

qboolean
medic_blocked(edict_t* self, float dist)
{
    if (!self)
    {
        return false;
    }

    if (blocked_checkshot(self, 0.25 + (0.05 * skill->value)))
    {
        return true;
    }

    if (blocked_checkplat(self, dist))
    {
        return true;
    }

    return false;
}

void
MedicCommanderCache(void)
{
    edict_t* newEnt;
    int i;

    /* better way to do this?  this is quick and dirty */
    for (i = 0; i < 7; i++)
    {
        newEnt = G_Spawn();

        VectorCopy(vec3_origin, newEnt->s.origin);
        VectorCopy(vec3_origin, newEnt->s.angles);
        newEnt->classname = ED_NewString(reinforcements[i]);

        newEnt->monsterinfo.aiflags |= AI_DO_NOT_COUNT;

        ED_CallSpawn(newEnt);
        G_FreeEdict(newEnt);
    }

    gi.modelindex("models/items/spawngro/tris.md2");
    gi.modelindex("models/items/spawngro2/tris.md2");
}



/*QUAKED monster_medic (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_medic(edict_t *self)
{
    if (!self)
    {
        return;
    }

    if (deathmatch->value)
    {
        G_FreeEdict(self);
        return;
    }

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/medic/tris.md2");
    VectorSet(self->mins, -24, -24, -24);
    VectorSet(self->maxs, 24, 24, 32);

    if (strcmp(self->classname, "monster_medic_commander") == 0)
    {
        self->health = 600;
        self->gib_health = -130;
        self->mass = 600;
        self->yaw_speed = 40;
        MedicCommanderCache();
    }
    else
    {
        self->health = 300;
        self->gib_health = -130;
        self->mass = 400;
    }

    self->pain = medic_pain;
    self->die = medic_die;

    self->monsterinfo.stand = medic_stand;
    self->monsterinfo.walk = medic_walk;
    self->monsterinfo.run = medic_run;
    self->monsterinfo.dodge = M_MonsterDodge;
    self->monsterinfo.duck = medic_duck;
    self->monsterinfo.unduck = monster_duck_up;
    self->monsterinfo.sidestep = medic_sidestep;
    self->monsterinfo.attack = medic_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = medic_sight;
    self->monsterinfo.idle = medic_idle;
    self->monsterinfo.search = medic_search;
    self->monsterinfo.checkattack = medic_checkattack;
    self->monsterinfo.blocked = medic_blocked;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &medic_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    walkmonster_start(self);

    self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

    if (self->mass > 400)
    {
        self->s.skinnum = 2;

        if (skill->value == 0)
        {
            self->monsterinfo.monster_slots = 3;
        }
        else if (skill->value == 1)
        {
            self->monsterinfo.monster_slots = 4;
        }
        else if (skill->value == 2)
        {
            self->monsterinfo.monster_slots = 6;
        }
        else if (skill->value == 3)
        {
            self->monsterinfo.monster_slots = 6;
        }

        /* commander sounds */
        commander_sound_idle1 = gi.soundindex("medic_commander/medidle.wav");
        commander_sound_pain1 = gi.soundindex("medic_commander/medpain1.wav");
        commander_sound_pain2 = gi.soundindex("medic_commander/medpain2.wav");
        commander_sound_die = gi.soundindex("medic_commander/meddeth.wav");
        commander_sound_sight = gi.soundindex("medic_commander/medsght.wav");
        commander_sound_search = gi.soundindex("medic_commander/medsrch.wav");
        commander_sound_hook_launch = gi.soundindex("medic_commander/medatck2c.wav");
        commander_sound_hook_hit = gi.soundindex("medic_commander/medatck3a.wav");
        commander_sound_hook_heal = gi.soundindex("medic_commander/medatck4a.wav");
        commander_sound_hook_retract = gi.soundindex("medic_commander/medatck5a.wav");
        commander_sound_spawn = gi.soundindex("medic_commander/monsterspawn1.wav");
        gi.soundindex("tank/tnkatck3.wav");
    }
    else
    {
        sound_idle1 = gi.soundindex("medic/idle.wav");
        sound_pain1 = gi.soundindex("medic/medpain1.wav");
        sound_pain2 = gi.soundindex("medic/medpain2.wav");
        sound_die = gi.soundindex("medic/meddeth1.wav");
        sound_sight = gi.soundindex("medic/medsght1.wav");
        sound_search = gi.soundindex("medic/medsrch1.wav");
        sound_hook_launch = gi.soundindex("medic/medatck2.wav");
        sound_hook_hit = gi.soundindex("medic/medatck3.wav");
        sound_hook_heal = gi.soundindex("medic/medatck4.wav");
        sound_hook_retract = gi.soundindex("medic/medatck5.wav");
        gi.soundindex("medic/medatck1.wav");

        self->s.skinnum = 0;
    }
}
