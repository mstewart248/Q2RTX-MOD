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

hover

==============================================================================
*/

#include "g_local.h"
#include "m_hover.h"

bool visible(edict_t *self, edict_t *other);


static int  sound_pain1;
static int  sound_pain2;
static int  sound_death1;
static int  sound_death2;
static int  sound_sight;
static int  sound_search1;
static int  sound_search2;

// ROGUE - the daedalus is the same model on skin 2 with its own voice. It is
// told apart from the icarus by mass alone (225 vs 150), exactly as rogue and
// the rerelease do it - there is no flag and no separate think chain.
static int  daed_sound_pain1;
static int  daed_sound_pain2;
static int  daed_sound_death1;
static int  daed_sound_death2;
static int  daed_sound_sight;
static int  daed_sound_search1;
static int  daed_sound_search2;

#define HOVER_IS_DAEDALUS(self)     ((self)->mass >= 225)


void hover_sight(edict_t *self, edict_t *other)
{
    if (HOVER_IS_DAEDALUS(self))
        gi.sound(self, CHAN_VOICE, daed_sound_sight, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void hover_search(edict_t *self)
{
    if (HOVER_IS_DAEDALUS(self)) {
        if (random() < 0.5f)
            gi.sound(self, CHAN_VOICE, daed_sound_search1, 1, ATTN_NORM, 0);
        else
            gi.sound(self, CHAN_VOICE, daed_sound_search2, 1, ATTN_NORM, 0);
    } else {
        if (random() < 0.5f)
            gi.sound(self, CHAN_VOICE, sound_search1, 1, ATTN_NORM, 0);
        else
            gi.sound(self, CHAN_VOICE, sound_search2, 1, ATTN_NORM, 0);
    }
}


void hover_run(edict_t *self);
void hover_stand(edict_t *self);
void hover_dead(edict_t *self);
void hover_attack(edict_t *self);
void hover_reattack(edict_t *self);
void hover_fire_blaster(edict_t *self);
void hover_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point);

mframe_t hover_frames_stand [] = {
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
mmove_t hover_move_stand = {FRAME_stand01, FRAME_stand30, hover_frames_stand, NULL};

mframe_t hover_frames_stop1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_stop1 = {FRAME_stop101, FRAME_stop109, hover_frames_stop1, NULL};

mframe_t hover_frames_stop2 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_stop2 = {FRAME_stop201, FRAME_stop208, hover_frames_stop2, NULL};

mframe_t hover_frames_takeoff [] = {
    { ai_move,    0,  NULL },
    { ai_move,    -2, NULL },
    { ai_move,    5,  NULL },
    { ai_move,    -1, NULL },
    { ai_move,    1,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    -1, NULL },
    { ai_move,    -1, NULL },
    { ai_move,    -1, NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    -6, NULL },
    { ai_move,    -9, NULL },
    { ai_move,    1,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_takeoff = {FRAME_takeof01, FRAME_takeof30, hover_frames_takeoff, NULL};

mframe_t hover_frames_pain3 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_pain3 = {FRAME_pain301, FRAME_pain309, hover_frames_pain3, hover_run};

mframe_t hover_frames_pain2 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_pain2 = {FRAME_pain201, FRAME_pain212, hover_frames_pain2, hover_run};

mframe_t hover_frames_pain1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    -8, NULL },
    { ai_move,    -4, NULL },
    { ai_move,    -6, NULL },
    { ai_move,    -4, NULL },
    { ai_move,    -3, NULL },
    { ai_move,    1,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    7,  NULL },
    { ai_move,    1,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    2,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    5,  NULL },
    { ai_move,    3,  NULL },
    { ai_move,    4,  NULL }
};
mmove_t hover_move_pain1 = {FRAME_pain101, FRAME_pain128, hover_frames_pain1, hover_run};

mframe_t hover_frames_land [] = {
    { ai_move,    0,  NULL }
};
mmove_t hover_move_land = {FRAME_land01, FRAME_land01, hover_frames_land, NULL};

mframe_t hover_frames_forward [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_forward = {FRAME_forwrd01, FRAME_forwrd35, hover_frames_forward, NULL};

mframe_t hover_frames_walk [] = {
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL },
    { ai_walk,    4,  NULL }
};
mmove_t hover_move_walk = {FRAME_forwrd01, FRAME_forwrd35, hover_frames_walk, NULL};

mframe_t hover_frames_run [] = {
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL },
    { ai_run, 10, NULL }
};
mmove_t hover_move_run = {FRAME_forwrd01, FRAME_forwrd35, hover_frames_run, NULL};

mframe_t hover_frames_death1 [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    -10, NULL },
    { ai_move,    3,  NULL },
    { ai_move,    5,  NULL },
    { ai_move,    4,  NULL },
    { ai_move,    7,  NULL }
};
mmove_t hover_move_death1 = {FRAME_death101, FRAME_death111, hover_frames_death1, hover_dead};

