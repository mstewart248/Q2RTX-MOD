/*
Copyright (c) ZeniMax Media Inc.
Licensed under the GNU General Public License 2.0.
*/
/*
==============================================================================

GUN COMMANDER

The rerelease's heavy gunner. It is the GUNNER MODEL at skin 2 (cskin) using
the appended c_* animations - so it needs the 799-frame rerelease gunner md2,
and this tree ships the 1997 209-frame one. SP_monster_guncmdr therefore falls
back to spawning a plain monster_gunner unless md5 models are on
(M_RereleaseAnims), which is where those frames actually exist.

Ported from src/rerelease/m_guncmdr.cpp. Divergences from id's source, all
because the feature does not exist in this tree, are commented inline:
  * no per-entity scale, so it renders gunner-sized rather than 1.25x
  * no blindfire (id leaves the commander's own blindfire commented out too)
  * no `bad_area` (rogue tesla zones)
  * generic gibs instead of the per-monster skinned set

==============================================================================
*/

#include "g_local.h"
#include "m_gunner.h"

#define SPAWNFLAG_GUNCMDR_NOJUMPING     8

void SP_monster_gunner(edict_t *self);
void guncmdr_setskin(edict_t *self);

static int sound_pain;
static int sound_pain2;
static int sound_death;
static int sound_idle;
static int sound_open;
static int sound_search;
static int sound_sight;

void guncmdr_idlesound(edict_t *self)
{
	gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

void guncmdr_sight(edict_t *self, edict_t *other)
{
	gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void guncmdr_search(edict_t *self)
{
	gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
}

void GunnerGrenade(edict_t *self);
void GunnerFire(edict_t *self);
void guncmdr_fire_chain(edict_t *self);
void guncmdr_refire_chain(edict_t *self);

void guncmdr_stand(edict_t *self);

mframe_t guncmdr_frames_fidget[] = {
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_idlesound },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_idlesound },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
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
mmove_t guncmdr_move_fidget = { FRAME_c_stand201, FRAME_c_stand254, guncmdr_frames_fidget, guncmdr_stand };

void guncmdr_fidget(edict_t *self)
{
	if (self->monsterinfo.aiflags & AI_STAND_GROUND)
		return;
	else if (self->enemy)
		return;
	if (random() <= 0.05f)
		self->monsterinfo.currentmove = &guncmdr_move_fidget;
}

mframe_t guncmdr_frames_stand[] = {
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_fidget },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_fidget },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_fidget },

	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, guncmdr_fidget }
};
mmove_t guncmdr_move_stand = { FRAME_c_stand101, FRAME_c_stand140, guncmdr_frames_stand, NULL };

void guncmdr_stand(edict_t *self)
{
	self->monsterinfo.currentmove = &guncmdr_move_stand;
}

mframe_t guncmdr_frames_walk[] = {
	{ ai_walk, 1.5f, NULL },
	{ ai_walk, 2.5f, NULL },
	{ ai_walk, 3.0f, NULL },
	{ ai_walk, 2.5f, NULL },
	{ ai_walk, 2.3f, NULL },
	{ ai_walk, 3.0f, NULL },
	{ ai_walk, 2.8f, NULL },
	{ ai_walk, 3.6f, NULL },
	{ ai_walk, 2.8f, NULL },
	{ ai_walk, 2.5f, NULL },

	{ ai_walk, 2.3f, NULL },
	{ ai_walk, 4.3f, NULL },
	{ ai_walk, 3.0f, NULL },
	{ ai_walk, 1.5f, NULL },
	{ ai_walk, 2.5f, NULL },
	{ ai_walk, 3.3f, NULL },
	{ ai_walk, 2.8f, NULL },
	{ ai_walk, 3.0f, NULL },
	{ ai_walk, 2.0f, NULL },
	{ ai_walk, 2.0f, NULL },

	{ ai_walk, 3.3f, NULL },
	{ ai_walk, 3.6f, NULL },
	{ ai_walk, 3.4f, NULL },
	{ ai_walk, 2.8f, NULL },
};
mmove_t guncmdr_move_walk = { FRAME_c_walk101, FRAME_c_walk124, guncmdr_frames_walk, NULL };

void guncmdr_walk(edict_t *self)
{
	self->monsterinfo.currentmove = &guncmdr_move_walk;
}

mframe_t guncmdr_frames_run[] = {
	{ ai_run, 15.f, monster_done_dodge },
	{ ai_run, 16.f, NULL },
	{ ai_run, 20.f, NULL },
	{ ai_run, 18.f, NULL },
	{ ai_run, 24.f, NULL },
	{ ai_run, 13.5f, NULL }
};

mmove_t guncmdr_move_run = { FRAME_c_run101, FRAME_c_run106, guncmdr_frames_run, NULL };

void guncmdr_run(edict_t *self)
{
	monster_done_dodge(self);
	if (self->monsterinfo.aiflags & AI_STAND_GROUND)
		self->monsterinfo.currentmove = &guncmdr_move_stand;
	else
		self->monsterinfo.currentmove = &guncmdr_move_run;
}

// standing pains

mframe_t guncmdr_frames_pain1[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
};
mmove_t guncmdr_move_pain1 = { FRAME_c_pain101, FRAME_c_pain104, guncmdr_frames_pain1, guncmdr_run };

