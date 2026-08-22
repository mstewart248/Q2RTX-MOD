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

gekk

The Xatrix water lizard, placed by the rerelease in badlands, w_treat,
mgu3m1 and mgu4m2. Ported from src/xatrix/monster/gekk, with behaviour and
balance taken from src/rerelease/xatrix/m_xatrix_gekk.cpp wherever the two
disagree - the rerelease maps are tuned against the rerelease gekk.

The deliberate differences from the Xatrix original are:
  - an 18-unit wide bounding box, not 24. The rerelease narrowed it and its
    maps have doorways to match.
  - the NOJUMPING and NOSWIM spawnflags, which mgu4m2 uses and Xatrix
    never had.
  - gekk_attack() replaces Xatrix's gekk_jump() as monsterinfo.attack. It is
    the only dispatcher that honours NOJUMPING.
  - gekk_stand() will not break an in-progress chant.
  - the loogie carries EF_BLASTER; Xatrix assigned a renderfx constant to
    s.effects, which lit nothing.

==============================================================================
*/

#include "g_local.h"
#include "m_gekk.h"

// Xatrix's Chant, plus the two the rerelease added.
#define SPAWNFLAG_GEKK_CHANT        8
#define SPAWNFLAG_GEKK_NOJUMPING    16
#define SPAWNFLAG_GEKK_NOSWIM       32

static int  sound_swing;
static int  sound_hit;
static int  sound_hit2;
static int  sound_speet;
static int  loogie_hit;
static int  sound_death;
static int  sound_pain1;
static int  sound_sight;
static int  sound_search;
static int  sound_step1;
static int  sound_step2;
static int  sound_step3;
static int  sound_thud;
static int  sound_chantlow;
static int  sound_chantmid;
static int  sound_chanthigh;

extern mmove_t gekk_move_stand;
extern mmove_t gekk_move_standunderwater;
extern mmove_t gekk_move_idle;
extern mmove_t gekk_move_idle2;
extern mmove_t gekk_move_chant;
extern mmove_t gekk_move_walk;
extern mmove_t gekk_move_run;
extern mmove_t gekk_move_run_start;
extern mmove_t gekk_move_swim_start;
extern mmove_t gekk_move_swim_loop;
extern mmove_t gekk_move_spit;
extern mmove_t gekk_move_attack;
extern mmove_t gekk_move_attack1;
extern mmove_t gekk_move_attack2;
extern mmove_t gekk_move_leapatk;
extern mmove_t gekk_move_leapatk2;
extern mmove_t gekk_move_lduck;
extern mmove_t gekk_move_rduck;

void gekk_swim(edict_t *self);
void gekk_jump_takeoff(edict_t *self);
void gekk_jump_takeoff2(edict_t *self);
void gekk_check_landing(edict_t *self);
void gekk_stop_skid(edict_t *self);
void gekk_check_underwater(edict_t *self);
void gekk_bite(edict_t *self);
void gekk_hit_left(edict_t *self);
void gekk_hit_right(edict_t *self);
void gekk_run_start(edict_t *self);
void gekk_run(edict_t *self);
void gekk_walk(edict_t *self);
void gekk_stand(edict_t *self);
void gekk_chant(edict_t *self);
void water_to_land(edict_t *self);
void land_to_water(edict_t *self);

// NOSWIM keeps a gekk out of swim mode entirely. Once it is already
// swimming, waterlevel alone still picks the underwater pain/death
// animations, exactly as the rerelease does.
static bool gekk_swims(edict_t *self)
{
    return !(self->spawnflags & SPAWNFLAG_GEKK_NOSWIM) && self->waterlevel >= 2;
}

static bool gekk_check_melee(edict_t *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return false;

    if (range(self, self->enemy) == RANGE_MELEE)
        return true;

    return false;
}

static bool gekk_check_jump(edict_t *self)
{
    vec3_t  v;
    float   distance;

    if (self->absmin[2] > (self->enemy->absmin[2] + 0.75f * self->enemy->size[2]))
        return false;

    if (self->absmax[2] < (self->enemy->absmin[2] + 0.25f * self->enemy->size[2]))
        return false;

    v[0] = self->s.origin[0] - self->enemy->s.origin[0];
    v[1] = self->s.origin[1] - self->enemy->s.origin[1];
    v[2] = 0;
    distance = VectorLength(v);

    if (distance < 100)
        return false;

    if (distance > 100) {
        // much more willing to lunge while swimming
        if (random() < (self->waterlevel >= 2 ? 0.2f : 0.9f))
            return false;
    }

    return true;
}

static bool gekk_check_jump_close(edict_t *self)
{
    vec3_t  v;
    float   distance;

    v[0] = self->s.origin[0] - self->enemy->s.origin[0];
    v[1] = self->s.origin[1] - self->enemy->s.origin[1];
    v[2] = 0;
    distance = VectorLength(v);

    if (distance < 100) {
        if (self->s.origin[2] < self->enemy->s.origin[2])
            return true;
        return false;
    }

    return true;
}

