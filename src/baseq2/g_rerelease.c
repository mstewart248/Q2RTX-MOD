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

/*QUAKED func_eye (0 1 0) ?
A camera-like eye brush that tracks the nearest player inside its cone of
vision. mguhub's monitor screens are these: 18 of them, all with
"eye_position" "24 0 -8" and no pathtarget.

The screen itself is a texture animation on the brush - s.frame 0 is the idle
face and s.frame 2 the alert one - which this renderer already drives per
entity (animate_material() in shader/vertex_buffer.h keys off the instance's
frame), so nothing new is needed on the client.

"pathtarget"    an info_notnull whose origin becomes the eye position
"eye_position"  eye position by hand, "forward right up" relative to the brush
"target"/"killtarget"/"delay"/"message"  fired the first time a player is seen
"radius"        detection radius, default 512
"speed"         degrees per second the eye turns on each axis, default 45
"vision_cone"   cone half-width as a dot product, default 0.5
"wait"          seconds to hold the last aim before returning to neutral
*/
#define SPAWNFLAG_FUNC_EYE_FIRED_TARGETS    0x00020000  // internal use only

void func_eye_think(edict_t *self)
{
    edict_t *closest_player = NULL;
    float   closest_dist = 0;
    vec3_t  fwd, rgt, up, eye_pos, dir, wanted_angles;
    int     i;

    // find the nearest player inside the cone
    for (i = 1; i <= game.maxclients; i++) {
        edict_t *player = &g_edicts[i];
        float   dist;

        if (!player->inuse || !player->client || player->health <= 0)
            continue;

        VectorSubtract(player->s.origin, self->s.origin, dir);
        dist = VectorNormalize(dir);

        if (DotProduct(dir, self->movedir) < self->yaw_speed)
            continue;

        if (dist >= self->dmg_radius)
            continue;

        if (!closest_player || dist < closest_dist) {
            closest_player = player;
            closest_dist = dist;
        }
    }

    self->enemy = closest_player;

    // Where the eye is looking from. move_origin holds a world-space offset by
    // the time we get here, but the rerelease reads its components back out as
    // forward/right/up scalars, so the offset ends up rotated. Kept as-is: it
    // is a 24 unit nudge on a decorative brush and changing it would aim these
    // differently than the retail game does.
    AngleVectors(self->s.angles, fwd, rgt, up);
    VectorCopy(self->s.origin, eye_pos);
    VectorMA(eye_pos, self->move_origin[0], fwd, eye_pos);
    VectorMA(eye_pos, self->move_origin[1], rgt, eye_pos);
    VectorMA(eye_pos, self->move_origin[2], up, eye_pos);

    if (self->enemy) {
        if (!(self->spawnflags & SPAWNFLAG_FUNC_EYE_FIRED_TARGETS)) {
            G_UseTargets(self, self->enemy);
            self->spawnflags |= SPAWNFLAG_FUNC_EYE_FIRED_TARGETS;
        }

        VectorSubtract(self->enemy->s.origin, eye_pos, dir);
        VectorNormalize(dir);
        vectoangles(dir, wanted_angles);

        self->s.frame = 2;
        self->timestamp = level.framenum + self->wait * BASE_FRAMERATE;
    } else if (self->timestamp <= level.framenum) {
        // return to neutral
        VectorCopy(self->move_angles, wanted_angles);
        self->s.frame = 0;
    } else {
        VectorCopy(self->s.angles, wanted_angles);
    }

    // pitch and yaw only
    for (i = 0; i < 2; i++) {
        float current = anglemod(self->s.angles[i]);
        float ideal = wanted_angles[i];
        float move;

        if (current == ideal)
            continue;

        move = ideal - current;

        if (ideal > current) {
            if (move >= 180)
                move -= 360;
        } else {
            if (move <= -180)
                move += 360;
        }

        if (move > 0) {
            if (move > self->speed)
                move = self->speed;
        } else {
            if (move < -self->speed)
                move = -self->speed;
        }

        self->s.angles[i] = anglemod(current + move);
    }

    self->nextthink = level.framenum + 1;
}

