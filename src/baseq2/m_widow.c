/* =======================================================================
 *
 * The Black Widow - rogue's final boss, stage 1.
 *
 * She fights on three weapons and a spawner: a wide sweeping blaster that
 * tracks with her TORSO independently of her feet, a railgun she leads you
 * with, a kick for anything that gets underneath, and a hatch that drops
 * pairs of stalkers while she keeps firing.
 *
 * Ported from src/rerelease/rogue/m_rogue_widow.cpp with the same mechanical
 * pass as m_carrier.c, which is also where most of the machinery she needs
 * (FindSpawnPoint, CreateGroundMonster, M_SlotsLeft, PickCoopTarget,
 * PredictAim, realrange, SpawnGrow_Spawn) already came into this tree.
 *
 * Deliberate differences from the rerelease, none of them accidental:
 *
 *  - THERE IS NO trigger_bad_area IN THIS TREE. m_carrier.c dropped it for
 *    the same reason. Her `self->bad_area` branch - a forced attack when she
 *    is standing somewhere she should not be - is therefore gone, and she
 *    picks her attack from the normal path instead.
 *
 *  - monsterinfo.setskin does not exist here. widow_setskin is an ordinary
 *    helper called from pain and die, which is how m_gekk.c and m_mutant.c
 *    already flip a damage skin.
 *
 *  - The rerelease's range_to() is a float distance; this tree's range() is
 *    the classic enum, which is what rogue itself compared against. The
 *    chance ladder in Widow_CheckAttack uses our enum directly.
 *
 *  - PredictAim has a different signature here: the target is the first
 *    argument and the outputs are plain vec3_t, not pointers.
 *
 *  - st.was_key_specified has no equivalent; a zero field means "the map did
 *    not set it", exactly as m_guncmdr.c documents.
 *
 *  - She MIRRORS the player's powerups onto herself so a quad picked up in
 *    her arena does not trivialise the fight. That needed three new
 *    monsterinfo framenums, which are in the savegame table - leaving them
 *    out is the bug class that made base_height come back zero.
 *
 * =======================================================================
 */

#include "g_local.h"
#include "m_widow.h"

/* rogue counts these in seconds; this tree counts server frames */
#define RAIL_TIME           (3 * BASE_FRAMERATE)
#define BLASTER_TIME        (2 * BASE_FRAMERATE)
#define BLASTER2_DAMAGE     10
#define WIDOW_RAIL_DAMAGE   50

/* how far off her aiming angle a blaster bolt may stray, in degrees */
#define WIDOW_VARIANCE      15.0f

/* the legs she leaves behind */
#define MAX_LEGSFRAME       23
#define LEG_WAIT_TIME       (1 * BASE_FRAMERATE)

static int sound_pain1;
static int sound_pain2;
static int sound_pain3;
static int sound_rail;

static unsigned shotsfired;

/* Where the two stalkers come out of her belly, in her own axes. */
static const vec3_t widow_spawnpoints[] = {
    {30, 100, 16},
    {30, -100, 16}
};

/* The two beam-out effects she dies with. */
static const vec3_t widow_beameffects[] = {
    {12.58f, -43.71f, 68.88f},
    {3.43f, 58.72f, 68.41f}
};

/* Her nine-shot blaster sweep, one yaw offset per flash. */
static const float widow_sweep_angles[] = {
    32.0f, 26.0f, 20.0f, 10.0f, 0.0f, -6.5f, -13.0f, -27.0f, -41.0f
};

static const vec3_t stalker_mins = {-28, -28, -18};
static const vec3_t stalker_maxs = {28, 28, 18};

unsigned widow_damage_multiplier;

void widow_run(edict_t *self);
void widow_dead(edict_t *self);
void widow_attack_blaster(edict_t *self);
void widow_reattack_blaster(edict_t *self);
void widow_start_spawn(edict_t *self);
void widow_done_spawn(edict_t *self);
void widow_spawn_check(edict_t *self);
void widow_attack_rail(edict_t *self);
void widow_start_run_5(edict_t *self);
void widow_start_run_10(edict_t *self);
void widow_start_run_12(edict_t *self);
void WidowCalcSlots(edict_t *self);
void Widowlegs_Spawn(vec3_t startpos, vec3_t angles);

extern mmove_t widow_move_attack_post_blaster;
extern mmove_t widow_move_attack_post_blaster_r;
extern mmove_t widow_move_attack_post_blaster_l;
extern mmove_t widow_move_attack_blaster;
extern mmove_t widow_move_attack_rail;
extern mmove_t widow_move_attack_rail_l;
extern mmove_t widow_move_attack_rail_r;
extern mmove_t widow_move_run;
extern mmove_t widow_move_run_attack;
extern mmove_t widow_move_stand;

void widow_search(edict_t *self)
{
}

void widow_sight(edict_t *self, edict_t *other)
{
    self->monsterinfo.fire_framenum = 0;
}

/*
 * The angle to the enemy, 0 = dead ahead, positive to her right.
 */
static float target_angle(edict_t *self)
{
    vec3_t  target;
    float   enemy_yaw;

    VectorSubtract(self->s.origin, self->enemy->s.origin, target);
    enemy_yaw = self->s.angles[YAW] - vectoyaw(target);

    if (enemy_yaw < 0)
        enemy_yaw += 360.0f;

    /* this gets me 0 degrees = forward */
    enemy_yaw -= 180.0f;

    return enemy_yaw;
}

/*
 * Which torso frame points her gun at the enemy. Returning 0 means "she has
 * to turn her whole body", and she is put into one of the two trans moves.
 */