bool gekk_checkattack(edict_t *self)
{
    if (!self->enemy || self->enemy->health <= 0)
        return false;

    if (gekk_check_melee(self)) {
        self->monsterinfo.attack_state = AS_MELEE;
        return true;
    }

    if (gekk_check_jump(self)) {
        self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    if (gekk_check_jump_close(self) && !self->waterlevel) {
        self->monsterinfo.attack_state = AS_MISSILE;
        return true;
    }

    return false;
}

void gekk_step(edict_t *self)
{
    int n;

    n = (rand() + 1) % 3;

    if (n == 0)
        gi.sound(self, CHAN_VOICE, sound_step1, 1, ATTN_NORM, 0);
    else if (n == 1)
        gi.sound(self, CHAN_VOICE, sound_step2, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_step3, 1, ATTN_NORM, 0);
}

void gekk_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

// Sets the damage skin. Health regenerates in gekk_search, so this runs
// in both directions, unlike most monsters' one-way pain skins.
static void gekk_setskin(edict_t *self)
{
    if (self->health < (self->max_health / 4))
        self->s.skinnum = 2;
    else if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;
    else
        self->s.skinnum = 0;
}

void gekk_search(edict_t *self)
{
    float   r;

    if (self->spawnflags & SPAWNFLAG_GEKK_CHANT) {
        r = random();

        if (r < 0.33f)
            gi.sound(self, CHAN_VOICE, sound_chantlow, 1, ATTN_NORM, 0);
        else if (r < 0.66f)
            gi.sound(self, CHAN_VOICE, sound_chantmid, 1, ATTN_NORM, 0);
        else
            gi.sound(self, CHAN_VOICE, sound_chanthigh, 1, ATTN_NORM, 0);
    } else {
        gi.sound(self, CHAN_VOICE, sound_search, 1, ATTN_NORM, 0);
    }

    // chanting gekks heal each other
    self->health += 10 + (10 * random());
    if (self->health > self->max_health)
        self->health = self->max_health;

    gekk_setskin(self);
}

void gekk_swing(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_swing, 1, ATTN_NORM, 0);
}

void gekk_face(edict_t *self)
{
    self->monsterinfo.currentmove = &gekk_move_run;
}

// A chanting gekk animates in place rather than standing idle, so it needs
// ai_move plus ai_stand's idle scheduling. Everything else uses ai_stand.
void ai_stand2(edict_t *self, float dist)
{
    if (self->spawnflags & SPAWNFLAG_GEKK_CHANT) {
        ai_move(self, dist);

        if (!(self->spawnflags & 1) && self->monsterinfo.idle
            && level.framenum > self->monsterinfo.idle_framenum) {
            if (self->monsterinfo.idle_framenum) {
                self->monsterinfo.idle(self);
                self->monsterinfo.idle_framenum = level.framenum + (15 + random() * 15) * BASE_FRAMERATE;
            } else {
                self->monsterinfo.idle_framenum = level.framenum + (random() * 15) * BASE_FRAMERATE;
            }
        }
    } else {
        ai_stand(self, dist);
    }
}

mframe_t gekk_frames_stand [] = {
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},       // 10

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},       // 20

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},       // 30

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},

    {ai_stand2, 0, gekk_check_underwater}
};
mmove_t gekk_move_stand = {FRAME_stand_01, FRAME_stand_39, gekk_frames_stand, NULL};

mframe_t gekk_frames_standunderwater [] = {
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},

    {ai_stand2, 0, gekk_check_underwater}
};
mmove_t gekk_move_standunderwater = {FRAME_amb_01, FRAME_amb_04, gekk_frames_standunderwater, NULL};

void gekk_swim_loop(edict_t *self)
{
    self->flags |= FL_SWIM;
    self->monsterinfo.currentmove = &gekk_move_swim_loop;
}

mframe_t gekk_frames_swim [] = {
    {ai_run, 16, NULL},
    {ai_run, 16, NULL},
    {ai_run, 16, NULL},

    {ai_run, 16, gekk_swim}
};
mmove_t gekk_move_swim_loop = {FRAME_amb_01, FRAME_amb_04, gekk_frames_swim, gekk_swim_loop};

mframe_t gekk_frames_swim_start [] = {
    {ai_run, 14, NULL},
    {ai_run, 14, NULL},
    {ai_run, 14, NULL},
    {ai_run, 14, NULL},
    {ai_run, 16, NULL},
    {ai_run, 16, NULL},
    {ai_run, 16, NULL},
    {ai_run, 18, NULL},
    {ai_run, 18, gekk_hit_left},
    {ai_run, 18, NULL},

    {ai_run, 20, NULL},
    {ai_run, 20, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 24, gekk_hit_right},
    {ai_run, 24, NULL},
    {ai_run, 26, NULL},
    {ai_run, 26, NULL},
    {ai_run, 24, NULL},
    {ai_run, 24, NULL},

    {ai_run, 22, gekk_bite},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 22, NULL},
    {ai_run, 18, NULL},
    {ai_run, 18, NULL},

    {ai_run, 18, NULL},
    {ai_run, 18, NULL}
};
mmove_t gekk_move_swim_start = {FRAME_swim_01, FRAME_swim_32, gekk_frames_swim_start, gekk_swim_loop};

