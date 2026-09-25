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
#include "conversion.h"
#include "shader/vertex_buffer.h"
#include "system/system.h"

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
// It is also the cost knob. Every per-frame expense scales linearly with it -
// the CPU generation loop, the host copy, and the dynamic BLAS rebuild, which
// happens every frame because the droplets move. The staging buffers are sized
// for MAX_BLOOD_SPHERES at the CURRENT tessellation and reallocated when it
// changes, so picking 320 costs memory only while it is selected.
#define BLOOD_SPHERE_MAX_SUBDIV 3
#define BLOOD_SPHERE_MAX_FACES  (20 << (2 * BLOOD_SPHERE_MAX_SUBDIV))
#define BLOOD_SPHERE_FACES(subdiv) (20 << (2 * (subdiv)))

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

// All four LOD levels, laid end to end: level 0 at [0,20), level 1 at [20,100),
// level 2 at [100,420), level 3 at [420,1700). Built once - the whole set is
// 1700 faces, so there is no reason to rebuild when the tessellation changes,
// and holding all of them is what lets the level be picked PER DROPLET.
#define BLOOD_TEMPLATE_TOTAL (20 + 80 + 320 + 1280)

static blood_face_t   sphere_template[BLOOD_TEMPLATE_TOTAL];
static uint32_t       sphere_normals[BLOOD_TEMPLATE_TOTAL][3];    // encode_normal of the above
static uint32_t       sphere_tangents[BLOOD_TEMPLATE_TOTAL][3];
static int            lod_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];    // first face of each level

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
} blood_cache_entry_t;

/*
ULTRA REGIONS - the top tessellation level only right in front of the camera.

pt_blood_tess 3 is 1280 faces a droplet, and every slot used to be that wide
whatever its droplet actually drew. With a floor of 2048 splats that made the
blood BLAS a 2.6M-triangle rebuild every frame, most of it padding, and every
regenerated droplet cleared and re-uploaded 210 KB of slot. demo1 at 2048/tess 3
went from ~90 fps to ~60 as the floor filled up, BVH_UPDATE 0.3 -> 3.5-4.5 ms,
with 15-130 ms renderer hitches while blood landed.

So at tess 3 the section is split into REGIONS:

    [ ultra 0 .. U-1 ][ normal slot 0 .. cl_blood_max-1 ]
      ultra_stride       stride = worst case at tess 2

A droplet lives in its client slot's normal region unless its chosen level
needs more faces than that holds - which choose_lods only allows within
pt_blood_ultra_dist of the camera - in which case it borrows one of
pt_blood_ultra_slots ultra regions for as long as it stays that close. When the
pool is full the extra droplets are drawn one level down, in their own slot.

Everything that used to be per slot (cache, dirty flag, device span, blanking,
uploads) is per REGION now, so a droplet changing region is just "the old region
went dead, the new one needs building" - which the machinery already handled.
Below tess 3 there is no pool: U = 0 and region r is slot r, exactly as before.
*/
#define BLOOD_ULTRA_MAX   64
#define BLOOD_MAX_REGIONS (MAX_BLOOD_SPHERES + BLOOD_ULTRA_MAX)

static blood_cache_entry_t blood_cache[BLOOD_MAX_REGIONS];

// Which ultra region a client slot holds (-1: none), and the reverse.
static int16_t ultra_of_slot[MAX_BLOOD_SPHERES];
static int16_t slot_of_ultra[BLOOD_ULTRA_MAX];
static bool    ultra_maps_ready = false;