mframe_t hover_frames_backward [] = {
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL },
    { ai_move,    0,  NULL }
};
mmove_t hover_move_backward = {FRAME_backwd01, FRAME_backwd24, hover_frames_backward, NULL};

mframe_t hover_frames_start_attack [] = {
    { ai_charge,  1,  NULL },
    { ai_charge,  1,  NULL },
    { ai_charge,  1,  NULL }
};
mmove_t hover_move_start_attack = {FRAME_attak101, FRAME_attak103, hover_frames_start_attack, hover_attack};

mframe_t hover_frames_attack1 [] = {
    { ai_charge,  -10,    hover_fire_blaster },
    { ai_charge,  -10,    hover_fire_blaster },
    { ai_charge,  0,      hover_reattack },
};
mmove_t hover_move_attack1 = {FRAME_attak104, FRAME_attak106, hover_frames_attack1, NULL};


mframe_t hover_frames_end_attack [] = {
    { ai_charge,  1,  NULL },
    { ai_charge,  1,  NULL }
};
mmove_t hover_move_end_attack = {FRAME_attak107, FRAME_attak108, hover_frames_end_attack, hover_run};

// ROGUE - the circle-strafe attack. Same frames as attack1, but the ai_charge
// distances are positive so the monster slides sideways while firing.
mframe_t hover_frames_attack2 [] = {
    { ai_charge,  10, hover_fire_blaster },
    { ai_charge,  10, hover_fire_blaster },
    { ai_charge,  10, hover_reattack },
};
mmove_t hover_move_attack2 = {FRAME_attak104, FRAME_attak106, hover_frames_attack2, NULL};

// rogue also declares start_attack2/end_attack2 tables, but nothing in rogue or
// the rerelease ever selects them - the strafing attack shares the icarus'
// start and end moves. Not ported, so they don't end up in g_ptrs.c.

void hover_reattack(edict_t *self)
{
    if (self->enemy->health > 0)
        if (visible(self, self->enemy))
            if (random() <= 0.6f) {
                // stay in whichever attack hover_attack picked, so a circle
                // strafe keeps sliding instead of snapping back to straight
                if (self->monsterinfo.attack_state == AS_SLIDING)
                    self->monsterinfo.currentmove = &hover_move_attack2;
                else
                    self->monsterinfo.currentmove = &hover_move_attack1;
                return;
            }
    self->monsterinfo.currentmove = &hover_move_end_attack;
}


void hover_fire_blaster(edict_t *self)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  end;
    vec3_t  dir;
    int     effect;
    int     flash;

    if (self->s.frame == FRAME_attak104)
		if (self->monsterFireHyperBlaster && !HOVER_IS_DAEDALUS(self)) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else
        effect = 0;

    // the daedalus fires from its own muzzle offset; the rerelease alternates a
    // second one per frame, but that flash number is a rerelease addition and
    // these MZ2_* values go over the wire, so only the rogue one is used here
    flash = HOVER_IS_DAEDALUS(self) ? MZ2_DAEDALUS_BLASTER : MZ2_HOVER_BLASTER_1;

    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash], forward, right, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);

    if (HOVER_IS_DAEDALUS(self)) {
        // ROGUE - the daedalus fires the green blaster2 bolt
        monster_fire_blaster2(self, start, dir, 1, 1000, flash, effect);
    }
	else if (self->monsterFireHyperBlaster) {
		monster_fire_hyper_blaster(self, start, dir, 1, 1000, flash, effect);
	}
	else {
		monster_fire_blaster(self, start, dir, 1, 1000, flash, effect);
	}
}


