/*
Copyright (C) 2026 Matt Stewart

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
SURFACE BLOOD - cl_blood_film (default 0)

Blood that has LANDED, simulated as a depth field on the surfaces themselves
instead of as splats that slide, merge and get pushed around. Droplets in flight
are still effects.c's particles; this is everything after they hit the world.

WHY. The splat system models every behaviour with its own rule - cling, trails,
the drain at the foot of a wall, pooling, the pool cap, the stretch limit, the
spread along a wall, the rim probe, the edge shove - and every one of them is a
place where two rules disagree. Matt (2026-11-06): "the current system looks
pretty good but there are these edge cases and problems that keep it a little
uncanny." Here they all come out of one rule: blood flows from high HEAD to low,
head being how high the surface is plus (on ground) how deep the blood on it is,
and some depth - the `hold` - never moves at all.

  - Floors: head is the depth, so a puddle spreads until it is cl_blood_film_thick
    deep and stops. Puddles that touch are one puddle. A wall standing on the
    floor is solid cells, so a pool at its foot spreads AWAY from it - what Matt
    asked for, with nothing written to make it happen.
  - Walls: head is the height, so blood runs down. It STARTS only once it is
    deeper than cl_blood_film_cling - one droplet never is, so spatter stays where
    it lands - and leaves cl_blood_film_trail behind it as it goes. A cell a run
    has passed through stays wet and runs at the trail depth from then on, so
    later blood follows the old trails, as it does on a real wall.
  - The foot of a wall: the cell below is solid, so the blood is handed to the
    surface it ran into (film_corner), a little way out from the wall.
  - An edge: blood spilling over it collects and drops off as a droplet, which
    lands wherever it lands - the existing flight code.
  - Water: blood running into it becomes effects.c's dissolving cloud.

THE GRID. One sparse grid per world plane, in TILES of 16x16 cells allocated as
blood reaches them. Each tile's cells are classified once, by tracing, when it is
made: SURFACE (blood can lie here), BLOCKED (something stands on the surface
here), OPEN (no surface - past an edge) or LIQUID (under water). World geometry
only; brush models and bodies keep the splat path.

DRAWING. Each changed tile is meshed - contoured over a smooth coverage field
sampled between the cells (cl_blood_film_detail) - into a triangle list the
renderer draws with the landed-splat material: see film_mesh, blood_film_t in
refresh.h and write_film_geometry in blood.c. A splat's material measures how
thick the blood is from its dome normals; a film is flat, so it hands its depth
over per vertex and its normals are the plane's (uv2.x 2).
*/

#include "client.h"

static cvar_t *cl_blood_film;
static cvar_t *cl_blood_film_tiles;
static cvar_t *cl_blood_film_tris;
static cvar_t *cl_blood_film_cell;
static cvar_t *cl_blood_film_thick;
static cvar_t *cl_blood_film_cling;
static cvar_t *cl_blood_film_trail;
static cvar_t *cl_blood_film_volume;
static cvar_t *cl_blood_film_flow;
static cvar_t *cl_blood_film_run;
static cvar_t *cl_blood_film_rate;
static cvar_t *cl_blood_film_height;
static cvar_t *cl_blood_film_full;
static cvar_t *cl_blood_film_rough;
static cvar_t *cl_blood_film_smooth;
static cvar_t *cl_blood_film_detail;
static cvar_t *cl_blood_film_stats;
static cvar_t *cl_blood_film_mesh_ms;

// Owned by effects.c; read here.
static cvar_t *film_splat_size;
static cvar_t *film_permanent;
static cvar_t *film_splat_life;

#define FILM_TILE       16
#define FILM_CELLS      (FILM_TILE * FILM_TILE)
#define FILM_MAX_PLANES 1024
#define FILM_HASH_SIZE  2048        // power of two

// How far the drawn surface sits off the wall or floor, so the two are never
// coplanar. The outline is at this height and the blood rises from it.
#define FILM_LIFT       0.03f

// Seconds a fading tile takes to go from full to gone (cl_blood_permanent 0).
#define FILM_FADE_SEC   2.0f

// While the pool is starved, fading goes this many times as fast (film_fade).
#define FILM_HURRY      8

// Only a tile faded down to this can be taken over for new blood (film_tile_get).
#define FILM_RECYCLE_ALPHA  0.25f

// Steps with nothing moving before a tile stops being simulated - "nothing"
// being no cell giving away more than FILM_SLEEP in a step. Spatter lands a
// little deeper (0.34) than a puddle settles (cl_blood_film_thick 0.3), so it
// creeps on forever by ~1e-4 a step; summed over a tile that kept every tile
// awake, and re-meshed, on every step of a fight (2026-11-07: 5000 meshes a
// second in a headless fight, half the frames over 2 ms).
#define FILM_CALM_STEPS 12
#define FILM_SLEEP      2e-5f

// How much a cell's blood has to change before it is drawn again. Meshes read
// h_mesh, not h - see film_publish.
#define FILM_VISIBLE    0.008f
#define FILM_VISIBLE_DEEP 0.05f

// How far a changed cell reaches into other quads' meshes: the blur (1), the
// Catmull-Rom sampling between cells (2) and a neighbour quad's fan (1). A cell
// this close to a tile's side changes the tile across it too.
#define FILM_REACH      4

enum {
    CELL_UNKNOWN = 0,   // not classified yet - see film_kind
    CELL_SURFACE = 1,
    CELL_BLOCKED,
    CELL_OPEN,
    CELL_LIQUID,
};

typedef struct {
    bool    used;
    bool    has_color;
    int     refs;           // tiles on this plane
    vec3_t  normal;
    float   dist;
    vec3_t  origin;         // normal * dist: the grid's origin
    vec3_t  u, v;           // in-plane axes, cross(u, v) = normal; on a slope v runs downhill
    float   slope;          // 0 on a floor, 1 on a vertical wall
    float   rise_u, rise_v; // world height gained per cell along u / v
    float   press;          // how much the blood's own depth pushes it sideways

    // One colour per surface, from the first blood to land on it. Per droplet
    // it jitters, and per tile that showed as seams along the tile edges.
    int     color;
    color_t rgba;
} film_plane_t;

typedef struct {
    bool        used;
    bool        awake;          // something on it can still move
    bool        touched;        // dh holds this step's flow
    bool        dirty;          // mesh out of date: mesh it this frame
    bool        due;            // a tick's publish, waiting for the frame's budget - see BUDGET
    bool        shown;          // handed to the renderer last frame
    bool        starved;        // meshed without a neighbour it needed: the pool was full
    int         plane;
    int         tx, ty;
    int         hash_next;
    int         calm;
    int         fed_time;       // cl.time blood last arrived
    int         fade_from;      // cl.time its patch was picked to make room (0: not)
    bool        fade_part;      // ... only this old part of a patch too big to go whole
    float       moved;
    float       moved_max;      // the most any one cell gave away this step
    bool        pending;        // h has moved on from h_mesh by less than shows
    bool        publish_now;    // ... or blood landed: show it on the next step
    float       alpha;
    float       seed;

    // Blood leaving the grid, gathered until there is a droplet's worth.
    float       spill_edge;
    vec3_t      spill_edge_org, spill_edge_dir;
    float       spill_liquid;
    vec3_t      spill_liquid_org;

    float       h[FILM_CELLS];      // depth, world units
    float       h_mesh[FILM_CELLS]; // the depth every mesh is made from - see film_publish
    float       dh[FILM_CELLS];     // this step's change
    float       bump[FILM_CELLS];   // fixed unevenness of the surface, so runs wander
    uint8_t     kind[FILM_CELLS];
    uint8_t     run[FILM_CELLS];    // a run has been through: holds only the trail

    blood_film_vert_t *verts;
    int         num_tris;
    int         max_tris;
    uint32_t    version;
} film_tile_t;

static film_plane_t film_planes[FILM_MAX_PLANES];
static film_tile_t  film_tiles[MAX_BLOOD_FILM_TILES];
static int          film_hash[FILM_HASH_SIZE];

static int          film_cap;       // tiles usable this session: cl_blood_film_tiles
static int          film_chunk_cap; // renderer chunks the tiles may fill: cl_blood_film_tris
static int          film_chunks_used;   // ... wanted by the last frame's tiles, drawn or not
static float        film_cell;      // cell size the grid was built with
static const bsp_t *film_bsp;
static bool         film_ready;
static int          film_last_time;
static float        film_accum;
static uint32_t     film_version_seq;   // never reset, so a reused slot never repeats a version

static blood_film_t film_out[MAX_BLOOD_FILM_TILES];
static int          film_num_out;

// cl_blood_film_stats counters, printed once a second
static int          film_stat_steps, film_stat_meshes, film_stat_drips, film_stat_corner;
static int          film_stat_full, film_stat_new, film_stat_dropped, film_stat_faded;
static int          film_stat_unshown;  // tiles left out of a frame: over cl_blood_film_tris
static int          film_starved;   // tiles asked for with the pool full, since film_fade last ran
static int          film_stat_classified;   // cells traced (film_classify_cell)
static int          film_stat_coarser;  // tiles meshed below cl_blood_film_detail to fit
static uint64_t     film_stat_step_us, film_stat_mesh_us, film_stat_new_us;

/*
===============
Helpers
===============
*/

// Floor division for cell -> tile, which has to be right for negative cells too.
static inline int film_div(int a)
{
    return a >= 0 ? a / FILM_TILE : -((-a - 1) / FILM_TILE) - 1;
}

static inline unsigned film_hash_key(int plane, int tx, int ty)
{
    unsigned h = (unsigned)plane * 73856093u ^ (unsigned)tx * 19349663u ^ (unsigned)ty * 83492791u;
    return h & (FILM_HASH_SIZE - 1);
}

static float film_noise(int plane, int gx, int gy)
{
    unsigned h = (unsigned)gx * 374761393u + (unsigned)gy * 668265263u + (unsigned)plane * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xffff) / 65535.f * 2.f - 1.f;
}

static void film_cell_center(const film_plane_t *p, int gx, int gy, vec3_t out)
{
    VectorMA(p->origin, (gx + 0.5f) * film_cell, p->u, out);
    VectorMA(out, (gy + 0.5f) * film_cell, p->v, out);
}

// The blood one droplet of `radius` puts down: the footprint the splat it would
// have made covered, at the depth a puddle settles to. Lands about twice that
// deep at its centre (film_add spreads it as a cone).
static float film_drop_volume(float radius)
{
    const float spread = radius * max(0.1f, film_splat_size->value);
    return (float)M_PI * spread * spread * max(0.01f, cl_blood_film_thick->value) *
           max(0.f, cl_blood_film_volume->value);
}

static float film_drop_radius(float volume)
{
    const float per = (float)M_PI * max(0.01f, cl_blood_film_thick->value) *
                      max(0.01f, cl_blood_film_volume->value);
    return sqrtf(max(0.f, volume) / per) / max(0.1f, film_splat_size->value);
}

