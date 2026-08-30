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

SOLDIER

==============================================================================
*/

#include "g_local.h"
#include "m_soldier.h"


static int  sound_idle;
static int  sound_sight1;
static int  sound_sight2;
static int  sound_pain_light;
static int  sound_pain;
static int  sound_pain_ss;
static int  sound_death_light;
static int  sound_death;
static int  sound_death_ss;
static int  sound_cock;

extern mmove_t soldier_move_trip;

/*
=================
Mapping this file onto src/rerelease/m_soldier.cpp

  their `self->count`      ours is `self->s.skinnum`.  They set count from the
                           skin at spawn (count = skinnum for the stock family,
                           skinnum - 6 for soldierh); this tree gives soldierh
                           skins 0/2/4 and marks the family with style == 1, so
                           skinnum is already on their count's scale.
                           <=1 blaster/ripper, 2-3 shotgun/hypergun,
                           >=4 machinegun/lasergun.
  their `s.skinnum >= 6`   ours is `self->style == 1` (the soldierh family).
  their `range_to()`       ours is `realrange()` - both are box distance - and
                           their RANGE_* are DISTANCES in units where this
                           tree's are enum tags for range().  Hence the two
                           SOLDIER_RANGE_* below, matching their g_local.h:2213.
  their `radius_dmg`       the force-a-refire flag.  Free here: this tree's
                           soldierh_laserbeam takes its flash index as an
                           argument instead of parking it in radius_dmg, which
                           is what their soldierh_laser_update does.
  their `self->dmg`        set by the shotgun shot, cleared by soldier_cock:
                           "this soldier still has a spent shell in the pipe".
                           soldier_duck already READ it in this tree; nothing
                           ever set it, so that branch was dead.
=================
*/
#define SOLDIER_RANGE_MELEE 20.0f
#define SOLDIER_RANGE_NEAR  440.0f

void soldier_idle(edict_t *self)
{
    if (random() > 0.8f)
        gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

void soldier_cock(edict_t *self)
{
    if (self->s.frame == FRAME_stand322)
        gi.sound(self, CHAN_WEAPON, sound_cock, 1, ATTN_IDLE, 0);
    else
        gi.sound(self, CHAN_WEAPON, sound_cock, 1, ATTN_NORM, 0);

    // the shell is out, so the shotgun checks stop forcing a refire
    self->dmg = 0;
}

// AI_CHARGING marks a monster closing on its enemy rather than holding
// position.  The run-and-shoot sets it on its first frame and both of its
// refire checks clear it again.
void soldier_start_charge(edict_t *self)
{
    self->monsterinfo.aiflags |= AI_CHARGING;
}

void soldier_stop_charge(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_CHARGING;
}


// STAND

void soldier_stand(edict_t *self);

/*
=================
soldier_death_shrink

[rerelease] Flatten the corpse partway through the death animation, and mark it
a dead monster there, instead of waiting for the animation to finish. A body
that falls in a doorway stops blocking it while the rest of the death plays.

Gated: this sits in a death table BOTH games play, and the original game keeps
its full-height corpse until the dead-frame handler runs.
=================
*/
static void soldier_death_shrink(edict_t *self)
{
    if (!M_RereleaseGame())
        return;

    self->maxs[2] = 0;
    self->svflags |= SVF_DEADMONSTER;
    gi.linkentity(self);
}

/*
=================
soldier_done_dodge_footstep

Their run table pairs these in a C++ lambda; an mframe_t here holds one think.
=================
*/
static void soldier_done_dodge_footstep(edict_t *self)
{
    monster_done_dodge(self);
    monster_footstep(self);
}

mframe_t soldier_frames_stand1 [] = {
    { ai_stand, 0, soldier_idle },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

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
mmove_t soldier_move_stand1 = {FRAME_stand101, FRAME_stand130, soldier_frames_stand1, soldier_stand};

mframe_t soldier_frames_stand3 [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, soldier_cock },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
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
mmove_t soldier_move_stand3 = {FRAME_stand301, FRAME_stand339, soldier_frames_stand3, soldier_stand};

// The rerelease's second idle, on the APPENDED stand201-240 frames.  The
// classic tris.md2 stops at 475, so this move is only reachable behind
// M_RereleaseAnims().  (Its monster_footstep calls have no equivalent here.)
mframe_t soldier_frames_stand2 [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, monster_footstep },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, monster_footstep },
};
mmove_t soldier_move_stand2 = {FRAME_stand201, FRAME_stand240, soldier_frames_stand2, soldier_stand};

#if 0
mframe_t soldier_frames_stand4 [] = {
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL },
    { ai_stand, 4, NULL },
    { ai_stand, 1, NULL },
    { ai_stand, -1, NULL },
    { ai_stand, -2, NULL },

    { ai_stand, 0, NULL },
    { ai_stand, 0, NULL }
};
mmove_t soldier_move_stand4 = {FRAME_stand401, FRAME_stand452, soldier_frames_stand4, NULL};
#endif

void soldier_stand(edict_t *self)
{
    float r = random();

    if (M_RereleaseAnims()) {
        // three idles instead of two, and the soldier only ever leaves stand1
        // for one of the others - stand1 is where it always comes back to
        if (self->monsterinfo.currentmove != &soldier_move_stand1 || r < 0.6f)
            self->monsterinfo.currentmove = &soldier_move_stand1;
        else if (r < 0.8f)
            self->monsterinfo.currentmove = &soldier_move_stand2;
        else
            self->monsterinfo.currentmove = &soldier_move_stand3;
        return;
    }

    if ((self->monsterinfo.currentmove == &soldier_move_stand3) || (r < 0.8f))
        self->monsterinfo.currentmove = &soldier_move_stand1;
    else
        self->monsterinfo.currentmove = &soldier_move_stand3;
}


//
// WALK
//

void soldier_walk1_random(edict_t *self)
{
    if (random() > 0.1f)
        self->monsterinfo.nextframe = FRAME_walk101;
}

mframe_t soldier_frames_walk1 [] = {
    { ai_walk, 3,  NULL },
    { ai_walk, 6,  NULL },
    { ai_walk, 2,  NULL },
    { ai_walk, 2, monster_footstep },
    { ai_walk, 2,  NULL },
    { ai_walk, 1,  NULL },
    { ai_walk, 6,  NULL },
    { ai_walk, 5,  NULL },
    { ai_walk, 3, monster_footstep },
    { ai_walk, -1, soldier_walk1_random },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL },
    { ai_walk, 0,  NULL }
};
mmove_t soldier_move_walk1 = {FRAME_walk101, FRAME_walk133, soldier_frames_walk1, NULL};

mframe_t soldier_frames_walk2 [] = {
    { ai_walk, 4, monster_footstep },
    { ai_walk, 4,  NULL },
    { ai_walk, 9,  NULL },
    { ai_walk, 8,  NULL },
    { ai_walk, 5,  NULL },
    { ai_walk, 1, monster_footstep },
    { ai_walk, 3,  NULL },
    { ai_walk, 7,  NULL },
    { ai_walk, 6,  NULL },
    { ai_walk, 7,  NULL }
};
mmove_t soldier_move_walk2 = {FRAME_walk209, FRAME_walk218, soldier_frames_walk2, NULL};

void soldier_walk(edict_t *self)
{
    if (random() < 0.5f)
        self->monsterinfo.currentmove = &soldier_move_walk1;
    else
        self->monsterinfo.currentmove = &soldier_move_walk2;
}


//
// RUN
//

void soldier_run(edict_t *self);

mframe_t soldier_frames_start_run [] = {
    { ai_run, 7,  NULL },
    { ai_run, 5,  NULL }
};
mmove_t soldier_move_start_run = {FRAME_run01, FRAME_run02, soldier_frames_start_run, soldier_run};

mframe_t soldier_frames_run [] = {
    { ai_run, 10, NULL },
    { ai_run, 11, soldier_done_dodge_footstep },
    { ai_run, 11, NULL },
    { ai_run, 16, NULL },
    { ai_run, 10, monster_footstep },
    { ai_run, 15, monster_done_dodge }
};
mmove_t soldier_move_run = {FRAME_run03, FRAME_run08, soldier_frames_run, NULL};

void soldier_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND) {
        self->monsterinfo.currentmove = &soldier_move_stand1;
        return;
    }

    if (self->monsterinfo.currentmove == &soldier_move_walk1 ||
        self->monsterinfo.currentmove == &soldier_move_walk2 ||
        self->monsterinfo.currentmove == &soldier_move_start_run) {
        self->monsterinfo.currentmove = &soldier_move_run;
    } else {
        self->monsterinfo.currentmove = &soldier_move_start_run;
    }
}


