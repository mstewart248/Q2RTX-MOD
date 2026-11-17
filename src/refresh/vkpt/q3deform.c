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

#include "vkpt.h"
#include "material.h"
#include "q3deform.h"
#include "common/q3shader.h"

/* QUAKE III SURFACES THAT MOVE (deformVertexes wave / bulge / move).

   Quake III moved these vertices on the CPU every frame (RB_DeformTessGeometry):
   q3dm4's spine tubes pulse along their length (bulge), banners and flags ripple
   (wave), the bot lamps bob (move). The path tracer's world is one static
   acceleration structure, so instead each such shader's world faces are taken
   out of it (bsp_mesh.c collect_surfaces hands their triangles here) and drawn
   as a MODEL built in memory: the deformation evaluated at a number of frames
   over one period and stored as vertex-animation frames, plus an entity each
   frame that picks the two frames either side of the time
   (vkpt_q3_deform_entities). The animated-model path does the rest, on both
   renderers: the per-frame acceleration structure, the motion vectors DLSS
   needs, the material.

   A model's meshes are one per (material, cluster): a model instance has ONE
   cluster for its light list (fill_model_instance), and a tube crossing a room
   crosses several.

   The shading follows the motion. Each frame's normal is the face's own plus
   how far the deformation has tipped the surface there - the smoothed face
   normal now minus the same at rest - which Quake III never did: its lightmap
   could not move.

   Collision is untouched; as in Quake III the deformation is only drawn. Not
   moved: liquids, lights, see-through surfaces, brush models. pt_q3_deform 0
   (read at map load) leaves them all in the world, still, as before. */

#ifndef Vector2Copy
#define Vector2Copy(a, b)       ((b)[0] = (a)[0], (b)[1] = (a)[1])
#endif

#define DEFORM_MAX_MODELS       16
#define DEFORM_MAX_MESHES       255     // entity_hash_t.mesh is 8 bits
#define DEFORM_STEPS_PER_CYCLE  16      // frames per period of the fastest wave
#define DEFORM_MIN_FRAMES       8
#define DEFORM_MAX_FRAMES       128
#define DEFORM_MAX_PERIOD       60.f    // seconds; past it two waves share no period
#define DEFORM_ENTITY_ID        0x3ffe  // entity_hash_t.entity is 14 bits; the model tells them apart
#define DEFORM_WELD_GRID        8.f     // positions closer than 1/8 unit smooth together

static cvar_t *cvar_pt_q3_deform = NULL;
static cvar_t *cvar_pt_q3_deform_mb = NULL;

typedef struct {
	vec3_t                  pos[3];
	vec2_t                  uv[3];
	vec3_t                  normal[3];
	vec3_t                  tangent[3];
	const pbr_material_t    *material;
	const q3shader_t        *shader;
	int                     cluster;
} deform_tri_t;

static deform_tri_t *s_tris = NULL;
static int s_num_tris = 0, s_alloc_tris = 0;
static bool s_collecting = false;

typedef struct {
	model_t                 *model;
	const maliasmesh_t      *meshes;    // what the slot held when built: still ours?
	float                   period;     // seconds the frames cover
	int                     *clusters;  // per mesh, in the model's hunk
} deform_model_t;

static deform_model_t s_models[DEFORM_MAX_MODELS];
static int s_num_models = 0;

static void register_cvars(void)
{
	if (cvar_pt_q3_deform)
		return;

	// 1 = Quake III surfaces with deformVertexes wave/bulge/move move; 0 = drawn
	// still in the world, as before. Read at map load.
	cvar_pt_q3_deform = Cvar_Get("pt_q3_deform", "1", 0);

	// Most memory one moving shader's frames may take on the GPU, in MB; fewer
	// frames (coarser motion) past it. Read at map load.
	cvar_pt_q3_deform_mb = Cvar_Get("pt_q3_deform_mb", "48", 0);
}

void vkpt_q3_deform_reset(void)
{
	// the models themselves go with the map, through MOD_FreeUnused
	memset(s_models, 0, sizeof(s_models));
	s_num_models = 0;

	Z_Free(s_tris);
	s_tris = NULL;
	s_num_tris = s_alloc_tris = 0;
	s_collecting = false;
}

void vkpt_q3_deform_begin(const bsp_t *bsp)
{
	register_cvars();
	vkpt_q3_deform_reset();
	s_collecting = bsp && bsp->q3 && cvar_pt_q3_deform->integer != 0;
}

