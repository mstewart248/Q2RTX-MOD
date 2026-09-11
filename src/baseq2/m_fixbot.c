/* =======================================================================
 *
 * The fixbot - xatrix's repair drone.
 *
 * Unlike every other monster here it has a JOB. Left alone it roams, looks
 * for something broken, and welds it; find a dead Strogg and it will fly over
 * and resurrect it with a repair beam, medic-style. Only if you get in its
 * way does it turn its blaster on you. Its spawnflags pick the job: FIXIT
 * roams and repairs, TAKEOFF lifts off a pad, LANDING sets down on one.
 *
 * Ported from src/rerelease/xatrix/m_xatrix_fixbot.cpp with the same
 * mechanical pass as the other monsters here. It flies on the rerelease's
 * AI_ALTERNATE_FLY velocity model, which this tree already has.
 *
 * Deliberate differences from the rerelease, none of them accidental:
 *
 *  - The rerelease keeps a PERSISTENT beam alive through a PRETHINK. This
 *    tree's monster_dabeam() is a one-frame beam, and the fixbot fires one on
 *    each of the six laserattack frames, so a fresh beam per frame is the
 *    same picture - exactly the substitution made for monster_guardian. The
 *    HEAL beam carries dmg 0; g_monster.c's dabeam_hit now skips T_Damage for
 *    a non-positive dmg, which is how rogue's damage -1 heal beam is meant to
 *    behave.
 *
 *  - `blastoff` is a 120-line copy of fire_bullet with a water trace and a
 *    custom impact effect. This tree's fire_bullet does all of that already,
 *    so the takeoff/landing thruster wash calls it directly with the same
 *    ramped spread. Cosmetic spray plus 2 points of damage, unchanged.
 *
 *  - The resurrection follows THIS TREE'S medic (m_medic.c), not id's fixbot:
 *    the rerelease version reads initial_power_armor_type, base_health and
 *    health_scaling, none of which exist here. The medic's block is the same
 *    operation and is already proven.
 *
 *  - monster_dead_think does not exist here, so the "is this a corpse" test
 *    is the classic `nextthink` one our medic uses.
 *
 *  - AI_STINKY has no equivalent; the resurrect mask is AI_SPAWNED_MASK, as
 *    in m_medic.c.
 *
 *  - HOLD_FOREVER is pause_framenum = INT_MAX here.
 *
 * object_repair is ported at the bottom: it is the thing the fixbot welds.
 * NOTE that no shipped map actually places one - xcompnd1 and xreactor have
 * fixbots (6 and 5) and no repair objects at all - so the weld path is there
 * for completeness and for anyone building a map, not for the stock game.
 *
 * =======================================================================
 */

#include "g_local.h"
#include "m_fixbot.h"
#include <limits.h>

#define SPAWNFLAG_FIXBOT_FIXIT      4
#define SPAWNFLAG_FIXBOT_TAKEOFF    8
#define SPAWNFLAG_FIXBOT_LANDING    16
#define SPAWNFLAG_FIXBOT_WORKING    32

bool infront(edict_t *self, edict_t *other);
bool FindTarget(edict_t *self);
void abortHeal(edict_t *self, bool change_frame, bool gib, bool mark);
void cleanupHealTarget(edict_t *ent);
void ED_CallSpawn(edict_t *ent);
void M_MoveToGoal(edict_t *ent, float dist);

static int sound_pain1;
static int sound_die;
static int sound_weld1;
static int sound_weld2;
static int sound_weld3;

void fixbot_run(edict_t *self);
void fixbot_attack(edict_t *self);
void fixbot_dead(edict_t *self);
void fixbot_fire_blaster(edict_t *self);
void fixbot_fire_welder(edict_t *self);
void fixbot_stand(edict_t *self);

void use_scanner(edict_t *self);
void change_to_roam(edict_t *self);
void fly_vertical(edict_t *self);
void roam_goal(edict_t *self);

extern mmove_t fixbot_move_forward;
extern mmove_t fixbot_move_stand;
extern mmove_t fixbot_move_stand2;
extern mmove_t fixbot_move_roamgoal;
extern mmove_t fixbot_move_weld_start;
extern mmove_t fixbot_move_weld;
extern mmove_t fixbot_move_weld_end;
extern mmove_t fixbot_move_takeoff;
extern mmove_t fixbot_move_landing;
extern mmove_t fixbot_move_turn;
extern mmove_t fixbot_move_run;
extern mmove_t fixbot_move_walk;
extern mmove_t fixbot_move_start_attack;
extern mmove_t fixbot_move_attack2;
extern mmove_t fixbot_move_laserattack;
extern mmove_t fixbot_move_pain3;
extern mmove_t fixbot_move_paina;
extern mmove_t fixbot_move_painb;