/*
===============
Planes
===============
*/
static int film_find_plane(const vec3_t normal, float dist, bool create)
{
    int free_slot = -1;

    for (int i = 0; i < FILM_MAX_PLANES; i++) {
        film_plane_t *p = &film_planes[i];

        if (!p->used) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }

        if (DotProduct(p->normal, normal) > 0.9995f && fabsf(p->dist - dist) < 0.1f)
            return i;
    }

    if (!create || free_slot < 0)
        return -1;

    film_plane_t *p = &film_planes[free_slot];
    memset(p, 0, sizeof(*p));
    p->used = true;
    VectorCopy(normal, p->normal);
    VectorNormalize(p->normal);
    p->dist = dist;
    VectorScale(p->normal, dist, p->origin);

    // v runs DOWNHILL wherever there is a downhill, which makes the grid's rows
    // level lines on a wall. On flat ground any in-plane axis will do.
    static const vec3_t down = { 0.f, 0.f, -1.f };
    VectorMA(down, -DotProduct(down, p->normal), p->normal, p->v);
    p->slope = VectorNormalize(p->v);

    if (p->slope < 0.05f) {
        vec3_t ax = { 1.f, 0.f, 0.f };
        if (fabsf(p->normal[0]) > 0.9f)
            VectorSet(ax, 0.f, 1.f, 0.f);
        VectorMA(ax, -DotProduct(ax, p->normal), p->normal, p->v);
        VectorNormalize(p->v);
        p->slope = 0.f;
    }

    CrossProduct(p->v, p->normal, p->u);
    VectorNormalize(p->u);

    p->rise_u = p->u[2] * film_cell;
    p->rise_v = p->v[2] * film_cell;

    // On ground the blood's own depth is what spreads it; on a wall it is only
    // weight, and the run goes straight down. A little of it is kept on walls
    // so a thick run widens slightly instead of being one cell wide.
    p->press = max(fabsf(p->normal[2]), 0.2f);

    return free_slot;
}

// A plane nothing ended up on - the landing found no usable cell.
static void film_release_plane_if_empty(int plane)
{
    if (plane >= 0 && film_planes[plane].refs <= 0)
        film_planes[plane].used = false;
}

/*
===============
Tiles
===============
*/
static int film_tile_lookup(int plane, int tx, int ty)
{
    for (int i = film_hash[film_hash_key(plane, tx, ty)]; i >= 0; i = film_tiles[i].hash_next) {
        const film_tile_t *t = &film_tiles[i];
        if (t->plane == plane && t->tx == tx && t->ty == ty)
            return i;
    }
    return -1;
}

static void film_mark_neighbours_dirty(const film_tile_t *t)
{
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy)
                continue;
            const int n = film_tile_lookup(t->plane, t->tx + dx, t->ty + dy);
            if (n >= 0)
                film_tiles[n].dirty = true;
        }
    }
}

/*
WHAT THE MESHES SEE. Every mesh reads h_mesh, never h. A tile copies h into
h_mesh - all its cells at once - only when some cell has changed VISIBLY, and
then re-meshes itself and every neighbour that draws one of the changed cells.
Two tiles meeting along a side therefore always mesh it from the same numbers,
however their meshes are timed: a tile reading its neighbour's live h while the
neighbour's own mesh was older would open a crack between them.

Visibly: where blood is thin its depth IS what shows - the outline, and how dark
and see-through it is - out to twice cl_blood_film_full past the outline level
(the blur mixes a cell's neighbours into it), so FILM_VISIBLE there. Deeper, a
change only raises the surface by as much, so FILM_VISIBLE_DEEP.

And when: on a wall, a big change (a run's front) goes out on the step it
happens, or the run would move in jumps. Everything else waits for the next
publish tick, every FILM_PUBLISH_EVERY steps, which all tiles share - so a
spreading pool's tiles re-mesh together a few times a second instead of each
re-meshing itself and its neighbours on every step. Blood that landed goes out on
the next step, with every other landing on the tile by then.

BUDGET. A tick's publishes are creep, and a fight's worth of pools creeping at
once is hundreds of tiles: all meshed on the tick's frame, that was a 10-13 ms
frame five times a second (2026-11-07, headless, 512 tiles) - choppy. So a tick
only makes a tile DUE, and CL_BloodFilmFrame publishes due tiles in BLOBS - from
the next due tile round-robin, its due neighbours outward, as many as the rest of
cl_blood_film_mesh_ms looks like it will mesh - and meshes everything a blob
dirtied then and there; the rest go on the next frame. A blob, because each
publish re-meshes the tile's neighbours too: scattered one by one, every shared
neighbour was meshed once per neighbour (measured: 60% more meshes). What is deferred is the publish, never
a mesh: meshing a tile's neighbours a frame before the tile itself (tried first)
left their shared side at two different depths - cl_blood_film_height lifts the
surface by it - and the crack check counted hundreds of open edges whenever
tiles were waiting. Blood landing, new and freed tiles and runs are never held
back.

What: only cells that have moved on by a quarter of visible are copied, so a
cell drifting by nothing much along a side does not drag the tile across it
into a re-mesh it has no use for.

Old pools tick less: a second after blood last landed on a tile it publishes
on every 2nd tick, from 3 seconds on every FILM_PUBLISH_SLOW-th - the edge of a
pool still spreading long after the fight moved on advances about once a second
instead of five times. Counted on the shared tick number, so the periods nest and
neighbours still publish together: a tile's publish re-meshes its neighbours
too, and out of step they multiplied (measured: a per-tile back-off saved
nothing).

Before all this, one shotgun blast's pool re-meshed every tile it covered 30
times a second for over 8 seconds, and a fight ran ~5000 meshes a second - each
one a re-upload and an acceleration-structure build in the renderer (2026-11-07,
measured headless).
*/
#define FILM_PUBLISH_EVERY  6       // steps between ticks: 5 Hz at cl_blood_film_rate 30
#define FILM_PUBLISH_NOW    4.f     // on a wall, this many times visible goes out at once
#define FILM_PUBLISH_COPY   0.25f   // cells moved on by less than this much of visible stay
#define FILM_PUBLISH_SLOW   4       // a pool left alone for 3 s publishes on every this-many-th tick

// How far past visible the tile's most-changed cell is (1: just visible).
static float film_unpublished(const film_tile_t *t, float band)
{
    float need = 0.f;

    for (int i = 0; i < FILM_CELLS; i++) {
        const float h = t->h[i], was = t->h_mesh[i];
        if (h == was)
            continue;
        const float visible = min(h, was) < band ? FILM_VISIBLE : FILM_VISIBLE_DEEP;
        need = max(need, fabsf(h - was) / visible);
    }

    return need;
}

static void film_publish(film_tile_t *t, float band)
{
    bool lo_x = false, hi_x = false, lo_y = false, hi_y = false;

    for (int i = 0; i < FILM_CELLS; i++) {
        const float h = t->h[i], was = t->h_mesh[i];
        if (h == was)
            continue;
        const float visible = min(h, was) < band ? FILM_VISIBLE : FILM_VISIBLE_DEEP;
        if (fabsf(h - was) < FILM_PUBLISH_COPY * visible && (h > 0.f) == (was > 0.f))
            continue;
        t->h_mesh[i] = h;

        const int cx = i % FILM_TILE, cy = i / FILM_TILE;
        lo_x |= cx < FILM_REACH;
        hi_x |= cx >= FILM_TILE - FILM_REACH;
        lo_y |= cy < FILM_REACH;
        hi_y |= cy >= FILM_TILE - FILM_REACH;
    }

    t->pending = false;
    t->publish_now = false;
    t->due = false;
    t->dirty = true;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((!dx && !dy) || (dx < 0 && !lo_x) || (dx > 0 && !hi_x) ||
                (dy < 0 && !lo_y) || (dy > 0 && !hi_y))
                continue;
            const int n = film_tile_lookup(t->plane, t->tx + dx, t->ty + dy);
            if (n >= 0)
                film_tiles[n].dirty = true;
        }
    }
}

// The depth under which a change shows at FILM_VISIBLE - see WHAT THE MESHES SEE.
static float film_band(void)
{
    return 2.f * (max(0.005f, cl_blood_film_trail->value * 0.4f) +
                  max(0.005f, cl_blood_film_full->value));
}

static void film_tile_free(int idx)
{
    film_tile_t *t = &film_tiles[idx];

    if (!t->used)
        return;

    int *link = &film_hash[film_hash_key(t->plane, t->tx, t->ty)];
    while (*link >= 0) {
        if (*link == idx) {
            *link = t->hash_next;
            break;
        }
        link = &film_tiles[*link].hash_next;
    }

    t->used = false;

    // The neighbours drew up to this tile and now have to close their outline
    // against nothing - see the dry samples in film_mesh.
    film_mark_neighbours_dirty(t);

    film_plane_t *p = &film_planes[t->plane];
    if (--p->refs <= 0)
        p->used = false;

    t->num_tris = 0;
}

/*
Classifies one cell, the first time anything asks (film_kind). Two traces, so it
used to be done for a whole new tile at once - 512 traces on the frame blood
first reached it, and a shotgun blast reaches ~45 new tiles in a third of a
second. Most of a tile never sees blood: now only cells blood lands on, flows
into, or comes within the blur's reach of are classified (2026-11-07).
*/
static void film_classify_cell(film_tile_t *t, int i)
{
    const film_plane_t *p = &film_planes[t->plane];
    mnode_t *headnode = cl.bsp->nodes;
    const int gx = t->tx * FILM_TILE + i % FILM_TILE;
    const int gy = t->ty * FILM_TILE + i / FILM_TILE;
    vec3_t c, above, start, end;
    trace_t tr;

    film_stat_classified++;

    film_cell_center(p, gx, gy, c);
    VectorMA(c, 0.3f, p->normal, above);

    t->bump[i] = film_noise(t->plane, gx, gy);

    const int contents = CM_PointContents(above, headnode);

    // Something stands on the surface here: the wall a floor runs into, or, for
    // a wall, the floor below its foot.
    if (contents & MASK_SOLID) {
        t->kind[i] = CELL_BLOCKED;
        return;
    }

    VectorMA(c, 0.6f, p->normal, start);
    VectorMA(c, -0.6f, p->normal, end);
    CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, headnode, MASK_SOLID);

    if (tr.startsolid || tr.allsolid) {
        t->kind[i] = CELL_BLOCKED;
    } else if (tr.fraction < 1.0f && DotProduct(tr.plane.normal, p->normal) > 0.98f &&
               fabsf(DotProduct(tr.endpos, p->normal) - p->dist) < 0.2f) {
        t->kind[i] = (contents & MASK_WATER) ? CELL_LIQUID : CELL_SURFACE;
    } else {
        t->kind[i] = CELL_OPEN;
    }
}

// What a cell is, classifying it if nothing has asked before. Blood only ever
// goes into SURFACE cells, so a cell holding any (h > 0) is already classified.
static inline int film_kind(const film_tile_t *t, int i)
{
    if (t->kind[i] == CELL_UNKNOWN)
        film_classify_cell((film_tile_t *)t, i);
    return t->kind[i];
}