void gekk_swim(edict_t *self)
{
    if (!self->enemy)
        return;

    if (gekk_checkattack(self)) {
        // chase the player out of the water
        if (self->enemy->waterlevel < 2 && random() > 0.7f)
            water_to_land(self);
        else
            self->monsterinfo.currentmove = &gekk_move_swim_start;
    } else {
        self->monsterinfo.currentmove = &gekk_move_swim_start;
    }
}

void gekk_stand(edict_t *self)
{
    if (self->waterlevel >= 2) {
        self->flags |= FL_SWIM;
        self->monsterinfo.currentmove = &gekk_move_standunderwater;
    } else if (self->monsterinfo.currentmove != &gekk_move_chant) {
        // don't break the chant loop that SP_monster_gekk started
        self->monsterinfo.currentmove = &gekk_move_stand;
    }
}

void gekk_chant(edict_t *self)
{
    self->monsterinfo.currentmove = &gekk_move_chant;
}

void gekk_idle_loop(edict_t *self)
{
    if (random() > 0.75f && self->health < self->max_health)
        self->monsterinfo.nextframe = FRAME_idle_01;
}

mframe_t gekk_frames_idle [] = {
    {ai_stand2, 0, gekk_search},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},
    {ai_stand2, 0, NULL},

    {ai_stand2, 0, NULL},
    {ai_stand2, 0, gekk_idle_loop}
};
mmove_t gekk_move_idle = {FRAME_idle_01, FRAME_idle_32, gekk_frames_idle, gekk_stand};
mmove_t gekk_move_idle2 = {FRAME_idle_01, FRAME_idle_32, gekk_frames_idle, gekk_face};

mframe_t gekk_frames_idle2 [] = {
    {ai_move, 0, gekk_search},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},

    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},

    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},

    {ai_move, 0, NULL},
    {ai_move, 0, gekk_idle_loop}
};
mmove_t gekk_move_chant = {FRAME_idle_01, FRAME_idle_32, gekk_frames_idle2, gekk_chant};

void gekk_idle(edict_t *self)
{
    if (!gekk_swims(self))
        self->monsterinfo.currentmove = &gekk_move_idle;
    else
        self->monsterinfo.currentmove = &gekk_move_swim_start;
}

mframe_t gekk_frames_walk [] = {
    {ai_walk, 3.849f, gekk_check_underwater},
    {ai_walk, 19.606f, NULL},
    {ai_walk, 25.583f, NULL},
    {ai_walk, 34.625f, gekk_step},
    {ai_walk, 27.365f, NULL},
    {ai_walk, 28.480f, NULL}
};
mmove_t gekk_move_walk = {FRAME_run_01, FRAME_run_06, gekk_frames_walk, NULL};

void gekk_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &gekk_move_walk;
}

void gekk_run_start(edict_t *self)
{
    if (gekk_swims(self))
        self->monsterinfo.currentmove = &gekk_move_swim_start;
    else
        self->monsterinfo.currentmove = &gekk_move_run_start;
}

void gekk_run(edict_t *self)
{
    if (gekk_swims(self)) {
        self->monsterinfo.currentmove = &gekk_move_swim_start;
        return;
    }

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &gekk_move_stand;
    else
        self->monsterinfo.currentmove = &gekk_move_run;
}

mframe_t gekk_frames_run [] = {
    {ai_run, 3.849f, gekk_check_underwater},
    {ai_run, 19.606f, NULL},
    {ai_run, 25.583f, NULL},
    {ai_run, 34.625f, gekk_step},
    {ai_run, 27.365f, NULL},
    {ai_run, 28.480f, NULL}
};
mmove_t gekk_move_run = {FRAME_run_01, FRAME_run_06, gekk_frames_run, NULL};

mframe_t gekk_frames_run_st [] = {
    {ai_run, 0.212f, NULL},
    {ai_run, 19.753f, NULL}
};
mmove_t gekk_move_run_start = {FRAME_stand_01, FRAME_stand_02, gekk_frames_run_st, gekk_run};

void gekk_hit_left(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->mins[0], 8);
    if (fire_hit(self, aim, (15 + (rand() % 5)), 100))
        gi.sound(self, CHAN_WEAPON, sound_hit, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_WEAPON, sound_swing, 1, ATTN_NORM, 0);
}

void gekk_hit_right(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->maxs[0], 8);
    if (fire_hit(self, aim, (15 + (rand() % 5)), 100))
        gi.sound(self, CHAN_WEAPON, sound_hit2, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_WEAPON, sound_swing, 1, ATTN_NORM, 0);
}

void gekk_check_refire(edict_t *self)
{
    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0)
        return;

    if (random() < (skill->value * 0.1f)) {
        if (range(self, self->enemy) == RANGE_MELEE) {
            if (self->s.frame == FRAME_clawatk3_09)
                self->monsterinfo.currentmove = &gekk_move_attack2;
            else if (self->s.frame == FRAME_clawatk5_09)
                self->monsterinfo.currentmove = &gekk_move_attack1;
        }
    }
}

