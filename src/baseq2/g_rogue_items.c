/*
==============================================================================

ROGUE ITEMS - the doppelganger

Ported from rerelease/rogue/g_rogue_newdm.cpp (the entity) and
g_rogue_items.cpp (the item hooks). Mirrors the reference file layout.

The doppelganger is a decoy: it drops a base plus a copy of your player model
standing idle, and anything that shoots it gets a sphere in the face. It is
NOT a monster - it never moves, never targets, and only reacts through pain.

--------------------------------------------------------------------------
IT IS DEATHMATCH-ONLY UPSTREAM, AND THAT IS PRESERVED HERE.

`Pickup_Doppleganger` in the rerelease opens with
`if (!deathmatch->integer) return false;` - the item spawns in single player
but cannot be picked up. Two of the Ground Zero maps place one. Per the
"rerelease game gets rerelease behaviour" scope rule that gate is reproduced
rather than quietly lifted; delete the deathmatch test in Pickup_Doppleganger
below to make it usable in the campaign, which is a one-line change.
--------------------------------------------------------------------------
TIMING: framenum based, as everywhere else in this tree.

The reference gates the body's frame stepping behind `teleport_time` because
the rerelease thinks at 40 Hz and the idle animation wants 10 Hz. This tree
already thinks at 10 Hz, so the body steps its frame every think and the
extra field is not needed - the resulting animation rate is identical.

FL_DAMAGEABLE / FL_TRAP have no equivalent here (see the prox mine in
g_weapon.c for the same substitution); SVF_DAMAGEABLE does the same job.
==============================================================================
*/

#include "g_local.h"
#include "m_player.h"

/*
=================
doppleganger_die

The payoff: whoever killed the decoy gets chased. Far away gets a hunter,
close range gets a vengeance sphere, and standing right on top of it gets
nothing but the explosion.
=================
*/
void doppleganger_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    edict_t *sphere;
    float   dist;
    vec3_t  dir;

    if (self->enemy && self->enemy != self->teammaster) {
        VectorSubtract(self->enemy->s.origin, self->s.origin, dir);
        dist = VectorLength(dir);

        if (dist > 80.0f) {
            if (dist > 768)
                sphere = Sphere_Spawn(self, SPHERE_HUNTER | SPHERE_DOPPLEGANGER);
            else
                sphere = Sphere_Spawn(self, SPHERE_VENGEANCE | SPHERE_DOPPLEGANGER);

            // wake it immediately and point it at the killer - a doppelganger
            // sphere has no owner to wait on
            if (sphere && sphere->pain)
                sphere->pain(sphere, attacker, 0, 0);
        }
    }

    self->takedamage = DAMAGE_NO;

    T_RadiusDamage(self, self->teammaster, 160.0f, self, 140.0f, MOD_DOPPLE_EXPLODE);

    if (self->teamchain)
        BecomeExplosion1(self->teamchain);
    BecomeExplosion1(self);
}

void doppleganger_pain(edict_t *self, edict_t *other, float kick, int damage)
{
    self->enemy = other;
}

void doppleganger_timeout(edict_t *self)
{
    doppleganger_die(self, self, self, 9999, self->s.origin);
}

/*
=================
body_think

The visible half - a copy of the player's entity state standing in the idle
animation, glancing around now and then so it does not read as a statue.
=================
*/
void body_think(edict_t *self)
{
    float r;

    if (fabsf(self->ideal_yaw - anglemod(self->s.angles[YAW])) < 2) {
        if (self->timestamp < level.framenum) {
            r = random();
            if (r < 0.10f) {
                self->ideal_yaw = random() * 350.0f;
                self->timestamp = level.framenum + 1 * BASE_FRAMERATE;
            }
        }
    } else {
        M_ChangeYaw(self);
    }

    // this tree thinks at 10 Hz, which IS the idle animation's rate
    self->s.frame++;
    if (self->s.frame > FRAME_stand40)
        self->s.frame = FRAME_stand01;

    self->nextthink = level.framenum + 1;
}

