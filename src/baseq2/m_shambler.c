/*
Copyright (c) ZeniMax Media Inc.
Licensed under the GNU General Public License 2.0.
*/
/*
==============================================================================

SHAMBLER

The Quake 1 shambler, brought back by the rerelease. Unlike the gun commander
this one has its OWN model and its own 94-frame table, so it needs no md5 gate -
it works on the md2 path too.

Ported from src/rerelease/m_shambler.cpp. Divergences, all because the feature
does not exist in this tree, are commented inline.

==============================================================================
*/

#include "g_local.h"
#include "m_shambler.h"

void shambler_setskin(edict_t *self);

static int sound_pain;
static int sound_idle;
static int sound_die;
static int sound_sight;
static int sound_windup;
static int sound_melee1;
static int sound_melee2;
static int sound_smack;
static int sound_boom;

//
// misc
//

void shambler_sight(edict_t* self, edict_t* other)
{
	gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

static const vec3_t lightning_left_hand[] = {
	{ 44, 36, 25 },
	{ 10, 44, 57 },
	{ -1, 40, 70 },
	{ -10, 34, 75 },
	{ 7.4f, 24, 89 }
};

static const vec3_t lightning_right_hand[] = {
	{ 28, -38, 25 },
	{ 31, -7, 70 },
	{ 20, 0, 80 },
	{ 16, 1.2f, 81 },
	{ 27, -11, 83 }
};

static void shambler_lightning_update(edict_t *self)
{
    edict_t *lightning = self->beam;
    vec3_t  f, r;

    if (!lightning)
        return;

	if (self->s.frame >= FRAME_magic01 + q_countof(lightning_left_hand))
	{
		G_FreeEdict(lightning);
		self->beam = NULL;
		return;
	}

    AngleVectors(self->s.angles, f, r, NULL);
    M_ProjectFlashSource(self, lightning_left_hand[self->s.frame - FRAME_magic01], f, r, lightning->s.origin);
    M_ProjectFlashSource(self, lightning_right_hand[self->s.frame - FRAME_magic01], f, r, lightning->s.old_origin);
	gi.linkentity(lightning);
}

void shambler_windup(edict_t* self)
{
	gi.sound(self, CHAN_WEAPON, sound_windup, 1, ATTN_NORM, 0);

    edict_t *lightning;

    self->beam = lightning = G_Spawn();
	lightning->s.modelindex = gi.modelindex("models/proj/lightning/tris.md2");
	lightning->s.renderfx |= RF_BEAM;
	lightning->owner = self;
	shambler_lightning_update(self);
}

void shambler_idle(edict_t* self)
{
	gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

void shambler_maybe_idle(edict_t* self)
{
	if (random() > 0.8)
		gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

//
// stand
//

mframe_t shambler_frames_stand[] = {
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
	{ ai_stand, 0, NULL },
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
mmove_t shambler_move_stand = { FRAME_stand01, FRAME_stand17, shambler_frames_stand, NULL };

void shambler_stand(edict_t* self)
{
	self->monsterinfo.currentmove = &shambler_move_stand;
}

//
// walk
//

void shambler_walk(edict_t* self);

mframe_t shambler_frames_walk[] = {
	{ ai_walk, 10, NULL }, // FIXME: add footsteps?
	{ ai_walk, 9, NULL },
	{ ai_walk, 9, NULL },
	{ ai_walk, 5, NULL },
	{ ai_walk, 6, NULL },
	{ ai_walk, 12, NULL },
	{ ai_walk, 8, NULL },
	{ ai_walk, 3, NULL },
	{ ai_walk, 13, NULL },
	{ ai_walk, 9, NULL },
	{ ai_walk, 7, shambler_maybe_idle },
	{ ai_walk, 5, NULL },
};
mmove_t shambler_move_walk = { FRAME_walk01, FRAME_walk12, shambler_frames_walk, NULL };

void shambler_walk(edict_t* self)
{
	self->monsterinfo.currentmove = &shambler_move_walk;
}

//
// run
//

void shambler_run(edict_t* self);

mframe_t shambler_frames_run[] = {
	{ ai_run, 20, NULL }, // FIXME: add footsteps?
	{ ai_run, 24, NULL },
	{ ai_run, 20, NULL },
	{ ai_run, 20, NULL },
	{ ai_run, 24, NULL },
	{ ai_run, 20, shambler_maybe_idle },
};
mmove_t shambler_move_run = { FRAME_run01, FRAME_run06, shambler_frames_run, NULL };

void shambler_run(edict_t* self)
{
	if (self->enemy && self->enemy->client)
		self->monsterinfo.aiflags |= AI_BRUTAL;
	else
		self->monsterinfo.aiflags &= ~AI_BRUTAL;

	if (self->monsterinfo.aiflags & AI_STAND_GROUND)
	{
		self->monsterinfo.currentmove = &shambler_move_stand;
		return;
	}

	self->monsterinfo.currentmove = &shambler_move_run;
}

//
// pain
//

// FIXME: needs halved explosion damage

mframe_t shambler_frames_pain[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
};
mmove_t shambler_move_pain = { FRAME_pain01, FRAME_pain06, shambler_frames_pain, shambler_run };

void shambler_pain(edict_t* self, edict_t* other, float kick, int damage)
{
    if (level.framenum < self->timestamp)
        return;

    // id uses 1ms here purely as a "once per event" latch; one server frame is
    // the finest granularity this tree has.
    self->timestamp = level.framenum + 1;
	gi.sound(self, CHAN_AUTO, sound_pain, 1, ATTN_NORM, 0);

    if (meansOfDeath != MOD_CHAINFIST && damage <= 30 && random() > 0.2f)
		return;

	// If hard or nightmare, don't go into pain while attacking
	if (skill->integer >= 2)
	{
		if ((self->s.frame >= FRAME_smash01) && (self->s.frame <= FRAME_smash12))
			return;

		if ((self->s.frame >= FRAME_swingl01) && (self->s.frame <= FRAME_swingl09))
			return;

		if ((self->s.frame >= FRAME_swingr01) && (self->s.frame <= FRAME_swingr09))
			return;
	}
	
    // M_ShouldReactToPain does not exist here; skill 3 is the nightmare gate
    // the rest of this tree uses for the same purpose.
    if (skill->value >= 3)
        return; // no pain anims in nightmare

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 2 * BASE_FRAMERATE;
	self->monsterinfo.currentmove = &shambler_move_pain;
}

void shambler_setskin(edict_t* self)
{
	// FIXME: create pain skin?
	//if (self->health < (self->max_health / 2))
	//	self->s.skinnum |= 1;
	//else
	//	self->s.skinnum &= ~1;
}

//
// attacks
//

/*
void() sham_magic3     =[      $magic3,       sham_magic4    ] {
	ai_face();
	self.nextthink = self.nextthink + 0.2;
	local entity o;

	self.effects = self.effects | EF_MUZZLEFLASH;
	ai_face();
	self.owner = spawn();
	o = self.owner;
	setmodel (o, "progs/s_light.mdl");
	setorigin (o, self.origin);
	o.angles = self.angles;
	o.nextthink = time + 0.7;
	o.think = SUB_Remove;
};
*/

void ShamblerSaveLoc(edict_t* self)
{
    // id's comment says "save for aiming the shot", but nothing ever reads
    // pos1 back - ShamblerCastLightning re-predicts against the live enemy.
    // Dead in their source too; kept so the frame callback matches theirs.
    VectorCopy(self->enemy->s.origin, self->pos1);
	self->pos1[2] += self->enemy->viewheight;
	self->monsterinfo.nextframe = FRAME_magic09;

	gi.sound(self, CHAN_WEAPON, sound_boom, 1, ATTN_NORM, 0);
	shambler_lightning_update(self);
}

#define SPAWNFLAG_SHAMBLER_PRECISE      1

/*
=================
FindShamblerOffset

Walk the muzzle down from head height until the bolt has a clear path, so the
shambler does not fire its lightning into the lip of whatever it is standing
behind. Falls back to head height if nothing is clear.
=================
*/
void FindShamblerOffset(edict_t *self, vec3_t offset)
{
    vec3_t  start;
    int     i;

    VectorSet(offset, 0, 0, 48);

    for (i = 0; i < 8; i++) {
        if (M_CheckClearShot(self, offset, start))
            return;

        offset[2] -= 4;
    }

    VectorSet(offset, 0, 0, 48);
}

void ShamblerCastLightning(edict_t* self)
{
	if (!self->enemy)
		return;

    vec3_t  start, dir, end, forward, right, offset;
    trace_t tr;

    FindShamblerOffset(self, offset);

    AngleVectors(self->s.angles, forward, right, NULL);
    M_ProjectFlashSource(self, offset, forward, right, start);

    // bolt_speed 0 - the lightning is hitscan, so there is no travel time to
    // lead. PredictAim MUST guard that division (see g_ai.c); it did not, and
    // the resulting infinite aim point fired the bolt behind the shambler.
    PredictAim(self->enemy, start, 0, false,
               (self->spawnflags & SPAWNFLAG_SHAMBLER_PRECISE) ? 0.0f : 0.1f, dir, NULL);

    VectorMA(start, 8192, dir, end);
    tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT | CONTENTS_SLIME | CONTENTS_LAVA);

    // gi.WriteEntity does not exist in this tree; TE_LIGHTNING reads two entity
    // numbers as shorts, which is what WriteShort sends.
    //
    // ORDER MATTERS AND IT IS DESTINATION-FIRST. The rerelease writes
    // (source, dest, start, endpos); this tree's client is the stock rogue one,
    // which reads (dest, source, DEST POINT, SOURCE POINT) - see the tesla in
    // g_weapon.c, the only other TE_LIGHTNING here, and CL_AddBeams in
    // src/client/tent.c, which marches from pos1 to pos2 with the model turned
    // 180 degrees. Writing the rerelease's order draws the bolt backwards.
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_LIGHTNING);
    gi.WriteShort(tr.ent ? tr.ent - g_edicts : 0);  // destination entity
    gi.WriteShort(self - g_edicts);                 // source entity
    gi.WritePosition(tr.endpos);                    // destination point
    gi.WritePosition(start);                        // source point
    gi.multicast(start, MULTICAST_PVS);

    fire_bullet(self, start, dir, 8 + (Q_rand() % 5), 15, 0, 0, MOD_TESLA);
}

mframe_t shambler_frames_magic[] = {
	{ ai_charge, 0, shambler_windup },
	{ ai_charge, 0, shambler_lightning_update },
	{ ai_charge, 0, shambler_lightning_update },
	{ ai_move, 0, shambler_lightning_update },
	{ ai_move, 0, shambler_lightning_update },
	{ ai_move, 0, ShamblerSaveLoc},
	{ ai_move, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_move, 0, ShamblerCastLightning },
	{ ai_move, 0, ShamblerCastLightning },
	{ ai_move, 0, ShamblerCastLightning },
	{ ai_move, 0, NULL },
};

mmove_t shambler_attack_magic = { FRAME_magic01, FRAME_magic12, shambler_frames_magic, shambler_run };

void shambler_attack(edict_t* self)
{
	self->monsterinfo.currentmove = &shambler_attack_magic;
}

//
// melee
//

void shambler_melee1(edict_t* self)
{
	gi.sound(self, CHAN_WEAPON, sound_melee1, 1, ATTN_NORM, 0);
}

void shambler_melee2(edict_t* self)
{
	gi.sound(self, CHAN_WEAPON, sound_melee2, 1, ATTN_NORM, 0);
}

void sham_swingl9(edict_t* self);
void sham_swingr9(edict_t* self);

void sham_smash10(edict_t* self)
{
	if (!self->enemy)
		return;

	ai_charge(self, 0);

	if (!CanDamage(self->enemy, self))
		return;

	vec3_t aim = { MELEE_DISTANCE, self->mins[0], -4 };
	bool hit = fire_hit(self, aim, 110 + (Q_rand() % 10), 120); // Slower attack

	if (hit)
		gi.sound(self, CHAN_WEAPON, sound_smack, 1, ATTN_NORM, 0);

	// SpawnMeatSpray(self.origin + v_forward * 16, crandom() * 100 * v_right);
	// SpawnMeatSpray(self.origin + v_forward * 16, crandom() * 100 * v_right);
};

void ShamClaw(edict_t* self)
{
	if (!self->enemy)
		return;

	ai_charge(self, 10);

	if (!CanDamage(self->enemy, self))
		return;

	vec3_t aim = { MELEE_DISTANCE, self->mins[0], -4 };
	bool hit = fire_hit(self, aim, 70 + (Q_rand() % 10), 80); // Slower attack

	if (hit)
		gi.sound(self, CHAN_WEAPON, sound_smack, 1, ATTN_NORM, 0);
	
	// 250 if left, -250 if right
	/*
	if (side)
	{
		makevectorsfixed(self.angles);
		SpawnMeatSpray(self.origin + v_forward * 16, side * v_right);
	}
	*/
};

mframe_t shambler_frames_smash[] = {
	{ ai_charge, 2, shambler_melee1 },
	{ ai_charge, 6, NULL },
	{ ai_charge, 6, NULL },
	{ ai_charge, 5, NULL },
	{ ai_charge, 4, NULL },
	{ ai_charge, 1, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, NULL },
	{ ai_charge, 0, sham_smash10 },
	{ ai_charge, 5, NULL },
	{ ai_charge, 4, NULL },
};

mmove_t shambler_attack_smash = { FRAME_smash01, FRAME_smash12, shambler_frames_smash, shambler_run };

mframe_t shambler_frames_swingl[] = {
	{ ai_charge, 5, shambler_melee1 },
	{ ai_charge, 3, NULL },
	{ ai_charge, 7, NULL },
	{ ai_charge, 3, NULL },
	{ ai_charge, 7, NULL },
	{ ai_charge, 9, NULL },
	{ ai_charge, 5, ShamClaw },
	{ ai_charge, 4, NULL },
	{ ai_charge, 8, sham_swingl9 },
};

mmove_t shambler_attack_swingl = { FRAME_swingl01, FRAME_swingl09, shambler_frames_swingl, shambler_run };

mframe_t shambler_frames_swingr[] = {
	{ ai_charge, 1, shambler_melee2 },
	{ ai_charge, 8, NULL },
	{ ai_charge, 14, NULL },
	{ ai_charge, 7, NULL },
	{ ai_charge, 3, NULL },
	{ ai_charge, 6, NULL },
	{ ai_charge, 6, ShamClaw },
	{ ai_charge, 3, NULL },
	{ ai_charge, 8, sham_swingr9 },
};

mmove_t shambler_attack_swingr = { FRAME_swingr01, FRAME_swingr09, shambler_frames_swingr, shambler_run };

void sham_swingl9(edict_t* self)
{
	ai_charge(self, 8);

	if ((random() < 0.5f) && self->enemy && realrange(self, self->enemy) < MELEE_DISTANCE)
		self->monsterinfo.currentmove = &shambler_attack_swingr;
}

void sham_swingr9(edict_t* self)
{
	ai_charge(self, 1);
	ai_charge(self, 10);

	if ((random() < 0.5f) && self->enemy && realrange(self, self->enemy) < MELEE_DISTANCE)
		self->monsterinfo.currentmove = &shambler_attack_swingl;
}

void shambler_melee(edict_t* self)
{
	float chance = random();
	if (chance > 0.6 || self->health == 600)
		self->monsterinfo.currentmove = &shambler_attack_smash;
	else if (chance > 0.3)
		self->monsterinfo.currentmove = &shambler_attack_swingl;
	else
		self->monsterinfo.currentmove = &shambler_attack_swingr;
}

//
// death
//

void shambler_dead(edict_t* self)
{
	VectorSet(self->mins, -16, -16, -24);
	VectorSet(self->maxs, 16, 16, -0);
    // monster_dead() is a rerelease helper; this tree does the same inline
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

static void shambler_shrink(edict_t* self)
{
	self->maxs[2] = 0;
	self->svflags |= SVF_DEADMONSTER;
	gi.linkentity(self);
}

mframe_t shambler_frames_death[] = {
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, shambler_shrink },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL },
	{ ai_move, 0, NULL }, // FIXME: thud?
};
mmove_t shambler_move_death = { FRAME_death01, FRAME_death11, shambler_frames_death, shambler_dead };

void shambler_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int n;

    if (self->beam) {
        G_FreeEdict(self->beam);
        self->beam = NULL;
    }

    // id also frees a `beam2` here, but nothing in their source ever SETS it -
    // the shambler only uses one beam. Not reproduced.

	// check for gib
    if (self->health <= self->gib_health) {
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
        // id's own FIXME: the shambler has no gibs of its own, so these are the
        // generic ones - same set, thrown the way this tree throws them.
        for (n = 0; n < 2; n++)
            ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
        ThrowGib(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
        ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
        self->deadflag = DEAD_DEAD;
		return;
	}

	if (self->deadflag)
		return;

	// regular death
	gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);
	self->deadflag = true;
	self->takedamage = true;

	self->monsterinfo.currentmove = &shambler_move_death;
}

