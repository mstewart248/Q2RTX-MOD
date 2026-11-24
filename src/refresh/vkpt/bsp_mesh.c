/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

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
#include "shader/global_textures.h"
#include "material.h"
#include "q3stages.h"
#include "q3deform.h"
#include "cameras.h"
#include "conversion.h"
#include "color.h"

#include <assert.h>
#include <float.h>

#define TINYOBJ_LOADER_C_IMPLEMENTATION
#include <tinyobj_loader_c.h>

extern cvar_t *cvar_pt_enable_nodraw;
extern cvar_t *cvar_pt_enable_surface_lights;
extern cvar_t *cvar_pt_enable_surface_lights_warp;
extern cvar_t* cvar_pt_bsp_radiance_scale;
extern cvar_t *cvar_pt_light_style_reach;
extern cvar_t *cvar_pt_bsp_sky_lights;
extern cvar_t *cvar_pt_q1_sky_lights;

// A Quake 1 map's sky face is a sky light (pt_q1_sky_lights, main.c)
static inline bool q1_sky_light(const bsp_t *bsp, int surf_flags)
{
	return bsp->q1 && (surf_flags & SURF_SKY) && cvar_pt_q1_sky_lights && cvar_pt_q1_sky_lights->integer;
}
extern cvar_t *cvar_pt_fog_lava;

static void
remove_collinear_edges(float* positions, float* tex_coords, mbasis_t* bases, int* num_vertices)
{
	int num_vertices_local = *num_vertices;

	for (int i = 1; i < num_vertices_local;)
	{
		float* p0 = positions + (i - 1) * 3;
		float* p1 = positions + (i % num_vertices_local) * 3;
		float* p2 = positions + ((i + 1) % num_vertices_local) * 3;

		vec3_t e1, e2;
		VectorSubtract(p1, p0, e1);
		VectorSubtract(p2, p1, e2);
		float l1 = VectorLength(e1);
		float l2 = VectorLength(e2);

		bool remove = false;
		if (l1 == 0)
		{
			remove = true;
		}
		else if (l2 > 0)
		{
			VectorScale(e1, 1.f / l1, e1);
			VectorScale(e2, 1.f / l2, e2);

			float dot = DotProduct(e1, e2);
			if (dot > 0.999f)
				remove = true;
		}

		if (remove)
		{
			if (num_vertices_local - i >= 1)
			{
				memcpy(p1, p2, (num_vertices_local - i - 1) * 3 * sizeof(float));

				if (tex_coords)
				{
					float* t1 = tex_coords + (i % num_vertices_local) * 2;
					float* t2 = tex_coords + ((i + 1) % num_vertices_local) * 2;
					memcpy(t1, t2, (num_vertices_local - i - 1) * 2 * sizeof(float));
				}

				if (bases)
				{
					mbasis_t* b1 = bases + (i % num_vertices_local);
					mbasis_t* b2 = bases + ((i + 1) % num_vertices_local);
					memcpy(b1, b2, (num_vertices_local - i - 1) * sizeof(mbasis_t));
				}
			}

			num_vertices_local--;
		}
		else
		{
			i++;
		}
	}

	*num_vertices = num_vertices_local;
}

// direct port of the encode_normal function from utils.glsl
uint32_t
encode_normal(const vec3_t normal)
{
	float invL1Norm = 1.0f / (fabsf(normal[0]) + fabsf(normal[1]) + fabsf(normal[2]));

	vec2_t p = { normal[0] * invL1Norm, normal[1] * invL1Norm };
	vec2_t pp = { p[0], p[1] };

	if (normal[2] < 0.f)
	{
		pp[0] = (1.f - fabsf(p[1])) * ((p[0] >= 0.f) ? 1.f : -1.f);
		pp[1] = (1.f - fabsf(p[0])) * ((p[1] >= 0.f) ? 1.f : -1.f);
	}

	pp[0] = pp[0] * 0.5f + 0.5f;
	pp[1] = pp[1] * 0.5f + 0.5f;

	clamp(pp[0], 0.f, 1.f);
	clamp(pp[1], 0.f, 1.f);

	uint32_t ux = (uint32_t)(pp[0] * 0xffffu);
	uint32_t uy = (uint32_t)(pp[1] * 0xffffu);

	return ux | (uy << 16);
}

// Compute emissive factor for a surface
/* `material` is the face's EFFECTIVE material - see face_effective_material.
   Passed in rather than read off the texinfo because bsp_radiance and
   default_radiance are exactly the kind of thing a per-surface instance exists
   to change, and taking them from the texture's material would ignore it. */
static float
compute_emissive(mtexinfo_t *texinfo, const pbr_material_t *material)
{
	if(!material)
		return 1.f;

	const float bsp_emissive = (float)texinfo->radiance * cvar_pt_bsp_radiance_scale->value;

	return ((texinfo->c.flags & SURF_LIGHT) && material->bsp_radiance)
		? bsp_emissive
		: material->default_radiance;
}

#define DUMP_WORLD_MESH_TO_OBJ 0
#if DUMP_WORLD_MESH_TO_OBJ
static FILE* obj_dump_file = NULL;
static int obj_vertex_num = 0;
#endif

/* SEEN FROM BOTH SIDES. A Quake III "cull none" surface - every torch flame,
   banners, open frames - is drawn from both sides. World geometry is traced with
   back faces culled: Quake II's brushes are closed, so a face is only ever seen
   from the front, and only the masked BLAS (filter_static_masked) turns that off.
   The material said double-sided (material.c, apply_q3_shader) and the face was
   still one-sided: from behind, the torch flames were not there at all (Matt,
   2026-11-16: "it works from one angle but not the other side"). So such a face
   is written twice, the second time turned round. Not a masked one, which is
   two-sided already; not water, slime or glass, whose rays tell going in from
   coming out by the side they hit; not sky. The light polys get their back faces
   in collect_light_polys (add_back_faces). Quake III maps only: double_sided on a
   Quake II world texture keeps meaning what it always did. */
static bool
face_has_back(const bsp_t* bsp, const pbr_material_t* material, uint32_t material_id)
{
	if (!bsp->q3 || !(material_id & MATERIAL_FLAG_DOUBLE_SIDED))
		return false;
	if (material && material->image_mask)
		return false;
	return !MAT_IsKind(material_id, MATERIAL_KIND_WATER) && !MAT_IsKind(material_id, MATERIAL_KIND_SLIME)
		&& !MAT_IsKind(material_id, MATERIAL_KIND_GLASS) && !MAT_IsKind(material_id, MATERIAL_KIND_SKY);
}

/* THE MOST EDGES ONE FACE CAN HAVE HERE. Quake II's compiler stops at 32, and
   the arrays below were that size; Quake 1's BSP2 maps from ericw-tools go to
   64 - mg1/start has a 33-edge face, mge3m2 a 41 - and loading q1/mg1/start
   overran the stack (a /GS fast-fail, 2026-11-20). A face with more than this
   is left out of the mesh and the light lists. */
#define MAX_FACE_VERTS  64

static uint32_t
create_poly(
	const bsp_t* bsp,
	const mface_t* surf,
	/* The face's EFFECTIVE material - see face_effective_material. Passed in
	   because the per-primitive emissive factor below comes off it, and a
	   per-surface instance exists precisely to change how bright a surface is. */
	const pbr_material_t* material,
	uint material_id,
	uint32_t primitive_index,
	uint32_t max_prim,
	VboPrimitive* primitives_out)
{
	float positions [3 * MAX_FACE_VERTS];
	float tex_coords[2 * MAX_FACE_VERTS];
	mbasis_t bases  [    MAX_FACE_VERTS];
	mtexinfo_t *texinfo = surf->texinfo;

	if (surf->numsurfedges > MAX_FACE_VERTS)
		return 0;

	float sc[2] = { 1.f, 1.f };
	if (texinfo->material && texinfo->material->original_width && texinfo->material->original_height) {
		sc[0] = 1.0f / (float)texinfo->material->original_width;
		sc[1] = 1.0f / (float)texinfo->material->original_height;
	}

	// [Sam-KEX] Rerelease surface flags.  SURF_N64_UV stretches the texture to
	// twice its size on the face; the scroll flags are per-face, so they travel
	// with the primitive instead of with the material.
	if (texinfo->c.flags & SURF_N64_UV) {
		sc[0] *= 0.5f;
		sc[1] *= 0.5f;
	}

	uint32_t texture_flags = 0;
	if (texinfo->c.flags & SURF_N64_SCROLL_X)
		texture_flags |= TEXTURE_FLAG_SCROLL_X;
	if (texinfo->c.flags & SURF_N64_SCROLL_Y)
		texture_flags |= TEXTURE_FLAG_SCROLL_Y;
	if (texinfo->c.flags & SURF_N64_SCROLL_FLIP)
		texture_flags |= TEXTURE_FLAG_SCROLL_FLIP;

	// the MGU drop pod fire - see the glocrys_1b hack in collect_surfaces
	if (Q_strHas(texinfo->name, "glocrys_1b") && texinfo->c.value == 0 && (texinfo->c.flags & SURF_TRANS_MASK))
		texture_flags |= TEXTURE_FLAG_FAST_FLOW;
	
	for (int i = 0; i < surf->numsurfedges; i++) {
		msurfedge_t *src_surfedge = surf->firstsurfedge + i;
		medge_t     *src_edge     = src_surfedge->edge;
		mvertex_t   *src_vert     = src_edge->v[src_surfedge->vert];

		float *p = positions + i * 3;
		float *t = tex_coords + i * 2;

		VectorCopy(src_vert->point, p);

		// Sink the pod fire deeper into the window recess, toward the sky face
		// 2 units behind it. The move is along the face normal and the texture
		// axes lie in the face plane, so the texture mapping is unchanged.
		if (texture_flags & TEXTURE_FLAG_FAST_FLOW) {
			float d = (surf->drawflags & DSURF_PLANEBACK) ? 1.5f : -1.5f;
			VectorMA(src_vert->point, d, surf->plane->normal, p);
		}

		if (surf->uv_mapped) {
			// Quake III: the face's own mapping, already in image units
			t[0] = DotProduct(p, surf->uv_axis[0]) + surf->uv_axis[0][3];
			t[1] = DotProduct(p, surf->uv_axis[1]) + surf->uv_axis[1][3];
		} else {
			t[0] = (DotProduct(p, texinfo->axis[0]) + texinfo->offset[0]) * sc[0];
			t[1] = (DotProduct(p, texinfo->axis[1]) + texinfo->offset[1]) * sc[1];
		}

		if (bsp->basisvectors)
		{
			bases[i] = *(bsp->bases + surf->firstbasis + i);
		}
		
#if DUMP_WORLD_MESH_TO_OBJ
		if (obj_dump_file)
		{
			fprintf(obj_dump_file, "v %.3f %.3f %.3f\n", src_vert->point[0], src_vert->point[1], src_vert->point[2]);
		}
#endif
	}

#if DUMP_WORLD_MESH_TO_OBJ
	if (obj_dump_file)
	{
		fprintf(obj_dump_file, "f ");
		for (int i = 0; i < surf->numsurfedges; i++) {
			fprintf(obj_dump_file, "%d ", obj_vertex_num);
			obj_vertex_num++;
		}
		fprintf(obj_dump_file, "\n");
	}
#endif
	
	int num_vertices = surf->numsurfedges;

	bool is_sky = MAT_IsKind(material_id, MATERIAL_KIND_SKY);
	if (is_sky)
	{
		// process skybox geometry in the same way as we process it for analytic light generation
		// to avoid mismatches between lights and geometry
		remove_collinear_edges(positions, tex_coords, bases, &num_vertices);
	}

	if (num_vertices < 3)
		return 0;
	
	const uint32_t num_triangles = (uint32_t)num_vertices - 2;
	const bool back = face_has_back(bsp, material, material_id);

	if (!primitives_out)
		return back ? num_triangles * 2 : num_triangles;

	const float emissive_factor = compute_emissive(texinfo, material);

	float alpha = 1.f;
	if (MAT_IsKind(material_id, MATERIAL_KIND_TRANSPARENT))
		alpha = (texinfo->c.flags & SURF_TRANS33) ? 0.33f : (texinfo->c.flags & SURF_TRANS66) ? 0.66f : 1.0f;
	// a Quake III effect that only adds light: nothing of it shows but the glow
	// (material.h q3_additive), so the primary ray goes straight through it
	if (material && material->q3_additive && MAT_IsKind(material_id, MATERIAL_KIND_TRANSPARENT))
		alpha = 0.f;

	const uint32_t emissive_and_alpha = floatToHalf(emissive_factor) | (floatToHalf(alpha) << 16);

	bool write_normals = bsp->basisvectors != NULL;
	
	for (uint32_t i = 0; i < num_triangles; i++)
	{
		// The prititive buffer is allocated based on the expected number of prims generated by the bsp,
		// so just verify that here, mostly for debugging.
		if (primitive_index + i >= max_prim)
		{
			assert(!"Primitive buffer overflow - there's a bug somewhere.");
			return i;
		}

		memset(primitives_out, 0, sizeof(VboPrimitive));
		
		int i1 = (i + 2) % num_vertices;
		int i2 = (i + 1) % num_vertices;

		float* pos = positions;
		float* tc = tex_coords;
		VectorCopy(pos, primitives_out->pos0);
		primitives_out->uv0[0] = tc[0];
		primitives_out->uv0[1] = tc[1];
		
		if (write_normals)
		{
			const mbasis_t* basis = bases;
			const vec3_t* normal = bsp->basisvectors + basis->normal;
			const vec3_t* tangent = bsp->basisvectors + basis->tangent;
			primitives_out->normals[0] = encode_normal(*normal);
			primitives_out->tangents[0] = encode_normal(*tangent);
		}

		pos = positions + i1 * 3;
		tc = tex_coords + i1 * 2;
		VectorCopy(pos, primitives_out->pos1);
		primitives_out->uv1[0] = tc[0];
		primitives_out->uv1[1] = tc[1];
		
		if (write_normals)
		{
			const mbasis_t* basis = bases + i1;
			const vec3_t* normal = bsp->basisvectors + basis->normal;
			const vec3_t* tangent = bsp->basisvectors + basis->tangent;
			primitives_out->normals[1] = encode_normal(*normal);
			primitives_out->tangents[1] = encode_normal(*tangent);
		}
		
		pos = positions + i2 * 3;
		tc = tex_coords + i2 * 2;
		VectorCopy(pos, primitives_out->pos2);
		primitives_out->uv2[0] = tc[0];
		primitives_out->uv2[1] = tc[1];
		
		if (write_normals)
		{
			const mbasis_t* basis = bases + i2;
			const vec3_t* normal = bsp->basisvectors + basis->normal;
			const vec3_t* tangent = bsp->basisvectors + basis->tangent;
			primitives_out->normals[2] = encode_normal(*normal);
			primitives_out->tangents[2] = encode_normal(*tangent);
		}

		primitives_out->material_id = material_id;
		primitives_out->texture_flags = texture_flags;
		primitives_out->emissive_and_alpha = emissive_and_alpha;
		primitives_out->instance = 0;

		++primitives_out;
	}

	if (!back)
		return num_triangles;

	// the back (face_has_back): each triangle again, wound the other way round
	// with its normals reversed, so it faces the other side; same texture
	// coordinates and tangents, so it shows the same picture from there
	const VboPrimitive* front = primitives_out - num_triangles;
	for (uint32_t i = 0; i < num_triangles; i++)
	{
		if (primitive_index + num_triangles + i >= max_prim)
		{
			assert(!"Primitive buffer overflow - count_triangles missed a back face.");
			return num_triangles + i;
		}

		*primitives_out = front[i];
		VectorCopy(front[i].pos2, primitives_out->pos1);
		VectorCopy(front[i].pos1, primitives_out->pos2);
		primitives_out->uv1[0] = front[i].uv2[0];
		primitives_out->uv1[1] = front[i].uv2[1];
		primitives_out->uv2[0] = front[i].uv1[0];
		primitives_out->uv2[1] = front[i].uv1[1];

		if (write_normals)
		{
			// the front's corners were 0, (i + 2), (i + 1); the back's 0, (i + 1), (i + 2)
			const int corner[3] = { 0, (int)(i + 1) % num_vertices, (int)(i + 2) % num_vertices };
			for (int c = 0; c < 3; c++)
			{
				const mbasis_t* basis = bases + corner[c];
				vec3_t normal;
				VectorNegate(bsp->basisvectors[basis->normal], normal);
				primitives_out->normals[c] = encode_normal(normal);
				primitives_out->tangents[c] = encode_normal(bsp->basisvectors[basis->tangent]);
			}
		}

		++primitives_out;
	}

	return num_triangles * 2;
}

static int
belongs_to_model(bsp_t *bsp, mface_t *surf)
{
	for (int i = 0; i < bsp->nummodels; i++) {
		if (surf >= bsp->models[i].firstface
		&& surf < bsp->models[i].firstface + bsp->models[i].numfaces)
			return 1;
	}
	return 0;
}

