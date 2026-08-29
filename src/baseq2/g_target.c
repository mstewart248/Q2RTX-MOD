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

/*QUAKED target_temp_entity (1 0 0) (-8 -8 -8) (8 8 8)
Fire an origin based temp entity event to the clients.
"style"     type byte
*/
void Use_Target_Tent(edict_t *ent, edict_t *other, edict_t *activator)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(ent->style);
    gi.WritePosition(ent->s.origin);
    gi.multicast(ent->s.origin, MULTICAST_PVS);
}

void SP_target_temp_entity(edict_t *ent)
{
    ent->use = Use_Target_Tent;
}


//==========================================================

//==========================================================

/*QUAKED target_speaker (1 0 0) (-8 -8 -8) (8 8 8) looped-on looped-off reliable
"noise"     wav file to play
"attenuation"
-1 = none, send to whole level
1 = normal fighting sounds
2 = idle sound level
3 = ambient sound level
"volume"    0.0 to 1.0

Normal sounds play each time the target is used.  The reliable flag can be set for crucial voiceovers.

Looped sounds are always atten 3 / vol 1, and the use function toggles it on/off.
Multiple identical looping sounds will just increase volume without any speed cost.
*/
void Use_Target_Speaker(edict_t *ent, edict_t *other, edict_t *activator)
{
    int     chan;

    if (ent->spawnflags & 3) {
        // looping sound toggles
        if (ent->s.sound)
            ent->s.sound = 0;   // turn it off
        else
            ent->s.sound = ent->noise_index;    // start it
    } else {
        // normal sound
        if (ent->spawnflags & 4)
            chan = CHAN_VOICE | CHAN_RELIABLE;
        else
            chan = CHAN_VOICE;
        // use a positioned_sound, because this entity won't normally be
        // sent to any clients because it is invisible
        gi.positioned_sound(ent->s.origin, ent, chan, ent->noise_index, ent->volume, ent->attenuation, 0);
    }
}

void SP_target_speaker(edict_t *ent)
{
    char    buffer[MAX_QPATH];

    if (!st.noise) {
        gi.dprintf("target_speaker with no noise set at %s\n", vtos(ent->s.origin));
        return;
    }
    if (!strstr(st.noise, ".wav"))
        Q_snprintf(buffer, sizeof(buffer), "%s.wav", st.noise);
    else
        Q_strlcpy(buffer, st.noise, sizeof(buffer));
    ent->noise_index = gi.soundindex(buffer);

    if (!ent->volume)
        ent->volume = 1.0f;

    if (!ent->attenuation)
        ent->attenuation = 1.0f;
    else if (ent->attenuation == -1)    // use -1 so 0 defaults to 1
        ent->attenuation = 0;

    // check for prestarted looping sound
    if (ent->spawnflags & 1)
        ent->s.sound = ent->noise_index;

    ent->use = Use_Target_Speaker;

    // must link the entity so we get areas and clusters so
    // the server can determine who to send updates to
    gi.linkentity(ent);
}


//==========================================================

#define SPAWNFLAG_HELP_HELP1    1
#define SPAWNFLAG_HELP_SET_POI  2

void Use_Target_Help(edict_t *ent, edict_t *other, edict_t *activator)
{
    if (ent->spawnflags & SPAWNFLAG_HELP_HELP1) {
        if (strcmp(game.helpmessage1, ent->message)) {
            Q_strlcpy(game.helpmessage1, ent->message, sizeof(game.helpmessage1));
            game.help1changed++;
        }
    } else {
        if (strcmp(game.helpmessage2, ent->message)) {
            Q_strlcpy(game.helpmessage2, ent->message, sizeof(game.helpmessage2));
            game.help2changed++;
        }
    }

    game.helpchanged++;

    // a help target can double as the objective marker
    if (ent->spawnflags & SPAWNFLAG_HELP_SET_POI)
        target_poi_use(ent, other, activator);
}

/*QUAKED target_help (1 0 1) (-16 -16 -24) (16 16 24) help1
When fired, the "message" key becomes the current personal computer string, and the message light will be set on all clients status bars.
*/
void SP_target_help(edict_t *ent)
{
    if (deathmatch->value) {
        // auto-remove for deathmatch
        G_FreeEdict(ent);
        return;
    }

    if (!ent->message) {
        gi.dprintf("%s with no message at %s\n", ent->classname, vtos(ent->s.origin));
        G_FreeEdict(ent);
        return;
    }
    ent->use = Use_Target_Help;
}

//==========================================================

/*QUAKED target_secret (1 0 1) (-8 -8 -8) (8 8 8)
Counts a secret found.
These are single use targets.
*/
void use_target_secret(edict_t *ent, edict_t *other, edict_t *activator)
{
    gi.sound(ent, CHAN_VOICE, ent->noise_index, 1, ATTN_NORM, 0);

    level.found_secrets++;

    G_UseTargets(ent, activator);
    G_FreeEdict(ent);
}

void SP_target_secret(edict_t *ent)
{
    if (deathmatch->value) {
        // auto-remove for deathmatch
        G_FreeEdict(ent);
        return;
    }

    ent->use = use_target_secret;
    if (!st.noise)
        st.noise = "misc/secret.wav";
    ent->noise_index = gi.soundindex(st.noise);
    ent->svflags = SVF_NOCLIENT;
    level.total_secrets++;
    // map bug hack
    if (!Q_stricmp(level.mapname, "mine3") && ent->s.origin[0] == 280 && ent->s.origin[1] == -2048 && ent->s.origin[2] == -624)
        ent->message = "You have found a secret area.";
}

//==========================================================

/*QUAKED target_goal (1 0 1) (-8 -8 -8) (8 8 8)
Counts a goal completed.
These are single use targets.
*/
void use_target_goal(edict_t *ent, edict_t *other, edict_t *activator)
{
    gi.sound(ent, CHAN_VOICE, ent->noise_index, 1, ATTN_NORM, 0);

    level.found_goals++;

    if (level.found_goals == level.total_goals)
        gi.configstring(CS_CDTRACK, "0");

    G_UseTargets(ent, activator);
    G_FreeEdict(ent);
}