/*
=================
THE DEFORMATIONS, as RB_DeformTessGeometry applies them
=================
*/

// The time one deform repeats over; 0 if it does not move.
static float deform_period(const q3deform_t *d)
{
	switch (d->type)
	{
	case Q3DEFORM_WAVE:
	case Q3DEFORM_MOVE:
		if (d->wave.func == Q3WAVE_NONE || d->wave.func == Q3WAVE_NOISE)
			return 0.f;
		if (d->wave.freq == 0.f || d->wave.amp == 0.f)
			return 0.f;
		if (d->type == Q3DEFORM_MOVE && VectorLength(d->move) == 0.f)
			return 0.f;
		return 1.f / fabsf(d->wave.freq);
	case Q3DEFORM_BULGE:
		if (d->bulge_speed == 0.f || d->bulge_height == 0.f)
			return 0.f;
		return 2.f * (float)M_PI / fabsf(d->bulge_speed);
	}
	return 0.f;
}

static bool shader_moves(const q3shader_t *q3)
{
	for (int i = 0; i < q3->num_deforms; i++)
		if (deform_period(&q3->deforms[i]) > 0.f)
			return true;
	return false;
}

/* The time every moving deform of the shader comes back round in, and the
   shortest single period among them. Two waves (protobanner: .2 and .7 Hz) share
   the shortest whole number of both cycles, up to 8 of the first (10 s there);
   with no such number the frames cover the slowest, and the faster one jumps a
   little where they loop. */
static float shader_period(const q3shader_t *q3, float *fastest)
{
	float period = 0.f, fast = 0.f;

	for (int i = 0; i < q3->num_deforms; i++)
	{
		const float p = deform_period(&q3->deforms[i]);
		if (p <= 0.f)
			continue;
		if (fast == 0.f || p < fast)
			fast = p;
		if (period == 0.f)
		{
			period = p;
			continue;
		}

		float common = 0.f;
		for (int m = 1; m <= 8 && m * period <= DEFORM_MAX_PERIOD; m++)
		{
			const float cycles = m * period / p;
			if (cycles >= 0.98f && fabsf(cycles - roundf(cycles)) < 0.02f)
			{
				common = m * period;
				break;
			}
		}
		period = (common > 0.f) ? common : max(period, p);
	}

	*fastest = fast;
	return period;
}

// A vertex moved by the shader's deforms at `time`, each reading the position
// the one before it left (as Quake III's run in place) and the rest normal.
static void deform_point(const q3shader_t *q3, const vec3_t in, const vec3_t normal, const vec2_t st,
                         float time, vec3_t out)
{
	VectorCopy(in, out);

	for (int i = 0; i < q3->num_deforms; i++)
	{
		const q3deform_t *d = &q3->deforms[i];

		switch (d->type)
		{
		case Q3DEFORM_WAVE: {
			// with no frequency Quake III takes the wave once for the whole
			// surface (EvalWaveForm), without the spread
			q3wave_t w = d->wave;
			if (w.freq != 0.f)
				w.phase += (out[0] + out[1] + out[2]) * d->spread;
			VectorMA(out, Q3Shader_EvalWave(&w, time), normal, out);
			break;
		}
		case Q3DEFORM_BULGE: {
			const float s = sinf(st[0] * d->bulge_width + time * d->bulge_speed) * d->bulge_height;
			VectorMA(out, s, normal, out);
			break;
		}
		case Q3DEFORM_MOVE:
			VectorMA(out, Q3Shader_EvalWave(&d->wave, time), d->move, out);
			break;
		}
	}
}

/*
=================
COLLECTING THE FACES
=================
*/

bool vkpt_q3_deform_wants(const pbr_material_t *material, uint32_t material_id, int surf_flags)
{
	if (!s_collecting || !material)
		return false;

	// a light would leave its light polys behind; a liquid, glass or see-through
	// face is drawn by paths a model does not take
	if (material_id & MATERIAL_FLAG_LIGHT)
		return false;
	if (surf_flags & (SURF_WARP | SURF_FLOWING | SURF_SKY | SURF_NODRAW))
		return false;
	if (!MAT_IsKind(material_id, MATERIAL_KIND_REGULAR) && !MAT_IsKind(material_id, MATERIAL_KIND_CHROME))
		return false;
	// the model draws with the material's own flags, so collect_surfaces must
	// not have changed the face's kind from them
	if ((material->flags & MATERIAL_KIND_MASK) != (material_id & MATERIAL_KIND_MASK))
		return false;

	const q3shader_t *q3 = MAT_Q3Shader(material);
	return q3 && shader_moves(q3);
}