void func_eye_setup(edict_t *self)
{
    edict_t *eye_pos = G_PickTarget(self->pathtarget);

    if (!eye_pos)
        gi.dprintf("%s: bad pathtarget '%s'\n", self->classname, self->pathtarget);
    else
        VectorSubtract(eye_pos->s.origin, self->s.origin, self->move_origin);

    VectorCopy(self->move_origin, self->movedir);
    VectorNormalize(self->movedir);

    self->think = func_eye_think;
    self->nextthink = level.framenum + 1;
}

void SP_func_eye(edict_t *ent)
{
    ent->movetype = MOVETYPE_PUSH;
    ent->solid = SOLID_BSP;
    gi.setmodel(ent, ent->model);

    if (!st.radius)
        ent->dmg_radius = 512;
    else
        ent->dmg_radius = st.radius;

    if (!ent->speed)
        ent->speed = 45;

    if (!ent->yaw_speed)
        ent->yaw_speed = 0.5f;

    ent->speed *= FRAMETIME;
    VectorCopy(ent->s.angles, ent->move_angles);

    // the rerelease overrides the documented "wait" key
    ent->wait = 1.0f;

    if (ent->pathtarget) {
        ent->think = func_eye_setup;
        ent->nextthink = level.framenum + 1;
    } else {
        vec3_t right, up, move_origin;

        ent->think = func_eye_think;
        ent->nextthink = level.framenum + 1;

        AngleVectors(ent->move_angles, ent->movedir, right, up);

        VectorCopy(ent->move_origin, move_origin);
        VectorScale(ent->movedir, move_origin[0], ent->move_origin);
        VectorMA(ent->move_origin, move_origin[1], right, ent->move_origin);
        VectorMA(ent->move_origin, move_origin[2], up, ent->move_origin);
    }

    gi.linkentity(ent);
}

/*
=================
P_ToggleFlashlight

The flashlight itself is already a client feature here - V_Flashlight() in
src/client/view.c adds a textured spot light from the view. The game only owns
the on/off state, and ships it to the client in STAT_FLASHLIGHT, because every
one of this protocol's 32 EF_ bits is taken and there is no room for the
rerelease's EF_FLASHLIGHT.
=================
*/
void P_ToggleFlashlight(edict_t *ent, bool state)
{
    if (!!(ent->flags & FL_FLASHLIGHT) == state)
        return;

    ent->flags ^= FL_FLASHLIGHT;

    gi.sound(ent, CHAN_AUTO, gi.soundindex(ent->flags & FL_FLASHLIGHT ?
             "items/flashlight_on.wav" : "items/flashlight_off.wav"), 1, ATTN_STATIC, 0);
}

/*QUAKED trigger_flashlight (.5 .5 .5) ?
Turns the player's flashlight on or off as they move through the brush. The
MGU maps use these to light the player into a dark stretch and switch the light
back off on the way out; all 87 of them set "style", so the angle-driven mode
below is never actually taken by a shipping map.

"style"  1 always on, 2 always off, otherwise "angles" decide: moving with the
         trigger's facing turns it on, moving against it turns it off

The rerelease's CLIPPED spawnflag needs gi.clip(), which this engine does not
have. No rerelease map sets it, so it is not implemented.
*/
void trigger_flashlight_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (!other->client)
        return;

    if (self->style == 1) {
        P_ToggleFlashlight(other, true);
    } else if (self->style == 2) {
        P_ToggleFlashlight(other, false);
    } else if (VectorLengthSquared(other->velocity) > 32) {
        vec3_t forward;

        VectorCopy(other->velocity, forward);
        VectorNormalize(forward);
        P_ToggleFlashlight(other, DotProduct(forward, self->movedir) > 0);
    }
}

void SP_trigger_flashlight(edict_t *self)
{
    if (self->s.angles[YAW] == 0)
        self->s.angles[YAW] = 360;

    InitTrigger(self);
    self->touch = trigger_flashlight_touch;
    self->movedir[2] = st.height;

    gi.linkentity(self);
}

