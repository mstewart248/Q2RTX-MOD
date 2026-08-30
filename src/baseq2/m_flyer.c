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

flyer

==============================================================================
*/

#include "g_local.h"
#include "m_flyer.h"

bool visible(edict_t *self, edict_t *other);

static int  nextmove;           // Used for start/stop frames

static int  sound_sight;
static int  sound_idle;
static int  sound_pain1;
static int  sound_pain2;
static int  sound_slash;
static int  sound_sproing;
static int  sound_die;


void flyer_check_melee(edict_t *self);
void flyer_loop_melee(edict_t *self);
void flyer_melee(edict_t *self);
void flyer_setstart(edict_t *self);
void flyer_stand(edict_t *self);
void flyer_nextmove(edict_t *self);
void flyer_kamikaze_check(edict_t *self);
bool flyer_blocked(edict_t *self, float dist);


void flyer_sight(edict_t *self, edict_t *other)
{
    gi.sound(self, CHAN_VOICE, sound_sight, 1, ATTN_NORM, 0);
}

void flyer_idle(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_idle, 1, ATTN_IDLE, 0);
}

void flyer_pop_blades(edict_t *self)
{
    gi.sound(self, CHAN_VOICE, sound_sproing, 1, ATTN_NORM, 0);
}


mframe_t flyer_frames_stand [] = {
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
    { ai_stand, 0, NULL }
};
mmove_t flyer_move_stand = {FRAME_stand01, FRAME_stand45, flyer_frames_stand, NULL};


mframe_t flyer_frames_walk [] = {
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL },
    { ai_walk, 5, NULL }
};
mmove_t flyer_move_walk = {FRAME_stand01, FRAME_stand45, flyer_frames_walk, NULL};

mframe_t flyer_frames_run [] = {
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
mmove_t flyer_move_run = {FRAME_stand01, FRAME_stand45, flyer_frames_run, NULL};

void flyer_run(edict_t *self)
{
    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        self->monsterinfo.currentmove = &flyer_move_stand;
    else
        self->monsterinfo.currentmove = &flyer_move_run;
}

void flyer_walk(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_walk;
}

void flyer_stand(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_stand;
}

mframe_t flyer_frames_start [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, flyer_nextmove }
};
mmove_t flyer_move_start = {FRAME_start01, FRAME_start06, flyer_frames_start, NULL};

mframe_t flyer_frames_stop [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, flyer_nextmove }
};
mmove_t flyer_move_stop = {FRAME_stop01, FRAME_stop07, flyer_frames_stop, NULL};

void flyer_stop(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_stop;
}

void flyer_start(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_start;
}