// shader/utils.glsl decode_normal, the other half of bsp_mesh.c encode_normal
static void decode_normal(uint32_t enc, vec3_t n)
{
	float x = (float)(enc & 0xffffu) / 65535.f * 2.f - 1.f;
	float y = (float)(enc >> 16) / 65535.f * 2.f - 1.f;
	const float z = 1.f - fabsf(x) - fabsf(y);

	if (z < 0.f)
	{
		const float ox = x;
		x = (1.f - fabsf(y)) * (ox >= 0.f ? 1.f : -1.f);
		y = (1.f - fabsf(ox)) * (y >= 0.f ? 1.f : -1.f);
	}

	VectorSet(n, x, y, z);
	if (VectorNormalize(n) == 0.f)
		VectorSet(n, 0.f, 0.f, 1.f);
}

void vkpt_q3_deform_collect(const pbr_material_t *material, const VboPrimitive *prims, int count)
{
	if (!s_collecting || count <= 0)
		return;

	const q3shader_t *q3 = MAT_Q3Shader(material);
	if (!q3)
		return;

	if (s_num_tris + count > s_alloc_tris)
	{
		s_alloc_tris = max(s_alloc_tris * 2, s_num_tris + count + 1024);
		s_tris = Z_Realloc(s_tris, sizeof(deform_tri_t) * s_alloc_tris);
	}

	for (int i = 0; i < count; i++)
	{
		const VboPrimitive *p = prims + i;
		deform_tri_t *t = s_tris + s_num_tris++;

		VectorCopy(p->pos0, t->pos[0]);
		VectorCopy(p->pos1, t->pos[1]);
		VectorCopy(p->pos2, t->pos[2]);
		Vector2Copy(p->uv0, t->uv[0]);
		Vector2Copy(p->uv1, t->uv[1]);
		Vector2Copy(p->uv2, t->uv[2]);
		for (int c = 0; c < 3; c++)
		{
			decode_normal(p->normals[c], t->normal[c]);
			decode_normal(p->tangents[c], t->tangent[c]);
		}
		t->material = material;
		t->shader = q3;
		t->cluster = p->cluster;
	}
}

/*
=================
BUILDING THE MODELS
=================
*/

// One corner after welding: the mesh's vertex it became, and its smoothing group.
typedef struct {
	int         mesh;
	int         local;      // index in its mesh
	int         smooth;     // smoothing group
	vec3_t      pos, normal, tangent;
	vec2_t      uv;
} weld_vert_t;

typedef struct {
	const pbr_material_t *material;
	int         cluster;
	int         num_verts, num_tris;
} weld_mesh_t;

typedef struct {
	int         key[3];     // the position on the weld grid
	vec3_t      normal;     // the first corner's, for telling hard edges apart
	int         next;
} smooth_group_t;

static uint32_t hash_ints(const int *v, int n)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i++)
	{
		h ^= (uint32_t)v[i];
		h *= 16777619u;
	}
	return h ^ (h >> 15);
}

static uint32_t hash_floats(const float *v, int n, uint32_t h)
{
	for (int i = 0; i < n; i++)
	{
		uint32_t b;
		memcpy(&b, &v[i], sizeof(b));
		h ^= b;
		h *= 16777619u;
	}
	return h ^ (h >> 13);
}

static int table_size_for(int count)
{
	int size = 1024;
	while (size < count * 2)
		size <<= 1;
	return size;
}

