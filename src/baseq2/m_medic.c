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

#include "g_local.h"
#include "m_medic.h"

bool visible(edict_t *self, edict_t *other);


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

// ROGUE - the commander is the same model on skin 2 with its own voice, told
// apart by mass alone (600 vs 400) exactly as rogue and the rerelease do it
static int  commander_sound_idle1;
static int  commander_sound_pain1;
static int  commander_sound_pain2;
static int  commander_sound_die;
static int  commander_sound_sight;
static int  commander_sound_search;
static int  commander_sound_hook_launch;
static int  commander_sound_hook_hit;
static int  commander_sound_hook_heal;
static int  commander_sound_hook_retract;
static int  commander_sound_spawn;

#define MEDIC_IS_COMMANDER(self)    ((self)->mass > 400)

// [rerelease] healing distances and the retry clock.  MEDIC_MIN_DISTANCE stops
// a medic resurrecting a corpse it is standing on top of (the revived monster
// would spawn inside it); MEDIC_MAX_HEAL_DISTANCE is the search radius for a
// medic that is holding its ground rather than roaming.
#define MEDIC_MIN_DISTANCE          32.0f
#define MEDIC_MAX_HEAL_DISTANCE     400.0f
#define MEDIC_TRY_TIME              (10 * BASE_FRAMERATE)

// declared per-file in this tree, the same way g_turret.c declares FindTarget
bool FindTarget(edict_t *self);
void HuntTarget(edict_t *self);

// where the summoned monsters appear, relative to the commander's facing
static const vec3_t reinforcement_position[MAX_REINFORCEMENTS] = {
    {  80,   0, 0 },
    {  40,  60, 0 },
    {  40, -60, 0 },
    {   0,  80, 0 },
    {   0, -80, 0 }
};

// the rerelease's default list, used when a map does not set `reinforcements`.
// Only mgu5m1 and mgu5m2 leave it out, and both of those commanders are
// trigger-spawned ambushes where the vanilla mix is the right answer.
static const char *medic_default_reinforcements =
    "monster_soldier_light 1;monster_soldier 2;monster_soldier_ss 2;"
    "monster_infantry 3;monster_gunner 4;monster_medic 5;monster_gladiator 6";


/*
=================
cleanupHealTarget / cleanupHeal / abortHeal

[rerelease] The medic's GIVING-UP path, and the reason a medic that cannot
reach its patient used to stand there working its arm forever: this tree's
medic_cable_attack simply `return`s on every failure, so the whole 19-frame
cable animation plays, achieves nothing, and medic_run then re-picks the SAME
unreachable corpse - which nothing has marked - on the very next think.

abortHeal is the missing exit.  `mark` records this medic in the corpse's
badMedic1/2 so this medic never targets it again; `gib` destroys a patient the
medic is standing on top of rather than resurrecting it inside itself.
=================
*/
void cleanupHealTarget(edict_t *ent)
{
    ent->monsterinfo.healer = NULL;
    ent->takedamage = DAMAGE_YES;
    ent->monsterinfo.aiflags &= ~AI_RESURRECTING;
    M_SetEffects(ent);
}

static void cleanupHeal(edict_t *self, bool change_frame)
{
    // clean up target, if we have one and it's legit
    if (self->enemy && self->enemy->inuse)
        cleanupHealTarget(self->enemy);

    if (self->oldenemy && self->oldenemy->inuse && self->oldenemy->health > 0) {
        self->enemy = self->oldenemy;
        HuntTarget(self);
    } else {
        self->enemy = self->goalentity = NULL;
        self->oldenemy = NULL;
        if (!FindTarget(self)) {
            // no valid enemy, so stop acting
            self->monsterinfo.pause_framenum = INT_MAX;
            self->monsterinfo.stand(self);
            return;
        }
    }

    if (change_frame)
        self->monsterinfo.nextframe = FRAME_attack52;
}

void abortHeal(edict_t *self, bool change_frame, bool gib, bool mark)
{
    int             hurt;
    static const vec3_t pain_normal = { 0, 0, 1 };

    if (self->enemy)
        cleanupHealTarget(self->enemy);

    if (mark && self->enemy && self->enemy->inuse) {
        // if the first badMedic slot already holds a live medic, use the second
        if (self->enemy->monsterinfo.badMedic1 &&
            self->enemy->monsterinfo.badMedic1->inuse &&
            self->enemy->monsterinfo.badMedic1->classname &&
            !strncmp(self->enemy->monsterinfo.badMedic1->classname, "monster_medic", 13))
            self->enemy->monsterinfo.badMedic2 = self;
        else
            self->enemy->monsterinfo.badMedic1 = self;
    }

    if (gib && self->enemy && self->enemy->inuse) {
        if (self->enemy->gib_health)
            hurt = -self->enemy->gib_health;
        else
            hurt = 500;

        T_Damage(self->enemy, self, self, vec3_origin, self->enemy->s.origin,
                 (float *)pain_normal, hurt, 0, 0, MOD_UNKNOWN);
    }

    cleanupHeal(self, change_frame);

    self->monsterinfo.aiflags &= ~AI_MEDIC;
    self->monsterinfo.medicTries = 0;
}