void SP_monster_shambler(edict_t* self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via walkmonster_start below.

	self->s.modelindex = gi.modelindex("models/monsters/shambler/tris.md2");
	VectorSet(self->mins, -32, -32, -24);
	VectorSet(self->maxs, 32, 32, 64);
	self->movetype = MOVETYPE_STEP;
	self->solid = SOLID_BBOX;

	gi.modelindex("models/proj/lightning/tris.md2");
	sound_pain = gi.soundindex("shambler/shurt2.wav");
	sound_idle = gi.soundindex("shambler/sidle.wav");
	sound_die = gi.soundindex("shambler/sdeath.wav");
	sound_windup = gi.soundindex("shambler/sattck1.wav");
	sound_melee1 = gi.soundindex("shambler/melee1.wav");
	sound_melee2 = gi.soundindex("shambler/melee2.wav");
	sound_sight = gi.soundindex("shambler/ssight.wav");
	sound_smack = gi.soundindex("shambler/smack.wav");
	sound_boom = gi.soundindex("shambler/sboom.wav");

    // st.health_multiplier is 0 here unless the map sets it (the rerelease
    // defaults it to 1), so multiplying unguarded spawns the monster dead.
    // Same one-line bug that broke monster_guncmdr.
    self->health = 600;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
	self->gib_health = -60;

	self->mass = 500;

	self->pain = shambler_pain;
	self->die = shambler_die;
	self->monsterinfo.stand = shambler_stand;
	self->monsterinfo.walk = shambler_walk;
	self->monsterinfo.run = shambler_run;
	self->monsterinfo.dodge = NULL;
	self->monsterinfo.attack = shambler_attack;
	self->monsterinfo.melee = shambler_melee;
	self->monsterinfo.sight = shambler_sight;
	self->monsterinfo.idle = shambler_idle;
	self->monsterinfo.blocked = NULL;
    // monsterinfo.setskin does not exist in this tree; shambler_setskin is
    // called directly from pain and die instead.

	gi.linkentity(self);

    if (self->spawnflags & SPAWNFLAG_SHAMBLER_PRECISE)
		self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

	self->monsterinfo.currentmove = &shambler_move_stand;
	self->monsterinfo.scale = MODEL_SCALE;

	walkmonster_start(self);
}
