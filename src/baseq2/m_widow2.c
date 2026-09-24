/* =======================================================================
 *
 * The Black Widow, stage 2 - rogue's final boss after she sheds her shell.
 *
 * Bigger, meaner and differently armed: a plasma beam she sweeps across the
 * room, a homing disruptor bolt, the same stalker hatch, and a TONGUE that
 * reels you in and crunches you. Her death is a 44-frame chain of scripted
 * explosions that tears her apart a piece at a time, then a search animation
 * that loops three times before she finally settles.
 *
 * Ported from src/rerelease/rogue/m_rogue_widow2.cpp, sharing the support
 * layer built for m_widow.c (G_ProjectSource2, CountPlayers, AI_BLOCKED, the
 * powerup mirroring) and adding two monster weapon wrappers to g_monster.c -
 * monster_fire_heatbeam and monster_fire_tracker.
 *
 * Deliberate differences from the rerelease, none of them accidental:
 *
 *  - No trigger_bad_area in this tree, so her `self->bad_area` forced-attack
 *    branch is gone. Same call m_carrier.c and m_widow.c made.
 *
 *  - M_CheckGib() does not exist here; the classic health <= gib_health test
 *    is what this tree uses to decide an over-kill gib.
 *
 *  - monsterinfo.setskin does not exist; widow2_setskin is called directly
 *    from pain and die.
 *
 *  - RF_DOT_SHADOW does not exist in this renderer, so the gib code does not
 *    clear it.
 *
 *  - MASK_PROJECTILE is spelled MASK_SHOT here.
 *
 *  - The rerelease uses a C++ lambda for one spawn frame; it is a named
 *    function (widow2_start_spawn_step) so genptr.py can see it.
 *
 * =======================================================================
 */

#include "g_local.h"
#include "m_widow2.h"

#define WIDOW2_TONGUE_RANGE 256.0f

static int sound_pain1;
static int sound_pain2;
static int sound_pain3;
static int sound_death;
static int sound_search1;
static int sound_tentacles_retract;

/* sqrt(64*64*2) + sqrt(28*28*2) => 130.1 */
static const vec3_t widow2_spawnpoints[] = {
    {30, 135, 0},
    {30, -135, 0}
};

static const float widow2_sweep_angles[] = {
    -40.0f, -32.0f, -24.0f, -16.0f, -8.0f, 0.0f, 8.0f, 16.0f, 24.0f, 32.0f, 40.0f
};

static const vec3_t stalker_mins = {-28, -28, -18};
static const vec3_t stalker_maxs = {28, 28, 18};

/* where the tongue comes out of her head, one per tongs frame */
static const vec3_t widow2_tongue_offsets[] = {
    {17.48f, 0.10f, 68.92f},
    {17.47f, 0.29f, 68.91f},
    {17.45f, 0.53f, 68.87f},
    {17.42f, 0.78f, 68.81f},
    {17.39f, 1.02f, 68.75f},
    {17.37f, 1.20f, 68.70f},
    {17.36f, 1.24f, 68.71f},
    {17.37f, 1.21f, 68.72f}
};

/* g_misc.c has these but g_local.h does not declare them */
void gib_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point);
void ClipGibVelocity(edict_t *ent);

void WidowCalcSlots(edict_t *self);
void WidowPowerups(edict_t *self);
void widow_start_spawn(edict_t *self);
void widow_done_spawn(edict_t *self);

void widow2_run(edict_t *self);
void widow2_dead(edict_t *self);
void widow2_attack_beam(edict_t *self);
void widow2_reattack_beam(edict_t *self);
void widow2_spawn_check(edict_t *self);
void Widow2SaveBeamTarget(edict_t *self);
void widow2_step(edict_t *self);

/* death sequence */
void WidowExplode(edict_t *self);
void WidowExplosion1(edict_t *self);
void WidowExplosion2(edict_t *self);
void WidowExplosion3(edict_t *self);
void WidowExplosion4(edict_t *self);
void WidowExplosion5(edict_t *self);
void WidowExplosion6(edict_t *self);
void WidowExplosion7(edict_t *self);
void WidowExplosionLeg(edict_t *self);
void ThrowArm1(edict_t *self);
void ThrowArm2(edict_t *self);
void ThrowWidowGibReal(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, bool sized, int hitsound, bool fade);
void ThrowWidowGibLoc(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, bool fade);
void ThrowWidowGibSized(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, int hitsound, bool fade);
void ThrowSmallStuff(edict_t *self, const vec3_t point);

extern mmove_t widow2_move_stand;
extern mmove_t widow2_move_run;
extern mmove_t widow2_move_walk;
extern mmove_t widow2_move_attack_beam;
extern mmove_t widow2_move_attack_post_beam;
extern mmove_t widow2_move_spawn;
extern mmove_t widow2_move_dead;
extern mmove_t widow2_move_really_dead;
extern mmove_t widow2_move_death;

void widow2_search(edict_t *self)
{
    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, sound_search1, 1, ATTN_NONE, 0);
}

