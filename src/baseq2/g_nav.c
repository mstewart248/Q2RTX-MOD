/*
==============================================================================

NAVIGATION MESH (rerelease NAV3)

The remaster ships 174 navmeshes in `bots/navigation/<map>.nav`. Despite the
`bots/` path they are not bot-only: the rerelease's own `M_NavPathToGoal`
(m_move.cpp) walks them whenever a monster paths to an enemy, including when it
has LOST SIGHT of one. This file is the loader, the pathfinder, and this tree's
stand-in for the engine's gi.GetPathToGoal (see MONSTER PATHING below).

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
        // the counts are already stored, so clear them back out - bailing
        // here with num_nodes > 0 and the pointers still NULL leaves
        // Nav_Loaded() true and the next query dereferencing NULL
        Nav_Free();
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
        Nav_Free();     // as above - counts set, pointers still NULL
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

MONSTER PATHING - the engine half of the rerelease's M_NavPathToGoal

The rerelease asks its engine `gi.GetPathToGoal(request, info)` and gets back
the next point to walk at (`firstMovePoint`), the one after it
(`secondMovePoint`), and a return code. The code that matters is
TraversalPending: the next leg of the route is a jump or a drop, and
firstMovePoint / secondMovePoint are then the TAKE-OFF and LANDING points of
that traversal. The monster walks at the landing point, SV_movestep refuses to
step off the ledge, and blocked_checkjump sees AI_PATHING + TraversalPending,
faces along the traversal and jumps. Nav_GetPathToGoal below is this tree's
GetPathToGoal; M_NavPathToGoal / M_MoveToPath in m_move.c are ported from
the rerelease on top of it.

WHAT THIS REPLACED. The first navmesh hook here only wrote a waypoint into
`monsterinfo.last_sighting` and left the 1997 movement to get there - keep
walking ideal_yaw, bump around in 45 degree steps when blocked. That mover
cannot take a ledge the route drops off (nothing ever told the jump code the
drop was the plan; it only compared heights against the enemy), and it never
turns towards a waypoint it has a clear line to. On base1, an infantry woken
in the last room while the player jumps out into the water outside has a
route - east along the hallway, then two drops of 100 and 156 units - and
stalled in the hallway instead.

NOT SAVED. The per monster state is a file-static keyed by entity number, so
a savegame load simply re-paths on the next think. monster_start resets a slot
so a reused edict does not inherit a dead monster's route.

PRECEDENCE, agreed with Matt: **an authored hint_path always wins.** Those are
hand placed by the level designers, 830 of them across the Ground Zero maps,
and already verified working. The navmesh only runs where a map has none.

==============================================================================
*/

#define NAV_MAX_PATH        128

// HORIZONTAL reach. See Nav_Reached.
#define NAV_WAYPOINT_REACHED    48.0f

// how many nodes ahead the string pull will look
#define NAV_LOOKAHEAD       6

// how many snap candidates to try before settling for the outright nearest
#define NAV_SNAP_TRIES      8

// the rerelease's default node search window around a WALKING monster's feet:
// a node on another storey is not the node we are standing on, however close
#define NAV_START_BELOW     32.0f
#define NAV_START_ABOVE     64.0f

static nav_monster_t    nav_monster[MAX_EDICTS];

void Nav_ClearCombat(void)
{
    memset(nav_monster, 0, sizeof(nav_monster));
}

void Nav_ClearPursuit(void)
{
    Nav_ClearCombat();
}

nav_monster_t *Nav_MonsterState(edict_t *ent)
{
    int num = ent->s.number;

    if (num < 0 || num >= MAX_EDICTS)
        num = 0;    // slot 0 is the world; never a monster, safe scratch
    return &nav_monster[num];
}