mframe_t guncmdr_frames_pain2[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_pain2 = { FRAME_c_pain201, FRAME_c_pain204, guncmdr_frames_pain2, guncmdr_run };

mframe_t guncmdr_frames_pain3[] = {
	{ ai_move, -3.0f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
};
mmove_t guncmdr_move_pain3 = { FRAME_c_pain301, FRAME_c_pain304, guncmdr_frames_pain3, guncmdr_run };

mframe_t guncmdr_frames_pain4[] = {
	{ ai_move, -17.1f, NULL },
	{ ai_move, -3.2f, NULL },
	{ ai_move, 0.9f, NULL },
	{ ai_move, 3.6f, NULL },
	{ ai_move, -2.6f, NULL },
	{ ai_move, 1.0f, NULL },
	{ ai_move, -5.1f, NULL },
	{ ai_move, -6.7f, NULL },
	{ ai_move, -8.8f, NULL },
	{ ai_move, 0, NULL },

	{ ai_move, 0, NULL },
	{ ai_move, -2.1f, NULL },
	{ ai_move, -2.3f, NULL },
	{ ai_move, -2.5f, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_pain4 = { FRAME_c_pain401, FRAME_c_pain415, guncmdr_frames_pain4, guncmdr_run };

void guncmdr_dead(edict_t *);

mframe_t guncmdr_frames_death1[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 4.0f, NULL }, // scoot
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
mmove_t guncmdr_move_death1 = { FRAME_c_death101, FRAME_c_death118, guncmdr_frames_death1, guncmdr_dead };

void guncmdr_pain5_to_death1(edict_t *self)
{
	if (self->health < 0)
		self->monsterinfo.currentmove = &guncmdr_move_death1;
}

mframe_t guncmdr_frames_death2[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_death2 = { FRAME_c_death201, FRAME_c_death204, guncmdr_frames_death2, guncmdr_dead };

void guncmdr_pain5_to_death2(edict_t *self)
{
	if (self->health < 0 && (random() < 0.5f))
		self->monsterinfo.currentmove = &guncmdr_move_death2;
}

mframe_t guncmdr_frames_pain5[] = {
	{ ai_move, -29.f, NULL },
	{ ai_move, -5.f, NULL },
	{ ai_move, -5.f, NULL },
	{ ai_move, -3.f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, guncmdr_pain5_to_death2 },
	{ ai_move, 9.f, NULL },
	{ ai_move, 3.f, NULL },
	{ ai_move, 0, guncmdr_pain5_to_death1 },
	{ ai_move, 0, NULL },

	{ ai_move, 0, NULL },
	{ ai_move, -4.6f, NULL },
	{ ai_move, -4.8f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 9.5f, NULL },
	{ ai_move, 3.4f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },

	{ ai_move, -2.4f, NULL },
	{ ai_move, -9.0f, NULL },
	{ ai_move, -5.0f, NULL },
	{ ai_move, -3.6f, NULL },
};
mmove_t guncmdr_move_pain5 = { FRAME_c_pain501, FRAME_c_pain524, guncmdr_frames_pain5, guncmdr_run };

void guncmdr_dead(edict_t *self)
{
    // The rerelease scales these by s.scale (the commander is the gunner at
    // 1.25). This protocol carries no per-entity scale, so the corpse box is
    // used unscaled - see M_ProjectFlashSource in g_monster.c.
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);

    // monster_dead() is a rerelease helper; this tree does the same work inline
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void guncmdr_shrink(edict_t *self)
{
    self->maxs[2] = -4;
	self->svflags |= SVF_DEADMONSTER;
	gi.linkentity(self);
}

mframe_t guncmdr_frames_death6[] = {
	{ ai_move, 0, guncmdr_shrink },
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
mmove_t guncmdr_move_death6 = { FRAME_c_death601, FRAME_c_death614, guncmdr_frames_death6, guncmdr_dead };

static void guncmdr_pain6_to_death6(edict_t *self)
{
	if (self->health < 0)
		self->monsterinfo.currentmove = &guncmdr_move_death6;
}

mframe_t guncmdr_frames_pain6[] = {
	{ ai_move, 16.f, NULL },
	{ ai_move, 16.f, NULL },
	{ ai_move, 12.f, NULL },
	{ ai_move, 5.5f, monster_duck_down },
	{ ai_move, 3.0f, NULL },
	{ ai_move, -4.7f, NULL },
	{ ai_move, -6.0f, guncmdr_pain6_to_death6 },
	{ ai_move, 0, NULL },
	{ ai_move, 1.8f, NULL },
	{ ai_move, 0.7f, NULL },

	{ ai_move, 0, NULL },
	{ ai_move, -2.1f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	
	{ ai_move, 0, NULL },
	{ ai_move, -6.1f, NULL },
	{ ai_move, 10.5f, NULL },
	{ ai_move, 4.3f, NULL },
	{ ai_move, 4.7f, monster_duck_up },
	{ ai_move, 1.4f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, -3.2f, NULL },
	{ ai_move, 2.3f, NULL },
	{ ai_move, -4.4f, NULL },

	{ ai_move, -4.4f, NULL },
	{ ai_move, -2.4f, NULL }
};
mmove_t guncmdr_move_pain6 = { FRAME_c_pain601, FRAME_c_pain632, guncmdr_frames_pain6, guncmdr_run };

mframe_t guncmdr_frames_pain7[] = {
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
mmove_t guncmdr_move_pain7 = { FRAME_c_pain701, FRAME_c_pain714, guncmdr_frames_pain7, guncmdr_run };

extern mmove_t guncmdr_move_jump;
extern mmove_t guncmdr_move_jump2;
extern mmove_t guncmdr_move_duck_attack;

bool guncmdr_sidestep(edict_t *self);

void guncmdr_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    vec3_t  forward, dif;
    int     r;

    monster_done_dodge(self);

	if (self->monsterinfo.currentmove == &guncmdr_move_jump || 
		self->monsterinfo.currentmove == &guncmdr_move_jump2 ||
		self->monsterinfo.currentmove == &guncmdr_move_duck_attack)
		return;

    if (level.framenum < self->pain_debounce_framenum)
	{
		if (random() < 0.3)
			self->monsterinfo.dodge(self, other, FRAMETIME, NULL, false);

		return;
	}

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

	if ((random() < 0.5f))
		gi.sound(self, CHAN_VOICE, sound_pain, 1, ATTN_NORM, 0);
	else
		gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);

    // M_ShouldReactToPain does not exist here; skill 3 is the nightmare gate
    // every other monster in this tree uses for the same purpose.
    if (skill->value >= 3)
	{
		if (random() < 0.3)
			self->monsterinfo.dodge(self, other, FRAMETIME, NULL, false);

		return; // no pain anims in nightmare
	}

    AngleVectors(self->s.angles, forward, NULL, NULL);

    VectorSubtract(other->s.origin, self->s.origin, dif);
    dif[2] = 0;
    VectorNormalize(dif);

	// small pain
	if (damage < 35)
	{
		r = (Q_rand() % (4));

		if (r == 0)
			self->monsterinfo.currentmove = &guncmdr_move_pain3;
		else if (r == 1)
			self->monsterinfo.currentmove = &guncmdr_move_pain2;
		else if (r == 2)
			self->monsterinfo.currentmove = &guncmdr_move_pain1;
		else
			self->monsterinfo.currentmove = &guncmdr_move_pain7;
	}
	// large pain from behind (aka Paril)
    else if (DotProduct(dif, forward) < -0.40f)
	{
		self->monsterinfo.currentmove = &guncmdr_move_pain6;

        self->pain_debounce_framenum += 1.5f * BASE_FRAMERATE;
    }
    else
    {
		if ((random() < 0.5f))
			self->monsterinfo.currentmove = &guncmdr_move_pain4;
		else
			self->monsterinfo.currentmove = &guncmdr_move_pain5;

        self->pain_debounce_framenum += 1.5f * BASE_FRAMERATE;
    }

    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;

	// PMM - clear duck flag
	if (self->monsterinfo.aiflags & AI_DUCKED)
		monster_duck_up(self);
}

void guncmdr_setskin(edict_t *self)
{
	if (self->health < (self->max_health / 2))
		self->s.skinnum |= 1;
	else
		self->s.skinnum &= ~1;
}

mframe_t guncmdr_frames_death3[] = {
	{ ai_move, 20.f, NULL },
	{ ai_move, 10.f, NULL },
	{ ai_move, 10.f, guncmdr_shrink },
	{ ai_move, 0.f, NULL },
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
mmove_t guncmdr_move_death3 = { FRAME_c_death301, FRAME_c_death321, guncmdr_frames_death3, guncmdr_dead };

mframe_t guncmdr_frames_death7[] = {
	{ ai_move, 30.f, NULL },
	{ ai_move, 20.f, NULL },
	{ ai_move, 16.f, guncmdr_shrink },
	{ ai_move, 5.f, NULL },
	{ ai_move, -6.f, NULL },
	{ ai_move, -7.f, NULL },
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
	{ ai_move, 0.f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0.f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
};
mmove_t guncmdr_move_death7 = { FRAME_c_death701, FRAME_c_death730, guncmdr_frames_death7, guncmdr_dead };

mframe_t guncmdr_frames_death4[] = {
	{ ai_move, -20.f, NULL },
	{ ai_move, -16.f, NULL },
	{ ai_move, -26.f, guncmdr_shrink },
	{ ai_move, 0.f, NULL },
	{ ai_move, -12.f, NULL },
	{ ai_move, 16.f, NULL },
	{ ai_move, 9.2f, NULL },
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
mmove_t guncmdr_move_death4 = { FRAME_c_death401, FRAME_c_death436, guncmdr_frames_death4, guncmdr_dead };

mframe_t guncmdr_frames_death5[] = {
	{ ai_move, -14.f, NULL },
	{ ai_move, -2.7f, NULL },
	{ ai_move, -2.5f, NULL },
	{ ai_move, -4.6f, NULL },
	{ ai_move, -4.0f, NULL },
	{ ai_move, -1.5f, NULL },
	{ ai_move, 2.3f, NULL },
	{ ai_move, 2.5f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 3.5f, NULL },
	{ ai_move, 12.9f, NULL },
	{ ai_move, 3.8f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	
	{ ai_move, -2.1f, NULL },
	{ ai_move, -1.3f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 3.4f, NULL },
	{ ai_move, 5.7f, NULL },
	{ ai_move, 11.2f, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_death5 = { FRAME_c_death501, FRAME_c_death528, guncmdr_frames_death5, guncmdr_dead };

void guncmdr_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    vec3_t  forward, dif;
    int     n, r;

    // check for gib
    if (self->health <= self->gib_health) {
		gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);

        // commander skins are 2/3; halving lands back on the gunner's 0/1,
        // which is what the gib models are skinned for
        self->s.skinnum /= 2;

        // The rerelease throws per-monster skinned gibs (chest/garm/gun/foot).
        // Those models are not shipped here, so this uses the generic set the
        // rest of this tree's monsters use - same shape, stock assets.
        for (n = 0; n < 2; n++)
            ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
        for (n = 0; n < 4; n++)
            ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
        ThrowGib(self, "models/objects/gibs/gear/tris.md2", damage, GIB_METALLIC);
        ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);

        self->deadflag = DEAD_DEAD;
        return;
    }

    if (self->deadflag == DEAD_DEAD)
        return;

    // regular death
    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

	// these animations cleanly transitions to death, so just keep going
	if (self->monsterinfo.currentmove == &guncmdr_move_pain5 &&
		self->s.frame < FRAME_c_pain508)
		return;
	else if (self->monsterinfo.currentmove == &guncmdr_move_pain6 &&
		self->s.frame < FRAME_c_pain607)
		return;

    AngleVectors(self->s.angles, forward, NULL, NULL);

    VectorSubtract(inflictor->s.origin, self->s.origin, dif);
    dif[2] = 0;
    VectorNormalize(dif);

    // decapitated - the shot came in level with the head
    if (fabsf((self->s.origin[2] + self->viewheight) - point[2]) <= 4 &&
        self->velocity[2] < 65.0f) {
        // The rerelease detaches a separate head gib model here. That model is
        // not shipped in this tree, so the death animation carries it instead.
        self->monsterinfo.currentmove = &guncmdr_move_death5;
    }
    // damage came from behind; use a backwards death
    else if (DotProduct(dif, forward) < -0.40f)
	{
		r = (Q_rand() % (self->monsterinfo.currentmove == &guncmdr_move_pain6 ? 2 : 3));

		if (r == 0)
			self->monsterinfo.currentmove = &guncmdr_move_death3;
		else if (r == 1)
			self->monsterinfo.currentmove = &guncmdr_move_death7;
		else if (r == 2)
			self->monsterinfo.currentmove = &guncmdr_move_pain6;
	}
	else
	{
		r = (Q_rand() % (self->monsterinfo.currentmove == &guncmdr_move_pain5 ? 1 : 2));

		if (r == 0)
			self->monsterinfo.currentmove = &guncmdr_move_death4;
		else
			self->monsterinfo.currentmove = &guncmdr_move_pain5;
	}
}

void guncmdr_opengun(edict_t *self)
{
	gi.sound(self, CHAN_VOICE, sound_open, 1, ATTN_IDLE, 0);
}

void GunnerCmdrFire(edict_t *self)
{
	vec3_t					 start;
	vec3_t					 forward, right;
    vec3_t  aim;
    int     flash_number;
    int     i;

    if (!self->enemy || !self->enemy->inuse) // PGM
		return;								 // PGM

	if (self->s.frame >= FRAME_c_attack401 && self->s.frame <= FRAME_c_attack505)
		flash_number = MZ2_GUNCMDR_CHAINGUN_2;
	else
		flash_number = MZ2_GUNCMDR_CHAINGUN_1;

	AngleVectors(self->s.angles, forward, right, NULL);
	M_ProjectFlashSource(self, monster_flash_offset[flash_number], forward, right, start);
	PredictAim(self->enemy, start, 800, false, random() * 0.3f, aim, NULL);
	for (i = 0; i < 3; i++)
		aim[i] += crandom() * 0.025f;
	monster_fire_flechette(self, start, aim, 4, 800, flash_number);
}

mframe_t guncmdr_frames_attack_chain[] = {
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, guncmdr_opengun },
	{ ai_charge, 0, NULL }
};
mmove_t guncmdr_move_attack_chain = { FRAME_c_attack101, FRAME_c_attack106, guncmdr_frames_attack_chain, guncmdr_fire_chain };

mframe_t guncmdr_frames_fire_chain[] = {
	{ ai_charge, 0, GunnerCmdrFire },
	{ ai_charge, 0, GunnerCmdrFire },
	{ ai_charge, 0, GunnerCmdrFire },
	{ ai_charge, 0, GunnerCmdrFire },
	{ ai_charge, 0, GunnerCmdrFire },
	{ ai_charge, 0, GunnerCmdrFire }
};
mmove_t guncmdr_move_fire_chain = { FRAME_c_attack107, FRAME_c_attack112, guncmdr_frames_fire_chain, guncmdr_refire_chain };

mframe_t guncmdr_frames_fire_chain_run[] = {
	{ ai_charge, 15.f, GunnerCmdrFire },
	{ ai_charge, 16.f, GunnerCmdrFire },
	{ ai_charge, 20.f, GunnerCmdrFire },
	{ ai_charge, 18.f, GunnerCmdrFire },
	{ ai_charge, 24.f, GunnerCmdrFire },
	{ ai_charge, 13.5f, GunnerCmdrFire }
};
mmove_t guncmdr_move_fire_chain_run = { FRAME_c_run201, FRAME_c_run206, guncmdr_frames_fire_chain_run, guncmdr_refire_chain };

mframe_t guncmdr_frames_fire_chain_dodge_right[] = {
	{ ai_charge, 5.1f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 9.0f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 3.5f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 3.6f * 2.0f, GunnerCmdrFire },
	{ ai_charge, -1.0f * 2.0f, GunnerCmdrFire }
};
mmove_t guncmdr_move_fire_chain_dodge_right = { FRAME_c_attack401, FRAME_c_attack405, guncmdr_frames_fire_chain_dodge_right, guncmdr_refire_chain };

mframe_t guncmdr_frames_fire_chain_dodge_left[] = {
	{ ai_charge, 5.1f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 9.0f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 3.5f * 2.0f, GunnerCmdrFire },
	{ ai_charge, 3.6f * 2.0f, GunnerCmdrFire },
	{ ai_charge, -1.0f * 2.0f, GunnerCmdrFire }
};
mmove_t guncmdr_move_fire_chain_dodge_left = { FRAME_c_attack501, FRAME_c_attack505, guncmdr_frames_fire_chain_dodge_left, guncmdr_refire_chain };

mframe_t guncmdr_frames_endfire_chain[] = {
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, guncmdr_opengun },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL }
};
mmove_t guncmdr_move_endfire_chain = { FRAME_c_attack118, FRAME_c_attack124, guncmdr_frames_endfire_chain, guncmdr_run };

static const float MORTAR_SPEED = 850.f;
static const float GRENADE_SPEED = 600.f;

/*
=================
GunnerCmdrGrenade

The commander's three grenade throws, told apart by which frame called us:
mortar (lobbed high), front (thrown flat) and crouch. The crouch variant is not
a grenade at all - it fires a spread of ionripper bolts.

The mortar and front throws ask M_CalculatePitchToFire for an arc that actually
lands on the target; if none exists it falls back to a flat throw with upward
bias, which is what id does.
=================
*/
static bool guncmdr_flash_is(int flash, int lo, int hi)
{
    return flash >= lo && flash <= hi;
}

void GunnerCmdrGrenade(edict_t *self)
{
    vec3_t  start, forward, right, up, aim, target, v;
    int     flash_number;
    float   spread;
    float   pitch = 0;
    float   dist, speed;
    int     i;
    bool    crouch, mortar;

    if (!self->enemy || !self->enemy->inuse)
        return;

    if (self->s.frame == FRAME_c_attack205) {
        spread = -0.1f; flash_number = MZ2_GUNCMDR_GRENADE_MORTAR_1;
    } else if (self->s.frame == FRAME_c_attack208) {
        spread = 0.0f;  flash_number = MZ2_GUNCMDR_GRENADE_MORTAR_2;
    } else if (self->s.frame == FRAME_c_attack211) {
        spread = 0.1f;  flash_number = MZ2_GUNCMDR_GRENADE_MORTAR_3;
    } else if (self->s.frame == FRAME_c_attack304) {
        spread = -0.1f; flash_number = MZ2_GUNCMDR_GRENADE_FRONT_1;
    } else if (self->s.frame == FRAME_c_attack307) {
        spread = 0.0f;  flash_number = MZ2_GUNCMDR_GRENADE_FRONT_2;
    } else if (self->s.frame == FRAME_c_attack310) {
        spread = 0.1f;  flash_number = MZ2_GUNCMDR_GRENADE_FRONT_3;
    } else if (self->s.frame == FRAME_c_attack911) {
        spread = 0.25f; flash_number = MZ2_GUNCMDR_GRENADE_CROUCH_1;
    } else if (self->s.frame == FRAME_c_attack912) {
        spread = 0.0f;  flash_number = MZ2_GUNCMDR_GRENADE_CROUCH_2;
    } else if (self->s.frame == FRAME_c_attack913) {
        spread = -0.25f; flash_number = MZ2_GUNCMDR_GRENADE_CROUCH_3;
    } else {
        return;     // not a firing frame
    }

    crouch = guncmdr_flash_is(flash_number, MZ2_GUNCMDR_GRENADE_CROUCH_1, MZ2_GUNCMDR_GRENADE_CROUCH_3);
    mortar = guncmdr_flash_is(flash_number, MZ2_GUNCMDR_GRENADE_MORTAR_1, MZ2_GUNCMDR_GRENADE_MORTAR_3);

    // The rerelease can fire blind at a remembered position (AI_MANUAL_STEERING
    // + blind_fire_target). This tree has no blind_fire_target, and id leaves
    // the commander's own blindfire commented out, so it always aims for real.
    VectorCopy(self->enemy->s.origin, target);

    AngleVectors(self->s.angles, forward, right, up);
    M_ProjectFlashSource(self, monster_flash_offset[flash_number], forward, right, start);

    if (!crouch) {
        VectorSubtract(target, self->s.origin, aim);
        dist = VectorLength(aim);

        // aim up if they are on our level but a long way off
        if (dist > 512 && aim[2] < 64 && aim[2] > -64)
            aim[2] += (dist - 512);

        VectorNormalize(aim);
        pitch = aim[2];
        if (pitch > 0.4f)
            pitch = 0.4f;
        else if (pitch < -0.5f)
            pitch = -0.5f;

        // lob harder when they are well above us
        if ((self->enemy->absmin[2] - self->absmax[2]) > 16.0f && mortar)
            pitch += 0.5f;
    }

    if (guncmdr_flash_is(flash_number, MZ2_GUNCMDR_GRENADE_FRONT_1, MZ2_GUNCMDR_GRENADE_FRONT_3))
        pitch -= 0.05f;

    if (!crouch) {
        VectorMA(forward, spread, right, aim);
        VectorMA(aim, pitch, up, aim);
        VectorNormalize(aim);
    } else {
        PredictAim(self->enemy, start, 800, false, 0.0f, aim, NULL);
        VectorMA(aim, spread, right, aim);
        VectorNormalize(aim);
    }

    if (crouch) {
        // not a grenade at all - a fan of ionripper bolts fired from the duck
        const float inner_spread = 0.125f;

        for (i = 0; i < 3; i++) {
            VectorMA(aim, -(inner_spread * 2) + (inner_spread * (i + 1)), right, v);
            fire_ionripper(self, start, v, 15, 800, EF_IONRIPPER);
        }

        gi.WriteByte(svc_muzzleflash2);
        gi.WriteShort(self - g_edicts);
        gi.WriteByte(flash_number);
        gi.multicast(start, MULTICAST_PVS);
    } else {
        speed = mortar ? MORTAR_SPEED : GRENADE_SPEED;

        // An arc that actually lands, or - failing that - a flat throw. The
        // rerelease passes extra right/up jitter to monster_fire_grenade; this
        // tree's takes no such arguments, so the aim vector carries it instead:
        // the fallback tilts upward to approximate their 200-unit up_adjust.
        if (!M_CalculatePitchToFire(self, target, start, aim, speed, 2.5f, mortar, false)) {
            aim[2] += 0.33f;
            VectorNormalize(aim);
        }

        monster_fire_grenade(self, start, aim, 50, speed, flash_number);
    }
}

mframe_t guncmdr_frames_attack_mortar[] = {
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, GunnerCmdrGrenade },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, GunnerCmdrGrenade },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },

	{ ai_charge, 0, GunnerCmdrGrenade },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, monster_duck_up },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL }
};
mmove_t guncmdr_move_attack_mortar = { FRAME_c_attack201, FRAME_c_attack221, guncmdr_frames_attack_mortar, guncmdr_run };

void guncmdr_grenade_mortar_resume(edict_t *self)
{
	self->monsterinfo.currentmove = &guncmdr_move_attack_mortar;
	self->monsterinfo.attack_state = AS_STRAIGHT;
	self->s.frame = self->count;
}

mframe_t guncmdr_frames_attack_mortar_dodge[] = {
	{ ai_charge, 11.f, NULL },
	{ ai_charge, 12.f, NULL },
	{ ai_charge, 16.f, NULL },
	{ ai_charge, 16.f, NULL },
	{ ai_charge, 12.f, NULL },
	{ ai_charge, 11.f, NULL }
};
mmove_t guncmdr_move_attack_mortar_dodge = { FRAME_c_duckstep01, FRAME_c_duckstep06, guncmdr_frames_attack_mortar_dodge, guncmdr_grenade_mortar_resume };

mframe_t guncmdr_frames_attack_back[] = {
	//{ ai_charge, 0, NULL },
	{ ai_charge, -2.f, NULL },
	{ ai_charge, -1.5f, NULL },
	{ ai_charge, -0.5f, GunnerCmdrGrenade },
	{ ai_charge, -6.0f, NULL },
	{ ai_charge, -4.f, NULL },
	{ ai_charge, -2.5f, GunnerCmdrGrenade },
	{ ai_charge, -7.0f, NULL },
	{ ai_charge, -3.5f, NULL },
	{ ai_charge, -1.1f, GunnerCmdrGrenade },

	{ ai_charge, -4.6f, NULL },
	{ ai_charge, 1.9f, NULL },
	{ ai_charge, 1.0f, NULL },
	{ ai_charge, -4.5f, NULL },
	{ ai_charge, 3.2f, NULL },
	{ ai_charge, 4.4f, NULL },
	{ ai_charge, -6.5f, NULL },
	{ ai_charge, -6.1f, NULL },
	{ ai_charge, 3.0f, NULL },
	{ ai_charge, -0.7f, NULL },
	{ ai_charge, -1.0f, NULL }
};
mmove_t guncmdr_move_attack_grenade_back = { FRAME_c_attack302, FRAME_c_attack321, guncmdr_frames_attack_back, guncmdr_run };

void guncmdr_grenade_back_dodge_resume(edict_t *self)
{
	self->monsterinfo.currentmove = &guncmdr_move_attack_grenade_back;
	self->monsterinfo.attack_state = AS_STRAIGHT;
	self->s.frame = self->count;
}

mframe_t guncmdr_frames_attack_grenade_back_dodge_right[] = {
	{ ai_charge, 5.1f * 2.0f, NULL },
	{ ai_charge, 9.0f * 2.0f, NULL },
	{ ai_charge, 3.5f * 2.0f, NULL },
	{ ai_charge, 3.6f * 2.0f, NULL },
	{ ai_charge, -1.0f * 2.0f, NULL }
};
mmove_t guncmdr_move_attack_grenade_back_dodge_right = { FRAME_c_attack601, FRAME_c_attack605, guncmdr_frames_attack_grenade_back_dodge_right, guncmdr_grenade_back_dodge_resume };

mframe_t guncmdr_frames_attack_grenade_back_dodge_left[] = {
	{ ai_charge, 5.1f * 2.0f, NULL },
	{ ai_charge, 9.0f * 2.0f, NULL },
	{ ai_charge, 3.5f * 2.0f, NULL },
	{ ai_charge, 3.6f * 2.0f, NULL },
	{ ai_charge, -1.0f * 2.0f, NULL }
};
mmove_t guncmdr_move_attack_grenade_back_dodge_left = { FRAME_c_attack701, FRAME_c_attack705, guncmdr_frames_attack_grenade_back_dodge_left, guncmdr_grenade_back_dodge_resume };

static void guncmdr_kick_finished(edict_t *self)
{
    self->monsterinfo.melee_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
	self->monsterinfo.attack(self);
}

static void guncmdr_kick(edict_t *self)
{
    vec3_t aim;

    VectorSet(aim, MELEE_DISTANCE, 0, -32);
    if (fire_hit(self, aim, 15, 400)) {
        // boot them into the air a little
        if (self->enemy && self->enemy->client && self->enemy->velocity[2] < 270.0f)
            self->enemy->velocity[2] = 270.0f;
	}
}

mframe_t guncmdr_frames_attack_kick[] = {
	{ ai_charge, -7.7f, NULL },
	{ ai_charge, -4.9f, NULL },
	{ ai_charge, 12.6f, guncmdr_kick },
	{ ai_charge, 0, NULL },
	{ ai_charge, -3.0f, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, -4.1f, NULL },
	{ ai_charge, 8.6f, NULL },
	//{ ai_charge, -3.5f, NULL }
};
mmove_t guncmdr_move_attack_kick = { FRAME_c_attack801, FRAME_c_attack808, guncmdr_frames_attack_kick, guncmdr_kick_finished };

// don't ever try grenades if we get this close
static const float RANGE_GRENADE = 100.f;

// always use mortar at this range
static const float RANGE_GRENADE_MORTAR = 525.f;

// at this range, run towards the enemy
static const float RANGE_CHAINGUN_RUN = 400.f;

/*
=================
guncmdr_try_lob

Would a grenade thrown from this muzzle actually land near the enemy? Wraps the
clear-shot test and the ballistic solve so guncmdr_attack stays readable.

`extra` is the caller's own precondition (range, height difference); false short
circuits the whole thing.
=================
*/
static bool guncmdr_try_lob(edict_t *self, int flash, const vec3_t forward, const vec3_t right,
                            float speed, bool mortar, bool extra)
{
    vec3_t start, aim;

    if (!extra)
        return false;

    if (!M_CheckClearShot(self, monster_flash_offset[flash], start))
        return false;

    M_ProjectFlashSource(self, monster_flash_offset[flash], forward, right, start);

    VectorSubtract(self->enemy->s.origin, self->s.origin, aim);
    VectorNormalize(aim);

    return M_CalculatePitchToFire(self, self->enemy->s.origin, start, aim,
                                  speed, 2.5f, mortar, false);
}

void guncmdr_attack(edict_t *self)
{
    vec3_t shot_start;

	monster_done_dodge(self);

	float d = realrange(self, self->enemy);

	vec3_t forward, right, aim;
	AngleVectors(self->s.angles, forward, right, NULL); // PGM

	// always use chaingun on tesla
	// kick close enemies
    // `bad_area` is rogue's "standing in a tesla/trap zone" flag, which this
    // tree has no concept of; it only ever forces the chaingun, so dropping it
    // just removes that special case.
    if (d < RANGE_MELEE && self->monsterinfo.melee_debounce_framenum < level.framenum)
        self->monsterinfo.currentmove = &guncmdr_move_attack_kick;
    else if ((d <= RANGE_GRENADE || (random() < 0.5f)) &&
             M_CheckClearShot(self, monster_flash_offset[MZ2_GUNCMDR_CHAINGUN_1], shot_start))
		self->monsterinfo.currentmove = &guncmdr_move_attack_chain;
    else if (guncmdr_try_lob(self, MZ2_GUNCMDR_GRENADE_MORTAR_1, forward, right,
                             MORTAR_SPEED, true,
                             d >= RANGE_GRENADE_MORTAR ||
                             fabsf(self->absmin[2] - self->enemy->absmax[2]) > 64.0f)) {
        // enemy far above or below us always gets the mortar
        self->monsterinfo.currentmove = &guncmdr_move_attack_mortar;
        monster_duck_down(self);
    }
    else if (!(self->monsterinfo.aiflags & AI_STAND_GROUND) &&
             guncmdr_try_lob(self, MZ2_GUNCMDR_GRENADE_FRONT_1, forward, right,
                             GRENADE_SPEED, false, true))
        self->monsterinfo.currentmove = &guncmdr_move_attack_grenade_back;
	else if (self->monsterinfo.aiflags & AI_STAND_GROUND)
		self->monsterinfo.currentmove = &guncmdr_move_attack_chain;
}

void guncmdr_fire_chain(edict_t *self)
{
	if (!(self->monsterinfo.aiflags & AI_STAND_GROUND) && self->enemy && realrange(self, self->enemy) > RANGE_CHAINGUN_RUN && ai_check_move(self, 8.0f))
		self->monsterinfo.currentmove = &guncmdr_move_fire_chain_run;
	else
		self->monsterinfo.currentmove = &guncmdr_move_fire_chain;
}

void guncmdr_refire_chain(edict_t *self)
{
	monster_done_dodge(self);
	self->monsterinfo.attack_state = AS_STRAIGHT;

	if (self->enemy->health > 0)
		if (visible(self, self->enemy))
			if (random() <= 0.5f)
			{
				if (!(self->monsterinfo.aiflags & AI_STAND_GROUND) && self->enemy && realrange(self, self->enemy) > RANGE_CHAINGUN_RUN && ai_check_move(self, 8.0f))
					self->monsterinfo.currentmove = &guncmdr_move_fire_chain_run;
				else
					self->monsterinfo.currentmove = &guncmdr_move_fire_chain;
				return;
			}
	self->monsterinfo.currentmove = &guncmdr_move_endfire_chain;
}

//===========
// PGM
void guncmdr_jump_now(edict_t *self)
{
	vec3_t forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 100, forward, self->velocity);
    VectorMA(self->velocity, 300, up, self->velocity);
}