/*
Finds a tile, making it if asked. `may_recycle` lets it take over a tile that has
all but faded out when the pool is full - only ever from a landing, never from
inside a step, which is iterating over the tiles.
*/
static int film_tile_get(int plane, int tx, int ty, bool create, bool may_recycle)
{
    int idx = film_tile_lookup(plane, tx, ty);

    if (idx >= 0 || !create)
        return idx;

    for (int i = 0; i < film_cap; i++) {
        if (!film_tiles[i].used) {
            idx = i;
            break;
        }
    }

    // The last resort: the faintest tile, if it is nearly gone anyway. It used
    // to be the tile furthest into its fade or else the one fed longest ago -
    // either of them often still all there, so in a fight squares of old blood
    // vanished as new blood landed (2026-11-07). Nothing faint enough: refused,
    // which hurries the fade (film_fade), and the landing is a splat meanwhile.
    if (idx < 0 && may_recycle) {
        float faintest = FILM_RECYCLE_ALPHA;
        for (int i = 0; i < film_cap; i++) {
            const film_tile_t *t = &film_tiles[i];
            if (t->used && t->alpha <= faintest) {
                faintest = t->alpha;
                idx = i;
            }
        }
        if (idx >= 0)
            film_tile_free(idx);
    }

    if (idx < 0) {
        film_stat_full++;
        film_starved++;
        return -1;
    }

    const uint64_t t0 = Sys_Microseconds();
    film_tile_t *t = &film_tiles[idx];
    blood_film_vert_t *verts = t->verts;
    const int max_tris = t->max_tris;

    memset(t, 0, sizeof(*t));
    t->verts = verts;
    t->max_tris = max_tris;
    t->used = true;
    t->plane = plane;
    t->tx = tx;
    t->ty = ty;
    t->alpha = 1.f;
    t->seed = frand() * 64.f;
    t->fed_time = cl.time;

    const unsigned key = film_hash_key(plane, tx, ty);
    t->hash_next = film_hash[key];
    film_hash[key] = idx;

    // Recycling may have freed the last tile on this very plane, which marks
    // the plane unused - its contents are intact, so just take it back.
    film_planes[plane].used = true;
    film_planes[plane].refs++;

    // A new tile is where the neighbours' outlines used to close - and it owns
    // border quads they drew until now, so it is meshed even with no blood of
    // its own. It was not (memset above): a tile made only to hold a pool's
    // border drew nothing, and the pool ended in a straight line along the tile
    // edge (2026-11-07, "the effect is cut off").
    t->dirty = true;
    film_mark_neighbours_dirty(t);

    film_stat_new++;
    film_stat_new_us += Sys_Microseconds() - t0;
    return idx;
}

// The tile and cell holding global cell (gx, gy) on `plane`.
static int film_cell_at(int plane, int gx, int gy, bool create, bool may_recycle, int *cell)
{
    const int tx = film_div(gx), ty = film_div(gy);
    const int idx = film_tile_get(plane, tx, ty, create, may_recycle);

    if (idx < 0)
        return -1;

    *cell = (gy - ty * FILM_TILE) * FILM_TILE + (gx - tx * FILM_TILE);
    return idx;
}

static void film_wake(film_tile_t *t)
{
    t->awake = true;
    t->calm = 0;
    t->fed_time = cl.time;

    // A patch fading to make room keeps going, new blood and all - see Fading.
    if (t->alpha != 1.f && !t->fade_from) {
        t->alpha = 1.f;
        t->version = ++film_version_seq;
    }
}

