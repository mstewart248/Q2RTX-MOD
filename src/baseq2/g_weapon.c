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


/*
=================
check_dodge

This is a support routine used when a client is firing
a non-instant attack weapon.  It checks to see if a
monster's dodge function should be called.
=================
*/
static void check_dodge(edict_t *self, vec3_t start, vec3_t dir, int speed)
{
    vec3_t  end;
    vec3_t  v;
    trace_t tr;
    float   eta;

    // easy mode only ducks one quarter the time
    if (skill->value == 0) {
        if (random() > 0.25f)
            return;
    }
    VectorMA(start, 8192, dir, end);
    tr = gi.trace(start, NULL, NULL, end, self, MASK_SHOT);
    if ((tr.ent) && (tr.ent->svflags & SVF_MONSTER) && (tr.ent->health > 0) && (tr.ent->monsterinfo.dodge) && infront(tr.ent, self)) {
        VectorSubtract(tr.endpos, start, v);
        eta = (VectorLength(v) - tr.ent->maxs[0]) / speed;
        tr.ent->monsterinfo.dodge(tr.ent, self, eta);
    }
}


/*
=================
fire_hit

Used for all impact (hit/punch/slash) attacks
=================
*/
bool fire_hit(edict_t *self, vec3_t aim, int damage, int kick)
{
    trace_t     tr;
    vec3_t      forward, right, up;
    vec3_t      v;
    vec3_t      point;
    float       range;
    vec3_t      dir;

    //see if enemy is in range
    VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
    range = VectorLength(dir);
    if (range > aim[0])
        return false;

    if (aim[1] > self->mins[0] && aim[1] < self->maxs[0]) {
        // the hit is straight on so back the range up to the edge of their bbox
        range -= self->enemy->maxs[0];
    } else {
        // this is a side hit so adjust the "right" value out to the edge of their bbox
        if (aim[1] < 0)
            aim[1] = self->enemy->mins[0];
        else
            aim[1] = self->enemy->maxs[0];
    }

    VectorMA(self->s.origin, range, dir, point);

    tr = gi.trace(self->s.origin, NULL, NULL, point, self, MASK_SHOT);
    if (tr.fraction < 1) {
        if (!tr.ent->takedamage)
            return false;
        // if it will hit any client/monster then hit the one we wanted to hit
        if ((tr.ent->svflags & SVF_MONSTER) || (tr.ent->client))
            tr.ent = self->enemy;
    }

    AngleVectors(self->s.angles, forward, right, up);
    VectorMA(self->s.origin, range, forward, point);
    VectorMA(point, aim[1], right, point);
    VectorMA(point, aim[2], up, point);
    VectorSubtract(point, self->enemy->s.origin, dir);

    // do the damage
    T_Damage(tr.ent, self, self, dir, point, vec3_origin, damage, kick / 2, DAMAGE_NO_KNOCKBACK, MOD_HIT);

    if (!(tr.ent->svflags & SVF_MONSTER) && (!tr.ent->client))
        return false;

    // do our special form of knockback here
    VectorMA(self->enemy->absmin, 0.5f, self->enemy->size, v);
    VectorSubtract(v, point, v);
    VectorNormalize(v);
    VectorMA(self->enemy->velocity, kick, v, self->enemy->velocity);
    if (self->enemy->velocity[2] > 0)
        self->enemy->groundentity = NULL;
    return true;
}


/*
=================
fire_lead

This is an internal support routine used for bullet/pellet based weapons.
=================
*/
static void fire_lead(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int te_impact, int hspread, int vspread, int mod)
{
    trace_t     tr;
    vec3_t      dir;
    vec3_t      forward, right, up;
    vec3_t      end;
    float       r;
    float       u;
    vec3_t      water_start;
    bool        water = false;
    int         content_mask = MASK_SHOT | MASK_WATER;

    tr = gi.trace(self->s.origin, NULL, NULL, start, self, MASK_SHOT);
    if (!(tr.fraction < 1.0f)) {
        vectoangles(aimdir, dir);
        AngleVectors(dir, forward, right, up);

        r = crandom() * hspread;
        u = crandom() * vspread;
        VectorMA(start, 8192, forward, end);
        VectorMA(end, r, right, end);
        VectorMA(end, u, up, end);

        if (gi.pointcontents(start) & MASK_WATER) {
            water = true;
            VectorCopy(start, water_start);
            content_mask &= ~MASK_WATER;
        }

        tr = gi.trace(start, NULL, NULL, end, self, content_mask);

        // see if we hit water
        if (tr.contents & MASK_WATER) {
            int     color;

            water = true;
            VectorCopy(tr.endpos, water_start);

            if (!VectorCompare(start, tr.endpos)) {
                if (tr.contents & CONTENTS_WATER) {
                    if (strcmp(tr.surface->name, "*brwater") == 0)
                        color = SPLASH_BROWN_WATER;
                    else
                        color = SPLASH_BLUE_WATER;
                } else if (tr.contents & CONTENTS_SLIME)
                    color = SPLASH_SLIME;
                else if (tr.contents & CONTENTS_LAVA)
                    color = SPLASH_LAVA;
                else
                    color = SPLASH_UNKNOWN;

                if (color != SPLASH_UNKNOWN) {
                    gi.WriteByte(svc_temp_entity);
                    gi.WriteByte(TE_SPLASH);
                    gi.WriteByte(8);
                    gi.WritePosition(tr.endpos);
                    gi.WriteDir(tr.plane.normal);
                    gi.WriteByte(color);
                    gi.multicast(tr.endpos, MULTICAST_PVS);
                }

                // change bullet's course when it enters water
                VectorSubtract(end, start, dir);
                vectoangles(dir, dir);
                AngleVectors(dir, forward, right, up);
                r = crandom() * hspread * 2;
                u = crandom() * vspread * 2;
                VectorMA(water_start, 8192, forward, end);
                VectorMA(end, r, right, end);
                VectorMA(end, u, up, end);
            }

            // re-trace ignoring water this time
            tr = gi.trace(water_start, NULL, NULL, end, self, MASK_SHOT);
        }
    }

    // send gun puff / flash
    if (!((tr.surface) && (tr.surface->flags & SURF_SKY))) {
        if (tr.fraction < 1.0f) {
            if (tr.ent->takedamage) {
                T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal, damage, kick, DAMAGE_BULLET, mod);
            } else {
                if (strncmp(tr.surface->name, "sky", 3) != 0) {
                    gi.WriteByte(svc_temp_entity);
                    gi.WriteByte(te_impact);
                    gi.WritePosition(tr.endpos);
                    gi.WriteDir(tr.plane.normal);
                    gi.multicast(tr.endpos, MULTICAST_PVS);

                    if (self->client)
                        PlayerNoise(self, tr.endpos, PNOISE_IMPACT);
                }
            }
        }
    }

    // if went through water, determine where the end and make a bubble trail
    if (water) {
        vec3_t  pos;

        VectorSubtract(tr.endpos, water_start, dir);
        VectorNormalize(dir);
        VectorMA(tr.endpos, -2, dir, pos);
        if (gi.pointcontents(pos) & MASK_WATER)
            VectorCopy(pos, tr.endpos);
        else
            tr = gi.trace(pos, NULL, NULL, water_start, tr.ent, MASK_WATER);

        VectorAdd(water_start, tr.endpos, pos);
        VectorScale(pos, 0.5f, pos);

        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_BUBBLETRAIL);
        gi.WritePosition(water_start);
        gi.WritePosition(tr.endpos);
        gi.multicast(pos, MULTICAST_PVS);
    }
}


/*
=================
fire_bullet

Fires a single round.  Used for machinegun and chaingun.  Would be fine for
pistols, rifles, etc....
=================
*/
void fire_bullet(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int hspread, int vspread, int mod)
{
    fire_lead(self, start, aimdir, damage, kick, TE_GUNSHOT, hspread, vspread, mod);
}


/*
=================
fire_shotgun

Shoots shotgun pellets.  Used by shotgun and super shotgun.
=================
*/
void fire_shotgun(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick, int hspread, int vspread, int count, int mod)
{
    int     i;

    for (i = 0; i < count; i++)
        fire_lead(self, start, aimdir, damage, kick, TE_SHOTGUN, hspread, vspread, mod);
}


/*
=================
fire_blaster

Fires a single blaster bolt.  Used by the blaster and hyper blaster.
=================
*/
void blaster_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    int     mod;

    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    if (other->takedamage) {
        if (self->spawnflags & 1)
            mod = MOD_HYPERBLASTER;
        else
            mod = MOD_BLASTER;
        T_Damage(other, self, self->owner, self->velocity, self->s.origin, plane->normal, self->dmg, 1, DAMAGE_ENERGY, mod);
    } else {
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_BLASTER);
        gi.WritePosition(self->s.origin);
        if (!plane)
            gi.WriteDir(vec3_origin);
        else
            gi.WriteDir(plane->normal);
        gi.multicast(self->s.origin, MULTICAST_PVS);
    }

    G_FreeEdict(self);
}

void hyper_blaster_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
	int     mod;

	if (other == self->owner)
		return;

	if (surf && (surf->flags & SURF_SKY)) {
		G_FreeEdict(self);
		return;
	}

	if (self->owner->client)
		PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

	if (other->takedamage) {
		if (self->spawnflags & 1)
			mod = MOD_HYPERBLASTER;
		else
			mod = MOD_BLASTER;
		T_Damage(other, self, self->owner, self->velocity, self->s.origin, plane->normal, self->dmg, 1, DAMAGE_ENERGY, mod);
	}
	else {
		gi.WriteByte(svc_temp_entity);
		gi.WriteByte(TE_HYPERBLASTER);
		gi.WritePosition(self->s.origin);
		if (!plane)
			gi.WriteDir(vec3_origin);
		else
			gi.WriteDir(plane->normal);
		gi.multicast(self->s.origin, MULTICAST_PVS);
	}

	G_FreeEdict(self);
}