// Classification of what kind of sky a surface is
enum sky_class_e
{
	// Not sky
	SKY_CLASS_NO,
	// Sky material (selected by filter_static_sky)
	SKY_CLASS_MATERIAL,
	// User-defined sky enabled by pt_bsp_sky_lights > 1 (selected by filter_nodraw_sky_lights)
	SKY_CLASS_NODRAW_SKYLIGHT
};

static inline enum sky_class_e classify_sky(int flags, int surf_flags)
{
	if (MAT_IsKind(flags, MATERIAL_KIND_SKY))
		return SKY_CLASS_MATERIAL;

	if (cvar_pt_bsp_sky_lights->integer > 1) {
		int nodraw_skylight_expected = SURF_SKY | SURF_LIGHT | SURF_NODRAW;
		if ((surf_flags & nodraw_skylight_expected) == nodraw_skylight_expected)
			return SKY_CLASS_NODRAW_SKYLIGHT;
	}

	return SKY_CLASS_NO;
}

static int filter_static_masked(int flags, int surf_flags)
{
	if ((surf_flags & SURF_NODRAW) && cvar_pt_enable_nodraw->integer)
		return 0;
	
	const pbr_material_t* mat = MAT_ForIndex(flags & MATERIAL_INDEX_MASK);

	/* MASKED GLASS: glass with its outside cut away - the Quake III skull's
	   spectacles (models/mapobjects/skull/monkeyhead2, q3dm15), one square quad
	   per lens, round glass on it with a solid frame (alpha glass) and nothing
	   round that. Here it was written twice: cut out in this BLAS, whose
	   AS_FLAG_OPAQUE made the lenses solid to shadow and bounce rays, and whole
	   in the transparent one (filter_static_transparent), whose forced-opaque
	   hits never asked the mask - so the square showed as a glass pane. It goes
	   to the transparent BLAS only, which then runs the masked hit
	   (transparent_masked, vkpt_bsp_mesh_set_instance_flags), as a brush model
	   holding masked glass always did. Glass only: a masked water or kind
	   TRANSPARENT face keeps its copy here, and the shadow it casts. */
	if (mat && mat->image_mask && MAT_IsKind(flags, MATERIAL_KIND_GLASS))
		return 0;

	if (mat && mat->image_mask)
		return 1;

	return 0;
}

static int filter_static_opaque(int flags, int surf_flags)
{
	if ((surf_flags & SURF_NODRAW) && cvar_pt_enable_nodraw->integer)
		return 0;
	
	if (filter_static_masked(flags, surf_flags))
		return 0;
	
	flags &= MATERIAL_KIND_MASK;
	if (flags == MATERIAL_KIND_SKY || flags == MATERIAL_KIND_WATER || flags == MATERIAL_KIND_SLIME || flags == MATERIAL_KIND_GLASS || flags == MATERIAL_KIND_TRANSPARENT)
		return 0;

	return 1;
}

static int filter_static_transparent(int flags, int surf_flags)
{
	if ((surf_flags & SURF_NODRAW) && cvar_pt_enable_nodraw->integer)
		return 0;
	
	flags &= MATERIAL_KIND_MASK;
	if (flags == MATERIAL_KIND_WATER || flags == MATERIAL_KIND_SLIME || flags == MATERIAL_KIND_GLASS || flags == MATERIAL_KIND_TRANSPARENT)
		return 1;
	
	return 0;
}

static int filter_static_sky(int flags, int surf_flags)
{
	enum sky_class_e sky_class = classify_sky(flags, surf_flags);

	// pt_enable_nodraw deliberately does not apply here. Many maps mark their sky faces
	// SKY|NODRAW; dropping them leaves holes in the sky through which rays hit other parts
	// of the level. Only the explicit skylight class (pt_bsp_sky_lights 2) is skipped.
	if (sky_class == SKY_CLASS_NODRAW_SKYLIGHT)
		return 0;
	
	if (sky_class == SKY_CLASS_MATERIAL)
		return 1;

	return 0;
}

static int filter_all(int flags, int surf_flags)
{
	if ((surf_flags & SURF_NODRAW) && cvar_pt_enable_nodraw->integer)
		return 0;
	
	if (MAT_IsKind(flags, MATERIAL_KIND_SKY))
		return 0;

	return 1;
}

static int filter_nodraw_sky_lights(int flags, int surf_flags)
{
	enum sky_class_e sky_class = classify_sky(flags, surf_flags);
	return sky_class == SKY_CLASS_NODRAW_SKYLIGHT;
}

// Computes a point at a small distance above the center of the triangle.
// Returns false if the triangle is degenerate, true otherwise.
bool
get_triangle_off_center(const float* positions, float* center, float* anti_center, float offset)
{
	const float* v0 = positions + 0;
	const float* v1 = positions + 3;
	const float* v2 = positions + 6;

	// Compute the triangle center

	VectorCopy(v0, center);
	VectorAdd(center, v1, center);
	VectorAdd(center, v2, center);
	VectorScale(center, 1.f / 3.f, center);

	// Compute the normal

	vec3_t e1, e2, normal;
	VectorSubtract(v1, v0, e1);
	VectorSubtract(v2, v0, e2);
	CrossProduct(e1, e2, normal);
	float length = VectorNormalize(normal);

	// Offset the center by a fraction of the normal to make sure that the point is
	// inside a BSP leaf and not on a boundary plane.

	VectorScale(normal, offset, normal);
	VectorAdd(center, normal, center);

	if (anti_center)
	{
		VectorMA(center, -2.f, normal, anti_center);
	}

	return (length > 0.f);
}

/* WHICH LIGHT STYLE ANIMATES A LAMP.

   A face's styles[] are its LIGHTMAP styles: one per animated or switchable light
   that reached the face when the map was lit. The renderer has always taken the
   first non-zero one as the lamp's own animation. On id's maps that holds - qrad3
   lit with linear falloff, so a style only lands on faces within its light's range
   ("light" value, 300 if unset), and every one of the 793 styled lamp faces in
   pak0 is within 1.014x of it.

   The rerelease maps were relit with bounce and inverse-square falloff, which
   spreads a style far past its light: 6814 styled lamp faces, 3174 of them beyond
   1.1x. base1's wall lamp at (-256 -188 56) picked up style 6 (flicker) from a
   light 330 units away and flickered, while its own lights sit 18 units off and are
   steady. Under A-SVGF a flickering light resets the history of everything it lights
   each change, so the metal round that lamp stayed noisy after the rest was fixed.

   So a style counts only if a light entity carrying it reaches the face directly:
   within pt_light_style_reach times its range of the face's bounds. A style no
   light entity carries at all (city1 has three) is kept, as before. Filled by
   bsp_mesh_create_from_bsp for the duration of the build. */
typedef struct {
	vec3_t origin;
	float range;
	int style;
} styled_light_t;

static styled_light_t *s_styled_lights = NULL;
static int s_num_styled_lights = 0;
static float s_light_style_reach = 0.f;

