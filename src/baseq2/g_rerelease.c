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

RERELEASE COSMETIC AND SCRIPTING ENTITIES

The classnames the rerelease maps use that vanilla Quake II has no spawn
function for. Kept together rather than scattered through g_target.c /
g_misc.c / g_func.c so it is obvious what came from where, and so the ones
that are still stubs are easy to find.

==============================================================================
*/

#include "g_local.h"

/*QUAKED target_poi (1 0 0) (-8 -8 -8) (8 8 8) NEAREST DUMMY DYNAMIC DISABLED
Marks the current objective. The rerelease draws a compass arrow at this spot;
this port tracks the objective correctly but does not draw it yet, so these are
inert on screen. See level.current_poi.

"count"     the objective stage this POI belongs to
"style"     priority within a team; lower wins
"image"     the compass icon
"team"      group several POIs; the best one is picked when the team is used
*/
#define SPAWNFLAG_POI_NEAREST   1
#define SPAWNFLAG_POI_DUMMY     2
#define SPAWNFLAG_POI_DYNAMIC   4
#define SPAWNFLAG_POI_DISABLED  8

/*
=================
distance_to_poi

The rerelease asks its bot navigation for a real path length so the "nearest"
POI is the nearest to *walk to*, not the nearest in a straight line. There is
no path system here, so this is the straight-line distance - which picks the
same POI in every case except a wall between two candidates.
=================
*/
static float distance_to_poi(vec3_t from, vec3_t to)
{
    vec3_t v;

    VectorSubtract(to, from, v);
    return VectorLength(v);
}

void target_poi_use(edict_t *ent, edict_t *other, edict_t *activator)
{
    // being used clears a DISABLED marker
    if (ent->spawnflags & SPAWNFLAG_POI_DISABLED)
        ent->spawnflags &= ~SPAWNFLAG_POI_DISABLED;

    // not part of the stage we have reached yet
    if (ent->count && level.current_poi_stage > ent->count)
        return;

    if (ent->team) {
        edict_t *poi_master = ent->teammaster;
        edict_t *poi;
        edict_t *dummy_fallback = NULL;
        float   best_distance = 1e30f;
        int     best_style = INT_MAX;

        // pick the best member of the team rather than this one
        ent = NULL;

        for (poi = poi_master; poi; poi = poi->teamchain) {
            float dist;

            if (poi->spawnflags & SPAWNFLAG_POI_DISABLED)
                continue;

            if (poi->spawnflags & SPAWNFLAG_POI_DUMMY) {
                dummy_fallback = poi;
                continue;
            }

            if (poi->count && level.current_poi_stage > poi->count)
                continue;

            if (poi->style > best_style)
                continue;

            dist = activator ? distance_to_poi(activator->s.origin, poi->s.origin) : 0;

            if ((poi_master->spawnflags & SPAWNFLAG_POI_NEAREST) && ent && dist > best_distance)
                continue;

            if (poi->style < best_style) {
                best_style = poi->style;
                if (poi_master->spawnflags & SPAWNFLAG_POI_NEAREST)
                    best_distance = dist;
                ent = poi;
                continue;
            }

            if (poi_master->spawnflags & SPAWNFLAG_POI_NEAREST) {
                if (dist < best_distance) {
                    best_distance = dist;
                    ent = poi;
                }
            } else {
                // order of appearance
                ent = poi;
            }
        }

        // not always an error - some maps rely on this
        if (!ent) {
            if (dummy_fallback && (dummy_fallback->spawnflags & SPAWNFLAG_POI_DYNAMIC))
                ent = dummy_fallback;
            else
                return;
        }
    } else if (ent->count) {
        if (level.current_poi_stage <= ent->count)
            level.current_poi_stage = ent->count;
        else
            return;
    }

    // a dummy on its own is a placeholder, not a destination
    if (!strcmp(ent->classname, "target_poi") &&
        (ent->spawnflags & SPAWNFLAG_POI_DUMMY) && !(ent->spawnflags & SPAWNFLAG_POI_DYNAMIC))
        return;

    level.valid_poi = true;
    VectorCopy(ent->s.origin, level.current_poi);
    level.current_poi_image = ent->noise_index;

    if (!strcmp(ent->classname, "target_poi") && (ent->spawnflags & SPAWNFLAG_POI_DYNAMIC)) {
        edict_t *m;

        level.current_dynamic_poi = NULL;

        // the dummy is the one member that is never freed, so it is what a
        // moving objective tracks
        for (m = ent->teammaster; m; m = m->teamchain) {
            if (m->spawnflags & SPAWNFLAG_POI_DUMMY) {
                level.current_dynamic_poi = m;
                break;
            }
        }

        if (!level.current_dynamic_poi)
            gi.dprintf("target_poi at %s is DYNAMIC but its team has no DUMMY\n", vtos(ent->s.origin));
    } else {
        level.current_dynamic_poi = NULL;
    }
}

