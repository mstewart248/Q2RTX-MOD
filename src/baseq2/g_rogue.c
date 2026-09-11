/* =======================================================================
 *
 * Ground Zero (rogue) entities and systems that the rest of this tree did
 * not already cover.
 *
 * Ported from src/rerelease/rogue/.  Right now this is the HINT PATH
 * system, which is by a wide margin rogue's largest single gap here: its
 * thirty maps place 830 hint_path nodes, and without a spawn function every
 * one of them prints a warning and then does nothing.
 *
 * =======================================================================
 */

#include "g_local.h"

/*
 * =======================================================================
 * HINT PATHS
 *
 * A hint path is a hand-placed chain of waypoints, linked head to tail by
 * the ordinary target/targetname keys, with the END spawnflag on both ends.
 * When a monster loses the player for long enough it looks for a chain
 * where SOME node can see the monster and SOME OTHER node on that SAME
 * chain can see the player, then walks the chain from the first to the
 * second.  That is the whole idea: the level designer has pre-drawn the
 * route around the corner that the monster cannot work out for itself.
 *
 * Three things to know before touching any of this:
 *
 *   - The chains are built ONCE, by InitHintPaths, after all entities have
 *     spawned.  hint_chain is that permanent link.  monster_hint_chain and
 *     target_hint_chain are scratch, rebuilt from nothing on every single
 *     call to monsterlost_checkhint - do not expect them to mean anything
 *     between calls.
 *
 *   - hint_paths_present is the fast out.  On the 76 baseq2/rerelease maps
 *     it is 0 and every entry point below returns immediately, so this
 *     system costs those maps nothing at all.
 *
 *   - The search is O(nodes) with a visibility trace per node, twice.  It
 *     is throttled to once per ten seconds per monster by
 *     monsterinfo.last_hint_framenum, and only runs after the monster has
 *     already been searching fruitlessly for five.  A single rogue map runs
 *     to ninety-odd nodes; this is why the throttle is not optional.
 *
 * The rerelease's AI_PATHING / nav-mesh interactions are dropped, as
 * everywhere else in this tree - there is no nav mesh here, so the hint
 * paths are the ONLY long-range recovery a rogue monster gets, which is
 * closer to how Ground Zero originally shipped anyway.
 * =======================================================================
 */

#define SPAWNFLAG_HINT_ENDPOINT     1
#define MAX_HINT_CHAINS             100

int      hint_paths_present;
static edict_t *hint_path_start[MAX_HINT_CHAINS];
static int      num_hint_paths;

// m_stalker.c has a static copy of this; hint paths need it too and cannot
// see that one.
static bool hint_has_valid_enemy(edict_t *self)
{
    if (!self->enemy)
        return false;
    if (!self->enemy->inuse)
        return false;
    if (self->enemy->health <= 0)
        return false;
    return true;
}

/*
 * =============
 * hintpath_go - starts a monster (self) moving towards the hintpath (point),
 *      disabling every contrary AI flag.
 * =============
 */
static void hintpath_go(edict_t *self, edict_t *point)
{
    vec3_t  dir;

    VectorSubtract(point->s.origin, self->s.origin, dir);

    self->ideal_yaw = vectoyaw(dir);
    self->goalentity = self->movetarget = point;
    self->monsterinfo.pause_framenum = 0;
    self->monsterinfo.aiflags |= AI_HINT_PATH;
    self->monsterinfo.aiflags &= ~(AI_SOUND_TARGET | AI_PURSUIT_LAST_SEEN | AI_PURSUE_NEXT | AI_PURSUE_TEMP);
    // run for it
    self->monsterinfo.search_framenum = level.framenum;
    self->monsterinfo.run(self);
}

/*
 * =============
 * hintpath_stop - bails a monster out of following hint paths.
 *
 * Called both on success (it reached its goal node) and on every failure
 * path, so it has to leave the monster in a sane state either way.
 * =============
 */