/*
 * [rerelease] clean up bot goals if we get interrupted
 */
void bot_goal_check(edict_t *self)
{
    if (!self->owner || !self->owner->inuse || self->owner->goalentity != self) {
        G_FreeEdict(self);
        return;
    }

    self->nextthink = level.framenum + 1;
}

static edict_t *fixbot_FindDeadMonster(edict_t *self)
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
        /* check to make sure we haven't bailed on this guy already */
        if (ent->monsterinfo.badMedic1 == self || ent->monsterinfo.badMedic2 == self)
            continue;
        if (ent->monsterinfo.healer) {
            /* ignore a stale claim from a healer that is dead, gone, or no
               longer in medic mode - rogue admits this papers over a bug */
            edict_t *h = ent->monsterinfo.healer;
            if (h->inuse && h->health > 0 && (h->svflags & SVF_MONSTER) &&
                (h->monsterinfo.aiflags & AI_MEDIC))
                continue;
        }
        if (ent->health > 0)
            continue;
        /* monster_dead_think does not exist here; this is our medic's test */
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

/*
 * How it flies depends on what it is doing: hurrying in to heal, hovering
 * close in to weld, or keeping a timid distance while fighting.
 */
static void fixbot_set_fly_parameters(edict_t *self, bool heal, bool weld)
{
    self->monsterinfo.fly_position_time = 0;
    self->monsterinfo.fly_acceleration = 5.0f;
    self->monsterinfo.fly_speed = 110.0f;
    self->monsterinfo.fly_buzzard = false;

    if (heal) {
        self->monsterinfo.fly_min_distance = 100.0f;
        self->monsterinfo.fly_max_distance = 100.0f;
        self->monsterinfo.fly_thrusters = true;
    } else if (weld) {
        self->monsterinfo.fly_min_distance = 24.0f;
        self->monsterinfo.fly_max_distance = 24.0f;
    } else {
        /* timid bot */
        self->monsterinfo.fly_min_distance = 300.0f;
        self->monsterinfo.fly_max_distance = 500.0f;
    }
}

static int fixbot_search(edict_t *self)
{
    edict_t *ent;

    if (!self->enemy) {
        ent = fixbot_FindDeadMonster(self);
        if (ent) {
            self->oldenemy = self->enemy;
            self->enemy = ent;
            self->enemy->monsterinfo.healer = self;
            self->monsterinfo.aiflags |= AI_MEDIC;
            FoundTarget(self);
            fixbot_set_fly_parameters(self, true, false);
            return 1;
        }
    }

    return 0;
}

static edict_t *fixbot_spawn_goal(edict_t *self)
{
    edict_t *ent;

    ent = G_Spawn();
    ent->classname = "bot_goal";
    ent->solid = SOLID_BBOX;
    ent->owner = self;
    ent->think = bot_goal_check;
    VectorSet(ent->mins, -32, -32, -24);
    VectorSet(ent->maxs, 32, 32, 24);
    gi.linkentity(ent);

    return ent;
}

void landing_goal(edict_t *self)
{
    trace_t  tr;
    vec3_t   forward, right, up;
    vec3_t   end;
    edict_t *ent;

    ent = fixbot_spawn_goal(self);

    AngleVectors(self->s.angles, forward, right, up);
    VectorMA(self->s.origin, -8096, up, end);

    tr = gi.trace(self->s.origin, ent->mins, ent->maxs, end, self, MASK_MONSTERSOLID);

    VectorCopy(tr.endpos, ent->s.origin);
    gi.linkentity(ent);

    self->goalentity = self->enemy = ent;
    self->monsterinfo.currentmove = &fixbot_move_landing;
}

void takeoff_goal(edict_t *self)
{
    trace_t  tr;
    vec3_t   forward, right, up;
    vec3_t   end;
    edict_t *ent;

    ent = fixbot_spawn_goal(self);

    AngleVectors(self->s.angles, forward, right, up);
    VectorMA(self->s.origin, 128, up, end);

    tr = gi.trace(self->s.origin, ent->mins, ent->maxs, end, self, MASK_MONSTERSOLID);

    VectorCopy(tr.endpos, ent->s.origin);
    gi.linkentity(ent);

    self->goalentity = self->enemy = ent;
    self->monsterinfo.currentmove = &fixbot_move_takeoff;
}