void guncmdr_jump2_now(edict_t *self)
{
	vec3_t forward, up;

    AngleVectors(self->s.angles, forward, NULL, up);
    VectorMA(self->velocity, 150, forward, self->velocity);
    VectorMA(self->velocity, 400, up, self->velocity);
}

void guncmdr_jump_wait_land(edict_t *self)
{
	if (self->groundentity == NULL)
	{
		self->monsterinfo.nextframe = self->s.frame;

		if (monster_jump_finished(self))
			self->monsterinfo.nextframe = self->s.frame + 1;
	}
	else
		self->monsterinfo.nextframe = self->s.frame + 1;
}

mframe_t guncmdr_frames_jump[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, guncmdr_jump_now },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, guncmdr_jump_wait_land },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_jump = { FRAME_c_jump01, FRAME_c_jump10, guncmdr_frames_jump, guncmdr_run };

mframe_t guncmdr_frames_jump2[] = {
	{ ai_move, -8, NULL },
	{ ai_move, -4, NULL },
	{ ai_move, -4, NULL },
	{ ai_move, 0, guncmdr_jump2_now },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, guncmdr_jump_wait_land },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t guncmdr_move_jump2 = { FRAME_c_jump01, FRAME_c_jump10, guncmdr_frames_jump2, guncmdr_run };