mframe_t flyer_frames_rollright [] = {
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
mmove_t flyer_move_rollright = {FRAME_rollr01, FRAME_rollr09, flyer_frames_rollright, NULL};

mframe_t flyer_frames_rollleft [] = {
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
mmove_t flyer_move_rollleft = {FRAME_rollf01, FRAME_rollf09, flyer_frames_rollleft, NULL};

mframe_t flyer_frames_pain3 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t flyer_move_pain3 = {FRAME_pain301, FRAME_pain304, flyer_frames_pain3, flyer_run};

mframe_t flyer_frames_pain2 [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t flyer_move_pain2 = {FRAME_pain201, FRAME_pain204, flyer_frames_pain2, flyer_run};

mframe_t flyer_frames_pain1 [] = {
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
mmove_t flyer_move_pain1 = {FRAME_pain101, FRAME_pain109, flyer_frames_pain1, flyer_run};

mframe_t flyer_frames_defense [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },       // Hold this frame
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t flyer_move_defense = {FRAME_defens01, FRAME_defens06, flyer_frames_defense, NULL};

mframe_t flyer_frames_bankright [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t flyer_move_bankright = {FRAME_bankr01, FRAME_bankr07, flyer_frames_bankright, NULL};

mframe_t flyer_frames_bankleft [] = {
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL },
    { ai_move, 0, NULL }
};
mmove_t flyer_move_bankleft = {FRAME_bankl01, FRAME_bankl07, flyer_frames_bankleft, NULL};


void flyer_fire(edict_t *self, int flash_number)
{
    vec3_t  start;
    vec3_t  forward, right;
    vec3_t  end;
    vec3_t  dir;
    int     effect;

    if ((self->s.frame == FRAME_attak204) || (self->s.frame == FRAME_attak207) || (self->s.frame == FRAME_attak210))
		if (self->monsterFireHyperBlaster) {
			effect = EF_HYPERBLASTER;
		}
		else {
			effect = EF_BLASTER;
		}
    else
        effect = 0;
    AngleVectors(self->s.angles, forward, right, NULL);
    G_ProjectSource(self->s.origin, monster_flash_offset[flash_number], forward, right, start);

    VectorCopy(self->enemy->s.origin, end);
    end[2] += self->enemy->viewheight;
    VectorSubtract(end, start, dir);

	if (self->monsterFireHyperBlaster) {
		monster_fire_hyper_blaster(self, start, dir, 1, 1000, flash_number, effect);
	}
	else {
		monster_fire_blaster(self, start, dir, 1, 1000, flash_number, effect);
	}
}

void flyer_fireleft(edict_t *self)
{
    flyer_fire(self, MZ2_FLYER_BLASTER_1);
}

void flyer_fireright(edict_t *self)
{
    flyer_fire(self, MZ2_FLYER_BLASTER_2);
}


mframe_t flyer_frames_attack2 [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, -10, flyer_fireleft },         // left gun
    { ai_charge, -10, flyer_fireright },        // right gun
    { ai_charge, -10, flyer_fireleft },         // left gun
    { ai_charge, -10, flyer_fireright },        // right gun
    { ai_charge, -10, flyer_fireleft },         // left gun
    { ai_charge, -10, flyer_fireright },        // right gun
    { ai_charge, -10, flyer_fireleft },         // left gun
    { ai_charge, -10, flyer_fireright },        // right gun
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t flyer_move_attack2 = {FRAME_attak201, FRAME_attak217, flyer_frames_attack2, flyer_run};


void flyer_slash_left(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->mins[0], 0);
    fire_hit(self, aim, 5, 0);
    gi.sound(self, CHAN_WEAPON, sound_slash, 1, ATTN_NORM, 0);
}

void flyer_slash_right(edict_t *self)
{
    vec3_t  aim;

    VectorSet(aim, MELEE_DISTANCE, self->maxs[0], 0);
    fire_hit(self, aim, 5, 0);
    gi.sound(self, CHAN_WEAPON, sound_slash, 1, ATTN_NORM, 0);
}

mframe_t flyer_frames_start_melee [] = {
    { ai_charge, 0, flyer_pop_blades },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t flyer_move_start_melee = {FRAME_attak101, FRAME_attak106, flyer_frames_start_melee, flyer_loop_melee};

mframe_t flyer_frames_end_melee [] = {
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }
};
mmove_t flyer_move_end_melee = {FRAME_attak119, FRAME_attak121, flyer_frames_end_melee, flyer_run};


mframe_t flyer_frames_loop_melee [] = {
    { ai_charge, 0, NULL },     // Loop Start
    { ai_charge, 0, NULL },
    { ai_charge, 0, flyer_slash_left },     // Left Wing Strike
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, flyer_slash_right },    // Right Wing Strike
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL },
    { ai_charge, 0, NULL }      // Loop Ends

};
mmove_t flyer_move_loop_melee = {FRAME_attak107, FRAME_attak118, flyer_frames_loop_melee, flyer_check_melee};

void flyer_loop_melee(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_loop_melee;
}



/*
=================
flyer_set_fly_parameters

[rerelease] The flyer has two flight modes.  Cruising, it hangs 45-200 units out
and circles.  Going for a slice it lights the thrusters: no orbit offset at all,
faster, and SV_alternate_flystep stops slowing it down on approach - which is
what turns the melee into a fly-BY rather than a hover-and-poke.
=================
*/
static void flyer_set_fly_parameters(edict_t *self, bool melee)
{
    if (melee) {
        // engage thrusters for a slice
        self->monsterinfo.fly_pinned = false;
        self->monsterinfo.fly_thrusters = true;
        self->monsterinfo.fly_position_time = 0;
        monster_fly_setup(self, 210.0f, 20.0f, 0.0f, 10.0f);
    } else {
        self->monsterinfo.fly_thrusters = false;
        monster_fly_setup(self, 165.0f, 15.0f, 45.0f, 200.0f);
    }
}

void flyer_attack(edict_t *self)
{
    float   range;

    if (!M_RereleaseGame()) {
        self->monsterinfo.currentmove = &flyer_move_attack2;
        return;
    }

    // the kamikaze variant has mass 100 and does not attack - it just flies in
    if (self->mass > 50) {
        flyer_run(self);
        return;
    }

    range = realrange(self, self->enemy);
    self->monsterinfo.attack_state = AS_STRAIGHT;

    // the closer it already is, the likelier it commits to the slice
    if (self->enemy && visible(self, self->enemy) && range <= 225.0f &&
        random() > (range / 225.0f) * 0.35f) {
        self->monsterinfo.currentmove = &flyer_move_start_melee;
        flyer_set_fly_parameters(self, true);
    } else {
        self->monsterinfo.currentmove = &flyer_move_attack2;
    }

    // [Paril-KEX] sometimes pin ourselves down, a pseudo stand-ground.  A
    // pinned flyer treats fly_ideal_position as a WORLD point instead of an
    // offset from the enemy, so it holds station and shoots instead of
    // orbiting.  fly_position_time unpins it again.
    if (!self->monsterinfo.fly_pinned && (Q_rand() & 1) &&
        self->enemy && visible(self, self->enemy)) {
        self->monsterinfo.fly_pinned = true;
        // enough time left to finish attack2/3
        self->monsterinfo.fly_position_time += 1.7f * BASE_FRAMERATE;

        if (Q_rand() & 1)
            // pin to roughly where we are now
            VectorMA(self->s.origin, random(), self->velocity,
                     self->monsterinfo.fly_ideal_position);
        else
            // make the relative offset absolute
            VectorAdd(self->monsterinfo.fly_ideal_position, self->enemy->s.origin,
                      self->monsterinfo.fly_ideal_position);
    }
}

/*
=================
flyer_touch

[rerelease] Two flyers that collide bounce apart at 500 units/sec and drop out
of thruster mode, with a one-second debounce so they do not ping-pong.  Without
this a pack converging on the same hover point piles into one another.
=================
*/
void flyer_touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t  dir;

    if (!(other->monsterinfo.aiflags & AI_ALTERNATE_FLY) || !(other->flags & FL_FLY))
        return;

    if (ent->monsterinfo.duck_wait_framenum >= level.framenum)
        return;

    ent->monsterinfo.duck_wait_framenum = level.framenum + 1 * BASE_FRAMERATE;
    ent->monsterinfo.fly_thrusters = false;

    VectorSubtract(ent->s.origin, other->s.origin, dir);
    VectorNormalize(dir);
    VectorScale(dir, 500.0f, ent->velocity);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_SPLASH);
    gi.WriteByte(32);
    gi.WritePosition(ent->s.origin);
    gi.WriteDir(dir);
    gi.WriteByte(SPLASH_SPARKS);
    gi.multicast(ent->s.origin, MULTICAST_PVS);
}