static int WidowTorso(edict_t *self)
{
    float enemy_yaw = target_angle(self);

    if (enemy_yaw >= 105) {
        self->monsterinfo.currentmove = &widow_move_attack_post_blaster_r;
        self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
        return 0;
    }

    if (enemy_yaw <= -75.0f) {
        self->monsterinfo.currentmove = &widow_move_attack_post_blaster_l;
        self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
        return 0;
    }

    if (enemy_yaw >= 95)
        return FRAME_fired03;
    else if (enemy_yaw >= 85)
        return FRAME_fired04;
    else if (enemy_yaw >= 75)
        return FRAME_fired05;
    else if (enemy_yaw >= 65)
        return FRAME_fired06;
    else if (enemy_yaw >= 55)
        return FRAME_fired07;
    else if (enemy_yaw >= 45)
        return FRAME_fired08;
    else if (enemy_yaw >= 35)
        return FRAME_fired09;
    else if (enemy_yaw >= 25)
        return FRAME_fired10;
    else if (enemy_yaw >= 15)
        return FRAME_fired11;
    else if (enemy_yaw >= 5)
        return FRAME_fired12;
    else if (enemy_yaw >= -5)
        return FRAME_fired13;
    else if (enemy_yaw >= -15)
        return FRAME_fired14;
    else if (enemy_yaw >= -25)
        return FRAME_fired15;
    else if (enemy_yaw >= -35)
        return FRAME_fired16;
    else if (enemy_yaw >= -45)
        return FRAME_fired17;
    else if (enemy_yaw >= -55)
        return FRAME_fired18;
    else if (enemy_yaw >= -65)
        return FRAME_fired19;
    else if (enemy_yaw >= -75)
        return FRAME_fired20;

    return 0;
}

void WidowBlaster(edict_t *self)
{
    vec3_t  forward, right, target, vec, targ_angles;
    vec3_t  start;
    int     flashnum;
    int     effect;

    if (!self->enemy || !self->enemy->inuse)
        return;

    shotsfired++;
    if (!(shotsfired % 4))
        effect = EF_BLASTER;
    else
        effect = 0;

    AngleVectors(self->s.angles, forward, right, NULL);

    if ((self->s.frame >= FRAME_spawn05) && (self->s.frame <= FRAME_spawn13)) {
        /* the nine-shot sweep she lays down while the hatch is open */
        flashnum = MZ2_WIDOW_BLASTER_SWEEP1 + self->s.frame - FRAME_spawn05;
        G_ProjectSource(self->s.origin, monster_flash_offset[flashnum], forward, right, start);

        VectorSubtract(self->enemy->s.origin, start, target);
        vectoangles(target, targ_angles);

        VectorCopy(self->s.angles, vec);
        vec[PITCH] += targ_angles[PITCH];
        vec[YAW] -= widow_sweep_angles[flashnum - MZ2_WIDOW_BLASTER_SWEEP1];

        AngleVectors(vec, forward, NULL, NULL);
        monster_fire_blaster2(self, start, forward,
                BLASTER2_DAMAGE * widow_damage_multiplier, 1000, flashnum, effect);
    } else if ((self->s.frame >= FRAME_fired02a) && (self->s.frame <= FRAME_fired20)) {
        vec3_t  angles;
        float   aim_angle, targ_angle;
        float   error;

        /* her torso tracks the enemy while her feet stay put */
        self->monsterinfo.aiflags |= AI_MANUAL_STEERING;

        self->monsterinfo.nextframe = WidowTorso(self);

        if (!self->monsterinfo.nextframe)
            self->monsterinfo.nextframe = self->s.frame;

        if (self->s.frame == FRAME_fired02a)
            flashnum = MZ2_WIDOW_BLASTER_0;
        else
            flashnum = MZ2_WIDOW_BLASTER_100 + self->s.frame - FRAME_fired03;

        G_ProjectSource(self->s.origin, monster_flash_offset[flashnum], forward, right, start);

        PredictAim(self->enemy, start, 1000, true, crandom() * 0.1f, forward, NULL);

        /* clamp it to within 15 degrees of where the arm is actually pointing */
        vectoangles(forward, angles);

        /* gives 100 -> -70 */
        aim_angle = (float)(100 - (10 * (flashnum - MZ2_WIDOW_BLASTER_100)));
        if (aim_angle <= 0)
            aim_angle += 360;

        targ_angle = self->s.angles[YAW] - angles[YAW];
        if (targ_angle <= 0)
            targ_angle += 360;

        error = aim_angle - targ_angle;

        /* positive error is to her left, which is the engine's positive
           direction - the aim_angle above calls positive RIGHT, hence the
           sign dance. rogue's own comment admits this was a mistake. */
        if (error > WIDOW_VARIANCE) {
            angles[YAW] = (self->s.angles[YAW] - aim_angle) + WIDOW_VARIANCE;
            AngleVectors(angles, forward, NULL, NULL);
        } else if (error < -WIDOW_VARIANCE) {
            angles[YAW] = (self->s.angles[YAW] - aim_angle) - WIDOW_VARIANCE;
            AngleVectors(angles, forward, NULL, NULL);
        }

        monster_fire_blaster2(self, start, forward,
                BLASTER2_DAMAGE * widow_damage_multiplier, 1000, flashnum, effect);
    } else if ((self->s.frame >= FRAME_run01) && (self->s.frame <= FRAME_run08)) {
        /* firing on the move */
        flashnum = MZ2_WIDOW_RUN_1 + self->s.frame - FRAME_run01;
        G_ProjectSource(self->s.origin, monster_flash_offset[flashnum], forward, right, start);

        VectorSubtract(self->enemy->s.origin, start, target);
        target[2] += self->enemy->viewheight;
        VectorNormalize(target);

        monster_fire_blaster2(self, start, target,
                BLASTER2_DAMAGE * widow_damage_multiplier, 1000, flashnum, effect);
    }
}

/*
 * Drop a pair of stalkers out of the hatch.
 */
