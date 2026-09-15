/*
==============================================================================

NAVIGATION MESH (rerelease NAV3)

The remaster ships 174 navmeshes in `bots/navigation/<map>.nav`. Despite the
`bots/` path they are not bot-only: the rerelease's own `M_NavPathToGoal`
(m_move.cpp) walks them whenever a monster paths to an enemy, including when it
has LOST SIGHT of one. This file is the loader and the pathfinder; nothing here
is wired into monster AI yet.

FORMAT - fully reverse engineered and validated against all 174 files; the
reference implementation and the validator are `tools/rerelease/nav.py`, which
documents every field. Little endian throughout:

    header      'NAV3', i version, i num_nodes, i num_links, i num_traversals,
                f agent radius
    node[]      8B:  u16 flags, u16 num_links, u16 first_link, u16 radius
    origin[]    12B: vec3, one per node
    link[]      6B:  u16 target, u16 flags, u16 traversal (0xffff = plain walk)
    traversal[] 48B: four vec3 - apex (or a 1e30 sentinel), start, end, unused
    volume[]    i count, then 30B: u16 link, u16 node, u16 pad, vec3 mins/maxs

TWO THINGS THE FORMAT WILL CATCH YOU OUT ON:

  * `first_link` is CUMULATIVE. Node N owns link[first_link .. +num_links).
    That invariant is what proves the tables line up and it is checked below.
  * the volume record's FIRST field is a LINK index, not a node index. Reading
    it as a node parses mgu5m1 perfectly and falls apart on 124 of the other
    files.

ONLY VERSION 6 IS LOADED - that is all 142 campaign maps including every mgu*.
v2/v4/v5 are the N64 and CTF maps, use a different layout, and are rejected
rather than misparsed.

Loaded through `gi.LoadFile` rather than fopen so it works when the remaster is
supplied as pak0.pak instead of an extracted tree.

==============================================================================
*/

#include "g_local.h"
#include <float.h>

#define NAV_VERSION     6
#define NAV_INVALID     0xffff

// how far a start or goal point may be from the nearest node and still snap
#define NAV_MAX_SNAP    512.0f

typedef struct {
    uint16_t    flags;
    uint16_t    num_links;
    uint16_t    first_link;
    uint16_t    radius;
} nav_node_t;

typedef struct {
    uint16_t    target;
    uint16_t    flags;
    uint16_t    traversal;
} nav_link_t;

typedef struct {
    vec3_t      apex;
    vec3_t      start;
    vec3_t      end;
    vec3_t      unused;
} nav_traversal_t;

typedef struct {
    int             num_nodes;
    int             num_links;
    int             num_traversals;
    float           agent_radius;

    nav_node_t      *nodes;
    vec3_t          *origins;
    nav_link_t      *links;
    nav_traversal_t *traversals;

    // A* scratch, allocated once with the mesh rather than per query
    float           *g_score;
    float           *f_score;
    int             *came_from;
    byte            *closed;
    int             *open;
    byte            *in_open;
    int             open_count;
} nav_mesh_t;

static nav_mesh_t   nav;

/*
=================
Nav_Free
=================
*/
void Nav_Free(void)
{
    Nav_ClearPursuit();

    // Deliberately NO gi.TagFree here. The mesh is one TAG_LEVEL block, and
    // TAG_LEVEL is reclaimed wholesale by the gi.FreeTags(TAG_LEVEL) that
    // SpawnEntities runs on every map load - so by the time anyone calls this,
    // the memory is already gone and freeing it again trips
    // "Z_Free: assertion `z->magic == Z_MAGIC' failed" (gi.TagFree IS Z_Free,
    // see import.TagFree in src/server/game.c).
    //
    // This function therefore only FORGETS the mesh, which makes it safe to
    // call at any point in the level cycle - the property that keeps this class
    // of bug from coming back. The one cost is that a mesh discarded by the
    // validation below leaks until that level's own FreeTags, which is bounded,
    // rare, and genuinely reclaimed.
    memset(&nav, 0, sizeof(nav));
}

