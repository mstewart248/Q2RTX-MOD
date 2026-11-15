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
Blood droplets as real, shaded sphere geometry (cl_blood_spheres).

WHY THIS IS NOT PART OF THE PARTICLE SYSTEM
-------------------------------------------
Particles live in the EFFECTS TLAS, and that is not a shading pass. A hit there
runs pt_logic_particle(), which fetches a colour, applies a radial falloff and
alpha-composites the result over the already-traced image. There is no normal,
no BRDF and no secondary ray anywhere in it, so a particle cannot be lit, cannot
cast a shadow and cannot appear in a reflection. "Shiny" is not reachable from
that path at any price.

So these go into the GEOMETRY TLAS instead, as ordinary triangles in the
instanced primitive buffer, and get the full material path: direct light,
indirect bounce, shadows, and a presence in mirrors and water.

HOW IT FITS THE EXISTING PIPELINE
---------------------------------
The instanced primitive/position buffers are laid out as a series of sections -
opaque models, transparent, masked, viewer models, viewer weapon, explosions -
each recorded in EntityUploadInfo as an offset and a count, and each getting its
own BLAS. Blood is one more such section. It differs from the others only in how
it is filled: the model sections are written on the GPU by instance_geometry.comp
from a source model VBO, and blood has no source model, so it is written here on
the CPU and copied in from a host-visible staging ring.

All droplets share ONE BLAS and ONE TLAS instance, with their positions baked in
world space. That keeps them entirely off the MAX_MODEL_INSTANCES budget - a
burst of sixty costs one instance, not sixty.

MOTION VECTORS
--------------
VboPrimitive carries the previous frame's position per vertex, in custom0..2, as
a packed half4 DELTA. That is the whole mechanism: no instance matching, no
transform history, no stable slot index required. Each droplet's previous origin
is tracked client-side (cparticle_t::prev_org) and the delta is a pure
translation, identical for all three vertices of every one of its triangles.

Get this wrong and each burst leaves a red comet trail under DLSS-RR, which is
the first thing to look at if these ever start smearing.
*/

#include "vkpt.h"
#include "jobs.h"	// vkpt_parallel_for: the film's chunks, the staging copy
#include "conversion.h"
#include "shader/vertex_buffer.h"
#include "system/system.h"
#include "common/cpuprof.h"	// the GPU profiler's CPU breakdown

#include <assert.h>

// Triangles per droplet, as an icosahedron subdivided pt_blood_tess times:
// 0 -> 20 faces, 1 -> 80, 2 -> 320, 3 -> 1280.
//
// The vertex normals are already smooth - on a unit sphere the normal IS the
// position, and primary_rays.rgen interpolates them across the triangle - so
// these droplets are smooth-shaded at any tessellation. What the triangle count
// buys is purely the SILHOUETTE, and that is what reads as "low poly": a 20-face
// icosahedron has a visibly polygonal outline once a droplet covers more than a
// few pixels. Hence 80 by default, with 320 available for close work.
//
// LEVEL 3 EXISTS FOR ONE CASE: A DROPLET LARGE IN FRAME.
//
// It is not a general quality step and it is not the default. Subdivision
// is inherently x4, so level 3 is 1280 faces, and the slot stride - which
// is what every buffer here is sized from - goes up by the same factor
// whether or not any droplet is actually close enough to use it. At the
// default cl_blood_max 512 that is about 102 MB per section, the figure
// the upload note near the bottom of this file quotes for 2048 slots at
// level 2. Raise cl_blood_max as well and it scales from there.
//
// Note also that pt_blood_lod_air (default 1) biases a droplet still in
// FLIGHT down one level, so at pt_blood_tess 3 the spray draws at 320 and
// only landed splats reach 1280. If it is the airborne beads that read as
// polygonal up close, pt_blood_lod_air 0 is the cheaper knob to try first.
//
// 11-14 UPDATE - that is all superseded for droplets in flight by the
// CLOSE-UP LOD (pt_blood_lod_px, see CLOSE-UP REGIONS): a normal slot now
// holds at most level 2 at either tess 2 or 3, and a droplet that is large
// ON SCREEN gets level 3 (1280) or BLOOD_CLOSEUP_LEVEL (5120) from a small
// fixed pool, chosen by how far its outline would sit from a true circle in
// pixels. Splats still top out at level 2, whose 80-segment rim is already
// within a pixel at any size a pool reaches on screen.
//
// It is also the cost knob. Every per-frame expense scales linearly with it -
// the CPU generation loop, the host copy, and the dynamic BLAS rebuild, which
// happens on every frame anything in the section moves (see BLAS REUSE - a
// settled floor keeps its BLAS). The staging buffers are sized
// for MAX_BLOOD_SPHERES at the CURRENT tessellation and reallocated when it
// changes, so picking 320 costs memory only while it is selected.
#define BLOOD_SPHERE_MAX_SUBDIV 3
#define BLOOD_SPHERE_MAX_FACES  (20 << (2 * BLOOD_SPHERE_MAX_SUBDIV))
#define BLOOD_SPHERE_FACES(subdiv) (20 << (2 * (subdiv)))

// The highest level a droplet's OWN slot holds. pt_blood_tess 3 no longer widens
// the slots: anything above this lives in a close-up region.
#define BLOOD_SLOT_TOP_LEVEL    2

// The top close-up level, for an airborne SPHERE only (never a puddle - the
// puddle tables stop at BLOOD_SPHERE_MAX_SUBDIV). 5120 faces: its outline is
// within a pixel of a circle up to a ~1170 px radius, i.e. a droplet filling a
// 4K screen.
#define BLOOD_CLOSEUP_LEVEL     4

typedef struct {
	vec3_t pos[3];
	vec3_t nrm[3];      // explicit: a puddle's flat base does not share its position
} blood_face_t;

/*
PUDDLE geometry, used for a droplet that has landed.

A splat used to be the droplet's sphere squashed along the surface normal, and
that is what made landed blood look wrong in a way that was hard to name. A
squashed sphere is still a closed body: its centre sits a full half-thickness
above the surface and it bulges DOWN as much as it bulges up, so it reads as a
lens resting on the floor rather than as liquid lying on it. Flattening it
further only trades one problem for another, because the volume-preserving spread
widens it as it thins.

A puddle is a different shape, not a thinner sphere: a flat base sitting IN the
surface, and a shallow dome on top. The dome is what catches the highlight and
reads as surface tension; the flat base means there is nothing underneath to
raise it. The base is sunk slightly below the surface, so the rim - the one hard
edge in the mesh - is buried and the puddle appears to feather out where it meets
the floor instead of ending in a visible lip.

Built once per LOD, like the spheres. Segments and rings per level below; face
count is segments * 2 * rings.

SEGMENTS ARE THE SILHOUETTE, RINGS ARE ONLY THE DOME PROFILE - AND THE BUDGET
WAS ALREADY PAID FOR.

A slot is `worst_faces = max(sphere, puddle)` primitives wide (see
ensure_buffers), and at the top LOD the sphere is 320 while the puddle was 256 -
so 64 primitives per slot were reserved and never written. Spending the whole
320 on the puddle costs nothing at all: the same buffers are allocated and the
memory table is unchanged.

Where to spend it is not a toss-up. A landed splat's silhouette is the RIM of a
disc, so the faceting is exactly `segments`, and pooling makes that worse
without touching the mesh: at cl_blood_pool_max 24 a pool is a ~48-unit disc
drawn with the same 32 segments a 2-unit droplet gets, i.e. a 4.7-unit straight
chord. Rings only subdivide a dome that is cl_blood_flatten (0.16) tall and
smooth-shaded, so they move the outline only at a grazing view. 80 segments x 2
rings is 320 faces: two and a half times the rim resolution for the same
primitive count.

Level 1 now also carries all four wobble harmonics (they gate at 32 segments),
so an LOD step between 1 and 2 resolves the SAME outline more finely instead of
drawing a differently shaped one. Level 0 still drops the highest harmonic, but
nothing reaches level 0 until it is small on screen.

Level 3 keeps the same rule and the same 2 rings: the sphere is 1280 faces
there, so 320 segments spends exactly that budget on the rim and the puddle
still costs nothing beyond the stride the sphere already sets. Every wobble
harmonic is in by 64 segments, so level 3 resolves the outline level 2 draws
rather than a differently shaped one - the same property the 1-to-2 step has.
*/
#define BLOOD_PUDDLE_MAX_FACES (16*2*2 + 32*2*2 + 80*2*2 + 320*2*2)

static const int puddle_segments[BLOOD_SPHERE_MAX_SUBDIV + 1] = { 16, 32, 80, 320 };
static const int puddle_rings[BLOOD_SPHERE_MAX_SUBDIV + 1]    = {  2,  2,  2,   2 };

#define BLOOD_PUDDLE_FACES(level) (puddle_segments[level] * 2 * puddle_rings[level])

// All five sphere LOD levels, laid end to end: level 0 at [0,20), level 1 at
// [20,100), level 2 at [100,420), level 3 at [420,1700), the close-up level 4
// at [1700,6820). Built once - the whole set is 6820 faces, so there is no
// reason to rebuild when the tessellation changes, and holding all of them is
// what lets the level be picked PER DROPLET.
#define BLOOD_TEMPLATE_TOTAL (20 + 80 + 320 + 1280 + 5120)

static blood_face_t   sphere_template[BLOOD_TEMPLATE_TOTAL];
static uint32_t       sphere_normals[BLOOD_TEMPLATE_TOTAL][3];    // encode_normal of the above
static uint32_t       sphere_tangents[BLOOD_TEMPLATE_TOTAL][3];
static int            lod_offset[BLOOD_CLOSEUP_LEVEL + 1];        // first face of each level

// How far each level's OUTLINE falls inside the true sphere, as a fraction of
// the radius: the worst edge midpoint's 1 - |m|, measured off the template when
// it is built. 0.149 / 0.049 / 0.013 / 0.0034 / 0.00085. Times a droplet's
// projected radius in pixels it is the faceting in pixels, which is what the
// close-up LOD (pt_blood_lod_px) holds to a target.
static float          sphere_sag[BLOOD_CLOSEUP_LEVEL + 1];

static blood_face_t   puddle_template[BLOOD_PUDDLE_MAX_FACES];
static uint32_t       puddle_tangents[BLOOD_PUDDLE_MAX_FACES][3];
static int            puddle_lod_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];

/*
THE PUDDLE TEMPLATE AS UNIQUE VERTICES, WHICH IS WHERE THE REBUILD COST WENT.

blood_face_t stores three corners per face, so a vertex shared by six faces is
stored six times - and every one of those copies used to be transformed
separately. Per copy that is vertex_wobble (a sqrtf, an atan2f and up to four
sinf), then the inverse-scale normal transform, a VectorNormalize and an
encode_normal. A level-2 puddle is 256 faces, so 768 corner transforms for 162
distinct vertices: the same six transcendentals computed nearly five times over.

Measured on Matt's log, a rebuilt droplet cost ~41 us against ~2 us for one that
merely moved, and 61 rebuilds a frame were ~2.5 ms of a 4.4 ms gen.

vertex_wobble is already documented as A FUNCTION OF THE ANGLE ALONE, precisely
so that shared vertices cannot disagree and tear the mesh open along an edge.
That is exactly the property that makes computing it once per vertex legitimate:
the deduplicated result is BIT-IDENTICAL to the per-corner one, not an
approximation of it.

Vertices are deduplicated on POSITION AND NORMAL TOGETHER. A puddle's flat base
shares its rim positions with the dome while facing the opposite way, so
position alone would weld the crease flat.
*/
#define BLOOD_PUDDLE_MAX_VERTS (BLOOD_PUDDLE_MAX_FACES * 3)

static vec3_t         puddle_vert_pos[BLOOD_PUDDLE_MAX_VERTS];
static vec3_t         puddle_vert_nrm[BLOOD_PUDDLE_MAX_VERTS];
static uint16_t       puddle_face_vid[BLOOD_PUDDLE_MAX_FACES][3]; // relative to the level's base
static int            puddle_vert_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];
static int            puddle_vert_count[BLOOD_SPHERE_MAX_SUBDIV + 1];

static bool           templates_built = false;

// Per-droplet LOD level chosen by choose_lods(), reused by write_blood_geometry
// so the two cannot disagree about how many primitives a droplet needs.
static uint8_t        sphere_lod[MAX_BLOOD_SPHERES];

/*
Per-slot cache of what was generated last frame.

A PARKED SPLAT PRODUCES BYTE-IDENTICAL GEOMETRY EVERY FRAME. It does not move,
so its vertices, its normals and its zero motion delta are all exactly what they
were - and regenerating them meant a transform, a VectorNormalize and an
encode_normal per vertex, ~60k of each per frame for a floorful, to arrive at
bytes that were already sitting in the shadow buffer.

So the shadow buffer IS the cache. If a droplet's parameters are unchanged AND it
lands at the same primitive offset as last frame, the bytes it needs are already
in place and the whole generation step is skipped - no math, and not even a copy.
Only droplets that actually changed are rebuilt.

Comparing the whole blood_sphere_t is exactly the right test: it is POD, and
every field that affects the output is in it - including prev_origin, so a
droplet that is still moving compares unequal and is correctly regenerated.
*/
typedef struct {
	blood_sphere_t sphere;      // what it was built from
	uint32_t       prim_offset; // where it was written
	int            level;       // and at which tessellation
	bool           valid;       // holds a droplet's geometry
	bool           blanked;     // holds degenerate filler for an empty slot
	// its whole region is zero in the SHADOW (made zeroed, or blanked since, and not
	// written after): blanking it again is a memset of zeros - see FIRST BLOOD
	bool           shadow_zero;

	/*
	WHAT THE DEVICE HOLDS, which is a different question from what the shadow
	buffer holds, and the three fields below are the whole answer.

	`faces` is how many primitives at the head of the slot are real geometry.
	The rest of the slot's stride is zero in the shadow buffer - see BLANK THE
	REST OF THE SLOT - so it never needs to travel twice.

	`dev_span` is how far into the slot the DEVICE has content. Past it, the
	device is known to be zero, which is what makes it safe to upload only
	`faces` primitives for a droplet whose mesh did not grow. When a droplet
	shrinks (a 256-face puddle replacing a 320-face sphere), dev_span is still
	the old 320 and the upload covers the difference, blanking the leftovers on
	the device as well as in the shadow copy.

	`dev_pending` is simply "the device is out of date for this slot". It is
	STICKY: a slot that could not be uploaded this frame - because the section's
	primitive range does not reach it yet - keeps the flag and is uploaded when
	it does. That is what replaced the old clean_frames counter, which forced a
	full-range copy for several frames after anything structural changed.
	*/
	uint16_t       faces;
	uint16_t       dev_span;
	bool           dev_pending;

	// A film chunk's key instead of `sphere` - see FILM REGIONS.
	bool           is_film;
	uint8_t        film_part;   // which BLOOD_FILM_CHUNK_TRIS of the tile's triangles
	blood_film_t   film;
} blood_cache_entry_t;

/*
ULTRA REGIONS - the top tessellation level only right in front of the camera.

pt_blood_tess 3 is 1280 faces a droplet, and every slot used to be that wide
whatever its droplet actually drew. With a floor of 2048 splats that made the
blood BLAS a 2.6M-triangle rebuild every frame, most of it padding, and every
regenerated droplet cleared and re-uploaded 210 KB of slot. demo1 at 2048/tess 3
went from ~90 fps to ~60 as the floor filled up, BVH_UPDATE 0.3 -> 3.5-4.5 ms,
with 15-130 ms renderer hitches while blood landed.

So the section is split into REGIONS:

    [ ultra 0 .. U-1 ][ normal slot 0 .. cl_blood_max-1 ]
      close-up pool      stride = worst case at BLOOD_SLOT_TOP_LEVEL

A droplet lives in its client slot's normal region unless its chosen level
needs more faces than that holds, in which case it borrows one of the ultra
regions for as long as it needs it. When the pool is full the extra droplets
are drawn at the slot's top level, in their own slot.

Everything that used to be per slot (cache, dirty flag, device span, blanking,
uploads) is per REGION now, so a droplet changing region is just "the old region
went dead, the new one needs building" - which the machinery already handled.
With no pool, U = 0 and region r is slot r, exactly as before.

CLOSE-UP REGIONS (11-14) - the pool, re-aimed at SCREEN SIZE.

It used to be pt_blood_tess 3 only, 32 regions of 1280, handed to any droplet
in flight within pt_blood_ultra_dist (40 units). Two things were wrong with
that. Matt's setting is tess 2, where there was no pool at all, so a droplet
in flight was 80 faces (tess 2 minus pt_blood_lod_air) however close it came:
a bead filling a third of a 4K screen had an outline 26 px off a circle. And
40 units says nothing about size on screen - one droplet at 5 units needs
four times the detail of one at 20.

Now the pool exists at pt_blood_tess 2 AND 3 (with pt_blood_lod_px on), in two
sizes:

    BLOOD_CLOSEUP_BIG regions of BLOOD_SPHERE_FACES(4) = 5120
    BLOOD_CLOSEUP_MID regions of BLOOD_SPHERE_FACES(3) = 1280

40960 primitives in all - exactly what 32 x 1280 used to be, so the blood BLAS
carries the same degenerate filler at tess 3 as before, and at tess 2 that
filler is the whole cost of having the pool (~0.05 ms on a rebuild, against
~1.1 ms measured for 957K blood primitives; nothing on a reused BLAS).

choose_lods measures each airborne droplet's projected radius in pixels and
asks for the lowest level whose outline (sphere_sag) is within pt_blood_lod_px
of a circle. assign_closeups then serves the requests LARGEST ON SCREEN FIRST:
5120 and 1280 come from the pool, and a step up that still fits the droplet's
own slot (80 -> 320) is paid from pt_blood_lod_budget. Whatever is left over
keeps its distance-band level. So the cost is bounded by the pool and the
budget however much blood is in the air, and it lands only on the few droplets
big enough on screen to show the difference.
*/
#define BLOOD_CLOSEUP_BIG 4
#define BLOOD_CLOSEUP_MID 16
#define BLOOD_ULTRA_MAX   (BLOOD_CLOSEUP_BIG + BLOOD_CLOSEUP_MID)

/*
FILM REGIONS - surface blood (cl_blood_film), in CHUNKS of BLOOD_FILM_CHUNK_TRIS.

    [ film chunk 0 .. F-1 ][ ultra 0 .. U-1 ][ normal slot 0 .. cl_blood_max-1 ]
      BLOOD_FILM_CHUNK_TRIS   F = cl_blood_film_tris / BLOOD_FILM_CHUNK_TRIS

At the FRONT, because the BLAS covers the section from its start to the highest
region written. With the film in front, the droplet slots - thousands of them -
are only covered up to the highest one in use, which with the film on is just the
spray in the air; at the back, any tile would have dragged every slot into the
build. F is 0 while cl_blood_film is off, and then every offset is what it was.

A tile takes as many chunks as its triangles fill, the lowest free ones, and
gives them all back whenever its triangles change - it is rewritten then anyway -
so the film stays packed at the front however tiles come and go (FILM CHUNKS).
It used to be one region of BLOOD_FILM_TILE_TRIS per tile slot: 192 tiles were a
393K-primitive BLAS whatever they drew - typically 300-600 triangles a tile, the
rest degenerate filler - and every rewrite cleared all 2048 (2026-11-07).

A chunk is drawn straight from the client's triangles: the cache keys on the
whole blood_film_t, whose `version` the client bumps whenever they change, and on
which part of its triangles the chunk holds.
*/
#define BLOOD_MAX_REGIONS (MAX_BLOOD_FILM_CHUNKS + BLOOD_ULTRA_MAX + MAX_BLOOD_SPHERES)

static blood_cache_entry_t blood_cache[BLOOD_MAX_REGIONS];

// Which ultra region a client slot holds (-1: none), and the reverse.
static int16_t ultra_of_slot[MAX_BLOOD_SPHERES];
static int16_t slot_of_ultra[BLOOD_ULTRA_MAX];
static bool    ultra_maps_ready = false;

