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

// first_link is 16 bits in the file but 32 here: a mesh Nav_Generate builds
// for a map with no .nav can carry more than 65535 links
typedef struct {
    uint16_t    flags;
    uint16_t    num_links;
    uint32_t    first_link;
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

    // A* scratch, allocated once with the mesh rather than per query (see
    // Nav_AllocScratch). The open set is a binary heap on f_score; heap_pos
    // is each node's slot in it, -1 when it is not there.
    float           *g_score;
    float           *f_score;
    int             *came_from;
    byte            *closed;
    int             *open;
    int             *heap_pos;
    int             open_count;

    // per link: what a PLAYER does to take it (nav_step_t). Worked out the
    // first time a bot asks (Nav_PrepareKinds), since the teleporters and
    // lifts it is read against do not exist yet when the mesh is loaded.
    // Nav_Generate fills it in as it builds.
    byte            *link_kind;
    bool            kinds_ready;
    bool            generated;      // built by Nav_Generate, not loaded
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

/*
=================
Nav_AllocScratch

The A* arrays and the per traversal lift flags, sized from the counts already
in `nav`. TAG_LEVEL like the mesh, so the same FreeTags reclaims them.
=================
*/
static void Nav_AllocScratch(void)
{
    int     n = nav.num_nodes;
    byte    *block;

    block = gi.TagMalloc(n * (sizeof(float) * 2 + sizeof(int) * 3 + 1) +
                         nav.num_links + 1, TAG_LEVEL);

    nav.g_score = (float *)block;
    block += n * sizeof(float);
    nav.f_score = (float *)block;
    block += n * sizeof(float);
    nav.came_from = (int *)block;
    block += n * sizeof(int);
    nav.open = (int *)block;
    block += n * sizeof(int);
    nav.heap_pos = (int *)block;
    block += n * sizeof(int);
    nav.closed = block;
    block += n;
    nav.link_kind = block;
    memset(nav.link_kind, NAV_STEP_WALK, nav.num_links + 1);
    nav.kinds_ready = false;
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

    // one allocation for the mesh itself; the A* scratch is its own block
    block = gi.TagMalloc(
        nav.num_nodes * sizeof(nav_node_t) +
        nav.num_nodes * sizeof(vec3_t) +
        nav.num_links * sizeof(nav_link_t) +
        nav.num_traversals * sizeof(nav_traversal_t), TAG_LEVEL);

    nav.nodes = (nav_node_t *)block;
    block += nav.num_nodes * sizeof(nav_node_t);
    nav.origins = (vec3_t *)block;
    block += nav.num_nodes * sizeof(vec3_t);
    nav.links = (nav_link_t *)block;
    block += nav.num_links * sizeof(nav_link_t);
    nav.traversals = (nav_traversal_t *)block;

    Nav_AllocScratch();

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

    // a bot: the player's own rules, worked out per link by Nav_PrepareKinds
    if (caps && caps->player)
        return nav.link_kind[link - nav.links] != NAV_STEP_BLOCKED;

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
written, or 0 if there is no path. The open set is a binary heap: the shipped
meshes are a few hundred nodes, but one Nav_Generate builds for a map with no
.nav runs to thousands, and every bot asks for routes several times a second.
=================
*/
int Nav_FindPath(int from, int to, int *out, int max_out)
{
    return Nav_FindPathCaps(from, to, NULL, out, max_out);
}

static void nav_heap_up(int i)
{
    int n = nav.open[i];

    while (i > 0) {
        int p = (i - 1) / 2;

        if (nav.f_score[nav.open[p]] <= nav.f_score[n])
            break;
        nav.open[i] = nav.open[p];
        nav.heap_pos[nav.open[i]] = i;
        i = p;
    }
    nav.open[i] = n;
    nav.heap_pos[n] = i;
}

static int nav_heap_pop(void)
{
    int top = nav.open[0];
    int last, i;

    nav.heap_pos[top] = -1;
    if (--nav.open_count == 0)
        return top;

    last = nav.open[nav.open_count];
    i = 0;
    while (1) {
        int c = 2 * i + 1;

        if (c >= nav.open_count)
            break;
        if (c + 1 < nav.open_count && nav.f_score[nav.open[c + 1]] < nav.f_score[nav.open[c]])
            c++;
        if (nav.f_score[last] <= nav.f_score[nav.open[c]])
            break;
        nav.open[i] = nav.open[c];
        nav.heap_pos[nav.open[i]] = i;
        i = c;
    }
    nav.open[i] = last;
    nav.heap_pos[last] = i;
    return top;
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
        nav.heap_pos[i] = -1;
    }

    nav.g_score[from] = 0;
    nav.f_score[from] = Distance(nav.origins[from], nav.origins[to]);
    nav.open[0] = from;
    nav.heap_pos[from] = 0;
    nav.open_count = 1;

    while (nav.open_count) {
        int     current, l;

        current = nav_heap_pop();

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

            // a node already in the heap only ever gets cheaper here, so it
            // moves up from where it is; a new one goes on the end
            if (nav.heap_pos[n] < 0) {
                if (nav.open_count >= nav.num_nodes)
                    continue;
                nav.open[nav.open_count] = n;
                nav.heap_pos[n] = nav.open_count++;
            }
            nav_heap_up(nav.heap_pos[n]);
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

    // A monster actually walking a hint_path chain keeps to it. A map merely
    // HAVING hint_paths is not a reason to switch the mesh off: the rerelease
    // has no such gate, and with it every Ground Zero map (they all carry
    // hint_paths, and all ship a .nav) lost SV_NewChaseDir's stuck fallback -
    // a stalker below a ledge ran into the wall under the player forever
    // instead of pathing round to the ramp.
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
==============================================================================

BOTS

The bots (g_bot.c) route over the same mesh, but as players: every link a
person can walk, drop off, jump, ride or teleport across. What each link asks
of them is worked out once per level (Nav_PrepareForBots) from its type byte,
its traversal, and the map's teleporters and lifts. The type byte is not
documented anywhere; reading it off all 31 DM meshes:

    0   plain floor                     5, 11   a jump up onto something
    1, 8, 7  a jump across a gap         3      a drop off a ledge
    2   a teleporter (horizontal reach 361 to 4006, no traversal)
    4, 10   a lift or a ladder (up to 780 units, almost no horizontal reach)
    13  up 72-192 and well across - a rocket jump; players skip it

A map with no usable .nav - the whole classic game unless the rerelease's
files are installed beside it, the N64 and CTF maps (other versions), and
anything custom - gets one built by Nav_Generate.

==============================================================================
*/

extern void teleporter_touch(edict_t *self, edict_t *other, cplane_t *plane, csurface_t *surf);

#define NAV_MAX_TELEPORTERS     128     // Quake III maps: jump pads count too

// a standing jump clears about 45 units; anything above this is not a jump
#define NAV_PLAYER_JUMP         56.0f

typedef struct {
    vec3_t  src;        // centre of the pad's trigger
    vec3_t  dest;       // where it puts you
} nav_teleporter_t;

static nav_teleporter_t nav_tele[NAV_MAX_TELEPORTERS];
static int              nav_num_tele;

static void Nav_Generate(void);

static void Nav_FindTeleporters(void)
{
    edict_t *e, *dest;
    int     i;

    nav_num_tele = 0;
    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        vec3_t  landing;
        bool    jumppad = false;

        e = g_edicts + i;
        if (!e->inuse || !e->target)
            continue;

        // Quake III's teleporters and jump pads (g_q3.c) take a bot from one
        // place to another just as these pads do: walk in, come out there
        if (e->touch == q3_jumppad_touch) {
            if (!Q3_JumpPadLanding(e, landing))
                continue;
            jumppad = true;
            dest = NULL;
        } else if (e->touch == teleporter_touch || e->touch == q3_teleport_touch ||
                   (e->touch == q1_teleport_touch && !e->targetname)) {
            // (a Quake 1 one with a targetname carries only just after it fires)
            dest = G_Find(NULL, FOFS(targetname), e->target);
            if (!dest)
                continue;
        } else {
            continue;
        }

        if (nav_num_tele == NAV_MAX_TELEPORTERS)
            break;

        VectorAdd(e->absmin, e->absmax, nav_tele[nav_num_tele].src);
        VectorScale(nav_tele[nav_num_tele].src, 0.5f, nav_tele[nav_num_tele].src);
        if (jumppad)
            VectorCopy(landing, nav_tele[nav_num_tele].dest);
        else
            VectorCopy(dest->s.origin, nav_tele[nav_num_tele].dest);
        nav_num_tele++;
    }
}

static float Nav_Dist2D(const vec3_t a, const vec3_t b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1];

    return sqrtf(dx * dx + dy * dy);
}

// the teleporter whose pad `a` is beside and whose far end `b` is beside
static int Nav_TeleporterFor(const vec3_t a, const vec3_t b)
{
    int i;

    for (i = 0; i < nav_num_tele; i++) {
        if (Nav_Dist2D(a, nav_tele[i].src) > 96 || fabsf(a[2] - nav_tele[i].src[2]) > 96)
            continue;
        if (Distance(b, nav_tele[i].dest) > 192)
            continue;
        return i;
    }

    return -1;
}

// a plat, a train or a door used as a lift over this spot
static bool Nav_MoverNear(const vec3_t point)
{
    edict_t *e;
    int     i;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        e = g_edicts + i;
        if (!e->inuse || e->solid != SOLID_BSP || e->movetype != MOVETYPE_PUSH)
            continue;
        if (point[0] < e->absmin[0] - 32 || point[0] > e->absmax[0] + 32 ||
            point[1] < e->absmin[1] - 32 || point[1] > e->absmax[1] + 32)
            continue;
        return true;
    }

    return false;
}