void fire_doppleganger(edict_t *ent, vec3_t start, vec3_t aimdir)
{
    edict_t *base;
    edict_t *body;
    vec3_t  dir;
    vec3_t  forward, right, up;
    int     number;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    base = G_Spawn();
    VectorCopy(start, base->s.origin);
    VectorCopy(dir, base->s.angles);
    base->movetype = MOVETYPE_TOSS;
    base->solid = SOLID_BBOX;
    base->s.renderfx |= RF_IR_VISIBLE;
    base->s.angles[PITCH] = 0;
    VectorSet(base->mins, -16, -16, -24);
    VectorSet(base->maxs, 16, 16, 32);
    base->s.modelindex = gi.modelindex("models/objects/dopplebase/tris.md2");
    base->s.alpha = 0.1f;
    base->teammaster = ent;
    base->svflags |= SVF_DAMAGEABLE;
    base->takedamage = DAMAGE_AIM;
    base->health = 30;
    base->pain = doppleganger_pain;
    base->die = doppleganger_die;

    base->nextthink = level.framenum + 30 * BASE_FRAMERATE;
    base->think = doppleganger_timeout;

    base->classname = "doppleganger";

    gi.linkentity(base);

    // the decoy body is a straight copy of the player's entity state, so it
    // wears the same model and skin. s.number must survive the copy.
    body = G_Spawn();
    number = body->s.number;
    body->s = ent->s;
    body->s.sound = 0;
    body->s.event = EV_NONE;
    body->s.number = number;
    body->yaw_speed = 30;
    body->ideal_yaw = 0;
    VectorCopy(start, body->s.origin);
    body->s.origin[2] += 8;
    body->think = body_think;
    body->nextthink = level.framenum + 1;
    gi.linkentity(body);

    base->teamchain = body;
    body->teammaster = base;
    body->owner = ent;

    gi.sound(body, CHAN_AUTO, gi.soundindex("medic_commander/monsterspawn1.wav"), 1, ATTN_NORM, 0);
}

/*
=================
Use_Doppleganger

Places the decoy 48 units in front of you, but only where it actually fits
and has ground under it - otherwise the item is not spent.
=================
*/
void Use_Doppleganger(edict_t *ent, gitem_t *item)
{
    vec3_t  forward, right;
    vec3_t  createPt, spawnPt;
    vec3_t  ang;

    ang[PITCH] = 0;
    ang[YAW] = ent->client->v_angle[YAW];
    ang[ROLL] = 0;
    AngleVectors(ang, forward, right, NULL);

    VectorMA(ent->s.origin, 48, forward, createPt);

    if (!FindSpawnPoint(createPt, ent->mins, ent->maxs, spawnPt, 32))
        return;

    if (!CheckGroundSpawnPoint(spawnPt, ent->mins, ent->maxs, 64, -1))
        return;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;

    SpawnGrow_Spawn(spawnPt, 2);
    fire_doppleganger(ent, spawnPt, forward);
}

bool Pickup_Doppleganger(edict_t *ent, edict_t *other)
{
    int quantity;

    // DEATHMATCH ONLY upstream - see the header comment. Delete these two
    // lines to make it usable in the campaign.
    if (!deathmatch->value)
        return false;

    quantity = other->client->pers.inventory[ITEM_INDEX(ent->item)];
    if (quantity >= 1)
        return false;

    other->client->pers.inventory[ITEM_INDEX(ent->item)]++;

    if (!(ent->spawnflags & DROPPED_ITEM))
        SetRespawn(ent, ent->item->quantity);

    return true;
}

/*
==============================================================================

THE A-M BOMB (rogue's "nuke")

Ported from rerelease/rogue/g_rogue_newweap.cpp 593-812 plus Use_Nuke from
g_rogue_items.cpp. You drop it, it beeps for 4 seconds, spends 6 seconds
flashing and screaming, then flattens everything within 512 units and shakes
the level for 3 more.

Timing is framenum based: `wait` is the absolute framenum it detonates on and
`timestamp` is the next warning beep.

NOT PORTED - the per-player screen white-out. The rerelease sets
`client->nuke_time` in T_RadiusNukeDamage and p_view.cpp turns that into a
blinding flash whose length depends on whether you had line of sight. That
needs a new gclient field, its save entry, and p_view work; the TE_NUKEBLAST
temp entity is already handled by this client (CL_ParseNuke), so the blast
still LOOKS like a nuke. Worth adding if Matt misses it.

FL_NOGIB is not ported either - it only stops players inside the inner
killzone from gibbing, which is cosmetic.
==============================================================================
*/

#define NUKE_DAMAGE             400
#define NUKE_RADIUS             512.0f
#define NUKE_DELAY              4.0f    // seconds of quiet beeping
#define NUKE_TIME_TO_LIVE       6.0f    // seconds of flashing before it goes
#define NUKE_QUAKE_TIME         3.0f
#define NUKE_QUAKE_STRENGTH     100.0f

/*
=================
Nuke_Quake

The aftershock. Throws every player on the ground into the air repeatedly for
NUKE_QUAKE_TIME, with a rumble every half second.
=================
*/
void Nuke_Quake(edict_t *self)
{
    int      i;
    edict_t *e;

    if (self->last_move_time < level.framenum) {
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, self->noise_index, 0.75f, ATTN_NONE, 0);
        self->last_move_time = level.framenum + 0.5f * BASE_FRAMERATE;
    }

    for (i = 1, e = g_edicts + i; i < globals.num_edicts; i++, e++) {
        if (!e->inuse)
            continue;
        if (!e->client)
            continue;
        if (!e->groundentity)
            continue;

        e->groundentity = NULL;
        e->velocity[0] += crandom() * 150;
        e->velocity[1] += crandom() * 150;
        e->velocity[2] = self->speed * (100.0f / e->mass);
    }

    if (level.framenum < self->timestamp)
        self->nextthink = level.framenum + 1;
    else
        G_FreeEdict(self);
}