static struct {
	VboPrimitive*     prim_shadow;
	prim_positions_t* pos_shadow;
	uint32_t          max_prims;        // capacity of the buffers as currently sized
	uint32_t          prim_count;       // written by the last vkpt_blood_update
	int               sphere_count;     // droplets actually written
	int               splat_count;      // of those, how many took the splat path
	int               cache_hits;       // droplets whose geometry was reused as-is
	int               cache_moves;      // droplets whose geometry was only TRANSLATED
	uint32_t          stride;           // primitives reserved per droplet slot
	int               ultra_big;        // the first ultra_big ultra regions are BLOOD_CLOSEUP_LEVEL wide, the rest one level less
	int               ultra_count;      // ultra regions, after the film's (0: no pool)
	int               closeup_up;       // droplets drawn finer than their distance band this frame
	int               closeup_pool;     // ... of those, in an ultra region
	int               closeup_short;    // wanted finer and did not get it (pool full or budget spent)
	int               film_count;       // film CHUNKS at the very front (0: film off)
	int               film_tiles;       // tiles drawn this frame, for pt_blood_stats
	int               film_tris;
	int               film_chunks_used;
	int               film_truncated;   // tiles not drawn whole: past BLOOD_FILM_TILE_TRIS, or out of chunks
	bool              dirty_slot[BLOOD_MAX_REGIONS];  // regions rewritten this frame
	bool              moved_slot[BLOOD_MAX_REGIONS];  // ... or only translated, for the GPU to move (region_move)
	bool              gpu_move;         // this frame's pt_blood_gpu_move, as the droplet jobs read it
	uint32_t          last_moves;       // moves sent, for pt_blood_stats
	uint32_t          last_clears;      // stale tails zeroed on the GPU (ZEROS ON THE GPU)
	uint32_t          last_prim_offset; // where the blood section sat last frame
	bool              have_last_offset;
	uint32_t          primbuf_generation; // which incarnation of the instanced buffers
	bool              epoch_reset;      // the device's copy of the section is void
	uint32_t          last_regions;     // copy regions used, for pt_blood_stats
	bool              last_epoch;
	int               model_instance_index;
	uint64_t          last_gen_usec;    // write_blood_geometry's time, for pt_blood_stats
	size_t            last_up_bytes;    // what the last frame's regions carried
	bool              buffers_ready;
} blood;

/*
THE VULKAN HALF'S STAGING. vkpt_blood_prepare is shared with the D3D12 backend
(d3d12/frontend.c, world.c): it fills the shadow copy and names the regions that
changed, and each backend has its own staging to carry them to the device.
Vulkan's is here - a host-visible pair per frame in flight, the shadow's size,
remade only when that changes (vk_staging_ensure).
*/
static struct {
	BufferResource_t  staging_prim[MAX_FRAMES_IN_FLIGHT];
	BufferResource_t  staging_pos[MAX_FRAMES_IN_FLIGHT];
	VboPrimitive*     mapped_prim[MAX_FRAMES_IN_FLIGHT];
	prim_positions_t* mapped_pos[MAX_FRAMES_IN_FLIGHT];
	BufferResource_t  staging_moves[MAX_FRAMES_IN_FLIGHT];    // BloodMove[MAX_BLOOD_SPHERES]
	BloodMove*        mapped_moves[MAX_FRAMES_IN_FLIGHT];
	uint32_t          frame_index;
	uint32_t          capacity;         // primitives; 0 = none made (and no Vulkan call)
} blood_vk;

static void vk_staging_release(void)
{
	if (!blood_vk.capacity)
		return;

	// The staging buffers may still be referenced by a command buffer that has
	// not finished, so this cannot just free them.
	vkpt_device_wait_idle();

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		if (blood_vk.mapped_prim[i]) { buffer_unmap(&blood_vk.staging_prim[i]); blood_vk.mapped_prim[i] = NULL; }
		if (blood_vk.mapped_pos[i])  { buffer_unmap(&blood_vk.staging_pos[i]);  blood_vk.mapped_pos[i] = NULL; }
		if (blood_vk.mapped_moves[i]) { buffer_unmap(&blood_vk.staging_moves[i]); blood_vk.mapped_moves[i] = NULL; }
		buffer_destroy(&blood_vk.staging_prim[i]);
		buffer_destroy(&blood_vk.staging_pos[i]);
		buffer_destroy(&blood_vk.staging_moves[i]);
	}
	blood_vk.capacity = 0;
}

