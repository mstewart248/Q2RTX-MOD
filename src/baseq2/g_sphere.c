/*
==============================================================================

ROGUE POWER SPHERES - defender, hunter, vengeance

Ported from rerelease/rogue/g_rogue_sphere.cpp (originally pmack, april 1998).

  defender  - orbits your head, shoots blaster bolts at anything that hurts you
  hunter    - sits idle until you drop below 25% health, then vore-balls
              whoever did it
  vengeance - sits idle until you die, then kills whoever killed you

All three share one entity and differ only in their think/pain/die hooks and
which model they wear. You may only have ONE at a time; the client holds it in
`owned_sphere` so the item use functions can refuse a second.

--------------------------------------------------------------------------
TIMING is FRAMENUM based here, not the rerelease's gtime_t. `self->wait` holds
an ABSOLUTE framenum (a float, because that is the field's type) rather than a
duration - same convention the prox mine uses in g_weapon.c.
--------------------------------------------------------------------------
THE SAM RAIMI CAM IS DELIBERATELY NOT PORTED.

In the rerelease, a hunter sphere launched by a player more than 192 units from
its target detaches the player from their body, gibs the body, and flies the
player's view along with the sphere. It is a deathmatch spectacle: the whole
block is gated on `!g_dm_force_respawn->integer && huntercam->integer`, neither
cvar exists here, and it needs FL_SAM_RAIMI plus ThrowGibs (this tree has
ThrowGib, singular) plus a chunk of player-detach code that would have to be
taught about this tree's view and save handling.

Dropping it costs nothing in single player - the sphere behaves identically,
you simply do not ride it. If it is ever wanted, the reference is
g_rogue_sphere.cpp hunter_pain() and hunter_think(), and it needs a real look
at p_view.c rather than a copy/paste.
==============================================================================
*/

#include "g_local.h"

#define DEFENDER_LIFESPAN   30.0f   // seconds
#define HUNTER_LIFESPAN     30.0f
#define VENGEANCE_LIFESPAN  30.0f
#define MINIMUM_FLY_TIME    15.0f

static void sphere_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf, int mod);

// *************************
// General sphere code
// *************************

void sphere_think_explode(edict_t *self)
{
    if (self->owner && self->owner->client && !(self->spawnflags & SPHERE_DOPPLEGANGER))
        self->owner->client->owned_sphere = NULL;

    BecomeExplosion1(self);
}

void sphere_explode(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    sphere_think_explode(self);
}

// only blows up if it is not currently chasing someone
void sphere_if_idle_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    if (!self->enemy)
        sphere_think_explode(self);
}

// *************************
// Movement
// *************************

// idle behaviour: hover just above the owner's head
static void sphere_fly(edict_t *self)
{
    vec3_t  dest;
    vec3_t  dir;

    if (level.framenum >= self->wait) {
        sphere_think_explode(self);
        return;
    }

    VectorCopy(self->owner->s.origin, dest);
    dest[2] = self->owner->absmax[2] + 4;

    // once a second, if we have lost sight of the owner, snap to them rather
    // than trying to fly through the wall between us
    if (level.framenum % BASE_FRAMERATE == 0) {
        if (!visible(self, self->owner)) {
            VectorCopy(dest, self->s.origin);
            gi.linkentity(self);
            return;
        }
    }

    VectorSubtract(dest, self->s.origin, dir);
    VectorScale(dir, 5, self->velocity);
}