edict_t *medic_FindDeadMonster(edict_t *self)
{
    edict_t *ent = NULL;
    edict_t *best = NULL;
    float   radius = 1024;

    // [rerelease] a medic holding its ground only looks as far as its cable
    // actually reaches, instead of walking off a ledge after a distant corpse
    if (M_RereleaseGame() && (self->monsterinfo.aiflags & AI_STAND_GROUND))
        radius = MEDIC_MAX_HEAL_DISTANCE;

    while ((ent = findradius(ent, self->s.origin, radius)) != NULL) {
        if (ent == self)
            continue;
        if (!(ent->svflags & SVF_MONSTER))
            continue;
        if (ent->monsterinfo.aiflags & AI_GOOD_GUY)
            continue;
        // [rerelease] the claim is monsterinfo.healer, not the overloaded
        // `owner`.  Ignore a stale claim from a healer that is dead, gone, or
        // no longer in medic mode - rogue's own comment admits this is
        // papering over a bug elsewhere, and it is still true here.
        if (ent->monsterinfo.healer) {
            edict_t *h = ent->monsterinfo.healer;
            if (h->inuse && h->health > 0 && (h->svflags & SVF_MONSTER) &&
                (h->monsterinfo.aiflags & AI_MEDIC))
                continue;
        }
        if (M_RereleaseGame()) {
            // check to make sure we haven't bailed on this guy already
            if (ent->monsterinfo.badMedic1 == self || ent->monsterinfo.badMedic2 == self)
                continue;
        }
        if (ent->health > 0)
            continue;
        if (ent->nextthink)
            continue;
        if (!visible(self, ent))
            continue;
        // [rerelease] don't resurrect someone right on top of us - the revived
        // monster would spawn inside the medic
        if (M_RereleaseGame() && realrange(self, ent) <= MEDIC_MIN_DISTANCE)
            continue;
        if (!best) {
            best = ent;
            continue;
        }
        if (ent->max_health <= best->max_health)
            continue;
        best = ent;
    }

    if (best)
        self->timestamp = level.framenum + MEDIC_TRY_TIME;

    return best;
}

void medic_idle(edict_t *self)
{
    edict_t *ent;

    gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_idle1 : sound_idle1, 1, ATTN_IDLE, 0);

    // [rerelease] id guards this with !oldenemy and SAVES the current enemy.
    // Rogue's version does neither, so an idling medic that spots a corpse
    // overwrites its live enemy with no way back to it.
    if (M_RereleaseGame() && self->oldenemy)
        return;

    ent = medic_FindDeadMonster(self);
    if (ent) {
        if (M_RereleaseGame())
            self->oldenemy = self->enemy;
        self->enemy = ent;
        self->enemy->monsterinfo.healer = self;
        self->monsterinfo.aiflags |= AI_MEDIC;
        FoundTarget(self);
    }
}

void medic_search(edict_t *self)
{
    edict_t *ent;

    gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_search : sound_search, 1, ATTN_IDLE, 0);

    if (!self->oldenemy) {
        ent = medic_FindDeadMonster(self);
        if (ent) {
            self->oldenemy = self->enemy;
            self->enemy = ent;
            self->enemy->monsterinfo.healer = self;
            self->monsterinfo.aiflags |= AI_MEDIC;
            FoundTarget(self);
        }
    }
}

void medic_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_sight : sound_sight, 1, ATTN_NORM, 0);
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
    { ai_walk, 18.1, monster_footstep },
    { ai_walk, 1,     NULL },
    { ai_walk, 9,     NULL },
    { ai_walk, 10,    NULL },
    { ai_walk, 9,     NULL },
    { ai_walk, 11,    NULL },
    { ai_walk, 11.6, monster_footstep },
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
    { ai_run, 22.5, monster_footstep },
    { ai_run, 25.4, monster_done_dodge },
    { ai_run, 23.4, monster_footstep },
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
            self->enemy->monsterinfo.healer = self;
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