void hintpath_stop(edict_t *self)
{
    self->goalentity = NULL;
    self->movetarget = NULL;
    self->monsterinfo.last_hint_framenum = level.framenum;
    self->monsterinfo.goal_hint = NULL;
    self->monsterinfo.aiflags &= ~AI_HINT_PATH;

    if (hint_has_valid_enemy(self)) {
        // if we can see our target, go nuts
        if (visible(self, self->enemy)) {
            FoundTarget(self);
            return;
        }
        // otherwise, keep chasing
        HuntTarget(self);
        return;
    }

    // if our enemy is no longer valid, forget about our enemy and go into
    // stand.  We need the pause, otherwise the stand code just reverts to
    // walking with no target and the monster wanders around aimlessly
    // trying to hunt the world entity.
    self->enemy = NULL;
    self->monsterinfo.pause_framenum = INT_MAX;
    self->monsterinfo.stand(self);
}

/*
 * =============
 * monsterlost_checkhint - the monster (self) checks around for a usable
 *      hint path.  A usable one is a chain with a node the MONSTER can see
 *      and a node the monster's ENEMY can see.  One or the other alone is
 *      not enough - a chain that only reaches the monster leads nowhere in
 *      particular, and one that only reaches the player cannot be got onto.
 *
 * Returns true if the monster has been put onto a chain.
 * =============
 */
bool monsterlost_checkhint(edict_t *self)
{
    edict_t *e, *monster_pathchain, *target_pathchain, *checkpoint = NULL;
    edict_t *closest;
    float    closest_range = 1000000;
    edict_t *start, *destination;
    int      count5 = 0;
    float    r;
    int      i;
    bool     hint_path_represented[MAX_HINT_CHAINS];

    // if there are no hint paths on this map, exit immediately.
    if (!hint_paths_present)
        return false;

    if (!self->enemy)
        return false;

    if (self->monsterinfo.aiflags & AI_STAND_GROUND)
        return false;

    // a turret cannot go anywhere, so a path is meaningless to it
    if (!strcmp(self->classname, "monster_turret"))
        return false;

    monster_pathchain = NULL;

    // gather every hint_path node into one flat list
    for (i = 0; i < num_hint_paths; i++) {
        e = hint_path_start[i];
        while (e) {
            if (e->monster_hint_chain)
                e->monster_hint_chain = NULL;

            if (monster_pathchain) {
                checkpoint->monster_hint_chain = e;
                checkpoint = e;
            } else {
                monster_pathchain = e;
                checkpoint = e;
            }
            e = e->hint_chain;
        }
    }

    // filter them by distance and visibility to the monster
    e = monster_pathchain;
    checkpoint = NULL;
    while (e) {
        r = realrange(self, e);

        if (r > 512 || !visible(self, e)) {
            if (checkpoint) {
                checkpoint->monster_hint_chain = e->monster_hint_chain;
                e->monster_hint_chain = NULL;
                e = checkpoint->monster_hint_chain;
            } else {
                // nothing valid found yet, so this one was the list head -
                // unlink it and move the head along
                checkpoint = e;
                e = e->monster_hint_chain;
                checkpoint->monster_hint_chain = NULL;
                checkpoint = NULL;
                monster_pathchain = e;
            }
            continue;
        }

        count5++;
        checkpoint = e;
        e = e->monster_hint_chain;
    }

    // at this point we have a list of every hint node eligible for the
    // monster.  Work out which chains those sit on, and then walk those
    // chains looking for a node that can see the player.
    if (count5 == 0)
        return false;

    for (i = 0; i < num_hint_paths; i++)
        hint_path_represented[i] = false;

    e = monster_pathchain;
    while (e) {
        if ((e->hint_chain_id < 0) || (e->hint_chain_id >= num_hint_paths))
            return false;

        hint_path_represented[e->hint_chain_id] = true;
        e = e->monster_hint_chain;
    }

    count5 = 0;

    // build target_pathchain: every node of every chain the monster can
    // reach, to be checked for validity against the enemy
    target_pathchain = NULL;
    checkpoint = NULL;
    for (i = 0; i < num_hint_paths; i++) {
        if (!hint_path_represented[i])
            continue;

        e = hint_path_start[i];
        while (e) {
            if (target_pathchain) {
                checkpoint->target_hint_chain = e;
                checkpoint = e;
            } else {
                target_pathchain = e;
                checkpoint = e;
            }
            e = e->hint_chain;
        }
    }

    // now filter those by distance and visibility to the ENEMY
    e = target_pathchain;
    checkpoint = NULL;
    while (e) {
        r = realrange(self->enemy, e);

        if (r > 512 || !visible(self->enemy, e)) {
            if (checkpoint) {
                checkpoint->target_hint_chain = e->target_hint_chain;
                e->target_hint_chain = NULL;
                e = checkpoint->target_hint_chain;
            } else {
                checkpoint = e;
                e = e->target_hint_chain;
                checkpoint->target_hint_chain = NULL;
                checkpoint = NULL;
                target_pathchain = e;
            }
            continue;
        }

        count5++;
        checkpoint = e;
        e = e->target_hint_chain;
    }

    if (count5 == 0)
        return false;

    // reuse the represented array, this time for the chains the TARGET is on
    for (i = 0; i < num_hint_paths; i++)
        hint_path_represented[i] = false;

    e = target_pathchain;
    while (e) {
        if ((e->hint_chain_id < 0) || (e->hint_chain_id >= num_hint_paths))
            return false;

        hint_path_represented[e->hint_chain_id] = true;
        e = e->target_hint_chain;
    }

    // walk the monster's list again, dropping any node whose chain the
    // target is not on, and keeping the closest of what survives.  That is
    // the node the monster will walk to first.
    closest = NULL;
    e = monster_pathchain;
    while (e) {
        if (!hint_path_represented[e->hint_chain_id]) {
            checkpoint = e->monster_hint_chain;
            e->monster_hint_chain = NULL;
            e = checkpoint;
            continue;
        }
        r = realrange(self, e);
        if (r < closest_range) {
            closest_range = r;
            closest = e;
        }
        e = e->monster_hint_chain;
    }

    if (!closest)
        return false;

    start = closest;

    // and the DESTINATION node is the one on that same chain closest to the
    // player
    closest = NULL;
    closest_range = 10000000;
    e = target_pathchain;
    while (e) {
        if (start->hint_chain_id == e->hint_chain_id) {
            r = realrange(self, e);
            if (r < closest_range) {
                closest_range = r;
                closest = e;
            }
        }
        e = e->target_hint_chain;
    }

    if (!closest)
        return false;

    destination = closest;

    self->monsterinfo.goal_hint = destination;
    hintpath_go(self, start);

    return true;
}