void Widow2Beam(edict_t *self)
{
    vec3_t  forward, right, target;
    vec3_t  start, targ_angles, vec;
    int     flashnum;

    if (!self->enemy || !self->enemy->inuse)
        return;

    AngleVectors(self->s.angles, forward, right, NULL);

    if ((self->s.frame >= FRAME_fireb05) && (self->s.frame <= FRAME_fireb09)) {
        /* the regular aimed beam */
        Widow2SaveBeamTarget(self);
        flashnum = MZ2_WIDOW2_BEAMER_1 + self->s.frame - FRAME_fireb05;
        G_ProjectSource(self->s.origin, monster_flash_offset[flashnum], forward, right, start);

        VectorCopy(self->pos2, target);
        target[2] += self->enemy->viewheight - 10;
        VectorSubtract(target, start, forward);
        VectorNormalize(forward);

        monster_fire_heatbeam(self, start, forward, vec3_origin, 10, 50, flashnum);
    } else if ((self->s.frame >= FRAME_spawn04) && (self->s.frame <= FRAME_spawn14)) {
        /* the eleven-shot sweep she lays down while the hatch is open */
        flashnum = MZ2_WIDOW2_BEAM_SWEEP_1 + self->s.frame - FRAME_spawn04;
        G_ProjectSource(self->s.origin, monster_flash_offset[flashnum], forward, right, start);

        VectorSubtract(self->enemy->s.origin, start, target);
        vectoangles(target, targ_angles);

        VectorCopy(self->s.angles, vec);
        vec[PITCH] += targ_angles[PITCH];
        vec[YAW] -= widow2_sweep_angles[flashnum - MZ2_WIDOW2_BEAM_SWEEP_1];

        AngleVectors(vec, forward, NULL, NULL);
        monster_fire_heatbeam(self, start, forward, vec3_origin, 10, 50, flashnum);
    } else {
        Widow2SaveBeamTarget(self);
        G_ProjectSource(self->s.origin, monster_flash_offset[MZ2_WIDOW2_BEAMER_1],
                forward, right, start);

        VectorCopy(self->pos2, target);
        target[2] += self->enemy->viewheight - 10;

        VectorSubtract(target, start, forward);
        VectorNormalize(forward);

        monster_fire_heatbeam(self, start, forward, vec3_origin, 10, 50, MZ2_WIDOW2_BEAM_SWEEP_1);
    }
}

void Widow2Spawn(edict_t *self)
{
    vec3_t  f, r, u, offset, startpoint, spawnpoint;
    edict_t *ent, *designated_enemy;
    int     i;

    AngleVectors(self->s.angles, f, r, u);

    for (i = 0; i < 2; i++) {
        VectorCopy(widow2_spawnpoints[i], offset);
        G_ProjectSource2(self->s.origin, offset, f, r, u, startpoint);

        if (!FindSpawnPoint(startpoint, (float *)stalker_mins, (float *)stalker_maxs, spawnpoint, 64))
            continue;

        ent = CreateGroundMonster(spawnpoint, self->s.angles,
                (float *)stalker_mins, (float *)stalker_maxs, "monster_stalker", 256);
        if (!ent)
            continue;

        self->monsterinfo.monster_used++;
        ent->monsterinfo.commander = self;

        /* run the deferred start now, or the enemy set below is undone */
        ent->nextthink = level.framenum;
        ent->think(ent);

        ent->monsterinfo.aiflags |= AI_SPAWNED_WIDOW | AI_DO_NOT_COUNT | AI_IGNORE_SHOTS;

        if (!coop->value) {
            designated_enemy = self->enemy;
        } else {
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
            if (ent->monsterinfo.attack)
                ent->monsterinfo.attack(ent);
        }
    }
}

void widow2_spawn_check(edict_t *self)
{
    Widow2Beam(self);
    Widow2Spawn(self);
}

void widow2_ready_spawn(edict_t *self)
{
    vec3_t  f, r, u, offset, startpoint, spawnpoint, mid;
    int     i;

    Widow2Beam(self);
    AngleVectors(self->s.angles, f, r, u);

    for (i = 0; i < 2; i++) {
        VectorCopy(widow2_spawnpoints[i], offset);
        G_ProjectSource2(self->s.origin, offset, f, r, u, startpoint);

        if (!FindSpawnPoint(startpoint, (float *)stalker_mins, (float *)stalker_maxs, spawnpoint, 64))
            continue;

        /* this tree's SpawnGrow_Spawn takes a size CLASS, not a radius pair */
        VectorAdd(stalker_mins, stalker_maxs, mid);
        VectorAdd(spawnpoint, mid, mid);

        SpawnGrow_Spawn(mid, 1);
    }
}

void widow2_step(edict_t *self)
{
    gi.sound(self, CHAN_BODY, gi.soundindex("widow/bwstep1.wav"), 1, ATTN_NORM, 0);
}

/* The rerelease uses a lambda here; genptr.py needs a real name. */
void widow2_start_spawn_step(edict_t *self)
{
    widow_start_spawn(self);
    widow2_step(self);
}

static mframe_t widow2_frames_stand[] = {
    {ai_stand}
};
mmove_t widow2_move_stand = {
    FRAME_blackwidow3,
    FRAME_blackwidow3,
    widow2_frames_stand,
    NULL
};

static mframe_t widow2_frames_walk[] = {
    {ai_walk, 9.01f, widow2_step},
    {ai_walk, 7.55f},
    {ai_walk, 7.01f},
    {ai_walk, 6.66f},
    {ai_walk, 6.20f},
    {ai_walk, 5.78f, widow2_step},
    {ai_walk, 7.25f},
    {ai_walk, 8.37f},
    {ai_walk, 10.41f}
};
mmove_t widow2_move_walk = {
    FRAME_walk01,
    FRAME_walk09,
    widow2_frames_walk,
    NULL
};

static mframe_t widow2_frames_run[] = {
    {ai_run, 9.01f, widow2_step},
    {ai_run, 7.55f},
    {ai_run, 7.01f},
    {ai_run, 6.66f},
    {ai_run, 6.20f},
    {ai_run, 5.78f, widow2_step},
    {ai_run, 7.25f},
    {ai_run, 8.37f},
    {ai_run, 10.41f}
};
mmove_t widow2_move_run = {
    FRAME_walk01,
    FRAME_walk09,
    widow2_frames_run,
    NULL
};

static mframe_t widow2_frames_attack_pre_beam[] = {
    {ai_charge, 4},
    {ai_charge, 4, widow2_step},
    {ai_charge, 4},
    {ai_charge, 4, widow2_attack_beam}
};
mmove_t widow2_move_attack_pre_beam = {
    FRAME_fireb01,
    FRAME_fireb04,
    widow2_frames_attack_pre_beam,
    NULL
};