// stupidChase: the vengeance sphere homes even with no line of sight
static void sphere_chase(edict_t *self, int stupidChase)
{
    vec3_t  dest;
    vec3_t  dir;
    float   dist;

    if (level.framenum >= self->wait || (self->enemy && self->enemy->health < 1)) {
        sphere_think_explode(self);
        return;
    }

    VectorCopy(self->enemy->s.origin, dest);
    if (self->enemy->client)
        dest[2] += self->enemy->viewheight;

    if (visible(self, self->enemy) || stupidChase) {
        if (!stupidChase)
            self->s.sound = gi.soundindex("spheres/h_active.wav");

        VectorSubtract(dest, self->s.origin, dir);
        VectorNormalize(dir);
        vectoangles(dir, self->s.angles);
        VectorScale(dir, 500, self->velocity);
        VectorCopy(dest, self->monsterinfo.saved_goal);
    } else if (VectorEmpty(self->monsterinfo.saved_goal)) {
        VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
        VectorNormalize(dir);
        vectoangles(dir, self->s.angles);

        self->s.sound = gi.soundindex("spheres/h_lurk.wav");
        VectorClear(self->velocity);
    } else {
        // head for the last place we saw them
        VectorSubtract(self->monsterinfo.saved_goal, self->s.origin, dir);
        dist = VectorNormalize(dir);

        if (dist > 1) {
            vectoangles(dir, self->s.angles);

            if (dist > 500)
                VectorScale(dir, 500, self->velocity);
            else if (dist < 20)
                VectorScale(dir, dist / FRAMETIME, self->velocity);
            else
                VectorScale(dir, dist, self->velocity);

            if (!stupidChase)
                self->s.sound = gi.soundindex("spheres/h_active.wav");
        } else {
            VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
            VectorNormalize(dir);
            vectoangles(dir, self->s.angles);

            if (!stupidChase)
                self->s.sound = gi.soundindex("spheres/h_lurk.wav");

            VectorClear(self->velocity);
        }
    }
}

// *************************
// Attacking
// *************************

/*
 * rogue defines sphere_fire() here - a vengeance sphere launching itself at a
 * target at 1000 ups. It is DEAD CODE in the original: there is no call site
 * anywhere in g_rogue_sphere.cpp or the rest of the rerelease tree, because
 * vengeance_pain sets the touch handler and lets sphere_chase do the flying.
 * Not ported rather than carried as an unreachable function.
 */

static void sphere_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf, int mod)
{
    if (self->spawnflags & SPHERE_DOPPLEGANGER) {
        if (other == self->teammaster)
            return;

        self->takedamage = DAMAGE_NO;
        self->owner = self->teammaster;
        self->teammaster = NULL;
    } else {
        if (other == self->owner)
            return;
        // don't blow up on corpses
        if (other->classname && !strcmp(other->classname, "bodyque"))
            return;
    }

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (self->owner) {
        if (other->takedamage) {
            T_Damage(other, self, self->owner, self->velocity, self->s.origin,
                     plane ? plane->normal : vec3_origin,
                     10000, 1, DAMAGE_DESTROY_ARMOR, mod);
        } else {
            T_RadiusDamage(self, self->owner, 512, self->owner, 256, mod);
        }
    }

    sphere_think_explode(self);
}

void vengeance_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (self->spawnflags & SPHERE_DOPPLEGANGER)
        sphere_touch(self, other, plane, surf, MOD_DOPPLE_VENGEANCE);
    else
        sphere_touch(self, other, plane, surf, MOD_VENGEANCE_SPHERE);
}

void hunter_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    // hitting the world is not a reason to detonate
    if (other == world)
        return;

    if (self->spawnflags & SPHERE_DOPPLEGANGER)
        sphere_touch(self, other, plane, surf, MOD_DOPPLE_HUNTER);
    else
        sphere_touch(self, other, plane, surf, MOD_HUNTER_SPHERE);
}

static void defender_shoot(edict_t *self, edict_t *enemy)
{
    vec3_t  dir;
    vec3_t  start;

    if (!enemy->inuse || enemy->health <= 0)
        return;

    if (enemy == self->owner)
        return;

    if (self->monsterinfo.attack_finished > level.framenum)
        return;

    if (!visible(self, self->enemy))
        return;

    VectorSubtract(enemy->s.origin, self->s.origin, dir);
    VectorNormalize(dir);

    VectorCopy(self->s.origin, start);
    start[2] += 2;
    fire_blaster2(self->owner, start, dir, 10, 1000, EF_BLASTER, false);

    self->monsterinfo.attack_finished = level.framenum + 0.4f * BASE_FRAMERATE;
}

// *************************
// Pain - this is what wakes a hunter or vengeance sphere up
// *************************