void fire_blaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int effect, bool hyper)
{
    edict_t *bolt;
    trace_t tr;

    VectorNormalize(dir);

    bolt = G_Spawn();
    bolt->svflags = SVF_DEADMONSTER;
    // yes, I know it looks weird that projectiles are deadmonsters
    // what this means is that when prediction is used against the object
    // (blaster/hyperblaster shots), the player won't be solid clipped against
    // the object.  Right now trying to run into a firing hyperblaster
    // is very jerky since you are predicted 'against' the shots.
    VectorCopy(start, bolt->s.origin);
    VectorCopy(start, bolt->s.old_origin);
    vectoangles(dir, bolt->s.angles);
    VectorScale(dir, speed, bolt->velocity);
    bolt->movetype = MOVETYPE_FLYMISSILE;
    bolt->clipmask = MASK_SHOT;
    bolt->solid = SOLID_BBOX;
    bolt->s.effects |= effect;
    VectorClear(bolt->mins);
    VectorClear(bolt->maxs);
    bolt->s.modelindex = gi.modelindex("models/objects/laser/tris.md2");
    bolt->s.sound = gi.soundindex("misc/lasfly.wav");
    bolt->owner = self;
    // Written as a branch rather than a ternary so genptr.py can see both
    // pointers; it only matches a bare identifier after the '='. Neither was
    // discoverable before, which is why g_ptrs.c had a hand-added entry for
    // blaster_touch and none at all for hyper_blaster_touch.
    if (hyper)
        bolt->touch = hyper_blaster_touch;
    else
        bolt->touch = blaster_touch;
    bolt->nextthink = level.framenum + 2 * BASE_FRAMERATE;
    bolt->think = G_FreeEdict;
    bolt->dmg = damage;
    bolt->classname = "bolt";
    if (hyper)
        bolt->spawnflags = 1;
    gi.linkentity(bolt);

    if (self->client)
        check_dodge(self, bolt->s.origin, dir, speed);

    tr = gi.trace(self->s.origin, NULL, NULL, bolt->s.origin, bolt, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(bolt->s.origin, -10, dir, bolt->s.origin);
        bolt->touch(bolt, tr.ent, NULL, NULL);
    }
}


/*
=================
fire_ionripper

Xatrix ion ripper blade. Ricochets off walls (MOVETYPE_WALLBOUNCE) until it hits
something damageable or its think expires. Used by monster_soldier_ripper.
=================
*/
void ionripper_sparks(edict_t *self)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WELDING_SPARKS);
    gi.WriteByte(0);
    gi.WritePosition(self->s.origin);
    gi.WriteDir(vec3_origin);
    gi.WriteByte(0xe4 + (Q_rand() & 3));
    gi.multicast(self->s.origin, MULTICAST_PVS);

    G_FreeEdict(self);
}

void ionripper_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    // no damage to give means this was a wall hit: keep bouncing rather than die
    if (!other->takedamage)
        return;

    T_Damage(other, self, self->owner, self->velocity, self->s.origin,
             plane ? plane->normal : vec3_origin, self->dmg, 1, DAMAGE_ENERGY, MOD_RIPPER);

    G_FreeEdict(self);
}

/*
=================
fire_blaster2

Fires a single green blaster bolt. Rogue's monsters use this; the player only
ever fires one from the defender sphere, which is not ported, so the owner is
always a monster here and MOD_BLASTER2 is the only means of death produced.

The bolt model is rogue's own `models/proj/laser2`. The rerelease instead
reskins the stock `models/objects/laser` with `s.skinnum = 2` plus `s.scale`,
neither of which this client has - the stock laser model carries a single skin.
=================
*/
void blaster2_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    int     damagestat;

    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner && self->owner->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    if (other->takedamage) {
        if (self->owner) {
            // don't splash the shooter with its own bolt
            damagestat = self->owner->takedamage;
            self->owner->takedamage = DAMAGE_NO;
            if (self->dmg >= 5)
                T_RadiusDamage(self, self->owner, self->dmg * 2, other, self->dmg_radius, MOD_UNKNOWN);
            T_Damage(other, self, self->owner, self->velocity, self->s.origin, plane ? plane->normal : vec3_origin,
                     self->dmg, 1, DAMAGE_ENERGY, MOD_BLASTER2);
            self->owner->takedamage = damagestat;
        } else {
            if (self->dmg >= 5)
                T_RadiusDamage(self, self->owner, self->dmg * 2, other, self->dmg_radius, MOD_UNKNOWN);
            T_Damage(other, self, self->owner, self->velocity, self->s.origin, plane ? plane->normal : vec3_origin,
                     self->dmg, 1, DAMAGE_ENERGY, MOD_BLASTER2);
        }
    } else {
        if (self->dmg >= 5)
            T_RadiusDamage(self, self->owner, self->dmg * 2, self->owner, self->dmg_radius, MOD_UNKNOWN);

        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_BLASTER2);
        gi.WritePosition(self->s.origin);
        if (!plane)
            gi.WriteDir(vec3_origin);
        else
            gi.WriteDir(plane->normal);
        gi.multicast(self->s.origin, MULTICAST_PVS);
    }

    G_FreeEdict(self);
}

void fire_blaster2(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int effect, bool hyper)
{
    edict_t *bolt;
    trace_t tr;

    VectorNormalize(dir);

    bolt = G_Spawn();
    bolt->svflags = SVF_DEADMONSTER;
    VectorCopy(start, bolt->s.origin);
    VectorCopy(start, bolt->s.old_origin);
    vectoangles(dir, bolt->s.angles);
    VectorScale(dir, speed, bolt->velocity);
    bolt->movetype = MOVETYPE_FLYMISSILE;
    bolt->clipmask = MASK_SHOT;
    bolt->solid = SOLID_BBOX;
    bolt->s.effects |= effect;
    if (effect)
        bolt->s.effects |= EF_TRACKER;
    VectorClear(bolt->mins);
    VectorClear(bolt->maxs);
    bolt->dmg_radius = 128;
    bolt->s.modelindex = gi.modelindex("models/proj/laser2/tris.md2");
    bolt->s.sound = gi.soundindex("misc/lasfly.wav");
    bolt->owner = self;
    bolt->touch = blaster2_touch;
    bolt->nextthink = level.framenum + 2 * BASE_FRAMERATE;
    bolt->think = G_FreeEdict;
    bolt->dmg = damage;
    bolt->classname = "bolt";
    gi.linkentity(bolt);

    if (self->client)
        check_dodge(self, bolt->s.origin, dir, speed);

    tr = gi.trace(self->s.origin, NULL, NULL, bolt->s.origin, bolt, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(bolt->s.origin, -10, dir, bolt->s.origin);
        bolt->touch(bolt, tr.ent, NULL, NULL);
    }
}

void fire_ionripper(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int effect)
{
    edict_t *ion;
    trace_t tr;

    VectorNormalize(dir);

    ion = G_Spawn();
    VectorCopy(start, ion->s.origin);
    VectorCopy(start, ion->s.old_origin);
    vectoangles(dir, ion->s.angles);
    VectorScale(dir, speed, ion->velocity);

    ion->movetype = MOVETYPE_WALLBOUNCE;
    ion->clipmask = MASK_SHOT;
    ion->solid = SOLID_BBOX;
    ion->s.effects |= effect;
    ion->s.renderfx |= RF_FULLBRIGHT;
    VectorClear(ion->mins);
    VectorClear(ion->maxs);
    ion->s.modelindex = gi.modelindex("models/objects/boomrang/tris.md2");
    ion->s.sound = gi.soundindex("misc/lasfly.wav");
    ion->owner = self;
    ion->touch = ionripper_touch;
    ion->nextthink = level.framenum + 3 * BASE_FRAMERATE;
    ion->think = ionripper_sparks;
    ion->dmg = damage;
    ion->dmg_radius = 100;
    ion->classname = "ionripper";
    gi.linkentity(ion);

    if (self->client)
        check_dodge(self, ion->s.origin, dir, speed);

    tr = gi.trace(self->s.origin, NULL, NULL, ion->s.origin, ion, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(ion->s.origin, -10, dir, ion->s.origin);
        ion->touch(ion, tr.ent, NULL, NULL);
    }
}


/*
=================
fire_blueblaster

The blue hyperblaster bolt fired by monster_soldier_hypergun. Same flight as a
blaster bolt, different model/effect, and it reuses blaster_touch - the damage
type it reports is decided by spawnflags there, so leave spawnflags clear.
=================
*/
void fire_blueblaster(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, int effect)
{
    edict_t *bolt;
    trace_t tr;

    VectorNormalize(dir);

    bolt = G_Spawn();
    bolt->svflags = SVF_DEADMONSTER;
    VectorCopy(start, bolt->s.origin);
    VectorCopy(start, bolt->s.old_origin);
    vectoangles(dir, bolt->s.angles);
    VectorScale(dir, speed, bolt->velocity);
    bolt->movetype = MOVETYPE_FLYMISSILE;
    bolt->clipmask = MASK_SHOT;
    bolt->solid = SOLID_BBOX;
    bolt->s.effects |= effect;
    VectorClear(bolt->mins);
    VectorClear(bolt->maxs);
    bolt->s.modelindex = gi.modelindex("models/objects/blaser/tris.md2");
    bolt->s.sound = gi.soundindex("misc/lasfly.wav");
    bolt->owner = self;
    bolt->touch = blaster_touch;
    bolt->nextthink = level.framenum + 2 * BASE_FRAMERATE;
    bolt->think = G_FreeEdict;
    bolt->dmg = damage;
    bolt->classname = "bolt";
    gi.linkentity(bolt);

    if (self->client)
        check_dodge(self, bolt->s.origin, dir, speed);

    tr = gi.trace(self->s.origin, NULL, NULL, bolt->s.origin, bolt, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(bolt->s.origin, -10, dir, bolt->s.origin);
        bolt->touch(bolt, tr.ent, NULL, NULL);
    }
}