static void build_model(const q3shader_t *q3, const int *tri_index, int num_tris)
{
	if (s_num_models >= DEFORM_MAX_MODELS)
	{
		Com_WPrintf("q3deform: more than %d moving shaders; %s drawn still\n", DEFORM_MAX_MODELS, q3->name);
		return;
	}

	float fastest = 0.f;
	const float period = shader_period(q3, &fastest);
	if (period <= 0.f || fastest <= 0.f)
		return;

	int frames = (int)ceilf(DEFORM_STEPS_PER_CYCLE * period / fastest);
	frames = max(DEFORM_MIN_FRAMES, min(DEFORM_MAX_FRAMES, frames));

	// the GPU keeps every frame of every triangle (VboPrimitive each)
	const double budget = max(1.0, (double)cvar_pt_q3_deform_mb->value) * 1024.0 * 1024.0;
	const double per_frame = (double)num_tris * sizeof(VboPrimitive);
	if (per_frame * frames > budget)
		frames = max(DEFORM_MIN_FRAMES, (int)(budget / per_frame));

	/* MESHES: one per (material, cluster). Past DEFORM_MAX_MESHES a triangle joins
	   the first mesh of its material, lit from that mesh's cluster. */
	weld_mesh_t *meshes = Z_Mallocz(sizeof(weld_mesh_t) * DEFORM_MAX_MESHES);
	int num_meshes = 0;
	int *tri_mesh = Z_Malloc(sizeof(int) * num_tris);

	for (int i = 0; i < num_tris; i++)
	{
		const deform_tri_t *t = s_tris + tri_index[i];
		int m, same_material = -1;
		for (m = 0; m < num_meshes; m++)
		{
			if (meshes[m].material != t->material)
				continue;
			if (same_material < 0)
				same_material = m;
			if (meshes[m].cluster == t->cluster)
				break;
		}
		if (m == num_meshes)
		{
			if (num_meshes < DEFORM_MAX_MESHES)
			{
				meshes[m].material = t->material;
				meshes[m].cluster = t->cluster;
				num_meshes++;
			}
			else
			{
				m = (same_material >= 0) ? same_material : 0;
			}
		}
		tri_mesh[i] = m;
		meshes[m].num_tris++;
	}

	/* WELDING. A mesh's vertex is a corner with the same position, texture
	   coordinate, normal and tangent (bit for bit: a curve's tessellated corners
	   are shared exactly). A smoothing group is a position on the weld grid with
	   normals within ~37 degrees - so a hard edge keeps two - and it is what a
	   frame's face normals are averaged over, across meshes. */
	const int max_verts = num_tris * 3;
	weld_vert_t *verts = Z_Malloc(sizeof(weld_vert_t) * max_verts);
	int num_verts = 0;
	int *corner_vert = Z_Malloc(sizeof(int) * max_verts);

	const int vert_table_size = table_size_for(max_verts);
	int *vert_head = Z_Malloc(sizeof(int) * vert_table_size);
	int *vert_next = Z_Malloc(sizeof(int) * max_verts);
	memset(vert_head, 0xff, sizeof(int) * vert_table_size);

	smooth_group_t *groups = Z_Malloc(sizeof(smooth_group_t) * max_verts);
	int num_groups = 0;
	int *group_head = Z_Malloc(sizeof(int) * vert_table_size);
	memset(group_head, 0xff, sizeof(int) * vert_table_size);

	for (int i = 0; i < num_tris; i++)
	{
		const deform_tri_t *t = s_tris + tri_index[i];
		const int m = tri_mesh[i];

		for (int c = 0; c < 3; c++)
		{
			uint32_t h = hash_floats(t->pos[c], 3, 2166136261u ^ (uint32_t)m);
			h = hash_floats(t->uv[c], 2, h);
			h = hash_floats(t->normal[c], 3, h);
			h = hash_floats(t->tangent[c], 3, h);
			const int bucket = (int)(h & (uint32_t)(vert_table_size - 1));

			int v;
			for (v = vert_head[bucket]; v >= 0; v = vert_next[v])
			{
				const weld_vert_t *w = verts + v;
				if (w->mesh == m && VectorCompare(w->pos, t->pos[c]) && w->uv[0] == t->uv[c][0] && w->uv[1] == t->uv[c][1]
					&& VectorCompare(w->normal, t->normal[c]) && VectorCompare(w->tangent, t->tangent[c]))
					break;
			}

			if (v < 0)
			{
				v = num_verts++;
				weld_vert_t *w = verts + v;
				w->mesh = m;
				w->local = meshes[m].num_verts++;
				VectorCopy(t->pos[c], w->pos);
				VectorCopy(t->normal[c], w->normal);
				VectorCopy(t->tangent[c], w->tangent);
				Vector2Copy(t->uv[c], w->uv);
				vert_next[v] = vert_head[bucket];
				vert_head[bucket] = v;

				// its smoothing group
				int key[3];
				for (int k = 0; k < 3; k++)
					key[k] = (int)floorf(w->pos[k] * DEFORM_WELD_GRID + 0.5f);
				const int gbucket = (int)(hash_ints(key, 3) & (uint32_t)(vert_table_size - 1));
				int g;
				for (g = group_head[gbucket]; g >= 0; g = groups[g].next)
				{
					if (groups[g].key[0] == key[0] && groups[g].key[1] == key[1] && groups[g].key[2] == key[2]
						&& DotProduct(groups[g].normal, w->normal) > 0.8f)
						break;
				}
				if (g < 0)
				{
					g = num_groups++;
					VectorCopy(key, groups[g].key);
					VectorCopy(w->normal, groups[g].normal);
					groups[g].next = group_head[gbucket];
					group_head[gbucket] = g;
				}
				w->smooth = g;
			}

			corner_vert[i * 3 + c] = v;
		}
	}

	Z_Free(vert_head);
	Z_Free(vert_next);
	Z_Free(group_head);

	/* THE MODEL, its hunk sized exactly (each allocation is rounded to 64). */
	size_t hunk = 4096;
	hunk += ALIGN(sizeof(maliasmesh_t) * num_meshes, 64);
	hunk += ALIGN(sizeof(maliasframe_t) * frames, 64);
	hunk += ALIGN(sizeof(int) * num_meshes, 64);
	for (int m = 0; m < num_meshes; m++)
	{
		const size_t nv = (size_t)meshes[m].num_verts * frames;
		hunk += ALIGN(sizeof(int) * meshes[m].num_tris * 3, 64);
		hunk += ALIGN(sizeof(vec3_t) * nv, 64) * 3;      // positions, normals, tangents
		hunk += ALIGN(sizeof(vec2_t) * nv, 64);          // tex_coords
	}

	char name[MAX_QPATH];
	const char *short_name = q3->name;
	if (!Q_strncasecmp(short_name, "textures/", 9))
		short_name += 9;
	Q_concat(name, sizeof(name), "q3deform/", short_name);

	model_t *model = MOD_AllocProcedural(name, hunk);
	if (!model)
	{
		Com_WPrintf("q3deform: no free model slot for %s; not drawn\n", q3->name);
		goto done;
	}
	if (model - r_models >= MAX_MODELS)
	{
		// past the slots that get a vertex buffer (model_vertex_data)
		Com_WPrintf("q3deform: model slot %d for %s has no vertex buffer; not drawn\n",
			(int)(model - r_models), q3->name);
		goto fail;
	}

	model->numframes = frames;
	model->nummeshes = num_meshes;
	model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * num_meshes);
	model->frames = MOD_Malloc(sizeof(maliasframe_t) * frames);
	int *clusters = MOD_Malloc(sizeof(int) * num_meshes);
	if (!model->meshes || !model->frames || !clusters)
		goto fail;

	for (int m = 0; m < num_meshes; m++)
	{
		maliasmesh_t *mesh = model->meshes + m;
		const size_t nv = (size_t)meshes[m].num_verts * frames;

		mesh->numverts = meshes[m].num_verts;
		mesh->numtris = meshes[m].num_tris;
		mesh->numindices = meshes[m].num_tris * 3;
		mesh->indices = MOD_Malloc(sizeof(int) * mesh->numindices);
		mesh->positions = MOD_Malloc(sizeof(vec3_t) * nv);
		mesh->normals = MOD_Malloc(sizeof(vec3_t) * nv);
		mesh->tangents = MOD_Malloc(sizeof(vec3_t) * nv);
		mesh->tex_coords = MOD_Malloc(sizeof(vec2_t) * nv);
		if (!mesh->indices || !mesh->positions || !mesh->normals || !mesh->tangents || !mesh->tex_coords)
			goto fail;
		mesh->materials[0] = (pbr_material_t *)meshes[m].material;
		mesh->numskins = 1;
		clusters[m] = meshes[m].cluster;
		meshes[m].num_tris = 0;     // counts again as the indices go in
	}

	// the triangles, in the world's winding
	for (int i = 0; i < num_tris; i++)
	{
		const int m = tri_mesh[i];
		maliasmesh_t *mesh = model->meshes + m;
		int *idx = mesh->indices + meshes[m].num_tris * 3;
		for (int c = 0; c < 3; c++)
			idx[c] = verts[corner_vert[i * 3 + c]].local;
		meshes[m].num_tris++;
	}

	/* THE FRAMES. Frame k is the deformation at k/frames of the period. Each
	   triangle's face normal is turned to the side its corners' normals face
	   (decided at rest, kept for every frame), summed - area-weighted - into its
	   corners' smoothing groups; a corner's normal is then its own plus how far
	   its group has turned from rest. */
	{
		vec3_t *moved = Z_Malloc(sizeof(vec3_t) * num_verts);
		vec3_t *rest_sum = Z_Mallocz(sizeof(vec3_t) * num_groups);
		vec3_t *sum = Z_Malloc(sizeof(vec3_t) * num_groups);
		float *side = Z_Malloc(sizeof(float) * num_tris);

		for (int i = 0; i < num_tris; i++)
		{
			const weld_vert_t *a = verts + corner_vert[i * 3 + 0];
			const weld_vert_t *b = verts + corner_vert[i * 3 + 1];
			const weld_vert_t *c = verts + corner_vert[i * 3 + 2];
			vec3_t e1, e2, fn, vn;
			VectorSubtract(b->pos, a->pos, e1);
			VectorSubtract(c->pos, a->pos, e2);
			CrossProduct(e1, e2, fn);
			VectorAdd(a->normal, b->normal, vn);
			VectorAdd(vn, c->normal, vn);
			side[i] = (DotProduct(fn, vn) >= 0.f) ? 1.f : -1.f;
			VectorScale(fn, side[i], fn);
			VectorAdd(rest_sum[a->smooth], fn, rest_sum[a->smooth]);
			VectorAdd(rest_sum[b->smooth], fn, rest_sum[b->smooth]);
			VectorAdd(rest_sum[c->smooth], fn, rest_sum[c->smooth]);
		}
		for (int g = 0; g < num_groups; g++)
			VectorNormalize(rest_sum[g]);

		for (int f = 0; f < frames; f++)
		{
			const float time = period * (float)f / (float)frames;
			maliasframe_t *frame = model->frames + f;
			ClearBounds(frame->bounds[0], frame->bounds[1]);
			VectorSet(frame->scale, 1.f, 1.f, 1.f);

			for (int v = 0; v < num_verts; v++)
				deform_point(q3, verts[v].pos, verts[v].normal, verts[v].uv, time, moved[v]);

			memset(sum, 0, sizeof(vec3_t) * num_groups);
			for (int i = 0; i < num_tris; i++)
			{
				const int ia = corner_vert[i * 3 + 0], ib = corner_vert[i * 3 + 1], ic = corner_vert[i * 3 + 2];
				vec3_t e1, e2, fn;
				VectorSubtract(moved[ib], moved[ia], e1);
				VectorSubtract(moved[ic], moved[ia], e2);
				CrossProduct(e1, e2, fn);
				VectorScale(fn, side[i], fn);
				VectorAdd(sum[verts[ia].smooth], fn, sum[verts[ia].smooth]);
				VectorAdd(sum[verts[ib].smooth], fn, sum[verts[ib].smooth]);
				VectorAdd(sum[verts[ic].smooth], fn, sum[verts[ic].smooth]);
			}
			for (int g = 0; g < num_groups; g++)
				VectorNormalize(sum[g]);

			for (int v = 0; v < num_verts; v++)
			{
				const weld_vert_t *w = verts + v;
				maliasmesh_t *mesh = model->meshes + w->mesh;
				const int at = f * mesh->numverts + w->local;

				vec3_t n, t;
				VectorSubtract(sum[w->smooth], rest_sum[w->smooth], n);
				VectorAdd(n, w->normal, n);
				if (VectorNormalize(n) == 0.f)
					VectorCopy(w->normal, n);

				VectorMA(w->tangent, -DotProduct(w->tangent, n), n, t);
				if (VectorNormalize(t) == 0.f)
					VectorCopy(w->tangent, t);

				VectorCopy(moved[v], mesh->positions[at]);
				VectorCopy(n, mesh->normals[at]);
				VectorCopy(t, mesh->tangents[at]);
				Vector2Copy(w->uv, mesh->tex_coords[at]);
				AddPointToBounds(moved[v], frame->bounds[0], frame->bounds[1]);
			}

			frame->radius = RadiusFromBounds(frame->bounds[0], frame->bounds[1]);
		}

		Z_Free(moved);
		Z_Free(rest_sum);
		Z_Free(sum);
		Z_Free(side);
	}

	Hunk_End(&model->hunk);

	deform_model_t *dm = s_models + s_num_models++;
	dm->model = model;
	dm->meshes = model->meshes;
	dm->period = period;
	dm->clusters = clusters;

	Com_DPrintf("q3deform: %s: %d triangles, %d meshes, %d frames over %.2f s, %.1f MB\n",
		q3->name, num_tris, num_meshes, frames, period, per_frame * frames / (1024.0 * 1024.0));
	goto done;