void guncmdr_jump(edict_t *self, blocked_jump_result_t result)
{
	if (!self->enemy)
		return;

	monster_done_dodge(self);

	if (result == JUMP_JUMP_UP)
		self->monsterinfo.currentmove = &guncmdr_move_jump2;
	else
		self->monsterinfo.currentmove = &guncmdr_move_jump;
}

/*
=================
GunnerCmdrCounter

The commander's ground slam, reusing the berserk's TE_BERSERK_SLAM effect and
T_SlamRadiusDamage - both already in this tree from that port.
=================
*/
static void GunnerCmdrCounter(edict_t *self)
{
    vec3_t  f, r, offset, start;
    trace_t tr;

    AngleVectors(self->s.angles, f, r, NULL);
    VectorSet(offset, 20.0f, 0.0f, 14.0f);
    G_ProjectSource(self->s.origin, offset, f, r, start);
    tr = gi.trace(self->s.origin, NULL, NULL, start, self, MASK_SOLID);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_BERSERK_SLAM);
    gi.WritePosition(tr.endpos);
    gi.WriteDir(f);
    gi.multicast(tr.endpos, MULTICAST_PHS);

    T_SlamRadiusDamage(tr.endpos, self, self, 15, 250.0f, self, 200.0f, MOD_UNKNOWN);
}

