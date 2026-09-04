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
// 0 -> 20 faces, 1 -> 80, 2 -> 320.
//
// The vertex normals are already smooth - on a unit sphere the normal IS the
// position, and primary_rays.rgen interpolates them across the triangle - so
// these droplets are smooth-shaded at any tessellation. What the triangle count
// buys is purely the SILHOUETTE, and that is what reads as "low poly": a 20-face
// icosahedron has a visibly polygonal outline once a droplet covers more than a
// few pixels. Hence 80 by default, with 320 available for close work.
//
// It is also the cost knob. Every per-frame expense scales linearly with it -
// the CPU generation loop, the host copy, and the dynamic BLAS rebuild, which
// happens every frame because the droplets move. The staging buffers are sized
// for MAX_BLOOD_SPHERES at the CURRENT tessellation and reallocated when it
// changes, so picking 320 costs memory only while it is selected.
#define BLOOD_SPHERE_MAX_SUBDIV 2
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
*/
#define BLOOD_PUDDLE_MAX_FACES (8*2*2 + 16*2*3 + 32*2*4)

static const int puddle_segments[BLOOD_SPHERE_MAX_SUBDIV + 1] = { 8, 16, 32 };
static const int puddle_rings[BLOOD_SPHERE_MAX_SUBDIV + 1]    = { 2,  3,  4 };

#define BLOOD_PUDDLE_FACES(level) (puddle_segments[level] * 2 * puddle_rings[level])

// All three LOD levels, laid end to end: level 0 at [0,20), level 1 at [20,100),
// level 2 at [100,420). Built once - the whole set is 420 faces, so there is no
// reason to rebuild when the tessellation changes, and holding all of them is
// what lets the level be picked PER DROPLET.
#define BLOOD_TEMPLATE_TOTAL (20 + 80 + 320)

static blood_face_t   sphere_template[BLOOD_TEMPLATE_TOTAL];
static uint32_t       sphere_normals[BLOOD_TEMPLATE_TOTAL][3];    // encode_normal of the above
static uint32_t       sphere_tangents[BLOOD_TEMPLATE_TOTAL][3];
static int            lod_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];    // first face of each level

static blood_face_t   puddle_template[BLOOD_PUDDLE_MAX_FACES];
static uint32_t       puddle_tangents[BLOOD_PUDDLE_MAX_FACES][3];
static int            puddle_lod_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];

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
} blood_cache_entry_t;

static blood_cache_entry_t blood_cache[MAX_BLOOD_SPHERES];

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
	uint32_t          stride;           // primitives reserved per droplet slot
	uint32_t          dirty_lo;         // primitive range rewritten this frame
	uint32_t          dirty_hi;
	bool              dirty_slot[MAX_BLOOD_SPHERES];
	uint32_t          last_prim_count;  // for detecting a frame that changed nothing
	uint32_t          last_prim_offset; // where the blood section sat last frame
	bool              have_last_offset;
	int               clean_frames;     // consecutive unchanged frames
	int               model_instance_index;
	bool              buffers_ready;
} blood;

// transparency.c - palette index or rgba to linear float RGB
extern void cast_u32_to_f32_color(int color_index, const color_t* pcolor, float* color_f32, float hdr_factor);
// bsp_mesh.c - the CPU port of utils.glsl's encode_normal
extern uint32_t encode_normal(const vec3_t normal);