static struct {
	BufferResource_t  staging_prim[MAX_FRAMES_IN_FLIGHT];
	BufferResource_t  staging_pos[MAX_FRAMES_IN_FLIGHT];
	VboPrimitive*     mapped_prim[MAX_FRAMES_IN_FLIGHT];
	prim_positions_t* mapped_pos[MAX_FRAMES_IN_FLIGHT];
	VboPrimitive*     prim_shadow;
	prim_positions_t* pos_shadow;
	uint32_t          frame_index;
	uint32_t          max_prims;        // capacity of the buffers as currently sized
	uint32_t          prim_count;       // written by the last vkpt_blood_update
	int               sphere_count;     // droplets actually written
	int               splat_count;      // of those, how many took the splat path
	int               cache_hits;       // droplets whose geometry was reused as-is
	int               cache_moves;      // droplets whose geometry was only TRANSLATED
	uint32_t          stride;           // primitives reserved per droplet slot
	uint32_t          ultra_stride;     // primitives per ultra region (0: no pool)
	int               ultra_count;      // ultra regions at the front of the section
	bool              dirty_slot[BLOOD_MAX_REGIONS];  // regions rewritten this frame
	uint32_t          last_prim_offset; // where the blood section sat last frame
	bool              have_last_offset;
	uint32_t          primbuf_generation; // which incarnation of the instanced buffers
	bool              epoch_reset;      // the device's copy of the section is void
	uint32_t          last_regions;     // copy regions used, for pt_blood_stats
	bool              last_epoch;
	int               model_instance_index;
	bool              buffers_ready;
} blood;

static inline uint32_t region_offset(int r)
{
	return r < blood.ultra_count
		? (uint32_t)r * blood.ultra_stride
		: (uint32_t)blood.ultra_count * blood.ultra_stride + (uint32_t)(r - blood.ultra_count) * blood.stride;
}

static inline uint32_t region_cap(int r)
{
	return r < blood.ultra_count ? blood.ultra_stride : blood.stride;
}

static inline int region_of_slot(int slot)
{
	if (blood.ultra_count && ultra_maps_ready && ultra_of_slot[slot] >= 0)
		return ultra_of_slot[slot];
	return blood.ultra_count + slot;
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
static cvar_t* cvar_pt_blood_ultra_dist = NULL;
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
	for (int level = 0; level <= BLOOD_SPHERE_MAX_SUBDIV; level++)
	{
		lod_offset[level] = face_count;

		for (int i = 0; i < 20; i++)
			subdivide_face(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]], level, &face_count);

		assert(face_count - lod_offset[level] == BLOOD_SPHERE_FACES(level));
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

Allocated on FIRST USE rather than at startup, because cl_blood_spheres defaults
off and this is ~20 MB of staging and shadow memory that a player who never turns
it on should not pay for.
================
*/
static void release_buffers(void)
{
	if (!blood.buffers_ready)
		return;

	// The staging buffers may still be referenced by a command buffer that has
	// not finished, so this cannot just free them.
	vkpt_device_wait_idle();

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		if (blood.mapped_prim[i]) { buffer_unmap(&blood.staging_prim[i]); blood.mapped_prim[i] = NULL; }
		if (blood.mapped_pos[i])  { buffer_unmap(&blood.staging_pos[i]);  blood.mapped_pos[i] = NULL; }
		buffer_destroy(&blood.staging_prim[i]);
		buffer_destroy(&blood.staging_pos[i]);
	}

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
and 320 faces it is 27 MB of shadow plus 54 MB of staging. Allocated on first
blood, not at startup, so a player who never turns this on pays nothing.
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
static uint32_t blood_layout(int max_droplets, int* ultra_count, uint32_t* ultra_stride, uint32_t* stride)
{
	const int subdiv = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));
	const int worst_faces = max(BLOOD_SPHERE_FACES(subdiv), BLOOD_PUDDLE_FACES(subdiv));

	int u = 0;
	if (subdiv == BLOOD_SPHERE_MAX_SUBDIV)
		u = max(0, min(Cvar_Get("pt_blood_ultra_slots", "32", CVAR_ARCHIVE)->integer, BLOOD_ULTRA_MAX));

	*ultra_count = u;
	*ultra_stride = u ? (uint32_t)worst_faces : 0;
	*stride = u ? (uint32_t)max(BLOOD_SPHERE_FACES(subdiv - 1), BLOOD_PUDDLE_FACES(subdiv - 1))
	            : (uint32_t)worst_faces;

	return (uint32_t)u * *ultra_stride + (uint32_t)max_droplets * *stride;
}