/*
===============
film_add

Puts `volume` of blood on the surface at `point`. Used for landings (immediate:
straight into h) and for blood arriving from another plane during a step (into
dh, applied with the rest of the step).

`away`, when given, is the direction to look for usable surface if the cell
under the point is not - blood arriving at the foot of a wall can land on a cell
the wall stands on, and the right place for it is the first free cell out from
the wall.
===============
*/
static bool film_add(const vec3_t point, const vec3_t normal, float volume, float spread,
                     int color, const color_t *rgba, const vec3_t away, bool immediate)
{
    const float dist = DotProduct(point, normal);
    const int plane = film_find_plane(normal, dist, true);

    if (plane < 0)
        return false;

    film_plane_t *p = &film_planes[plane];
    vec3_t d;
    VectorSubtract(point, p->origin, d);
    const float fs = DotProduct(d, p->u) / film_cell;
    const float ft = DotProduct(d, p->v) / film_cell;
    int gx = (int)floorf(fs), gy = (int)floorf(ft);

    // Find a cell that can hold blood: the one under the point, else walk away
    // from whatever is in the way.
    int cell, ti = film_cell_at(plane, gx, gy, true, immediate, &cell);

    if (ti >= 0 && film_kind(&film_tiles[ti], cell) != CELL_SURFACE && away) {
        const float au = DotProduct(away, p->u), av = DotProduct(away, p->v);
        const int sx = fabsf(au) > 0.35f ? (au > 0 ? 1 : -1) : 0;
        const int sy = fabsf(av) > 0.35f ? (av > 0 ? 1 : -1) : 0;

        for (int step = 1; step <= 3 && (sx || sy); step++) {
            ti = film_cell_at(plane, gx + sx * step, gy + sy * step, true, immediate, &cell);
            if (ti >= 0 && film_kind(&film_tiles[ti], cell) == CELL_SURFACE) {
                gx += sx * step;
                gy += sy * step;
                break;
            }
        }
    }

    if (ti < 0 || film_kind(&film_tiles[ti], cell) != CELL_SURFACE) {
        if (!immediate) {
            film_release_plane_if_empty(plane);
            return false;
        }

        // A landing: try the cells around it before giving up.
        bool found = false;
        for (int r = 1; r <= 2 && !found; r++) {
            for (int oy = -r; oy <= r && !found; oy++) {
                for (int ox = -r; ox <= r && !found; ox++) {
                    ti = film_cell_at(plane, gx + ox, gy + oy, true, true, &cell);
                    if (ti >= 0 && film_kind(&film_tiles[ti], cell) == CELL_SURFACE) {
                        gx += ox;
                        gy += oy;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            film_release_plane_if_empty(plane);
            return false;
        }
    }

    if (!p->has_color && rgba) {
        p->has_color = true;
        p->color = color;
        p->rgba = *rgba;
    }

    // Spread over the droplet's footprint, peaked in the middle, so one droplet
    // makes a round spot rather than a single square cell of blood.
    const float rc = max(0.5f, spread / film_cell);
    const int reach = (int)ceilf(rc);
    float wsum = 0.f;

    // Two passes: weigh, then deposit, so cells that cannot hold blood do not
    // take a share of it.
    for (int pass = 0; pass < 2; pass++) {
        for (int oy = -reach; oy <= reach; oy++) {
            for (int ox = -reach; ox <= reach; ox++) {
                const float dx = (gx + ox + 0.5f) - fs, dy = (gy + oy + 0.5f) - ft;
                float w = 1.f - (dx * dx + dy * dy) / (rc * rc);

                if (ox == 0 && oy == 0)
                    w = max(w, 0.25f);      // the landing cell always takes some
                if (w <= 0.f)
                    continue;

                int c, t2 = film_cell_at(plane, gx + ox, gy + oy, pass == 0 && immediate,
                                         immediate, &c);
                if (t2 < 0 || film_kind(&film_tiles[t2], c) != CELL_SURFACE)
                    continue;

                if (pass == 0) {
                    wsum += w;
                    continue;
                }

                film_tile_t *t = &film_tiles[t2];
                const float add = volume * w / wsum;
                if (immediate) {
                    // Shown on the next step, with every other landing on the
                    // tile by then: one mesh, not one per droplet (a blast lands
                    // dozens a frame) - see WHAT THE MESHES SEE.
                    t->h[c] += add;
                    t->pending = true;
                    t->publish_now = true;
                } else {
                    t->dh[c] += add;
                    t->touched = true;
                }

                film_wake(t);
            }
        }

        if (pass == 0 && wsum <= 0.f) {
            film_release_plane_if_empty(plane);
            return false;
        }
    }

    return true;
}

/*
===============
Leaving the grid
===============
*/

// Over an edge: gathered until there is a droplet's worth, then dropped off it.
static void film_spill_edge(film_tile_t *t, const film_plane_t *p, int gx, int gy,
                            int dx, int dy, float volume)
{
    vec3_t c, dir;
    film_cell_center(p, gx, gy, c);
    VectorScale(p->u, (float)dx, dir);
    VectorMA(dir, (float)dy, p->v, dir);

    VectorMA(c, film_cell * 0.6f, dir, t->spill_edge_org);
    VectorMA(t->spill_edge_org, 0.4f, p->normal, t->spill_edge_org);
    VectorCopy(dir, t->spill_edge_dir);
    t->spill_edge += volume;

    if (t->spill_edge >= film_drop_volume(cl_blood_sphere_radius->value * 0.8f)) {
        vec3_t vel;
        VectorScale(t->spill_edge_dir, 6.f, vel);
        CL_BloodFilmDrip(t->spill_edge_org, vel, film_drop_radius(t->spill_edge), p->color, &p->rgba);
        t->spill_edge = 0.f;
        film_stat_drips++;
    }
}

// Into water: becomes a dissolving cloud once there is a droplet's worth.
static void film_spill_liquid(film_tile_t *t, const film_plane_t *p, int gx, int gy, float volume)
{
    vec3_t c;
    film_cell_center(p, gx, gy, c);
    VectorMA(c, 0.3f, p->normal, t->spill_liquid_org);
    t->spill_liquid += volume;

    if (t->spill_liquid >= film_drop_volume(cl_blood_sphere_radius->value * 0.8f)) {
        static const vec3_t up = { 0.f, 0.f, 1.f };
        CL_BloodFilmDissolve(t->spill_liquid_org, up, film_drop_radius(t->spill_liquid),
                             p->color, &p->rgba);
        t->spill_liquid = 0.f;
    }
}

/*
The foot of a run: the cell downhill is solid, so the blood goes onto whatever it
ran into. Traced from just off the surface in the run's direction; the receiving
surface's own grid takes it from there, starting a little way out from the wall
(film_add's `away`), so a pool at the foot of a wall grows out from it.
*/
static bool film_corner(const film_plane_t *p, int gx, int gy, int dx, int dy, float volume)
{
    vec3_t c, dir, start, end;
    trace_t tr;

    film_cell_center(p, gx, gy, c);
    VectorScale(p->u, (float)dx, dir);
    VectorMA(dir, (float)dy, p->v, dir);
    VectorMA(c, 0.4f, p->normal, start);
    VectorMA(start, film_cell * 1.5f, dir, end);

    CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);

    if (tr.startsolid || tr.allsolid || tr.fraction >= 1.0f)
        return false;
    if (DotProduct(tr.plane.normal, p->normal) > 0.98f || tr.plane.normal[2] < -0.3f)
        return false;

    film_stat_corner++;
    return film_add(tr.endpos, tr.plane.normal, volume, 0.f, p->color, &p->rgba, p->normal, false);
}

/*
===============
film_step

One explicit step of the flow. Each cell above its hold gives blood to each
neighbour whose head is lower, in proportion to the difference and to how mobile
it is - thin blood creeps, thick blood runs - and never more than it has above
the hold, so nothing goes negative and nothing oscillates. All the flow is
gathered into dh and applied at the end, so the order tiles are visited in does
not matter.
===============
*/
enum { TO_NONE, TO_CELL, TO_EDGE, TO_LIQUID, TO_CORNER };

static void film_step(float dt)
{
    const float thick = max(0.01f, cl_blood_film_thick->value);
    const float cling = max(0.f, cl_blood_film_cling->value);
    const float trail = max(0.f, min(cl_blood_film_trail->value, cling));
    const float k = max(0.f, cl_blood_film_flow->value) * 6.f;
    const float run_k = max(0.f, cl_blood_film_run->value);
    const float rough = max(0.f, cl_blood_film_rough->value);
    static const int dirs[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };

    for (int ti = 0; ti < film_cap; ti++) {
        film_tile_t *t = &film_tiles[ti];

        if (!t->used || !t->awake)
            continue;

        const int plane = t->plane;
        const film_plane_t *p = &film_planes[plane];
        const float hold_still = thick * (1.f - p->slope) + cling * p->slope;
        const float hold_run = thick * (1.f - p->slope) + trail * p->slope;

        // The flow on a floor is spreading - diffusion, which an explicit step
        // only keeps stable below a quarter of the depth difference per
        // neighbour. Down a wall it is mostly the slope, with the depth pushing
        // sideways only `press` as hard, so the same stability allows 1 / press
        // as much: a run can move as fast as cl_blood_film_run asks.
        const float k_plane = k * (1.f + (run_k - 1.f) * p->slope);
        const float cap = min(1.f, 0.24f / p->press);

        t->moved = 0.f;
        t->moved_max = 0.f;

        for (int cy = 0; cy < FILM_TILE; cy++) {
            for (int cx = 0; cx < FILM_TILE; cx++) {
                const int i = cy * FILM_TILE + cx;

                // Only SURFACE cells ever hold blood, and only they are looked
                // at - most of a tile is never classified (film_kind).
                const float h = t->h[i];
                if (h <= 0.f)
                    continue;

                // Still blood only starts to move past the cling depth; once it
                // has, it keeps going down to the trail.
                if (!t->run[i] && h > hold_still)
                    t->run[i] = 1;

                const float mobile = h - (t->run[i] ? hold_run : hold_still);

                if (mobile <= 1e-4f)
                    continue;

                const float m = min(mobile / thick, 1.f);
                const float rate = min(cap, k_plane * m * m * dt);

                if (rate <= 0.f)
                    continue;

                const int gx = t->tx * FILM_TILE + cx;
                const int gy = t->ty * FILM_TILE + cy;
                const float elev = gx * p->rise_u + gy * p->rise_v;
                const float head = h * p->press + elev + t->bump[i] * rough;

                float out[4] = { 0.f, 0.f, 0.f, 0.f };
                int to[4] = { TO_NONE, TO_NONE, TO_NONE, TO_NONE };
                int nt[4] = { -1, -1, -1, -1 }, nc[4] = { 0, 0, 0, 0 };
                float total = 0.f;

                for (int d = 0; d < 4; d++) {
                    const int dx = dirs[d][0], dy = dirs[d][1];
                    const float elev_j = (gx + dx) * p->rise_u + (gy + dy) * p->rise_v;

                    // Cannot possibly flow that way (uphill on a wall): do not
                    // make a tile just to find that out.
                    if (head - elev_j + rough <= 0.f)
                        continue;

                    const int ncx = cx + dx, ncy = cy + dy;
                    if (ncx >= 0 && ncx < FILM_TILE && ncy >= 0 && ncy < FILM_TILE) {
                        nt[d] = ti;
                        nc[d] = ncy * FILM_TILE + ncx;
                    } else {
                        nt[d] = film_cell_at(plane, gx + dx, gy + dy, true, false, &nc[d]);
                        if (nt[d] < 0)
                            continue;
                    }

                    const film_tile_t *n = &film_tiles[nt[d]];
                    float drive = 0.f;

                    switch (film_kind(n, nc[d])) {
                    case CELL_SURFACE:
                        drive = head - (n->h[nc[d]] * p->press + elev_j + n->bump[nc[d]] * rough);
                        to[d] = TO_CELL;
                        break;
                    case CELL_OPEN:
                        drive = head - elev_j;
                        to[d] = TO_EDGE;
                        break;
                    case CELL_LIQUID:
                        drive = head - elev_j;
                        to[d] = TO_LIQUID;
                        break;
                    case CELL_BLOCKED:
                        // Only DOWNHILL into something solid is the foot of a run;
                        // level or uphill it is simply a wall in the way.
                        if (elev_j < elev - 0.2f * film_cell) {
                            drive = head - elev_j;
                            to[d] = TO_CORNER;
                        }
                        break;
                    }

                    if (drive > 0.f) {
                        out[d] = rate * drive;
                        total += out[d];
                    } else {
                        to[d] = TO_NONE;
                    }
                }

                if (total <= 0.f)
                    continue;

                if (total > mobile) {
                    const float s = mobile / total;
                    for (int d = 0; d < 4; d++)
                        out[d] *= s;
                    total = mobile;
                }

                for (int d = 0; d < 4; d++) {
                    if (out[d] <= 0.f || to[d] == TO_NONE)
                        continue;

                    const int dx = dirs[d][0], dy = dirs[d][1];

                    switch (to[d]) {
                    case TO_CELL: {
                        film_tile_t *n = &film_tiles[nt[d]];
                        n->dh[nc[d]] += out[d];
                        n->touched = true;
                        // Blood arriving from ABOVE is a run coming through.
                        // From the side it only widens the wet patch.
                        if ((gx + dx) * p->rise_u + (gy + dy) * p->rise_v < elev - 1e-3f)
                            n->run[nc[d]] = 1;
                        break;
                    }
                    case TO_EDGE:
                        film_spill_edge(t, p, gx, gy, dx, dy, out[d]);
                        break;
                    case TO_LIQUID:
                        film_spill_liquid(t, p, gx + dx, gy + dy, out[d]);
                        break;
                    case TO_CORNER:
                        if (!film_corner(p, gx, gy, dx, dy, out[d]))
                            total -= out[d];    // nowhere to put it: it stays
                        break;
                    }
                }

                t->dh[i] -= total;
                t->touched = true;
                t->moved += total;
                t->moved_max = max(t->moved_max, total);
            }
        }
    }

    // Apply. Whose mesh changed is film_publish's business - see WHAT THE
    // MESHES SEE.
    static unsigned publish_count;
    const bool tick = (++publish_count % FILM_PUBLISH_EVERY) == 0;
    const unsigned tick_number = publish_count / FILM_PUBLISH_EVERY;
    const float band = film_band();

    for (int ti = 0; ti < film_cap; ti++) {
        film_tile_t *t = &film_tiles[ti];

        if (!t->used)
            continue;

        if (t->touched) {
            bool arrived = false;

            for (int i = 0; i < FILM_CELLS; i++) {
                if (t->dh[i] == 0.f)
                    continue;

                arrived |= t->dh[i] > FILM_SLEEP;
                t->h[i] = max(0.f, t->h[i] + t->dh[i]);
                t->dh[i] = 0.f;
            }

            t->touched = false;
            t->pending = true;

            // Blood arriving wakes a tile; a trickle too small to keep the
            // sender awake does not.
            if (arrived && !t->awake) {
                t->awake = true;
                t->calm = 0;
            }
        }

        if (t->pending) {
            const float need = film_unpublished(t, band);
            const bool wall = film_planes[t->plane].slope > 0.5f;
            const bool now = t->publish_now || (wall && need >= FILM_PUBLISH_NOW);

            const int age = cl.time - t->fed_time;
            const unsigned period = age < 1000 ? 1 : age < 3000 ? 2 : FILM_PUBLISH_SLOW;

            if (now)
                film_publish(t, band);
            else if (need >= 1.f && tick && (tick_number % period) == 0)
                t->due = true;
            else if (need <= 0.f)
                t->pending = false;
        }

        if (t->awake) {
            if (t->moved_max > FILM_SLEEP)
                t->calm = 0;
            else if (++t->calm >= FILM_CALM_STEPS)
                t->awake = false;
        }
    }

    film_stat_steps++;
}

/*
===============
Meshing

A tile draws the quads whose lower corner is one of its cells, plus the ones
along its low sides whose lower corner is in a tile that does not exist - one
rule (film_quad_mine) both sides of every edge agree on. Samples in a tile that
does not exist, past an edge, or under water are DRY whatever the blur says, so
an outline always closes inside quads somebody draws.

THE COVERAGE FIELD. The depth is lightly blurred (cl_blood_film_smooth), the
outline level taken off it (0.4 of the trail depth, so a wall's trail is drawn
as the thin film it is), and the result mapped to g: -1 dry, 0 the outline, 1 at
cl_blood_film_full and deeper. Contouring the depth itself put every outline
right up against the dry samples - a wet cell is ~15x deeper than the outline
level - so a pool came out as its cells' squares (Matt, 2026-11-07: blocky). On
g a lone wet cell's outline sits halfway to its dry neighbours, and a cell
filling up moves the outline smoothly instead of popping a whole cell on.

DETAIL (cl_blood_film_detail, 1-3). Each cell is cut detail x detail, with g and
the depth sampled between the cells by Catmull-Rom, so an outline is a smooth
curve rather than a polyline on the cell grid. Only quads the outline crosses,
or the thin margin inside it, are cut (EDGE); a quad that is full pool all over
(FULL) stays two triangles, or a fan where it meets an EDGE neighbour so the
fine vertices along that side are shared - no T-junction, no crack.

WATERTIGHT. Every vertex two quads share is computed the same way from the same
inputs by both - a fine sample from its global fine coordinate, an outline
crossing from its two ends taken in a fixed order - so neighbouring quads, in
this tile or the next, agree to the bit and no ray slips between them.

THE DEPTH goes to the renderer per vertex (blood_film_vert_t) as T, what the
landed-splat material darkens and thins the blood by: depth / cl_blood_film_full,
so a trail reads thin, but never more than 2g, so even a deep pool thins to
nothing over the last quarter cell to its outline - the narrow see-through rim a
splat has. The normals are the plane's. A vertex stands FILM_LIFT plus its depth
off the surface, the depth ramped in by T so the outline lies flush.
===============
*/
#define FILM_PAD        4                       // raw samples kept either side of the tile
#define FILM_RAW        (FILM_TILE + 2 * FILM_PAD)
#define FILM_DETAIL_MAX 3

// Quads FILM_Q_LO .. FILM_TILE: the ones this tile may draw (-1 .. FILM_TILE - 1)
// and a ring round them, classified so a drawn quad knows what its neighbours are.
#define FILM_Q_LO       (-2)
#define FILM_Q_N        (FILM_TILE + 3)
#define FILM_FINE_N     (FILM_Q_N * FILM_DETAIL_MAX + 1)

enum { QUAD_EMPTY, QUAD_EDGE, QUAD_FULL };

static const film_tile_t *film_near[3][3];  // the tile and its neighbours, [dy+1][dx+1]

static inline const film_tile_t *film_near_tile(int x, int y)
{
    const int ox = x < 0 ? 0 : (x >= FILM_TILE ? 2 : 1);
    const int oy = y < 0 ? 0 : (y >= FILM_TILE ? 2 : 1);
    return film_near[oy][ox];
}

// Who draws the quad anchored at tile-local (x, y): the first existing tile of
// the four whose cells are its corners.
static bool film_quad_mine(int x, int y)
{
    const int cx[4] = { x, x + 1, x, x + 1 };
    const int cy[4] = { y, y, y + 1, y + 1 };

    for (int k = 0; k < 4; k++) {
        const film_tile_t *t = film_near_tile(cx[k], cy[k]);
        if (t)
            return t == film_near[1][1];
    }
    return false;
}

// Blood within the blur's reach of a side needs the tile on that side to exist,
// or the outline is cut off along the tile edge: two cells, since the blur
// carries a cell's blood one further, into the cell at the edge. Made here,
// never recycled; false if the pool had no room for one (see Fading: starved).
static bool film_ensure_neighbours(const film_tile_t *t)
{
    bool lo_x = false, hi_x = false, lo_y = false, hi_y = false;
    bool ok = true;

    for (int i = 0; i < FILM_CELLS; i++) {
        if (t->h[i] <= 0.f)
            continue;
        const int cx = i % FILM_TILE, cy = i / FILM_TILE;
        lo_x |= cx < 2;
        hi_x |= cx >= FILM_TILE - 2;
        lo_y |= cy < 2;
        hi_y |= cy >= FILM_TILE - 2;
    }

    if (!lo_x && !hi_x && !lo_y && !hi_y)
        return true;

    const int plane = t->plane, tx = t->tx, ty = t->ty;

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if ((!dx && !dy) || (dx < 0 && !lo_x) || (dx > 0 && !hi_x) ||
                (dy < 0 && !lo_y) || (dy > 0 && !hi_y))
                continue;
            if (film_tile_get(plane, tx + dx, ty + dy, true, false) < 0)
                ok = false;
        }
    }

    return ok;
}