void flyer_setstart(edict_t *self)
{
    nextmove = ACTION_run;
    self->monsterinfo.currentmove = &flyer_move_start;
}

void flyer_nextmove(edict_t *self)
{
    if (nextmove == ACTION_attack1)
        self->monsterinfo.currentmove = &flyer_move_start_melee;
    else if (nextmove == ACTION_attack2)
        self->monsterinfo.currentmove = &flyer_move_attack2;
    else if (nextmove == ACTION_run)
        self->monsterinfo.currentmove = &flyer_move_run;
}

void flyer_melee(edict_t *self)
{
//  flyer.nextmove = ACTION_attack1;
//  self->monsterinfo.currentmove = &flyer_move_stop;
    self->monsterinfo.currentmove = &flyer_move_start_melee;
}

void flyer_check_melee(edict_t *self)
{
    if (range(self, self->enemy) == RANGE_MELEE)
        if (random() <= 0.8f)
            self->monsterinfo.currentmove = &flyer_move_loop_melee;
        else
            self->monsterinfo.currentmove = &flyer_move_end_melee;
    else
        self->monsterinfo.currentmove = &flyer_move_end_melee;
}

void flyer_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    int     n;

    M_SetDamageSkin(self);

    if (level.framenum < self->pain_debounce_framenum)
        return;

    self->pain_debounce_framenum = level.framenum + 3 * BASE_FRAMERATE;
    if (skill->value == 3)
        return;     // no pain anims in nightmare

    n = Q_rand() % 3;
    if (n == 0) {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &flyer_move_pain1;
    } else if (n == 1) {
        gi.sound(self, CHAN_VOICE, sound_pain2, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &flyer_move_pain2;
    } else {
        gi.sound(self, CHAN_VOICE, sound_pain1, 1, ATTN_NORM, 0);
        self->monsterinfo.currentmove = &flyer_move_pain3;
    }
}