/*
 * =============
 * hint_path_touch - a monster has reached this node.
 *
 * Work out which way along the chain its goal lies, and hand it the next
 * node in that direction.  The chain is singly linked, so "upstream" is
 * found by scanning for the node whose hint_chain points at us.
 * =============
 */
void hint_path_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    edict_t *e, *goal, *next = NULL;
    bool     goalFound = false;

    // make sure we are the target of its obsession
    if (other->movetarget != self)
        return;

    goal = other->monsterinfo.goal_hint;

    // if the monster is where he wants to be
    if (goal == self) {
        hintpath_stop(other);
        return;
    }

    // otherwise figure out which way we want to go
    e = hint_path_start[self->hint_chain_id];
    while (e) {
        // if we get up to ourselves on the hint chain, we are going down it
        if (e == self) {
            next = e->hint_chain;
            break;
        }
        if (e == goal)
            goalFound = true;
        // if the next link is us and we passed the goal on the way, we are
        // going upstream, so the previous link is where we want to go
        if ((e->hint_chain == self) && goalFound) {
            next = e;
            break;
        }
        e = e->hint_chain;
    }

    // if we could not find it, have the monster go back to normal hunting
    if (!next) {
        hintpath_stop(other);
        return;
    }

    hintpath_go(other, next);

    // have the monster freeze if the hint path we just touched has a wait
    // time on it - for example, when riding a plat
    if (self->wait)
        other->nextthink = level.framenum + (int)(self->wait * BASE_FRAMERATE);
}

/*
 * QUAKED hint_path (.5 .3 0) (-8 -8 -8) (8 8 8) END
 * Target: next hint path
 *
 * END - set this flag on the endpoints of each hintpath.
 *
 * "wait" - set this if you want the monster to freeze when they touch this
 * hintpath
 */