/*
=================
the rerelease's retimed medic

id trimmed almost every medic animation at one or both ends: the flinches are
shorter, the crouch drops and stands sooner, and the blaster attack starts three
frames in.  The medic is the same monster with the dead frames cut out of it,
which is most of why the rerelease's feels so much sharper.

None of these need M_RereleaseAnims() - every range is a SUBSET of a range the
1997 model already has, so the classic md2 plays them all.  They sit alongside
the classic tables and are chosen at the point of use, the same way the infantry
keeps attack1 and attack1_classic.
=================
*/
mframe_t medic_frames_pain1_rr [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t medic_move_pain1_rr = {FRAME_paina2, FRAME_paina6, medic_frames_pain1_rr, medic_run};

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

mframe_t medic_frames_pain2_rr [] = {
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
mmove_t medic_move_pain2_rr = {FRAME_painb2, FRAME_painb13, medic_frames_pain2_rr, medic_run};

void medic_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    // the pain skin is the low bit: 0 -> 1 for the medic, 2 -> 3 for the
    // commander
    if (self->health < (self->max_health / 2))
        self->s.skinnum |= 1;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    if (random() < 0.5f) {
        if (M_RereleaseGame())
            self->monsterinfo.currentmove = &medic_move_pain1_rr;
        else
            self->monsterinfo.currentmove = &medic_move_pain1;
        gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_pain1 : sound_pain1, 1, ATTN_NORM, 0);
    } else {
        if (M_RereleaseGame())
            self->monsterinfo.currentmove = &medic_move_pain2_rr;
        else
            self->monsterinfo.currentmove = &medic_move_pain2;
        gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_pain2 : sound_pain2, 1, ATTN_NORM, 0);
    }
}

void medic_fire_blaster(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  end;
    vec3_t  dir;
    int     effect;
    int     flash;

    if ((self->s.frame == FRAME_attack9) || (self->s.frame == FRAME_attack12))
		if (self->monsterFireHyperBlaster && !MEDIC_IS_COMMANDER(self)) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else if ((self->s.frame == FRAME_attack19) || (self->s.frame == FRAME_attack22) || (self->s.frame == FRAME_attack25) || (self->s.frame == FRAME_attack28))
		if (self->monsterFireHyperBlaster && !MEDIC_IS_COMMANDER(self)) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else
        effect = 0;

    // [rerelease] The blaster and the hyperblaster come out of different
    // muzzles - the arm sits about 9 units further back for the burst - and the
    // burst is 12 of the medic's 14 shots, so firing it from the blaster offset
    // puts the flash out in front of the model.  The two blaster shots are the
    // ones on attack9 and attack12; everything else is the hyperblaster.
    if (M_RereleaseGame() && self->s.frame != FRAME_attack9
        && self->s.frame != FRAME_attack12)
        flash = MZ2_MEDIC_HYPERBLASTER;
    else
        flash = MEDIC_IS_COMMANDER(self) ? MZ2_MEDIC_BLASTER_2 : MZ2_MEDIC_BLASTER_1;

    AngleVectors(self->s.angles, forward, right, NULL);

    if (flash == MZ2_MEDIC_HYPERBLASTER) {
        // the barrel is turning, so the muzzle moves shot to shot
        int i = self->s.frame - FRAME_attack19;

        clamp(i, 0, MEDIC_HYPERBLASTER_SHOTS - 1);
        G_ProjectSource(self->s.origin, medic_hyperblaster_offset[i], forward, right, start);
    } else {
        G_ProjectSource(self->s.origin, monster_flash_offset[flash], forward, right, start);
    }

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);

    if (MEDIC_IS_COMMANDER(self)) {
        // ROGUE - the commander shoots the green blaster2 bolt, and hits for
        // 6 rather than 2
        monster_fire_blaster2(self, start, dir, 6, 1000, flash, effect);
    }
	else if (self->monsterFireHyperBlaster) {
		monster_fire_hyper_blaster(self, start, dir, 2, 1000, flash, effect);
	}
	else {
		monster_fire_blaster(self, start, dir, 2, 1000, flash, effect);
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

/*
=================
medic_shrink

[rerelease] Flattens the body partway through the death animation and marks it a
dead monster there, rather than only at the very end, so a medic dying in a
doorway stops blocking it while the rest of the animation plays.
=================
*/
static void medic_shrink(edict_t *self)
{
    self->maxs[2] = -2;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

mframe_t medic_frames_death_rr [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, -18, NULL },
    { ai_move, -10, medic_shrink },
    { ai_move, -6, NULL },
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
mmove_t medic_move_death_rr = {FRAME_death2, FRAME_death30, medic_frames_death_rr, medic_dead};

void medic_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

    // if we had a pending patient, free him up for another medic
    if ((self->enemy) && (self->enemy->monsterinfo.healer == self))
        cleanupHealTarget(self->enemy);

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
    gi.sound(self, CHAN_VOICE, MEDIC_IS_COMMANDER(self) ? commander_sound_die : sound_die, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

    if (M_RereleaseGame())
        self->monsterinfo.currentmove = &medic_move_death_rr;
    else
        self->monsterinfo.currentmove = &medic_move_death;
}





mframe_t medic_frames_duck [] = {
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    monster_duck_down },
    { ai_move, -1,    monster_duck_hold },
    { ai_move, -1,    NULL },
    { ai_move, -1,    NULL },
    { ai_move, -1,    monster_duck_up },
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

// [rerelease] the crouch drops one frame earlier and holds until the very last
// frame - id's own comment on the removed entry is "PMM - duck up used to be here"
mframe_t medic_frames_duck_rr [] = {
    { ai_move, -1, NULL },
    { ai_move, -1, monster_duck_down },
    { ai_move, -1, monster_duck_hold },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, NULL },
    { ai_move, -1, monster_duck_up }
};
mmove_t medic_move_duck_rr = {FRAME_duck2, FRAME_duck14, medic_frames_duck_rr, medic_run};

// Defined further down; the dodge pair tests against them.
extern mmove_t medic_move_attackBlaster;
extern mmove_t medic_move_attackBlaster_rr;
extern mmove_t medic_move_attackCable;
extern mmove_t medic_move_attackHyperBlaster;
extern mmove_t medic_move_attackHyperBlaster_rr;

/*
=================
medic_duck / medic_sidestep

The ROGUE/rerelease dodge pair. Returning a bool is what lets
M_MonsterDodge fall back from a sidestep to a duck. Neither interrupts a
firing sequence - a monster that ducked mid-burst threw the shot away.
=================
*/
static bool medic_is_attacking(edict_t *self)
{
    const mmove_t *move = self->monsterinfo.currentmove;

    return move == &medic_move_attackHyperBlaster
        || move == &medic_move_attackHyperBlaster_rr
        || move == &medic_move_attackCable
        || move == &medic_move_attackBlaster
        || move == &medic_move_attackBlaster_rr;
}

bool medic_duck(edict_t *self, float eta)
{
    if (medic_is_attacking(self)) {
        // already shooting - stand back up rather than half-duck
        monster_duck_up(self);
        return false;
    }

    if (M_RereleaseGame())
        self->monsterinfo.currentmove = &medic_move_duck_rr;
    else
        self->monsterinfo.currentmove = &medic_move_duck;
    return true;
}

bool medic_sidestep(edict_t *self)
{
    if (medic_is_attacking(self))
        return false;

    // strafing happens on the run move; AS_SLIDING is what makes
    // ai_run sidestep rather than close
    if (self->monsterinfo.currentmove != &medic_move_run)
        self->monsterinfo.currentmove = &medic_move_run;

    return true;
}

/*
=================
medic_dodge

monsterinfo.dodge for every medic, in both games.  The rerelease hands over to
M_MonsterDodge and its duck + sidestep pair; the ORIGINAL game gets id's 1997
dodge back verbatim - a flat 25% chance of a plain crouch.

(This symbol also has to keep existing: g_ptrs_compat_v2.c is a frozen table
for version-2 saves and names it.)
=================
*/
void medic_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    if (M_RereleaseGame()) {
        M_MonsterDodge(self, attacker, eta, tr, gravity);
        return;
    }

    if (random() > 0.25f)
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

// [rerelease] the same twelve shots, but the animation runs four frames further
// so it settles out of the burst instead of cutting off - id's comment on the
// tail is "end on 36 as intended"
mframe_t medic_frames_attackHyperBlaster_rr [] = {
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
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 2,   NULL },
    { ai_charge, 3,   NULL }
};
mmove_t medic_move_attackHyperBlaster_rr = {FRAME_attack15, FRAME_attack34, medic_frames_attackHyperBlaster_rr, medic_run};

