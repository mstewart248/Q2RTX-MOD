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

    // calculate frame bounds
    dst_frame = model->frames;
    for (i = 0; i < header.num_frames; i++) {
        VectorScale(dst_frame->bounds[0], MD3_XYZ_SCALE, dst_frame->bounds[0]);
        VectorScale(dst_frame->bounds[1], MD3_XYZ_SCALE, dst_frame->bounds[1]);

        dst_frame->radius = RadiusFromBounds(dst_frame->bounds[0], dst_frame->bounds[1]);

        VectorAdd(dst_frame->bounds[0], dst_frame->translate, dst_frame->bounds[0]);
        VectorAdd(dst_frame->bounds[1], dst_frame->translate, dst_frame->bounds[1]);

        dst_frame++;
    }

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

/*
=================
MD5_SkinPath

The rerelease re-unwrapped its skeletal meshes and ships new artwork for them
in an md5/ directory, so a skin cannot be taken from the .md2 path as written.
What the .md2 still owns is the skin LIST - its order is what entity->skinnum
indexes - so each entry is redirected into the md5/ directory beside it.

Which md5/ directory, though, is not always the model's own. A monster names a
skin in its own folder, so the two coincide. The player's third-person weapons
do not: players/male/md5/w_railgun.md5mesh takes its list from
players/male/w_railgun.md2, whose skin is models/weapons/g_rail/skin.pcx - and
that mesh is vertex-for-vertex the world railgun, so the artwork it wants is
the world model's, in models/weapons/g_rail/md5/. Redirecting into the MODEL's
directory instead looked for players/male/md5/skin.png, which does not exist,
and every held weapon rendered white.

So: try the skin's own md5/ directory first and fall back to the model's. The
stem is carried across either way - g_blast's skin is base.pcx, not skin.pcx.
=================
*/
static bool MD5_SkinExists(const char *path)
{
	char probe[MAX_QPATH];
	static const char *const exts[] = { "png", "tga", "jpg", "pcx" };

	for (int i = 0; i < q_countof(exts); i++) {
		Q_snprintf(probe, sizeof(probe), "%s.%s", path, exts[i]);
		if (FS_FileExists(probe))
			return true;
	}
	return false;
}

/*
=================
MD5_HeldWeaponSkin

The last few player-held weapons cannot be resolved by path at all. Some ship
no .md2 to take a skin list from (the rogue/xatrix guns were never in the 1997
game); the shotgun and the default weapon do have one, but it names
players/male/weapon.pcx - the low-detail texture the MD2 shared with the player
body, which has no md5-era equivalent.

Each of these meshes is vertex-for-vertex identical to a world weapon model
(verified by comparing vertex counts, triangle counts and UVs), so the artwork
they want is that model's. There is no rule that derives these names -
w_disrupt's counterpart is g_dist, w_grapple's is g_flareg - so the mapping is
written out. Keyed on basename, which is what makes it serve male, female and
cyborg alike.
=================
*/
static bool MD5_HeldWeaponSkin(const char *mod_name, char *buffer, size_t size)
{
	static const struct {
		const char *model;
		const char *skin;
	} held[] = {
		{ "weapon",      "models/weapons/g_shotg/md5/skin"      },
		{ "w_shotgun",   "models/weapons/g_shotg/md5/skin"      },
		{ "w_chainfist", "models/weapons/g_chainf/md5/skin"     },
		{ "w_disrupt",   "models/weapons/g_dist/md5/skin"       },
		{ "w_etfrifle",  "models/weapons/g_etf_rifle/md5/skin"  },
		{ "w_grapple",   "models/weapons/g_flareg/md5/base"     },
		{ "w_plasma",    "models/weapons/g_beamer/md5/skin"     },
		{ "w_plauncher", "models/weapons/g_plaunch/md5/proxskin" },
	};
	const char *slash;
	char name[MAX_QPATH];
	char *dot;

	// only ever applies to a model under players/<model>/md5/
	if (strncmp(mod_name, "players/", 8) || !strstr(mod_name, "/md5/"))
		return false;

	slash = strrchr(mod_name, '/');
	Q_strlcpy(name, slash ? slash + 1 : mod_name, sizeof(name));
	dot = strrchr(name, '.');
	if (dot)
		*dot = 0;

	for (int i = 0; i < q_countof(held); i++) {
		if (Q_stricmp(name, held[i].model))
			continue;
		if (!MD5_SkinExists(held[i].skin))
			break;
		Q_snprintf(buffer, size, "%s.png", held[i].skin);
		return true;
	}
	return false;
}

