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
// g_combat.c

#include "g_local.h"

/*
============
CanDamage

Returns true if the inflictor can directly damage the target.  Used for
explosions and melee attacks.
============
*/
/*
[rerelease] CanDamage from g_combat.cpp: traces run between the CENTRES of the
two boxes (when linked) rather than the origins, and a BSP target is first
tested at the point of its box closest to the blast. A monster's origin sits
at its feet-ish middle, so the origin traces caught on low cover that the
blast plainly clears.
*/
static bool CanDamage_rerelease(edict_t *targ, edict_t *inflictor)
{
    vec3_t  dest, inflictor_center, targ_center;
    trace_t trace;
    int     i;

    if (inflictor->area.prev) {
        VectorAdd(inflictor->absmin, inflictor->absmax, inflictor_center);
        VectorScale(inflictor_center, 0.5f, inflictor_center);
    } else {
        VectorCopy(inflictor->s.origin, inflictor_center);
    }

    if (targ->solid == SOLID_BSP) {
        M_ClosestPointToBox(inflictor_center, targ->absmin, targ->absmax, dest);

        trace = gi.trace(inflictor_center, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
        if (trace.fraction == 1.0f)
            return true;
    }

    if (targ->area.prev) {
        VectorAdd(targ->absmin, targ->absmax, targ_center);
        VectorScale(targ_center, 0.5f, targ_center);
    } else {
        VectorCopy(targ->s.origin, targ_center);
    }

    trace = gi.trace(inflictor_center, vec3_origin, vec3_origin, targ_center, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;

    for (i = 0; i < 4; i++) {
        VectorCopy(targ_center, dest);
        dest[0] += (i & 2) ? -15.0f : 15.0f;
        dest[1] += (i & 1) ? -15.0f : 15.0f;
        trace = gi.trace(inflictor_center, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
        if (trace.fraction == 1.0f)
            return true;
    }

    return false;
}

bool CanDamage(edict_t *targ, edict_t *inflictor)
{
    vec3_t  dest;
    trace_t trace;

    if (M_RereleaseGame())
        return CanDamage_rerelease(targ, inflictor);

// bmodels need special checking because their origin is 0,0,0
    if (targ->movetype == MOVETYPE_PUSH) {
        int i;

        // [rerelease] test against the closest point of the target's box,
        // not only its centre. A blast inside the box always counts: mgu4m1's
        // crate drop sets off its explosions inside a func_explosive that the
        // train is still sliding out of, so the centre trace hit the train
        // first and the crate never broke, leaving the player walled in.
        for (i = 0; i < 3; i++) {
            dest[i] = inflictor->s.origin[i];
            clamp(dest[i], targ->absmin[i], targ->absmax[i]);
        }
        if (VectorCompare(dest, inflictor->s.origin))
            return true;
        trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
        if (trace.fraction == 1.0f || trace.ent == targ)
            return true;

        VectorAdd(targ->absmin, targ->absmax, dest);
        VectorScale(dest, 0.5f, dest);
        trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
        if (trace.fraction == 1.0f)
            return true;
        if (trace.ent == targ)
            return true;
        return false;
    }

    trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, targ->s.origin, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;

    VectorCopy(targ->s.origin, dest);
    dest[0] += 15.0f;
    dest[1] += 15.0f;
    trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;

    VectorCopy(targ->s.origin, dest);
    dest[0] += 15.0f;
    dest[1] -= 15.0f;
    trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;

    VectorCopy(targ->s.origin, dest);
    dest[0] -= 15.0f;
    dest[1] += 15.0f;
    trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;

    VectorCopy(targ->s.origin, dest);
    dest[0] -= 15.0f;
    dest[1] -= 15.0f;
    trace = gi.trace(inflictor->s.origin, vec3_origin, vec3_origin, dest, inflictor, MASK_SOLID);
    if (trace.fraction == 1.0f)
        return true;


    return false;
}


/*
============
Killed
============
*/
qboolean Killed(edict_t *targ, edict_t *inflictor, edict_t *attacker, int damage, vec3_t point)
{
    bool        rerelease = M_RereleaseGame();
    bool        medic_c_spawn = false;
    edict_t     *medic_commander = NULL;
    int         medic_slots = 0;

    if (targ->health < -999)
        targ->health = -999;

    // [Paril-KEX] a medic that dies lets go of the corpse it was healing
    if (rerelease && (targ->svflags & SVF_MONSTER) && (targ->monsterinfo.aiflags & AI_MEDIC)) {
        if (targ->enemy && targ->enemy->inuse && (targ->enemy->svflags & SVF_MONSTER))
            cleanupHealTarget(targ->enemy);
        targ->monsterinfo.aiflags &= ~AI_MEDIC;
    }

    targ->enemy = attacker;

    // [Paril-KEX] a medic commander only gets his slots back once the monster
    // he summoned is GIBBED, since another medic can revive it - see the end of
    // this function. Remember the link now: die() may free the edict.
    if (rerelease && (targ->svflags & SVF_MONSTER) && (targ->monsterinfo.aiflags & AI_SPAWNED_MEDIC_C)) {
        medic_c_spawn = true;
        medic_commander = targ->monsterinfo.commander;
        medic_slots = targ->monsterinfo.monster_slots;
    }

    if ((targ->svflags & SVF_MONSTER) && (targ->deadflag != DEAD_DEAD)) {
//      targ->svflags |= SVF_DEADMONSTER;   // now treat as a different content type

        // ROGUE - hand the slot back to whoever summoned this one, so a
        // commander can keep replacing its escort as the player kills it.
        // [rerelease] not the medic commander's - that waits for the gib.
        if ((targ->monsterinfo.aiflags & AI_SPAWNED_MASK) && targ->monsterinfo.commander &&
            !medic_c_spawn) {
            edict_t *commander = targ->monsterinfo.commander;
            if (commander->inuse && commander->monsterinfo.monster_used > 0)
                commander->monsterinfo.monster_used--;
        }

        // AI_DO_NOT_COUNT covers summoned and medic-resurrected monsters; they
        // must not inflate the level's kill total or the coop score
        if (!(targ->monsterinfo.aiflags & (AI_GOOD_GUY | AI_DO_NOT_COUNT))) {
            level.killed_monsters++;
            if (coop->value && attacker->client)
                attacker->client->resp.score++;
        }

        // medics won't heal monsters that they kill themselves
        if (!(targ->monsterinfo.aiflags & AI_GOOD_GUY) && strcmp(attacker->classname, "monster_medic") == 0)
            targ->owner = attacker;
    }

    if ((targ->movetype == MOVETYPE_PUSH || targ->movetype == MOVETYPE_STOP || targ->movetype == MOVETYPE_NONE) &&
        !(rerelease && (targ->svflags & SVF_MONSTER))) {
        // doors, triggers, etc - never gib, so report "did not gib". This used
        // to be a bare `return;` in a qboolean function, which handed the
        // caller an indeterminate value; under ludicrous gibs that decides
        // whether the target becomes the re-kill candidate.
        targ->die(targ, inflictor, attacker, damage, point);
        return qfalse;
    }

    if ((targ->svflags & SVF_MONSTER) && (targ->deadflag != DEAD_DEAD)) {
        targ->touch = NULL;
        monster_death_use(targ);
    }

    targ->die(targ, inflictor, attacker, damage, point);

    // [Paril-KEX] gibbed (or freed outright), so no medic can bring it back:
    // refund the strength it cost its commander. monster_slots on a SUMMONED
    // monster is that strength, set by the medic commander when it spawned it.
    // (0 there would mean a summon from before the medic set it; count 1, the
    // classic refund, rather than leak the slot.)
    if (medic_c_spawn && (!targ->inuse || targ->health <= targ->gib_health)) {
        if (medic_commander && medic_commander->inuse && medic_commander->classname &&
            !strcmp(medic_commander->classname, "monster_medic_commander")) {
            medic_commander->monsterinfo.monster_used -= (medic_slots > 0) ? medic_slots : 1;
            if (medic_commander->monsterinfo.monster_used < 0)
                medic_commander->monsterinfo.monster_used = 0;
        }
        if (targ->inuse)
            targ->monsterinfo.commander = NULL;
    }

    // did it come apart? the caller uses this to decide whether the corpse is
    // worth tracking for further damage
    return (!targ->inuse || targ->health <= targ->gib_health) ? qtrue : qfalse;
}


/*
================
SpawnDamage
================
*/
void SpawnDamage(int type, const vec3_t origin, const vec3_t normal, int damage)
{
    if (damage > 255)
        damage = 255;
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(type);
//  gi.WriteByte (damage);
    gi.WritePosition(origin);
    gi.WriteDir(normal);
    gi.multicast(origin, MULTICAST_PVS);
}


/*
============
T_Damage

targ        entity that is being damaged
inflictor   entity that is causing the damage
attacker    entity that caused the inflictor to damage targ
    example: targ=monster, inflictor=rocket, attacker=player

dir         direction of the attack
point       point at which the damage is being inflicted
normal      normal vector from that point
damage      amount of damage being inflicted
knockback   force to be applied against targ as a result of the damage

dflags      these flags are used to control how T_Damage works
    DAMAGE_RADIUS           damage was indirect (from a nearby explosion)
    DAMAGE_NO_ARMOR         armor does not protect from this damage
    DAMAGE_ENERGY           damage is from an energy based weapon
    DAMAGE_NO_KNOCKBACK     do not affect velocity, just view angles
    DAMAGE_BULLET           damage is from a bullet (used for ricochets)
    DAMAGE_NO_PROTECTION    kills godmode, armor, everything
============
*/
static int CheckPowerArmor(edict_t *ent, const vec3_t point, const vec3_t normal, int damage, int dflags)
{
    gclient_t   *client;
    int         save;
    int         power_armor_type;
    int         index;
    int         damagePerCell;
    int         pa_te_type;
    int         power;
    int         power_used;

    if (!damage)
        return 0;

    client = ent->client;

    if (dflags & (DAMAGE_NO_ARMOR | DAMAGE_NO_POWER_ARMOR))
        return 0;

    // [rerelease] power armor does nothing for the dead
    if (M_RereleaseGame() && ent->health <= 0)
        return 0;

    index = 0;  // shut up gcc

    if (client) {
        power_armor_type = PowerArmorType(ent);
        if (power_armor_type != POWER_ARMOR_NONE) {
            index = ITEM_INDEX(FindItem("Cells"));
            power = client->pers.inventory[index];
        }
    } else if (ent->svflags & SVF_MONSTER) {
        power_armor_type = ent->monsterinfo.power_armor_type;
        power = ent->monsterinfo.power_armor_power;
    } else
        return 0;

    if (power_armor_type == POWER_ARMOR_NONE)
        return 0;
    if (!power)
        return 0;

    if (power_armor_type == POWER_ARMOR_SCREEN) {
        vec3_t      vec;
        float       dot;
        vec3_t      forward;

        // only works if damage point is in front
        AngleVectors(ent->s.angles, forward, NULL, NULL);
        VectorSubtract(point, ent->s.origin, vec);
        VectorNormalize(vec);
        dot = DotProduct(vec, forward);
        if (dot <= 0.3f)
            return 0;

        damagePerCell = 1;
        pa_te_type = TE_SCREEN_SPARKS;
        damage = damage / 3;
    } else {
        damagePerCell = 2;
        pa_te_type = TE_SHIELD_SPARKS;
        damage = (2 * damage) / 3;
    }

    if (M_RereleaseGame()) {
        // Paril: fix small amounts of damage not being absorbed
        if (damage < 1)
            damage = 1;

        save = power * damagePerCell;
        if (!save)
            return 0;

        // [Paril-KEX] energy damage should do more to power armor
        if (dflags & DAMAGE_ENERGY) {
            save /= 2;
            if (save < 1)
                save = 1;
        }

        if (save > damage)
            save = damage;

        if (dflags & DAMAGE_ENERGY)
            power_used = (save / damagePerCell) * 2;
        else
            power_used = save / damagePerCell;
        if (power_used < 1)
            power_used = 1;

        SpawnDamage(pa_te_type, point, normal, save);
        ent->powerarmor_framenum = level.framenum + 0.2f * BASE_FRAMERATE;

        // Paril: power armor always uses damagePerCell even if it only does a
        // single point of damage
        if (power_used < damagePerCell)
            power_used = damagePerCell;
        if (power_used > power)
            power_used = power;

        if (client) {
            client->pers.inventory[index] -= power_used;
        } else {
            ent->monsterinfo.power_armor_power -= power_used;

            // a monster's screen/shield giving out is announced
            if (!ent->monsterinfo.power_armor_power)
                gi.sound(ent, CHAN_AUTO, gi.soundindex("misc/mon_power2.wav"), 1, ATTN_NORM, 0);
        }
        return save;
    }

    save = power * damagePerCell;
    if (!save)
        return 0;
    if (save > damage)
        save = damage;

    SpawnDamage(pa_te_type, point, normal, save);
    ent->powerarmor_framenum = level.framenum + 0.2f * BASE_FRAMERATE;

    power_used = save / damagePerCell;

    if (client)
        client->pers.inventory[index] -= power_used;
    else
        ent->monsterinfo.power_armor_power -= power_used;
    return save;
}

static int CheckArmor(edict_t *ent, const vec3_t point, const vec3_t normal, int damage, int te_sparks, int dflags)
{
    gclient_t   *client;
    int         save;
    int         index;
    gitem_t     *armor;

    if (!damage)
        return 0;

    client = ent->client;

    // [rerelease] a monster may wear ordinary armour too - only the turret
    // does, but the accounting is the player's, just held in monsterinfo
    // instead of an inventory slot.
    if (!client && !(M_RereleaseGame() && ent->monsterinfo.armor_power > 0))
        return 0;

    if (dflags & (DAMAGE_NO_ARMOR | DAMAGE_NO_REG_ARMOR))
        return 0;

    index = client ? ArmorIndex(ent) : ent->monsterinfo.armor_type;
    if (!index)
        return 0;

    armor = GetItemByIndex(index);

    if (dflags & DAMAGE_ENERGY)
        save = ceil(((gitem_armor_t *)armor->info)->energy_protection * damage);
    else
        save = ceil(((gitem_armor_t *)armor->info)->normal_protection * damage);

    if (client) {
        if (save >= client->pers.inventory[index])
            save = client->pers.inventory[index];
    } else if (save >= ent->monsterinfo.armor_power) {
        save = ent->monsterinfo.armor_power;
    }

    if (!save)
        return 0;

    if (client) {
        client->pers.inventory[index] -= save;
    } else {
        ent->monsterinfo.armor_power -= save;
        if (!ent->monsterinfo.armor_power)
            ent->monsterinfo.armor_type = 0;
    }

    SpawnDamage(te_sparks, point, normal, save);

    return save;
}

/*
=================
M_ReactToDamage_rerelease

[rerelease] M_ReactToDamage from g_combat.cpp. A monster that just switched
targets because it was hurt ignores further damage for 3-5 seconds, one held
on its enemy by target_anger stays on it above a third of its health, and a
medic stays on its patient above a quarter. Nothing happens when the attacker
already is the enemy, and the "help our buddy" case no longer re-runs
FoundTarget on the enemy it already has. (The tesla_mine targeting at the top
of id's version needs rogue's bad-area tracking, which this tree lacks.)
=================
*/
static void M_SwitchEnemyMedicCleanup(edict_t *targ)
{
    // [Paril-KEX] a medic that turns on someone lets go of its patient
    if ((targ->svflags & SVF_MONSTER) && (targ->monsterinfo.aiflags & AI_MEDIC)) {
        if (targ->enemy && targ->enemy->inuse && (targ->enemy->svflags & SVF_MONSTER))
            cleanupHealTarget(targ->enemy);
        targ->monsterinfo.aiflags &= ~AI_MEDIC;
    }
}

static void M_ReactToDamage_rerelease(edict_t *targ, edict_t *attacker, edict_t *inflictor)
{
    if (!(attacker->client) && !(attacker->svflags & SVF_MONSTER))
        return;

    if (attacker == targ || attacker == targ->enemy)
        return;

    // dead monsters, like misc_deadsoldier, don't have AI functions
    if (targ->svflags & SVF_DEADMONSTER)
        return;

    // if we are a good guy monster and our attacker is a player
    // or another good guy, do not get mad at them
    if (targ->monsterinfo.aiflags & AI_GOOD_GUY) {
        if (attacker->client || (attacker->monsterinfo.aiflags & AI_GOOD_GUY))
            return;
    }

    // PGM - if we're currently mad at something a target_anger made us mad at,
    // ignore damage
    if (targ->enemy && (targ->monsterinfo.aiflags & AI_TARGET_ANGER)) {
        // make sure whatever we were pissed at is still around.
        if (targ->enemy->inuse) {
            float percentHealth = (float)targ->health / (float)targ->max_health;

            if (percentHealth > 0.33f)
                return;
        }

        // remove the target anger flag
        targ->monsterinfo.aiflags &= ~AI_TARGET_ANGER;
    }

    // we recently switched from reacting to damage, don't do it
    if (targ->monsterinfo.react_to_damage_framenum > level.framenum)
        return;

    // PMM - if we're healing someone, try to stay with them
    if (targ->enemy && (targ->monsterinfo.aiflags & AI_MEDIC)) {
        float percentHealth = (float)targ->health / (float)targ->max_health;

        // ignore it some of the time
        if (targ->enemy->inuse && percentHealth > 0.25f)
            return;

        // remove the medic flag
        cleanupHealTarget(targ->enemy);
        targ->monsterinfo.aiflags &= ~AI_MEDIC;
    }

    // we now know that we are not both good guys
    targ->monsterinfo.react_to_damage_framenum = level.framenum + (int)((3.0f + 2.0f * random()) * BASE_FRAMERATE);

    // if attacker is a client, get mad at them because he's good and we're not
    if (attacker->client) {
        targ->monsterinfo.aiflags &= ~AI_SOUND_TARGET;

        // this can only happen in coop (both new and old enemies are clients)
        // only switch if can't see the current enemy
        if (targ->enemy != attacker) {
            if (targ->enemy && targ->enemy->client) {
                if (visible(targ, targ->enemy)) {
                    targ->oldenemy = attacker;
                    return;
                }
                targ->oldenemy = targ->enemy;
            }

            M_SwitchEnemyMedicCleanup(targ);

            targ->enemy = attacker;
            if (!(targ->monsterinfo.aiflags & AI_DUCKED))
                FoundTarget(targ);
        }
        return;
    }

    if (attacker->enemy == targ ||  // if they *meant* to shoot us, then shoot back
        // it's the same base (walk/swim/fly) type and both don't ignore shots,
        // get mad at them
        (((targ->flags & (FL_FLY | FL_SWIM)) == (attacker->flags & (FL_FLY | FL_SWIM))) &&
         strcmp(targ->classname, attacker->classname) != 0 &&
         !(attacker->monsterinfo.aiflags & AI_IGNORE_SHOTS) &&
         !(targ->monsterinfo.aiflags & AI_IGNORE_SHOTS))) {
        if (targ->enemy != attacker) {
            M_SwitchEnemyMedicCleanup(targ);

            if (targ->enemy && targ->enemy->client)
                targ->oldenemy = targ->enemy;
            targ->enemy = attacker;
            if (!(targ->monsterinfo.aiflags & AI_DUCKED))
                FoundTarget(targ);
        }
    }
    // otherwise get mad at whoever they are mad at (help our buddy) unless it is us!
    else if (attacker->enemy && attacker->enemy != targ && targ->enemy != attacker->enemy) {
        M_SwitchEnemyMedicCleanup(targ);

        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker->enemy;
        if (!(targ->monsterinfo.aiflags & AI_DUCKED))
            FoundTarget(targ);
    }
}

void M_ReactToDamage(edict_t *targ, edict_t *attacker, edict_t *inflictor)
{
    if (M_RereleaseGame()) {
        M_ReactToDamage_rerelease(targ, attacker, inflictor);
        return;
    }

    if (!(attacker->client) && !(attacker->svflags & SVF_MONSTER))
        return;

    if (attacker == targ || attacker == targ->enemy)
        return;

    // dead monsters, like misc_deadsoldier, don't have AI functions, but 
    // M_ReactToDamage might still be called on them
    if (targ->svflags & SVF_DEADMONSTER)
        return;

    // if we are a good guy monster and our attacker is a player
    // or another good guy, do not get mad at them
    if (targ->monsterinfo.aiflags & AI_GOOD_GUY) {
        if (attacker->client || (attacker->monsterinfo.aiflags & AI_GOOD_GUY))
            return;
    }

    // we now know that we are not both good guys

    // if attacker is a client, get mad at them because he's good and we're not
    if (attacker->client) {
        targ->monsterinfo.aiflags &= ~AI_SOUND_TARGET;

        // this can only happen in coop (both new and old enemies are clients)
        // only switch if can't see the current enemy
        if (targ->enemy && targ->enemy->client) {
            if (visible(targ, targ->enemy)) {
                targ->oldenemy = attacker;
                return;
            }
            targ->oldenemy = targ->enemy;
        }
        targ->enemy = attacker;
        if (!(targ->monsterinfo.aiflags & AI_DUCKED))
            FoundTarget(targ);
        return;
    }

    // It's the same base (walk/swim/fly) type and a different classname, so get
    // mad at them - unless the shooter is one of the ones that sprays too much
    // to hold responsible.
    //
    // [rerelease] id replaced the hardcoded classname list with AI_IGNORE_SHOTS,
    // which is both cleaner and wider: it also covers boss2, the carrier, the
    // medic commander, the shambler and the turret, and it is checked in BOTH
    // directions, so a flagged monster is neither blamed nor provoked.
    if (((targ->flags & (FL_FLY | FL_SWIM)) == (attacker->flags & (FL_FLY | FL_SWIM))) &&
        (strcmp(targ->classname, attacker->classname) != 0) &&
        (M_RereleaseGame()
         ? (!(attacker->monsterinfo.aiflags & AI_IGNORE_SHOTS) &&
            !(targ->monsterinfo.aiflags & AI_IGNORE_SHOTS))
         : ((strcmp(attacker->classname, "monster_tank") != 0) &&
            (strcmp(attacker->classname, "monster_supertank") != 0) &&
            (strcmp(attacker->classname, "monster_makron") != 0) &&
            (strcmp(attacker->classname, "monster_jorg") != 0)))) {
        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker;
        if (!(targ->monsterinfo.aiflags & AI_DUCKED))
            FoundTarget(targ);
    }
    // if they *meant* to shoot us, then shoot back
    else if (attacker->enemy == targ) {
        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker;
        if (!(targ->monsterinfo.aiflags & AI_DUCKED))
            FoundTarget(targ);
    }
    // otherwise get mad at whoever they are mad at (help our buddy) unless it is us!
    else if (attacker->enemy && attacker->enemy != targ) {
        if (targ->enemy && targ->enemy->client)
            targ->oldenemy = targ->enemy;
        targ->enemy = attacker->enemy;
        if (!(targ->monsterinfo.aiflags & AI_DUCKED))
            FoundTarget(targ);
    }
}

bool CheckTeamDamage(edict_t *targ, edict_t *attacker)
{
    //FIXME make the next line real and uncomment this block
    // if ((ability to damage a teammate == OFF) && (targ's team == attacker's team))
    return false;
}

void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, const vec3_t dir, vec3_t point, const vec3_t normal, int damage, int knockback, int dflags, int mod)
{
    gclient_t   *client;
    int         take;
    int         save;
    int         asave;
    int         psave;
    int         te_sparks;
    bool        sphere_notified = false;    // PGM
    // Ludicrous gibs only: which corpse was last re-killed, and when. See the
    // staged-death block further down.
    static edict_t *lastTarget = NULL;
    static float lastKillTime = 0.0f;

    if (!targ->takedamage)
        return;

    // easy mode takes half damage
    if (skill->value == 0 && deathmatch->value == 0 && targ->client) {
        damage *= 0.5f;
        if (!damage)
            damage = 1;
    }

    // friendly fire avoidance
    // if enabled you can't hurt teammates (but you can hurt yourself)
    // knockback still occurs
    if ((targ != attacker) && ((deathmatch->value && ((int)(dmflags->value) & (DF_MODELTEAMS | DF_SKINTEAMS))) || (coop->value && targ->client))) {
        if (OnSameTeam(targ, attacker)) {
            if ((int)(dmflags->value) & DF_NO_FRIENDLY_FIRE)
                damage = 0;
            else
                mod |= MOD_FRIENDLY_FIRE;
        }
    }
    meansOfDeath = mod;

    client = targ->client;

    // ROGUE/[rerelease] a player with a defender sphere out takes half damage
    if (M_RereleaseGame() && damage && client && client->owned_sphere &&
        client->owned_sphere->spawnflags == SPHERE_DEFENDER) {
        damage /= 2;
        if (!damage)
            damage = 1;
    }

    if (dflags & DAMAGE_BULLET)
        te_sparks = TE_BULLET_SPARKS;
    else
        te_sparks = TE_SPARKS;

// bonus damage for suprising a monster
    if (M_RereleaseGame()) {
        // [rerelease] every hit of the frame the surprise landed on counts -
        // all of a shotgun blast, not just the first pellet
        if (!(dflags & DAMAGE_RADIUS) && (targ->svflags & SVF_MONSTER) && (attacker->client) &&
            (!targ->enemy || targ->monsterinfo.surprise_framenum == level.framenum) && (targ->health > 0)) {
            damage *= 2;
            targ->monsterinfo.surprise_framenum = level.framenum;
        }
    } else if (!(dflags & DAMAGE_RADIUS) && (targ->svflags & SVF_MONSTER) && (attacker->client) && (!targ->enemy) && (targ->health > 0))
        damage *= 2;

    if (targ->flags & FL_NO_KNOCKBACK)
        knockback = 0;
    // [rerelease] a dead body only takes the knockback of the frame it died on
    if ((targ->flags & FL_ALIVE_KNOCKBACK_ONLY) &&
        (!targ->deadflag || targ->dead_framenum != level.framenum))
        knockback = 0;

// figure momentum add
    if (!(dflags & DAMAGE_NO_KNOCKBACK)) {
        if ((knockback) && (targ->movetype != MOVETYPE_NONE) && (targ->movetype != MOVETYPE_BOUNCE) && (targ->movetype != MOVETYPE_PUSH) && (targ->movetype != MOVETYPE_STOP)) {
            vec3_t  kvel;
            float   mass;

            if (targ->mass < 50)
                mass = 50;
            else
                mass = targ->mass;

            VectorNormalize2(dir, kvel);

            if (targ->client  && attacker == targ)
                VectorScale(kvel, 1600.0f * (float)knockback / mass, kvel);  // the rocket jump hack...
            else
                VectorScale(kvel, 500.0f * (float)knockback / mass, kvel);

            VectorAdd(targ->velocity, kvel, targ->velocity);
        }
    }

    take = damage;
    save = 0;

    // check for godmode
    if ((targ->flags & FL_GODMODE) && !(dflags & DAMAGE_NO_PROTECTION)) {
        take = 0;
        save = damage;
        SpawnDamage(te_sparks, point, normal, save);
    }

    // check for invincibility
    // ROGUE - [rerelease] a monster has one too (the widow mirrors the player's)
    if (((client && client->invincible_framenum > level.framenum) ||
         (M_RereleaseGame() && (targ->svflags & SVF_MONSTER) &&
          targ->monsterinfo.invincible_framenum > level.framenum)) &&
        !(dflags & DAMAGE_NO_PROTECTION)) {
        if (targ->pain_debounce_framenum < level.framenum) {
            gi.sound(targ, CHAN_ITEM, gi.soundindex("items/protect4.wav"), 1, ATTN_NORM, 0);
            targ->pain_debounce_framenum = level.framenum + 2 * BASE_FRAMERATE;
        }
        take = 0;
        save = damage;
    }

    psave = CheckPowerArmor(targ, point, normal, take, dflags);
    take -= psave;

    asave = CheckArmor(targ, point, normal, take, te_sparks, dflags);
    take -= asave;

    // DAMAGE_DESTROY_ARMOR chews through the armor *and* still lands in full on
    // health - rogue used it for the chainfist and DPU rounds. The armor loss
    // above already happened; this just puts the damage back.
    if (dflags & DAMAGE_DESTROY_ARMOR) {
        if (!(targ->flags & FL_GODMODE) && !(dflags & DAMAGE_NO_PROTECTION) &&
            !(client && client->invincible_framenum > level.framenum))
            take = damage;
    }

    //treat cheat/powerup savings the same as armor
    asave += save;

    // team damage avoidance
    if (!(dflags & DAMAGE_NO_PROTECTION) && CheckTeamDamage(targ, attacker))
        return;

// do the damage
    if (take) {
        if (targ->flags & FL_MECHANICAL)
        {
            // ROGUE - a turret sparks rather than bleeds
            SpawnDamage(TE_ELECTRIC_SPARKS, point, normal, take);
        }
        else if ((targ->svflags & SVF_MONSTER) || (client))
        {
            // NORMAL, not dir.  This was briefly changed to `dir` to give the
            // spray some directionality and that was simply wrong: `dir` is the
            // bullet's direction of travel, so the blood was thrown THROUGH the
            // monster and out the far side - and CL_BloodParticleEffect scatters
            // it up to 310 units, which buried the whole spray in whatever was
            // behind the target.  That is why the blood looked like it had
            // disappeared.  `normal` is the surface normal, pointing back out
            // towards the shooter, and is what the rerelease passes too
            // (src/rerelease/g_combat.cpp: SpawnDamage(TE_BLOOD, point, normal,
            // take)).
            SpawnDamage(TE_BLOOD, point, normal, take);
        }
        else
            SpawnDamage(te_sparks, point, normal, take);


        targ->health = targ->health - take;

        // PGM - spheres need to know who to shoot at
        if (M_RereleaseGame() && client && client->owned_sphere) {
            sphere_notified = true;
            if (client->owned_sphere->pain)
                client->owned_sphere->pain(client->owned_sphere, attacker, 0, 0);
        }

        if (targ->health <= 0) {
            // [rerelease] a monster's body keeps that frame's knockback
            // (FL_ALIVE_KNOCKBACK_ONLY); the classic body takes none after the
            // killing hit. Players keep FL_NO_KNOCKBACK, which their respawn
            // clears.
            if (M_RereleaseGame() && (targ->svflags & SVF_MONSTER)) {
                targ->flags |= FL_ALIVE_KNOCKBACK_ONLY;
                targ->dead_framenum = level.framenum;
                // a death supersedes any pain still pending this frame
                targ->monsterinfo.damage_blood = 0;
                targ->monsterinfo.damage_knockback = 0;
                targ->monsterinfo.damage_attacker = NULL;
            } else if ((targ->svflags & SVF_MONSTER) || (client))
                targ->flags |= FL_NO_KNOCKBACK;

            if (!LUDICROUS_GIBS()) {
                // stock behaviour: a thing dies exactly once
                Killed(targ, inflictor, attacker, take, point);
                return;
            }

            // LUDICROUS GIBS - a corpse keeps taking damage and can be torn
            // apart in stages. Killed() reports whether the target gibbed, and
            // death_count drives how far each monster's die function escalates.
            // Re-killing the same target is rate limited to twice a second,
            // except for the rapid-fire weapons and monster blaster bolts,
            // which are allowed through every hit so sustained fire keeps
            // chewing on the body.
            if (lastTarget == NULL) {
                if (Killed(targ, inflictor, attacker, take, point)) {
                    lastTarget = targ;
                    targ->death_count++;
                    lastKillTime = level.time;
                }
            }
            else if (lastTarget == targ) {
                float diff = level.time - lastKillTime;

                if (diff > 0.5f) {
                    Killed(targ, inflictor, attacker, take, point);
                    lastTarget = targ;
                    targ->death_count++;
                    lastKillTime = level.time;
                }
                else {
                    if (inflictor->client != NULL) {
                        if (!Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_machinegun") || !Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_chaingun") ||
                            !Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_supershotgun") || !Q_stricmp(inflictor->client->pers.weapon->classname, "weapon_shotgun")) {
                            Killed(targ, inflictor, attacker, take, point);
                        }
                    }
                    else if (!Q_stricmp(inflictor->classname, "bolt")) {
                        Killed(targ, inflictor, attacker, take, point);
                    }
                }
            }
            else {
                if (Killed(targ, inflictor, attacker, take, point)) {
                    targ->death_count++;
                    lastTarget = targ;
                    lastKillTime = level.time;
                }
            }
            return;
        }
    }

    // PGM - spheres need to know who to shoot at
    if (M_RereleaseGame() && !sphere_notified && client && client->owned_sphere) {
        sphere_notified = true;
        if (client->owned_sphere->pain)
            client->owned_sphere->pain(client->owned_sphere, attacker, 0, 0);
    }

    if ((targ->svflags & SVF_MONSTER) && M_RereleaseGame()) {
        // [rerelease] pain is deferred: the hit is added to the frame's total
        // and M_ProcessPain runs pain() once at the end of the frame, ducked or
        // not - the callback decides the animation (M_ShouldReactToPain), so
        // there is no blanket nightmare debounce. A hit that does no damage
        // at all does not provoke; one that armor fully absorbed still does
        // (damage is the amount before armor), with no pain to show for it.
        if (damage > 0) {
            M_ReactToDamage(targ, attacker, inflictor);

            targ->monsterinfo.damage_attacker = attacker;
            targ->monsterinfo.damage_blood += take;
            targ->monsterinfo.damage_knockback += knockback;
            targ->monsterinfo.damage_mod = mod;
        }
    } else if (targ->svflags & SVF_MONSTER) {
        M_ReactToDamage(targ, attacker, inflictor);
        if (!(targ->monsterinfo.aiflags & AI_DUCKED) && (take)) {
            targ->pain(targ, attacker, knockback, take);
            // nightmare mode monsters don't go into pain frames often
            if (skill->value == 3)
                targ->pain_debounce_framenum = level.framenum + 5 * BASE_FRAMERATE;
        }
        // [rerelease] fire the health target on every hit, not just on pain
        // frames - id does this in M_ProcessPain
        if (take && targ->inuse)
            M_FireHealthTarget(targ);
    } else if (client) {
        if (!(targ->flags & FL_GODMODE) && (take))
            targ->pain(targ, attacker, knockback, take);
    } else if (take) {
        if (targ->pain)
            targ->pain(targ, attacker, knockback, take);
    }

    // add to the damage inflicted on a player this frame
    // the total will be turned into screen blends and view angle kicks
    // at the end of the frame
    if (client) {
        client->damage_parmor += psave;
        client->damage_armor += asave;
        client->damage_blood += take;
        client->damage_knockback += knockback;
        VectorCopy(point, client->damage_from);
    }
}

qboolean InflictorGibExplosion(edict_t* inflictor, edict_t* self) {
	if (!Q_stricmp(inflictor->classname, "rocket") || !Q_stricmp(inflictor->classname, "misc_explobox")
		|| !Q_stricmp(inflictor->classname, "hgrenade") || !Q_stricmp(inflictor->classname, "grenade")
		|| !Q_stricmp(inflictor->classname, "bfg blast") || !Q_stricmp(self->classname, "turret_driver") || self->health < -400) {
		return qtrue;
	}
	else {
		return qfalse;
	}
}

/*
============
T_RadiusDamage
============
*/
/*
============
T_RadiusNukeDamage

Rogue's A-M bomb blast. Unlike T_RadiusDamage this does NOT trace for line of
sight - a nuke goes through walls - and it has two zones: everything inside
`radius` takes a flat 10000 (i.e. dies), and out to twice that the damage
falls off linearly.

The rerelease follows this with a second pass that sets client->nuke_time for
the screen white-out. That is not ported - see the header of the nuke section
in g_rogue_items.c.
============
*/
void T_RadiusNukeDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore, float radius, int mod)
{
    float    points;
    edict_t *ent = NULL;
    vec3_t   v;
    vec3_t   dir;
    float    len;
    float    killzone, killzone2;

    killzone = radius;
    killzone2 = radius * 2.0f;

    while ((ent = findradius(ent, inflictor->s.origin, killzone2)) != NULL) {
        if (ent == ignore)
            continue;
        if (!ent->takedamage)
            continue;
        if (!ent->inuse)
            continue;
        if (!(ent->client || (ent->svflags & SVF_MONSTER) || (ent->svflags & SVF_DAMAGEABLE)))
            continue;

        VectorAdd(ent->mins, ent->maxs, v);
        VectorMA(ent->s.origin, 0.5f, v, v);
        VectorSubtract(inflictor->s.origin, v, v);
        len = VectorLength(v);

        if (len <= killzone)
            points = 10000;
        else if (len <= killzone2)
            points = (damage / killzone) * (killzone2 - len);
        else
            points = 0;

        if (points > 0) {
            VectorSubtract(ent->s.origin, inflictor->s.origin, dir);
            T_Damage(ent, inflictor, attacker, dir, inflictor->s.origin, vec3_origin,
                     (int)points, (int)points, DAMAGE_RADIUS, mod);
        }
    }
}

void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore, float radius, int mod)
{
    float   points;
    edict_t *ent = NULL;
    vec3_t  v;
    vec3_t  dir;

    while ((ent = findradius(ent, inflictor->s.origin, radius)) != NULL) {
        if (ent == ignore)
            continue;
        if (!ent->takedamage)
            continue;

        VectorAdd(ent->mins, ent->maxs, v);
        VectorMA(ent->s.origin, 0.5f, v, v);
        VectorSubtract(inflictor->s.origin, v, v);
        points = damage - 0.5f * VectorLength(v);
        if (ent == attacker)
            points = points * 0.5f;
        if (points > 0) {
            if (CanDamage(ent, inflictor)) {
                VectorSubtract(ent->s.origin, inflictor->s.origin, dir);
                T_Damage(ent, inflictor, attacker, dir, inflictor->s.origin, vec3_origin, (int)points, (int)points, DAMAGE_RADIUS, mod);
            }
        }
    }
}