static nav_step_t Nav_ClassifyLink(int a, const nav_link_t *link)
{
    const nav_traversal_t   *t = Nav_LinkTraversal(link);
    const float             *pa = nav.origins[a];
    const float             *pb = nav.origins[link->target];
    int                     type = link->flags & 0xff;
    vec3_t                  s, e;
    float                   dz, horiz;

    if (t) {
        VectorCopy(t->start, s);
        VectorCopy(t->end, e);
    } else {
        VectorCopy(pa, s);
        VectorCopy(pb, e);
    }
    dz = e[2] - s[2];
    horiz = Nav_Dist2D(s, e);

    if (Nav_Dist2D(pa, pb) > 96 && Nav_TeleporterFor(pa, pb) >= 0)
        return NAV_STEP_TELEPORT;
    if (!t && type == 2)
        return NAV_STEP_TELEPORT;

    if (type == 4 || type == 10)
        return NAV_STEP_ELEVATOR;
    if (dz > NAV_PLAYER_JUMP && horiz < 96 && Nav_MoverNear(s))
        return NAV_STEP_ELEVATOR;

    // plain floor: ramps and stairs as well as level ground
    if (!t)
        return dz < -NAV_PLAYER_JUMP ? NAV_STEP_DROP : NAV_STEP_WALK;

    if (dz > NAV_PLAYER_JUMP)
        return NAV_STEP_BLOCKED;    // a rocket jump
    if (dz < -24 && horiz < 96)
        return NAV_STEP_DROP;
    return NAV_STEP_JUMP;
}

