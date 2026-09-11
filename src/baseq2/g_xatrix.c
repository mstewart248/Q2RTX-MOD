/* =======================================================================
 *
 * The Reckoning (xatrix) entities that the rest of this tree did not already
 * cover.
 *
 * Ported from src/rerelease/xatrix/. Everything else The Reckoning places -
 * its monsters, its weapons, func_object_repair, item_flashlight - was either
 * already here or landed with the rerelease work; these three are what a
 * census of the twenty x* maps showed still had no spawn function:
 *
 *   target_mal_laser   32, all in xship
 *   misc_transport      1, xmoon1  (Maxx's transport at the end of the game)
 *   misc_amb4           1, xship   (Mal's ambient loop)
 *
 * item_invisibility is deliberately NOT here: its single instance is in xdm7,
 * a deathmatch map, and it is a rogue/CTF powerup rather than a Reckoning one.
 *
 * =======================================================================
 */

#include "g_local.h"

void target_laser_think(edict_t *self);
void target_laser_off(edict_t *self);
void func_train_find(edict_t *self);
void misc_strogg_ship_use(edict_t *self, edict_t *other, edict_t *activator);

/*
 * =======================================================================
 * target_mal_laser - "Mal's laser"
 *
 * A target_laser that BLINKS rather than burning continuously: it shows for
 * 100 ms, hides for `wait`, and repeats. xship is full of them.
 *
 * This deliberately reuses this tree's target_laser_think / target_laser_off
 * rather than carrying its own copy, so the two stay in step. The spawnflag
 * numbers are the same ones SP_target_laser uses here (1 START_ON, 2 RED,
 * 4 GREEN, 8 BLUE, 16 YELLOW, 32 ORANGE, 64 FAT).
 *
 * Two rerelease-isms are dropped: FL_TRAP / FL_TRAP_LASER_FIELD, which only
 * exist to stop rogue's traps eating each other, and MODELINDEX_WORLD, which
 * is literally 1 here.
 * =======================================================================
 */

#define MAL_LASER_START_ON  1
#define MAL_LASER_RED       2
#define MAL_LASER_GREEN     4
#define MAL_LASER_BLUE      8
#define MAL_LASER_YELLOW    16
#define MAL_LASER_ORANGE    32
#define MAL_LASER_FAT       64

/* the internal "currently zapping" bit target_laser_on sets alongside START_ON */
#define MAL_LASER_ZAP       0x80000000

void mal_laser_think(edict_t *self);

void target_mal_laser_on(edict_t *self)
{
    if (!self->activator)
        self->activator = self;

    self->spawnflags |= MAL_LASER_ZAP | MAL_LASER_START_ON;
    self->svflags &= ~SVF_NOCLIENT;
    self->nextthink = level.framenum + (int)((self->wait + self->delay) * BASE_FRAMERATE);
}

void target_mal_laser_use(edict_t *self, edict_t *other, edict_t *activator)
{
    self->activator = activator;

    if (self->spawnflags & MAL_LASER_START_ON)
        target_laser_off(self);
    else
        target_mal_laser_on(self);
}

/* hidden half of the blink */
void mal_laser_think2(edict_t *self)
{
    self->svflags |= SVF_NOCLIENT;
    self->think = mal_laser_think;
    self->nextthink = level.framenum + (int)(self->wait * BASE_FRAMERATE);
    self->spawnflags |= MAL_LASER_ZAP;
}

/* visible half of the blink - one tenth of a second of actual beam */
void mal_laser_think(edict_t *self)
{
    self->svflags &= ~SVF_NOCLIENT;
    target_laser_think(self);
    self->think = mal_laser_think2;
    self->nextthink = level.framenum + 1;
}

/*
 * QUAKED target_mal_laser (1 0 0) (-4 -4 -4) (4 4 4) START_ON RED GREEN BLUE YELLOW ORANGE FAT
 * Mal's laser
 */