void change_to_roam(edict_t *self)
{
    if (fixbot_search(self))
        return;

    self->monsterinfo.currentmove = &fixbot_move_roamgoal;

    if (self->spawnflags & SPAWNFLAG_FIXBOT_LANDING) {
        landing_goal(self);
        self->monsterinfo.currentmove = &fixbot_move_landing;
        self->spawnflags &= ~SPAWNFLAG_FIXBOT_LANDING;
        self->spawnflags = SPAWNFLAG_FIXBOT_WORKING;
    }

    if (self->spawnflags & SPAWNFLAG_FIXBOT_TAKEOFF) {
        takeoff_goal(self);
        self->monsterinfo.currentmove = &fixbot_move_takeoff;
        self->spawnflags &= ~SPAWNFLAG_FIXBOT_TAKEOFF;
        self->spawnflags = SPAWNFLAG_FIXBOT_WORKING;
    }

    if (self->spawnflags & SPAWNFLAG_FIXBOT_FIXIT) {
        self->monsterinfo.currentmove = &fixbot_move_roamgoal;
        self->spawnflags &= ~SPAWNFLAG_FIXBOT_FIXIT;
        self->spawnflags = SPAWNFLAG_FIXBOT_WORKING;
    }

    if (!self->spawnflags)
        self->monsterinfo.currentmove = &fixbot_move_stand2;
}

/*
 * Sweep twelve directions and head for whichever has the most open space.
 */
void roam_goal(edict_t *self)
{
    trace_t  tr;
    vec3_t   forward, right, up;
    vec3_t   end;
    edict_t *ent;
    vec3_t   dang;
    float    len, oldlen;
    int      i;
    vec3_t   vec;
    vec3_t   whichvec;

    ent = fixbot_spawn_goal(self);
    ent->nextthink = level.framenum + 1;

    oldlen = 0;
    VectorCopy(self->s.origin, whichvec);

    for (i = 0; i < 12; i++) {
        VectorCopy(self->s.angles, dang);

        if (i < 6)
            dang[YAW] += 30 * i;
        else
            dang[YAW] -= 30 * (i - 6);

        AngleVectors(dang, forward, right, up);
        VectorMA(self->s.origin, 8192, forward, end);

        tr = gi.trace(self->s.origin, NULL, NULL, end, self, MASK_SHOT);

        VectorSubtract(self->s.origin, tr.endpos, vec);
        len = VectorNormalize(vec);

        if (len > oldlen) {
            oldlen = len;
            VectorCopy(tr.endpos, whichvec);
        }
    }

    VectorCopy(whichvec, ent->s.origin);
    gi.linkentity(ent);

    self->goalentity = self->enemy = ent;

    self->monsterinfo.currentmove = &fixbot_move_turn;
}

/*
 * Look for something to repair; otherwise keep heading for the roam goal.
 */
void use_scanner(edict_t *self)
{
    edict_t *ent = NULL;
    float   radius = 1024;
    vec3_t  vec;
    float   len;

    while ((ent = findradius(ent, self->s.origin, radius)) != NULL) {
        if (ent->health < 100)
            continue;
        if (strcmp(ent->classname, "object_repair") != 0)
            continue;
        if (!visible(self, ent))
            continue;

        /* remove the old goal */
        if (self->goalentity && strcmp(self->goalentity->classname, "bot_goal") == 0) {
            self->goalentity->nextthink = level.framenum + 1;
            self->goalentity->think = G_FreeEdict;
        }

        self->goalentity = self->enemy = ent;

        VectorSubtract(self->s.origin, self->goalentity->s.origin, vec);
        len = VectorLength(vec);

        fixbot_set_fly_parameters(self, false, true);

        if (len < 32)
            self->monsterinfo.currentmove = &fixbot_move_weld_start;
        return;
    }

    if (!self->goalentity) {
        self->monsterinfo.currentmove = &fixbot_move_stand;
        return;
    }

    VectorSubtract(self->s.origin, self->goalentity->s.origin, vec);
    len = VectorLength(vec);

    if (len < 32) {
        if (strcmp(self->goalentity->classname, "object_repair") == 0) {
            self->monsterinfo.currentmove = &fixbot_move_weld_start;
        } else {
            self->goalentity->nextthink = level.framenum + 1;
            self->goalentity->think = G_FreeEdict;
            self->goalentity = self->enemy = NULL;
            self->monsterinfo.currentmove = &fixbot_move_stand;
        }
        return;
    }

    VectorSubtract(self->s.origin, self->s.old_origin, vec);
    len = VectorLength(vec);

    /* bot is stuck - get a new goalentity */
    if (len == 0) {
        if (strcmp(self->goalentity->classname, "object_repair") == 0) {
            self->monsterinfo.currentmove = &fixbot_move_stand;
        } else {
            self->goalentity->nextthink = level.framenum + 1;
            self->goalentity->think = G_FreeEdict;
            self->goalentity = self->enemy = NULL;
            self->monsterinfo.currentmove = &fixbot_move_stand;
        }
    }
}

