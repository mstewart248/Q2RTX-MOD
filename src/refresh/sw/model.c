/*
Copyright (C) 1997-2001 Id Software, Inc.

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

// models.c -- world setup and alias model loading

#include "sw.h"
#include "format/md2.h"
#if USE_MD3
#include "format/md3.h"
#endif

// lightmaps resampled for DECOUPLED_LM faces, all in one block per map
static byte     *r_lightdata;

// largest face, in texels, the surface cache can build at some mip level
#define MAX_FACE_EXTENT     (256 << (MIPLEVELS - 1))

/*
=================
ProcessTexinfo
=================
*/
static void ProcessTexinfo(bsp_t *bsp)
{
    mtexinfo_t *tex;
    int i;
    vec_t len1, len2;
    char name[MAX_QPATH];
    imageflags_t flags;

    tex = bsp->texinfo;
    for (i = 0; i < bsp->numtexinfo; i++, tex++) {
        len1 = VectorLength(tex->axis[0]);
        len2 = VectorLength(tex->axis[1]);
        len1 = (len1 + len2) / 2;
        if (len1 < 0.32)
            tex->mipadjust = 4;
        else if (len1 < 0.49)
            tex->mipadjust = 3;
        else if (len1 < 0.99)
            tex->mipadjust = 2;
        else
            tex->mipadjust = 1;

        if (tex->c.flags & (SURF_WARP | SURF_FLOWING))
            flags = IF_TURBULENT;
        else
            flags = IF_NONE;

        Q_concat(name, sizeof(name), "textures/", tex->name, ".wal");
        FS_NormalizePath(name);
        tex->image = IMG_Find(name, IT_WALL, flags);
    }
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void CalcSurfaceExtents(mface_t *s)
{
    vec_t   mins[2], maxs[2], val;
    int     i, j;
    msurfedge_t *e;
    mvertex_t   *v;
    mtexinfo_t  *tex;
    int     bmins[2], bmaxs[2];

    mins[0] = mins[1] = 999999;
    maxs[0] = maxs[1] = -999999;

    tex = s->texinfo;
    e = s->firstsurfedge;
    for (i = 0; i < s->numsurfedges; i++, e++) {
        v = e->edge->v[e->vert];
        for (j = 0; j < 2; j++) {
            // the same arithmetic GL and the light compilers use, which is
            // not the same as DotProduct() + offset in single precision
            val = (double)v->point[0] * tex->axis[j][0] +
                  (double)v->point[1] * tex->axis[j][1] +
                  (double)v->point[2] * tex->axis[j][2] +
                  (double)tex->offset[j];
            if (val < mins[j])
                mins[j] = val;
            if (val > maxs[j])
                maxs[j] = val;
        }
    }

    for (i = 0; i < 2; i++) {
        bmins[i] = floor(mins[i] / 16);
        bmaxs[i] = ceil(maxs[i] / 16);

        s->texturemins[i] = bmins[i] << 4;
        s->extents[i] = (bmaxs[i] - bmins[i]) << 4;
        if (s->extents[i] < 16)
            s->extents[i] = 16; // take at least one cache block
    }
}

/*
================
R_FaceDrawable

Whether the surface cache can build this face. Classic maps keep every face
within 256 texels, which is what the cache is laid out for; a face up to
MAX_FACE_EXTENT is built at a coarser mip, see D_SolidSurf.
================
*/
static bool R_FaceDrawable(const mface_t *s)
{
    return s->extents[0] <= MAX_FACE_EXTENT && s->extents[1] <= MAX_FACE_EXTENT &&
           SW_S_MAX(s) * SW_T_MAX(s) <= MAX_BLOCKLIGHTS;
}

/*
================
R_SampleDecoupled

One luxel of a DECOUPLED_LM lightmap at (u, v) in its own luxel units,
bilinear, clamped to the edge.
================
*/
static void R_SampleDecoupled(const mface_t *s, const byte *lightmap, float u, float v, byte *out)
{
    int     w = s->lm_width, h = s->lm_height;
    int     u0, v0, u1, v1, i;
    float   fu, fv;
    const byte  *p00, *p01, *p10, *p11;

    u = Q_clipf(u, 0, w - 1);
    v = Q_clipf(v, 0, h - 1);
    u0 = (int)u;
    v0 = (int)v;
    u1 = min(u0 + 1, w - 1);
    v1 = min(v0 + 1, h - 1);
    fu = u - u0;
    fv = v - v0;

    p00 = &lightmap[(v0 * w + u0) * LIGHTMAP_BYTES];
    p01 = &lightmap[(v0 * w + u1) * LIGHTMAP_BYTES];
    p10 = &lightmap[(v1 * w + u0) * LIGHTMAP_BYTES];
    p11 = &lightmap[(v1 * w + u1) * LIGHTMAP_BYTES];

    for (i = 0; i < LIGHTMAP_BYTES; i++) {
        float top = p00[i] + (p01[i] - p00[i]) * fu;
        float bot = p10[i] + (p11[i] - p10[i]) * fu;
        out[i] = (byte)Q_clipf(top + (bot - top) * fv + 0.5f, 0, 255);
    }
}

/*
================
R_ResampleLightmap

A DECOUPLED_LM face lights from a grid of its own, lm_axis taking a world
point to luxels. The surface cache can only light from the classic grid - a
luxel every 16 texels from texturemins - so this finds the world point under
each classic luxel and samples the face's own lightmap there, every style.
================
*/
static void R_ResampleLightmap(mface_t *s, byte *out)
{
    mtexinfo_t  *tex = s->texinfo;
    cplane_t    *plane = s->plane;
    int         smax = SW_S_MAX(s), tmax = SW_T_MAX(s);
    int         i, j, maps;
    double      m[3][3], inv[3][3], det;
    vec3_t      p;
    float       u, v;
    const byte  *src = s->lightmap;

    // rows: texture s, texture t, plane normal
    for (i = 0; i < 3; i++) {
        m[0][i] = tex->axis[0][i];
        m[1][i] = tex->axis[1][i];
        m[2][i] = plane->normal[i];
    }

    det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
          m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
          m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

    if (fabs(det) < 1e-8) {
        // texture axes along the normal: one colour for the whole face
        for (maps = 0; maps < s->numstyles; maps++) {
            for (i = 0; i < smax * tmax; i++)
                memcpy(out + i * LIGHTMAP_BYTES, src, LIGHTMAP_BYTES);
            out += smax * tmax * LIGHTMAP_BYTES;
            src += s->lm_width * s->lm_height * LIGHTMAP_BYTES;
        }
        return;
    }

    inv[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    inv[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) / det;
    inv[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    inv[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]) / det;
    inv[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    inv[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) / det;
    inv[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    inv[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) / det;
    inv[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;

    for (maps = 0; maps < s->numstyles; maps++) {
        for (j = 0; j < tmax; j++) {
            for (i = 0; i < smax; i++) {
                double rs = s->texturemins[0] + i * 16 - tex->offset[0];
                double rt = s->texturemins[1] + j * 16 - tex->offset[1];
                double rd = plane->dist;

                p[0] = inv[0][0] * rs + inv[0][1] * rt + inv[0][2] * rd;
                p[1] = inv[1][0] * rs + inv[1][1] * rt + inv[1][2] * rd;
                p[2] = inv[2][0] * rs + inv[2][1] * rt + inv[2][2] * rd;

                u = DotProduct(p, s->lm_axis[0]) + s->lm_axis[0][3];
                v = DotProduct(p, s->lm_axis[1]) + s->lm_axis[1][3];

                R_SampleDecoupled(s, src, u, v, out);
                out += LIGHTMAP_BYTES;
            }
        }
        src += s->lm_width * s->lm_height * LIGHTMAP_BYTES;
    }
}

/*
=================
ProcessFaces
=================
*/
static void ProcessFaces(bsp_t *bsp)
{
    mface_t     *s;
    int         i, j;
    size_t      size = 0;
    byte        *out;
    int         skipped = 0;

    for (i = 0; i < bsp->numedges; i++)
        bsp->edges[i].cachededgeoffset = 0;

    s = bsp->faces;
    for (i = 0; i < bsp->numfaces; i++, s++) {
        // GL leaves its surface flags in here, and the bsp_t can outlive
        // the renderer that set them
        s->drawflags &= DSURF_PLANEBACK;
        s->sw_lightmap = NULL;
        memset(s->cachespots, 0, sizeof(s->cachespots));

        // set the drawing flags
        if (s->texinfo->c.flags & SURF_SKY) {
            continue;
        }
        if (s->texinfo->c.flags & (SURF_WARP | SURF_FLOWING)) {
            s->drawflags |= DSURF_TURB;
            for (j = 0; j < 2; j++) {
                s->extents[j] = 16384;
                s->texturemins[j] = -8192;
            }
            continue;
        }
        CalcSurfaceExtents(s);

        if (!R_FaceDrawable(s)) {
            skipped++;
            continue;
        }

        if (s->lightmap && s->lm_width)
            size += SW_S_MAX(s) * SW_T_MAX(s) * LIGHTMAP_BYTES * s->numstyles;
        else
            s->sw_lightmap = s->lightmap;
    }

    if (skipped)
        Com_WPrintf("%s: %d faces too big for the software renderer\n", bsp->name, skipped);

    if (!size)
        return;

    out = r_lightdata = R_Malloc(size);

    s = bsp->faces;
    for (i = 0; i < bsp->numfaces; i++, s++) {
        if (!s->lightmap || !s->lm_width)
            continue;
        if (s->texinfo->c.flags & (SURF_SKY | SURF_WARP | SURF_FLOWING))
            continue;
        if (!R_FaceDrawable(s))
            continue;

        s->sw_lightmap = out;
        R_ResampleLightmap(s, out);
        out += SW_S_MAX(s) * SW_T_MAX(s) * LIGHTMAP_BYTES * s->numstyles;
    }

    Com_DPrintf("%s: %zu bytes of lightmap resampled\n", bsp->name, size);
}

/*
=================
R_ReleaseWorld

Leaves the bsp_t the way the loader made it: it is shared with the server
and outlives this renderer when another one starts.
=================
*/
static void R_ReleaseWorld(bsp_t *bsp)
{
    mface_t *s;
    int     i;

    for (i = 0, s = bsp->faces; i < bsp->numfaces; i++, s++) {
        s->drawflags &= DSURF_PLANEBACK;
        s->sw_lightmap = NULL;
        memset(s->cachespots, 0, sizeof(s->cachespots));
    }

    Z_Freep((void **)&r_lightdata);
}


/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

static byte *r_latlngtab;   // md3 latitude/longitude normal -> bytedirs index

static int R_NormalIndex(const vec3_t dir)
{
    int     i, best = 0;
    float   d, bestd = -999;

    for (i = 0; i < NUMVERTEXNORMALS; i++) {
        d = DotProduct(dir, bytedirs[i]);
        if (d > bestd) {
            bestd = d;
            best = i;
        }
    }

    return best;
}

static int R_LatLngIndex(const uint8_t norm[2])
{
    if (!r_latlngtab) {
        int     lat, lng;
        vec3_t  dir;

        r_latlngtab = R_Malloc(256 * 256);
        for (lat = 0; lat < 256; lat++) {
            for (lng = 0; lng < 256; lng++) {
                float a = lat * (M_PI * 2 / 255);
                float b = lng * (M_PI * 2 / 255);
                dir[0] = cos(b) * sin(a);
                dir[1] = sin(b) * sin(a);
                dir[2] = cos(a);
                r_latlngtab[lat * 256 + lng] = R_NormalIndex(dir);
            }
        }
    }

    return r_latlngtab[norm[0] * 256 + norm[1]];
}

/*
=================
MOD_LoadMD2
=================
*/
int MOD_LoadMD2_SW(model_t *model, const void *rawdata, size_t length, const char *mod_name)
{
    dmd2header_t    header;
    dmd2frame_t     *src_frame;
    dmd2trivertx_t  *src_vert;
    dmd2triangle_t  *src_tri;
    dmd2stvert_t    *src_st;
    maliasframe_t   *dst_frame;
    maliasvert_t    *dst_vert;
    maliastri_t     *dst_tri;
    maliasst_t      *dst_st;
    maliasmesh_t    *mesh;
    char            skinname[MAX_QPATH];
    char            *src_skin;
    int             i, j, k, numtris;
    size_t          size;
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

    size = sizeof(maliasmesh_t) +
           header.num_frames * sizeof(maliasframe_t) +
           header.num_tris * sizeof(maliastri_t) +
           header.num_st * sizeof(maliasst_t) +
           (size_t)header.num_frames * header.num_xyz * sizeof(maliasvert_t);

    Hunk_Begin(&model->hunk, size + 4096);
    model->type = MOD_ALIAS;
    model->nummeshes = 1;
    model->numframes = header.num_frames;
    CHECK(model->meshes = mesh = MOD_Malloc(sizeof(maliasmesh_t)));
    CHECK(model->frames = MOD_Malloc(header.num_frames * sizeof(maliasframe_t)));
    memset(mesh, 0, sizeof(*mesh));

    // load triangle indices, skipping the broken ones some models have
    CHECK(mesh->tris = MOD_Malloc(header.num_tris * sizeof(maliastri_t)));

    src_tri = (dmd2triangle_t *)((byte *)rawdata + header.ofs_tris);
    dst_tri = mesh->tris;
    numtris = 0;
    for (i = 0; i < header.num_tris; i++, src_tri++) {
        for (j = 0; j < 3; j++) {
            uint16_t idx_xyz = LittleShort(src_tri->index_xyz[j]);
            uint16_t idx_st = LittleShort(src_tri->index_st[j]);

            if (idx_xyz >= header.num_xyz || idx_st >= header.num_st)
                break;

            dst_tri->index_xyz[j] = idx_xyz;
            dst_tri->index_st[j] = idx_st;
        }
        if (j == 3) {
            dst_tri++;
            numtris++;
        }
    }
    mesh->numtris = numtris;

    if (numtris != header.num_tris) {
        Com_DPrintf("%s has %d bad triangles\n", mod_name, header.num_tris - numtris);
    }

    // load base s and t vertices
    CHECK(mesh->sts = MOD_Malloc(header.num_st * sizeof(maliasst_t)));
    mesh->numsts = header.num_st;

    src_st = (dmd2stvert_t *)((byte *)rawdata + header.ofs_st);
    dst_st = mesh->sts;
    for (i = 0; i < header.num_st; i++, src_st++, dst_st++) {
        dst_st->st[0] = (float)(int16_t)LittleShort(src_st->s) / header.skinwidth;
        dst_st->st[1] = (float)(int16_t)LittleShort(src_st->t) / header.skinheight;
    }

    // load the frames
    mesh->numverts = header.num_xyz;
    CHECK(mesh->verts = MOD_Malloc((size_t)header.num_frames * header.num_xyz * sizeof(maliasvert_t)));

    src_frame = (dmd2frame_t *)((byte *)rawdata + header.ofs_frames);
    dst_frame = model->frames;
    dst_vert = mesh->verts;
    for (i = 0; i < header.num_frames; i++, dst_frame++) {
        vec3_t mins, maxs;

        LittleVector(src_frame->scale, dst_frame->scale);
        LittleVector(src_frame->translate, dst_frame->translate);

        ClearBounds(mins, maxs);
        src_vert = src_frame->verts;
        for (j = 0; j < header.num_xyz; j++, src_vert++, dst_vert++) {
            for (k = 0; k < 3; k++) {
                dst_vert->v[k] = src_vert->v[k];
                mins[k] = min(mins[k], dst_vert->v[k]);
                maxs[k] = max(maxs[k], dst_vert->v[k]);
            }
            dst_vert->lightnormalindex = src_vert->lightnormalindex < NUMVERTEXNORMALS ?
                                         src_vert->lightnormalindex : 0;
            dst_vert->pad = 0;
        }

        VectorVectorScale(mins, dst_frame->scale, mins);
        VectorVectorScale(maxs, dst_frame->scale, maxs);
        dst_frame->radius = RadiusFromBounds(mins, maxs);
        VectorAdd(mins, dst_frame->translate, dst_frame->bounds[0]);
        VectorAdd(maxs, dst_frame->translate, dst_frame->bounds[1]);

        src_frame = (dmd2frame_t *)((byte *)src_frame + header.framesize);
    }

    // register all skins
    src_skin = (char *)rawdata + header.ofs_skins;
    for (i = 0; i < header.num_skins; i++) {
        if (!Q_memccpy(skinname, src_skin, 0, sizeof(skinname))) {
            ret = Q_ERR_STRING_TRUNCATED;
            goto fail;
        }
        FS_NormalizePath(skinname);
        mesh->skins[i] = IMG_Find(skinname, IT_SKIN, IF_NONE);
        src_skin += MD2_MAX_SKINNAME;
    }
    mesh->numskins = header.num_skins;

    Hunk_End(&model->hunk);
    return Q_ERR_SUCCESS;

fail:
    Hunk_Free(&model->hunk);
    return ret;
}

#if USE_MD3
static int MOD_LoadMD3Mesh(model_t *model, maliasmesh_t *mesh,
                           const byte *rawdata, size_t length, size_t *offset_p)
{
    dmd3mesh_t      header;
    size_t          end;
    dmd3vertex_t    *src_vert;
    dmd3coord_t     *src_tc;
    dmd3skin_t      *src_skin;
    uint32_t        *src_idx;
    maliasvert_t    *dst_vert;
    maliastri_t     *dst_tri;
    char            skinname[MAX_QPATH];
    int             i, j, k, ret;

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
    if (header.num_verts > MD3_MAX_VERTS)
        return Q_ERR_TOO_MANY;
    if (header.num_tris < 1)
        return Q_ERR_TOO_FEW;
    if (header.num_tris > MD3_MAX_TRIANGLES)
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

    memset(mesh, 0, sizeof(*mesh));
    mesh->numtris = header.num_tris;
    mesh->numverts = header.num_verts;
    mesh->numsts = header.num_verts;
    mesh->numskins = header.num_skins;
    CHECK(mesh->verts = MOD_Malloc(sizeof(maliasvert_t) * header.num_verts * model->numframes));
    CHECK(mesh->sts = MOD_Malloc(sizeof(maliasst_t) * header.num_verts));
    CHECK(mesh->tris = MOD_Malloc(sizeof(maliastri_t) * header.num_tris));

    // load all skins
    src_skin = (dmd3skin_t *)(rawdata + header.ofs_skins);
    for (i = 0; i < header.num_skins; i++, src_skin++) {
        if (!Q_memccpy(skinname, src_skin->name, 0, sizeof(skinname)))
            return Q_ERR_STRING_TRUNCATED;
        FS_NormalizePath(skinname);
        mesh->skins[i] = IMG_Find(skinname, IT_SKIN, IF_NONE);
    }

    // load all vertices
    src_vert = (dmd3vertex_t *)(rawdata + header.ofs_verts);
    dst_vert = mesh->verts;
    for (i = 0; i < model->numframes; i++) {
        maliasframe_t *f = &model->frames[i];

        for (j = 0; j < header.num_verts; j++) {
            for (k = 0; k < 3; k++) {
                dst_vert->v[k] = (int16_t)LittleShort(src_vert->point[k]);
                f->bounds[0][k] = min(f->bounds[0][k], dst_vert->v[k]);
                f->bounds[1][k] = max(f->bounds[1][k], dst_vert->v[k]);
            }
            dst_vert->lightnormalindex = R_LatLngIndex(src_vert->norm);
            dst_vert->pad = 0;

            src_vert++; dst_vert++;
        }
    }

    // load all texture coords
    src_tc = (dmd3coord_t *)(rawdata + header.ofs_tcs);
    for (i = 0; i < header.num_verts; i++, src_tc++) {
        mesh->sts[i].st[0] = LittleFloat(src_tc->st[0]);
        mesh->sts[i].st[1] = LittleFloat(src_tc->st[1]);
    }

    // load all triangle indices; md3 has one index for both
    src_idx = (uint32_t *)(rawdata + header.ofs_indexes);
    dst_tri = mesh->tris;
    for (i = 0; i < header.num_tris; i++, dst_tri++) {
        for (j = 0; j < 3; j++) {
            uint32_t index = LittleLong(*src_idx++);
            if (index >= header.num_verts)
                return Q_ERR_BAD_INDEX;
            dst_tri->index_xyz[j] = dst_tri->index_st[j] = index;
        }
    }

    *offset_p = header.meshsize;
    return Q_ERR_SUCCESS;

fail:
    return ret;
}

int MOD_LoadMD3_SW(model_t *model, const void *rawdata, size_t length, const char *mod_name)
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

    // no bigger than the file's own vertices, triangles and coords
    Hunk_Begin(&model->hunk, length * 2 + header.num_meshes * sizeof(maliasmesh_t) +
               header.num_frames * sizeof(maliasframe_t) + 4096);
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

    Hunk_End(&model->hunk);
    return Q_ERR_SUCCESS;

fail:
    Hunk_Free(&model->hunk);
    return ret;
}
#else
int MOD_LoadMD3_SW(model_t *model, const void *rawdata, size_t length, const char *mod_name)
{
    return Q_ERR_UNKNOWN_FORMAT;
}
#endif

void MOD_Reference_SW(model_t *model)
{
    int     i, j;

    // register any images used by the models
    switch (model->type) {
    case MOD_ALIAS:
        for (i = 0; i < model->nummeshes; i++) {
            maliasmesh_t *mesh = &model->meshes[i];
            for (j = 0; j < mesh->numskins; j++) {
                mesh->skins[j]->registration_sequence = registration_sequence;
            }
        }
        break;
    case MOD_SPRITE:
        for (i = 0; i < model->numframes; i++) {
            model->spriteframes[i].image->registration_sequence = registration_sequence;
        }
        break;
    case MOD_EMPTY:
        break;
    default:
        Com_Error(ERR_FATAL, "%s: bad model type", __func__);
    }

    model->registration_sequence = registration_sequence;
}

/*
@@@@@@@@@@@@@@@@@@@@@
R_BeginRegistration

Specifies the model that will be used as the world
@@@@@@@@@@@@@@@@@@@@@
*/
void R_BeginRegistration_SW(const char *model)
{
    char        fullname[MAX_QPATH];
    bsp_t       *bsp;
    int         ret;
    int         i;

    registration_sequence++;
    r_oldviewcluster = -1;      // force markleafs

    D_FlushCaches();

    Q_concat(fullname, sizeof(fullname), "maps/", model, ".bsp");
    ret = BSP_Load(fullname, &bsp);
    if (!bsp) {
        Com_Error(ERR_DROP, "%s: couldn't load %s: %s",
                  __func__, fullname, Q_ErrorString(ret));
    }

    if (bsp == r_worldmodel) {
        for (i = 0; i < bsp->numtexinfo; i++) {
            bsp->texinfo[i].image->registration_sequence = registration_sequence;
        }
        bsp->refcount--;
        return;
    }

    R_FreeMap();
    r_worldmodel = bsp;

    ProcessTexinfo(bsp);
    ProcessFaces(bsp);

    R_NewMap();
}

/*
@@@@@@@@@@@@@@@@@@@@@
R_EndRegistration

@@@@@@@@@@@@@@@@@@@@@
*/
void R_EndRegistration_SW(void)
{
    MOD_FreeUnused();
    IMG_FreeUnused();
}

/*
=================
R_FreeWorld

Called by R_FreeMap before the world's reference is dropped.
=================
*/
void R_FreeWorld(void)
{
    if (r_worldmodel)
        R_ReleaseWorld(r_worldmodel);
    Z_Freep((void **)&r_latlngtab);
}