void loogie_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    if (other->takedamage)
        T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                 plane ? plane->normal : vec3_origin, self->dmg, 1, DAMAGE_ENERGY, MOD_GEKK);

    gi.sound(self, CHAN_AUTO, loogie_hit, 1, ATTN_NORM, 0);

    G_FreeEdict(self);
}

void fire_loogie(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed)
{
    edict_t *loogie;
    trace_t tr;

    VectorNormalize(dir);

    loogie = G_Spawn();
    VectorCopy(start, loogie->s.origin);
    VectorCopy(start, loogie->s.old_origin);
    vectoangles(dir, loogie->s.angles);
    VectorScale(dir, speed, loogie->velocity);
    loogie->movetype = MOVETYPE_FLYMISSILE;
    loogie->clipmask = MASK_SHOT;
    loogie->solid = SOLID_BBOX;
    // Xatrix put a renderfx constant in s.effects here, which lit nothing.
    // The rerelease's choice of EF_BLASTER reads as acid.
    loogie->s.effects |= EF_BLASTER;
    loogie->s.renderfx |= RF_FULLBRIGHT;
    VectorClear(loogie->mins);
    VectorClear(loogie->maxs);
    loogie->s.modelindex = gi.modelindex("models/objects/loogy/tris.md2");
    loogie->owner = self;
    loogie->touch = loogie_touch;
    loogie->nextthink = level.framenum + 2 * BASE_FRAMERATE;
    loogie->think = G_FreeEdict;
    loogie->dmg = damage;
    gi.linkentity(loogie);

    tr = gi.trace(self->s.origin, NULL, NULL, loogie->s.origin, loogie, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(loogie->s.origin, -10, dir, loogie->s.origin);
        loogie->touch(loogie, tr.ent, NULL, NULL);
    }
}

void loogie(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right, up;
    vec3_t  end;
    vec3_t  dir;
    vec3_t  gekkoffset;

    if (!self->enemy || self->enemy->health <= 0)
        return;

    VectorSet(gekkoffset, -18, -0.8f, 24);

    AngleVectors(self->s.angles, forward, right, up);
    G_ProjectSource(self->s.origin, gekkoffset, forward, right, start);
    VectorMA(start, 2, up, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);

    gi.sound(self, CHAN_BODY, sound_speet, 1, ATTN_NORM, 0);
    fire_loogie(self, start, dir, 5, 550);
}

void reloogie(edict_t *self)
{
    if (random() > 0.8f && self->health < self->max_health) {
        self->monsterinfo.currentmove = &gekk_move_idle2;
        return;
    }

    if (self->enemy && self->enemy->health >= 0) {
        if (random() > 0.7f && range(self, self->enemy) == RANGE_NEAR)
            self->monsterinfo.currentmove = &gekk_move_spit;
    }
}

mframe_t gekk_frames_spit [] = {
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},

    {ai_charge, 0, loogie},
    {ai_charge, 0, reloogie}
};
mmove_t gekk_move_spit = {FRAME_spit_01, FRAME_spit_07, gekk_frames_spit, gekk_run_start};

mframe_t gekk_frames_attack1 [] = {
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},

    {ai_charge, 0, gekk_hit_left},
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},

    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, gekk_check_refire}
};
mmove_t gekk_move_attack1 = {FRAME_clawatk3_01, FRAME_clawatk3_09, gekk_frames_attack1, gekk_run_start};

mframe_t gekk_frames_attack2 [] = {
    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, gekk_hit_left},

    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, gekk_hit_right},

    {ai_charge, 0, NULL},
    {ai_charge, 0, NULL},
    {ai_charge, 0, gekk_check_refire}
};
mmove_t gekk_move_attack2 = {FRAME_clawatk5_01, FRAME_clawatk5_09, gekk_frames_attack2, gekk_run_start};

void gekk_check_underwater(edict_t *self)
{
    if (gekk_swims(self))
        land_to_water(self);
}

mframe_t gekk_frames_leapatk [] = {
    {ai_charge, 0, NULL},
    {ai_charge, -0.387f, NULL},
    {ai_charge, -1.113f, NULL},
    {ai_charge, -0.237f, NULL},
    {ai_charge, 6.720f, gekk_jump_takeoff},     // last frame on ground
    {ai_charge, 6.414f, NULL},                  // leaves ground
    {ai_charge, 0.163f, NULL},
    {ai_charge, 28.316f, NULL},
    {ai_charge, 24.198f, NULL},
    {ai_charge, 31.742f, NULL},
    {ai_charge, 35.977f, gekk_check_landing},   // last frame in air
    {ai_charge, 12.303f, gekk_stop_skid},       // feet back on ground
    {ai_charge, 20.122f, gekk_stop_skid},
    {ai_charge, -1.042f, gekk_stop_skid},
    {ai_charge, 2.556f, gekk_stop_skid},
    {ai_charge, 0.544f, gekk_stop_skid},
    {ai_charge, 1.862f, gekk_stop_skid},
    {ai_charge, 1.224f, gekk_stop_skid},

    {ai_charge, -0.457f, gekk_check_underwater}
};
mmove_t gekk_move_leapatk = {FRAME_leapatk_01, FRAME_leapatk_19, gekk_frames_leapatk, gekk_run_start};