/*
=================
Nav_PrepareForBots

Once per level, when the first bot needs a route: build a mesh if the map has
none, then read every link as a player.
=================
*/
void Nav_PrepareForBots(void)
{
    int counts[NAV_STEP_BLOCKED + 1] = { 0 };
    int a, l;

    if (nav.kinds_ready)
        return;

    if (!Nav_Loaded())
        Nav_Generate();
    if (!Nav_Loaded())
        return;

    Nav_FindTeleporters();

    for (a = 0; a < nav.num_nodes; a++) {
        for (l = 0; l < nav.nodes[a].num_links; l++) {
            int idx = nav.nodes[a].first_link + l;

            if (!nav.generated)
                nav.link_kind[idx] = Nav_ClassifyLink(a, &nav.links[idx]);
            counts[nav.link_kind[idx]]++;
        }
    }

    nav.kinds_ready = true;

    gi.dprintf("Nav: for bots - %d walk, %d drop, %d jump, %d lift, %d teleport, %d skipped\n",
               counts[NAV_STEP_WALK], counts[NAV_STEP_DROP], counts[NAV_STEP_JUMP],
               counts[NAV_STEP_ELEVATOR], counts[NAV_STEP_TELEPORT], counts[NAV_STEP_BLOCKED]);
}

int Nav_NumNodes(void)
{
    return nav.num_nodes;
}

bool Nav_IsGenerated(void)
{
    return nav.generated;
}

bool Nav_BodyCanReach(edict_t *self, const vec3_t point)
{
    return Nav_Loaded() && Nav_BodyReaches(self, point);
}