/* Looped. */
static mframe_t widow2_frames_attack_beam[] = {
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, widow2_reattack_beam}
};
mmove_t widow2_move_attack_beam = {
    FRAME_fireb05,
    FRAME_fireb09,
    widow2_frames_attack_beam,
    NULL
};

static mframe_t widow2_frames_attack_post_beam[] = {
    {ai_charge, 4},
    {ai_charge, 4}
};
mmove_t widow2_move_attack_post_beam = {
    FRAME_fireb06,
    FRAME_fireb07,
    widow2_frames_attack_post_beam,
    widow2_run
};

void WidowDisrupt(edict_t *self)
{
    vec3_t  start;
    vec3_t  dir;
    vec3_t  forward, right;
    float   len;

    if (!self->enemy || !self->enemy->inuse)
        return;

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[MZ2_WIDOW_DISRUPTOR],
            forward, right, start);

    VectorSubtract(self->pos1, self->enemy->s.origin, dir);
    len = VectorLength(dir);

    if (len < 30) {
        /* he has barely moved since she marked him - send a homing bolt */
        VectorSubtract(self->pos1, start, dir);
        VectorNormalize(dir);

        monster_fire_tracker(self, start, dir, 20, 500, self->enemy, MZ2_WIDOW_DISRUPTOR);
    } else {
        /* he is moving - lead him and fire a dumb one */
        PredictAim(self->enemy, start, 1200, true, 0, dir, NULL);
        monster_fire_tracker(self, start, dir, 20, 1200, NULL, MZ2_WIDOW_DISRUPTOR);
    }

    widow2_step(self);
}

void Widow2SaveDisruptLoc(edict_t *self)
{
    if (self->enemy && self->enemy->inuse) {
        VectorCopy(self->enemy->s.origin, self->pos1);  /* save for aiming */
        self->pos1[2] += self->enemy->viewheight;
    } else {
        VectorClear(self->pos1);
    }
}

void widow2_disrupt_reattack(edict_t *self)
{
    float luck = random();

    if (luck < (0.25f + (skill->value * 0.15f)))
        self->monsterinfo.nextframe = FRAME_firea01;
}

static mframe_t widow2_frames_attack_disrupt[] = {
    {ai_charge, 2},
    {ai_charge, 2},
    {ai_charge, 2, Widow2SaveDisruptLoc},
    {ai_charge, -20, WidowDisrupt},
    {ai_charge, 2},
    {ai_charge, 2},
    {ai_charge, 2, widow2_disrupt_reattack}
};
mmove_t widow2_move_attack_disrupt = {
    FRAME_firea01,
    FRAME_firea07,
    widow2_frames_attack_disrupt,
    widow2_run
};

void Widow2SaveBeamTarget(edict_t *self)
{
    if (self->enemy && self->enemy->inuse) {
        VectorCopy(self->pos1, self->pos2);
        VectorCopy(self->enemy->s.origin, self->pos1);
    } else {
        VectorClear(self->pos1);
        VectorClear(self->pos2);
    }
}

static mframe_t widow2_frames_spawn[] = {
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow2_start_spawn_step},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},             /* 5 */
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, widow2_ready_spawn},     /* 10 */
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, Widow2Beam},
    {ai_charge, 0, widow2_spawn_check},
    {ai_charge},                            /* 15 */
    {ai_charge},
    {ai_charge},
    {ai_charge, 0, widow2_reattack_beam}
};
mmove_t widow2_move_spawn = {
    FRAME_spawn01,
    FRAME_spawn18,
    widow2_frames_spawn,
    NULL
};

/*
 * the tongue
 */

static bool widow2_tongue_attack_ok(const vec3_t start, const vec3_t end, float range)
{
    vec3_t dir, angles;

    /* check for max distance */
    VectorSubtract(start, end, dir);
    if (VectorLength(dir) > range)
        return false;

    /* check for min/max pitch */
    vectoangles(dir, angles);
    if (angles[0] < -180)
        angles[0] += 360;
    if (fabsf(angles[0]) > 30)
        return false;

    return true;
}

void Widow2Tongue(edict_t *self)
{
    vec3_t  f, r, u;
    vec3_t  start, end, dir;
    trace_t tr;

    if (!self->enemy || !self->enemy->inuse)
        return;

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin,
            widow2_tongue_offsets[self->s.frame - FRAME_tongs01], f, r, u, start);

    VectorCopy(self->enemy->s.origin, end);
    if (!widow2_tongue_attack_ok(start, end, WIDOW2_TONGUE_RANGE)) {
        end[2] = self->enemy->s.origin[2] + self->enemy->maxs[2] - 8;
        if (!widow2_tongue_attack_ok(start, end, WIDOW2_TONGUE_RANGE)) {
            end[2] = self->enemy->s.origin[2] + self->enemy->mins[2] + 8;
            if (!widow2_tongue_attack_ok(start, end, WIDOW2_TONGUE_RANGE))
                return;
        }
    }
    VectorCopy(self->enemy->s.origin, end);

    tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);
    if (tr.ent != self->enemy)
        return;

    gi.sound(self, CHAN_WEAPON, sound_tentacles_retract, 1, ATTN_NORM, 0);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_PARASITE_ATTACK);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(end);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    VectorSubtract(start, end, dir);
    T_Damage(self->enemy, self, self, dir, self->enemy->s.origin, vec3_origin,
             2, 0, DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN);
}