static bool vk_staging_ensure(uint32_t capacity)
{
	if (blood_vk.capacity == capacity)
		return true;

	vk_staging_release();

	const size_t prim_size = sizeof(VboPrimitive) * capacity;
	const size_t pos_size = sizeof(prim_positions_t) * capacity;

	// set first, so a failure part way through releases what was made
	blood_vk.capacity = capacity;
	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		if (buffer_create(&blood_vk.staging_prim[i], prim_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS
			|| buffer_create(&blood_vk.staging_pos[i], pos_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS
			|| buffer_create(&blood_vk.staging_moves[i], sizeof(BloodMove) * MAX_BLOOD_SPHERES,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
		{
			vk_staging_release();
			return false;
		}

		blood_vk.mapped_prim[i] = buffer_map(&blood_vk.staging_prim[i]);
		blood_vk.mapped_pos[i] = buffer_map(&blood_vk.staging_pos[i]);
		blood_vk.mapped_moves[i] = buffer_map(&blood_vk.staging_moves[i]);
	}
	return true;
}

/*
BLAS REUSE - pt_blood_blas_reuse, default 1 (0 rebuilds every frame, as before).

path_tracer.c used to rebuild blas_blood[] every frame over the whole section, and
on a settled floor that is most of what blood costs: 2048 splats at
pt_blood_tess 3 kept BVH_UPDATE at 3.5-4.5 ms for triangles that had not moved.

A bottom-level build reads exactly one thing, the positions in its range of
buf_positions_instanced, and the device's copy of this section is written in
exactly one place, vkpt_blood_update - the epoch fill and the per-region copies.
Nothing else ever lands in it while the section is reserved: instance_geometry
writes the model sections below blood_prim_base, which is pinned. The two ways
the bytes can change behind this file's back both end the epoch first, and an
epoch frame always fills and copies:
  - vkpt_vertex_buffer_ensure_primbuf_size recreating the buffers (generation);
  - a frame with no blood, when the base is not reserved and models may grow
    into it (have_last_offset goes false).

So `version` counts frames that wrote the section, and a BLAS built at the same
version, offset, count and build flags is the BLAS a rebuild would make now: same
input bytes, same flags, same allocation. Reusing it changes nothing a shader
reads either - attributes come from buf_primitive_instanced at
tlas_instance_prim_offsets + primitive id, and neither that buffer (no write, so
no bump) nor the offset (part of the key) has moved. The TLAS is still rebuilt
every frame and takes the BLAS's device address, which is unchanged because
nothing reallocated it: only a build can.

There is one BLAS per frame in flight and one shared section, so the version is
global and each BLAS is rebuilt once, on its own frame, after a change.
*/
static cvar_t* cvar_pt_blood_blas_reuse = NULL;

typedef struct {
	bool     valid;
	uint32_t version;
	uint32_t prim_offset;
	uint32_t prim_count;
	bool     fast_build;
} blood_blas_record_t;

static struct {
	uint32_t            version;          // bumped on every frame that writes the section
	uint64_t            reported_frame;   // qvk.frame_counter of the last vkpt_blood_update
	uint32_t            reported_offset;  // and the section it reported
	uint32_t            reported_count;
	blood_blas_record_t built[MAX_FRAMES_IN_FLIGHT];
	int                 builds, reuses;   // since the last pt_blood_stats line
} blood_blas;

static void blood_blas_reset(void)
{
	memset(&blood_blas, 0, sizeof(blood_blas));
	blood_blas.reported_frame = ~0ull;
}

// What vkpt_blood_update leaves for the BLAS build this frame. `wrote` is true
// whenever the device's copy of the section may have changed.
static void blood_blas_report(uint32_t prim_offset, uint32_t prim_count, bool wrote)
{
	if (wrote)
		blood_blas.version++;

	blood_blas.reported_frame = qvk.frame_counter;
	blood_blas.reported_offset = prim_offset;
	blood_blas.reported_count = prim_count;
}

static inline bool blood_blas_matches_report(uint32_t prim_offset, uint32_t prim_count)
{
	return blood_blas.reported_frame == qvk.frame_counter
		&& blood_blas.reported_offset == prim_offset
		&& blood_blas.reported_count == prim_count;
}

/* path_tracer.c, around its blas_blood[frame_idx] build: whether that BLAS can
   stand as it is. Anything this cannot vouch for - no section this frame, a
   section other than the one vkpt_blood_update reported, a frame index out of
   range - is a build. The caller still checks that the BLAS exists. */
bool vkpt_blood_blas_needs_build(int frame_idx, uint32_t prim_offset, uint32_t prim_count, bool fast_build)
{
	if (frame_idx < 0 || frame_idx >= MAX_FRAMES_IN_FLIGHT || prim_count == 0)
		return true;

	if (!cvar_pt_blood_blas_reuse || !cvar_pt_blood_blas_reuse->integer)
		return true;

	if (!blood_blas_matches_report(prim_offset, prim_count))
		return true;

	const blood_blas_record_t* b = blood_blas.built + frame_idx;

	if (!b->valid || b->version != blood_blas.version || b->prim_offset != prim_offset
		|| b->prim_count != prim_count || b->fast_build != fast_build)
		return true;

	blood_blas.reuses++;
	return false;
}

// ...and after every build it does make, so the next frame on this index knows
// what the BLAS holds. Called with reuse off too, so turning it back on is safe.
void vkpt_blood_blas_built(int frame_idx, uint32_t prim_offset, uint32_t prim_count, bool fast_build)
{
	if (frame_idx < 0 || frame_idx >= MAX_FRAMES_IN_FLIGHT)
		return;

	blood_blas_record_t* b = blood_blas.built + frame_idx;

	b->valid = prim_count > 0 && blood_blas_matches_report(prim_offset, prim_count);
	b->version = blood_blas.version;
	b->prim_offset = prim_offset;
	b->prim_count = prim_count;
	b->fast_build = fast_build;

	if (prim_count > 0)
		blood_blas.builds++;
}

// Inside the close-up pool: `big` regions of the close-up level, then the rest
// one level down (CLOSE-UP REGIONS). ultra_offset(count) is the pool's size.
static inline uint32_t ultra_offset(int u, int big)
{
	const uint32_t big_faces = BLOOD_SPHERE_FACES(BLOOD_CLOSEUP_LEVEL);
	const uint32_t mid_faces = BLOOD_SPHERE_FACES(BLOOD_CLOSEUP_LEVEL - 1);

	return u <= big ? (uint32_t)u * big_faces
	                : (uint32_t)big * big_faces + (uint32_t)(u - big) * mid_faces;
}

// Regions are [film][ultra][slots] - see FILM REGIONS and ULTRA REGIONS.
static inline uint32_t region_offset(int r)
{
	if (r < blood.film_count)
		return (uint32_t)r * BLOOD_FILM_CHUNK_TRIS;

	const uint32_t base = (uint32_t)blood.film_count * BLOOD_FILM_CHUNK_TRIS;
	r -= blood.film_count;

	return base + (r < blood.ultra_count
		? ultra_offset(r, blood.ultra_big)
		: ultra_offset(blood.ultra_count, blood.ultra_big) + (uint32_t)(r - blood.ultra_count) * blood.stride);
}

static inline uint32_t region_cap(int r)
{
	if (r < blood.film_count)
		return BLOOD_FILM_CHUNK_TRIS;
	r -= blood.film_count;
	if (r >= blood.ultra_count)
		return blood.stride;
	return (uint32_t)BLOOD_SPHERE_FACES(r < blood.ultra_big ? BLOOD_CLOSEUP_LEVEL : BLOOD_CLOSEUP_LEVEL - 1);
}

static inline int region_of_slot(int slot)
{
	if (blood.ultra_count && ultra_maps_ready && ultra_of_slot[slot] >= 0)
		return blood.film_count + ultra_of_slot[slot];
	return blood.film_count + blood.ultra_count + slot;
}

/*
FILM CHUNKS - which chunks hold each client tile's triangles, part by part, and
the blood_film_t they were given. Chunk c is region c (see FILM REGIONS).
*/
#define BLOOD_FILM_TILE_CHUNKS (BLOOD_FILM_TILE_TRIS / BLOOD_FILM_CHUNK_TRIS)

static int16_t      film_chunk_of[MAX_BLOOD_FILM_TILES][BLOOD_FILM_TILE_CHUNKS];
static uint8_t      film_num_chunks[MAX_BLOOD_FILM_TILES];
static blood_film_t film_held[MAX_BLOOD_FILM_TILES];
static bool         film_chunk_taken[MAX_BLOOD_FILM_CHUNKS];
static int          film_chunk_low;     // no free chunk below this one

static void reset_film_chunks(void)
{
	memset(film_num_chunks, 0, sizeof(film_num_chunks));
	memset(film_held, 0, sizeof(film_held));
	memset(film_chunk_taken, 0, sizeof(film_chunk_taken));
	film_chunk_low = 0;
}

static void release_film_chunks_from(int slot, int keep)
{
	for (int p = keep; p < film_num_chunks[slot]; p++)
	{
		const int c = film_chunk_of[slot][p];
		film_chunk_taken[c] = false;
		if (c < film_chunk_low)
			film_chunk_low = c;
	}
	if (film_num_chunks[slot] > keep)
		film_num_chunks[slot] = keep;
}

static void release_film_chunks(int slot)
{
	release_film_chunks_from(slot, 0);
}

// The lowest free chunk, or -1.
static int take_film_chunk(void)
{
	for (int c = film_chunk_low; c < blood.film_count; c++)
	{
		if (!film_chunk_taken[c])
		{
			film_chunk_taken[c] = true;
			film_chunk_low = c + 1;
			return c;
		}
	}
	film_chunk_low = blood.film_count;
	return -1;
}

static void reset_ultra_maps(void)
{
	memset(ultra_of_slot, 0xff, sizeof(ultra_of_slot));
	memset(slot_of_ultra, 0xff, sizeof(slot_of_ultra));
	ultra_maps_ready = true;
}

// transparency.c - palette index or rgba to linear float RGB
extern void cast_u32_to_f32_color(int color_index, const color_t* pcolor, float* color_f32, float hdr_factor);
// bsp_mesh.c - the CPU port of utils.glsl's encode_normal
extern uint32_t encode_normal(const vec3_t normal);

static cvar_t* cvar_pt_blood_spheres = NULL;
static cvar_t* cvar_pt_blood_lod_px = NULL;
static cvar_t* cvar_pt_blood_lod_budget = NULL;
static cvar_t* cvar_pt_blood_upload_merge = NULL;
static cvar_t* cvar_pt_blood_gpu_move = NULL;

/*
MOVED ON THE GPU (pt_blood_gpu_move, 11-15). A droplet that only slid keeps its
mesh, translated (drop_job's THE SAME MESH, MOVED), and the device already holds
that mesh - yet the whole of it went over again: 164 bytes a triangle, every
frame, for every droplet in motion. Matt's 11-15 dip captures were blood uploads
in 12 of 22, 29-64 MB and 2.4-6.8 ms on the GPU a frame plus 1.3-3.4 ms of the
CPU writing the staging, with 200-2000 droplets moved and none made again.

Now such a droplet is a BloodMove (vertex_buffer.h) - its offset, its cluster and
motion vector, where its primitives sit - and blood_move.comp applies it to the
device's copy. The shadow copy is still moved on the CPU as before, so it stays
what the device holds: a region sent whole later is right either way.

A move is only good against a device copy in step with the shadow's LAST frame,
so it falls back to the region's upload whenever that is in doubt: an epoch, the
region already owed an upload (dev_pending), its device span not its mesh, or the
region outside this frame's section. Any backend that cannot carry the moves out
must invalidate (an epoch), exactly as for the regions. 0 = every moved droplet
uploaded whole, as before - the A/B.
*/
static BloodMove region_move[BLOOD_MAX_REGIONS];   // drop_job's, by region
static BloodMove move_list[MAX_BLOOD_SPHERES];     // vkpt_blood_prepare's, to the backend
static cvar_t* cvar_pt_blood_tess = NULL;
static cvar_t* cvar_pt_blood_stats = NULL;
static cvar_t* cvar_pt_blood_lod_near = NULL;
static cvar_t* cvar_pt_blood_lod_far = NULL;
static cvar_t* cvar_pt_blood_lod_air = NULL;
static cvar_t* cvar_pt_blood_puddle_sink = NULL;
static cvar_t* cvar_pt_blood_wobble = NULL;
static cvar_t* cvar_pt_blood_wobble_max = NULL;
static cvar_t* cvar_pt_blood_splat_alpha = NULL;

// cl_blood_splat_size, read once per frame - splat_basis is called per droplet
// and the client owns the cvar.
static float global_blood_splat_size = 1.5f;

/*
================
Icosphere generation

Built once, on first use. A unit sphere's vertex normal IS its position, which is
also what makes get_blood_normal() possible without a tangent basis - see the
comment on that function in water.glsl.
================
*/
static void subdivide_face(const vec3_t a, const vec3_t b, const vec3_t c, int depth, int* face_count)
{
	if (depth == 0)
	{
		if (*face_count >= BLOOD_TEMPLATE_TOTAL)
			return;

		blood_face_t* f = sphere_template + *face_count;
		VectorCopy(a, f->pos[0]);
		VectorCopy(b, f->pos[1]);
		VectorCopy(c, f->pos[2]);
		// On a unit sphere the vertex normal IS the position.
		VectorCopy(a, f->nrm[0]);
		VectorCopy(b, f->nrm[1]);
		VectorCopy(c, f->nrm[2]);
		(*face_count)++;
		return;
	}

	vec3_t ab, bc, ca;
	for (int i = 0; i < 3; i++)
	{
		ab[i] = a[i] + b[i];
		bc[i] = b[i] + c[i];
		ca[i] = c[i] + a[i];
	}
	VectorNormalize(ab);
	VectorNormalize(bc);
	VectorNormalize(ca);

	subdivide_face(a,  ab, ca, depth - 1, face_count);
	subdivide_face(ab, b,  bc, depth - 1, face_count);
	subdivide_face(ca, bc, c,  depth - 1, face_count);
	subdivide_face(ab, bc, ca, depth - 1, face_count);
}

/*
================
build_puddle_templates

A dome over a flat base, in a unit space where xy spans the radius and z runs
from 0 at the base to 1 at the apex.

The dome follows a hemisphere profile rather than a cone or a flat disc, because
a curved top is what gives a moving highlight - a flat top lights uniformly and
reads as a decal, which is most of what made this look uncanny.
================
*/
static void emit_puddle_face(int* n, const vec3_t a, const vec3_t b, const vec3_t c,
                             const vec3_t na, const vec3_t nb, const vec3_t nc)
{
	if (*n >= BLOOD_PUDDLE_MAX_FACES)
		return;

	blood_face_t* f = puddle_template + *n;
	VectorCopy(a, f->pos[0]); VectorCopy(b, f->pos[1]); VectorCopy(c, f->pos[2]);
	VectorCopy(na, f->nrm[0]); VectorCopy(nb, f->nrm[1]); VectorCopy(nc, f->nrm[2]);
	(*n)++;
}

static void build_puddle_templates(void)
{
	int n = 0;

	for (int level = 0; level <= BLOOD_SPHERE_MAX_SUBDIV; level++)
	{
		puddle_lod_offset[level] = n;

		const int segs = puddle_segments[level];
		const int rings = puddle_rings[level];

		const vec3_t down = { 0.f, 0.f, -1.f };
		const vec3_t apex = { 0.f, 0.f,  1.f };
		const vec3_t base_centre = { 0.f, 0.f, 0.f };

		for (int i = 0; i < segs; i++)
		{
			const float a0 = (float)i / segs * 2.f * (float)M_PI;
			const float a1 = (float)(i + 1) / segs * 2.f * (float)M_PI;

			// Flat base. Wound so it faces down, away from the dome.
			vec3_t b0 = { cosf(a0), sinf(a0), 0.f };
			vec3_t b1 = { cosf(a1), sinf(a1), 0.f };
			emit_puddle_face(&n, base_centre, b1, b0, down, down, down);

			// Dome, ring by ring from the rim up. On a unit hemisphere the
			// normal IS the position, which is what keeps this cheap.
			for (int r = 0; r < rings; r++)
			{
				const float t0 = (float)r / rings * ((float)M_PI * 0.5f);
				const float t1 = (float)(r + 1) / rings * ((float)M_PI * 0.5f);

				const float r0 = cosf(t0), z0 = sinf(t0);
				const float r1 = cosf(t1), z1 = sinf(t1);

				vec3_t p00 = { r0 * cosf(a0), r0 * sinf(a0), z0 };
				vec3_t p01 = { r0 * cosf(a1), r0 * sinf(a1), z0 };
				vec3_t p10 = { r1 * cosf(a0), r1 * sinf(a0), z1 };
				vec3_t p11 = { r1 * cosf(a1), r1 * sinf(a1), z1 };

				if (r + 1 == rings)
				{
					// Top band closes on the apex.
					emit_puddle_face(&n, p00, p01, apex, p00, p01, apex);
				}
				else
				{
					emit_puddle_face(&n, p00, p01, p11, p00, p01, p11);
					emit_puddle_face(&n, p00, p11, p10, p00, p11, p10);
				}
			}
		}

		assert(n - puddle_lod_offset[level] == BLOOD_PUDDLE_FACES(level));

		// Deduplicate this level's corners into unique vertices. Exact float
		// compare on purpose: the corners come from the same expressions, so
		// shared ones are bit-identical, and a tolerance would risk welding two
		// vertices that are genuinely distinct at the smallest LOD.
		const int vbase = (level == 0) ? 0
			: puddle_vert_offset[level - 1] + puddle_vert_count[level - 1];

		puddle_vert_offset[level] = vbase;

		int nverts = 0;

		for (int f = puddle_lod_offset[level]; f < n; f++)
		{
			for (int i = 0; i < 3; i++)
			{
				const float* p = puddle_template[f].pos[i];
				const float* q = puddle_template[f].nrm[i];

				int found = -1;
				for (int v = 0; v < nverts; v++)
				{
					if (VectorCompare(puddle_vert_pos[vbase + v], p)
						&& VectorCompare(puddle_vert_nrm[vbase + v], q))
					{
						found = v;
						break;
					}
				}

				if (found < 0)
				{
					assert(vbase + nverts < BLOOD_PUDDLE_MAX_VERTS);
					VectorCopy(p, puddle_vert_pos[vbase + nverts]);
					VectorCopy(q, puddle_vert_nrm[vbase + nverts]);
					found = nverts++;
				}

				puddle_face_vid[f][i] = (uint16_t)found;
			}
		}

		puddle_vert_count[level] = nverts;

		// SELF-CHECK, because "bit-identical" is the entire claim being made.
		//
		// The wobble and the transforms below are the same expressions whichever
		// way the mesh is walked, so the deduplicated build differs from the
		// per-corner one only if an INDEX is wrong - an off-by-one in the level
		// base being the obvious way. Verify the mapping once, at startup, rather
		// than discovering it as torn puddles: every corner must resolve to a
		// vertex holding exactly the position and normal that corner had.
		for (int f = puddle_lod_offset[level]; f < n; f++)
		{
			for (int i = 0; i < 3; i++)
			{
				const int v = vbase + puddle_face_vid[f][i];

				if (!VectorCompare(puddle_vert_pos[v], puddle_template[f].pos[i])
					|| !VectorCompare(puddle_vert_nrm[v], puddle_template[f].nrm[i]))
				{
					Com_EPrintf("blood: puddle vertex table is wrong at level %d, "
						"face %d corner %d - splats will render torn\n", level, f, i);
					break;
				}
			}
		}

		assert(vbase + nverts <= BLOOD_PUDDLE_MAX_VERTS);
	}

	// Tangents. Nothing samples a tangent-space map on this material, so any
	// well-conditioned perpendicular will do - same construction as the spheres.
	for (int f = 0; f < n; f++)
	{
		for (int i = 0; i < 3; i++)
		{
			const float* nv = puddle_template[f].nrm[i];
			vec3_t seed, tangent;
			if (fabsf(nv[0]) < 0.577f)      VectorSet(seed, 1.f, 0.f, 0.f);
			else if (fabsf(nv[1]) < 0.577f) VectorSet(seed, 0.f, 1.f, 0.f);
			else                            VectorSet(seed, 0.f, 0.f, 1.f);

			CrossProduct(seed, nv, tangent);
			VectorNormalize(tangent);
			puddle_tangents[f][i] = encode_normal(tangent);
		}
	}
}

static void build_sphere_templates(void)
{
	// Regular icosahedron, from the golden ratio.
	const float t = 1.6180339887f;
	vec3_t v[12] = {
		{ -1,  t,  0 }, {  1,  t,  0 }, { -1, -t,  0 }, {  1, -t,  0 },
		{  0, -1,  t }, {  0,  1,  t }, {  0, -1, -t }, {  0,  1, -t },
		{  t,  0, -1 }, {  t,  0,  1 }, { -t,  0, -1 }, { -t,  0,  1 }
	};
	for (int i = 0; i < 12; i++)
		VectorNormalize(v[i]);

	static const int faces[20][3] = {
		{  0, 11,  5 }, {  0,  5,  1 }, {  0,  1,  7 }, {  0,  7, 10 }, {  0, 10, 11 },
		{  1,  5,  9 }, {  5, 11,  4 }, { 11, 10,  2 }, { 10,  7,  6 }, {  7,  1,  8 },
		{  3,  9,  4 }, {  3,  4,  2 }, {  3,  2,  6 }, {  3,  6,  8 }, {  3,  8,  9 },
		{  4,  9,  5 }, {  2,  4, 11 }, {  6,  2, 10 }, {  8,  6,  7 }, {  9,  8,  1 }
	};

	int face_count = 0;
	for (int level = 0; level <= BLOOD_CLOSEUP_LEVEL; level++)
	{
		lod_offset[level] = face_count;

		for (int i = 0; i < 20; i++)
			subdivide_face(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]], level, &face_count);

		assert(face_count - lod_offset[level] == BLOOD_SPHERE_FACES(level));

		// The outline is made of edges, so its worst gap to the circle is the
		// edge whose midpoint sits deepest inside the unit sphere.
		float sag = 0.f;
		for (int f = lod_offset[level]; f < face_count; f++)
		{
			for (int e = 0; e < 3; e++)
			{
				vec3_t mid;
				VectorAdd(sphere_template[f].pos[e], sphere_template[f].pos[(e + 1) % 3], mid);
				sag = max(sag, 1.f - 0.5f * VectorLength(mid));
			}
		}
		sphere_sag[level] = sag;
	}

	assert(face_count == BLOOD_TEMPLATE_TOTAL);

	// Precompute the encoded normals and tangents. Both are constant for the
	// template: a droplet is only ever translated and uniformly scaled, and
	// neither of those changes a direction.
	for (int f = 0; f < face_count; f++)
	{
		for (int i = 0; i < 3; i++)
		{
			const float* n = sphere_template[f].pos[i];
			sphere_normals[f][i] = encode_normal(n);

			// Any unit vector perpendicular to the normal will do - nothing
			// samples a tangent-space map on this material - but it must be
			// well conditioned, so seed the cross product from whichever axis
			// the normal is least aligned with. One component of a unit vector
			// is always below 1/sqrt(3), so the seed is never closer than 54
			// degrees to the normal.
			vec3_t seed, tangent;
			if (fabsf(n[0]) < 0.577f)      VectorSet(seed, 1.f, 0.f, 0.f);
			else if (fabsf(n[1]) < 0.577f) VectorSet(seed, 0.f, 1.f, 0.f);
			else                           VectorSet(seed, 0.f, 0.f, 1.f);

			CrossProduct(seed, n, tangent);
			VectorNormalize(tangent);
			sphere_tangents[f][i] = encode_normal(tangent);
		}
	}

	build_puddle_templates();

	templates_built = true;
}

/*
================
ensure_buffers

Allocated at MAP LOAD when blood is on (vkpt_blood_preallocate), otherwise on first
use, rather than at startup: cl_blood_spheres defaults off, and a player who never
turns it on should not pay for this memory.
================
*/
static void release_buffers(void)
{
	if (!blood.buffers_ready)
		return;

	// Only the shadow copy, which the GPU never reads. The staging belongs to
	// each backend (vk_staging_ensure remakes Vulkan's when the size changes).
	if (blood.prim_shadow) { Z_Free(blood.prim_shadow); blood.prim_shadow = NULL; }
	if (blood.pos_shadow)  { Z_Free(blood.pos_shadow);  blood.pos_shadow = NULL; }

	blood.buffers_ready = false;
	blood.max_prims = 0;

	memset(blood_cache, 0, sizeof(blood_cache));
}

/*
================
ensure_buffers

Sized for the WORST CASE up front, and reallocated only when the settings that
define that worst case change.

THIS IS WHY, and it is the single worst bug this feature had. The buffers used to
grow to fit whatever the current frame needed, rounded up to 8191 primitives. Full
capacity is cl_blood_max * 320 faces, about 164000 primitives - so filling up from
empty took twenty growth steps, and EVERY ONE of them called
vkpt_device_wait_idle() through release_buffers(). That is a full GPU flush.

Twenty pipeline flushes spread across the second or two while a burst of blood is
landing, each also wiping the geometry cache so every splat rebuilt from scratch
afterwards. It presented exactly as reported: a massive hitch while blood was
landing, gone the moment it settled, and no CPU core anywhere near saturated -
because the thread was not computing, it was blocked waiting on the GPU.

The capacity is bounded and small enough to just take: at the default 512 droplets
and 320 faces it is 27 MB of shadow plus 54 MB of staging (cl_blood_max 5120: ten
times that). Allocated at map load when blood is on (vkpt_blood_preallocate), else
on first blood, never at startup, so a player who never turns this on pays nothing.
================
*/
/*
The number of slots the buffers cover, and the ONE definition of it.

MAX_BLOOD_SPHERES is only a ceiling now. The real capacity is cl_blood_max, and
three things have to agree on it or droplets go silently missing: ensure_buffers
sizes the memory from it, vkpt_blood_prim_count reserves address space from it,
and CL_AllocBloodSlot must never hand out a slot beyond it.

They used to disagree - the reservation was MAX_BLOOD_SPHERES while the buffers
were cl_blood_max - which was harmless only because the two constants happened to
be equal. Raising the ceiling without this would have produced droplets that
allocate a slot, generate geometry, and then fail the blood.max_prims bounds check
at the write and simply never appear.
*/
static int vkpt_blood_slot_capacity(void)
{
	const int wanted = Cvar_Get("cl_blood_max", "512", CVAR_ARCHIVE)->integer;

	return max(1, min(wanted, MAX_BLOOD_SPHERES));
}

/*
The section layout - see ULTRA REGIONS. The ONE definition, shared by
ensure_buffers and vkpt_blood_prim_count, which have to agree to the primitive.
*/
/*
How many film CHUNKS the section carries - see FILM REGIONS. The client registers
cl_blood_film_tris too and keeps its tiles within it, counting the same way.
0 while the film is off: the layout is then exactly the old one.
*/
static int blood_film_capacity(void)
{
	const cvar_t* film = Cvar_FindVar("cl_blood_film");

	if (!film || !film->integer)
		return 0;

	int tris = Cvar_Get("cl_blood_film_tris", BLOOD_FILM_TRIS_DEFAULT, CVAR_ARCHIVE)->integer;
	tris = max(BLOOD_FILM_CHUNK_TRIS, min(tris, MAX_BLOOD_FILM_CHUNKS * BLOOD_FILM_CHUNK_TRIS));

	// An eighth more than the client fills with new blood: a tile already on
	// screen that grows past the cap keeps being drawn (film_finish in
	// blood_film.c, which counts the same eighth).
	const int chunks = BLOOD_FILM_CHUNKS(tris);
	return min(chunks + chunks / 8, MAX_BLOOD_FILM_CHUNKS);
}

// The highest level a droplet's own slot is sized for: pt_blood_tess, but never
// past BLOOD_SLOT_TOP_LEVEL - finer than that is the close-up pool's job.
static inline int slot_top_level(void)
{
	return max(0, min(cvar_pt_blood_tess->integer, BLOOD_SLOT_TOP_LEVEL));
}

// Whether the section carries the close-up pool (CLOSE-UP REGIONS): from tess 2
// up, with the close-up LOD on. Only whether pt_blood_lod_px is ZERO matters
// here, so dragging its slider never reallocates anything.
static inline bool closeup_pool_on(void)
{
	return cvar_pt_blood_tess->integer >= BLOOD_SLOT_TOP_LEVEL
		&& cvar_pt_blood_lod_px && cvar_pt_blood_lod_px->value > 0.f;
}

static uint32_t blood_layout(int max_droplets, int* film_count, int* ultra_count,
                             int* ultra_big, uint32_t* stride)
{
	*film_count = blood_film_capacity();

	const int top = slot_top_level();
	*stride = (uint32_t)max(BLOOD_SPHERE_FACES(top), BLOOD_PUDDLE_FACES(top));

	const bool pool = closeup_pool_on();
	*ultra_big = pool ? BLOOD_CLOSEUP_BIG : 0;
	*ultra_count = pool ? BLOOD_CLOSEUP_BIG + BLOOD_CLOSEUP_MID : 0;

	return (uint32_t)*film_count * BLOOD_FILM_CHUNK_TRIS
	     + ultra_offset(*ultra_count, *ultra_big) + (uint32_t)max_droplets * *stride;
}

// set by vkpt_blood_prepare around its calls: the buffers made mid-game, not at load
static bool blood_in_frame;

static bool ensure_buffers(void)
{
	const int max_droplets = cvar_pt_blood_spheres->integer ? vkpt_blood_slot_capacity() : 1;

	int film_count, ultra_count, ultra_big;
	uint32_t stride;
	const uint32_t needed = blood_layout(max_droplets, &film_count, &ultra_count, &ultra_big, &stride);

	// Exact compare, not >=: the point is to reallocate ONLY when the settings
	// change, never in response to how much blood happens to be on screen.
	if (blood.buffers_ready && blood.max_prims == needed
		&& blood.stride == stride && blood.ultra_count == ultra_count && blood.ultra_big == ultra_big
		&& blood.film_count == film_count)
		return true;

	/* Said every time (11-14): Matt's first blood of rbase1 took 14.6 ms in the
	   renderer's blood meshes for 627 new droplets - the frame after, 0.7 ms for 744.
	   Made in a frame rather than at map load, this is the one thing that big: a
	   zeroed shadow of every primitive. What changed since the last one says why. */
	const uint64_t alloc_t0 = Sys_Microseconds();
	const bool had = blood.buffers_ready;
	char why[160] = "first";
	if (had)
		Q_snprintf(why, sizeof(why), "prims %u -> %u, stride %u -> %u, close-up %d/%d -> %d/%d, film chunks %d -> %d",
			blood.max_prims, needed, blood.stride, stride, blood.ultra_count, blood.ultra_big,
			ultra_count, ultra_big, blood.film_count, film_count);

	release_buffers();

	const size_t prim_size = sizeof(VboPrimitive) * needed;
	const size_t pos_size = sizeof(prim_positions_t) * needed;

	// Build into ordinary memory and copy the used range in one go, rather than
	// writing straight through the mapped pointer: that is write-combined host
	// memory, and the generation loop touches each primitive's fields out of
	// order.
	blood.prim_shadow = Z_Mallocz(prim_size);
	blood.pos_shadow = Z_Mallocz(pos_size);

	blood.stride = stride;
	blood.ultra_big = ultra_big;
	blood.ultra_count = ultra_count;
	blood.film_count = film_count;
	blood.max_prims = needed;
	reset_ultra_maps();
	blood.buffers_ready = true;

	// New buffers, a new stride, and a shadow copy full of zeros: nothing the
	// device holds for the old layout means anything now.
	blood.epoch_reset = true;

	// The shadow buffer is the cache's backing store, so a reallocation throws
	// away everything it was vouching for - the film's chunks included.
	memset(blood_cache, 0, sizeof(blood_cache));
	reset_film_chunks();
	// Z_Mallocz: every region of the new shadow is zero already
	for (int i = 0; i < BLOOD_MAX_REGIONS; i++)
		blood_cache[i].shadow_zero = true;

	Com_Printf("Blood: %.0f MB of shadow for %u primitives in %.1f ms (%s; %s)\n",
		(double)(prim_size + pos_size) / (1024.0 * 1024.0), needed,
		(double)(Sys_Microseconds() - alloc_t0) * 1e-3,
		blood_in_frame ? "IN A FRAME" : "map load", why);
	return true;
}

/*
================
vkpt_blood_initialize / vkpt_blood_destroy
================
*/
// vkpt_init_frontend: prepare_entities sizes the blood section from these in
// the frame's CPU half
void vkpt_blood_init_cvars(void)
{
	// Renderer-side master switch, separate from the client's cl_blood_spheres
	// so a screenshot A/B does not have to wait for droplets already in the air
	// to expire.
	cvar_pt_blood_spheres = Cvar_Get("pt_blood_spheres", "1", 0);

	// Triangles per droplet: 0 -> 20, 1 -> 80. This is the cost knob; see the
	// comment on BLOOD_SPHERE_MAX_SUBDIV.
	cvar_pt_blood_tess = Cvar_Get("pt_blood_tess", "2", CVAR_ARCHIVE);

	// CLOSE-UP LOD (see CLOSE-UP REGIONS): how far, in screen pixels, a droplet
	// in flight may let its faceted outline stray from a circle before it is
	// drawn finer. 0 = off, the distance bands alone (and no pool). Replaces
	// pt_blood_ultra_dist.
	cvar_pt_blood_lod_px = Cvar_Get("pt_blood_lod_px", "1", CVAR_ARCHIVE);

	// Extra triangles a frame the close-up LOD may add INSIDE droplets' own
	// slots (an 80-face bead stepped up to 320 is 240). The pool is bounded by
	// its own size and does not draw on this.
	cvar_pt_blood_lod_budget = Cvar_Get("pt_blood_lod_budget", "32768", CVAR_ARCHIVE);
	// upload regions this many primitives apart or closer go as one copy, the gap
	// with them (see MERGING ACROSS A GAP in vkpt_blood_prepare); 0 = only touching ones
	cvar_pt_blood_upload_merge = Cvar_Get("pt_blood_upload_merge", "128", 0);
	// droplets that only moved are moved by the GPU in place, not sent again (see
	// MOVED ON THE GPU); 0 = sent whole, as before
	cvar_pt_blood_gpu_move = Cvar_Get("pt_blood_gpu_move", "1", 0);

	// Print live droplet and triangle counts once a second, so the cost of a
	// given firefight can be read off rather than guessed at.
	cvar_pt_blood_stats = Cvar_Get("pt_blood_stats", "0", 0);

	// Keep the blood BLAS while the section is unchanged - see BLAS REUSE. Not
	// archived: an A/B switch should come back on by itself.
	cvar_pt_blood_blas_reuse = Cvar_Get("pt_blood_blas_reuse", "1", 0);
}

VkResult vkpt_blood_initialize(void)
{
	memset(&blood, 0, sizeof(blood));

	vkpt_blood_init_cvars();
	blood_blas_reset();

	// The appearance cvars live in the global UBO, which registers everything in
	// its list with no flags at all. That is right for a tuning knob nobody sets
	// twice, but these are exposed in the Effects menu, and a slider that forgets
	// its value every launch is not a setting. Re-Cvar_Get them with CVAR_ARCHIVE
	// to OR the flag in; whichever of the two registrations runs first wins the
	// default, so THESE STRINGS MUST MATCH the UBO_CVAR_DO list in global_ubo.h.
	Cvar_Get("pt_blood_roughness",       "0.06", CVAR_ARCHIVE);
	Cvar_Get("pt_blood_specular",        "1.6",  CVAR_ARCHIVE);
	Cvar_Get("pt_blood_normal_strength", "0.35", CVAR_ARCHIVE);
	Cvar_Get("pt_blood_normal_scale",    "1.4",  CVAR_ARCHIVE);
	Cvar_Get("pt_blood_normal_speed",    "1.0",  CVAR_ARCHIVE);
	Cvar_Get("pt_blood_splat_dark",      "0.55", CVAR_ARCHIVE);
	Cvar_Get("pt_blood_thin_dark",       "0.12", CVAR_ARCHIVE);
	Cvar_Get("pt_blood_thin_power",      "2.0",  CVAR_ARCHIVE);

	// Distance LOD. Inside _near a droplet gets the full pt_blood_tess; past
	// _far it drops two levels; between them, one. Set _near very large to
	// disable the LOD and tessellate everything at full rate.
	cvar_pt_blood_lod_near = Cvar_Get("pt_blood_lod_near", "160", CVAR_ARCHIVE);
	cvar_pt_blood_lod_far = Cvar_Get("pt_blood_lod_far", "500", CVAR_ARCHIVE);

	// Tessellation levels to drop for a droplet still in FLIGHT. 0 restores the
	// old behaviour exactly - see the note in choose_lods.
	cvar_pt_blood_lod_air = Cvar_Get("pt_blood_lod_air", "1", CVAR_ARCHIVE);

	// How far a puddle's flat base is sunk below the surface, as a fraction of
	// its own height. Its only job is to bury the rim - the one hard edge in the
	// mesh - so the puddle appears to feather into the floor instead of ending
	// in a visible lip. Too much and the puddle disappears into the ground.
	cvar_pt_blood_puddle_sink = Cvar_Get("pt_blood_puddle_sink", "0.4", CVAR_ARCHIVE);

	// How irregular a landed puddle's outline is, as a fraction of its radius.
	// A perfect ellipse reads as a squashed sphere no matter how well it is lit;
	// a rim that wanders even slightly reads as something that splashed. Applied
	// once, at generation, and then cached with the rest of the geometry.
	cvar_pt_blood_wobble = Cvar_Get("pt_blood_wobble", "0.3", CVAR_ARCHIVE);

	// Largest wobble lobe in WORLD UNITS, whatever the pool's radius.
	// pt_blood_wobble is a fraction of the radius, which is the right unit for a
	// droplet and the wrong one for a pool: at cl_blood_pool_max 24 a 0.3
	// fraction is a plus or minus 7-unit lobe, and three of those is an amoeba
	// rather than a splash. 0 restores the uncapped behaviour.
	cvar_pt_blood_wobble_max = Cvar_Get("pt_blood_wobble_max", "3.5", CVAR_ARCHIVE);

	// Opacity of a LANDED splat, written into each primitive's alpha below.
	//
	// Registered in the UBO cvar list, not here - the shader needs it too, to
	// drop blood out of the shadow mask. The name and default must match that
	// registration exactly; the flags do not, and must not - Cvar_Get ORs them
	// into an existing cvar, which is how CVAR_ARCHIVE reaches a UBO cvar at
	// all. Same "fetch a cvar someone else owns" pattern that
	// vkpt_blood_slot_capacity uses for cl_blood_max.
	cvar_pt_blood_splat_alpha = Cvar_Get("pt_blood_splat_alpha", "0.95", CVAR_ARCHIVE);

	return VK_SUCCESS;
}

/*
================
vkpt_blood_preallocate

THE FIRST-HIT HITCH (11-06). ensure_buffers sizes the shadow copy for the worst
case, cl_blood_max droplets at the full mesh, and it used to run on the session's
first droplet - the first monster shot after starting a map. At cl_blood_max 5120
and pt_blood_tess 2 that is 1.6M primitives: 270 MB of shadow zeroed in one frame,
then each backend's staging (Vulkan two frames' worth, D3D12 one per frame slot)
made on that same frame, several hundred ms in all.

vkpt_begin_registration calls this for both backends while the map loads, and
hands what it returns to vkpt_prep_backend->reserve_blood_staging, so all of it is
in place before play starts. Only with blood on (cl_blood_spheres and
pt_blood_spheres): a player who never turns it on still pays nothing. Turning it on
mid-map, or changing cl_blood_max / pt_blood_tess, still allocates on the next
droplet, as before.

Returns the capacity in primitives (0: blood off, nothing made).
================
*/
uint32_t vkpt_blood_preallocate(void)
{
	const cvar_t* client = Cvar_FindVar("cl_blood_spheres");
	if (!client || !client->integer || !cvar_pt_blood_spheres || !cvar_pt_blood_spheres->integer
		|| !cvar_pt_blood_tess)
		return 0;

	if (!templates_built)
		build_sphere_templates();

	if (!ensure_buffers())
		return 0;

	return blood.max_prims;
}

// vkpt_prep_backend->reserve_blood_staging on Vulkan: the staging vkpt_blood_update
// would otherwise make on the first droplet (vk_staging_ensure, an exact size)
void vkpt_blood_reserve_vk_staging(uint32_t capacity)
{
	vk_staging_ensure(capacity);
}

VkResult vkpt_blood_destroy(void)
{
	release_buffers();
	vk_staging_release();

	memset(&blood, 0, sizeof(blood));
	blood_blas_reset();
	templates_built = false;
	return VK_SUCCESS;
}

/*
================
vkpt_blood_prim_count

How many primitives this frame's droplets will need. Called before the buffers
are sized, so it must not depend on anything allocated in ensure_buffers().
================
*/
uint32_t vkpt_blood_prim_count(int num_spheres, int num_films)
{
	if (!cvar_pt_blood_spheres || !cvar_pt_blood_spheres->integer || (num_spheres <= 0 && num_films <= 0))
		return 0;

	// THE WHOLE SLOT SPACE, not this frame's droplet count.
	//
	// Droplets are written at slot * stride and slots are handed out for a
	// droplet's whole life, so the highest occupied slot has nothing to do with
	// how many droplets are currently alive - 125 droplets routinely reach slot
	// 180 once a few have died and left gaps. Reserving by droplet count let the
	// write run past the end of the reserved section.
	//
	// The reservation only claims ADDRESS SPACE in the instanced buffer; the BLAS
	// is still built over the range actually written, so over-reserving costs
	// nothing at trace time.
	// The larger of the two meshes, because which one each droplet gets is not
	// known until vkpt_blood_update picks it - and the close-up pool in front of
	// the normal slots (blood_layout).
	int film_count, ultra_count, ultra_big;
	uint32_t stride;

	// The SLOT SPACE, which is cl_blood_max - not this frame's droplet count, and
	// not MAX_BLOOD_SPHERES. Slots live as long as their droplet and are handed
	// out lowest-first, so the highest occupied slot has nothing to do with how
	// many are currently alive; and it must match what ensure_buffers allocated.
	return blood_layout(vkpt_blood_slot_capacity(), &film_count, &ultra_count, &ultra_big, &stride);
}

// Face count for a droplet at a given LOD. A landed droplet is a puddle mesh and
// a flying one is a sphere, and the two have different counts - so every place
// that sizes a buffer or advances an offset has to go through this.
static inline int faces_for(const blood_sphere_t* sphere, int level);

/*
================
vertex_wobble

How far in or out one template vertex is pushed, to break up the outline.

A SMOOTH FUNCTION OF THE ANGLE around the puddle, not a per-vertex random number.
The first version hashed each vertex independently, which is white noise: two
neighbouring rim vertices got completely unrelated offsets, so the outline
alternated in-out-in-out between every pair and came out as a starburst of
spikes rather than a splash. Correlating neighbours is the whole fix, and doing
it by angle gets it for free - a continuous function sampled at neighbouring
angles necessarily gives neighbouring answers.

Three harmonics, phase-shifted by the droplet's seed, give lobes at a few
different scales without landing on anything as regular as a flower. INTEGER
harmonics matter: they are periodic over a full turn, so the rim closes on itself
seamlessly instead of leaving a seam where angle wraps.

Harmonics above segments/2 cannot be represented by the ring's vertices and
alias straight back into the per-vertex jaggedness this exists to remove, so the
higher terms are dropped on the coarser LODs.

`amount` ARRIVES ALREADY CAPPED IN WORLD UNITS - see write_blood_geometry. It is
a fraction of the radius here, which is right for a droplet and wrong for a pool
that has absorbed twenty of them.

Keyed on position (via the angle), never on the face or vertex slot: the puddle's
vertices are shared between neighbouring faces, which store their own copies, so
anything face-local would give one physical vertex a different offset per face
and tear the mesh open along every edge.
================
*/
static inline float vertex_wobble(const vec3_t v, float seed, float amount, int segs, uint32_t rim)
{
	const float r = sqrtf(v[0] * v[0] + v[1] * v[1]);

	// The apex has no radius to push, and dividing into it would be meaningless.
	if (r < 1e-4f)
		return 1.f;

	const float theta = atan2f(v[1], v[0]);

	float n = sinf(theta * 2.f + seed * 1.7f) * 0.50f
	        + sinf(theta * 3.f + seed * 3.1f) * 0.33f;

	if (segs >= 16)
		n += sinf(theta * 5.f + seed * 5.3f) * 0.17f;
	if (segs >= 32)
		n += sinf(theta * 8.f + seed * 7.9f) * 0.10f;
	// A fifth term, affordable only now that the top LOD has 80 segments. It is
	// what keeps a POOL from reading as a smooth amoeba: the amplitude cap in
	// write_blood_geometry holds the lobes to a fixed size in world units as a
	// pool grows, and without a higher harmonic to take over, a large outline
	// capped that way is simply a circle.
	if (segs >= 64)
		n += sinf(theta * 13.f + seed * 11.3f) * 0.06f;

	float w = 1.f + n * amount;

	// CLIP TO WHERE THE SURFACE ACTUALLY REACHES, when it does not reach all the
	// way round.  A splat lands wherever its CENTRE lands, so one that landed near
	// the lip of a crate draws its far half over thin air.  The client measured
	// how far the floor goes in eight directions once, when the droplet parked
	// (cparticle_t::blood_rim), and this is the whole of what is done with it.
	//
	// The early-out is the point: BLOOD_RIM_FULL covers the great majority of
	// splats, and those generate byte-identical geometry to before this existed.
	if (rim != BLOOD_RIM_FULL)
	{
		// Read the two neighbouring samples and blend between them.  A FUNCTION
		// OF THE ANGLE ALONE, exactly like the wobble above and for exactly the
		// same reason: a puddle's vertices are shared between faces that each
		// store their own copy, so anything keyed on the face or the vertex slot
		// gives one physical vertex two different answers and tears the mesh open
		// along every edge.
		float u = theta * ((float)BLOOD_RIM_SAMPLES / (2.f * (float)M_PI));
		if (u < 0.f)
			u += (float)BLOOD_RIM_SAMPLES;

		const int i0 = (int)u & (BLOOD_RIM_SAMPLES - 1);
		const int i1 = (i0 + 1) & (BLOOD_RIM_SAMPLES - 1);

		const float r0 = (float)((rim >> (i0 * 4)) & 15u) * (1.f / BLOOD_RIM_SCALE);
		const float r1 = (float)((rim >> (i1 * 4)) & 15u) * (1.f / BLOOD_RIM_SCALE);

		// Smoothstepped, not linear: with only eight samples a linear blend
		// leaves a crease pointing at every one of them, and a clipped puddle
		// reads as an octagon rather than as blood stopping at an edge.
		float f = u - floorf(u);
		f = f * f * (3.f - 2.f * f);

		w = min(w, r0 + (r1 - r0) * f);
	}

	return w;
}

static inline bool is_splat_sphere(const blood_sphere_t* sphere)
{
	return (sphere->flatten < 0.999f) && (DotProduct(sphere->normal, sphere->normal) > 0.5f);
}

static inline int faces_for(const blood_sphere_t* sphere, int level)
{
	return is_splat_sphere(sphere) ? BLOOD_PUDDLE_FACES(level) : BLOOD_SPHERE_FACES(level);
}

/*
================
splat_basis

Builds the frame a stuck droplet is drawn in: two axes across the surface and
one along its normal, scaled so the droplet squashes against the wall.

`flatten` is the thickness along the normal.  The tangential axes grow by
1/sqrt(flatten) so the ellipsoid keeps roughly the volume the sphere had - a
splat should read as the same droplet spread out, not as a differently sized one.

`cross` IS AN INDEPENDENT EXTENT, NOT A FUNCTION OF `stretch`, AND THAT IS THE
WHOLE POINT OF IT.  Two earlier versions derived it and both were wrong in a way
that showed:

  - Dividing the cross axis by the full stretch makes the aspect ratio the SQUARE
    of it, so a pool that had run out to the 3.5 cap was drawn 12:1 and a
    hillside of them read as a fan of red needles rather than as blood.
  - Dividing it by the IMPACT's share only fixed that, but left the cross axis
    permanently at or below the droplet's own width.  A mark can then never be
    wider across its travel than along it, so a horizontal smear that starts
    running down a wall has no shape to become except a vertical smear of the
    same proportions - it PIVOTS instead of deforming.

The client now carries both extents and reshapes them (CL_BloodReshapeSlide), so
all this has to do is scale by them.  Both are in units of `spread`.

Seeding the cross product from whichever axis the normal is least aligned with
keeps it well conditioned; one component of a unit vector is always below
1/sqrt(3), so the seed is never closer than 54 degrees to the normal.
================
*/
static void splat_basis(const vec3_t normal, const vec3_t tangent, float radius,
                        float flatten, float stretch, float cross,
                        vec3_t out_t1, vec3_t out_t2, vec3_t out_n)
{
    // The first tangent axis is the direction the droplet was travelling, when
    // the impact had one. That is what makes the elongation below point the way
    // the blood was going instead of an arbitrary way; without it the stretch
    // would still be there but would face a direction picked by the seed axis,
    // and a wall of splats would all lean the same meaningless way.
    bool have_dir = DotProduct(tangent, tangent) > 0.5f;

    if (have_dir)
    {
        // Re-orthogonalise against the normal - the client projected it into the
        // surface plane, but the surface it finally stuck to may not be the one
        // that projection was made against.
        VectorMA(tangent, -DotProduct(tangent, normal), normal, out_t1);
        have_dir = VectorNormalize(out_t1) > 0.1f;
    }

    if (!have_dir)
    {
        vec3_t seed;
        if (fabsf(normal[0]) < 0.577f)      VectorSet(seed, 1.f, 0.f, 0.f);
        else if (fabsf(normal[1]) < 0.577f) VectorSet(seed, 0.f, 1.f, 0.f);
        else                                VectorSet(seed, 0.f, 0.f, 1.f);

        CrossProduct(seed, normal, out_t1);
        VectorNormalize(out_t1);
        stretch = 1.f;
        cross = 1.f;
    }

    CrossProduct(normal, out_t1, out_t2);
    VectorNormalize(out_t2);

    // Spread used to be radius / sqrt(flatten), conserving the droplet's volume.
    // That coupling is wrong for a puddle and was why thinner splats got wider:
    // real blood spreading out does not keep its depth proportional to its width,
	// and it left flatness unable to be tuned without also changing size. The
    // puddle's width is now its own quantity, and flatten only sets its height.
    const float spread = radius * max(0.1f, global_blood_splat_size);

    // Both extents come straight from the client. `stretch` is floored at the
    // droplet's own width because the tangent is the LONG axis by construction;
    // `cross` is free to sit either side of it, which is what lets a mark be
    // wider across its travel than along it while it is turning into a run.
    // Only the degenerate values are excluded.
    stretch = max(1.f, stretch);
    cross = max(0.05f, min(cross, 16.f));

    VectorScale(out_t1, spread * stretch, out_t1);
    VectorScale(out_t2, spread * cross, out_t2);
    VectorScale(normal, radius * flatten, out_n);
}

/*
================
choose_lods

Picks a tessellation level per droplet from its distance to the camera, and
returns the total primitive count that implies.

The triangle count only ever buys SILHOUETTE - the vertex normals are smooth at
every level - so the right measure is how many pixels the droplet covers, and
distance is the cheap proxy for that. A droplet at arm's length gets 320 faces
and a round outline; one across the room gets 20, where its outline is a couple
of pixels and no amount of geometry would show.

Called once per frame, before the buffers are sized, so its answer is also what
tells ensure_buffers how much room this frame needs.

Its levels are the DISTANCE BANDS, capped at what a droplet's own slot holds.
Airborne droplets whose outline the bands leave visibly faceted - measured in
pixels, so it takes the view's fov and height - are listed in `closeups` for
assign_closeups to step up, largest on screen first (CLOSE-UP REGIONS). The
total returned is the banded one.
================
*/
typedef struct {
	float px;       // projected radius, screen pixels
	int   s;        // index into this frame's spheres
	int   slot;     // its client slot - the tie-break, so the order is stable
	int   need;     // the lowest level whose outline is within pt_blood_lod_px
} blood_closeup_t;

static blood_closeup_t closeups[MAX_BLOOD_SPHERES];
static int             num_closeups;

static uint32_t choose_lods(const blood_sphere_t* spheres, int num_spheres, const vec3_t cam_pos,
                            float fov_y, int view_height)
{
	const int max_level = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));
	const int slot_top = slot_top_level();

	num_closeups = 0;

	// Pixels per unit of tan(angle) off the view axis, vertically. 0 turns the
	// close-up LOD off for the frame (no view size to measure against).
	const float lod_px = cvar_pt_blood_lod_px->value;
	float px_per_tan = 0.f;
	if (lod_px > 0.f && view_height > 0 && fov_y > 1.f && fov_y < 179.f)
		px_per_tan = 0.5f * (float)view_height / tanf(DEG2RAD(fov_y) * 0.5f);

	// Squared, to keep a square root out of the per-droplet loop.
	const float near_dist = max(1.f, cvar_pt_blood_lod_near->value);
	const float far_dist = max(near_dist + 1.f, cvar_pt_blood_lod_far->value);
	const float near_sq = near_dist * near_dist;
	const float far_sq = far_dist * far_dist;

	/*
	THE LEVEL IS A SCREEN SIZE, NOT A DISTANCE - and that is the whole of "a big
	pool should have more polygons".

	Distance alone answers the wrong question. Pooling merges by AREA and
	cl_blood_pool_max lets one splat reach twenty-odd droplet radii, so two marks
	at the same distance can differ by a factor of twenty in how much screen they
	cover while being handed the same 32-segment rim. Projected size is the
	quantity a tessellation level is actually for, and it costs one multiply more
	than the distance test did.

	The existing cvars keep their exact meaning. Both thresholds are re-expressed
	as the angular size of a REFERENCE droplet (cl_blood_sphere_radius) at that
	distance, so a droplet of that size still changes level at precisely
	pt_blood_lod_near and pt_blood_lod_far. A pool twenty times wider changes
	level twenty times further away, and a spray of small ones drops a level
	sooner than it used to - which is where the extra primitives come from.

	The comparison stays squared: `extent^2 > threshold^2 * dist^2` is the same
	test as `extent/dist > threshold` with no division and no square root.
	*/
	const float ref_radius = max(0.05f,
		Cvar_Get("cl_blood_sphere_radius", "1.2", CVAR_ARCHIVE)->value);
	const float ref_sq = ref_radius * ref_radius;

	const float near_thr_sq = ref_sq / near_sq;
	const float far_thr_sq = ref_sq / far_sq;

	/*
	HYSTERESIS, AND IT IS LOAD-BEARING RATHER THAN COSMETIC.

	The level is part of the geometry cache's key, so a splat parked on a
	threshold would rebuild every vertex every frame - the same failure the fade
	shrink and the slide turn both had, arriving by a third route. A pool GROWS
	through its threshold as it absorbs droplets, so this is not a rare case.

	Widening whichever band a droplet is already in by 25% of its metric costs
	one compare and makes oscillation impossible: leaving a band needs a quarter
	more (or less) projected size than entering it did. Squared, because
	everything here is.
	*/
	const float hyst_sq = 1.25f * 1.25f;

	uint32_t total = 0;

	for (int s = 0; s < num_spheres; s++)
	{
		vec3_t delta;
		VectorSubtract(spheres[s].origin, cam_pos, delta);
		const float dist_sq = DotProduct(delta, delta);

		/*
		The drawn half-width, which for a splat is not its radius. splat_basis
		scales the two in-plane axes by `spread * stretch` and `spread * cross`,
		spread being radius * cl_blood_splat_size - so a mark that has run down a
		wall is several times wider than the droplet that made it, and the
		segments are distributed by ANGLE, which puts the longest chords on the
		long axis. The larger extent is therefore the one that decides the
		faceting.
		*/
		const bool is_splat = is_splat_sphere(spheres + s);

		float extent = spheres[s].radius;

		if (is_splat)
		{
			const float spread = spheres[s].radius * max(0.1f, global_blood_splat_size);
			extent = spread * max(1.f, max(spheres[s].stretch, spheres[s].cross));
		}

		const float extent_sq = extent * extent;

		// The band the droplet is currently in is the one that gets widened.
		// An invalid cache entry means a brand new droplet, which has no band to
		// be kept in and simply takes the raw answer.
		const blood_cache_entry_t* prev = (spheres[s].slot >= 0 && spheres[s].slot < MAX_BLOOD_SPHERES)
			? blood_cache + region_of_slot(spheres[s].slot) : NULL;

		float near_thr = near_thr_sq;
		float far_thr = far_thr_sq;

		/*
		SPLATS ONLY, and that is not a shortcut - it is the pt_blood_lod_air
		interaction.

		The cached level is the level a droplet was BUILT at, and for an airborne
		one that is already one step below the raw answer (the bias a few lines
		down). Feeding it back in as "the band I am in" would then bias the
		threshold a second time, every frame, and settle the spray a level lower
		than asked for.

		Nothing is lost by leaving it out. Hysteresis exists to stop a droplet
		parked on a threshold from missing the geometry cache every frame, and an
		airborne droplet misses it every frame regardless - its origin moves.
		*/
		if (is_splat && prev && prev->valid)
		{
			if (prev->level >= max_level)
				near_thr /= hyst_sq;
			else if (prev->level == max_level - 1)
			{
				near_thr *= hyst_sq;
				far_thr /= hyst_sq;
			}
			else
				far_thr *= hyst_sq;
		}

		int level;
		if (extent_sq > near_thr * dist_sq)     level = max_level;
		else if (extent_sq > far_thr * dist_sq) level = max_level - 1;
		else                                    level = max_level - 2;

		/*
		A DROPLET IN FLIGHT DOES NOT NEED A SETTLED SPLAT'S TESSELLATION.

		These are the two populations, and they are not alike. On Matt's log the
		droplets counted as `moved` were, frame for frame, the AIRBORNE ones -
		136 airborne against 139 moved, then 963 against 915 - because a droplet
		in flight changes its origin every frame while a settled splat does not.
		So the flying half is what pays the translate cost AND what fills the
		upload: 963 droplets at 320 faces is ~50 MB of shadow writes and the same
		again over the bus, every frame of a firefight.

		Cutting one level takes an in-flight sphere from 320 faces to 80, and it
		buys back four times that cost on exactly the population that spikes.

		This does NOT contradict the note below about splats. That says a
		FLATTENED splat wants more tessellation than a sphere, because its
		silhouette is the rim of a disc where an icosphere has only about
		sqrt(faces) segments. An airborne droplet is not flattened - it is a
		round ball whose outline an 80-face icosphere already resolves, and it is
		moving fast enough that nobody is studying its edge. Splats are untouched
		by this and still get no reduction at all.

		Landing forces a rebuild anyway (sphere mesh to puddle mesh), so the
		level change that comes with it costs nothing extra.
		*/
		if (!is_splat)
			level -= max(0, cvar_pt_blood_lod_air->integer);

		// NOTE: splats deliberately get NO tessellation reduction, and there was
		// once a pt_blood_splat_tess cvar that applied one. Both halves of the
		// idea were wrong. It did not pay - measured across bias 0/1/2 on a
		// floorful of splats, 11.97 / 12.20 / 11.22 seconds per 128 frames, i.e.
		// no difference. And it cost more than it looked: flattening turns the
		// silhouette into the RIM of a disc, where an icosphere has only about
		// sqrt(faces) segments, so faceting that is invisible on a round droplet
		// is glaring on a squashed one. A splat wants MORE tessellation than a
		// sphere, not less.

		/*
		NOTHING PAST THE SLOT'S OWN LEVEL FROM THE BANDS.

		At tess 3 the bands ask for 1280, and a floor of 2048 splats at 1280 was
		the whole 90 -> 60 fps slide, so a splat tops out at tess 2's level and
		falls off from there - as it always has. A droplet in flight used to get
		1280 inside pt_blood_ultra_dist; now it gets it, or 5120, from the
		close-up LOD below, by size on screen rather than by distance.
		*/
		level = max(0, min(level, min(max_level, slot_top)));

		/*
		THE CLOSE-UP LOD: is this droplet's outline visibly faceted?

		Airborne only - a splat's rim is 80 segments at level 2, within a pixel
		of an ellipse at any size a pool reaches on screen. The projected radius
		is the true one, r / sqrt(d^2 - r^2) (the tangent of the sphere's angular
		radius), because the droplets this exists for are the ones close enough
		for r / d to undersell them. Off-axis stretch is ignored: it makes an
		edge-of-screen droplet up to ~1.4x larger, which only means the target
		is met a little less tightly there.

		No hysteresis, unlike the splat bands: a droplet in flight rebuilds every
		frame anyway, and a frozen one (pause, photo mode) asks the same question
		with the same numbers and gets the same answer.
		*/
		if (px_per_tan > 0.f && !is_splat && spheres[s].radius > 0.f
			&& spheres[s].slot >= 0 && spheres[s].slot < MAX_BLOOD_SPHERES)
		{
			const float r = spheres[s].radius;
			const float d2 = dist_sq - r * r;

			// Inside or touching the droplet: as large as it gets.
			const float px = d2 > 1e-4f * r * r ? r * px_per_tan / sqrtf(d2) : 1e6f;

			int need = 0;
			while (need < BLOOD_CLOSEUP_LEVEL && sphere_sag[need] * px > lod_px)
				need++;

			if (need > level)
			{
				blood_closeup_t* c = closeups + num_closeups++;
				c->px = px;
				c->s = s;
				c->slot = spheres[s].slot;
				c->need = need;
			}
		}

		sphere_lod[s] = (uint8_t)level;
		total += faces_for(spheres + s, level);
	}

	return total;
}