/*
=================
plasma_touch

Impact handler for the xatrix plasma bolt fired by monster_gladb. Unlike a
blaster bolt this does splash damage as well as direct damage.
=================
*/
void plasma_touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t  origin;

    if (other == ent->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(ent);
        return;
    }

    if (ent->owner->client)
        PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

    // back the explosion out of whatever we hit so it is not buried in it
    VectorMA(ent->s.origin, -0.02f, ent->velocity, origin);

    if (other->takedamage)
        T_Damage(other, ent, ent->owner, ent->velocity, ent->s.origin,
                 plane ? plane->normal : vec3_origin, ent->dmg, 0, 0, MOD_PHALANX);

    T_RadiusDamage(ent, ent->owner, ent->radius_dmg, other, ent->dmg_radius, MOD_PHALANX);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_PLASMA_EXPLOSION);
    gi.WritePosition(origin);
    gi.multicast(ent->s.origin, MULTICAST_PVS);

    G_FreeEdict(ent);
}

/*
=================
fire_plasma

The plasma bolt fired by monster_gladb. The client already knows EF_PLASMA and
TE_PLASMA_EXPLOSION, so only the game side was missing.
=================
*/
void fire_plasma(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, float damage_radius, int radius_damage)
{
    edict_t *plasma;

    VectorNormalize(dir);

    plasma = G_Spawn();
    VectorCopy(start, plasma->s.origin);
    VectorCopy(dir, plasma->movedir);
    vectoangles(dir, plasma->s.angles);
    VectorScale(dir, speed, plasma->velocity);
    plasma->movetype = MOVETYPE_FLYMISSILE;
    plasma->clipmask = MASK_SHOT;
    plasma->solid = SOLID_BBOX;
    VectorClear(plasma->mins);
    VectorClear(plasma->maxs);
    plasma->s.modelindex = gi.modelindex("sprites/s_photon.sp2");
    plasma->s.effects |= EF_PLASMA | EF_ANIM_ALLFAST;
    plasma->s.sound = gi.soundindex("weapons/rockfly.wav");
    plasma->owner = self;
    plasma->touch = plasma_touch;
    // xatrix used level.time + 8000/speed; same lifetime expressed in frames
    plasma->nextthink = level.framenum + (int)(8000.0f / speed * BASE_FRAMERATE);
    plasma->think = G_FreeEdict;
    plasma->dmg = damage;
    plasma->radius_dmg = radius_damage;
    plasma->dmg_radius = damage_radius;
    plasma->classname = "plasma";
    gi.linkentity(plasma);

    if (self->client)
        check_dodge(self, plasma->s.origin, dir, speed);
}


/*
=================
fire_grenade
=================
*/
void Grenade_Explode(edict_t *ent)
{
    vec3_t      origin;
    int         mod;

    if (ent->owner->client)
        PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

    //FIXME: if we are onground then raise our Z just a bit since we are a point?
    if (ent->enemy) {
        float   points;
        vec3_t  v;
        vec3_t  dir;

        VectorAdd(ent->enemy->mins, ent->enemy->maxs, v);
        VectorMA(ent->enemy->s.origin, 0.5f, v, v);
        VectorSubtract(ent->s.origin, v, v);
        points = ent->dmg - 0.5f * VectorLength(v);
        VectorSubtract(ent->enemy->s.origin, ent->s.origin, dir);
        if (ent->spawnflags & 1)
            mod = MOD_HANDGRENADE;
        else
            mod = MOD_GRENADE;
        T_Damage(ent->enemy, ent, ent->owner, dir, ent->s.origin, vec3_origin, (int)points, (int)points, DAMAGE_RADIUS, mod);
    }

    if (ent->spawnflags & 2)
        mod = MOD_HELD_GRENADE;
    else if (ent->spawnflags & 1)
        mod = MOD_HG_SPLASH;
    else
        mod = MOD_G_SPLASH;
    T_RadiusDamage(ent, ent->owner, ent->dmg, ent->enemy, ent->dmg_radius, mod);

    VectorMA(ent->s.origin, -0.02f, ent->velocity, origin);
    gi.WriteByte(svc_temp_entity);
    if (ent->waterlevel) {
        if (ent->groundentity)
            gi.WriteByte(TE_GRENADE_EXPLOSION_WATER);
        else
            gi.WriteByte(TE_ROCKET_EXPLOSION_WATER);
    } else {
        if (ent->groundentity)
            gi.WriteByte(TE_GRENADE_EXPLOSION);
        else
            gi.WriteByte(TE_ROCKET_EXPLOSION);
    }
    gi.WritePosition(origin);
    gi.multicast(ent->s.origin, MULTICAST_PHS);

    G_FreeEdict(ent);
}

void Grenade_Touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == ent->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(ent);
        return;
    }

    if (!other->takedamage) {
        if (ent->spawnflags & 1) {
            if (random() > 0.5f)
                gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
            else
                gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
        } else {
            gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/grenlb1b.wav"), 1, ATTN_NORM, 0);
        }
        return;
    }

    ent->enemy = other;
    Grenade_Explode(ent);
}

void fire_grenade(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int speed, float timer, float damage_radius)
{
    edict_t *grenade;
    vec3_t  dir;
    vec3_t  forward, right, up;
    float   scale;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    grenade = G_Spawn();
    VectorCopy(start, grenade->s.origin);
    VectorScale(aimdir, speed, grenade->velocity);
    scale = 200 + crandom() * 10.0f;
    VectorMA(grenade->velocity, scale, up, grenade->velocity);
    scale = crandom() * 10.0f;
    VectorMA(grenade->velocity, scale, right, grenade->velocity);
    VectorSet(grenade->avelocity, 300, 300, 300);
    grenade->movetype = MOVETYPE_BOUNCE;
    grenade->clipmask = MASK_SHOT;
    grenade->solid = SOLID_BBOX;
    grenade->s.effects |= EF_GRENADE;
    VectorClear(grenade->mins);
    VectorClear(grenade->maxs);
    grenade->s.modelindex = gi.modelindex("models/objects/grenade/tris.md2");
    grenade->owner = self;
    grenade->touch = Grenade_Touch;
    grenade->nextthink = level.framenum + timer * BASE_FRAMERATE;
    grenade->think = Grenade_Explode;
    grenade->dmg = damage;
    grenade->dmg_radius = damage_radius;
    grenade->classname = "grenade";

    gi.linkentity(grenade);
}

void fire_grenade2(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int speed, float timer, float damage_radius, bool held)
{
    edict_t *grenade;
    vec3_t  dir;
    vec3_t  forward, right, up;
    float   scale;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    grenade = G_Spawn();
    VectorCopy(start, grenade->s.origin);
    VectorScale(aimdir, speed, grenade->velocity);
    scale = 200 + crandom() * 10.0f;
    VectorMA(grenade->velocity, scale, up, grenade->velocity);
    scale = crandom() * 10.0f;
    VectorMA(grenade->velocity, scale, right, grenade->velocity);
    VectorSet(grenade->avelocity, 300, 300, 300);
    grenade->movetype = MOVETYPE_BOUNCE;
    grenade->clipmask = MASK_SHOT;
    grenade->solid = SOLID_BBOX;
    grenade->s.effects |= EF_GRENADE;
    VectorClear(grenade->mins);
    VectorClear(grenade->maxs);
    grenade->s.modelindex = gi.modelindex("models/objects/grenade2/tris.md2");
    grenade->owner = self;
    grenade->touch = Grenade_Touch;
    grenade->nextthink = level.framenum + timer * BASE_FRAMERATE;
    grenade->think = Grenade_Explode;
    grenade->dmg = damage;
    grenade->dmg_radius = damage_radius;
    grenade->classname = "hgrenade";
    if (held)
        grenade->spawnflags = 3;
    else
        grenade->spawnflags = 1;
    grenade->s.sound = gi.soundindex("weapons/hgrenc1b.wav");

    if (timer <= 0.0f)
        Grenade_Explode(grenade);
    else {
        gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/hgrent1a.wav"), 1, ATTN_NORM, 0);
        gi.linkentity(grenade);
    }
}


/*
=================
fire_rocket
=================
*/
void rocket_touch(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t      origin;
    int         n;

    if (other == ent->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(ent);
        return;
    }

    if (ent->owner->client)
        PlayerNoise(ent->owner, ent->s.origin, PNOISE_IMPACT);

    // calculate position for the explosion entity
    VectorMA(ent->s.origin, -0.02f, ent->velocity, origin);

    if (other->takedamage) {
        T_Damage(other, ent, ent->owner, ent->velocity, ent->s.origin, plane->normal, ent->dmg, 0, 0, MOD_ROCKET);
    } else {
        // don't throw any debris in net games
        if (!deathmatch->value && !coop->value) {
            if ((surf) && !(surf->flags & (SURF_WARP | SURF_TRANS33 | SURF_TRANS66 | SURF_FLOWING))) {
                n = Q_rand() % 5;
                while (n--)
                    ThrowDebris(ent, "models/objects/debris2/tris.md2", 2, ent->s.origin);
            }
        }
    }

    T_RadiusDamage(ent, ent->owner, ent->radius_dmg, other, ent->dmg_radius, MOD_R_SPLASH);

    gi.WriteByte(svc_temp_entity);
    if (ent->waterlevel)
        gi.WriteByte(TE_ROCKET_EXPLOSION_WATER);
    else
        gi.WriteByte(TE_ROCKET_EXPLOSION);
    gi.WritePosition(origin);
    gi.multicast(ent->s.origin, MULTICAST_PHS);

    G_FreeEdict(ent);
}