void hunter_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    edict_t *owner;

    if (self->enemy)
        return;

    owner = self->owner;

    if (!(self->spawnflags & SPHERE_DOPPLEGANGER)) {
        // a hunter only wakes when its owner is dead
        if (owner && owner->health > 0)
            return;
        if (other == owner)
            return;
    } else {
        self->wait = level.framenum + MINIMUM_FLY_TIME * BASE_FRAMERATE;
    }

    // always give it a fair run at the target once it is awake
    if (self->wait - level.framenum < MINIMUM_FLY_TIME * BASE_FRAMERATE)
        self->wait = level.framenum + MINIMUM_FLY_TIME * BASE_FRAMERATE;

    self->s.effects |= EF_BLASTER | EF_TRACKER;
    self->touch = hunter_touch;
    self->enemy = other;

    // (the Sam Raimi cam went here - see the header comment)
}

void defender_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    if (other == self->owner)
        return;

    self->enemy = other;
}

void vengeance_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    if (self->enemy)
        return;

    if (!(self->spawnflags & SPHERE_DOPPLEGANGER)) {
        // vengeance wakes when its owner drops below 25 health
        if (self->owner && self->owner->health >= 25)
            return;
        if (other == self->owner)
            return;
    } else {
        self->wait = level.framenum + MINIMUM_FLY_TIME * BASE_FRAMERATE;
    }

    if (self->wait - level.framenum < MINIMUM_FLY_TIME * BASE_FRAMERATE)
        self->wait = level.framenum + MINIMUM_FLY_TIME * BASE_FRAMERATE;

    self->s.effects |= EF_ROCKET;
    self->touch = vengeance_touch;
    self->enemy = other;
}

// *************************
// Think functions
// *************************

void defender_think(edict_t *self)
{
    if (!self->owner) {
        G_FreeEdict(self);
        return;
    }

    // do not follow the player into the intermission
    if (level.intermission_framenum) {
        sphere_think_explode(self);
        return;
    }

    if (self->owner->health <= 0) {
        sphere_think_explode(self);
        return;
    }

    self->s.frame++;
    if (self->s.frame > 19)
        self->s.frame = 0;

    if (self->enemy) {
        if (self->enemy->health > 0)
            defender_shoot(self, self->enemy);
        else
            self->enemy = NULL;
    }

    sphere_fly(self);

    if (self->inuse)
        self->nextthink = level.framenum + 1;
}

void hunter_think(edict_t *self)
{
    edict_t *owner;

    if (level.intermission_framenum) {
        sphere_think_explode(self);
        return;
    }

    owner = self->owner;

    if (!owner && !(self->spawnflags & SPHERE_DOPPLEGANGER)) {
        G_FreeEdict(self);
        return;
    }

    if (owner) {
        self->ideal_yaw = owner->s.angles[YAW];
    } else if (self->enemy) {    // fired by a doppelganger
        vec3_t dir;

        VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
        self->ideal_yaw = vectoyaw(dir);
    }

    M_ChangeYaw(self);

    if (self->enemy)
        sphere_chase(self, 0);
    else
        sphere_fly(self);

    if (self->inuse)
        self->nextthink = level.framenum + 1;
}

void vengeance_think(edict_t *self)
{
    if (level.intermission_framenum) {
        sphere_think_explode(self);
        return;
    }

    if (!self->owner && !(self->spawnflags & SPHERE_DOPPLEGANGER)) {
        G_FreeEdict(self);
        return;
    }

    if (self->enemy)
        sphere_chase(self, 1);
    else
        sphere_fly(self);

    if (self->inuse)
        self->nextthink = level.framenum + 1;
}

// *************************
// Spawning
// *************************