/*
=================
Nav_RouteFor

A player's route from where `self` stands to `goal`: node indices into `out`,
the count returned (0 = none). *length is the route's length in units.
=================
*/
int Nav_RouteFor(edict_t *self, const vec3_t goal, int *out, int max_out, float *length)
{
    static const nav_caps_t caps = { 0, 0, true };
    int from, to, n;

    if (length)
        *length = 0;

    Nav_PrepareForBots();
    if (!Nav_Loaded())
        return 0;

    from = Nav_NearestReachable(self->s.origin, self, NAV_MAX_SNAP);
    to = Nav_NearestReachable(goal, NULL, NAV_MAX_SNAP);
    if (from < 0 || to < 0)
        return 0;

    n = Nav_FindPathCaps(from, to, &caps, out, max_out);
    if (n > 1 && length)
        *length = nav.g_score[to];
    return n;
}

/*
=================
Nav_StepKind

How to get from node `a` to node `b` of a route, and the two points that
step runs between: the traversal's take-off and landing, the teleporter pad
and its exit, or simply the two nodes.
=================
*/
nav_step_t Nav_StepKind(int a, int b, vec3_t start, vec3_t end)
{
    const nav_traversal_t   *t;
    const nav_link_t        *link = NULL;
    nav_step_t              kind = NAV_STEP_WALK;
    int                     l, i;

    if (!Nav_Loaded() || a < 0 || b < 0 || a >= nav.num_nodes || b >= nav.num_nodes) {
        VectorClear(start);
        VectorClear(end);
        return NAV_STEP_WALK;
    }

    VectorCopy(nav.origins[a], start);
    VectorCopy(nav.origins[b], end);

    for (l = 0; l < nav.nodes[a].num_links; l++) {
        const nav_link_t *k = &nav.links[nav.nodes[a].first_link + l];

        if (k->target == b && nav.link_kind[k - nav.links] != NAV_STEP_BLOCKED) {
            link = k;
            break;
        }
    }
    if (!link)
        return NAV_STEP_WALK;

    kind = nav.link_kind[link - nav.links];

    t = Nav_LinkTraversal(link);
    if (t) {
        VectorCopy(t->start, start);
        VectorCopy(t->end, end);
    }

    // walk onto the pad itself, not just to the node beside it
    if (kind == NAV_STEP_TELEPORT && !nav.generated) {
        i = Nav_TeleporterFor(nav.origins[a], nav.origins[b]);
        if (i >= 0) {
            start[0] = nav_tele[i].src[0];
            start[1] = nav_tele[i].src[1];
        }
    }

    return kind;
}


/*
==============================================================================

GENERATED MESH

A flood fill over a 32 unit grid from every spawn point, item and teleporter:
a node wherever a player can stand, a link wherever one can walk, climb a
stair or ramp, drop, or jump up to the next cell - plus the lifts and the
teleporters, which no amount of walking finds. Coarser than the shipped
meshes, and it does not swim or climb ladders (deep water, lava and slime are
left out), but it reaches every item a player can reach on foot.

Doors that open when walked into are taken out of the world while it runs,
and so are the players, or one standing on a spawn point would put a node on
their head.

==============================================================================
*/

#define GEN_STEP            32
#define GEN_MAX_NODES       24000
#define GEN_MAX_LINKS       (GEN_MAX_NODES * 9)
#define GEN_STEPHEIGHT      18.0f
#define GEN_RAMPHEIGHT      48.0f
#define GEN_JUMPHEIGHT      40.0f
#define GEN_MAX_DROP        300.0f
#define GEN_HASH_SIZE       16384
#define GEN_MAX_MOVED       256

typedef struct {
    int     a, b;
    byte    kind;
} gen_link_t;

typedef struct {
    vec3_t      *feet;      // node positions, on the floor
    int         *gx, *gy;   // grid cell
    int         *next;      // hash chain
    int         hash[GEN_HASH_SIZE];
    int         num;
    gen_link_t  *links;
    int         num_links;
    int         *queue;
    int         qhead, qtail;
} gen_state_t;

static gen_state_t  *gen;
static const vec3_t gen_mins = { -12, -12, -24 };
static const vec3_t gen_maxs = { 12, 12, 32 };