void Widow2TonguePull(edict_t *self)
{
    vec3_t vec;
    vec3_t f, r, u;
    vec3_t start, end;

    if (!self->enemy || !self->enemy->inuse) {
        self->monsterinfo.run(self);
        return;
    }

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin,
            widow2_tongue_offsets[self->s.frame - FRAME_tongs01], f, r, u, start);
    VectorCopy(self->enemy->s.origin, end);

    if (!widow2_tongue_attack_ok(start, end, WIDOW2_TONGUE_RANGE))
        return;

    if (self->enemy->groundentity) {
        self->enemy->s.origin[2] += 1;
        self->enemy->groundentity = NULL;
        /* interesting, you don't have to relink the player */
    }

    VectorSubtract(self->s.origin, self->enemy->s.origin, vec);

    if (self->enemy->client) {
        VectorNormalize(vec);
        VectorMA(self->enemy->velocity, 1000, vec, self->enemy->velocity);
    } else {
        self->enemy->ideal_yaw = vectoyaw(vec);
        M_ChangeYaw(self->enemy);
        VectorScale(f, 1000, self->enemy->velocity);
    }
}

void Widow2Crunch(edict_t *self)
{
    vec3_t aim;

    if (!self->enemy || !self->enemy->inuse) {
        self->monsterinfo.run(self);
        return;
    }

    Widow2TonguePull(self);

    /* 70 + 32 */
    VectorSet(aim, 150, 0, 4);

    if (self->s.frame != FRAME_tongs07)
        fire_hit(self, aim, 20 + (Q_rand() % 6), 0);
    else if (self->enemy->groundentity)
        fire_hit(self, aim, 20 + (Q_rand() % 6), 500);
    else
        /* less kick if he is in the air - harder to land on her head */
        fire_hit(self, aim, 20 + (Q_rand() % 6), 250);
}

void Widow2Toss(edict_t *self)
{
    self->timestamp = level.framenum + 3 * BASE_FRAMERATE;
}

static mframe_t widow2_frames_tongs[] = {
    {ai_charge, 0, Widow2Tongue},
    {ai_charge, 0, Widow2Tongue},
    {ai_charge, 0, Widow2Tongue},
    {ai_charge, 0, Widow2TonguePull},
    {ai_charge, 0, Widow2TonguePull},
    {ai_charge, 0, Widow2TonguePull},
    {ai_charge, 0, Widow2Crunch},
    {ai_charge, 0, Widow2Toss}
};
mmove_t widow2_move_tongs = {
    FRAME_tongs01,
    FRAME_tongs08,
    widow2_frames_tongs,
    widow2_run
};

static mframe_t widow2_frames_pain[] = {
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move}
};
mmove_t widow2_move_pain = {
    FRAME_pain01,
    FRAME_pain05,
    widow2_frames_pain,
    widow2_run
};

/*
 * death - 44 frames of scripted demolition
 */

static mframe_t widow2_frames_death[] = {
    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion1},          /* 3 boom */
    {ai_move},
    {ai_move},                              /* 5 */

    {ai_move, 0, WidowExplosion2},          /* 6 boom */
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},                              /* 10 */

    {ai_move},
    {ai_move},                              /* 12 */
    {ai_move},
    {ai_move},
    {ai_move},                              /* 15 */

    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion3},          /* 18 */
    {ai_move},                              /* 19 */
    {ai_move},                              /* 20 */

    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion4},          /* 25 */

    {ai_move},                              /* 26 */
    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion5},
    {ai_move, 0, WidowExplosionLeg},        /* 30 */

    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion6},
    {ai_move},                              /* 35 */

    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplosion7},
    {ai_move},
    {ai_move},                              /* 40 */

    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move, 0, WidowExplode}              /* 44 */
};
mmove_t widow2_move_death = {
    FRAME_death01,
    FRAME_death44,
    widow2_frames_death,
    NULL
};

void widow2_start_searching(edict_t *self);
void widow2_keep_searching(edict_t *self);
void widow2_finaldeath(edict_t *self);

static mframe_t widow2_frames_dead[] = {
    {ai_move, 0, widow2_start_searching},
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
    {ai_move, 0, widow2_keep_searching}
};
mmove_t widow2_move_dead = {
    FRAME_dthsrh01,
    FRAME_dthsrh15,
    widow2_frames_dead,
    NULL
};

static mframe_t widow2_frames_really_dead[] = {
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},
    {ai_move},

    {ai_move},
    {ai_move, 0, widow2_finaldeath}
};
mmove_t widow2_move_really_dead = {
    FRAME_dthsrh16,
    FRAME_dthsrh22,
    widow2_frames_really_dead,
    NULL
};

void widow2_start_searching(edict_t *self)
{
    self->count = 0;
}

void widow2_keep_searching(edict_t *self)
{
    if (self->count <= 2) {
        self->monsterinfo.currentmove = &widow2_move_dead;
        self->s.frame = FRAME_dthsrh01;
        self->count++;
        return;
    }

    self->monsterinfo.currentmove = &widow2_move_really_dead;
}

void widow2_finaldeath(edict_t *self)
{
    VectorSet(self->mins, -70, -70, 0);
    VectorSet(self->maxs, 70, 70, 80);
    self->movetype = MOVETYPE_TOSS;
    self->takedamage = DAMAGE_YES;
    self->nextthink = 0;
    gi.linkentity(self);
}

void widow2_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &widow2_move_stand;
}

void widow2_run(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &widow2_move_stand;
    else
        self->monsterinfo.currentmove = &widow2_move_run;
}

void widow2_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &widow2_move_walk;
}

void widow2_melee(edict_t *self)
{
    self->monsterinfo.currentmove = &widow2_move_tongs;
}