void target_poi_setup(edict_t *self)
{
    if (self->team) {
        edict_t *m;

        // NEAREST and DYNAMIC are properties of the whole group
        if (self->spawnflags & (SPAWNFLAG_POI_NEAREST | SPAWNFLAG_POI_DYNAMIC)) {
            for (m = self->teammaster; m; m = m->teamchain)
                m->spawnflags |= self->spawnflags & (SPAWNFLAG_POI_NEAREST | SPAWNFLAG_POI_DYNAMIC);
        }
    }
}

void SP_target_poi(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    if (st.image)
        self->noise_index = gi.imageindex(st.image);
    else
        self->noise_index = gi.imageindex("friend");

    self->use = target_poi_use;
    self->svflags |= SVF_NOCLIENT;
    self->think = target_poi_setup;
    self->nextthink = level.framenum + 1;
}

/*QUAKED target_music (1 0 0) (-8 -8 -8) (8 8 8)
Change the music track when used.

"sounds"    the track number

The rerelease maps ask for tracks well above the original 21 - see
MAX_NUM_OGGTRACKS in the client, and the wrap in OGG_PlayTrack for what
happens when a track has no file.
*/
void use_target_music(edict_t *ent, edict_t *other, edict_t *activator)
{
    char buf[16];

    Q_snprintf(buf, sizeof(buf), "%d", ent->sounds);
    gi.configstring(CS_CDTRACK, buf);
}

void SP_target_music(edict_t *self)
{
    self->use = use_target_music;
}

/*QUAKED target_autosave (0 1 0) (-8 -8 -8) (8 8 8)
Auto save on command.

Rate limited, because several maps fire one from a trigger the player can walk
back and forth across.
*/
void use_target_autosave(edict_t *ent, edict_t *other, edict_t *activator)
{
    int min_frames = (int)(gi.cvar("g_auto_save_min_time", "60", 0)->value * BASE_FRAMERATE);

    if (deathmatch->value || coop->value)
        return;

    if (level.next_auto_save && level.framenum - level.next_auto_save < min_frames)
        return;

    level.next_auto_save = level.framenum;
    gi.AddCommandString("autosave\n");
}

void SP_target_autosave(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    self->use = use_target_autosave;
}

/*QUAKED target_story (0 1 0) (-8 -8 -8) (8 8 8)
Puts a line of story text on screen until cleared with an empty message.
*/
void use_target_story(edict_t *ent, edict_t *other, edict_t *activator)
{
    // The rerelease has a dedicated CONFIG_STORY slot and a HUD layer that
    // draws it. Until that exists here, show it the way every other map
    // message is shown so the text is not simply lost.
    if (ent->message && *ent->message) {
        if (activator && activator->client)
            gi.centerprintf(activator, "%s", ent->message);
        else
            gi.bprintf(PRINT_HIGH, "%s\n", ent->message);
    }
}

void SP_target_story(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    self->use = use_target_story;
}

/*QUAKED rotating_light (1 0 0) (-8 -8 -8) (8 8 8) START_OFF ALARM
"health"    if set, the light can be shot out (default 10)
*/
#define SPAWNFLAG_ROTATING_LIGHT_START_OFF  1
#define SPAWNFLAG_ROTATING_LIGHT_ALARM      2