/*QUAKED misc_flare (1.0 1.0 0.0) (-32 -32 -32) (32 32 32) RED GREEN BLUE LOCK_ANGLE
A glowing corona sprite, hung on lamps and machinery all over the MGU maps
(186 of them). It is art, not a light source - the rerelease's flare emits
nothing, and neither does this one. dynamic_light is what actually lights those
rooms.

"rgba"              tint, "R G B A" as 0..255 or 0..1
"radius"            size multiplier, 0.3 to 4.2 in the shipping maps
"image"             the sprite, e.g. sprites/flare_03.tga
"fade_start_dist"   below this distance the flare is invisible
"fade_end_dist"     at this distance it is at full strength

Everything the client needs rides in the entity state, because there is no
per-entity scale or colour in this protocol:

    s.frame         image index (CS_IMAGES)
    s.skinnum       packed RGBA, straight from the "rgba" key
    s.modelindex4   size * FLARE_SCALE_UNIT
    s.modelindex2/3 fade start/end in units / FLARE_FADE_UNIT

modelindex2/3/4 are bytes on the wire, hence the two scale factors - see
SP_misc_flare for the ranges they have to cover.
*/
#define SPAWNFLAG_FLARE_RED         1
#define SPAWNFLAG_FLARE_GREEN       2
#define SPAWNFLAG_FLARE_BLUE        4
#define SPAWNFLAG_FLARE_LOCK_ANGLE  8

void misc_flare_use(edict_t *ent, edict_t *other, edict_t *activator)
{
    ent->svflags ^= SVF_NOCLIENT;
    gi.linkentity(ent);
}

static int flare_byte(float value, float unit)
{
    int i = (int)(value / unit + 0.5f);

    clamp(i, 0, 255);
    return i;
}

void SP_misc_flare(edict_t *ent)
{
    float scale;

    // modelindex has to be non-zero or the client drops the entity before it
    // ever looks at renderfx. Nothing is drawn from it - CL_AddPacketEntities
    // intercepts RF_FLARE first.
    ent->s.modelindex = 1;
    ent->s.renderfx = RF_FLARE;
    ent->movetype = MOVETYPE_NONE;
    ent->solid = SOLID_NOT;

    scale = st.radius ? st.radius : 1.0f;
    ent->s.modelindex4 = flare_byte(scale, FLARE_SCALE_UNIT);

    // The rerelease also sets RF_SHELL_* from these. Colour shells need
    // EF_COLOR_SHELL in this client and a flare never sets it, so the bits
    // would do nothing; every flare in every map carries an "rgba" saying the
    // same thing, and that is what tints it here.
    if (ent->spawnflags & SPAWNFLAG_FLARE_LOCK_ANGLE)
        ent->s.renderfx |= RF_FLARE_LOCK_ANGLE;

    if (st.image && *st.image) {
        char name[MAX_QPATH];

        // Two things to fix up. The maps are inconsistent about the path -
        // some say "sprites/flare_01.tga", some just "flare_04.tga" - and the
        // client precaches CS_IMAGES with R_RegisterPic2, which prefixes
        // "pics/" onto any name that is not absolute. A leading slash is the
        // engine's own escape hatch for that (see R_RegisterImage).
        if (!strchr(st.image, '/'))
            Q_snprintf(name, sizeof(name), "/sprites/%s", st.image);
        else
            Q_snprintf(name, sizeof(name), "/%s", st.image);

        ent->s.renderfx |= RF_CUSTOMSKIN;
        ent->s.frame = gi.imageindex(name);
    } else {
        ent->s.frame = gi.imageindex("/sprites/flare_01.tga");
    }

    // no "rgba" key means white
    if (!ent->s.skinnum)
        ent->s.skinnum = -1;

    ent->s.modelindex2 = flare_byte(st.fade_start_dist, FLARE_FADE_UNIT);
    ent->s.modelindex3 = flare_byte(st.fade_end_dist, FLARE_FADE_UNIT);

    VectorSet(ent->mins, -32, -32, -32);
    VectorSet(ent->maxs, 32, 32, 32);

    if (ent->targetname)
        ent->use = misc_flare_use;

    gi.linkentity(ent);
}