void widow2_attack(edict_t *self)
{
    float   r, luck;
    bool    blocked = false;

    if (self->monsterinfo.aiflags & AI_BLOCKED) {
        blocked = true;
        self->monsterinfo.aiflags &= ~AI_BLOCKED;
    }

    if (!self->enemy || !self->enemy->inuse)
        return;

    /* rogue's self->bad_area branch lived here; there is no trigger_bad_area
       in this tree, so she falls through to the ordinary choice below. */

    WidowCalcSlots(self);

    /* if she cannot see him, spawn stuff */
    if ((self->monsterinfo.attack_state == AS_BLIND) && (M_SlotsLeft(self) >= 2)) {
        self->monsterinfo.currentmove = &widow2_move_spawn;
        return;
    }

    /* a block biases her towards spawning */
    if (blocked && (M_SlotsLeft(self) >= 2)) {
        self->monsterinfo.currentmove = &widow2_move_spawn;
        return;
    }

    r = realrange(self, self->enemy);
    luck = random();

    if (r < 600) {
        if (M_SlotsLeft(self) >= 2) {
            if (luck <= 0.40f)
                self->monsterinfo.currentmove = &widow2_move_attack_pre_beam;
            else if ((luck <= 0.7f) && !(level.framenum < self->monsterinfo.attack_finished))
                self->monsterinfo.currentmove = &widow2_move_attack_disrupt;
            else
                self->monsterinfo.currentmove = &widow2_move_spawn;
        } else {
            if ((luck <= 0.50f) || (level.framenum < self->monsterinfo.attack_finished))
                self->monsterinfo.currentmove = &widow2_move_attack_pre_beam;
            else
                self->monsterinfo.currentmove = &widow2_move_attack_disrupt;
        }
    } else {
        if (M_SlotsLeft(self) >= 2) {
            if (luck < 0.3f)
                self->monsterinfo.currentmove = &widow2_move_attack_pre_beam;
            else if ((luck < 0.65f) || (level.framenum < self->monsterinfo.attack_finished))
                self->monsterinfo.currentmove = &widow2_move_spawn;
            else
                self->monsterinfo.currentmove = &widow2_move_attack_disrupt;
        } else {
            if ((luck < 0.45f) || (level.framenum < self->monsterinfo.attack_finished))
                self->monsterinfo.currentmove = &widow2_move_attack_pre_beam;
            else
                self->monsterinfo.currentmove = &widow2_move_attack_disrupt;
        }
    }
}

void widow2_attack_beam(edict_t *self)
{
    self->monsterinfo.currentmove = &widow2_move_attack_beam;
    widow2_step(self);
}

void widow2_reattack_beam(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;

    if (self->enemy && infront(self, self->enemy)) {
        if (random() <= 0.5f) {
            if ((random() < 0.7f) || (M_SlotsLeft(self) < 2))
                self->monsterinfo.currentmove = &widow2_move_attack_beam;
            else
                self->monsterinfo.currentmove = &widow2_move_spawn;
        } else {
            self->monsterinfo.currentmove = &widow2_move_attack_post_beam;
        }
    } else {
        self->monsterinfo.currentmove = &widow2_move_attack_post_beam;
    }
}

/*
 * monsterinfo.setskin does not exist in this tree; called from pain and die.
 */
static void widow2_setskin(edict_t *self)
{
    if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;
    else
        self->s.skinnum = 0;
}

void widow2_pain(edict_t *self, edict_t *other /* unused */,
        float kick /* unused */, int damage)
{
    widow2_setskin(self);

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

    if (damage >= 15) {
        if (damage < 75) {
            if (random() < (0.6f - (0.2f * skill->value))) {
                self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
                self->monsterinfo.currentmove = &widow2_move_pain;
            }
        } else {
            if (random() < (0.75f - (0.1f * skill->value))) {
                self->monsterinfo.aiflags &= ~AI_MANUAL_STEERING;
                self->monsterinfo.currentmove = &widow2_move_pain;
            }
        }
    }
}

void widow2_dead(edict_t *self)
{
}

/*
 * She takes her stalkers with her.
 */
void KillChildren(edict_t *self)
{
    edict_t *ent = NULL;

    while ((ent = G_Find(ent, FOFS(classname), "monster_stalker")) != NULL) {
        if (ent->inuse && ent->health > 0)
            T_Damage(ent, self, self, vec3_origin, ent->s.origin, vec3_origin,
                     (ent->health + 1), 0, DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN);
    }
}

void widow2_die(edict_t *self, edict_t *inflictor /* unused */,
        edict_t *attacker /* unused */, int damage,
        vec3_t point /* unused */)
{
    int n;
    int clipped;

    /* M_CheckGib() does not exist here - the classic over-kill test does the
       same job: already dead and blown well past gib_health. */
    if (self->deadflag == DEAD_DEAD && self->health <= self->gib_health) {
        clipped = min(damage, 100);

        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);

        for (n = 0; n < 2; n++)
            ThrowWidowGibLoc(self, "models/objects/gibs/bone/tris.md2", clipped, GIB_ORGANIC, NULL, false);
        for (n = 0; n < 3; n++)
            ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", clipped, GIB_ORGANIC, NULL, false);
        for (n = 0; n < 3; n++) {
            ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib1/tris.md2", clipped, GIB_METALLIC, NULL,
                               0, false);
            ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib2/tris.md2", clipped, GIB_METALLIC, NULL,
                               gi.soundindex("misc/fhit3.wav"), false);
        }
        for (n = 0; n < 2; n++) {
            ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib3/tris.md2", clipped, GIB_METALLIC, NULL,
                               0, false);
            ThrowWidowGibSized(self, "models/monsters/blackwidow/gib3/tris.md2", clipped, GIB_METALLIC, NULL,
                               0, false);
        }

        ThrowGib(self, "models/objects/gibs/chest/tris.md2", damage, GIB_ORGANIC);
        ThrowHead(self, "models/objects/gibs/head2/tris.md2", damage, GIB_ORGANIC);
        return;
    }

    if (self->deadflag == DEAD_DEAD)
        return;

    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NONE, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_NO;
    self->count = 0;
    KillChildren(self);
    self->monsterinfo.quad_framenum = 0;
    self->monsterinfo.double_framenum = 0;
    self->monsterinfo.invincible_framenum = 0;
    widow2_setskin(self);
    self->monsterinfo.currentmove = &widow2_move_death;
}