//===========
// PGM
mframe_t guncmdr_frames_duck_attack[] = {
	{ ai_move, 3.6f, NULL },
	{ ai_move, 5.6f, monster_duck_down },
	{ ai_move, 8.4f, NULL },
	{ ai_move, 2.0f, monster_duck_hold },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },

	//{ ai_charge, 0, GunnerCmdrGrenade },
	//{ ai_charge, 9.5f, GunnerCmdrGrenade },
	//{ ai_charge, -1.5f, GunnerCmdrGrenade },
	
	{ ai_charge, 0, NULL },
	{ ai_charge, 9.5f, GunnerCmdrCounter },
	{ ai_charge, -1.5f, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, monster_duck_up },
	{ ai_charge, 0, NULL },
	{ ai_charge, 11.f, NULL },
	{ ai_charge, 2.0f, NULL },
	{ ai_charge, 5.6f, NULL }
};
mmove_t guncmdr_move_duck_attack = { FRAME_c_attack901, FRAME_c_attack919, guncmdr_frames_duck_attack, guncmdr_run };

bool guncmdr_duck(edict_t *self, float eta)
{
	if ((self->monsterinfo.currentmove == &guncmdr_move_jump2) ||
		(self->monsterinfo.currentmove == &guncmdr_move_jump))
	{
		return false;
	}

	if ((self->monsterinfo.currentmove == &guncmdr_move_fire_chain_dodge_left) ||
		(self->monsterinfo.currentmove == &guncmdr_move_fire_chain_dodge_right) ||
		(self->monsterinfo.currentmove == &guncmdr_move_attack_grenade_back_dodge_right) ||
		(self->monsterinfo.currentmove == &guncmdr_move_attack_grenade_back_dodge_right) ||
		(self->monsterinfo.currentmove == &guncmdr_move_attack_mortar_dodge))
	{
		// if we're dodging, don't duck
		self->monsterinfo.unduck(self);
		return false;
	}

	self->monsterinfo.currentmove = &guncmdr_move_duck_attack;

	return true;
}