static int closeup_compare(const void* a, const void* b)
{
	const blood_closeup_t* x = a;
	const blood_closeup_t* y = b;

	if (x->px != y->px)
		return x->px > y->px ? -1 : 1;
	return x->slot - y->slot;
}

/*
================
assign_closeups

Serves choose_lods' close-up requests, LARGEST ON SCREEN FIRST, then maps the
ones that won a pool region onto actual regions (see CLOSE-UP REGIONS).

A request above the slot's own level wants a region: BLOOD_CLOSEUP_LEVEL takes a
big one, level 3 a mid one, and either takes whatever is left of the other size
once its own is gone (a big droplet in a mid region is drawn at level 3). With
the pool spent it falls back to the slot's top level. A step up that fits the
droplet's own slot is paid from pt_blood_lod_budget, and is skipped once the
budget cannot cover it.

Regions are kept by the droplet that already holds one of the right size, so a
droplet that stays big keeps its region and the cache key with it. Everything
else - a droplet that shrank, landed, died, or changed size class - lets go
before anything new is handed out, which is what guarantees the counts above
always find a free region.
================
*/
static void assign_closeups(void)
{
	blood.closeup_up = 0;
	blood.closeup_pool = 0;
	blood.closeup_short = 0;

	if (blood.ultra_count && !ultra_maps_ready)
		reset_ultra_maps();

	// The region size each client slot holds a claim to this frame: 0 none,
	// 1 mid (level 3), 2 big (BLOOD_CLOSEUP_LEVEL).
	static uint8_t want[MAX_BLOOD_SPHERES];
	memset(want, 0, sizeof(want));

	if (num_closeups > 1)
		qsort(closeups, num_closeups, sizeof(closeups[0]), closeup_compare);

	const int slot_top = slot_top_level();
	int big_left = blood.ultra_big;
	int mid_left = blood.ultra_count - blood.ultra_big;
	int budget = max(0, cvar_pt_blood_lod_budget->integer);

	for (int c = 0; c < num_closeups; c++)
	{
		const blood_closeup_t* cu = closeups + c;
		const int base = sphere_lod[cu->s];
		int level = cu->need;

		if (level > slot_top)
		{
			if (level >= BLOOD_CLOSEUP_LEVEL && big_left > 0)
			{
				big_left--;
				want[cu->slot] = 2;
				level = BLOOD_CLOSEUP_LEVEL;
			}
			else if (mid_left > 0)
			{
				mid_left--;
				want[cu->slot] = 1;
				level = BLOOD_CLOSEUP_LEVEL - 1;
			}
			else if (big_left > 0)
			{
				big_left--;
				want[cu->slot] = 2;
			}
			else
			{
				level = slot_top;
				blood.closeup_short++;
			}
		}

		if (!want[cu->slot])
		{
			const int extra = BLOOD_SPHERE_FACES(level) - BLOOD_SPHERE_FACES(base);
			if (extra <= 0)
				continue;
			if (extra > budget)
			{
				blood.closeup_short++;
				continue;
			}
			budget -= extra;
		}
		else
			blood.closeup_pool++;

		sphere_lod[cu->s] = (uint8_t)level;
		blood.closeup_up++;
	}

	if (!blood.ultra_count)
		return;

	// Let go of every region whose holder no longer has a claim to that size.
	for (int u = 0; u < blood.ultra_count; u++)
	{
		const int slot = slot_of_ultra[u];
		const int size = u < blood.ultra_big ? 2 : 1;

		if (slot >= 0 && want[slot] != size)
		{
			ultra_of_slot[slot] = -1;
			slot_of_ultra[u] = -1;
		}
	}

	// And hand the free ones to the claims that do not hold one yet.
	for (int c = 0; c < num_closeups; c++)
	{
		const int slot = closeups[c].slot;
		if (!want[slot] || ultra_of_slot[slot] >= 0)
			continue;

		const int lo = want[slot] == 2 ? 0 : blood.ultra_big;
		const int hi = want[slot] == 2 ? blood.ultra_big : blood.ultra_count;

		for (int u = lo; u < hi; u++)
		{
			if (slot_of_ultra[u] < 0)
			{
				slot_of_ultra[u] = (int16_t)slot;
				ultra_of_slot[slot] = (int16_t)u;
				break;
			}
		}

		// The counting above makes this unreachable; if it ever is reached, the
		// droplet must still fit where it is about to be written, or it would
		// silently not draw.
		if (ultra_of_slot[slot] < 0)
			sphere_lod[closeups[c].s] = (uint8_t)min((int)sphere_lod[closeups[c].s], slot_top);
	}
}