bool Widow2_CheckAttack(edict_t *self)
{
    vec3_t  spot1, spot2;
    vec3_t  temp;
    float   chance;
    trace_t tr;
    float   enemy_yaw;
    float   real_enemy_range;
    int     enemy_range;
    vec3_t  f, r, u;

    if (!self->enemy || !self->enemy->inuse)
        return false;

    WidowPowerups(self);

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

        if (tr.ent != self->enemy) {
            if (self->enemy->client && M_SlotsLeft(self) >= 2) {
                self->monsterinfo.attack_state = AS_BLIND;
                return true;
            }

            if (self->enemy->solid != SOLID_NOT || tr.fraction < 1.0f)
                return false;
        }
    }

    enemy_range = range(self, self->enemy);
    VectorSubtract(self->enemy->s.origin, self->s.origin, temp);
    enemy_yaw = vectoyaw(temp);

    self->ideal_yaw = enemy_yaw;

    /* melee attack - only once the toss cooldown has expired */
    if (self->timestamp < level.framenum) {
        real_enemy_range = realrange(self, self->enemy);
        if (real_enemy_range < 300) {
            AngleVectors(self->s.angles, f, r, u);
            G_ProjectSource2(self->s.origin, widow2_tongue_offsets[0], f, r, u, spot1);
            VectorCopy(self->enemy->s.origin, spot2);

            if (widow2_tongue_attack_ok(spot1, spot2, WIDOW2_TONGUE_RANGE)) {
                /* be nice in easy mode */
                if (skill->value == 0 && (Q_rand() % 4))
                    return false;

                if (self->monsterinfo.melee)
                    self->monsterinfo.attack_state = AS_MELEE;
                else
                    self->monsterinfo.attack_state = AS_MISSILE;
                return true;
            }
        }
    }

    if (level.framenum < self->monsterinfo.attack_finished)
        return false;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        chance = 0.4f;
    else if (enemy_range <= RANGE_NEAR)
        chance = 0.8f;
    else if (enemy_range <= RANGE_MID)
        chance = 0.8f;
    else
        chance = 0.5f;

    if ((random() < chance) || (self->enemy->solid == SOLID_NOT)) {
        self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    return false;
}

void Widow2Precache(void)
{
    gi.soundindex("parasite/parpain1.wav");
    gi.soundindex("parasite/parpain2.wav");
    gi.soundindex("parasite/pardeth1.wav");
    gi.soundindex("parasite/paratck1.wav");
    gi.soundindex("parasite/parsght1.wav");
    gi.soundindex("infantry/melee2.wav");
    gi.soundindex("misc/fhit3.wav");

    gi.soundindex("tank/tnkatck3.wav");
    gi.soundindex("weapons/disrupt.wav");
    gi.soundindex("weapons/disint2.wav");

    gi.modelindex("models/monsters/stalker/tris.md2");
    gi.modelindex("models/objects/gibs/sm_metal/tris.md2");
    gi.modelindex("models/objects/laser/tris.md2");
    gi.modelindex("models/proj/disintegrator/tris.md2");

    gi.modelindex("models/monsters/blackwidow/gib1/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib2/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib3/tris.md2");
    gi.modelindex("models/monsters/blackwidow/gib4/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib1/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib2/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib3/tris.md2");
    gi.modelindex("models/monsters/blackwidow2/gib4/tris.md2");
}

/*
 * QUAKED monster_widow2 (1 .5 0) (-70 -70 0) (70 70 144) Ambush Trigger_Spawn Sight
 */
void SP_monster_widow2(edict_t *self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via walkmonster_start below.

    sound_pain1 = gi.soundindex("widow/bw2pain1.wav");
    sound_pain2 = gi.soundindex("widow/bw2pain2.wav");
    sound_pain3 = gi.soundindex("widow/bw2pain3.wav");
    sound_death = gi.soundindex("widow/death.wav");
    sound_search1 = gi.soundindex("bosshovr/bhvunqv1.wav");
    sound_tentacles_retract = gi.soundindex("brain/brnatck3.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/blackwidow2/tris.md2");
    VectorSet(self->mins, -70, -70, 0);
    VectorSet(self->maxs, 70, 70, 144);

    // st.health_multiplier is 0 here unless the map sets it (the rerelease
    // defaults it to 1), so multiplying unguarded spawns the monster DEAD.
    // Applied here rather than in monster_start because the rerelease scales
    // only the base, before the coop bonus; zeroed so monster_start does not
    // apply it a second time.
    self->health = 2000 + 800 + 1000 * (int)skill->value;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
    st.health_multiplier = 0;
    if (coop->value)
        self->health += 500 * (int)skill->value;

    self->gib_health = -900;
    self->mass = 2500;

    if (skill->value == 3) {
        // [rerelease] st.was_key_specified: an explicit "0" key wins
        if (!(st.keys_specified & SPAWNKEY_POWER_ARMOR_TYPE))
            self->monsterinfo.power_armor_type = POWER_ARMOR_SHIELD;
        if (!(st.keys_specified & SPAWNKEY_POWER_ARMOR_POWER))
            self->monsterinfo.power_armor_power = 750;
    }

    self->yaw_speed = 30;

    self->flags |= FL_IMMUNE_LASER;
    self->monsterinfo.aiflags |= AI_IGNORE_SHOTS;

    self->pain = widow2_pain;
    self->die = widow2_die;

    self->monsterinfo.melee = widow2_melee;
    self->monsterinfo.stand = widow2_stand;
    self->monsterinfo.walk = widow2_walk;
    self->monsterinfo.run = widow2_run;
    self->monsterinfo.attack = widow2_attack;
    self->monsterinfo.search = widow2_search;
    self->monsterinfo.checkattack = Widow2_CheckAttack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &widow2_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    Widow2Precache();
    WidowCalcSlots(self);

    walkmonster_start(self);
}

/*
 * =======================================================================
 * The death-sequence gib system.
 *
 * rogue gives the widow her own gib thrower rather than reusing ThrowGib,
 * because these pieces are big, bounce, make a noise when they land, and some
 * of them are placed at a specific point on her body rather than scattered
 * from her centre.
 * =======================================================================
 */