//
// PAIN
//

mframe_t soldier_frames_pain1 [] = {
    { ai_move, -3, NULL },
    { ai_move, 4,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 0,  NULL }
};
mmove_t soldier_move_pain1 = {FRAME_pain101, FRAME_pain105, soldier_frames_pain1, soldier_run};

mframe_t soldier_frames_pain2 [] = {
    { ai_move, -13, NULL },
    { ai_move, -1,  NULL },
    { ai_move, 2,   NULL },
    { ai_move, 4,   NULL },
    { ai_move, 2,   NULL },
    { ai_move, 3,   NULL },
    { ai_move, 2,   NULL }
};
mmove_t soldier_move_pain2 = {FRAME_pain201, FRAME_pain207, soldier_frames_pain2, soldier_run};

mframe_t soldier_frames_pain3 [] = {
    { ai_move, -8, NULL },
    { ai_move, 10, NULL },
    { ai_move, -4, monster_footstep },
    { ai_move, -1, NULL },
    { ai_move, -3, NULL },
    { ai_move, 0,  NULL },
    { ai_move, 3,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 0,  NULL },
    { ai_move, 1,  NULL },
    { ai_move, 2,  NULL },
    { ai_move, 4,  NULL },
    { ai_move, 3,  NULL },
    { ai_move, 2, monster_footstep }
};
mmove_t soldier_move_pain3 = {FRAME_pain301, FRAME_pain318, soldier_frames_pain3, soldier_run};

mframe_t soldier_frames_pain4 [] = {
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, -10, NULL },
    { ai_move, -6,  NULL },
    { ai_move, 8,   NULL },
    { ai_move, 4,   NULL },
    { ai_move, 1,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 2,   NULL },
    { ai_move, 5,   NULL },
    { ai_move, 2,   NULL },
    { ai_move, -1,  NULL },
    { ai_move, -1,  NULL },
    { ai_move, 3,   NULL },
    { ai_move, 2,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_pain4 = {FRAME_pain401, FRAME_pain417, soldier_frames_pain4, soldier_run};


void soldier_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    float   r;
    int     n;

    // pain replaces currentmove, so the burst never reaches its sound_end frame
    self->s.sound = 0;

    if (self->health < (self->max_health / 2))
        self->s.skinnum |= 1;

    if (level.framenum < self->pain_debounce_framenum) {
        if ((self->velocity[2] > 100) && ((self->monsterinfo.currentmove == &soldier_move_pain1) || (self->monsterinfo.currentmove == &soldier_move_pain2) || (self->monsterinfo.currentmove == &soldier_move_pain3))) {
            // PMM - clear the duck flag before abandoning the current move,
            // or a soldier hurt mid-duck/mid-trip stays permanently shrunk.
            // monster_duck_up() is unguarded, so this must only run where the
            // move really is being replaced - never on the nightmare path
            // below, which leaves the move alone and still owes its own duck_up.
            if (self->monsterinfo.aiflags & AI_DUCKED)
                monster_duck_up(self);
            self->monsterinfo.currentmove = &soldier_move_pain4;
        }
        return;
    }

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    n = self->s.skinnum | 1;
    if (n == 1)
        gi.sound(self, CHAN_VOICE, sound_pain_light, 1, ATTN_NORM, 0);
    else if (n == 3)
        gi.sound(self, CHAN_VOICE, sound_pain, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_pain_ss, 1, ATTN_NORM, 0);

    if (self->velocity[2] > 100) {
            // PMM - clear the duck flag before abandoning the current move,
            // or a soldier hurt mid-duck/mid-trip stays permanently shrunk.
            // monster_duck_up() is unguarded, so this must only run where the
            // move really is being replaced - never on the nightmare path
            // below, which leaves the move alone and still owes its own duck_up.
            if (self->monsterinfo.aiflags & AI_DUCKED)
                monster_duck_up(self);
        self->monsterinfo.currentmove = &soldier_move_pain4;
        return;
    }

    // Trip trigger.  The rerelease enters this from soldier_duck out of its
    // attack6 run-and-gun, which this tree does not have.  It must NOT hang off
    // soldier_dodge either: check_dodge() in g_weapon.c is only called from the
    // projectile weapons, never from fire_lead, so hitscan fire would never
    // trigger it and the trip would almost never be seen.  Taking pain while
    // running fires for every weapon and is already debounced to 3 seconds by
    // pain_debounce_framenum above.  It sits ABOVE the nightmare early-out on
    // purpose: skill 3 skips pain ANIMATIONS, but the trip is a behaviour and
    // should still happen there.

    if (M_RereleaseGame() &&
        !(self->monsterinfo.aiflags & AI_STAND_GROUND) &&
        self->monsterinfo.currentmove != &soldier_move_trip &&
        self->enemy && random() < 0.5f) {
        self->monsterinfo.currentmove = &soldier_move_trip;
        return;
    }

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    r = random();

    // PMM - clear the duck flag before abandoning the current move, or a
    // soldier hurt mid-duck stays permanently shrunk.  monster_duck_up() is
    // unguarded, so this only runs where the move really is being replaced -
    // never on the nightmare path above, which leaves the move alone and still
    // owes its own duck_up.  The trip above does not need it either: it calls
    // duck_down (guarded) and duck_up itself, so the pair still balances.
    if (self->monsterinfo.aiflags & AI_DUCKED)
        monster_duck_up(self);

    if (r < 0.33f)
        self->monsterinfo.currentmove = &soldier_move_pain1;
    else if (r < 0.66f)
        self->monsterinfo.currentmove = &soldier_move_pain2;
    else
        self->monsterinfo.currentmove = &soldier_move_pain3;
}


//
// ATTACK
//

static int blaster_flash [] = {MZ2_SOLDIER_BLASTER_1, MZ2_SOLDIER_BLASTER_2, MZ2_SOLDIER_BLASTER_3, MZ2_SOLDIER_BLASTER_4, MZ2_SOLDIER_BLASTER_5, MZ2_SOLDIER_BLASTER_6, MZ2_SOLDIER_BLASTER_7, MZ2_SOLDIER_BLASTER_8, MZ2_SOLDIER_BLASTER_9};
static int shotgun_flash [] = {MZ2_SOLDIER_SHOTGUN_1, MZ2_SOLDIER_SHOTGUN_2, MZ2_SOLDIER_SHOTGUN_3, MZ2_SOLDIER_SHOTGUN_4, MZ2_SOLDIER_SHOTGUN_5, MZ2_SOLDIER_SHOTGUN_6, MZ2_SOLDIER_SHOTGUN_7, MZ2_SOLDIER_SHOTGUN_8, MZ2_SOLDIER_SHOTGUN_9};
static int machinegun_flash [] = {MZ2_SOLDIER_MACHINEGUN_1, MZ2_SOLDIER_MACHINEGUN_2, MZ2_SOLDIER_MACHINEGUN_3, MZ2_SOLDIER_MACHINEGUN_4, MZ2_SOLDIER_MACHINEGUN_5, MZ2_SOLDIER_MACHINEGUN_6, MZ2_SOLDIER_MACHINEGUN_7, MZ2_SOLDIER_MACHINEGUN_8, MZ2_SOLDIER_MACHINEGUN_9};

/*
=================
soldierh_laserbeam / soldierh_fire_weapon

The Xatrix soldierh variants, selected by skinnum on models/monsters/soldierh:
    0/1  ripper   - bouncing ion blade
    2/3  hypergun - blue hyperblaster bolts
    4/5  lasergun - continuous damage beam
The odd skin of each pair is that variant's pain skin, which soldier_pain sets
with |= 1, so every test here is a range and not an equality.
=================
*/
static void soldierh_laserbeam(edict_t *self, int flash_index)
{
    vec3_t  forward, right, up;
    vec3_t  start, dir, angles, end;
    vec3_t  tempvec;
    edict_t *ent;

    if (Q_rand() % 5 == 0)
        gi.sound(self, CHAN_AUTO, gi.soundindex("misc/lasfly.wav"), 1, ATTN_STATIC, 0);

    VectorCopy(self->s.origin, start);
    VectorCopy(self->enemy->s.origin, end);
    VectorSubtract(end, start, dir);
    vectoangles(dir, angles);
    VectorCopy(monster_flash_offset[flash_index], tempvec);

    ent = G_Spawn();
    VectorCopy(self->s.origin, ent->s.origin);
    AngleVectors(angles, forward, right, up);
    VectorCopy(angles, ent->s.angles);
    VectorCopy(ent->s.origin, start);

    // flash 85 is the left-hand muzzle; its offset needs mirroring
    if (flash_index == 85)
        VectorMA(start, tempvec[0] - 14, right, start);
    else
        VectorMA(start, tempvec[0] + 2, right, start);
    VectorMA(start, tempvec[2] + 8, up, start);
    VectorMA(start, tempvec[1], forward, start);

    VectorCopy(start, ent->s.origin);
    ent->enemy = self->enemy;
    ent->owner = self;
    ent->dmg = 1;
    ent->classname = "soldier_laserbeam";

    monster_dabeam(ent);
}

