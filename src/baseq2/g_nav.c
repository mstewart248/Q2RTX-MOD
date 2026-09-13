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
        nav.num_nodes * (sizeof(float) * 2 + sizeof(int) * 2 + 1), TAG_LEVEL);

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
    }

    nav.g_score[from] = 0;
    nav.f_score[from] = Distance(nav.origins[from], nav.origins[to]);
    nav.open[0] = from;
    nav.open_count = 1;

    while (nav.open_count) {
        int     best = 0, current, l;

        for (i = 1; i < nav.open_count; i++)
            if (nav.f_score[nav.open[i]] < nav.f_score[nav.open[best]])
                best = i;

        current = nav.open[best];
        nav.open[best] = nav.open[--nav.open_count];

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

            for (i = 0; i < nav.open_count; i++)
                if (nav.open[i] == n)
                    break;
            if (i == nav.open_count && nav.open_count < nav.num_nodes)
                nav.open[nav.open_count++] = n;
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

NOT SAVED. The throttle below is a file-static keyed by entity number, so a
savegame load simply re-paths on the next think. That is the whole reason
there are no new monsterinfo fields and no SAVE_VERSION bump for this.

==============================================================================
*/

// next frame each entity may recompute. A* over ~1000 nodes is cheap but not
// free, and a monster does not need a new plan every frame.
static int  nav_next_path[MAX_EDICTS];

#define NAV_REPATH_DELAY    (1 * BASE_FRAMERATE)
#define NAV_WAYPOINT_REACHED 48.0f

void Nav_ClearCombat(void);

void Nav_ClearPursuit(void)
{
    memset(nav_next_path, 0, sizeof(nav_next_path));
    Nav_ClearCombat();
}

/*
=================
Nav_MonsterPursue

Returns true if it set a fresh waypoint into last_sighting. False means "not
applicable, carry on with the classic pursuit" - which is also what happens on
every frame between recomputes, so the monster keeps walking to the waypoint
already chosen.
=================
*/
bool Nav_MonsterPursue(edict_t *self)
{
    static int  path[256];
    nav_caps_t  caps;
    vec3_t      waypoint;
    int         n, i, num;

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

    n = self->s.number;
    if (n < 0 || n >= MAX_EDICTS)
        return false;

    if (level.framenum < nav_next_path[n])
        return false;

    nav_next_path[n] = level.framenum + NAV_REPATH_DELAY;

    // route only over moves this monster can actually make. A monster with no
    // jump at all is limited to plain floor, which is correct - it should go
    // the long way round rather than get stuck at a ledge.
    caps.jump_height = self->monsterinfo.can_jump ? self->monsterinfo.jump_height : 0;
    caps.drop_height = self->monsterinfo.drop_height;

    num = Nav_PathToPointCaps(self->s.origin, self->enemy->s.origin, &caps,
                              path, q_countof(path));
    if (num < 2)
        return false;

    // walk forward past any node we are effectively standing on, so we aim at
    // somewhere we actually have to travel to
    for (i = 0; i < num; i++) {
        if (!Nav_NodeOrigin(path[i], waypoint))
            return false;
        if (Distance(waypoint, self->s.origin) > NAV_WAYPOINT_REACHED)
            break;
    }
    if (i == num)
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

Returns the point to walk at.  The answer is cached per edict between
recomputes, because pathing every monster every frame is far too expensive and
because the monster needs something to keep walking towards in between.
=================
*/
static vec3_t   nav_combat_goal[MAX_EDICTS];
static int      nav_combat_until[MAX_EDICTS];
static int      nav_combat_next[MAX_EDICTS];

#define NAV_COMBAT_GOAL_LIFETIME    (3 * BASE_FRAMERATE)

void Nav_ClearCombat(void)
{
    memset(nav_combat_until, 0, sizeof(nav_combat_until));
    memset(nav_combat_next, 0, sizeof(nav_combat_next));
}

bool Nav_CombatWaypoint(edict_t *self, vec3_t out)
{
    static int  path[256];
    nav_caps_t  caps;
    vec3_t      waypoint;
    float       zdiff, standing;
    int         n, i, num;

    if (!Nav_Loaded() || hint_paths_present)
        return false;

    if (!self->enemy || !self->enemy->inuse || self->enemy->health <= 0)
        return false;

    // something else is already steering this monster
    if (self->monsterinfo.aiflags & (AI_HINT_PATH | AI_COMBAT_POINT |
                                     AI_SOUND_TARGET | AI_TARGET_ANGER))
        return false;

    // the mesh describes walkable floor
    if (self->flags & (FL_FLY | FL_SWIM))
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

    n = self->s.number;
    if (n < 0 || n >= MAX_EDICTS)
        return false;

    if (level.framenum >= nav_combat_next[n]) {
        nav_combat_next[n] = level.framenum + NAV_REPATH_DELAY;

        caps.jump_height = self->monsterinfo.can_jump ? self->monsterinfo.jump_height : 0;
        caps.drop_height = self->monsterinfo.drop_height;

        num = Nav_PathToPointCaps(self->s.origin, self->enemy->s.origin, &caps,
                                  path, q_countof(path));
        if (num >= 2) {
            // skip any node we are effectively standing on already
            for (i = 0; i < num; i++) {
                if (!Nav_NodeOrigin(path[i], waypoint))
                    break;
                if (Distance(waypoint, self->s.origin) > NAV_WAYPOINT_REACHED)
                    break;
            }
            if (i < num && Nav_NodeOrigin(path[i], waypoint)) {
                VectorCopy(waypoint, nav_combat_goal[n]);
                nav_combat_until[n] = level.framenum + NAV_COMBAT_GOAL_LIFETIME;
            }
        }
    }

    if (level.framenum > nav_combat_until[n])
        return false;

    VectorCopy(nav_combat_goal[n], out);
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