void fire_rocket(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, float damage_radius, int radius_damage)
{
    edict_t *rocket;

    rocket = G_Spawn();
    VectorCopy(start, rocket->s.origin);
    VectorCopy(dir, rocket->movedir);
    vectoangles(dir, rocket->s.angles);
    VectorScale(dir, speed, rocket->velocity);
    rocket->movetype = MOVETYPE_FLYMISSILE;
    rocket->clipmask = MASK_SHOT;
    rocket->solid = SOLID_BBOX;
    rocket->s.effects |= EF_ROCKET;
    VectorClear(rocket->mins);
    VectorClear(rocket->maxs);
    rocket->s.modelindex = gi.modelindex("models/objects/rocket/tris.md2");
    rocket->owner = self;
    rocket->touch = rocket_touch;
    rocket->nextthink = level.framenum + BASE_FRAMERATE * 8000 / speed;
    rocket->think = G_FreeEdict;
    rocket->dmg = damage;
    rocket->radius_dmg = radius_damage;
    rocket->dmg_radius = damage_radius;
    rocket->s.sound = gi.soundindex("weapons/rockfly.wav");
    rocket->classname = "rocket";

    if (self->client)
        check_dodge(self, rocket->s.origin, dir, speed);

    gi.linkentity(rocket);
}


/*
=================
fire_rail
=================
*/
void fire_rail(edict_t *self, vec3_t start, vec3_t aimdir, int damage, int kick)
{
    vec3_t      from;
    vec3_t      end;
    trace_t     tr;
    edict_t     *ignore;
    int         mask;
    bool        water;
    float       lastfrac;

    VectorMA(start, 8192, aimdir, end);
    VectorCopy(start, from);
    ignore = self;
    water = false;
    mask = MASK_SHOT | CONTENTS_SLIME | CONTENTS_LAVA;
    lastfrac = 1;
    while (ignore) {
        tr = gi.trace(from, NULL, NULL, end, ignore, mask);

        if (tr.contents & (CONTENTS_SLIME | CONTENTS_LAVA)) {
            mask &= ~(CONTENTS_SLIME | CONTENTS_LAVA);
            water = true;
        } else {
            //ZOID--added so rail goes through SOLID_BBOX entities (gibs, etc)
            if (((tr.ent->svflags & SVF_MONSTER) || (tr.ent->client) ||
                (tr.ent->solid == SOLID_BBOX)) && (lastfrac + tr.fraction > 0))
                ignore = tr.ent;
            else
                ignore = NULL;

            if ((tr.ent != self) && (tr.ent->takedamage))
                T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal, damage, kick, 0, MOD_RAILGUN);
        }

        VectorCopy(tr.endpos, from);
        lastfrac = tr.fraction;
    }

    // send gun puff / flash
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_RAILTRAIL);
    gi.WritePosition(start);
    gi.WritePosition(tr.endpos);
    gi.multicast(self->s.origin, MULTICAST_PHS);
//  gi.multicast (start, MULTICAST_PHS);
    if (water) {
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_RAILTRAIL);
        gi.WritePosition(start);
        gi.WritePosition(tr.endpos);
        gi.multicast(tr.endpos, MULTICAST_PHS);
    }

    if (self->client)
        PlayerNoise(self, tr.endpos, PNOISE_IMPACT);
}


/*
=================
fire_bfg
=================
*/
void bfg_explode(edict_t *self)
{
    edict_t *ent;
    float   points;
    vec3_t  v;
    float   dist;

    if (self->s.frame == 0) {
        // the BFG effect
        ent = NULL;
        while ((ent = findradius(ent, self->s.origin, self->dmg_radius)) != NULL) {
            if (!ent->takedamage)
                continue;
            if (ent == self->owner)
                continue;
            if (!CanDamage(ent, self))
                continue;
            if (!CanDamage(ent, self->owner))
                continue;

            VectorAdd(ent->mins, ent->maxs, v);
            VectorMA(ent->s.origin, 0.5f, v, v);
            VectorSubtract(self->s.origin, v, v);
            dist = VectorLength(v);
            points = self->radius_dmg * (1.0f - sqrtf(dist / self->dmg_radius));
            if (ent == self->owner)
                points = points * 0.5f;

            gi.WriteByte(svc_temp_entity);
            gi.WriteByte(TE_BFG_EXPLOSION);
            gi.WritePosition(ent->s.origin);
            gi.multicast(ent->s.origin, MULTICAST_PHS);
            T_Damage(ent, self, self->owner, self->velocity, ent->s.origin, vec3_origin, (int)points, 0, DAMAGE_ENERGY, MOD_BFG_EFFECT);
        }
    }

    self->nextthink = level.framenum + 1;
    self->s.frame++;
    if (self->s.frame == 5)
        self->think = G_FreeEdict;
}

void bfg_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    // core explosion - prevents firing it into the wall/floor
    if (other->takedamage)
        T_Damage(other, self, self->owner, self->velocity, self->s.origin, plane->normal, 200, 0, 0, MOD_BFG_BLAST);
    T_RadiusDamage(self, self->owner, 200, other, 100, MOD_BFG_BLAST);

    gi.sound(self, CHAN_VOICE, gi.soundindex("weapons/bfg__x1b.wav"), 1, ATTN_NORM, 0);
    self->solid = SOLID_NOT;
    self->touch = NULL;
    VectorMA(self->s.origin, -1 * FRAMETIME, self->velocity, self->s.origin);
    VectorClear(self->velocity);
    self->s.modelindex = gi.modelindex("sprites/s_bfg3.sp2");
    self->s.frame = 0;
    self->s.sound = 0;
    self->s.effects &= ~EF_ANIM_ALLFAST;
    self->think = bfg_explode;
    self->nextthink = level.framenum + 1;
    self->enemy = other;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_BFG_BIGEXPLOSION);
    gi.WritePosition(self->s.origin);
    gi.multicast(self->s.origin, MULTICAST_PVS);
}


void bfg_think(edict_t *self)
{
    edict_t *ent;
    edict_t *ignore;
    vec3_t  point;
    vec3_t  dir;
    vec3_t  start;
    vec3_t  end;
    int     dmg;
    trace_t tr;

    if (deathmatch->value)
        dmg = 5;
    else
        dmg = 10;

    ent = NULL;
    while ((ent = findradius(ent, self->s.origin, 256)) != NULL) {
        if (ent == self)
            continue;

        if (ent == self->owner)
            continue;

        if (!ent->takedamage)
            continue;

        if (!(ent->svflags & SVF_MONSTER) && (!ent->client) && (strcmp(ent->classname, "misc_explobox") != 0))
            continue;

        VectorMA(ent->absmin, 0.5f, ent->size, point);

        VectorSubtract(point, self->s.origin, dir);
        VectorNormalize(dir);

        ignore = self;
        VectorCopy(self->s.origin, start);
        VectorMA(start, 2048, dir, end);
        while (1) {
            tr = gi.trace(start, NULL, NULL, end, ignore, CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_DEADMONSTER);

            if (!tr.ent)
                break;

            // hurt it if we can
            if ((tr.ent->takedamage) && !(tr.ent->flags & FL_IMMUNE_LASER) && (tr.ent != self->owner))
                T_Damage(tr.ent, self, self->owner, dir, tr.endpos, vec3_origin, dmg, 1, DAMAGE_ENERGY, MOD_BFG_LASER);

            // if we hit something that's not a monster or player we're done
            if (!(tr.ent->svflags & SVF_MONSTER) && (!tr.ent->client)) {
                gi.WriteByte(svc_temp_entity);
                gi.WriteByte(TE_LASER_SPARKS);
                gi.WriteByte(4);
                gi.WritePosition(tr.endpos);
                gi.WriteDir(tr.plane.normal);
                gi.WriteByte(self->s.skinnum);
                gi.multicast(tr.endpos, MULTICAST_PVS);
                break;
            }

            ignore = tr.ent;
            VectorCopy(tr.endpos, start);
        }

        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_BFG_LASER);
        gi.WritePosition(self->s.origin);
        gi.WritePosition(tr.endpos);
        gi.multicast(self->s.origin, MULTICAST_PHS);
    }

    self->nextthink = level.framenum + 1;
}


void fire_bfg(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, float damage_radius)
{
    edict_t *bfg;

    bfg = G_Spawn();
    VectorCopy(start, bfg->s.origin);
    VectorCopy(dir, bfg->movedir);
    vectoangles(dir, bfg->s.angles);
    VectorScale(dir, speed, bfg->velocity);
    bfg->movetype = MOVETYPE_FLYMISSILE;
    bfg->clipmask = MASK_SHOT;
    bfg->solid = SOLID_BBOX;
    bfg->s.effects |= EF_BFG | EF_ANIM_ALLFAST;
    VectorClear(bfg->mins);
    VectorClear(bfg->maxs);
    bfg->s.modelindex = gi.modelindex("sprites/s_bfg1.sp2");
    bfg->owner = self;
    bfg->touch = bfg_touch;
    bfg->nextthink = level.framenum + BASE_FRAMERATE * 8000 / speed;
    bfg->think = G_FreeEdict;
    bfg->radius_dmg = damage;
    bfg->dmg_radius = damage_radius;
    bfg->classname = "bfg blast";
    bfg->s.sound = gi.soundindex("weapons/bfg__l1a.wav");

    bfg->think = bfg_think;
    bfg->nextthink = level.framenum + 1;
    bfg->teammaster = bfg;
    bfg->teamchain = NULL;

    if (self->client)
        check_dodge(self, bfg->s.origin, dir, speed);

    gi.linkentity(bfg);
}

/*
 * Drops a spark from the flare flying thru the air.  Checks to make
 * sure we aren't in the water.
 */