static void soldierh_fire_weapon(edict_t *self, int flash_index)
{
    vec3_t  start;
    vec3_t  forward, right, up;
    vec3_t  aim, dir, end;
    float   r, u;

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash_index], forward, right, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, aim);
    vectoangles(aim, dir);
    AngleVectors(dir, forward, right, up);

    // these three aim far tighter than the stock soldier's 1000/500 scatter
    r = crandom() * 100;
    u = crandom() * 50;
    VectorMA(start, 8192, forward, end);
    VectorMA(end, r, right, end);
    VectorMA(end, u, up, end);
    VectorSubtract(end, start, aim);
    VectorNormalize(aim);

    if (self->s.skinnum <= 1) {
        monster_fire_ionripper(self, start, aim, 5, 600, MZ_IONRIPPER, EF_IONRIPPER);
    } else if (self->s.skinnum <= 3) {
        monster_fire_blueblaster(self, start, aim, 1, 600, MZ_BLUEHYPERBLASTER, EF_BLUEHYPERBLASTER);
    } else {
        if (!(self->monsterinfo.aiflags & AI_HOLD_FRAME))
            self->monsterinfo.pause_framenum = level.framenum + (3 + Q_rand() % 8);

        soldierh_laserbeam(self, flash_index);

        if (level.framenum >= self->monsterinfo.pause_framenum)
            self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
        else
            self->monsterinfo.aiflags |= AI_HOLD_FRAME;
    }
}

void soldier_fire(edict_t *self, int flash_number)
{
    vec3_t  start;
    vec3_t  forward, right, up;
    vec3_t  aim;
    vec3_t  dir;
    vec3_t  end;
    float   r, u;
    int     flash_index;

    // style 1 is the Xatrix "soldierh" family (ripper / hypergun / lasergun). It
    // shares every animation with the stock soldier - soldierh.md2 is a pure reskin,
    // frame-for-frame identical - so only the muzzle flash and the weapon differ.
    if (self->style == 1) {
        if (self->s.skinnum < 4)
            flash_index = blaster_flash[flash_number];  // ripper and hypergun
        else
            flash_index = machinegun_flash[flash_number];   // laser beam
    } else if (self->s.skinnum < 2)
        flash_index = blaster_flash[flash_number];
    else if (self->s.skinnum < 4)
        flash_index = shotgun_flash[flash_number];
    else
        flash_index = machinegun_flash[flash_number];

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash_index], forward, right, start);

    if (flash_number == 5 || flash_number == 6) {
        VectorCopy(forward, aim);
    } else {
        VectorCopy(self->enemy->s.origin, end);
        end[2] += self->enemy->viewheight;
        VectorSubtract(end, start, aim);
        vectoangles(aim, dir);
        AngleVectors(dir, forward, right, up);

        r = crandom() * 1000;
        u = crandom() * 500;
        VectorMA(start, 8192, forward, end);
        VectorMA(end, r, right, end);
        VectorMA(end, u, up, end);

        VectorSubtract(end, start, aim);
        VectorNormalize(aim);
    }

    if (self->style == 1) {
        soldierh_fire_weapon(self, flash_index);
        return;
    }

    if (self->s.skinnum <= 1) {
		if (self->monsterFireHyperBlaster) {
			monster_fire_hyper_blaster(self, start, aim, 5, 600, flash_index, EF_HYPERBLASTER);
		}
		else {
			monster_fire_blaster(self, start, aim, 5, 600, flash_index, EF_BLASTER);
		}
    } else if (self->s.skinnum <= 3) {
        monster_fire_shotgun(self, start, aim, 2, 1, DEFAULT_SHOTGUN_HSPREAD, DEFAULT_SHOTGUN_VSPREAD, DEFAULT_SHOTGUN_COUNT, flash_index);
        // [Paril-KEX] this soldier must cock before it can fire again.  Every
        // reader of self->dmg is rerelease-only, but keep the write gated so a
        // baseq2 soldier's dmg field stays exactly as the 1997 game left it.
        if (M_RereleaseGame())
            self->dmg = 1;
    } else {
        if (!(self->monsterinfo.aiflags & AI_HOLD_FRAME))
            self->monsterinfo.pause_framenum = level.framenum + (3 + Q_rand() % 8);

        monster_fire_bullet(self, start, aim, 2, 4, DEFAULT_BULLET_HSPREAD, DEFAULT_BULLET_VSPREAD, flash_index);

        if (level.framenum >= self->monsterinfo.pause_framenum)
            self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
        else
            self->monsterinfo.aiflags |= AI_HOLD_FRAME;
    }
}

// ATTACK1 (blaster/shotgun)

void soldier_fire1(edict_t *self)
{
    soldier_fire(self, 0);
}

void soldier_attack1_refire1(edict_t *self)
{
    if (M_RereleaseGame()) {
        // [Paril-KEX] a blaster soldier that has run its burst skips straight
        // to the tail of the animation
        if (self->s.skinnum <= 0)
            self->monsterinfo.nextframe = FRAME_attak110;

        // PMM - blindfire: one shot only, then drop the manual aim
        if (self->monsterinfo.aiflags & AI_MANUAL_STEERING) {
            self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
            return;
        }

        if (!self->enemy)
            return;

        if (self->s.skinnum > 1)
            return;

        if (self->enemy->health <= 0)
            return;

        if (((random() < 0.5f) && visible(self, self->enemy)) ||
            (realrange(self, self->enemy) <= SOLDIER_RANGE_MELEE))
            self->monsterinfo.nextframe = FRAME_attak102;
        else
            self->monsterinfo.nextframe = FRAME_attak110;
        return;
    }

    if (self->s.skinnum > 1)
        return;

    if (self->enemy->health <= 0)
        return;

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attak102;
    else
        self->monsterinfo.nextframe = FRAME_attak110;
}

void soldier_attack1_refire2(edict_t *self)
{
    if (M_RereleaseGame()) {
        if (!self->enemy)
            return;

        if (self->s.skinnum < 2)
            return;

        if (self->enemy->health <= 0)
            return;

        // radius_dmg is the forced refire the shotgun check below asked for
        if (((self->radius_dmg || random() < 0.5f) && visible(self, self->enemy)) ||
            (realrange(self, self->enemy) <= SOLDIER_RANGE_MELEE)) {
            self->monsterinfo.nextframe = FRAME_attak102;
            self->radius_dmg = 0;
        }
        return;
    }

    if (self->s.skinnum < 2)
        return;

    if (self->enemy->health <= 0)
        return;

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attak102;
}

/*
=================
The shotgun checks, and soldier_blind_check

Both of these sit in frame tables the ORIGINAL game plays too, and both are
self-gating rather than wrapped in M_RereleaseGame():

  the shotgun checks act only when self->dmg is set, and only the rerelease
  branch of soldier_fire ever sets it;
  soldier_blind_check acts only under AI_MANUAL_STEERING, which only the
  rerelease's blindfire raises.

A shotgun soldier that fired but has not cocked jumps forward to the cocking
half of the animation and sets radius_dmg so the refire that follows is forced -
that is what makes the shotgun guard fire in pairs instead of one shot a cycle.
=================
*/
static void soldier_attack1_shotgun_check(edict_t *self)
{
    if (self->dmg) {
        self->monsterinfo.nextframe = FRAME_attak106;
        self->radius_dmg = 1;
    }
}

static void soldier_blind_check(edict_t *self)
{
    vec3_t  aim;

    if (self->monsterinfo.aiflags & AI_MANUAL_STEERING) {
        VectorSubtract(self->monsterinfo.blind_fire_target, self->s.origin, aim);
        self->ideal_yaw = vectoyaw(aim);
    }
}

mframe_t soldier_frames_attack1 [] = {
    { ai_charge, 0,  soldier_blind_check },
    { ai_charge, 0,  soldier_attack1_shotgun_check },
    { ai_charge, 0,  soldier_fire1 },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  soldier_attack1_refire1 },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  soldier_cock },
    { ai_charge, 0,  soldier_attack1_refire2 },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL }
};
mmove_t soldier_move_attack1 = {FRAME_attak101, FRAME_attak112, soldier_frames_attack1, soldier_run};

