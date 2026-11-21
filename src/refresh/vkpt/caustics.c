/*
Copyright (C) 2026

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
PHOTON-TRACED CAUSTICS, THE CPU HALF (pt_caustics 2) - see shader/caustics.h.

Every frame this lists the EMITTERS: each (light, caustic glass) pair whose light
the photon pass is to carry through that glass, sorted by light index, with the
glass's bounding sphere and a share of pt_caustics_photons. It is the list, and
only the list, that decides which light goes by photons: the shaders take the
diffuse out of a shadow ray exactly where a listed emitter's sphere holds the
glass it crosses, so a pair left off for any reason - out of range, over
MAX_CAUSTIC_EMITTERS, a sky portal - simply keeps pt_caustics 1's factor.

CAUSTIC GLASS is kind GLASS with `caustic` or `caustic_ripple` above 0. Two places
have it:
  the world      grouped by prim_instance_hash (the connected faces of one
                 material: one kmlamp bulb, one window) and found again whenever
                 the map or the set of caustic materials changes
  models         an entity whose mesh draws with it (the health orbs), from
                 process_regular_entity every frame, by the frame's bounds
Spheres are generous - they must hold every bit of the glass, or that bit's light
goes nowhere: photons dropped as not their emitter's, shadow rays still blocked.

PHOTONS go to each pair in proportion to the light it sends at the glass over the
square of the glass's distance from the camera (a far caustic is drawn in bigger
cells, so it needs fewer photons for the same grain), at least
CAUSTIC_MIN_PHOTONS each. The count is rounded at random and the shader divides
by the unrounded EXPECTED count, so the light delivered is right on average.
*/

#include "vkpt.h"
#include "shader/vertex_buffer.h"
#include "material.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CAUSTIC_PI 3.14159265358979f

extern cvar_t *cvar_pt_caustics;
extern cvar_t *cvar_pt_caustics_photons;
extern cvar_t *cvar_pt_caustics_light_range;
extern cvar_t *cvar_pt_caustics_range;

#define CAUSTIC_MAX_WORLD_CASTERS   1024
#define CAUSTIC_MAX_MODEL_CASTERS   256
#define CAUSTIC_MAX_PER_LIGHT       64      // caustic_owner in path_tracer_rgen.h looks no further
#define CAUSTIC_MIN_PHOTONS         16.f
#define CAUSTIC_MAX_PHOTONS         (1u << 22)

typedef struct {
	vec3_t center;
	float radius;
} caustic_sphere_t;

static caustic_sphere_t world_casters[CAUSTIC_MAX_WORLD_CASTERS];
static int num_world_casters;
static const bsp_mesh_t *world_casters_mesh;
static uint32_t world_casters_prims;
static uint32_t world_casters_signature;

static caustic_sphere_t model_casters[CAUSTIC_MAX_MODEL_CASTERS];
static int num_model_casters;

static bool frame_active;
static uint32_t frame_photons;
static uint32_t frame_rng = 0x9e3779b9u;
static uint32_t frame_number;

static bool material_is_caustic(const pbr_material_t *mat)
{
	return mat && MAT_IsKind(mat->flags, MATERIAL_KIND_GLASS)
		&& (mat->glass_caustic > 0.f || mat->caustic_ripple > 0.f);
}

bool vkpt_caustics_material(const pbr_material_t *mat)
{
	return material_is_caustic(mat);
}

static float caustic_rand(void)
{
	frame_rng = frame_rng * 1664525u + 1013904223u;
	return (float)(frame_rng >> 8) * (1.f / 16777216.f);
}

// which materials are caustic glass, as one number: the world's casters are found
// again when it changes (`mat caustic 1` on a new texture, a .mat reload)
static uint32_t caustic_material_signature(void)
{
	uint32_t sig = 2166136261u;
	for (int i = 0; i <= MATERIAL_INDEX_MASK; i++)
	{
		const pbr_material_t *mat = MAT_ForIndex(i);
		if (material_is_caustic(mat))
			sig = (sig ^ (uint32_t)i) * 16777619u;
	}
	return sig;
}