fail:
	if (model - r_models < MAX_MODELS)
		Com_WPrintf("q3deform: out of memory building %s; not drawn\n", q3->name);
	Hunk_Free(&model->hunk);
	memset(model, 0, sizeof(*model));

done:
	Z_Free(meshes);
	Z_Free(tri_mesh);
	Z_Free(verts);
	Z_Free(corner_vert);
	Z_Free(groups);
}

void vkpt_q3_deform_build(void)
{
	if (!s_collecting)
		return;
	s_collecting = false;

	if (s_num_tris > 0)
	{
		int *order = Z_Malloc(sizeof(int) * s_num_tris);
		bool *taken = Z_Mallocz(sizeof(bool) * s_num_tris);
		int moved = 0;

		// one model per shader, its triangles in the order they came
		for (int i = 0; i < s_num_tris; i++)
		{
			if (taken[i])
				continue;
			const q3shader_t *q3 = s_tris[i].shader;
			int n = 0;
			for (int j = i; j < s_num_tris; j++)
			{
				if (!taken[j] && s_tris[j].shader == q3)
				{
					taken[j] = true;
					order[n++] = j;
				}
			}
			build_model(q3, order, n);
			moved += n;
		}

		Com_Printf("Quake III deformVertexes: %d triangles move, as %d model(s)\n", moved, s_num_models);

		Z_Free(order);
		Z_Free(taken);
	}

	Z_Free(s_tris);
	s_tris = NULL;
	s_num_tris = s_alloc_tris = 0;
}