// ATTACK2 (blaster/shotgun)

void soldier_fire2(edict_t *self)
{
    soldier_fire(self, 1);
}

void soldier_attack2_refire1(edict_t *self)
{
    if (M_RereleaseGame()) {
        if (self->s.skinnum <= 0)
            self->monsterinfo.nextframe = FRAME_attak216;

        if (!self->enemy)
            return;

        if (self->s.skinnum > 1)
            return;

        if (self->enemy->health <= 0)
            return;

        if (((random() < 0.5f) && visible(self, self->enemy)) ||
            (realrange(self, self->enemy) <= SOLDIER_RANGE_MELEE))
            self->monsterinfo.nextframe = FRAME_attak204;
        return;
    }

    if (self->s.skinnum > 1)
        return;

    if (self->enemy->health <= 0)
        return;

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attak204;
    else
        self->monsterinfo.nextframe = FRAME_attak216;
}

void soldier_attack2_refire2(edict_t *self)
{
    if (M_RereleaseGame()) {
        if (!self->enemy)
            return;

        if (self->s.skinnum < 2)
            return;

        if (self->enemy->health <= 0)
            return;

        // the lasergun soldier (style 1, skin >= 4) is excluded from the
        // melee-range clause: it is already firing a continuous beam
        if (((self->radius_dmg || random() < 0.5f) && visible(self, self->enemy)) ||
            ((self->style == 0 || self->s.skinnum < 4) &&
             (realrange(self, self->enemy) <= SOLDIER_RANGE_MELEE))) {
            self->monsterinfo.nextframe = FRAME_attak204;
            self->radius_dmg = 0;
        }
        return;
    }

    if (self->s.skinnum < 2)
        return;

    if (self->enemy->health <= 0)
        return;

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attak204;
}

static void soldier_attack2_shotgun_check(edict_t *self)
{
    if (self->dmg) {
        self->monsterinfo.nextframe = FRAME_attak210;
        self->radius_dmg = 1;
    }
}

mframe_t soldier_frames_attack2 [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_attack2_shotgun_check },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_fire2 },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_attack2_refire1 },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_cock },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_attack2_refire2 },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t soldier_move_attack2 = {FRAME_attak201, FRAME_attak218, soldier_frames_attack2, soldier_run};

// ATTACK3 (duck and shoot)



// declared here because the crouched and run-and-shoot attacks below use them,
// and the soldierh burst block that defines them sits further down the file
static void soldierh_hyper_laser_sound_start(edict_t *self);
static void soldierh_hyper_laser_sound_end(edict_t *self);
static void soldierh_hyperripper3(edict_t *self);
static void soldierh_hyperripper8(edict_t *self);

// The rerelease ducks on the FIRST frame of the crouched attack, two frames
// earlier than id did, and so drops the duck out of soldier_fire3.  Doing that
// unconditionally would change how the original game's soldier crouches, so the
// two halves gate against each other.
static void soldier_attack3_duck(edict_t *self)
{
    if (M_RereleaseGame())
        monster_duck_down(self);
}

void soldier_fire3(edict_t *self)
{
    if (!M_RereleaseGame())
        monster_duck_down(self);
    soldier_fire(self, 2);
}

void soldier_attack3_refire(edict_t *self)
{
    if (M_RereleaseGame()) {
        // a shotgun soldier caught mid-cock holds the crouch instead of
        // popping back up with a spent shell
        if (self->dmg) {
            monster_duck_hold(self);
            return;
        }

        // they read the DUCK timer here.  id had only one field
        // (monsterinfo.pausetime, this tree's pause_framenum) and shared it
        // with the machinegun's firing window, which is why the classic branch
        // below must keep reading that one.
        if ((level.framenum + 0.4f * BASE_FRAMERATE) < self->monsterinfo.duck_wait_framenum)
            self->monsterinfo.nextframe = FRAME_attak303;
        return;
    }

    if ((level.framenum + 0.4f * BASE_FRAMERATE) < self->monsterinfo.pause_framenum)
        self->monsterinfo.nextframe = FRAME_attak303;
}

mframe_t soldier_frames_attack3 [] = {
    { ai_charge, 0, soldier_attack3_duck },
    { ai_charge, 0, soldierh_hyper_laser_sound_start },
    { ai_charge, 0, soldier_fire3 },
    { ai_charge, 0, soldierh_hyperripper3 },
    { ai_charge, 0, soldierh_hyperripper3 },
    { ai_charge, 0, soldier_attack3_refire },
    { ai_charge, 0, monster_duck_up },
    { ai_charge, 0, soldierh_hyper_laser_sound_end },
    { ai_charge, 0, NULL }
};
mmove_t soldier_move_attack3 = {FRAME_attak301, FRAME_attak309, soldier_frames_attack3, soldier_run};

// ATTACK4 (machinegun)

void soldier_fire4(edict_t *self)
{
    soldier_fire(self, 3);
//
//  if (self->enemy->health <= 0)
//      return;
//
//  if ( ((skill->value == 3) && (random() < 0.5)) || (range(self, self->enemy) == RANGE_MELEE) )
//      self->monsterinfo.nextframe = FRAME_attak402;
}

mframe_t soldier_frames_attack4 [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldierh_hyper_laser_sound_start },
    { ai_charge, 0, soldier_fire4 },
    { ai_charge, 0, soldierh_hyper_laser_sound_end },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t soldier_move_attack4 = {FRAME_attak401, FRAME_attak406, soldier_frames_attack4, soldier_run};

#if 0
// ATTACK5 (prone)

void soldier_fire5(edict_t *self)
{
    soldier_fire(self, 4);
}

void soldier_attack5_refire(edict_t *self)
{
    if (self->enemy->health <= 0)
        return;

    if (((skill->value == 3) && (random() < 0.5f)) || (range(self, self->enemy) == RANGE_MELEE))
        self->monsterinfo.nextframe = FRAME_attak505;
}

mframe_t soldier_frames_attack5 [] = {
    { ai_charge, 8, NULL },
    { ai_charge, 8, monster_footstep },
    { ai_charge, 0, monster_footstep },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_fire5 },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_attack5_refire }
};
mmove_t soldier_move_attack5 = {FRAME_attak501, FRAME_attak508, soldier_frames_attack5, soldier_run};
#endif

// ATTACK6 (run & shoot)

void soldier_fire8(edict_t *self)
{
    soldier_fire(self, 7);
}

void soldier_attack6_refire(edict_t *self)
{
    if (self->enemy->health <= 0)
        return;

    if (range(self, self->enemy) < RANGE_MID)
        return;

    if (skill->value == 3)
        self->monsterinfo.nextframe = FRAME_runs03;
}

/*
=================
The rerelease's run-and-shoot

Three changes to a move id shipped but barely used:

  it runs on ai_run, not ai_charge, so the soldier actually navigates towards
  its enemy while firing instead of sliding straight at it.  ai_soldier_charge
  below is the per-slot switch - the frame table cannot be gated any other way,
  and duplicating it would mean every `currentmove == &soldier_move_attack6`
  test in this file had to check two moves.

  the tail refire splits in two.  refire1 is the blaster soldier's (skin <= 1)
  and refire2 the rest's; both clear the dodge and charge bits first, and both
  break off into a plain run when the enemy is dead, too close, or out of
  sight - "don't endlessly run into walls".

  the shotgun check at slot 1 jumps to the cocking half, exactly as attack1 and
  attack2 do.

Dropped: their MMOVE_T's 5th field (0.65f) is `sidestep_scale`, and this tree's
mmove_t has no such field - it only scales sidestep distance, not run speed.
=================
*/
static void ai_soldier_charge(edict_t *self, float dist)
{
    if (M_RereleaseGame())
        ai_run(self, dist);
    else
        ai_charge(self, dist);
}

void soldier_attack6_refire1(edict_t *self)
{
    // PMM - make sure dodge & charge bits are cleared
    monster_done_dodge(self);
    soldier_stop_charge(self);

    if (!self->enemy)
        return;

    if (self->s.skinnum > 1)
        return;

    if (self->enemy->health <= 0 ||
        realrange(self, self->enemy) < SOLDIER_RANGE_NEAR ||
        !visible(self, self->enemy)) {
        soldier_run(self);
        return;
    }

    if (random() < 0.25f)
        self->monsterinfo.nextframe = FRAME_runs03;
    else
        soldier_run(self);
}

