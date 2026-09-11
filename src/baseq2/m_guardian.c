/* =======================================================================
 *
 * The guardian. A rerelease-original boss: a two-legged walker that spins up
 * a hyperblaster arm at close range, sweeps a pair of damage beams at long
 * range, and kicks anything that gets under its feet.
 *
 * Ported from src/rerelease/m_guardian.cpp with the same mechanical pass as
 * m_carrier.c: the C++ MMOVE_T/MONSTERINFO_* wrappers are unwrapped, every
 * float level.time site becomes this tree's integer level.framenum, and
 * M_SetAnimation() becomes a direct monsterinfo.currentmove assignment.
 *
 * Four substitutions worth naming, because they are not literal ports:
 *
 *  - The rerelease's RANGE_NEAR is a FLOAT DISTANCE of 440 units. This tree's
 *    RANGE_NEAR is the enum value 1 returned by range(). Using ours here would
 *    have compared a distance against 1 and sent the guardian into its long
 *    range attack from anywhere. It is spelled out as GUARDIAN_RANGE_NEAR.
 *
 *  - M_ShouldReactToPain() does not exist here; skill 3 is the nightmare gate
 *    the rest of this tree uses for the same purpose.
 *
 *  - There is no monsterinfo.weapon_sound in this tree. It only ever fed
 *    ent->s.sound in the rerelease, so the spin-up loop is set on s.sound
 *    directly - the idiom m_boss2.c, m_boss31.c and m_soldier.c already use.
 *
 *  - The rerelease re-aims a PERSISTENT beam through a PRETHINK. This tree's
 *    monster_dabeam() is a one-frame beam that traces, damages and frees
 *    itself, and the guardian fires one on each of the four atk2_fire frames -
 *    so a fresh beam per frame is the same picture, and is exactly how
 *    monster_soldier_lasergun already works here.
 *
 * NOTE ON THE MODEL: the guardian is the ONLY monster in the rerelease's pak
 * with no md5/ remaster - it ships as tris.md2 with a .pcx skin and no _glow
 * map, so it renders as a plain, rather dark md2 and cl_md5_models does
 * nothing for it. That is the shipped art, not a loader fault.
 *
 * =======================================================================
 */

#include "g_local.h"
#include "m_guardian.h"

/* The rerelease's RANGE_NEAR - a distance, not this tree's range() enum. */
#define GUARDIAN_RANGE_NEAR 440.0f

/* The kick will not chain: a miss stands the guardian down for a second. */
#define GUARDIAN_MELEE_DEBOUNCE (1 * BASE_FRAMERATE)

void BossExplode(edict_t *self);

void guardian_run(edict_t *self);
void guardian_stand(edict_t *self);
void guardian_walk(edict_t *self);
void guardian_attack(edict_t *self);
void guardian_dead(edict_t *self);
void guardian_atk1(edict_t *self);
void guardian_atk2(edict_t *self);
void guardian_atk2_out(edict_t *self);
void guardian_atk1_finish(edict_t *self);

static int sound_step;
static int sound_charge;
static int sound_spin_loop;
static int sound_laser;

/* The seven metal chunks it comes apart into. */
static const char *guardian_gibs[] = {
    "models/monsters/guardian/gib1.md2",
    "models/monsters/guardian/gib2.md2",
    "models/monsters/guardian/gib3.md2",
    "models/monsters/guardian/gib4.md2",
    "models/monsters/guardian/gib5.md2",
    "models/monsters/guardian/gib6.md2",
    "models/monsters/guardian/gib7.md2"
};

/*
 * stand
 */

static mframe_t guardian_frames_stand[] = {
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}
};
mmove_t guardian_move_stand = {
    FRAME_idle1,
    FRAME_idle52,
    guardian_frames_stand,
    NULL
};

void guardian_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_stand;
}

/*
 * walk
 */

void guardian_footstep(edict_t *self)
{
    gi.sound(self, CHAN_BODY, sound_step, 1, ATTN_NORM, 0);
}