void SP_hint_path(edict_t *self)
{
    if (deathmatch->value) {
        G_FreeEdict(self);
        return;
    }

    if (!self->targetname && !self->target) {
        gi.dprintf("hint_path at %s: unlinked\n", vtos(self->s.origin));
        G_FreeEdict(self);
        return;
    }

    self->solid = SOLID_TRIGGER;
    self->touch = hint_path_touch;
    VectorSet(self->mins, -8, -8, -8);
    VectorSet(self->maxs, 8, 8, 8);
    self->svflags |= SVF_NOCLIENT;
    gi.linkentity(self);
}

/*
 * ============
 * InitHintPaths - called once the map has finished spawning, and again after
 *      a savegame load: the per-node links are saved fields and come back,
 *      but hint_path_start[] and num_hint_paths are globals that do not.
 *
 * Finds every chain start - an END node that has a target but no targetname
 * - and follows target/targetname down it, stamping hint_chain and
 * hint_chain_id as it goes.  Forked and circular chains are reported and
 * disabled rather than followed, because either would hang the walk.
 * ============
 */
void InitHintPaths(void)
{
    edict_t *e, *current;
    int      i;

    hint_paths_present = 0;
    num_hint_paths = 0;
    memset(hint_path_start, 0, sizeof(hint_path_start));

    e = G_Find(NULL, FOFS(classname), "hint_path");
    if (!e)
        return;
    hint_paths_present = 1;

    while (e) {
        if (e->spawnflags & SPAWNFLAG_HINT_ENDPOINT) {
            if (e->target) {    // start point
                if (e->targetname) {    // this is a bad end, ignore it
                    gi.dprintf("hint_path at %s: marked as endpoint with both target (%s) and targetname (%s)\n",
                               vtos(e->s.origin), e->target, e->targetname);
                } else {
                    if (num_hint_paths >= MAX_HINT_CHAINS)
                        break;

                    hint_path_start[num_hint_paths++] = e;
                }
            }
        }
        e = G_Find(e, FOFS(classname), "hint_path");
    }

    // a reload has to start from a clean slate, or every chain looks
    // circular the second time around
    for (e = G_Find(NULL, FOFS(classname), "hint_path"); e; e = G_Find(e, FOFS(classname), "hint_path"))
        e->hint_chain = NULL;

    for (i = 0; i < num_hint_paths; i++) {
        current = hint_path_start[i];
        current->hint_chain_id = i;

        e = G_Find(NULL, FOFS(targetname), current->target);
        if (G_Find(e, FOFS(targetname), current->target)) {
            gi.dprintf("hint_path at %s: forked path detected for chain %d, target %s\n",
                       vtos(current->s.origin), i, current->target);
            hint_path_start[i]->hint_chain = NULL;
            continue;
        }

        while (e) {
            if (e->hint_chain) {
                gi.dprintf("hint_path at %s: circular path detected for chain %d, targetname %s\n",
                           vtos(e->s.origin), i, e->targetname);
                hint_path_start[i]->hint_chain = NULL;
                break;
            }
            current->hint_chain = e;
            current = e;
            current->hint_chain_id = i;
            if (!current->target)
                break;
            e = G_Find(NULL, FOFS(targetname), current->target);
            if (G_Find(e, FOFS(targetname), current->target)) {
                gi.dprintf("hint_path at %s: forked path detected for chain %d, target %s\n",
                           vtos(current->s.origin), i, current->target);
                hint_path_start[i]->hint_chain = NULL;
                break;
            }
        }
    }
}

/*
==============================================================================

ROGUE ONE-OFF ENTITIES

The last handful of Ground Zero classnames, ported from
rerelease/rogue/g_rogue_newtarg.cpp and g_rogue_newtrig.cpp.

  target_killplayers   - kills every player on the map (1 instance)
  target_blacklight    - a pulsing black light (1)
  target_orb           - a pulsing speckled orb (1)
  trigger_disguise     - marks players as disguised so monsters ignore them (2)
  info_player_coop_lava - a coop-only spawn point (26, rmine2)

target_blacklight and target_orb are the same entity wearing different skins
of models/items/spawngro3 - the reference literally shares blacklight_think
between them.
==============================================================================
*/