/*
 * The thruster wash under it during takeoff and landing. rogue hand-rolls a
 * copy of fire_bullet here with its own impact effect and water trace; this
 * tree's fire_bullet already does all of that.
 */
static void blastoff(edict_t *self, const vec3_t start, const vec3_t aimdir,
        int damage, int kick, int hspread, int vspread)
{
    hspread += (self->s.frame - FRAME_takeoff_01);
    vspread += (self->s.frame - FRAME_takeoff_01);

    fire_bullet(self, (float *)start, (float *)aimdir, damage, kick,
            hspread, vspread, MOD_UNKNOWN);
}

void fly_vertical(edict_t *self)
{
    int    i;
    vec3_t v;
    vec3_t forward, right, up;
    vec3_t start;
    vec3_t tempvec;

    if (!self->goalentity) {
        self->monsterinfo.currentmove = &fixbot_move_stand;
        return;
    }

    VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);

    if (self->s.frame == FRAME_landing_58 || self->s.frame == FRAME_takeoff_16) {
        self->goalentity->nextthink = level.framenum + 1;
        self->goalentity->think = G_FreeEdict;
        self->monsterinfo.currentmove = &fixbot_move_stand;
        self->goalentity = self->enemy = NULL;
        return;
    }

    /* kick up some particles */
    VectorCopy(self->s.angles, tempvec);
    tempvec[PITCH] += 90;

    AngleVectors(tempvec, forward, right, up);
    VectorCopy(self->s.origin, start);

    for (i = 0; i < 10; i++)
        blastoff(self, start, forward, 2, 1, DEFAULT_SHOTGUN_HSPREAD, DEFAULT_SHOTGUN_VSPREAD);
}

void fly_vertical2(edict_t *self)
{
    vec3_t v;
    float  len;

    if (!self->goalentity) {
        self->monsterinfo.currentmove = &fixbot_move_stand;
        return;
    }

    VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
    len = VectorLength(v);
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);

    if (len < 32) {
        self->goalentity->nextthink = level.framenum + 1;
        self->goalentity->think = G_FreeEdict;
        self->monsterinfo.currentmove = &fixbot_move_stand;
        self->goalentity = self->enemy = NULL;
    }
}

static mframe_t fixbot_frames_landing[] = {
    {ai_move},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},

    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2},

    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2},

    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2},

    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2},

    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2},
    {ai_move, 0, fly_vertical2}, {ai_move, 0, fly_vertical2}
};
mmove_t fixbot_move_landing = {
    FRAME_landing_01,
    FRAME_landing_58,
    fixbot_frames_landing,
    NULL
};

/*
 * generic ambient stand
 */
static mframe_t fixbot_frames_stand[] = {
    {ai_move}, {ai_move}, {ai_move}, {ai_move}, {ai_move},
    {ai_move}, {ai_move}, {ai_move}, {ai_move}, {ai_move},
    {ai_move}, {ai_move}, {ai_move}, {ai_move}, {ai_move},
    {ai_move}, {ai_move}, {ai_move},
    {ai_move, 0, change_to_roam}
};
mmove_t fixbot_move_stand = {
    FRAME_ambient_01,
    FRAME_ambient_19,
    fixbot_frames_stand,
    NULL
};

static mframe_t fixbot_frames_stand2[] = {
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand}, {ai_stand}, {ai_stand},
    {ai_stand, 0, change_to_roam}
};
mmove_t fixbot_move_stand2 = {
    FRAME_ambient_01,
    FRAME_ambient_19,
    fixbot_frames_stand2,
    NULL
};

/*
 * generic frame to move bot
 */
static mframe_t fixbot_frames_roamgoal[] = {
    {ai_move, 0, roam_goal}
};
mmove_t fixbot_move_roamgoal = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_roamgoal,
    NULL
};

void ai_facing(edict_t *self, float dist)
{
    vec3_t v;

    if (!self->goalentity) {
        fixbot_stand(self);
        return;
    }

    if (infront(self, self->goalentity)) {
        self->monsterinfo.currentmove = &fixbot_move_forward;
    } else {
        VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
        self->ideal_yaw = vectoyaw(v);
        M_ChangeYaw(self);
    }
}

static mframe_t fixbot_frames_turn[] = {
    {ai_facing}
};
mmove_t fixbot_move_turn = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_turn,
    NULL
};

/*
 * takeoff
 */
