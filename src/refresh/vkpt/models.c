/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2018 Florian Simon
Copyright (C) 2003-2006 Andrey Nazarov
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

/*
Copyright (C) 2003-2006 Andrey Nazarov

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

#include "format/md2.h"
#include "format/md3.h"
#include "format/sp2.h"
#include "material.h"
#include "format/iqm.h"
#include <assert.h>

#if MAX_ALIAS_VERTS > TESS_MAX_VERTICES
#error TESS_MAX_VERTICES
#endif

#if MD2_MAX_TRIANGLES > TESS_MAX_INDICES / 3
#error TESS_MAX_INDICES
#endif

static void extract_model_lights(model_t* model)
{
	// Count the triangles in the model that have a material with the is_light flag set
	
	int num_lights = 0;
	
	for (int mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++)
	{
		const maliasmesh_t* mesh = model->meshes + mesh_idx;
		for (int skin_idx = 0; skin_idx < mesh->numskins; skin_idx++)
		{
			const pbr_material_t* mat = mesh->materials[skin_idx];
			if ((mat->flags & MATERIAL_FLAG_LIGHT) != 0 && mat->image_emissive)
			{
				if (mesh->numskins != 1)
				{
					Com_DPrintf("Warning: model %s mesh %d has LIGHT material(s) but more than 1 skin (%d), "
						"which is unsupported.\n", model->name, mesh_idx, mesh->numskins);
					return;
				}

				num_lights += mesh->numtris;
			}
		}
	}

	// If there are no light triangles, there's nothing to do
	if (num_lights == 0)
		return;

	// Validate our current implementation limitations, give warnings if they are hit
	
	if (model->numframes > 1)
	{
		Com_DPrintf("Warning: model %s has LIGHT material(s) but more than 1 vertex animation frame, "
			"which is unsupported.\n", model->name);
		return;
	}

	if (model->iqmData && model->iqmData->blend_weights)
	{
		Com_DPrintf("Warning: model %s has LIGHT material(s) and skeletal animations, "
			"which is unsupported.\n", model->name);
		return;
	}

	// Actually extract the lights now
	
	if (!(model->light_polys = Hunk_Alloc(&model->hunk, sizeof(light_poly_t) * num_lights))) {
		Com_DPrintf("Warning: unable to allocate memory for %i light polygons.\n", num_lights);
		return;
	}
	model->num_light_polys = num_lights;

	num_lights = 0;

	for (int mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++)
	{
		const maliasmesh_t* mesh = model->meshes + mesh_idx;
		assert(mesh->numskins == 1);
		assert(mesh->indices);
		assert(mesh->positions);
		
		pbr_material_t* mat = mesh->materials[0];
		if ((mat->flags & MATERIAL_FLAG_LIGHT) != 0 && mat->image_emissive)
		{
			for (int tri_idx = 0; tri_idx < mesh->numtris; tri_idx++)
			{
				light_poly_t* light = model->light_polys + num_lights;
				num_lights++;

				int i0 = mesh->indices[tri_idx * 3 + 0];
				int i1 = mesh->indices[tri_idx * 3 + 1];
				int i2 = mesh->indices[tri_idx * 3 + 2];
				
				assert(i0 < mesh->numverts);
				assert(i1 < mesh->numverts);
				assert(i2 < mesh->numverts);

				memcpy(light->positions + 0, mesh->positions + i0, sizeof(vec3_t));
				memcpy(light->positions + 3, mesh->positions + i1, sizeof(vec3_t));
				memcpy(light->positions + 6, mesh->positions + i2, sizeof(vec3_t));
				
				// Cluster is assigned after model instancing and transformation
				light->cluster = -1;

				light->material = mat;
				// this array is allocated zeroed, and 0 is a legitimate "no fog from
				// this light" - so the unset sentinel has to be written, not assumed
				light->volumetric_scale = LIGHT_VOLUMETRIC_SCALE_UNSET;

				VectorCopy(mat->image_emissive->light_color, light->color);

				if (!mat->image_emissive->entire_texture_emissive)
				{
					// This extraction doesn't support partially emissive textures, so pretend the entire
					// texture is uniformly emissive and dim the light according to the area fraction.
					light->emissive_factor =
						(mat->image_emissive->max_light_texcoord[0] - mat->image_emissive->min_light_texcoord[0]) *
						(mat->image_emissive->max_light_texcoord[1] - mat->image_emissive->min_light_texcoord[1]);
				}
				else
					light->emissive_factor = 1.f;
				
				get_triangle_off_center(light->positions, light->off_center, NULL, 1.f);
				
			}
		}
	}
}

/*
PHONG SHADING FOR ALIAS MODELS - RECONSTRUCTING THE VERTEX NORMALS.

The path tracer already interpolates vertex normals barycentrically at every
hit (primary_rays.rgen, indirect_lighting.rgen and reflect_refract.rgen all do
normalize(triangle.normals * bary)), so smooth shading is not something the
renderer lacks - it is something the model files do not always feed it.

.MD2 is the reason this exists.  It does not store a normal; it stores ONE
BYTE per vertex per frame, an index into the 162-entry `bytedirs` table (see
NUMVERTEXNORMALS in inc/common/math.h).  162 directions over a sphere is an
average spacing of about 16 degrees, so every vertex across a gently curved
surface - a head, a shoulder, a barrel - snaps to the SAME table entry as its
neighbours.  The interpolation then has nothing to interpolate: the normal is
constant over a patch of triangles and steps 16 degrees at the patch border.
That is the faceted look, and it is why turning this off restores it exactly.

ONLY .MD2 COMES THROUGH HERE (2026-10-26).  It used to run on .md3, .md5 and
.iqm as well, and did harm on every one of them:
  - .md3 stores an authored normal to about 1.4 degrees, but rebuilds it here
    from positions rounded to 1/64 unit, separately for every frame.  On the
    first-person rocket launcher (tris.md3, 3900 verts) the rebuilt normals
    jumped up to 63 degrees between consecutive idle frames where the file's
    move at most 5.6, 71% of the rocket vertices by more than 10 - the glossy
    rockets visibly throbbed as the gun animated.  Every weapon .md3 also
    landed 20-35 degrees away from its authored normals on average (the weld
    caveat below).
  - .md5 normals are already smooth: model_md5.c accumulates them across
    every face and welds the UV-seam copies (MD5_WeldSeamNormals), which is
    the remaster's look.  This pass re-cut creases into them - the line over
    the top of the soldier's helmet.
  - .iqm carries its own authored normals, the same case as .md3.
What still looked faceted on .md5 models with this off was never their
normals: it was shadow rays leaving the flat triangles, see
terminator_offset_position in path_tracer_rgen.h.

Known caveat for .md2: the crease test below is made per CORNER, but the sum
is per WELDED position, so the copies on the two sides of a hard edge share
one sum and edges up to about 120 degrees are averaged anyway.  Left as it
is, because fixing it changes how every .md2 looks.

So this pass does not add an interpolation scheme; it reconstructs the normal
each vertex was rounded from, as the usual angle-weighted average of the
adjacent face normals.  Three details make it safe on 1997 content:

  - VERTICES ARE WELDED BY POSITION, not by index.  Every one of these loaders
    splits a vertex where two triangles give it different texture coordinates,
    so a UV seam running over the top of a head arrives here as two coincident
    vertices.  Averaging per index would leave a lit seam along every one of
    them.  The split copies are bit-identical in position, so an exact-bit hash
    welds them and nothing else.  (model_md5.c already does this for its own
    derived normals - see MD5_WeldSeamNormals - which is where the problem was
    first found, on the rerelease soldier's helmet.)

  - THE FACE NORMAL'S SIGN IS DERIVED, NOT ASSUMED.  The loaders do not all
    reverse their index order the same way, so cross(e0, e1) does not point
    the same way for all of them.  Taking the sign from the authored normals
    of the corners makes this independent of the loader.

  - THE AUTHORED NORMAL IS A SMOOTHING GROUP HINT, and it is respected.  A face
    whose normal is more than pt_model_smooth_angle away from the normal stored
    at that corner belongs to a different surface and is left out of the
    average, and if the reconstruction ends up outside that cone anyway the
    authored normal is kept.  Hard edges - a gun's barrel meeting its receiver,
    the corners of a crate - survive.  This also gives the right answer for the
    models whose normals are all the same byte (the players/w_*.md2 held
    weapons), where MOD_LoadMD2_RTX substitutes flat face normals: the hint is
    then the face normal itself and the cone test becomes the classic crease
    angle.

The work is per frame because .md2 quantises each frame's positions
independently, so a pose's normals have to come from that pose's geometry.
*/
static inline uint32_t hash_vertex_position(const vec3_t p)
{
	uint32_t bits[3];
	memcpy(bits, p, sizeof(bits));

	// Zero and negative zero compare equal as floats but not as bits, and a
	// .md2's frame translation happily produces both.
	for (int i = 0; i < 3; i++)
		if (bits[i] == 0x80000000u)
			bits[i] = 0;

	return bits[0] * 73856093u ^ bits[1] * 19349663u ^ bits[2] * 83492791u;
}