void WidowSpawn(edict_t *self)
{
    vec3_t  f, r, u, offset, startpoint, spawnpoint;
    edict_t *ent, *designated_enemy;
    int     i;

    AngleVectors(self->s.angles, f, r, u);

    for (i = 0; i < 2; i++) {
        VectorCopy(widow_spawnpoints[i], offset);
        G_ProjectSource2(self->s.origin, offset, f, r, u, startpoint);

        if (!FindSpawnPoint(startpoint, (float *)stalker_mins, (float *)stalker_maxs, spawnpoint, 64))
            continue;

        ent = CreateGroundMonster(spawnpoint, self->s.angles,
                (float *)stalker_mins, (float *)stalker_maxs, "monster_stalker", 256);
        if (!ent)
            continue;

        self->monsterinfo.monster_used++;
        ent->monsterinfo.commander = self;

        /* walkmonster_start defers setup by a frame; run it now, the same way
           CarrierSpawn does for its flyers, or the enemy set below is undone */
        ent->nextthink = level.framenum;
        ent->think(ent);

        ent->monsterinfo.aiflags |= AI_SPAWNED_WIDOW | AI_DO_NOT_COUNT | AI_IGNORE_SHOTS;

        if (!coop->value) {
            designated_enemy = self->enemy;
        } else {
            designated_enemy = PickCoopTarget(ent);
            if (designated_enemy) {
                /* try to avoid using my own enemy */
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
            if (ent->monsterinfo.attack)
                ent->monsterinfo.attack(ent);
        }
    }
}

void widow_spawn_check(edict_t *self)
{
    WidowBlaster(self);
    WidowSpawn(self);
}

/*
 * The growing sphere that telegraphs where a stalker is about to appear.
 */
void widow_ready_spawn(edict_t *self)
{
    vec3_t  f, r, u, offset, startpoint, spawnpoint, mid;
    int     i;

    WidowBlaster(self);
    AngleVectors(self->s.angles, f, r, u);

    for (i = 0; i < 2; i++) {
        VectorCopy(widow_spawnpoints[i], offset);
        G_ProjectSource2(self->s.origin, offset, f, r, u, startpoint);

        if (!FindSpawnPoint(startpoint, (float *)stalker_mins, (float *)stalker_maxs, spawnpoint, 64))
            continue;

        /* The rerelease passes a start and end RADIUS; this tree's
           SpawnGrow_Spawn takes a size CLASS that picks the model, the way
           m_medic.c does it - a stalker is 36 units tall, so it is the small
           one. The offset is rogue's; for a symmetric bbox it is zero. */
        VectorAdd(stalker_mins, stalker_maxs, mid);
        VectorAdd(spawnpoint, mid, mid);

        SpawnGrow_Spawn(mid, 1);
    }
}

void widow_step(edict_t *self)
{
    gi.sound(self, CHAN_BODY, gi.soundindex("widow/bwstep3.wav"), 1, ATTN_NORM, 0);
}

static mframe_t widow_frames_stand[] = {
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand},
    {ai_stand}
};
mmove_t widow_move_stand = {
    FRAME_idle01,
    FRAME_idle11,
    widow_frames_stand,
    NULL
};

static mframe_t widow_frames_walk[] = {
    {ai_walk, 2.79f, widow_step},
    {ai_walk, 2.77f},
    {ai_walk, 3.53f},
    {ai_walk, 3.97f},
    {ai_walk, 4.13f},
    {ai_walk, 4.09f},
    {ai_walk, 3.84f},
    {ai_walk, 3.62f, widow_step},
    {ai_walk, 3.29f},
    {ai_walk, 6.08f},
    {ai_walk, 6.94f},
    {ai_walk, 5.73f},
    {ai_walk, 2.85f}
};
mmove_t widow_move_walk = {
    FRAME_walk01,
    FRAME_walk13,
    widow_frames_walk,
    NULL
};

static mframe_t widow_frames_run[] = {
    {ai_run, 2.79f, widow_step},
    {ai_run, 2.77f},
    {ai_run, 3.53f},
    {ai_run, 3.97f},
    {ai_run, 4.13f},
    {ai_run, 4.09f},
    {ai_run, 3.84f},
    {ai_run, 3.62f, widow_step},
    {ai_run, 3.29f},
    {ai_run, 6.08f},
    {ai_run, 6.94f},
    {ai_run, 5.73f},
    {ai_run, 2.85f}
};
mmove_t widow_move_run = {
    FRAME_walk01,
    FRAME_walk13,
    widow_frames_run,
    NULL
};

void widow_stepshoot(edict_t *self)
{
    gi.sound(self, CHAN_BODY, gi.soundindex("widow/bwstep2.wav"), 1, ATTN_NORM, 0);
    WidowBlaster(self);
}

static mframe_t widow_frames_run_attack[] = {
    {ai_charge, 13, widow_stepshoot},
    {ai_charge, 11.72f, WidowBlaster},
    {ai_charge, 18.04f, WidowBlaster},
    {ai_charge, 14.58f, WidowBlaster},
    {ai_charge, 13, widow_stepshoot},
    {ai_charge, 12.12f, WidowBlaster},
    {ai_charge, 19.63f, WidowBlaster},
    {ai_charge, 11.37f, WidowBlaster}
};
mmove_t widow_move_run_attack = {
    FRAME_run01,
    FRAME_run08,
    widow_frames_run_attack,
    widow_run
};

/*
 * These three allow specific entry into the run sequence.
 */

void widow_start_run_5(edict_t *self)
{
    self->monsterinfo.currentmove = &widow_move_run;
    self->monsterinfo.nextframe = FRAME_walk05;
}

void widow_start_run_10(edict_t *self)
{
    self->monsterinfo.currentmove = &widow_move_run;
    self->monsterinfo.nextframe = FRAME_walk10;
}

void widow_start_run_12(edict_t *self)
{
    self->monsterinfo.currentmove = &widow_move_run;
    self->monsterinfo.nextframe = FRAME_walk12;
}

static mframe_t widow_frames_attack_pre_blaster[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_attack_blaster}
};
mmove_t widow_move_attack_pre_blaster = {
    FRAME_fired01,
    FRAME_fired02a,
    widow_frames_attack_pre_blaster,
    NULL
};

/* Looped: every frame re-decides whether to keep firing. */
static mframe_t widow_frames_attack_blaster[] = {
    {ai_charge, 0, widow_reattack_blaster},     /* straight ahead */
    {ai_charge, 0, widow_reattack_blaster},     /* 100 degrees right */
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},     /* 50 degrees right */
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},     /* straight */
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster},     /* 50 degrees left */
    {ai_charge, 0, widow_reattack_blaster},
    {ai_charge, 0, widow_reattack_blaster}      /* 70 degrees left */
};
mmove_t widow_move_attack_blaster = {
    FRAME_fired02a,
    FRAME_fired20,
    widow_frames_attack_blaster,
    NULL
};