void hover_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &hover_move_stand;
}

void hover_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &hover_move_stand;
    else
        self->monsterinfo.currentmove = &hover_move_run;
}

void hover_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &hover_move_walk;
}

void hover_start_attack(edict_t *self)
{
    self->monsterinfo.currentmove = &hover_move_start_attack;
}

void hover_attack(edict_t *self)
{
    float chance = 0.5f;

    if (HOVER_IS_DAEDALUS(self))    // the daedalus strafes more
        chance += 0.1f;

    if (random() > chance) {
        self->monsterinfo.currentmove = &hover_move_attack1;
        self->monsterinfo.attack_state = AS_STRAIGHT;
    } else {                        // circle strafe
        if (random() <= 0.5f)       // switch directions
            self->monsterinfo.lefty = !self->monsterinfo.lefty;
        self->monsterinfo.currentmove = &hover_move_attack2;
        self->monsterinfo.attack_state = AS_SLIDING;
    }
}


void hover_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    bool    daed;

    // the pain skin is the low bit, so this reads 0 -> 1 for the icarus and
    // 2 -> 3 for the daedalus
    if (self->health < (self->max_health / 2))
        self->s.skinnum |= 1;

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;

    if (skill->value == 3)
        return;     // no pain anims in nightmare

    daed = HOVER_IS_DAEDALUS(self);

    if (damage <= 25) {
        if (random() < 0.5f) {
            gi.sound(self, CHAN_VOICE, daed ? daed_sound_pain1 : sound_pain1, 1, ATTN_NORM, 0);
            self->monsterinfo.currentmove = &hover_move_pain3;
        } else {
            gi.sound(self, CHAN_VOICE, daed ? daed_sound_pain2 : sound_pain2, 1, ATTN_NORM, 0);
            self->monsterinfo.currentmove = &hover_move_pain2;
        }
    } else {
        gi.sound(self, CHAN_VOICE, daed ? daed_sound_pain1 : sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &hover_move_pain1;
    }
}

void hover_deadthink(edict_t *self)
{
	int n;

    // hover_dead stores timestamp as a frame number, so compare and
    // reschedule in frames - comparing it against level.time (seconds) made
    // the "wait until it lands" check pass on the very first think.
    if (!self->groundentity && level.framenum < self->timestamp) {
        self->nextthink = level.framenum + 1;
        return;
    }
    // Stock Quake II throws no gibs here - the icarus just explodes.
    if (LUDICROUS_GIBS()) {
        for (n = 0; n < 16; n++) {
            if (n < 8) {
                ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", 100, GIB_ORGANIC);
                ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", 100, GIB_ORGANIC);
                ThrowGibNoExplode(self, "models/objects/gibs/bone/tris.md2", 100, GIB_ORGANIC);
            }
            ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", 100, GIB_ORGANIC);
        }
    }

    BecomeExplosion1(self);
}

void hover_dead(edict_t *self)
{
    VectorSet(self->mins, -16, -16, -24);
    VectorSet(self->maxs, 16, 16, -8);
    self->movetype = MOVETYPE_TOSS;
    self->think = hover_deadthink;
    self->nextthink = level.framenum + 1;
    self->timestamp = level.framenum + 15 * BASE_FRAMERATE;
    gi.linkentity(self);
}

void hover_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    int     n;