static void find_world_casters(const bsp_mesh_t *wm)
{
	typedef struct { uint32_t hash; vec3_t mins, maxs; } group_t;
	static group_t groups[CAUSTIC_MAX_WORLD_CASTERS];
	int num_groups = 0;
	bool warned = false;

	num_world_casters = 0;

	for (uint32_t p = 0; p < wm->num_primitives; p++)
	{
		const VboPrimitive *prim = wm->primitives + p;
		const pbr_material_t *mat = MAT_ForIndex((int)(prim->material_id & MATERIAL_INDEX_MASK));
		if (!material_is_caustic(mat))
			continue;

		// 0 = not from a BSP face: a group of its own
		uint32_t hash = wm->prim_instance_hash ? wm->prim_instance_hash[p] : 0;

		int g = -1;
		if (hash != 0)
		{
			for (int i = 0; i < num_groups; i++)
			{
				if (groups[i].hash == hash)
				{
					g = i;
					break;
				}
			}
		}

		if (g < 0)
		{
			if (num_groups >= CAUSTIC_MAX_WORLD_CASTERS)
			{
				if (!warned)
					Com_WPrintf("pt_caustics 2: more than %d pieces of caustic glass in the map; the rest keep pt_caustics 1.\n",
						CAUSTIC_MAX_WORLD_CASTERS);
				warned = true;
				continue;
			}
			g = num_groups++;
			groups[g].hash = hash;
			VectorSet(groups[g].mins, 1e30f, 1e30f, 1e30f);
			VectorSet(groups[g].maxs, -1e30f, -1e30f, -1e30f);
		}

		const float *corners[3] = { prim->pos0, prim->pos1, prim->pos2 };
		for (int c = 0; c < 3; c++)
		{
			for (int a = 0; a < 3; a++)
			{
				groups[g].mins[a] = min(groups[g].mins[a], corners[c][a]);
				groups[g].maxs[a] = max(groups[g].maxs[a], corners[c][a]);
			}
		}
	}

	for (int g = 0; g < num_groups; g++)
	{
		caustic_sphere_t *s = world_casters + num_world_casters++;
		vec3_t half;
		for (int a = 0; a < 3; a++)
		{
			s->center[a] = 0.5f * (groups[g].mins[a] + groups[g].maxs[a]);
			half[a] = 0.5f * (groups[g].maxs[a] - groups[g].mins[a]);
		}
		s->radius = VectorLength(half) * 1.02f + 0.5f;
	}

	if (num_world_casters > 0)
		Com_DPrintf("pt_caustics 2: %d pieces of caustic glass in the map\n", num_world_casters);
}

void vkpt_caustics_begin_frame(void)
{
	num_model_casters = 0;
}

/* An entity drawing a caustic glass mesh (process_regular_entity, once per entity).
   transform is create_entity_matrix's, column-major. */
void vkpt_caustics_add_model(const entity_t *entity, const model_t *model, const float transform[16])
{
	if (!cvar_pt_caustics || cvar_pt_caustics->integer != 2)
		return;
	if (num_model_casters >= CAUSTIC_MAX_MODEL_CASTERS || !model->frames || model->numframes <= 0)
		return;

	int frame = entity->frame;
	int oldframe = entity->oldframe;
	if (frame < 0 || frame >= model->numframes) frame = 0;
	if (oldframe < 0 || oldframe >= model->numframes) oldframe = 0;

	// both frames' boxes, as the mesh is drawn somewhere between them
	vec3_t mins, maxs;
	for (int a = 0; a < 3; a++)
	{
		mins[a] = min(model->frames[frame].bounds[0][a], model->frames[oldframe].bounds[0][a]);
		maxs[a] = max(model->frames[frame].bounds[1][a], model->frames[oldframe].bounds[1][a]);
	}

	vec3_t local, half;
	for (int a = 0; a < 3; a++)
	{
		local[a] = 0.5f * (mins[a] + maxs[a]);
		half[a] = 0.5f * (maxs[a] - mins[a]);
	}

	// the largest scale of the three axes
	float sx = sqrtf(transform[0] * transform[0] + transform[1] * transform[1] + transform[2] * transform[2]);
	float sy = sqrtf(transform[4] * transform[4] + transform[5] * transform[5] + transform[6] * transform[6]);
	float sz = sqrtf(transform[8] * transform[8] + transform[9] * transform[9] + transform[10] * transform[10]);
	float scale = max(sx, max(sy, sz));

	caustic_sphere_t *s = model_casters + num_model_casters++;
	for (int a = 0; a < 3; a++)
		s->center[a] = transform[a] * local[0] + transform[4 + a] * local[1] + transform[8 + a] * local[2] + transform[12 + a];
	// models.c rounds the orbs onto a sphere a little outside their low-poly box
	s->radius = VectorLength(half) * scale * 1.1f + 1.f;
}