static void
collect_styled_lights(const bsp_t *bsp)
{
	s_num_styled_lights = 0;
	s_light_style_reach = cvar_pt_light_style_reach ? cvar_pt_light_style_reach->value : 0.f;

	if (s_light_style_reach <= 0.f || !bsp->entitystring)
		return;

	int allocated = 0;
	const char *data = bsp->entitystring;

	while (data)
	{
		const char *token = COM_Parse(&data);
		if (!data || token[0] != '{')
			break;

		char classname[MAX_QPATH] = "";
		vec3_t origin = { 0.f, 0.f, 0.f };
		bool has_origin = false;
		int style = 0;
		float range = 300.f; // qrad3's default "light"

		while (data)
		{
			char key[MAX_QPATH];
			Q_strlcpy(key, COM_Parse(&data), sizeof(key));
			if (!data || key[0] == '}')
				break;

			const char *value = COM_Parse(&data);
			if (!data)
				break;

			if (!strcmp(key, "classname"))
				Q_strlcpy(classname, value, sizeof(classname));
			else if (!strcmp(key, "origin"))
				has_origin = sscanf(value, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
			else if (!strcmp(key, "style"))
				style = atoi(value);
			else if (!strcmp(key, "light"))
				range = (float)atof(value);
		}

		if (strncmp(classname, "light", 5) || !has_origin || style <= 0 || style >= 255)
			continue;

		if (s_num_styled_lights >= allocated)
		{
			allocated = max(64, allocated * 2);
			styled_light_t *grown = Z_Malloc(allocated * sizeof(styled_light_t));
			if (s_styled_lights)
			{
				memcpy(grown, s_styled_lights, s_num_styled_lights * sizeof(styled_light_t));
				Z_Free(s_styled_lights);
			}
			s_styled_lights = grown;
		}

		styled_light_t *light = s_styled_lights + s_num_styled_lights++;
		VectorCopy(origin, light->origin);
		light->range = max(range, 1.f);
		light->style = style;
	}
}

static void
free_styled_lights(void)
{
	if (s_styled_lights)
		Z_Free(s_styled_lights);
	s_styled_lights = NULL;
	s_num_styled_lights = 0;
}

static bool
light_style_reaches_surf(const mface_t *surf, int style)
{
	if (s_light_style_reach <= 0.f)
		return true;

	vec3_t mins = { FLT_MAX, FLT_MAX, FLT_MAX };
	vec3_t maxs = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (int i = 0; i < surf->numsurfedges; i++)
	{
		const float *p = surf->firstsurfedge[i].edge->v[surf->firstsurfedge[i].vert]->point;
		for (int k = 0; k < 3; k++)
		{
			mins[k] = min(mins[k], p[k]);
			maxs[k] = max(maxs[k], p[k]);
		}
	}

	bool style_has_light = false;
	for (int i = 0; i < s_num_styled_lights; i++)
	{
		const styled_light_t *light = s_styled_lights + i;
		if (light->style != style)
			continue;

		style_has_light = true;

		float dist2 = 0.f;
		for (int k = 0; k < 3; k++)
		{
			float d = max(max(mins[k] - light->origin[k], light->origin[k] - maxs[k]), 0.f);
			dist2 += d * d;
		}

		float reach = light->range * s_light_style_reach;
		if (dist2 <= reach * reach)
			return true;
	}

	return !style_has_light;
}

static int
get_surf_light_style(const mface_t* surf)
{
	for (int nstyle = 0; nstyle < 4; nstyle++)
	{
		int style = surf->styles[nstyle];
		if (style != 0 && style != 255 && light_style_reaches_surf(surf, style))
		{
			return style;
		}
	}

	return 0;
}

static bool
get_surf_plane_equation(mface_t* surf, float* plane)
{
	// Go over multiple planes defined by different edge pairs of the surface.
	// Some of the edges may be collinear or almost-collinear to each other,
	// so we can't just take the first pair of edges.
	// We can't even take the first pair of edges with nonzero cross product
	// because of numerical issues - they may be almost-collinear.
	// So this function finds the plane equation from the triangle with the
	// largest area, i.e. the longest cross product.
	
	float maxlen = 0.f;
	for (int i = 0; i < surf->numsurfedges - 1; i++)
	{
		float* v0 = surf->firstsurfedge[i + 0].edge->v[surf->firstsurfedge[i + 0].vert]->point;
		float* v1 = surf->firstsurfedge[i + 1].edge->v[surf->firstsurfedge[i + 1].vert]->point;
		float* v2 = surf->firstsurfedge[(i + 2) % surf->numsurfedges].edge->v[surf->firstsurfedge[(i + 2) % surf->numsurfedges].vert]->point;
		vec3_t e0, e1;
		VectorSubtract(v1, v0, e0);
		VectorSubtract(v2, v1, e1);
		vec3_t normal;
		CrossProduct(e0, e1, normal);
		float len = VectorLength(normal);
		if (len > maxlen)
		{
			VectorScale(normal, 1.0f / len, plane);
			plane[3] = -DotProduct(plane, v0);
			maxlen = len;
		}
	}
	return (maxlen > 0.f);
}

/* An invisible lava face - SURF_NODRAW, hidden by pt_enable_nodraw like every
   other nodraw face - must not light anything, surfaces or fog. Maps put the
   lava texture on nodraw brushes for the damage volume and the inside of a
   pool, and those turned into walls of glowing fog where nothing is drawn. */
static bool
is_invisible_lava(int surf_flags, const pbr_material_t *material)
{
	return (surf_flags & SURF_NODRAW) && cvar_pt_enable_nodraw->integer &&
	       material && MAT_IsKind(material->flags, MATERIAL_KIND_LAVA);
}

static bool
is_sky_or_lava_cluster(bsp_mesh_t* wm, mface_t* surf, int cluster, int material_id)
{
	if (cluster < 0)
		return false;

	if (MAT_IsKind(material_id, MATERIAL_KIND_LAVA) && wm->all_lava_emissive)
	{
		vec4_t plane;
		if (get_surf_plane_equation(surf, plane))
		{
			if (plane[2] < 0.f)
				return true;
		}
	}
	else
	{
		for (int i = 0; i < wm->num_sky_clusters; i++)
		{
			if (wm->sky_clusters[i] == cluster)
			{
				return true;
			}
		}
	}

	return false;
}

static void merge_pvs_rows(bsp_t* bsp, byte* src, byte* dst)
{
	for (int i = 0; i < bsp->visrowsize; i++)
	{
		dst[i] |= src[i];
	}
}

#define FOREACH_BIT_BEGIN(SET,ROWSIZE,VAR) \
	for (int _byte_idx = 0; _byte_idx < (ROWSIZE); _byte_idx++) { \
	if (SET[_byte_idx]) { \
		for (int _bit_idx = 0; _bit_idx < 8; _bit_idx++) { \
			if (SET[_byte_idx] & (1 << _bit_idx)) { \
				int VAR = (_byte_idx << 3) | _bit_idx;

#define FOREACH_BIT_END  } } } }

static void connect_pvs(bsp_t* bsp, int cluster_a, byte* pvs_a, int cluster_b, byte* pvs_b)
{
	FOREACH_BIT_BEGIN(pvs_a, bsp->visrowsize, vis_cluster_a)
		if (vis_cluster_a != cluster_a && vis_cluster_a != cluster_b)
		{
			merge_pvs_rows(bsp, pvs_b, BSP_GetPvs(bsp, vis_cluster_a));
		}
	FOREACH_BIT_END

	FOREACH_BIT_BEGIN(pvs_b, bsp->visrowsize, vis_cluster_b)
		if (vis_cluster_b != cluster_a && vis_cluster_b != cluster_b)
		{
			merge_pvs_rows(bsp, pvs_a, BSP_GetPvs(bsp, vis_cluster_b));
		}
	FOREACH_BIT_END

	merge_pvs_rows(bsp, pvs_a, pvs_b);
	merge_pvs_rows(bsp, pvs_b, pvs_a);
}

static void make_pvs_symmetric(bsp_t* bsp)
{
	for (int cluster = 0; cluster < bsp->vis->numclusters; cluster++)
	{
		byte* pvs = BSP_GetPvs(bsp, cluster);

		FOREACH_BIT_BEGIN(pvs, bsp->visrowsize, vis_cluster)
			if (vis_cluster != cluster)
			{
				byte* vis_pvs = BSP_GetPvs(bsp, vis_cluster);
				Q_SetBit(vis_pvs, cluster);
			}
		FOREACH_BIT_END
	}
}

static void build_pvs2(bsp_t* bsp)
{
	size_t matrix_size = bsp->visrowsize * bsp->vis->numclusters;

	bsp->pvs2_matrix = Z_Mallocz(matrix_size);

	for (int cluster = 0; cluster < bsp->vis->numclusters; cluster++)
	{
		byte* pvs = BSP_GetPvs(bsp, cluster);
		byte* dest_pvs = BSP_GetPvs2(bsp, cluster);
		memcpy(dest_pvs, pvs, bsp->visrowsize);

		FOREACH_BIT_BEGIN(pvs, bsp->visrowsize, vis_cluster)
			byte* pvs2 = BSP_GetPvs(bsp, vis_cluster);
			merge_pvs_rows(bsp, pvs2, dest_pvs);
		FOREACH_BIT_END
	}

}

// Provides an upper estimate (not counting the collinear edge removal, invisible materials etc.)
// for the total number of triangles needed to represent the bsp and one instance of every model.
static int count_triangles(const bsp_t* bsp)
{
	int num_tris = 0;

	for (int i = 0; i < bsp->numfaces; i++)
	{
		mface_t* surf = bsp->faces + i;
		int num_vertices = surf->numsurfedges;

		if (num_vertices >= 3)
		{
			// room for a double-sided face's back (create_poly), on the texture's
			// own material - generously, before kinds and instances are settled
			const pbr_material_t* mat = surf->texinfo ? surf->texinfo->material : NULL;
			const bool back = bsp->q3 && mat && (mat->flags & MATERIAL_FLAG_DOUBLE_SIDED);
			num_tris += (num_vertices - 2) * (back ? 2 : 1);
		}
	}

	return num_tris;
}

/* ==========================================================================
   MATERIAL INSTANCE IDENTITY - giving one use of a texture a name of its own.

   A .mat file keys on a TEXTURE, so every surface drawn with textures/white
   shares one set of values. That is usually what you want and occasionally
   ruinous: the same blank white texture is the emissive face of a red spot
   lamp in one room and an ordinary white panel in another, and tuning the
   first to be a red light drags the second along with it.

   RTX Remix solves this by hashing the geometry of a draw call, which gives
   each mesh an identity independent of the texture on it. The same idea works
   here, with BSP faces standing in for draw calls: hash the vertex positions
   of a face and you have a name for that face alone.

   A face is too small a unit on its own, though - a lamp housing is half a
   dozen faces and tuning them one at a time, to identical values, is not a
   workflow. So the unit is the CONNECTED GROUP: start at a face and take
   everything reachable from it across a shared edge that draws with the same
   material. That is "the mesh" in the sense a person means it, and it is what
   `mat create_instance` puts under one .mat section.

   STABILITY IS THE WHOLE REQUIREMENT. The hash is written into a material file
   by a human and has to still mean the same surface next week, so it may only
   depend on what is in the BSP: quantised vertex positions and the material.
   Not face order, not the order edges happen to be walked, not any pointer,
   not anything about this renderer's build. Recompiling the map WILL change
   it - the geometry genuinely changed - and that is the honest behaviour.
   ========================================================================== */

/* Vertices land on integer or near-integer coordinates in a Quake II BSP, so
   an eighth-unit grid quantises away float noise without ever merging two
   distinct vertices. */
#define INSTANCE_HASH_GRID 8.0f

static inline uint32_t
instance_hash_mix(uint32_t h, uint32_t value)
{
	/* FNV-1a, byte at a time. */
	for (int b = 0; b < 4; b++)
	{
		h ^= (value >> (b * 8)) & 0xffu;
		h *= 16777619u;
	}
	return h;
}

/* A face's own contribution to its group's hash.

   The per-vertex hashes are SUMMED rather than chained, so the result does not
   depend on which vertex the winding happens to start at - two faces with the
   same corners hash alike however the BSP compiler chose to order them. */
static uint32_t
hash_face_geometry(const mface_t *surf)
{
	uint32_t accum = 0;

	for (int i = 0; i < surf->numsurfedges; i++)
	{
		const msurfedge_t *surfedge = surf->firstsurfedge + i;
		const float *point = surfedge->edge->v[surfedge->vert]->point;

		uint32_t h = 2166136261u;

		for (int c = 0; c < 3; c++)
		{
			int32_t quantized = (int32_t)lroundf(point[c] * INSTANCE_HASH_GRID);
			h = instance_hash_mix(h, (uint32_t)quantized);
		}

		accum += h;
	}

	return accum;
}

static int
uf_find(int *parent, int x)
{
	while (parent[x] != x)
	{
		parent[x] = parent[parent[x]];   // path halving
		x = parent[x];
	}
	return x;
}

static void
uf_union(int *parent, int a, int b)
{
	a = uf_find(parent, a);
	b = uf_find(parent, b);
	if (a != b)
		parent[b] = a;
}

/* Which inline model owns a face, or -1 for the world.

   Groups must not span this boundary: a door and the wall it sits flush
   against can share an edge in the BSP, and merging them would let a door's
   material instance reach out onto static geometry. Derived the same way
   belongs_to_model decides what collect_surfaces skips, so the two agree. */
static int
face_model_owner(bsp_t *bsp, const mface_t *surf)
{
	for (int i = 0; i < bsp->nummodels; i++)
	{
		if (surf >= bsp->models[i].firstface &&
		    surf < bsp->models[i].firstface + bsp->models[i].numfaces)
			return i;
	}
	return -1;
}

/* One hash per BSP face, shared by every face in its connected group.
   Caller frees. Returns NULL if the map has no faces. */
static uint32_t *
build_face_instance_hashes(bsp_t *bsp)
{
	if (bsp->numfaces <= 0)
		return NULL;

	uint32_t *face_hash = Z_Mallocz(sizeof(uint32_t) * bsp->numfaces);
	int *parent = Z_Malloc(sizeof(int) * bsp->numfaces);
	int *owner = Z_Malloc(sizeof(int) * bsp->numfaces);

	for (int i = 0; i < bsp->numfaces; i++)
	{
		parent[i] = i;
		owner[i] = face_model_owner(bsp, bsp->faces + i);
	}

	/* Join faces that share an edge and draw with the same material.

	   Recording only the FIRST face seen on each edge is enough: an edge shared
	   by three faces joins the second and third to that first one, and
	   union-find makes the relation transitive from there. */
	int *edge_first_face = Z_Malloc(sizeof(int) * max(bsp->numedges, 1));
	for (int i = 0; i < bsp->numedges; i++)
		edge_first_face[i] = -1;

	for (int i = 0; i < bsp->numfaces; i++)
	{
		const mface_t *surf = bsp->faces + i;

		if (!surf->texinfo)
			continue;

		for (int e = 0; e < surf->numsurfedges; e++)
		{
			ptrdiff_t edge_index = surf->firstsurfedge[e].edge - bsp->edges;

			if (edge_index < 0 || edge_index >= bsp->numedges)
				continue;

			int other = edge_first_face[edge_index];

			if (other < 0)
			{
				edge_first_face[edge_index] = i;
				continue;
			}

			const mface_t *other_surf = bsp->faces + other;

			if (!other_surf->texinfo)
				continue;

			if (owner[i] != owner[other])
				continue;

			if (surf->texinfo->material != other_surf->texinfo->material)
				continue;

			uf_union(parent, i, other);
		}
	}

	Z_Free(edge_first_face);

	/* Accumulate each face's geometry into its group's root. Summing keeps this
	   independent of the order the faces are walked in. */
	uint32_t *group_accum = Z_Mallocz(sizeof(uint32_t) * bsp->numfaces);

	for (int i = 0; i < bsp->numfaces; i++)
	{
		if (!bsp->faces[i].texinfo)
			continue;

		group_accum[uf_find(parent, i)] += hash_face_geometry(bsp->faces + i);
	}

	/* Fold in the material name, so two groups that happen to occupy mirrored
	   positions cannot collide, and so a hash visibly belongs to its texture. */
	for (int i = 0; i < bsp->numfaces; i++)
	{
		const mface_t *surf = bsp->faces + i;

		if (!surf->texinfo)
			continue;

		uint32_t h = group_accum[uf_find(parent, i)];

		for (const char *c = surf->texinfo->name; *c; c++)
			h = instance_hash_mix(h, (uint32_t)(unsigned char)Q_tolower(*c));

		/* 0 is reserved for "this primitive has no group". */
		face_hash[i] = h ? h : 1u;
	}

	Z_Free(group_accum);
	Z_Free(parent);
	Z_Free(owner);

	return face_hash;
}

/* Filled by bsp_mesh_create_from_bsp for the duration of the build, read by
   collect_surfaces as it emits each face's primitives. */
static uint32_t *s_face_instance_hash = NULL;

/* The world faces q3deform.c draws moving (collect_surfaces), one byte per
   face, while the mesh is built: their light polys move with them
   (give_lights_to_deform) instead of staying in the world's list. */
static byte *s_deform_faces = NULL;

/* THE MATERIAL A FACE ACTUALLY DRAWS WITH.

   Every path that asks "what material is on this surface?" has to come through
   here, not through surf->texinfo->material, or an instance is only half
   applied. That is not a hypothetical: applying it to the drawn geometry alone
   left a light fixture shaded with the instance's white texture while the light
   it CAST still came from the base material's red emissive, because the light
   polygons are collected separately and were still reading the texture's
   material. The surface looked right and lit the room the old colour. */
static pbr_material_t*
face_effective_material(bsp_t *bsp, const mface_t *surf)
{
	if (!surf->texinfo || !surf->texinfo->material)
		return NULL;

	pbr_material_t *base = surf->texinfo->material;

	if (!s_face_instance_hash)
		return base;

	ptrdiff_t face_index = surf - bsp->faces;

	if (face_index < 0 || face_index >= bsp->numfaces)
		return base;

	uint32_t instance_hash = s_face_instance_hash[face_index];

	if (!instance_hash)
		return base;

	pbr_material_t *instance = MAT_FindInstance(base, instance_hash);

	return instance ? instance : base;
}

// True if a leaf touching either side of the face has any of `contents`.
// qbsp emits a liquid boundary as two faces, one facing each way, so the
// liquid can be on either side. Brush model faces are still in model space
// here, so they are probed against the model's own subtree.
static bool
face_borders_contents(bsp_t *bsp, const mface_t *surf, int model_idx, int contents)
{
	mnode_t *headnode = model_idx < 0 ? bsp->nodes : bsp->models[model_idx].headnode;
	vec3_t center = { 0.f, 0.f, 0.f };

	if (!headnode || surf->numsurfedges <= 0)
		return false;

	for (int i = 0; i < surf->numsurfedges; i++)
		VectorAdd(center, surf->firstsurfedge[i].edge->v[surf->firstsurfedge[i].vert]->point, center);
	VectorScale(center, 1.f / surf->numsurfedges, center);

	for (int side = -1; side <= 1; side += 2)
	{
		vec3_t probe;
		VectorMA(center, 0.5f * side, surf->plane->normal, probe);
		if (BSP_PointLeaf(headnode, probe)->contents & contents)
			return true;
	}

	return false;
}

static void
collect_surfaces(uint32_t *prim_ctr, bsp_mesh_t *wm, bsp_t *bsp, int model_idx, int (*filter)(int, int))
{
	mface_t *surfaces = model_idx < 0 ? bsp->faces : bsp->models[model_idx].firstface;
	int num_faces = model_idx < 0 ? bsp->numfaces : bsp->models[model_idx].numfaces;
	bool any_pvs_patches = false;

	for (int i = 0; i < num_faces; i++) {
		mface_t *surf = surfaces + i;

		if (model_idx < 0 && belongs_to_model(bsp, surf)) {
			continue;
		}

		if (Q_strHas(surf->texinfo->name, "lava") && !Q_strHas(surf->texinfo->name, "brlava") && Q_strHas(bsp->name, "q2dm3")) {
			continue;
		}

		
		/* PER-SURFACE MATERIAL INSTANCE.

		   If this face's connected group has a .mat section keyed on its
		   geometry hash, the face draws with THAT material rather than the one
		   its texture names - which is the whole point of the mechanism, and
		   why the swap happens here, before the kind and flag fixups below.
		   Those read the material's own flags, so substituting afterwards
		   would leave the instance wearing the base material's `is_light`. */
		uint32_t face_hash = s_face_instance_hash && surf >= bsp->faces
			&& (surf - bsp->faces) < bsp->numfaces
			? s_face_instance_hash[surf - bsp->faces] : 0;

		pbr_material_t* surf_material = face_effective_material(bsp, surf);

		uint32_t material_id = surf_material ? surf_material->flags : 0;
		uint32_t surf_flags = surf->drawflags | surf->texinfo->c.flags;

		/* kind INVISIBLE: not drawn at all, as for a model's mesh (main.c) - a
		   Quake III shader whose alpha test passes nowhere (material.c,
		   apply_q3_shader), or a .mat entry saying so */
		if (MAT_IsKind(material_id, MATERIAL_KIND_INVISIBLE))
			continue;

		// ugly hacks for situations when the same texture is used with different effects

		// MGU drop pods: the fire over the pod window is a thin func_wall brush
		// in tomf/glocrys_1b with light value 0. Only its inner face is
		// TRANS33|FLOWING; the rest are 0x0 and the back face is coplanar with
		// the sky face behind it (mgu6m1: *73 at x -514..-516, sky at -516).
		// The opaque faces become SKY, which filter_all drops from brush models,
		// so the window still shows the sky. The translucent face is forced to
		// TRANSPARENT - rerelease.mat marks glocrys_1b is_light for the tomb
		// crystals, and a LIGHT material never reaches TRANSPARENT below, which
		// is why this used to delete the whole brush and lose the fire.
		// Emission still comes from the material's emissive_factor.
		if (Q_strHas(surf->texinfo->name, "glocrys_1b") && surf->texinfo->c.value == 0) {
			if (surf_flags & SURF_TRANS_MASK) {
				material_id &= ~MATERIAL_FLAG_LIGHT;
				material_id = MAT_SetKind(material_id, MATERIAL_KIND_TRANSPARENT);
			} else {
				surf_flags = SURF_SKY;
			}
		}

		// The two downgrades below exist because a texture's material is shared by
		// every face that uses it, and stock maps reuse liquid and glass textures on
		// plain walls (mine1 has 173 faces of WATER-kind e2u1/grass1_7). A surface
		// instance whose own section states 'kind' names these faces alone, so
		// that kind is what they draw as.
		bool kind_stated = surf_material && surf_material != surf->texinfo->material
			&& MAT_KindSpecified(surf_material);

		// Waterfalls: mgu6m2's e1u1/water1 columns are CONTENTS_WATER brushes whose
		// faces are FLOWING instead of WARP, so the texture scrolls down. Those stay
		// water. A FLOWING water texture on a solid wall still goes opaque, since
		// refracting into it would show the void behind.
		if ((MAT_IsKind(material_id, MATERIAL_KIND_WATER) || MAT_IsKind(material_id, MATERIAL_KIND_SLIME)) && !(surf_flags & SURF_WARP)
			&& !kind_stated
			&& !((surf_flags & SURF_FLOWING) && face_borders_contents(bsp, surf, model_idx, CONTENTS_WATER | CONTENTS_SLIME)))
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_REGULAR);

		if (MAT_IsKind(material_id, MATERIAL_KIND_GLASS) && !(surf_flags & SURF_TRANS_MASK) && !kind_stated)
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_REGULAR);
		// custom transparent surfaces
		if (surf_flags & SURF_SKY)
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_SKY);

		/* On a Quake III map the TRANS bits are the shader's own "see-through"
		   (bsp_q3.c translate_shaders), which apply_q3_shader already weighed in
		   choosing the kind. An entry that STATES 'kind REGULAR' means a solid
		   surface: Matt's slamp2 lamp shade, made REGULAR to reflect for real,
		   went TRANSPARENT here and reflected nothing (2026-11-17). */
		const bool q3_regular_stated = bsp->q3 && MAT_KindSpecified(surf_material);
		if (MAT_IsKind(material_id, MATERIAL_KIND_REGULAR) && (surf_flags & SURF_TRANS_MASK) && !(material_id & MATERIAL_FLAG_LIGHT)
			&& !q3_regular_stated)
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_TRANSPARENT);

		if (MAT_IsKind(material_id, MATERIAL_KIND_SCREEN) && (surf_flags & SURF_TRANS_MASK))
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_GLASS);

		if (surf_flags & SURF_WARP)
			material_id |= MATERIAL_FLAG_WARP;

		if (surf_flags & SURF_FLOWING)
			material_id |= MATERIAL_FLAG_FLOWING;

		/* A Quake 1 teleporter's gate: a window onto where it sends you
		   (cameras.c, vkpt_cameras_load_q1), solid, with none of its texture's
		   water, warp or glow. Decided before the filter, so it goes in with the
		   opaque world rather than the water. */
		const int q1_gate = bsp->q1 && model_idx < 0 ? vkpt_cameras_q1_face(wm, surf) : -1;
		if (q1_gate >= 0) {
			material_id &= ~(MATERIAL_FLAG_LIGHT | MATERIAL_FLAG_WARP | MATERIAL_FLAG_FLOWING | MATERIAL_LIGHT_STYLE_MASK);
			material_id = MAT_SetKind(material_id, MATERIAL_KIND_CAMERA);
			surf_flags &= ~(SURF_WARP | SURF_FLOWING | SURF_TRANS_MASK | SURF_NODRAW);
		}

		if (!filter(material_id, surf_flags))
			continue;

		if ((material_id & MATERIAL_FLAG_LIGHT) && surf_material && surf_material->light_styles)
		{
			int light_style = get_surf_light_style(surf);
			material_id |= (light_style << MATERIAL_LIGHT_STYLE_SHIFT) & MATERIAL_LIGHT_STYLE_MASK;
		}

		if (q1_gate >= 0)
		{
			// its slot fixed, as a Quake III portal's below
			material_id |= ((q1_gate * 4) << MATERIAL_LIGHT_STYLE_SHIFT) & MATERIAL_LIGHT_STYLE_MASK;
		}
		else if (MAT_IsKind(material_id, MATERIAL_KIND_CAMERA) && bsp->q3)
		{
			/* A Quake III portal shader: the view through the misc_portal_surface
			   next to it (cameras.c), its slot fixed - animate_materials.comp leaves
			   portals alone. With no portal camera there, Quake III draws a mirror. */
			int portal = vkpt_cameras_q3_face(wm, surf);
			if (portal < 0)
				material_id = (material_id & ~(MATERIAL_KIND_MASK | MATERIAL_LIGHT_STYLE_MASK)) | MATERIAL_KIND_CHROME;
			else
				material_id = (material_id & ~MATERIAL_LIGHT_STYLE_MASK) | (((portal * 4) << MATERIAL_LIGHT_STYLE_SHIFT) & MATERIAL_LIGHT_STYLE_MASK);
		}
		else if (MAT_IsKind(material_id, MATERIAL_KIND_CAMERA) && wm->num_cameras > 0)
		{
			/* Pick the camera slot (camera = slot / 4; animate_materials.comp steps the slot
			   every half second). It used to be a random slot PER FACE, but the BSP cuts one
			   monitor into several faces - base3's screens are 2-16 faces on one texinfo - so
			   a single monitor showed pieces of different cameras, cutting at different times,
			   with a seam between them. Hash the texinfo and plane instead: every face of one
			   monitor shares both. pt_camera_per_face 1 = the old random per face (map reload). */
			static cvar_t *cvar_pt_camera_per_face = NULL;
			if (!cvar_pt_camera_per_face)
				cvar_pt_camera_per_face = Cvar_Get("pt_camera_per_face", "0", 0);

			int num_slots = wm->num_cameras * 4;
			int camera_id;
			if (cvar_pt_camera_per_face->integer)
				camera_id = Q_rand() % num_slots;
			else
			{
				uint32_t h = (uint32_t)(surf->texinfo - bsp->texinfo) * 0x9E3779B1u
					^ (uint32_t)(surf->plane - bsp->planes) * 0x85EBCA77u;
				h ^= h >> 16;
				h *= 0x7FEB352Du;
				h ^= h >> 15;
				camera_id = (int)(h % (uint32_t)num_slots);
			}
			material_id = (material_id & ~MATERIAL_LIGHT_STYLE_MASK) | ((camera_id << MATERIAL_LIGHT_STYLE_SHIFT) & MATERIAL_LIGHT_STYLE_MASK);
		}
		
		VboPrimitive* surface_prims = wm->primitives + *prim_ctr;
		
		uint32_t prims_in_surface = create_poly(bsp, surf, surf_material, material_id, *prim_ctr, wm->num_primitives_allocated, surface_prims);

		/* Remember which connected group each emitted triangle came from, so
		   the crosshair can name it later. Indexed by the same primitive number
		   the path tracer reads back (Triangle.instance_prim for world
		   geometry), which is why it is stored per primitive and not per face. */
		if (wm->prim_instance_hash)
		{
			for (uint32_t k = 0; k < prims_in_surface; ++k)
			{
				uint32_t prim_index = *prim_ctr + k;
				if (prim_index < wm->num_primitives_allocated)
					wm->prim_instance_hash[prim_index] = face_hash;
			}
		}

		for (uint32_t k = 0; k < prims_in_surface; ++k) 
		{
			if (model_idx < 0)
			{
				// Collect the positions into one array for compatibility with get_triangle_off_center(...)
				float positions[9];
				VectorCopy(surface_prims[k].pos0, positions + 0);
				VectorCopy(surface_prims[k].pos1, positions + 3);
				VectorCopy(surface_prims[k].pos2, positions + 6);
				
				// Compute the BSP node for this specific triangle based on its center.
				// The face lists in the BSP are slightly incorrect, or the original code 
				// in q2vkpt that was extracting them was incorrect.

				vec3_t center, anti_center;
				get_triangle_off_center(positions, center, anti_center, 0.01f);

				int cluster = BSP_PointLeaf(bsp->nodes, center)->cluster;

				// If the small offset for the off-center point was too small, and that point
				// is not inside any cluster, try a larger offset.
				if (cluster < 0) {
					get_triangle_off_center(positions, center, anti_center, 1.f);
					cluster = BSP_PointLeaf(bsp->nodes, center)->cluster;
				}

				// Some compilers (the rerelease q2dm6, for one) don't split wall faces at a
				// liquid surface, so a fan triangle can have its center under lava while
				// half of it sticks up into the room. It then gets the liquid cluster's
				// light list and light stats, and the part above the surface renders dark
				// in sharp triangle shapes. Probe near each corner and prefer an air cluster.
				if (BSP_PointLeaf(bsp->nodes, center)->contents & MASK_WATER)
				{
					vec3_t tri_center, e1, e2, normal;
					VectorAdd(positions + 0, positions + 3, tri_center);
					VectorAdd(tri_center, positions + 6, tri_center);
					VectorScale(tri_center, 1.f / 3.f, tri_center);
					VectorSubtract(positions + 3, positions + 0, e1);
					VectorSubtract(positions + 6, positions + 0, e2);
					CrossProduct(e1, e2, normal);
					VectorNormalize(normal);

					for (int v = 0; v < 3; v++)
					{
						vec3_t probe;
						LerpVector(positions + v * 3, tri_center, 0.1f, probe);
						VectorMA(probe, 0.01f, normal, probe);
						const mleaf_t* leaf = BSP_PointLeaf(bsp->nodes, probe);
						if (leaf->cluster >= 0 && !(leaf->contents & (MASK_WATER | CONTENTS_SOLID)))
						{
							cluster = leaf->cluster;
							break;
						}
					}
				}

				surface_prims[k].cluster = cluster;

				if (cluster >= 0 && (MAT_IsKind(material_id, MATERIAL_KIND_SKY) || MAT_IsKind(material_id, MATERIAL_KIND_LAVA)))
				{
					bool is_bsp_sky_light = (surf_flags & (SURF_LIGHT | SURF_SKY)) == (SURF_LIGHT | SURF_SKY);
					if (is_sky_or_lava_cluster(wm, surf, cluster, material_id) || (cvar_pt_bsp_sky_lights->integer && is_bsp_sky_light)
						|| q1_sky_light(bsp, surf_flags))
					{
						surface_prims[k].material_id |= MATERIAL_FLAG_LIGHT;
					}
				}

				if (!bsp->pvs_patched)
				{
					if (MAT_IsKind(material_id, MATERIAL_KIND_SLIME) || MAT_IsKind(material_id, MATERIAL_KIND_WATER) || MAT_IsKind(material_id, MATERIAL_KIND_GLASS) || MAT_IsKind(material_id, MATERIAL_KIND_TRANSPARENT))
					{
						int anti_cluster = BSP_PointLeaf(bsp->nodes, anti_center)->cluster;

						if (cluster >= 0 && anti_cluster >= 0 && cluster != anti_cluster)
						{
							byte* pvs_cluster = BSP_GetPvs(bsp, cluster);
							byte* pvs_anti_cluster = BSP_GetPvs(bsp, anti_cluster);

							if (!Q_IsBitSet(pvs_cluster, anti_cluster) || !Q_IsBitSet(pvs_anti_cluster, cluster))
							{
								connect_pvs(bsp, cluster, pvs_cluster, anti_cluster, pvs_anti_cluster);
								any_pvs_patches = true;
							}
						}
					}
				}
			}
			else
				surface_prims[k].cluster = -1;
		}

		/* deformVertexes: a Quake III face that moves is drawn as a model
		   (q3deform.c). Its triangles - the front ones; the model's material is
		   two-sided by itself - go there, and the world keeps none of them. */
		if (model_idx < 0 && vkpt_q3_deform_wants(surf_material, material_id, surf_flags))
		{
			const uint32_t front = face_has_back(bsp, surf_material, material_id) ? prims_in_surface / 2 : prims_in_surface;
			vkpt_q3_deform_collect(surf_material, surface_prims, (int)front);
			if (s_deform_faces && surf >= bsp->faces && surf - bsp->faces < bsp->numfaces)
				s_deform_faces[surf - bsp->faces] = 1;
			continue;
		}

		*prim_ctr += prims_in_surface;
	}

	if (any_pvs_patches)
		make_pvs_symmetric(bsp);
}