/*
=================
medic_quick_attack

[rerelease] Half the time the medic skips the rest of the blaster wind-up and
snaps straight into the middle of the hyperblaster burst.  This is the single
biggest reason the rerelease medic feels aggressive: the classic one always
plays eight lead-in frames before its first shot.
=================
*/
static void medic_quick_attack(edict_t *self)
{
    if (random() < 0.5f) {
        self->monsterinfo.currentmove = &medic_move_attackHyperBlaster_rr;
        self->monsterinfo.nextframe = FRAME_attack16;
    }
}


void medic_continue(edict_t *self)
{
    if (visible(self, self->enemy) && random() <= 0.95f) {
        if (M_RereleaseGame())
            self->monsterinfo.currentmove = &medic_move_attackHyperBlaster_rr;
        else
            self->monsterinfo.currentmove = &medic_move_attackHyperBlaster;
    }
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

// [rerelease] two fewer lead-in frames, and the first shot lands on the seventh
// entry rather than the ninth
mframe_t medic_frames_attackBlaster_rr [] = {
    { ai_charge, 5,   NULL },
    { ai_charge, 3,   NULL },
    { ai_charge, 2,   NULL },
    { ai_charge, 0,   medic_quick_attack },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_fire_blaster },
    { ai_charge, 0,   NULL },
    { ai_charge, 0,   medic_continue }
};
mmove_t medic_move_attackBlaster_rr = {FRAME_attack3, FRAME_attack14, medic_frames_attackBlaster_rr, medic_run};