static void Nuke_Explode(edict_t *ent)
{
    if (ent->teammaster->client)
        PlayerNoise(ent->teammaster, ent->s.origin, PNOISE_IMPACT);

    T_RadiusNukeDamage(ent, ent->teammaster, (float)ent->dmg, ent, ent->dmg_radius, MOD_NUKE);

    if (ent->dmg > NUKE_DAMAGE)
        gi.sound(ent, CHAN_ITEM, gi.soundindex("items/damage3.wav"), 1, ATTN_NORM, 0);

    gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/grenlx1a.wav"), 1, ATTN_NONE, 0);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1_BIG);
    gi.WritePosition(ent->s.origin);
    gi.multicast(ent->s.origin, MULTICAST_PHS);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_NUKEBLAST);
    gi.WritePosition(ent->s.origin);
    gi.multicast(ent->s.origin, MULTICAST_ALL);

    // it does not disappear - it becomes the earthquake
    ent->svflags |= SVF_NOCLIENT;
    ent->noise_index = gi.soundindex("world/rumble.wav");
    ent->think = Nuke_Quake;
    ent->speed = NUKE_QUAKE_STRENGTH;
    ent->timestamp = level.framenum + NUKE_QUAKE_TIME * BASE_FRAMERATE;
    ent->nextthink = level.framenum + 1;
    ent->last_move_time = 0;
}

void nuke_die(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    self->takedamage = DAMAGE_NO;

    // one nuke setting off another just removes it - otherwise a pile of them
    // recurses through Nuke_Explode
    if (attacker && attacker->classname && !strcmp(attacker->classname, "nuke")) {
        G_FreeEdict(self);
        return;
    }

    Nuke_Explode(self);
}

void Nuke_Think(edict_t *ent)
{
    float   attenuation, default_atten = 1.8f;
    int     nuke_damage_multiplier;
    int     muzzleflash;
    float   ttl_frames = NUKE_TIME_TO_LIVE * BASE_FRAMERATE;

    // a quad-boosted bomb is heard from further away
    nuke_damage_multiplier = ent->dmg / NUKE_DAMAGE;
    switch (nuke_damage_multiplier) {
    case 1:  attenuation = default_atten / 1.4f; muzzleflash = MZ_NUKE1; break;
    case 2:  attenuation = default_atten / 2.0f; muzzleflash = MZ_NUKE2; break;
    case 4:  attenuation = default_atten / 3.0f; muzzleflash = MZ_NUKE4; break;
    case 8:  attenuation = default_atten / 5.0f; muzzleflash = MZ_NUKE8; break;
    default: attenuation = default_atten;        muzzleflash = MZ_NUKE1; break;
    }

    if (ent->wait < level.framenum) {
        Nuke_Explode(ent);
    } else if (level.framenum >= ent->wait - ttl_frames) {
        // the loud half: flashing, faster beeping, and now shootable
        ent->s.frame++;

        if (ent->s.frame > 11)
            ent->s.frame = 6;

        if (gi.pointcontents(ent->s.origin) & (CONTENTS_SLIME | CONTENTS_LAVA)) {
            Nuke_Explode(ent);
            return;
        }

        ent->think = Nuke_Think;
        ent->nextthink = level.framenum + 1;
        ent->health = 1;
        ent->owner = NULL;

        gi.WriteByte(svc_muzzleflash);
        gi.WriteShort(ent - g_edicts);
        gi.WriteByte(muzzleflash);
        gi.multicast(ent->s.origin, MULTICAST_PHS);

        if (ent->timestamp <= level.framenum) {
            gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/nukewarn2.wav"), 1, attenuation, 0);

            // beeps get twice as fast over the last half
            if (ent->wait - level.framenum <= ttl_frames / 2.0f)
                ent->timestamp = level.framenum + 0.3f * BASE_FRAMERATE;
            else
                ent->timestamp = level.framenum + 0.5f * BASE_FRAMERATE;
        }
    } else {
        // the quiet half: one beep a second
        if (ent->timestamp <= level.framenum) {
            gi.sound(ent, CHAN_NO_PHS_ADD | CHAN_VOICE, gi.soundindex("weapons/nukewarn2.wav"), 1, attenuation, 0);
            ent->timestamp = level.framenum + 1 * BASE_FRAMERATE;
        }
        ent->nextthink = level.framenum + 1;
    }
}