typedef struct {
    vec3_t  base;   // on the surface; film_emit lifts it
    float   g;      // coverage: -1 dry, 0 the outline, 1 full
    float   f;      // depth past the outline level
    int     X, Y;   // GLOBAL fine coordinates of a sample (not set on a crossing)
} film_mvert_t;

// Per sample of the tile and its margin, [y + FILM_PAD][x + FILM_PAD].
static float film_gs[FILM_RAW][FILM_RAW];
static float film_fs[FILM_RAW][FILM_RAW];

// Fine samples over the quads FILM_Q_LO .., [FY - FILM_Q_LO * detail][FX - ...].
static film_mvert_t film_fine[FILM_FINE_N][FILM_FINE_N];
static uint8_t      film_quad[FILM_Q_N][FILM_Q_N];

// Catmull-Rom weights at i / detail, [detail - 1][i][4]. At i = 0 they are
// exactly (0, 1, 0, 0), so a fine sample on a cell centre IS the cell's value.
static float film_cr[FILM_DETAIL_MAX][FILM_DETAIL_MAX][4];

static int   film_dropped;      // triangles film_emit had no room for, this mesh
static float film_level;        // the outline level, for this mesh

// Whether any of the 4x4 cells a fine sample is interpolated from holds blood,
// by the support's top-left raw sample. Most of a spatter tile is dry, and a
// sample over nothing but dry cells is dry - -1 and -level, set rather than
// summed. Both tiles along a side decide this from the same cells, so they
// still agree to the bit (WATERTIGHT).
static bool  film_support[FILM_RAW][FILM_RAW];
static float inv_full;          // 1 / cl_blood_film_full, for this mesh

static void film_cr_init(void)
{
    for (int s = 1; s <= FILM_DETAIL_MAX; s++) {
        for (int i = 0; i < s; i++) {
            const float t = (float)i / (float)s, t2 = t * t, t3 = t2 * t;
            float *w = film_cr[s - 1][i];
            w[0] = 0.5f * (-t + 2.f * t2 - t3);
            w[1] = 0.5f * (2.f - 5.f * t2 + 3.f * t3);
            w[2] = 0.5f * (t + 4.f * t2 - 3.f * t3);
            w[3] = 0.5f * (-t2 + t3);
        }
    }
}

static inline int film_floor_div(int a, int b)
{
    return a >= 0 ? a / b : -((-a - 1) / b) - 1;
}

// The fine sample at tile-local fine coordinate (FX, FY). Separable, rows
// first, the same instructions for every tile: see WATERTIGHT above.
static void film_fine_sample(film_mvert_t *m, const film_tile_t *t, const film_plane_t *p,
                             int detail, int FX, int FY)
{
    const int x0 = film_floor_div(FX, detail), y0 = film_floor_div(FY, detail);
    const float *wx = film_cr[detail - 1][FX - x0 * detail];
    const float *wy = film_cr[detail - 1][FY - y0 * detail];
    const int rx = x0 - 1 + FILM_PAD;
    float g = -1.f, f = -film_level;

    if (film_support[y0 - 1 + FILM_PAD][rx]) {
        g = f = 0.f;
        for (int b = 0; b < 4; b++) {
            const int ry = y0 - 1 + b + FILM_PAD;
            const float *gr = &film_gs[ry][rx], *fr = &film_fs[ry][rx];
            const float rg = wx[0] * gr[0] + wx[1] * gr[1] + wx[2] * gr[2] + wx[3] * gr[3];
            const float rf = wx[0] * fr[0] + wx[1] * fr[1] + wx[2] * fr[2] + wx[3] * fr[3];
            g += wy[b] * rg;
            f += wy[b] * rf;
        }
    }

    m->g = g;
    m->f = f;
    m->X = t->tx * FILM_TILE * detail + FX;
    m->Y = t->ty * FILM_TILE * detail + FY;

    // From the global integers, so the tile on the other side of an edge puts
    // the same vertex in the same place.
    const float sx = ((float)m->X / (float)detail + 0.5f) * film_cell;
    const float sy = ((float)m->Y / (float)detail + 0.5f) * film_cell;
    VectorMA(p->origin, sx, p->u, m->base);
    VectorMA(m->base, sy, p->v, m->base);
}

static inline const film_mvert_t *film_fine_at(int detail, int FX, int FY)
{
    return &film_fine[FY - FILM_Q_LO * detail][FX - FILM_Q_LO * detail];
}

static inline int film_quad_at(int x, int y)
{
    if (x < FILM_Q_LO || y < FILM_Q_LO || x >= FILM_Q_LO + FILM_Q_N || y >= FILM_Q_LO + FILM_Q_N)
        return QUAD_EMPTY;
    return film_quad[y - FILM_Q_LO][x - FILM_Q_LO];
}

static bool film_emit(film_tile_t *t, const film_plane_t *p, const film_mvert_t *a,
                      const film_mvert_t *b, const film_mvert_t *c, float height_scale)
{
    if (t->num_tris >= BLOOD_FILM_TILE_TRIS) {
        film_dropped++;
        return false;
    }

    if (t->num_tris >= t->max_tris) {
        const int grow = min(BLOOD_FILM_TILE_TRIS, max(256, t->max_tris * 2));
        t->verts = Z_Realloc(t->verts, sizeof(blood_film_vert_t) * 3 * grow);
        t->max_tris = grow;
    }

    blood_film_vert_t *out = t->verts + t->num_tris * 3;
    const film_mvert_t *v[3] = { a, b, c };

    for (int k = 0; k < 3; k++) {
        const float T = max(0.f, min(min(v[k]->f * inv_full, 2.f * v[k]->g), 1.f));
        const float lift = FILM_LIFT + max(0.f, v[k]->f) * height_scale * T;
        VectorMA(v[k]->base, lift, p->normal, out[k].pos);
        out[k].depth = T;
    }

    t->num_tris++;
    return true;
}

// Where the outline crosses the fine edge s-e. Ordered first, so the quad on the
// other side of the edge - which walks it the other way - gets the same point.
static void film_cross(film_mvert_t *m, const film_mvert_t *s, const film_mvert_t *e)
{
    if (e->Y < s->Y || (e->Y == s->Y && e->X < s->X)) {
        const film_mvert_t *swap = s;
        s = e;
        e = swap;
    }

    const float u = s->g / (s->g - e->g);
    for (int j = 0; j < 3; j++)
        m->base[j] = s->base[j] + (e->base[j] - s->base[j]) * u;
    m->g = 0.f;
    m->f = 0.f;
    m->X = m->Y = 0;
}

static void film_poly_add(film_mvert_t *poly, int *n, const film_mvert_t *m)
{
    if (*n && !memcmp(m->base, poly[*n - 1].base, sizeof(vec3_t))) {
        if (m->g > poly[*n - 1].g)
            poly[*n - 1] = *m;
        return;
    }
    poly[(*n)++] = *m;
}

// One triangle, clipped to where there is blood (g > 0). g is linear over it, so
// the clip is exact for the interpolated coverage.
static void film_clip_tri(film_tile_t *t, const film_plane_t *p, const film_mvert_t *a,
                          const film_mvert_t *b, const film_mvert_t *c, float height_scale)
{
    const film_mvert_t *in[3] = { a, b, c };
    film_mvert_t poly[4];
    int n = 0;

    if (a->g <= 0.f && b->g <= 0.f && c->g <= 0.f)
        return;

    if (a->g > 0.f && b->g > 0.f && c->g > 0.f) {
        film_emit(t, p, a, b, c, height_scale);
        return;
    }

    // A crossing within rounding of a corner comes out AS that corner (or as the
    // crossing next to it): kept twice, the triangle fanned off it has no area
    // and an edge used three times. So equal neighbours merge, the corner
    // winning over the crossing - every quad that has the point has that corner,
    // so they all lift it the same.
    for (int k = 0; k < 3; k++) {
        const film_mvert_t *s = in[k], *e = in[(k + 1) % 3];

        if (s->g > 0.f)
            film_poly_add(poly, &n, s);

        if ((s->g > 0.f) != (e->g > 0.f)) {
            film_mvert_t c;
            film_cross(&c, s, e);
            film_poly_add(poly, &n, &c);
        }
    }

    if (n > 1 && !memcmp(poly[n - 1].base, poly[0].base, sizeof(vec3_t))) {
        if (poly[n - 1].g > poly[0].g)
            poly[0] = poly[n - 1];
        n--;
    }

    for (int k = 1; k + 1 < n; k++)
        film_emit(t, p, &poly[0], &poly[k], &poly[k + 1], height_scale);
}

// A quad the outline (or the thin margin inside it) crosses: detail x detail
// small quads, each split along its wetter diagonal so a thin strip stays
// connected, and clipped.
static void film_edge_quad(film_tile_t *t, const film_plane_t *p, int detail, int x, int y,
                           float height_scale)
{
    for (int j = 0; j < detail; j++) {
        for (int i = 0; i < detail; i++) {
            const int FX = x * detail + i, FY = y * detail + j;
            const film_mvert_t *a = film_fine_at(detail, FX, FY);
            const film_mvert_t *b = film_fine_at(detail, FX + 1, FY);
            const film_mvert_t *c = film_fine_at(detail, FX + 1, FY + 1);
            const film_mvert_t *d = film_fine_at(detail, FX, FY + 1);

            if (a->g <= 0.f && b->g <= 0.f && c->g <= 0.f && d->g <= 0.f)
                continue;

            if (a->g + c->g >= b->g + d->g) {
                film_clip_tri(t, p, a, b, c, height_scale);
                film_clip_tri(t, p, a, c, d, height_scale);
            } else {
                film_clip_tri(t, p, a, b, d, height_scale);
                film_clip_tri(t, p, b, c, d, height_scale);
            }
        }
    }
}