void target_killplayers_use(edict_t *self, edict_t *other, edict_t *activator)
{
    int      i;
    edict_t *player;

    // kill any monster or breakable in the players' PVS first, so the level
    // does not carry survivors through the wipe
    for (i = 1, player = g_edicts + i; i < globals.num_edicts; i++, player++) {
        if (!player->inuse)
            continue;
        if (player->client)
            continue;
        if (player->health < 1)
            continue;
        if (!player->takedamage)
            continue;

        T_Damage(player, self, self, vec3_origin, player->s.origin, vec3_origin,
                 player->health, 0, DAMAGE_NO_PROTECTION, MOD_TELEFRAG);
    }

    // then the players themselves
    for (i = 0; i < game.maxclients; i++) {
        player = &g_edicts[1 + i];
        if (!player->inuse)
            continue;

        T_Damage(player, self, self, vec3_origin, self->s.origin, vec3_origin,
                 100000, 0, DAMAGE_NO_PROTECTION, MOD_TELEFRAG);
    }
}

/*QUAKED target_killplayers (1 0 0) (-8 -8 -8) (8 8 8)
When triggered, kills every player on the map.
*/
void SP_target_killplayers(edict_t *self)
{
    self->use = target_killplayers_use;
    self->svflags = SVF_NOCLIENT;
}

void blacklight_think(edict_t *self)
{
    self->s.angles[0] += random() * 10.0f;
    self->s.angles[1] += random() * 10.0f;
    self->s.angles[2] += random() * 10.0f;
    self->nextthink = level.framenum + 1;
}

/*QUAKED target_blacklight (1 0 1) (-16 -16 -24) (16 16 24)
Pulsing black light with a sphere in the centre. Removed in deathmatch.
*/
void SP_target_blacklight(edict_t *ent)
{
    if (deathmatch->value) {
        G_FreeEdict(ent);
        return;
    }

    VectorClear(ent->mins);
    VectorClear(ent->maxs);

    ent->s.effects |= (EF_TRACKERTRAIL | EF_TRACKER);
    ent->think = blacklight_think;
    ent->s.modelindex = gi.modelindex("models/items/spawngro3/tris.md2");
    ent->s.scale = 6.0f;
    ent->s.skinnum = 0;
    ent->nextthink = level.framenum + 1;
    gi.linkentity(ent);
}

/*QUAKED target_orb (1 0 1) (-16 -16 -24) (16 16 24)
Translucent pulsing orb with speckles. Removed in deathmatch.
*/
void SP_target_orb(edict_t *ent)
{
    if (deathmatch->value) {
        G_FreeEdict(ent);
        return;
    }

    VectorClear(ent->mins);
    VectorClear(ent->maxs);

    ent->think = blacklight_think;
    ent->nextthink = level.framenum + 1;
    ent->s.skinnum = 1;
    ent->s.modelindex = gi.modelindex("models/items/spawngro3/tris.md2");
    ent->s.frame = 2;
    ent->s.scale = 6.0f;
    gi.linkentity(ent);
}

// ***************************
// TRIGGER_DISGUISE
// ***************************

#define SPAWNFLAG_DISGUISE_START_ON     2
#define SPAWNFLAG_DISGUISE_REMOVE       4

void trigger_disguise_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf)
{
    if (other->client) {
        if (self->spawnflags & SPAWNFLAG_DISGUISE_REMOVE)
            other->flags &= ~FL_DISGUISED;
        else
            other->flags |= FL_DISGUISED;
    }
}

void trigger_disguise_use(edict_t *self, edict_t *other, edict_t *activator)
{
    if (self->solid == SOLID_NOT)
        self->solid = SOLID_TRIGGER;
    else
        self->solid = SOLID_NOT;

    gi.linkentity(self);
}

/*QUAKED trigger_disguise (.5 .5 .5) ? TOGGLE START_ON REMOVE
Anything passing through this while it is active is marked disguised, and
monsters will not acquire it. REMOVE clears the disguise instead.
*/
void SP_trigger_disguise(edict_t *self)
{
    if (self->spawnflags & SPAWNFLAG_DISGUISE_START_ON)
        self->solid = SOLID_TRIGGER;
    else
        self->solid = SOLID_NOT;

    self->touch = trigger_disguise_touch;
    self->use = trigger_disguise_use;
    self->movetype = MOVETYPE_NONE;
    self->svflags = SVF_NOCLIENT;

    gi.setmodel(self, self->model);
    gi.linkentity(self);
}