static mframe_t widow_frames_attack_post_blaster[] = {
    {ai_charge},
    {ai_charge}
};
mmove_t widow_move_attack_post_blaster = {
    FRAME_fired21,
    FRAME_fired22,
    widow_frames_attack_post_blaster,
    widow_run
};

static mframe_t widow_frames_attack_post_blaster_r[] = {
    {ai_charge, -2},
    {ai_charge, -10},
    {ai_charge, -2},
    {ai_charge},
    {ai_charge, 0, widow_start_run_12}
};
mmove_t widow_move_attack_post_blaster_r = {
    FRAME_transa01,
    FRAME_transa05,
    widow_frames_attack_post_blaster_r,
    NULL
};

static mframe_t widow_frames_attack_post_blaster_l[] = {
    {ai_charge},
    {ai_charge, 14},
    {ai_charge, -2},
    {ai_charge, 10},
    {ai_charge, 10, widow_start_run_12}
};
mmove_t widow_move_attack_post_blaster_l = {
    FRAME_transb01,
    FRAME_transb05,
    widow_frames_attack_post_blaster_l,
    NULL
};

void WidowRail(edict_t *self)
{
    vec3_t  start;
    vec3_t  dir;
    vec3_t  forward, right;
    int     flash;

    AngleVectors(self->s.angles, forward, right, NULL);

    if (self->monsterinfo.currentmove == &widow_move_attack_rail_l)
        flash = MZ2_WIDOW_RAIL_LEFT;
    else if (self->monsterinfo.currentmove == &widow_move_attack_rail_r)
        flash = MZ2_WIDOW_RAIL_RIGHT;
    else
        flash = MZ2_WIDOW_RAIL;

    G_ProjectSource(self->s.origin, monster_flash_offset[flash], forward, right, start);

    /* aim at where she marked, not where he is now - that is the lead */
    VectorSubtract(self->pos1, start, dir);
    VectorNormalize(dir);

    monster_fire_railgun(self, start, dir,
            WIDOW_RAIL_DAMAGE * widow_damage_multiplier, 100, flash);
    self->timestamp = level.framenum + RAIL_TIME;
}

void WidowSaveLoc(edict_t *self)
{
    if (!self->enemy)
        return;

    VectorCopy(self->enemy->s.origin, self->pos1);  /* save for aiming the shot */
    self->pos1[2] += self->enemy->viewheight;
}

void widow_start_rail(edict_t *self)
{
    self->monsterinfo.aiflags |= AI_MANUAL_STEERING;
}

void widow_rail_done(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
}

static mframe_t widow_frames_attack_pre_rail[] = {
    {ai_charge, 0, widow_start_rail},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_attack_rail}
};
mmove_t widow_move_attack_pre_rail = {
    FRAME_transc01,
    FRAME_transc04,
    widow_frames_attack_pre_rail,
    NULL
};

static mframe_t widow_frames_attack_rail[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, WidowSaveLoc},
    {ai_charge, -10, WidowRail},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_rail_done}
};
mmove_t widow_move_attack_rail = {
    FRAME_firea01,
    FRAME_firea09,
    widow_frames_attack_rail,
    widow_run
};

static mframe_t widow_frames_attack_rail_r[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, WidowSaveLoc},
    {ai_charge, -10, WidowRail},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_rail_done}
};
mmove_t widow_move_attack_rail_r = {
    FRAME_fireb01,
    FRAME_fireb09,
    widow_frames_attack_rail_r,
    widow_run
};

static mframe_t widow_frames_attack_rail_l[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, WidowSaveLoc},
    {ai_charge, -10, WidowRail},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_rail_done}
};
mmove_t widow_move_attack_rail_l = {
    FRAME_firec01,
    FRAME_firec09,
    widow_frames_attack_rail_l,
    widow_run
};

void widow_attack_rail(edict_t *self)
{
    float enemy_angle;

    enemy_angle = target_angle(self);

    if (enemy_angle < -15)
        self->monsterinfo.currentmove = &widow_move_attack_rail_l;
    else if (enemy_angle > 15)
        self->monsterinfo.currentmove = &widow_move_attack_rail_r;
    else
        self->monsterinfo.currentmove = &widow_move_attack_rail;
}

void widow_start_spawn(edict_t *self)
{
    self->monsterinfo.aiflags |= AI_MANUAL_STEERING;
}

void widow_done_spawn(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
}

static mframe_t widow_frames_spawn[] = {
    {ai_charge},                            /* 1 */
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_start_spawn},
    {ai_charge},                            /* 5 */
    {ai_charge, 0, WidowBlaster},           /* 6 */
    {ai_charge, 0, widow_ready_spawn},      /* 7 */
    {ai_charge, 0, WidowBlaster},
    {ai_charge, 0, WidowBlaster},           /* 9 */
    {ai_charge, 0, widow_spawn_check},
    {ai_charge, 0, WidowBlaster},           /* 11 */
    {ai_charge, 0, WidowBlaster},
    {ai_charge, 0, WidowBlaster},           /* 13 */
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow_done_spawn}
};
mmove_t widow_move_spawn = {
    FRAME_spawn01,
    FRAME_spawn18,
    widow_frames_spawn,
    widow_run
};