/*
  Sutherland-Hodgman polygon clipping algorithm, mostly copied from
  https://rosettacode.org/wiki/Sutherland-Hodgman_polygon_clipping#C
*/

typedef struct {
	float x, y;
} point2_t;

// a face's (MAX_FACE_VERTS) clipped by a texture tile's square, which can add
// as many corners again
#define MAX_POLY_VERTS (2 * MAX_FACE_VERTS)
typedef struct {
	point2_t v[MAX_POLY_VERTS];
	int len;
} poly_t;

static inline float
dot2(point2_t* a, point2_t* b)
{
	return a->x * b->x + a->y * b->y;
}

static inline float
cross2(point2_t* a, point2_t* b)
{
	return a->x * b->y - a->y * b->x;
}

static inline point2_t
vsub(point2_t* a, point2_t* b)
{
	point2_t res;
	res.x = a->x - b->x;
	res.y = a->y - b->y;
	return res;
}

/* tells if vec c lies on the left side of directed edge a->b
 * 1 if left, -1 if right, 0 if colinear
 */
static int
left_of(point2_t* a, point2_t* b, point2_t* c)
{
	float x;
	point2_t tmp1 = vsub(b, a);
	point2_t tmp2 = vsub(c, b);
	x = cross2(&tmp1, &tmp2);
	return x < 0 ? -1 : x > 0;
}

static int
line_sect(point2_t* x0, point2_t* x1, point2_t* y0, point2_t* y1, point2_t* res)
{
	point2_t dx, dy, d;
	dx = vsub(x1, x0);
	dy = vsub(y1, y0);
	d = vsub(x0, y0);
	/* x0 + a dx = y0 + b dy ->
	   x0 X dx = y0 X dx + b dy X dx ->
	   b = (x0 - y0) X dx / (dy X dx) */
	float dyx = cross2(&dy, &dx);
	if (!dyx) return 0;
	dyx = cross2(&d, &dx) / dyx;
	if (dyx <= 0 || dyx >= 1) return 0;

	res->x = y0->x + dyx * dy.x;
	res->y = y0->y + dyx * dy.y;
	return 1;
}

static void
poly_append(poly_t* p, point2_t* v)
{
	assert(p->len < MAX_POLY_VERTS - 1);
	p->v[p->len++] = *v;
}

static int
poly_winding(poly_t* p)
{
	return left_of(p->v, p->v + 1, p->v + 2);
}

static void
poly_edge_clip(poly_t* sub, point2_t* x0, point2_t* x1, int left, poly_t* res)
{
	int i, side0, side1;
	point2_t tmp;
	point2_t* v0 = sub->v + sub->len - 1;
	point2_t* v1;
	res->len = 0;

	side0 = left_of(x0, x1, v0);
	if (side0 != -left) poly_append(res, v0);

	for (i = 0; i < sub->len; i++) {
		v1 = sub->v + i;
		side1 = left_of(x0, x1, v1);
		if (side0 + side1 == 0 && side0)
			/* last point and current straddle the edge */
			if (line_sect(x0, x1, v0, v1, &tmp))
				poly_append(res, &tmp);
		if (i == sub->len - 1) break;
		if (side1 != -left) poly_append(res, v1);
		v0 = v1;
		side0 = side1;
	}
}

static void
clip_polygon(poly_t* input, poly_t* clipper, poly_t* output)
{
	int i;
	poly_t p1_m, p2_m;
	poly_t* p1 = &p1_m;
	poly_t* p2 = &p2_m;
	poly_t* tmp;

	int dir = poly_winding(clipper);
	poly_edge_clip(input, clipper->v + clipper->len - 1, clipper->v, dir, p2);
	for (i = 0; i < clipper->len - 1; i++) {
		tmp = p2; p2 = p1; p1 = tmp;
		if (p1->len == 0) {
			p2->len = 0;
			break;
		}
		poly_edge_clip(p1, clipper->v + i, clipper->v + i + 1, dir, p2);
	}

	output->len = p2->len;
	if (output->len)
		memcpy(output->v, p2->v, p2->len * sizeof(point2_t));
}

static light_poly_t*
append_light_poly(int* num_lights, int* allocated, light_poly_t** lights)
{
	if (*num_lights == *allocated)
	{
		*allocated = max(*allocated * 2, 128);
		*lights = Z_Realloc(*lights, *allocated * sizeof(light_poly_t));
	}
	return *lights + (*num_lights)++;
}

static inline bool
is_light_material(uint32_t material)
{
	return (material & MATERIAL_FLAG_LIGHT) != 0;
}

/* A Quake III program's glow (light_poly_t q3_glow): copy_light takes the
   light's colour from the program every frame, averaged over the triangle with
   these texture coordinates (1 = one repeat). Every light made here goes
   through this, so the fields are never left as the stack had them. */
static void
set_light_q3_glow(light_poly_t *light, bool q3_glow, const float *uv0, const float *uv1, const float *uv2)
{
	light->q3_glow = q3_glow;
	light->q3_glow_key = 0;
	VectorClear(light->q3_glow_rgb);
	VectorClear(light->q3_glow_stages_rgb);
	light->q3_samples = 1;
	memset(light->q3_uv, 0, sizeof(light->q3_uv));
	if (!q3_glow)
		return;

	light->q3_uv[0] = uv0[0]; light->q3_uv[1] = uv0[1];
	light->q3_uv[2] = uv1[0]; light->q3_uv[3] = uv1[1];
	light->q3_uv[4] = uv2[0]; light->q3_uv[5] = uv2[1];

	// a sample grid two to a repeat along the longest side, 1 to 4 a side
	float longest = 0.f;
	for (int e = 0; e < 3; e++)
	{
		const float *a = light->q3_uv + e * 2, *b = light->q3_uv + ((e + 1) % 3) * 2;
		longest = max(longest, sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1])));
	}
	light->q3_samples = (uint8_t)max(1, min(4, (int)ceilf(2.f * longest)));
}

static void
collect_one_light_poly_entire_texture(bsp_t *bsp, mface_t *surf, mtexinfo_t *texinfo, pbr_material_t *material, int model_idx,
									  const vec3_t light_color, float emissive_factor, int light_style, bool q3_glow,
									  int *num_lights, int *allocated_lights, light_poly_t **lights)
{
	float positions[3 * MAX_FACE_VERTS];
	float tex_coords[2 * MAX_FACE_VERTS];

	if (surf->numsurfedges > MAX_FACE_VERTS)
		return;

	for (int i = 0; i < surf->numsurfedges; i++)
	{
		msurfedge_t *src_surfedge = surf->firstsurfedge + i;
		medge_t     *src_edge = src_surfedge->edge;
		mvertex_t   *src_vert = src_edge->v[src_surfedge->vert];

		float *p = positions + i * 3;
		float *t = tex_coords + i * 2;

		VectorCopy(src_vert->point, p);

		// in texture repeats, as the light reads them (only a glow needs them)
		if (surf->uv_mapped) {
			t[0] = DotProduct(p, surf->uv_axis[0]) + surf->uv_axis[0][3];
			t[1] = DotProduct(p, surf->uv_axis[1]) + surf->uv_axis[1][3];
		} else {
			t[0] = (DotProduct(p, texinfo->axis[0]) + texinfo->offset[0]) / (float)max(1, material->original_width);
			t[1] = (DotProduct(p, texinfo->axis[1]) + texinfo->offset[1]) / (float)max(1, material->original_height);
		}
	}

	int num_vertices = surf->numsurfedges;
	remove_collinear_edges(positions, tex_coords, NULL, &num_vertices);

	const int num_triangles = surf->numsurfedges - 2;

	for (int i = 0; i < num_triangles; i++)
	{
		const int e = surf->numsurfedges;

		int i1 = (i + 2) % e;
		int i2 = (i + 1) % e;

		light_poly_t light;
		VectorCopy(positions, light.positions + 0);
		VectorCopy(positions + i1 * 3, light.positions + 3);
		VectorCopy(positions + i2 * 3, light.positions + 6);
		VectorScale(light_color, emissive_factor, light.color);
		set_light_q3_glow(&light, q3_glow, tex_coords, tex_coords + i1 * 2, tex_coords + i2 * 2);

		light.material = material;
		light.style = light_style;
		light.type = DYNLIGHT_POLYGON;
		// Nobody has named a volumetric scale for this light, so copy_light()
		// will pick the default for its class. light_poly_t is built on the
		// stack here and is NOT zero-initialised, so this has to be explicit -
		// every site that creates one carries the same line.
		light.volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;

		if(!get_triangle_off_center(light.positions, light.off_center, NULL, 1.f))
			continue;

		light.emissive_factor = emissive_factor;
		
		if (model_idx >= 0)
			light.cluster = -1; // Cluster will be determined when the model is instanced
		else
			light.cluster = BSP_PointLeaf(bsp->nodes, light.off_center)->cluster;
		
		if (model_idx >= 0 || light.cluster >= 0)
		{
			light_poly_t* list_light = append_light_poly(num_lights, allocated_lights, lights);
			memcpy(list_light, &light, sizeof(light_poly_t));
		}
	}
}