// A quad of full pool. Two triangles - unless a neighbour across a side is cut
// finer, whose fine vertices along that side this quad must share: then a fan
// round its centre takes them in.
static void film_full_quad(film_tile_t *t, const film_plane_t *p, int detail, int x, int y,
                           float height_scale)
{
    const int X0 = x * detail, Y0 = y * detail, X1 = X0 + detail, Y1 = Y0 + detail;
    const film_mvert_t *A = film_fine_at(detail, X0, Y0);
    const film_mvert_t *B = film_fine_at(detail, X1, Y0);
    const film_mvert_t *C = film_fine_at(detail, X1, Y1);
    const film_mvert_t *D = film_fine_at(detail, X0, Y1);

    const bool below = detail > 1 && film_quad_at(x, y - 1) == QUAD_EDGE;
    const bool right = detail > 1 && film_quad_at(x + 1, y) == QUAD_EDGE;
    const bool above = detail > 1 && film_quad_at(x, y + 1) == QUAD_EDGE;
    const bool left  = detail > 1 && film_quad_at(x - 1, y) == QUAD_EDGE;

    if (!below && !right && !above && !left) {
        if (A->f + C->f >= B->f + D->f) {
            film_emit(t, p, A, B, C, height_scale);
            film_emit(t, p, A, C, D, height_scale);
        } else {
            film_emit(t, p, A, B, D, height_scale);
            film_emit(t, p, B, C, D, height_scale);
        }
        return;
    }

    // The rim, counter-clockwise in (u, v) like every other triangle here.
    const film_mvert_t *rim[4 * FILM_DETAIL_MAX];
    int n = 0;

    rim[n++] = A;
    if (below)
        for (int i = 1; i < detail; i++)
            rim[n++] = film_fine_at(detail, X0 + i, Y0);
    rim[n++] = B;
    if (right)
        for (int j = 1; j < detail; j++)
            rim[n++] = film_fine_at(detail, X1, Y0 + j);
    rim[n++] = C;
    if (above)
        for (int i = 1; i < detail; i++)
            rim[n++] = film_fine_at(detail, X1 - i, Y1);
    rim[n++] = D;
    if (left)
        for (int j = 1; j < detail; j++)
            rim[n++] = film_fine_at(detail, X0, Y1 - j);

    film_mvert_t centre;
    for (int k = 0; k < 3; k++)
        centre.base[k] = 0.25f * (A->base[k] + B->base[k] + C->base[k] + D->base[k]);
    centre.g = 1.f;
    centre.f = 0.25f * (A->f + B->f + C->f + D->f);
    centre.X = centre.Y = 0;

    for (int k = 0; k < n; k++)
        film_emit(t, p, &centre, rim[k], rim[(k + 1) % n], height_scale);
}

static void film_mesh(film_tile_t *t)
{
    const film_plane_t *p = &film_planes[t->plane];
    const float trail = max(0.f, cl_blood_film_trail->value);
    const float level = max(0.005f, trail * 0.4f);
    const float height_scale = max(0.f, cl_blood_film_height->value);
    const float full = max(0.005f, cl_blood_film_full->value);
    const bool smooth = cl_blood_film_smooth->integer != 0;
    int detail = max(1, min(cl_blood_film_detail->integer, FILM_DETAIL_MAX));

    inv_full = 1.f / full;
    t->starved = !film_ensure_neighbours(t);

    // The tile and its eight neighbours.
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const int n = film_tile_lookup(t->plane, t->tx + dx, t->ty + dy);
            film_near[dy + 1][dx + 1] = n >= 0 ? &film_tiles[n] : NULL;
        }
    }

    // Raw depth over the tile and FILM_PAD cells either side, and which samples
    // must be dry whatever their neighbours hold.
    static float raw[FILM_RAW][FILM_RAW];
    static bool  dry[FILM_RAW][FILM_RAW];
    // Only SURFACE cells hold blood, so the depth needs no classification.
    for (int y = -FILM_PAD; y < FILM_TILE + FILM_PAD; y++) {
        for (int x = -FILM_PAD; x < FILM_TILE + FILM_PAD; x++) {
            const film_tile_t *n = film_near_tile(x, y);
            const int i = ((y + FILM_TILE) % FILM_TILE) * FILM_TILE + (x + FILM_TILE) % FILM_TILE;
            raw[y + FILM_PAD][x + FILM_PAD] = n ? n->h_mesh[i] : 0.f;
        }
    }

    // Dry matters only for a sample holding no blood that the blur gives some
    // to - anywhere else it comes out -1 either way - so only those are
    // classified. Inside a wall or under the floor it does not matter what is
    // drawn; past an edge or under water it does.
    for (int y = -FILM_PAD; y < FILM_TILE + FILM_PAD; y++) {
        for (int x = -FILM_PAD; x < FILM_TILE + FILM_PAD; x++) {
            const int ry = y + FILM_PAD, rx = x + FILM_PAD;
            const film_tile_t *n = film_near_tile(x, y);
            bool d = n == NULL;

            if (n && raw[ry][rx] <= 0.f && smooth) {
                bool near = false;
                for (int oy = -1; oy <= 1 && !near; oy++)
                    for (int ox = -1; ox <= 1 && !near; ox++) {
                        const int yy = ry + oy, xx = rx + ox;
                        near = yy >= 0 && yy < FILM_RAW && xx >= 0 && xx < FILM_RAW && raw[yy][xx] > 0.f;
                    }
                if (near) {
                    const int i = ((y + FILM_TILE) % FILM_TILE) * FILM_TILE + (x + FILM_TILE) % FILM_TILE;
                    const int k = film_kind(n, i);
                    d = k == CELL_OPEN || k == CELL_LIQUID;
                }
            }
            dry[ry][rx] = d;
        }
    }

    // Blurred (or not), minus the outline level, and as coverage. Kept over local
    // [-3, FILM_TILE + 3): the fine samples of quads -2 .. FILM_TILE reach that far.
    // The outermost ring is only ever read at weight 0; dry, so it is defined.
    for (int k = 0; k < FILM_RAW; k++) {
        film_fs[0][k] = film_fs[FILM_RAW - 1][k] = film_fs[k][0] = film_fs[k][FILM_RAW - 1] = -level;
        film_gs[0][k] = film_gs[FILM_RAW - 1][k] = film_gs[k][0] = film_gs[k][FILM_RAW - 1] = -1.f;
    }
    for (int y = 1; y < FILM_RAW - 1; y++) {
        for (int x = 1; x < FILM_RAW - 1; x++) {
            float v = raw[y][x];
            if (smooth) {
                v = (4.f * raw[y][x]
                     + 2.f * (raw[y][x - 1] + raw[y][x + 1] + raw[y - 1][x] + raw[y + 1][x])
                     + raw[y - 1][x - 1] + raw[y - 1][x + 1] + raw[y + 1][x - 1] + raw[y + 1][x + 1])
                    * (1.f / 16.f);
            }

            const float f = dry[y][x] ? -level : v - level;
            film_fs[y][x] = f;
            film_gs[y][x] = dry[y][x] ? -1.f
                          : (f >= 0.f ? min(f / full, 1.f) : max(f / level, -1.f));
        }
    }

    {
        static bool row_any[FILM_RAW][FILM_RAW];
        for (int y = 0; y < FILM_RAW; y++)
            for (int x = 0; x + 3 < FILM_RAW; x++)
                row_any[y][x] = film_gs[y][x] > -1.f || film_gs[y][x + 1] > -1.f ||
                                film_gs[y][x + 2] > -1.f || film_gs[y][x + 3] > -1.f;
        memset(film_support, 0, sizeof(film_support));
        for (int y = 0; y + 3 < FILM_RAW; y++)
            for (int x = 0; x + 3 < FILM_RAW; x++)
                film_support[y][x] = row_any[y][x] || row_any[y + 1][x] ||
                                     row_any[y + 2][x] || row_any[y + 3][x];
    }

    // A tile with more outline than BLOOD_FILM_TILE_TRIS holds at this detail is
    // done again coarser: truncated, it would be missing a piece. Its border then
    // has hairline seams against neighbours at the finer detail - the lesser
    // evil, and only ever seen at cl_blood_film_detail 3.
remesh:;
    // Fine samples, then what each quad is.
    const int fine_lo = FILM_Q_LO * detail, fine_n = FILM_Q_N * detail + 1;
    film_level = level;
    for (int j = 0; j < fine_n; j++)
        for (int i = 0; i < fine_n; i++)
            film_fine_sample(&film_fine[j][i], t, p, detail, fine_lo + i, fine_lo + j);

    for (int qy = 0; qy < FILM_Q_N; qy++) {
        for (int qx = 0; qx < FILM_Q_N; qx++) {
            float lo = 1e9f, hi = -1e9f;
            for (int j = 0; j <= detail; j++) {
                for (int i = 0; i <= detail; i++) {
                    const float g = film_fine[qy * detail + j][qx * detail + i].g;
                    lo = min(lo, g);
                    hi = max(hi, g);
                }
            }
            // Not lo >= 1: the weights sum to 1 only to rounding, and a quad of
            // pool all over would come out 0.99999994 and be cut up for nothing.
            film_quad[qy][qx] = hi <= 0.f ? QUAD_EMPTY : (lo >= 0.999f ? QUAD_FULL : QUAD_EDGE);
        }
    }

    t->num_tris = 0;
    film_dropped = 0;

    for (int y = -1; y < FILM_TILE; y++) {
        for (int x = -1; x < FILM_TILE; x++) {
            const int kind = film_quad_at(x, y);

            if (kind == QUAD_EMPTY)
                continue;
            if ((x < 0 || y < 0) && !film_quad_mine(x, y))
                continue;

            if (kind == QUAD_FULL)
                film_full_quad(t, p, detail, x, y, height_scale);
            else
                film_edge_quad(t, p, detail, x, y, height_scale);
        }
    }

    if (film_dropped && detail > 1) {
        detail--;
        film_stat_coarser++;
        goto remesh;
    }

    film_stat_dropped += film_dropped;

    t->version = ++film_version_seq;
    t->dirty = false;
    film_stat_meshes++;
}