void soldier_attack6_refire2(edict_t *self)
{
    // PMM - make sure dodge & charge bits are cleared
    monster_done_dodge(self);
    soldier_stop_charge(self);

    if (!self->enemy || self->s.skinnum <= 0)
        return;

    if (self->enemy->health <= 0 ||
        (!self->radius_dmg && realrange(self, self->enemy) < SOLDIER_RANGE_NEAR) ||
        !visible(self, self->enemy)) {
        soldierh_hyper_laser_sound_end(self);
        return;
    }

    if (self->radius_dmg || random() < 0.25f) {
        self->monsterinfo.nextframe = FRAME_runs03;
        self->radius_dmg = 0;
    }
}

static void soldier_attack6_shotgun_check(edict_t *self)
{
    if (self->dmg) {
        self->monsterinfo.nextframe = FRAME_runs09;
        self->radius_dmg = 1;
    }
}

/*
The four slots below carry a rerelease call in a table the original game plays
too, and unlike the shotgun/blindfire/soldierh checks they are not self-gating -
a bare soldier_cock here would fire a sound the 1997 soldier never made, and
refire1 would break the classic run-and-shoot off into a plain run.  Slot 13
holds id's single refire and the rerelease's second one, which is the pair that
cannot share a wrapper.
*/
static void soldier_attack6_start_charge(edict_t *self)
{
    if (M_RereleaseGame())
        soldier_start_charge(self);
}

static void soldier_attack6_do_refire1(edict_t *self)
{
    if (M_RereleaseGame())
        soldier_attack6_refire1(self);
}

static void soldier_attack6_cock(edict_t *self)
{
    if (M_RereleaseGame())
        soldier_cock(self);
}

static void soldier_attack6_refire_tail(edict_t *self)
{
    if (M_RereleaseGame())
        soldier_attack6_refire2(self);
    else
        soldier_attack6_refire(self);
}

// their slots 3 and 4 pack two calls each into one C++ lambda
static void soldier_fire8_footstep(edict_t *self)
{
    soldier_fire8(self);
    monster_footstep(self);
}

static void soldierh_hyperripper8_done_dodge(edict_t *self)
{
    soldierh_hyperripper8(self);
    monster_done_dodge(self);
}

mframe_t soldier_frames_attack6 [] = {
    { ai_soldier_charge, 10, soldier_attack6_start_charge },
    { ai_soldier_charge,  4, soldier_attack6_shotgun_check },
    { ai_soldier_charge, 12, soldierh_hyper_laser_sound_start },
    { ai_soldier_charge, 11, soldier_fire8_footstep },
    { ai_soldier_charge, 13, soldierh_hyperripper8_done_dodge },
    { ai_soldier_charge, 18, soldierh_hyperripper8 },
    { ai_soldier_charge, 15, monster_footstep },
    { ai_soldier_charge, 14, soldier_attack6_do_refire1 },
    { ai_soldier_charge, 11, NULL },
    { ai_soldier_charge,  8, monster_footstep },
    { ai_soldier_charge, 11, soldier_attack6_cock },
    { ai_soldier_charge, 12, NULL },
    { ai_soldier_charge, 12, monster_footstep },
    { ai_soldier_charge, 17, soldier_attack6_refire_tail }
};
mmove_t soldier_move_attack6 = {FRAME_runs01, FRAME_runs14, soldier_frames_attack6, soldier_run};


/*
=================
The rerelease's hyper-soldier burst fire

The ripper / hypergun / lasergun soldiers (SP_monster_soldier_h, style == 1) are
rapid-fire variants, but this tree fired them on the STANDARD soldier's cadence -
one shot per attack cycle - because soldier_fire just branches to
soldierh_fire_weapon inside the normal frame tables.  The rerelease gives them
their own tables: the same frames, but two extra shots after the first and a
looping weapon sound bracketing the burst.

Same frame ranges as soldier_move_attack1/2 (attak101-112 and attak201-218), so
these need no new frames and no M_RereleaseAnims() gating.  They do change how
the xatrix variants play, so soldier_attack only picks them under
M_RereleaseGame().

Mapping notes vs src/rerelease/m_soldier.cpp:
  - their `self->count` is the variant index; ours is `self->s.skinnum`
    (<=1 blaster/ripper, 2-3 shotgun/hypergun, >=4 machinegun/lasergun).
  - their `monsterinfo.weapon_sound` only ever feeds `ent->s.sound`
    (g_monster.cpp ~405), and this tree has no such monsterinfo field, so set
    `self->s.sound` directly - the idiom m_boss2.c and m_boss31.c already use.
  - `soldier_fire` here takes no angle_limited parameter.
=================
*/
static void soldierh_hyper_laser_sound_start(edict_t *self)
{
    // only the hypergun variant has a spin-up loop; ripper and lasergun do not
    if (self->style == 1 && self->s.skinnum >= 2 && self->s.skinnum < 4)
        self->s.sound = gi.soundindex("weapons/hyprbl1a.wav");
}

static void soldierh_hyper_laser_sound_end(edict_t *self)
{
    if (self->s.sound) {
        if (self->s.skinnum >= 2 && self->s.skinnum < 4)
            gi.sound(self, CHAN_AUTO, gi.soundindex("weapons/hyprbd1a.wav"), 1, ATTN_NORM, 0);

        self->s.sound = 0;
    }
}

static void soldierh_hyperripper1(edict_t *self)
{
    if (self->s.skinnum < 4)
        soldier_fire(self, 0);
}

static void soldierh_hyperripper2(edict_t *self)
{
    if (self->s.skinnum < 4)
        soldier_fire(self, 1);
}

/*
The remaining three burst shots.  hyperripper1/2 above ride the soldierh-only
attack1/attack2 tables and so only need the skin test, but 3, 5 and 8 sit in the
crouched attack, the prone attack and the run-and-shoot - tables the stock
soldier plays as well.  The rerelease's test there is `s.skinnum >= 6 &&
count < 4`, i.e. "is soldierh AND is not the lasergun", which is style == 1 and
skinnum < 4 here.
*/
static void soldierh_hyperripper3(edict_t *self)
{
    if (self->style == 1 && self->s.skinnum < 4)
        soldier_fire(self, 2);
}

static void soldierh_hyperripper5(edict_t *self)
{
    if (self->style == 1 && self->s.skinnum < 4)
        soldier_fire(self, 8);
}

static void soldierh_hyperripper8(edict_t *self)
{
    if (self->style == 1 && self->s.skinnum < 4)
        soldier_fire(self, 7);
}

static void soldierh_hyper_refire1(edict_t *self)
{
    if (!self->enemy)
        return;

    if (self->s.skinnum >= 2 && self->s.skinnum < 4)
        if (random() < 0.7f && visible(self, self->enemy))
            self->s.frame = FRAME_attak103;
}

static void soldierh_hyper_refire2(edict_t *self)
{
    if (!self->enemy)
        return;

    if (self->s.skinnum >= 2 && self->s.skinnum < 4)
        if (random() < 0.7f && visible(self, self->enemy))
            self->s.frame = FRAME_attak205;
}

mframe_t soldierh_frames_attack1 [] = {
    { ai_charge, 0,  soldier_blind_check },
    { ai_charge, 0,  soldierh_hyper_laser_sound_start },
    { ai_charge, 0,  soldier_fire1 },
    { ai_charge, 0,  soldierh_hyperripper1 },
    { ai_charge, 0,  soldierh_hyperripper1 },
    { ai_charge, 0,  soldier_attack1_refire1 },
    { ai_charge, 0,  soldierh_hyper_refire1 },
    { ai_charge, 0,  soldier_cock },
    { ai_charge, 0,  soldier_attack1_refire2 },
    { ai_charge, 0,  soldierh_hyper_laser_sound_end },
    { ai_charge, 0,  NULL },
    { ai_charge, 0,  NULL }
};
mmove_t soldierh_move_attack1 = {FRAME_attak101, FRAME_attak112, soldierh_frames_attack1, soldier_run};

mframe_t soldierh_frames_attack2 [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldierh_hyper_laser_sound_start },
    { ai_charge, 0, soldier_fire2 },
    { ai_charge, 0, soldierh_hyperripper2 },
    { ai_charge, 0, soldierh_hyperripper2 },
    { ai_charge, 0, soldier_attack2_refire1 },
    { ai_charge, 0, soldierh_hyper_refire2 },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_cock },
    { ai_charge, 0, NULL },
    { ai_charge, 0, soldier_attack2_refire2 },
    { ai_charge, 0, soldierh_hyper_laser_sound_end },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t soldierh_move_attack2 = {FRAME_attak201, FRAME_attak218, soldierh_frames_attack2, soldier_run};