bool guncmdr_sidestep(edict_t *self)
{
	// use special dodge during the main firing anim
	if (self->monsterinfo.currentmove == &guncmdr_move_fire_chain ||
		self->monsterinfo.currentmove == &guncmdr_move_fire_chain_run)
	{
        // NOT a ternary: genptr.py scans for "currentmove = &X" line by line and
        // silently drops moves hidden inside one.
        if (!self->monsterinfo.lefty)
            self->monsterinfo.currentmove = &guncmdr_move_fire_chain_dodge_right;
        else
            self->monsterinfo.currentmove = &guncmdr_move_fire_chain_dodge_left;
        return true;
	}

	// for backwards mortar, back up where we are in the animation and do a quick dodge
	if (self->monsterinfo.currentmove == &guncmdr_move_attack_grenade_back)
	{
		self->count = self->s.frame;
        if (!self->monsterinfo.lefty)
            self->monsterinfo.currentmove = &guncmdr_move_attack_grenade_back_dodge_right;
        else
            self->monsterinfo.currentmove = &guncmdr_move_attack_grenade_back_dodge_left;
        return true;
	}

	// use crouch-move for mortar dodge
	if (self->monsterinfo.currentmove == &guncmdr_move_attack_mortar)
	{
		self->count = self->s.frame;
		self->monsterinfo.currentmove = &guncmdr_move_attack_mortar_dodge;
		return true;
	}

	// regular sidestep during run
	if (self->monsterinfo.currentmove == &guncmdr_move_run)
	{
		self->monsterinfo.currentmove = &guncmdr_move_run;
		return true;
	}

	return false;
}