void medic_hook_launch(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, MEDIC_IS_COMMANDER(self) ? commander_sound_hook_launch : sound_hook_launch, 1, ATTN_NORM, 0);
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

    // [rerelease] EVERY failure below used to be a bare `return`, so a medic
    // whose patient it cannot reach played all 19 cable frames, healed nobody,
    // and medic_run then re-picked the same corpse - forever, because nothing
    // marked it.  That is the medic stuck in a corner working its arm.  id
    // aborts instead, and gives up for good on the second try.
    if (!self->enemy->inuse) {
        if (M_RereleaseGame())
            abortHeal(self, false, false, false);
        return;
    }

    AngleVectors(self->s.angles, f, r, NULL);
    VectorCopy(medic_cable_offsets[self->s.frame - FRAME_attack42], offset);
    G_ProjectSource(self->s.origin, offset, f, r, start);

    // check for max distance
    VectorSubtract(start, self->enemy->s.origin, dir);
    distance = VectorLength(dir);
    if (distance > 256) {
        if (M_RereleaseGame())
            abortHeal(self, false, false, false);
        return;
    }

    // [rerelease] too close to resurrect - the patient would come back inside
    // the medic, so gib it instead
    if (M_RereleaseGame() && distance < MEDIC_MIN_DISTANCE) {
        abortHeal(self, true, true, false);
        return;
    }

    // check for min/max pitch
    vectoangles(dir, angles);
    if (angles[0] < -180)
        angles[0] += 360;
    if (fabsf(angles[0]) > 45) {
        if (M_RereleaseGame())
            abortHeal(self, true, false, false);
        return;
    }

    tr = gi.trace(start, NULL, NULL, self->enemy->s.origin, self, MASK_SHOT);
    if (tr.fraction != 1.0f && tr.ent != self->enemy) {
        if (M_RereleaseGame()) {
            if (tr.ent == g_edicts) {
                // blocked by level geometry: retreat and re-approach once,
                // then write this medic off the patient for good
                if (self->monsterinfo.medicTries > 1) {
                    abortHeal(self, true, false, true);
                    return;
                }
                self->monsterinfo.medicTries++;
                cleanupHeal(self, true);
                return;
            }
            abortHeal(self, true, false, false);
        }
        return;
    }

    if (self->s.frame == FRAME_attack43) {
        gi.sound(self->enemy, CHAN_AUTO, MEDIC_IS_COMMANDER(self) ? commander_sound_hook_hit : sound_hook_hit, 1, ATTN_NORM, 0);
        self->enemy->monsterinfo.aiflags |= AI_RESURRECTING;
    } else if (self->s.frame == FRAME_attack50) {
        reinforcement_t saved_reinf[MAX_REINFORCEMENT_TYPES];
        int     saved_num_reinf = 0, saved_slots = 0, saved_used = 0;
        int     saved_gib_health = 0;

        self->enemy->spawnflags = 0;
        // [rerelease] id keeps the SPAWNED flags across a resurrection.  Zeroing
        // aiflags outright loses the marker that says a commander summoned this
        // monster, so a revived escort starts counting toward the level total.
        if (M_RereleaseGame())
            self->enemy->monsterinfo.aiflags &= AI_SPAWNED_MASK;
        else
            self->enemy->monsterinfo.aiflags = 0;
        self->enemy->target = NULL;
        self->enemy->targetname = NULL;
        self->enemy->combattarget = NULL;
        self->enemy->deathtarget = NULL;

        if (M_RereleaseGame()) {
            // ED_CallSpawn re-reads the SPAWN TEMP, and `st` is only cleared
            // while the map is being parsed - mid-game it still holds whatever
            // the last parsed entity left behind.  Resurrecting a medic
            // commander would therefore re-parse a stale `reinforcements`
            // string, so clear it and put the monster's real summon list back
            // afterwards.  Same reasoning as id's `st = {}` here.
            memcpy(saved_reinf, self->enemy->monsterinfo.reinforcements, sizeof(saved_reinf));
            saved_num_reinf   = self->enemy->monsterinfo.num_reinforcements;
            saved_slots       = self->enemy->monsterinfo.monster_slots;
            saved_used        = self->enemy->monsterinfo.monster_used;
            saved_gib_health  = self->enemy->gib_health;

            memset(&st, 0, sizeof(st));
        }

        self->enemy->owner = self;
        ED_CallSpawn(self->enemy);
        self->enemy->owner = NULL;

        if (M_RereleaseGame()) {
            memcpy(self->enemy->monsterinfo.reinforcements, saved_reinf, sizeof(saved_reinf));
            self->enemy->monsterinfo.num_reinforcements = saved_num_reinf;
            self->enemy->monsterinfo.monster_slots      = saved_slots;
            self->enemy->monsterinfo.monster_used       = saved_used;

            // a body that has already been killed once gibs twice as easily
            self->enemy->gib_health = saved_gib_health / 2;

            // and must not be counted as a fresh kill
            self->enemy->monsterinfo.aiflags |= AI_DO_NOT_COUNT;
        }
        if (self->enemy->think) {
            self->enemy->nextthink = level.framenum;
            self->enemy->think(self->enemy);
        }
        self->enemy->monsterinfo.aiflags |= AI_RESURRECTING;

        // [rerelease] the patient is a live monster again, so release every
        // trace of the heal: the claim, the give-up marks (a medic that once
        // bailed on the CORPSE may legitimately heal this new body later),
        // this medic's try counter, and the corpse flies.
        if (M_RereleaseGame()) {
            self->enemy->monsterinfo.healer = NULL;
            self->enemy->monsterinfo.badMedic1 = NULL;
            self->enemy->monsterinfo.badMedic2 = NULL;
            self->enemy->s.effects &= ~EF_FLIES;
            self->monsterinfo.medicTries = 0;
        }

        if (self->oldenemy && self->oldenemy->client) {
            self->enemy->enemy = self->oldenemy;
            FoundTarget(self->enemy);
        }
    } else {
        if (self->s.frame == FRAME_attack44)
            gi.sound(self, CHAN_WEAPON, MEDIC_IS_COMMANDER(self) ? commander_sound_hook_heal : sound_hook_heal, 1, ATTN_NORM, 0);
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
    gi.sound(self, CHAN_WEAPON, MEDIC_IS_COMMANDER(self) ? commander_sound_hook_retract : sound_hook_retract, 1, ATTN_NORM, 0);
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




/*
==============================================================================

MEDIC COMMANDER - CALLING REINFORCEMENTS                            (rogue/KEX)

The summon runs across one animation, FRAME_attack33..55, in four steps:

  42  medic_start_spawn     - the call goes out, then skip ahead to 48
  48  medic_determine_spawn - pick the monsters and find them somewhere to
                              stand; turn around if the only room is behind
  49  medic_spawngrows      - play the spawn shell over each spot
  52  medic_finish_spawn    - actually build them

Steps 48/49/52 each re-run FindSpawnPoint, because the commander may have
turned between them and a player can walk into the spot in the meantime.

==============================================================================
*/

void medic_start_spawn(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, commander_sound_spawn, 1, ATTN_NORM, 0);
    self->monsterinfo.nextframe = FRAME_attack48;
}