static mframe_t fixbot_frames_takeoff[] = {
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical},
    {ai_move, 0.01f, fly_vertical}, {ai_move, 0.01f, fly_vertical}
};
mmove_t fixbot_move_takeoff = {
    FRAME_takeoff_01,
    FRAME_takeoff_16,
    fixbot_frames_takeoff,
    NULL
};

static mframe_t fixbot_frames_paina[] = {
    {ai_move}, {ai_move}, {ai_move}, {ai_move}, {ai_move}, {ai_move}
};
mmove_t fixbot_move_paina = {
    FRAME_paina_01,
    FRAME_paina_06,
    fixbot_frames_paina,
    fixbot_run
};

static mframe_t fixbot_frames_painb[] = {
    {ai_move}, {ai_move}, {ai_move}, {ai_move},
    {ai_move}, {ai_move}, {ai_move}, {ai_move}
};
mmove_t fixbot_move_painb = {
    FRAME_painb_01,
    FRAME_painb_08,
    fixbot_frames_painb,
    fixbot_run
};

/*
 * back up from pain
 */
static mframe_t fixbot_frames_pain3[] = {
    {ai_move, -1}
};
mmove_t fixbot_move_pain3 = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_pain3,
    fixbot_run
};

void ai_movetogoal(edict_t *self, float dist)
{
    M_MoveToGoal(self, dist);
}

static mframe_t fixbot_frames_forward[] = {
    {ai_movetogoal, 5, use_scanner}
};
mmove_t fixbot_move_forward = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_forward,
    NULL
};

static mframe_t fixbot_frames_walk[] = {
    {ai_walk, 5}
};
mmove_t fixbot_move_walk = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_walk,
    NULL
};

static mframe_t fixbot_frames_run[] = {
    {ai_run, 10}
};
mmove_t fixbot_move_run = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_run,
    NULL
};

static mframe_t fixbot_frames_start_attack[] = {
    {ai_charge}
};
mmove_t fixbot_move_start_attack = {
    FRAME_freeze_01,
    FRAME_freeze_01,
    fixbot_frames_start_attack,
    fixbot_attack
};

/*
 * The repair beam, and the resurrection at the end of it.
 */