void SP_target_goal(edict_t *ent)
{
    if (deathmatch->value) {
        // auto-remove for deathmatch
        G_FreeEdict(ent);
        return;
    }

    ent->use = use_target_goal;
    if (!st.noise)
        st.noise = "misc/secret.wav";
    ent->noise_index = gi.soundindex(st.noise);
    ent->svflags = SVF_NOCLIENT;
    level.total_goals++;
}

//==========================================================


/*QUAKED target_explosion (1 0 0) (-8 -8 -8) (8 8 8)
Spawns an explosion temporary entity when used.

"delay"     wait this long before going off
"dmg"       how much radius damage should be done, defaults to 0
*/
void target_explosion_explode(edict_t *self)
{
    float       save;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_EXPLOSION1);
    gi.WritePosition(self->s.origin);
    gi.multicast(self->s.origin, MULTICAST_PHS);

    T_RadiusDamage(self, self->activator, self->dmg, NULL, self->dmg + 40, MOD_EXPLOSIVE);

    save = self->delay;
    self->delay = 0;
    G_UseTargets(self, self->activator);
    self->delay = save;
}

void use_target_explosion(edict_t *self, edict_t *other, edict_t *activator)
{
    self->activator = activator;

    if (!self->delay) {
        target_explosion_explode(self);
        return;
    }

    self->think = target_explosion_explode;
    self->nextthink = level.framenum + self->delay * BASE_FRAMERATE;
}

void SP_target_explosion(edict_t *ent)
{
    ent->use = use_target_explosion;
    ent->svflags = SVF_NOCLIENT;
}


//==========================================================

/*QUAKED target_changelevel (1 0 0) (-8 -8 -8) (8 8 8)
Changes level to "map" when fired
*/
void use_target_changelevel(edict_t *self, edict_t *other, edict_t *activator)
{
    if (level.intermission_framenum)
        return;     // already activated

    if (!deathmatch->value && !coop->value) {
        if (g_edicts[1].health <= 0)
            return;
    }

    // if noexit, do a ton of damage to other
    if (deathmatch->value && !((int)dmflags->value & DF_ALLOW_EXIT) && other != world) {
        T_Damage(other, self, self, vec3_origin, other->s.origin, vec3_origin, 10 * other->max_health, 1000, 0, MOD_EXIT);
        return;
    }

    // if multiplayer, let everyone know who hit the exit
    if (deathmatch->value) {
        if (activator && activator->client)
            gi.bprintf(PRINT_HIGH, "%s exited the level.\n", activator->client->pers.netname);
    }

    // if going to a new unit, clear cross triggers
    if (strstr(self->map, "*"))
        game.serverflags &= ~(SFL_CROSS_TRIGGER_MASK);

    BeginIntermission(self);
}

void SP_target_changelevel(edict_t *ent)
{
    if (!ent->map) {
        gi.dprintf("target_changelevel with no map at %s\n", vtos(ent->s.origin));
        G_FreeEdict(ent);
        return;
    }

    // ugly hack because *SOMEBODY* screwed up their map
    if ((Q_stricmp(level.mapname, "fact1") == 0) && (Q_stricmp(ent->map, "fact3") == 0))
        ent->map = "fact3$secret1";

    ent->use = use_target_changelevel;
    ent->svflags = SVF_NOCLIENT;
}


//==========================================================

/*QUAKED target_splash (1 0 0) (-8 -8 -8) (8 8 8)
Creates a particle splash effect when used.

Set "sounds" to one of the following:
  1) sparks
  2) blue water
  3) brown water
  4) slime
  5) lava
  6) blood

"count" how many pixels in the splash
"dmg"   if set, does a radius damage at this location when it splashes
        useful for lava/sparks
*/

void use_target_splash(edict_t *self, edict_t *other, edict_t *activator)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_SPLASH);
    gi.WriteByte(self->count);
    gi.WritePosition(self->s.origin);
    gi.WriteDir(self->movedir);
    gi.WriteByte(self->sounds);
    gi.multicast(self->s.origin, MULTICAST_PVS);

    if (self->dmg)
        T_RadiusDamage(self, activator, self->dmg, NULL, self->dmg + 40, MOD_SPLASH);
}

void SP_target_splash(edict_t *self)
{
    self->use = use_target_splash;
    G_SetMovedir(self->s.angles, self->movedir);

    if (!self->count)
        self->count = 32;

    self->svflags = SVF_NOCLIENT;
}


//==========================================================

/*QUAKED target_spawner (1 0 0) (-8 -8 -8) (8 8 8)
Set target to the type of entity you want spawned.
Useful for spawning monsters and gibs in the factory levels.

For monsters:
    Set direction to the facing you want it to have.

For gibs:
    Set direction if you want it moving and
    speed how fast it should be moving otherwise it
    will just be dropped
*/
void ED_CallSpawn(edict_t *ent);

void use_target_spawner(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *ent;

    ent = G_Spawn();
    ent->classname = self->target;
    VectorCopy(self->s.origin, ent->s.origin);
    VectorCopy(self->s.angles, ent->s.angles);
    ED_CallSpawn(ent);
    gi.unlinkentity(ent);
    KillBox(ent);
    gi.linkentity(ent);
    if (self->speed)
        VectorCopy(self->movedir, ent->velocity);
}

void SP_target_spawner(edict_t *self)
{
    self->use = use_target_spawner;
    self->svflags = SVF_NOCLIENT;
    if (self->speed) {
        G_SetMovedir(self->s.angles, self->movedir);
        VectorScale(self->movedir, self->speed, self->movedir);
    }
}

//==========================================================

/*QUAKED target_blaster (1 0 0) (-8 -8 -8) (8 8 8) NOTRAIL NOEFFECTS
Fires a blaster bolt in the set direction when triggered.

dmg     default is 15
speed   default is 1000
*/