static void
collect_one_light_poly(bsp_t *bsp, mface_t *surf, mtexinfo_t *texinfo, pbr_material_t *material, int model_idx, const vec4_t plane,
					   const float tex_scale[], const vec2_t min_light_texcoord, const vec2_t max_light_texcoord,
					   const vec3_t light_color, float emissive_factor, int light_style, bool q3_glow,
					   int* num_lights, int* allocated_lights, light_poly_t** lights)
{
	if (surf->numsurfedges > MAX_FACE_VERTS)
		return;

	// Scale the texture axes according to the original resolution of the game's .wal textures
	vec4_t tex_axis0, tex_axis1;
	if (surf->uv_mapped) {
		// Quake III: the face's own mapping, already in image units
		Vector4Copy(surf->uv_axis[0], tex_axis0);
		Vector4Copy(surf->uv_axis[1], tex_axis1);
	} else {
		VectorScale(texinfo->axis[0], tex_scale[0], tex_axis0);
		VectorScale(texinfo->axis[1], tex_scale[1], tex_axis1);
		tex_axis0[3] = texinfo->offset[0] * tex_scale[0];
		tex_axis1[3] = texinfo->offset[1] * tex_scale[1];
	}

	// The texture basis is not normalized, so we need the lengths of the axes to convert
	// texture coordinates back into world space
	float tex_axis0_inv_square_length = 1.0f / DotProduct(tex_axis0, tex_axis0);
	float tex_axis1_inv_square_length = 1.0f / DotProduct(tex_axis1, tex_axis1);

	// Find the normal of the texture plane
	vec3_t tex_normal;
	CrossProduct(tex_axis0, tex_axis1, tex_normal);
	VectorNormalize(tex_normal);

	float surf_normal_dot_tex_normal = DotProduct(tex_normal, plane);

	if (surf_normal_dot_tex_normal == 0.f)
	{
		// Surface is perpendicular to texture plane, which means we can't un-project
		// texture coordinates back onto the surface. This shouldn't happen though,
		// so it should be safe to skip such lights.
		return;
	}

	// Construct the surface polygon in texture space, and find its texture extents

	poly_t tex_poly;
	tex_poly.len = surf->numsurfedges;

	point2_t tex_min = { FLT_MAX, FLT_MAX };
	point2_t tex_max = { -FLT_MAX, -FLT_MAX };

	for (int i = 0; i < surf->numsurfedges; i++)
	{
		msurfedge_t *src_surfedge = surf->firstsurfedge + i;
		medge_t     *src_edge = src_surfedge->edge;
		mvertex_t   *src_vert = src_edge->v[src_surfedge->vert];
		
		point2_t t;
		t.x = DotProduct(src_vert->point, tex_axis0) + tex_axis0[3];
		t.y = DotProduct(src_vert->point, tex_axis1) + tex_axis1[3];

		tex_poly.v[i] = t;

		tex_min.x = min(tex_min.x, t.x);
		tex_min.y = min(tex_min.y, t.y);
		tex_max.x = max(tex_max.x, t.x);
		tex_max.y = max(tex_max.y, t.y);
	}

	// Instantiate a square polygon for every repetition of the texture in this surface,
	// then clip the original surface against that square polygon.

	for (float y_tile = floorf(tex_min.y); y_tile <= ceilf(tex_max.y); y_tile++)
	{
		for (float x_tile = floorf(tex_min.x); x_tile <= ceilf(tex_max.x); x_tile++)
		{
			float x_min = x_tile + min_light_texcoord[0];
			float x_max = x_tile + max_light_texcoord[0];
			float y_min = y_tile + min_light_texcoord[1];
			float y_max = y_tile + max_light_texcoord[1];

			// The square polygon, for this repetition, according to the extents of emissive pixels

			poly_t clipper;
			clipper.len = 4;
			clipper.v[0].x = x_min; clipper.v[0].y = y_min;
			clipper.v[1].x = x_max; clipper.v[1].y = y_min;
			clipper.v[2].x = x_max; clipper.v[2].y = y_max;
			clipper.v[3].x = x_min; clipper.v[3].y = y_max;

			// Clip it

			poly_t instance;
			clip_polygon(&tex_poly, &clipper, &instance);

			if (instance.len < 3)
			{
				// The square polygon was outside of the original surface
				continue;
			}

			// Map the clipped polygon back onto the surface plane

			vec3_t instance_positions[MAX_POLY_VERTS];
			for (int vert = 0; vert < instance.len; vert++)
			{
				// Find a world space point on the texture projection plane

				vec3_t p0, p1, point_on_texture_plane;
				VectorScale(tex_axis0, (instance.v[vert].x - tex_axis0[3]) * tex_axis0_inv_square_length, p0);
				VectorScale(tex_axis1, (instance.v[vert].y - tex_axis1[3]) * tex_axis1_inv_square_length, p1);
				VectorAdd(p0, p1, point_on_texture_plane);

				// Shoot a ray from that point in the texture normal direction,
				// and intersect it with the surface plane.

				// plane: P.N + d = 0
				// ray: P = At + B
				// (At + B).N + d = 0
				// (A.N)t + B.N + d = 0
				// t = -(B.N + d) / (A.N)

				float bn = DotProduct(point_on_texture_plane, plane);

				float ray_t = -(bn + plane[3]) / surf_normal_dot_tex_normal;

				vec3_t p2;
				VectorScale(tex_normal, ray_t, p2);
				VectorAdd(p2, point_on_texture_plane, instance_positions[vert]);
			}

			// Create triangles for the polygon, using a triangle fan topology

			const int num_triangles = 1; //instance.len - 2;

			for (int i = 0; i < num_triangles; i++)
			{
				const int e = instance.len;

				int i1 = (i + 2) % e;
				int i2 = (i + 1) % e;

				light_poly_t* light = append_light_poly(num_lights, allocated_lights, lights);
				light->material = material;
				light->style = light_style;
				light->type = DYNLIGHT_POLYGON;
				light->emissive_factor = emissive_factor;
				light->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
				VectorCopy(instance_positions[0], light->positions + 0);
				VectorCopy(instance_positions[i1], light->positions + 3);
				VectorCopy(instance_positions[i2], light->positions + 6);
				VectorScale(light_color, emissive_factor, light->color);
				// the clipped polygon is in texture space already, in repeats
				set_light_q3_glow(light, q3_glow, &instance.v[0].x, &instance.v[i1].x, &instance.v[i2].x);
				
				get_triangle_off_center(light->positions, light->off_center, NULL, 1.f);

				if (model_idx < 0)
				{
					// Find the cluster for this triangle
					light->cluster = BSP_PointLeaf(bsp->nodes, light->off_center)->cluster;

					if (light->cluster < 0)
					{
						// Cluster not found - which happens sometimes.
						// The lighting system can't work with lights that have no cluster, so remove the triangle.
						(*num_lights)--;
					}
				}
				else
				{
					// It's a model: cluster will be determined after model instantiation.
					light->cluster = -1;
				}
			}
		}
	}

}

static bool
collect_frames_emissive_info(pbr_material_t* material, bool* entire_texture_emissive, vec2_t min_light_texcoord, vec2_t max_light_texcoord, vec3_t light_color)
{
	*entire_texture_emissive = false;
	min_light_texcoord[0] = min_light_texcoord[1] = 1.0f;
	max_light_texcoord[0] = max_light_texcoord[1] = 0.0f;

	bool any_emissive_valid = false;
	pbr_material_t *current_material = material;
	do
	{
		const image_t *image = current_material->image_emissive;
		if (!image)
		{
			current_material = r_materials + current_material->next_frame;
			continue;
		}
		if(!any_emissive_valid)
		{
			// emissive light color of first frame
			memcpy(light_color, image->light_color, sizeof(vec3_t));
		}
		any_emissive_valid = true;

		*entire_texture_emissive |= image->entire_texture_emissive;
		min_light_texcoord[0] = min(min_light_texcoord[0], image->min_light_texcoord[0]);
		min_light_texcoord[1] = min(min_light_texcoord[1], image->min_light_texcoord[1]);
		max_light_texcoord[0] = max(max_light_texcoord[0], image->max_light_texcoord[0]);
		max_light_texcoord[1] = max(max_light_texcoord[1], image->max_light_texcoord[1]);
		current_material = r_materials + current_material->next_frame;
	} while (current_material != material);

	return any_emissive_valid;
}

/* A light poly shines from its front face only (light_lists.h), and is listed
   in the cluster in front of it. A double-sided glow - Quake III's "cull none",
   q3dm0's console orb, a flat card Quake III turned to face the viewer - shines
   from both. So each of its light polys [first, *num_lights) gets a twin facing
   the other way, listed in the cluster on that side. */
static void
add_back_faces(bsp_t *bsp, int model_idx, int first, int *num_lights, int *allocated_lights, light_poly_t **lights)
{
	const int last = *num_lights;
	for (int i = first; i < last; i++)
	{
		// a copy: append_light_poly may move the array
		light_poly_t light = (*lights)[i];

		// two corners swapped: the normal turns over
		vec3_t corner;
		VectorCopy(light.positions + 3, corner);
		VectorCopy(light.positions + 6, light.positions + 3);
		VectorCopy(corner, light.positions + 6);
		// and their texture coordinates with them
		const float u1 = light.q3_uv[2], v1 = light.q3_uv[3];
		light.q3_uv[2] = light.q3_uv[4];
		light.q3_uv[3] = light.q3_uv[5];
		light.q3_uv[4] = u1;
		light.q3_uv[5] = v1;

		if (!get_triangle_off_center(light.positions, light.off_center, NULL, 1.f))
			continue;

		if (model_idx >= 0)
			light.cluster = -1;     // as its front: set when the model is instanced
		else
		{
			light.cluster = BSP_PointLeaf(bsp->nodes, light.off_center)->cluster;
			if (light.cluster < 0)
				continue;
		}

		*append_light_poly(num_lights, allocated_lights, lights) = light;
	}
}

/*
=================
STAINED GLASS (material.h stained_glass, shader/stained_glass.h)

A stained glass face makes no light from its emissive texture. It makes two:
  * its soft FILL, the share the glass spreads (stained_scatter): ordinary
    light polys over the whole face, in the glass's average colour - what the
    texture lets through on average, the base image times base_factor, less
    the two faces' reflection. copy_light scales it by stained_scatter, so the
    console can change that live.
  * a WINDOW LIGHT (DYNLIGHT_WINDOW), the share that comes straight through:
    one for each window, which is the faces on the same tile of the same
    plane with the same material - a window the BSP compiler cut into
    several faces is still one light, with one lamp behind it. A brush
    entity's windows too (Matt 11-23: e4m5's twelve window01_2 windows are
    func_doors, and cast only the fill): built in the model's own space,
    with no cluster, and moved with the entity each frame by
    instance_model_lights (main.c transform_window_light).
=================
*/
typedef struct {
	const pbr_material_t *material;
	vec3_t n;           // the glass's normal, room side
	float d;            // dot(x, n) on the glass
	vec3_t S, T;        // the texture axes in the glass's plane, 1 = one tile
	float off[2];       // ... and their offsets, the tile's own index taken out
	float rect[4];      // the glass's extent in the tile: u0, v0, u1, v1
	int style;
	float emissive;     // the face's emissive scale (compute_emissive)
	float area;         // to place the cluster point on the biggest face
	vec3_t centre;      // the biggest face's middle
} stained_window_t;

/* Is the texel (x, y) of a w x h image glass, by the material's stained glass
   mask (texture_stained_mask)? Read at the same place on the mask, whatever
   its size; white is glass, the cut is half way. No mask (or one whose pixels
   are not here) = all glass. */
static bool
stained_mask_texel(const image_t *mask, int x, int y, int w, int h)
{
	if (!mask || !mask->pix_data || mask->upload_width <= 0 || mask->upload_height <= 0)
		return true;
	const int mx = min(mask->upload_width - 1, (int)(((int64_t)x * mask->upload_width) / w));
	const int my = min(mask->upload_height - 1, (int)(((int64_t)y * mask->upload_height) / h));
	const size_t i = (size_t)my * mask->upload_width + mx;
	if (mask->pixel_format == PF_R16_UNORM)
		return ((const uint16_t *)mask->pix_data)[i] >= 32768;
	if (mask->pixel_format == PF_R8G8B8A8_UNORM)
		return mask->pix_data[i * 4] >= 128;
	return true;
}

/* The part of the tile a stained glass mask says is glass: the box round its
   white texels, in tile coordinates (u0, v0, u1, v1; v down the picture, as
   the texture runs). False with no mask (or none readable here) - the whole
   tile then - and with a mask that has no glass at all, *empty set. */
static bool
stained_mask_bounds(const pbr_material_t *material, float out[4], bool *empty)
{
	*empty = false;
	const image_t *mask = material->image_stained_mask;
	if (!mask || !mask->pix_data || mask->upload_width <= 0 || mask->upload_height <= 0)
		return false;
	if (mask->pixel_format != PF_R16_UNORM && mask->pixel_format != PF_R8G8B8A8_UNORM)
		return false;

	const int w = mask->upload_width, h = mask->upload_height;
	int x0 = w, y0 = h, x1 = -1, y1 = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (stained_mask_texel(mask, x, y, w, h))
			{
				x0 = min(x0, x); x1 = max(x1, x);
				y0 = min(y0, y); y1 = max(y1, y);
			}

	if (x1 < 0)
	{
		*empty = true;
		return false;
	}
	out[0] = (float)x0 / (float)w;
	out[1] = (float)y0 / (float)h;
	out[2] = (float)(x1 + 1) / (float)w;
	out[3] = (float)(y1 + 1) / (float)h;
	return true;
}

// The glass's average colour: what the texture lets through, linear - over
// the whole tile, so a stained glass mask's wall texels count as letting
// nothing through (the fill is the glass's colour times its share of the tile)
static void
stained_average_transmission(const pbr_material_t *material, vec3_t out)
{
	VectorSet(out, 0.5f, 0.5f, 0.5f);
	const image_t *image = material->image_base;
	if (!image || !image->pix_data || image->pixel_format != PF_R8G8B8A8_UNORM)
		return;

	const int w = image->upload_width, h = image->upload_height;
	const int count = w * h;
	if (count <= 0)
		return;

	const image_t *mask = material->image_stained_mask;
	const float f = material->base_factor;
	double sum[3] = { 0, 0, 0 };
	const byte *p = image->pix_data;
	for (int i = 0; i < count; i++, p += 4)
	{
		if (mask && !stained_mask_texel(mask, i % w, i / w, w, h))
			continue;
		for (int c = 0; c < 3; c++)
		{
			float x = image->is_srgb ? decode_srgb(p[c]) : (float)p[c] * (1.f / 255.f);
			sum[c] += max(0.f, min(1.f, x * f));
		}
	}
	// the two faces' reflection at normal incidence (stained_face_transmit)
	const float faces = 0.96f * 0.96f;
	for (int c = 0; c < 3; c++)
		out[c] = (float)(sum[c] / (double)count) * faces;
}

// two of a window's extents, 12 bits each over -1..2 of the tile, rounded outward
static float
stained_pack_extent(float lo, float hi)
{
	int a = (int)floorf((lo + 1.f) * (4095.f / 3.f));
	int b = (int)ceilf((hi + 1.f) * (4095.f / 3.f));
	a = max(0, min(4095, a));
	b = max(0, min(4095, b));
	return (float)(a | (b << 12));
}

/* One stained glass face: its fill now, and its share of a window, which is
   made into a light when all the faces are in (stained_emit_windows). */
static void
collect_stained_face(bsp_t *bsp, mface_t *surf, mtexinfo_t *texinfo, pbr_material_t *material, int model_idx,
                     int light_style, float emissive, stained_window_t **windows, int *num_windows, int *allocated_windows,
                     int *num_lights, int *allocated_lights, light_poly_t **lights)
{
	const bool both_faces = (material->flags & MATERIAL_FLAG_DOUBLE_SIDED) != 0;
	const int first_light = *num_lights;

	vec3_t fill;
	stained_average_transmission(material, fill);
	collect_one_light_poly_entire_texture(bsp, surf, texinfo, material, model_idx, fill, emissive, light_style,
	                                      false, num_lights, allocated_lights, lights);
	if (both_faces)
		add_back_faces(bsp, model_idx, first_light, num_lights, allocated_lights, lights);

	if (!surf->plane || surf->numsurfedges < 3 || surf->numsurfedges > MAX_FACE_VERTS)
		return;

	// the face's front, where the room is
	stained_window_t w;
	memset(&w, 0, sizeof(w));
	const float side = (surf->drawflags & DSURF_PLANEBACK) ? -1.f : 1.f;
	VectorScale(surf->plane->normal, side, w.n);
	w.d = surf->plane->dist * side;

	// the face's texture axes and offsets, as create_poly maps it
	vec3_t S, T;
	float off[2];
	if (surf->uv_mapped)
	{
		VectorCopy(surf->uv_axis[0], S);
		VectorCopy(surf->uv_axis[1], T);
		off[0] = surf->uv_axis[0][3];
		off[1] = surf->uv_axis[1][3];
	}
	else
	{
		float sc[2] = { 1.f / (float)max(1, material->original_width), 1.f / (float)max(1, material->original_height) };
		if (texinfo->c.flags & SURF_N64_UV)
		{
			sc[0] *= 0.5f;
			sc[1] *= 0.5f;
		}
		VectorScale(texinfo->axis[0], sc[0], S);
		VectorScale(texinfo->axis[1], sc[1], T);
		off[0] = texinfo->offset[0] * sc[0];
		off[1] = texinfo->offset[1] * sc[1];
	}

	// into the plane: on it, dot(x, S) = dot(x, S - (S.n) n) + (S.n) d
	const float sn = DotProduct(S, w.n), tn = DotProduct(T, w.n);
	VectorMA(S, -sn, w.n, w.S);
	VectorMA(T, -tn, w.n, w.T);
	off[0] += sn * w.d;
	off[1] += tn * w.d;

	vec3_t cross;
	CrossProduct(w.S, w.T, cross);
	if (VectorLength(cross) < 1e-12f)
		return;

	// the face's extent in texture repeats, and its middle
	float lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
	vec3_t mid = { 0, 0, 0 };
	for (int i = 0; i < surf->numsurfedges; i++)
	{
		const msurfedge_t *se = surf->firstsurfedge + i;
		const float *p = se->edge->v[se->vert]->point;
		const float u = DotProduct(p, w.S) + off[0];
		const float v = DotProduct(p, w.T) + off[1];
		lo[0] = min(lo[0], u); hi[0] = max(hi[0], u);
		lo[1] = min(lo[1], v); hi[1] = max(hi[1], v);
		VectorAdd(mid, p, mid);
	}
	VectorScale(mid, 1.f / (float)surf->numsurfedges, mid);
	if (!(hi[0] - lo[0] > 1e-4f) || !(hi[1] - lo[1] > 1e-4f))
		return;

	// the tile its middle is on
	const float tile[2] = { floorf(0.5f * (lo[0] + hi[0])), floorf(0.5f * (lo[1] + hi[1])) };
	w.off[0] = off[0] - tile[0];
	w.off[1] = off[1] - tile[1];
	w.rect[0] = lo[0] - tile[0];
	w.rect[1] = lo[1] - tile[1];
	w.rect[2] = hi[0] - tile[0];
	w.rect[3] = hi[1] - tile[1];
	w.material = material;
	w.style = light_style;
	w.emissive = emissive;
	w.area = (hi[0] - lo[0]) * (hi[1] - lo[1]);
	VectorCopy(mid, w.centre);

	// a window already begun: the same glass, plane and tile
	for (int k = 0; k < *num_windows; k++)
	{
		stained_window_t *o = *windows + k;
		if (o->material != w.material || DotProduct(o->n, w.n) < 0.9999f || fabsf(o->d - w.d) > 0.25f)
			continue;
		vec3_t ds, dt;
		VectorSubtract(o->S, w.S, ds);
		VectorSubtract(o->T, w.T, dt);
		if (DotProduct(ds, ds) > 1e-10f || DotProduct(dt, dt) > 1e-10f)
			continue;
		if (fabsf(o->off[0] - w.off[0]) > 1e-3f || fabsf(o->off[1] - w.off[1]) > 1e-3f)
			continue;

		o->rect[0] = min(o->rect[0], w.rect[0]);
		o->rect[1] = min(o->rect[1], w.rect[1]);
		o->rect[2] = max(o->rect[2], w.rect[2]);
		o->rect[3] = max(o->rect[3], w.rect[3]);
		if (w.area > o->area)
		{
			o->area = w.area;
			VectorCopy(w.centre, o->centre);
		}
		return;
	}

	if (*num_windows == *allocated_windows)
	{
		*allocated_windows = max(*allocated_windows * 2, 16);
		*windows = Z_Realloc(*windows, *allocated_windows * sizeof(stained_window_t));
	}
	(*windows)[(*num_windows)++] = w;
}