static unsigned gen_key(int gx, int gy)
{
    return ((unsigned)gx * 73856093u ^ (unsigned)gy * 19349663u) & (GEN_HASH_SIZE - 1);
}

// the node in this cell on this floor, or -1
static int gen_find(int gx, int gy, float z)
{
    int i;

    for (i = gen->hash[gen_key(gx, gy)]; i >= 0; i = gen->next[i])
        if (gen->gx[i] == gx && gen->gy[i] == gy && fabsf(gen->feet[i][2] - z) < 20)
            return i;

    return -1;
}

static int gen_add(int gx, int gy, const vec3_t feet)
{
    int         i = gen_find(gx, gy, feet[2]);
    unsigned    h;

    if (i >= 0)
        return i;
    if (gen->num >= GEN_MAX_NODES)
        return -1;

    i = gen->num++;
    VectorCopy(feet, gen->feet[i]);
    gen->gx[i] = gx;
    gen->gy[i] = gy;
    h = gen_key(gx, gy);
    gen->next[i] = gen->hash[h];
    gen->hash[h] = i;
    gen->queue[gen->qtail++] = i;
    return i;
}

static void gen_link(int a, int b, nav_step_t kind)
{
    int i;

    if (a < 0 || b < 0 || a == b)
        return;

    // a node's own links are added together, so a short look back finds a
    // duplicate (two directions that ended on the same off-grid seed)
    for (i = gen->num_links - 1; i >= 0 && i >= gen->num_links - 12; i--)
        if (gen->links[i].a == a && gen->links[i].b == b)
            return;

    if (gen->num_links >= GEN_MAX_LINKS)
        return;

    gen->links[gen->num_links].a = a;
    gen->links[gen->num_links].b = b;
    gen->links[gen->num_links].kind = kind;
    gen->num_links++;
}

/*
Where a player at `top`'s xy would stand, looking at most `depth` down: the
floor point under the hull. false over a pit, on a slope too steep to stand
on, in lava or slime, or with their head under water.
*/
static bool gen_floor(const vec3_t top, float depth, vec3_t feet)
{
    vec3_t  end, p;
    trace_t tr;

    VectorSet(end, top[0], top[1], top[2] - depth);
    tr = gi.trace(top, gen_mins, gen_maxs, end, g_edicts, MASK_PLAYERSOLID);
    if (tr.startsolid || tr.allsolid || tr.fraction == 1.0f)
        return false;
    if (tr.plane.normal[2] < 0.7f)
        return false;

    VectorCopy(tr.endpos, feet);
    feet[2] += gen_mins[2];

    VectorSet(p, feet[0], feet[1], feet[2] + 1);
    if (gi.pointcontents(p) & (CONTENTS_LAVA | CONTENTS_SLIME))
        return false;

    p[2] = feet[2] + 46;    // eye height
    if (gi.pointcontents(p) & MASK_WATER)
        return false;

    return true;
}

static void gen_try_move(int n, const vec3_t origin, float tx, float ty, int gx, int gy)
{
    static const float  lifts[2] = { GEN_STEPHEIGHT, GEN_RAMPHEIGHT };
    const float         *feet = gen->feet[n];
    vec3_t              start, end, land;
    trace_t             tr;
    int                 i;

    // walk: up a stair's height (or, against a ramp, enough to clear it),
    // across, and down onto whatever floor is there
    for (i = 0; i < 2; i++) {
        VectorSet(start, origin[0], origin[1], origin[2] + lifts[i]);
        tr = gi.trace(origin, gen_mins, gen_maxs, start, g_edicts, MASK_PLAYERSOLID);
        if (tr.fraction < 1.0f)
            break;

        VectorSet(end, tx, ty, start[2]);
        tr = gi.trace(start, gen_mins, gen_maxs, end, g_edicts, MASK_PLAYERSOLID);
        if (tr.startsolid)
            break;

        if (tr.fraction == 1.0f) {
            float dz;

            if (!gen_floor(end, lifts[i] + GEN_MAX_DROP, land))
                return;

            dz = land[2] - feet[2];
            gen_link(n, gen_add(gx, gy, land),
                     dz < -GEN_RAMPHEIGHT ? NAV_STEP_DROP : NAV_STEP_WALK);
            return;
        }

        // only a slope a player can walk up earns the higher try
        if (tr.plane.normal[2] < 0.7f)
            break;
    }

    // a jump up onto a ledge
    VectorSet(start, origin[0], origin[1], origin[2] + GEN_JUMPHEIGHT);
    tr = gi.trace(origin, gen_mins, gen_maxs, start, g_edicts, MASK_PLAYERSOLID);
    if (tr.fraction < 1.0f)
        return;

    VectorSet(end, tx, ty, start[2]);
    tr = gi.trace(start, gen_mins, gen_maxs, end, g_edicts, MASK_PLAYERSOLID);
    if (tr.startsolid || tr.fraction < 1.0f)
        return;

    if (!gen_floor(end, GEN_JUMPHEIGHT + 8, land))
        return;
    if (land[2] - feet[2] <= GEN_STEPHEIGHT || land[2] - feet[2] > GEN_JUMPHEIGHT)
        return;

    gen_link(n, gen_add(gx, gy, land), NAV_STEP_JUMP);
}