void use_target_blaster(edict_t *self, edict_t *other, edict_t *activator)
{
#if 0
    int effect;

    if (self->spawnflags & 2)
        effect = 0;
    else if (self->spawnflags & 1)
        effect = EF_HYPERBLASTER;
    else
        effect = EF_BLASTER;
#endif

    fire_blaster(self, self->s.origin, self->movedir, self->dmg, self->speed, EF_BLASTER, MOD_TARGET_BLASTER);
    gi.sound(self, CHAN_VOICE, self->noise_index, 1, ATTN_NORM, 0);
}

void SP_target_blaster(edict_t *self)
{
    self->use = use_target_blaster;
    G_SetMovedir(self->s.angles, self->movedir);
    self->noise_index = gi.soundindex("weapons/laser2.wav");

    if (!self->dmg)
        self->dmg = 15;
    if (!self->speed)
        self->speed = 1000;

    self->svflags = SVF_NOCLIENT;
}


//==========================================================

// a cross level/unit target has to think at least one frame after it spawns:
// SV_RunThink treats nextthink 0 as "no think scheduled", and on level entry
// (framenum 0) any sub-frame delay - mguhub uses 0.001 - truncates to exactly that.
int cross_trigger_delay(float delay)
{
    int frames = delay * BASE_FRAMERATE;

    return frames > 1 ? frames : 1;
}

/*QUAKED target_crosslevel_trigger (.5 .5 .5) (-8 -8 -8) (8 8 8) trigger1 trigger2 trigger3 trigger4 trigger5 trigger6 trigger7 trigger8
Once this trigger is touched/used, any trigger_crosslevel_target with the same trigger number is automatically used when a level is started within the same unit.  It is OK to check multiple triggers.  Message, delay, target, and killtarget also work.
*/
void trigger_crosslevel_trigger_use(edict_t *self, edict_t *other, edict_t *activator)
{
    game.serverflags |= self->spawnflags;
    G_FreeEdict(self);
}

void SP_target_crosslevel_trigger(edict_t *self)
{
    self->svflags = SVF_NOCLIENT;
    self->use = trigger_crosslevel_trigger_use;
}

/*QUAKED target_crosslevel_target (.5 .5 .5) (-8 -8 -8) (8 8 8) trigger1 trigger2 trigger3 trigger4 trigger5 trigger6 trigger7 trigger8
Triggered by a trigger_crosslevel elsewhere within a unit.  If multiple triggers are checked, all must be true.  Delay, target and
killtarget also work.

"delay"     delay before using targets if the trigger has been activated (default 1)
*/
void target_crosslevel_target_think(edict_t *self)
{
    if (self->spawnflags == (game.serverflags & SFL_CROSS_TRIGGER_MASK & self->spawnflags)) {
        G_UseTargets(self, self);
        G_FreeEdict(self);
    }
}

void SP_target_crosslevel_target(edict_t *self)
{
    if (! self->delay)
        self->delay = 1;
    self->svflags = SVF_NOCLIENT;

    self->think = target_crosslevel_target_think;
    self->nextthink = level.framenum + cross_trigger_delay(self->delay);
}

//==========================================================

/*QUAKED target_crossunit_trigger (.5 .5 .5) (-8 -8 -8) (8 8 8) trigger1 trigger2 trigger3 trigger4 trigger5 trigger6 trigger7 trigger8
Once this trigger is touched/used, any target_crossunit_target with the same trigger number is automatically used when a level is started, in this unit or any other.  It is OK to check multiple triggers.

Unlike the cross *level* triggers these survive a unit change, which is how the machine games hub remembers which units have already been finished.
*/
void trigger_crossunit_trigger_use(edict_t *self, edict_t *other, edict_t *activator)
{
    game.cross_unit_flags |= self->spawnflags & SFL_CROSS_UNIT_MASK;
    G_FreeEdict(self);
}

void SP_target_crossunit_trigger(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    self->svflags = SVF_NOCLIENT;
    self->use = trigger_crossunit_trigger_use;
}

/*QUAKED target_crossunit_target (.5 .5 .5) (-8 -8 -8) (8 8 8) trigger1 trigger2 trigger3 trigger4 trigger5 trigger6 trigger7 trigger8
Triggered by a target_crossunit_trigger fired in any unit.  If multiple triggers are checked, all must be true.  Delay, target and
killtarget also work.

"delay"     delay before using targets if the trigger has been activated (default 1)
*/
void target_crossunit_target_think(edict_t *self)
{
    int flags = self->spawnflags & SFL_CROSS_UNIT_MASK;

    if (flags == (game.cross_unit_flags & flags)) {
        G_UseTargets(self, self);
        G_FreeEdict(self);
    }
}

void SP_target_crossunit_target(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    if (!self->delay)
        self->delay = 1;
    self->svflags = SVF_NOCLIENT;

    self->think = target_crossunit_target_think;
    self->nextthink = level.framenum + cross_trigger_delay(self->delay);
}

//==========================================================

/*QUAKED target_laser (0 .5 .8) (-8 -8 -8) (8 8 8) START_ON RED GREEN BLUE YELLOW ORANGE FAT
When triggered, fires a laser.  You can either set a target
or a direction.
*/