/*QUAKED target_light (1 0 0) (-8 -8 -8) (8 8 8) START_ON NO_LERP FLICKER
A dynamic light the map can switch on and off. All 38 in the rerelease maps are
"radius" 48, "rgba" "1 1 0.25 1", off at spawn and turned on by a trigger -
door and lift indicators, spark emitters and the like.

"radius"    light radius, default 150
"rgba"      colour, "R G B A" as 0..255 or 0..1

s.frame carries the radius and s.skinnum the packed colour; the client turns
that into a sphere light when it sees RF_CUSTOM_LIGHT.

The rerelease also lerps the colour along a lightstyle string, or toward a
"target"ed dynamic_light. That needs to read CS_LIGHTS back out of the server,
which this game API cannot do, and no map in scope uses either - none sets
"style", "target" or any spawnflag.
*/
#define SPAWNFLAG_TARGET_LIGHT_START_ON 1
#define SPAWNFLAG_TARGET_LIGHT_NO_LERP  2
#define SPAWNFLAG_TARGET_LIGHT_FLICKER  4

void target_light_flicker_think(edict_t *self)
{
    if (random() < 0.5f)
        self->svflags ^= SVF_NOCLIENT;

    self->nextthink = level.framenum + 1;
}

void target_light_use(edict_t *self, edict_t *other, edict_t *activator)
{
    self->health = !self->health;

    if (self->health)
        self->svflags &= ~SVF_NOCLIENT;
    else
        self->svflags |= SVF_NOCLIENT;

    if (!self->health) {
        self->think = NULL;
        self->nextthink = 0;
        return;
    }

    if (self->spawnflags & SPAWNFLAG_TARGET_LIGHT_FLICKER) {
        self->think = target_light_flicker_think;
        self->nextthink = level.framenum + 1;
    }
}

void SP_target_light(edict_t *self)
{
    self->s.modelindex = 1;     // see SP_misc_flare
    self->s.renderfx = RF_CUSTOM_LIGHT;
    self->s.frame = st.radius ? (int)st.radius : 150;
    self->svflags |= SVF_NOCLIENT;
    self->health = 0;

    if (!self->s.skinnum)
        self->s.skinnum = -1;

    if (self->spawnflags & SPAWNFLAG_TARGET_LIGHT_START_ON)
        target_light_use(self, self, self);

    self->use = target_light_use;

    gi.linkentity(self);
}

/*QUAKED dynamic_light (0 1 0) (-8 -8 -8) (8 8 8) START_OFF
The rerelease's real-time light. The light itself is built on the CLIENT, out of
the BSP entity lump - see src/client/dynamiclights.c - because the lights are
static and the path tracer already has the light types they need.

Most of them are scenery and this function just frees the edict, which is enough
to stop the server reporting the classname as unimplemented (69 lines a map on
mine1/mine2).

The ones with a "targetname" are different: a trigger switches them, and 11 of
the 15 across the in-scope maps START OFF. The rerelease does this by toggling
SVF_NOCLIENT on the light entity itself. We cannot - our lights are not entities
- so the game keeps a bookkeeping edict and publishes the on/off state as a bit
in CS_DYNAMICLIGHTS. Bit index is position among "targetname"ed dynamic_lights
in ENTITY LUMP ORDER, which is also this spawn function's call order, and is how
the client counts them too.
*/
#define SPAWNFLAG_DYNAMIC_LIGHT_START_OFF   1

static void dynamic_light_publish(void)
{
    gi.configstring(CS_DYNAMICLIGHTS, va("%x", level.dynamiclight_bits));
}

void dynamic_light_use(edict_t *self, edict_t *other, edict_t *activator)
{
    level.dynamiclight_bits ^= 1u << self->count;
    dynamic_light_publish();
}

void SP_dynamic_light(edict_t *self)
{
    // scenery: the client already has it, and nothing will ever ask it to change
    if (!self->targetname) {
        G_FreeEdict(self);
        return;
    }

    if (level.dynamiclight_count >= MAX_SWITCHABLE_DLIGHTS) {
        gi.dprintf("%s: more than %d switchable dynamic_light at %s\n",
                   self->classname, MAX_SWITCHABLE_DLIGHTS, vtos(self->s.origin));
        G_FreeEdict(self);
        return;
    }

    self->count = level.dynamiclight_count++;
    self->use = dynamic_light_use;
    self->svflags |= SVF_NOCLIENT;

    if (!(self->spawnflags & SPAWNFLAG_DYNAMIC_LIGHT_START_OFF))
        level.dynamiclight_bits |= 1u << self->count;

    dynamic_light_publish();
}
