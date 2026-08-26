// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.
/*
==============================================================================

ARACHNID

==============================================================================
*/

#include "g_local.h"
#include "m_arachnid.h"

static int sound_pain;
static int sound_death;
static int sound_sight;

void arachnid_sight(edict_t *self, edict_t *other)
{
	gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

//
// stand
//

mframe_t arachnid_frames_stand[] = {
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
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
mmove_t arachnid_move_stand = { FRAME_idle1, FRAME_idle13, arachnid_frames_stand, NULL };

void arachnid_stand(edict_t *self)
{
	self->monsterinfo.currentmove = &arachnid_move_stand;
}

//
// walk
//

static int sound_step;

void arachnid_footstep(edict_t *self)
{
	gi.sound(self, CHAN_BODY, sound_step, 0.5f, ATTN_IDLE, 0.0f);
}

mframe_t arachnid_frames_walk[] = {
	{ ai_walk, 8, arachnid_footstep },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, arachnid_footstep },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 8, NULL }
};
mmove_t arachnid_move_walk = { FRAME_walk1, FRAME_walk10, arachnid_frames_walk, NULL };

void arachnid_walk(edict_t *self)
{
	self->monsterinfo.currentmove = &arachnid_move_walk;
}

//
// run
//

mframe_t arachnid_frames_run[] = {
	{ ai_run, 8, arachnid_footstep },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL },
	{ ai_run, 8, arachnid_footstep },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL },
	{ ai_run, 8, NULL }
};
mmove_t arachnid_move_run = { FRAME_walk1, FRAME_walk10, arachnid_frames_run, NULL };

void arachnid_run(edict_t *self)
{
	if (self->monsterinfo.aiflags & AI_STAND_GROUND)
	{
		self->monsterinfo.currentmove = &arachnid_move_stand;
		return;
	}

	self->monsterinfo.currentmove = &arachnid_move_run;
}

//
// pain
//

mframe_t arachnid_frames_pain1[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t arachnid_move_pain1 = { FRAME_pain11, FRAME_pain15, arachnid_frames_pain1, arachnid_run };

mframe_t arachnid_frames_pain2[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t arachnid_move_pain2 = { FRAME_pain21, FRAME_pain26, arachnid_frames_pain2, arachnid_run };

void arachnid_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    float r;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
    gi.sound(self, CHAN_VOICE, sound_pain, 1, ATTN_NORM, 0);

    // M_ShouldReactToPain does not exist here; skill 3 is the nightmare gate
    // the rest of this tree uses.
    if (skill->value >= 3)
        return; // no pain anims in nightmare

    r = random();

	if (r < 0.5f)
		self->monsterinfo.currentmove = &arachnid_move_pain1;
	else
		self->monsterinfo.currentmove = &arachnid_move_pain2;
}

static int sound_charge;

void arachnid_charge_rail(edict_t *self)
{
	if (!self->enemy || !self->enemy->inuse)
		return;

	gi.sound(self, CHAN_WEAPON, sound_charge, 1.f, ATTN_NORM, 0.f);
	VectorCopy(self->enemy->s.origin, self->pos1);
	self->pos1[2] += self->enemy->viewheight;
}

void arachnid_rail(edict_t *self)
{
	vec3_t start;
	vec3_t dir;
	vec3_t forward, right;
	int    id;

	switch (self->s.frame)
	{
		case FRAME_rails4:
		default:
			id = MZ2_ARACHNID_RAIL1;
			break;
		case FRAME_rails8:
			id = MZ2_ARACHNID_RAIL2;
			break;
		case FRAME_rails_up7:
			id = MZ2_ARACHNID_RAIL_UP1;
			break;
		case FRAME_rails_up11:
			id = MZ2_ARACHNID_RAIL_UP2;
			break;
	}

	AngleVectors(self->s.angles, forward, right, NULL);
	M_ProjectFlashSource(self, monster_flash_offset[id], forward, right, start);

	// calc direction to where we targeted
	VectorSubtract(self->pos1, start, dir);
	VectorNormalize(dir);

	monster_fire_railgun(self, start, dir, 35, 100, id);
}

mframe_t arachnid_frames_attack1[] = {
	{ ai_charge, 0, arachnid_charge_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_rail },
	{ ai_charge, 0, arachnid_charge_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL }
};
mmove_t arachnid_attack1 = { FRAME_rails1, FRAME_rails11, arachnid_frames_attack1, arachnid_run };

mframe_t arachnid_frames_attack_up1[] = {
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_charge_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_rail },
	{ ai_charge, 0, arachnid_charge_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_rail },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
};
mmove_t arachnid_attack_up1 = { FRAME_rails_up1, FRAME_rails_up16, arachnid_frames_attack_up1, arachnid_run };