void fixbot_fire_laser(edict_t *self)
{
    edict_t *beam;
    vec3_t   dir, start;

    /* critter dun got blown up while bein' fixed */
    if (!self->enemy || !self->enemy->inuse ||
        self->enemy->health <= self->enemy->gib_health) {
        self->monsterinfo.currentmove = &fixbot_move_stand;
        self->monsterinfo.aiflags &= ~AI_MEDIC;
        return;
    }

    /* A one-frame beam, as for monster_guardian. dmg 0 marks it a HEALING
       beam - dabeam_hit skips T_Damage for a non-positive dmg. */
    AngleVectors(self->s.angles, dir, NULL, NULL);
    VectorMA(self->s.origin, 16, dir, start);

    beam = G_Spawn();
    VectorCopy(start, beam->s.origin);
    VectorCopy(self->s.angles, beam->s.angles);
    beam->enemy = self->enemy;
    beam->owner = self;
    beam->dmg = 0;
    beam->classname = "fixbot_healbeam";
    monster_dabeam(beam);

    if (self->enemy->health > (self->enemy->mass / 10)) {
        vec3_t  maxs;
        trace_t tr;
        reinforcement_t saved_reinf[MAX_REINFORCEMENT_TYPES];
        int     saved_num_reinf, saved_slots, saved_used, saved_gib_health;

        self->enemy->spawnflags = 0;
        self->enemy->monsterinfo.aiflags &= AI_SPAWNED_MASK;
        self->enemy->target = NULL;
        self->enemy->targetname = NULL;
        self->enemy->combattarget = NULL;
        self->enemy->deathtarget = NULL;
        self->enemy->monsterinfo.healer = self;

        VectorCopy(self->enemy->maxs, maxs);
        maxs[2] += 48;  /* compensate for the change when they died */

        tr = gi.trace(self->enemy->s.origin, self->enemy->mins, maxs,
                      self->enemy->s.origin, self->enemy, MASK_MONSTERSOLID);
        if (tr.startsolid || tr.allsolid || tr.ent != world) {
            abortHeal(self, false, true, false);
            return;
        }

        self->enemy->monsterinfo.aiflags |= AI_IGNORE_SHOTS | AI_DO_NOT_COUNT;

        /* Same save/restore our medic does: ED_CallSpawn re-reads the spawn
           temp, which mid-game still holds whatever the last parsed entity
           left behind. */
        memcpy(saved_reinf, self->enemy->monsterinfo.reinforcements, sizeof(saved_reinf));
        saved_num_reinf  = self->enemy->monsterinfo.num_reinforcements;
        saved_slots      = self->enemy->monsterinfo.monster_slots;
        saved_used       = self->enemy->monsterinfo.monster_used;
        saved_gib_health = self->enemy->gib_health;

        memset(&st, 0, sizeof(st));

        self->enemy->owner = self;
        ED_CallSpawn(self->enemy);
        self->enemy->owner = NULL;

        memcpy(self->enemy->monsterinfo.reinforcements, saved_reinf, sizeof(saved_reinf));
        self->enemy->monsterinfo.num_reinforcements = saved_num_reinf;
        self->enemy->monsterinfo.monster_slots      = saved_slots;
        self->enemy->monsterinfo.monster_used       = saved_used;

        /* a body killed once gibs twice as easily */
        self->enemy->gib_health = saved_gib_health / 2;

        if (self->enemy->think) {
            self->enemy->nextthink = level.framenum;
            self->enemy->think(self->enemy);
        }

        self->enemy->monsterinfo.aiflags &= ~AI_RESURRECTING;
        self->enemy->monsterinfo.aiflags |= AI_IGNORE_SHOTS | AI_DO_NOT_COUNT;
        self->enemy->s.effects &= ~EF_FLIES;
        self->enemy->monsterinfo.healer = NULL;
        self->enemy->monsterinfo.badMedic1 = NULL;
        self->enemy->monsterinfo.badMedic2 = NULL;

        if (self->enemy->inuse) {
            cleanupHealTarget(self->enemy);

            if (self->oldenemy && self->oldenemy->inuse && self->oldenemy->health > 0) {
                self->enemy->enemy = self->oldenemy;
                FoundTarget(self->enemy);
            } else {
                self->enemy->enemy = NULL;
                if (!FindTarget(self->enemy)) {
                    /* no valid enemy, so stop acting */
                    self->enemy->monsterinfo.pause_framenum = INT_MAX;
                    self->enemy->monsterinfo.stand(self->enemy);
                }
                self->enemy = NULL;
                self->oldenemy = NULL;
                self->monsterinfo.aiflags &= ~AI_MEDIC;
                if (!FindTarget(self)) {
                    self->monsterinfo.pause_framenum = INT_MAX;
                    self->monsterinfo.stand(self);
                    return;
                }
            }
        }

        self->monsterinfo.aiflags &= ~AI_MEDIC;
        self->monsterinfo.currentmove = &fixbot_move_stand;
    } else {
        self->enemy->monsterinfo.aiflags |= AI_RESURRECTING;
    }
}

static mframe_t fixbot_frames_laserattack[] = {
    {ai_charge, 0, fixbot_fire_laser},
    {ai_charge, 0, fixbot_fire_laser},
    {ai_charge, 0, fixbot_fire_laser},
    {ai_charge, 0, fixbot_fire_laser},
    {ai_charge, 0, fixbot_fire_laser},
    {ai_charge, 0, fixbot_fire_laser}
};
mmove_t fixbot_move_laserattack = {
    FRAME_shoot_01,
    FRAME_shoot_06,
    fixbot_frames_laserattack,
    NULL
};

/*
 * the charge-and-shoot attack
 */
static mframe_t fixbot_frames_attack2[] = {
    {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge},
    {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge},

    {ai_charge, -10}, {ai_charge, -10}, {ai_charge, -10}, {ai_charge, -10},
    {ai_charge, -10}, {ai_charge, -10}, {ai_charge, -10}, {ai_charge, -10},
    {ai_charge, -10}, {ai_charge, -10},

    {ai_charge, 0, fixbot_fire_blaster},
    {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge},
    {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge}, {ai_charge},

    {ai_charge}
};
mmove_t fixbot_move_attack2 = {
    FRAME_charging_01,
    FRAME_charging_31,
    fixbot_frames_attack2,
    fixbot_run
};

void weldstate(edict_t *self)
{
    if (self->s.frame == FRAME_weldstart_10) {
        self->monsterinfo.currentmove = &fixbot_move_weld;
    } else if (self->goalentity && self->s.frame == FRAME_weldmiddle_07) {
        if (self->goalentity->health <= 0) {
            if (self->enemy)
                self->enemy->owner = NULL;
            self->monsterinfo.currentmove = &fixbot_move_weld_end;
        } else {
            self->goalentity->health -= 10;
        }
    } else {
        self->goalentity = self->enemy = NULL;
        self->monsterinfo.currentmove = &fixbot_move_stand;
    }
}