void flare_sparks(edict_t *self)
{
	vec3_t dir;
	vec3_t forward, right, up;
	// Spawn some sparks.  This isn't net-friendly at all, but will 
	// be fine for single player. 
	// 
	gi.WriteByte(svc_temp_entity);
	gi.WriteByte(TE_FLARE);

    gi.WriteShort((int)(self - g_edicts));
    // if this is the first tick of flare, set count to 1 to start the sound
    gi.WriteByte( self->timestamp - level.framenum < (int)(14.75f * BASE_FRAMERATE) ? 0 : 1);

    gi.WritePosition(self->s.origin);

	// If we are still moving, calculate the normal to the direction 
	 // we are travelling. 
	 // 
	if (VectorLength(self->velocity) > 0.0)
	{
		vectoangles(self->velocity, dir);
		AngleVectors(dir, forward, right, up);

		gi.WriteDir(up);
	}
	// If we're stopped, just write out the origin as our normal 
	// 
	else
	{
		gi.WriteDir(vec3_origin);
	}
	gi.multicast(self->s.origin, MULTICAST_PVS);
}

/*
   void flare_think( edict_t *self )

   Purpose: The think function of a flare round.  It generates sparks
			on the flare using a temp entity, and kills itself after
			self->timestamp runs out.
   Parameters:
	 self: A pointer to the edict_t structure representing the
		   flare round.  self->timestamp is the value used to
		   measure the lifespan of the round, and is set in
		   fire_flaregun blow.

   Notes:
	 - I'm not sure how much bandwidth is eaten by spawning a temp
	   entity every FRAMETIME seconds.  It might very well turn out
	   that the sparks need to go bye-bye in favor of less bandwidth
	   usage.  Then again, why the hell would you use this gun on
	   a DM server????

	 - I haven't seen self->timestamp used anywhere else in the code,
	   but I never really looked that hard.  It doesn't seem to cause
	   any problems, and is aptly named, so I used it.
 */
void flare_think(edict_t *self)
{
	// self->timestamp is 15 seconds after the flare was spawned. 
	// 
	if (level.framenum > self->timestamp)
	{
		G_FreeEdict(self);
		return;
	}

	// We're still active, so lets shoot some sparks. 
	// 
	flare_sparks(self);
	
	// We'll think again in .2 seconds 
	// 
	self->nextthink = level.framenum + (int)(.2f * BASE_FRAMERATE);
}

void flare_touch(edict_t *ent, edict_t *other,
	cplane_t *plane, csurface_t *surf)
{
	// Flares don't weigh that much, so let's have them stop 
	// the instant they whack into anything. 
	// 
	VectorClear(ent->velocity);
}

void fire_flaregun(edict_t *self, vec3_t start, vec3_t aimdir,
	int damage, int speed, float timer,
	float damage_radius)
{
	edict_t *flare;
	vec3_t dir;
	vec3_t forward, right, up;

	vectoangles(aimdir, dir);
	AngleVectors(dir, forward, right, up);

	flare = G_Spawn();
	VectorCopy(start, flare->s.origin);
	VectorScale(aimdir, speed, flare->velocity);
	VectorSet(flare->avelocity, 300, 300, 300);
	flare->movetype = MOVETYPE_BOUNCE;
	flare->clipmask = MASK_SHOT;
	flare->solid = SOLID_BBOX;

	const float size = 4;
	VectorSet(flare->mins, -size, -size, -size);
	VectorSet(flare->maxs, size, size, size);

	flare->s.modelindex = gi.modelindex("models/objects/flare/tris.md2");
	flare->owner = self;
	flare->touch = flare_touch;
	flare->nextthink = level.framenum + (int)(.2f * BASE_FRAMERATE);
	flare->think = flare_think;
	flare->radius_dmg = damage;
	flare->dmg_radius = damage_radius;
	flare->classname = "flare";
	flare->timestamp = level.framenum + (int)(60.f * BASE_FRAMERATE); //live for 60 seconds 
	gi.linkentity(flare);
}

/*
=================
fire_player_melee

Rogue's generic player melee trace, used here by the chainfist. `quiet`
suppresses the swing/hit/tink sounds - the chainfist always passes 1, which is
just as well: weapons/swish.wav, meatht.wav and tink1.wav are not shipped in
this install, so those branches are deliberately unreachable.
=================
*/
void fire_player_melee(edict_t *self, vec3_t start, vec3_t aim, int reach,
                       int damage, int kick, int quiet, int mod)
{
    vec3_t      forward, right, up;
    vec3_t      v;
    vec3_t      point;
    trace_t     tr;

    vectoangles(aim, v);
    AngleVectors(v, forward, right, up);
    VectorNormalize(forward);
    VectorMA(start, reach, forward, point);

    // see if the hit connects
    tr = gi.trace(start, NULL, NULL, point, self, MASK_SHOT);

    if (tr.fraction == 1.0f) {
        if (!quiet)
            gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/swish.wav"), 1, ATTN_NORM, 0);
        return;
    }

    if (tr.ent->takedamage == DAMAGE_YES || tr.ent->takedamage == DAMAGE_AIM) {
        // pull the player forward if you do damage
        VectorMA(self->velocity, 75, forward, self->velocity);
        VectorMA(self->velocity, 75, up, self->velocity);

        if (mod == MOD_CHAINFIST)
            T_Damage(tr.ent, self, self, vec3_origin, tr.ent->s.origin, vec3_origin,
                     damage, kick / 2, DAMAGE_DESTROY_ARMOR | DAMAGE_NO_KNOCKBACK, mod);
        else
            T_Damage(tr.ent, self, self, vec3_origin, tr.ent->s.origin, vec3_origin,
                     damage, kick / 2, DAMAGE_NO_KNOCKBACK, mod);

        if (!quiet)
            gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/meatht.wav"), 1, ATTN_NORM, 0);
    } else {
        if (!quiet)
            gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/tink1.wav"), 1, ATTN_NORM, 0);

        VectorScale(tr.plane.normal, 256, point);
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_GUNSHOT);
        gi.WritePosition(tr.endpos);
        gi.WriteDir(point);
        gi.multicast(tr.endpos, MULTICAST_PVS);
    }
}

/*
=================
fire_beams / fire_heatbeam

The plasma beam (rogue's "heatbeam"). A hitscan beam re-traced through water,
with the visible beam sent as a temp entity every frame it fires. The client
already parses and draws TE_HEATBEAM, TE_HEATBEAM_SPARKS and TE_HEATBEAM_STEAM,
so nothing was needed on that side.
=================
*/
static void fire_beams(edict_t *self, vec3_t start, vec3_t aimdir, vec3_t offset,
                       int damage, int kick, int te_beam, int te_impact, int mod)
{
    trace_t     tr;
    vec3_t      dir;
    vec3_t      forward, right, up;
    vec3_t      end;
    vec3_t      water_start, endpoint;
    bool        water = false, underwater = false;
    int         content_mask = MASK_SHOT | MASK_WATER;
    vec3_t      beam_endpt;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    VectorMA(start, 8192, forward, end);

    if (gi.pointcontents(start) & MASK_WATER) {
        underwater = true;
        VectorCopy(start, water_start);
        content_mask &= ~MASK_WATER;
    }

    tr = gi.trace(start, NULL, NULL, end, self, content_mask);

    // see if we hit water
    if (tr.contents & MASK_WATER) {
        water = true;
        VectorCopy(tr.endpos, water_start);

        if (!VectorCompare(start, tr.endpos)) {
            gi.WriteByte(svc_temp_entity);
            gi.WriteByte(te_impact);
            gi.WritePosition(water_start);
            gi.WriteDir(tr.plane.normal);
            gi.multicast(tr.endpos, MULTICAST_PVS);
        }

        // re-trace ignoring water this time
        tr = gi.trace(water_start, NULL, NULL, end, self, MASK_SHOT);
    }

    VectorCopy(tr.endpos, endpoint);

    // halve the damage if the target is underwater
    if (water)
        damage = damage / 2;

    if (!(tr.surface && (tr.surface->flags & SURF_SKY))) {
        if (tr.fraction < 1.0f) {
            if (tr.ent->takedamage) {
                T_Damage(tr.ent, self, self, aimdir, tr.endpos, tr.plane.normal,
                         damage, kick, DAMAGE_ENERGY, mod);
            } else if (!water && strncmp(tr.surface->name, "sky", 3)) {
                gi.WriteByte(svc_temp_entity);
                gi.WriteByte(TE_HEATBEAM_STEAM);
                gi.WritePosition(tr.endpos);
                gi.WriteDir(tr.plane.normal);
                gi.multicast(tr.endpos, MULTICAST_PVS);

                if (self->client)
                    PlayerNoise(self, tr.endpos, PNOISE_IMPACT);
            }
        }
    }

    // if it went through water, find the end and make a bubble trail
    if (water || underwater) {
        vec3_t  pos;

        VectorSubtract(tr.endpos, water_start, dir);
        VectorNormalize(dir);
        VectorMA(tr.endpos, -2, dir, pos);

        if (gi.pointcontents(pos) & MASK_WATER)
            VectorCopy(pos, tr.endpos);
        else
            tr = gi.trace(pos, NULL, NULL, water_start, tr.ent, MASK_WATER);

        VectorAdd(water_start, tr.endpos, pos);
        VectorScale(pos, 0.5f, pos);

        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_BUBBLETRAIL2);
        gi.WritePosition(water_start);
        gi.WritePosition(tr.endpos);
        gi.multicast(pos, MULTICAST_PVS);
    }

    if (!underwater && !water)
        VectorCopy(tr.endpos, beam_endpt);
    else
        VectorCopy(endpoint, beam_endpt);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(te_beam);
    gi.WriteShort(self - g_edicts);
    gi.WritePosition(start);
    gi.WritePosition(beam_endpt);
    gi.multicast(self->s.origin, MULTICAST_ALL);
}