void soldier_attack(edict_t *self)
{
    // style 1 is the ripper / hypergun / lasergun family; the rerelease fires
    // those in bursts off their own frame tables
    bool hyper = M_RereleaseGame() && self->style == 1;
    vec3_t  ignored;    // M_CheckClearShot's out-parameter; the shot start

    // NOTE: written as plain if/else on purpose.  genptr.py scrapes
    // `->monsterinfo.currentmove = &name` with a regex and cannot see through a
    // ternary - it silently harvests the CONDITION as a move name and drops the
    // real ones, which breaks the link and corrupts the save pointer table.
    if (!M_RereleaseGame()) {
        if (self->s.skinnum < 4) {
            if (random() < 0.5f)
                self->monsterinfo.currentmove = &soldier_move_attack1;
            else
                self->monsterinfo.currentmove = &soldier_move_attack2;
        } else {
            self->monsterinfo.currentmove = &soldier_move_attack4;
        }
        return;
    }

    monster_done_dodge(self);

    // PMM - blindfire.  M_CheckAttack put us in AS_BLIND because the enemy is
    // out of sight but recently seen; the chance ladder falls off with the
    // accumulated delay.  Blind shots always use attack1 - soldier_blind_check
    // on its first frame is what steers the aim at blind_fire_target.
    if (self->monsterinfo.attack_state == AS_BLIND) {
        float   chance;

        if (self->monsterinfo.blind_fire_delay < 1.0f * BASE_FRAMERATE)
            chance = 1.0f;
        else if (self->monsterinfo.blind_fire_delay < 7.5f * BASE_FRAMERATE)
            chance = 0.4f;
        else
            chance = 0.1f;

        // minimum of 4.1 seconds, plus 0-3, after the shots are done
        self->monsterinfo.blind_fire_delay += (4.1f + 3.0f * random()) * BASE_FRAMERATE;

        // don't shoot at the origin
        if (VectorEmpty(self->monsterinfo.blind_fire_target))
            return;

        if (random() > chance)
            return;

        // AI_MANUAL_STEERING signals both manual steering and blindfire
        self->monsterinfo.aiflags |= AI_MANUAL_STEERING;

        if (hyper)
            self->monsterinfo.currentmove = &soldierh_move_attack1;
        else
            self->monsterinfo.currentmove = &soldier_move_attack1;

        self->monsterinfo.attack_finished =
            level.framenum + (1.5f + random()) * BASE_FRAMERATE;
        return;
    }

    // PMM - run TOWARDS the player and shoot rather than stopping to shoot.
    // Not limited by M_CheckClearShot: at this range it does not matter.
    if (!(self->monsterinfo.aiflags & AI_STAND_GROUND) && random() < 0.25f &&
        self->s.skinnum <= 3 &&
        realrange(self, self->enemy) >= (SOLDIER_RANGE_NEAR * 0.5f)) {
        self->monsterinfo.currentmove = &soldier_move_attack6;
        return;
    }

    if (self->s.skinnum < 4) {
        bool    attack1_possible;
        bool    attack2_possible;

        // [Paril-KEX] the shotgun guard only uses attack2 up close - attack1
        // is its long pose and the spread makes it useless there
        if (self->style == 0 && self->s.skinnum >= 2 && self->s.skinnum <= 3 &&
            realrange(self, self->enemy) <= (SOLDIER_RANGE_NEAR * 0.65f))
            attack1_possible = false;
        else
            attack1_possible = M_CheckClearShot(self, monster_flash_offset[MZ2_SOLDIER_BLASTER_1], ignored);

        attack2_possible = M_CheckClearShot(self, monster_flash_offset[MZ2_SOLDIER_BLASTER_2], ignored);

        if (attack1_possible && (!attack2_possible || random() < 0.5f)) {
            if (hyper)
                self->monsterinfo.currentmove = &soldierh_move_attack1;
            else
                self->monsterinfo.currentmove = &soldier_move_attack1;
        } else if (attack2_possible) {
            if (hyper)
                self->monsterinfo.currentmove = &soldierh_move_attack2;
            else
                self->monsterinfo.currentmove = &soldier_move_attack2;
        }
    } else if (M_CheckClearShot(self, monster_flash_offset[MZ2_SOLDIER_MACHINEGUN_4], ignored)) {
        self->monsterinfo.currentmove = &soldier_move_attack4;
    }
}


//
// SIGHT
//

void soldier_sight(edict_t *self, edict_t *other)
{
    vec3_t  ignored;    // M_CheckClearShot's out-parameter

    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, sound_sight1, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_sight2, 1, ATTN_NORM, 0);

    if (!M_RereleaseGame()) {
        if ((skill->value > 0) && (range(self, self->enemy) >= RANGE_MID)) {
            if (random() > 0.5f)
                self->monsterinfo.currentmove = &soldier_move_attack6;
        }
        return;
    }

    // [Paril-KEX] the rerelease drops the skill test and adds a visibility one -
    // don't break into a run-and-shoot at something you cannot see.  The
    // soldierh family always takes it; the stock soldier one time in four.
    if (self->enemy && realrange(self, self->enemy) >= SOLDIER_RANGE_NEAR &&
        visible(self, self->enemy)) {
        if (self->style == 1 || random() > 0.75f) {
            // legacy bug fix: no run-and-shoot for the machinegun and lasergun
            // soldiers, whose animation for it reads wrong
            if (self->s.skinnum < 4)
                self->monsterinfo.currentmove = &soldier_move_attack6;
            else if (M_CheckClearShot(self, monster_flash_offset[MZ2_SOLDIER_MACHINEGUN_4], ignored))
                self->monsterinfo.currentmove = &soldier_move_attack4;
        }
    }
}

//
// DUCK
//


mframe_t soldier_frames_duck [] = {
    { ai_move, 5, monster_duck_down },
    { ai_move, -1, monster_duck_hold },
    { ai_move, 1,  NULL },
    { ai_move, 0,  monster_duck_up },
    { ai_move, 5,  NULL }
};
mmove_t soldier_move_duck = {FRAME_duck01, FRAME_duck05, soldier_frames_duck, soldier_run};

//
// PRONE SHOOTING  (rerelease soldier_move_attack5)
//
// The soldier drops onto its front partway through the trip and keeps firing
// from the ground, then gets back up.  Uses the appended attak501-508, so it is
// gated on M_RereleaseAnims().
//
// Dropped from the rerelease version: monster_footstep (no such sound here) and
// the soldierh_* hyper-soldier hooks.  soldier_fire() also has no angle_limited
// parameter in this tree - soldier_prone_shoot_ok()'s cone test below enforces
// the same constraint, which is what that flag is for.

// Can the soldier still see its enemy within the narrow cone it can cover while
// lying down?  Once it cannot, it has to stand up rather than spin on the floor.
static bool soldier_prone_shoot_ok(edict_t *self)
{
    vec3_t  fwd;
    vec3_t  diff;

    if (!self->enemy || !self->enemy->inuse)
        return false;

    AngleVectors(self->s.angles, fwd, NULL, NULL);

    VectorSubtract(self->enemy->s.origin, self->s.origin, diff);
    diff[2] = 0;
    VectorNormalize(diff);

    return DotProduct(fwd, diff) >= 0.80f;
}

void soldier_stand_up(edict_t *self)
{
    soldierh_hyper_laser_sound_end(self);

    // rejoin the trip animation at its get-back-up half
    self->monsterinfo.currentmove = &soldier_move_trip;
    self->monsterinfo.nextframe = FRAME_runt08;
}

// ai_move that bails out of the prone pose the moment the shot is no longer on
static void ai_soldier_move(edict_t *self, float dist)
{
    ai_move(self, dist);

    if (!soldier_prone_shoot_ok(self))
        soldier_stand_up(self);
}

void soldier_fire5(edict_t *self)
{
    soldier_fire(self, 8);
}

mframe_t soldier_frames_attack5 [] = {
    { ai_move, 18, monster_duck_down },
    { ai_move, 11, monster_footstep },
    { ai_move, 0,  monster_footstep },
    { ai_soldier_move, 0, NULL },
    { ai_soldier_move, 0, soldierh_hyper_laser_sound_start },
    { ai_soldier_move, 0, soldier_fire5 },
    { ai_soldier_move, 0, soldierh_hyperripper5 },
    { ai_soldier_move, 0, soldierh_hyperripper5 }
};
mmove_t soldier_move_attack5 = {FRAME_attak501, FRAME_attak508, soldier_frames_attack5, soldier_stand_up};