/*
================
write_blood_geometry

Expands the droplet list into instanced-buffer primitives.

THE UV PACKING. These primitives have no albedo texture and no unwrap, so uv0/uv1
are dead storage - and per-primitive storage is exactly what a per-droplet colour
needs, because VboPrimitive has no colour field of its own. get_material() in
path_tracer_rgen.h reads them back:

    uv0.x, uv0.y, uv1.x  ->  linear base colour
    uv1.y                ->  per-droplet ripple phase

uv2 is left at zero. If a future change ever gives blood a real texture, this is
the thing that has to move first.
================
*/
/*
The cluster a chunk's triangles light from - see write_film_geometry. When the
box around them, lifted off the surface, has all eight corners in one leaf, every
triangle is in that leaf too (leaves are convex): most chunks, and eight lookups
instead of one per triangle. Otherwise -2: each triangle looks its own up.

Per CHUNK since 11-14 (it was per tile): a chunk's box is a part of its tile's, so
it is in one leaf whenever the tile's was and often when the tile's was not -
fewer per-triangle lookups - and each chunk is now written by a job of its own.
*/
static int film_chunk_cluster(const blood_film_vert_t* verts, int faces, const vec3_t plane, bsp_t* bsp)
{
	if (!bsp)
		return -1;

	vec3_t lo, hi;
	ClearBounds(lo, hi);
	for (int i = 0; i < faces * 3; i++)
		AddPointToBounds(verts[i].pos, lo, hi);
	VectorMA(lo, 0.25f, plane, lo);
	VectorMA(hi, 0.25f, plane, hi);

	const mleaf_t* first = NULL;
	for (int i = 0; i < 8; i++)
	{
		const vec3_t corner = {
			(i & 1) ? hi[0] : lo[0],
			(i & 2) ? hi[1] : lo[1],
			(i & 4) ? hi[2] : lo[2],
		};
		const mleaf_t* leaf = BSP_PointLeaf(bsp->nodes, corner);
		if (!first)
			first = leaf;
		else if (leaf != first)
			return -2;
	}

	return first->cluster;
}

/*
THE FILM'S CHUNKS ON THE RENDERER'S WORKERS (jobs.c, 11-14). Matt's dip capture:
26 ms of render, 20 of it this function - ~250K triangles rewritten in one frame
while blood flowed (45.9 MB uploaded), one thread at ~80 ns a triangle. A tile's
version moves for its whole mesh whenever its flow steps (cl_blood_film_rate), so
every chunk of it used to be rewritten and sent again.

Now the main thread does the bookkeeping - which chunks a tile holds, which ones
the tile's own struct already matches - and every chunk left is a job: its own
region of the shadow, its own cache entry and dirty flag, so the jobs share
nothing they write. A job first checks whether the chunk ALREADY HOLDS these
triangles (same positions, depths and look): the part of a tile the flow did not
reach is byte for byte what it was, and is then neither rewritten nor uploaded.
*/
typedef struct {
	const blood_film_t*	film;
	uint32_t			prim_index;		// the chunk's region, in primitives
	int					region;
	int					part;			// which chunk of the tile
	int					first, count;	// its triangles in the tile's list
	vec3_t				color;
	uint32_t			plane_nrm;
	uint32_t			alpha_word;		// emissive_and_alpha
	uint32_t			instance;
	bool				kept;			// out: it held these triangles already
} film_job_t;

static film_job_t	film_jobs[MAX_BLOOD_FILM_CHUNKS];
static bsp_t*		film_jobs_bsp;

// Does the chunk's region hold exactly what film_chunk_job would write?
static bool film_chunk_holds(const film_job_t* j, const blood_cache_entry_t* cache)
{
	if (!cache->valid || !cache->is_film || cache->faces != (uint16_t)j->count)
		return false;

	const blood_film_vert_t* v = j->film->verts + j->first * 3;
	for (int f = 0; f < j->count; f++, v += 3)
	{
		const VboPrimitive* prim = blood.prim_shadow + j->prim_index + f;
		if (!VectorCompare(prim->pos0, v[0].pos) || !VectorCompare(prim->pos1, v[1].pos)
			|| !VectorCompare(prim->pos2, v[2].pos)
			|| prim->custom0[1] != ((uint32_t)floatToHalf(v[0].depth) << 16)
			|| prim->custom1[1] != ((uint32_t)floatToHalf(v[1].depth) << 16)
			|| prim->custom2[1] != ((uint32_t)floatToHalf(v[2].depth) << 16)
			|| prim->normals[0] != j->plane_nrm
			|| prim->emissive_and_alpha != j->alpha_word
			|| prim->instance != j->instance
			|| prim->uv0[0] != j->color[0] || prim->uv0[1] != j->color[1]
			|| prim->uv1[0] != j->color[2] || prim->uv1[1] != j->film->seed)
			return false;
	}
	return true;
}

static void film_chunk_job(void* ctx, int index)
{
	(void)ctx;
	film_job_t* j = film_jobs + index;
	const blood_film_t* film = j->film;
	blood_cache_entry_t* cache = blood_cache + j->region;
	const uint32_t prim_index = j->prim_index;
	const int count = j->count;
	const blood_film_vert_t* verts = film->verts + j->first * 3;

	if (film_chunk_holds(j, cache))
	{
		cache->film_part = (uint8_t)j->part;
		cache->film = *film;
		cache->prim_offset = prim_index;
		j->kept = true;
		return;
	}

	// What the chunk held before, for clearing what sticks out past
	// the new triangles: everything beyond it is zero already.
	const uint32_t held = cache->faces;

	cache->valid = true;
	cache->blanked = false;
	cache->shadow_zero = false;
	cache->is_film = true;
	cache->film_part = (uint8_t)j->part;
	cache->film = *film;
	cache->prim_offset = prim_index;
	cache->level = 0;
	cache->faces = (uint16_t)count;

	blood.dirty_slot[j->region] = true;

	// The light lists are per cluster, and a tile lies along walls and
	// across doorways, so one cluster for every tile would light some of
	// them from the wrong room - see film_chunk_cluster.
	const int chunk_cluster = film_chunk_cluster(verts, count, film->plane, film_jobs_bsp);

	for (int f = 0; f < count; f++)
	{
		const uint32_t wi = prim_index + (uint32_t)f;
		VboPrimitive* prim = blood.prim_shadow + wi;
		float (*pos)[3] = blood.pos_shadow[wi];
		const blood_film_vert_t* v = verts + f * 3;

		VectorCopy(v[0].pos, pos[0]);
		VectorCopy(v[1].pos, pos[1]);
		VectorCopy(v[2].pos, pos[2]);
		VectorCopy(pos[0], prim->pos0);
		VectorCopy(pos[1], prim->pos1);
		VectorCopy(pos[2], prim->pos2);

		// Lifted off the surface so the point is in the open leaf the
		// blood is actually in.
		int cluster = chunk_cluster;
		if (cluster == -2)
		{
			vec3_t c;
			VectorAdd(v[0].pos, v[1].pos, c);
			VectorAdd(c, v[2].pos, c);
			VectorScale(c, 1.f / 3.f, c);
			VectorMA(c, 0.25f, film->plane, c);
			cluster = BSP_PointLeaf(film_jobs_bsp->nodes, c)->cluster;
		}

		prim->material_id = MATERIAL_KIND_BLOOD;
		prim->cluster = cluster;
		prim->texture_flags = 0;

		// The plane's normal, not one carrying the depth: every pass that
		// treats the interpolated normal as the surface's - the sun's and
		// the lights' horizon tests, ReSTIR's reuse, the see-through ray's
		// second refraction, a secondary hit's facing flip - got a normal
		// lying ALONG the floor at the outline, which is where the white rim
		// was (2026-11-07). The depth rides in custom.w below instead.
		prim->normals[0] = j->plane_nrm;
		prim->normals[1] = j->plane_nrm;
		prim->normals[2] = j->plane_nrm;
		prim->tangents[0] = j->plane_nrm;
		prim->tangents[1] = j->plane_nrm;
		prim->tangents[2] = j->plane_nrm;

		prim->instance = j->instance;
		prim->emissive_and_alpha = j->alpha_word;

		prim->uv0[0] = j->color[0];
		prim->uv0[1] = j->color[1];
		prim->uv1[0] = j->color[2];
		prim->uv1[1] = film->seed;
		prim->uv2[0] = 2.f;     // landed, and a film: shaded from the plane
		prim->uv2[1] = 0.f;

		// custom = half4(motion.xyz, w) per vertex. A film never moves, and
		// w - unused by everything else - is its depth (Triangle.custom_w,
		// blood_film_depth in path_tracer_rgen.h).
		prim->custom0[0] = 0; prim->custom0[1] = (uint32_t)floatToHalf(v[0].depth) << 16;
		prim->custom1[0] = 0; prim->custom1[1] = (uint32_t)floatToHalf(v[1].depth) << 16;
		prim->custom2[0] = 0; prim->custom2[1] = (uint32_t)floatToHalf(v[2].depth) << 16;
	}

	// The rest of the chunk, as BLANK THE REST OF THE SLOT does for
	// droplets - only as far as the old triangles reached. Clearing the
	// whole region every time was ~300 KB per rewrite at the old size.
	if (held > (uint32_t)count)
	{
		const uint32_t tail = prim_index + (uint32_t)count;
		const uint32_t n = min(held, (uint32_t)BLOOD_FILM_CHUNK_TRIS) - (uint32_t)count;

		memset(blood.prim_shadow + tail, 0, sizeof(VboPrimitive) * n);
		memset(blood.pos_shadow + tail, 0, sizeof(prim_positions_t) * n);
	}
}

/*
================
write_film_geometry

The surface-blood tiles (cl_blood_film) into their chunks - see FILM REGIONS and
FILM CHUNKS. The client has already built every triangle; this is the same
per-primitive packing write_blood_geometry gives a LANDED splat, so the material,
the translucency and the shadow handling are a splat's:

  - normals: the plane's, with the depth per vertex in custom.w (uv2.x = 2);
  - tangents: the surface plane, the material's thickness reference;
  - no motion delta: a tile is world geometry and never translates.

A chunk is rewritten only when its tile's blood_film_t changes - in practice its
`version`.
================
*/
static void write_film_geometry(const blood_film_t* films, int num_films, bsp_t* bsp,
                                bool* slot_live, int* highest_slot)
{
	blood.film_tiles = 0;
	blood.film_tris = 0;
	blood.film_chunks_used = 0;
	blood.film_truncated = 0;

	if (!blood.film_count)
		return;

	static bool seen[MAX_BLOOD_FILM_TILES];
	memset(seen, 0, sizeof(seen));

	// Everything that is going goes first: the chunks of tiles that are gone, and
	// those a changed tile no longer fills. A changed tile KEEPS the chunks it still
	// needs (11-14; it used to give them all back and take the lowest free again):
	// in the same chunks, the part of it the flow did not reach is found already
	// there by film_chunk_holds, and is neither rewritten nor uploaded.
	for (int k = 0; k < num_films; k++)
	{
		const blood_film_t* film = films + k;

		if (film->slot < 0 || film->slot >= MAX_BLOOD_FILM_TILES || film->num_tris <= 0 || !film->verts)
			continue;

		seen[film->slot] = true;
		if (film_num_chunks[film->slot] && memcmp(&film_held[film->slot], film, sizeof(*film)) != 0)
			release_film_chunks_from(film->slot,
				BLOOD_FILM_CHUNKS(min(film->num_tris, BLOOD_FILM_TILE_TRIS)));
	}

	for (int slot = 0; slot < MAX_BLOOD_FILM_TILES; slot++)
	{
		if (!seen[slot] && film_num_chunks[slot])
			release_film_chunks(slot);
	}

	int num_jobs = 0;

	for (int k = 0; k < num_films; k++)
	{
		const blood_film_t* film = films + k;

		if (film->slot < 0 || film->slot >= MAX_BLOOD_FILM_TILES || film->num_tris <= 0 || !film->verts)
			continue;

		const int slot = film->slot;
		const int faces = min(film->num_tris, BLOOD_FILM_TILE_TRIS);
		const int need = BLOOD_FILM_CHUNKS(faces);

		// Short of chunks (the client keeps within cl_blood_film_tris, so only
		// for a frame), a tile draws what fits and tries for the rest next frame.
		while (film_num_chunks[slot] < need)
		{
			const int c = take_film_chunk();
			if (c < 0)
				break;
			film_chunk_of[slot][film_num_chunks[slot]++] = (int16_t)c;
		}
		film_held[slot] = *film;

		const int have = film_num_chunks[slot];
		if (have < need || film->num_tris > faces)
			blood.film_truncated++;
		if (!have)
			continue;

		const int drawn = min(faces, have * BLOOD_FILM_CHUNK_TRIS);
		blood.film_tiles++;
		blood.film_tris += drawn;
		blood.film_chunks_used += have;

		// what every chunk of the tile takes from it
		vec3_t color;
		cast_u32_to_f32_color(film->color, &film->rgba, color, 1.0f);

		const uint32_t plane_nrm = encode_normal(film->plane);
		const float alpha = max(0.05f, min(cvar_pt_blood_splat_alpha->value, 1.f)) *
		                    max(0.f, min(film->alpha, 1.f));
		const uint32_t alpha_half = (uint32_t)floatToHalf(alpha);

		for (int part = 0; part < have; part++)
		{
			const int region = film_chunk_of[slot][part];  // film chunk c is region c
			const uint32_t prim_index = region_offset(region);
			const int first = part * BLOOD_FILM_CHUNK_TRIS;
			const int count = min(BLOOD_FILM_CHUNK_TRIS, drawn - first);

			slot_live[region] = true;
			if (region > *highest_slot)
				*highest_slot = region;

			blood_cache_entry_t* cache = blood_cache + region;

			if (cache->valid && cache->is_film && cache->film_part == part &&
			    memcmp(&cache->film, film, sizeof(*film)) == 0)
			{
				blood.cache_hits++;
				continue;
			}

			// the rest is the chunk's job (film_chunk_job)
			if (num_jobs >= MAX_BLOOD_FILM_CHUNKS)
				continue;
			film_job_t* j = film_jobs + num_jobs++;
			j->film = film;
			j->prim_index = prim_index;
			j->region = region;
			j->part = part;
			j->first = first;
			j->count = count;
			VectorCopy(color, j->color);
			j->plane_nrm = plane_nrm;
			j->alpha_word = 0x3c00u | (alpha_half << 16);
			j->instance = (uint32_t)blood.model_instance_index;
			j->kept = false;
		}
	}

	film_jobs_bsp = bsp;
	vkpt_parallel_for(num_jobs, film_chunk_job, NULL);
	for (int i = 0; i < num_jobs; i++)
		if (film_jobs[i].kept)
			blood.cache_hits++;
}