edict_t *Sphere_Spawn(edict_t *owner, int spawnflags)
{
    edict_t *sphere;

    sphere = G_Spawn();
    VectorCopy(owner->s.origin, sphere->s.origin);
    sphere->s.origin[2] = owner->absmax[2];
    sphere->s.angles[YAW] = owner->s.angles[YAW];
    sphere->solid = SOLID_BBOX;
    sphere->clipmask = MASK_SHOT;
    sphere->s.renderfx = RF_FULLBRIGHT | RF_IR_VISIBLE;
    sphere->movetype = MOVETYPE_FLYMISSILE;

    if (spawnflags & SPHERE_DOPPLEGANGER)
        sphere->teammaster = owner->teammaster;
    else
        sphere->owner = owner;

    sphere->classname = "sphere";
    sphere->yaw_speed = 40;
    sphere->monsterinfo.attack_finished = 0;
    sphere->spawnflags = spawnflags;    // the HUD reads this to name the sphere
    sphere->takedamage = DAMAGE_NO;

    switch (spawnflags & SPHERE_TYPE) {
    case SPHERE_DEFENDER:
        sphere->s.modelindex = gi.modelindex("models/items/defender/tris.md2");
        sphere->s.modelindex2 = gi.modelindex("models/items/shell/tris.md2");
        sphere->s.sound = gi.soundindex("spheres/d_idle.wav");
        sphere->pain = defender_pain;
        sphere->wait = level.framenum + DEFENDER_LIFESPAN * BASE_FRAMERATE;
        sphere->die = sphere_explode;
        sphere->think = defender_think;
        break;
    case SPHERE_HUNTER:
        sphere->s.modelindex = gi.modelindex("models/items/hunter/tris.md2");
        sphere->s.sound = gi.soundindex("spheres/h_idle.wav");
        sphere->wait = level.framenum + HUNTER_LIFESPAN * BASE_FRAMERATE;
        sphere->pain = hunter_pain;
        sphere->die = sphere_if_idle_die;
        sphere->think = hunter_think;
        break;
    case SPHERE_VENGEANCE:
        sphere->s.modelindex = gi.modelindex("models/items/vengnce/tris.md2");
        sphere->s.sound = gi.soundindex("spheres/v_idle.wav");
        sphere->wait = level.framenum + VENGEANCE_LIFESPAN * BASE_FRAMERATE;
        sphere->pain = vengeance_pain;
        sphere->die = sphere_if_idle_die;
        sphere->think = vengeance_think;
        VectorSet(sphere->avelocity, 30, 30, 0);
        break;
    default:
        gi.dprintf("Sphere_Spawn: invalid sphere type %d\n", spawnflags & SPHERE_TYPE);
        G_FreeEdict(sphere);
        return NULL;
    }

    sphere->nextthink = level.framenum + 1;

    gi.linkentity(sphere);

    return sphere;
}

/*
=================
Own_Sphere

Attach the sphere to the client so the item use functions can find it later.
Only one at a time; a second launch frees the first.
=================
*/
void Own_Sphere(edict_t *self, edict_t *sphere)
{
    if (!sphere)
        return;

    if (!self->client)
        return;

    if (self->client->owned_sphere && self->client->owned_sphere->inuse)
        G_FreeEdict(self->client->owned_sphere);

    self->client->owned_sphere = sphere;
}

void Defender_Launch(edict_t *self)
{
    Own_Sphere(self, Sphere_Spawn(self, SPHERE_DEFENDER));
}

void Hunter_Launch(edict_t *self)
{
    Own_Sphere(self, Sphere_Spawn(self, SPHERE_HUNTER));
}

void Vengeance_Launch(edict_t *self)
{
    Own_Sphere(self, Sphere_Spawn(self, SPHERE_VENGEANCE));
}

// *************************
// Item use
// *************************

static bool Sphere_RefuseSecond(edict_t *ent)
{
    if (ent->client && ent->client->owned_sphere) {
        gi.cprintf(ent, PRINT_HIGH, "Only one sphere at a time!\n");
        return true;
    }
    return false;
}

void Use_Defender(edict_t *ent, gitem_t *item)
{
    if (Sphere_RefuseSecond(ent))
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    Defender_Launch(ent);
}

void Use_Hunter(edict_t *ent, gitem_t *item)
{
    if (Sphere_RefuseSecond(ent))
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    Hunter_Launch(ent);
}

void Use_Vengeance(edict_t *ent, gitem_t *item)
{
    if (Sphere_RefuseSecond(ent))
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;
    Vengeance_Launch(ent);
}