// the windows collect_stained_face gathered, as DYNLIGHT_WINDOW lights - for
// a brush model (model_idx >= 0) in its own space, placed when it is instanced
static void
stained_emit_windows(bsp_t *bsp, int model_idx, const stained_window_t *windows, int num_windows,
                     int *num_lights, int *allocated_lights, light_poly_t **lights)
{
	const pbr_material_t *bounds_of = NULL;
	float glass[4] = { 0, 0, 1, 1 };
	bool has_bounds = false, no_glass = false;

	for (int k = 0; k < num_windows; k++)
	{
		const stained_window_t *w = windows + k;

		// with a glass mask (texture_stained_mask), only its glass lets the
		// light through: the window shrinks to the box round it, so the light
		// is not picked for a floor that would see it through the wall part
		float rect[4] = { w->rect[0], w->rect[1], w->rect[2], w->rect[3] };
		// one pass over the mask per material, not per window
		if (w->material != bounds_of)
		{
			bounds_of = w->material;
			has_bounds = stained_mask_bounds(w->material, glass, &no_glass);
		}
		if (has_bounds)
		{
			rect[0] = max(rect[0], glass[0]);
			rect[1] = max(rect[1], glass[1]);
			rect[2] = min(rect[2], glass[2]);
			rect[3] = min(rect[3], glass[3]);
			if (!(rect[2] > rect[0]) || !(rect[3] > rect[1]))
				continue;
		}
		else if (no_glass)
			continue;

		light_poly_t light;
		memset(&light, 0, sizeof(light));
		VectorCopy(w->S, light.positions + 0);
		VectorCopy(w->T, light.positions + 3);
		light.positions[6] = stained_pack_extent(rect[0], rect[2]);
		light.positions[7] = stained_pack_extent(rect[1], rect[3]);
		light.positions[8] = 0.f;
		light.window[0] = w->off[0];
		light.window[1] = w->off[1];
		light.window[2] = w->d;

		vec3_t cross;
		CrossProduct(w->S, w->T, cross);
		light.spot_emission_profile = DotProduct(cross, w->n) < 0.f ? 1 : 0;

		// the irradiance on the glass at the middle of the tile, before
		// emissive_factor x light_factor (copy_light): pi x the face's scale,
		// so that fully spread glass glows like an emissive texture would
		VectorSet(light.color, (float)M_PI * w->emissive, (float)M_PI * w->emissive, (float)M_PI * w->emissive);
		light.emissive_factor = w->emissive;
		light.material = (pbr_material_t *)w->material;
		light.style = w->style;
		light.type = DYNLIGHT_WINDOW;
		light.volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
		set_light_q3_glow(&light, false, NULL, NULL, NULL);

		// the room just in front of the biggest of its faces; a brush model's
		// is found where the entity is (instance_model_lights), which also
		// drops a window whose front is in the wall - a door's edges, or the
		// door once it has slid away
		VectorMA(w->centre, 2.f, w->n, light.off_center);
		if (model_idx >= 0)
			light.cluster = -1;
		else
		{
			light.cluster = BSP_PointLeaf(bsp->nodes, light.off_center)->cluster;
			if (light.cluster < 0)
				continue;
		}

		*append_light_poly(num_lights, allocated_lights, lights) = light;
	}
}

/* A face that moves (s_deform_faces): the light polys just made for it, from
   first_light on, go to its q3deform model, which carries them with it each
   frame as a model's lights - q3dm16's ceiling lamps, whose tile
   (bot_light, q3map_surfacelight) bobs with the fixture. Left here if no model
   took them. */
static void
give_lights_to_deform(bsp_t *bsp, const mface_t *surf, const pbr_material_t *material, int model_idx,
                      int first_light, int *num_lights, const light_poly_t *lights)
{
	if (model_idx >= 0 || !s_deform_faces || *num_lights <= first_light)
		return;
	const ptrdiff_t f = surf - bsp->faces;
	if (f < 0 || f >= bsp->numfaces || !s_deform_faces[f])
		return;
	if (vkpt_q3_deform_take_lights(material, lights + first_light, *num_lights - first_light))
		*num_lights = first_light;
}

static void
collect_light_polys(bsp_mesh_t *wm, bsp_t *bsp, int model_idx, int* num_lights, int* allocated_lights, light_poly_t** lights)
{
	mface_t *surfaces = model_idx < 0 ? bsp->faces : bsp->models[model_idx].firstface;
	int num_faces = model_idx < 0 ? bsp->numfaces : bsp->models[model_idx].numfaces;

	// stained glass's windows, made into lights at the end (collect_stained_face)
	stained_window_t *windows = NULL;
	int num_windows = 0, allocated_windows = 0;

	for (int i = 0; i < num_faces; i++)
	{
		mface_t *surf = surfaces + i;

		if (model_idx < 0 && belongs_to_model(bsp, surf))
			continue;

		mtexinfo_t *texinfo = surf->texinfo;

		if(!texinfo->material)
			continue;

		// a Quake 1 teleporter's gate is a window, without its texture's glow
		// (collect_surfaces)
		if (bsp->q1 && model_idx < 0 && vkpt_cameras_q1_face(wm, surf) >= 0)
			continue;

		/* Everything below asks about the material on this face, and all of it
		   has to see the per-surface instance if one has claimed the face -
		   the emissive texture the light colour is sampled from, the radiance,
		   the light style, and the material the light poly is tagged with. */
		pbr_material_t *material = face_effective_material(bsp, surf);

		if (!material)
			continue;

		int flags = surf->drawflags;
		if (surf->texinfo) flags |= surf->texinfo->c.flags;

		// Don't create light polys from SKY surfaces, those are handled separately.
		// Sometimes, textures with a light fixture are used on sky polys (like in rlava1),
		// and that leads to subdivision of those sky polys into a large number of lights.
		if (flags & SURF_SKY)
			continue;

		if (is_invisible_lava(flags, material))
			continue;

		// not drawn, so no light either (collect_surfaces)
		if (MAT_IsKind(material->flags, MATERIAL_KIND_INVISIBLE))
			continue;

		// stained glass: a fill and a window light, not its emissive texture's
		if (material->stained_glass && MAT_IsKind(material->flags, MATERIAL_KIND_REGULAR))
		{
			const float emissive = compute_emissive(texinfo, material);
			if (emissive != 0.f)
			{
				const int style = material->light_styles ? get_surf_light_style(surf) : 0;
				collect_stained_face(bsp, surf, texinfo, material, model_idx, style, emissive,
				                     &windows, &num_windows, &allocated_windows, num_lights, allocated_lights, lights);
			}
			continue;
		}

		// Check if any animation frame is a light material
		bool any_light_frame = false;
		{
			pbr_material_t *current_material = material;
			do
			{
				any_light_frame |= is_light_material(current_material->flags);
				current_material = r_materials + current_material->next_frame;
			} while (current_material != material);
		}
		if(!any_light_frame)
			continue;

		// Collect emissive texture info from across frames
		bool entire_texture_emissive;
		vec2_t min_light_texcoord;
		vec2_t max_light_texcoord;
		vec3_t light_color;

		/* A Quake III shader drawn stage by stage: what it emits, and when, is
		   the program's (q3stages.c vkpt_q3_light_glow), worked out every frame
		   for each light poly. The emissive image only says where on a tile the
		   glow is - and when it moves, or there is more than one (a pulse
		   sweeping along a trim over a glow), it does not say that either, so
		   the whole face is the light. */
		const bool q3_glow = material->q3_program != 0;

		if (!collect_frames_emissive_info(material, &entire_texture_emissive, min_light_texcoord, max_light_texcoord, light_color))
		{
			// This algorithm relies on information from the emissive texture,
			// specifically the extents of the emissive pixels in that texture.
			// Ignore surfaces that don't have an emissive texture attached.
			if (!q3_glow)
				continue;
			entire_texture_emissive = true;
			VectorClear(light_color);
		}

		if (q3_glow && vkpt_q3_program_glow_moves(material))
			entire_texture_emissive = true;

		float emissive_factor = compute_emissive(texinfo, material);
		if(emissive_factor == 0)
			continue;

		int light_style = (material->light_styles) ? get_surf_light_style(surf) : 0;

		const int first_light = *num_lights;
		const bool both_faces = (material->flags & MATERIAL_FLAG_DOUBLE_SIDED) != 0;

		if (entire_texture_emissive)
		{
			collect_one_light_poly_entire_texture(bsp, surf, texinfo, material, model_idx, light_color, emissive_factor, light_style,
												  q3_glow, num_lights, allocated_lights, lights);
			if (both_faces)
				add_back_faces(bsp, model_idx, first_light, num_lights, allocated_lights, lights);
			give_lights_to_deform(bsp, surf, material, model_idx, first_light, num_lights, *lights);
			continue;
		}

		vec4_t plane;
		if (!get_surf_plane_equation(surf, plane))
		{
			// It's possible that some polygons in the game are degenerate, ignore these.
			continue;
		}

		float tex_scale[2] = { 1.0f / material->original_width, 1.0f / material->original_height };

		collect_one_light_poly(bsp, surf, texinfo, material, model_idx, plane,
							   tex_scale, min_light_texcoord, max_light_texcoord,
							   light_color, emissive_factor, light_style, q3_glow,
							   num_lights, allocated_lights, lights);
		if (both_faces)
			add_back_faces(bsp, model_idx, first_light, num_lights, allocated_lights, lights);
		give_lights_to_deform(bsp, surf, material, model_idx, first_light, num_lights, *lights);
	}

	if (num_windows > 0)
	{
		const int before = *num_lights;
		stained_emit_windows(bsp, model_idx, windows, num_windows, num_lights, allocated_lights, lights);
		Com_DPrintf("Stained glass: %d window lights\n", *num_lights - before);
	}
	Z_Free(windows);
}

static void
collect_sky_and_lava_light_polys(bsp_mesh_t *wm, bsp_t* bsp)
{
	for (int i = 0; i < bsp->numfaces; i++)
	{
		mface_t *surf = bsp->faces + i;

		if (belongs_to_model(bsp, surf))
			continue;

		if (!surf->texinfo)
			continue;

		int flags = surf->drawflags;
		if (surf->texinfo) flags |= surf->texinfo->c.flags;

		bool is_sky = !!(flags & SURF_SKY);
		bool is_light = !!(flags & SURF_LIGHT);
		bool is_nodraw = !!(flags & SURF_NODRAW);
		pbr_material_t *material = face_effective_material(bsp, surf);

		bool is_lava = material ? MAT_IsKind(material->flags, MATERIAL_KIND_LAVA) : false;

		is_lava &= (material && material->image_emissive != NULL);

		if (!is_sky && !is_lava)
			continue;

		if (is_lava && is_invisible_lava(flags, material))
			continue;

		float positions[3 * MAX_FACE_VERTS];

		if (surf->numsurfedges > MAX_FACE_VERTS)
			continue;

		for (int i = 0; i < surf->numsurfedges; i++)
		{
			msurfedge_t *src_surfedge = surf->firstsurfedge + i;
			medge_t     *src_edge = src_surfedge->edge;
			mvertex_t   *src_vert = src_edge->v[src_surfedge->vert];

			float *p = positions + i * 3;

			VectorCopy(src_vert->point, p);
		}

		int num_vertices = surf->numsurfedges;
		remove_collinear_edges(positions, NULL, NULL, &num_vertices);

		const int num_triangles = num_vertices - 2;

		for (int i = 0; i < num_triangles; i++)
		{
			int i1 = (i + 2) % num_vertices;
			int i2 = (i + 1) % num_vertices;

			light_poly_t light;
			VectorCopy(positions, light.positions + 0);
			VectorCopy(positions + i1 * 3, light.positions + 3);
			VectorCopy(positions + i2 * 3, light.positions + 6);

			if (is_sky)
			{
				VectorSet(light.color, -1.f, -1.f, -1.f); // special value for the sky
				light.material = 0;
			}
			else
			{
				VectorCopy(material->image_emissive->light_color, light.color);
				light.material = material;
			}

			light.style = 0;
			light.type = DYNLIGHT_POLYGON;
			light.volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
			set_light_q3_glow(&light, false, NULL, NULL, NULL);

			if (!get_triangle_off_center(light.positions, light.off_center, NULL, 1.f))
				continue;

			light.cluster = BSP_PointLeaf(bsp->nodes, light.off_center)->cluster;
			
			if (is_sky_or_lava_cluster(wm, surf, light.cluster, surf->texinfo->material->flags) ||
				(cvar_pt_bsp_sky_lights->integer && is_sky && is_light && (cvar_pt_bsp_sky_lights->integer > 1 || !is_nodraw)) ||
				q1_sky_light(bsp, flags))
			{
				light_poly_t* list_light = append_light_poly(&wm->num_light_polys, &wm->allocated_light_polys, &wm->light_polys);
				memcpy(list_light, &light, sizeof(light_poly_t));
			}
		}
	}
}

static bool
is_model_transparent(bsp_mesh_t *wm, bsp_model_t *model)
{
	if (model->geometry.num_geometries == 0)
		return false;

	for (uint prim_idx = 0; prim_idx < model->geometry.prim_counts[0]; prim_idx++)
	{
		uint prim = model->geometry.prim_offsets[0] + prim_idx;
		uint material = wm->primitives[prim].material_id;
		
		if (!(MAT_IsKind(material, MATERIAL_KIND_SLIME) || MAT_IsKind(material, MATERIAL_KIND_WATER) || MAT_IsKind(material, MATERIAL_KIND_GLASS) || MAT_IsKind(material, MATERIAL_KIND_TRANSPARENT)))
			return false;
	}

	return true;
}

static bool
is_model_masked(bsp_mesh_t *wm, bsp_model_t *model)
{
	if (model->geometry.num_geometries == 0)
		return false;

	for (uint prim_idx = 0; prim_idx < model->geometry.prim_counts[0]; prim_idx++)
	{
		uint prim = model->geometry.prim_offsets[0] + prim_idx;
		uint material = wm->primitives[prim].material_id;

		const pbr_material_t* mat = MAT_ForIndex((int)(material & MATERIAL_INDEX_MASK));
		
		if (mat && mat->image_mask)
			return true;
	}

	return false;
}