/*
THE DROPLETS ON THE RENDERER'S WORKERS (jobs.c, 11-14) - the film's chunks'
treatment (film_chunk_job) for the spray and the runs: write_blood_geometry does
the bookkeeping - each droplet's region, the slots in use, the droplets already in
the shadow exactly as they are - and every droplet left, moved or made again, is a
job writing its own region, cache entry and dirty flag. Matt's dip captures had
these at 0.6-2 ms of the frame with ~1000 droplets in flight.
*/
typedef struct {
	const blood_sphere_t*	sphere;
	int						region;
	uint32_t				prim_index, cap;
	int						level, faces;
	bool					is_splat;
	uint8_t					result;		// out: 0 made again, 2 moved
} drop_job_t;

static drop_job_t	drop_jobs[MAX_BLOOD_SPHERES];
static bsp_t*		drop_jobs_bsp;

static void drop_job(void* ctx, int index)
{
	(void)ctx;
	drop_job_t* j = drop_jobs + index;
	const blood_sphere_t* sphere = j->sphere;
	const int region = j->region;
	const uint32_t prim_index = j->prim_index;
	const uint32_t cap = j->cap;
	const int level = j->level;
	const int faces = j->faces;
	const bool is_splat = j->is_splat;
	bsp_t* bsp = drop_jobs_bsp;
	blood_cache_entry_t* cache = blood_cache + region;

	const blood_face_t* tmpl = is_splat
		? puddle_template + puddle_lod_offset[level]
		: sphere_template + lod_offset[level];

	const uint32_t (*tmpl_normals)[3] = sphere_normals + lod_offset[level];
	const uint32_t (*tmpl_tangents)[3] = is_splat
		? puddle_tangents + puddle_lod_offset[level]
		: sphere_tangents + lod_offset[level];

	/*
	THE SAME MESH, MOVED - the case that costs everything while blood is
	running down a wall.

	A droplet that is sliding changes its ORIGIN and nothing else: same
	normal, same radius, same flatten, same stretch, same seed, same LOD. Its
	geometry is therefore last frame's geometry translated, and every
	expensive thing below is already correct in the shadow buffer. Rebuilding
	it recomputed, per vertex per frame: vertex_wobble (three harmonics, so
	trig), the inverse-scale normal transform, a VectorNormalize and an
	encode_normal - for a mesh identical apart from where it sits.

	That is what "the splats cost a lot while they are dripping" was. A
	SETTLED splat has always been free, because it hits the exact-match test
	above; a MOVING one paid full price every frame, and moving is precisely
	when there are a lot of them at once.

	The test is a memcmp of the whole struct with the two position fields
	masked out, rather than a field-by-field compare, for the same reason the
	exact test above is a memcmp: a field added to blood_sphere_t later is
	then automatically part of the check instead of silently escaping it.

	Positions accumulate here rather than being rebuilt from the template, so
	they drift by up to half an ulp per frame. At world coordinates of a few
	thousand that is ~1e-4 units per frame against a droplet radius of 1.2,
	and a droplet parks within a second or two - it cannot reach anything
	visible.
	*/
	if (cache->valid && cache->level == level)
	{
		blood_sphere_t probe = *sphere;
		VectorCopy(cache->sphere.origin, probe.origin);
		VectorCopy(cache->sphere.prev_origin, probe.prev_origin);

		if (memcmp(&cache->sphere, &probe, sizeof(blood_sphere_t)) == 0)
		{
			vec3_t move;
			VectorSubtract(sphere->origin, cache->sphere.origin, move);

			// The motion vector and the cluster are the only two things that
			// genuinely change with position, and both are per DROPLET, not
			// per vertex.
			vec3_t mv_delta;
			VectorSubtract(sphere->prev_origin, sphere->origin, mv_delta);
			const uint32_t mv_xy = (uint32_t)floatToHalf(mv_delta[0])
			                     | ((uint32_t)floatToHalf(mv_delta[1]) << 16);
			const uint32_t mv_zw = (uint32_t)floatToHalf(mv_delta[2]);
			const int moved_cluster = bsp
				? BSP_PointLeaf(bsp->nodes, sphere->origin)->cluster : -1;

			for (int f = 0; f < faces; f++)
			{
				const uint32_t wi = prim_index + (uint32_t)f;
				VboPrimitive* prim = blood.prim_shadow + wi;
				float (*pos)[3] = blood.pos_shadow[wi];

				for (int i = 0; i < 3; i++)
					VectorAdd(pos[i], move, pos[i]);

				VectorCopy(pos[0], prim->pos0);
				VectorCopy(pos[1], prim->pos1);
				VectorCopy(pos[2], prim->pos2);

				prim->cluster = moved_cluster;

				prim->custom0[0] = mv_xy; prim->custom0[1] = mv_zw;
				prim->custom1[0] = mv_xy; prim->custom1[1] = mv_zw;
				prim->custom2[0] = mv_xy; prim->custom2[1] = mv_zw;
			}

			cache->sphere = *sphere;
			cache->prim_offset = prim_index;
			cache->faces = (uint16_t)faces;

			if (blood.gpu_move)
			{
				// the device moves its own copy (MOVED ON THE GPU); `first` is
				// the section's, which vkpt_blood_prepare knows
				BloodMove* m = region_move + region;
				VectorCopy(move, m->move);
				m->first = 0;
				m->count = (uint32_t)faces;
				m->cluster = moved_cluster;
				m->mv_xy = mv_xy;
				m->mv_z = mv_zw;
				blood.moved_slot[region] = true;
			}
			else
				blood.dirty_slot[region] = true;

			j->result = 2;
			return;
		}
	}

	// What the region held before, for clearing what sticks out past the new
	// mesh: everything beyond it is zero already (BLANK THE REST OF THE SLOT).
	const uint32_t held = cache->faces;

	cache->valid = true;
	cache->blanked = false;
	cache->shadow_zero = false;
	cache->is_film = false;
	cache->prim_offset = prim_index;
	cache->level = level;
	cache->sphere = *sphere;
	cache->faces = (uint16_t)faces;

	blood.dirty_slot[region] = true;

	vec3_t color;
	cast_u32_to_f32_color(sphere->color, &sphere->rgba, color, 1.0f);

	// The whole droplet translates rigidly, so one delta covers all of its
	// vertices. Halves carry ~3 decimal digits, ample for the few units a
	// droplet moves in a frame.
	vec3_t delta;
	VectorSubtract(sphere->prev_origin, sphere->origin, delta);

	uint32_t custom_xy = (uint32_t)floatToHalf(delta[0]) | ((uint32_t)floatToHalf(delta[1]) << 16);
	uint32_t custom_zw = (uint32_t)floatToHalf(delta[2]);

	int cluster = bsp ? BSP_PointLeaf(bsp->nodes, sphere->origin)->cluster : -1;

	// A droplet in flight is a plain sphere and takes the cheap path: a
	// uniform scale changes no direction, so the template's precomputed
	// encoded normals stand. A landed one is a puddle mesh under a
	// non-uniform scale, which needs a basis AND needs its normals
	// transformed by the INVERSE scale - flattening a surface steepens its
	// normals rather than flattening them.
	vec3_t ax_t1, ax_t2, ax_n;
	vec3_t inv_t1, inv_t2, inv_n;

	if (is_splat)
	{
		splat_basis(sphere->normal, sphere->tangent, sphere->radius,
			sphere->flatten, sphere->stretch, sphere->cross,
			ax_t1, ax_t2, ax_n);

		// Inverse-transpose of the scale, in the same frame. The basis is
		// orthonormal before scaling, so this is just the reciprocal of each
		// axis length applied to the unit axis - no matrix inverse needed.
		// The two tangents now have DIFFERENT lengths once a splat is
		// stretched, so they need separate reciprocals; sharing one was
		// correct only while the splat was circular.
		const float len_t1 = VectorLength(ax_t1);
		const float len_t2 = VectorLength(ax_t2);
		const float len_n = VectorLength(ax_n);
		VectorScale(ax_t1, 1.f / (len_t1 * len_t1), inv_t1);
		VectorScale(ax_t2, 1.f / (len_t2 * len_t2), inv_t2);
		VectorScale(ax_n,  1.f / (len_n * len_n), inv_n);

		// A CLOUD DISSOLVING INTO WATER is measured, not shaded, by its
		// normals: blood_hit_alpha fades it by the same dot() the rim
		// darkening reads. Steepened like a puddle's, that dot saturates in
		// the outermost band and the cloud kept a hard outline as it faded.
		// Scaled as if the dome were as tall as it is wide, the template's
		// own hemisphere normals come through: dot falls 1 -> 0.7 -> 0 from
		// the centre through the middle ring to the rim, a fade across the
		// whole cloud. The cloud is shaded from its plane, so this changes
		// nothing about how it is lit.
		if (sphere->wobble > 0.f)
			VectorScale(ax_n, 1.f / (len_n * sqrtf(len_t1 * len_t2)), inv_n);
	}

	/*
	THE SURFACE PLANE NORMAL, CARRIED IN THE TANGENT SLOT.

	get_material() reads it to work out how THICK the puddle is at a shading
	point, which is what drives the rim darkening: on a dome whose normals
	have been steepened by the flatten, dot(shading normal, plane normal)
	runs from 0 at the rim - where the surface faces sideways and the film is
	a feather edge - to 1 over the body of the pool. No new vertex channel
	and no interpolation of our own: it is one encode_normal per droplet, and
	load_triangle already decodes tangents for every hit.

	Safe because a blood surface never reaches the tangent-space code.
	get_material intercepts is_blood() ahead of every texture fetch and
	returns before the normal-map block, and the only other reader of
	triangle.tangents is get_water_normal, which blood is not. The puddle
	template's own tangents are left built and simply unused on this path.
	*/
	const uint32_t splat_plane_nrm = is_splat ? encode_normal(sphere->normal) : 0;

	// Landed splats can be translucent; droplets in flight never are.
	// A splat dissolving into water carries its own fade on top of that.
	const float splat_alpha = is_splat
		? max(0.05f, min(cvar_pt_blood_splat_alpha->value, 1.f)) *
		  max(0.f, min(sphere->alpha, 1.f)) : 1.f;
	const uint32_t splat_alpha_half = (uint32_t)floatToHalf(splat_alpha);

	// A puddle's z runs from 0 at its base to 1 at its apex, where a sphere's
	// spans the centre, so the two want different anchors: the puddle sits ON
	// the contact point, sunk just far enough to bury its rim.
	float wobble = is_splat ? max(0.f, min(cvar_pt_blood_wobble->value, 0.9f)) : 0.f;

	/*
	CAP THE LOBES IN WORLD UNITS.

	pt_blood_wobble is a FRACTION of the outline's radius, which is the
	correct unit for a droplet and badly wrong for a pool. At
	cl_blood_pool_max 24 the drawn half-width reaches ~36 units, so the
	shipped 0.3 pushes the rim in and out by eleven of them - three lobes
	that size is an amoeba, and no amount of extra tessellation fixes it
	because the shape itself is wrong.

	Capping the amplitude rather than the fraction keeps a lone droplet
	exactly as irregular as it was (it never reaches the cap) and holds a
	pool's lobes to the size of a splash while it grows. The 13th harmonic
	added to vertex_wobble at 64 segments is the other half of this: a large
	outline with small lobes needs MORE of them, or it reads as a circle.

	Cache-safe by construction - every input is either a cvar or a field of
	blood_sphere_t that is already quantized, so this cannot make a parked
	splat's geometry change from one frame to the next.
	*/
	if (is_splat && sphere->wobble > 0.f)
	{
		// Dissolving into water - see blood_sphere_t::wobble. Uncapped on
		// purpose: the cloud grows several times its landed width and is
		// meant to stay ragged the whole way, not round off into a disc.
		wobble = min(sphere->wobble, 0.9f);
	}
	else if (is_splat && cvar_pt_blood_wobble_max->value > 0.f)
	{
		const float spread = sphere->radius * max(0.1f, global_blood_splat_size);
		const float half_width = spread * max(1.f, max(sphere->stretch, sphere->cross));

		if (half_width > 1e-3f)
			wobble = min(wobble, cvar_pt_blood_wobble_max->value / half_width);
	}

	vec3_t mesh_origin;
	VectorCopy(sphere->origin, mesh_origin);
	if (is_splat)
		VectorMA(mesh_origin, -cvar_pt_blood_puddle_sink->value, ax_n, mesh_origin);

	// THE TRAIL HANGS BEHIND, NOT AROUND.
	//
	// splat_basis scales the ellipse symmetrically about the origin, so the
	// length a pool picks up while sliding would grow forward as much as back -
	// the leading edge running out ahead of the droplet, which reads as a
	// stretched blob being carried rather than a smear being drawn.
	//
	// ax_t1 is spread*stretch long, so scaling it by trail/stretch gives a
	// vector of exactly spread*trail: the length that was ADDED. Shifting back
	// by it leaves the leading edge where a round splat's was and puts every
	// bit of the growth behind. The impact smear is not included in `trail`
	// and stays centred on the point of contact.
	if (is_splat && sphere->stretch_trail > 0.f)
	{
		const float s = max(1.f, sphere->stretch);
		VectorMA(mesh_origin, -sphere->stretch_trail / s, ax_t1, mesh_origin);
	}

	// ONCE PER UNIQUE VERTEX, NOT ONCE PER CORNER - see the block comment on
	// puddle_vert_pos. The face loop below is then a pure gather, and the
	// bytes it produces are identical to transforming every corner.
	//
	// Only the splat path needs this. A droplet in flight is a uniformly
	// scaled sphere: its position is one VectorMA and its normals come
	// straight from the template, so a corner costs about what a table lookup
	// would.
	// On the job's own stack (86 KB): the jobs run at once.
	vec3_t   vert_pos[BLOOD_PUDDLE_MAX_VERTS];
	uint32_t vert_nrm[BLOOD_PUDDLE_MAX_VERTS];

	// Splats only: the puddle tables stop at BLOOD_SPHERE_MAX_SUBDIV, and a
	// sphere can be at BLOOD_CLOSEUP_LEVEL.
	const uint16_t (*face_vid)[3] = is_splat ? puddle_face_vid + puddle_lod_offset[level] : NULL;

	if (is_splat)
	{
		const int vbase = puddle_vert_offset[level];
		const int vcount = puddle_vert_count[level];

		for (int v = 0; v < vcount; v++)
		{
			const float* t = puddle_vert_pos[vbase + v];

			// Push the vertex in or out radially. Only the two in-plane
			// axes are scaled, so the puddle stays exactly as tall and
			// exactly as flat on the floor - it is the OUTLINE that goes
			// irregular, which is the part that reads as a splash. The
			// apex has zero radius and so cannot move, which keeps the
			// dome centred over its own base.
			const float w = vertex_wobble(t, sphere->seed, wobble,
				puddle_segments[level], sphere->rim_support);

			for (int a = 0; a < 3; a++)
				vert_pos[v][a] = mesh_origin[a]
				               + ax_t1[a] * t[0] * w + ax_t2[a] * t[1] * w + ax_n[a] * t[2];

			const float* tn = puddle_vert_nrm[vbase + v];
			vec3_t nv;

			// A cloud is seen from under the water as well, through its flat
			// base - whose normals all point straight down and so measured it
			// as solid to the rim: a hard disc from below. Give the base the
			// dome's profile mirrored, so it fades out from the centre too.
			vec3_t cloud_n;
			if (sphere->wobble > 0.f && tn[2] < -0.5f)
			{
				VectorSet(cloud_n, t[0], t[1], -sqrtf(max(0.f, 1.f - t[0] * t[0] - t[1] * t[1])));
				tn = cloud_n;
			}

			for (int a = 0; a < 3; a++)
				nv[a] = inv_t1[a] * tn[0] + inv_t2[a] * tn[1] + inv_n[a] * tn[2];
			VectorNormalize(nv);
			vert_nrm[v] = encode_normal(nv);
		}
	}

	uint32_t write_index = prim_index;

	for (int f = 0; f < faces; f++)
	{
		VboPrimitive* prim = blood.prim_shadow + write_index;
		float (*pos)[3] = blood.pos_shadow[write_index];

		for (int i = 0; i < 3; i++)
		{
			if (is_splat)
				VectorCopy(vert_pos[face_vid[f][i]], pos[i]);
			else
				VectorMA(sphere->origin, sphere->radius, tmpl[f].pos[i], pos[i]);
		}

		VectorCopy(pos[0], prim->pos0);
		VectorCopy(pos[1], prim->pos1);
		VectorCopy(pos[2], prim->pos2);

		prim->material_id = MATERIAL_KIND_BLOOD;
		prim->cluster = cluster;
		prim->texture_flags = 0;

		if (is_splat)
		{
			prim->normals[0] = vert_nrm[face_vid[f][0]];
			prim->normals[1] = vert_nrm[face_vid[f][1]];
			prim->normals[2] = vert_nrm[face_vid[f][2]];
		}
		else
		{
			prim->normals[0] = tmpl_normals[f][0];
			prim->normals[1] = tmpl_normals[f][1];
			prim->normals[2] = tmpl_normals[f][2];
		}

		if (is_splat)
		{
			prim->tangents[0] = splat_plane_nrm;
			prim->tangents[1] = splat_plane_nrm;
			prim->tangents[2] = splat_plane_nrm;
		}
		else
		{
			prim->tangents[0] = tmpl_tangents[f][0];
			prim->tangents[1] = tmpl_tangents[f][1];
			prim->tangents[2] = tmpl_tangents[f][2];
		}

		// One ModelInstance backs the whole blood section. It exists only so
		// load_and_transform_triangle() can subtract render_prim_offset and
		// get a primitive index; nothing about the droplet is transformed by
		// it, because the positions above are already in world space.
		prim->instance = (uint32_t)blood.model_instance_index;

		// Two halves: emissive_factor in the LOW 16 bits, alpha in the
		// HIGH ones - unpackHalf2x16 puts .x in the low half and
		// load_triangle reads alpha from .y. 0x3c00 is 1.0.
		//
		// Only a LANDED splat is ever less than solid. A droplet in flight
		// is a body of liquid and reads correctly opaque, and making the
		// spray translucent would mean tracing through sixty of them.
		prim->emissive_and_alpha = 0x3c00u | (splat_alpha_half << 16);

		prim->uv0[0] = color[0];
		prim->uv0[1] = color[1];
		prim->uv1[0] = color[2];
		prim->uv1[1] = sphere->seed;
		// uv2.x: "this droplet has landed", read by get_material to freeze the
		// animated surface ripple (2 is a film tile - write_film_geometry).
		// uv2.y: 1 = dissolving into water, which blood_hit_alpha fades out
		// to its rim and get_material draws without a dark edge or highlight.
		prim->uv2[0] = is_splat ? 1.f : 0.f;
		prim->uv2[1] = (is_splat && sphere->wobble > 0.f) ? 1.f : 0.f;

		prim->custom0[0] = custom_xy;
		prim->custom0[1] = custom_zw;
		prim->custom1[0] = custom_xy;
		prim->custom1[1] = custom_zw;
		prim->custom2[0] = custom_xy;
		prim->custom2[1] = custom_zw;

		write_index++;
	}

	// BLANK THE REST OF THE SLOT.
	//
	// A slot is `stride` primitives wide, sized for the worst case, but a
	// droplet only writes as many as its own mesh needs - and the two differ
	// constantly. A droplet that lands switches from a 320-face sphere to a
	// 256-face puddle, and without this the sphere's last 64 faces stay in
	// the slot and keep drawing: thin slivers of the old droplet hanging over
	// the puddle it just became. A slot reused by a new droplet at a lower
	// LOD leaves the same kind of debris.
	//
	// Only runs when the droplet was regenerated, so the cost sits with the
	// work that was already happening rather than with every frame.
	//
	// ONLY AS FAR AS THE OLD MESH REACHED (11-15), as film_chunk_job does: past
	// cache->faces a region's shadow is always zero - every rewrite clears what it
	// no longer covers, a dead region is zeroed whole, and a new shadow is made
	// zeroed. Blanking to the cap was most of a burst's cost: Matt's captures had
	// ~1000 new droplets a frame at 80 faces in 320-face slots, so each wrote
	// 13 KB of mesh and memset 39 KB of zeros over zeros - 40 ms of work over the
	// renderer's threads, 5 ms of the frame.
	const uint32_t blank_to = min(held, cap);
	if ((uint32_t)faces < blank_to)
	{
		const uint32_t tail = prim_index + (uint32_t)faces;
		const uint32_t count = blank_to - (uint32_t)faces;

		memset(blood.prim_shadow + tail, 0, sizeof(VboPrimitive) * count);
		memset(blood.pos_shadow + tail, 0, sizeof(prim_positions_t) * count);
	}

	// The stale primitives past the new mesh are cleared in the shadow copy
	// above, and blood_cache_entry_t::dev_span is what gets them cleared on
	// the GPU too: the upload covers max(faces, dev_span), so a droplet whose
	// mesh shrank sends the difference exactly once.
}

// this frame's droplet jobs (vkpt_jobs_last), for the slow-frame line: the first blood
// of a map took 14 ms for 637 droplets, about 3x a later frame's rate per droplet
static vkpt_jobs_stats_t blood_drop_stats;