static void WidowVelocityForDamage(int damage, vec3_t v)
{
    v[0] = damage * crandom();
    v[1] = damage * crandom();
    v[2] = damage * crandom() + 200.0f;
}

void widow_gib_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    self->solid = SOLID_NOT;
    self->touch = NULL;
    self->s.angles[PITCH] = 0;
    self->s.angles[ROLL] = 0;
    VectorClear(self->avelocity);

    if (self->style)
        gi.sound(self, CHAN_VOICE, self->style, 1, ATTN_NORM, 0);
}

void ThrowWidowGibReal(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, bool sized, int hitsound, bool fade)
{
    edict_t *gib;
    vec3_t   vd;
    vec3_t   origin;
    vec3_t   size;
    float    vscale;

    if (!gibname)
        return;

    gib = G_Spawn();

    if (startpos) {
        VectorCopy(startpos, gib->s.origin);
    } else {
        VectorScale(self->size, 0.5f, size);
        VectorAdd(self->absmin, self->absmax, origin);
        VectorScale(origin, 0.5f, origin);
        gib->s.origin[0] = origin[0] + crandom() * size[0];
        gib->s.origin[1] = origin[1] + crandom() * size[1];
        gib->s.origin[2] = origin[2] + crandom() * size[2];
    }

    gib->solid = SOLID_NOT;
    gib->s.effects |= EF_GIB;
    gib->flags |= FL_NO_KNOCKBACK;
    gib->takedamage = DAMAGE_YES;
    gib->die = gib_die;
    gib->s.renderfx |= RF_IR_VISIBLE;

    gib->think = G_FreeEdict;
    if (fade) {
        /* sized gibs last longer */
        if (sized)
            gib->nextthink = level.framenum + (int)((20 + random() * 15) * BASE_FRAMERATE);
        else
            gib->nextthink = level.framenum + (int)((5 + random() * 10) * BASE_FRAMERATE);
    } else {
        if (sized)
            gib->nextthink = level.framenum + (int)((60 + random() * 15) * BASE_FRAMERATE);
        else
            gib->nextthink = level.framenum + (int)((25 + random() * 10) * BASE_FRAMERATE);
    }

    if (!(type & GIB_METALLIC)) {
        gib->movetype = MOVETYPE_TOSS;
        vscale = 0.5f;
    } else {
        gib->movetype = MOVETYPE_BOUNCE;
        vscale = 1.0f;
    }

    WidowVelocityForDamage(damage, vd);
    VectorMA(self->velocity, vscale, vd, gib->velocity);
    ClipGibVelocity(gib);

    gi.setmodel(gib, (char *)gibname);

    if (sized) {
        gib->style = hitsound;
        gib->solid = SOLID_BBOX;
        gib->avelocity[0] = random() * 400;
        gib->avelocity[1] = random() * 400;
        gib->avelocity[2] = random() * 400;

        if (gib->velocity[2] < 0)
            gib->velocity[2] *= -1;
        gib->velocity[0] *= 2;
        gib->velocity[1] *= 2;
        ClipGibVelocity(gib);

        if (gib->velocity[2] < 350 + random() * 100)
            gib->velocity[2] = 350 + random() * 100;

        gib->gravity = 0.25f;
        gib->touch = widow_gib_touch;
        gib->owner = self;

        if (gib->s.modelindex == gi.modelindex("models/monsters/blackwidow2/gib2/tris.md2")) {
            VectorSet(gib->mins, -10, -10, 0);
            VectorSet(gib->maxs, 10, 10, 10);
        } else {
            VectorSet(gib->mins, -5, -5, 0);
            VectorSet(gib->maxs, 5, 5, 5);
        }
    } else {
        gib->velocity[0] *= 2;
        gib->velocity[1] *= 2;
        gib->avelocity[0] = random() * 600;
        gib->avelocity[1] = random() * 600;
        gib->avelocity[2] = random() * 600;
    }

    gi.linkentity(gib);
}

void ThrowWidowGib(edict_t *self, const char *gibname, int damage, int type)
{
    ThrowWidowGibReal(self, gibname, damage, type, NULL, false, 0, true);
}

void ThrowWidowGibLoc(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, bool fade)
{
    ThrowWidowGibReal(self, gibname, damage, type, startpos, false, 0, fade);
}

void ThrowWidowGibSized(edict_t *self, const char *gibname, int damage, int type,
        const vec3_t startpos, int hitsound, bool fade)
{
    ThrowWidowGibReal(self, gibname, damage, type, startpos, true, hitsound, fade);
}

void ThrowSmallStuff(edict_t *self, const vec3_t point)
{
    int n;

    for (n = 0; n < 2; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, point, false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 300, GIB_METALLIC, point, false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, point, false);
}

void ThrowMoreStuff(edict_t *self, const vec3_t point)
{
    int n;

    if (coop->value) {
        ThrowSmallStuff(self, point);
        return;
    }

    for (n = 0; n < 1; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, point, false);
    for (n = 0; n < 2; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 300, GIB_METALLIC, point, false);
    for (n = 0; n < 3; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, point, false);
}

/*
 * The final chain: thirteen explosions walked around her body, then she drops
 * into the search animation.
 */