static mframe_t guardian_frames_walk[] = {
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8, guardian_footstep},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8},
    {ai_walk, 8, guardian_footstep},
    {ai_walk, 8}
};
mmove_t guardian_move_walk = {
    FRAME_walk1,
    FRAME_walk19,
    guardian_frames_walk,
    NULL
};

void guardian_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_walk;
}

/*
 * run
 */

static mframe_t guardian_frames_run[] = {
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8, guardian_footstep},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8},
    {ai_run, 8, guardian_footstep},
    {ai_run, 8}
};
mmove_t guardian_move_run = {
    FRAME_walk1,
    FRAME_walk19,
    guardian_frames_run,
    NULL
};

void guardian_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND) {
        self->monsterinfo.currentmove = &guardian_move_stand;
        return;
    }

    self->monsterinfo.currentmove = &guardian_move_run;
}

/*
 * pain
 */

static mframe_t guardian_frames_pain1[] = {
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move}
};
mmove_t guardian_move_pain1 = {
    FRAME_pain1_1,
    FRAME_pain1_8,
    guardian_frames_pain1,
    guardian_run
};

void guardian_pain(edict_t *self, edict_t *other /* unused */,
        float kick /* unused */, int damage)
{
    if (meansOfDeath != MOD_CHAINFIST && damage <= 10)
        return;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    if (meansOfDeath != MOD_CHAINFIST && damage <= 75)
        if (random() > 0.2f)
            return;

    /* don't go into pain while attacking */
    if ((self->s.frame >= FRAME_atk1_spin1) && (self->s.frame <= FRAME_atk1_spin15))
        return;

    if ((self->s.frame >= FRAME_atk2_fire1) && (self->s.frame <= FRAME_atk2_fire4))
        return;

    if ((self->s.frame >= FRAME_kick_in1) && (self->s.frame <= FRAME_kick_in13))
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    // M_ShouldReactToPain does not exist here; skill 3 is the nightmare gate
    // the rest of this tree uses for the same purpose.
    if (skill->value >= 3)
        return; // no pain anims in nightmare

    self->monsterinfo.currentmove = &guardian_move_pain1;
    self->s.sound = 0;
}

/*
 * attack 1 - the spin-up hyperblaster arm
 */

static mframe_t guardian_frames_atk1_out[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge}
};
mmove_t guardian_move_atk1_out = {
    FRAME_atk1_out1,
    FRAME_atk1_out3,
    guardian_frames_atk1_out,
    guardian_run
};

void guardian_atk1_finish(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_atk1_out;
    self->s.sound = 0;
}

void guardian_atk1_charge(edict_t *self)
{
    self->s.sound = sound_spin_loop;
    gi.sound(self, CHAN_WEAPON, sound_charge, 1, ATTN_NORM, 0);
}

void guardian_fire_blaster(edict_t *self)
{
    vec3_t  forward, right, target;
    vec3_t  start;
    int     i;
    int     flash_index = MZ2_GUARDIAN_BLASTER;

    if (!self->enemy || !self->enemy->inuse)
        return;

    AngleVectors(self->s.angles, forward, right, NULL);
    M_ProjectFlashSource(self, monster_flash_offset[flash_index], forward, right, start);

    VectorCopy(self->enemy->s.origin, target);
    target[2] += self->enemy->viewheight;

    for (i = 0; i < 3; i++)
        target[i] += crandom() * 5;

    VectorSubtract(target, start, forward);
    VectorNormalize(forward);

    monster_fire_blaster(self, start, forward, 2, 1000, flash_index,
            (self->s.frame % 4) ? 0 : EF_HYPERBLASTER);

    /* Hold the spin: while the window in ->timestamp is open and the enemy is
       still visible, jump back to spin5 rather than winding down. Verified in
       play - the firing frames run 166-173 and then loop to 166 again. */
    if (self->enemy->health > 0 &&
        self->s.frame == FRAME_atk1_spin12 &&
        self->timestamp > level.framenum && visible(self, self->enemy))
        self->monsterinfo.nextframe = FRAME_atk1_spin5;
}