void
append_aabb(const VboPrimitive* primitives, uint32_t numprims, float* aabb_min, float* aabb_max)
{
	for (uint32_t prim_idx = 0; prim_idx < numprims; prim_idx++)
	{
		const VboPrimitive* prim = primitives + prim_idx;

		for (uint32_t vert_idx = 0; vert_idx < 3; vert_idx++)
		{
			const float* position;
			switch (vert_idx)
			{
			case 0:  position = prim->pos0; break;
			case 1:  position = prim->pos1; break;
			default: position = prim->pos2; break;
			}

			aabb_min[0] = min(aabb_min[0], position[0]);
			aabb_min[1] = min(aabb_min[1], position[1]);
			aabb_min[2] = min(aabb_min[2], position[2]);

			aabb_max[0] = max(aabb_max[0], position[0]);
			aabb_max[1] = max(aabb_max[1], position[1]);
			aabb_max[2] = max(aabb_max[2], position[2]);
		}
	}
}

void
compute_aabb(const VboPrimitive* primitives, uint32_t numprims, float* aabb_min, float* aabb_max)
{
	VectorSet(aabb_min, FLT_MAX, FLT_MAX, FLT_MAX);
	VectorSet(aabb_max, -FLT_MAX, -FLT_MAX, -FLT_MAX);

	append_aabb(primitives, numprims, aabb_min, aabb_max);
}

void
compute_world_tangents(bsp_t* bsp, bsp_mesh_t* wm)
{
	if (bsp->basisvectors)
		return;

	// Compute the tangent basis if it's not provided by the BSPX

	for (int idx_tri = 0; idx_tri < wm->num_primitives; ++idx_tri)
	{
		VboPrimitive* prim = wm->primitives + idx_tri;
		
		float const * pA = prim->pos0;
		float const * pB = prim->pos1;
		float const * pC = prim->pos2;

		float const * tA = prim->uv0;
		float const * tB = prim->uv1;
		float const * tC = prim->uv2;

		vec3_t dP0, dP1;
		VectorSubtract(pB, pA, dP0);
		VectorSubtract(pC, pA, dP1);

		vec2_t dt0, dt1;
		Vector2Subtract(tB, tA, dt0);
		Vector2Subtract(tC, tA, dt1);
		
		float r = 1.f / (dt0[0] * dt1[1] - dt1[0] * dt0[1]);

		vec3_t sdir = {
			(dt1[1] * dP0[0] - dt0[1] * dP1[0]) * r,
			(dt1[1] * dP0[1] - dt0[1] * dP1[1]) * r,
			(dt1[1] * dP0[2] - dt0[1] * dP1[2]) * r };

		vec3_t normal;
		CrossProduct(dP0, dP1, normal);
		VectorNormalize(normal);

		uint32_t encoded_normal = encode_normal(normal);
		prim->normals[0] = encoded_normal;
		prim->normals[1] = encoded_normal;
		prim->normals[2] = encoded_normal;

		vec3_t tangent;

		vec3_t t;
		VectorScale(normal, DotProduct(normal, sdir), t);
		VectorSubtract(sdir, t, t);
		VectorNormalize2(t, tangent); // Graham-Schmidt : t = normalize(t - n * (n.t))

		uint32_t encoded_tangent = encode_normal(tangent);
		prim->tangents[0] = encoded_tangent;
		prim->tangents[1] = encoded_tangent;
		prim->tangents[2] = encoded_tangent;

	}
}

static void
load_sky_and_lava_clusters(bsp_mesh_t* wm, const char* map_name)
{
	wm->num_sky_clusters = 0;
	wm->all_lava_emissive = false;

    // try a map-specific file first
    char filename[MAX_QPATH];
    Q_snprintf(filename, sizeof(filename), "maps/sky/%s.txt", map_name);

    char* filebuf = NULL;
    FS_LoadFile(filename, (void**)&filebuf);
    
    if (!filebuf)
    {
        Com_DPrintf("Couldn't read %s\n", filename);
        return;
    }

	char const * ptr = (char const *)filebuf;
	char linebuf[1024];

	while (sgets(linebuf, sizeof(linebuf), &ptr))
	{
		{ char* t = strchr(linebuf, '#'); if (t) *t = 0; }   // remove comments
		{ char* t = strchr(linebuf, '\n'); if (t) *t = 0; }  // remove newline

		const char* delimiters = " \t\r\n";
		
		const char* word = strtok(linebuf, delimiters);
		while (word)
		{
			assert(wm->num_sky_clusters < MAX_SKY_CLUSTERS);

			if (!strcmp(word, "!all_lava"))
				wm->all_lava_emissive = true;
			else
			{
				int cluster = atoi(word);
				wm->sky_clusters[wm->num_sky_clusters++] = cluster;
			}

			word = strtok(NULL, delimiters);
		}
	}

	Z_Free(filebuf);
}

static void
mark_clusters_with_sky(const bsp_mesh_t* wm, const model_geometry_t* geom, uint8_t* clusters_with_sky)
{
	for (uint32_t prim_idx = 0; prim_idx < geom->prim_counts[0]; prim_idx++)
	{
		uint32_t prim = geom->prim_offsets[0] + prim_idx;

		int cluster = wm->primitives[prim].cluster;
		if (cluster < 0) continue;
		if ((cluster >> 3) < VIS_MAX_BYTES)
			clusters_with_sky[cluster >> 3] |= (1 << (cluster & 7));
	}
}

static void
compute_sky_visibility(bsp_mesh_t *wm, bsp_t *bsp)
{
	memset(wm->sky_visibility, 0, VIS_MAX_BYTES);
	memset(wm->sky_cluster_mask, 0, VIS_MAX_BYTES);

	if (wm->geom_sky.num_geometries == 0 && wm->geom_custom_sky.num_geometries == 0)
		return; 

	uint32_t numclusters = bsp->vis->numclusters;

	uint8_t clusters_with_sky[VIS_MAX_BYTES] = { 0 };

	mark_clusters_with_sky(wm, &wm->geom_sky, clusters_with_sky);
	mark_clusters_with_sky(wm, &wm->geom_custom_sky, clusters_with_sky);

	// keep the strict version too - the volumetric fog needs "this cluster has
	// sky in it", not "this cluster might be able to see sky from somewhere"
	memcpy(wm->sky_cluster_mask, clusters_with_sky, VIS_MAX_BYTES);

	for (uint32_t cluster = 0; cluster < numclusters; cluster++)
	{
		if (clusters_with_sky[cluster >> 3] & (1 << (cluster & 7)))
		{
			byte* mask = BSP_GetPvs(bsp, (int)cluster);

			for (int i = 0; i < bsp->visrowsize; i++)
				wm->sky_visibility[i] |= mask[i];
		}
	}
}

static void
compute_cluster_aabbs(bsp_mesh_t* wm)
{
	wm->cluster_aabbs = Z_Malloc(wm->num_clusters * sizeof(aabb_t));
	for (int c = 0; c < wm->num_clusters; c++)
	{
		VectorSet(wm->cluster_aabbs[c].mins, FLT_MAX, FLT_MAX, FLT_MAX);
		VectorSet(wm->cluster_aabbs[c].maxs, -FLT_MAX, -FLT_MAX, -FLT_MAX);
	}

	for (uint prim_idx = 0; prim_idx < wm->geom_opaque.prim_counts[0]; prim_idx++)
	{
		int c = wm->primitives[prim_idx].cluster;

		if(c < 0 || c >= wm->num_clusters)
			continue;

		aabb_t* aabb = wm->cluster_aabbs + c;
		
		const VboPrimitive* prim = wm->primitives + prim_idx;

		for (int i = 0; i < 3; i++)
		{
			const float* position;
			switch(i)
			{
			case 0:  position = prim->pos0; break;
			case 1:  position = prim->pos1; break;
			default: position = prim->pos2; break;
			}

			aabb->mins[0] = min(aabb->mins[0], position[0]);
			aabb->mins[1] = min(aabb->mins[1], position[1]);
			aabb->mins[2] = min(aabb->mins[2], position[2]);

			aabb->maxs[0] = max(aabb->maxs[0], position[0]);
			aabb->maxs[1] = max(aabb->maxs[1], position[1]);
			aabb->maxs[2] = max(aabb->maxs[2], position[2]);
		}
	}
}

static void
get_aabb_corner(const aabb_t* aabb, int corner_idx, vec3_t corner)
{
	corner[0] = (corner_idx & 1) ? aabb->maxs[0] : aabb->mins[0];
	corner[1] = (corner_idx & 2) ? aabb->maxs[1] : aabb->mins[1];
	corner[2] = (corner_idx & 4) ? aabb->maxs[2] : aabb->mins[2];
}

static bool
light_affects_cluster(light_poly_t* light, const aabb_t* aabb)
{
	// Empty cluster, nothing is visible
	if (aabb->mins[0] > aabb->maxs[0])
		return false;

	// a stained glass window light (stained_emit_windows): positions are its
	// texture axes, and the glass's plane is n . x = window[2]
	if (light->type == DYNLIGHT_WINDOW)
	{
		vec3_t n;
		CrossProduct(light->positions + 0, light->positions + 3, n);
		VectorNormalize(n);
		if (light->spot_emission_profile & 1)
			VectorInverse(n);
		for (int corner_idx = 0; corner_idx < 8; corner_idx++)
		{
			vec3_t corner;
			get_aabb_corner(aabb, corner_idx, corner);
			if (DotProduct(n, corner) - light->window[2] > 0.f)
				return true;
		}
		return false;
	}

	const float* v0 = light->positions + 0;
	const float* v1 = light->positions + 3;
	const float* v2 = light->positions + 6;
	
	// Get the light plane equation
	vec3_t e1, e2, normal;
	VectorSubtract(v1, v0, e1);
	VectorSubtract(v2, v0, e2);
	CrossProduct(e1, e2, normal);
	VectorNormalize(normal);
	
	float plane_distance = -DotProduct(normal, v0);

	bool all_culled = true;

	// If all 8 corners of the cluster's AABB are behind the light, it's definitely invisible
	for (int corner_idx = 0; corner_idx < 8; corner_idx++)
	{
		vec3_t corner;
		get_aabb_corner(aabb, corner_idx, corner);

		float side = DotProduct(normal, corner) + plane_distance;
		if (side > 0)
			all_culled = false;
	}

	if (all_culled)
	{
		return false;
	}

	return true;
}

static void
build_cluster_light_lists(bsp_mesh_t *wm, bsp_t *bsp, light_poly_t *lights, int num_lights, int index_base,
                          int max_total, int *out_num, int **out_offsets, int **out_lights)
{
#define MAX_LIGHTS_PER_CLUSTER 3064
	int* cluster_lights = Z_Malloc(MAX_LIGHTS_PER_CLUSTER * wm->num_clusters * sizeof(int));
	int* cluster_light_counts = Z_Mallocz(wm->num_clusters * sizeof(int));

	// Construct an array of visible lights for each cluster.
	// The array is in `cluster_lights`, with MAX_LIGHTS_PER_CLUSTER stride.

	for (int nlight = 0; nlight < num_lights; nlight++)
	{
		light_poly_t* light = lights + nlight;

		if(light->cluster < 0)
			continue;

		const byte* pvs = (const byte*)BSP_GetPvs(bsp, light->cluster);

		FOREACH_BIT_BEGIN(pvs, bsp->visrowsize, other_cluster)
			aabb_t* cluster_aabb = wm->cluster_aabbs + other_cluster;
			if (light_affects_cluster(light, cluster_aabb))
			{
				int* num_cluster_lights = cluster_light_counts + other_cluster;
				if (*num_cluster_lights < MAX_LIGHTS_PER_CLUSTER)
				{
					cluster_lights[other_cluster * MAX_LIGHTS_PER_CLUSTER + *num_cluster_lights] = index_base + nlight;
					(*num_cluster_lights)++;
				}
			}
		FOREACH_BIT_END
	}

	// Count the total number of cluster <-> light relations to allocate memory

	int num_relations = 0;
	for (int cluster = 0; cluster < wm->num_clusters; cluster++)
	{
		num_relations += cluster_light_counts[cluster];
	}

	if (max_total > 0 && num_relations > max_total)
	{
		Com_WPrintf("%d cluster <-> light relations, more than the limit of %d; the rest are dropped.\n",
		            num_relations, max_total);
		num_relations = max_total;
	}

	int *list = Z_Mallocz(max(num_relations, 1) * sizeof(int));
	int *offsets = Z_Mallocz((wm->num_clusters + 1) * sizeof(int));

	// Com_Printf("Total interactions: %d, culled bbox: %d, culled proj: %d\n", num_relations, lights_culled_bbox, lights_culled_proj);

	// Compact the previously constructed array into `list`

	int list_offset = 0;
	for (int cluster = 0; cluster < wm->num_clusters; cluster++)
	{
		assert(list_offset >= 0);
		offsets[cluster] = list_offset;
		int count = min(cluster_light_counts[cluster], num_relations - list_offset);
		memcpy(
			list + list_offset,
			cluster_lights + MAX_LIGHTS_PER_CLUSTER * cluster,
			count * sizeof(int));
		list_offset += count;
	}
	offsets[wm->num_clusters] = list_offset;

	*out_num = list_offset;
	*out_offsets = offsets;
	*out_lights = list;

	Z_Free(cluster_lights);
	Z_Free(cluster_light_counts);
#undef MAX_LIGHTS_PER_CLUSTER
}

static void
collect_cluster_lights(bsp_mesh_t *wm, bsp_t *bsp)
{
	build_cluster_light_lists(wm, bsp, wm->light_polys, wm->num_light_polys, 0, 0,
	                          &wm->num_cluster_lights, &wm->cluster_light_offsets, &wm->cluster_lights);
}

/*
Lava as a FOG-ONLY light.

Lava that the sky/lava cluster files do not promote to a poly-light (nearly all
of it - only city1 and q2dm6 say !all_lava) lights its surroundings only through
bounce rays hitting its emissive texture, and the fog in-scatters from the light
lists alone, so it made no fog. Promoting it to a real light would change how it
lights the room. Instead it goes into a separate list that only the fog walks:
the path tracer, ReSTIR and the primitives' MATERIAL_FLAG_LIGHT are untouched.

A face counts if its emitting side - the +normal side of the light triangle,
which is the side light_affects_cluster and the fog's facing test keep - is open
air. That drops the copy of the surface seen from inside the lava.
*/
static void
collect_fog_lava_lights(bsp_mesh_t *wm, bsp_t *bsp)
{
	wm->num_fog_light_polys = 0;

	if (!cvar_pt_fog_lava->integer)
		return;

	bool truncated = false;

	for (int i = 0; i < bsp->numfaces && !truncated; i++)
	{
		mface_t *surf = bsp->faces + i;

		if (belongs_to_model(bsp, surf) || !surf->texinfo)
			continue;

		pbr_material_t *material = face_effective_material(bsp, surf);
		if (!material || !material->image_emissive || !MAT_IsKind(material->flags, MATERIAL_KIND_LAVA))
			continue;

		if (is_invisible_lava(surf->drawflags | surf->texinfo->c.flags, material))
			continue;

		// already a real light
		if (material->flags & MATERIAL_FLAG_LIGHT)
			continue;

		float positions[3 * MAX_FACE_VERTS];
		int num_vertices = min(surf->numsurfedges, MAX_FACE_VERTS);

		for (int k = 0; k < num_vertices; k++)
		{
			msurfedge_t *src_surfedge = surf->firstsurfedge + k;
			VectorCopy(src_surfedge->edge->v[src_surfedge->vert]->point, positions + k * 3);
		}

		remove_collinear_edges(positions, NULL, NULL, &num_vertices);

		for (int k = 0; k < num_vertices - 2; k++)
		{
			int i1 = (k + 2) % num_vertices;
			int i2 = (k + 1) % num_vertices;

			light_poly_t light;
			VectorCopy(positions, light.positions + 0);
			VectorCopy(positions + i1 * 3, light.positions + 3);
			VectorCopy(positions + i2 * 3, light.positions + 6);

			if (!get_triangle_off_center(light.positions, light.off_center, NULL, 1.f))
				continue;

			mleaf_t *leaf = BSP_PointLeaf(bsp->nodes, light.off_center);
			if (leaf->cluster < 0 || (leaf->contents & (CONTENTS_SOLID | CONTENTS_LAVA)))
				continue;

			// a lava light from the map's cluster file already covers this face
			if (is_sky_or_lava_cluster(wm, surf, leaf->cluster, surf->texinfo->material->flags))
				continue;

			if (wm->num_fog_light_polys >= MAX_FOG_LIGHT_POLYS)
			{
				truncated = true;
				break;
			}

			VectorCopy(material->image_emissive->light_color, light.color);
			light.material = material;
			light.style = 0;
			light.type = DYNLIGHT_POLYGON;
			light.volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
			set_light_q3_glow(&light, false, NULL, NULL, NULL);
			light.cluster = leaf->cluster;

			light_poly_t *dst = append_light_poly(&wm->num_fog_light_polys, &wm->allocated_fog_light_polys, &wm->fog_light_polys);
			memcpy(dst, &light, sizeof(light_poly_t));
		}
	}

	if (truncated)
		Com_WPrintf("More than %d lava triangles; the rest make no fog. Raise MAX_FOG_LIGHT_POLYS.\n", MAX_FOG_LIGHT_POLYS);

	if (wm->num_fog_light_polys == 0)
		return;

	build_cluster_light_lists(wm, bsp, wm->fog_light_polys, wm->num_fog_light_polys,
	                          MAX_LIGHT_POLYS - wm->num_fog_light_polys, MAX_FOG_LIGHT_LIST_NODES,
	                          &wm->num_fog_cluster_lights, &wm->fog_cluster_light_offsets, &wm->fog_cluster_lights);
}

