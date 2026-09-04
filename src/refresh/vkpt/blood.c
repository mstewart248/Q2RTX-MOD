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
	vec3_t pos[3];      // unit sphere; doubles as the smooth vertex normals
} blood_face_t;

// All three LOD levels, laid end to end: level 0 at [0,20), level 1 at [20,100),
// level 2 at [100,420). Built once - the whole set is 420 faces, so there is no
// reason to rebuild when the tessellation changes, and holding all of them is
// what lets the level be picked PER DROPLET.
#define BLOOD_TEMPLATE_TOTAL (20 + 80 + 320)

static blood_face_t   sphere_template[BLOOD_TEMPLATE_TOTAL];
static uint32_t       sphere_normals[BLOOD_TEMPLATE_TOTAL][3];    // encode_normal of the above
static uint32_t       sphere_tangents[BLOOD_TEMPLATE_TOTAL][3];
static int            lod_offset[BLOOD_SPHERE_MAX_SUBDIV + 1];    // first face of each level
static bool           templates_built = false;

// Per-droplet LOD level chosen by choose_lods(), reused by write_blood_geometry
// so the two cannot disagree about how many primitives a droplet needs.
static uint8_t        sphere_lod[MAX_BLOOD_SPHERES];

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
}

static bool ensure_buffers(uint32_t needed)
{
	// Grow only, never shrink: a firefight that once needed the room will very
	// likely need it again a second later, and reallocating costs a device idle.
	if (blood.buffers_ready && blood.max_prims >= needed)
		return true;

	// Round up so a burst that creeps up by one droplet a frame does not
	// reallocate every frame.
	needed = (needed + 8191u) & ~8191u;

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

	blood.max_prims = needed;
	blood.buffers_ready = true;
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

	// An UPPER BOUND, not the exact figure: the per-droplet LOD is not chosen
	// until vkpt_blood_update runs, and it can only ever reduce the count. This
	// reserves space in the instanced buffers, so over-reserving is harmless
	// (the BLAS is built from the count actually written) while under-reserving
	// would let the copy run past the end.
	int subdiv = max(0, min(cvar_pt_blood_tess->integer, BLOOD_SPHERE_MAX_SUBDIV));

	return min(num_spheres, MAX_BLOOD_SPHERES) * BLOOD_SPHERE_FACES(subdiv);
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

		level = max(0, min(level, max_level));

		sphere_lod[s] = (uint8_t)level;
		total += BLOOD_SPHERE_FACES(level);
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
	uint32_t prim_index = 0;
	blood.sphere_count = 0;

	for (int s = 0; s < num_spheres; s++)
	{
		const blood_sphere_t* sphere = spheres + s;

		if (sphere->radius <= 0.f)
			continue;

		const int level = sphere_lod[s];
		const int faces = BLOOD_SPHERE_FACES(level);
		const blood_face_t* tmpl = sphere_template + lod_offset[level];
		const uint32_t (*tmpl_normals)[3] = sphere_normals + lod_offset[level];
		const uint32_t (*tmpl_tangents)[3] = sphere_tangents + lod_offset[level];

		if (prim_index + (uint32_t)faces > blood.max_prims)
			break;

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

		for (int f = 0; f < faces; f++)
		{
			VboPrimitive* prim = blood.prim_shadow + prim_index;
			float (*pos)[3] = blood.pos_shadow[prim_index];

			for (int i = 0; i < 3; i++)
				VectorMA(sphere->origin, sphere->radius, tmpl[f].pos[i], pos[i]);

			VectorCopy(pos[0], prim->pos0);
			VectorCopy(pos[1], prim->pos1);
			VectorCopy(pos[2], prim->pos2);

			prim->material_id = MATERIAL_KIND_BLOOD;
			prim->cluster = cluster;
			prim->texture_flags = 0;

			prim->normals[0] = tmpl_normals[f][0];
			prim->normals[1] = tmpl_normals[f][1];
			prim->normals[2] = tmpl_normals[f][2];

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
			prim->uv2[0] = 0.f;
			prim->uv2[1] = 0.f;

			prim->custom0[0] = custom_xy;
			prim->custom0[1] = custom_zw;
			prim->custom1[0] = custom_xy;
			prim->custom1[1] = custom_zw;
			prim->custom2[0] = custom_xy;
			prim->custom2[1] = custom_zw;

			prim_index++;
		}

		blood.sphere_count++;
	}

	return prim_index;
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

	// Pick each droplet's tessellation from its distance to the camera, and size
	// the buffers to what that actually needs rather than to the worst case. The
	// worst case - every droplet at full tessellation - is 512 * 320 primitives,
	// around 80 MB of staging and shadow memory that a typical frame nowhere near
	// uses. Growing to the high-water mark keeps the common case cheap without
	// capping the uncommon one.
	uint32_t needed = choose_lods(spheres, num_spheres, cam_pos);
	if (needed == 0)
		return;

	if (!ensure_buffers(needed))
		return;

	blood.model_instance_index = model_instance_index;
	blood.frame_index = (blood.frame_index + 1) % MAX_FRAMES_IN_FLIGHT;

	uint32_t prim_count = write_blood_geometry(spheres, num_spheres, bsp);
	if (prim_count == 0)
		return;

	const size_t prim_bytes = sizeof(VboPrimitive) * prim_count;
	const size_t pos_bytes = sizeof(prim_positions_t) * prim_count;

	memcpy(blood.mapped_prim[blood.frame_index], blood.prim_shadow, prim_bytes);
	memcpy(blood.mapped_pos[blood.frame_index], blood.pos_shadow, pos_bytes);

	VkBufferCopy copy_prim = {
		.srcOffset = 0,
		.dstOffset = (VkDeviceSize)prim_offset * sizeof(VboPrimitive),
		.size = prim_bytes
	};
	vkCmdCopyBuffer(cmd_buf, blood.staging_prim[blood.frame_index].buffer,
		qvk.buf_primitive_instanced.buffer, 1, &copy_prim);

	VkBufferCopy copy_pos = {
		.srcOffset = 0,
		.dstOffset = (VkDeviceSize)prim_offset * sizeof(prim_positions_t),
		.size = pos_bytes
	};
	vkCmdCopyBuffer(cmd_buf, blood.staging_pos[blood.frame_index].buffer,
		qvk.buf_positions_instanced.buffer, 1, &copy_pos);

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

	VkBufferMemoryBarrier barriers[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_primitive_instanced.buffer,
			.offset = copy_prim.dstOffset,
			.size = prim_bytes,
		},
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.buffer = qvk.buf_positions_instanced.buffer,
			.offset = copy_pos.dstOffset,
			.size = pos_bytes,
		}
	};

	vkCmdPipelineBarrier(cmd_buf,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | shader_read_stages,
		0, 0, NULL, 2, barriers, 0, NULL);

	if (cvar_pt_blood_stats->integer)
	{
		static unsigned last_report = 0;
		unsigned now = Sys_Milliseconds();
		if (now - last_report > 1000)
		{
			last_report = now;
			Com_Printf("blood: %d droplets, %d tris (%.0f avg/droplet), %.2f MB/frame\n",
				blood.sphere_count, prim_count,
				blood.sphere_count ? (float)prim_count / blood.sphere_count : 0.f,
				(float)(prim_bytes + pos_bytes) / (1024.f * 1024.f));
		}
	}

	blood.prim_count = prim_count;
	*prim_count_out = prim_count;
}