/*
=================
EVERY FRAME
=================
*/

static bool model_alive(const deform_model_t *dm)
{
	return dm->model && dm->model->type == MOD_ALIAS && dm->model->meshes == dm->meshes;
}

const entity_t *vkpt_q3_deform_entities(const entity_t *entities, int *num_entities, float time)
{
	static entity_t frame_entities[MAX_ENTITIES];
	static int frame_base = 0;     // how many of them are the client's

	int n;
	bool copied;
	if (entities == frame_entities)
	{
		// the client kept last frame's list (an invalid frame leaves cl.refdef
		// as it was): ours were appended to it, so pick them again from scratch
		n = frame_base;
		copied = true;
	}
	else
	{
		if (s_num_models == 0)
			return entities;
		n = min(*num_entities, MAX_ENTITIES - DEFORM_MAX_MODELS);
		copied = false;
	}

	for (int i = 0; i < s_num_models; i++)
	{
		const deform_model_t *dm = s_models + i;
		if (!model_alive(dm))
			continue;

		if (!copied)
		{
			memcpy(frame_entities, entities, sizeof(entity_t) * n);
			frame_base = n;
			copied = true;
		}

		const int frames = dm->model->numframes;
		double cycles = (double)time / (double)dm->period;
		cycles -= floor(cycles);
		const double at = cycles * frames;
		int k = (int)at;
		if (k >= frames)
			k = frames - 1;

		entity_t *e = frame_entities + n++;
		memset(e, 0, sizeof(*e));
		e->model = (qhandle_t)(dm->model - r_models) + 1;
		e->oldframe = k;
		e->frame = (k + 1) % frames;
		e->backlerp = 1.f - (float)(at - k);   // 0 = frame, 1 = oldframe
		e->alpha = 1.f;
		e->scale = 1.f;
		e->id = DEFORM_ENTITY_ID;
	}

	if (!copied)
		return entities;

	*num_entities = n;
	return frame_entities;
}

bool vkpt_q3_deform_mesh_cluster(const model_t *model, const maliasmesh_t *mesh, int *cluster)
{
	for (int i = 0; i < s_num_models; i++)
	{
		const deform_model_t *dm = s_models + i;
		if (dm->model != model || !model_alive(dm))
			continue;
		const ptrdiff_t m = mesh - model->meshes;
		if (m < 0 || m >= model->nummeshes)
			return false;
		*cluster = dm->clusters[m];
		return true;
	}
	return false;
}