static void smooth_model_normals(model_t* model)
{
	// Read here rather than cached, because the normals are baked into the
	// vertex buffer and so a change can only be applied by reloading the
	// models.  CVAR_FILES is what makes that happen by itself - it is the
	// same flag cl_md5_models uses to swap the .md2s for .md5s live.
	if (!Cvar_Get("pt_model_smooth_normals", "1", CVAR_ARCHIVE | CVAR_FILES)->integer)
		return;

	const float crease_degrees = Cvar_Get("pt_model_smooth_angle", "60", CVAR_ARCHIVE | CVAR_FILES)->value;
	const float min_cos = cosf((float)DEG2RAD(max(1.f, min(179.f, crease_degrees))));

	for (int mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++)
	{
		maliasmesh_t* mesh = model->meshes + mesh_idx;

		if (!mesh->positions || !mesh->normals || !mesh->indices)
			continue;
		if (mesh->numverts <= 0 || mesh->numtris <= 0)
			continue;

		size_t table_size = 16;
		while (table_size < (size_t)mesh->numverts * 2)
			table_size *= 2;

		int* table = malloc(table_size * sizeof(int));
		int* chain = malloc(mesh->numverts * sizeof(int));
		int* weld  = malloc(mesh->numverts * sizeof(int));
		vec3_t* accum = malloc(mesh->numverts * sizeof(vec3_t));

		if (!table || !chain || !weld || !accum)
		{
			free(table); free(chain); free(weld); free(accum);
			return;
		}

		for (int frame = 0; frame < model->numframes; frame++)
		{
			vec3_t* positions = mesh->positions + (size_t)frame * mesh->numverts;
			vec3_t* normals   = mesh->normals   + (size_t)frame * mesh->numverts;

			for (size_t i = 0; i < table_size; i++)
				table[i] = -1;

			memset(accum, 0, mesh->numverts * sizeof(vec3_t));

			// Weld the UV-seam duplicates back together for shading purposes only.
			for (int v = 0; v < mesh->numverts; v++)
			{
				uint32_t bucket = hash_vertex_position(positions[v]) & (uint32_t)(table_size - 1);
				int rep = v;

				for (int j = table[bucket]; j >= 0; j = chain[j])
				{
					if (positions[j][0] == positions[v][0] &&
						positions[j][1] == positions[v][1] &&
						positions[j][2] == positions[v][2])
					{
						rep = weld[j];
						break;
					}
				}

				chain[v] = table[bucket];
				table[bucket] = v;
				weld[v] = rep;
			}

			for (int tri = 0; tri < mesh->numtris; tri++)
			{
				const int idx[3] = {
					mesh->indices[tri * 3 + 0],
					mesh->indices[tri * 3 + 1],
					mesh->indices[tri * 3 + 2] };

				if (idx[0] < 0 || idx[0] >= mesh->numverts ||
					idx[1] < 0 || idx[1] >= mesh->numverts ||
					idx[2] < 0 || idx[2] >= mesh->numverts)
					continue;

				vec3_t e0, e1, face;
				VectorSubtract(positions[idx[1]], positions[idx[0]], e0);
				VectorSubtract(positions[idx[2]], positions[idx[0]], e1);
				CrossProduct(e0, e1, face);

				if (VectorNormalize(face) == 0.f)
					continue; // degenerate, as the tangent pass also skips

				float authored = DotProduct(face, normals[idx[0]])
				               + DotProduct(face, normals[idx[1]])
				               + DotProduct(face, normals[idx[2]]);

				if (authored < 0.f)
					VectorInverse(face);

				for (int c = 0; c < 3; c++)
				{
					int iv = idx[c];

					// Outside this corner's smoothing group - a hard edge.
					if (DotProduct(face, normals[iv]) < min_cos)
						continue;

					vec3_t a, b;
					VectorSubtract(positions[idx[(c + 1) % 3]], positions[iv], a);
					VectorSubtract(positions[idx[(c + 2) % 3]], positions[iv], b);

					if (VectorNormalize(a) == 0.f || VectorNormalize(b) == 0.f)
						continue;

					// Weight by the corner angle, so a fan of slivers does not
					// outvote the one large triangle that shares the vertex.
					float angle = acosf(max(-1.f, min(1.f, (float)DotProduct(a, b))));

					VectorMA(accum[weld[iv]], angle, face, accum[weld[iv]]);
				}
			}

			for (int v = 0; v < mesh->numverts; v++)
			{
				vec3_t n;
				VectorCopy(accum[weld[v]], n);

				if (VectorNormalize(n) == 0.f)
					continue; // nothing accumulated - keep what the file said

				if (DotProduct(n, normals[v]) < min_cos)
					continue; // the reconstruction disagrees with the file; trust the file

				VectorCopy(n, normals[v]);
			}
		}

		free(table);
		free(chain);
		free(weld);
		free(accum);
	}
}

static void compute_missing_model_tangents(model_t* model)
{
	for (int mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++)
	{
		maliasmesh_t* mesh = model->meshes + mesh_idx;

		if (mesh->tangents)
			continue;

		size_t tangent_size = mesh->numverts * model->numframes * sizeof(vec3_t);

		mesh->tangents = MOD_Malloc(tangent_size);

		memset(mesh->tangents, 0, tangent_size);

		int handedness = 0;

		for (int frame = 0; frame < model->numframes; frame++)
		{
			int voffset = frame * mesh->numverts;

			for (int tri = 0; tri < mesh->numtris; tri++)
			{
				int iA = mesh->indices[tri * 3 + 0] + voffset;
				int iB = mesh->indices[tri * 3 + 1] + voffset;
				int iC = mesh->indices[tri * 3 + 2] + voffset;

				const vec3_t* pA = mesh->positions + iA;
				const vec3_t* pB = mesh->positions + iB;
				const vec3_t* pC = mesh->positions + iC;

				const vec2_t* tA = mesh->tex_coords + iA;
				const vec2_t* tB = mesh->tex_coords + iB;
				const vec2_t* tC = mesh->tex_coords + iC;

				vec3_t dP0, dP1;
				VectorSubtract(*pB, *pA, dP0);
				VectorSubtract(*pC, *pA, dP1);

				vec2_t dt0, dt1;
				Vector2Subtract(*tB, *tA, dt0);
				Vector2Subtract(*tC, *tA, dt1);

				float inv_r = dt0[0] * dt1[1] - dt1[0] * dt0[1];

				if (inv_r == 0.f)
					continue;

				float r = 1.f / inv_r;

				vec3_t tangent = {
					(dt1[1] * dP0[0] - dt0[1] * dP1[0]) * r,
					(dt1[1] * dP0[1] - dt0[1] * dP1[1]) * r,
					(dt1[1] * dP0[2] - dt0[1] * dP1[2]) * r };

				VectorNormalize(tangent);

				vec3_t* tangentA = mesh->tangents + iA;
				vec3_t* tangentB = mesh->tangents + iB;
				vec3_t* tangentC = mesh->tangents + iC;

				VectorAdd(*tangentA, tangent, *tangentA);
				VectorAdd(*tangentB, tangent, *tangentB);
				VectorAdd(*tangentC, tangent, *tangentC);

				if (handedness == 0)
				{
					vec3_t bitangent = {
						(dt0[0] * dP1[0] - dt1[0] * dP0[0]) * r,
						(dt0[0] * dP1[1] - dt1[0] * dP0[1]) * r,
						(dt0[0] * dP1[2] - dt1[0] * dP0[2]) * r };

					VectorNormalize(bitangent);

					const vec3_t* normal = mesh->normals + iA;

					vec3_t cross;
					CrossProduct(*normal, tangent, cross);

					float dot = DotProduct(cross, bitangent);

					if (dot < 0.f)
						handedness = -1;
					else if (dot > 0.f)
						handedness = 1;
				}
			}
		}

		for (int vtx = 0; vtx < mesh->numverts * model->numframes; vtx++)
		{
			vec3_t* tangent = mesh->tangents + vtx;

			VectorNormalize(*tangent);
		}

		// Kept for the material flag in the vertex buffer, but the path tracer
		// no longer reads it: one bool cannot describe a mesh with mirrored UV
		// islands, so get_material() in path_tracer_rgen.h now derives the
		// bitangent sign per triangle from the UV winding.
		mesh->handedness = (handedness < 0);
	}
}