void medic_determine_spawn(edict_t *self)
{
    vec3_t  f, r, offset, startpoint, spawnpoint;
    int     count, num_summoned;
    int     num_success = 0;
    reinforcement_t *re;

    AngleVectors(self->s.angles, f, r, NULL);

    num_summoned = M_PickReinforcements(self, 0);

    for (count = 0; count < num_summoned; count++) {
        VectorCopy(reinforcement_position[count], offset);
        G_ProjectSource(self->s.origin, offset, f, r, startpoint);
        startpoint[2] += 10;    // a little off the ground

        re = &self->monsterinfo.reinforcements[self->monsterinfo.chosen_reinforcements[count]];

        if (FindSpawnPoint(startpoint, re->mins, re->maxs, spawnpoint, 32)) {
            if (CheckGroundSpawnPoint(spawnpoint, re->mins, re->maxs, 256, -1)) {
                num_success++;
                break;          // found a spot, that is enough to commit
            }
        }
    }

    // nothing in front - see whether spinning round helps
    if (num_success == 0) {
        for (count = 0; count < num_summoned; count++) {
            VectorCopy(reinforcement_position[count], offset);
            offset[0] *= -1.0f;
            offset[1] *= -1.0f;
            G_ProjectSource(self->s.origin, offset, f, r, startpoint);
            startpoint[2] += 10;

            re = &self->monsterinfo.reinforcements[self->monsterinfo.chosen_reinforcements[count]];

            if (FindSpawnPoint(startpoint, re->mins, re->maxs, spawnpoint, 32)) {
                if (CheckGroundSpawnPoint(spawnpoint, re->mins, re->maxs, 256, -1)) {
                    num_success++;
                    break;
                }
            }
        }

        if (num_success) {
            self->monsterinfo.aiflags |= AI_MANUAL_STEERING;
            self->ideal_yaw = anglemod(self->s.angles[YAW]) + 180;
            if (self->ideal_yaw > 360.0f)
                self->ideal_yaw -= 360.0f;
        }
    }

    if (num_success == 0)
        self->monsterinfo.nextframe = FRAME_attack53;
}

void medic_spawngrows(edict_t *self)
{
    vec3_t  f, r, offset, startpoint, spawnpoint;
    int     count, num_summoned;
    int     num_success = 0;
    float   current_yaw;
    reinforcement_t *re;

    // if we have been directed to turn around, hold here until we have
    if (self->monsterinfo.aiflags & AI_MANUAL_STEERING) {
        current_yaw = anglemod(self->s.angles[YAW]);
        if (fabsf(current_yaw - self->ideal_yaw) > 0.1f) {
            self->monsterinfo.aiflags |= AI_HOLD_FRAME;
            return;
        }
        self->monsterinfo.aiflags &= ~(AI_HOLD_FRAME | AI_MANUAL_STEERING);
    }

    AngleVectors(self->s.angles, f, r, NULL);

    num_summoned = self->monsterinfo.num_chosen_reinforcements;

    for (count = 0; count < num_summoned; count++) {
        VectorCopy(reinforcement_position[count], offset);
        G_ProjectSource(self->s.origin, offset, f, r, startpoint);
        startpoint[2] += 10;

        re = &self->monsterinfo.reinforcements[self->monsterinfo.chosen_reinforcements[count]];

        if (FindSpawnPoint(startpoint, re->mins, re->maxs, spawnpoint, 32)) {
            if (CheckGroundSpawnPoint(spawnpoint, re->mins, re->maxs, 256, -1)) {
                num_success++;
                // anything with a body taller than a soldier gets the big shell
                SpawnGrow_Spawn(spawnpoint, (re->maxs[2] - re->mins[2]) > 60 ? 2 : 1);
            }
        }
    }

    if (num_success == 0)
        self->monsterinfo.nextframe = FRAME_attack53;
}