static mframe_t widow_frames_pain_heavy[] = {
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
mmove_t widow_move_pain_heavy = {
    FRAME_pain01,
    FRAME_pain13,
    widow_frames_pain_heavy,
    widow_run
};

static mframe_t widow_frames_pain_light[] = {
    {ai_move},
    {ai_move},
    {ai_move}
};
mmove_t widow_move_pain_light = {
    FRAME_pain201,
    FRAME_pain203,
    widow_frames_pain_light,
    widow_run
};

/*
 * death - she beams out and leaves her legs standing
 */

void spawn_out_start(edict_t *self)
{
    vec3_t startpoint, f, r, u;

    AngleVectors(self->s.angles, f, r, u);

    G_ProjectSource2(self->s.origin, widow_beameffects[0], f, r, u, startpoint);
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WIDOWBEAMOUT);
    gi.WriteShort(20001);
    gi.WritePosition(startpoint);
    gi.multicast(startpoint, MULTICAST_ALL);

    G_ProjectSource2(self->s.origin, widow_beameffects[1], f, r, u, startpoint);
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WIDOWBEAMOUT);
    gi.WriteShort(20002);
    gi.WritePosition(startpoint);
    gi.multicast(startpoint, MULTICAST_ALL);

    gi.sound(self, CHAN_VOICE, gi.soundindex("misc/bwidowbeamout.wav"), 1, ATTN_NORM, 0);
}

void spawn_out_do(edict_t *self)
{
    vec3_t startpoint, f, r, u;

    AngleVectors(self->s.angles, f, r, u);

    G_ProjectSource2(self->s.origin, widow_beameffects[0], f, r, u, startpoint);
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WIDOWSPLASH);
    gi.WritePosition(startpoint);
    gi.multicast(startpoint, MULTICAST_ALL);

    G_ProjectSource2(self->s.origin, widow_beameffects[1], f, r, u, startpoint);
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WIDOWSPLASH);
    gi.WritePosition(startpoint);
    gi.multicast(startpoint, MULTICAST_ALL);

    VectorCopy(self->s.origin, startpoint);
    startpoint[2] += 36;
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_BOSSTPORT);
    gi.WritePosition(startpoint);
    gi.multicast(startpoint, MULTICAST_PHS);

    Widowlegs_Spawn(self->s.origin, self->s.angles);

    G_FreeEdict(self);
}

static mframe_t widow_frames_death[] = {
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move, 0, spawn_out_start},          /* 10 */
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
    {ai_move, 0, spawn_out_do}              /* 31 */
};
mmove_t widow_move_death = {
    FRAME_death01,
    FRAME_death31,
    widow_frames_death,
    NULL
};

void widow_attack_kick(edict_t *self)
{
    vec3_t aim;

    VectorSet(aim, 100, 0, 4);

    if (self->enemy && self->enemy->groundentity)
        fire_hit(self, aim, 50 + (Q_rand() % 6), 500);
    else
        /* less kick if he is in the air - makes it harder to land on her head */
        fire_hit(self, aim, 50 + (Q_rand() % 6), 250);
}

static mframe_t widow_frames_attack_kick[] = {
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move, 0, widow_attack_kick},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move}
};
mmove_t widow_move_attack_kick = {
    FRAME_kick01,
    FRAME_kick08,
    widow_frames_attack_kick,
    widow_run
};

void widow_stand(edict_t *self)
{
    gi.sound(self, CHAN_WEAPON, gi.soundindex("widow/laugh.wav"), 1, ATTN_NORM, 0);
    self->monsterinfo.currentmove = &widow_move_stand;
}

void widow_run(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &widow_move_stand;
    else
        self->monsterinfo.currentmove = &widow_move_run;
}

void widow_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &widow_move_walk;
}

void widow_attack(edict_t *self)
{
    float   luck;
    bool    rail_frames = false, blaster_frames = false;
    bool    blocked = false, anger = false;

    self->movetarget = NULL;

    if (self->monsterinfo.aiflags & AI_BLOCKED) {
        blocked = true;
        self->monsterinfo.aiflags &= ~AI_BLOCKED;
    }

    if (self->monsterinfo.aiflags & AI_TARGET_ANGER) {
        anger = true;
        self->monsterinfo.aiflags &= ~AI_TARGET_ANGER;
    }

    if (!self->enemy || !self->enemy->inuse)
        return;

    /* rogue's self->bad_area branch lived here. There is no trigger_bad_area
       in this tree (m_carrier.c dropped it for the same reason), so she falls
       through to the ordinary choice below. */

    /* walk13, walk01-03 are the railgun entry frames;
       walk09-12 are the spawn and blaster entry frames */
    if ((self->s.frame == FRAME_walk13) ||
        ((self->s.frame >= FRAME_walk01) && (self->s.frame <= FRAME_walk03)))
        rail_frames = true;

    if ((self->s.frame >= FRAME_walk09) && (self->s.frame <= FRAME_walk12))
        blaster_frames = true;

    WidowCalcSlots(self);

    /* if she cannot see him, spawn regardless of frame */
    if ((self->monsterinfo.attack_state == AS_BLIND) && (M_SlotsLeft(self) >= 2)) {
        self->monsterinfo.currentmove = &widow_move_spawn;
        return;
    }

    /* a block biases her hard towards spawning */
    if (blocked && (M_SlotsLeft(self) >= 2)) {
        self->monsterinfo.currentmove = &widow_move_spawn;
        return;
    }

    if ((realrange(self, self->enemy) > 300) && (!anger) && (random() < 0.5f) && (!blocked)) {
        self->monsterinfo.currentmove = &widow_move_run_attack;
        return;
    }

    if (blaster_frames) {
        if (M_SlotsLeft(self) >= 2) {
            self->monsterinfo.currentmove = &widow_move_spawn;
            return;
        } else if (self->monsterinfo.fire_framenum + BLASTER_TIME <= level.framenum) {
            self->monsterinfo.currentmove = &widow_move_attack_pre_blaster;
            return;
        }
    }

    if (rail_frames) {
        if (!(level.framenum < self->timestamp)) {
            gi.sound(self, CHAN_WEAPON, sound_rail, 1, ATTN_NORM, 0);
            self->monsterinfo.currentmove = &widow_move_attack_pre_rail;
        }
    }

    if (rail_frames || blaster_frames)
        return;

    luck = random();

    if (M_SlotsLeft(self) >= 2) {
        if ((luck <= 0.40f) && (self->monsterinfo.fire_framenum + BLASTER_TIME <= level.framenum)) {
            self->monsterinfo.currentmove = &widow_move_attack_pre_blaster;
        } else if ((luck <= 0.7f) && !(level.framenum < self->timestamp)) {
            gi.sound(self, CHAN_WEAPON, sound_rail, 1, ATTN_NORM, 0);
            self->monsterinfo.currentmove = &widow_move_attack_pre_rail;
        } else {
            self->monsterinfo.currentmove = &widow_move_spawn;
        }
    } else {
        if (level.framenum < self->timestamp) {
            self->monsterinfo.currentmove = &widow_move_attack_pre_blaster;
        } else if ((luck <= 0.50f) || (level.framenum + BLASTER_TIME >= self->monsterinfo.fire_framenum)) {
            gi.sound(self, CHAN_WEAPON, sound_rail, 1, ATTN_NORM, 0);
            self->monsterinfo.currentmove = &widow_move_attack_pre_rail;
        } else {
            /* hold out for the blaster */
            self->monsterinfo.currentmove = &widow_move_attack_pre_blaster;
        }
    }
}