static mframe_t guardian_frames_atk1_spin[] = {
    {ai_charge, 0, guardian_atk1_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0, guardian_fire_blaster},
    {ai_charge, 0},
    {ai_charge, 0},
    {ai_charge, 0}
};
mmove_t guardian_move_atk1_spin = {
    FRAME_atk1_spin1,
    FRAME_atk1_spin15,
    guardian_frames_atk1_spin,
    guardian_atk1_finish
};

void guardian_atk1(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_atk1_spin;
    /* 650ms + up to 1.5s of extra spin, in frames */
    self->timestamp = level.framenum + (int)(0.65f * BASE_FRAMERATE) +
            (int)(random() * 1.5f * BASE_FRAMERATE);
}

static mframe_t guardian_frames_atk1_in[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge}
};
mmove_t guardian_move_atk1_in = {
    FRAME_atk1_in1,
    FRAME_atk1_in3,
    guardian_frames_atk1_in,
    guardian_atk1
};

/*
 * attack 2 - the twin damage beams
 */

static mframe_t guardian_frames_atk2_out[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge}
};
mmove_t guardian_move_atk2_out = {
    FRAME_atk2_out1,
    FRAME_atk2_out7,
    guardian_frames_atk2_out,
    guardian_run
};

void guardian_atk2_out(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_atk2_out;
}

/* The two beam muzzles, alternating by frame parity. */
static const vec3_t guardian_laser_positions[] = {
    {125.0f, -70.0f, 60.0f},
    {112.0f, -62.0f, 60.0f}
};

void guardian_laser_fire(edict_t *self)
{
    vec3_t  forward, right, start;
    edict_t *beam;

    if (!self->enemy || !self->enemy->inuse)
        return;

    gi.sound(self, CHAN_WEAPON, sound_laser, 1, ATTN_NORM, 0);

    AngleVectors(self->s.angles, forward, right, NULL);
    M_ProjectFlashSource(self,
            guardian_laser_positions[1 - (self->s.frame & 1)],
            forward, right, start);

    /* monster_dabeam() aims itself at ->enemy and frees itself a frame later,
       so all this has to hand it is where the beam starts and who owns it. */
    beam = G_Spawn();
    VectorCopy(start, beam->s.origin);
    VectorCopy(self->s.angles, beam->s.angles);
    beam->enemy = self->enemy;
    beam->owner = self;
    beam->dmg = 25;
    beam->classname = "guardian_laserbeam";

    monster_dabeam(beam);
}

static mframe_t guardian_frames_atk2_fire[] = {
    {ai_charge, 0, guardian_laser_fire},
    {ai_charge, 0, guardian_laser_fire},
    {ai_charge, 0, guardian_laser_fire},
    {ai_charge, 0, guardian_laser_fire}
};
mmove_t guardian_move_atk2_fire = {
    FRAME_atk2_fire1,
    FRAME_atk2_fire4,
    guardian_frames_atk2_fire,
    guardian_atk2_out
};

void guardian_atk2(edict_t *self)
{
    self->monsterinfo.currentmove = &guardian_move_atk2_fire;
}

static mframe_t guardian_frames_atk2_in[] = {
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge}
};
mmove_t guardian_move_atk2_in = {
    FRAME_atk2_in1,
    FRAME_atk2_in12,
    guardian_frames_atk2_in,
    guardian_atk2
};

/*
 * the kick
 */

void guardian_kick(edict_t *self)
{
    vec3_t aim;

    VectorSet(aim, MELEE_DISTANCE, 0, -80);

    if (!fire_hit(self, aim, 85, 700))
        self->monsterinfo.melee_debounce_framenum =
                level.framenum + GUARDIAN_MELEE_DEBOUNCE;
}

static mframe_t guardian_frames_kick[] = {
    {ai_charge},
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_kick},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, guardian_footstep},
    {ai_charge},
    {ai_charge}
};
mmove_t guardian_move_kick = {
    FRAME_kick_in1,
    FRAME_kick_in13,
    guardian_frames_kick,
    guardian_run
};