int MOD_LoadMD2_RTX(model_t *model, const void *rawdata, size_t length, const char* mod_name)
{
	dmd2header_t    header;
	dmd2frame_t     *src_frame;
	dmd2trivertx_t  *src_vert;
	dmd2triangle_t  *src_tri;
	dmd2stvert_t    *src_tc;
	char            *src_skin;
	maliasframe_t   *dst_frame;
	maliasmesh_t    *dst_mesh;
	int             val;
	uint16_t*        remap = (uint16_t*)malloc(TESS_MAX_INDICES * sizeof(uint16_t)); 
	uint16_t*       vertIndices = (uint16_t*)malloc(TESS_MAX_INDICES * sizeof(uint16_t));
	uint16_t*        tcIndices = (uint16_t*)malloc(TESS_MAX_INDICES * sizeof(uint16_t));
	uint16_t*        finalIndices = (uint16_t*)malloc(TESS_MAX_INDICES * sizeof(uint16_t));
	int             numverts, numindices;
	char            skinname[MAX_QPATH];
	vec_t           scale_s, scale_t;
	vec3_t          mins, maxs;
	int             ret;

	if (length < sizeof(header)) {
		return Q_ERR_FILE_TOO_SMALL;
	}

	// byte swap the header
	LittleBlock(&header, rawdata, sizeof(header));

	// validate the header
	ret = MOD_ValidateMD2(&header, length);
	if (ret) {
		if (ret == Q_ERR_TOO_FEW) {
			// empty models draw nothing
			model->type = MOD_EMPTY;
			return Q_ERR_SUCCESS;
		}
		return ret;
	}

	// load all triangle indices
	numindices = 0;
	src_tri = (dmd2triangle_t *)((byte *)rawdata + header.ofs_tris);
	for (int i = 0; i < header.num_tris; i++) {
		int good = 1;
		for (int j = 0; j < 3; j++) {
			uint16_t idx_xyz = LittleShort(src_tri->index_xyz[j]);
			uint16_t idx_st = LittleShort(src_tri->index_st[j]);

			// some broken models have 0xFFFF indices
			if (idx_xyz >= header.num_xyz || idx_st >= header.num_st) {
				good = 0;
				break;
			}

			vertIndices[numindices + j] = idx_xyz;
			tcIndices[numindices + j] = idx_st;
		}
		if (good) {
			// only count good triangles
			numindices += 3;
		}
		src_tri++;
	}

	if (numindices < 3) {
		return Q_ERR_TOO_FEW;
	}

	bool all_normals_same = true;
	int same_normal = -1;

	src_frame = (dmd2frame_t *)((byte *)rawdata + header.ofs_frames);
	for (int i = 0; i < numindices; i++)
	{
		int v = vertIndices[i];
		int normal = src_frame->verts[v].lightnormalindex;

		// detect if the model has broken normals - they are all the same in that case
		// it happens with players/w_<weapon>.md2 models for example
		if (same_normal < 0)
			same_normal = normal;
		else if (normal != same_normal)
			all_normals_same = false;
	}

	for (int i = 0; i < numindices; i++) {
		remap[i] = 0xFFFF;
	}

	// remap all triangle indices
	numverts = 0;
	src_tc = (dmd2stvert_t *)((byte *)rawdata + header.ofs_st);
	for (int i = 0; i < numindices; i++) {
		if (remap[i] != 0xFFFF) {
			continue; // already remapped
		}

		// only dedup vertices if we're not regenerating normals
		if (!all_normals_same)
		{
			for (int j = i + 1; j < numindices; j++) {
				if (vertIndices[i] == vertIndices[j] &&
					(src_tc[tcIndices[i]].s == src_tc[tcIndices[j]].s &&
						src_tc[tcIndices[i]].t == src_tc[tcIndices[j]].t)) {
					// duplicate vertex
					remap[j] = i;
					finalIndices[j] = numverts;
				}
			}
		}

		// new vertex
		remap[i] = i;
		finalIndices[i] = numverts++;
	}

	Hunk_Begin(&model->hunk, 50u<<20);
	model->type = MOD_ALIAS;
	model->nummeshes = 1;
	model->numframes = header.num_frames;
	CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t)));
	CHECK(model->frames = MOD_Malloc(header.num_frames * sizeof(maliasframe_t)));

	dst_mesh = model->meshes;
	dst_mesh->numtris    = numindices / 3;
	dst_mesh->numindices = numindices;
	dst_mesh->numverts   = numverts;
	dst_mesh->numskins   = header.num_skins;
	CHECK(dst_mesh->positions  = MOD_Malloc(numverts   * header.num_frames * sizeof(vec3_t)));
	CHECK(dst_mesh->normals    = MOD_Malloc(numverts   * header.num_frames * sizeof(vec3_t)));
	CHECK(dst_mesh->tex_coords = MOD_Malloc(numverts   * header.num_frames * sizeof(vec2_t)));
    CHECK(dst_mesh->indices    = MOD_Malloc(numindices * sizeof(int)));

	if (dst_mesh->numtris != header.num_tris) {
		Com_DPrintf("%s has %d bad triangles\n", model->name, header.num_tris - dst_mesh->numtris);
	}

	// store final triangle indices
	for (int i = 0; i < numindices; i++) {
		dst_mesh->indices[i] = finalIndices[i];
	}

	// load all skins
	src_skin = (char *)rawdata + header.ofs_skins;
	for (int i = 0; i < header.num_skins; i++) {
		if (!Q_memccpy(skinname, src_skin, 0, sizeof(skinname))) {
			ret = Q_ERR_STRING_TRUNCATED;
			goto fail;
		}
		FS_NormalizePath(skinname);

		pbr_material_t * mat = MAT_Find(skinname, IT_SKIN, IF_NONE);
		
		dst_mesh->materials[i] = mat;

        src_skin += MD2_MAX_SKINNAME;
	}

	// load all tcoords
	src_tc = (dmd2stvert_t *)((byte *)rawdata + header.ofs_st);
	scale_s = 1.0f / header.skinwidth;
	scale_t = 1.0f / header.skinheight;

	// load all frames
	src_frame = (dmd2frame_t *)((byte *)rawdata + header.ofs_frames);
	dst_frame = model->frames;
	for (int j = 0; j < header.num_frames; j++) {
		LittleVector(src_frame->scale, dst_frame->scale);
		LittleVector(src_frame->translate, dst_frame->translate);

		// load frame vertices
		ClearBounds(mins, maxs);

		for (int i = 0; i < numindices; i++) {
			if (remap[i] != i) {
				continue;
			}
			src_vert = &src_frame->verts[vertIndices[i]];
			vec3_t *dst_pos = &dst_mesh->positions [j * numverts + finalIndices[i]];
			vec3_t *dst_nrm = &dst_mesh->normals   [j * numverts + finalIndices[i]];
			vec2_t *dst_tc  = &dst_mesh->tex_coords[j * numverts + finalIndices[i]];

			(*dst_tc)[0] = scale_s * src_tc[tcIndices[i]].s;
			(*dst_tc)[1] = scale_t * src_tc[tcIndices[i]].t;

			(*dst_pos)[0] = src_vert->v[0] * dst_frame->scale[0] + dst_frame->translate[0];
			(*dst_pos)[1] = src_vert->v[1] * dst_frame->scale[1] + dst_frame->translate[1];
			(*dst_pos)[2] = src_vert->v[2] * dst_frame->scale[2] + dst_frame->translate[2];

			(*dst_nrm)[0] = 0.0f;
			(*dst_nrm)[1] = 0.0f;
			(*dst_nrm)[2] = 0.0f;

			val = src_vert->lightnormalindex;

			if (val < NUMVERTEXNORMALS) {
				(*dst_nrm)[0] = bytedirs[val][0];
				(*dst_nrm)[1] = bytedirs[val][1];
				(*dst_nrm)[2] = bytedirs[val][2];
			}

			for (int k = 0; k < 3; k++) {
				val = (*dst_pos)[k];
				if (val < mins[k])
					mins[k] = val;
				if (val > maxs[k])
					maxs[k] = val;
			}
		}

		// if all normals are the same, rebuild them as flat triangle normals
		if (all_normals_same)
		{
			for (int tri = 0; tri < numindices / 3; tri++)
			{
				int i0 = j * numverts + finalIndices[tri * 3 + 0];
				int i1 = j * numverts + finalIndices[tri * 3 + 1];
				int i2 = j * numverts + finalIndices[tri * 3 + 2];

				vec3_t *p0 = &dst_mesh->positions[i0];
				vec3_t *p1 = &dst_mesh->positions[i1];
				vec3_t *p2 = &dst_mesh->positions[i2];

				vec3_t e1, e2, n;
				VectorSubtract(*p1, *p0, e1);
				VectorSubtract(*p2, *p0, e2);
				CrossProduct(e2, e1, n);
				VectorNormalize(n);

				VectorCopy(n, dst_mesh->normals[i0]);
				VectorCopy(n, dst_mesh->normals[i1]);
				VectorCopy(n, dst_mesh->normals[i2]);
			}
		}

		VectorVectorScale(mins, dst_frame->scale, mins);
		VectorVectorScale(maxs, dst_frame->scale, maxs);

		dst_frame->radius = RadiusFromBounds(mins, maxs);

		VectorAdd(mins, dst_frame->translate, dst_frame->bounds[0]);
		VectorAdd(maxs, dst_frame->translate, dst_frame->bounds[1]);

		src_frame = (dmd2frame_t *)((byte *)src_frame + header.framesize);
		dst_frame++;
	}

	// fix winding order
	for (int i = 0; i < dst_mesh->numindices; i += 3) {
		int tmp = dst_mesh->indices[i + 1];
		dst_mesh->indices[i + 1] = dst_mesh->indices[i + 2];
		dst_mesh->indices[i + 2] = tmp;
	}

	smooth_model_normals(model);

	compute_missing_model_tangents(model);

	extract_model_lights(model);

	Hunk_End(&model->hunk);
	return Q_ERR_SUCCESS;

