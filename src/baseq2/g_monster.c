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

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_shotgun(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int hspread, int vspread, int count, int flashtype)
{
    fire_shotgun(self, start, aimdir, damage, kick, hspread, vspread, count, MOD_UNKNOWN);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blaster(self, start, dir, damage, speed, effect, false);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
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

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blaster2(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blaster2(self, start, dir, damage, speed, effect, false);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_hyper_blaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
	fire_blaster(self, start, dir, damage, speed, effect, true);
	gi.WriteByte(svc_muzzleflash2);
	gi.WriteShort(self - g_edicts);
	gi.WriteByte(flashtype);
	gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_ionripper(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_ionripper(self, start, dir, damage, speed, effect);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_blueblaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype, int effect)
{
    fire_blueblaster(self, start, dir, damage, speed, effect);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
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

        if (tr.ent->takedamage && !(tr.ent->flags & FL_IMMUNE_LASER) && tr.ent != self->owner)
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

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_heat(edict_t *self, vec3_t start, vec3_t dir, int damage,
                       int speed, int flashtype, float turn_fraction)
{
    fire_heat(self, start, dir, damage, speed, damage, damage, turn_fraction);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_rocket(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int flashtype)
{
    fire_rocket(self, start, dir, damage, speed, damage + 20, damage);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_railgun(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int flashtype)
{
    fire_rail(self, start, aimdir, damage, kick);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
    gi.multicast(start, MULTICAST_PVS);
}

void monster_fire_bfg(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int speed, int kick, float damage_radius, int flashtype)
{
    fire_bfg(self, start, aimdir, damage, speed, damage_radius);

    gi.WriteByte(svc_muzzleflash2);
    gi.WriteShort(self - g_edicts);
    gi.WriteByte(flashtype);
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


void monster_triggered_spawn(edict_t *self)
{
    self->s.origin[2] += 1;
    KillBox(self);

    self->solid = SOLID_BBOX;
    self->movetype = MOVETYPE_STEP;
    self->svflags &= ~SVF_NOCLIENT;
    self->air_finished_framenum = level.framenum + 12 * BASE_FRAMERATE;
    gi.linkentity(self);

    monster_start_go(self);

    if (self->enemy && !(self->spawnflags & 1) && !(self->enemy->flags & FL_NOTARGET)) {
        FoundTarget(self);
    } else {
        self->enemy = NULL;
    }
}

void monster_triggered_spawn_use(edict_t *self, edict_t *other, edict_t *activator)
{
    // we have a one frame delay here so we don't telefrag the guy who activated us
    self->think = monster_triggered_spawn;
    self->nextthink = level.framenum + 1;
    if (activator->client)
        self->enemy = activator;
    self->use = monster_use;
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

    monster_start_go(self);

    if (self->enemy && !(self->spawnflags & 1) && !(self->enemy->flags & FL_NOTARGET))
        FoundTarget(self);
    else
        self->enemy = NULL;
}

void stationarymonster_triggered_spawn_use(edict_t *self, edict_t *other, edict_t *activator)
{
    // one frame of delay so we do not telefrag whoever triggered us
    self->think = stationarymonster_triggered_spawn;
    self->nextthink = level.framenum + 1;
    if (activator->client)
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

The frames exist only in the rerelease models, so gate on the client's
cl_md5_models.  This is approximate by nature - it is a client rendering choice
being read by server-side game code - so a dedicated server, where each client
could answer differently and no refresh has registered the cvar at all, always
says no.
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
    // The rerelease scales the offset by s.scale here, because the gun
    // commander is the gunner model at 1.25. THIS TREE HAS NO PER-ENTITY SCALE:
    // entity_state_t carries none and the protocol does not send one (same
    // limitation the flares hit). So the commander renders gunner-sized and the
    // offsets need no scaling. If per-entity scale is ever added, scale here.
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

void monster_duck_down(edict_t *self)
{
    self->monsterinfo.aiflags |= AI_DUCKED;

    // base_height is captured at spawn in monster_start_go; without it a duck
    // would shrink the monster relative to whatever maxs[2] happened to be.
    self->maxs[2] = self->monsterinfo.base_height - 32;
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

    self->monsterinfo.aiflags &= ~AI_DUCKED;
    self->maxs[2] = self->monsterinfo.base_height;
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

void monster_death_use(edict_t *self)
{
    self->flags &= ~(FL_FLY | FL_SWIM);
    self->monsterinfo.aiflags &= AI_GOOD_GUY;

    if (self->item) {
        Drop_Item(self, self->item);
        self->item = NULL;
    }

    if (self->deathtarget)
        self->target = self->deathtarget;

    if (!self->target)
        return;

    G_UseTargets(self, self->enemy);
}


//============================================================================

bool monster_start(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return false;
    }

    if ((self->spawnflags & 4) && !(self->monsterinfo.aiflags & AI_GOOD_GUY)) {
        self->spawnflags &= ~4;
        self->spawnflags |= 1;
//      gi.dprintf("fixed spawnflags on %s at %s\n", self->classname, vtos(self->s.origin));
    }

    // ROGUE - AI_DO_NOT_COUNT keeps summoned and healed monsters, and the
    // throwaway entities DetermineBBox spawns, out of the level tally
    if (!(self->monsterinfo.aiflags & (AI_GOOD_GUY | AI_DO_NOT_COUNT)))
        level.total_monsters++;

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

    if (st.item) {
        self->item = FindItemByClassname(st.item);
        if (!self->item)
            gi.dprintf("%s at %s has bad item: %s\n", self->classname, vtos(self->s.origin), st.item);
    }

    // randomize what frame they start on
    if (self->monsterinfo.currentmove)
        self->s.frame = self->monsterinfo.currentmove->firstframe + (Q_rand() % (self->monsterinfo.currentmove->lastframe - self->monsterinfo.currentmove->firstframe + 1));

    // ROGUE/rerelease duck system: remember how tall this monster stands, so
    // monster_duck_down has something to shrink from and monster_duck_up has
    // something to restore. Captured here, after the spawn function has set
    // mins/maxs and before anything can duck.
    self->monsterinfo.base_height = self->maxs[2];

    return true;
}

void monster_start_go(edict_t *self)
{
    vec3_t  v;

    if (self->health <= 0)
        return;

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

    if (self->target) {
        self->goalentity = self->movetarget = G_PickTarget(self->target);
        if (!self->movetarget) {
            gi.dprintf("%s can't find target %s at %s\n", self->classname, self->target, vtos(self->s.origin));
            self->target = NULL;
            self->monsterinfo.pause_framenum = INT_MAX;
            self->monsterinfo.stand(self);
        } else if (strcmp(self->movetarget->classname, "path_corner") == 0) {
            VectorSubtract(self->goalentity->s.origin, self->s.origin, v);
            self->ideal_yaw = self->s.angles[YAW] = vectoyaw(v);
            self->monsterinfo.walk(self);
            self->target = NULL;
        } else {
            self->goalentity = self->movetarget = NULL;
            self->monsterinfo.pause_framenum = INT_MAX;
            self->monsterinfo.stand(self);
        }
    } else {
        self->monsterinfo.pause_framenum = INT_MAX;
        self->monsterinfo.stand(self);
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