void guardian_attack(edict_t *self)
{
    float r;

    if (!self->enemy || !self->enemy->inuse)
        return;

    r = M_RangeBetween(self, self->enemy);

    if (r > GUARDIAN_RANGE_NEAR)
        self->monsterinfo.currentmove = &guardian_move_atk2_in;
    else if (self->monsterinfo.melee_debounce_framenum < level.framenum && r < 120.0f)
        self->monsterinfo.currentmove = &guardian_move_kick;
    else
        self->monsterinfo.currentmove = &guardian_move_atk1_in;
}

/*
 * death
 */

void guardian_explode(edict_t *self)
{
    vec3_t org;

    org[0] = self->s.origin[0] + self->mins[0] + random() * self->size[0];
    org[1] = self->s.origin[1] + self->mins[1] + random() * self->size[1];
    org[2] = self->s.origin[2] + self->mins[2] + random() * self->size[2];

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1_BIG);
    gi.WritePosition(org);
    gi.multicast(self->s.origin, MULTICAST_ALL);
}

void guardian_dead(edict_t *self)
{
    int i;

    for (i = 0; i < 3; i++)
        guardian_explode(self);

    for (i = 0; i < 2; i++)
        ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", 125, GIB_ORGANIC);

    for (i = 0; i < 4; i++)
        ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", 125, GIB_METALLIC);

    /* two of each of the six body chunks, then the head */
    for (i = 0; i < 6; i++) {
        ThrowGib(self, (char *)guardian_gibs[i], 125, GIB_METALLIC);
        ThrowGib(self, (char *)guardian_gibs[i], 125, GIB_METALLIC);
    }

    ThrowHead(self, (char *)guardian_gibs[6], 125, GIB_METALLIC);
}

static mframe_t guardian_frames_death1[] = {
    {ai_move, 0, BossExplode},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move}
};
mmove_t guardian_move_death = {
    FRAME_death1,
    FRAME_death26,
    guardian_frames_death1,
    guardian_dead
};

void guardian_die(edict_t *self, edict_t *inflictor /* unused */,
        edict_t *attacker /* unused */, int damage /* unused */,
        vec3_t point /* unused */)
{
    self->s.sound = 0;
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;

    self->monsterinfo.currentmove = &guardian_move_death;
}

/*
 * QUAKED monster_guardian (1 .5 0) (-96 -96 -66) (96 96 62) Ambush Trigger_Spawn Sight
 */
void SP_monster_guardian(edict_t *self)
{
    int i;

    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via walkmonster_start below.

    sound_step = gi.soundindex("zortemp/step.wav");
    sound_charge = gi.soundindex("weapons/hyprbu1a.wav");
    sound_spin_loop = gi.soundindex("weapons/hyprbl1a.wav");
    sound_laser = gi.soundindex("weapons/laser2.wav");

    for (i = 0; i < 7; i++)
        gi.modelindex((char *)guardian_gibs[i]);

    self->s.modelindex = gi.modelindex("models/monsters/guardian/tris.md2");
    VectorSet(self->mins, -96, -96, -66);
    VectorSet(self->maxs, 96, 96, 62);
    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    // st.health_multiplier is 0 here unless the map sets it (the rerelease
    // defaults it to 1), so multiplying unguarded spawns the monster DEAD -
    // health 0, inert, and killmonsters skips it. Same one-line bug that broke
    // monster_guncmdr and monster_shambler; caught here in play.
    self->health = 2500;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
    self->gib_health = -200;

    self->monsterinfo.scale = MODEL_SCALE;

    self->mass = 850;

    self->pain = guardian_pain;
    self->die = guardian_die;
    self->monsterinfo.stand = guardian_stand;
    self->monsterinfo.walk = guardian_walk;
    self->monsterinfo.run = guardian_run;
    self->monsterinfo.attack = guardian_attack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &guardian_move_stand;

    walkmonster_start(self);
}