static void gen_expand(int n)
{
    vec3_t  origin;
    int     dx, dy;

    VectorCopy(gen->feet[n], origin);
    origin[2] -= gen_mins[2];

    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            int gx = gen->gx[n] + dx;
            int gy = gen->gy[n] + dy;

            if (!dx && !dy)
                continue;
            gen_try_move(n, origin, gx * GEN_STEP, gy * GEN_STEP, gx, gy);
        }
    }
}

// a node where `point` stands, in the grid cell around it
static int gen_seed(const vec3_t point)
{
    static const float  raise[2] = { 16, 2 };
    vec3_t              top, land;
    int                 i;

    for (i = 0; i < 2; i++) {
        VectorSet(top, point[0], point[1], point[2] + raise[i]);
        if (gen_floor(top, 160, land))
            return gen_add((int)floorf(land[0] / GEN_STEP + 0.5f),
                           (int)floorf(land[1] / GEN_STEP + 0.5f), land);
    }

    return -1;
}

// a node at exactly this floor point, if a player fits there
static int gen_place(const vec3_t feet)
{
    vec3_t  origin;
    trace_t tr;

    VectorSet(origin, feet[0], feet[1], feet[2] - gen_mins[2]);
    tr = gi.trace(origin, gen_mins, gen_maxs, origin, g_edicts, MASK_PLAYERSOLID);
    if (tr.startsolid || tr.allsolid)
        return -1;

    return gen_add((int)floorf(feet[0] / GEN_STEP + 0.5f),
                   (int)floorf(feet[1] / GEN_STEP + 0.5f), feet);
}

// a link up each func_plat: from its floor at the bottom to the same spot at
// the top, where it will be when it has carried you there
static void gen_plats(void)
{
    edict_t *e;
    int     i;

    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        vec3_t  bottom, top;
        float   height, offset;
        int     a, b;

        e = g_edicts + i;
        if (!e->inuse || !e->classname || Q_strncasecmp(e->classname, "func_plat", 9))
            continue;

        height = e->pos1[2] - e->pos2[2];
        if (height < GEN_JUMPHEIGHT)
            continue;

        // how far up it is right now (0 at rest at the bottom)
        offset = e->s.origin[2] - e->pos2[2];

        VectorSet(bottom, (e->absmin[0] + e->absmax[0]) * 0.5f,
                  (e->absmin[1] + e->absmax[1]) * 0.5f, e->absmax[2] - offset);
        VectorSet(top, bottom[0], bottom[1], bottom[2] + height);

        a = gen_place(bottom);
        b = gen_place(top);
        gen_link(a, b, NAV_STEP_ELEVATOR);
    }
}