mframe_t gekk_frames_leapatk2 [] = {
    {ai_charge, 0, NULL},
    {ai_charge, -0.387f, NULL},
    {ai_charge, -1.113f, NULL},
    {ai_charge, -0.237f, NULL},
    {ai_charge, 6.720f, gekk_jump_takeoff2},    // last frame on ground
    {ai_charge, 6.414f, NULL},                  // leaves ground
    {ai_charge, 0.163f, NULL},
    {ai_charge, 28.316f, NULL},
    {ai_charge, 24.198f, NULL},
    {ai_charge, 31.742f, NULL},
    {ai_charge, 35.977f, gekk_check_landing},   // last frame in air
    {ai_charge, 12.303f, gekk_stop_skid},       // feet back on ground
    {ai_charge, 20.122f, gekk_stop_skid},
    {ai_charge, -1.042f, gekk_stop_skid},
    {ai_charge, 2.556f, gekk_stop_skid},
    {ai_charge, 0.544f, gekk_stop_skid},
    {ai_charge, 1.862f, gekk_stop_skid},
    {ai_charge, 1.224f, gekk_stop_skid},

    {ai_charge, -0.457f, gekk_check_underwater}
};
mmove_t gekk_move_leapatk2 = {FRAME_leapatk_01, FRAME_leapatk_19, gekk_frames_leapatk2, gekk_run_start};

void gekk_bite(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, 0, 0);
    fire_hit(self, aim, 5, 0);
}

mframe_t gekk_frames_attack [] = {
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, gekk_bite},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, gekk_bite},

    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, gekk_hit_left},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, NULL},
    {ai_charge, 16, gekk_hit_right},
    {ai_charge, 16, NULL},

    {ai_charge, 16, NULL}
};
mmove_t gekk_move_attack = {FRAME_attack_01, FRAME_attack_21, gekk_frames_attack, gekk_run_start};

void gekk_melee(edict_t *self)
{
    float   r;

    if (self->waterlevel >= 2) {
        self->monsterinfo.currentmove = &gekk_move_attack;
    } else {
        r = random();
        if (r > 0.66f)
            self->monsterinfo.currentmove = &gekk_move_attack1;
        else
            self->monsterinfo.currentmove = &gekk_move_attack2;
    }
}

void gekk_jump_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (self->health <= 0) {
        self->touch = NULL;
        return;
    }

    if (other->takedamage) {
        if (VectorLength(self->velocity) > 200) {
            vec3_t  point;
            vec3_t  normal;
            int     damage;

            VectorCopy(self->velocity, normal);
            VectorNormalize(normal);
            VectorMA(self->s.origin, self->maxs[0], normal, point);
            damage = 10 + 10 * random();
            T_Damage(other, self, self, self->velocity, point, normal, damage, damage, 0, MOD_GEKK);
        }
    }

    if (!M_CheckBottom(self)) {
        if (self->groundentity) {
            self->monsterinfo.nextframe = FRAME_leapatk_11;
            self->touch = NULL;
        }
        return;
    }

    self->touch = NULL;
}

void gekk_jump_takeoff(edict_t *self)
{
    vec3_t  forward;

    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
    AngleVectors(self->s.angles, forward, NULL, NULL);
    self->s.origin[2] += 1;

    if (gekk_check_jump(self)) {
        // high jump
        VectorScale(forward, 700, self->velocity);
        self->velocity[2] = 250;
    } else {
        VectorScale(forward, 250, self->velocity);
        self->velocity[2] = 400;
    }

    self->groundentity = NULL;
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->monsterinfo.attack_finished = level.framenum + 3 * BASE_FRAMERATE;
    self->touch = gekk_jump_touch;
}

// the water-to-land lunge: starts from the enemy's height, and much softer
void gekk_jump_takeoff2(edict_t *self)
{
    vec3_t  forward;

    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
    AngleVectors(self->s.angles, forward, NULL, NULL);

    if (self->enemy)
        self->s.origin[2] = self->enemy->s.origin[2];

    if (gekk_check_jump(self)) {
        VectorScale(forward, 300, self->velocity);
        self->velocity[2] = 250;
    } else {
        VectorScale(forward, 150, self->velocity);
        self->velocity[2] = 300;
    }

    self->groundentity = NULL;
    self->monsterinfo.aiflags |= AI_DUCKED;
    self->monsterinfo.attack_finished = level.framenum + 3 * BASE_FRAMERATE;
    self->touch = gekk_jump_touch;
}

void gekk_stop_skid(edict_t *self)
{
    if (self->groundentity)
        VectorClear(self->velocity);
}