fail:
	Hunk_Free(&model->hunk);
	return ret;
}

#if USE_MD3

#define TAB_SIN(x) qvk.sintab[(x) & 255]
#define TAB_COS(x) qvk.sintab[((x) + 64) & 255]

static int MOD_LoadMD3Mesh(model_t *model, maliasmesh_t *mesh,
		const byte *rawdata, size_t length, size_t *offset_p)
{
	dmd3mesh_t      header;
	size_t          end;
	dmd3vertex_t    *src_vert;
	dmd3coord_t     *src_tc;
	dmd3skin_t      *src_skin;
	uint32_t        *src_idx;
	vec3_t          *dst_vert;
	vec3_t          *dst_norm;
	vec2_t          *dst_tc;
	int  *dst_idx;
	char            skinname[MAX_QPATH];
	int             i, ret;

	if (length < sizeof(header))
		return Q_ERR_BAD_EXTENT;

	// byte swap the header
	LittleBlock(&header, rawdata, sizeof(header));

	if (header.meshsize < sizeof(header) || header.meshsize > length)
		return Q_ERR_BAD_EXTENT;
	if (header.meshsize % q_alignof(dmd3mesh_t))
		return Q_ERR_BAD_ALIGN;
	if (header.num_verts < 3)
		return Q_ERR_TOO_FEW;
	if (header.num_verts > TESS_MAX_VERTICES)
		return Q_ERR_TOO_MANY;
	if (header.num_tris < 1)
		return Q_ERR_TOO_FEW;
	if (header.num_tris > TESS_MAX_INDICES / 3)
		return Q_ERR_TOO_MANY;
	if (header.num_skins > MAX_ALIAS_SKINS)
		return Q_ERR_TOO_MANY;
	end = header.ofs_skins + header.num_skins * sizeof(dmd3skin_t);
	if (end < header.ofs_skins || end > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_skins % q_alignof(dmd3skin_t))
		return Q_ERR_BAD_ALIGN;
	end = header.ofs_verts + header.num_verts * model->numframes * sizeof(dmd3vertex_t);
	if (end < header.ofs_verts || end > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_verts % q_alignof(dmd3vertex_t))
		return Q_ERR_BAD_ALIGN;
	end = header.ofs_tcs + header.num_verts * sizeof(dmd3coord_t);
	if (end < header.ofs_tcs || end > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_tcs % q_alignof(dmd3coord_t))
		return Q_ERR_BAD_ALIGN;
	end = header.ofs_indexes + header.num_tris * 3 * sizeof(uint32_t);
	if (end < header.ofs_indexes || end > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_indexes & 3)
		return Q_ERR_BAD_ALIGN;

	mesh->numtris = header.num_tris;
	mesh->numindices = header.num_tris * 3;
	mesh->numverts = header.num_verts;
	mesh->numskins = header.num_skins;
	CHECK(mesh->positions = MOD_Malloc(header.num_verts * model->numframes * sizeof(vec3_t)));
	CHECK(mesh->normals = MOD_Malloc(header.num_verts * model->numframes * sizeof(vec3_t)));
	CHECK(mesh->tex_coords = MOD_Malloc(header.num_verts * model->numframes * sizeof(vec2_t)));
    CHECK(mesh->indices = MOD_Malloc(sizeof(int) * header.num_tris * 3));

	// load all skins
	src_skin = (dmd3skin_t *)(rawdata + header.ofs_skins);
	for (i = 0; i < header.num_skins; i++, src_skin++) {
		if (!Q_memccpy(skinname, src_skin->name, 0, sizeof(skinname)))
			return Q_ERR_STRING_TRUNCATED;
		FS_NormalizePath(skinname);

		pbr_material_t * mat = MAT_Find(skinname, IT_SKIN, IF_NONE);
		
		mesh->materials[i] = mat;
    }

	// load all vertices
	src_vert = (dmd3vertex_t *)(rawdata + header.ofs_verts);
	dst_vert = mesh->positions;
	dst_norm = mesh->normals;
    dst_tc = mesh->tex_coords;
    for (int frame = 0; frame < header.num_frames; frame++)
	{
		maliasframe_t *f = &model->frames[frame];
		src_tc = (dmd3coord_t *)(rawdata + header.ofs_tcs);

		for (i = 0; i < header.num_verts; i++) 
		{
			(*dst_vert)[0] = (float)(src_vert->point[0]) / 64.f;
			(*dst_vert)[1] = (float)(src_vert->point[1]) / 64.f;
			(*dst_vert)[2] = (float)(src_vert->point[2]) / 64.f;

			unsigned int lat = src_vert->norm[0];
			unsigned int lng = src_vert->norm[1];

			(*dst_norm)[0] = TAB_SIN(lat) * TAB_COS(lng);
			(*dst_norm)[1] = TAB_SIN(lat) * TAB_SIN(lng);
			(*dst_norm)[2] = TAB_COS(lat);

			VectorNormalize(*dst_norm);

			(*dst_tc)[0] = LittleFloat(src_tc->st[0]);
			(*dst_tc)[1] = LittleFloat(src_tc->st[1]);

			for (int k = 0; k < 3; k++) {
                f->bounds[0][k] = min(f->bounds[0][k], (*dst_vert)[k]);
                f->bounds[1][k] = max(f->bounds[1][k], (*dst_vert)[k]);
            }

			src_vert++; dst_vert++; dst_norm++;
			src_tc++; dst_tc++;
		}
	}


	// load all triangle indices
	src_idx = (uint32_t *)(rawdata + header.ofs_indexes);
	dst_idx = mesh->indices;
	for (i = 0; i < header.num_tris; i++) 
	{
		dst_idx[0] = LittleLong(src_idx[2]);
		dst_idx[1] = LittleLong(src_idx[1]);
		dst_idx[2] = LittleLong(src_idx[0]);

		if (dst_idx[0] >= header.num_verts)
			return Q_ERR_BAD_INDEX;

		src_idx += 3;
		dst_idx += 3;
	}
	
	*offset_p = header.meshsize;

	return Q_ERR_SUCCESS;

fail:
	return ret;
}

/* QUAKE III'S HEALTH SPHERES, MADE ROUND.

   Each is a ball of 128 triangles round the cross, which Quake III drew with
   an additive reflection map that hid the facets. Drawn as glass the facets
   show in the outline and in what is seen through it (Matt, q3dm0, 11-16).
   So it is fitted to the sphere it stands for - the centre of its box, the
   mean distance of its corners from that - split ROUND_SPHERE_LEVELS times,
   each triangle into four (128 -> 2048), and every corner put on the sphere
   with its normal straight out of it. Single-frame meshes only. */
#define ROUND_SPHERE_LEVELS 2

static bool md3_is_round_sphere(const char *mod_name)
{
	return mod_name && Q_stristr(mod_name, "powerups/health/") && Q_stristr(mod_name, "_sphere");
}

typedef struct {
	int a, b;       // the edge's corners, a < b; a = -1 for an empty slot
	int mid;        // the corner made at its middle
} sphere_edge_t;