static bool ensure_buffers(void)
{
	const int max_droplets = cvar_pt_blood_spheres->integer ? vkpt_blood_slot_capacity() : 1;

	int ultra_count;
	uint32_t ultra_stride, stride;
	const uint32_t needed = blood_layout(max_droplets, &ultra_count, &ultra_stride, &stride);

	// Exact compare, not >=: the point is to reallocate ONLY when the settings
	// change, never in response to how much blood happens to be on screen.
	if (blood.buffers_ready && blood.max_prims == needed
		&& blood.stride == stride && blood.ultra_count == ultra_count && blood.ultra_stride == ultra_stride)
		return true;

	release_buffers();

	const size_t prim_size = sizeof(VboPrimitive) * needed;
	const size_t pos_size = sizeof(prim_positions_t) * needed;

	for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
	{
		if (buffer_create(&blood.staging_prim[i], prim_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
			return false;

		if (buffer_create(&blood.staging_pos[i], pos_size,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
			return false;

		blood.mapped_prim[i] = buffer_map(&blood.staging_prim[i]);
		blood.mapped_pos[i] = buffer_map(&blood.staging_pos[i]);
	}

	// Build into ordinary memory and copy the used range in one go, rather than
	// writing straight through the mapped pointer: that is write-combined host
	// memory, and the generation loop touches each primitive's fields out of
	// order.
	blood.prim_shadow = Z_Mallocz(prim_size);
	blood.pos_shadow = Z_Mallocz(pos_size);

	blood.stride = stride;
	blood.ultra_stride = ultra_stride;
	blood.ultra_count = ultra_count;
	blood.max_prims = needed;
	reset_ultra_maps();
	blood.buffers_ready = true;

	// New buffers, a new stride, and a shadow copy full of zeros: nothing the
	// device holds for the old layout means anything now.
	blood.epoch_reset = true;

	// The shadow buffer is the cache's backing store, so a reallocation throws
	// away everything it was vouching for.
	memset(blood_cache, 0, sizeof(blood_cache));

	return true;
}

/*
================
vkpt_blood_initialize / vkpt_blood_destroy
================
*/
VkResult vkpt_blood_initialize(void)
{
	memset(&blood, 0, sizeof(blood));

	// Renderer-side master switch, separate from the client's cl_blood_spheres
	// so a screenshot A/B does not have to wait for droplets already in the air
	// to expire.
	cvar_pt_blood_spheres = Cvar_Get("pt_blood_spheres", "1", 0);

	// Triangles per droplet: 0 -> 20, 1 -> 80. This is the cost knob; see the
	// comment on BLOOD_SPHERE_MAX_SUBDIV.
	cvar_pt_blood_tess = Cvar_Get("pt_blood_tess", "2", CVAR_ARCHIVE);
	cvar_pt_blood_ultra_dist = Cvar_Get("pt_blood_ultra_dist", "40", CVAR_ARCHIVE);

	// Print live droplet and triangle counts once a second, so the cost of a
	// given firefight can be read off rather than guessed at.
	cvar_pt_blood_stats = Cvar_Get("pt_blood_stats", "0", 0);

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

VkResult vkpt_blood_destroy(void)
{
	release_buffers();

	memset(&blood, 0, sizeof(blood));
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
uint32_t vkpt_blood_prim_count(int num_spheres)
{
	if (!cvar_pt_blood_spheres || !cvar_pt_blood_spheres->integer || num_spheres <= 0)
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
	// known until vkpt_blood_update picks it - and at tess 3, the ultra pool in
	// front of the normal slots (blood_layout).
	int ultra_count;
	uint32_t ultra_stride, stride;

	// The SLOT SPACE, which is cl_blood_max - not this frame's droplet count, and
	// not MAX_BLOOD_SPHERES. Slots live as long as their droplet and are handed
	// out lowest-first, so the highest occupied slot has nothing to do with how
	// many are currently alive; and it must match what ensure_buffers allocated.
	return blood_layout(vkpt_blood_slot_capacity(), &ultra_count, &ultra_stride, &stride);
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
================
*/
static uint32_t choose_lods(const blood_sphere_t* spheres, int num_spheres, const vec3_t cam_pos)
{
	const int max_level = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));

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

		level = max(0, min(level, max_level));

		/*
		THE ULTRA LEVEL (pt_blood_tess 3) IS FOR DROPLETS IN FLIGHT, RIGHT IN
		FRONT OF THE CAMERA, and for nothing else.

		It was built for the airborne droplets, yet pt_blood_lod_air kept every
		one of those a level down while the SPLATS - which the bands above measure
		as several reference droplets wide - sat at 1280 faces far across the
		room. A floor of 2048 of them was the whole 90 -> 60 fps slide.

		So at tess 3: a splat tops out at tess 2's level and falls off from
		there; a droplet within pt_blood_ultra_dist (~3 feet) gets the full level,
		ignoring the in-flight bias, and past it is capped the same as a splat.
		25% hysteresis on the distance, for the cache's sake like the bands.
		*/
		if (max_level == BLOOD_SPHERE_MAX_SUBDIV)
		{
			if (is_splat)
				level = min(level, max_level - 1);
			else
			{
				float ultra = max(0.f, cvar_pt_blood_ultra_dist->value);
				if (prev && prev->valid && prev->level == max_level)
					ultra *= 1.25f;
				level = (dist_sq <= ultra * ultra) ? max_level : min(level, max_level - 1);
			}
		}

		sphere_lod[s] = (uint8_t)level;
		total += faces_for(spheres + s, level);
	}

	return total;
}

/*
================
assign_ultra_regions

Gives every droplet whose chosen level does not fit a normal region one of the
ultra regions (see ULTRA REGIONS), keeping the one it already holds so a parked
splat stays cached. Droplets that no longer need one release it first; when the
pool is still full, the rest are drawn one level down in their own slot.
================
*/
static void assign_ultra_regions(const blood_sphere_t* spheres, int num_spheres)
{
	if (!blood.ultra_count)
		return;
	if (!ultra_maps_ready)
		reset_ultra_maps();

	static bool wants[MAX_BLOOD_SPHERES];
	memset(wants, 0, sizeof(wants));

	for (int s = 0; s < num_spheres; s++)
	{
		const int slot = spheres[s].slot;
		if (spheres[s].radius <= 0.f || slot < 0 || slot >= MAX_BLOOD_SPHERES)
			continue;
		if ((uint32_t)faces_for(spheres + s, sphere_lod[s]) > blood.stride)
			wants[slot] = true;
	}

	for (int u = 0; u < blood.ultra_count; u++)
	{
		const int slot = slot_of_ultra[u];
		if (slot >= 0 && !wants[slot])
		{
			ultra_of_slot[slot] = -1;
			slot_of_ultra[u] = -1;
		}
	}

	int next_free = 0;
	for (int s = 0; s < num_spheres; s++)
	{
		const int slot = spheres[s].slot;
		if (spheres[s].radius <= 0.f || slot < 0 || slot >= MAX_BLOOD_SPHERES || !wants[slot])
			continue;
		if (ultra_of_slot[slot] >= 0)
			continue;

		while (next_free < blood.ultra_count && slot_of_ultra[next_free] >= 0)
			next_free++;

		if (next_free < blood.ultra_count)
		{
			slot_of_ultra[next_free] = (int16_t)slot;
			ultra_of_slot[slot] = (int16_t)next_free;
		}
		else
		{
			sphere_lod[s] = (uint8_t)max(0, (int)sphere_lod[s] - 1);
			wants[slot] = false;
		}
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
static uint32_t write_blood_geometry(const blood_sphere_t* spheres, int num_spheres, bsp_t* bsp)
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

	// Which slots were actually rewritten this frame. Uploading the whole used
	// range every frame meant pushing 25 MB over PCIe to change a few slots -
	// with stable slots almost everything is untouched, so only the slots that
	// changed need to travel.
	memset(blood.dirty_slot, 0, sizeof(blood.dirty_slot));

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

		slot_live[region] = true;
		if (region > highest_slot)
			highest_slot = region;

		const bool is_splat = is_splat_sphere(sphere);

		const int level = sphere_lod[s];
		const int faces = faces_for(sphere, level);

		const blood_face_t* tmpl = is_splat
			? puddle_template + puddle_lod_offset[level]
			: sphere_template + lod_offset[level];

		const uint32_t (*tmpl_normals)[3] = sphere_normals + lod_offset[level];
		const uint32_t (*tmpl_tangents)[3] = is_splat
			? puddle_tangents + puddle_lod_offset[level]
			: sphere_tangents + lod_offset[level];

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

				blood.dirty_slot[region] = true;

				blood.cache_moves++;
				continue;
			}
		}

		cache->valid = true;
		cache->blanked = false;
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
		const float splat_alpha = is_splat
			? max(0.05f, min(cvar_pt_blood_splat_alpha->value, 1.f)) : 1.f;
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
		if (is_splat && cvar_pt_blood_wobble_max->value > 0.f)
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
		static vec3_t   vert_pos[BLOOD_PUDDLE_MAX_VERTS];
		static uint32_t vert_nrm[BLOOD_PUDDLE_MAX_VERTS];

		const uint16_t (*face_vid)[3] = puddle_face_vid + puddle_lod_offset[level];

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
			// animated surface ripple. uv2.y is still spare.
			prim->uv2[0] = is_splat ? 1.f : 0.f;
			prim->uv2[1] = 0.f;

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
		if ((uint32_t)faces < cap)
		{
			const uint32_t tail = prim_index + (uint32_t)faces;
			const uint32_t count = cap - (uint32_t)faces;

			memset(blood.prim_shadow + tail, 0, sizeof(VboPrimitive) * count);
			memset(blood.pos_shadow + tail, 0, sizeof(prim_positions_t) * count);
		}

		// The stale primitives past the new mesh are cleared in the shadow copy
		// above, and blood_cache_entry_t::dev_span is what gets them cleared on
		// the GPU too: the upload covers max(faces, dev_span), so a droplet whose
		// mesh shrank sends the difference exactly once.
	}

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

		memset(blood.prim_shadow + region_offset(i), 0, sizeof(VboPrimitive) * region_cap(i));
		memset(blood.pos_shadow + region_offset(i), 0, sizeof(prim_positions_t) * region_cap(i));

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
vkpt_blood_update

Fills the blood section of the instanced buffers and reports how many primitives
it actually wrote.

Must run BEFORE vkpt_pt_create_all_dynamic, and the caller must already have
reserved vkpt_blood_prim_count() primitives at prim_offset.
================
*/
void vkpt_blood_update(
	VkCommandBuffer cmd_buf,
	const blood_sphere_t* spheres,
	int num_spheres,
	bsp_t* bsp,
	const vec3_t cam_pos,
	int model_instance_index,
	uint32_t prim_offset,
	uint32_t* prim_count_out)
{
	*prim_count_out = 0;
	blood.prim_count = 0;

	// A negative instance index means prepare_entities ran out of ModelInstance
	// slots. Every blood primitive names that instance, so there is nothing safe
	// to draw - drop the frame's droplets rather than index the UBO array out of
	// bounds.
	//
	// A frame that reserves nothing also ENDS THE EPOCH. The section's address is
	// not tracked across a frame it does not occupy, and whatever the device
	// holds for it belongs to a model by the time blood comes back.
	if (num_spheres <= 0 || model_instance_index < 0 || !cvar_pt_blood_spheres->integer)
	{
		blood.have_last_offset = false;
		return;
	}

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
	uint32_t needed = choose_lods(spheres, num_spheres, cam_pos);
	if (needed == 0)
	{
		blood.have_last_offset = false;
		return;
	}

	if (!ensure_buffers())
	{
		blood.have_last_offset = false;
		return;
	}

	assign_ultra_regions(spheres, num_spheres);

	if (needed > blood.max_prims)
	{
		// More droplets than the buffers were sized for. Cannot happen while
		// cl_blood_max is what sized them, but clamp rather than overrun if it
		// ever does - the tail simply does not draw this frame.
		needed = blood.max_prims;
	}

	blood.model_instance_index = model_instance_index;
	blood.frame_index = (blood.frame_index + 1) % MAX_FRAMES_IN_FLIGHT;

	const uint64_t gen_t0 = Sys_Microseconds();
	uint32_t prim_count = write_blood_geometry(spheres, num_spheres, bsp);
	const uint64_t gen_usec = Sys_Microseconds() - gen_t0;

	if (prim_count == 0)
	{
		blood.have_last_offset = false;
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
		const uint32_t total = (uint32_t)blood.ultra_count + (uint32_t)vkpt_blood_slot_capacity();
		while (slot_limit < total && slot_limit < BLOOD_MAX_REGIONS
			&& region_offset((int)slot_limit) + region_cap((int)slot_limit) <= limit)
			slot_limit++;
	}

	// AN EPOCH is a run of frames over which the section keeps the same address
	// in the same pair of instanced buffers. Inside one, the device's copy of an
	// untouched slot is still good. Across one it is not, and neither is anything
	// dev_span remembers - and note that vkpt_vertex_buffer_ensure_primbuf_size
	// DESTROYS and recreates those buffers, which is why the generation counter
	// is part of the test rather than just the offset.
	const uint32_t primbuf_gen = vkpt_primbuf_generation();

	const bool epoch_reset = blood.epoch_reset
		|| !blood.have_last_offset
		|| prim_offset != blood.last_prim_offset
		|| primbuf_gen != blood.primbuf_generation;

	blood.epoch_reset = false;
	blood.last_prim_offset = prim_offset;
	blood.have_last_offset = true;
	blood.primbuf_generation = primbuf_gen;

	if (epoch_reset)
	{
		// Zero the section on the GPU instead of sending zeros through staging.
		// After this every slot needs only its real faces uploaded, and a slot
		// holding nothing needs no upload at all.
		vkCmdFillBuffer(cmd_buf, qvk.buf_primitive_instanced.buffer,
			(VkDeviceSize)prim_offset * sizeof(VboPrimitive),
			(VkDeviceSize)prim_count * sizeof(VboPrimitive), 0);

		vkCmdFillBuffer(cmd_buf, qvk.buf_positions_instanced.buffer,
			(VkDeviceSize)prim_offset * sizeof(prim_positions_t),
			(VkDeviceSize)prim_count * sizeof(prim_positions_t), 0);

		for (uint32_t i = 0; i < BLOOD_MAX_REGIONS; i++)
		{
			blood_cache[i].dev_pending = true;

			// Past the filled range the device holds bytes that belong to
			// somebody else, so assume the worst for those slots until one is
			// written across its whole stride.
			blood_cache[i].dev_span = (i < slot_limit) ? 0 : (uint16_t)region_cap((int)i);
		}

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

	// One region per dirty slot. File scope rather than stack: 4096 slots is
	// 200 KB of VkBufferCopy across the two.
	static VkBufferCopy regions_prim[BLOOD_MAX_REGIONS];
	static VkBufferCopy regions_pos[BLOOD_MAX_REGIONS];
	uint32_t num_regions = 0;
	size_t up_prim_bytes = 0;
	size_t up_pos_bytes = 0;

	for (uint32_t i = 0; i < slot_limit; i++)
	{
		blood_cache_entry_t* c = blood_cache + i;

		if (!c->dev_pending)
			continue;

		// This slot's real geometry, plus however much of the device's older
		// content sticks out past it. Everything beyond that is already zero on
		// both sides.
		uint32_t n = max((uint32_t)c->faces, (uint32_t)c->dev_span);
		n = min(n, region_cap((int)i));

		c->dev_pending = false;
		c->dev_span = c->faces;

		if (n == 0)
			continue;

		const uint32_t lo = region_offset((int)i);

		// Neighbouring slots that both need their whole stride are one copy
		// rather than two - which is the common shape on an epoch frame.
		if (num_regions
			&& regions_prim[num_regions - 1].srcOffset + regions_prim[num_regions - 1].size
			   == (VkDeviceSize)lo * sizeof(VboPrimitive))
		{
			regions_prim[num_regions - 1].size += (VkDeviceSize)n * sizeof(VboPrimitive);
			regions_pos[num_regions - 1].size += (VkDeviceSize)n * sizeof(prim_positions_t);
		}
		else
		{
			regions_prim[num_regions] = (VkBufferCopy){
				(VkDeviceSize)lo * sizeof(VboPrimitive),
				(VkDeviceSize)(prim_offset + lo) * sizeof(VboPrimitive),
				(VkDeviceSize)n * sizeof(VboPrimitive) };

			regions_pos[num_regions] = (VkBufferCopy){
				(VkDeviceSize)lo * sizeof(prim_positions_t),
				(VkDeviceSize)(prim_offset + lo) * sizeof(prim_positions_t),
				(VkDeviceSize)n * sizeof(prim_positions_t) };

			num_regions++;
		}

		up_prim_bytes += (size_t)n * sizeof(VboPrimitive);
		up_pos_bytes += (size_t)n * sizeof(prim_positions_t);
	}

	// Nothing dirty is the common case for a settled floor, and then the whole
	// upload is dead work - the device still holds exactly this.
	const bool skip_upload = (num_regions == 0);

	blood.last_regions = num_regions;
	blood.last_epoch = epoch_reset;

	const uint64_t copy_t0 = Sys_Microseconds();
	if (!skip_upload)
	{
		for (uint32_t r = 0; r < num_regions; r++)
		{
			memcpy((char*)blood.mapped_prim[blood.frame_index] + regions_prim[r].srcOffset,
				(const char*)blood.prim_shadow + regions_prim[r].srcOffset, regions_prim[r].size);
			memcpy((char*)blood.mapped_pos[blood.frame_index] + regions_pos[r].srcOffset,
				(const char*)blood.pos_shadow + regions_pos[r].srcOffset, regions_pos[r].size);
		}
	}
	const uint64_t copy_usec = Sys_Microseconds() - copy_t0;

	if (!skip_upload && num_regions)
	{
		vkCmdCopyBuffer(cmd_buf, blood.staging_prim[blood.frame_index].buffer,
			qvk.buf_primitive_instanced.buffer, num_regions, regions_prim);

		vkCmdCopyBuffer(cmd_buf, blood.staging_pos[blood.frame_index].buffer,
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

	if (cvar_pt_blood_stats->integer)
	{
		static unsigned last_report = 0;
		unsigned now = Sys_Milliseconds();
		if (now - last_report > 1000)
		{
			last_report = now;
			Com_Printf("blood gpu-side: %d droplets (%d splats, %d cached, %d moved), %d tris (%.0f avg), gen %.2f ms + copy %.2f ms%s, %.2f MB in %d regions%s\n",
				blood.sphere_count, blood.splat_count, blood.cache_hits, blood.cache_moves, prim_count,
				blood.sphere_count ? (float)prim_count / blood.sphere_count : 0.f,
				gen_usec / 1000.f, copy_usec / 1000.f, skip_upload ? " (skipped)" : "",
				(float)(up_prim_bytes + up_pos_bytes) / (1024.f * 1024.f),
				blood.last_regions, blood.last_epoch ? " EPOCH" : "");
		}
	}

	blood.prim_count = prim_count;
	*prim_count_out = prim_count;
}