void gekk_check_landing(edict_t *self)
{
    if (self->groundentity) {
        gi.sound(self, CHAN_WEAPON, sound_thud, 1, ATTN_NORM, 0);
        self->monsterinfo.attack_finished = 0;
        self->monsterinfo.aiflags &= ~AI_DUCKED;
        VectorClear(self->velocity);
        return;
    }

    if (level.framenum > self->monsterinfo.attack_finished)
        self->monsterinfo.nextframe = FRAME_leapatk_11;
    else
        self->monsterinfo.nextframe = FRAME_leapatk_12;
}

// The rerelease's dispatcher, not Xatrix's gekk_jump. This is the only one
// that honours NOJUMPING, which mgu4m2 sets on eight of its gekks.
void gekk_attack(edict_t *self)
{
    int r;

    if (!self->enemy)
        return;

    r = range(self, self->enemy);

    if (self->flags & FL_SWIM) {
        // stay in the water while the enemy is still in it
        if (self->enemy->waterlevel >= 2 && r <= RANGE_NEAR)
            return;

        self->flags &= ~FL_SWIM;
        self->monsterinfo.currentmove = &gekk_move_leapatk;
        self->monsterinfo.nextframe = FRAME_leapatk_05;
        return;
    }

    if (r >= RANGE_MID) {
        if (random() > 0.5f) {
            self->monsterinfo.currentmove = &gekk_move_spit;
        } else {
            self->monsterinfo.currentmove = &gekk_move_run_start;
            self->monsterinfo.attack_finished = level.framenum + 2 * BASE_FRAMERATE;
        }
    } else if (random() > 0.7f) {
        self->monsterinfo.currentmove = &gekk_move_spit;
    } else {
        if ((self->spawnflags & SPAWNFLAG_GEKK_NOJUMPING) || random() > 0.7f) {
            self->monsterinfo.currentmove = &gekk_move_run_start;
            self->monsterinfo.attack_finished = level.framenum + 1.4f * BASE_FRAMERATE;
        } else {
            self->monsterinfo.currentmove = &gekk_move_leapatk;
        }
    }
}

mframe_t gekk_frames_pain [] = {
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};
mmove_t gekk_move_pain = {FRAME_pain_01, FRAME_pain_06, gekk_frames_pain, gekk_run_start};

mframe_t gekk_frames_pain1 [] = {
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},

    {ai_move, 0, gekk_check_underwater}
};
mmove_t gekk_move_pain1 = {FRAME_pain3_01, FRAME_pain3_11, gekk_frames_pain1, gekk_run_start};

mframe_t gekk_frames_pain2 [] = {
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, NULL},

    {ai_move, 0, NULL},
    {ai_move, 0, NULL},
    {ai_move, 0, gekk_check_underwater}
};
mmove_t gekk_move_pain2 = {FRAME_pain4_01, FRAME_pain4_13, gekk_frames_pain2, gekk_run_start};

void gekk_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    float   r;

    // being hit breaks the chant
    if (self->spawnflags & SPAWNFLAG_GEKK_CHANT) {
        self->spawnflags &= ~SPAWNFLAG_GEKK_CHANT;
        return;
    }

    if (self->health < (self->max_health / 4))
        self->s.skinnum = 2;
    else if (self->health < (self->max_health / 2))
        self->s.skinnum = 1;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);

    if (self->waterlevel >= 2) {
        if (!(self->flags & FL_SWIM))
            self->flags |= FL_SWIM;
        self->monsterinfo.currentmove = &gekk_move_pain;
    } else {
        r = random();
        if (r > 0.5f)
            self->monsterinfo.currentmove = &gekk_move_pain1;
        else
            self->monsterinfo.currentmove = &gekk_move_pain2;
    }
}

void gekk_dead(edict_t *self)
{
    // a corpse in water keeps its full box, or it stops blocking and the
    // player swims through it
    if (self->waterlevel >= 2)
        return;

    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->svflags |= SVF_DEADMONSTER;
    self->nextthink = 0;
    gi.linkentity(self);
}

void gekk_gibfest(edict_t *self)
{
    int damage = 20;

    gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);

    ThrowGibACID(self, "models/objects/gekkgib/pelvis/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/arm/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/arm/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/torso/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/claw/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/leg/tris.md2", damage, GIB_ORGANIC);
    ThrowGibACID(self, "models/objects/gekkgib/leg/tris.md2", damage, GIB_ORGANIC);

    ThrowHeadACID(self, "models/objects/gekkgib/head/tris.md2", damage, GIB_ORGANIC);

    self->deadflag = DEAD_DEAD;
}

void isgibfest(edict_t *self)
{
    if (random() > 0.9f)
        gekk_gibfest(self);
}

mframe_t gekk_frames_death1 [] = {
    {ai_move, -5.151f, NULL},
    {ai_move, -12.223f, NULL},
    {ai_move, -11.484f, NULL},
    {ai_move, -17.952f, NULL},
    {ai_move, -6.953f, NULL},
    {ai_move, -7.393f, NULL},
    {ai_move, -10.713f, NULL},
    {ai_move, -17.464f, NULL},
    {ai_move, -11.678f, NULL},
    {ai_move, -11.678f, NULL}
};
mmove_t gekk_move_death1 = {FRAME_death1_01, FRAME_death1_10, gekk_frames_death1, gekk_dead};