// Called from the trip, one frame in: decide whether to go prone and shoot
// instead of simply falling and getting back up.
static void monster_check_prone(edict_t *self)
{
    if (!M_RereleaseAnims())
        return;     // attak501-508 do not exist on the classic md2

    // a shotgun guard waiting to cock cannot fire from the ground either
    if (self->style == 0 && self->s.skinnum >= 2 && self->s.skinnum <= 3 && self->dmg)
        return;

    if (!soldier_prone_shoot_ok(self))
        return;     // not going to shoot at this angle

    self->monsterinfo.currentmove = &soldier_move_attack5;
}

//
// BLIND  (rerelease soldier_move_blind)
//
// Spawnflag 8 gives the soldier a different idle: it stands scanning, unaware,
// on stand101-130.  These frames already exist in the classic model, so this
// needs no gating.  Only the STAND handler is replaced - once it acquires a
// target it runs and fights exactly like any other soldier.

void soldier_blind(edict_t *self);

mframe_t soldier_frames_blind [] = {
    { ai_move, 0, soldier_idle },
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
mmove_t soldier_move_blind = {FRAME_stand101, FRAME_stand130, soldier_frames_blind, soldier_blind};

void soldier_blind(edict_t *self)
{
    self->monsterinfo.currentmove = &soldier_move_blind;
}

//
// TRIP
//
// The soldier's runt01-runt19 frames are a trip-and-recover animation that id
// shipped in the original tris.md2 and never used - nothing in the classic game
// code references them.  The rerelease finally wires them up, which is why the
// animation is already correct on the classic MD2 as well as the MD5 model and
// needs no cl_md5_models gating.
//
// Ported from soldier_frames_trip in src/rerelease/m_soldier.cpp.  Two of its
// think calls have no equivalent here: monster_footstep is a sound this tree
// does not have, and monster_check_prone belongs to the prone-shooting system
// (soldier_move_attack5, which needs the new attak501-508 frames) that is not
// ported.  Everything else, including the duck down/up at the same frames, is
// the rerelease's.
mframe_t soldier_frames_trip [] = {
    { ai_move, 10,  NULL },
    { ai_move, 2,   monster_check_prone },
    { ai_move, 18,  monster_duck_down },
    { ai_move, 11, monster_footstep },
    { ai_move, 9,   NULL },
    { ai_move, -11, monster_footstep },
    { ai_move, -2,  NULL },
    { ai_move, 0,   NULL },
    { ai_move, 6,   NULL },
    { ai_move, -5,  NULL },
    { ai_move, 0,   NULL },
    { ai_move, 1,   NULL },
    { ai_move, 0, monster_footstep },
    { ai_move, 0,   monster_duck_up },
    { ai_move, 3,   NULL },
    { ai_move, 2, monster_footstep },
    { ai_move, -1,  NULL },
    { ai_move, 2,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_trip = {FRAME_runt01, FRAME_runt19, soldier_frames_trip, soldier_run};

/*
=================
soldier_duck / soldier_sidestep

The soldier is the irregular one. Its duck is not a single crouch: mid-burst it
trips instead, otherwise it picks between the plain duck and the crouched
attack. Both paths end the looping hypergun sound, which would otherwise keep
playing through the dodge.

The classic soldier_dodge that this replaces also drove the trip, so the trip
still has a route in - see soldier_duck below.
=================
*/
bool soldier_duck(edict_t *self, float eta)
{
    // whatever we were holding, we are moving now
    self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;

    if (self->monsterinfo.currentmove == &soldier_move_attack6) {
        // caught mid prone-fire: trip rather than crouch
        self->monsterinfo.currentmove = &soldier_move_trip;
    } else if (self->dmg || random() < 0.5f) {
        self->monsterinfo.currentmove = &soldier_move_duck;
    } else {
        self->monsterinfo.currentmove = &soldier_move_attack3;
    }

    soldierh_hyper_laser_sound_end(self);
    return true;
}

bool soldier_sidestep(edict_t *self)
{
    // don't sidestep out of a trip or the prone-recovery pain
    if (self->monsterinfo.currentmove == &soldier_move_trip ||
        self->monsterinfo.currentmove == &soldier_move_attack5 ||
        self->monsterinfo.currentmove == &soldier_move_pain4)
        return false;

    // their `self->count <= 3` is "not the machinegun/lasergun": keep firing
    // from the run rather than breaking off.  This read self->count directly,
    // which no soldier in this tree ever sets - so it was always 0, always
    // true, and the machinegun soldier run-and-shot too.  count maps to
    // s.skinnum here; see the header comment.
    if (self->s.skinnum <= 3) {
        if (self->monsterinfo.currentmove != &soldier_move_attack6) {
            self->monsterinfo.currentmove = &soldier_move_attack6;
            soldierh_hyper_laser_sound_end(self);
        }
    } else {
        if (self->monsterinfo.currentmove != &soldier_move_start_run &&
            self->monsterinfo.currentmove != &soldier_move_run) {
            self->monsterinfo.currentmove = &soldier_move_start_run;
            soldierh_hyper_laser_sound_end(self);
        }
    }

    return true;
}

/*
=================
soldier_dodge

monsterinfo.dodge for every soldier, in both games.

The rerelease hands straight over to M_MonsterDodge, which drives the duck +
sidestep pair above.  The ORIGINAL game gets id's 1997 dodge back verbatim -
a flat 25% chance, then a skill-weighted pick between the plain crouch and the
crouched return fire.  Anything else changes how baseq2 plays.

(This symbol also has to keep existing: g_ptrs_compat_v2.c is a frozen table
for version-2 saves and names it.)
=================
*/
void soldier_dodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    float   r;

    if (M_RereleaseGame()) {
        M_MonsterDodge(self, attacker, eta, tr, gravity);
        return;
    }

    r = random();
    if (r > 0.25f)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    if (skill->value == 0) {
        self->monsterinfo.currentmove = &soldier_move_duck;
        return;
    }

    // id wrote this to pause_framenum, which its own soldier_duck_hold read.
    // The shared monster_duck_hold reads duck_wait_framenum, so set both.
    self->monsterinfo.pause_framenum = level.framenum + (eta + 0.3f) * BASE_FRAMERATE;
    self->monsterinfo.duck_wait_framenum = self->monsterinfo.pause_framenum;
    r = random();

    if (skill->value == 1) {
        if (r > 0.33f)
            self->monsterinfo.currentmove = &soldier_move_duck;
        else
            self->monsterinfo.currentmove = &soldier_move_attack3;
        return;
    }

    if (skill->value >= 2) {
        if (r > 0.66f)
            self->monsterinfo.currentmove = &soldier_move_duck;
        else
            self->monsterinfo.currentmove = &soldier_move_attack3;
        return;
    }

    self->monsterinfo.currentmove = &soldier_move_attack3;
}


//
// DEATH
//

/*
=================
soldier_blocked

monsterinfo.blocked.  The soldier has no jump animations, so unlike the gunner
this is the plat half only - it rides a moving platform instead of walking into
its edge.  Nothing while it is already dodging or ducked.
=================
*/
bool soldier_blocked(edict_t *self, float dist)
{
    if ((self->monsterinfo.aiflags & AI_DODGING) || (self->monsterinfo.aiflags & AI_DUCKED))
        return false;

    return blocked_checkplat(self, dist);
}

void soldier_fire6(edict_t *self)
{
    soldier_fire(self, 5);

    // a shotgun soldier shot in the middle of its death fires once and then
    // skips the rest of the reload it can no longer finish
    if (M_RereleaseGame() && self->dmg)
        self->monsterinfo.nextframe = FRAME_death126;
}

void soldier_fire7(edict_t *self)
{
    soldier_fire(self, 6);
}

void soldier_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

mframe_t soldier_frames_death1 [] = {
    { ai_move, 0,   NULL },
    { ai_move, -10, NULL },
    { ai_move, -10, NULL },
    { ai_move, -10, soldier_death_shrink },
    { ai_move, -5,  NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   soldier_fire6 },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   soldier_fire7 },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_death1 = {FRAME_death101, FRAME_death136, soldier_frames_death1, soldier_dead};

mframe_t soldier_frames_death2 [] = {
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, 0, soldier_death_shrink },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_death2 = {FRAME_death201, FRAME_death235, soldier_frames_death2, soldier_dead};

mframe_t soldier_frames_death3 [] = {
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, 0, soldier_death_shrink },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
};
mmove_t soldier_move_death3 = {FRAME_death301, FRAME_death345, soldier_frames_death3, soldier_dead};

mframe_t soldier_frames_death4 [] = {
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0, soldier_death_shrink },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_death4 = {FRAME_death401, FRAME_death453, soldier_frames_death4, soldier_dead};

mframe_t soldier_frames_death5 [] = {
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, -5,  NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0, soldier_death_shrink },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },

    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_death5 = {FRAME_death501, FRAME_death524, soldier_frames_death5, soldier_dead};