void SP_target_mal_laser(edict_t *self)
{
    self->movetype = MOVETYPE_NONE;
    self->solid = SOLID_NOT;
    self->s.renderfx |= RF_BEAM;
    self->s.modelindex = 1;         // must be non-zero

    // set the beam diameter
    if (self->spawnflags & MAL_LASER_FAT)
        self->s.frame = 16;
    else
        self->s.frame = 4;

    // set the color
    if (self->spawnflags & MAL_LASER_RED)
        self->s.skinnum = 0xf2f2f0f0;
    else if (self->spawnflags & MAL_LASER_GREEN)
        self->s.skinnum = 0xd0d1d2d3;
    else if (self->spawnflags & MAL_LASER_BLUE)
        self->s.skinnum = 0xf3f3f1f1;
    else if (self->spawnflags & MAL_LASER_YELLOW)
        self->s.skinnum = 0xdcdddedf;
    else if (self->spawnflags & MAL_LASER_ORANGE)
        self->s.skinnum = 0xe0e1e2e3;

    G_SetMovedir(self->s.angles, self->movedir);

    if (!self->delay)
        self->delay = 0.1f;

    if (!self->wait)
        self->wait = 0.1f;

    if (!self->dmg)
        self->dmg = 5;

    VectorSet(self->mins, -8, -8, -8);
    VectorSet(self->maxs, 8, 8, 8);

    self->nextthink = level.framenum + (int)(self->delay * BASE_FRAMERATE);
    self->think = mal_laser_think;

    self->use = target_mal_laser_use;

    gi.linkentity(self);

    if (self->spawnflags & MAL_LASER_START_ON)
        target_mal_laser_on(self);
    else
        target_laser_off(self);
}

/*
 * =======================================================================
 * misc_transport - Maxx's transport at the end of the game (xmoon1).
 *
 * A func_train wearing the strogg ship model. It is SVF_NOCLIENT and inert
 * until used, then rides its path like any train.
 * =======================================================================
 */

/*
 * QUAKED misc_transport (1 0 0) (-8 -8 -8) (8 8 8)
 * Maxx's transport at end of game
 */
void SP_misc_transport(edict_t *ent)
{
    if (!ent->target) {
        gi.dprintf("%s at %s: no target\n", ent->classname, vtos(ent->s.origin));
        G_FreeEdict(ent);
        return;
    }

    if (!ent->speed)
        ent->speed = 300;

    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_NOT;
    ent->s.modelindex = gi.modelindex("models/objects/ship/tris.md2");

    VectorSet(ent->mins, -16, -16, 0);
    VectorSet(ent->maxs, 16, 16, 32);

    ent->think = func_train_find;
    ent->nextthink = level.framenum + 1;
    ent->use = misc_strogg_ship_use;
    ent->svflags |= SVF_NOCLIENT;
    ent->moveinfo.accel = ent->moveinfo.decel = ent->moveinfo.speed = ent->speed;

    // START_ON: the transport is always a running train once triggered
    ent->spawnflags |= 1;

    gi.linkentity(ent);
}

/*
 * =======================================================================
 * misc_amb4 - "Mal's amb4 loop entity" (xship).
 *
 * A bare ambient sound emitter that re-triggers world/amb4.wav every 2.7
 * seconds at ATTN_NONE, i.e. audible across the whole map.
 * =======================================================================
 */

static int amb4sound;

void amb4_think(edict_t *ent)
{
    ent->nextthink = level.framenum + (int)(2.7f * BASE_FRAMERATE);
    gi.sound(ent, CHAN_VOICE, amb4sound, 1, ATTN_NONE, 0);
}

/*
 * QUAKED misc_amb4 (1 0 0) (-16 -16 -16) (16 16 16)
 * Mal's amb4 loop entity
 */
void SP_misc_amb4(edict_t *ent)
{
    ent->think = amb4_think;
    ent->nextthink = level.framenum + 1 * BASE_FRAMERATE;
    amb4sound = gi.soundindex("world/amb4.wav");
    gi.linkentity(ent);
}