static void Nav_Generate(void)
{
    edict_t     *moved[GEN_MAX_MOVED];
    int         num_moved = 0;
    int         i, *counts, total;
    edict_t     *e;
    byte        *block;

    if (!deathmatch->value)
        return;

    gen = gi.TagMalloc(sizeof(*gen), TAG_LEVEL);
    memset(gen, 0, sizeof(*gen));
    gen->feet = gi.TagMalloc(GEN_MAX_NODES * sizeof(vec3_t), TAG_LEVEL);
    gen->gx = gi.TagMalloc(GEN_MAX_NODES * sizeof(int), TAG_LEVEL);
    gen->gy = gi.TagMalloc(GEN_MAX_NODES * sizeof(int), TAG_LEVEL);
    gen->next = gi.TagMalloc(GEN_MAX_NODES * sizeof(int), TAG_LEVEL);
    gen->queue = gi.TagMalloc(GEN_MAX_NODES * sizeof(int), TAG_LEVEL);
    gen->links = gi.TagMalloc(GEN_MAX_LINKS * sizeof(gen_link_t), TAG_LEVEL);
    for (i = 0; i < GEN_HASH_SIZE; i++)
        gen->hash[i] = -1;

    // out of the way while it runs: walk-in doors, and the players
    for (i = 1; i < globals.num_edicts && num_moved < GEN_MAX_MOVED; i++) {
        e = g_edicts + i;
        if (!e->inuse || e->solid == SOLID_NOT)
            continue;

        if (i <= game.maxclients) {
            moved[num_moved++] = e;
            gi.unlinkentity(e);
            continue;
        }

        if (e->classname && !e->targetname && !e->health &&
            (!Q_stricmp(e->classname, "func_door") || !Q_stricmp(e->classname, "func_door_rotating"))) {
            moved[num_moved++] = e;
            gi.unlinkentity(e);
        }
    }

    Nav_FindTeleporters();

    // seeds: everywhere a player starts, everything worth walking to, and
    // both ends of every teleporter
    for (i = game.maxclients + 1; i < globals.num_edicts; i++) {
        e = g_edicts + i;
        if (!e->inuse || !e->classname)
            continue;
        if (e->item || !Q_strncasecmp(e->classname, "info_player_", 12))
            gen_seed(e->s.origin);
    }
    for (i = 0; i < nav_num_tele; i++)
        gen_link(gen_seed(nav_tele[i].src), gen_seed(nav_tele[i].dest), NAV_STEP_TELEPORT);
    gen_plats();

    while (gen->qhead < gen->qtail)
        gen_expand(gen->queue[gen->qhead++]);

    for (i = 0; i < num_moved; i++)
        gi.linkentity(moved[i]);

    if (gen->num < 2) {
        gi.dprintf("Nav: could not build a mesh for %s\n", level.mapname);
        goto done;
    }

    // into the same layout a loaded mesh has, links grouped by node
    Nav_Free();
    nav.num_nodes = gen->num;
    nav.num_links = gen->num_links;
    nav.num_traversals = 0;
    nav.agent_radius = 0.8f;

    block = gi.TagMalloc(nav.num_nodes * (sizeof(nav_node_t) + sizeof(vec3_t)) +
                         nav.num_links * sizeof(nav_link_t), TAG_LEVEL);
    nav.nodes = (nav_node_t *)block;
    block += nav.num_nodes * sizeof(nav_node_t);
    nav.origins = (vec3_t *)block;
    block += nav.num_nodes * sizeof(vec3_t);
    nav.links = (nav_link_t *)block;

    Nav_AllocScratch();

    counts = nav.came_from;     // scratch, before any query uses it
    memset(counts, 0, nav.num_nodes * sizeof(int));
    for (i = 0; i < gen->num_links; i++)
        counts[gen->links[i].a]++;

    total = 0;
    for (i = 0; i < nav.num_nodes; i++) {
        nav.nodes[i].flags = 0;
        nav.nodes[i].radius = GEN_STEP / 2;
        nav.nodes[i].first_link = total;
        nav.nodes[i].num_links = 0;
        total += counts[i];
        VectorCopy(gen->feet[i], nav.origins[i]);
    }

    for (i = 0; i < gen->num_links; i++) {
        nav_node_t  *node = &nav.nodes[gen->links[i].a];
        int         idx = node->first_link + node->num_links++;

        nav.links[idx].target = gen->links[i].b;
        nav.links[idx].flags = 0;
        nav.links[idx].traversal = NAV_INVALID;
        nav.link_kind[idx] = gen->links[i].kind;
    }

    nav.generated = true;

    gi.dprintf("Nav: no navmesh for %s - built one: %d nodes, %d links\n",
               level.mapname, nav.num_nodes, nav.num_links);

done:
    gi.TagFree(gen->links);
    gi.TagFree(gen->queue);
    gi.TagFree(gen->next);
    gi.TagFree(gen->gy);
    gi.TagFree(gen->gx);
    gi.TagFree(gen->feet);
    gi.TagFree(gen);
    gen = NULL;
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