mframe_t soldier_frames_death6 [] = {
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0, soldier_death_shrink },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL },
    { ai_move, 0,   NULL }
};
mmove_t soldier_move_death6 = {FRAME_death601, FRAME_death610, soldier_frames_death6, soldier_dead};

void soldier_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

    // a hyper-soldier killed mid-burst would otherwise loop its spin-up sound
    // forever - s.sound is sticky until something clears it
    self->s.sound = 0;

// check for gib

    if (self->health <= self->gib_health) {
        // Stock Quake II: one burst of gibs and the body is gone.
        if (!LUDICROUS_GIBS()) {
            gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
            for (n = 0; n < 3; n++)
                ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowGib(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
            ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
            self->deadflag = DEAD_DEAD;
            return;
        }

        // LUDICROUS GIBS: the burst scales with what killed it, and the
        // corpse is left shootable so it can be torn down in stages.
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NONE, 0);     		
		
		if (InflictorGibExplosion(inflictor, self)) {
			
			VectorScale(self->size, 1.2, self->size);

			for (n = 0; n < 16; n++) {
				if (n < 8) {
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
				}
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
				for (n = 0; n < 8; n++) {
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
				}
				
				ThrowGibNoExplode(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
				ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
				self->takedamage = DAMAGE_NO;
			}			
		}
		else {			
			if(!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_machinegun")) {
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
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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
	// 
    if (self->deadflag == DEAD_DEAD) 
        return;

// regular death
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->s.skinnum |= 1;

    if (self->s.skinnum == 1)
        gi.sound(self, CHAN_VOICE, sound_death_light, 1, ATTN_NORM, 0);
    else if (self->s.skinnum == 3)
        gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    else // (self->s.skinnum == 5)
        gi.sound(self, CHAN_VOICE, sound_death_ss, 1, ATTN_NORM, 0);

    if (fabsf((self->s.origin[2] + self->viewheight) - point[2]) <= 4) {
        // head shot
        self->monsterinfo.currentmove = &soldier_move_death3;
        return;
    }

    n = Q_rand() % 5;
    if (n == 0)
        self->monsterinfo.currentmove = &soldier_move_death1;
    else if (n == 1)
        self->monsterinfo.currentmove = &soldier_move_death2;
    else if (n == 2)
        self->monsterinfo.currentmove = &soldier_move_death4;
    else if (n == 3)
        self->monsterinfo.currentmove = &soldier_move_death5;
    else
        self->monsterinfo.currentmove = &soldier_move_death6;
}


//
// SPAWN
//

void SP_monster_soldier_x(edict_t *self)
{

    self->s.modelindex = gi.modelindex("models/monsters/soldier/tris.md2");
    self->monsterinfo.scale = MODEL_SCALE;
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 32);
    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    sound_idle =    gi.soundindex("soldier/solidle1.wav");
    sound_sight1 =  gi.soundindex("soldier/solsght1.wav");
    sound_sight2 =  gi.soundindex("soldier/solsrch1.wav");
    sound_cock =    gi.soundindex("infantry/infatck3.wav");

    self->mass = 100;

    self->pain = soldier_pain;
    self->die = soldier_die;

    self->monsterinfo.stand = soldier_stand;

    // SPAWNFLAG_SOLDIER_BLIND (8) - stands with the unaware idle instead.
    // Rerelease-only: bit 8 is unused by the original game's soldiers, and a
    // baseq2 map that happens to set it must keep the normal idle.
    if (M_RereleaseGame() && (self->spawnflags & 8))
        self->monsterinfo.stand = soldier_blind;
    self->monsterinfo.walk = soldier_walk;
    self->monsterinfo.run = soldier_run;
    // soldier_dodge is the classic dodge in baseq2 and forwards to
    // M_MonsterDodge in the rerelease.  duck/sidestep are what M_MonsterDodge
    // drives, so the original game must not advertise them at all.
    self->monsterinfo.dodge = soldier_dodge;
    if (M_RereleaseGame()) {
        self->monsterinfo.duck = soldier_duck;
        self->monsterinfo.unduck = monster_duck_up;
        self->monsterinfo.sidestep = soldier_sidestep;
        self->monsterinfo.blocked = soldier_blocked;
        // PMM - shoot at where you last saw them through a wall.  attack1 is
        // the blind volley; soldier_blind_check on its first frame does the
        // aiming.  Named `blindfire` after the rerelease's own opt-in field.
        self->monsterinfo.blindfire = true;
    }
    self->monsterinfo.attack = soldier_attack;
    self->monsterinfo.melee = NULL;
    self->monsterinfo.sight = soldier_sight;

    gi.linkentity(self);

    self->monsterinfo.stand(self);

    walkmonster_start(self);
}


/*QUAKED monster_soldier_light (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier_light(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_x(self);

	float val = crandom();

	if (val < 0) {
		self->monsterFireHyperBlaster = qtrue;
	}
	else {
		self->monsterFireHyperBlaster = qfalse;
	}

    sound_pain_light = gi.soundindex("soldier/solpain2.wav");
    sound_death_light = gi.soundindex("soldier/soldeth2.wav");
    gi.modelindex("models/objects/laser/tris.md2");
    gi.soundindex("misc/lasfly.wav");
    gi.soundindex("soldier/solatck2.wav");

    self->s.skinnum = 0;
    self->health = 20;
    self->gib_health = -30;
}

/*QUAKED monster_soldier (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_x(self);

    sound_pain = gi.soundindex("soldier/solpain1.wav");
    sound_death = gi.soundindex("soldier/soldeth1.wav");
    gi.soundindex("soldier/solatck1.wav");

    self->s.skinnum = 2;
    self->health = 30;
    self->gib_health = -30;
}

/*QUAKED monster_soldier_ss (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier_ss(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_x(self);

    sound_pain_ss = gi.soundindex("soldier/solpain3.wav");
    sound_death_ss = gi.soundindex("soldier/soldeth3.wav");
    gi.soundindex("soldier/solatck3.wav");

    self->s.skinnum = 4;
    self->health = 40;
    self->gib_health = -30;
}


/*
=================
The Xatrix "soldierh" family, used throughout the rerelease Call of the Machine
maps. models/monsters/soldierh is a pure reskin of the stock soldier - identical
475 frames and 434 triangles - so these reuse every soldier animation and differ
only in model, skin, health and weapon. style is set to 1 to tell soldier_fire()
which family this is, since the skin numbers overlap with the stock soldier's;
the rerelease marks them the same way. No map sets "style" on a soldier, so
there is nothing to collide with.
=================
*/
static void SP_monster_soldier_h(edict_t *self)
{
    SP_monster_soldier_x(self);

    self->s.modelindex = gi.modelindex("models/monsters/soldierh/tris.md2");
    self->style = 1;
}

/*QUAKED monster_soldier_ripper (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier_ripper(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_h(self);

    sound_pain_light = gi.soundindex("soldier/solpain2.wav");
    sound_death_light = gi.soundindex("soldier/soldeth2.wav");

    gi.modelindex("models/objects/boomrang/tris.md2");
    gi.soundindex("misc/lasfly.wav");
    gi.soundindex("soldier/solatck2.wav");

    self->s.skinnum = 0;
    self->health = 50;
    self->gib_health = -30;
}

/*QUAKED monster_soldier_hypergun (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier_hypergun(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_h(self);

    gi.modelindex("models/objects/blaser/tris.md2");
    sound_pain = gi.soundindex("soldier/solpain1.wav");
    sound_death = gi.soundindex("soldier/soldeth1.wav");
    gi.soundindex("soldier/solatck1.wav");
    gi.soundindex("misc/lasfly.wav");

    self->s.skinnum = 2;
    self->health = 60;
    self->gib_health = -30;
}

/*QUAKED monster_soldier_lasergun (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_soldier_lasergun(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_soldier_h(self);

    sound_pain_ss = gi.soundindex("soldier/solpain3.wav");
    sound_death_ss = gi.soundindex("soldier/soldeth3.wav");
    gi.soundindex("soldier/solatck3.wav");
    gi.soundindex("misc/lasfly.wav");

    self->s.skinnum = 4;
    self->health = 70;
    self->gib_health = -30;
}