void WidowExplode(edict_t *self)
{
    vec3_t org;
    int    n;

    self->think = WidowExplode;

    VectorCopy(self->s.origin, org);
    org[2] += 24 + (Q_rand() % 17);
    if (self->count < 8)
        org[2] += 24 + (Q_rand() % 33);

    switch (self->count) {
    case 0:
        org[0] -= 24;
        org[1] -= 24;
        break;
    case 1:
        org[0] += 24;
        org[1] += 24;
        ThrowSmallStuff(self, org);
        break;
    case 2:
        org[0] += 24;
        org[1] -= 24;
        break;
    case 3:
        org[0] -= 24;
        org[1] += 24;
        ThrowMoreStuff(self, org);
        break;
    case 4:
        org[0] -= 48;
        org[1] -= 48;
        break;
    case 5:
        org[0] += 48;
        org[1] += 48;
        ThrowArm1(self);
        break;
    case 6:
        org[0] -= 48;
        org[1] += 48;
        ThrowArm2(self);
        break;
    case 7:
        org[0] += 48;
        org[1] -= 48;
        ThrowSmallStuff(self, org);
        break;
    case 8:
        org[0] += 18;
        org[1] += 18;
        org[2] = self->s.origin[2] + 48;
        ThrowMoreStuff(self, org);
        break;
    case 9:
        org[0] -= 18;
        org[1] += 18;
        org[2] = self->s.origin[2] + 48;
        break;
    case 10:
        org[0] += 18;
        org[1] -= 18;
        org[2] = self->s.origin[2] + 48;
        break;
    case 11:
        org[0] -= 18;
        org[1] -= 18;
        org[2] = self->s.origin[2] + 48;
        break;
    case 12:
        self->s.sound = 0;
        for (n = 0; n < 1; n++)
            ThrowWidowGib(self, "models/objects/gibs/sm_meat/tris.md2", 400, GIB_ORGANIC);
        for (n = 0; n < 2; n++)
            ThrowWidowGib(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC);
        for (n = 0; n < 2; n++)
            ThrowWidowGib(self, "models/objects/gibs/sm_metal/tris.md2", 400, GIB_METALLIC);
        self->deadflag = DEAD_DEAD;
        self->think = monster_think;
        self->nextthink = level.framenum + 1;
        self->monsterinfo.currentmove = &widow2_move_dead;
        return;
    }

    self->count++;

    gi.WriteByte(svc_temp_entity);
    if (self->count >= 9 && self->count <= 12) {
        gi.WriteByte(TE_EXPLOSION1_BIG);
    } else {
        if (self->count % 2)
            gi.WriteByte(TE_EXPLOSION1);
        else
            gi.WriteByte(TE_EXPLOSION1_NP);
    }
    gi.WritePosition(org);
    gi.multicast(self->s.origin, MULTICAST_ALL);

    self->nextthink = level.framenum + 1;
}

/*
 * The seven scripted body explosions. Each is a fixed point on her model,
 * projected through her own axes so it tracks as she falls.
 */
static void WidowExplosionAt(edict_t *self, const vec3_t offset, int te)
{
    int    n;
    vec3_t f, r, u, startpoint;

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin, offset, f, r, u, startpoint);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(te);
    gi.WritePosition(startpoint);
    gi.multicast(self->s.origin, MULTICAST_ALL);

    for (n = 0; n < 1; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, startpoint, false);
    for (n = 0; n < 1; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, startpoint, false);
    for (n = 0; n < 2; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 300, GIB_METALLIC, startpoint, false);
}

void WidowExplosion1(edict_t *self)
{
    static const vec3_t offset = {23.74f, -37.67f, 76.96f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion2(edict_t *self)
{
    static const vec3_t offset = {-20.49f, 36.92f, 73.52f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion3(edict_t *self)
{
    static const vec3_t offset = {2.11f, 0.05f, 92.20f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion4(edict_t *self)
{
    static const vec3_t offset = {-28.04f, -35.57f, -77.56f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion5(edict_t *self)
{
    static const vec3_t offset = {-20.11f, -1.11f, 40.76f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion6(edict_t *self)
{
    static const vec3_t offset = {-20.11f, -1.11f, 40.76f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosion7(edict_t *self)
{
    static const vec3_t offset = {-20.11f, -1.11f, 40.76f};
    WidowExplosionAt(self, offset, TE_EXPLOSION1);
}

void WidowExplosionLeg(edict_t *self)
{
    static const vec3_t offset1 = {-31.89f, -47.86f, 67.02f};
    static const vec3_t offset2 = {-44.9f, -82.14f, 54.72f};
    vec3_t f, r, u, startpoint;

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin, offset1, f, r, u, startpoint);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1_BIG);
    gi.WritePosition(startpoint);
    gi.multicast(self->s.origin, MULTICAST_ALL);

    ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib2/tris.md2", 200, GIB_METALLIC, startpoint,
                       gi.soundindex("misc/fhit3.wav"), false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, startpoint, false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, startpoint, false);

    G_ProjectSource2(self->s.origin, offset2, f, r, u, startpoint);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1);
    gi.WritePosition(startpoint);
    gi.multicast(self->s.origin, MULTICAST_ALL);

    ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib1/tris.md2", 300, GIB_METALLIC, startpoint,
                       gi.soundindex("misc/fhit3.wav"), false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, startpoint, false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, startpoint, false);
}

void ThrowArm1(edict_t *self)
{
    static const vec3_t offset1 = {65.76f, 17.52f, 7.56f};
    int    n;
    vec3_t f, r, u, startpoint;

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin, offset1, f, r, u, startpoint);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1_BIG);
    gi.WritePosition(startpoint);
    gi.multicast(self->s.origin, MULTICAST_ALL);

    for (n = 0; n < 2; n++)
        ThrowWidowGibLoc(self, "models/objects/gibs/sm_metal/tris.md2", 100, GIB_METALLIC, startpoint, false);
}

void ThrowArm2(edict_t *self)
{
    static const vec3_t offset1 = {65.76f, 17.52f, 7.56f};
    vec3_t f, r, u, startpoint;

    AngleVectors(self->s.angles, f, r, u);
    G_ProjectSource2(self->s.origin, offset1, f, r, u, startpoint);

    ThrowWidowGibSized(self, "models/monsters/blackwidow2/gib4/tris.md2", 200, GIB_METALLIC, startpoint,
                       gi.soundindex("misc/fhit3.wav"), false);
    ThrowWidowGibLoc(self, "models/objects/gibs/sm_meat/tris.md2", 300, GIB_ORGANIC, startpoint, false);
}