static cvar_t* cvar_pt_blood_spheres = NULL;
static cvar_t* cvar_pt_blood_tess = NULL;
static cvar_t* cvar_pt_blood_stats = NULL;
static cvar_t* cvar_pt_blood_lod_near = NULL;
static cvar_t* cvar_pt_blood_lod_far = NULL;
static cvar_t* cvar_pt_blood_puddle_sink = NULL;
static cvar_t* cvar_pt_blood_wobble = NULL;

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
static bool ensure_buffers(void)
{
	const int max_droplets = max(1, cvar_pt_blood_spheres->integer
		? Cvar_Get("cl_blood_max", "512", CVAR_ARCHIVE)->integer : 1);
	const int subdiv = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));
	const int worst_faces = max(BLOOD_SPHERE_FACES(subdiv), BLOOD_PUDDLE_FACES(subdiv));

	const uint32_t needed = (uint32_t)max_droplets * (uint32_t)worst_faces;

	// Exact compare, not >=: the point is to reallocate ONLY when the settings
	// change, never in response to how much blood happens to be on screen.
	if (blood.buffers_ready && blood.max_prims == needed)
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

	blood.stride = (uint32_t)worst_faces;
	blood.max_prims = needed;
	blood.buffers_ready = true;

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

	// Distance LOD. Inside _near a droplet gets the full pt_blood_tess; past
	// _far it drops two levels; between them, one. Set _near very large to
	// disable the LOD and tessellate everything at full rate.
	cvar_pt_blood_lod_near = Cvar_Get("pt_blood_lod_near", "160", CVAR_ARCHIVE);
	cvar_pt_blood_lod_far = Cvar_Get("pt_blood_lod_far", "500", CVAR_ARCHIVE);

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
	int subdiv = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));

	// The larger of the two meshes, because which one each droplet gets is not
	// known until vkpt_blood_update picks it.
	const int worst = max(BLOOD_SPHERE_FACES(subdiv), BLOOD_PUDDLE_FACES(subdiv));

	return (uint32_t)MAX_BLOOD_SPHERES * (uint32_t)worst;
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

Keyed on position (via the angle), never on the face or vertex slot: the puddle's
vertices are shared between neighbouring faces, which store their own copies, so
anything face-local would give one physical vertex a different offset per face
and tear the mesh open along every edge.
================
*/
static inline float vertex_wobble(const vec3_t v, float seed, float amount, int segs)
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

	return 1.f + n * amount;
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