void fire_heatbeam(edict_t *self, vec3_t start, vec3_t aimdir, vec3_t offset,
                   int damage, int kick, bool monster)
{
    if (monster)
        fire_beams(self, start, aimdir, offset, damage, kick,
                   TE_MONSTER_HEATBEAM, TE_HEATBEAM_SPARKS, MOD_HEATBEAM);
    else
        fire_beams(self, start, aimdir, offset, damage, kick,
                   TE_HEATBEAM, TE_HEATBEAM_SPARKS, MOD_HEATBEAM);
}


/*
======================================================================

HEAT-SEEKING ROCKET (xatrix / rerelease)

What monster_chick_heat and monster_boss5 fire. Not to be confused with
fire_heatbeam above, which is rogue's plasma beam - this tree used to call
that one "fire_heat", which is the opposite of the rerelease's naming.

The rocket re-aims at the nearest visible player every frame, slewing
turn_fraction of the way there. The rerelease slerps rather than lerping, so
the turn rate stays constant no matter how far off target it starts.

======================================================================
*/

/*
=================
VectorSlerp

Spherical interpolation from -> to by frac, both unit length. Falls back to a
plain lerp when the two are almost parallel, where the sine denominator goes to
zero and the two agree anyway.
=================
*/
static void VectorSlerp(vec3_t from, vec3_t to, float frac, vec3_t out)
{
    float   dot, theta, sin_theta, a, b;
    vec3_t  scaled;
    int     i;

    dot = DotProduct(from, to);
    clamp(dot, -1.0f, 1.0f);

    theta = acosf(dot) * frac;
    sin_theta = sinf(acosf(dot));

    if (sin_theta < 0.001f) {
        for (i = 0; i < 3; i++)
            out[i] = from[i] + (to[i] - from[i]) * frac;
        VectorNormalize(out);
        return;
    }

    a = sinf(acosf(dot) - theta) / sin_theta;
    b = sinf(theta) / sin_theta;

    VectorScale(from, a, out);
    VectorScale(to, b, scaled);
    VectorAdd(out, scaled, out);
    VectorNormalize(out);
}

void heat_think(edict_t *self)
{
    edict_t *target = NULL;
    edict_t *acquire = NULL;
    vec3_t  fwd, vec, dir;
    float   len, oldlen = 0;
    float   dot, olddot = 1;

    AngleVectors(self->s.angles, fwd, NULL, NULL);

    while ((target = findradius(target, self->s.origin, 1024)) != NULL) {
        if (self->owner == target)
            continue;
        if (!target->client)
            continue;
        if (target->health <= 0)
            continue;
        if (!visible(self, target))
            continue;

        VectorSubtract(self->s.origin, target->s.origin, vec);
        len = VectorNormalize(vec);
        dot = DotProduct(vec, fwd);

        // a target that needs less turning wins
        if (dot >= olddot)
            continue;

        if (!acquire || dot < olddot || len < oldlen) {
            acquire = target;
            oldlen = len;
            olddot = dot;
        }
    }

    if (acquire) {
        VectorSubtract(acquire->s.origin, self->s.origin, dir);
        VectorNormalize(dir);

        // if the target is off to the side rather than ahead or behind, the
        // rerelease flips the target direction so the rocket arcs around
        // instead of stalling in a turn it cannot make
        if (DotProduct(self->movedir, dir) < 0.45f && DotProduct(self->movedir, dir) > -0.45f)
            VectorNegate(dir, dir);

        VectorSlerp(self->movedir, dir, self->accel, self->movedir);
        vectoangles(self->movedir, self->s.angles);

        if (!self->enemy) {
            gi.sound(self, CHAN_WEAPON, gi.soundindex("weapons/railgr1a.wav"), 1, ATTN_STATIC, 0);
            self->enemy = acquire;
        }
    } else {
        self->enemy = NULL;
    }

    VectorScale(self->movedir, self->speed, self->velocity);
    self->nextthink = level.framenum + 1;
}

void fire_heat(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed,
               float damage_radius, int radius_damage, float turn_fraction)
{
    edict_t *heat;

    heat = G_Spawn();
    VectorCopy(start, heat->s.origin);
    VectorCopy(dir, heat->movedir);
    vectoangles(dir, heat->s.angles);
    VectorScale(dir, speed, heat->velocity);
    heat->movetype = MOVETYPE_FLYMISSILE;
    heat->clipmask = MASK_SHOT;
    heat->solid = SOLID_BBOX;
    heat->s.effects |= EF_ROCKET;
    VectorClear(heat->mins);
    VectorClear(heat->maxs);
    heat->s.modelindex = gi.modelindex("models/objects/rocket/tris.md2");
    heat->owner = self;
    heat->touch = rocket_touch;
    heat->speed = speed;
    heat->accel = turn_fraction;
    heat->classname = "rocket";

    heat->nextthink = level.framenum + 1;
    heat->think = heat_think;

    heat->dmg = damage;
    heat->radius_dmg = radius_damage;
    heat->dmg_radius = damage_radius;
    heat->s.sound = gi.soundindex("weapons/rockfly.wav");

    gi.linkentity(heat);
}

/*
======================================================================

TESLA MINE (rogue)

The "land mines" the MGU maps scatter around - 18 of them in scope, 5 in mgu5m1
alone. Thrown like a grenade, it bounces, unfolds over ~1.5s, then spawns a
SOLID_TRIGGER child covering its zap radius and electrocutes anything damageable
that it can see, until its life runs out or it is shot.

All timing is converted from rogue's float level.time to this tree's integer
level.framenum. Note the child trigger is held on teamchain, so tesla_remove
must free it - a tesla that dies without one is a bug and says so.

======================================================================
*/

#define TESLA_TIME_TO_LIVE          30
#define TESLA_DAMAGE_RADIUS         128
#define TESLA_DAMAGE                3
#define TESLA_KNOCKBACK             8
#define TESLA_ACTIVATE_TIME         3
#define TESLA_EXPLOSION_DAMAGE_MULT 50
#define TESLA_EXPLOSION_RADIUS      200

void tesla_remove(edict_t *self)
{
    edict_t *cur, *next;

    self->takedamage = DAMAGE_NO;

    if (self->teamchain) {
        cur = self->teamchain;
        while (cur) {
            next = cur->teamchain;
            G_FreeEdict(cur);
            cur = next;
        }
    } else if (self->air_finished_framenum) {
        gi.dprintf("tesla without a field!\n");
    }

    self->owner = self->teammaster;     // going away, set the owner correctly
    self->enemy = NULL;

    // play the quad sound if quadded and it is an underwater explosion
    if (self->dmg_radius && self->dmg > TESLA_DAMAGE * TESLA_EXPLOSION_DAMAGE_MULT)
        gi.sound(self, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

    Grenade_Explode(self);
}

void tesla_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    tesla_remove(self);
}

static void tesla_blow(edict_t *self)
{
    self->dmg = self->dmg * TESLA_EXPLOSION_DAMAGE_MULT;
    self->dmg_radius = TESLA_EXPLOSION_RADIUS;
    tesla_remove(self);
}

// the trigger exists only so BoxEdicts has something to size the field from;
// all the damage is done in tesla_think_active
void tesla_zap(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
}