static int sphere_edge_mid(sphere_edge_t *table, uint32_t mask, int a, int b,
                           vec3_t *pos, vec2_t *tc, int *num_verts)
{
	if (a > b) {
		int t = a; a = b; b = t;
	}

	uint32_t h = ((uint32_t)a * 2654435761u) ^ ((uint32_t)b * 40503u);
	for (uint32_t i = h & mask;; i = (i + 1) & mask) {
		sphere_edge_t *e = &table[i];
		if (e->a == a && e->b == b)
			return e->mid;
		if (e->a < 0) {
			int v = (*num_verts)++;
			VectorAdd(pos[a], pos[b], pos[v]);
			VectorScale(pos[v], 0.5f, pos[v]);
			tc[v][0] = 0.5f * (tc[a][0] + tc[b][0]);
			tc[v][1] = 0.5f * (tc[a][1] + tc[b][1]);
			e->a = a;
			e->b = b;
			e->mid = v;
			return v;
		}
	}
}

static void round_sphere_mesh(model_t *model, maliasmesh_t *mesh)
{
	if (model->numframes != 1 || mesh->numverts < 4 || mesh->numtris < 4)
		return;

	// the most corners and triangles there can be: every level adds at most
	// one corner per edge, three per triangle
	int max_verts = mesh->numverts, tris = mesh->numtris;
	for (int l = 0; l < ROUND_SPHERE_LEVELS; l++) {
		max_verts += 3 * tris;
		tris *= 4;
	}
	if (max_verts > TESS_MAX_VERTICES || tris * 3 > TESS_MAX_INDICES)
		return;

	vec3_t mins, maxs, centre;
	ClearBounds(mins, maxs);
	for (int v = 0; v < mesh->numverts; v++)
		AddPointToBounds(mesh->positions[v], mins, maxs);
	VectorAdd(mins, maxs, centre);
	VectorScale(centre, 0.5f, centre);

	float radius = 0.f;
	for (int v = 0; v < mesh->numverts; v++)
		radius += Distance(mesh->positions[v], centre);
	radius /= (float)mesh->numverts;
	if (radius <= 0.f)
		return;

	vec3_t *pos = MOD_Malloc(max_verts * sizeof(vec3_t));
	vec3_t *nrm = MOD_Malloc(max_verts * sizeof(vec3_t));
	vec2_t *tc = MOD_Malloc(max_verts * sizeof(vec2_t));
	int *idx = MOD_Malloc(tris * 3 * sizeof(int));
	int *src = Z_Malloc(tris * 3 * sizeof(int));
	int *dst = Z_Malloc(tris * 3 * sizeof(int));
	if (!pos || !nrm || !tc || !idx) {
		Z_Free(src);
		Z_Free(dst);
		return;
	}

	int num_verts = mesh->numverts, num_tris = mesh->numtris;
	memcpy(pos, mesh->positions, num_verts * sizeof(vec3_t));
	memcpy(tc, mesh->tex_coords, num_verts * sizeof(vec2_t));
	memcpy(src, mesh->indices, num_tris * 3 * sizeof(int));

	for (int l = 0; l < ROUND_SPHERE_LEVELS; l++) {
		// a table at least twice the edges (3 per triangle at most)
		uint32_t size = 1;
		while (size < (uint32_t)num_tris * 6)
			size <<= 1;
		sphere_edge_t *table = Z_Malloc(size * sizeof(*table));
		for (uint32_t i = 0; i < size; i++)
			table[i].a = table[i].b = -1;

		for (int t = 0; t < num_tris; t++) {
			const int i0 = src[t * 3 + 0], i1 = src[t * 3 + 1], i2 = src[t * 3 + 2];
			const int m01 = sphere_edge_mid(table, size - 1, i0, i1, pos, tc, &num_verts);
			const int m12 = sphere_edge_mid(table, size - 1, i1, i2, pos, tc, &num_verts);
			const int m20 = sphere_edge_mid(table, size - 1, i2, i0, pos, tc, &num_verts);
			int *out = dst + t * 12;

			// the corners keep their order, so the winding does too
			out[0] = i0;  out[1] = m01;  out[2] = m20;
			out[3] = m01; out[4] = i1;   out[5] = m12;
			out[6] = m20; out[7] = m12;  out[8] = i2;
			out[9] = m01; out[10] = m12; out[11] = m20;
		}
		Z_Free(table);

		num_tris *= 4;
		int *swap = src; src = dst; dst = swap;
	}

	for (int v = 0; v < num_verts; v++) {
		vec3_t d;
		VectorSubtract(pos[v], centre, d);
		if (VectorNormalize(d) <= 0.f)
			VectorSet(d, 0.f, 0.f, 1.f);
		VectorMA(centre, radius, d, pos[v]);
		VectorCopy(d, nrm[v]);
	}

	memcpy(idx, src, num_tris * 3 * sizeof(int));
	Z_Free(src);
	Z_Free(dst);

	mesh->positions = pos;
	mesh->normals = nrm;
	mesh->tex_coords = tc;
	mesh->indices = idx;
	mesh->numverts = num_verts;
	mesh->numtris = num_tris;
	mesh->numindices = num_tris * 3;
}

int MOD_LoadMD3_RTX(model_t *model, const void *rawdata, size_t length, const char* mod_name)
{
	dmd3header_t    header;
	size_t          end, offset, remaining;
	dmd3frame_t     *src_frame;
	maliasframe_t   *dst_frame;
	const byte      *src_mesh;
	int             i;
	int             ret;

	if (length < sizeof(header))
		return Q_ERR_FILE_TOO_SMALL;

	// byte swap the header
	LittleBlock(&header, rawdata, sizeof(header));

	if (header.ident != MD3_IDENT)
		return Q_ERR_UNKNOWN_FORMAT;
	if (header.version != MD3_VERSION)
		return Q_ERR_UNKNOWN_FORMAT;
	if (header.num_frames < 1)
		return Q_ERR_TOO_FEW;
	if (header.num_frames > MD3_MAX_FRAMES)
		return Q_ERR_TOO_MANY;
	end = header.ofs_frames + sizeof(dmd3frame_t) * header.num_frames;
	if (end < header.ofs_frames || end > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_frames % q_alignof(dmd3frame_t))
		return Q_ERR_BAD_ALIGN;
	if (header.num_meshes < 1)
		return Q_ERR_TOO_FEW;
	if (header.num_meshes > MD3_MAX_MESHES)
		return Q_ERR_TOO_MANY;
	if (header.ofs_meshes > length)
		return Q_ERR_BAD_EXTENT;
	if (header.ofs_meshes % q_alignof(dmd3mesh_t))
		return Q_ERR_BAD_ALIGN;

	Hunk_Begin(&model->hunk, 0x4000000);
	model->type = MOD_ALIAS;
	model->numframes = header.num_frames;
	model->nummeshes = header.num_meshes;
	CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * header.num_meshes));
	CHECK(model->frames = MOD_Malloc(sizeof(maliasframe_t) * header.num_frames));

	// load all frames
	src_frame = (dmd3frame_t *)((byte *)rawdata + header.ofs_frames);
	dst_frame = model->frames;
	for (i = 0; i < header.num_frames; i++) {
		LittleVector(src_frame->translate, dst_frame->translate);
		VectorSet(dst_frame->scale, MD3_XYZ_SCALE, MD3_XYZ_SCALE, MD3_XYZ_SCALE);

		ClearBounds(dst_frame->bounds[0], dst_frame->bounds[1]);

		src_frame++; dst_frame++;
	}

	// load all meshes
	src_mesh = (const byte *)rawdata + header.ofs_meshes;
	remaining = length - header.ofs_meshes;
	for (i = 0; i < header.num_meshes; i++) {
		ret = MOD_LoadMD3Mesh(model, &model->meshes[i], src_mesh, remaining, &offset);
		if (ret)
			goto fail;
		src_mesh += offset;
		remaining -= offset;
	}

	// Quake III's health spheres, made round - see round_sphere_mesh
	if (md3_is_round_sphere(mod_name) && model->numframes == 1) {
		maliasframe_t *f = &model->frames[0];

		ClearBounds(f->bounds[0], f->bounds[1]);
		for (i = 0; i < header.num_meshes; i++) {
			maliasmesh_t *mesh = &model->meshes[i];

			round_sphere_mesh(model, mesh);
			for (int v = 0; v < mesh->numverts; v++)
				AddPointToBounds(mesh->positions[v], f->bounds[0], f->bounds[1]);
		}
	}

    // calculate frame bounds. MOD_LoadMD3Mesh took them from vertices already
    // in world units (point / 64) and never moved by translate, so they are
    // final as they stand. They were scaled by MD3_XYZ_SCALE again and moved,
    // which left every md3 box 64 times too small - harmless until the blood's
    // mesh trace rejected on it (11-15: no blood on md3 pickups and barrels).
    dst_frame = model->frames;
    for (i = 0; i < header.num_frames; i++) {
        dst_frame->radius = RadiusFromBounds(dst_frame->bounds[0], dst_frame->bounds[1]);
        dst_frame++;
    }

	// no smooth_model_normals: the authored normals are better than any
	// rebuilt from 1/64-unit positions (see the note above it)

	compute_missing_model_tangents(model);

	extract_model_lights(model);

	Hunk_End(&model->hunk);
	return Q_ERR_SUCCESS;