// Same as caustic_light_bounds in path_tracer_rgen.h.
static void light_bounds(const light_poly_t *light, vec3_t center, float *radius)
{
	if (light->type == DYNLIGHT_POLYGON)
	{
		for (int a = 0; a < 3; a++)
			center[a] = (light->positions[a] + light->positions[3 + a] + light->positions[6 + a]) * (1.f / 3.f);
		float r2 = 0.f;
		for (int v = 0; v < 3; v++)
		{
			vec3_t d;
			VectorSubtract(light->positions + 3 * v, center, d);
			r2 = max(r2, DotProduct(d, d));
		}
		*radius = sqrtf(r2);
	}
	else
	{
		VectorCopy(light->positions, center);
		*radius = light->positions[3];
	}
}

typedef struct {
	uint32_t light;
	int caster;         // into the frame's casters
	float importance;   // light sent at the glass / camera distance^2
} caustic_pair_t;

static caustic_pair_t pairs[MAX_CAUSTIC_EMITTERS * 4];
static int num_pairs;
static bool pairs_overflowed;

static const caustic_sphere_t *frame_casters[CAUSTIC_MAX_WORLD_CASTERS + CAUSTIC_MAX_MODEL_CASTERS];
static float frame_caster_cam_dist2[CAUSTIC_MAX_WORLD_CASTERS + CAUSTIC_MAX_MODEL_CASTERS];
static int num_frame_casters;

/* What the light sends into the glass's cone, in luminance - only to share the
   photons out, so near enough will do. Matches the shader's flux per light type
   (caustic_photons.rgen). 0 = it sends nothing that way. */
static float pair_power(const light_poly_t *light, float luminance, const vec3_t lc, float lr, const caustic_sphere_t *s)
{
	vec3_t to_glass;
	VectorSubtract(s->center, lc, to_glass);
	float dist = VectorLength(to_glass);
	bool inside = dist <= s->radius + lr;

	float sin_a = inside ? 1.f : (s->radius + lr) / dist;
	float cos_a = inside ? -1.f : sqrtf(max(0.f, 1.f - sin_a * sin_a));
	float solid_angle = 2.f * CAUSTIC_PI * (1.f - cos_a);
	vec3_t dir = { 0, 0, 1 };
	if (!inside)
		VectorScale(to_glass, 1.f / dist, dir);

	switch (light->type)
	{
	case DYNLIGHT_SPHERE:
		return 0.8f * luminance * lr * lr * solid_angle;

	case DYNLIGHT_SPOT:
		if (!inside)
		{
			// the glass's cone must reach into the beam
			vec3_t axis;
			VectorCopy(light->positions + 6, axis);
			if (VectorNormalize(axis) <= 0.f)
				return 0.f;
			float to_axis = acosf(max(-1.f, min(1.f, DotProduct(axis, dir))));
			float half_width = (light->spot_emission_profile == DYNLIGHT_SPOT_EMISSION_PROFILE_AXIS_ANGLE_TEXTURE)
				? light->positions[4]
				: acosf(max(-1.f, min(1.f, light->positions[4])));
			if (to_axis - asinf(min(sin_a, 1.f)) > half_width)
				return 0.f;
		}
		return 2.f * luminance * solid_angle;

	default: // DYNLIGHT_POLYGON, one-sided
	{
		vec3_t e1, e2, g;
		VectorSubtract(light->positions + 3, light->positions, e1);
		VectorSubtract(light->positions + 6, light->positions, e2);
		CrossProduct(e1, e2, g);
		float g_len = VectorLength(g);
		if (g_len <= 0.f)
			return 0.f;
		float area = 0.5f * g_len;
		if (inside)
			return 0.8f * CAUSTIC_PI * luminance * area;
		float c = DotProduct(g, dir) / g_len + sin_a; // the cone's best side
		if (c <= 0.f)
			return 0.f; // all of the glass is behind it
		c = min(c, 1.f);
		return luminance * area * solid_angle * c * sqrtf(c);
	}
	}
}