mframe_t gekk_frames_death3 [] = {
    {ai_move, 0, NULL},
    {ai_move, 0.022f, NULL},
    {ai_move, 0.169f, NULL},
    {ai_move, -0.710f, NULL},
    {ai_move, -13.446f, NULL},
    {ai_move, -7.654f, isgibfest},
    {ai_move, -31.951f, NULL}
};
mmove_t gekk_move_death3 = {FRAME_death3_01, FRAME_death3_07, gekk_frames_death3, gekk_dead};

mframe_t gekk_frames_death4 [] = {
    {ai_move, 5.103f, NULL},
    {ai_move, -4.808f, NULL},
    {ai_move, -10.509f, NULL},
    {ai_move, -9.899f, NULL},
    {ai_move, 4.033f, isgibfest},
    {ai_move, -5.197f, NULL},
    {ai_move, -0.919f, NULL},
    {ai_move, -8.821f, NULL},
    {ai_move, -5.626f, NULL},
    {ai_move, -8.865f, isgibfest},
    {ai_move, -0.845f, NULL},
    {ai_move, 1.986f, NULL},
    {ai_move, 0.170f, NULL},
    {ai_move, 1.339f, isgibfest},
    {ai_move, -0.922f, NULL},
    {ai_move, 0.818f, NULL},
    {ai_move, -1.288f, NULL},
    {ai_move, -1.408f, isgibfest},
    {ai_move, -7.787f, NULL},
    {ai_move, -3.995f, NULL},
    {ai_move, -4.604f, NULL},
    {ai_move, -1.715f, isgibfest},
    {ai_move, -0.564f, NULL},
    {ai_move, -0.597f, NULL},
    {ai_move, 0.074f, NULL},
    {ai_move, -0.309f, isgibfest},
    {ai_move, -0.395f, NULL},
    {ai_move, -0.501f, NULL},
    {ai_move, -0.325f, NULL},
    {ai_move, -0.931f, isgibfest},
    {ai_move, -1.433f, NULL},
    {ai_move, -1.626f, NULL},
    {ai_move, 4.680f, NULL},
    {ai_move, 0.560f, NULL},
    {ai_move, -0.549f, gekk_gibfest}
};
mmove_t gekk_move_death4 = {FRAME_death4_01, FRAME_death4_35, gekk_frames_death4, gekk_dead};

mframe_t gekk_frames_wdeath [] = {
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};
mmove_t gekk_move_wdeath = {FRAME_wdeath_01, FRAME_wdeath_45, gekk_frames_wdeath, gekk_dead};

void gekk_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    float   r;

    if (self->health <= self->gib_health) {
        gekk_gibfest(self);
        return;
    }

    if (self->deadflag == DEAD_DEAD)
        return;

    gi.sound(self, CHAN_VOICE, sound_death, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->s.skinnum = 2;

    if (self->waterlevel >= 2) {
        self->monsterinfo.currentmove = &gekk_move_wdeath;
    } else {
        r = random();
        if (r > 0.66f)
            self->monsterinfo.currentmove = &gekk_move_death1;
        else if (r > 0.33f)
            self->monsterinfo.currentmove = &gekk_move_death3;
        else
            self->monsterinfo.currentmove = &gekk_move_death4;
    }
}

void gekk_duck_down(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_DUCKED)
        return;

    self->monsterinfo.aiflags |= AI_DUCKED;
    self->maxs[2] -= 32;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.pause_framenum = level.framenum + 1 * BASE_FRAMERATE;
    gi.linkentity(self);
}

void gekk_duck_up(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->maxs[2] += 32;
    self->takedamage = DAMAGE_AIM;
    gi.linkentity(self);
}

void gekk_duck_hold(edict_t *self)
{
    if (level.framenum >= self->monsterinfo.pause_framenum)
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
    else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

mframe_t gekk_frames_lduck [] = {
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};
mmove_t gekk_move_lduck = {FRAME_lduck_01, FRAME_lduck_13, gekk_frames_lduck, gekk_run_start};

mframe_t gekk_frames_rduck [] = {
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL}, {ai_move, 0, NULL},
    {ai_move, 0, NULL}
};
mmove_t gekk_move_rduck = {FRAME_rduck_01, FRAME_rduck_13, gekk_frames_rduck, gekk_run_start};