void flyer_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
	int     n;

    gi.sound(self, CHAN_VOICE, sound_die, 1, ATTN_NORM, 0);

    // Stock Quake II throws no gibs here at all - the flyer just explodes.
    if (LUDICROUS_GIBS()) {
        for (n = 0; n < 4; n++) {
            ThrowGib(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowGibRail(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowGibNoExplode(self, "models/objects/gibs/sm_meat/tris.md2", damage, GIB_ORGANIC);
            ThrowGibNoExplode(self, "models/objects/gibs/sm_metal/tris.md2", damage, GIB_METALLIC);
        }
    }

    BecomeExplosion1(self);
}


/*QUAKED monster_flyer (1 .5 0) (-16 -16 -24) (16 16 32) Ambush Trigger_Spawn Sight
*/
void SP_monster_flyer(edict_t *self)
{
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

    // fix a map bug in jail5.bsp
    if (!Q_stricmp(level.mapname, "jail5") && (self->s.origin[2] == -104)) {
        self->targetname = self->target;
        self->target = NULL;
    }

    sound_sight = gi.soundindex("flyer/flysght1.wav");
    sound_idle = gi.soundindex("flyer/flysrch1.wav");
    sound_pain1 = gi.soundindex("flyer/flypain1.wav");
    sound_pain2 = gi.soundindex("flyer/flypain2.wav");
    sound_slash = gi.soundindex("flyer/flyatck2.wav");
    sound_sproing = gi.soundindex("flyer/flyatck1.wav");
    sound_die = gi.soundindex("flyer/flydeth1.wav");

    gi.soundindex("flyer/flyatck3.wav");

    self->s.modelindex = gi.modelindex("models/monsters/flyer/tris.md2");
    // [rerelease] id's own comment: "PMM - shortened to 16 from 32".  Our tree already gave this shorter box to the kamikaze variant only; the rerelease gives it to every flyer
    if (M_RereleaseGame()) {
        VectorSet(self->mins, -16, -16, -24);
        VectorSet(self->maxs, 16, 16, 16);
    } else {
        VectorSet(self->mins, -16, -16, -24);
        VectorSet(self->maxs, 16, 16, 32);
    }
    self->movetype = MOVETYPE_STEP;
    self->solid = SOLID_BBOX;

    self->s.sound = gi.soundindex("flyer/flyidle1.wav");

    self->health = 50;
    self->mass = 50;

    self->pain = flyer_pain;
    self->die = flyer_die;

    self->monsterinfo.stand = flyer_stand;
    self->monsterinfo.walk = flyer_walk;
    self->monsterinfo.run = flyer_run;
    self->monsterinfo.attack = flyer_attack;
    self->monsterinfo.melee = flyer_melee;
    self->monsterinfo.sight = flyer_sight;
    self->monsterinfo.idle = flyer_idle;

    gi.linkentity(self);

    self->monsterinfo.currentmove = &flyer_move_stand;
    self->monsterinfo.scale = MODEL_SCALE;

    // [rerelease] the alternate fly system.  fly_buzzard lets it orbit the
    // whole sphere around its enemy rather than a level band, which is the
    // above-and-below weaving the rerelease flyer does.
    if (M_RereleaseGame()) {
        self->monsterinfo.fly_buzzard = true;
        flyer_set_fly_parameters(self, false);
        self->touch = flyer_touch;
        self->monsterinfo.blocked = flyer_blocked;
    }

    flymonster_start(self);
}

/*
==============================================================================

ROGUE - the two moves the carrier puts its spawned flyers into, plus the
kamikaze variant it can spawn instead.

flyer_move_attack3 is the same frames as attack2 but with forward motion, so a
flyer the carrier launches strafes past the player instead of hovering.

==============================================================================
*/

void flyer_kamikaze(edict_t *self);

// circle strafe: attack2's frames, but moving
mframe_t flyer_frames_attack3 [] = {
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, flyer_fireleft },          // left gun
    { ai_charge, 10, flyer_fireright },         // right gun
    { ai_charge, 10, flyer_fireleft },          // left gun
    { ai_charge, 10, flyer_fireright },         // right gun
    { ai_charge, 10, flyer_fireleft },          // left gun
    { ai_charge, 10, flyer_fireright },         // right gun
    { ai_charge, 10, flyer_fireleft },          // left gun
    { ai_charge, 10, flyer_fireright },         // right gun
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL },
    { ai_charge, 10, NULL }
};
mmove_t flyer_move_attack3 = {FRAME_attak201, FRAME_attak217, flyer_frames_attack3, flyer_run};