void ai_move2(edict_t *self, float dist)
{
    vec3_t v;

    if (!self->goalentity) {
        fixbot_stand(self);
        return;
    }

    M_walkmove(self, self->s.angles[YAW], dist);

    VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
    self->ideal_yaw = vectoyaw(v);
    M_ChangeYaw(self);
}

static mframe_t fixbot_frames_weld_start[] = {
    {ai_move2, 0}, {ai_move2, 0}, {ai_move2, 0}, {ai_move2, 0}, {ai_move2, 0},
    {ai_move2, 0}, {ai_move2, 0}, {ai_move2, 0}, {ai_move2, 0},
    {ai_move2, 0, weldstate}
};
mmove_t fixbot_move_weld_start = {
    FRAME_weldstart_01,
    FRAME_weldstart_10,
    fixbot_frames_weld_start,
    NULL
};

static mframe_t fixbot_frames_weld[] = {
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, fixbot_fire_welder},
    {ai_move2, 0, weldstate}
};
mmove_t fixbot_move_weld = {
    FRAME_weldmiddle_01,
    FRAME_weldmiddle_07,
    fixbot_frames_weld,
    NULL
};

static mframe_t fixbot_frames_weld_end[] = {
    {ai_move2, -2}, {ai_move2, -2}, {ai_move2, -2},
    {ai_move2, -2}, {ai_move2, -2}, {ai_move2, -2},
    {ai_move2, -2, weldstate}
};
mmove_t fixbot_move_weld_end = {
    FRAME_weldend_01,
    FRAME_weldend_07,
    fixbot_frames_weld_end,
    NULL
};

void fixbot_fire_welder(edict_t *self)
{
    vec3_t start;
    vec3_t forward, right, up;
    vec3_t vec;
    float  r;

    if (!self->enemy)
        return;

    VectorSet(vec, 24.0f, -0.8f, -10.0f);

    AngleVectors(self->s.angles, forward, right, up);
    M_ProjectFlashSource(self, vec, forward, right, start);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WELDING_SPARKS);
    gi.WriteByte(10);
    gi.WritePosition(start);
    gi.WriteDir(vec3_origin);
    gi.WriteByte(0xe0 + (Q_rand() % 9));
    gi.multicast(self->s.origin, MULTICAST_PVS);

    if (random() > 0.8f) {
        r = random();

        if (r < 0.33f)
            gi.sound(self, CHAN_VOICE, sound_weld1, 1, ATTN_IDLE, 0);
        else if (r < 0.66f)
            gi.sound(self, CHAN_VOICE, sound_weld2, 1, ATTN_IDLE, 0);
        else
            gi.sound(self, CHAN_VOICE, sound_weld3, 1, ATTN_IDLE, 0);
    }
}

void fixbot_fire_blaster(edict_t *self)
{
    vec3_t start;
    vec3_t forward, right, up;
    vec3_t end;
    vec3_t dir;

    if (!self->enemy || !self->enemy->inuse)
        return;

    if (!visible(self, self->enemy)) {
        self->monsterinfo.currentmove = &fixbot_move_run;
        return;
    }

    AngleVectors(self->s.angles, forward, right, up);
    M_ProjectFlashSource(self, monster_flash_offset[MZ2_HOVER_BLASTER_1], forward, right, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);
    VectorNormalize(dir);

    monster_fire_blaster(self, start, dir, 15, 1000, MZ2_HOVER_BLASTER_1, EF_BLASTER);
}

void fixbot_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &fixbot_move_stand;
}

void fixbot_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &fixbot_move_stand;
    else
        self->monsterinfo.currentmove = &fixbot_move_run;
}

void fixbot_walk(edict_t *self)
{
    vec3_t vec;
    float  len;

    if (self->goalentity && strcmp(self->goalentity->classname, "object_repair") == 0) {
        VectorSubtract(self->s.origin, self->goalentity->s.origin, vec);
        len = VectorLength(vec);
        if (len < 32) {
            self->monsterinfo.currentmove = &fixbot_move_weld_start;
            return;
        }
    }

    self->monsterinfo.currentmove = &fixbot_move_walk;
}

void fixbot_start_attack(edict_t *self)
{
    self->monsterinfo.currentmove = &fixbot_move_start_attack;
}

void fixbot_attack(edict_t *self)
{
    vec3_t vec;
    float  len;

    if (!self->enemy || !self->enemy->inuse)
        return;

    if (self->monsterinfo.aiflags & AI_MEDIC) {
        if (!visible(self, self->enemy))
            return;

        VectorSubtract(self->s.origin, self->enemy->s.origin, vec);
        len = VectorLength(vec);
        if (len > 128)
            return;

        self->monsterinfo.currentmove = &fixbot_move_laserattack;
    } else {
        fixbot_set_fly_parameters(self, false, false);
        self->monsterinfo.currentmove = &fixbot_move_attack2;
    }
}