static int sound_melee, sound_melee_hit;

void arachnid_melee_charge(edict_t *self)
{
	gi.sound(self, CHAN_WEAPON, sound_melee, 1.f, ATTN_NORM, 0.f);
}

void arachnid_melee_hit(edict_t *self)
{
	vec3_t aim;

	VectorSet(aim, MELEE_DISTANCE, 0, 0);
	if (!fire_hit(self, aim, 15, 50))
        self->monsterinfo.melee_debounce_framenum = level.framenum + 1 * BASE_FRAMERATE;
}

mframe_t arachnid_frames_melee[] = {
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_melee_charge },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_melee_hit },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_melee_charge },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, arachnid_melee_hit },
	{ ai_charge, 0, NULL }
};
mmove_t arachnid_melee = { FRAME_melee_atk1, FRAME_melee_atk12, arachnid_frames_melee, arachnid_run };

void arachnid_attack(edict_t *self)
{
	if (!self->enemy || !self->enemy->inuse)
		return;

    if (self->monsterinfo.melee_debounce_framenum < level.framenum &&
        realrange(self, self->enemy) < MELEE_DISTANCE)
		self->monsterinfo.currentmove = &arachnid_melee;
	else if ((self->enemy->s.origin[2] - self->s.origin[2]) > 150.f)
		self->monsterinfo.currentmove = &arachnid_attack_up1;
	else
		self->monsterinfo.currentmove = &arachnid_attack1;
}

//
// death
//

void arachnid_dead(edict_t *self)
{
	VectorSet(self->mins, -16, -16, -24);
	VectorSet(self->maxs, 16, 16, -8);
	self->movetype = MOVETYPE_TOSS;
	self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
	gi.linkentity(self);
}

mframe_t arachnid_frames_death1[] = {
	{ ai_move, 0, NULL },
	{ ai_move, -1.23f, NULL },
	{ ai_move, -1.23f, NULL },
	{ ai_move, -1.23f, NULL },
	{ ai_move, -1.23f, NULL },
	{ ai_move, -1.64f, NULL },
	{ ai_move, -1.64f, NULL },
	{ ai_move, -2.45f, NULL },
	{ ai_move, -8.63f, NULL },
	{ ai_move, -4.0f, NULL },
	{ ai_move, -4.5f, NULL },
	{ ai_move, -6.8f, NULL },
	{ ai_move, -8.0f, NULL },
	{ ai_move, -5.4f, NULL },
	{ ai_move, -3.4f, NULL },
	{ ai_move, -1.9f, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }
};
mmove_t arachnid_move_death = { FRAME_death1, FRAME_death20, arachnid_frames_death1, arachnid_dead };

void arachnid_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
	int n;

	// check for gib
    if (self->health <= self->gib_health) {
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
        // the arachnid ships no gibs of its own; generic set, thrown this
        // tree's way
        for (n = 0; n < 2; n++)
            ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
        for (n = 0; n < 4; n++)
            ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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

	self->monsterinfo.currentmove = &arachnid_move_death;
}

//
// monster_arachnid
//

/*QUAKED monster_arachnid (1 .5 0) (-48 -48 -20) (48 48 48) Ambush Trigger_Spawn Sight
 */
void SP_monster_arachnid(edict_t *self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via walkmonster_start below.

	sound_step = gi.soundindex("insane/insane11.wav");
	sound_charge = gi.soundindex("gladiator/railgun.wav");
	sound_melee = gi.soundindex("gladiator/melee3.wav");
	sound_melee_hit = gi.soundindex("gladiator/melee2.wav");
	sound_pain = gi.soundindex("arachnid/pain.wav");
	sound_death = gi.soundindex("arachnid/death.wav");
	sound_sight = gi.soundindex("arachnid/sight.wav");

	self->s.modelindex = gi.modelindex("models/monsters/arachnid/tris.md2");
	VectorSet(self->mins, -48, -48, -20);
	VectorSet(self->maxs, 48, 48, 48);
	self->movetype = MOVETYPE_STEP;
	self->solid = SOLID_BBOX;

	self->health = 1000;
	if (st.health_multiplier > 0)
		self->health = (int)(self->health * st.health_multiplier);
	self->gib_health = -200;

	self->monsterinfo.scale = MODEL_SCALE;

	self->mass = 450;

	self->pain = arachnid_pain;
	self->die = arachnid_die;
	self->monsterinfo.stand = arachnid_stand;
	self->monsterinfo.walk = arachnid_walk;
	self->monsterinfo.run = arachnid_run;
	self->monsterinfo.attack = arachnid_attack;
	self->monsterinfo.sight = arachnid_sight;

	gi.linkentity(self);

	self->monsterinfo.currentmove = &arachnid_move_stand;

	walkmonster_start(self);
}