fail:
	Hunk_Free(&model->hunk);
	return ret;
}
#endif

int MOD_LoadIQM_RTX(model_t* model, const void* rawdata, size_t length, const char* mod_name)
{
	Hunk_Begin(&model->hunk, 0x4000000);
	model->type = MOD_ALIAS;

	int res = MOD_LoadIQM_Base(model, rawdata, length, mod_name), ret;

	if (res != Q_ERR_SUCCESS)
	{
		Hunk_Free(&model->hunk);
		return res;
	}

	char base_path[MAX_QPATH];
	COM_FilePath(mod_name, base_path, sizeof(base_path));

	CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * model->iqmData->num_meshes));
	model->nummeshes = (int)model->iqmData->num_meshes;
	model->numframes = 1; // these are baked frames, so that the VBO uploader will only make one copy of the vertices

	for (unsigned model_idx = 0; model_idx < model->iqmData->num_meshes; model_idx++)
	{
		iqm_mesh_t* iqm_mesh = &model->iqmData->meshes[model_idx];
		maliasmesh_t* mesh = &model->meshes[model_idx];

		mesh->indices = iqm_mesh->data->indices ? (int*)iqm_mesh->data->indices + iqm_mesh->first_triangle * 3 : NULL;
		mesh->positions = iqm_mesh->data->positions ? (vec3_t*)(iqm_mesh->data->positions + iqm_mesh->first_vertex * 3) : NULL;
		mesh->normals = iqm_mesh->data->normals ? (vec3_t*)(iqm_mesh->data->normals + iqm_mesh->first_vertex * 3) : NULL;
		mesh->tex_coords = iqm_mesh->data->texcoords ? (vec2_t*)(iqm_mesh->data->texcoords + iqm_mesh->first_vertex * 2) : NULL;
		mesh->tangents = iqm_mesh->data->tangents ? (vec3_t*)(iqm_mesh->data->tangents + iqm_mesh->first_vertex * 3) : NULL;
		mesh->blend_indices = iqm_mesh->data->blend_indices ? (uint32_t*)(iqm_mesh->data->blend_indices + iqm_mesh->first_vertex * 4) : NULL;
		mesh->blend_weights = iqm_mesh->data->blend_weights ? (uint32_t*)(iqm_mesh->data->blend_weights + iqm_mesh->first_vertex * 4) : NULL;

		mesh->numindices = (int)(iqm_mesh->num_triangles * 3);
		mesh->numverts = (int)iqm_mesh->num_vertexes;
		mesh->numtris = (int)iqm_mesh->num_triangles;

		// convert the indices from IQM global space to mesh-local space; fix winding order.
		for (unsigned triangle_idx = 0; triangle_idx < iqm_mesh->num_triangles; triangle_idx++)
		{
			int tri[3];
			tri[0] = mesh->indices[triangle_idx * 3 + 0];
			tri[1] = mesh->indices[triangle_idx * 3 + 1];
			tri[2] = mesh->indices[triangle_idx * 3 + 2];

			mesh->indices[triangle_idx * 3 + 0] = tri[2] - (int)iqm_mesh->first_vertex;
			mesh->indices[triangle_idx * 3 + 1] = tri[1] - (int)iqm_mesh->first_vertex;
			mesh->indices[triangle_idx * 3 + 2] = tri[0] - (int)iqm_mesh->first_vertex;
		}

	    char filename[MAX_QPATH];
		Q_snprintf(filename, sizeof(filename), "%s/%s.pcx", base_path, iqm_mesh->material);
		pbr_material_t* mat = MAT_Find(filename, IT_SKIN, IF_NONE);
		assert(mat); // it's either found or created
		
		mesh->materials[0] = mat;
		mesh->numskins = 1; // looks like IQM only supports one skin?
	}

	compute_missing_model_tangents(model);

	extract_model_lights(model);

	Hunk_End(&model->hunk);
	
	return Q_ERR_SUCCESS;

fail:
	return ret;
}

int MOD_LoadMD5_RTX(model_t *model, const void *rawdata, size_t length, const char *mod_name)
{
	int ret;

	Hunk_Begin(&model->hunk, 0x4000000);
	model->type = MOD_ALIAS;

	int res = MOD_LoadMD5_Base(model, rawdata, length, mod_name);

	if (res != Q_ERR_SUCCESS)
	{
		Hunk_Free(&model->hunk);
		return res;
	}

	CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * model->iqmData->num_meshes));
	model->nummeshes = (int)model->iqmData->num_meshes;
	model->numframes = 1; // baked frames, so the VBO uploader makes one copy of the vertices

	// every mesh of a model shares the one skin list, exactly as the md2's did;
	// which image each skin is comes from model_md5.c, shared with GL and software
	md5_skin_t skins[MAX_ALIAS_SKINS];
	pbr_material_t *shared_materials[MAX_ALIAS_SKINS] = { 0 };
	int numskins = MD5_ResolveSkins(mod_name, skins);

	for (int i = 0; i < numskins; i++)
	{
		shared_materials[i] = MAT_Find(skins[i].path, IT_SKIN, IF_NONE);
		assert(shared_materials[i]);

		// the classic material's tuning - see MD5_LoadSkinsFromMD2
		if (skins[i].inherit[0])
			MAT_InheritScalars(shared_materials[i], skins[i].inherit);
	}

	for (unsigned model_idx = 0; model_idx < model->iqmData->num_meshes; model_idx++)
	{
		iqm_mesh_t *iqm_mesh = &model->iqmData->meshes[model_idx];
		maliasmesh_t *mesh = &model->meshes[model_idx];

		mesh->indices = (int *)model->iqmData->indices + iqm_mesh->first_triangle * 3;
		mesh->positions = (vec3_t *)(model->iqmData->positions + iqm_mesh->first_vertex * 3);
		mesh->normals = (vec3_t *)(model->iqmData->normals + iqm_mesh->first_vertex * 3);
		mesh->tex_coords = (vec2_t *)(model->iqmData->texcoords + iqm_mesh->first_vertex * 2);
		mesh->tangents = NULL;
		mesh->blend_indices = (uint32_t *)(model->iqmData->blend_indices + iqm_mesh->first_vertex * 4);
		mesh->blend_weights = (uint32_t *)(model->iqmData->blend_weights + iqm_mesh->first_vertex * 4);

		mesh->numindices = (int)(iqm_mesh->num_triangles * 3);
		mesh->numverts = (int)iqm_mesh->num_vertexes;
		mesh->numtris = (int)iqm_mesh->num_triangles;

		// the loader emits global indices in the file's winding; make them
		// mesh local and reverse them, as the IQM and MD2 paths both do
		for (unsigned triangle_idx = 0; triangle_idx < iqm_mesh->num_triangles; triangle_idx++)
		{
			int tri[3];
			tri[0] = mesh->indices[triangle_idx * 3 + 0];
			tri[1] = mesh->indices[triangle_idx * 3 + 1];
			tri[2] = mesh->indices[triangle_idx * 3 + 2];

			mesh->indices[triangle_idx * 3 + 0] = tri[2] - (int)iqm_mesh->first_vertex;
			mesh->indices[triangle_idx * 3 + 1] = tri[1] - (int)iqm_mesh->first_vertex;
			mesh->indices[triangle_idx * 3 + 2] = tri[0] - (int)iqm_mesh->first_vertex;
		}

		memcpy(mesh->materials, shared_materials, sizeof(mesh->materials));
		mesh->numskins = numskins;
	}

	// no smooth_model_normals: model_md5.c already welded these smooth, and
	// the crease test only cut seams back into them (see the note above it)

	compute_missing_model_tangents(model);

	extract_model_lights(model);

	Hunk_End(&model->hunk);

	return Q_ERR_SUCCESS;

fail:
	Hunk_Free(&model->hunk);
	return ret;
}

extern model_vbo_t model_vertex_data[];

void MOD_Reference_RTX(model_t *model)
{
	int mesh_idx, skin_idx, frame_idx;

	// register any images used by the models
	switch (model->type) {
	case MOD_ALIAS:
		for (mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++) {
			maliasmesh_t *mesh = &model->meshes[mesh_idx];
			for (skin_idx = 0; skin_idx < mesh->numskins; skin_idx++) {
				MAT_UpdateRegistration(mesh->materials[skin_idx]);
			}
		}
		break;
	case MOD_SPRITE:
		for (frame_idx = 0; frame_idx < model->numframes; frame_idx++) {
			model->spriteframes[frame_idx].image->registration_sequence = registration_sequence;
		}
		break;
	case MOD_EMPTY:
		break;
	default:
		Q_assert(!"bad model type");
	}

	model->registration_sequence = registration_sequence;
	model_vertex_data[model - r_models].registration_sequence = registration_sequence;
}