static void MD5_SkinPath(const char *base_path, const char *skin,
                         const char *mod_name, char *buffer, size_t size)
{
	const char *slash = strrchr(skin, '/');
	const char *stem = slash ? slash + 1 : skin;
	char name[MAX_QPATH];
	char candidate[MAX_QPATH];
	char *dot;

	Q_strlcpy(name, stem, sizeof(name));
	dot = strrchr(name, '.');
	if (dot)
		*dot = 0;

	// the md5/ directory belonging to the SKIN, which for a monster is the
	// same one the model lives in and for a held weapon is not
	if (slash) {
		size_t dirlen = (size_t)(slash - skin);

		if (dirlen < sizeof(candidate)) {
			memcpy(candidate, skin, dirlen);
			Q_snprintf(candidate + dirlen, sizeof(candidate) - dirlen,
			           "/md5/%s", name);

			if (MD5_SkinExists(candidate)) {
				Q_snprintf(buffer, size, "%s.png", candidate);
				return;
			}
		}
	}

	// a held weapon whose .md2 skin name points at nothing usable
	if (MD5_HeldWeaponSkin(mod_name, buffer, size))
		return;

	// MAT_Find truncates the extension anyway; the material system is what
	// decides which image file actually backs this name
	Q_snprintf(buffer, size, "%s/%s.png", base_path, name);
}

/*
=================
MD5_LoneSkinInDir

Last resort when no .md2 skin list is readable and neither the model's own
stem nor the "skin" convention names an existing image.

monster_fixbot is why this exists. Its md5 dir holds exactly one skin and it
is called droid.png - the name comes from the .md2's skin list, and the retail
pak the .md2 lives in is not mounted here, so that list is unavailable. The
model then fell back to md5/skin.png, which does not exist, and rendered pure
white. Every other monster happens to call its lone skin "skin", which is why
only this one showed the fault.

A directory with exactly ONE candidate image has no ambiguity to resolve, so
take it. Sidecars (_n / _rough / _metallic / _glow) are not candidates. If
there is more than one, do nothing and let the caller fall through to the
convention - guessing between skins would be worse than the old behaviour.
=================
*/
static bool MD5_LoneSkinInDir(const char *base_path, char *buffer, size_t size)
{
	static const char *const sidecars[] = { "_n", "_rough", "_metallic", "_glow" };
	void **list;
	int  num_files = 0;
	int  found = 0;
	char stem[MAX_QPATH];

	list = FS_ListFiles(base_path, ".png;.tga;.jpg;.pcx", 0, &num_files);
	if (!list)
		return false;

	for (int i = 0; i < num_files; i++) {
		const char *file_name = list[i];
		char name[MAX_QPATH];
		char *dot;
		bool is_sidecar = false;

		Q_strlcpy(name, file_name, sizeof(name));
		dot = strrchr(name, '.');
		if (dot)
			*dot = 0;

		for (int s = 0; s < q_countof(sidecars); s++) {
			size_t nlen = strlen(name), slen = strlen(sidecars[s]);
			if (nlen > slen && !Q_stricmp(name + nlen - slen, sidecars[s])) {
				is_sidecar = true;
				break;
			}
		}
		if (is_sidecar)
			continue;

		found++;
		if (found > 1)
			break;

		Q_strlcpy(stem, name, sizeof(stem));
	}

	FS_FreeList(list);

	if (found != 1)
		return false;

	Q_snprintf(buffer, size, "%s/%s.png", base_path, stem);
	return true;
}

/*
=================
MD5_LoadSkinsFromMD2

Reads the classic model's skin list so that skinnum keeps selecting the same
skin it always did. Returns 0 when the .md2 is unreadable, in which case the
caller uses a single default skin.
=================
*/
static int MD5_LoadSkinsFromMD2(const char *mod_name, const char *base_path, maliasmesh_t *mesh)
{
	char md2_path[MAX_QPATH];
	char skinname[MAX_QPATH];
	char skinpath[MAX_QPATH];
	byte *rawdata = NULL;
	dmd2header_t header;
	const char *src_skin;
	const char *slash;
	size_t dirlen, namelen;
	int filelen, numskins = 0;

	// "<dir>/md5/tris.md5mesh" -> "<dir>/tris.md2"
	namelen = strlen(mod_name);
	if (namelen < 13 || Q_stricmp(mod_name + namelen - 8, ".md5mesh"))
		return 0;

	slash = strrchr(mod_name, '/');
	if (!slash || slash - mod_name < 4 || strncmp(slash - 4, "/md5", 4))
		return 0;

	dirlen = (size_t)(slash - mod_name) - 4 + 1;    // keep the '/' before "md5"
	if (dirlen + (namelen - 8 - (size_t)(slash + 1 - mod_name)) + 5 >= sizeof(md2_path))
		return 0;

	memcpy(md2_path, mod_name, dirlen);
	memcpy(md2_path + dirlen, slash + 1, namelen - 8 - (size_t)(slash + 1 - mod_name));
	strcpy(md2_path + dirlen + namelen - 8 - (size_t)(slash + 1 - mod_name), ".md2");

	filelen = FS_LoadFile(md2_path, (void **)&rawdata);
	if (!rawdata)
		return 0;

	if (filelen < (int)sizeof(header))
		goto done;

	LittleBlock(&header, rawdata, sizeof(header));

	if (MOD_ValidateMD2(&header, filelen))
		goto done;

	src_skin = (const char *)rawdata + header.ofs_skins;
	for (int i = 0; i < header.num_skins && i < MAX_ALIAS_SKINS; i++, src_skin += MD2_MAX_SKINNAME) {
		if (!Q_memccpy(skinname, src_skin, 0, sizeof(skinname)))
			break;

		FS_NormalizePath(skinname);
		MD5_SkinPath(base_path, skinname, mod_name, skinpath, sizeof(skinpath));

		mesh->materials[numskins] = MAT_Find(skinpath, IT_SKIN, IF_NONE);
		assert(mesh->materials[numskins]);

		// The re-unwrapped MD5 skins cannot use Q2RTX's hand-authored normal
		// and emissive maps, but the classic material's tuning is not tied to
		// a UV layout - without this the model renders at base_factor 1.0
		// against the 1.5-2.5 the classic skin asks for, i.e. darker than the
		// .md2 it replaces.
		MAT_InheritScalars(mesh->materials[numskins], skinname);

		numskins++;
	}

done:
	FS_FreeFile(rawdata);
	return numskins;
}