static uint32_t write_blood_geometry(const blood_sphere_t* spheres, int num_spheres,
                                     const blood_film_t* films, int num_films, bsp_t* bsp)
{
	blood.sphere_count = 0;
	blood.splat_count = 0;
	blood.cache_hits = 0;
	blood.cache_moves = 0;

	// Which REGIONS this frame's droplets occupy (see ULTRA REGIONS). Anything
	// else inside the used range has to be blanked, or a dead droplet's
	// leftovers stay in the acceleration structure.
	static bool slot_live[BLOOD_MAX_REGIONS];
	memset(slot_live, 0, sizeof(slot_live));

	int highest_slot = -1;
	int num_drop_jobs = 0;

	// Which slots were actually rewritten this frame. Uploading the whole used
	// range every frame meant pushing 25 MB over PCIe to change a few slots -
	// with stable slots almost everything is untouched, so only the slots that
	// changed need to travel.
	memset(blood.dirty_slot, 0, sizeof(blood.dirty_slot));
	memset(blood.moved_slot, 0, sizeof(blood.moved_slot));
	blood.gpu_move = cvar_pt_blood_gpu_move && cvar_pt_blood_gpu_move->integer;

	/*
	A SHAPE CVAR CHANGED, SO EVERY CACHED SPLAT IS NOW WRONG.

	The geometry cache keys on blood_sphere_t and nothing else, which is exactly
	right for droplets and blind to the cvars that shape them. A settled floor
	hits the exact-match test every frame, so dragging the irregularity or the
	sink slider used to change nothing at all until the blood moved - the new
	value reached only droplets that happened to be rebuilding. That reads as a
	dead slider, and it is the first thing anyone does with a new knob.

	Clearing `valid` alone is the correct invalidation, and deliberately not the
	memset ensure_buffers does: the bytes on the device are at the right
	ADDRESSES and only their content is stale, so dev_span and faces must
	survive or a droplet whose mesh shrinks will not blank its tail on the GPU.

	Costs one full regeneration on the frame the slider moves, which is the
	1.7 ms this cache was built to remove - paid once, while tuning.
	*/
	{
		const float look_now[5] = {
			cvar_pt_blood_wobble->value,
			cvar_pt_blood_wobble_max->value,
			cvar_pt_blood_puddle_sink->value,
			global_blood_splat_size,
			cvar_pt_blood_splat_alpha->value,
		};

		static float look_was[5];
		static bool  look_seen = false;

		if (!look_seen || memcmp(look_now, look_was, sizeof(look_now)) != 0)
		{
			memcpy(look_was, look_now, sizeof(look_now));
			look_seen = true;

			for (int i = 0; i < BLOOD_MAX_REGIONS; i++)
				blood_cache[i].valid = false;
		}
	}

	// Surface blood first: its regions are the lowest - see FILM REGIONS.
	CPUPROF_DETAIL_BEGIN(FILM_MESH);
	write_film_geometry(films, num_films, bsp, slot_live, &highest_slot);
	CPUPROF_DETAIL_END(FILM_MESH);

	for (int s = 0; s < num_spheres; s++)
	{
		const blood_sphere_t* sphere = spheres + s;

		if (sphere->radius <= 0.f)
			continue;

		if (sphere->slot < 0 || sphere->slot >= MAX_BLOOD_SPHERES)
			continue;

		// A droplet's geometry always goes in its own region - its slot's, or the
		// ultra region it holds - so it stays put however the array shifts.
		const int region = region_of_slot(sphere->slot);
		const uint32_t prim_index = region_offset(region);
		const uint32_t cap = region_cap(region);

		// two droplets naming one slot would be two jobs writing one region:
		// the first keeps it (the client never hands out a slot twice)
		if (slot_live[region])
			continue;
		slot_live[region] = true;
		if (region > highest_slot)
			highest_slot = region;

		const bool is_splat = is_splat_sphere(sphere);

		const int level = sphere_lod[s];
		const int faces = faces_for(sphere, level);

		if (prim_index + (uint32_t)faces > blood.max_prims || (uint32_t)faces > cap)
			continue;

		blood.sphere_count++;
		if (is_splat)
			blood.splat_count++;

		// Already in the shadow buffer, exactly as it needs to be? Then there is
		// nothing to do at all - see the comment on blood_cache_entry_t. With a
		// stable slot this is the fast path for any droplet that did not change
		// since last frame, not merely for a floor that has finished settling.
		blood_cache_entry_t* cache = blood_cache + region;

		if (cache->valid
			&& cache->level == level
			&& memcmp(&cache->sphere, sphere, sizeof(blood_sphere_t)) == 0)
		{
			blood.cache_hits++;
			continue;
		}

		// the rest is the droplet's job (drop_job): moved, or made again
		drop_job_t* j = drop_jobs + num_drop_jobs++;
		j->sphere = sphere;
		j->region = region;
		j->prim_index = prim_index;
		j->cap = cap;
		j->level = level;
		j->faces = faces;
		j->is_splat = is_splat;
		j->result = 0;
	}

	drop_jobs_bsp = bsp;
	vkpt_parallel_for(num_drop_jobs, drop_job, NULL);
	if (num_drop_jobs > 0)
		blood_drop_stats = vkpt_jobs_last;
	else
		memset(&blood_drop_stats, 0, sizeof(blood_drop_stats));
	for (int i = 0; i < num_drop_jobs; i++)
		if (drop_jobs[i].result == 2)
			blood.cache_moves++;

	// Blank any slot inside the used range that no longer holds a droplet.
	// Degenerate triangles - all three vertices identical - cost the acceleration
	// structure build almost nothing and can never be hit, which is what lets the
	// range stay contiguous without dead droplets reappearing in it.
	//
	// Done only on the transition to empty and then remembered, so a gap does not
	// re-blank itself every frame.
	for (int i = 0; i <= highest_slot; i++)
	{
		if (slot_live[i] || blood_cache[i].blanked)
			continue;

		/* FIRST BLOOD (Matt 11-14: 13.4 ms of "blood meshes" for 619 new droplets, of
		   which the droplets' own jobs were 1.5). The regions run [film][close-up][slots],
		   so the first droplet in a slot puts every film chunk and close-up region below
		   it in range - none of them blanked yet - and this cleared each one: the whole
		   film area (164 MB of shadow at cl_blood_film_tris 1048576), on the first blood of every map. The shadow was
		   made zeroed; a region still all zero needs only the bookkeeping. The device side
		   is unchanged (dirty_slot, dev_span): the epoch reset zeroed the section there. */
		if (!blood_cache[i].shadow_zero)
		{
			memset(blood.prim_shadow + region_offset(i), 0, sizeof(VboPrimitive) * region_cap(i));
			memset(blood.pos_shadow + region_offset(i), 0, sizeof(prim_positions_t) * region_cap(i));
			blood_cache[i].shadow_zero = true;
		}

		blood_cache[i].valid = false;
		blood_cache[i].blanked = true;
		blood_cache[i].faces = 0;

		blood.dirty_slot[i] = true;
	}

	// Everything past the used range is simply not covered by the build.
	for (int i = highest_slot + 1; i < BLOOD_MAX_REGIONS; i++)
	{
		blood_cache[i].valid = false;
		blood_cache[i].blanked = false;
	}

	return highest_slot < 0 ? 0 : region_offset(highest_slot) + region_cap(highest_slot);
}

/*
================
vkpt_blood_prepare

THE CPU HALF of vkpt_blood_update, shared with the D3D12 backend: the frame's
droplets written into the shadow copy of the section, and what has to reach the
device - the whole section zeroed when the device's copy of it is void (an
epoch), then one region per dirty slot. Each backend carries those regions out
of the shadow copy through its own staging into the instanced buffers, before
the BLAS build over the section; Vulkan's is vkpt_blood_update, below.

The caller must already have reserved vkpt_blood_prim_count() primitives at
prim_offset. frame->prim_count is what the BLAS covers (0: no blood this frame).
================
*/
void vkpt_blood_prepare(
	const blood_sphere_t* spheres,
	int num_spheres,
	const blood_film_t* films,
	int num_films,
	bsp_t* bsp,
	const vec3_t cam_pos,
	float fov_y,
	int view_height,
	int model_instance_index,
	uint32_t prim_offset,
	vkpt_blood_frame_t* frame)
{
	memset(frame, 0, sizeof(*frame));
	frame->prim_offset = prim_offset;
	blood.prim_count = 0;

	if (num_spheres < 0 || !spheres)
		num_spheres = 0;
	if (num_films < 0 || !films)
		num_films = 0;

	// A negative instance index means prepare_entities ran out of ModelInstance
	// slots. Every blood primitive names that instance, so there is nothing safe
	// to draw - drop the frame's droplets rather than index the UBO array out of
	// bounds.
	//
	// A frame that reserves nothing also ENDS THE EPOCH. The section's address is
	// not tracked across a frame it does not occupy, and whatever the device
	// holds for it belongs to a model by the time blood comes back.
	if ((num_spheres <= 0 && num_films <= 0) || model_instance_index < 0 || !cvar_pt_blood_spheres->integer)
	{
		blood.have_last_offset = false;
		blood_blas_report(prim_offset, 0, true);
		return;
	}

	const uint64_t prep_t0 = Sys_Microseconds();
	const bool had_templates = templates_built;
	if (!templates_built)
		build_sphere_templates();

	num_spheres = min(num_spheres, MAX_BLOOD_SPHERES);

	{
		cvar_t* sz = Cvar_Get("cl_blood_splat_size", "1.5", CVAR_ARCHIVE);
		global_blood_splat_size = sz->value;
	}

	// Pick each droplet's tessellation from how much SCREEN it covers - its
	// drawn extent against its distance, not distance alone - and size the
	// buffers to what that actually needs rather than to the worst case. The
	// worst case - every droplet at full tessellation - is 512 * 320 primitives,
	// around 80 MB of staging and shadow memory that a typical frame nowhere near
	// uses. Growing to the high-water mark keeps the common case cheap without
	// capping the uncommon one.
	// Surface blood alone still needs the section, with no droplet in the air.
	const uint64_t lods_t0 = Sys_Microseconds();
	uint32_t needed = num_spheres > 0 ? choose_lods(spheres, num_spheres, cam_pos, fov_y, view_height) : 0;
	for (int k = 0; k < num_films; k++)
		needed += (uint32_t)max(0, films[k].num_tris);
	if (needed == 0)
	{
		blood.have_last_offset = false;
		blood_blas_report(prim_offset, 0, true);
		return;
	}

	const uint64_t buffers_t0 = Sys_Microseconds();
	blood_in_frame = true;
	const bool buffers_ok = ensure_buffers();
	blood_in_frame = false;
	if (!buffers_ok)
	{
		blood.have_last_offset = false;
		blood_blas_report(prim_offset, 0, true);
		return;
	}

	const uint64_t closeups_t0 = Sys_Microseconds();
	if (num_spheres > 0)
		assign_closeups();
	const uint64_t closeups_t1 = Sys_Microseconds();

	if (needed > blood.max_prims)
	{
		// More droplets than the buffers were sized for. Cannot happen while
		// cl_blood_max is what sized them, but clamp rather than overrun if it
		// ever does - the tail simply does not draw this frame.
		needed = blood.max_prims;
	}

	blood.model_instance_index = model_instance_index;

	const uint64_t gen_t0 = Sys_Microseconds();
	uint32_t prim_count = write_blood_geometry(spheres, num_spheres, films, num_films, bsp);
	blood.last_gen_usec = Sys_Microseconds() - gen_t0;

	if (prim_count == 0)
	{
		blood.have_last_offset = false;
		blood_blas_report(prim_offset, 0, true);
		return;
	}

	// prim_count is (highest_slot + 1) * stride, and a slot ABOVE the current
	// capacity can outlive a lowered cl_blood_max - the droplet holding it keeps
	// its slot while the buffers shrink underneath it. Everything below sizes a
	// copy, and now a vkCmdFillBuffer, from this number, so clamp it to what was
	// actually allocated and reserved. The tail simply does not draw, which is
	// already what the per-write bounds check in write_blood_geometry implies.
	if (prim_count > blood.max_prims)
		prim_count = blood.max_prims;

	/*
	WHAT HAS TO TRAVEL, AND WHY IT IS NOT A SINGLE SPAN

	The shadow buffer is authoritative; the device holds a copy of it at
	prim_offset. Keeping the two in step is per SLOT, and the cost of getting it
	wrong is enormous: a full section is cl_blood_max * stride primitives, 102 MB
	at 2048 slots and pt_blood_tess 2, and the CPU memcpy into write-combined
	staging runs at single-digit GB/s - measured 6-7 ms, invisible to any GPU
	profiler. Matt saw exactly that as a hitch when blood spawned or landed.

	Three things used to force the whole 102 MB across:

	  1. More than 64 dirty slots fell back to ONE span from the lowest dirty
	     slot to the highest. Moving droplets are scattered through a floor of
	     settled ones, so that span is nearly the whole buffer - 128 moving
	     droplets uploaded 94 MB to change 6 MB. There is no region budget now:
	     one region per dirty slot, at most one per slot, so the array cannot
	     overflow and no fallback is needed.

	  2. A change in the section's primitive COUNT reset a "clean frames"
	     counter, and until it recovered every frame sent the full range. It
	     never needed to: a slot that comes into range is either a droplet the
	     cache missed or a slot the blank loop cleared, and both mark themselves
	     dirty. dev_pending is sticky, so nothing can be lost by uploading late.

	  3. The section MOVING, which really does invalidate every byte - and used
	     to happen whenever an entity appeared or vanished. The base is pinned in
	     prepare_entities now, so this is rare; when it does happen the section is
	     zeroed on the GPU with vkCmdFillBuffer and only real geometry is sent,
	     rather than pushing a stride's worth of zeros per slot over the bus.

	The old comment here claimed a partial upload was only safe once both staging
	buffers had seen the full range. That was never true: the memcpy below fills
	exactly the source ranges the GPU copy reads, in the same frame, so whatever
	else is stale in this frame's staging buffer is never read.
	*/
	// How many REGIONS the section covers this frame (see ULTRA REGIONS).
	uint32_t slot_limit = 0;
	{
		const uint32_t limit = min(prim_count, blood.max_prims);
		const uint32_t total = (uint32_t)blood.film_count + (uint32_t)blood.ultra_count
		                     + (uint32_t)vkpt_blood_slot_capacity();
		while (slot_limit < total && slot_limit < BLOOD_MAX_REGIONS
			&& region_offset((int)slot_limit) + region_cap((int)slot_limit) <= limit)
			slot_limit++;
	}

	// AN EPOCH is a run of frames over which the section keeps the same address
	// in the same pair of instanced buffers. Inside one, the device's copy of an
	// untouched slot is still good. Across one it is not, and neither is anything
	// dev_span remembers - and note that vkpt_vertex_buffer_ensure_primbuf_size
	// DESTROYS and recreates those buffers, which is why the generation counter
	// is part of the test rather than just the offset. (The D3D12 backend bumps
	// it too when it remakes its own - vkpt_primbuf_new_generation.)
	const uint32_t primbuf_gen = vkpt_primbuf_generation();

	const bool epoch_reset = blood.epoch_reset
		|| !blood.have_last_offset
		|| prim_offset != blood.last_prim_offset
		|| primbuf_gen != blood.primbuf_generation;

	/* WHY EVERYTHING WENT AGAIN (11-15: Matt's capture 004, xhangar2's first seconds,
	   had 1122 moved droplets sent whole - 69 MB, 7 ms of upload on the GPU - and its
	   blood work was under the slow-frame line's 4 ms, so nothing said why). Not the
	   section's first frame of a map: that one is always an epoch. */
	if (epoch_reset && blood.have_last_offset && blood.sphere_count > 0)
	{
		char why[96];
		if (blood.epoch_reset)
			Q_strlcpy(why, "invalidated (new shadow buffers, or a backend could not send a frame)", sizeof(why));
		else if (primbuf_gen != blood.primbuf_generation)
			Q_strlcpy(why, "the instanced buffers were remade (grown)", sizeof(why));
		else
			Q_snprintf(why, sizeof(why), "the section moved, primitive %u -> %u", blood.last_prim_offset, prim_offset);
		Con_SkipNotify(true);
		Com_Printf("Blood: epoch - %s; all %d droplets sent again\n", why, blood.sphere_count);
		Con_SkipNotify(false);
	}

	blood.epoch_reset = false;
	blood.last_prim_offset = prim_offset;
	blood.have_last_offset = true;
	blood.primbuf_generation = primbuf_gen;

	if (epoch_reset)
	{
		// The backend zeroes the section on the GPU instead of sending zeros
		// through staging. After this every slot needs only its real faces
		// uploaded, and a slot holding nothing needs no upload at all.
		for (uint32_t i = 0; i < BLOOD_MAX_REGIONS; i++)
		{
			blood_cache[i].dev_pending = true;

			// Past the filled range the device holds bytes that belong to
			// somebody else, so assume the worst for those slots until one is
			// written across its whole stride.
			blood_cache[i].dev_span = (i < slot_limit) ? 0 : (uint16_t)region_cap((int)i);
		}
	}
	else
	{
		// The whole array, not just the slots in range: dev_pending is sticky, so
		// a slot the section does not reach yet keeps its flag for the frame it
		// does.
		for (uint32_t i = 0; i < BLOOD_MAX_REGIONS; i++)
		{
			if (blood.dirty_slot[i])
				blood_cache[i].dev_pending = true;
		}
	}

	// The droplets that only moved, as moves where the device's copy allows
	// (MOVED ON THE GPU) - and otherwise owed their region's upload, below.
	uint32_t num_moves = 0;
	for (uint32_t i = 0; i < BLOOD_MAX_REGIONS; i++)
	{
		if (!blood.moved_slot[i])
			continue;

		blood_cache_entry_t* c = blood_cache + i;
		if (epoch_reset || i >= slot_limit || c->dev_pending || c->dev_span != c->faces
			|| region_move[i].count != c->faces || num_moves >= MAX_BLOOD_SPHERES)
		{
			c->dev_pending = true;
			blood.moved_slot[i] = false;    // sent whole below, not moved
			continue;
		}

		BloodMove* m = move_list + num_moves++;
		*m = region_move[i];
		m->first = prim_offset + region_offset((int)i);
	}

	// One region per dirty slot. File scope rather than stack: 4096 slots is
	// 48 KB of regions.
	static vkpt_blood_region_t regions[BLOOD_MAX_REGIONS];
	uint32_t num_regions = 0;
	size_t up_bytes = 0;
	// a slot the GPU moves since the last region: no merging across it (below)
	bool gap_has_move = false;
	uint32_t num_clears = 0;
	const bool gpu_clear = blood.gpu_move && !epoch_reset;

	for (uint32_t i = 0; i < slot_limit; i++)
	{
		blood_cache_entry_t* c = blood_cache + i;

		if (blood.moved_slot[i])
			gap_has_move = true;

		if (!c->dev_pending)
			continue;

		// This slot's real geometry, plus however much of the device's older
		// content sticks out past it. Everything beyond that is already zero on
		// both sides.
		uint32_t n = max((uint32_t)c->faces, (uint32_t)c->dev_span);
		n = min(n, region_cap((int)i));

		const uint32_t lo = region_offset((int)i);

		/*
		ZEROS ON THE GPU (pt_blood_gpu_move, 11-15). The part past the new mesh is
		zeros in the shadow, and sending them was most of a burst's upload: a new
		droplet of 80 faces in a slot whose last puddle had 256 sent 42 KB, 13 of it
		mesh - Matt's 1000-droplet frames were 60-81 MB, 5.5 ms of copy into the
		staging. Now that part is a clear record beside the moves (BLOOD_MOVE_CLEAR,
		blood_move.comp), and a region that went (faces 0) is a clear alone.

		Ordered as the moves are: the copies first, then the dispatch. A copy that
		merges across the cleared part (below) writes the shadow's zeros there
		first, and the clear zeros again - the same bytes either way, so merging
		need not stop at one, unlike at a move.
		*/
		const uint32_t real = min((uint32_t)c->faces, n);
		if (gpu_clear && n > real && num_moves < MAX_BLOOD_SPHERES)
		{
			BloodMove* m = move_list + num_moves++;
			memset(m, 0, sizeof(*m));
			m->first = prim_offset + lo + real;
			m->count = n - real;
			m->mv_z = BLOOD_MOVE_CLEAR;
			num_clears++;
			n = real;
		}

		c->dev_pending = false;
		c->dev_span = c->faces;

		if (n == 0)
			continue;

		/*
		MERGING ACROSS A GAP (11-14). Neighbouring slots that both need their whole
		stride were always one copy rather than two - the common shape on an epoch
		frame. Now so are two regions a few primitives apart, the gap going with
		them: D3D12 has no many-region buffer copy, so each region was two
		CopyBufferRegions, and Matt's dip captures had 700-1250 regions a frame -
		up to 2500 copy commands - with the upload slower per byte the more of
		them there were (46 MB in 712 regions 1.6 ms, 31 MB in 1252 2.1 ms).

		Safe because the gap's bytes in the shadow are what the device holds: a
		slot that is not dev_pending is in step with the device, and past a
		region's n both sides are zero (dev_span; the shadow starts zeroed and
		every rewrite clears what it no longer covers). pt_blood_upload_merge is
		the most primitives a gap may be: 128 is 21 KB, well under what two copy
		commands cost in time at PCIe rates.
		*/
		const uint32_t merge_gap = (uint32_t)max(0, cvar_pt_blood_upload_merge->integer);
		vkpt_blood_region_t* last = num_regions ? &regions[num_regions - 1] : NULL;
		const uint32_t last_end = last ? last->src + last->count : 0;

		// NOT ACROSS A MOVED SLOT (MOVED ON THE GPU): the shadow there is already
		// moved, and blood_move.comp then adds the move again on the device.
		const bool may_merge = !gap_has_move;
		gap_has_move = false;

		if (last && may_merge && lo >= last_end && lo - last_end <= merge_gap)
		{
			up_bytes += (size_t)(lo - last_end) * (sizeof(VboPrimitive) + sizeof(prim_positions_t));
			last->count = lo + n - last->src;
		}
		else
		{
			regions[num_regions] = (vkpt_blood_region_t){ lo, prim_offset + lo, n };
			num_regions++;
		}

		up_bytes += (size_t)n * (sizeof(VboPrimitive) + sizeof(prim_positions_t));
	}

	up_bytes += (size_t)num_moves * sizeof(BloodMove);

	blood.last_regions = num_regions;
	blood.last_moves = num_moves - num_clears;
	blood.last_clears = num_clears;
	blood.last_epoch = epoch_reset;
	blood.last_up_bytes = up_bytes;

	// what the frame's blood work was, beside its times (cpuprof.h)
	CPUPROF_COUNT_SET(BLOOD_MOVED, blood.cache_moves);
	CPUPROF_COUNT_SET(BLOOD_STILL, blood.cache_hits);
	CPUPROF_COUNT_SET(BLOOD_REBUILT, max(0, blood.sphere_count - blood.cache_hits - blood.cache_moves));
	CPUPROF_COUNT_SET(BLOOD_COPIES, num_regions);
	CPUPROF_COUNT_SET(BLOOD_UP_KB, (up_bytes + 1023) / 1024);
	CPUPROF_COUNT_SET(BLOOD_GPU_MOVES, num_moves - num_clears);

	// The fill, the copies and the moves are the only writes this section gets -
	// see BLAS REUSE. None of them means the device holds what it held.
	blood_blas_report(prim_offset, prim_count, epoch_reset || num_regions > 0 || num_moves > 0);

	/* A SLOW BLOOD FRAME SAYS WHERE (11-14): the profiler's "blood meshes" is this
	   whole function; over 4 ms, its parts go to the log beside the PROFCAPTURE
	   lines - the first blood of a map took 14.6 ms there, and its mesh work
	   alone cannot have been more than about 3. */
	{
		const uint64_t done = Sys_Microseconds();
		const double total = (double)(done - prep_t0) * 1e-3;
		if (total >= 4.0)
		{
			// the droplet jobs' split: work / wall near the thread count = they ran side
			// by side and each was simply slow; a big wake or wait = a worker came late
			char jobs[160] = "";
			const vkpt_jobs_stats_t* js = &blood_drop_stats;
			if (js->count > 0)
				Q_snprintf(jobs, sizeof(jobs), "; droplet jobs %.1f ms on %d threads - work %.1f, caller ran %d of %d, "
					"slowest wake %.1f, caller waited %.1f", js->wall_us * 1e-3, js->woken + 1, js->work_us * 1e-3,
					js->by_caller, js->count, js->wake_us * 1e-3, js->tail_us * 1e-3);
			Con_SkipNotify(true);
			Com_Printf("Blood: slow frame %.1f ms - templates %.1f%s, lods %.1f, buffers %.1f, close-ups %.1f, "
				"meshes %.1f (%d droplets: %d moved, %d rebuilt; %d film tiles), regions %.1f (%u + %u moves + %u clears, %.1f MB)%s%s\n",
				total, (double)(lods_t0 - prep_t0) * 1e-3, had_templates ? "" : " (made)",
				(double)(buffers_t0 - lods_t0) * 1e-3, (double)(closeups_t0 - buffers_t0) * 1e-3,
				(double)(closeups_t1 - closeups_t0) * 1e-3, (double)blood.last_gen_usec * 1e-3,
				blood.sphere_count, blood.cache_moves,
				max(0, blood.sphere_count - blood.cache_hits - blood.cache_moves), blood.film_tiles,
				(double)(done - (closeups_t1 + blood.last_gen_usec)) * 1e-3, num_regions, num_moves - num_clears, num_clears,
				(double)up_bytes / (1024.0 * 1024.0), epoch_reset ? ", epoch reset" : "", jobs);
			Con_SkipNotify(false);
		}
	}

	blood.prim_count = prim_count;

	frame->prim_count = prim_count;
	frame->zero_section = epoch_reset;
	frame->num_regions = num_regions;
	frame->regions = regions;
	frame->num_moves = num_moves;
	frame->moves = move_list;
	frame->prims = blood.prim_shadow;
	frame->positions = blood.pos_shadow;
	frame->capacity = blood.max_prims;
}