/*
===============================================================================

MOD_TraceMesh - a ray against an alias model's real triangles

Why this exists at all is in inc/refresh/models.h: the collision world only has
one axis-aligned box per entity, and a box is fine for stopping something and
useless for placing something.  Blood droplets are the caller.

Three things make it affordable:

  THE POSE CACHE.  Posing a model is per-vertex work and a burst of blood is
  sixty rays against the same body in the same frame.  So a pose is built once -
  every vertex position and normal in model space, plus one AABB per triangle -
  and keyed on (model, frame).  Snapping to the discrete frame rather than the
  renderer's interpolated one is what makes that key stand still: a corpse holds
  one frame forever and never rebuilds, and a walking monster rebuilds at the
  animation rate rather than the frame rate.

  THE PER-TRIANGLE AABB.  A droplet moves a few units per frame, so its segment's
  own bounding box is tiny and misses ~99% of the triangles outright.  Six
  compares to reject beats forty flops of Moller-Trumbore by enough to turn a
  1200-triangle monster from ~20us per ray into ~3us.

  FRONT FACES ONLY.  A droplet that starts inside a body - a wound spawns them on
  the skin and the spray cone points every way - leaves through the far side
  instead of sticking to the inside of the mesh where nobody would ever see it.

===============================================================================
*/

// Big enough for everything in the game: the largest monster here is the tank at
// 2160 triangles / 1821 vertices. A model over the limit simply gets no mesh
// trace, which degrades to the old behaviour for that model rather than failing.
#define MOD_TRACE_MAX_VERTS     8192
#define MOD_TRACE_MAX_TRIS      8192

// Three were enough for a couple of bodies being shot at once. Six since blood
// tests corpses and items too (cl_blood_skin, 11-15): their frames are frozen, so
// each holds a slot as long as blood flies near it.
#define MOD_TRACE_CACHE_SLOTS   6

typedef struct {
    const model_t *model;
    int         registration;   // model->registration_sequence, so a level
                                // change cannot match a recycled r_models slot
    int         frame;
    int         serial;         // LRU stamp
    bool        valid;

    int         numverts;
    int         numtris;
    vec3_t      positions[MOD_TRACE_MAX_VERTS];
    vec3_t      normals[MOD_TRACE_MAX_VERTS];
    vec2_t      st[MOD_TRACE_MAX_VERTS];        // the meshes' texture coordinates
    // Per triangle: its three vertex indices into the arrays above, and the
    // bounding box that rejects it.
    int         tris[MOD_TRACE_MAX_TRIS][3];
    vec3_t      tri_mins[MOD_TRACE_MAX_TRIS];
    vec3_t      tri_maxs[MOD_TRACE_MAX_TRIS];
    uint8_t     tri_mesh[MOD_TRACE_MAX_TRIS];   // which mesh it came from
    vec3_t      mins, maxs;                     // the whole pose
} mod_trace_pose_cache_t;

static mod_trace_pose_cache_t *mod_trace_cache;
static int mod_trace_serial;

// Skin one vertex or normal by the IQM blend matrices, matching get_triangle()
// in instance_geometry.comp: a mat3x4 applied as a row vector, weights summed
// and divided rather than assumed to be normalised.  w is 1 for a position and
// 0 for a direction.
static void mod_trace_skin(const float *pose_matrices, uint32_t indices, uint32_t weights,
                           const vec3_t in, float w, vec3_t out)
{
    float acc[12] = { 0 };
    float weight_sum = 0.f;

    for (int i = 0; i < 4; i++) {
        const uint32_t bone = (indices >> (i * 8)) & 0xff;
        const float weight = (float)((weights >> (i * 8)) & 0xff);

        if (weight <= 0.f)
            continue;

        const float *m = pose_matrices + bone * 12;
        for (int j = 0; j < 12; j++)
            acc[j] += m[j] * weight;

        weight_sum += weight;
    }

    if (weight_sum <= 0.f) {
        VectorCopy(in, out);
        return;
    }

    const float rcp = 1.f / weight_sum;
    const vec3_t p = { in[0] * rcp, in[1] * rcp, in[2] * rcp };
    const float pw = w * rcp;

    for (int j = 0; j < 3; j++)
        out[j] = acc[j * 4 + 0] * p[0] + acc[j * 4 + 1] * p[1]
               + acc[j * 4 + 2] * p[2] + acc[j * 4 + 3] * pw;
}

static bool mod_trace_build_pose(mod_trace_pose_cache_t *slot, const model_t *model, int frame)
{
    float pose_matrices[IQM_MAX_JOINTS * 12];
    const bool skinned = model->iqmData && model->iqmData->num_poses;

    slot->valid = false;
    slot->model = model;
    slot->registration = model->registration_sequence;
    slot->frame = frame;
    slot->numverts = 0;
    slot->numtris = 0;
    ClearBounds(slot->mins, slot->maxs);

    if (model->nummeshes > 256)
        return false;   // tri_mesh is a byte

    if (skinned) {
        entity_t e;

        if (model->iqmData->num_poses > IQM_MAX_JOINTS)
            return false;

        // R_ComputeIQMTransforms reads exactly these three fields.
        memset(&e, 0, sizeof(e));
        e.frame = frame;
        e.oldframe = frame;
        e.backlerp = 0.f;

        if (!R_ComputeIQMTransforms(model->iqmData, &e, pose_matrices))
            return false;
    }

    for (int mesh_idx = 0; mesh_idx < model->nummeshes; mesh_idx++) {
        const maliasmesh_t *mesh = &model->meshes[mesh_idx];
        const int base = slot->numverts;

        if (!mesh->positions || !mesh->normals || !mesh->indices || !mesh->tex_coords)
            return false;

        if (base + mesh->numverts > MOD_TRACE_MAX_VERTS)
            return false;

        if (slot->numtris + mesh->numtris > MOD_TRACE_MAX_TRIS)
            return false;

        if (skinned) {
            if (!mesh->blend_indices || !mesh->blend_weights)
                return false;

            for (int i = 0; i < mesh->numverts; i++) {
                mod_trace_skin(pose_matrices, mesh->blend_indices[i], mesh->blend_weights[i],
                               mesh->positions[i], 1.f, slot->positions[base + i]);
                mod_trace_skin(pose_matrices, mesh->blend_indices[i], mesh->blend_weights[i],
                               mesh->normals[i], 0.f, slot->normals[base + i]);
                VectorNormalize(slot->normals[base + i]);
            }
        } else {
            // MD2 and MD3 store every frame's vertices back to back.
            const int off = frame * mesh->numverts;

            for (int i = 0; i < mesh->numverts; i++) {
                VectorCopy(mesh->positions[off + i], slot->positions[base + i]);
                VectorCopy(mesh->normals[off + i], slot->normals[base + i]);
            }
        }

        for (int i = 0; i < mesh->numverts; i++) {
            // one set for every frame (md2/md3 and skinned alike)
            slot->st[base + i][0] = mesh->tex_coords[i][0];
            slot->st[base + i][1] = mesh->tex_coords[i][1];
            AddPointToBounds(slot->positions[base + i], slot->mins, slot->maxs);
        }

        slot->numverts += mesh->numverts;

        for (int i = 0; i < mesh->numtris; i++) {
            const int t = slot->numtris + i;
            int j;

            slot->tri_mesh[t] = (uint8_t)mesh_idx;

            for (j = 0; j < 3; j++) {
                const int idx = mesh->indices[i * 3 + j];

                if (idx < 0 || idx >= mesh->numverts)
                    return false;

                slot->tris[t][j] = base + idx;
            }

            ClearBounds(slot->tri_mins[t], slot->tri_maxs[t]);
            for (j = 0; j < 3; j++)
                AddPointToBounds(slot->positions[slot->tris[t][j]],
                                 slot->tri_mins[t], slot->tri_maxs[t]);
        }

        slot->numtris += mesh->numtris;
    }

    slot->valid = true;
    return true;
}

