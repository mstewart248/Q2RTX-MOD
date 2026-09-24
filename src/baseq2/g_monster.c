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
#include "g_local.h"


//
// monster weapons
//

//FIXME mosnters should call these with a totally accurate direction
// and we can mess it up based on skill.  Spread should be for normal
// and we can tighten or loosen based on skill.  We could muck with
// the damages too, but I'm not sure that's such a good idea.
void monster_fire_bullet(edict_t *self, vec3_t start, vec3_t dir, int damage, int kick, int hspread, int vspread, int flashtype)
{
    fire_bullet(self, start, dir, damage, kick, hspread, vspread, MOD_UNKNOWN);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_shotgun(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int hspread, int vspread, int count, int flashtype)
{
    fire_shotgun(self, start, aimdir, damage, kick, hspread, vspread, count, MOD_UNKNOWN);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(aimdir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blaster(self, start, dir, damage, speed, effect, false);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

/*
=================
monster_fire_flechette

ROGUE. The gun commander's chaingun. Kick is half the damage, matching the
rerelease.
=================
*/
void monster_fire_flechette(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype)
{
    fire_flechette(self, start, dir, damage, speed, damage / 2);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blaster2(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blaster2(self, start, dir, damage, speed, effect, false);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_hyper_blaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
	fire_blaster(self, start, dir, damage, speed, effect, true);
	gi.WriteByte(svc_muzzleflash3);
	gi.WriteShort(self - g_edicts);
	gi.WriteShort(flashtype);
	gi.WriteDir(dir);
	gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_ionripper(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_ionripper(self, start, dir, damage, speed, effect);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blueblaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blueblaster(self, start, dir, damage, speed, effect);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

/*
=================
monster_dabeam

Xatrix "damage beam": a one-frame RF_BEAM entity that traces from the monster to
its enemy and hurts whatever it passes through. Used by monster_soldier_lasergun.
The beam entity is spawned by the caller and freed by dabeam_hit a frame later.
=================
*/
void dabeam_hit(edict_t *self)
{
    edict_t *ignore;
    vec3_t  start, end;
    trace_t tr;

    ignore = self;
    VectorCopy(self->s.origin, start);
    VectorMA(start, 2048, self->movedir, end);

    while (1) {
        tr = gi.trace(start, NULL, NULL, end, ignore,
                      CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_DEADMONSTER);
        if (!tr.ent)
            break;

        // A beam with dmg <= 0 is a HEALING beam - the fixbot's repair laser,
        // which rogue marks with damage -1. It is drawn and traced exactly the
        // same way, it just must not hurt what it is pointed at.
        if (self->dmg > 0 && tr.ent->takedamage &&
            !(tr.ent->flags & FL_IMMUNE_LASER) && tr.ent != self->owner)
            T_Damage(tr.ent, self, self->owner, self->movedir, tr.endpos,
                     vec3_origin, self->dmg, skill->value, DAMAGE_ENERGY, MOD_TARGET_LASER);

        // stop at the first thing that is not a monster or player
        if (!(tr.ent->svflags & SVF_MONSTER) && !tr.ent->client) {
            if (self->spawnflags & SPAWNFLAG_DABEAM_SPARK) {
                self->spawnflags &= ~SPAWNFLAG_DABEAM_SPARK;
                gi.WriteByte(svc_temp_entity);
                gi.WriteByte(TE_LASER_SPARKS);
                gi.WriteByte(10);
                gi.WritePosition(tr.endpos);
                gi.WriteDir(tr.plane.normal);
                gi.WriteByte(self->s.skinnum);
                gi.multicast(tr.endpos, MULTICAST_PVS);
            }
            break;
        }

        ignore = tr.ent;
        VectorCopy(tr.endpos, start);
    }

    VectorCopy(tr.endpos, self->s.old_origin);
    self->nextthink = level.framenum + 1;
    self->think = G_FreeEdict;
}

void monster_dabeam(edict_t *self)
{
    vec3_t last_movedir;
    vec3_t point;

    self->movetype = MOVETYPE_NONE;
    self->solid = SOLID_NOT;
    self->s.renderfx |= RF_BEAM | RF_TRANSLUCENT;
    self->s.modelindex = 1;     // must be non-zero for the beam to be sent

    self->s.frame = 2;          // beam width
    self->s.skinnum = 0xf2f2f0f0;   // beam colour (packed palette indices)

    if (self->enemy) {
        VectorCopy(self->movedir, last_movedir);
        VectorMA(self->enemy->absmin, 0.5f, self->enemy->size, point);
        VectorSubtract(point, self->s.origin, self->movedir);
        VectorNormalize(self->movedir);
        if (!VectorCompare(self->movedir, last_movedir))
            self->spawnflags |= SPAWNFLAG_DABEAM_SPARK;
    } else {
        G_SetMovedir(self->s.angles, self->movedir);
    }

    self->think = dabeam_hit;
    self->nextthink = level.framenum + 1;
    VectorSet(self->mins, -8, -8, -8);
    VectorSet(self->maxs, 8, 8, 8);
    gi.linkentity(self);

    self->spawnflags |= SPAWNFLAG_DABEAM_SPARK | SPAWNFLAG_DABEAM_ON;
    self->svflags &= ~SVF_NOCLIENT;
}

void monster_fire_grenade(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int speed, int flashtype)
{
    fire_grenade(self, start, aimdir, damage, speed, 2.5f, damage + 40);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(aimdir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_heat(edict_t *self, vec3_t start, vec3_t dir, int damage,
                       int speed, int flashtype, float turn_fraction)
{
    fire_heat(self, start, dir, damage, speed, damage, damage, turn_fraction);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_rocket(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype)
{
    fire_rocket(self, start, dir, damage, speed, damage + 20, damage);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_railgun(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int flashtype)
{
    fire_rail(self, start, aimdir, damage, kick);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(aimdir);
    gi.multicast(start, MULTICAST_PVS);
}

/*
=================
monster_fire_heatbeam

rogue's plasma beam, fired by a monster. The widow's second stage sweeps it
across the room. `fire_heatbeam`'s last argument is the monster flag, which
widens the beam and makes it forgiving about aim - see g_weapon.c.
=================
*/
void monster_fire_heatbeam(edict_t *self, vec3_t start, vec3_t dir, vec3_t offset,
                           int damage, int kick, int flashtype)
{
    fire_heatbeam(self, start, dir, offset, damage, kick, true);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

/*
=================
monster_fire_tracker

The disruptor bolt, fired by a monster. Passing an `enemy` makes it home;
passing NULL sends it straight, which is what the widow does at long range
after leading the shot with PredictAim.
=================
*/
void monster_fire_tracker(edict_t *self, vec3_t start, vec3_t dir, int damage,
                          int speed, edict_t *enemy, int flashtype)
{
    fire_tracker(self, start, dir, damage, speed, enemy);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(dir);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_bfg(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int speed, int kick, float damage_radius, int flashtype)
{
    fire_bfg(self, start, aimdir, damage, speed, damage_radius);

    gi.WriteByte(svc_muzzleflash3);
    gi.WriteShort(self - g_edicts);
    gi.WriteShort(flashtype);
    gi.WriteDir(aimdir);
    gi.multicast(start, MULTICAST_PVS);
}



//
// Monster utility functions
//

void M_FliesOff(edict_t *self)
{
    self->s.effects &= ~EF_FLIES;
    self->s.sound = 0;
}

void M_FliesOn(edict_t *self)
{
    if (self->waterlevel)
        return;
    self->s.effects |= EF_FLIES;
    self->s.sound = gi.soundindex("infantry/inflies1.wav");
    self->think = M_FliesOff;
    self->nextthink = level.framenum + 60 * BASE_FRAMERATE;
}

void M_FlyCheck(edict_t *self)
{
    if (self->waterlevel)
        return;

    if (random() > 0.5f)
        return;

    self->think = M_FliesOn;
    self->nextthink = level.framenum + (5 + 10 * random()) * BASE_FRAMERATE;
}

/*
=================
monster_footstep

[rerelease] A foot-plant sound on the frames where a monster's foot actually
lands. id added ~185 of these calls across eleven monsters, and they are most of
why the rerelease's monsters feel like they have weight.

The whole feature costs one event write here: EV_FOOTSTEP already exists in this
tree and the client already plays it (cl_sfx_footsteps, gated on the client's
own cl_footsteps) - it was simply never emitted by anything but the player.

Gated on M_RereleaseGame(): the 1997 monsters are silent on the move, and these
calls sit in frame tables that BOTH games play.
=================
*/
void monster_footstep(edict_t *self)
{
    if (!M_RereleaseGame())
        return;

    // no sound in mid-air - the rerelease's own test
    if (self->groundentity)
        self->s.event = EV_FOOTSTEP;
}

/*
=================
monster_fly_setup

[rerelease] Opt a flying monster into SV_alternate_flystep - the velocity-driven
hover model in m_move.c, which is what makes rerelease flyers circle and bank
instead of tracking in on rails.

`accel` is the rerelease's own literal, and theirs is a speed change per ENGINE
TICK.  Their engine runs at 20 or 40hz (p_weapon.cpp:459 gates the quick weapon
switch on exactly those two values, and g_monster.cpp:596 divides authored frame
distances by tick_rate/10) where this tree's monsters think at 10hz, so the
number has to be scaled up or a flyer takes four times as long to get going.
FLY_TICK_SCALE is that factor; callers keep the rerelease's own numbers so the
port stays readable against their source.  fly_speed needs no scale - a speed in
units per second is the same at any tick rate.

Callers gate on M_RereleaseGame() themselves; every one of them has other
rerelease-only fields to set alongside this.
=================
*/
#define FLY_TICK_SCALE  4.0f    // their 40hz tick against this tree's 10hz think

void monster_fly_setup(edict_t *self, float speed, float accel, float min_dist, float max_dist)
{
    self->monsterinfo.aiflags |= AI_ALTERNATE_FLY;
    self->monsterinfo.fly_speed = speed;
    self->monsterinfo.fly_acceleration = accel * FLY_TICK_SCALE;
    self->monsterinfo.fly_min_distance = min_dist;
    self->monsterinfo.fly_max_distance = max_dist;
}

/*
=================
M_SetDamageSkin

[rerelease] monsterinfo.setskin, which all 22 monsters that define it implement
identically: bit 0 of skinnum is the "bloodied" flag, SET below half health and
CLEARED above it.

The bit matters. Our pain handlers mostly wrote `skinnum = 1`, which does not
flag the skin, it REPLACES it - so a monster whose variant lives in the higher
bits reverts to the base monster's damaged skin as soon as it is hurt. That is a
real bug on the chick: monster_chick_heat spawns with skinnum 2, ChickRocket
tests `skinnum > 1` to decide between heat-seeking and plain rockets, so a
damaged heat chick silently stopped firing homing rockets and rendered as an
ordinary chick. The soldier, hover, medic and guncmdr already used |= here; the
rest did not.

Clearing the bit again is rerelease-only: the classic game never restores a
damage skin, and a medic healing a monster back above half health is the case
where the difference shows.
=================
*/
void M_SetDamageSkin(edict_t *self)
{
    if (self->health < (self->max_health / 2))
        self->s.skinnum |= 1;
    else if (M_RereleaseGame())
        self->s.skinnum &= ~1;
}

void AttackFinished(edict_t *self, float time)
{
    self->monsterinfo.attack_finished = level.framenum + time * BASE_FRAMERATE;
}


void M_CheckGround(edict_t *ent)
{
    vec3_t      point;
    trace_t     trace;

    if (ent->flags & (FL_SWIM | FL_FLY))
        return;

    // ROGUE - "up" is whichever way this entity's gravity is not. Multiplying
    // by gravityVector[2] makes the rising test work for a ceiling walker too.
    if ((ent->velocity[2] * ent->gravityVector[2]) < -100) {
        ent->groundentity = NULL;
        return;
    }

// if the hull point one-quarter unit down is solid the entity is on ground
    point[0] = ent->s.origin[0];
    point[1] = ent->s.origin[1];
    point[2] = ent->s.origin[2] + (0.25f * ent->gravityVector[2]);

    trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, point, ent, MASK_MONSTERSOLID);

    // check steepness
    if (ent->gravityVector[2] < 0) {        // normal gravity
        if (trace.plane.normal[2] < 0.7f && !trace.startsolid) {
            ent->groundentity = NULL;
            return;
        }
    } else {                                // inverted gravity
        if (trace.plane.normal[2] > -0.7f && !trace.startsolid) {
            ent->groundentity = NULL;
            return;
        }
    }

//  ent->groundentity = trace.ent;
//  ent->groundentity_linkcount = trace.ent->linkcount;
//  if (!trace.startsolid && !trace.allsolid)
//      VectorCopy (trace.endpos, ent->s.origin);
    if (!trace.startsolid && !trace.allsolid) {
        VectorCopy(trace.endpos, ent->s.origin);
        ent->groundentity = trace.ent;
        ent->groundentity_linkcount = trace.ent->linkcount;
        ent->velocity[2] = 0;
    }
}


void M_CatagorizePosition(edict_t *ent)
{
    vec3_t      point;
    int         cont;

//
// get waterlevel
//
    point[0] = ent->s.origin[0];
    point[1] = ent->s.origin[1];
    point[2] = ent->s.origin[2] + ent->mins[2] + 1;
    cont = gi.pointcontents(point);

    if (!(cont & MASK_WATER)) {
        ent->waterlevel = 0;
        ent->watertype = 0;
        return;
    }

    ent->watertype = cont;
    ent->waterlevel = 1;
    point[2] += 26;
    cont = gi.pointcontents(point);
    if (!(cont & MASK_WATER))
        return;

    ent->waterlevel = 2;
    point[2] += 22;
    cont = gi.pointcontents(point);
    if (cont & MASK_WATER)
        ent->waterlevel = 3;
}


void M_WorldEffects(edict_t *ent)
{
    int     dmg;

    if (ent->health > 0) {
        if (!(ent->flags & FL_SWIM)) {
            if (ent->waterlevel < 3) {
                ent->air_finished_framenum = level.framenum + 12 * BASE_FRAMERATE;
            } else if (ent->air_finished_framenum < level.framenum) {
                // drown!
                if (ent->pain_debounce_framenum < level.framenum) {
                    dmg = 2 + 2 * ((level.framenum - ent->air_finished_framenum) / BASE_FRAMERATE);
                    if (dmg > 15)
                        dmg = 15;
                    T_Damage(ent, world, world, vec3_origin, ent->s.origin, vec3_origin, dmg, 0, DAMAGE_NO_ARMOR, MOD_WATER);
                    ent->pain_debounce_framenum = level.framenum + 1 * BASE_FRAMERATE;
                }
            }
        } else {
            if (ent->waterlevel > 0) {
                ent->air_finished_framenum = level.framenum + 9 * BASE_FRAMERATE;
            } else if (ent->air_finished_framenum < level.framenum) {
                // suffocate!
                if (ent->pain_debounce_framenum < level.framenum) {
                    dmg = 2 + 2 * ((level.framenum - ent->air_finished_framenum) / BASE_FRAMERATE);
                    if (dmg > 15)
                        dmg = 15;
                    T_Damage(ent, world, world, vec3_origin, ent->s.origin, vec3_origin, dmg, 0, DAMAGE_NO_ARMOR, MOD_WATER);
                    ent->pain_debounce_framenum = level.framenum + 1 * BASE_FRAMERATE;
                }
            }
        }
    }

    if (ent->waterlevel == 0) {
        if (ent->flags & FL_INWATER) {
            gi.sound(ent, CHAN_BODY, gi.soundindex("player/watr_out.wav"), 1, ATTN_NORM, 0);
            ent->flags &= ~FL_INWATER;
        }
        return;
    }

    if ((ent->watertype & CONTENTS_LAVA) && !(ent->flags & FL_IMMUNE_LAVA)) {
        if (ent->damage_debounce_framenum < level.framenum) {
            ent->damage_debounce_framenum = level.framenum + 0.2f * BASE_FRAMERATE;
            T_Damage(ent, world, world, vec3_origin, ent->s.origin, vec3_origin, 10 * ent->waterlevel, 0, 0, MOD_LAVA);
        }
    }
    if ((ent->watertype & CONTENTS_SLIME) && !(ent->flags & FL_IMMUNE_SLIME)) {
        if (ent->damage_debounce_framenum < level.framenum) {
            ent->damage_debounce_framenum = level.framenum + 1 * BASE_FRAMERATE;
            T_Damage(ent, world, world, vec3_origin, ent->s.origin, vec3_origin, 4 * ent->waterlevel, 0, 0, MOD_SLIME);
        }
    }

    if (!(ent->flags & FL_INWATER)) {
        if (!(ent->svflags & SVF_DEADMONSTER)) {
            if (ent->watertype & CONTENTS_LAVA)
                if (random() <= 0.5f)
                    gi.sound(ent, CHAN_BODY, gi.soundindex("player/lava1.wav"), 1, ATTN_NORM, 0);
                else
                    gi.sound(ent, CHAN_BODY, gi.soundindex("player/lava2.wav"), 1, ATTN_NORM, 0);
            else if (ent->watertype & CONTENTS_SLIME)
                gi.sound(ent, CHAN_BODY, gi.soundindex("player/watr_in.wav"), 1, ATTN_NORM, 0);
            else if (ent->watertype & CONTENTS_WATER)
                gi.sound(ent, CHAN_BODY, gi.soundindex("player/watr_in.wav"), 1, ATTN_NORM, 0);
        }

        ent->flags |= FL_INWATER;
        ent->damage_debounce_framenum = 0;
    }
}


void M_droptofloor(edict_t *ent)
{
    vec3_t      end;
    trace_t     trace;

    // placed off the floor on purpose - leave it where the mapper put it
    if (ent->spawnflags & SPAWNFLAG_MONSTER_NO_DROP) {
        gi.linkentity(ent);
        M_CheckGround(ent);
        M_CatagorizePosition(ent);
        return;
    }

    // ROGUE - a ceiling walker drops *up* to its ceiling
    if (ent->gravityVector[2] < 0) {
        ent->s.origin[2] += 1;
        VectorCopy(ent->s.origin, end);
        end[2] -= 256;
    } else {
        ent->s.origin[2] -= 1;
        VectorCopy(ent->s.origin, end);
        end[2] += 256;
    }

    trace = gi.trace(ent->s.origin, ent->mins, ent->maxs, end, ent, MASK_MONSTERSOLID);

    if (trace.fraction == 1 || trace.allsolid)
        return;

    VectorCopy(trace.endpos, ent->s.origin);

    gi.linkentity(ent);
    M_CheckGround(ent);
    M_CatagorizePosition(ent);
}


void M_SetEffects(edict_t *ent)
{
    ent->s.effects &= ~(EF_COLOR_SHELL | EF_POWERSCREEN);
    ent->s.renderfx &= ~(RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE);

    if (ent->monsterinfo.aiflags & AI_RESURRECTING) {
        ent->s.effects |= EF_COLOR_SHELL;
        ent->s.renderfx |= RF_SHELL_RED;
    }

    if (ent->health <= 0)
        return;

    if (ent->powerarmor_framenum > level.framenum) {
        if (ent->monsterinfo.power_armor_type == POWER_ARMOR_SCREEN) {
            ent->s.effects |= EF_POWERSCREEN;
        } else if (ent->monsterinfo.power_armor_type == POWER_ARMOR_SHIELD) {
            ent->s.effects |= EF_COLOR_SHELL;
            ent->s.renderfx |= RF_SHELL_GREEN;
        }
    }
}


void M_MoveFrame(edict_t *self)
{
    mmove_t *move;
    int     index;

    move = self->monsterinfo.currentmove;
    self->nextthink = level.framenum + 1;

    if ((self->monsterinfo.nextframe) && (self->monsterinfo.nextframe >= move->firstframe) && (self->monsterinfo.nextframe <= move->lastframe)) {
        self->s.frame = self->monsterinfo.nextframe;
        self->monsterinfo.nextframe = 0;
    } else {
        if (self->s.frame == move->lastframe) {
            if (move->endfunc) {
                move->endfunc(self);

                // regrab move, endfunc is very likely to change it
                move = self->monsterinfo.currentmove;

                // check for death
                if (self->svflags & SVF_DEADMONSTER)
                    return;
            }
        }

        if (self->s.frame < move->firstframe || self->s.frame > move->lastframe) {
            self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
            self->s.frame = move->firstframe;
        } else {
            if (!(self->monsterinfo.aiflags & AI_HOLD_FRAME)) {
                self->s.frame++;
                if (self->s.frame > move->lastframe)
                    self->s.frame = move->firstframe;
            }
        }
    }

    index = self->s.frame - move->firstframe;
    if (move->frame[index].aifunc) {
        if (!(self->monsterinfo.aiflags & AI_HOLD_FRAME))
            move->frame[index].aifunc(self, move->frame[index].dist * self->monsterinfo.scale);
        else
            move->frame[index].aifunc(self, 0);
    }

    if (move->frame[index].thinkfunc)
        move->frame[index].thinkfunc(self);
}


void monster_think(edict_t *self)
{
    M_MoveFrame(self);
    if (self->linkcount != self->monsterinfo.linkcount) {
        self->monsterinfo.linkcount = self->linkcount;
        M_CheckGround(self);
    }
    M_CatagorizePosition(self);
    M_WorldEffects(self);
    M_SetEffects(self);
}


/*
================
monster_use

Using a monster makes it angry at the current activator
================
*/
void monster_use(edict_t *self, edict_t *other, edict_t *activator)
{
    if (self->enemy)
        return;
    if (self->health <= 0)
        return;
    if (!activator)
        return;
    if (activator->flags & FL_NOTARGET)
        return;
    if (!(activator->client) && !(activator->monsterinfo.aiflags & AI_GOOD_GUY))
        return;

// delay reaction so if the monster is teleported, its sound is still heard
    self->enemy = activator;
    FoundTarget(self);
}


void monster_start_go(edict_t *self);

// true while a trigger-spawned monster runs monster_start_go from its
// triggered spawn; walkmonster_start_go & co. call monster_start_go at map load
// too, before hiding the monster, and a SPAWNFLAG_MONSTER_DEAD corpse must only
// be laid out once it is actually spawned
static bool m_triggered_spawning;


void monster_triggered_spawn(edict_t *self)
{
    self->s.origin[2] += 1;
    KillBox(self);

    self->solid = SOLID_BBOX;
    self->movetype = MOVETYPE_STEP;
    self->svflags &= ~SVF_NOCLIENT;
    self->air_finished_framenum = level.framenum + 12 * BASE_FRAMERATE;
    gi.linkentity(self);

    // Trigger-spawned monsters never reach the M_droptofloor in
    // walkmonster_start_go - it is gated on !(spawnflags & 2) - so without this
    // they stay at the exact height the mapper placed them. Original Q2 maps put
    // them on the floor, so that was invisible; rerelease maps place them well
    // above it and rely on the drop (mgu6m1 spawns two gladiators 153 units up in
    // a ceiling alcove), and they hang in the air. The rerelease drops them from
    // monster_start_go and skips it for fliers/swimmers - see
    // src/rerelease/g_monster.cpp. Match that here. A mapper who wants a monster
    // left in the air still has SPAWNFLAG_MONSTER_NO_DROP, which M_droptofloor
    // honours.
    if (!(self->flags & (FL_FLY | FL_SWIM)))
        M_droptofloor(self);

    m_triggered_spawning = true;
    monster_start_go(self);
    m_triggered_spawning = false;

    // [rerelease] a good guy (SCENIC) does not go for whoever triggered it
    if (self->health > 0 && self->enemy && !(self->spawnflags & 1) &&
        !(self->enemy->flags & FL_NOTARGET) && !(self->monsterinfo.aiflags & AI_GOOD_GUY)) {
        FoundTarget(self);
    } else {
        self->enemy = NULL;
    }
}

void monster_triggered_spawn_use(edict_t *self, edict_t *other, edict_t *activator)
{
    int i;

    // we have a one frame delay here so we don't telefrag the guy who activated us
    self->think = monster_triggered_spawn;
    self->nextthink = level.framenum + 1;
    // [rerelease] q64/command's end-cutscene procession is spawned by the
    // player but must not turn on them
    if (activator->client && !(self->hackflags & HACKFLAG_END_CUTSCENE))
        self->enemy = activator;
    self->use = monster_use;

    // [rerelease] a SCENIC monster appears already in motion: spawn it now
    // (monster_triggered_spawn drops it to the floor) and run 30 animation
    // frames on the spot, so the scene it belongs to is under way the moment
    // it becomes visible - biggun's monster_soldier_light group, boss2's one.
    // 30 frames is 30 frames here too: the rerelease clears next_move_time so
    // each of its calls advances one 10 Hz animation frame, as monster_think
    // does in this tree.
    if (self->spawnflags & SPAWNFLAG_MONSTER_SCENIC) {
        self->nextthink = 0;
        self->think(self);

        if ((self->spawnflags & 1) && self->inuse)
            monster_use(self, other, activator);

        for (i = 0; i < 30; i++) {
            if (!self->inuse || !self->think)
                break;
            self->think(self);
        }
    }
}

bool monster_start(edict_t *self);

// ROGUE - a monster that never walks (the turret). Same as walkmonster_start
// but with no ground checks and no drop to floor.
void stationarymonster_triggered_spawn(edict_t *self)
{
    KillBox(self);

    self->solid = SOLID_BBOX;
    self->movetype = MOVETYPE_NONE;
    self->svflags &= ~SVF_NOCLIENT;
    self->air_finished_framenum = level.framenum + 12 * BASE_FRAMERATE;
    gi.linkentity(self);

    m_triggered_spawning = true;
    monster_start_go(self);
    m_triggered_spawning = false;

    if (self->health > 0 && self->enemy && !(self->spawnflags & 1) &&
        !(self->enemy->flags & FL_NOTARGET) && !(self->monsterinfo.aiflags & AI_GOOD_GUY))
        FoundTarget(self);
    else
        self->enemy = NULL;
}

void stationarymonster_triggered_spawn_use(edict_t *self, edict_t *other, edict_t *activator)
{
    // one frame of delay so we do not telefrag whoever triggered us
    self->think = stationarymonster_triggered_spawn;
    self->nextthink = level.framenum + 1;
    if (activator->client && !(self->hackflags & HACKFLAG_END_CUTSCENE))
        self->enemy = activator;
    self->use = monster_use;
}

void stationarymonster_triggered_start(edict_t *self)
{
    self->solid = SOLID_NOT;
    self->movetype = MOVETYPE_NONE;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = 0;
    self->use = stationarymonster_triggered_spawn_use;
}

void stationarymonster_start_go(edict_t *self)
{
    if (!self->yaw_speed)
        self->yaw_speed = 20;

    monster_start_go(self);

    if (self->spawnflags & 2)
        stationarymonster_triggered_start(self);
}

void stationarymonster_start(edict_t *self)
{
    self->think = stationarymonster_start_go;
    monster_start(self);
}

void monster_triggered_start(edict_t *self)
{
    self->solid = SOLID_NOT;
    self->movetype = MOVETYPE_NONE;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = 0;
    self->use = monster_triggered_spawn_use;
}


/*
================
monster_death_use

When a monster dies, it fires all of its targets with the current
enemy as activator.
================
*/
/*
=================
M_RereleaseAnims

Whether the monster animation frames that the rerelease APPENDED to its models
are safe to play.  The classic baseq2 tris.md2 stops short of them (infantry 207
vs 264, soldier 475 vs 575, gunner 209 vs 799), and an out-of-range frame is
clamped to 0 by the renderer, so a monster driven onto those frames with the
classic model just freezes in its default pose.

The frames ship ONLY with the rerelease game data, so the first and hardest
requirement is M_RereleaseGame().  Without it cl_md5_models - which defaults to
1 - answered yes in plain baseq2 too, and every monster gated on this drove
itself onto frames the 1997 tris.md2 does not have.  That is what made the
soldier and the infantry play the wrong animation or stop animating altogether
in the ORIGINAL game.

Given the rerelease game, cl_md5_models then decides whether the md5 models are
the ones actually loaded.  That half is approximate by nature - it is a client
rendering choice being read by server-side game code - so a dedicated server,
where each client could answer differently and no refresh has registered the
cvar at all, always says no.
=================
*/
/*
=================
M_RereleaseGame

True when we are running as the rerelease mod rather than plain baseq2.  Unlike
M_RereleaseAnims() this is not about which models are loaded - it gates
behavioural changes that would alter how the ORIGINAL game plays.
=================
*/
bool M_RereleaseGame(void)
{
    static cvar_t *gamedir;

    if (!gamedir)
        gamedir = gi.cvar("game", "", CVAR_LATCH | CVAR_SERVERINFO);

    return gamedir && !Q_stricmp(gamedir->string, "rerelease");
}

bool M_RereleaseAnims(void)
{
    // the appended frames only exist in the rerelease's own models, so plain
    // baseq2 must never be driven onto them whatever the client is rendering
    if (!M_RereleaseGame())
        return false;

    if (dedicated && dedicated->value)
        return false;

    return cl_md5_models && cl_md5_models->value != 0;
}

/*
=================
M_ProjectFlashSource

Where a monster's muzzle actually is, honouring s.scale - the gun commander is
the gunner at 1.25, so its flash offsets have to scale with it or every shot
leaves the model in the wrong place.
=================
*/
void M_ProjectFlashSource(edict_t *self, const vec3_t offset, const vec3_t forward, const vec3_t right, vec3_t result)
{
    // Scale the offset with the model, or a scaled monster's shots leave from
    // where its muzzle would have been at 1x - for mgu6m3's 5.5x Modir that is
    // somewhere inside its own shin.
    if (self->s.scale > 0.f && self->s.scale != 1.f) {
        vec3_t scaled;
        VectorScale(offset, self->s.scale, scaled);
        G_ProjectSource(self->s.origin, scaled, forward, right, result);
        return;
    }

    G_ProjectSource(self->s.origin, offset, forward, right, result);
}

/*
=================
M_CheckClearShot

Would a shot fired from this muzzle offset actually reach the enemy, or is
there geometry in the way? Lets a monster pick an attack it can land instead of
firing into a wall.

Aimed along ideal_yaw rather than the current angles, because the monster is
usually still turning when the choice is made.
=================
*/
bool M_CheckClearShot(edict_t *self, const vec3_t offset, vec3_t start)
{
    vec3_t  f, r, target, real_angles;
    trace_t tr;

    if (!self->enemy)
        return false;

    VectorSet(real_angles, self->s.angles[PITCH], self->ideal_yaw, 0);
    AngleVectors(real_angles, f, r, NULL);
    M_ProjectFlashSource(self, offset, f, r, start);

    VectorCopy(self->enemy->s.origin, target);
    target[2] += self->enemy->viewheight;

    tr = gi.trace(start, NULL, NULL, target, self, MASK_SHOT);
    if (tr.ent == self->enemy || (tr.ent && tr.ent->client) ||
        (tr.fraction > 0.8f && !tr.startsolid))
        return true;

    // try the body as well as the eyes before giving up
    VectorCopy(self->enemy->s.origin, target);
    tr = gi.trace(start, NULL, NULL, target, self, MASK_SHOT);
    if (tr.ent == self->enemy || (tr.ent && tr.ent->client) ||
        (tr.fraction > 0.8f && !tr.startsolid))
        return true;

    return false;
}

/*
=================
M_CalculatePitchToFire

Find a launch pitch whose ballistic arc lands near the target, by simulating the
flight in 0.1s steps and bouncing off whatever it hits. Approximate by design -
id's own comment calls it "very approximate".

`aim` is both input (the flat direction) and output (the pitched direction).
`mortar` restricts it to the steep pitches, which is what makes the commander
lob over cover instead of throwing flat.

Returns false when no pitch lands anywhere useful, which is the caller's cue to
pick a different attack entirely.
=================
*/
bool M_CalculatePitchToFire(edict_t *self, const vec3_t target, const vec3_t start,
                            vec3_t aim, float speed, float time_remaining,
                            bool mortar, bool destroy_on_touch)
{
    static const float pitches[] = { -80, -70, -60, -50, -40, -30, -20, -10, -5 };
    const float sim_time = 0.1f;

    float   best_pitch = 0;
    float   best_dist = 1e30f;      // stands in for FLT_MAX; float.h is not included here
    bool    found = false;
    vec3_t  pitched_aim;
    int     i;

    vectoangles(aim, pitched_aim);

    for (i = 0; i < q_countof(pitches); i++) {
        vec3_t  fwd, velocity, origin, end, diff, clipped;
        float   t;

        if (mortar && pitches[i] >= -30.0f)
            break;

        pitched_aim[PITCH] = pitches[i];
        AngleVectors(pitched_aim, fwd, NULL, NULL);

        VectorScale(fwd, speed, velocity);
        VectorCopy(start, origin);

        for (t = time_remaining; t > 0.0f; t -= sim_time) {
            trace_t tr;
            float   dist;

            velocity[2] -= sv_gravity->value * sim_time;

            VectorMA(origin, sim_time, velocity, end);
            tr = gi.trace(origin, NULL, NULL, end, NULL, MASK_SHOT);
            VectorCopy(tr.endpos, origin);

            if (tr.fraction < 1.0f) {
                if (tr.surface && (tr.surface->flags & SURF_SKY))
                    break;

                VectorAdd(origin, tr.plane.normal, origin);
                ClipVelocity(velocity, tr.plane.normal, clipped, 1.6f);
                VectorCopy(clipped, velocity);

                VectorSubtract(origin, target, diff);
                dist = VectorLengthSquared(diff);

                if (tr.ent == self->enemy || (tr.ent && tr.ent->client) ||
                    (tr.plane.normal[2] >= 0.7f && dist < (128.0f * 128.0f) && dist < best_dist)) {
                    best_pitch = pitches[i];
                    best_dist = dist;
                    found = true;
                }

                if (destroy_on_touch)
                    break;
            }
        }
    }

    if (found) {
        pitched_aim[PITCH] = best_pitch;
        AngleVectors(pitched_aim, aim, NULL, NULL);
        return true;
    }

    return false;
}

/*
=================================================================

  ROGUE / rerelease DUCK + SIDESTEP

  Two ways for a monster to get out of the way of an incoming shot: crouch
  under it (AI_DUCKED) or strafe aside (AI_DODGING). A monster opts in by
  filling monsterinfo.duck/unduck (to crouch) and/or monsterinfo.sidestep (to
  strafe); M_MonsterDodge is the shared dispatcher and is what
  monsterinfo.dodge points at.

  This tree previously had none of it - berserk, gunner and infantry all note
  "dropped: monster_done_dodge (no AI_DODGING flag in this tree)". Those
  behaviours can now be revisited.

  TIME UNITS: the rerelease works in gtime_t seconds; everything here is in
  server FRAMES, hence the *_framenum names. DUCK_INTERVAL is their 0.5s.

=================================================================
*/

#define DUCK_INTERVAL   (int)(0.5f * BASE_FRAMERATE)

// random frame count in [lo, hi] seconds
static int random_frames(float lo, float hi)
{
    return (int)((lo + (hi - lo) * random()) * BASE_FRAMERATE);
}

/*
=================
monster_done_dodge

Leave the sidestep state. Safe to call when not dodging.
=================
*/
void monster_done_dodge(edict_t *self)
{
    self->monsterinfo.aiflags &= ~AI_DODGING;
    if (self->monsterinfo.attack_state == AS_SLIDING)
        self->monsterinfo.attack_state = AS_STRAIGHT;
}

/*
=================
M_BaseHeight

The standing maxs[2] monster_duck_down shrinks from and monster_duck_up
restores.  monster_start captures it at spawn, and it is saved - but a monster
whose base_height is somehow 0 (an old save, or any future start path that
misses the capture) would otherwise duck to maxs[2] = -32, BELOW mins[2].  That
is a degenerate bounding box: nothing can trace against it, so the monster is
invisible to every bullet while still drawing at full height.  Recover instead,
and only ever return a height above the monster's own mins.
=================
*/
static float M_BaseHeight(edict_t *self)
{
    if (self->monsterinfo.base_height > self->mins[2])
        return self->monsterinfo.base_height;

    // Not usable.  If the monster is standing at the time, its current maxs[2]
    // IS the standing height; if it is already ducked, all that is left is the
    // classic 32 above the origin.
    if (!(self->monsterinfo.aiflags & AI_DUCKED) && self->maxs[2] > self->mins[2])
        self->monsterinfo.base_height = self->maxs[2];
    else
        self->monsterinfo.base_height = 32;

    return self->monsterinfo.base_height;
}

void monster_duck_down(edict_t *self)
{
    // Read the standing height BEFORE anything sets AI_DUCKED - M_BaseHeight's
    // recovery path needs to know whether the monster is standing right now.
    float crouch = M_BaseHeight(self) - 32;

    // The classic dodge (see the *_dodge functions, still used whenever the
    // game is not the rerelease) has no M_MonsterDodge in front of it to set
    // the hold up, so it reproduces the 1997 *_duck_down here: refuse to
    // re-duck, and hold the crouch for a flat one second.  pause_framenum is
    // written as well because that is the field id clobbered, and the classic
    // infantry's firing window reads it.
    //
    // The soldier, infantry, gunner, chick and medic all did exactly this.  The
    // BRAIN is the exception - its duck_down set no timer at all and left the
    // hold to whatever brain_dodge had put there - so it wraps this call in
    // brain_duck_down() to put its own value back.
    if (!M_RereleaseGame()) {
        if (self->monsterinfo.aiflags & AI_DUCKED)
            return;

        self->monsterinfo.duck_wait_framenum = level.framenum + 1 * BASE_FRAMERATE;
        self->monsterinfo.pause_framenum = self->monsterinfo.duck_wait_framenum;
    }

    // A crouch may never take maxs[2] to or below mins[2]: that is a degenerate
    // box and the monster stops being hittable altogether.
    if (crouch < self->mins[2] + 1)
        crouch = self->mins[2] + 1;

    self->monsterinfo.aiflags |= AI_DUCKED;

    self->maxs[2] = crouch;
    self->takedamage = DAMAGE_YES;
    self->monsterinfo.next_duck_framenum = level.framenum + DUCK_INTERVAL;
    gi.linkentity(self);
}

/*
=================
monster_duck_hold

Called from a crouch frame. Holds the animation on that frame until the duck
expires - note AI_HOLD_FRAME stops MOVEMENT too, which is what is wanted here
because a ducking monster should not slide along the floor.
=================
*/
void monster_duck_hold(edict_t *self)
{
    if (level.framenum >= self->monsterinfo.duck_wait_framenum)
        self->monsterinfo.aiflags &= ~AI_HOLD_FRAME;
    else
        self->monsterinfo.aiflags |= AI_HOLD_FRAME;
}

void monster_duck_up(edict_t *self)
{
    if (!(self->monsterinfo.aiflags & AI_DUCKED))
        return;

    self->maxs[2] = M_BaseHeight(self);
    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->takedamage = DAMAGE_YES;

    // finishing a duck cleanly halves the remaining cooldown
    if (self->monsterinfo.next_duck_framenum > level.framenum)
        self->monsterinfo.next_duck_framenum =
            level.framenum + (self->monsterinfo.next_duck_framenum - level.framenum) / 2;

    gi.linkentity(self);
}

/*
=================
M_MonsterDodge

monsterinfo.dodge for every monster that implements duck or sidestep. `eta` is
how long until the shot arrives, in seconds.

The rerelease passes a trace and a gravity flag as well; this tree's dodge
signature has neither, so the shot-height test that decides "duck under it" vs
"step around it" cannot be made. Without it the choice is: sidestep when the
monster can, otherwise duck. Monsters that only duck are unaffected.
=================
*/
void M_MonsterDodge(edict_t *self, edict_t *attacker, float eta, trace_t *tr, bool gravity)
{
    // You cannot duck under something that arcs down onto you, so a bouncing or
    // tossed projectile disables the crouch and leaves only the sidestep.
    bool ducker = (self->monsterinfo.duck && self->monsterinfo.unduck && !gravity);
    bool dodger = (self->monsterinfo.sidestep != NULL);
    float height;

    if (!ducker && !dodger)
        return;

    if (!self->enemy)
        return;

    // one frame of warning is not enough to react to, and 2.5s is so far off
    // that reacting now is pointless
    if (eta < FRAMETIME || eta > 2.5f)
        return;

    // half the time, just take it
    if (random() > 0.5f)
        return;

    // How high the shot is going to pass. The -1 is because absmax is
    // s.origin + maxs + 1. A shot arriving BELOW this can be ducked under; one
    // above it has to be stepped around.
    if (ducker && tr) {
        height = self->absmax[2] - 32 - 1;

        // nothing to duck under, and no sidestep available
        if (!dodger && (tr->endpos[2] <= height || (self->monsterinfo.aiflags & AI_DUCKED)))
            return;
    } else {
        height = self->absmax[2];
    }

    if (dodger) {
        // already mid-sidestep: let it finish
        if (self->monsterinfo.aiflags & AI_DODGING)
            return;

        if (level.framenum < self->monsterinfo.dodge_framenum)
            return;

        // If we can duck and the shot is coming in low, prefer the duck - fall
        // through to it rather than stepping into the shot's path.
        if (!(!ducker || !tr || tr->endpos[2] <= height || (self->monsterinfo.aiflags & AI_DUCKED)))
            goto try_duck;

        // easy/normal sidestep less often
        if (skill->value < 2 && random() > (skill->value < 1 ? 0.25f : 0.50f)) {
            self->monsterinfo.dodge_framenum = level.framenum + random_frames(0.8f, 1.4f);
            return;
        }

        // Step away from where the shot is going, not randomly, when we know.
        if (tr) {
            vec3_t right, diff;

            AngleVectors(self->s.angles, NULL, right, NULL);
            VectorSubtract(tr->endpos, self->s.origin, diff);
            self->monsterinfo.lefty = (DotProduct(right, diff) < 0) ? 0 : 1;
        } else {
            self->monsterinfo.lefty = (random() < 0.5f);
        }

        if (self->monsterinfo.sidestep(self)) {
            if (ducker && (self->monsterinfo.aiflags & AI_DUCKED))
                self->monsterinfo.unduck(self);

            self->monsterinfo.aiflags |= AI_DODGING;
            self->monsterinfo.attack_state = AS_SLIDING;
            self->monsterinfo.dodge_framenum = level.framenum + random_frames(0.4f, 2.0f);
            return;
        }
    }

try_duck:
    // no sidestep available (or it declined) - crouch instead, but only once
    // the shot is genuinely close
    if (ducker && eta < 0.5f) {
        if (level.framenum < self->monsterinfo.next_duck_framenum)
            return;

        monster_done_dodge(self);

        if (self->monsterinfo.duck(self, eta)) {
            if (self->monsterinfo.duck_wait_framenum < level.framenum)
                self->monsterinfo.duck_wait_framenum =
                    level.framenum + (int)(eta * BASE_FRAMERATE);

            monster_duck_down(self);

            // stay down longer on the easier skills
            if (skill->value < 1)
                self->monsterinfo.duck_wait_framenum += random_frames(0.5f, 1.0f);
            else if (skill->value < 2)
                self->monsterinfo.duck_wait_framenum += random_frames(0.1f, 0.35f);
        }

        self->monsterinfo.dodge_framenum = level.framenum + random_frames(0.2f, 0.7f);
    }
}

void ED_CallSpawn(edict_t *ent);
void drop_temp_touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf);
void drop_make_touchable(edict_t *ent);

/*
=================
M_DropHealthItem

[rerelease] A monster whose "item" is one of the health classnames. The
rerelease gives each health size its own itemlist row with a classname
(g_items.cpp), so FindItemByClassname finds it and Drop_Item tosses it. This
tree has a single classname-less "Health" row whose size lives in the spawn
function (count/style/model), so there is nothing for FindItemByClassname to
find. monster_start keeps the classname instead (in ->map, see there) and this
spawns the real item entity and then tosses it exactly like Drop_Item does:
same box, same 100 forward / 300 up throw, same 1 second before it can be
picked up. mgu1m3's mutants (boss_d1/2/3 ...) drop item_health_small this way;
18 of the 27 such monsters are there.
=================
*/
static edict_t *M_DropHealthItem(edict_t *self, const char *classname)
{
    edict_t *dropped;
    vec3_t  forward;

    dropped = G_Spawn();
    dropped->classname = (char *)classname;
    VectorCopy(self->s.origin, dropped->s.origin);

    // the spawn function sets the model, count and style for this size
    ED_CallSpawn(dropped);
    if (!dropped->inuse)
        return NULL;
    if (!dropped->item || !dropped->model) {
        G_FreeEdict(dropped);
        return NULL;
    }

    dropped->spawnflags = DROPPED_ITEM;
    dropped->s.effects = dropped->item->world_model_flags;
    dropped->s.renderfx = RF_GLOW;
    VectorSet(dropped->mins, -15, -15, -15);
    VectorSet(dropped->maxs, 15, 15, 15);
    gi.setmodel(dropped, dropped->model);
    dropped->solid = SOLID_TRIGGER;
    dropped->movetype = MOVETYPE_TOSS;
    dropped->touch = drop_temp_touch;
    dropped->owner = self;

    AngleVectors(self->s.angles, forward, NULL, NULL);
    VectorScale(forward, 100, dropped->velocity);
    dropped->velocity[2] = 300;

    // replaces the droptofloor think SpawnItem queued
    dropped->think = drop_make_touchable;
    dropped->nextthink = level.framenum + 1 * BASE_FRAMERATE;

    gi.linkentity(dropped);
    return dropped;
}

void monster_death_use(edict_t *self)
{
    self->flags &= ~(FL_FLY | FL_SWIM);
    self->monsterinfo.aiflags &= AI_GOOD_GUY;

    // health drop - see M_DropHealthItem and monster_start
    if (!self->item && self->map) {
        edict_t *dropped = M_DropHealthItem(self, self->map);

        if (dropped && self->itemtarget) {
            dropped->target = self->itemtarget;
            self->itemtarget = NULL;
        }

        self->map = NULL;
    }

    if (self->item) {
        edict_t *dropped = Drop_Item(self, self->item);

        // [rerelease] "itemtarget" fires when the DROPPED item is picked up, not
        // on death, so it hands its name to the item as a target. Only
        // rhangar1's monster_gladiator actually pairs the two keys - it drops
        // key_airstrike_target and points at the target_poi "poi_turret".
        // mgu5m1 and mgu5m3 each carry an itemtarget with no "item" key at all,
        // which does nothing here exactly as it does nothing in the rerelease.
        if (self->itemtarget) {
            dropped->target = self->itemtarget;
            self->itemtarget = NULL;
        }

        self->item = NULL;
    }

    if (self->deathtarget)
        self->target = self->deathtarget;

    if (self->target)
        G_UseTargets(self, self->enemy);

    // [rerelease] healthtarget fires on every pain hit AND once more on death
    if (self->healthtarget) {
        self->target = self->healthtarget;
        G_UseTargets(self, self->enemy);
    }
}

/*
=================
M_FireHealthTarget

[rerelease] A monster with a "healthtarget" fires it every time it is hurt.
id does this in M_ProcessPain, right after the pain callback; this tree calls
pain straight out of T_Damage, so this is called from there instead.
=================
*/
void M_FireHealthTarget(edict_t *self)
{
    char *saved;

    if (!self->healthtarget)
        return;

    saved = self->target;
    self->target = self->healthtarget;
    G_UseTargets(self, self->enemy);
    self->target = saved;
}


//============================================================================

bool monster_start(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return false;
    }

    // [rerelease] SCENIC monsters are set dressing - biggun's eight
    // monster_soldier_light (sf 524291) and boss2's one play a scripted scene
    // instead of fighting, so they are good guys and never enter the tally
    if (self->spawnflags & SPAWNFLAG_MONSTER_SCENIC)
        self->monsterinfo.aiflags |= AI_GOOD_GUY;

    // [rerelease] the N64 hackflags. q64/command's closing procession (18
    // monsters with HACKFLAG_END_CUTSCENE marching down a path) must not count
    // toward the kill total or the level could never read 100%.
    if (self->hackflags & (HACKFLAG_END_CUTSCENE | HACKFLAG_ATTACK_PLAYER))
        self->monsterinfo.aiflags |= AI_DO_NOT_COUNT;

    if ((self->spawnflags & 4) && !(self->monsterinfo.aiflags & AI_GOOD_GUY)) {
        self->spawnflags &= ~4;
        self->spawnflags |= 1;
//      gi.dprintf("fixed spawnflags on %s at %s\n", self->classname, vtos(self->s.origin));
    }

    // ROGUE - AI_DO_NOT_COUNT keeps summoned and healed monsters, and the
    // throwaway entities DetermineBBox spawns, out of the level tally.
    // [rerelease] a corpse (SPAWNFLAG_MONSTER_DEAD) is not a kill either -
    // mgu3m1/m2/m4 each lay out 8-11 dead mutants.
    if (!(self->monsterinfo.aiflags & (AI_GOOD_GUY | AI_DO_NOT_COUNT)) &&
        !(self->spawnflags & SPAWNFLAG_MONSTER_DEAD))
        level.total_monsters++;

    // [rerelease] "health_multiplier" scales the spawn function's base health.
    // id does it in every monster's spawn function (health = 300 *
    // st.health_multiplier in m_mutant.cpp and ~40 others); doing it here, once,
    // before max_health is taken covers every monster in this tree without
    // touching each file. The MGU maps set it on 530 monsters, mostly to make
    // the hard-skill duplicates tougher (mgu3m* place a 0.75 and a 1.0 copy of
    // each monster). gib_health is not scaled, as in the rerelease. 0 means the
    // key was not given. The value is consumed so a monster spawned later at
    // runtime (a carrier's flyers, a medic commander's summons) cannot inherit
    // it from whatever entity was parsed last - `st` is not cleared after the
    // map loads. widow/widow2/carrier apply it themselves (their coop bonus is
    // added after the multiply) and zero it before getting here.
    if (st.health_multiplier > 0)
        self->health = (int)(self->health * st.health_multiplier);
    st.health_multiplier = 0;

    self->nextthink = level.framenum + 1;
    self->svflags |= SVF_MONSTER;
    self->s.renderfx |= RF_FRAMELERP;
    self->takedamage = DAMAGE_AIM;
    self->air_finished_framenum = level.framenum + 12 * BASE_FRAMERATE;
    self->use = monster_use;
    self->max_health = self->health;
    self->clipmask = MASK_MONSTERSOLID;

    self->s.skinnum = 0;
    self->deadflag = DEAD_NO;
    self->svflags &= ~SVF_DEADMONSTER;

    if (!self->monsterinfo.checkattack)
        self->monsterinfo.checkattack = M_CheckAttack;
    VectorCopy(self->s.origin, self->s.old_origin);

    // ->map carries a health drop's classname on monsters (below); a stray
    // "map" key must not be mistaken for one
    self->map = NULL;

    if (st.item) {
        self->item = FindItemByClassname(st.item);
        if (!self->item) {
            // [rerelease] health sizes have no classnamed itemlist row here;
            // remember the classname and spawn it on death (M_DropHealthItem).
            // ->map is free on a monster (only changelevel/sky/coop-relay
            // entities use it) and is saved as a level string, so this
            // survives a savegame. Precache now: the drop happens mid-level.
            static const struct {
                const char *classname;
                const char *model;
            } health_drops[] = {
                { "item_health",       "models/items/healing/medium/tris.md2" },
                { "item_health_small", "models/items/healing/stimpack/tris.md2" },
                { "item_health_large", "models/items/healing/large/tris.md2" },
                { "item_health_mega",  "models/items/mega_h/tris.md2" },
            };
            int i;
            bool found = false;

            for (i = 0; i < (int)(sizeof(health_drops) / sizeof(health_drops[0])); i++) {
                if (!Q_stricmp(st.item, health_drops[i].classname)) {
                    self->map = (char *)health_drops[i].classname;
                    gi.modelindex(health_drops[i].model);
                    PrecacheItem(FindItem("Health"));
                    found = true;
                    break;
                }
            }

            if (!found)
                gi.dprintf("%s at %s has bad item: %s\n", self->classname, vtos(self->s.origin), st.item);
        }

        // consumed, like health_multiplier above: a monster summoned later
        // must not inherit the last parsed entity's drop
        st.item = NULL;
    }

    // randomize what frame they start on
    if (self->monsterinfo.currentmove)
        self->s.frame = self->monsterinfo.currentmove->firstframe + (Q_rand() % (self->monsterinfo.currentmove->lastframe - self->monsterinfo.currentmove->firstframe + 1));

    // [rerelease] per-entity model scale. The spawn function has just set
    // mins/maxs, so this is where id grows the monster to match the model:
    // the bounding box, the mass, and monsterinfo.scale, which is the per-frame
    // movement distance multiplier. mgu6m3's Modir is a monster_shambler at
    // 5.5, and without this it would be a normal-sized shambler wearing a giant
    // model. Done BEFORE base_height is captured so a scaled monster ducks from
    // its real height.
    if (self->s.scale > 0.f && self->s.scale != 1.f) {
        self->monsterinfo.scale *= self->s.scale;
        VectorScale(self->mins, self->s.scale, self->mins);
        VectorScale(self->maxs, self->s.scale, self->maxs);
        self->mass = (int)(self->mass * self->s.scale);
        gi.linkentity(self);
    }

    // [rerelease] set the pathing style if the spawn function did not. A
    // monster with a melee attack and no ranged one has to close the distance
    // or it is harmless, so it gets the navmesh even in plain sight;
    // everything else is MIXED, which paths only from mid range out.
    if (self->monsterinfo.combat_style == COMBAT_UNKNOWN) {
        if (!self->monsterinfo.attack && self->monsterinfo.melee)
            self->monsterinfo.combat_style = COMBAT_MELEE;
        else
            self->monsterinfo.combat_style = COMBAT_MIXED;
    }

    // ROGUE/rerelease duck system: remember how tall this monster stands, so
    // monster_duck_down has something to shrink from and monster_duck_up has
    // something to restore. Captured here, after the spawn function has set
    // mins/maxs and before anything can duck.
    self->monsterinfo.base_height = self->maxs[2];

    // the power armor key bits belong to the entity being parsed; drop them so
    // a monster summoned later (CreateMonster reuses the stale `st`) takes its
    // spawn function's default armor rather than the last map entity's keys
    st.keys_specified &= ~(SPAWNKEY_POWER_ARMOR_TYPE | SPAWNKEY_POWER_ARMOR_POWER);

    return true;
}

/*
=================
M_SpawnDead

[rerelease] SPAWNFLAG_MONSTER_DEAD: lay the monster out as a corpse, the way
monster_start_go does in g_monster.cpp - by making it die naturally and then
fast-forwarding the death animation. mgu3m1/m2/m4 scatter 30 dead mutants this
way, mgu1m2 has dead gunners/infantry/light soldiers, mgu1m4 a dead SS.

die() is called straight, not through Killed(), so the corpse is never
counted as a kill (monster_start left it out of total_monsters too) and
monster_death_use never runs - no deathtarget, no item drop, as in the
rerelease. Health 0 is above every gib_health, so die() picks a death
animation rather than gibbing; it also sets deadflag DEAD_DEAD, which keeps
Killed() from counting it when the player shoots the body later.

AI_SPAWNED_DEAD is up while the death frames' think functions run, so the few
that would act on the world (death-frame weapon fire, BossExplode) can skip.
Only the frames' thinkfuncs run, not their movement - the body shrinks and
settles exactly where it was placed.
=================
*/
static void M_SpawnDead(edict_t *self)
{
    mmove_t *move;
    vec3_t  f;
    vec3_t  point = { 0, 0, 0 };
    int     i;

    self->health = 0;
    VectorCopy(self->s.origin, f);

    self->monsterinfo.aiflags |= AI_SPAWNED_DEAD;

    if (self->die)
        self->die(self, self, self, 0, point);

    if (!self->inuse)
        return;

    // the rerelease calls monsterinfo.setskin here: a corpse wears the
    // damaged skin even though it never took the pain that sets it
    if (self->health < self->max_health / 2)
        self->s.skinnum |= 1;

    move = self->monsterinfo.currentmove;
    if (move) {
        for (i = move->firstframe; i < move->lastframe; i++) {
            self->s.frame = i;

            if (move->frame[i - move->firstframe].thinkfunc)
                move->frame[i - move->firstframe].thinkfunc(self);

            if (!self->inuse)
                return;

            // a thinkfunc swapped the animation - stop fast-forwarding a move
            // that is no longer the one playing
            if (self->monsterinfo.currentmove != move)
                break;
        }

        if (self->monsterinfo.currentmove == move) {
            if (move->endfunc)
                move->endfunc(self);

            if (!self->inuse)
                return;

            self->s.frame = move->lastframe;
        }
    }

    VectorCopy(f, self->s.origin);
    gi.linkentity(self);

    self->monsterinfo.aiflags &= ~AI_SPAWNED_DEAD;
}

void monster_start_go(edict_t *self)
{
    vec3_t  v;
    bool    spawn_dead;

    if (self->health <= 0)
        return;

    // [rerelease] a SCALED monster's eyes are not 25 units off the floor. The
    // callers above have just set the classic fixed viewheight, so scale it
    // here, where every start path passes. Left alone at 1x, because the
    // rerelease's wider change (viewheight = maxs[2] - 8 for everything) would
    // move every monster's sight line in the classic game too.
    if (self->s.scale > 0.f && self->s.scale != 1.f)
        self->viewheight = (int)(self->viewheight * self->s.scale);

    // check for target to combat_point and change to combattarget
    if (self->target) {
        bool        notcombat;
        bool        fixup;
        edict_t     *target;

        target = NULL;
        notcombat = false;
        fixup = false;
        while ((target = G_Find(target, FOFS(targetname), self->target)) != NULL) {
            if (strcmp(target->classname, "point_combat") == 0) {
                self->combattarget = self->target;
                fixup = true;
            } else {
                notcombat = true;
            }
        }
        if (notcombat && self->combattarget)
            gi.dprintf("%s at %s has target with mixed types\n", self->classname, vtos(self->s.origin));
        if (fixup)
            self->target = NULL;
    }

    // validate combattarget
    if (self->combattarget) {
        edict_t     *target;

        target = NULL;
        while ((target = G_Find(target, FOFS(targetname), self->combattarget)) != NULL) {
            if (strcmp(target->classname, "point_combat") != 0) {
                gi.dprintf("%s at (%i %i %i) has a bad combattarget %s : %s at (%i %i %i)\n",
                           self->classname, (int)self->s.origin[0], (int)self->s.origin[1], (int)self->s.origin[2],
                           self->combattarget, target->classname, (int)target->s.origin[0], (int)target->s.origin[1],
                           (int)target->s.origin[2]);
            }
        }
    }

    // [rerelease] allow spawning dead. A trigger-spawned corpse is laid out
    // when it is triggered, not at map load (see m_triggered_spawning).
    spawn_dead = (self->spawnflags & SPAWNFLAG_MONSTER_DEAD) &&
                 (!(self->spawnflags & 2) || m_triggered_spawning);

    if (self->target) {
        self->goalentity = self->movetarget = G_PickTarget(self->target);
        if (!self->movetarget) {
            gi.dprintf("%s can't find target %s at %s\n", self->classname, self->target, vtos(self->s.origin));
            self->target = NULL;
            self->monsterinfo.pause_framenum = INT_MAX;
            if (!spawn_dead)
                self->monsterinfo.stand(self);
        } else if (strcmp(self->movetarget->classname, "path_corner") == 0) {
            VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
            self->ideal_yaw = self->s.angles[YAW] = vectoyaw(v);
            if (!spawn_dead)
                self->monsterinfo.walk(self);
            self->target = NULL;
        } else {
            self->goalentity = self->movetarget = NULL;
            self->monsterinfo.pause_framenum = INT_MAX;
            if (!spawn_dead)
                self->monsterinfo.stand(self);
        }
    } else {
        self->monsterinfo.pause_framenum = INT_MAX;
        if (!spawn_dead)
            self->monsterinfo.stand(self);
    }

    if (spawn_dead) {
        M_SpawnDead(self);
        return;
    }

    self->think = monster_think;
    self->nextthink = level.framenum + 1;
}


void walkmonster_start_go(edict_t *self)
{
    if (!(self->spawnflags & 2) && level.time < 1) {
        M_droptofloor(self);

        if (self->groundentity)
            if (!M_walkmove(self, 0, 0))
                gi.dprintf("%s in solid at %s\n", self->classname, vtos(self->s.origin));
    }

    if (!self->yaw_speed)
        self->yaw_speed = 20;
    self->viewheight = 25;

    monster_start_go(self);

    if (self->spawnflags & 2)
        monster_triggered_start(self);
}

void walkmonster_start(edict_t *self)
{
    self->think = walkmonster_start_go;
    monster_start(self);
}


void flymonster_start_go(edict_t *self)
{
    if (!M_walkmove(self, 0, 0))
        gi.dprintf("%s in solid at %s\n", self->classname, vtos(self->s.origin));

    if (!self->yaw_speed)
        self->yaw_speed = 10;
    self->viewheight = 25;

    monster_start_go(self);

    if (self->spawnflags & 2)
        monster_triggered_start(self);
}


void flymonster_start(edict_t *self)
{
    self->flags |= FL_FLY;
    self->think = flymonster_start_go;
    monster_start(self);
}


void swimmonster_start_go(edict_t *self)
{
    if (!self->yaw_speed)
        self->yaw_speed = 10;
    self->viewheight = 10;

    monster_start_go(self);

    if (self->spawnflags & 2)
        monster_triggered_start(self);
}

void swimmonster_start(edict_t *self)
{
    self->flags |= FL_SWIM;
    self->think = swimmonster_start_go;
    monster_start(self);
}