void medic_finish_spawn(edict_t *self)
{
    edict_t *ent;
    vec3_t  f, r, offset, startpoint, spawnpoint;
    int     count, num_summoned;
    edict_t *designated_enemy;
    reinforcement_t *re;

    AngleVectors(self->s.angles, f, r, NULL);

    num_summoned = self->monsterinfo.num_chosen_reinforcements;

    for (count = 0; count < num_summoned; count++) {
        re = &self->monsterinfo.reinforcements[self->monsterinfo.chosen_reinforcements[count]];

        VectorCopy(reinforcement_position[count], offset);
        G_ProjectSource(self->s.origin, offset, f, r, startpoint);
        startpoint[2] += 10;

        ent = NULL;
        if (FindSpawnPoint(startpoint, re->mins, re->maxs, spawnpoint, 32)) {
            if (CheckSpawnPoint(spawnpoint, re->mins, re->maxs))
                ent = CreateGroundMonster(spawnpoint, self->s.angles, re->mins, re->maxs, re->classname, 256);
        }

        if (!ent)
            continue;

        // run its first think now so it is on its feet this frame
        if (ent->think) {
            ent->nextthink = level.framenum;
            ent->think(ent);
        }

        ent->monsterinfo.aiflags |= AI_IGNORE_SHOTS | AI_DO_NOT_COUNT | AI_SPAWNED_MEDIC_C;
        ent->monsterinfo.commander = self;
        self->monsterinfo.monster_used += re->strength;

        if (self->monsterinfo.aiflags & AI_MEDIC)
            designated_enemy = self->oldenemy;
        else
            designated_enemy = self->enemy;

        if (coop->value) {
            // spread the escort across the coop players rather than dogpiling
            designated_enemy = PickCoopTarget(ent);
            if (designated_enemy) {
                if (designated_enemy == self->enemy) {
                    designated_enemy = PickCoopTarget(ent);
                    if (!designated_enemy)
                        designated_enemy = self->enemy;
                }
            } else {
                designated_enemy = self->enemy;
            }
        }

        if (designated_enemy && designated_enemy->inuse && designated_enemy->health > 0) {
            ent->enemy = designated_enemy;
            FoundTarget(ent);
        } else {
            ent->enemy = NULL;
            ent->monsterinfo.stand(ent);
        }
    }
}

mframe_t medic_frames_callReinforcements [] = {
    // 33-36 are ai_charge here, unlike the cable attack
    { ai_charge, 2,     NULL },                  // 33
    { ai_charge, 3,     NULL },
    { ai_charge, 5,     NULL },
    { ai_charge, 4.4f,  NULL },                  // 36
    { ai_charge, 4.7f,  NULL },
    { ai_charge, 5,     NULL },
    { ai_charge, 6,     NULL },
    { ai_charge, 4,     NULL },                  // 40
    { ai_charge, 0, monster_footstep },
    { ai_move,   0,     medic_start_spawn },     // 42
    { ai_move,   0,     NULL },                  // 43 - 43..47 are skipped
    { ai_move,   0,     NULL },
    { ai_move,   0,     NULL },
    { ai_move,   0,     NULL },
    { ai_move,   0,     NULL },
    { ai_move,   0,     medic_determine_spawn }, // 48
    { ai_charge, 0,     medic_spawngrows },      // 49
    { ai_move,   0,     NULL },                  // 50
    { ai_move,   0,     NULL },                  // 51
    { ai_move,   -15,   medic_finish_spawn },    // 52
    { ai_move,   -1.5f, NULL },
    { ai_move,   -1.2f, NULL },
    { ai_move, -3, monster_footstep }
};
mmove_t medic_move_callReinforcements = {FRAME_attack33, FRAME_attack55, medic_frames_callReinforcements, medic_run};