void tesla_think_active(edict_t *self)
{
    int         i, num;
    edict_t     *touch[MAX_EDICTS], *hit;
    vec3_t      dir, start;
    trace_t     tr;

    if (level.framenum > self->air_finished_framenum) {
        tesla_remove(self);
        return;
    }

    VectorCopy(self->s.origin, start);
    start[2] += 16;

    num = gi.BoxEdicts(self->teamchain->absmin, self->teamchain->absmax,
                       touch, MAX_EDICTS, AREA_SOLID);

    for (i = 0; i < num; i++) {
        // if the tesla died while zapping things, stop zapping
        if (!self->inuse)
            break;

        hit = touch[i];
        if (!hit->inuse)
            continue;
        if (hit == self)
            continue;
        if (hit->health < 1)
            continue;

        // don't hit clients in single-player or coop
        if (hit->client && (coop->value || !deathmatch->value))
            continue;

        if (!(hit->svflags & (SVF_MONSTER | SVF_DAMAGEABLE)) && !hit->client)
            continue;

        tr = gi.trace(start, vec3_origin, vec3_origin, hit->s.origin, self, MASK_SHOT);

        if (tr.fraction == 1 || tr.ent == hit) {
            VectorSubtract(hit->s.origin, start, dir);

            // play the quad sound if it is above the "normal" damage
            if (self->dmg > TESLA_DAMAGE)
                gi.sound(self, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

            // don't do knockback to walking monsters
            if ((hit->svflags & SVF_MONSTER) && !(hit->flags & (FL_FLY | FL_SWIM)))
                T_Damage(hit, self, self->teammaster, dir, tr.endpos, tr.plane.normal,
                         self->dmg, 0, 0, MOD_TESLA);
            else
                T_Damage(hit, self, self->teammaster, dir, tr.endpos, tr.plane.normal,
                         self->dmg, TESLA_KNOCKBACK, 0, MOD_TESLA);

            gi.WriteByte(svc_temp_entity);
            gi.WriteByte(TE_LIGHTNING);
            gi.WriteShort(hit - g_edicts);      // destination entity
            gi.WriteShort(self - g_edicts);     // source entity
            gi.WritePosition(tr.endpos);
            gi.WritePosition(start);
            gi.multicast(start, MULTICAST_PVS);
        }
    }

    if (self->inuse) {
        self->think = tesla_think_active;
        self->nextthink = level.framenum + 1;
    }
}

void tesla_activate(edict_t *self)
{
    edict_t *trigger;
    edict_t *search;

    if (gi.pointcontents(self->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA | CONTENTS_WATER)) {
        tesla_blow(self);
        return;
    }

    // only check for spawn points in deathmatch
    if (deathmatch->value) {
        search = NULL;
        while ((search = findradius(search, self->s.origin, 1.5f * TESLA_DAMAGE_RADIUS)) != NULL) {
            if (search->classname &&
                (!strcmp(search->classname, "info_player_deathmatch") ||
                 !strcmp(search->classname, "info_player_start") ||
                 !strcmp(search->classname, "info_player_coop") ||
                 !strcmp(search->classname, "misc_teleporter_dest")) &&
                visible(search, self)) {
                tesla_remove(self);
                return;
            }
        }
    }

    trigger = G_Spawn();
    VectorCopy(self->s.origin, trigger->s.origin);
    VectorSet(trigger->mins, -TESLA_DAMAGE_RADIUS, -TESLA_DAMAGE_RADIUS, self->mins[2]);
    VectorSet(trigger->maxs, TESLA_DAMAGE_RADIUS, TESLA_DAMAGE_RADIUS, TESLA_DAMAGE_RADIUS);
    trigger->movetype = MOVETYPE_NONE;
    trigger->solid = SOLID_TRIGGER;
    trigger->owner = self;
    trigger->touch = tesla_zap;
    trigger->classname = "tesla trigger";

    // does not need to be a teamslave: the bounce move code looks for teamchains
    gi.linkentity(trigger);

    VectorClear(self->s.angles);

    // clear the owner in deathmatch so it can zap the thrower too
    if (deathmatch->value)
        self->owner = NULL;

    self->teamchain = trigger;
    self->think = tesla_think_active;
    self->nextthink = level.framenum + 1;
    self->air_finished_framenum = level.framenum + TESLA_TIME_TO_LIVE * BASE_FRAMERATE;
}

void tesla_think(edict_t *ent)
{
    if (gi.pointcontents(ent->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
        tesla_remove(ent);
        return;
    }

    VectorClear(ent->s.angles);

    if (!ent->s.frame)
        gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/teslaopen.wav"), 1, ATTN_NORM, 0);

    ent->s.frame++;

    if (ent->s.frame > 14) {
        ent->s.frame = 14;
        ent->think = tesla_activate;
        ent->nextthink = level.framenum + 1;
    } else {
        // the unfolding animation swaps skins as the legs come out
        if (ent->s.frame > 9) {
            if (ent->s.frame == 10) {
                if (ent->owner && ent->owner->client)
                    PlayerNoise(ent->owner, ent->s.origin, PNOISE_WEAPON);
                ent->s.skinnum = 1;
            } else if (ent->s.frame == 12) {
                ent->s.skinnum = 2;
            } else if (ent->s.frame == 14) {
                ent->s.skinnum = 3;
            }
        }

        ent->think = tesla_think;
        ent->nextthink = level.framenum + 1;
    }
}

void tesla_lava(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    vec3_t  land_point;

    if (!plane)
        return;

    VectorMA(ent->s.origin, -20.0f, plane->normal, land_point);

    if (gi.pointcontents(land_point) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
        tesla_blow(ent);
        return;
    }

    if (random() > 0.5f)
        gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
    else
        gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
}

void fire_tesla(edict_t *self, vec3_t start, vec3_t aimdir, int damage_mult, int speed)
{
    edict_t *tesla;
    vec3_t  dir;
    vec3_t  forward, right, up;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    tesla = G_Spawn();
    VectorCopy(start, tesla->s.origin);
    VectorScale(aimdir, speed, tesla->velocity);
    VectorMA(tesla->velocity, 200 + crandom() * 10.0f, up, tesla->velocity);
    VectorMA(tesla->velocity, crandom() * 10.0f, right, tesla->velocity);
    VectorClear(tesla->s.angles);
    tesla->movetype = MOVETYPE_BOUNCE;
    tesla->solid = SOLID_BBOX;
    tesla->s.effects |= EF_GRENADE;
    tesla->s.renderfx |= RF_IR_VISIBLE;
    VectorSet(tesla->mins, -12, -12, 0);
    VectorSet(tesla->maxs, 12, 12, 20);
    tesla->s.modelindex = gi.modelindex("models/weapons/g_tesla/tris.md2");

    tesla->owner = self;
    tesla->teammaster = self;

    tesla->think = tesla_think;
    tesla->nextthink = level.framenum + TESLA_ACTIVATE_TIME * BASE_FRAMERATE;

    // blow up on contact with lava or slime
    tesla->touch = tesla_lava;

    if (deathmatch->value)
        tesla->health = 20;
    else
        tesla->health = 30;

    tesla->takedamage = DAMAGE_YES;
    tesla->die = tesla_die;
    tesla->dmg = TESLA_DAMAGE * damage_mult;
    tesla->classname = "tesla";
    tesla->svflags |= SVF_DAMAGEABLE;
    tesla->clipmask = MASK_SHOT | CONTENTS_SLIME | CONTENTS_LAVA;

    gi.linkentity(tesla);
}

/*
======================================================================

DISRUPTOR / TRACKER (rogue)

The disintegrator fires a homing bolt that latches onto whatever the player was
aiming at and then keeps hurting it over half a second via a separate "pain
daemon" entity, rather than doing its damage in one hit. That is why it needs an
enemy at fire time - Weapon_Disintegrator traces for one first.

EF_TRACKER, EF_TRACKERTRAIL and TE_TRACKER_EXPLOSION already exist in shared.h
and are all drawn by the client, so nothing was needed on that side.

======================================================================
*/

#define TRACKER_DAMAGE_FLAGS    (DAMAGE_NO_POWER_ARMOR | DAMAGE_ENERGY | DAMAGE_NO_KNOCKBACK)
#define TRACKER_IMPACT_FLAGS    (DAMAGE_NO_POWER_ARMOR | DAMAGE_ENERGY)
#define TRACKER_DAMAGE_TIME     0.5f

void tracker_pain_daemon_think(edict_t *self)
{
    static const vec3_t pain_normal = { 0, 0, 1 };
    int     hurt;

    if (!self->inuse)
        return;

    if (level.framenum - self->timestamp > TRACKER_DAMAGE_TIME * BASE_FRAMERATE) {
        if (!self->enemy->client)
            self->enemy->s.effects &= ~EF_TRACKERTRAIL;
        G_FreeEdict(self);
        return;
    }

    if (self->enemy->health > 0) {
        T_Damage(self->enemy, self, self->owner, vec3_origin, self->enemy->s.origin,
                 pain_normal, self->dmg, 0, TRACKER_DAMAGE_FLAGS, MOD_TRACKER);

        // if we killed the player we will have been removed with them
        if (self->inuse) {
            // if we killed a monster, gib them
            if (self->enemy->health < 1) {
                if (self->enemy->gib_health)
                    hurt = -self->enemy->gib_health;
                else
                    hurt = 500;

                T_Damage(self->enemy, self, self->owner, vec3_origin, self->enemy->s.origin,
                         pain_normal, hurt, 0, TRACKER_DAMAGE_FLAGS, MOD_TRACKER);
            }

            if (self->enemy->client)
                self->enemy->client->tracker_pain_framenum = level.framenum + 1;
            else
                self->enemy->s.effects |= EF_TRACKERTRAIL;

            self->nextthink = level.framenum + 1;
        }
    } else {
        if (!self->enemy->client)
            self->enemy->s.effects &= ~EF_TRACKERTRAIL;
        G_FreeEdict(self);
    }
}

static void tracker_pain_daemon_spawn(edict_t *owner, edict_t *enemy, int damage)
{
    edict_t *daemon;

    if (!owner || !enemy)
        return;

    daemon = G_Spawn();
    daemon->classname = "pain daemon";
    daemon->think = tracker_pain_daemon_think;
    daemon->nextthink = level.framenum + 1;
    daemon->timestamp = level.framenum;
    daemon->owner = owner;
    daemon->enemy = enemy;
    daemon->dmg = damage;
}

static void tracker_explode(edict_t *self, cplane_t *plane)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_TRACKER_EXPLOSION);
    gi.WritePosition(self->s.origin);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    G_FreeEdict(self);
}

void tracker_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    float   damagetime;

    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->client)
        PlayerNoise(self->owner, self->s.origin, PNOISE_IMPACT);

    if (other->takedamage) {
        if ((other->svflags & SVF_MONSTER) || other->client) {
            if (other->health > 0) {    // knockback only for living creatures
                T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                         plane ? plane->normal : vec3_origin, 0, self->dmg * 3,
                         TRACKER_IMPACT_FLAGS, MOD_TRACKER);

                if (!(other->flags & (FL_FLY | FL_SWIM)))
                    other->velocity[2] += 140;

                damagetime = ((float)self->dmg) * FRAMETIME;
                damagetime = damagetime / TRACKER_DAMAGE_TIME;

                tracker_pain_daemon_spawn(self->owner, other, (int)damagetime);
            } else {    // lots of damage (almost autogib) for dead bodies
                T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                         plane ? plane->normal : vec3_origin, self->dmg * 4, self->dmg * 3,
                         TRACKER_IMPACT_FLAGS, MOD_TRACKER);
            }
        } else {    // full damage in one shot for inanimate objects
            T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                     plane ? plane->normal : vec3_origin, self->dmg, self->dmg * 3,
                     TRACKER_IMPACT_FLAGS, MOD_TRACKER);
        }
    }

    tracker_explode(self, plane);
}

void tracker_fly(edict_t *self)
{
    vec3_t  dest;
    vec3_t  dir;
    vec3_t  center;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health < 1) {
        tracker_explode(self, NULL);
        return;
    }

    // hunt for the centre of the enemy where we can work it out
    if (self->enemy->client) {
        VectorCopy(self->enemy->s.origin, dest);
        dest[2] += self->enemy->viewheight;
    } else if (VectorCompare(self->enemy->absmin, vec3_origin) ||
               VectorCompare(self->enemy->absmax, vec3_origin)) {
        VectorCopy(self->enemy->s.origin, dest);
    } else {
        VectorMA(vec3_origin, 0.5f, self->enemy->absmin, center);
        VectorMA(center, 0.5f, self->enemy->absmax, center);
        VectorCopy(center, dest);
    }

    VectorSubtract(dest, self->s.origin, dir);
    VectorNormalize(dir);
    vectoangles(dir, self->s.angles);
    VectorScale(dir, self->speed, self->velocity);
    VectorCopy(dest, self->monsterinfo.saved_goal);

    self->nextthink = level.framenum + 1;
}