static void add_light_pairs(const light_poly_t *light, uint32_t gpu_index, float light_range)
{
	if (light->color[0] < 0.f)
		return; // a sky portal: the sky is not traced (pt_caustics 1 keeps it)
	if (light->type == DYNLIGHT_WINDOW)
		return; // stained glass's light behind a wall: no shape photons can leave

	float style = 1.f;
	if (light->style != 0 && vkpt_refdef.fd->lightstyles)
		style = max(0.f, min(2.f, vkpt_refdef.fd->lightstyles[light->style].white));
	float mat_scale = light->material ? light->material->emissive_factor * light->material->light_factor : 1.f;
	float luminance = (0.2126f * light->color[0] + 0.7152f * light->color[1] + 0.0722f * light->color[2]) * style * mat_scale;
	if (!(luminance > 0.f))
		return;

	vec3_t lc;
	float lr;
	light_bounds(light, lc, &lr);

	int per_light = 0;
	for (int k = 0; k < num_frame_casters && per_light < CAUSTIC_MAX_PER_LIGHT; k++)
	{
		const caustic_sphere_t *s = frame_casters[k];
		vec3_t d;
		VectorSubtract(s->center, lc, d);
		float gap = VectorLength(d) - s->radius - lr;
		if (gap > light_range)
			continue;

		float power = pair_power(light, luminance, lc, lr, s);
		if (!(power > 0.f))
			continue;

		if (num_pairs >= (int)LENGTH(pairs))
		{
			pairs_overflowed = true;
			return;
		}

		caustic_pair_t *pair = pairs + num_pairs++;
		pair->light = gpu_index;
		pair->caster = k;
		pair->importance = power / max(frame_caster_cam_dist2[k], 64.f * 64.f);
		per_light++;
	}
}

static int compare_importance_desc(const void *a, const void *b)
{
	float fa = *(const float *)a, fb = *(const float *)b;
	return (fa < fb) - (fa > fb);
}

/* Writes the frame's emitters into the light staging buffer. Called by
   vkpt_light_buffer_upload_to_staging once this frame's lights are known. */