Seeding the cross product from whichever axis the normal is least aligned with
keeps it well conditioned; one component of a unit vector is always below
1/sqrt(3), so the seed is never closer than 54 degrees to the normal.
================
*/
static void splat_basis(const vec3_t normal, const vec3_t tangent, float radius,
                        float flatten, float stretch,
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
    }

    CrossProduct(normal, out_t1, out_t2);
    VectorNormalize(out_t2);

    // Spread used to be radius / sqrt(flatten), conserving the droplet's volume.
    // That coupling is wrong for a puddle and was why thinner splats got wider:
    // real blood spreading out does not keep its depth proportional to its width,
	// and it left flatness unable to be tuned without also changing size. The
    // puddle's width is now its own quantity, and flatten only sets its height.
    const float spread = radius * max(0.1f, global_blood_splat_size);

    // Elongate along the travel direction and narrow across it by the same
    // factor, so a directional mark covers the same area as the round splat it
    // replaces - a smear, not a bigger splat.
    stretch = max(1.f, stretch);
    const float inv = 1.f / stretch;

    VectorScale(out_t1, spread * stretch, out_t1);
    VectorScale(out_t2, spread * inv, out_t2);
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

	uint32_t total = 0;

	for (int s = 0; s < num_spheres; s++)
	{
		vec3_t delta;
		VectorSubtract(spheres[s].origin, cam_pos, delta);
		const float dist_sq = DotProduct(delta, delta);

		int level;
		if (dist_sq < near_sq)      level = max_level;
		else if (dist_sq < far_sq)  level = max_level - 1;
		else                        level = max_level - 2;

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

		sphere_lod[s] = (uint8_t)level;
		total += faces_for(spheres + s, level);
	}

	return total;
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

	const uint32_t stride = blood.stride;

	// Which slots this frame's droplets occupy. Anything else inside the used
	// range has to be blanked, or a dead droplet's leftovers stay in the
	// acceleration structure.
	static bool slot_live[MAX_BLOOD_SPHERES];
	memset(slot_live, 0, sizeof(slot_live));

	int highest_slot = -1;

	// The primitive range actually rewritten this frame. Uploading the whole used
	// range every frame meant pushing 25 MB over PCIe to change a few slots -
	// with stable slots almost everything is untouched, so only the span that
	// changed needs to travel.
	blood.dirty_lo = UINT32_MAX;
	blood.dirty_hi = 0;
	memset(blood.dirty_slot, 0, sizeof(blood.dirty_slot));

	for (int s = 0; s < num_spheres; s++)
	{
		const blood_sphere_t* sphere = spheres + s;

		if (sphere->radius <= 0.f)
			continue;

		if (sphere->slot < 0 || sphere->slot >= MAX_BLOOD_SPHERES)
			continue;

		slot_live[sphere->slot] = true;
		if (sphere->slot > highest_slot)
			highest_slot = sphere->slot;

		// A droplet's geometry always goes in its own slot, so it stays put for
		// the droplet's whole life however the array around it shifts.
		const uint32_t prim_index = (uint32_t)sphere->slot * stride;

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

		if (prim_index + (uint32_t)faces > blood.max_prims)
			continue;

		blood.sphere_count++;
		if (is_splat)
			blood.splat_count++;

		// Already in the shadow buffer, exactly as it needs to be? Then there is
		// nothing to do at all - see the comment on blood_cache_entry_t. With a
		// stable slot this is the fast path for any droplet that did not change
		// since last frame, not merely for a floor that has finished settling.
		blood_cache_entry_t* cache = blood_cache + sphere->slot;

		if (cache->valid
			&& cache->level == level
			&& memcmp(&cache->sphere, sphere, sizeof(blood_sphere_t)) == 0)
		{
			blood.cache_hits++;
			continue;
		}

		cache->valid = true;
		cache->blanked = false;
		cache->prim_offset = prim_index;
		cache->level = level;
		cache->sphere = *sphere;

		blood.dirty_lo = min(blood.dirty_lo, prim_index);
		blood.dirty_hi = max(blood.dirty_hi, prim_index + (uint32_t)faces);
		blood.dirty_slot[sphere->slot] = true;

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
				sphere->flatten, sphere->stretch, ax_t1, ax_t2, ax_n);

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

		// A puddle's z runs from 0 at its base to 1 at its apex, where a sphere's
		// spans the centre, so the two want different anchors: the puddle sits ON
		// the contact point, sunk just far enough to bury its rim.
		const float wobble = is_splat ? max(0.f, min(cvar_pt_blood_wobble->value, 0.9f)) : 0.f;

		vec3_t mesh_origin;
		VectorCopy(sphere->origin, mesh_origin);
		if (is_splat)
			VectorMA(mesh_origin, -cvar_pt_blood_puddle_sink->value, ax_n, mesh_origin);

		uint32_t write_index = prim_index;

		for (int f = 0; f < faces; f++)
		{
			VboPrimitive* prim = blood.prim_shadow + write_index;
			float (*pos)[3] = blood.pos_shadow[write_index];

			for (int i = 0; i < 3; i++)
			{
				if (is_splat)
				{
					const float* t = tmpl[f].pos[i];

					// Push the vertex in or out radially. Only the two in-plane
					// axes are scaled, so the puddle stays exactly as tall and
					// exactly as flat on the floor - it is the OUTLINE that goes
					// irregular, which is the part that reads as a splash. The
					// apex has zero radius and so cannot move, which keeps the
					// dome centred over its own base.
					const float w = vertex_wobble(t, sphere->seed, wobble, puddle_segments[level]);

					for (int a = 0; a < 3; a++)
						pos[i][a] = mesh_origin[a]
						          + ax_t1[a] * t[0] * w + ax_t2[a] * t[1] * w + ax_n[a] * t[2];
				}
				else
				{
					VectorMA(sphere->origin, sphere->radius, tmpl[f].pos[i], pos[i]);
				}
			}

			VectorCopy(pos[0], prim->pos0);
			VectorCopy(pos[1], prim->pos1);
			VectorCopy(pos[2], prim->pos2);

			prim->material_id = MATERIAL_KIND_BLOOD;
			prim->cluster = cluster;
			prim->texture_flags = 0;

			if (is_splat)
			{
				for (int i = 0; i < 3; i++)
				{
					const float* t = tmpl[f].nrm[i];
					vec3_t n;
					for (int a = 0; a < 3; a++)
						n[a] = inv_t1[a] * t[0] + inv_t2[a] * t[1] + inv_n[a] * t[2];
					VectorNormalize(n);
					prim->normals[i] = encode_normal(n);
				}
			}
			else
			{
				prim->normals[0] = tmpl_normals[f][0];
				prim->normals[1] = tmpl_normals[f][1];
				prim->normals[2] = tmpl_normals[f][2];
			}

			prim->tangents[0] = tmpl_tangents[f][0];
			prim->tangents[1] = tmpl_tangents[f][1];
			prim->tangents[2] = tmpl_tangents[f][2];

			// One ModelInstance backs the whole blood section. It exists only so
			// load_and_transform_triangle() can subtract render_prim_offset and
			// get a primitive index; nothing about the droplet is transformed by
			// it, because the positions above are already in world space.
			prim->instance = (uint32_t)blood.model_instance_index;

			prim->emissive_and_alpha = 0x3c003c00;  // (1.0, 1.0) as two halves

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
		if ((uint32_t)faces < stride)
		{
			const uint32_t tail = prim_index + (uint32_t)faces;
			const uint32_t count = stride - (uint32_t)faces;

			memset(blood.prim_shadow + tail, 0, sizeof(VboPrimitive) * count);
			memset(blood.pos_shadow + tail, 0, sizeof(prim_positions_t) * count);
		}

		// The dirty span has to cover the blanked tail as well, or the stale
		// primitives are cleared in the shadow copy and left alive on the GPU.
		blood.dirty_hi = max(blood.dirty_hi, prim_index + stride);
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

		memset(blood.prim_shadow + (size_t)i * stride, 0, sizeof(VboPrimitive) * stride);
		memset(blood.pos_shadow + (size_t)i * stride, 0, sizeof(prim_positions_t) * stride);

		blood_cache[i].valid = false;
		blood_cache[i].blanked = true;

		blood.dirty_lo = min(blood.dirty_lo, (uint32_t)i * stride);
		blood.dirty_hi = max(blood.dirty_hi, (uint32_t)(i + 1) * stride);
		blood.dirty_slot[i] = true;
	}

	// Everything past the used range is simply not covered by the build.
	for (int i = highest_slot + 1; i < MAX_BLOOD_SPHERES; i++)
	{
		blood_cache[i].valid = false;
		blood_cache[i].blanked = false;
	}

	return (uint32_t)(highest_slot + 1) * stride;
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
	if (num_spheres <= 0 || model_instance_index < 0 || !cvar_pt_blood_spheres->integer)
		return;

	if (!templates_built)
		build_sphere_templates();

	num_spheres = min(num_spheres, MAX_BLOOD_SPHERES);

	{
		cvar_t* sz = Cvar_Get("cl_blood_splat_size", "1.5", CVAR_ARCHIVE);
		global_blood_splat_size = sz->value;
	}

	// Pick each droplet's tessellation from its distance to the camera, and size
	// the buffers to what that actually needs rather than to the worst case. The
	// worst case - every droplet at full tessellation - is 512 * 320 primitives,
	// around 80 MB of staging and shadow memory that a typical frame nowhere near
	// uses. Growing to the high-water mark keeps the common case cheap without
	// capping the uncommon one.
	uint32_t needed = choose_lods(spheres, num_spheres, cam_pos);
	if (needed == 0)
		return;

	if (!ensure_buffers())
		return;

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
		return;

	const size_t prim_bytes = sizeof(VboPrimitive) * prim_count;
	const size_t pos_bytes = sizeof(prim_positions_t) * prim_count;

	// Nothing changed AND every staging buffer has already been given this exact
	// content? Then the device buffer still holds it too, and the whole upload is
	// dead work - several megabytes over PCIe every frame to write bytes that are
	// already there. A settled floor of splats hits this every frame.
	//
	// It is safe because the per-droplet cache validates the PRIM OFFSET as well
	// as the contents: if the blood section moved within the instanced buffer,
	// every droplet misses, clean_frames resets, and the copy happens.
	// THE SECTION CAN MOVE. blood_prim_offset is recomputed every frame, after
	// every model section, so it shifts whenever the world gains or loses an
	// animated entity. The per-slot cache only tracks a droplet's offset WITHIN
	// the section, so it cannot see that - and skipping the upload after a move
	// leaves every droplet's geometry sitting at the section's old address,
	// which by then belongs to a model. That is a two-way corruption: garbage
	// where the blood used to be, and a model's vertices read as blood.
	//
	// A move invalidates every byte already uploaded, so it forces a full copy.
	const bool section_moved = !blood.have_last_offset || prim_offset != blood.last_prim_offset;

	blood.last_prim_offset = prim_offset;
	blood.have_last_offset = true;

	// "Clean" here means the whole range has been uploaded at least once since
	// the last change of shape, which is what makes a partial upload safe.
	if (prim_count == blood.last_prim_count && !section_moved)
		blood.clean_frames++;
	else
		blood.clean_frames = 0;

	const bool nothing_changed = (blood.cache_hits == blood.sphere_count)
		&& (blood.dirty_lo >= blood.dirty_hi)
		&& !section_moved;

	blood.last_prim_count = prim_count;

	const bool skip_upload = nothing_changed && blood.clean_frames > MAX_FRAMES_IN_FLIGHT;

	// A staging buffer is only current for the frames since it last received a
	// full copy, so a partial upload is valid only once BOTH have seen the whole
	// range. Until then, copy everything.
	const bool partial = blood.clean_frames >= MAX_FRAMES_IN_FLIGHT
		&& blood.dirty_lo < blood.dirty_hi;

	// Which primitive runs to upload.
	//
	// A single lo..hi span is not enough. The droplets that change are the ones
	// still in the air, and their slots are scattered through a range otherwise
	// full of settled splats - so one span covering them covers nearly
	// everything, and a dozen moving droplets dragged the whole 12 MB across the
	// bus every frame. Merging only the slots that actually changed turns that
	// into a few kilobytes.
	#define BLOOD_MAX_COPY_REGIONS 64

	VkBufferCopy regions_prim[BLOOD_MAX_COPY_REGIONS];
	VkBufferCopy regions_pos[BLOOD_MAX_COPY_REGIONS];
	uint32_t num_regions = 0;
	size_t up_prim_bytes = 0;
	size_t up_pos_bytes = 0;

	// blood.stride, not the generation function's local - this is a different
	// scope.
	const uint32_t stride = blood.stride;
	const uint32_t slot_limit = stride ? min((uint32_t)MAX_BLOOD_SPHERES, prim_count / stride) : 0;

	if (!partial)
	{
		// First frames after a change of shape, or after the section moved: the
		// staging buffers have not all seen the full range, so send all of it.
		if (prim_count > 0)
		{
			regions_prim[0] = (VkBufferCopy){ 0, (VkDeviceSize)prim_offset * sizeof(VboPrimitive),
				sizeof(VboPrimitive) * prim_count };
			regions_pos[0] = (VkBufferCopy){ 0, (VkDeviceSize)prim_offset * sizeof(prim_positions_t),
				sizeof(prim_positions_t) * prim_count };
			num_regions = 1;
			up_prim_bytes = regions_prim[0].size;
			up_pos_bytes = regions_pos[0].size;
		}
	}
	else
	{
		for (uint32_t i = 0; i < slot_limit && num_regions < BLOOD_MAX_COPY_REGIONS; )
		{
			if (!blood.dirty_slot[i]) { i++; continue; }

			uint32_t run_start = i;
			while (i < slot_limit && blood.dirty_slot[i])
				i++;

			const uint32_t lo = run_start * stride;
			const uint32_t n = (i - run_start) * stride;

			regions_prim[num_regions] = (VkBufferCopy){
				(VkDeviceSize)lo * sizeof(VboPrimitive),
				(VkDeviceSize)(prim_offset + lo) * sizeof(VboPrimitive),
				sizeof(VboPrimitive) * n };

			regions_pos[num_regions] = (VkBufferCopy){
				(VkDeviceSize)lo * sizeof(prim_positions_t),
				(VkDeviceSize)(prim_offset + lo) * sizeof(prim_positions_t),
				sizeof(prim_positions_t) * n };

			up_prim_bytes += regions_prim[num_regions].size;
			up_pos_bytes += regions_pos[num_regions].size;
			num_regions++;
		}

		// Too fragmented to describe in the region budget - fall back to the one
		// span that certainly covers everything.
		if (num_regions == BLOOD_MAX_COPY_REGIONS && blood.dirty_hi > blood.dirty_lo)
		{
			const uint32_t lo = blood.dirty_lo;
			const uint32_t n = min(blood.dirty_hi, prim_count) - lo;

			regions_prim[0] = (VkBufferCopy){ (VkDeviceSize)lo * sizeof(VboPrimitive),
				(VkDeviceSize)(prim_offset + lo) * sizeof(VboPrimitive), sizeof(VboPrimitive) * n };
			regions_pos[0] = (VkBufferCopy){ (VkDeviceSize)lo * sizeof(prim_positions_t),
				(VkDeviceSize)(prim_offset + lo) * sizeof(prim_positions_t), sizeof(prim_positions_t) * n };
			num_regions = 1;
			up_prim_bytes = regions_prim[0].size;
			up_pos_bytes = regions_pos[0].size;
		}
	}

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

	// One barrier per buffer covering first..last region, rather than one per
	// region: the ranges are disjoint but a single span over them is still far
	// tighter than the whole buffer, and costs one barrier instead of sixty.
	VkDeviceSize barrier_prim_size = 0, barrier_pos_size = 0;
	if (num_regions)
	{
		const VkBufferCopy* lastp = &regions_prim[num_regions - 1];
		const VkBufferCopy* lastq = &regions_pos[num_regions - 1];
		barrier_prim_size = (lastp->dstOffset + lastp->size) - regions_prim[0].dstOffset;
		barrier_pos_size = (lastq->dstOffset + lastq->size) - regions_pos[0].dstOffset;
	}

	VkBufferMemoryBarrier barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_primitive_instanced.buffer,
			.offset = regions_prim[0].dstOffset,
			.size = barrier_prim_size,
		},
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_positions_instanced.buffer,
			.offset = regions_pos[0].dstOffset,
			.size = barrier_pos_size,
		}
	};

	if (!skip_upload && num_regions)
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
			Com_Printf("blood gpu-side: %d droplets (%d splats, %d cached), %d tris (%.0f avg), gen %.2f ms + copy %.2f ms%s, %.2f MB\n",
				blood.sphere_count, blood.splat_count, blood.cache_hits, prim_count,
				blood.sphere_count ? (float)prim_count / blood.sphere_count : 0.f,
				gen_usec / 1000.f, copy_usec / 1000.f, skip_upload ? " (skipped)" : "",
				(float)(up_prim_bytes + up_pos_bytes) / (1024.f * 1024.f));
		}
	}

	blood.prim_count = prim_count;
	*prim_count_out = prim_count;
}