void target_laser_think(edict_t *self)
{
    edict_t *ignore;
    vec3_t  start;
    vec3_t  end;
    trace_t tr;
    vec3_t  point;
    vec3_t  last_movedir;
    int     count;

    if (self->spawnflags & 0x80000000)
        count = 8;
    else
        count = 4;

    if (self->enemy) {
        VectorCopy(self->movedir, last_movedir);
        VectorMA(self->enemy->absmin, 0.5f, self->enemy->size, point);
        VectorSubtract(point, self->s.origin, self->movedir);
        VectorNormalize(self->movedir);
        if (!VectorCompare(self->movedir, last_movedir))
            self->spawnflags |= 0x80000000;
    }

    ignore = self;
    VectorCopy(self->s.origin, start);
    VectorMA(start, 2048, self->movedir, end);
    while (1) {
        tr = gi.trace(start, NULL, NULL, end, ignore, CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_DEADMONSTER);

        if (!tr.ent)
            break;

        // hurt it if we can
        if ((tr.ent->takedamage) && !(tr.ent->flags & FL_IMMUNE_LASER))
            T_Damage(tr.ent, self, self->activator, self->movedir, tr.endpos, vec3_origin, self->dmg, 1, DAMAGE_ENERGY, MOD_TARGET_LASER);

        // if we hit something that's not a monster or player or is immune to lasers, we're done
        if (!(tr.ent->svflags & SVF_MONSTER) && (!tr.ent->client)) {
            if (self->spawnflags & 0x80000000) {
                self->spawnflags &= ~0x80000000;
                gi.WriteByte(svc_temp_entity);
                gi.WriteByte(TE_LASER_SPARKS);
                gi.WriteByte(count);
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
}

void target_laser_on(edict_t *self)
{
    if (!self->activator)
        self->activator = self;
    self->spawnflags |= 0x80000001;
    self->svflags &= ~SVF_NOCLIENT;
    target_laser_think(self);
}

void target_laser_off(edict_t *self)
{
    self->spawnflags &= ~1;
    self->svflags |= SVF_NOCLIENT;
    self->nextthink = 0;
}

void target_laser_use(edict_t *self, edict_t *other, edict_t *activator)
{
    self->activator = activator;
    if (self->spawnflags & 1)
        target_laser_off(self);
    else
        target_laser_on(self);
}

void target_laser_start(edict_t *self)
{
    edict_t *ent;

    self->movetype = MOVETYPE_NONE;
    self->solid = SOLID_NOT;
    self->s.renderfx |= RF_BEAM | RF_TRANSLUCENT;
    self->s.modelindex = 1;         // must be non-zero

    /*
     * [rerelease] SPAWNFLAG_LASER_LIGHTNING (0x10000). id added this well after
     * the original six colour flags, and it OVERRIDES them: it forces the blue
     * ramp and asks the renderer for a lightning bolt instead of a straight
     * laser (their RF_BEAM_LIGHTNING is literally RF_BEAM | RF_GLOW).
     *
     * mgu6m3's kill beam - the one that ends the Modir fight - carries
     * spawnflags 65602 = LIGHTNING | FAT | RED. Without this branch the RED
     * flag won, which is why that beam came out as a flat red tube here and is
     * blue in the retail game. The colour block below is now guarded on skinnum
     * being unset, exactly as theirs is, so LIGHTNING wins over RED.
     */
    if (self->spawnflags & SPAWNFLAG_LASER_LIGHTNING) {
        self->s.renderfx |= RF_GLOW;    // RF_BEAM | RF_GLOW == their lightning
        if (!self->s.skinnum)
            self->s.skinnum = 0xf3f3f1f1;   // their default lightning colour
    }

    // set the beam diameter
    if (self->spawnflags & 64)
        self->s.frame = 16;
    else
        self->s.frame = 4;

    // set the color
    if (!self->s.skinnum) {
        if (self->spawnflags & 2)
            self->s.skinnum = 0xf2f2f0f0;   // red
        else if (self->spawnflags & 4)
            self->s.skinnum = 0xd0d1d2d3;   // green
        else if (self->spawnflags & 8)
            self->s.skinnum = 0xf3f3f1f1;   // blue
        else if (self->spawnflags & 16)
            self->s.skinnum = 0xdcdddedf;   // yellow
        else if (self->spawnflags & 32)
            self->s.skinnum = 0xe0e1e2e3;   // orange
    }

    if (!self->enemy) {
        if (self->target) {
            ent = G_Find(NULL, FOFS(targetname), self->target);
            if (!ent)
                gi.dprintf("%s at %s: %s is a bad target\n", self->classname, vtos(self->s.origin), self->target);
            self->enemy = ent;
        } else {
            G_SetMovedir(self->s.angles, self->movedir);
        }
    }
    self->use = target_laser_use;
    self->think = target_laser_think;

    if (!self->dmg)
        self->dmg = 1;

    VectorSet(self->mins, -8, -8, -8);
    VectorSet(self->maxs, 8, 8, 8);
    gi.linkentity(self);

    if (self->spawnflags & 1)
        target_laser_on(self);
    else
        target_laser_off(self);
}

void SP_target_laser(edict_t *self)
{
    // let everything else get spawned before we start firing
    self->think = target_laser_start;
    self->nextthink = level.framenum + 1 * BASE_FRAMERATE;
}

//==========================================================

/*QUAKED target_lightramp (0 .5 .8) (-8 -8 -8) (8 8 8) TOGGLE
speed       How many seconds the ramping will take
message     two letters; starting lightlevel and ending lightlevel
*/

void target_lightramp_think(edict_t *self)
{
    char    style[2];
    float   diff = (level.framenum - self->timestamp) * FRAMETIME;

    style[0] = 'a' + self->movedir[0] + diff * self->movedir[2];
    style[1] = 0;
    gi.configstring(CS_LIGHTS + self->enemy->style, style);

    if (diff < self->speed) {
        self->nextthink = level.framenum + 1;
    } else if (self->spawnflags & 1) {
        char    temp;

        temp = self->movedir[0];
        self->movedir[0] = self->movedir[1];
        self->movedir[1] = temp;
        self->movedir[2] *= -1;
    }
}

void target_lightramp_use(edict_t *self, edict_t *other, edict_t *activator)
{
    if (!self->enemy) {
        edict_t     *e;

        // check all the targets
        e = NULL;
        while (1) {
            e = G_Find(e, FOFS(targetname), self->target);
            if (!e)
                break;
            if (strcmp(e->classname, "light") != 0) {
                gi.dprintf("%s at %s ", self->classname, vtos(self->s.origin));
                gi.dprintf("target %s (%s at %s) is not a light\n", self->target, e->classname, vtos(e->s.origin));
            } else {
                self->enemy = e;
            }
        }

        if (!self->enemy) {
            gi.dprintf("%s target %s not found at %s\n", self->classname, self->target, vtos(self->s.origin));
            G_FreeEdict(self);
            return;
        }
    }

    self->timestamp = level.framenum;
    target_lightramp_think(self);
}

void SP_target_lightramp(edict_t *self)
{
    if (!self->message || strlen(self->message) != 2 || self->message[0] < 'a' || self->message[0] > 'z' || self->message[1] < 'a' || self->message[1] > 'z' || self->message[0] == self->message[1]) {
        gi.dprintf("target_lightramp has bad ramp (%s) at %s\n", self->message, vtos(self->s.origin));
        G_FreeEdict(self);
        return;
    }

    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    if (!self->target) {
        gi.dprintf("%s with no target at %s\n", self->classname, vtos(self->s.origin));
        G_FreeEdict(self);
        return;
    }

    self->svflags |= SVF_NOCLIENT;
    self->use = target_lightramp_use;
    self->think = target_lightramp_think;

    self->movedir[0] = self->message[0] - 'a';
    self->movedir[1] = self->message[1] - 'a';
    self->movedir[2] = (self->movedir[1] - self->movedir[0]) / self->speed;
}

//==========================================================

/*QUAKED target_earthquake (1 0 0) (-8 -8 -8) (8 8 8)
When triggered, this initiates a level-wide earthquake.
All players and monsters are affected.
"speed"     severity of the quake (default:200)
"count"     duration of the quake (default:5)
*/

void target_earthquake_think(edict_t *self)
{
    int     i;
    edict_t *e;

    if (self->last_move_framenum < level.framenum) {
        gi.positioned_sound(self->s.origin, self, CHAN_AUTO, self->noise_index, 1.0f, ATTN_NONE, 0);
        self->last_move_framenum = level.framenum + 0.5f * BASE_FRAMERATE;
    }

    for (i = 1, e = g_edicts + i; i < globals.num_edicts; i++, e++) {
        if (!e->inuse)
            continue;
        if (!e->client)
            continue;
        if (!e->groundentity)
            continue;

        // The rerelease made this a pure screen shake - its own QUAKED comment
        // says "All players are affected with a screen shake" - and the MGU maps
        // are tuned for that. Vanilla instead threw the player upward at
        // speed * (100/mass), which for mgu1m1's speed 1600 is 800 u/s and
        // launches you straight out of the 64-unit drop pod before it can eject
        // you. Do not restore the impulse without re-tuning every MGU quake.
        e->client->quake_framenum = level.framenum + 1 * BASE_FRAMERATE;
    }

    if (level.framenum < self->timestamp)
        self->nextthink = level.framenum + 1;
}

void target_earthquake_use(edict_t *self, edict_t *other, edict_t *activator)
{
    self->timestamp = level.framenum + self->count * BASE_FRAMERATE;
    self->nextthink = level.framenum + 1;
    self->activator = activator;
    self->last_move_framenum = 0;
}

void SP_target_earthquake(edict_t *self)
{
    if (!self->targetname)
        gi.dprintf("untargeted %s at %s\n", self->classname, vtos(self->s.origin));

    if (!self->count)
        self->count = 5;

    if (!self->speed)
        self->speed = 200;

    self->svflags |= SVF_NOCLIENT;
    self->think = target_earthquake_think;
    self->use = target_earthquake_use;

    self->noise_index = gi.soundindex("world/quake.wav");
}

/*QUAKED target_steam (1 0 0) (-8 -8 -8) (8 8 8)
A jet of particles along a line - steam, sparks, dripping water and so on.
Ported from src/rerelease/rogue/g_rogue_newtarg.cpp.

It emits only when USED, so every one of these needs something poking it: the
rerelease maps drive them from trigger_multiple, trigger_relay and func_timer.

"speed"     particle velocity, default 75. Also sets the width of the base -
            faster is wider. Stored in `style`, because `speed` is reused.
"count"     number of particles, default 32
"sounds"    palette colour, default 8 (steam). The effect spans colour..colour+6.
            224 sparks, 176 blue water, 80 brown water, 208 slime, 232 blood
"wait"      seconds to run. Taken from the entity that used us when unset, so a
            func_timer's own wait drives it.
"target"    aim at this entity instead of using "angle"

The client half of this already existed - TE_STEAM, CL_ParseSteam() and the
cl_sustain system in src/client/tent.c - so this is game side only.
*/
void use_target_steam(edict_t *self, edict_t *other, edict_t *activator)
{
    // Matches the id original: an id used to tie the repeating (sustained)
    // effect on the client to this emitter across frames. -1 means one-shot.
    static int nextid;

    vec3_t  point;

    if (nextid > 20000)
        nextid = nextid % 20000;
    nextid++;

    // Take the timing from whatever used us, unless the mapper set it. The
    // guard means this is resolved once and then cached in self->wait.
    if (!self->wait) {
        if (other)
            self->wait = other->wait * 1000;
        else
            self->wait = 1000;
    }

    // re-aim every time, in case the target moved
    if (self->enemy) {
        VectorAdd(self->enemy->absmin, self->enemy->absmax, point);
        VectorScale(point, 0.5f, point);
        VectorSubtract(point, self->s.origin, self->movedir);
        VectorNormalize(self->movedir);
    }

    // (id computes a `point` offset along movedir here and then never uses it -
    // it writes s.origin either way. Not reproduced.)

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_STEAM);
    if (self->wait > 100)
        gi.WriteShort(nextid);
    else
        gi.WriteShort(-1);          // one-shot puff, no sustain slot
    gi.WriteByte(self->count);
    gi.WritePosition(self->s.origin);
    gi.WriteDir(self->movedir);
    gi.WriteByte(self->sounds & 0xff);
    gi.WriteShort((int)self->style);
    if (self->wait > 100)
        gi.WriteLong((int)self->wait);      // only the sustained form sends this
    gi.multicast(self->s.origin, MULTICAST_PVS);
}

void target_steam_start(edict_t *self)
{
    edict_t *ent;

    self->use = use_target_steam;

    if (self->target) {
        ent = G_Find(NULL, FOFS(targetname), self->target);
        if (!ent)
            gi.dprintf("%s: target %s not found\n", self->classname, self->target);
        self->enemy = ent;
    } else {
        G_SetMovedir(self->s.angles, self->movedir);
    }

    if (!self->count)
        self->count = 32;
    if (!self->style)
        self->style = 75;
    if (!self->sounds)
        self->sounds = 8;
    if (self->wait)
        self->wait *= 1000;     // the key is in seconds, the wire wants ms

    // both go over the wire as a single byte
    self->sounds &= 0xff;
    self->count &= 0xff;

    self->svflags = SVF_NOCLIENT;

    gi.linkentity(self);
}

void SP_target_steam(edict_t *self)
{
    // "speed" is moved into style because the wire field is the particle
    // velocity AND the width of the jet
    self->style = (int)self->speed;

    if (self->target) {
        // defer, so the aim entity is guaranteed to have spawned
        self->think = target_steam_start;
        self->nextthink = level.framenum + 1 * BASE_FRAMERATE;
    } else {
        target_steam_start(self);
    }
}

/*QUAKED target_anger (1 0 0) (-8 -8 -8) (8 8 8)
Points the monster named by "target" at the entity named by "killtarget", making
the latter a valid enemy even if it is not a monster. Ported from
src/rerelease/rogue/g_rogue_newtarg.cpp.

Note: the rerelease also sets AI_DO_NOT_COUNT on the promoted entity so it is
excluded from the level monster total. That flag does not exist in this tree, so
a promoted non-monster will count toward the total shown at level end.
*/
void target_anger_use(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *target;
    edict_t *t;

    target = G_Find(NULL, FOFS(targetname), self->killtarget);

    if (!target || !self->target)
        return;

    // make whatever it is a "good guy" so the monster will try to kill it
    if (!(target->svflags & SVF_MONSTER)) {
        target->monsterinfo.aiflags |= AI_GOOD_GUY;
        target->svflags |= SVF_MONSTER;
        target->health = 300;
    }

    t = NULL;
    while ((t = G_Find(t, FOFS(targetname), self->target))) {
        if (t == self) {
            gi.dprintf("WARNING: entity used itself.\n");
            continue;
        }
        if (!t->use)
            continue;
        if (t->health <= 0)
            continue;

        t->enemy = target;
        t->monsterinfo.aiflags |= AI_TARGET_ANGER;
        FoundTarget(t);
    }
}

void SP_target_anger(edict_t *self)
{
    if (!self->target) {
        gi.dprintf("target_anger without target!\n");
        G_FreeEdict(self);
        return;
    }
    if (!self->killtarget) {
        gi.dprintf("target_anger without killtarget!\n");
        G_FreeEdict(self);
        return;
    }

    self->use = target_anger_use;
    self->svflags = SVF_NOCLIENT;
}

/*QUAKED target_sky (.5 .5 .5) (-8 -8 -8) (8 8 8)
Changes the sky (and/or its rotation) when used.

Every MGU unit opener fires one of these from droppod_tele, so the sky swaps to
the destination environment at the moment the drop pod ejects you: mgu2m1 to
unit3_, mgu3m1 to ruined_earth_city_, mgu5m1 to strogg_moon_.

"sky"           new sky basename
"skyrotate"     rotation speed in degrees/second
"skyautorotate" 0 to stop rotating, 1 to rotate with time
"skyaxis"       axis to rotate around

Absent keys are left alone, which is why these have to be tracked separately
from their values - mgu3m1 deliberately sets skyrotate 0 to halt the sky.
*/
void use_target_sky(edict_t *self, edict_t *other, edict_t *activator)
{
    if (self->map)
        gi.configstring(CS_SKY, self->map);

    if (self->count & 3) {
        // only overwrite the half this entity actually specified; the other
        // half comes from level.sky_* rather than a configstring read-back,
        // which this game import does not offer.
        if (self->count & 1)
            level.sky_rotate = self->accel;
        if (self->count & 2)
            level.sky_autorotate = self->style;

        gi.configstring(CS_SKYROTATE, va("%f %d", level.sky_rotate, level.sky_autorotate));
    }

    if (self->count & 4)
        gi.configstring(CS_SKYAXIS, va("%f %f %f",
                                       self->movedir[0], self->movedir[1], self->movedir[2]));
}

void SP_target_sky(edict_t *self)
{
    self->use = use_target_sky;

    if (st.keys_specified & SPAWNKEY_SKY)
        self->map = st.sky;

    if (st.keys_specified & SPAWNKEY_SKYAXIS) {
        self->count |= 4;
        VectorCopy(st.skyaxis, self->movedir);
    }

    if (st.keys_specified & SPAWNKEY_SKYROTATE) {
        self->count |= 1;
        self->accel = st.skyrotate;
    }

    if (st.keys_specified & SPAWNKEY_SKYAUTOROTATE) {
        self->count |= 2;
        self->style = st.skyautorotate;
    }

    // Map hack: mgu1m1's post-landing sky never stops.
    //
    // Every other MGU opener parks the sky and halts it - mgu2m1 70/auto 0,
    // mgu3m1 0/auto 0, mgu4m1 180/auto 0, mgu5m1 127/auto 0, mgu6m1 -90/auto 0
    // - so skyrotate becomes a fixed ORIENTATION rather than a speed (see
    // prepare_sky_matrix, which multiplies by time only when autorotate is set).
    // mgu1m1 alone says skyrotate 0.5 with skyautorotate 1, leaving the sky
    // turning for the rest of the level. The 4 deg/sec tumble worldspawn asks
    // for during the drop-pod descent is deliberate and is left alone; this is
    // only about the state the pod hands over on landing.
    if (!Q_stricmp(level.mapname, "mgu1m1")) {
        self->count |= 2;   // force the halt to be published even if the key was absent
        self->style = 0;
    }
}

//==========================================================
// [rerelease] target_camera
//
/*QUAKED target_camera (1 0 0) (-8 -8 -8) (8 8 8)
[Sam-KEX] Creates a camera path as seen in the N64 version.

"target"        the first path_corner to fly to
"pathtarget"    the entity the camera keeps pointed at while it flies
"speed"         units/sec along the path (a path_corner's own speed overrides)
"wait"          delay before the fly-through starts
"killtarget"    fired once the path runs out - this is how the N64-style maps
                reach their target_changelevel
"sounds"        CD track to switch to
*/

// id calls these "hackflags". A sweep of all 142 shipped BSPs finds NOT ONE
// entity setting the key, so every branch below that tests them is dead in the
// maps we ship - they are kept because the mechanism is cheap and faithful.
#define HACKFLAG_TELEPORT_OUT   2
#define HACKFLAG_SKIPPABLE      64
#define HACKFLAG_END_OF_UNIT    128

void G_SetClientFrame(edict_t *ent);
extern float xyspeed;

static void camera_lookat_pathtarget(edict_t *self, vec3_t origin, vec3_t dest)
{
    edict_t *pt;
    vec3_t  delta;
    float   yaw, pitch, d;

    if (!self->pathtarget)
        return;

    pt = G_Find(NULL, FOFS(targetname), self->pathtarget);
    if (!pt)
        return;

    VectorSubtract(pt->s.origin, origin, delta);

    d = delta[0] * delta[0] + delta[1] * delta[1];
    if (d == 0.0f) {
        yaw = 0.0f;
        pitch = (delta[2] > 0.0f) ? 90.0f : -90.0f;
    } else {
        yaw = RAD2DEG(atan2f(delta[1], delta[0]));
        pitch = RAD2DEG(atan2f(delta[2], sqrtf(d)));
    }

    dest[YAW] = yaw;
    dest[PITCH] = -pitch;
    dest[ROLL] = 0;
}

// keep every live client parked on the intermission view
static void camera_move_clients(void)
{
    edict_t *client;
    int     i;

    for (i = 0; i < game.maxclients; i++) {
        client = g_edicts + 1 + i;
        if (!client->inuse)
            continue;
        MoveClientToIntermission(client);
    }
}

void update_target_camera(edict_t *self)
{
    bool    do_skip = false;
    vec3_t  delta, newpos;
    float   frac;

    // only allow skipping after two seconds
    if ((self->hackflags & HACKFLAG_SKIPPABLE) && level.framenum > 2 * BASE_FRAMERATE) {
        int i;

        for (i = 0; i < game.maxclients; i++) {
            edict_t *client = g_edicts + 1 + i;
            if (!client->inuse || !client->client)
                continue;
            if (client->client->buttons & BUTTON_ANY) {
                do_skip = true;
                break;
            }
        }
    }

    if (!do_skip && self->movetarget) {
        self->moveinfo.remaining_distance -= (self->moveinfo.move_speed * FRAMETIME) * 0.8f;

        if (self->moveinfo.remaining_distance <= 0) {
            // arrived at this corner - hop to the next leg
            if (self->movetarget->hackflags & HACKFLAG_TELEPORT_OUT) {
                if (self->enemy) {
                    // no per-entity alpha in this protocol, so id's fade-out
                    // cannot be reproduced - the pop is all we get
                    self->enemy->s.event = EV_PLAYER_TELEPORT;
                    self->enemy->hackflags = HACKFLAG_TELEPORT_OUT;
                }
            }

            VectorCopy(self->movetarget->s.origin, self->s.origin);
            self->nextthink = level.framenum + self->movetarget->wait * BASE_FRAMERATE;

            if (self->movetarget->target) {
                self->movetarget = G_PickTarget(self->movetarget->target);

                if (self->movetarget) {
                    self->moveinfo.move_speed = self->movetarget->speed ? self->movetarget->speed : 55;
                    VectorSubtract(self->movetarget->s.origin, self->s.origin, delta);
                    self->moveinfo.remaining_distance = VectorLength(delta);
                    self->moveinfo.distance = self->moveinfo.remaining_distance;
                }
            } else {
                self->movetarget = NULL;
            }

            return;
        } else {
            frac = 1.0f - (self->moveinfo.remaining_distance / self->moveinfo.distance);

            VectorSubtract(self->movetarget->s.origin, self->s.origin, delta);
            VectorScale(delta, frac, delta);
            VectorAdd(self->s.origin, delta, newpos);

            camera_lookat_pathtarget(self, newpos, level.intermission_angle);
            VectorCopy(newpos, level.intermission_origin);

            camera_move_clients();
        }
    } else {
        if (self->killtarget) {
            edict_t *t = NULL;

            // destroy the stand-in player
            if (self->enemy) {
                G_FreeEdict(self->enemy);
                self->enemy = NULL;
            }

            // BeginIntermission refuses to run a second time while
            // intermission_framenum is set, so clear it across the killtarget -
            // and tell it the view is already placed so it leaves our shot alone
            level.intermission_framenum = 0;
            level.level_intermission_set = true;

            while ((t = G_Find(t, FOFS(targetname), self->killtarget))) {
                if (t->use)
                    t->use(t, self, self->activator);
            }

            level.intermission_framenum = level.framenum;

            // end of unit requires a wait; anything else leaves immediately
            if (level.changemap && !strchr(level.changemap, '*'))
                level.exitintermission = true;
        }

        self->think = NULL;
        return;
    }

    self->nextthink = level.framenum + 1;
}

// the dummy is a plain edict wearing the player's model, so it has to be
// animated by hand
void target_camera_dummy_think(edict_t *self)
{
    if (!self->owner || !self->owner->client) {
        G_FreeEdict(self);
        return;
    }

    // bit of a hack, but this will let the dummy move like a player
    self->client = self->owner->client;
    xyspeed = sqrtf(self->velocity[0] * self->velocity[0] + self->velocity[1] * self->velocity[1]);
    G_SetClientFrame(self);
    self->client = NULL;

    self->nextthink = level.framenum + 1;
}

void use_target_camera(edict_t *self, edict_t *other, edict_t *activator)
{
    vec3_t  delta;
    edict_t *client;
    int     i;

    if (self->sounds)
        gi.configstring(CS_CDTRACK, va("%d", self->sounds));

    if (!self->target)
        return;

    self->movetarget = G_PickTarget(self->target);
    if (!self->movetarget)
        return;

    level.intermission_framenum = level.framenum;
    level.exitintermission = 0;

    // spawn a fake player where the activator was standing, so the shot has
    // something of the player in it rather than an empty room
    if (activator->client) {
        edict_t *dummy = self->enemy = G_Spawn();

        dummy->owner = activator;
        dummy->clipmask = activator->clipmask;
        VectorCopy(activator->s.origin, dummy->s.origin);
        VectorCopy(activator->s.angles, dummy->s.angles);
        dummy->groundentity = activator->groundentity;
        dummy->groundentity_linkcount = dummy->groundentity ? dummy->groundentity->linkcount : 0;
        dummy->think = target_camera_dummy_think;
        dummy->nextthink = level.framenum + 1;
        dummy->solid = SOLID_BBOX;
        dummy->movetype = MOVETYPE_STEP;
        VectorCopy(activator->mins, dummy->mins);
        VectorCopy(activator->maxs, dummy->maxs);
        dummy->s.modelindex = dummy->s.modelindex2 = 255;   // use the client's own model
        dummy->s.skinnum = activator->s.skinnum;
        VectorCopy(activator->velocity, dummy->velocity);
        dummy->s.renderfx = RF_MINLIGHT;
        dummy->s.frame = activator->s.frame;
        gi.linkentity(dummy);
    }

    camera_lookat_pathtarget(self, self->s.origin, level.intermission_angle);
    VectorCopy(self->s.origin, level.intermission_origin);

    for (i = 0; i < game.maxclients; i++) {
        client = g_edicts + 1 + i;
        if (!client->inuse)
            continue;

        // respawn any dead clients, otherwise they sit at the death view
        if (client->health <= 0)
            respawn(client);

        MoveClientToIntermission(client);
    }

    self->activator = activator;
    self->think = update_target_camera;
    self->nextthink = level.framenum + self->wait * BASE_FRAMERATE;
    self->moveinfo.move_speed = self->speed;

    VectorSubtract(self->movetarget->s.origin, self->s.origin, delta);
    self->moveinfo.remaining_distance = VectorLength(delta);
    self->moveinfo.distance = self->moveinfo.remaining_distance;

    // HACKFLAG_END_OF_UNIT drives id's G_EndOfUnitMessage(), the unit-completion
    // stat screen. No shipped map sets the flag and this tree has no such
    // screen, so it is deliberately not reproduced.
}

void SP_target_camera(edict_t *self)
{
    if (deathmatch->value) {
        // auto-remove for deathmatch
        G_FreeEdict(self);
        return;
    }

    self->use = use_target_camera;
    self->svflags = SVF_NOCLIENT;
}

/*QUAKED target_healthbar (0 1 0) (-8 -8 -8) (8 8 8) PVS_ONLY

[rerelease] Puts a named health bar on the HUD for a monster.

"target"  the monster to watch
"message" its display name (already localized by ED_NewString)
"delay"   how long to keep the bar up after it dies

20 of these ship in the MGU maps. mgu6m3's is the one that names Modir - the
5.5x monster_shambler in the main chamber - and it is fired by a trigger_once at
the chamber door.

The bar itself is drawn by the client (SCR_DrawHealthBars) from STAT_HEALTH_BARS
and CS_HEALTH_BAR_NAME, gated on scr_health_bars, so the player can turn it off
without the game having to know.
*/

#define SPAWNFLAG_HEALTHBAR_PVS_ONLY    1

void use_target_healthbar(edict_t *ent, edict_t *other, edict_t *activator)
{
    edict_t *target = G_PickTarget(ent->target);
    int      i;

    if (!target || !(target->svflags & SVF_MONSTER)) {
        gi.dprintf("%s: target '%s' is not a monster\n", __func__,
                   ent->target ? ent->target : "(none)");
        G_FreeEdict(ent);
        return;
    }

    for (i = 0; i < MAX_HEALTH_BARS; i++) {
        if (level.health_bar_entities[i])
            continue;

        ent->enemy = target;
        ent->timestamp = 0;
        level.health_bar_entities[i] = ent;
        gi.configstring(CS_HEALTH_BAR_NAME, ent->message ? ent->message : "");
        return;
    }

    gi.dprintf("%s: too many health bars\n", __func__);
    G_FreeEdict(ent);
}

// One-shot sanity check a frame after spawn: the target has to exist and has to
// be a monster, and the map is the only place that can get that wrong.
void check_target_healthbar(edict_t *ent)
{
    edict_t *target = G_PickTarget(ent->target);

    if (!target || !(target->svflags & SVF_MONSTER)) {
        if (target)
            gi.dprintf("%s: target '%s' does not appear to be a monster\n",
                       __func__, ent->target);
        G_FreeEdict(ent);
        return;
    }

    ent->nextthink = 0;
}

void SP_target_healthbar(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    if (!self->target || !*self->target) {
        gi.dprintf("%s: no target\n", __func__);
        G_FreeEdict(self);
        return;
    }

    if (!self->message) {
        gi.dprintf("%s: no message\n", __func__);
        G_FreeEdict(self);
        return;
    }

    self->svflags = SVF_NOCLIENT;
    self->use = use_target_healthbar;
    self->think = check_target_healthbar;
    // the monsters spawn in the same pass as this entity, so the check cannot
    // run until the next frame
    self->nextthink = level.framenum + 1;
}