// A backend could not carry a prepared frame to the device (no staging): what
// it holds for the section is no longer what the cache believes, so the next
// frame starts a new epoch - the section zeroed and every slot sent again.
void vkpt_blood_invalidate(void)
{
	blood.epoch_reset = true;
}

/* pt_blood_stats' line, once a second; copy_usec is the backend's memcpy into its
   staging. Called every frame by both backends. The second line is the GPU's side over
   that second (profiler rows, so profiler 1 has to be on): the dynamic BLAS builds
   (blood's among them) and the whole frame, average and worst. Blood the BLAS builds
   cost shows in bvh; blood that costs tracing shows in frame but not bvh. "blas
   built / reused" is pt_blood_blas_reuse's (Vulkan); left out where nothing counted it. */
void vkpt_blood_print_stats(uint64_t copy_usec)
{
	if (!cvar_pt_blood_stats->integer)
		return;

	static double bvh_sum, bvh_max, frame_sum, frame_max;
	static int samples;
	const double bvh = vkpt_get_profiler_result(PROFILER_BVH_UPDATE);
	const double frame = vkpt_get_profiler_result(PROFILER_GPU_FRAME);
	if (frame > 0.0)
	{
		bvh_sum += bvh;
		frame_sum += frame;
		bvh_max = max(bvh_max, bvh);
		frame_max = max(frame_max, frame);
		samples++;
	}

	static unsigned last_report = 0;
	unsigned now = Sys_Milliseconds();
	if (now - last_report > 1000)
	{
		last_report = now;
		char reuse[64] = "";
		if (blood_blas.builds || blood_blas.reuses)
			Q_snprintf(reuse, sizeof(reuse), ", blas %d built / %d reused", blood_blas.builds, blood_blas.reuses);
		Com_Printf("blood gpu-side: %d droplets (%d splats, %d cached, %d moved), %d tris (%.0f avg), gen %.2f ms + copy %.2f ms%s, %.2f MB in %d regions + %u moves + %u clears%s%s\n",
			blood.sphere_count, blood.splat_count, blood.cache_hits, blood.cache_moves, blood.prim_count,
			blood.sphere_count ? (float)blood.prim_count / blood.sphere_count : 0.f,
			blood.last_gen_usec / 1000.f, copy_usec / 1000.f, blood.last_regions == 0 ? " (skipped)" : "",
			(float)blood.last_up_bytes / (1024.f * 1024.f),
			blood.last_regions, blood.last_moves, blood.last_clears, blood.last_epoch ? " EPOCH" : "", reuse);
		if (blood.closeup_up || blood.closeup_short)
			Com_Printf("blood close-up (pt_blood_lod_px %g): %d droplets drawn finer, %d of them in the %d-region pool, %d short of pool or budget\n",
				cvar_pt_blood_lod_px->value, blood.closeup_up, blood.closeup_pool, blood.ultra_count, blood.closeup_short);
		if (blood.film_count)
			Com_Printf("blood film: %d tiles, %d tris in %d / %d chunks%s\n",
				blood.film_tiles, blood.film_tris, blood.film_chunks_used, blood.film_count,
				blood.film_truncated ? va(", %d tiles TRUNCATED", blood.film_truncated) : "");
		if (samples)
			Com_Printf("blood gpu times: bvh update %.2f ms (worst %.2f), gpu frame %.2f ms (worst %.2f), %d frames\n",
				bvh_sum / samples, bvh_max, frame_sum / samples, frame_max, samples);
		else
			Com_Printf("blood gpu times: none - profiler 1 for the GPU's side\n");
		blood_blas.builds = 0;
		blood_blas.reuses = 0;
		bvh_sum = bvh_max = frame_sum = frame_max = 0.0;
		samples = 0;
	}
}

// vkpt_blood_update's copy into the staging, a region a job (jobs.c)
typedef struct {
	const VkBufferCopy*	prim;
	const VkBufferCopy*	pos;
	char*				dst_prim;
	char*				dst_pos;
	const char*			src_prim;
	const char*			src_pos;
} vk_copy_t;

static void vk_copy_job(void* ctx, int r)
{
	const vk_copy_t* c = ctx;
	memcpy(c->dst_prim + c->prim[r].srcOffset, c->src_prim + c->prim[r].srcOffset, c->prim[r].size);
	memcpy(c->dst_pos + c->pos[r].srcOffset, c->src_pos + c->pos[r].srcOffset, c->pos[r].size);
}

/*
================
vkpt_blood_update

Fills the blood section of the instanced buffers and reports how many primitives
it actually wrote: vkpt_blood_prepare, then Vulkan's half - the fill on an epoch
frame, the dirty regions through this frame's staging, and the barrier before
the BLAS build and the shaders read them.

Must run BEFORE vkpt_pt_create_all_dynamic, and the caller must already have
reserved vkpt_blood_prim_count() primitives at prim_offset.
================
*/
void vkpt_blood_update(
	VkCommandBuffer cmd_buf,
	const blood_sphere_t* spheres,
	int num_spheres,
	const blood_film_t* films,
	int num_films,
	bsp_t* bsp,
	const vec3_t cam_pos,
	float fov_y,
	int view_height,
	int model_instance_index,
	uint32_t prim_offset,
	uint32_t* prim_count_out)
{
	*prim_count_out = 0;

	vkpt_blood_frame_t frame;
	CPUPROF_DETAIL_BEGIN(BLOOD_MESH);
	vkpt_blood_prepare(spheres, num_spheres, films, num_films, bsp, cam_pos, fov_y, view_height,
	                   model_instance_index, prim_offset, &frame);
	CPUPROF_DETAIL_END(BLOOD_MESH);
	if (frame.prim_count == 0)
		return;

	// The staging follows the shadow copy's size, which changes only with the
	// settings that define the worst case (see ensure_buffers).
	if (!vk_staging_ensure(frame.capacity))
	{
		// Nothing reached the device: the next frame starts the section afresh.
		vkpt_blood_invalidate();
		blood.prim_count = 0;
		blood_blas_report(prim_offset, 0, true);
		return;
	}

	blood_vk.frame_index = (blood_vk.frame_index + 1) % MAX_FRAMES_IN_FLIGHT;

	const uint32_t prim_count = frame.prim_count;
	const bool epoch_reset = frame.zero_section;

	if (epoch_reset)
	{
		// Zero the section on the GPU instead of sending zeros through staging.
		vkCmdFillBuffer(cmd_buf, qvk.buf_primitive_instanced.buffer,
			(VkDeviceSize)prim_offset * sizeof(VboPrimitive),
			(VkDeviceSize)prim_count * sizeof(VboPrimitive), 0);

		vkCmdFillBuffer(cmd_buf, qvk.buf_positions_instanced.buffer,
			(VkDeviceSize)prim_offset * sizeof(prim_positions_t),
			(VkDeviceSize)prim_count * sizeof(prim_positions_t), 0);

		// The fill and the copies below write the same memory, so they have to be
		// ordered against each other. Transfer to transfer, and only these two
		// buffers - not a pipeline drain.
		VkBufferMemoryBarrier fill_barriers[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = qvk.buf_primitive_instanced.buffer,
				.offset = (VkDeviceSize)prim_offset * sizeof(VboPrimitive),
				.size = (VkDeviceSize)prim_count * sizeof(VboPrimitive),
			},
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = qvk.buf_positions_instanced.buffer,
				.offset = (VkDeviceSize)prim_offset * sizeof(prim_positions_t),
				.size = (VkDeviceSize)prim_count * sizeof(prim_positions_t),
			}
		};

		vkCmdPipelineBarrier(cmd_buf,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, NULL, 2, fill_barriers, 0, NULL);
	}

	// The prepare's regions as copies, in bytes. File scope rather than stack:
	// 4096 slots is 200 KB of VkBufferCopy across the two.
	static VkBufferCopy regions_prim[BLOOD_MAX_REGIONS];
	static VkBufferCopy regions_pos[BLOOD_MAX_REGIONS];
	const uint32_t num_regions = frame.num_regions;
	for (uint32_t r = 0; r < num_regions; r++)
	{
		const vkpt_blood_region_t* g = frame.regions + r;
		regions_prim[r] = (VkBufferCopy){
			(VkDeviceSize)g->src * sizeof(VboPrimitive),
			(VkDeviceSize)g->dst * sizeof(VboPrimitive),
			(VkDeviceSize)g->count * sizeof(VboPrimitive) };
		regions_pos[r] = (VkBufferCopy){
			(VkDeviceSize)g->src * sizeof(prim_positions_t),
			(VkDeviceSize)g->dst * sizeof(prim_positions_t),
			(VkDeviceSize)g->count * sizeof(prim_positions_t) };
	}

	// Nothing dirty is the common case for a settled floor, and then the whole
	// upload is dead work - the device still holds exactly this.
	const bool skip_upload = (num_regions == 0);

	const uint64_t copy_t0 = Sys_Microseconds();
	if (!skip_upload)
	{
		// a region a job (jobs.c): see the D3D12 backend's blood_copy_job
		vk_copy_t copy = {
			.prim = regions_prim, .pos = regions_pos,
			.dst_prim = (char*)blood_vk.mapped_prim[blood_vk.frame_index],
			.dst_pos = (char*)blood_vk.mapped_pos[blood_vk.frame_index],
			.src_prim = (const char*)frame.prims, .src_pos = (const char*)frame.positions,
		};
		vkpt_parallel_for((int)num_regions, vk_copy_job, &copy);
	}
	const uint64_t copy_usec = Sys_Microseconds() - copy_t0;
	CPUPROF_DETAIL_ADD(BLOOD_COPY, copy_usec);

	if (!skip_upload && num_regions)
	{
		vkCmdCopyBuffer(cmd_buf, blood_vk.staging_prim[blood_vk.frame_index].buffer,
			qvk.buf_primitive_instanced.buffer, num_regions, regions_prim);

		vkCmdCopyBuffer(cmd_buf, blood_vk.staging_pos[blood_vk.frame_index].buffer,
			qvk.buf_positions_instanced.buffer, num_regions, regions_pos);
	}

	// These are transfer writes into buffers that the BLAS build reads as vertex
	// data and the path tracer reads as primitive data. The barrier
	// vkpt_instance_geometry() emits covers neither case: it is COMPUTE ->
	// COMPUTE on buf_primitive_instanced alone, and says nothing about a
	// transfer write or about the position buffer. So state the dependency here
	// rather than relying on someone else's.
	//
	// Deliberately NOT the BUFFER_BARRIER macro, which is
	// ALL_COMMANDS -> ALL_COMMANDS: that is a full pipeline drain, and doing two
	// of them every frame a droplet is alive costs far more than the droplets do.
	// Naming the real stages lets everything unrelated keep overlapping. Both
	// ranges are limited to what was actually written, not VK_WHOLE_SIZE.
	//
	// The destination stage must not name RAY_TRACING_SHADER_BIT_KHR on a
	// ray-query device, where it is invalid - the same rule ACCEL_STRUCT_READ_STAGES
	// follows in path_tracer.c.
	const VkPipelineStageFlags shader_read_stages = qvk.use_ray_query
		? (VkPipelineStageFlags)VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
		: (VkPipelineStageFlags)(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
		                       | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	// THE RANGE COVERED IS THE RANGE THAT WAS WRITTEN, and on an epoch frame that
	// is the whole section: the fill wrote the parts no copy region touches, and
	// the BLAS build reads those too - as the degenerate triangles that keep dead
	// slots from drawing. Covering only the copy regions there would leave the
	// fill unsynchronised, which is the exact shape of bug that has cost this
	// renderer whole-frame faults before.
	VkDeviceSize barrier_prim_offset = 0, barrier_prim_size = 0;
	VkDeviceSize barrier_pos_offset = 0, barrier_pos_size = 0;
	bool need_barrier = false;

	if (epoch_reset)
	{
		barrier_prim_offset = (VkDeviceSize)prim_offset * sizeof(VboPrimitive);
		barrier_prim_size = (VkDeviceSize)prim_count * sizeof(VboPrimitive);
		barrier_pos_offset = (VkDeviceSize)prim_offset * sizeof(prim_positions_t);
		barrier_pos_size = (VkDeviceSize)prim_count * sizeof(prim_positions_t);
		need_barrier = true;
	}
	else if (!skip_upload && num_regions)
	{
		// One barrier per buffer covering first..last region, rather than one per
		// region: the ranges are disjoint but a single span over them is still far
		// tighter than the whole buffer, and costs one barrier instead of hundreds.
		const VkBufferCopy* lastp = &regions_prim[num_regions - 1];
		const VkBufferCopy* lastq = &regions_pos[num_regions - 1];

		barrier_prim_offset = regions_prim[0].dstOffset;
		barrier_prim_size = (lastp->dstOffset + lastp->size) - barrier_prim_offset;
		barrier_pos_offset = regions_pos[0].dstOffset;
		barrier_pos_size = (lastq->dstOffset + lastq->size) - barrier_pos_offset;
		need_barrier = true;
	}

	VkBufferMemoryBarrier barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_primitive_instanced.buffer,
			.offset = barrier_prim_offset,
			.size = barrier_prim_size,
		},
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_positions_instanced.buffer,
			.offset = barrier_pos_offset,
			.size = barrier_pos_size,
		}
	};

	if (need_barrier)
	{
		vkCmdPipelineBarrier(cmd_buf,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | shader_read_stages,
			0, 0, NULL, 2, barriers, 0, NULL);
	}

	/*
	THE MOVES (MOVED ON THE GPU): the records through this frame's staging, then
	blood_move.comp adds each one's offset to its droplet's corners in place. Its
	regions are none of the copies' (vkpt_blood_prepare), so only last frame's use
	of the section and the records' copy come before it: the barrier takes the
	BLAS build's and the shaders' reads and the transfers, and gives the dispatch
	the records and the section. After it, the build and the shaders read what it
	wrote, as after the copies.
	*/
	const uint32_t num_moves = frame.num_moves;
	if (num_moves && blood_vk.mapped_moves[blood_vk.frame_index] && qvk.buf_blood_moves.buffer)
	{
		memcpy(blood_vk.mapped_moves[blood_vk.frame_index], frame.moves, sizeof(BloodMove) * num_moves);
		const VkBufferCopy move_copy = { 0, 0, sizeof(BloodMove) * num_moves };
		vkCmdCopyBuffer(cmd_buf, blood_vk.staging_moves[blood_vk.frame_index].buffer,
			qvk.buf_blood_moves.buffer, 1, &move_copy);

		const VkDeviceSize sec_prim_offset = (VkDeviceSize)prim_offset * sizeof(VboPrimitive);
		const VkDeviceSize sec_prim_size = (VkDeviceSize)prim_count * sizeof(VboPrimitive);
		const VkDeviceSize sec_pos_offset = (VkDeviceSize)prim_offset * sizeof(prim_positions_t);
		const VkDeviceSize sec_pos_size = (VkDeviceSize)prim_count * sizeof(prim_positions_t);

		VkBufferMemoryBarrier before[3] = {
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = qvk.buf_blood_moves.buffer,
				.offset = 0,
				.size = move_copy.size,
			},
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = qvk.buf_primitive_instanced.buffer,
				.offset = sec_prim_offset,
				.size = sec_prim_size,
			},
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.buffer = qvk.buf_positions_instanced.buffer,
				.offset = sec_pos_offset,
				.size = sec_pos_size,
			}
		};
		vkCmdPipelineBarrier(cmd_buf,
			VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | shader_read_stages,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 0, NULL, 3, before, 0, NULL);

		vkpt_blood_move_dispatch(cmd_buf, num_moves);

		VkBufferMemoryBarrier after[2] = { before[1], before[2] };
		for (int i = 0; i < 2; i++)
		{
			after[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			after[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		}
		vkCmdPipelineBarrier(cmd_buf,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | shader_read_stages,
			0, 0, NULL, 2, after, 0, NULL);
	}
	else if (num_moves)
	{
		// no staging for them: the section starts afresh next frame
		vkpt_blood_invalidate();
	}

	vkpt_blood_print_stats(copy_usec);

	*prim_count_out = prim_count;
}