void widow_attack_blaster(edict_t *self)
{
    self->monsterinfo.fire_framenum = level.framenum +
            (int)((1.0f + random() * 2.0f) * BASE_FRAMERATE);
    self->monsterinfo.currentmove = &widow_move_attack_blaster;
    self->monsterinfo.nextframe = WidowTorso(self);
}

void widow_reattack_blaster(edict_t *self)
{
    WidowBlaster(self);

    /* if WidowBlaster bailed us out of the frames, just bail */
    if ((self->monsterinfo.currentmove == &widow_move_attack_post_blaster_l) ||
        (self->monsterinfo.currentmove == &widow_move_attack_post_blaster_r))
        return;

    /* not done with the attack yet - stay in the sequence */
    if (self->monsterinfo.fire_framenum >= level.framenum)
        return;

    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;

    self->monsterinfo.currentmove = &widow_move_attack_post_blaster;
}

/*
 * monsterinfo.setskin does not exist in this tree; this is called directly
 * from pain and die, the way m_gekk.c and m_mutant.c flip a damage skin.
 */
static void widow_setskin(edict_t *self)
{
    if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;
    else
        self->s.skinnum = 0;
}

void widow_pain(edict_t *self, edict_t *other /* unused */,
        float kick /* unused */, int damage)
{
    widow_setskin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 5 * BASE_FRAMERATE;

    if (damage < 15)
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NONE, 0);
    else if (damage < 75)
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NONE, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_pain3, 1, ATTN_NONE, 0);

    // M_ShouldReactToPain does not exist here; skill 3 is the nightmare gate
    // the rest of this tree uses for the same purpose.
    if (skill->value >= 3)
        return; // no pain anims in nightmare

    self->monsterinfo.fire_framenum = 0;

    if (damage >= 15) {
        if (damage < 75) {
            if (random() < (0.6f - (0.2f * skill->value))) {
                self->monsterinfo.currentmove = &widow_move_pain_light;
                self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
            }
        } else {
            if (random() < (0.75f - (0.1f * skill->value))) {
                self->monsterinfo.currentmove = &widow_move_pain_heavy;
                self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
            }
        }
    }
}