void flyer_kamikaze_explode(edict_t *self)
{
    vec3_t  dir;

    // hand the slot back to the carrier that launched us
    if (self->monsterinfo.commander && self->monsterinfo.commander->inuse &&
        !strcmp(self->monsterinfo.commander->classname, "monster_carrier"))
        self->monsterinfo.commander->monsterinfo.monster_slots++;

    VectorClear(dir);

    if (self->enemy) {
        VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
        T_Damage(self->enemy, self, self, dir, self->s.origin, vec3_origin,
                 50, 50, DAMAGE_RADIUS, MOD_UNKNOWN);
    }

    flyer_die(self, NULL, NULL, 0, dir);
}

/*
=================
flyer_blocked

monsterinfo.blocked.  Only the kamikaze has anything to do here: if it cannot
get past whatever is in the way, that is close enough - it detonates rather than
milling about.  A normal flyer returns false and takes the default handling.
=================
*/
bool flyer_blocked(edict_t *self, float dist)
{
    // kamikaze = 100, normal = 50
    if (self->mass != 100)
        return false;

    flyer_kamikaze_check(self);

    // if that did not blow us up - i.e. the player is what blocked us - then
    // finish the job by hand
    if (self->inuse)
        T_Damage(self, self, self, vec3_origin, self->s.origin, vec3_origin,
                 9999, 100, 0, MOD_UNKNOWN);

    return true;
}

void flyer_kamikaze_check(edict_t *self)
{
    // the blocked handling can free us before we get here
    if (!self->inuse)
        return;

    if (!self->enemy || !self->enemy->inuse) {
        flyer_kamikaze_explode(self);
        return;
    }

    self->goalentity = self->enemy;

    if (realrange(self, self->enemy) < 90)
        flyer_kamikaze_explode(self);
}

mframe_t flyer_frames_kamikaze [] = {
    { ai_charge, 40, flyer_kamikaze_check },
    { ai_charge, 40, flyer_kamikaze_check },
    { ai_charge, 40, flyer_kamikaze_check },
    { ai_charge, 40, flyer_kamikaze_check },
    { ai_charge, 40, flyer_kamikaze_check }
};
mmove_t flyer_move_kamikaze = {FRAME_rollr02, FRAME_rollr06, flyer_frames_kamikaze, flyer_kamikaze};

void flyer_kamikaze(edict_t *self)
{
    self->monsterinfo.currentmove = &flyer_move_kamikaze;
}

/*QUAKED monster_kamikaze (1 .5 0) (-16 -16 -24) (16 16 16) Ambush Trigger_Spawn Sight
ROGUE - a flyer that flies into the player and detonates. Only ever spawned at
runtime by monster_carrier; no map places one directly.
*/
void SP_monster_kamikaze(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    SP_monster_flyer(self);

    if (!self->inuse)
        return;

    // the difference from a plain flyer: it trails fire, and its box is the
    // shorter one rogue gives it
    VectorSet(self->maxs, 16, 16, 16);
    self->s.effects |= EF_ROCKET;
    self->mass = 100;

    // [rerelease] the kamikaze is explicitly NOT an alternate flyer - it goes
    // straight at its target and detonates, and it must not bounce off the
    // other flyers the carrier spawned alongside it.  SP_monster_flyer above
    // opted it in because EF_ROCKET is not set until here, so undo that.
    // flyer_blocked stays: it is the half that is FOR the kamikaze.
    self->monsterinfo.aiflags &= ~AI_ALTERNATE_FLY;
    self->monsterinfo.fly_buzzard = false;
    self->monsterinfo.fly_thrusters = false;
    self->touch = NULL;
    self->yaw_speed = 5;

    gi.linkentity(self);
}