void rotating_light_alarm(edict_t *self)
{
    if (self->spawnflags & SPAWNFLAG_ROTATING_LIGHT_START_OFF) {
        self->think = NULL;
        self->nextthink = 0;
    } else {
        gi.sound(self, CHAN_NO_PHS_ADD + CHAN_VOICE, self->moveinfo.sound_start, 1, ATTN_STATIC, 0);
        self->nextthink = level.framenum + 1 * BASE_FRAMERATE;
    }
}

void rotating_light_killed(edict_t *self, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_WELDING_SPARKS);
    gi.WriteByte(30);
    gi.WritePosition(self->s.origin);
    gi.WriteDir(vec3_origin);
    gi.WriteByte(0xe0 + (Q_rand() % 8));
    gi.multicast(self->s.origin, MULTICAST_PVS);

    self->s.effects &= ~EF_SPINNINGLIGHTS;
    self->use = NULL;

    self->think = G_FreeEdict;
    self->nextthink = level.framenum + 1;
}

void rotating_light_use(edict_t *self, edict_t *other, edict_t *activator)
{
    if (self->spawnflags & SPAWNFLAG_ROTATING_LIGHT_START_OFF) {
        self->spawnflags &= ~SPAWNFLAG_ROTATING_LIGHT_START_OFF;
        self->s.effects |= EF_SPINNINGLIGHTS;

        if (self->spawnflags & SPAWNFLAG_ROTATING_LIGHT_ALARM) {
            self->think = rotating_light_alarm;
            self->nextthink = level.framenum + 1;
        }
    } else {
        self->spawnflags |= SPAWNFLAG_ROTATING_LIGHT_START_OFF;
        self->s.effects &= ~EF_SPINNINGLIGHTS;
    }
}

void SP_rotating_light(edict_t *self)
{
    self->movetype = MOVETYPE_STOP;
    self->solid = SOLID_BBOX;

    self->s.modelindex = gi.modelindex("models/objects/light/tris.md2");
    self->s.frame = 0;

    self->use = rotating_light_use;

    if (self->spawnflags & SPAWNFLAG_ROTATING_LIGHT_START_OFF)
        self->s.effects &= ~EF_SPINNINGLIGHTS;
    else
        self->s.effects |= EF_SPINNINGLIGHTS;

    if (!self->speed)
        self->speed = 32;

    if (!self->health) {
        self->health = 10;
        self->max_health = self->health;
        self->die = rotating_light_killed;
        self->takedamage = DAMAGE_YES;
    } else {
        self->max_health = self->health;
        self->die = rotating_light_killed;
        self->takedamage = DAMAGE_YES;
    }

    if (self->spawnflags & SPAWNFLAG_ROTATING_LIGHT_ALARM) {
        self->moveinfo.sound_start = gi.soundindex("misc/alarm.wav");
        self->think = rotating_light_alarm;
        self->nextthink = level.framenum + 1;
    }

    gi.linkentity(self);
}

/*QUAKED misc_viper_missile (1 0 0) (-8 -8 -8) (8 8 8)
The bomb the Viper drops on its target when used.

"dmg"   blast damage, default 250
*/
void misc_viper_missile_use(edict_t *self, edict_t *other, edict_t *activator)
{
    vec3_t  forward, right, up;
    vec3_t  start, dir, vec;

    AngleVectors(self->s.angles, forward, right, up);

    self->enemy = G_Find(NULL, FOFS(targetname), self->target);

    if (!self->enemy) {
        gi.dprintf("%s: no target '%s'\n", self->classname, self->target ? self->target : "");
        return;
    }

    VectorCopy(self->enemy->s.origin, vec);
    VectorCopy(self->s.origin, start);
    VectorSubtract(vec, start, dir);
    VectorNormalize(dir);

    monster_fire_rocket(self, start, dir, self->dmg, 500, MZ2_CHICK_ROCKET_1);

    self->nextthink = level.framenum + 1;
    self->think = G_FreeEdict;
}

void SP_misc_viper_missile(edict_t *self)
{
    self->movetype = MOVETYPE_NONE;
    self->solid = SOLID_NOT;
    VectorSet(self->mins, -8, -8, -8);
    VectorSet(self->maxs, 8, 8, 8);

    if (!self->dmg)
        self->dmg = 250;

    self->s.modelindex = gi.modelindex("models/objects/bomb/tris.md2");

    self->use = misc_viper_missile_use;
    self->svflags |= SVF_NOCLIENT;

    gi.linkentity(self);
}