/*
===============
Fading

Blood goes as whole PATCHES - the tiles joined side or corner on one plane -
never a tile at a time: one tile going leaves its neighbours' outline closing
along a straight tile edge, a square bite out of a pool (2026-11-07, "the effect
is cut off").

  - Not permanent (g_no_janitor 0): a patch fades once blood has stopped
    reaching ALL of it for cl_blood_splat_life seconds, as a splat would.
  - The pool running low, permanent or not: the patch fed longest ago starts
    fading while a reserve of tiles is still free, so a landing does not have
    to recycle a tile out of the middle of a pool. A tile that joins a fading
    patch goes with it.
  - A patch bigger than a quarter of the pool - a whole fight's floor, joined
    up - would take all of that blood with it, so only its oldest quarter goes
    (fade_part), and that does not spread to the rest. The one place a fade can
    still show a tile edge.
  - STARVED: a tile was asked for with the pool full - a burst outran the fade,
    and some outline is closing along a tile edge for want of its neighbour.
    Whatever is fading goes FILM_HURRY times as fast while it lasts, and the
    tiles that went without are meshed again as soon as anything is freed. NOT
    the most faded patch at once: one picked a moment before is still all there,
    and that was blood vanishing mid-fight (2026-11-07).
  - The renderer's triangles (cl_blood_film_tris) run low the same way as the
    tiles do, with a fifth of them in reserve: one shotgun blast's spatter is
    about a tenth, and a burst that finds them full has to wait to be drawn.
===============
*/
static int film_group_of[MAX_BLOOD_FILM_TILES];
static int film_group_fed[MAX_BLOOD_FILM_TILES];    // a patch's most recent fed_time
static int film_group_fade[MAX_BLOOD_FILM_TILES];   // when it was picked to make room (0: not)
static int film_group_size[MAX_BLOOD_FILM_TILES];

static int film_label_groups(void)
{
    static int queue[MAX_BLOOD_FILM_TILES];
    int groups = 0;

    for (int i = 0; i < film_cap; i++)
        film_group_of[i] = -1;

    for (int i = 0; i < film_cap; i++) {
        if (!film_tiles[i].used || film_group_of[i] >= 0)
            continue;

        const int g = groups++;
        int head = 0, tail = 0;

        film_group_fed[g] = INT_MIN;
        film_group_fade[g] = 0;
        film_group_size[g] = 0;
        film_group_of[i] = g;
        queue[tail++] = i;

        while (head < tail) {
            const film_tile_t *t = &film_tiles[queue[head++]];

            film_group_size[g]++;
            film_group_fed[g] = max(film_group_fed[g], t->fed_time);
            if (t->fade_from && !t->fade_part &&
                (!film_group_fade[g] || t->fade_from < film_group_fade[g]))
                film_group_fade[g] = t->fade_from;

            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int n = film_tile_lookup(t->plane, t->tx + dx, t->ty + dy);
                    if (n >= 0 && n < film_cap && film_group_of[n] < 0) {
                        film_group_of[n] = g;
                        queue[tail++] = n;
                    }
                }
            }
        }
    }

    return groups;
}

// The patch fed longest ago starts to fade - or, if it is too big to go whole,
// its oldest quarter. Returns how many tiles that is.
static int film_pick_fade(int groups, int now)
{
    int pick = -1;

    for (int g = 0; g < groups; g++) {
        if (film_group_fade[g])
            continue;
        if (pick < 0 || film_group_fed[g] < film_group_fed[pick])
            pick = g;
    }
    if (pick < 0)
        return 0;

    const int start = now ? now : 1;

    if (film_group_size[pick] <= max(max(4, film_cap / 8), film_cap / 4)) {
        film_group_fade[pick] = start;
        for (int i = 0; i < film_cap; i++) {
            if (film_tiles[i].used && film_group_of[i] == pick) {
                film_tiles[i].fade_from = start;
                film_tiles[i].fade_part = false;
            }
        }
        film_stat_faded += film_group_size[pick];
        return film_group_size[pick];
    }

    int oldest = INT_MAX;
    for (int i = 0; i < film_cap; i++)
        if (film_tiles[i].used && film_group_of[i] == pick && !film_tiles[i].fade_from)
            oldest = min(oldest, film_tiles[i].fed_time);
    if (oldest == INT_MAX)
        return 0;

    const int cutoff = oldest + (film_group_fed[pick] - oldest) / 4;
    int n = 0;
    for (int i = 0; i < film_cap; i++) {
        film_tile_t *t = &film_tiles[i];
        if (t->used && film_group_of[i] == pick && !t->fade_from && t->fed_time <= cutoff) {
            t->fade_from = start;
            t->fade_part = true;
            n++;
        }
    }
    film_stat_faded += n;
    return n;
}

static void film_fade(int now, int elapsed)
{
    const bool permanent = film_permanent->integer != 0;
    const int reserve = max(4, film_cap / 8);
    const int chunk_reserve = max(8, film_chunk_cap / 5);   // a burst is ~100 chunks
    const bool starved = film_starved > 0;
    int used = 0, fading = 0, freed = 0;

    film_starved = 0;

    for (int i = 0; i < film_cap; i++) {
        if (!film_tiles[i].used)
            continue;
        used++;
        fading += film_tiles[i].fade_from != 0;
    }

    const bool low = used > film_cap - reserve || film_chunks_used > film_chunk_cap - chunk_reserve;

    if (permanent && !low && !fading && !starved)
        return;

    int groups = film_label_groups();

    // Running low, and what is already going will not free enough: the patch
    // fed longest ago goes too.
    if ((low || starved) && fading < reserve)
        fading += film_pick_fade(groups, now);

    // Starved: what is fading goes FILM_HURRY times as fast - its start moved
    // back by the extra time, so every tile of a patch stays in step.
    if (starved && elapsed > 0) {
        const int hurry = elapsed * (FILM_HURRY - 1);
        bool any = false;

        for (int i = 0; i < film_cap; i++) {
            film_tile_t *t = &film_tiles[i];
            if (t->used && t->fade_from) {
                t->fade_from = max(1, t->fade_from - hurry);
                any = true;
            }
        }
        if (any)
            groups = film_label_groups();
    }

    const int life = (int)(max(0.1f, film_splat_life->value) * 1000.f);

    for (int i = 0; i < film_cap; i++) {
        film_tile_t *t = &film_tiles[i];
        if (!t->used)
            continue;

        const int g = film_group_of[i];
        int start = film_group_fade[g];

        // A tile that joined a fading patch goes with it; an old part of a big
        // patch goes on its own.
        if (start) {
            t->fade_from = start;
            t->fade_part = false;
        } else if (t->fade_part) {
            start = t->fade_from;
        } else {
            t->fade_from = 0;
        }

        if (!permanent) {
            const int by_life = film_group_fed[g] + life;
            if (!start || by_life < start)
                start = by_life;
        }

        float a = 1.f;
        if (start && now > start)
            a = 1.f - (now - start) * 0.001f / FILM_FADE_SEC;

        if (a <= 0.f) {
            film_tile_free(i);
            freed++;
            continue;
        }

        // Sixteenths, so the renderer rebuilds a fading tile sixteen times
        // rather than every frame.
        a = ceilf(a * 16.f) / 16.f;
        if (a != t->alpha) {
            t->alpha = a;
            t->version = ++film_version_seq;
        }
    }

    // Room again: the tiles that went without a neighbour try again.
    if (freed) {
        for (int i = 0; i < film_cap; i++)
            if (film_tiles[i].used && film_tiles[i].starved)
                film_tiles[i].dirty = true;
    }
}

/*
===============
Public
===============
*/
void CL_BloodFilmClear(void)
{
    for (int i = 0; i < MAX_BLOOD_FILM_TILES; i++) {
        film_tile_t *t = &film_tiles[i];
        blood_film_vert_t *verts = t->verts;
        const int max_tris = t->max_tris;

        memset(t, 0, sizeof(*t));
        t->verts = verts;
        t->max_tris = max_tris;
    }

    memset(film_planes, 0, sizeof(film_planes));
    for (int i = 0; i < FILM_HASH_SIZE; i++)
        film_hash[i] = -1;

    film_num_out = 0;
    film_accum = 0.f;
    film_last_time = cl.time;
    film_ready = false;
}

void CL_BloodFilmInit(void)
{
    // Off by default while it is being tried out. Needs cl_blood_spheres - the
    // droplets that land are the blood that goes into it - and a path tracer.
    cl_blood_film = Cvar_Get("cl_blood_film", "0", CVAR_ARCHIVE);

    // How many tiles of 16x16 cells can hold blood at once, and how many
    // triangles all of them together may draw - the renderer's share of the
    // blood section, which it sizes from the same cvar. As either runs low the
    // patch blood last reached longest ago fades out. 192 tiles, each its own
    // 2048 triangles in the renderer, filled up in four shotgun blasts
    // (2026-11-07).
    cl_blood_film_tiles = Cvar_Get("cl_blood_film_tiles", "512", CVAR_ARCHIVE);
    cl_blood_film_tris = Cvar_Get("cl_blood_film_tris", BLOOD_FILM_TRIS_DEFAULT, CVAR_ARCHIVE);

    // Cell size in world units. Smaller is smoother and costs the square of it.
    cl_blood_film_cell = Cvar_Get("cl_blood_film_cell", "0.75", CVAR_ARCHIVE);

    // How deep a puddle settles on flat ground - so, with the volume, how far a
    // given amount of blood spreads. A droplet lands about twice this deep.
    cl_blood_film_thick = Cvar_Get("cl_blood_film_thick", "0.3", CVAR_ARCHIVE);

    // Walls: how much blood a cell holds before it starts to RUN. A lone droplet
    // peaks at about 0.34 (twice the thick depth, over a cell's area), so it
    // sticks where it lands; two or three landing together run. It was 0.8, which
    // only a dozen droplets on one spot ever reached - nothing ran (2026-11-07,
    // measured in a headless run of this file).
    cl_blood_film_cling = Cvar_Get("cl_blood_film_cling", "0.45", CVAR_ARCHIVE);

    // Walls: what a run leaves behind it - the trail.
    cl_blood_film_trail = Cvar_Get("cl_blood_film_trail", "0.05", CVAR_ARCHIVE);

    // Blood per droplet, as a multiple of the splat it would have made.
    cl_blood_film_volume = Cvar_Get("cl_blood_film_volume", "1.0", CVAR_ARCHIVE);

    // How fast blood moves.
    cl_blood_film_flow = Cvar_Get("cl_blood_film_flow", "1.0", CVAR_ARCHIVE);

    // Walls: how much faster a run goes than blood spreading on a floor. Thin
    // blood creeps (the square of its depth), so without this a run that had
    // started looked stuck (Matt, 2026-11-07: "it sticks there"). At 2 a spray
    // of two dozen droplets runs ~16 units in its first second and slows to a
    // stop ~40 units down; at 3 and up it dashes and stops dead.
    cl_blood_film_run = Cvar_Get("cl_blood_film_run", "2", CVAR_ARCHIVE);

    // Simulation steps per second.
    cl_blood_film_rate = Cvar_Get("cl_blood_film_rate", "30", CVAR_ARCHIVE);

    // Drawn depth, as a multiple of the simulated one.
    cl_blood_film_height = Cvar_Get("cl_blood_film_height", "1.0", CVAR_ARCHIVE);

    // Depth at which blood reads as a full pool rather than a thin film -
    // thinner is darker and more see-through, as a splat's rim is
    // (pt_blood_thin_dark, pt_blood_edge_alpha, pt_blood_thin_power).
    cl_blood_film_full = Cvar_Get("cl_blood_film_full", "0.06", CVAR_ARCHIVE);

    // Unevenness of the surface in units of depth: makes runs wander and
    // outlines irregular. 0 is a perfectly flat world.
    cl_blood_film_rough = Cvar_Get("cl_blood_film_rough", "0.04", CVAR_ARCHIVE);

    cl_blood_film_smooth = Cvar_Get("cl_blood_film_smooth", "1", CVAR_ARCHIVE);

    // Milliseconds a frame may spend re-meshing blood that is only creeping -
    // see BUDGET. Blood landing, new tiles and runs are never held back.
    cl_blood_film_mesh_ms = Cvar_Get("cl_blood_film_mesh_ms", "1", 0);

    // How finely an outline is drawn: each cell is cut this many times each way
    // where an outline crosses it, and the outline follows a smooth curve
    // between the cells. 1 is the cell grid, 3 the finest.
    cl_blood_film_detail = Cvar_Get("cl_blood_film_detail", "2", CVAR_ARCHIVE);
    film_cr_init();
    cl_blood_film_stats = Cvar_Get("cl_blood_film_stats", "0", 0);

    film_splat_size = Cvar_Get("cl_blood_splat_size", "1.5", CVAR_ARCHIVE);
    film_permanent = Cvar_Get("g_no_janitor", "1", CVAR_ARCHIVE);
    film_splat_life = Cvar_Get("cl_blood_splat_life", "8", CVAR_ARCHIVE);

    CL_BloodFilmClear();
}