void vkpt_caustics_build(LightBuffer *lbo, bool render_world, const bsp_mesh_t *wm,
	int num_model_lights, const light_poly_t *model_lights)
{
	frame_active = false;
	frame_photons = 0;
	num_pairs = 0;
	num_frame_casters = 0;
	frame_number++;

	bool want = render_world && cvar_pt_caustics && cvar_pt_caustics->integer == 2
		&& vkpt_refdef.fd && !(vkpt_refdef.fd->rdflags & RDF_NOWORLDMODEL);

	if (!want)
	{
		uint32_t header[4] = { 0, 0, 0, 0 };
		memcpy(lbo->caustic_header, header, sizeof(header));
		return;
	}

	// the map's caustic glass, found again if the map or the caustic materials changed
	uint32_t signature = caustic_material_signature();
	if (wm != world_casters_mesh || wm->num_primitives != world_casters_prims || signature != world_casters_signature)
	{
		find_world_casters(wm);
		world_casters_mesh = wm;
		world_casters_prims = wm->num_primitives;
		world_casters_signature = signature;
	}

	// the glass near enough to the camera to be worth photons
	const float *cam = vkpt_refdef.fd->vieworg;
	float range = max(cvar_pt_caustics_range->value, 0.f);
	for (int pass = 0; pass < 2; pass++)
	{
		const caustic_sphere_t *list = pass ? model_casters : world_casters;
		int count = pass ? num_model_casters : num_world_casters;
		for (int i = 0; i < count; i++)
		{
			vec3_t d;
			VectorSubtract(list[i].center, cam, d);
			float dist = VectorLength(d);
			if (dist - list[i].radius > range)
				continue;
			frame_casters[num_frame_casters] = list + i;
			frame_caster_cam_dist2[num_frame_casters] = dist * dist;
			num_frame_casters++;
		}
	}

	// the pairs, in light order - the order the light buffer has them
	float light_range = max(cvar_pt_caustics_light_range->value, 0.f);
	pairs_overflowed = false;
	if (num_frame_casters > 0)
	{
		int num_bsp = wm->num_light_polys;
		for (int i = 0; i < num_bsp && i < MAX_LIGHT_POLYS; i++)
			add_light_pairs(wm->light_polys + i, (uint32_t)i, light_range);
		for (int i = 0; i < num_model_lights && num_bsp + i < MAX_LIGHT_POLYS; i++)
			add_light_pairs(model_lights + i, (uint32_t)(num_bsp + i), light_range);
	}

	// over the emitter limit: keep the most important, in their light order
	float threshold = -1.f;
	if (num_pairs > MAX_CAUSTIC_EMITTERS || pairs_overflowed)
	{
		static float sorted[LENGTH(pairs)];
		for (int i = 0; i < num_pairs; i++)
			sorted[i] = pairs[i].importance;
		qsort(sorted, num_pairs, sizeof(float), compare_importance_desc);
		if (num_pairs > MAX_CAUSTIC_EMITTERS)
			threshold = sorted[MAX_CAUSTIC_EMITTERS - 1];

		static uint32_t warned_frame;
		if (warned_frame == 0 || frame_number - warned_frame > 600)
		{
			Com_DPrintf("pt_caustics 2: %d light/glass pairs, %d traced; the rest keep pt_caustics 1\n",
				num_pairs, MAX_CAUSTIC_EMITTERS);
			warned_frame = frame_number;
		}
	}

	double total_importance = 0.0;
	int num_kept = 0;
	for (int i = 0; i < num_pairs; i++)
	{
		if (threshold >= 0.f && pairs[i].importance < threshold)
			continue;
		if (num_kept >= MAX_CAUSTIC_EMITTERS)
			break;
		pairs[num_kept++] = pairs[i];
		total_importance += pairs[i].importance;
	}

	float budget = (float)max(0, min((int)CAUSTIC_MAX_PHOTONS, (int)cvar_pt_caustics_photons->value));
	frame_rng ^= frame_number * 0x85ebca6bu;

	uint32_t first = 0;
	int num_emitters = 0;
	if (budget > 0.f && total_importance > 0.0)
	{
		for (int i = 0; i < num_kept; i++)
		{
			const caustic_pair_t *pair = pairs + i;
			const caustic_sphere_t *s = frame_casters[pair->caster];

			float expected = max(CAUSTIC_MIN_PHOTONS, (float)(budget * (pair->importance / total_importance)));
			uint32_t count = (uint32_t)floorf(expected + caustic_rand());
			if (first + count > CAUSTIC_MAX_PHOTONS)
				count = 0; // never: the budget is capped well below

			float *e = lbo->caustic_emitters[num_emitters * CAUSTIC_EMITTER_VEC4S];
			e[0] = s->center[0];
			e[1] = s->center[1];
			e[2] = s->center[2];
			e[3] = s->radius;
			uint32_t info[3] = { pair->light, first, count };
			memcpy(e + 4, info, sizeof(info));
			e[7] = expected;

			first += count;
			num_emitters++;
		}
	}

	frame_photons = first;
	frame_active = true; // a grid lookup of nothing is 0 - and on every frame, so it clears as it goes

	uint32_t header[4] = { (uint32_t)num_emitters, first, 1, 0 };
	memcpy(lbo->caustic_header, header, sizeof(header));
}

// After vkpt_caustics_build: whether the photon pass runs this frame, and how many.
bool vkpt_caustics_frame(uint32_t *num_photons)
{
	*num_photons = frame_photons;
	return frame_active;
}