bool guncmdr_blocked(edict_t *self, float dist)
{
    blocked_jump_result_t result;

    if (blocked_checkplat(self, dist))
		return true;
	
    result = blocked_checkjump(self, dist);

    if (result != NO_JUMP) {
        if (result != JUMP_TURN)
            guncmdr_jump(self, result);

        return true;
    }

	return false;
}
// PGM
//===========

/*QUAKED monster_guncmdr (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight NoJumping
model="models/monsters/guncmdr/tris.md2"
*/
void SP_monster_guncmdr(edict_t *self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here and is called via walkmonster_start below.
    //
    // THE COMMANDER ANIMATIONS ONLY EXIST ON THE 799-FRAME RERELEASE GUNNER
    // MODEL. This tree ships the 1997 209-frame one, so without md5 models
    // there is nothing to animate - spawn an ordinary gunner instead so the
    // encounter still happens rather than leaving an empty room.
    if (!M_RereleaseAnims()) {
        SP_monster_gunner(self);
        return;
    }

	sound_death = gi.soundindex("guncmdr/gcdrdeath1.wav");
	sound_pain = gi.soundindex("guncmdr/gcdrpain2.wav");
	sound_pain2 = gi.soundindex("guncmdr/gcdrpain1.wav");
	sound_idle = gi.soundindex("guncmdr/gcdridle1.wav");
	sound_open = gi.soundindex("guncmdr/gcdratck1.wav");
	sound_search = gi.soundindex("guncmdr/gcdrsrch1.wav");
	sound_sight = gi.soundindex("guncmdr/sight1.wav");

	gi.soundindex("guncmdr/gcdratck2.wav");
	gi.soundindex("guncmdr/gcdratck3.wav");

	self->movetype = MOVETYPE_STEP;
	self->solid = SOLID_BBOX;
	self->s.modelindex = gi.modelindex("models/monsters/gunner/tris.md2");
	
	gi.modelindex("models/monsters/gunner/gibs/chest.md2");
	gi.modelindex("models/monsters/gunner/gibs/foot.md2");
	gi.modelindex("models/monsters/gunner/gibs/garm.md2");
	gi.modelindex("models/monsters/gunner/gibs/gun.md2");
	gi.modelindex("models/monsters/gunner/gibs/head.md2");

    // The rerelease sets s.scale 1.25 here - the commander is a bigger gunner.
    // This protocol carries no per-entity scale, so it renders gunner-sized.
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, 36);
    self->s.skinnum = 2;    // cskin, the commander skin

    // st.health_multiplier is 0 unless the map sets it - the rerelease defaults
    // it to 1, this tree does not. Multiplying unguarded gave the commander 0
    // health, so it spawned already dead: no animation, no movement, no attack.
    // Every other monster here guards it the same way.
    self->health = 325;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
	self->gib_health = -175;
	self->mass = 255;

	self->pain = guncmdr_pain;
	self->die = guncmdr_die;

	self->monsterinfo.stand = guncmdr_stand;
	self->monsterinfo.walk = guncmdr_walk;
	self->monsterinfo.run = guncmdr_run;
	// pmm
	self->monsterinfo.dodge = M_MonsterDodge;
	self->monsterinfo.duck = guncmdr_duck;
	self->monsterinfo.unduck = monster_duck_up;
	self->monsterinfo.sidestep = guncmdr_sidestep;
	self->monsterinfo.blocked = guncmdr_blocked; // PGM
	// pmm
	self->monsterinfo.attack = guncmdr_attack;
	self->monsterinfo.melee = NULL;
	self->monsterinfo.sight = guncmdr_sight;
	self->monsterinfo.search = guncmdr_search;
    // monsterinfo.setskin does not exist in this tree; guncmdr_setskin is
    // called directly from pain and die instead.

	gi.linkentity(self);

	self->monsterinfo.currentmove = &guncmdr_move_stand;
	self->monsterinfo.scale = MODEL_SCALE;

    // st.was_key_specified has no equivalent here; a zero field means the
    // mapper did not set one, which is the same test in practice.
    if (!self->monsterinfo.power_armor_power)
        self->monsterinfo.power_armor_power = 200;
    if (!self->monsterinfo.power_armor_type)
        self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;

	// PMM
	//self->monsterinfo.blindfire = true;
    self->monsterinfo.can_jump = !(self->spawnflags & SPAWNFLAG_GUNCMDR_NOJUMPING);
	self->monsterinfo.drop_height = 192;
	self->monsterinfo.jump_height = 40;

	walkmonster_start(self);
}