void Nav_ResetMonster(edict_t *ent)
{
    memset(Nav_MonsterState(ent), 0, sizeof(nav_monster_t));
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

bool Nav_PointReached(const edict_t *self, const vec3_t point)
{
    return Nav_Reached(self, point);
}

/*
=================
Nav_BodyReaches

Could something the size of `self` get from where it is to `point` in a
straight line? The node is on the floor, so the trace aims at where the
monster's own origin would be if it stood there - otherwise every trace
scrapes along the ground and fails.

Only ever makes the monster MORE willing to walk straight, so a false negative
here costs nothing worse than an extra navmesh corner.
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
wrong room is worse than no route at all, so the closest handful of candidates
get a trace and the first one that is really reachable wins.

`ent` non-NULL is the START of a route: a body sized trace, and only nodes on
the floor the monster is standing on. Without that window a monster standing
at a ledge snaps to the node BELOW it - the trace down through open air is
clear - and its route then begins at the bottom of a drop it has not taken.

`ent` NULL is the goal end: a point probe, any height, falling back to the
outright nearest when nothing traces clear (the enemy may be swimming, or up
on something the mesh does not cover).
=================
*/
static int Nav_NearestReachable(const vec3_t point, edict_t *ent, float max_dist)
{
    float   best[NAV_SNAP_TRIES];
    int     cand[NAV_SNAP_TRIES];
    int     count = 0;
    float   limit = max_dist * max_dist;
    float   feet = 0;
    int     i, j;

    if (!Nav_Loaded())
        return -1;

    if (ent)
        feet = point[2] + ent->mins[2];

    for (i = 0; i < nav.num_nodes; i++) {
        vec3_t  d;
        float   len;

        if (ent && (nav.origins[i][2] < feet - NAV_START_BELOW ||
                    nav.origins[i][2] > feet + NAV_START_ABOVE))
            continue;

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

// The link A* used to get from `a` to `b`: the first one this mover may take.
static const nav_link_t *Nav_LinkBetween(int a, int b, const nav_caps_t *caps)
{
    int l;

    for (l = 0; l < nav.nodes[a].num_links; l++) {
        const nav_link_t *link = &nav.links[nav.nodes[a].first_link + l];

        if (link->target == b && Nav_LinkAllowed(link, caps))
            return link;
    }

    return NULL;
}

static const nav_traversal_t *Nav_LinkTraversal(const nav_link_t *link)
{
    if (!link || link->traversal == NAV_INVALID ||
        link->traversal >= nav.num_traversals)
        return NULL;
    return &nav.traversals[link->traversal];
}

/*
=================
Nav_GetPathToGoal

This tree's gi.GetPathToGoal. Routes `self` to `goal` over links `caps`
allows and fills `out` the way the rerelease engine does:

  NAV_PATH_IN_PROGRESS      first = the furthest node ahead we can walk
                            straight to (string pulled, never past the start
                            of a traversal), second = the node after it
  NAV_PATH_TRAVERSAL        we are at the take-off of a jump/drop:
                            first = take-off, second = landing
  NAV_PATH_REACHED_GOAL     we are on the goal's node; first = the goal

Returns false with an error code (> NAV_PATH_START_ERRORS) when there is no
route, which sends the caller back to the classic pursuit.
=================
*/
bool Nav_GetPathToGoal(edict_t *self, const vec3_t goal, const nav_caps_t *caps,
                       nav_path_t *out)
{
    static int  path[NAV_MAX_PATH];
    const nav_link_t        *link;
    const nav_traversal_t   *t;
    vec3_t      node;
    int         from, to, num, i, cur, best, limit;

    if (!Nav_Loaded()) {
        out->code = NAV_PATH_NO_NAV;
        return false;
    }

    from = Nav_NearestReachable(self->s.origin, self, NAV_MAX_SNAP);
    if (from < 0) {
        out->code = NAV_PATH_NO_START_NODE;
        return false;
    }

    to = Nav_NearestReachable(goal, NULL, NAV_MAX_SNAP);
    if (to < 0) {
        out->code = NAV_PATH_NO_GOAL_NODE;
        return false;
    }

    num = Nav_FindPathCaps(from, to, caps, path, q_countof(path));
    if (num < 1) {
        out->code = NAV_PATH_NO_PATH;
        return false;
    }

    // Where along the route are we? path[0] is only the node we SNAPPED to,
    // which may well be behind us, so take the furthest of the next few nodes
    // we are actually standing on. Never look past a traversal for that: the
    // landing of a drop is horizontally close to the ledge above it.
    cur = 0;
    limit = min(num, NAV_LOOKAHEAD);
    for (i = 0; i < limit; i++) {
        Nav_NodeOrigin(path[i], node);
        if (Nav_Reached(self, node))
            cur = i;
        if (i + 1 < num && Nav_LinkTraversal(Nav_LinkBetween(path[i], path[i + 1], caps)))
            break;
    }

    if (cur >= num - 1) {
        // on the goal's node: walk straight at the goal itself
        VectorCopy(goal, out->first_move_point);
        VectorCopy(goal, out->second_move_point);
        out->code = NAV_PATH_REACHED_GOAL;
        return true;
    }

    link = Nav_LinkBetween(path[cur], path[cur + 1], caps);
    t = Nav_LinkTraversal(link);

    if (t) {
        VectorCopy(t->start, out->first_move_point);
        VectorCopy(t->end, out->second_move_point);

        Nav_NodeOrigin(path[cur], node);
        if (Nav_Reached(self, node) || Nav_Reached(self, t->start)) {
            out->code = NAV_PATH_TRAVERSAL;
        } else {
            // not at the take-off yet: walk to it first
            out->code = NAV_PATH_IN_PROGRESS;
        }
        return true;
    }

    // string pull: the furthest node ahead we could walk straight to, stopping
    // at the node a traversal leaves from - that jump is its own leg
    best = cur + 1;
    limit = min(num, cur + 1 + NAV_LOOKAHEAD);
    for (i = best + 1; i < limit; i++) {
        if (Nav_LinkTraversal(Nav_LinkBetween(path[i - 1], path[i], caps)))
            break;
        Nav_NodeOrigin(path[i], node);
        if (!Nav_BodyReaches(self, node))
            break;
        best = i;
    }

    Nav_NodeOrigin(path[best], out->first_move_point);
    if (best + 1 < num)
        Nav_NodeOrigin(path[best + 1], out->second_move_point);
    else
        VectorCopy(goal, out->second_move_point);
    out->code = NAV_PATH_IN_PROGRESS;
    return true;
}

/*
=================
Nav_MonsterCanPath

Is there a mesh, and is this monster one the mesh describes and we are allowed
to steer? The rest of the rerelease's gating lives in M_MoveToPath.
=================
*/
bool Nav_MonsterCanPath(edict_t *self)
{
    if (!Nav_Loaded())
        return false;

    // an authored hint_path network beats the navmesh wherever one exists
    if (hint_paths_present)
        return false;

    if (self->monsterinfo.aiflags & AI_HINT_PATH)
        return false;

    // The mesh describes walkable floor. The rerelease also paths fliers and
    // swimmers, but through SV_alternate_flystep's nav_path branches, which
    // this tree does not have.
    if (self->flags & (FL_FLY | FL_SWIM))
        return false;

    if (self->s.number < 1 || self->s.number >= MAX_EDICTS)
        return false;

    return true;
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