void fixbot_pain(edict_t *self, edict_t *other /* unused */,
        float kick /* unused */, int damage)
{
    if (level.framenum < self->pain_debounce_framenum)
        return;

    fixbot_set_fly_parameters(self, false, false);
    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
    gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);

    if (damage <= 10)
        self->monsterinfo.currentmove = &fixbot_move_pain3;
    else if (damage <= 25)
        self->monsterinfo.currentmove = &fixbot_move_painb;
    else
        self->monsterinfo.currentmove = &fixbot_move_paina;
}

void fixbot_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

void fixbot_die(edict_t *self, edict_t *inflictor /* unused */,
        edict_t *attacker /* unused */, int damage /* unused */,
        vec3_t point /* unused */)
{
    gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);
    BecomeExplosion1(self);
}

/*
 * QUAKED monster_fixbot (1 .5 0) (-32 -32 -24) (32 32 24) Ambush Trigger_Spawn Fixit Takeoff Landing
 */
void SP_monster_fixbot(edict_t *self)
{
    // M_AllowSpawn is the rerelease's deathmatch/coop gate; monster_start()
    // does that job here, via flymonster_start below.

    sound_pain1 = gi.soundindex("flyer/flypain1.wav");
    sound_die = gi.soundindex("flyer/flydeth1.wav");

    sound_weld1 = gi.soundindex("misc/welder1.wav");
    sound_weld2 = gi.soundindex("misc/welder2.wav");
    sound_weld3 = gi.soundindex("misc/welder3.wav");

    self->s.modelindex = gi.modelindex("models/monsters/fixbot/tris.md2");

    VectorSet(self->mins, -32, -32, -24);
    VectorSet(self->maxs, 32, 32, 24);

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    // st.health_multiplier is 0 here unless the map sets it (the rerelease
    // defaults it to 1), so multiplying unguarded spawns the monster DEAD.
    self->health = 150;
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);

    self->mass = 150;

    self->pain = fixbot_pain;
    self->die = fixbot_die;

    self->monsterinfo.stand = fixbot_stand;
    self->monsterinfo.walk = fixbot_walk;
    self->monsterinfo.run = fixbot_run;
    self->monsterinfo.attack = fixbot_attack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &fixbot_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;
    self->monsterinfo.aiflags |= AI_ALTERNATE_FLY;
    fixbot_set_fly_parameters(self, false, false);

    flymonster_start(self);
}

/*
 * =======================================================================
 * object_repair - the thing a fixbot welds.
 *
 * From src/rerelease/xatrix/g_xatrix_func.cpp. It sits there throwing sparks
 * while broken; a fixbot drives its health back up to 100, at which point it
 * fires its targets and settles down. NOTE: no shipped map places one, so
 * this exists for completeness and for map authors.
 * =======================================================================
 */

void object_repair_fx(edict_t *ent);
void object_repair_dead(edict_t *ent);

static void object_repair_sparks_at(edict_t *ent)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WELDING_SPARKS);
    gi.WriteByte(10);
    gi.WritePosition(ent->s.origin);
    gi.WriteDir(vec3_origin);
    gi.WriteByte(0xe0 + (Q_rand() % 9));
    gi.multicast(ent->s.origin, MULTICAST_PVS);
}

void object_repair_fx(edict_t *ent)
{
    ent->nextthink = level.framenum + (int)(ent->delay * BASE_FRAMERATE);

    if (ent->health <= 100)
        ent->health++;
    else
        object_repair_sparks_at(ent);
}

void object_repair_dead(edict_t *ent)
{
    G_UseTargets(ent, ent);
    ent->nextthink = level.framenum + 1;
    ent->think = object_repair_fx;
}

void object_repair_sparks(edict_t *ent)
{
    if (ent->health <= 0) {
        ent->nextthink = level.framenum + 1;
        ent->think = object_repair_dead;
        return;
    }

    ent->nextthink = level.framenum + (int)(ent->delay * BASE_FRAMERATE);
    object_repair_sparks_at(ent);
}

void SP_object_repair(edict_t *ent)
{
    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_BBOX;
    ent->classname = "object_repair";
    VectorSet(ent->mins, -8, -8, 8);
    VectorSet(ent->maxs, 8, 8, 8);
    ent->think = object_repair_sparks;
    ent->nextthink = level.framenum + 1 * BASE_FRAMERATE;
    ent->health = 100;
    if (!ent->delay)
        ent->delay = 1.0f;
}