static tinyobj_attrib_t custom_sky_attrib;

static uint32_t
bsp_mesh_load_custom_sky(const char* map_name)
{
	char filename[MAX_QPATH];
	Q_snprintf(filename, sizeof(filename), "maps/sky/%s.obj", map_name);

	void* file_buffer = NULL;
	int file_size = FS_LoadFile(filename, &file_buffer);
	if (!file_buffer)
		return 0;

	tinyobj_shape_t* shapes = NULL;
	size_t num_shapes;
	tinyobj_material_t* materials = NULL;
	size_t num_materials;

	unsigned int flags = TINYOBJ_FLAG_TRIANGULATE;
	int ret = tinyobj_parse_obj(&custom_sky_attrib, &shapes, &num_shapes, &materials,
		&num_materials, (const char*)file_buffer, file_size, flags);

	FS_FreeFile(file_buffer);

	if (ret != TINYOBJ_SUCCESS) {
		Com_WPrintf("Couldn't parse sky polygon definition file %s.\n", filename);
		return 0;
	}

	tinyobj_shapes_free(shapes, num_shapes);
	tinyobj_materials_free(materials, num_materials);

	if (custom_sky_attrib.num_face_num_verts == 0)
		tinyobj_attrib_free(&custom_sky_attrib);
		
	return custom_sky_attrib.num_face_num_verts;
}

static uint32_t
bsp_mesh_create_custom_sky_prims(uint32_t* prim_ctr, bsp_mesh_t* wm, const bsp_t* bsp)
{
	int face_offset = 0;
	for (uint32_t nprim = 0; nprim < custom_sky_attrib.num_face_num_verts; nprim++)
	{
		int face_num_verts = custom_sky_attrib.face_num_verts[nprim];
		int i0 = custom_sky_attrib.faces[face_offset + 0].v_idx;
		int i1 = custom_sky_attrib.faces[face_offset + 1].v_idx;
		int i2 = custom_sky_attrib.faces[face_offset + 2].v_idx;

		float positions[9];
		VectorCopy(custom_sky_attrib.vertices + i0 * 3, positions + 0);
		VectorCopy(custom_sky_attrib.vertices + i1 * 3, positions + 3);
		VectorCopy(custom_sky_attrib.vertices + i2 * 3, positions + 6);

		if (*prim_ctr >= wm->num_primitives_allocated)
		{
			assert(!"Primitive buffer overflow.");
			return nprim;
		}
		
		VboPrimitive* prim = wm->primitives + *prim_ctr;

		memset(prim, 0, sizeof(*prim));
		VectorCopy(positions + 0, prim->pos0);
		VectorCopy(positions + 3, prim->pos1);
		VectorCopy(positions + 6, prim->pos2);
		
		vec3_t center;
		get_triangle_off_center(positions, center, NULL, 1.f);

		int cluster = BSP_PointLeaf(bsp->nodes, center)->cluster;
		prim->cluster = cluster;
		prim->material_id = MATERIAL_FLAG_LIGHT | MATERIAL_KIND_SKY;

		light_poly_t* light = append_light_poly(&wm->num_light_polys, &wm->allocated_light_polys, &wm->light_polys);

		memcpy(light->positions, positions, sizeof(prim_positions_t));
		VectorSet(light->color, -1.f, -1.f, -1.f); // special value for the sky
		VectorCopy(center, light->off_center);
		light->material = 0;
		light->style = 0;
		light->cluster = cluster;
		light->type = DYNLIGHT_POLYGON;
		light->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;
		set_light_q3_glow(light, false, NULL, NULL, NULL);

		++*prim_ctr;

		face_offset += face_num_verts;
	}

	tinyobj_attrib_free(&custom_sky_attrib);

	return custom_sky_attrib.num_face_num_verts;
}

void
bsp_mesh_create_from_bsp(bsp_mesh_t *wm, bsp_t *bsp, const char* map_name)
{
	const char* full_game_map_name = map_name;
	if (strcmp(map_name, "demo1") == 0)
		full_game_map_name = "base1";
	else if (strcmp(map_name, "demo2") == 0)
		full_game_map_name = "base2";
	else if (strcmp(map_name, "demo3") == 0)
		full_game_map_name = "base3";

	load_sky_and_lava_clusters(wm, full_game_map_name);
	vkpt_cameras_load(wm, full_game_map_name);
	vkpt_cameras_load_q3(wm, bsp);	// a Quake III map's portals
	vkpt_cameras_load_q1(wm, bsp);	// a Quake 1 map's teleporters, seen through
	wm->q3 = bsp->q3;
	wm->q1 = bsp->q1;

	wm->models = Z_Malloc(bsp->nummodels * sizeof(bsp_model_t));
	memset(wm->models, 0, bsp->nummodels * sizeof(bsp_model_t));

    wm->num_models = bsp->nummodels;
	wm->num_clusters = bsp->vis->numclusters;

	if (wm->num_clusters + 1 >= MAX_LIGHT_LISTS)
	{
		Com_Error(ERR_FATAL, "The BSP model has too many clusters (%d)", wm->num_clusters);
	}
	
	wm->num_primitives_allocated = count_triangles(bsp);

	uint32_t num_custom_sky_prims = bsp_mesh_load_custom_sky(full_game_map_name);
	if (num_custom_sky_prims > 0)
		wm->num_primitives_allocated += num_custom_sky_prims;

	wm->primitives = Z_Malloc(wm->num_primitives_allocated * sizeof(VboPrimitive));
	wm->num_primitives = 0;

	/* Group the BSP faces before any of them are turned into triangles: the
	   grouping decides which material each face draws with, so it has to exist
	   by the time collect_surfaces reads it. */
	wm->prim_instance_hash = Z_Mallocz(wm->num_primitives_allocated * sizeof(uint32_t));
	s_face_instance_hash = build_face_instance_hashes(bsp);

	// Read by get_surf_light_style, for the faces and the light polys alike.
	collect_styled_lights(bsp);

	// clear these here because `bsp_mesh_load_custom_sky` creates lights before `collect_light_polys`
	wm->num_light_polys = 0;
	wm->allocated_light_polys = 0;
	wm->light_polys = NULL;

    uint32_t prim_ctr = 0;

#if DUMP_WORLD_MESH_TO_OBJ
	{
		char filename[MAX_QPATH];
		Q_snprintf(filename, sizeof(filename), "C:\\temp\\%s.obj", map_name);
		obj_dump_file = fopen(filename, "w");
		obj_vertex_num = 1;
	}
#endif

	vkpt_init_model_geometry(&wm->geom_opaque, 1);
	vkpt_init_model_geometry(&wm->geom_transparent, 1);
	vkpt_init_model_geometry(&wm->geom_masked, 1);
	vkpt_init_model_geometry(&wm->geom_sky, 1);
	vkpt_init_model_geometry(&wm->geom_custom_sky, 1);

	// the faces that move go to q3deform.c instead (collect_surfaces)
	vkpt_q3_deform_begin(bsp);
	Z_Free(s_deform_faces);
	s_deform_faces = bsp->numfaces > 0 ? Z_Mallocz(bsp->numfaces) : NULL;

	uint32_t first_prim = prim_ctr;
	collect_surfaces(&prim_ctr, wm, bsp, -1, filter_static_opaque);
	vkpt_append_model_geometry(&wm->geom_opaque, prim_ctr - first_prim, first_prim, "bsp");

	first_prim = prim_ctr;
	collect_surfaces(&prim_ctr, wm, bsp, -1, filter_static_transparent);
	vkpt_append_model_geometry(&wm->geom_transparent, prim_ctr - first_prim, first_prim, "bsp");

	// masked glass among them (filter_static_masked): the masked hit for all of it
	wm->transparent_masked = false;
	for (uint32_t prim = first_prim; prim < prim_ctr && !wm->transparent_masked; prim++)
	{
		const uint32_t material = wm->primitives[prim].material_id;
		const pbr_material_t* mat = MAT_ForIndex((int)(material & MATERIAL_INDEX_MASK));
		if (mat && mat->image_mask && MAT_IsKind(material, MATERIAL_KIND_GLASS))
			wm->transparent_masked = true;
	}

	first_prim = prim_ctr;
	collect_surfaces(&prim_ctr, wm, bsp, -1, filter_static_masked);
	vkpt_append_model_geometry(&wm->geom_masked, prim_ctr - first_prim, first_prim, "bsp");

	first_prim = prim_ctr;
	collect_surfaces(&prim_ctr, wm, bsp, -1, filter_static_sky);
	vkpt_append_model_geometry(&wm->geom_sky, prim_ctr - first_prim, first_prim, "bsp");
	
	first_prim = prim_ctr;
	if (num_custom_sky_prims > 0)
		bsp_mesh_create_custom_sky_prims(&prim_ctr, wm, bsp);
	if (cvar_pt_bsp_sky_lights->integer > 1)
		collect_surfaces(&prim_ctr, wm, bsp, -1, filter_nodraw_sky_lights);
	vkpt_append_model_geometry(&wm->geom_custom_sky, prim_ctr - first_prim, first_prim, "bsp");

    for (int k = 0; k < bsp->nummodels; k++) {
		bsp_model_t* model = wm->models + k;
		first_prim = prim_ctr;
		collect_surfaces(&prim_ctr, wm, bsp, k, filter_all);
		vkpt_init_model_geometry(&model->geometry, 1);
		vkpt_append_model_geometry(&model->geometry, prim_ctr - first_prim, first_prim, "bsp_model");
    }

#if DUMP_WORLD_MESH_TO_OBJ
	fclose(obj_dump_file);
	obj_dump_file = NULL;
#endif


	if (!bsp->pvs_patched)
	{
		build_pvs2(bsp);

		if (!BSP_SavePatchedPVS(bsp))
		{
			Com_EPrintf("Couldn't save patched PVS for %s.\n", bsp->name);
		}
	}

	wm->num_primitives = prim_ctr;

	vkpt_q3_deform_build();

	compute_world_tangents(bsp, wm);
	
	for(int i = 0; i < wm->num_models; i++) 
	{
		bsp_model_t* model = wm->models + i;

		compute_aabb(wm->primitives + model->geometry.prim_offsets[0], model->geometry.prim_counts[0], model->aabb_min, model->aabb_max);

		VectorAdd(model->aabb_min, model->aabb_max, model->center);
		VectorScale(model->center, 0.5f, model->center);
	}

	compute_aabb(wm->primitives + wm->geom_opaque.prim_offsets[0], wm->geom_opaque.prim_counts[0], wm->world_aabb.mins, wm->world_aabb.maxs);
	append_aabb(wm->primitives + wm->geom_transparent.prim_offsets[0], wm->geom_transparent.prim_counts[0], wm->world_aabb.mins, wm->world_aabb.maxs);
	append_aabb(wm->primitives + wm->geom_masked.prim_offsets[0], wm->geom_masked.prim_counts[0], wm->world_aabb.mins, wm->world_aabb.maxs);

	vec3_t margin = { 1.f, 1.f, 1.f };
	VectorSubtract(wm->world_aabb.mins, margin, wm->world_aabb.mins);
	VectorAdd(wm->world_aabb.maxs, margin, wm->world_aabb.maxs);

	compute_cluster_aabbs(wm);

	collect_light_polys(wm, bsp, -1, &wm->num_light_polys, &wm->allocated_light_polys, &wm->light_polys);
	collect_sky_and_lava_light_polys(wm, bsp);

	for (int k = 0; k < bsp->nummodels; k++)
	{
		bsp_model_t* model = wm->models + k;

		model->num_light_polys = 0;
		model->allocated_light_polys = 0;
		model->light_polys = NULL;
		
		collect_light_polys(wm, bsp, k, &model->num_light_polys, &model->allocated_light_polys, &model->light_polys);

		model->transparent = is_model_transparent(wm, model);
		model->masked = is_model_masked(wm, model);
	}

	collect_cluster_lights(wm, bsp);
	collect_fog_lava_lights(wm, bsp);

	compute_sky_visibility(wm, bsp);

	/* Last consumer has run. The per-face table only exists for the duration of
	   the build - the per-primitive copy on wm is what outlives it. Freed here
	   rather than right after the geometry passes because the LIGHT passes above
	   need it too, and freeing it early is what left instanced fixtures emitting
	   their base material's colour. */
	if (s_face_instance_hash)
	{
		Z_Free(s_face_instance_hash);
		s_face_instance_hash = NULL;
	}
	Z_Free(s_deform_faces);
	s_deform_faces = NULL;

	free_styled_lights();
}

void
bsp_mesh_destroy(bsp_mesh_t *wm)
{
	Z_Free(wm->models);

	Z_Free(wm->primitives);
	Z_Free(wm->prim_instance_hash);

	Z_Free(wm->light_polys);
	Z_Free(wm->cluster_lights);
	Z_Free(wm->cluster_light_offsets);
	Z_Free(wm->fog_light_polys);
	Z_Free(wm->fog_cluster_lights);
	Z_Free(wm->fog_cluster_light_offsets);
	Z_Free(wm->cluster_aabbs);

	memset(wm, 0, sizeof(*wm));
}

void
bsp_mesh_register_textures(bsp_t *bsp)
{
	MAT_ChangeMap(bsp->name);
	
	for (int i = 0; i < bsp->numtexinfo; i++) {
		mtexinfo_t *info = bsp->texinfo + i;
		imageflags_t flags;
		if (info->c.flags & SURF_WARP)
			flags = IF_TURBULENT;
		else
			flags = IF_NONE;

		char buffer[MAX_QPATH];
		Q_concat(buffer, sizeof(buffer), "textures/", info->name, ".wal");
		FS_NormalizePath(buffer);

		pbr_material_t * mat = MAT_Find(buffer, IT_WALL, flags);
		if (!mat)
			Com_EPrintf("error finding material '%s'\n", buffer);
		
		if(cvar_pt_enable_surface_lights->integer)
		{
			/* Synthesize an emissive material if the BSP surface has the LIGHT flag but the
			   material has no emissive image.
			   - Skip SKY and NODRAW surfaces, they'll be handled differently.
			   - Make WARP surfaces optional, as giving water, slime... an emissive texture clashes visually. */
			bool synth_surface_material = ((info->c.flags & (SURF_LIGHT | SURF_SKY |  SURF_NODRAW)) == SURF_LIGHT)
				&& (info->radiance != 0);
			
			bool is_warp_surface = (info->c.flags & SURF_WARP) != 0;
			
			bool material_custom = !mat->source_matfile[0];
			
			synth_surface_material &= (cvar_pt_enable_surface_lights->integer >= 2) || material_custom;
			/* A Quake III shader's light is settled in material.c (apply_q3_shader):
			   its glow or a synthesized one, at the default_radiance it shows in
			   'mat print'. This rule's default_radiance 0 would switch them off. */
			synth_surface_material &= !bsp->q3;
			if (cvar_pt_enable_surface_lights_warp->integer == 0)
				synth_surface_material &= !is_warp_surface;
			
			if (synth_surface_material)
			{
				MAT_SynthesizeEmissive(mat);
				/* If emissive is "fake", treat absence of BSP radiance flag as "not emissive":
				* The assumption is that this is closer to the author's intention */
				mat->default_radiance = 0.0f;
			}
		}
		
		info->material = mat;
	}

	// link the animation sequences
	for (int i = 0; i < bsp->numtexinfo; i++) 
	{
		mtexinfo_t *texinfo = bsp->texinfo + i;
		pbr_material_t* material = texinfo->material;

		if (texinfo->numframes > 1)
		{
			assert(texinfo->next);
			assert(texinfo->next->material);

			material->num_frames = texinfo->numframes;
			material->next_frame = texinfo->next->material->flags & MATERIAL_INDEX_MASK;
		}
	}
}

static void animate_light_polys(int num_light_polys, light_poly_t *light_polys)
{
	for (int i = 0; i < num_light_polys; i++)
	{
		pbr_material_t *material = light_polys[i].material;
		if(!material || (material->num_frames <= 1))
			continue;
		// stained glass's colour is not its emissive image's (collect_stained_face)
		if (light_polys[i].type == DYNLIGHT_WINDOW || material->stained_glass)
			continue;

		pbr_material_t *new_material = r_materials + material->next_frame;
		light_polys[i].material = new_material;
		float emissive_factor = new_material->emissive_factor * light_polys[i].emissive_factor;
		if(new_material->image_emissive)
			VectorScale(new_material->image_emissive->light_color, emissive_factor, light_polys[i].color);
		else
			VectorSet(light_polys[i].color, 0, 0, 0);
	}
}

void bsp_mesh_animate_light_polys(bsp_mesh_t *wm)
{
	animate_light_polys(wm->num_light_polys, wm->light_polys);
	for (int k = 0; k < wm->num_models; k++)
	{
		bsp_model_t* model = wm->models + k;
		animate_light_polys(model->num_light_polys, model->light_polys);
	}
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