static inline uint16_t nav_u16(const byte *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline int nav_i32(const byte *p)
{
    return (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static float nav_float(const byte *p)
{
    union { uint32_t u; float f; } v;
    v.u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return v.f;
}

/*
=================
Nav_Load

Called once per level. A missing or unsupported navmesh is NOT an error - the
mesh is simply absent and every entry point below returns false.
=================
*/
void Nav_Load(const char *mapname)
{
    char        path[MAX_QPATH];
    byte        *raw = NULL;
    const byte  *p;
    int         len, i, version, total;
    size_t      need;
    byte        *block;

    Nav_Free();

    if (!mapname || !*mapname)
        return;

    if (!gi.LoadFile) {
        gi.dprintf("Nav: engine has no LoadFile import, navmeshes unavailable\n");
        return;
    }

    Q_snprintf(path, sizeof(path), "bots/navigation/%s.nav", mapname);

    len = gi.LoadFile(path, (void **)&raw);
    if (!raw || len < 24)
        goto done;

    if (memcmp(raw, "NAV3", 4)) {
        gi.dprintf("Nav: %s is not a NAV3 file\n", path);
        goto done;
    }

    version = nav_i32(raw + 4);
    if (version != NAV_VERSION) {
        // v2/v4/v5 are the N64 and CTF maps and use a different layout
        gi.dprintf("Nav: %s is version %d, only %d is supported\n",
                   path, version, NAV_VERSION);
        goto done;
    }

    nav.num_nodes      = nav_i32(raw + 8);
    nav.num_links      = nav_i32(raw + 12);
    nav.num_traversals = nav_i32(raw + 16);
    nav.agent_radius   = nav_float(raw + 20);

    if (nav.num_nodes <= 0 || nav.num_links < 0 || nav.num_traversals < 0) {
        gi.dprintf("Nav: %s has nonsense counts\n", path);
        goto done;
    }

    need = (size_t)24
         + (size_t)nav.num_nodes * 8
         + (size_t)nav.num_nodes * 12
         + (size_t)nav.num_links * 6
         + (size_t)nav.num_traversals * 48;
    if ((size_t)len < need) {
        gi.dprintf("Nav: %s is truncated (%d bytes, need %u)\n",
                   path, len, (unsigned)need);
        goto done;
    }

    // one allocation for the whole mesh so Nav_Free is a single TagFree
    block = gi.TagMalloc(
        nav.num_nodes * sizeof(nav_node_t) +
        nav.num_nodes * sizeof(vec3_t) +
        nav.num_links * sizeof(nav_link_t) +
        nav.num_traversals * sizeof(nav_traversal_t) +
        nav.num_nodes * (sizeof(float) * 2 + sizeof(int) * 2 + 2), TAG_LEVEL);

    nav.nodes = (nav_node_t *)block;
    block += nav.num_nodes * sizeof(nav_node_t);
    nav.origins = (vec3_t *)block;
    block += nav.num_nodes * sizeof(vec3_t);
    nav.links = (nav_link_t *)block;
    block += nav.num_links * sizeof(nav_link_t);
    nav.traversals = (nav_traversal_t *)block;
    block += nav.num_traversals * sizeof(nav_traversal_t);
    nav.g_score = (float *)block;
    block += nav.num_nodes * sizeof(float);
    nav.f_score = (float *)block;
    block += nav.num_nodes * sizeof(float);
    nav.came_from = (int *)block;
    block += nav.num_nodes * sizeof(int);
    nav.open = (int *)block;
    block += nav.num_nodes * sizeof(int);
    nav.closed = (byte *)block;
    block += nav.num_nodes;
    nav.in_open = (byte *)block;

    p = raw + 24;
    for (i = 0; i < nav.num_nodes; i++, p += 8) {
        nav.nodes[i].flags      = nav_u16(p);
        nav.nodes[i].num_links  = nav_u16(p + 2);
        nav.nodes[i].first_link = nav_u16(p + 4);
        nav.nodes[i].radius     = nav_u16(p + 6);
    }

    for (i = 0; i < nav.num_nodes; i++, p += 12) {
        nav.origins[i][0] = nav_float(p);
        nav.origins[i][1] = nav_float(p + 4);
        nav.origins[i][2] = nav_float(p + 8);
    }

    for (i = 0; i < nav.num_links; i++, p += 6) {
        nav.links[i].target    = nav_u16(p);
        nav.links[i].flags     = nav_u16(p + 2);
        nav.links[i].traversal = nav_u16(p + 4);
    }

    for (i = 0; i < nav.num_traversals; i++, p += 48) {
        int k;

        for (k = 0; k < 3; k++) {
            nav.traversals[i].apex[k]   = nav_float(p + k * 4);
            nav.traversals[i].start[k]  = nav_float(p + 12 + k * 4);
            nav.traversals[i].end[k]    = nav_float(p + 24 + k * 4);
            nav.traversals[i].unused[k] = nav_float(p + 36 + k * 4);
        }
    }

    // the cumulative-first_link invariant is what proves the node and link
    // tables agree; a mesh that fails it would send monsters to random places
    total = 0;
    for (i = 0; i < nav.num_nodes; i++) {
        if (nav.nodes[i].first_link != total) {
            gi.dprintf("Nav: %s node %d first_link %d, expected %d - discarding\n",
                       path, i, nav.nodes[i].first_link, total);
            Nav_Free();
            goto done;
        }
        total += nav.nodes[i].num_links;
    }
    if (total != nav.num_links) {
        gi.dprintf("Nav: %s link total %d != header %d - discarding\n",
                   path, total, nav.num_links);
        Nav_Free();
        goto done;
    }
    for (i = 0; i < nav.num_links; i++) {
        if (nav.links[i].target >= nav.num_nodes) {
            gi.dprintf("Nav: %s link %d targets node %d of %d - discarding\n",
                       path, i, nav.links[i].target, nav.num_nodes);
            Nav_Free();
            goto done;
        }
    }

    gi.dprintf("Nav: %s loaded - %d nodes, %d links, %d traversals\n",
               path, nav.num_nodes, nav.num_links, nav.num_traversals);

done:
    if (raw && gi.FreeFile)
        gi.FreeFile(raw);
}

bool Nav_Loaded(void)
{
    return nav.num_nodes > 0;
}

/*
=================
Nav_NearestNode

Closest node to a point, ignoring anything further than max_dist. Linear over
the node list - the biggest mesh in the pak is 1233 nodes, and this is not on
a per-frame path.
=================
*/
int Nav_NearestNode(const vec3_t point, float max_dist)
{
    float   best = max_dist * max_dist;
    int     best_i = -1;
    int     i;

    if (!Nav_Loaded())
        return -1;

    for (i = 0; i < nav.num_nodes; i++) {
        vec3_t  d;
        float   len;

        VectorSubtract(nav.origins[i], point, d);
        len = DotProduct(d, d);
        if (len < best) {
            best = len;
            best_i = i;
        }
    }

    return best_i;
}

/*
=================
Nav_LinkAllowed

Can a mover with these capabilities actually USE this link?

A link with no traversal (0xffff) is plain walkable floor - 178089 of the
190041 links in the pak, flag 0x300 - and is always allowed. Everything else
carries a traversal, and the height difference between its start and end says
what kind of move it is:

    end below start   a DROP of that much; needs drop_height
    end above start   a CLIMB; needs jump_height
    level             a gap jump across; needs either

This is the same decision the rerelease makes, just from the other side: it
tells its path query up front what the monster can do (PathFlags::BarrierJump
when jump_height is set, WalkOffLedge when drop_height is) so the route comes
back already viable. We filter instead of asking, which gets the same result
from a file we read ourselves.

caps == NULL means "no limits" - what the compass uses, since a player will
happily walk off anything.
=================
*/
static bool Nav_LinkAllowed(const nav_link_t *link, const nav_caps_t *caps)
{
    const nav_traversal_t *t;
    float   dz;

    if (link->traversal == NAV_INVALID)
        return true;            // plain floor

    if (!caps)
        return true;

    if (link->traversal >= nav.num_traversals)
        return false;

    t = &nav.traversals[link->traversal];
    dz = t->end[2] - t->start[2];

    if (dz < -8.0f)
        return caps->drop_height >= -dz;
    if (dz > 8.0f)
        return caps->jump_height >= dz;

    // a level traversal is a gap to clear; either ability covers it
    return caps->jump_height > 0 || caps->drop_height > 0;
}

/*
=================
Nav_FindPath

A* from `from` to `to`. Fills `out` with node indices and returns how many were
written, or 0 if there is no path. The open set is a plain array scanned for
the lowest f - with a thousand nodes that is cheaper than maintaining a heap,
and a path query is not a per-frame cost.
=================
*/
int Nav_FindPath(int from, int to, int *out, int max_out)
{
    return Nav_FindPathCaps(from, to, NULL, out, max_out);
}

int Nav_FindPathCaps(int from, int to, const nav_caps_t *caps,
                     int *out, int max_out)
{
    int     i, count;

    if (!Nav_Loaded() || max_out <= 0)
        return 0;
    if (from < 0 || to < 0 || from >= nav.num_nodes || to >= nav.num_nodes)
        return 0;

    if (from == to) {
        out[0] = from;
        return 1;
    }

    for (i = 0; i < nav.num_nodes; i++) {
        nav.g_score[i] = FLT_MAX;
        nav.f_score[i] = FLT_MAX;
        nav.came_from[i] = -1;
        nav.closed[i] = 0;
        nav.in_open[i] = 0;
    }

    nav.g_score[from] = 0;
    nav.f_score[from] = Distance(nav.origins[from], nav.origins[to]);
    nav.open[0] = from;
    nav.in_open[from] = 1;
    nav.open_count = 1;

    while (nav.open_count) {
        int     best = 0, current, l;

        for (i = 1; i < nav.open_count; i++)
            if (nav.f_score[nav.open[i]] < nav.f_score[nav.open[best]])
                best = i;

        current = nav.open[best];
        nav.open[best] = nav.open[--nav.open_count];
        nav.in_open[current] = 0;

        if (current == to) {
            // walk the parents back, then reverse in place
            count = 0;
            for (i = current; i != -1 && count < max_out; i = nav.came_from[i])
                out[count++] = i;
            for (i = 0; i < count / 2; i++) {
                int t = out[i];
                out[i] = out[count - 1 - i];
                out[count - 1 - i] = t;
            }
            return count;
        }

        if (nav.closed[current])
            continue;
        nav.closed[current] = 1;

        for (l = 0; l < nav.nodes[current].num_links; l++) {
            const nav_link_t *link = &nav.links[nav.nodes[current].first_link + l];
            int   n = link->target;
            float tentative;

            if (nav.closed[n])
                continue;

            // a route through a jump this mover cannot make is not a route
            if (!Nav_LinkAllowed(link, caps))
                continue;

            tentative = nav.g_score[current] +
                        Distance(nav.origins[current], nav.origins[n]);

            if (tentative >= nav.g_score[n])
                continue;

            nav.came_from[n] = current;
            nav.g_score[n] = tentative;
            nav.f_score[n] = tentative + Distance(nav.origins[n], nav.origins[to]);

            // membership is a flag, not a scan: with ~100 links per node the
            // old linear search over the open set was most of the query cost
            if (!nav.in_open[n] && nav.open_count < nav.num_nodes) {
                nav.in_open[n] = 1;
                nav.open[nav.open_count++] = n;
            }
        }
    }

    return 0;
}

/*
=================
Nav_NodeOrigin
=================
*/
bool Nav_NodeOrigin(int node, vec3_t out)
{
    if (!Nav_Loaded() || node < 0 || node >= nav.num_nodes)
        return false;

    VectorCopy(nav.origins[node], out);
    return true;
}

/*
=================
Nav_PathToPoint

Convenience wrapper: path from one world point to another. Returns the number
of nodes written.
=================
*/
int Nav_PathToPoint(const vec3_t from, const vec3_t to, int *out, int max_out)
{
    return Nav_PathToPointCaps(from, to, NULL, out, max_out);
}

int Nav_PathToPointCaps(const vec3_t from, const vec3_t to,
                        const nav_caps_t *caps, int *out, int max_out)
{
    int a = Nav_NearestNode(from, NAV_MAX_SNAP);
    int b = Nav_NearestNode(to, NAV_MAX_SNAP);

    if (a < 0 || b < 0)
        return 0;

    return Nav_FindPathCaps(a, b, caps, out, max_out);
}

/*
==============================================================================

PURSUIT - the monster AI hook

`ai_run` already has a perfectly good pursuit loop: it walks toward
`monsterinfo.last_sighting` and does its own little left/right course
correction when the straight line is blocked. What it CANNOT do is go round a
corner it never saw the player disappear behind - it only knows the last place
it saw them and the breadcrumb trail they left.

So this does not replace that loop, it STEERS it: pick the next navmesh
waypoint toward the enemy and drop it into `last_sighting`. Everything
downstream - the move, the course correction, the blocked hook - is unchanged.
That is deliberately the smallest possible change to working AI.

PRECEDENCE, agreed with Matt: **an authored hint_path always wins.** Those are
hand placed by the level designers, 830 of them across the Ground Zero maps,
and already verified working. The navmesh only runs where a map has none -
which is the other ~130 maps, where a monster that loses you currently just
mills around its last sighting.

NOT SAVED. The plan cache below is a file-static keyed by entity number, so a
savegame load simply re-paths on the next think. That is the whole reason
there are no new monsterinfo fields and no SAVE_VERSION bump for this.

THREE THINGS THE FIRST CUT OF THIS GOT WRONG, all of which showed up as
monsters grinding along walls on mgu5m1 - a map with two mutants, a navmesh,
and, the reason it is affected at all, zero hint_paths:

  * "am I standing on this node" was a plain 3D distance against a 48 unit
    budget. Nodes sit ON the floor and a monster's origin is a couple of feet
    above them, so ~23 units of that budget is gone before the monster has
    moved anywhere, and node spacing on a real mesh is ~100 units. Measured
    over every link on mgu5m1, a monster part way along a link was further
    than 48 units from the nearest node 36% of the time - and the skip loop
    then handed back path[0], the node the monster had SNAPPED to, which is
    routinely BEHIND it. The monster was being told to walk backwards.
    Reach is now horizontal with a separate vertical window, and the route
    always advances at least one node past the snap.

  * the route was followed node centre to node centre, which makes a monster
    zig-zag down a corridor it could cross in a straight line. It is string
    pulled now: aim at the furthest node ahead a body sized trace can reach.

  * the plan was rebuilt on a fixed 1 Hz timer and `last_sighting` was left to
    the player-trail code in between, so the goal flipped between a navmesh
    corner and a trail breadcrumb twice a second and the monster visibly
    dithered. The plan is cached per entity and re-driven every frame; it is
    only rebuilt when it actually goes stale.

==============================================================================
*/

#define NAV_MAX_PATH        128

// Earliest we may run A* again for the same monster. The plan is only rebuilt
// when it goes stale at all, so this is a floor on the rate, not a schedule.
#define NAV_REPATH_DELAY    (BASE_FRAMERATE / 5)

// ...and a longer backoff when there was no route, so a monster that cannot
// reach the player does not run A* over and over to find that out.
#define NAV_REPATH_FAILED   (2 * BASE_FRAMERATE)

#define NAV_PLAN_LIFETIME   (3 * BASE_FRAMERATE)

// HORIZONTAL reach. See the note above about the vertical half of this.
#define NAV_WAYPOINT_REACHED    64.0f

// how far the enemy may move before the plan is worth rebuilding
#define NAV_GOAL_SLOP       192.0f

// how many nodes ahead the string pull will look
#define NAV_LOOKAHEAD       6

// how many snap candidates to try before settling for the outright nearest
#define NAV_SNAP_TRIES      8

typedef struct {
    vec3_t  waypoint;       // the point to walk at
    vec3_t  planned_for;    // enemy origin this plan was built around
    int     expire;         // framenum the plan goes stale on its own
    int     next_repath;    // earliest framenum we may A* again
    bool    valid;
} nav_plan_t;

static nav_plan_t   nav_plan[MAX_EDICTS];

void Nav_ClearCombat(void)
{
    memset(nav_plan, 0, sizeof(nav_plan));
}

void Nav_ClearPursuit(void)
{
    Nav_ClearCombat();
}

/*
=================
Nav_Reached

Is `self` close enough to `point` to count as having arrived?

Deliberately NOT a 3D distance. A navmesh node sits on the floor; a monster's
origin sits -mins[2] above the floor it is standing on. Measured in 3D a
monster standing perfectly on a node is already ~23 units away from it, which
eats most of any sane budget and makes "arrived" fire far too rarely. So the
test is horizontal, with a vertical window that only asks whether this is the
same floor.
=================
*/
static bool Nav_Reached(const edict_t *self, const vec3_t point)
{
    vec3_t  d;

    VectorSubtract(point, self->s.origin, d);

    // above our head, or more than a jump below our feet: a different storey
    if (d[2] > self->maxs[2] || d[2] < self->mins[2] - 64.0f)
        return false;

    d[2] = 0;
    return VectorLength(d) <= NAV_WAYPOINT_REACHED;
}

/*
=================
Nav_BodyReaches

Could something the size of `self` get from where it is to `point` in a
straight line? The node is on the floor, so the trace aims at where the
monster's own origin would be if it stood there - otherwise every trace
scrapes along the ground and fails.

Used two ways: to string pull a route, and to decide whether a route is needed
at all. Both only ever make the monster MORE willing to walk straight, so a
false negative here costs nothing worse than an extra navmesh corner.
=================
*/
static bool Nav_BodyReaches(edict_t *self, const vec3_t point)
{
    vec3_t  target;
    trace_t tr;

    VectorSet(target, point[0], point[1], point[2] - self->mins[2]);

    tr = gi.trace(self->s.origin, self->mins, self->maxs, target, self,
                  MASK_PLAYERSOLID);

    return tr.fraction == 1.0f;
}

/*
=================
Nav_NearestReachable

The nearest node that `ent` could actually join, rather than merely the
nearest node.

NAV_MAX_SNAP is 512 units, which is generous enough to pick a node through a
wall, on the storey below, or across a pit. A route that starts or ends in the
wrong room is worse than no route at all - the monster walks a path it can
never join, which looks exactly like running into a wall - so the closest
handful of candidates get a body sized trace and the first one that is really
reachable wins. Falling back to the outright nearest keeps the old behaviour
when nothing traces clear, which is no worse than before.

`ent` NULL means a point sized probe with no ignore entity - what the goal end
and the compass use.
=================
*/
static int Nav_NearestReachable(const vec3_t point, edict_t *ent, float max_dist)
{
    float   best[NAV_SNAP_TRIES];
    int     cand[NAV_SNAP_TRIES];
    int     count = 0;
    float   limit = max_dist * max_dist;
    int     i, j;

    if (!Nav_Loaded())
        return -1;

    for (i = 0; i < nav.num_nodes; i++) {
        vec3_t  d;
        float   len;

        VectorSubtract(nav.origins[i], point, d);
        len = DotProduct(d, d);
        if (len > limit)
            continue;

        // keep the NAV_SNAP_TRIES closest, nearest first
        if (count == NAV_SNAP_TRIES && len >= best[count - 1])
            continue;
        if (count < NAV_SNAP_TRIES)
            count++;
        for (j = count - 1; j > 0 && best[j - 1] > len; j--) {
            best[j] = best[j - 1];
            cand[j] = cand[j - 1];
        }
        best[j] = len;
        cand[j] = i;
    }

    if (!count)
        return -1;

    for (i = 0; i < count; i++) {
        vec3_t  target, mins, maxs;
        trace_t tr;

        VectorCopy(nav.origins[cand[i]], target);
        if (ent) {
            VectorCopy(ent->mins, mins);
            VectorCopy(ent->maxs, maxs);
            target[2] -= ent->mins[2];
        } else {
            VectorClear(mins);
            VectorClear(maxs);
            target[2] += 24;
        }

        tr = gi.trace(point, mins, maxs, target, ent, MASK_PLAYERSOLID);
        if (tr.fraction == 1.0f)
            return cand[i];
    }

    return cand[0];
}

/*
=================
Nav_Replan

Rebuild this monster's route to `goal` and pick the point to walk at. Returns
false when there is no usable route, which puts the caller straight back on
the classic movement.
=================
*/
static bool Nav_Replan(edict_t *self, const vec3_t goal, nav_plan_t *plan)
{
    static int  path[NAV_MAX_PATH];
    nav_caps_t  caps;
    vec3_t      node, best;
    int         from, to, num, i, start, limit;

    // route only over moves this monster can actually make. A monster with no
    // jump at all is limited to plain floor, which is correct - it should go
    // the long way round rather than get stuck at a ledge.
    caps.jump_height = self->monsterinfo.can_jump ? self->monsterinfo.jump_height : 0;
    caps.drop_height = self->monsterinfo.drop_height;

    from = Nav_NearestReachable(self->s.origin, self, NAV_MAX_SNAP);
    to   = Nav_NearestReachable(goal, NULL, NAV_MAX_SNAP);
    if (from < 0 || to < 0)
        return false;

    num = Nav_FindPathCaps(from, to, &caps, path, q_countof(path));
    if (num < 2)
        return false;

    // Skip every node we have already reached, and then one more: path[0] is
    // the node we SNAPPED to, not a node we have arrived at, and aiming at it
    // is how a monster ends up walking away from its own goal.
    start = 1;
    for (i = 0; i < num; i++) {
        if (!Nav_NodeOrigin(path[i], node))
            return false;
        if (Nav_Reached(self, node))
            start = i + 1;
    }
    if (start >= num)
        return false;       // the whole route is behind us; we are there

    if (!Nav_NodeOrigin(path[start], best))
        return false;

    // string pull: the furthest node ahead we could walk straight to
    limit = min(num, start + NAV_LOOKAHEAD);
    for (i = start + 1; i < limit; i++) {
        if (!Nav_NodeOrigin(path[i], node))
            break;
        if (!Nav_BodyReaches(self, node))
            break;
        VectorCopy(node, best);
    }

    VectorCopy(best, plan->waypoint);
    VectorCopy(goal, plan->planned_for);
    plan->expire = level.framenum + NAV_PLAN_LIFETIME;
    plan->valid = true;
    return true;
}

/*
=================
Nav_Waypoint

The cached plan, rebuilt only when it has gone stale. The cache is what lets
the monster be steered EVERY frame - which in turn is what stops the player
trail from grabbing the goal back in between recomputes.
=================
*/
static bool Nav_Waypoint(edict_t *self, const vec3_t goal, vec3_t out)
{
    nav_plan_t  *plan = &nav_plan[self->s.number];
    bool        stale;

    // A savegame load restores level.framenum without going anywhere near this
    // cache (Nav_Load only runs from SpawnEntities), so the clock can jump
    // BACKWARDS underneath a plan and leave it looking valid for minutes. A
    // deadline further out than a plan is ever given means exactly that.
    if (plan->expire > level.framenum + NAV_PLAN_LIFETIME ||
        plan->next_repath > level.framenum + NAV_REPATH_FAILED) {
        plan->valid = false;
        plan->expire = 0;
        plan->next_repath = 0;
    }

    stale = !plan->valid
         || level.framenum >= plan->expire
         || Nav_Reached(self, plan->waypoint)
         || Distance(goal, plan->planned_for) > NAV_GOAL_SLOP;

    if (stale && level.framenum >= plan->next_repath) {
        if (Nav_Replan(self, goal, plan)) {
            plan->next_repath = level.framenum + NAV_REPATH_DELAY;
        } else {
            plan->valid = false;
            plan->next_repath = level.framenum + NAV_REPATH_FAILED;
        }
    }

    if (!plan->valid || level.framenum >= plan->expire)
        return false;

    VectorCopy(plan->waypoint, out);
    return true;
}

/*
=================
Nav_Eligible

The checks both entry points share: is there a mesh, is this monster ours to
steer, and is it something the mesh even describes.
=================
*/
static bool Nav_Eligible(edict_t *self)
{
    if (!Nav_Loaded())
        return false;

    // an authored hint_path network beats the navmesh wherever one exists
    if (hint_paths_present)
        return false;

    if (!self->enemy || !self->enemy->inuse)
        return false;

    // these all mean the monster is already being steered by something else
    if (self->monsterinfo.aiflags & (AI_HINT_PATH | AI_COMBAT_POINT |
                                     AI_SOUND_TARGET | AI_TARGET_ANGER))
        return false;

    // the mesh describes walkable floor; fliers and swimmers ignore it
    if (self->flags & (FL_FLY | FL_SWIM))
        return false;

    if (self->s.number < 1 || self->s.number >= MAX_EDICTS)
        return false;

    return true;
}

/*
=================
Nav_MonsterPursue

Returns true if it set a fresh waypoint into last_sighting. False means "not
applicable, carry on with the classic pursuit".

Unlike the first cut this returns true on EVERY frame it is steering, not only
on the frames it recomputes. ai_run reads that as "the navmesh owns the goal",
which is what keeps the player-trail code from overwriting the waypoint
between recomputes.
=================
*/
bool Nav_MonsterPursue(edict_t *self)
{
    vec3_t  waypoint;

    if (!Nav_Eligible(self))
        return false;

    if (!Nav_Waypoint(self, self->enemy->s.origin, waypoint))
        return false;

    VectorCopy(waypoint, self->monsterinfo.last_sighting);
    return true;
}

/*
=================
Nav_CombatWaypoint

[rerelease] The navmesh while the enemy is IN SIGHT, which is the half this
tree never had - Nav_MonsterPursue above only runs once a monster has lost
track of you, so every monster here behaved like the rerelease's COMBAT_RANGED
no matter what it was. A berserk that cannot see a way around a railing would
just mill about in front of it.

monsterinfo.combat_style decides who gets it: a melee-only monster has to close
the distance or it is harmless, a mixed one closes to mid range, and a ranged
one is left alone to stand and shoot (returning false here puts it straight
back on the classic movement, which is what the rerelease does too).

Returns the point to walk at.
=================
*/
bool Nav_CombatWaypoint(edict_t *self, vec3_t out)
{
    float   zdiff, standing;
    trace_t tr;

    if (!Nav_Eligible(self))
        return false;

    if (self->enemy->health <= 0)
        return false;

    // this function is ONLY the in-sight half; the rest is Nav_MonsterPursue
    if (!visible(self, self->enemy))
        return false;

    // how far off our own eyeline the enemy has to be before a flat walk
    // towards them stops being good enough
    standing = max(self->maxs[2], -self->mins[2]);
    zdiff = fabsf(self->s.origin[2] - self->enemy->s.origin[2]);

    switch (self->monsterinfo.combat_style) {
    case COMBAT_MELEE:
        // path close, then let ordinary Quake movement finish the job
        if (realrange(self, self->enemy) <= 240.0f && zdiff <= standing)
            return false;
        break;
    case COMBAT_MIXED:
        // most mixed attacks are short ranged, so aim for mid range
        if (realrange(self, self->enemy) <= 440.0f && zdiff <= standing * 2.0f)
            return false;
        break;
    default:
        // COMBAT_RANGED, or a style we never derived: shoot where you stand
        return false;
    }

    // If the monster can just walk straight at the enemy, let it. `visible`
    // above is an EYE LINE trace and says nothing about whether a body fits or
    // where the floor goes, so on its own it is not grounds for handing the
    // monster over to the mesh - and steering at a node centre while the way
    // ahead is wide open is precisely what made monsters veer into walls with
    // the player in plain sight. The mesh is for when the direct route is
    // actually blocked.
    tr = gi.trace(self->s.origin, self->mins, self->maxs, self->enemy->s.origin,
                  self, MASK_PLAYERSOLID);
    if (tr.fraction == 1.0f || tr.ent == self->enemy)
        return false;

    return Nav_Waypoint(self, self->enemy->s.origin, out);
}



/*
=================
Cmd_Nav_f

Test harness, cheat protected. `nav` reports the mesh; `nav path` paths from
where you stand to the current objective marker (target_poi) and prints the
node list and total length.
=================
*/
void Cmd_Nav_f(edict_t *ent)
{
    static int  path[512];
    const char  *cmd = gi.argc() > 1 ? gi.argv(1) : "";

    if (!Nav_Loaded()) {
        gi.cprintf(ent, PRINT_HIGH, "No navmesh loaded for this map.\n");
        return;
    }

    if (!Q_stricmp(cmd, "path")) {
        vec3_t  goal;
        int     n, i;
        float   total = 0;

        if (!level.valid_poi) {
            gi.cprintf(ent, PRINT_HIGH, "No objective marker to path to.\n");
            return;
        }
        VectorCopy(level.current_poi, goal);

        n = Nav_PathToPoint(ent->s.origin, goal, path, q_countof(path));
        if (!n) {
            gi.cprintf(ent, PRINT_HIGH, "No path found.\n");
            return;
        }

        for (i = 1; i < n; i++) {
            vec3_t a, b;

            Nav_NodeOrigin(path[i - 1], a);
            Nav_NodeOrigin(path[i], b);
            total += Distance(a, b);
        }

        gi.cprintf(ent, PRINT_HIGH, "Path: %d nodes, %.0f units\n", n, total);
        for (i = 0; i < n && i < 12; i++) {
            vec3_t o;

            Nav_NodeOrigin(path[i], o);
            gi.cprintf(ent, PRINT_HIGH, "  %d: node %d (%.0f %.0f %.0f)\n",
                       i, path[i], o[0], o[1], o[2]);
        }
        if (n > 12)
            gi.cprintf(ent, PRINT_HIGH, "  ... %d more\n", n - 12);
        return;
    }

    gi.cprintf(ent, PRINT_HIGH,
               "Navmesh: %d nodes, %d links, %d traversals, agent %.2f\n",
               nav.num_nodes, nav.num_links, nav.num_traversals,
               nav.agent_radius);
    gi.cprintf(ent, PRINT_HIGH, "Nearest node to you: %d\n",
               Nav_NearestNode(ent->s.origin, 512));
    gi.cprintf(ent, PRINT_HIGH, "Use \"nav path\" to path to the objective.\n");
}