static bool film_active(void)
{
    return cl_blood_film && cl_blood_film->integer && cl_blood_spheres->integer &&
           CL_PathTraced() && cl.bsp && cl.bsp->nodes;
}

// The grid is built for one map, one cell size and one pool size; a change of
// any of them starts it again.
static void film_check_world(void)
{
    const int cap = max(1, min(cl_blood_film_tiles->integer, MAX_BLOOD_FILM_TILES));
    const float cell = max(0.25f, min(cl_blood_film_cell->value, 4.f));

    if (!film_ready || film_bsp != cl.bsp || film_cap != cap || film_cell != cell) {
        CL_BloodFilmClear();
        film_bsp = cl.bsp;
        film_cap = cap;
        film_cell = cell;
        film_ready = true;
    }
}

/*
===============
CL_BloodFilmDeposit

A droplet landed on the world. True means its blood is in the film and the
droplet should be retired; false leaves it to the splat path (film off, a
ceiling, or no surface the grid can use under it).
===============
*/
bool CL_BloodFilmDeposit(const vec3_t point, const vec3_t normal, float radius,
                         int color, const color_t *rgba)
{
    if (!film_active())
        return false;

    // Blood on a ceiling drips; the splat path already does that.
    if (normal[2] < -0.3f)
        return false;

    film_check_world();

    const float spread = radius * max(0.1f, film_splat_size->value);
    return film_add(point, normal, film_drop_volume(radius), spread, color, rgba, NULL, true);
}

bool CL_BloodFilmOn(void)
{
    return film_active();
}

// Meshing a tile can make its neighbours (film_ensure_neighbours), which then
// own border quads it stopped drawing: they are meshed this frame too, or the
// border is missing for a frame. Empty tiles make no more, so the second pass is
// the last that finds anything.
static void film_mesh_dirty(void)
{
    for (int pass = 0; pass < 3; pass++) {
        bool any = false;
        for (int i = 0; i < film_cap; i++) {
            if (film_tiles[i].used && film_tiles[i].dirty) {
                film_mesh(&film_tiles[i]);
                any = true;
            }
        }
        if (!any)
            break;
    }
}

void CL_BloodFilmFrame(void)
{
    film_num_out = 0;

    if (!film_active()) {
        if (film_ready)
            CL_BloodFilmClear();
        return;
    }

    film_check_world();

    {
        int tris = cl_blood_film_tris->integer;
        tris = max(BLOOD_FILM_CHUNK_TRIS, min(tris, MAX_BLOOD_FILM_CHUNKS * BLOOD_FILM_CHUNK_TRIS));
        film_chunk_cap = BLOOD_FILM_CHUNKS(tris);
    }

    // Fixed steps on the game clock, so a pause stops the blood and a slow frame
    // does not make it flow faster. At most three a frame; a hitch drops time
    // rather than catching up.
    const int now = cl.time;
    const int elapsed = max(0, min(now - film_last_time, 100));
    film_last_time = now;
    film_accum += elapsed * 0.001f;

    const float step = 1.f / max(5.f, min(cl_blood_film_rate->value, 120.f));
    int steps = 0;
    const uint64_t t0 = Sys_Microseconds();

    while (film_accum >= step && steps < 3) {
        film_step(step);
        film_accum -= step;
        steps++;
    }
    if (steps == 3)
        film_accum = 0.f;

    const uint64_t t1 = Sys_Microseconds();

    film_fade(now, elapsed);

    // A change to how the film is drawn redraws all of it.
    {
        static float drawn[6];
        const float now_drawn[6] = {
            (float)cl_blood_film_detail->integer, cl_blood_film_height->value,
            cl_blood_film_full->value, cl_blood_film_trail->value,
            (float)cl_blood_film_smooth->integer, 1.f
        };
        if (memcmp(drawn, now_drawn, sizeof(drawn))) {
            memcpy(drawn, now_drawn, sizeof(drawn));
            for (int i = 0; i < film_cap; i++)
                if (film_tiles[i].used)
                    film_tiles[i].dirty = true;
        }
    }

    film_mesh_dirty();

    // Then the ticks' publishes, a blob at a time, within the frame's budget -
    // see BUDGET. The seeds go round-robin, so nothing waits for long.
    {
        static int next;
        static float mesh_us = 30.f;    // what a mesh has been costing
        static int queue[MAX_BLOOD_FILM_TILES];
        static unsigned queued[MAX_BLOOD_FILM_TILES], stamp;
        const float budget = max(0.f, cl_blood_film_mesh_ms->value) * 1000.f;
        const float band = film_band();

        for (;;) {
            const float spent = (float)(Sys_Microseconds() - t1);
            if (spent >= budget)
                break;

            int seed = -1;
            for (int n = 0; n < film_cap && seed < 0; n++) {
                const int i = (next + n) % film_cap;
                if (film_tiles[i].used && film_tiles[i].due)
                    seed = i;
            }
            if (seed < 0)
                break;
            next = (seed + 1) % film_cap;

            // A blob's meshes are its own tiles and the ring around them.
            const int most = max(1, (int)((budget - spent) / (mesh_us * 2.f)));
            int head = 0, tail = 0, published = 0;

            stamp++;
            queue[tail++] = seed;
            queued[seed] = stamp;

            while (head < tail && published < most) {
                film_tile_t *t = &film_tiles[queue[head++]];

                film_publish(t, band);
                published++;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        const int k = film_tile_lookup(t->plane, t->tx + dx, t->ty + dy);
                        if (k >= 0 && film_tiles[k].due && queued[k] != stamp) {
                            queued[k] = stamp;
                            queue[tail++] = k;
                        }
                    }
                }
            }

            const int meshes = film_stat_meshes;
            const uint64_t m0 = Sys_Microseconds();
            film_mesh_dirty();
            if (film_stat_meshes > meshes)
                mesh_us = 0.75f * mesh_us + 0.25f * (float)(Sys_Microseconds() - m0) / (film_stat_meshes - meshes);
        }
    }

    // Out to the renderer, within cl_blood_film_tris, counted in its chunks. The
    // fade keeps a fifth free, so this binds only while a burst outruns it -
    // and then it is new blood that waits a frame or two, not blood already on
    // screen that vanishes: what was drawn last frame goes first, the fading
    // part of it last.
    static bool was_shown[MAX_BLOOD_FILM_TILES];
    int chunks = 0, wanted = 0;

    for (int i = 0; i < film_cap; i++) {
        was_shown[i] = film_tiles[i].shown;
        film_tiles[i].shown = false;
    }

    for (int pass = 0; pass < 3; pass++)
    for (int i = 0; i < film_cap; i++) {
        film_tile_t *t = &film_tiles[i];

        if (!t->used || t->num_tris <= 0)
            continue;
        if (pass != (!was_shown[i] ? 2 : t->fade_from ? 1 : 0))
            continue;

        const int need = BLOOD_FILM_CHUNKS(min(t->num_tris, BLOOD_FILM_TILE_TRIS));
        wanted += need;
        if (chunks + need > film_chunk_cap) {
            film_starved++;
            film_stat_unshown++;
            continue;
        }
        chunks += need;
        t->shown = true;

        const film_plane_t *p = &film_planes[t->plane];
        blood_film_t *o = &film_out[film_num_out++];
        memset(o, 0, sizeof(*o));
        o->slot = i;
        o->version = t->version;
        o->num_tris = t->num_tris;
        o->verts = t->verts;
        VectorCopy(p->normal, o->plane);
        o->color = p->color;
        o->rgba = p->rgba;
        o->alpha = t->alpha;
        o->seed = t->seed;
    }
    film_chunks_used = wanted;

    const uint64_t t2 = Sys_Microseconds();
    film_stat_step_us += t1 - t0;
    film_stat_mesh_us += t2 - t1;

    if (cl_blood_film_stats->integer) {
        static unsigned last;
        const unsigned ms = Sys_Milliseconds();

        if (ms - last >= 1000) {
            int used = 0, awake = 0, tris = 0;
            for (int i = 0; i < film_cap; i++) {
                if (!film_tiles[i].used)
                    continue;
                used++;
                awake += film_tiles[i].awake;
                tris += film_tiles[i].num_tris;
            }

            // Built a piece at a time: va() has only four buffers.
            char extra[MAX_STRING_CHARS] = "";
            if (film_stat_full)
                Q_strlcat(extra, va(", %d refused (pool full)", film_stat_full), sizeof(extra));
            if (film_stat_unshown)
                Q_strlcat(extra, va(", %d tile-frames not drawn (over cl_blood_film_tris)", film_stat_unshown), sizeof(extra));
            if (film_stat_faded)
                Q_strlcat(extra, va(", %d tiles faded to make room", film_stat_faded), sizeof(extra));
            if (film_stat_dropped)
                Q_strlcat(extra, va(", %d tris over the tile cap", film_stat_dropped), sizeof(extra));
            if (film_stat_coarser)
                Q_strlcat(extra, va(", %d meshes done coarser to fit", film_stat_coarser), sizeof(extra));

            Com_Printf("blood film: %d/%d tiles (%d awake), %d tris in %d/%d chunks | per second: "
                       "%d steps %.2f ms, %d meshes %.2f ms, %d new tiles %.2f ms (in the above), "
                       "%d cells traced, %d corners, %d drips%s\n",
                       used, film_cap, awake, tris, film_chunks_used, film_chunk_cap,
                       film_stat_steps, film_stat_step_us / 1000.f,
                       film_stat_meshes, film_stat_mesh_us / 1000.f, film_stat_new,
                       film_stat_new_us / 1000.f, film_stat_classified, film_stat_corner,
                       film_stat_drips, extra);

            last = ms;
            film_stat_steps = film_stat_meshes = film_stat_drips = film_stat_corner = 0;
            film_stat_full = film_stat_new = film_stat_dropped = film_stat_faded = film_stat_coarser = 0;
            film_stat_unshown = film_stat_classified = 0;
            film_stat_step_us = film_stat_mesh_us = film_stat_new_us = 0;
        }
    }
}

const blood_film_t *CL_BloodFilmList(int *count)
{
    *count = film_num_out;
    return film_num_out ? film_out : NULL;
}