void medic_attack(edict_t *self)
{
    float r = random();

    if (self->monsterinfo.aiflags & AI_MEDIC) {
        // a commander with slots left would rather summon than heal
        if (MEDIC_IS_COMMANDER(self) && r > 0.8f && M_SlotsLeft(self) > 0)
            self->monsterinfo.currentmove = &medic_move_callReinforcements;
        else
            self->monsterinfo.currentmove = &medic_move_attackCable;
    } else {
        if (MEDIC_IS_COMMANDER(self) && r > 0.2f && M_SlotsLeft(self) > 0 &&
            range(self, self->enemy) > RANGE_MELEE)
            self->monsterinfo.currentmove = &medic_move_callReinforcements;
        else if (M_RereleaseGame())
            self->monsterinfo.currentmove = &medic_move_attackBlaster_rr;
        else
            self->monsterinfo.currentmove = &medic_move_attackBlaster;
    }
}

bool medic_checkattack(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_MEDIC) {
        // [rerelease] THE TIMEOUT.  Rogue's version below attacks
        // unconditionally - no distance test and no clock - so a medic locked
        // onto a corpse it can never reach plays the cable animation forever,
        // from any range, and nothing ever breaks the lock.  id gives up after
        // MEDIC_TRY_TIME and marks the patient so this medic drops it for good.
        if (M_RereleaseGame()) {
            // if our target went away
            if (!self->enemy || !self->enemy->inuse) {
                abortHeal(self, true, false, false);
                return false;
            }

            // if we ran out of time, give up
            if (self->timestamp < level.framenum) {
                abortHeal(self, true, false, true);
                self->timestamp = 0;
                return false;
            }

            // out of cable range - keep closing rather than flailing
            if (realrange(self, self->enemy) >= MEDIC_MAX_HEAL_DISTANCE + 10) {
                self->monsterinfo.attack_state = AS_STRAIGHT;
                return false;
            }
        }

        medic_attack(self);
        return true;
    }

    return M_CheckAttack(self);
}


/*QUAKED monster_medic (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
/*QUAKED monster_medic_commander (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
The medic commander summons reinforcements. The map's `reinforcements` key sets
what it can call and how much of its slot budget each one costs.
*/
void SP_monster_medic(edict_t *self)
{
    bool    commander;

    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    // ROGUE - like the daedalus, the commander is told apart by mass from here
    // on, so this has to be settled before anything reads MEDIC_IS_COMMANDER
    commander = !strcmp(self->classname, "monster_medic_commander");

	float val = crandom();

	if (val < 0) {
		self->monsterFireHyperBlaster = qtrue;
	}
	else {
		self->monsterFireHyperBlaster = qfalse;
	}

    if (commander) {
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

        gi.soundindex("medic_commander/medatck1a.wav");
    } else {
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
    }

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/medic/tris.md2");
    VectorSet(self->mins, -24, -24, -24);
    VectorSet(self->maxs, 24, 24, 32);

    self->health = 300;
    self->gib_health = -130;
    self->mass = 400;

    if (commander) {
        self->health = 600;
        self->gib_health = -130;
        self->mass = 600;
        self->yaw_speed = 40;
    }

    // the rerelease scales a monster's health per difficulty with this key;
    // mgu3m3 and mgu4m3 both use it to soften their commanders on hard
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);

    self->pain = medic_pain;
    self->die = medic_die;

    self->monsterinfo.stand = medic_stand;
    self->monsterinfo.walk = medic_walk;
    self->monsterinfo.run = medic_run;
    // medic_dodge is the classic dodge in baseq2 and forwards to M_MonsterDodge
    // in the rerelease.  duck/sidestep are what M_MonsterDodge drives, so the
    // original game must not advertise them at all.
    self->monsterinfo.dodge = medic_dodge;
    if (M_RereleaseGame()) {
        self->monsterinfo.duck = medic_duck;
        self->monsterinfo.unduck = monster_duck_up;
        self->monsterinfo.sidestep = medic_sidestep;
    }
    self->monsterinfo.attack = medic_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = medic_sight;
    self->monsterinfo.idle = medic_idle;
    self->monsterinfo.search = medic_search;
    self->monsterinfo.checkattack = medic_checkattack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &medic_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    walkmonster_start(self);

    if (commander) {
        // walkmonster_start clears skinnum, so this has to come after it
        self->s.skinnum = 2;

        // the commander ignores incoming fire while it is working
        self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

        // how much it can summon, before the per-monster strength costs
        switch ((int)skill->value) {
        case 0:  self->monsterinfo.monster_slots = 3; break;
        case 1:  self->monsterinfo.monster_slots = 4; break;
        default: self->monsterinfo.monster_slots = 6; break;
        }

        // power_armor_type/power_armor_power are real spawn fields, so mgu4m3's
        // 250-300 point screens are already in place by the time we get here.
        // Rogue gives the commander none by default, so nothing to set.

        // parsing the list also precaches every monster in it - a summon
        // mid-level has no safe way to precache
        M_SetupReinforcements(self, st.reinforcements ? st.reinforcements : medic_default_reinforcements);

        gi.modelindex("models/items/spawngro/tris.md2");
        gi.modelindex("models/items/spawngro2/tris.md2");
    } else {
        self->s.skinnum = 0;
    }
}