void gekk_dodge(edict_t *self, edict_t *attacker, float eta)
{
    float   r;

    r = random();
    if (r > 0.25f)
        return;

    if (!self->enemy)
        self->enemy = attacker;

    if (self->waterlevel >= 2) {
        self->monsterinfo.currentmove = &gekk_move_attack;
        return;
    }

    if (skill->value == 0) {
        if (random() > 0.5f)
            self->monsterinfo.currentmove = &gekk_move_lduck;
        else
            self->monsterinfo.currentmove = &gekk_move_rduck;
        return;
    }

    self->monsterinfo.pause_framenum = level.framenum + (eta + 0.3f) * BASE_FRAMERATE;
    r = random();

    if (skill->value == 1) {
        if (r > 0.33f) {
            if (random() > 0.5f)
                self->monsterinfo.currentmove = &gekk_move_lduck;
            else
                self->monsterinfo.currentmove = &gekk_move_rduck;
        } else {
            if (random() > 0.66f)
                self->monsterinfo.currentmove = &gekk_move_attack1;
            else
                self->monsterinfo.currentmove = &gekk_move_attack2;
        }
        return;
    }

    if (skill->value == 2) {
        if (r > 0.66f) {
            if (random() > 0.5f)
                self->monsterinfo.currentmove = &gekk_move_lduck;
            else
                self->monsterinfo.currentmove = &gekk_move_rduck;
        } else {
            if (random() > 0.66f)
                self->monsterinfo.currentmove = &gekk_move_attack1;
            else
                self->monsterinfo.currentmove = &gekk_move_attack2;
        }
        return;
    }

    // nightmare never ducks
    if (random() > 0.66f)
        self->monsterinfo.currentmove = &gekk_move_attack1;
    else
        self->monsterinfo.currentmove = &gekk_move_attack2;
}

void water_to_land(edict_t *self)
{
    self->flags &= ~FL_SWIM;
    self->yaw_speed = 20;
    self->viewheight = 25;

    self->monsterinfo.currentmove = &gekk_move_leapatk2;

    VectorSet(self->mins, -18, -18, -24);
    VectorSet(self->maxs, 18, 18, 24);
    gi.linkentity(self);
}

void land_to_water(edict_t *self)
{
    self->flags |= FL_SWIM;
    self->yaw_speed = 10;
    self->viewheight = 10;

    self->monsterinfo.currentmove = &gekk_move_swim_start;

    VectorSet(self->mins, -18, -18, -24);
    VectorSet(self->maxs, 18, 18, 16);
    gi.linkentity(self);
}

/*QUAKED monster_gekk (1 .5 0) (-18 -18 -24) (18 18 24) Ambush Trigger_Spawn Sight Chant NoJumping NoSwim
*/
void SP_monster_gekk(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    sound_swing = gi.soundindex("gek/gk_atck1.wav");
    sound_hit = gi.soundindex("gek/gk_atck2.wav");
    sound_hit2 = gi.soundindex("gek/gk_atck3.wav");
    sound_speet = gi.soundindex("gek/gk_atck4.wav");
    loogie_hit = gi.soundindex("gek/loogie_hit.wav");
    sound_death = gi.soundindex("gek/gk_deth1.wav");
    sound_pain1 = gi.soundindex("gek/gk_pain1.wav");
    sound_sight = gi.soundindex("gek/gk_sght1.wav");
    sound_search = gi.soundindex("gek/gk_idle1.wav");
    sound_step1 = gi.soundindex("gek/gk_step1.wav");
    sound_step2 = gi.soundindex("gek/gk_step2.wav");
    sound_step3 = gi.soundindex("gek/gk_step3.wav");
    sound_thud = gi.soundindex("mutant/thud1.wav");

    sound_chantlow = gi.soundindex("gek/gek_low.wav");
    sound_chantmid = gi.soundindex("gek/gek_mid.wav");
    sound_chanthigh = gi.soundindex("gek/gek_high.wav");

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/gekk/tris.md2");
    // the rerelease's box, not Xatrix's 24-wide one - its maps are built
    // to these dimensions
    VectorSet(self->mins, -18, -18, -24);
    VectorSet(self->maxs, 18, 18, 24);

    gi.modelindex("models/objects/gekkgib/pelvis/tris.md2");
    gi.modelindex("models/objects/gekkgib/arm/tris.md2");
    gi.modelindex("models/objects/gekkgib/torso/tris.md2");
    gi.modelindex("models/objects/gekkgib/claw/tris.md2");
    gi.modelindex("models/objects/gekkgib/leg/tris.md2");
    gi.modelindex("models/objects/gekkgib/head/tris.md2");
    gi.modelindex("models/objects/loogy/tris.md2");

    self->health = 125;
    self->gib_health = -30;
    self->mass = 300;

    self->pain = gekk_pain;
    self->die = gekk_die;

    self->monsterinfo.stand = gekk_stand;
    self->monsterinfo.walk = gekk_walk;
    self->monsterinfo.run = gekk_run_start;
    self->monsterinfo.dodge = gekk_dodge;
    self->monsterinfo.attack = gekk_attack;
    self->monsterinfo.melee = gekk_melee;
    self->monsterinfo.sight = gekk_sight;
    self->monsterinfo.search = gekk_search;
    self->monsterinfo.idle = gekk_idle;
    self->monsterinfo.checkattack = gekk_checkattack;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &gekk_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    walkmonster_start(self);

    if (self->spawnflags & SPAWNFLAG_GEKK_CHANT)
        self->monsterinfo.currentmove = &gekk_move_chant;
}