void fire_tracker(edict_t *self, vec3_t start, vec3_t dir, int damage, int speed, edict_t *enemy)
{
    edict_t *bolt;
    trace_t tr;

    VectorNormalize(dir);

    bolt = G_Spawn();
    VectorCopy(start, bolt->s.origin);
    VectorCopy(start, bolt->s.old_origin);
    vectoangles(dir, bolt->s.angles);
    VectorScale(dir, speed, bolt->velocity);
    bolt->movetype = MOVETYPE_FLYMISSILE;
    bolt->clipmask = MASK_SHOT;
    bolt->solid = SOLID_BBOX;
    bolt->speed = speed;
    bolt->s.effects = EF_TRACKER;
    bolt->s.sound = gi.soundindex("weapons/disrupt.wav");
    VectorClear(bolt->mins);
    VectorClear(bolt->maxs);

    bolt->s.modelindex = gi.modelindex("models/proj/disintegrator/tris.md2");
    bolt->touch = tracker_touch;
    bolt->enemy = enemy;
    bolt->owner = self;
    bolt->dmg = damage;
    bolt->classname = "tracker";
    gi.linkentity(bolt);

    if (enemy) {
        bolt->nextthink = level.framenum + 1;
        bolt->think = tracker_fly;
    } else {
        bolt->nextthink = level.framenum + 10 * BASE_FRAMERATE;
        bolt->think = G_FreeEdict;
    }

    if (self->client)
        check_dodge(self, bolt->s.origin, dir, speed);

    tr = gi.trace(self->s.origin, NULL, NULL, bolt->s.origin, bolt, MASK_SHOT);
    if (tr.fraction < 1.0f) {
        VectorMA(bolt->s.origin, -10, dir, bolt->s.origin);
        bolt->touch(bolt, tr.ent, NULL, NULL);
    }
}

/*
======================================================================

TRAP (xatrix)

Thrown like a grenade. Once it lands it opens, drags the nearest living thing
into itself, kills it outright, sprays blood while it digests, and finally
coughs up a food cube worth a fraction of the victim's mass.

The client already draws EF_TRAP, so nothing was needed on that side.

======================================================================
*/

void Trap_Think(edict_t *ent)
{
    edict_t *target = NULL;
    edict_t *best = NULL;
    vec3_t  vec;
    int     len, i;
    int     oldlen = 8000;
    vec3_t  forward, right, up;

    if (ent->timestamp < level.framenum) {
        BecomeExplosion1(ent);
        return;
    }

    ent->nextthink = level.framenum + 1;

    if (!ent->groundentity)
        return;

    // digesting: spray blood in a slowly shrinking ring
    if (ent->s.frame > 4) {
        if (ent->s.frame == 5) {
            if (ent->wait == 64)
                gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/trapdown.wav"), 1, ATTN_IDLE, 0);

            ent->wait -= 2;
            ent->delay += level.time;

            for (i = 0; i < 3; i++) {
                float   ang, c, s;

                best = G_Spawn();

                if (ent->enemy && !strcmp(ent->enemy->classname, "monster_gekk")) {
                    best->s.modelindex = gi.modelindex("models/objects/gekkgib/torso/tris.md2");
                    best->s.effects |= TE_GREENBLOOD;
                } else if (ent->mass > 200) {
                    best->s.modelindex = gi.modelindex("models/objects/gibs/chest/tris.md2");
                    best->s.effects |= TE_BLOOD;
                } else {
                    best->s.modelindex = gi.modelindex("models/objects/gibs/sm_meat/tris.md2");
                    best->s.effects |= TE_BLOOD;
                }

                AngleVectors(ent->s.angles, forward, right, up);

                // xatrix calls RotatePointAroundVector(vec, up, right, ang) here.
                // That helper (and the PerpendicularVector / R_ConcatRotations it
                // needs) does not exist in this tree, but it is not needed: right
                // and forward already span the plane perpendicular to up, so the
                // rotation of `right` about `up` is exactly this.
                ang = DEG2RAD((360.0f / 3) * i + ent->delay);
                c = cosf(ang);
                s = sinf(ang);
                VectorScale(right, c, vec);
                VectorMA(vec, s, forward, vec);

                VectorMA(vec, ent->wait / 2, vec, vec);
                VectorAdd(vec, ent->s.origin, vec);
                VectorAdd(vec, forward, best->s.origin);

                best->s.origin[2] = ent->s.origin[2] + ent->wait;

                VectorCopy(ent->s.angles, best->s.angles);

                best->solid = SOLID_NOT;
                best->s.effects |= EF_GIB;
                best->takedamage = DAMAGE_YES;

                best->movetype = MOVETYPE_TOSS;
                best->svflags |= SVF_MONSTER;
                best->deadflag = DEAD_DEAD;

                VectorClear(best->mins);
                VectorClear(best->maxs);

                best->watertype = gi.pointcontents(best->s.origin);
                if (best->watertype & MASK_WATER)
                    best->waterlevel = 1;

                best->nextthink = level.framenum + 1;
                best->think = G_FreeEdict;
                gi.linkentity(best);
            }

            if (ent->wait < 19)
                ent->s.frame++;

            return;
        }

        ent->s.frame++;

        if (ent->s.frame == 8) {
            ent->nextthink = level.framenum + 1 * BASE_FRAMERATE;
            ent->think = G_FreeEdict;

            best = G_Spawn();
            SP_item_foodcube(best);
            VectorCopy(ent->s.origin, best->s.origin);
            best->s.origin[2] += 16;
            best->velocity[2] = 400;
            best->count = ent->mass;
            gi.linkentity(best);
            return;
        }

        return;
    }

    ent->s.effects &= ~EF_TRAP;

    if (ent->s.frame >= 4) {
        ent->s.effects |= EF_TRAP;
        VectorClear(ent->mins);
        VectorClear(ent->maxs);
    }

    if (ent->s.frame < 4)
        ent->s.frame++;

    // find the closest visible living thing
    while ((target = findradius(target, ent->s.origin, 256)) != NULL) {
        if (target == ent)
            continue;
        if (!(target->svflags & SVF_MONSTER) && !target->client)
            continue;
        if (target->health <= 0)
            continue;
        if (!visible(ent, target))
            continue;

        if (!best) {
            best = target;
            continue;
        }

        VectorSubtract(ent->s.origin, target->s.origin, vec);
        len = VectorLength(vec);

        if (len < oldlen) {
            oldlen = len;
            best = target;
        }
    }

    // pull the enemy in
    if (best) {
        vec3_t  fwd;

        if (best->groundentity) {
            best->s.origin[2] += 1;
            best->groundentity = NULL;
        }

        VectorSubtract(ent->s.origin, best->s.origin, vec);
        len = VectorLength(vec);

        if (best->client) {
            VectorNormalize(vec);
            VectorMA(best->velocity, 250, vec, best->velocity);
        } else {
            best->ideal_yaw = vectoyaw(vec);
            M_ChangeYaw(best);
            AngleVectors(best->s.angles, fwd, NULL, NULL);
            VectorScale(fwd, 256, best->velocity);
        }

        gi.sound(ent, CHAN_VOICE, gi.soundindex("weapons/trapsuck.wav"), 1, ATTN_IDLE, 0);

        if (len < 32) {
            if (best->mass < 400) {
                T_Damage(best, ent, ent->owner, vec3_origin, best->s.origin,
                         vec3_origin, 100000, 1, 0, MOD_TRAP);
                ent->enemy = best;
                ent->wait = 64;
                VectorCopy(ent->s.origin, ent->s.old_origin);
                ent->timestamp = level.framenum + 30 * BASE_FRAMERATE;

                if (deathmatch->value)
                    ent->mass = best->mass / 4;
                else
                    ent->mass = best->mass / 10;

                // start digesting
                ent->s.frame = 5;
            } else {
                // too big to swallow
                BecomeExplosion1(ent);
                return;
            }
        }
    }
}

void fire_trap(edict_t *self, vec3_t start, vec3_t aimdir, int damage,
               int speed, float timer, float damage_radius, bool held)
{
    edict_t *trap;
    vec3_t  dir;
    vec3_t  forward, right, up;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    trap = G_Spawn();
    VectorCopy(start, trap->s.origin);
    VectorScale(aimdir, speed, trap->velocity);
    VectorMA(trap->velocity, 200 + crandom() * 10.0f, up, trap->velocity);
    VectorMA(trap->velocity, crandom() * 10.0f, right, trap->velocity);
    VectorSet(trap->avelocity, 0, 300, 0);
    trap->movetype = MOVETYPE_BOUNCE;
    trap->clipmask = MASK_SHOT;
    trap->solid = SOLID_BBOX;
    VectorSet(trap->mins, -4, -4, 0);
    VectorSet(trap->maxs, 4, 4, 8);
    trap->s.modelindex = gi.modelindex("models/weapons/z_trap/tris.md2");
    trap->owner = self;
    trap->nextthink = level.framenum + 1 * BASE_FRAMERATE;
    trap->think = Trap_Think;
    trap->dmg = damage;
    trap->dmg_radius = damage_radius;
    trap->classname = "htrap";
    trap->s.sound = gi.soundindex("weapons/traploop.wav");

    if (held)
        trap->spawnflags = 3;
    else
        trap->spawnflags = 1;

    if (timer <= 0.0f)
        Grenade_Explode(trap);
    else
        gi.linkentity(trap);

    trap->timestamp = level.framenum + 30 * BASE_FRAMERATE;
}