// check for gib
    if (self->health <= self->gib_health) {
        // Stock Quake II: one burst of gibs and the body is gone.
        if (!LUDICROUS_GIBS()) {
            gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
            for (n = 0; n < 2; n++)
                ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
            for (n = 0; n < 2; n++)
                ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowHead(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            self->deadflag = DEAD_DEAD;
            return;
        }

        // LUDICROUS GIBS: the burst scales with what killed it, and the
        // corpse is left shootable so it can be torn down in stages.
        gi.sound(self, CHAN_VOICE, gi.soundindex("misc/udeath.wav"), 1, ATTN_NORM, 0);
		

		if (InflictorGibExplosion(inflictor, self)) {
			VectorScale(self->size, 1.2, self->size);
			for (n = 0; n < 16; n++)
				ThrowGib(self, "models/objects/gibs/bone/tris.md2", damage, GIB_ORGANIC);
			for (n = 0; n < 16; n++)
				ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			ThrowHead(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
			VectorScale(self->size, 0.8, self->size);
		}
		else if (!Q_stricmp(inflictor->classname, "bolt")) {
			ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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
					ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
					ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
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
    if (random() < 0.5f)
        gi.sound(self, CHAN_VOICE, HOVER_IS_DAEDALUS(self) ? daed_sound_death1 : sound_death1, 1, ATTN_NORM, 0);
    else
        gi.sound(self, CHAN_VOICE, HOVER_IS_DAEDALUS(self) ? daed_sound_death2 : sound_death2, 1, ATTN_NORM, 0);
    self->deadflag = DEAD_DEAD;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.currentmove = &hover_move_death1;
}

/*QUAKED monster_hover (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
/*QUAKED monster_daedalus (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
This is the improved icarus monster.
*/
void SP_monster_hover(edict_t *self)
{
    bool    daedalus;

    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

	float val = crandom();

	if (val < 0) {
		self->monsterFireHyperBlaster = qtrue;
	}
	else {
		self->monsterFireHyperBlaster = qfalse;
	}

    // ROGUE - the daedalus is told apart by mass alone from here on, so this
    // has to be settled before any of the HOVER_IS_DAEDALUS branches run
    daedalus = !strcmp(self->classname, "monster_daedalus");

    if (daedalus) {
        daed_sound_pain1 = gi.soundindex("daedalus/daedpain1.wav");
        daed_sound_pain2 = gi.soundindex("daedalus/daedpain2.wav");
        daed_sound_death1 = gi.soundindex("daedalus/daeddeth1.wav");
        daed_sound_death2 = gi.soundindex("daedalus/daeddeth2.wav");
        daed_sound_sight = gi.soundindex("daedalus/daedsght1.wav");
        daed_sound_search1 = gi.soundindex("daedalus/daedsrch1.wav");
        daed_sound_search2 = gi.soundindex("daedalus/daedsrch2.wav");

        gi.soundindex("daedalus/daedatck1.wav");

        self->s.sound = gi.soundindex("daedalus/daedidle1.wav");
    } else {
        sound_pain1 = gi.soundindex("hover/hovpain1.wav");
        sound_pain2 = gi.soundindex("hover/hovpain2.wav");
        sound_death1 = gi.soundindex("hover/hovdeth1.wav");
        sound_death2 = gi.soundindex("hover/hovdeth2.wav");
        sound_sight = gi.soundindex("hover/hovsght1.wav");
        sound_search1 = gi.soundindex("hover/hovsrch1.wav");
        sound_search2 = gi.soundindex("hover/hovsrch2.wav");

        gi.soundindex("hover/hovatck1.wav");

        self->s.sound = gi.soundindex("hover/hovidle1.wav");
    }

    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;
    self->s.modelindex = gi.modelindex("models/monsters/hover/tris.md2");
    VectorSet(self->mins, -24, -24, -24);
    VectorSet(self->maxs, 24, 24, 32);

    self->health = 240;
    self->gib_health = -100;
    self->mass = 150;
    self->yaw_speed = 18;

    if (daedalus) {
        self->health = 450;
        self->mass = 225;
        self->yaw_speed = 23;
        self->monsterinfo.power_armor_type = POWER_ARMOR_SCREEN;
        self->monsterinfo.power_armor_power = 100;
    }

    self->pain = hover_pain;
    self->die = hover_die;

    self->monsterinfo.stand = hover_stand;
    self->monsterinfo.walk = hover_walk;
    self->monsterinfo.run = hover_run;
//  self->monsterinfo.dodge = hover_dodge;
    self->monsterinfo.attack = hover_start_attack;
    self->monsterinfo.sight = hover_sight;
    self->monsterinfo.search = hover_search;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &hover_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    flymonster_start(self);

    // after flymonster_start, which resets skinnum on some paths
    if (daedalus)
        self->s.skinnum = 2;
}