int MOD_LoadMD5_RTX(model_t *model, const void *rawdata, size_t length, const char *mod_name)
{
	char base_path[MAX_QPATH];
	int ret;

	Hunk_Begin(&model->hunk, 0x4000000);
	model->type = MOD_ALIAS;

	int res = MOD_LoadMD5_Base(model, rawdata, length, mod_name);

	if (res != Q_ERR_SUCCESS)
	{
		Hunk_Free(&model->hunk);
		return res;
	}

	COM_FilePath(mod_name, base_path, sizeof(base_path));

	CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * model->iqmData->num_meshes));
	model->nummeshes = (int)model->iqmData->num_meshes;
	model->numframes = 1; // baked frames, so the VBO uploader makes one copy of the vertices

	// every mesh of a model shares the one skin list, exactly as the md2's did
	int numskins = 0;
	char default_skin[MAX_QPATH];
	pbr_material_t *shared_materials[MAX_ALIAS_SKINS];
	maliasmesh_t probe = { 0 };

	numskins = MD5_LoadSkinsFromMD2(mod_name, base_path, &probe);
	if (numskins == 0)
	{
		// No readable .md2 to take the list from. The rerelease usually names
		// the lone skin of such a model "skin", but not always - the carried
		// CTF flags are players/male/md5/flag1.md5mesh + flag1.png - so try
		// the model's own name before falling back to that convention.
		const char *slash = strrchr(mod_name, '/');
		const char *stem = slash ? slash + 1 : mod_name;
		char name[MAX_QPATH], probe_path[MAX_QPATH];
		char *dot;

		Q_strlcpy(name, stem, sizeof(name));
		dot = strrchr(name, '.');
		if (dot)
			*dot = 0;

		Q_snprintf(probe_path, sizeof(probe_path), "%s/%s", base_path, name);

		if (MD5_SkinExists(probe_path))
			Q_snprintf(default_skin, sizeof(default_skin), "%s.png", probe_path);
		else if (!MD5_HeldWeaponSkin(mod_name, default_skin, sizeof(default_skin))
		         && !MD5_LoneSkinInDir(base_path, default_skin, sizeof(default_skin)))
			Q_snprintf(default_skin, sizeof(default_skin), "%s/skin.png", base_path);

		probe.materials[0] = MAT_Find(default_skin, IT_SKIN, IF_NONE);
		numskins = 1;
	}
	memcpy(shared_materials, probe.materials, sizeof(shared_materials));

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

// Three, because the realistic worst case is a couple of bodies being shot at
// once. A fourth would only ever be paid for.
#define MOD_TRACE_CACHE_SLOTS   3

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
    // Per triangle: its three vertex indices into the arrays above, and the
    // bounding box that rejects it.
    int         tris[MOD_TRACE_MAX_TRIS][3];
    vec3_t      tri_mins[MOD_TRACE_MAX_TRIS];
    vec3_t      tri_maxs[MOD_TRACE_MAX_TRIS];
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

        if (!mesh->positions || !mesh->normals || !mesh->indices)
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

        slot->numverts += mesh->numverts;

        for (int i = 0; i < mesh->numtris; i++) {
            const int t = slot->numtris + i;
            int j;

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
                       float *out_frac, vec3_t out_normal)
{
    mod_trace_pose_cache_t *c;
    vec3_t axis[3], local_start, local_end, dir, seg_mins, seg_maxs, normal;
    float scale, rcp_scale, best = 1.f;
    int best_tri = -1;
    float best_u = 0.f, best_v = 0.f;
    bool rotated;

    if (!model || model->type != MOD_ALIAS || model->nummeshes <= 0)
        return false;

    int frame = pose->frame;
    if (model->iqmData && model->iqmData->num_frames)
        frame = (int)((unsigned)frame % model->iqmData->num_frames);
    else if (model->numframes > 0)
        frame = (int)((unsigned)frame % (unsigned)model->numframes);
    else
        return false;

    c = mod_trace_get_pose(model, frame);
    if (!c)
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

    // Model -> world. Uniform scale, so the normal needs the rotation only.
    if (rotated) {
        TransposeAxis(axis);
        RotatePoint(normal, axis);
    }

    VectorCopy(normal, out_normal);
    *out_frac = best;
    return true;
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