void nuke_bounce(edict_t *ent, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (surf && surf->name[0]) {
        if (random() > 0.5f)
            gi.sound(ent, CHAN_BODY, gi.soundindex("weapons/hgrenb1a.wav"), 1, ATTN_NORM, 0);
        else
            gi.sound(ent, CHAN_BODY, gi.soundindex("weapons/hgrenb2a.wav"), 1, ATTN_NORM, 0);
    }
}

void fire_nuke(edict_t *self, vec3_t start, vec3_t aimdir, int speed, int damage_modifier)
{
    edict_t *nuke;
    vec3_t  dir;
    vec3_t  forward, right, up;
    float   scale;

    vectoangles(aimdir, dir);
    AngleVectors(dir, forward, right, up);

    nuke = G_Spawn();
    VectorCopy(start, nuke->s.origin);
    VectorScale(aimdir, speed, nuke->velocity);
    scale = 200 + crandom() * 10.0f;
    VectorMA(nuke->velocity, scale, up, nuke->velocity);
    scale = crandom() * 10.0f;
    VectorMA(nuke->velocity, scale, right, nuke->velocity);
    nuke->movetype = MOVETYPE_BOUNCE;
    nuke->clipmask = MASK_SHOT;
    nuke->solid = SOLID_BBOX;
    nuke->s.effects |= EF_GRENADE;
    nuke->s.renderfx |= RF_IR_VISIBLE;
    VectorSet(nuke->mins, -8, -8, 0);
    VectorSet(nuke->maxs, 8, 8, 16);
    nuke->s.modelindex = gi.modelindex("models/weapons/g_nuke/tris.md2");
    nuke->owner = self;
    nuke->teammaster = self;
    nuke->nextthink = level.framenum + 1;
    nuke->wait = level.framenum + (NUKE_DELAY + NUKE_TIME_TO_LIVE) * BASE_FRAMERATE;
    nuke->think = Nuke_Think;
    nuke->touch = nuke_bounce;

    nuke->health = 10000;
    nuke->takedamage = DAMAGE_AIM;
    nuke->svflags |= SVF_DAMAGEABLE;
    nuke->dmg = NUKE_DAMAGE * damage_modifier;
    if (damage_modifier == 1)
        nuke->dmg_radius = NUKE_RADIUS;
    else
        nuke->dmg_radius = NUKE_RADIUS + NUKE_RADIUS * (0.25f * (float)damage_modifier);

    nuke->classname = "nuke";
    nuke->die = nuke_die;

    gi.linkentity(nuke);
}

void Use_Nuke(edict_t *ent, gitem_t *item)
{
    vec3_t  forward, right, start;
    int     speed;
    int     mult;

    ent->client->pers.inventory[ITEM_INDEX(item)]--;

    // p_weapon.c's damage_multiplier is file-static and this is not a weapon
    // think, so work it out the same way it does. Quad and double do not stack
    // outside deathmatch, matching rogue.
    mult = 1;
    if (ent->client->quad_framenum > level.framenum)
        mult *= 4;
    if (ent->client->double_framenum > level.framenum &&
        (deathmatch->value || mult == 1))
        mult *= 2;

    AngleVectors(ent->client->v_angle, forward, right, NULL);

    VectorCopy(ent->s.origin, start);
    speed = 100;
    fire_nuke(ent, start, forward, speed, mult);
}

/*
==============================================================================

ROGUE POWERUPS - IR goggles and the cloak

Both are plain timed powerups; the work is in p_view.c, which turns the
framenum into a render flag.

The rerelease's cloak also has an `invisibility_fade_time`: for the first
moment after activating, and briefly when you fire, a monster CAN still see
you. That second field is not ported - here the cloak is simply on or off,
which is rogue's original behaviour.
==============================================================================
*/

void Use_IR(edict_t *ent, gitem_t *item)
{
    ent->client->pers.inventory[ITEM_INDEX(item)]--;

    if (ent->client->ir_framenum > level.framenum)
        ent->client->ir_framenum += 60 * BASE_FRAMERATE;
    else
        ent->client->ir_framenum = level.framenum + 60 * BASE_FRAMERATE;

    gi.sound(ent, CHAN_ITEM, gi.soundindex("misc/ir_start.wav"), 1, ATTN_NORM, 0);
}

void Use_Invisibility(edict_t *ent, gitem_t *item)
{
    ent->client->pers.inventory[ITEM_INDEX(item)]--;

    if (ent->client->invisible_framenum > level.framenum)
        ent->client->invisible_framenum += 30 * BASE_FRAMERATE;
    else
        ent->client->invisible_framenum = level.framenum + 30 * BASE_FRAMERATE;

    gi.sound(ent, CHAN_ITEM, gi.soundindex("items/protect.wav"), 1, ATTN_NORM, 0);
}