void widow_dead(edict_t *self)
{
    VectorSet(self->mins, -56, -56, 0);
    VectorSet(self->maxs, 56, 56, 80);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

void widow_die(edict_t *self, edict_t *inflictor /* unused */,
        edict_t *attacker /* unused */, int damage /* unused */,
        vec3_t point /* unused */)
{
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_NO;
    self->count = 0;
    self->monsterinfo.quad_framenum = 0;
    self->monsterinfo.double_framenum = 0;
    self->monsterinfo.invincible_framenum = 0;
    widow_setskin(self);
    self->monsterinfo.currentmove = &widow_move_death;
}

void widow_melee(edict_t *self)
{
    self->monsterinfo.currentmove = &widow_move_attack_kick;
}

/*
 * She mirrors whatever powerup the player is running, so a quad picked up in
 * her arena does not simply delete her.
 */

static void WidowGoinQuad(edict_t *self, int framenum)
{
    self->monsterinfo.quad_framenum = framenum;
    widow_damage_multiplier = 4;
}

static void WidowDouble(edict_t *self, int framenum)
{
    self->monsterinfo.double_framenum = framenum;
    widow_damage_multiplier = 2;
}

static void WidowPent(edict_t *self, int framenum)
{
    self->monsterinfo.invincible_framenum = framenum;
}

static void WidowPowerArmor(edict_t *self)
{
    self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;
    if (self->monsterinfo.power_armor_power <= 0)
        self->monsterinfo.power_armor_power += 250 * (int)skill->value;
}

static void WidowRespondPowerup(edict_t *self, edict_t *other)
{
    if (!other || !other->client)
        return;

    if (other->s.effects & EF_QUAD) {
        if (skill->value == 1)
            WidowDouble(self, other->client->quad_framenum);
        else if (skill->value == 2)
            WidowGoinQuad(self, other->client->quad_framenum);
        else if (skill->value == 3) {
            WidowGoinQuad(self, other->client->quad_framenum);
            WidowPowerArmor(self);
        }
    } else if (other->s.effects & EF_DOUBLE) {
        if (skill->value == 2)
            WidowDouble(self, other->client->double_framenum);
        else if (skill->value == 3) {
            WidowDouble(self, other->client->double_framenum);
            WidowPowerArmor(self);
        }
    } else {
        widow_damage_multiplier = 1;
    }

    if (other->s.effects & EF_PENT) {
        if (skill->value == 1)
            WidowPowerArmor(self);
        else if (skill->value == 2)
            WidowPent(self, other->client->invincible_framenum);
        else if (skill->value == 3) {
            WidowPent(self, other->client->invincible_framenum);
            WidowPowerArmor(self);
        }
    }
}

/* shared with m_widow2.c - stage 2 mirrors powerups the same way */
void WidowPowerups(edict_t *self)
{
    edict_t *ent;
    int     player;
    int     effect;
    int     pass;

    if (!coop->value) {
        WidowRespondPowerup(self, self->enemy);
        return;
    }

    /* in coop, check for pents, then quads, then doubles */
    for (pass = 0; pass < 3; pass++) {
        effect = (pass == 0) ? EF_PENT : (pass == 1) ? EF_QUAD : EF_DOUBLE;

        for (player = 1; player <= game.maxclients; player++) {
            ent = &g_edicts[player];
            if (!ent->inuse)
                continue;
            if (!ent->client)
                continue;
            if (ent->s.effects & effect) {
                WidowRespondPowerup(self, ent);
                return;
            }
        }
    }
}

bool Widow_CheckAttack(edict_t *self)
{
    vec3_t  spot1, spot2;
    vec3_t  temp;
    float   chance;
    trace_t tr;
    float   enemy_yaw;
    float   real_enemy_range;
    int     enemy_range;

    if (!self->enemy || !self->enemy->inuse)
        return false;

    WidowPowerups(self);

    if (self->monsterinfo.currentmove == &widow_move_run) {
        /* only some of the run frames are good to fire from:
           1,2,3,9,10,11,13 */
        switch (self->s.frame) {
        case FRAME_walk04:
        case FRAME_walk05:
        case FRAME_walk06:
        case FRAME_walk07:
        case FRAME_walk08:
        case FRAME_walk12:
            return false;
        default:
            break;
        }
    }

    /* a LARGE bias towards spawning when there is room.
       AI_BLOCKED is the signal to widow_attack that it should spawn. */
    if ((random() < 0.8f) && (M_SlotsLeft(self) >= 2) &&
        (realrange(self, self->enemy) > 150)) {
        self->monsterinfo.aiflags |= AI_BLOCKED;
        self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    if (self->enemy->health > 0) {
        /* see if anything is in the way of the shot */
        VectorCopy(self->s.origin, spot1);
        spot1[2] += self->viewheight;
        VectorCopy(self->enemy->s.origin, spot2);
        spot2[2] += self->enemy->viewheight;

        tr = gi.trace(spot1, NULL, NULL, spot2, self,
                      CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_SLIME | CONTENTS_LAVA);

        /* do we have a clear shot? */
        if (tr.ent != self->enemy) {
            /* go ahead and spawn stuff if she is angry at a client */
            if (self->enemy->client && M_SlotsLeft(self) >= 2) {
                self->monsterinfo.attack_state = AS_BLIND;
                return true;
            }

            /* PGM - we want them to shoot at info_notnulls if they can */
            if (self->enemy->solid != SOLID_NOT || tr.fraction < 1.0f)
                return false;
        }
    }

    enemy_range = range(self, self->enemy);
    VectorSubtract(self->enemy->s.origin, self->s.origin, temp);
    enemy_yaw = vectoyaw(temp);

    self->ideal_yaw = enemy_yaw;

    real_enemy_range = realrange(self, self->enemy);

    /* melee attack */
    if (real_enemy_range <= (MELEE_DISTANCE + 20)) {
        /* don't always melee in easy mode */
        if (skill->value == 0 && (Q_rand() % 4))
            return false;

        if (self->monsterinfo.melee)
            self->monsterinfo.attack_state = AS_MELEE;
        else
            self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    if (level.framenum < self->monsterinfo.attack_finished)
        return false;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        chance = 0.4f;
    else if (enemy_range <= RANGE_MELEE)
        chance = 0.8f;
    else if (enemy_range <= RANGE_NEAR)
        chance = 0.7f;
    else if (enemy_range <= RANGE_MID)
        chance = 0.6f;
    else
        chance = 0.5f;

    /* PGM - always shoot at an info_notnull */
    if ((random() < chance) || (self->enemy->solid == SOLID_NOT)) {
        self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    return false;
}

bool widow_blocked(edict_t *self, float dist)
{
    /* If she gets blocked while running-and-gunning, raise a flag that means
       nothing in this context and ask for a new attack sequence. rogue uses
       AI_TARGET_ANGER for it; so do we. */
    if (self->monsterinfo.currentmove == &widow_move_run_attack) {
        self->monsterinfo.aiflags |= AI_TARGET_ANGER;
        if (self->monsterinfo.checkattack(self))
            self->monsterinfo.attack(self);
        else
            self->monsterinfo.run(self);
        return true;
    }

    return false;
}

void WidowCalcSlots(edict_t *self)
{
    switch ((int)skill->value) {
    case 0:
    case 1:
        self->monsterinfo.monster_slots = 3;
        break;
    case 2:
        self->monsterinfo.monster_slots = 4;
        break;
    case 3:
        self->monsterinfo.monster_slots = 6;
        break;
    default:
        self->monsterinfo.monster_slots = 3;
        break;
    }

    if (coop->value) {
        self->monsterinfo.monster_slots =
                min(6, self->monsterinfo.monster_slots +
                        ((int)skill->value * (CountPlayers() - 1)));
    }
}

void WidowPrecache(void)
{
    /* cache in all of the stalker stuff, widow stuff, spawngro stuff, gibs */
    gi.soundindex("stalker/pain.wav");
    gi.soundindex("stalker/death.wav");
    gi.soundindex("stalker/sight.wav");
    gi.soundindex("stalker/melee1.wav");
    gi.soundindex("stalker/melee2.wav");
    gi.soundindex("stalker/idle.wav");

    gi.soundindex("tank/tnkatck3.wav");
    gi.modelindex("models/objects/laser/tris.md2");

    gi.modelindex("models/monsters/stalker/tris.md2");
    gi.modelindex("models/items/spawngro3/tris.md2");
    gi.modelindex("models/objects/gibs/sm_metal/tris.md2");
    gi.modelindex("models/objects/gibs/gear/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib1/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib2/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib3/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib4/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib1/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib2/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib3/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib4/tris.md2");
    gi.modelindex("models/monsters/legs/tris.md2");
    gi.soundindex("misc/bwidowbeamout.wav");

    gi.soundindex("misc/bigtele.wav");
    gi.soundindex("widow/bwstep3.wav");
    gi.soundindex("widow/bwstep2.wav");
    gi.soundindex("widow/bwstep1.wav");
}

/*
 * The legs she leaves standing after she beams out. Lives here rather than in
 * rogue's g_rogue_spawn.cpp because nothing else in this tree spawns them.
 */

void widowlegs_think(edict_t *self)
{
    vec3_t offset;
    vec3_t point;
    vec3_t f, r, u;

    if (self->s.frame == 17) {
        VectorSet(offset, 11.77f, -7.24f, 23.31f);
        AngleVectors(self->s.angles, f, r, u);
        G_ProjectSource2(self->s.origin, offset, f, r, u, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_EXPLOSION1);
        gi.WritePosition(point);
        gi.multicast(point, MULTICAST_ALL);
        ThrowGib(self, "models/objects/gibs/sm_metal/tris.md2", 30, GIB_METALLIC);
    }

    if (self->s.frame < MAX_LEGSFRAME) {
        self->s.frame++;
        self->nextthink = level.framenum + 1;
        return;
    } else if (self->wait == 0) {
        self->wait = level.framenum + LEG_WAIT_TIME;
    }

    if (level.framenum > self->wait) {
        AngleVectors(self->s.angles, f, r, u);

        VectorSet(offset, -65.6f, -8.44f, 28.59f);
        G_ProjectSource2(self->s.origin, offset, f, r, u, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_EXPLOSION1);
        gi.WritePosition(point);
        gi.multicast(point, MULTICAST_ALL);

        ThrowGib(self, "models/monsters/blackwidow/gib1/tris.md2", 80 + (int)(random() * 20), GIB_METALLIC);
        ThrowGib(self, "models/monsters/blackwidow/gib2/tris.md2", 80 + (int)(random() * 20), GIB_METALLIC);

        VectorSet(offset, -1.04f, -51.18f, 7.04f);
        G_ProjectSource2(self->s.origin, offset, f, r, u, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_EXPLOSION1);
        gi.WritePosition(point);
        gi.multicast(point, MULTICAST_ALL);

        ThrowGib(self, "models/monsters/blackwidow/gib1/tris.md2", 80 + (int)(random() * 20), GIB_METALLIC);
        ThrowGib(self, "models/monsters/blackwidow/gib2/tris.md2", 80 + (int)(random() * 20), GIB_METALLIC);
        ThrowGib(self, "models/monsters/blackwidow/gib3/tris.md2", 80 + (int)(random() * 20), GIB_METALLIC);

        G_FreeEdict(self);
        return;
    }

    if ((level.framenum > self->wait - (BASE_FRAMERATE / 2)) && (self->count == 0)) {
        self->count = 1;
        AngleVectors(self->s.angles, f, r, u);

        VectorSet(offset, 31, -88.7f, 10.96f);
        G_ProjectSource2(self->s.origin, offset, f, r, u, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_EXPLOSION1);
        gi.WritePosition(point);
        gi.multicast(point, MULTICAST_ALL);

        VectorSet(offset, -12.67f, -4.39f, 15.68f);
        G_ProjectSource2(self->s.origin, offset, f, r, u, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_EXPLOSION1);
        gi.WritePosition(point);
        gi.multicast(point, MULTICAST_ALL);

        self->nextthink = level.framenum + 1;
        return;
    }

    self->nextthink = level.framenum + 1;
}

void Widowlegs_Spawn(vec3_t startpos, vec3_t angles)
{
    edict_t *ent;

    ent = G_Spawn();
    VectorCopy(startpos, ent->s.origin);
    VectorCopy(angles, ent->s.angles);
    ent->solid = SOLID_NOT;
    ent->s.renderfx = RF_IR_VISIBLE;
    ent->movetype = MOVETYPE_NONE;
    ent->classname = "widowlegs";

    ent->s.modelindex = gi.modelindex("models/monsters/legs/tris.md2");
    ent->think = widowlegs_think;

    ent->nextthink = level.framenum + 1;
    gi.linkentity(ent);
}

/*
 * QUAKED monster_widow (1 .5 0) (-40 -40 0) (40 40 144) Ambush Trigger_Spawn Sight
 */
void SP_monster_widow(edict_t *self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via walkmonster_start below.

    sound_pain1 = gi.soundindex("widow/bw1pain1.wav");
    sound_pain2 = gi.soundindex("widow/bw1pain2.wav");
    sound_pain3 = gi.soundindex("widow/bw1pain3.wav");
    sound_rail = gi.soundindex("gladiator/railgun.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/blackwidow/tris.md2");
    VectorSet(self->mins, -40, -40, 0);
    VectorSet(self->maxs, 40, 40, 144);

    // st.health_multiplier is 0 here unless the map sets it (the rerelease
    // defaults it to 1), so multiplying unguarded spawns the monster DEAD.
    // Same one-line bug that broke guncmdr, shambler and guardian.
    self->health = 2000 + 1000 * (int)skill->value;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
    if (coop->value)
        self->health += 500 * (int)skill->value;

    self->gib_health = -5000;
    self->mass = 1500;

    if (skill->value == 3) {
        // st.was_key_specified has no equivalent here; a zero field means the
        // map did not set it, exactly as m_guncmdr.c documents.
        if (!self->monsterinfo.power_armor_type)
            self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;
        if (!self->monsterinfo.power_armor_power)
            self->monsterinfo.power_armor_power = 500;
    }

    self->yaw_speed = 30;

    self->flags |= FL_IMMUNE_LASER;
    self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

    self->pain = widow_pain;
    self->die = widow_die;

    self->monsterinfo.melee = widow_melee;
    self->monsterinfo.stand = widow_stand;
    self->monsterinfo.walk = widow_walk;
    self->monsterinfo.run = widow_run;
    self->monsterinfo.attack = widow_attack;
    self->monsterinfo.search = widow_search;
    self->monsterinfo.checkattack = Widow_CheckAttack;
    self->monsterinfo.sight = widow_sight;
    self->monsterinfo.blocked = widow_blocked;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &widow_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    WidowPrecache();
    WidowCalcSlots(self);
    widow_damage_multiplier = 1;

    walkmonster_start(self);
}