/*QUAKED misc_lavaball (1 0 0) (-8 -8 -8) (8 8 8)
Throws a burning ball out of the lava every few seconds.

"speed"     launch speed, default 185
*/
void lavaball_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other == self->owner)
        return;

    if (surf && (surf->flags & SURF_SKY)) {
        G_FreeEdict(self);
        return;
    }

    if (other->takedamage)
        T_Damage(other, self, self, vec3_origin, self->s.origin, vec3_origin, 20, 0, 0, MOD_EXPLOSIVE);

    if (gi.pointcontents(self->s.origin) & CONTENTS_LAVA)
        G_FreeEdict(self);
    else
        BecomeExplosion1(self);
}

void lavaball_fly(edict_t *self)
{
    edict_t *fireball = G_Spawn();

    // The rerelease marks these EF_FIREBALL. Every one of this protocol's 32
    // EF_ bits is already allocated, so there is no room for a new one -
    // EF_ROCKET is the closest thing the client already draws: a fire trail
    // with an orange dynamic light.
    fireball->s.effects = EF_ROCKET;
    fireball->s.renderfx = RF_MINLIGHT;
    fireball->solid = SOLID_BBOX;
    fireball->movetype = MOVETYPE_TOSS;
    fireball->clipmask = MASK_SHOT;
    fireball->velocity[0] = crandom() * 50;
    fireball->velocity[1] = crandom() * 50;
    fireball->velocity[2] = (self->speed * 1.75f) + (random() * 200);
    fireball->avelocity[0] = crandom() * 360;
    fireball->avelocity[1] = crandom() * 360;
    fireball->avelocity[2] = crandom() * 360;
    fireball->classname = "fireball";
    gi.setmodel(fireball, "models/objects/gibs/sm_meat/tris.md2");
    VectorCopy(self->s.origin, fireball->s.origin);
    fireball->nextthink = level.framenum + 5 * BASE_FRAMERATE;
    fireball->think = G_FreeEdict;
    fireball->touch = lavaball_touch;
    fireball->spawnflags = self->spawnflags;
    gi.linkentity(fireball);

    self->nextthink = level.framenum + (random() * 5) * BASE_FRAMERATE;
}

void SP_misc_lavaball(edict_t *self)
{
    self->classname = "fireball";
    self->think = lavaball_fly;
    self->nextthink = level.framenum + (random() * 5) * BASE_FRAMERATE;

    if (!self->speed)
        self->speed = 185;

    gi.modelindex("models/objects/gibs/sm_meat/tris.md2");
}

/*QUAKED misc_nuke (1 0 0) (-16 -16 -16) (16 16 16)
Kills everything that can be killed when used.
*/
void misc_nuke_use(edict_t *self, edict_t *other, edict_t *activator)
{
    edict_t *ent = NULL;

    while ((ent = G_Find(ent, FOFS(classname), "player")) != NULL) {
        if (ent->takedamage)
            T_Damage(ent, self, self, vec3_origin, ent->s.origin, vec3_origin,
                     100000, 0, DAMAGE_NO_PROTECTION, MOD_TELEFRAG);
    }

    for (ent = g_edicts + 1; ent < &g_edicts[globals.num_edicts]; ent++) {
        if (!ent->inuse || !ent->takedamage)
            continue;
        if (!(ent->svflags & SVF_MONSTER))
            continue;
        T_Damage(ent, self, self, vec3_origin, ent->s.origin, vec3_origin,
                 100000, 0, DAMAGE_NO_PROTECTION, MOD_TELEFRAG);
    }
}

void SP_misc_nuke(edict_t *self)
{
    self->use = misc_nuke_use;
}

/*QUAKED info_nav_lock (1 0 0) (-8 -8 -8) (8 8 8)
Tells the rerelease's bot navigation that a door/object gates progress. This
port has no bots, so it only needs to exist and do nothing - without a spawn
function the engine complains once per instance.
*/
void SP_info_nav_lock(edict_t *self)
{
    G_FreeEdict(self);
}