static mod_trace_pose_cache_t *mod_trace_get_pose(const model_t *model, int frame)
{
    mod_trace_pose_cache_t *slot = NULL;
    int oldest = INT_MAX;

    if (!mod_trace_cache) {
        mod_trace_cache = Z_Mallocz(sizeof(mod_trace_pose_cache_t) * MOD_TRACE_CACHE_SLOTS);
        if (!mod_trace_cache)
            return NULL;
    }

    for (int i = 0; i < MOD_TRACE_CACHE_SLOTS; i++) {
        mod_trace_pose_cache_t *c = &mod_trace_cache[i];

        if (c->model == model && c->frame == frame
            && c->registration == model->registration_sequence) {
            c->serial = ++mod_trace_serial;
            return c->valid ? c : NULL;
        }

        if (c->serial < oldest) {
            oldest = c->serial;
            slot = c;
        }
    }

    slot->serial = ++mod_trace_serial;

    if (!mod_trace_build_pose(slot, model, frame))
        return NULL;    // stays in the slot, so the failure is not re-derived

    return slot;
}

// Called on renderer shutdown. Every renderer init starts registration_sequence
// at 1 again and refills r_models from the front, so a slot kept across the
// restart could match a different model on the same r_models entry.
void MOD_TraceMeshShutdown_RTX(void)
{
    Z_Free(mod_trace_cache);
    mod_trace_cache = NULL;
}

// Moller-Trumbore against a segment, with dir spanning the whole segment so the
// hit parameter comes out directly as a fraction in [0, 1].
static bool mod_trace_triangle(const vec3_t start, const vec3_t dir,
                               const vec3_t v0, const vec3_t v1, const vec3_t v2,
                               float *out_t, float *out_u, float *out_v)
{
    vec3_t e1, e2, pv, tv, qv;
    float det, inv_det, u, v, t;

    VectorSubtract(v1, v0, e1);
    VectorSubtract(v2, v0, e2);
    CrossProduct(dir, e2, pv);

    det = DotProduct(e1, pv);
    if (fabsf(det) < 1e-9f)
        return false;   // parallel, or a degenerate triangle

    inv_det = 1.f / det;

    VectorSubtract(start, v0, tv);
    u = DotProduct(tv, pv) * inv_det;
    if (u < 0.f || u > 1.f)
        return false;

    CrossProduct(tv, e1, qv);
    v = DotProduct(dir, qv) * inv_det;
    if (v < 0.f || u + v > 1.f)
        return false;

    t = DotProduct(e2, qv) * inv_det;
    if (t < 0.f || t > 1.f)
        return false;

    *out_t = t;
    *out_u = u;
    *out_v = v;
    return true;
}

bool MOD_TraceMesh_RTX(const model_t *model, const mod_pose_t *pose,
                       const vec3_t start, const vec3_t end,
                       mod_trace_result_t *out)
{
    mod_trace_pose_cache_t *c;
    vec3_t axis[3], local_start, local_end, dir, seg_mins, seg_maxs, normal;
    float scale, rcp_scale, best = 1.f;
    int best_tri = -1;
    float best_u = 0.f, best_v = 0.f;
    bool rotated;

    out->tested = false;
    if (!model || model->type != MOD_ALIAS || model->nummeshes <= 0)
        return false;

    int frame = pose->frame;
    if (model->iqmData && model->iqmData->num_frames)
        frame = (int)((unsigned)frame % model->iqmData->num_frames);
    else if (model->numframes > 0)
        frame = (int)((unsigned)frame % (unsigned)model->numframes);
    else
        return false;

    // World -> model space. The inverse of create_entity_matrix, which builds
    // its columns from AnglesToAxis and scales all three the same, so the
    // inverse rotation is RotatePoint against the same axis and the inverse
    // scale is one reciprocal.
    scale = (pose->scale != 0.f) ? pose->scale : 1.f;
    if (scale <= 0.f)
        return false;
    rcp_scale = 1.f / scale;

    VectorSubtract(start, pose->origin, local_start);
    VectorSubtract(end, pose->origin, local_end);

    rotated = !VectorEmpty(pose->angles);
    if (rotated) {
        AnglesToAxis(pose->angles, axis);
        RotatePoint(local_start, axis);
        RotatePoint(local_end, axis);
    }

    VectorScale(local_start, rcp_scale, local_start);
    VectorScale(local_end, rcp_scale, local_end);
    VectorSubtract(local_end, local_start, dir);

    for (int i = 0; i < 3; i++) {
        seg_mins[i] = min(local_start[i], local_end[i]);
        seg_maxs[i] = max(local_start[i], local_end[i]);
    }

    // An md2/md3 frame's box, before any posing: corpses and items are tested
    // with the client's generous boxes (cl_blood_skin), and most of those tests
    // end here. Skinned models have no frame boxes; theirs is the pose's, below.
    if (!model->iqmData && model->frames && frame < model->numframes) {
        const maliasframe_t *f = &model->frames[frame];
        for (int i = 0; i < 3; i++)
            if (seg_maxs[i] < f->bounds[0][i] - 1.f || seg_mins[i] > f->bounds[1][i] + 1.f)
                return false;
    }

    c = mod_trace_get_pose(model, frame);
    if (!c)
        return false;

    for (int i = 0; i < 3; i++)
        if (seg_maxs[i] < c->mins[i] || seg_mins[i] > c->maxs[i])
            return false;
    out->tested = true;

    for (int t = 0; t < c->numtris; t++) {
        const int *tri = c->tris[t];
        float hit_t, hit_u, hit_v;

        // The cheap reject, and the reason this is affordable at all.
        if (c->tri_maxs[t][0] < seg_mins[0] || c->tri_mins[t][0] > seg_maxs[0] ||
            c->tri_maxs[t][1] < seg_mins[1] || c->tri_mins[t][1] > seg_maxs[1] ||
            c->tri_maxs[t][2] < seg_mins[2] || c->tri_mins[t][2] > seg_maxs[2])
            continue;

        if (!mod_trace_triangle(local_start, dir,
                                c->positions[tri[0]], c->positions[tri[1]], c->positions[tri[2]],
                                &hit_t, &hit_u, &hit_v))
            continue;

        if (hit_t >= best)
            continue;

        // Front faces only: a droplet that started inside the body leaves
        // through the far side rather than sticking to the inside of the skin.
        VectorAdd(c->normals[tri[0]], c->normals[tri[1]], normal);
        VectorAdd(normal, c->normals[tri[2]], normal);
        if (DotProduct(dir, normal) >= 0.f)
            continue;

        best = hit_t;
        best_tri = t;
        best_u = hit_u;
        best_v = hit_v;
    }

    if (best_tri < 0)
        return false;

    // The interpolated normal, so a splat lies along a curved body rather than
    // faceting with the triangle it happened to land on.
    {
        const int *tri = c->tris[best_tri];
        const float w0 = 1.f - best_u - best_v;

        VectorScale(c->normals[tri[0]], w0, normal);
        VectorMA(normal, best_u, c->normals[tri[1]], normal);
        VectorMA(normal, best_v, c->normals[tri[2]], normal);

        if (VectorNormalize(normal) < 0.5f) {
            // Degenerate interpolation - fall back to the face.
            vec3_t e1, e2;
            VectorSubtract(c->positions[tri[1]], c->positions[tri[0]], e1);
            VectorSubtract(c->positions[tri[2]], c->positions[tri[0]], e2);
            CrossProduct(e1, e2, normal);
            if (VectorNormalize(normal) == 0.f)
                return false;
            if (DotProduct(dir, normal) > 0.f)
                VectorInverse(normal);
        }
    }

    // Where on the skin (cl_blood_skin): the texture coordinate at the hit, and
    // how far one world unit goes in texture space there - the triangle's texture
    // area over its world area, so a splat is the same size on any UV layout.
    {
        const int *tri = c->tris[best_tri];
        const float w0 = 1.f - best_u - best_v;
        vec3_t e1, e2, cross;

        for (int i = 0; i < 2; i++)
            out->st[i] = c->st[tri[0]][i] * w0 + c->st[tri[1]][i] * best_u + c->st[tri[2]][i] * best_v;

        VectorSubtract(c->positions[tri[1]], c->positions[tri[0]], e1);
        VectorSubtract(c->positions[tri[2]], c->positions[tri[0]], e2);
        CrossProduct(e1, e2, cross);
        const float world_area = VectorLength(cross) * scale * scale;
        const float s1 = c->st[tri[1]][0] - c->st[tri[0]][0], t1 = c->st[tri[1]][1] - c->st[tri[0]][1];
        const float s2 = c->st[tri[2]][0] - c->st[tri[0]][0], t2 = c->st[tri[2]][1] - c->st[tri[0]][1];
        const float st_area = fabsf(s1 * t2 - s2 * t1);
        out->st_per_unit = world_area > 1e-6f ? sqrtf(st_area / world_area) : 0.f;
        out->mesh = c->tri_mesh[best_tri];
    }

    // Model -> world. Uniform scale, so the normal needs the rotation only.
    if (rotated) {
        TransposeAxis(axis);
        RotatePoint(normal, axis);
    }

    VectorCopy(normal, out->normal);
    out->frac = best;
    return true;
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
