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

//
// model_md5.c -- loader for the rerelease's skeletal models
//
// The output is an iqm_model_t, because the renderer already knows how to
// skin one: R_ComputeIQMTransforms() turns model->poses into bone matrices
// and instance_geometry.comp applies them.  Everything here is about getting
// MD5 data into that shape.
//
// Three format facts drive the code below, all of them measured against the
// rerelease pak rather than assumed:
//
//   * the joints in a .md5mesh are ABSOLUTE, while the joints in a .md5anim
//     frame are relative to their parent.  That is exactly the split IQM
//     wants (bindJoints absolute, poses relative), so neither side needs
//     recomposing.
//   * a vertex carries an arbitrary number of weights, each with its own
//     position in ITS joint's space.  Every one of those maps to the same
//     model-space point, so summing them bias-weighted gives the bind
//     position that the IQM skinning path expects.  The rerelease uses up to
//     7 weights per vertex and only 4 survive.
//   * the .md5anim frame count matches the .md2 frame count exactly (198 of
//     the 199 rerelease models; the odd one has no .md2 at all), so
//     entity->frame indexes the animation with no remapping.
//

#include <assert.h>
#include <shared/shared.h>
#include <common/common.h>
#include <common/files.h>
#include <format/md5.h>
#include <refresh/models.h>
#include <refresh/refresh.h>

/*
=================================================================

  text parsing

  MD5 files are plain text and every brace and paren is whitespace
  separated, so COM_Parse is enough.  The parser is strict: anything
  unexpected fails the load and the caller falls back to the .md2.

=================================================================
*/

typedef struct {
    const char  *data;
    const char  *name;      // for error messages
    bool        error;
} md5_parse_t;

static const char *MD5_Token(md5_parse_t *p)
{
    const char *tok;

    if (p->error)
        return "";

    tok = COM_Parse(&p->data);
    if (!p->data && !*tok) {
        Com_DPrintf("%s: unexpected end of file\n", p->name);
        p->error = true;
    }
    return tok;
}

static bool MD5_Expect(md5_parse_t *p, const char *expect)
{
    const char *tok = MD5_Token(p);

    if (p->error)
        return false;
    if (strcmp(tok, expect)) {
        Com_DPrintf("%s: expected \"%s\", found \"%s\"\n", p->name, expect, tok);
        p->error = true;
        return false;
    }
    return true;
}

// consumes and returns true only if the next token matches
static bool MD5_Check(md5_parse_t *p, const char *expect)
{
    const char *saved = p->data;
    const char *tok;

    if (p->error)
        return false;

    tok = COM_Parse(&p->data);
    if (!strcmp(tok, expect))
        return true;

    p->data = saved;
    return false;
}

static float MD5_Float(md5_parse_t *p)
{
    return atof(MD5_Token(p));
}

static int MD5_Int(md5_parse_t *p)
{
    return atoi(MD5_Token(p));
}

// a count that has to be sane before it is used to size an allocation
static int MD5_Count(md5_parse_t *p, int limit)
{
    int val = MD5_Int(p);

    if (val < 0 || val > limit) {
        Com_DPrintf("%s: count %d out of range (limit %d)\n", p->name, val, limit);
        p->error = true;
        return 0;
    }
    return val;
}

/*
=================================================================

  math

  Kept byte-identical to model_iqm.c so that the matrices this
  produces compose correctly with R_ComputeIQMTransforms().

=================================================================
*/

static void MD5_JointToMatrix(const quat_t rot, const vec3_t trans, float *mat)
{
    float xx = 2.0f * rot[0] * rot[0];
    float yy = 2.0f * rot[1] * rot[1];
    float zz = 2.0f * rot[2] * rot[2];
    float xy = 2.0f * rot[0] * rot[1];
    float xz = 2.0f * rot[0] * rot[2];
    float yz = 2.0f * rot[1] * rot[2];
    float wx = 2.0f * rot[3] * rot[0];
    float wy = 2.0f * rot[3] * rot[1];
    float wz = 2.0f * rot[3] * rot[2];

    mat[0] = 1.0f - (yy + zz);  mat[1] = xy - wz;           mat[2] = xz + wy;           mat[3] = trans[0];
    mat[4] = xy + wz;           mat[5] = 1.0f - (xx + zz);  mat[6] = yz - wx;           mat[7] = trans[1];
    mat[8] = xz - wy;           mat[9] = yz + wx;           mat[10] = 1.0f - (xx + yy); mat[11] = trans[2];
}

static void MD5_Matrix34Invert(const float *inMat, float *outMat)
{
    float invSqrLen, *v;
    vec3_t trans;

    outMat[0] = inMat[0]; outMat[1] = inMat[4]; outMat[2] = inMat[8];
    outMat[4] = inMat[1]; outMat[5] = inMat[5]; outMat[6] = inMat[9];
    outMat[8] = inMat[2]; outMat[9] = inMat[6]; outMat[10] = inMat[10];

    v = outMat + 0; invSqrLen = 1.0f / DotProduct(v, v); VectorScale(v, invSqrLen, v);
    v = outMat + 4; invSqrLen = 1.0f / DotProduct(v, v); VectorScale(v, invSqrLen, v);
    v = outMat + 8; invSqrLen = 1.0f / DotProduct(v, v); VectorScale(v, invSqrLen, v);

    trans[0] = inMat[3];
    trans[1] = inMat[7];
    trans[2] = inMat[11];

    outMat[3] = -DotProduct(outMat + 0, trans);
    outMat[7] = -DotProduct(outMat + 4, trans);
    outMat[11] = -DotProduct(outMat + 8, trans);
}

static void MD5_TransformPoint(const float *mat, const vec3_t in, vec3_t out)
{
    out[0] = mat[0] * in[0] + mat[1] * in[1] + mat[2] * in[2] + mat[3];
    out[1] = mat[4] * in[0] + mat[5] * in[1] + mat[6] * in[2] + mat[7];
    out[2] = mat[8] * in[0] + mat[9] * in[1] + mat[10] * in[2] + mat[11];
}

// MD5 stores only the vector part; w is reconstructed and is always negative
static void MD5_ParseQuat(md5_parse_t *p, quat_t out)
{
    float t;

    MD5_Expect(p, "(");
    out[0] = MD5_Float(p);
    out[1] = MD5_Float(p);
    out[2] = MD5_Float(p);
    MD5_Expect(p, ")");

    t = 1.0f - (out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    out[3] = (t > 0.0f) ? -sqrtf(t) : 0.0f;
}

static void MD5_ParseVector(md5_parse_t *p, vec3_t out)
{
    MD5_Expect(p, "(");
    out[0] = MD5_Float(p);
    out[1] = MD5_Float(p);
    out[2] = MD5_Float(p);
    MD5_Expect(p, ")");
}

/*
=================================================================

  intermediate mesh storage

  The totals are not known until every mesh header has been read, so
  the per-mesh arrays are parsed into temporary memory first and only
  the packed result goes on the model hunk.

=================================================================
*/

#define MD5_MAX_MESHES      32
#define MD5_MAX_VERTS       65536
#define MD5_MAX_TRIS        65536
#define MD5_MAX_WEIGHTS     262144
#define MD5_MAX_FRAMES      4096

typedef struct {
    int     joint;
    float   bias;
    vec3_t  pos;
} md5_weight_t;

typedef struct {
    vec2_t  st;
    int     first_weight;
    int     num_weights;
} md5_vertex_t;

typedef struct {
    char            shader[MAX_QPATH];
    int             num_verts;
    int             num_tris;
    int             num_weights;
    md5_vertex_t    *verts;
    int             *indices;       // 3 per triangle, mesh local
    md5_weight_t    *weights;
} md5_mesh_t;

static void MD5_FreeMeshes(md5_mesh_t *meshes, int count)
{
    for (int i = 0; i < count; i++) {
        Z_Free(meshes[i].verts);
        Z_Free(meshes[i].indices);
        Z_Free(meshes[i].weights);
    }
}

/*
=================================================================

  .md5anim

  Fills iqmData->poses with num_frames * num_joints parent-relative
  transforms.  Returns false if the file is missing or unusable, in
  which case the model still loads as a static bind pose.

=================================================================
*/

typedef struct {
    int     parent;
    int     flags;
    int     offset;
} md5_hierarchy_t;

static bool MD5_LoadAnim(model_t *model, iqm_model_t *iqmData, const char *mesh_name,
                         const char *jointNames, int num_joints)
{
    char            anim_name[MAX_QPATH];
    char            *rawdata = NULL;
    md5_parse_t     p;
    md5_hierarchy_t *hierarchy = NULL;
    iqm_transform_t *baseframe = NULL;
    float           *raw = NULL;
    int             num_frames = 0, anim_joints = 0, num_components = 0;
    int             len;
    bool            success = false;

    Q_strlcpy(anim_name, mesh_name, sizeof(anim_name));
    len = strlen(anim_name);
    if (len < 8 || Q_stricmp(anim_name + len - 8, ".md5mesh"))
        return false;
    memcpy(anim_name + len - 8, ".md5anim", 9);

    if (FS_LoadFile(anim_name, (void **)&rawdata) < 0 || !rawdata)
        return false;

    p.data = rawdata;
    p.name = anim_name;
    p.error = false;

    MD5_Expect(&p, "MD5Version");
    MD5_Expect(&p, "10");
    if (MD5_Check(&p, "commandline"))
        MD5_Token(&p);

    MD5_Expect(&p, "numFrames");
    num_frames = MD5_Count(&p, MD5_MAX_FRAMES);
    MD5_Expect(&p, "numJoints");
    anim_joints = MD5_Count(&p, MD5_MAX_JOINTS);
    MD5_Expect(&p, "frameRate");
    MD5_Int(&p);
    MD5_Expect(&p, "numAnimatedComponents");
    num_components = MD5_Count(&p, MD5_MAX_JOINTS * 6);

    if (p.error)
        goto done;

    if (anim_joints != num_joints) {
        Com_DPrintf("%s: has %d joints, mesh has %d\n", anim_name, anim_joints, num_joints);
        goto done;
    }
    if (num_frames < 1)
        goto done;

    hierarchy = Z_Mallocz(sizeof(*hierarchy) * num_joints);
    baseframe = Z_Mallocz(sizeof(*baseframe) * num_joints);
    raw = Z_Mallocz(sizeof(*raw) * max(num_components, 1));

    MD5_Expect(&p, "hierarchy");
    MD5_Expect(&p, "{");
    for (int i = 0; i < num_joints; i++) {
        const char *name = MD5_Token(&p);

        // the .md5anim repeats the joint names; they have to line up with the
        // mesh or the parent indices below refer to the wrong bones
        if (strncmp(name, jointNames + i * MAX_QPATH, MAX_QPATH)) {
            Com_DPrintf("%s: joint %d is \"%s\", mesh has \"%s\"\n",
                        anim_name, i, name, jointNames + i * MAX_QPATH);
            p.error = true;
            goto done;
        }
        hierarchy[i].parent = MD5_Int(&p);
        hierarchy[i].flags = MD5_Int(&p);
        hierarchy[i].offset = MD5_Int(&p);

        if (hierarchy[i].parent >= i || hierarchy[i].parent < -1) {
            Com_DPrintf("%s: joint %d has out of order parent %d\n",
                        anim_name, i, hierarchy[i].parent);
            p.error = true;
            goto done;
        }
        if (hierarchy[i].offset < 0 || hierarchy[i].offset > num_components - 1) {
            // a joint with no animated components is legal and its offset is
            // never read, so only complain when a flag would actually use it
            if (hierarchy[i].flags) {
                Com_DPrintf("%s: joint %d has bad component offset %d\n",
                            anim_name, i, hierarchy[i].offset);
                p.error = true;
                goto done;
            }
        }
    }
    MD5_Expect(&p, "}");

    // bounds are per frame and the renderer does not use them
    if (MD5_Check(&p, "bounds")) {
        MD5_Expect(&p, "{");
        for (int i = 0; i < num_frames; i++) {
            vec3_t unused;
            MD5_ParseVector(&p, unused);
            MD5_ParseVector(&p, unused);
        }
        MD5_Expect(&p, "}");
    }

    MD5_Expect(&p, "baseframe");
    MD5_Expect(&p, "{");
    for (int i = 0; i < num_joints; i++) {
        MD5_ParseVector(&p, baseframe[i].translate);
        MD5_ParseQuat(&p, baseframe[i].rotate);
        VectorSet(baseframe[i].scale, 1.0f, 1.0f, 1.0f);
    }
    MD5_Expect(&p, "}");

    if (p.error)
        goto done;

    if (!(iqmData->poses = MOD_Malloc(sizeof(iqm_transform_t) * num_frames * num_joints)))
        goto done;
    iqmData->num_frames = num_frames;

    for (int f = 0; f < num_frames; f++) {
        iqm_transform_t *dst = iqmData->poses + f * num_joints;
        int index;

        MD5_Expect(&p, "frame");
        index = MD5_Int(&p);
        MD5_Expect(&p, "{");
        for (int i = 0; i < num_components; i++)
            raw[i] = MD5_Float(&p);
        MD5_Expect(&p, "}");

        if (p.error)
            goto done;
        if (index != f) {
            Com_DPrintf("%s: frame %d is out of order (%d)\n", anim_name, f, index);
            goto done;
        }

        for (int i = 0; i < num_joints; i++) {
            const float *r = raw + hierarchy[i].offset;
            int flags = hierarchy[i].flags;
            float t;

            VectorCopy(baseframe[i].translate, dst[i].translate);
            QuatCopy(baseframe[i].rotate, dst[i].rotate);
            VectorSet(dst[i].scale, 1.0f, 1.0f, 1.0f);

            if (flags & 1)  dst[i].translate[0] = *r++;
            if (flags & 2)  dst[i].translate[1] = *r++;
            if (flags & 4)  dst[i].translate[2] = *r++;
            if (flags & 8)  dst[i].rotate[0] = *r++;
            if (flags & 16) dst[i].rotate[1] = *r++;
            if (flags & 32) dst[i].rotate[2] = *r++;

            if (flags & 56) {
                t = 1.0f - (dst[i].rotate[0] * dst[i].rotate[0] +
                            dst[i].rotate[1] * dst[i].rotate[1] +
                            dst[i].rotate[2] * dst[i].rotate[2]);
                dst[i].rotate[3] = (t > 0.0f) ? -sqrtf(t) : 0.0f;
            }
        }
    }

    success = true;

done:
    if (!success) {
        // a partly parsed animation is worse than none: the pose array would
        // be allocated but full of garbage for every frame after the failure
        iqmData->poses = NULL;
        iqmData->num_frames = 0;
    }
    Z_Free(hierarchy);
    Z_Free(baseframe);
    Z_Free(raw);
    FS_FreeFile(rawdata);
    return success;
}

/*
=================================================================

  .md5scale

  A JSON-ish sidecar that scales named joints to zero on named
  frames, which is how the rerelease hides body parts for the gib
  and decapitation frames.  It is parsed leniently: a malformed file
  costs nothing but the effect.

=================================================================
*/

static void MD5_LoadScale(iqm_model_t *iqmData, const char *mesh_name,
                          const char *jointNames, int num_joints)
{
    char        scale_name[MAX_QPATH];
    char        *rawdata = NULL;
    md5_parse_t p;
    int         len, applied = 0;

    if (!iqmData->poses || !iqmData->num_frames)
        return;

    Q_strlcpy(scale_name, mesh_name, sizeof(scale_name));
    len = strlen(scale_name);
    if (len < 8 || len + 2 > (int)sizeof(scale_name))
        return;
    memcpy(scale_name + len - 8, ".md5scale", 10);

    if (FS_LoadFile(scale_name, (void **)&rawdata) < 0 || !rawdata)
        return;

    // COM_Parse does not split on ':' or ',', so strip the punctuation and
    // let the file read as an alternating stream of names and numbers
    for (char *c = rawdata; *c; c++) {
        if (*c == ':' || *c == ',' || *c == '{' || *c == '}')
            *c = ' ';
    }

    p.data = rawdata;
    p.name = scale_name;
    p.error = false;

    while (1) {
        const char *tok = COM_Parse(&p.data);
        int joint;

        if (!p.data && !*tok)
            break;

        // a joint name, followed by "<frame> <scale>" pairs
        for (joint = 0; joint < num_joints; joint++) {
            if (!strncmp(tok, jointNames + joint * MAX_QPATH, MAX_QPATH))
                break;
        }
        if (joint == num_joints)
            continue;

        while (1) {
            const char *saved = p.data;
            const char *a = COM_Parse(&p.data);
            const char *b;
            int frame;
            float scale;

            if (!p.data && !*a)
                break;
            if (!Q_isdigit(*a)) {
                p.data = saved;
                break;
            }
            frame = atoi(a);
            b = COM_Parse(&p.data);
            scale = atof(b);

            if (frame >= 0 && frame < (int)iqmData->num_frames) {
                iqm_transform_t *t = iqmData->poses + frame * num_joints + joint;
                VectorSet(t->scale, scale, scale, scale);
                applied++;
            }
        }
    }

    if (applied)
        Com_DPrintf("%s: scaled %d joint/frame pairs\n", scale_name, applied);

    FS_FreeFile(rawdata);
}

/*
=================================================================

  MOD_LoadMD5_Base

  Fills model->iqmData.  The caller owns the hunk and is responsible
  for turning iqmData into renderer meshes, exactly as it does for
  IQM.

=================================================================
*/

int MOD_LoadMD5_Base(model_t *model, const void *rawdata, size_t length, const char *mod_name)
{
    md5_parse_t     p;
    iqm_model_t     *iqmData = NULL;
    md5_mesh_t      meshes[MD5_MAX_MESHES];
    float           *bindJoints = NULL;
    int             num_joints = 0, num_meshes = 0;
    int             total_verts = 0, total_tris = 0;
    int             parsed_meshes = 0;
    int             ret = Q_ERR_INVALID_FORMAT;

    memset(meshes, 0, sizeof(meshes));

    (void)length;   // FS_LoadFile NUL terminates, and the parser stops there

    p.data = (const char *)rawdata;
    p.name = mod_name;
    p.error = false;

    MD5_Expect(&p, "MD5Version");
    MD5_Expect(&p, "10");
    if (MD5_Check(&p, "commandline"))
        MD5_Token(&p);

    MD5_Expect(&p, "numJoints");
    num_joints = MD5_Count(&p, MD5_MAX_JOINTS);
    MD5_Expect(&p, "numMeshes");
    num_meshes = MD5_Count(&p, MD5_MAX_MESHES);

    if (p.error)
        goto fail;
    if (num_joints < 1 || num_meshes < 1) {
        Com_DPrintf("%s: %d joints, %d meshes\n", mod_name, num_joints, num_meshes);
        goto fail;
    }

    if (!(iqmData = MOD_Malloc(sizeof(iqm_model_t)))) {
        ret = Q_ERR(ENOMEM);
        goto fail;
    }
    memset(iqmData, 0, sizeof(*iqmData));

    iqmData->num_joints = num_joints;
    iqmData->num_poses = num_joints;

    if (!(iqmData->jointNames = MOD_Malloc(num_joints * MAX_QPATH)) ||
        !(iqmData->jointParents = MOD_Malloc(num_joints * sizeof(int))) ||
        !(iqmData->bindJoints = MOD_Malloc(num_joints * 12 * sizeof(float))) ||
        !(iqmData->invBindJoints = MOD_Malloc(num_joints * 12 * sizeof(float)))) {
        ret = Q_ERR(ENOMEM);
        goto fail;
    }
    memset(iqmData->jointNames, 0, num_joints * MAX_QPATH);
    bindJoints = iqmData->bindJoints;

    // joints. these are already absolute, so the bind matrix is direct
    MD5_Expect(&p, "joints");
    MD5_Expect(&p, "{");
    for (int i = 0; i < num_joints; i++) {
        vec3_t pos;
        quat_t rot;
        int parent;

        Q_strlcpy(iqmData->jointNames + i * MAX_QPATH, MD5_Token(&p), MAX_QPATH);
        parent = MD5_Int(&p);
        MD5_ParseVector(&p, pos);
        MD5_ParseQuat(&p, rot);

        if (p.error)
            goto fail;
        if (parent >= i || parent < -1) {
            Com_DPrintf("%s: joint %d has out of order parent %d\n", mod_name, i, parent);
            goto fail;
        }
        iqmData->jointParents[i] = parent;

        MD5_JointToMatrix(rot, pos, bindJoints + i * 12);
        MD5_Matrix34Invert(bindJoints + i * 12, iqmData->invBindJoints + i * 12);
    }
    MD5_Expect(&p, "}");

    if (p.error)
        goto fail;

    // meshes
    for (int m = 0; m < num_meshes; m++) {
        md5_mesh_t *mesh = &meshes[m];

        MD5_Expect(&p, "mesh");
        MD5_Expect(&p, "{");
        if (p.error)
            goto fail;

        if (MD5_Check(&p, "shader"))
            Q_strlcpy(mesh->shader, MD5_Token(&p), sizeof(mesh->shader));

        MD5_Expect(&p, "numverts");
        mesh->num_verts = MD5_Count(&p, MD5_MAX_VERTS);
        if (p.error)
            goto fail;
        mesh->verts = Z_Mallocz(sizeof(md5_vertex_t) * max(mesh->num_verts, 1));
        parsed_meshes = m + 1;

        for (int i = 0; i < mesh->num_verts; i++) {
            int index;

            MD5_Expect(&p, "vert");
            index = MD5_Int(&p);
            if (index < 0 || index >= mesh->num_verts) {
                Com_DPrintf("%s: vertex index %d out of range\n", mod_name, index);
                goto fail;
            }
            MD5_Expect(&p, "(");
            mesh->verts[index].st[0] = MD5_Float(&p);
            // t is already in image space, origin at the top - do NOT flip it.
            // Verified against the rerelease's own _glow maps: with t as-is the
            // 17 emissive blobs on the soldier land on Chest, both arms and
            // L_Hand, each with a single clean owner; flipped, they scatter
            // onto feet and shins.
            mesh->verts[index].st[1] = MD5_Float(&p);
            MD5_Expect(&p, ")");
            mesh->verts[index].first_weight = MD5_Int(&p);
            mesh->verts[index].num_weights = MD5_Int(&p);
            if (p.error)
                goto fail;
        }

        MD5_Expect(&p, "numtris");
        mesh->num_tris = MD5_Count(&p, MD5_MAX_TRIS);
        if (p.error)
            goto fail;
        mesh->indices = Z_Mallocz(sizeof(int) * 3 * max(mesh->num_tris, 1));

        for (int i = 0; i < mesh->num_tris; i++) {
            int index;

            MD5_Expect(&p, "tri");
            index = MD5_Int(&p);
            if (index < 0 || index >= mesh->num_tris) {
                Com_DPrintf("%s: triangle index %d out of range\n", mod_name, index);
                goto fail;
            }
            for (int j = 0; j < 3; j++) {
                int v = MD5_Int(&p);
                if (v < 0 || v >= mesh->num_verts) {
                    Com_DPrintf("%s: triangle %d references vertex %d\n", mod_name, index, v);
                    goto fail;
                }
                mesh->indices[index * 3 + j] = v;
            }
            if (p.error)
                goto fail;
        }

        MD5_Expect(&p, "numweights");
        mesh->num_weights = MD5_Count(&p, MD5_MAX_WEIGHTS);
        if (p.error)
            goto fail;
        mesh->weights = Z_Mallocz(sizeof(md5_weight_t) * max(mesh->num_weights, 1));

        for (int i = 0; i < mesh->num_weights; i++) {
            int index;

            MD5_Expect(&p, "weight");
            index = MD5_Int(&p);
            if (index < 0 || index >= mesh->num_weights) {
                Com_DPrintf("%s: weight index %d out of range\n", mod_name, index);
                goto fail;
            }
            mesh->weights[index].joint = MD5_Int(&p);
            mesh->weights[index].bias = MD5_Float(&p);
            MD5_ParseVector(&p, mesh->weights[index].pos);

            if (mesh->weights[index].joint < 0 || mesh->weights[index].joint >= num_joints) {
                Com_DPrintf("%s: weight %d references joint %d\n",
                            mod_name, index, mesh->weights[index].joint);
                goto fail;
            }
            if (p.error)
                goto fail;
        }

        MD5_Expect(&p, "}");
        if (p.error)
            goto fail;

        total_verts += mesh->num_verts;
        total_tris += mesh->num_tris;
    }

    if (total_verts < 3 || total_tris < 1) {
        Com_DPrintf("%s: %d verts, %d tris\n", mod_name, total_verts, total_tris);
        ret = Q_ERR_TOO_FEW;
        goto fail;
    }

    // pack into the iqm arrays
    iqmData->num_vertexes = total_verts;
    iqmData->num_triangles = total_tris;
    iqmData->num_meshes = num_meshes;
    iqmData->num_frames = 0;

    if (!(iqmData->meshes = MOD_Malloc(sizeof(iqm_mesh_t) * num_meshes)) ||
        !(iqmData->indices = MOD_Malloc(sizeof(uint32_t) * 3 * total_tris)) ||
        !(iqmData->positions = MOD_Malloc(sizeof(float) * 3 * total_verts)) ||
        !(iqmData->normals = MOD_Malloc(sizeof(float) * 3 * total_verts)) ||
        !(iqmData->texcoords = MOD_Malloc(sizeof(float) * 2 * total_verts)) ||
        !(iqmData->blend_indices = MOD_Malloc(4 * total_verts)) ||
        !(iqmData->blend_weights = MOD_Malloc(4 * total_verts))) {
        ret = Q_ERR(ENOMEM);
        goto fail;
    }
    memset(iqmData->normals, 0, sizeof(float) * 3 * total_verts);
    memset(iqmData->blend_indices, 0, 4 * total_verts);
    memset(iqmData->blend_weights, 0, 4 * total_verts);

    {
        int first_vertex = 0, first_triangle = 0;

        for (int m = 0; m < num_meshes; m++) {
            md5_mesh_t *mesh = &meshes[m];
            iqm_mesh_t *dst = &iqmData->meshes[m];

            memset(dst, 0, sizeof(*dst));
            Q_strlcpy(dst->name, mesh->shader, sizeof(dst->name));
            Q_strlcpy(dst->material, mesh->shader, sizeof(dst->material));
            dst->data = iqmData;
            dst->first_vertex = first_vertex;
            dst->num_vertexes = mesh->num_verts;
            dst->first_triangle = first_triangle;
            dst->num_triangles = mesh->num_tris;

            for (int i = 0; i < mesh->num_verts; i++) {
                const md5_vertex_t *v = &mesh->verts[i];
                float *pos = iqmData->positions + (first_vertex + i) * 3;
                float *tc = iqmData->texcoords + (first_vertex + i) * 2;
                byte *idx = iqmData->blend_indices + (first_vertex + i) * 4;
                byte *wgt = iqmData->blend_weights + (first_vertex + i) * 4;
                float best[MD5_MAX_INFLUENCES] = { 0 };
                int total, sum, largest;

                tc[0] = v->st[0];
                tc[1] = v->st[1];

                VectorClear(pos);

                if (v->first_weight < 0 || v->num_weights < 0 ||
                    v->first_weight + v->num_weights > mesh->num_weights) {
                    Com_DPrintf("%s: vertex %d has weights %d..%d of %d\n", mod_name, i,
                                v->first_weight, v->first_weight + v->num_weights,
                                mesh->num_weights);
                    goto fail;
                }

                for (int w = 0; w < v->num_weights; w++) {
                    const md5_weight_t *weight = &mesh->weights[v->first_weight + w];
                    vec3_t p;

                    // every weight of a vertex maps to the same model space
                    // point, so this sums to the bind position
                    MD5_TransformPoint(bindJoints + weight->joint * 12, weight->pos, p);
                    VectorMA(pos, weight->bias, p, pos);

                    // keep the MD5_MAX_INFLUENCES strongest
                    if (w < MD5_MAX_INFLUENCES) {
                        best[w] = weight->bias;
                        idx[w] = weight->joint;
                    } else {
                        int lowest = 0;
                        for (int k = 1; k < MD5_MAX_INFLUENCES; k++) {
                            if (best[k] < best[lowest])
                                lowest = k;
                        }
                        if (best[lowest] < weight->bias) {
                            best[lowest] = weight->bias;
                            idx[lowest] = weight->joint;
                        }
                    }
                }

                // quantize to bytes, renormalizing for any weights dropped
                // above, and give the rounding error to the largest one so
                // that the shader's weight_sum stays exactly 255
                total = 0;
                for (int k = 0; k < MD5_MAX_INFLUENCES; k++)
                    total += (int)(best[k] * 1024.0f);

                sum = 0;
                largest = 0;
                for (int k = 0; k < MD5_MAX_INFLUENCES; k++) {
                    int q = total > 0 ? (int)(best[k] * 1024.0f) * 255 / total : 0;
                    wgt[k] = (byte)q;
                    sum += q;
                    if (best[k] > best[largest])
                        largest = k;
                }
                if (sum == 0) {
                    // a vertex with no usable weights follows its first joint
                    wgt[0] = 255;
                } else if (sum < 255) {
                    wgt[largest] += (byte)(255 - sum);
                }
            }

            for (int i = 0; i < mesh->num_tris * 3; i++)
                iqmData->indices[first_triangle * 3 + i] = first_vertex + mesh->indices[i];

            // MD5 has no normals. Derive them from the bind pose with the same
            // handedness MOD_LoadMD2_RTX uses, so that the winding fix the
            // renderer applies afterwards leaves them facing outward.
            for (int t = 0; t < mesh->num_tris; t++) {
                const int *tri = mesh->indices + t * 3;
                const float *p0 = iqmData->positions + (first_vertex + tri[0]) * 3;
                const float *p1 = iqmData->positions + (first_vertex + tri[1]) * 3;
                const float *p2 = iqmData->positions + (first_vertex + tri[2]) * 3;
                vec3_t d1, d2, n;

                VectorSubtract(p1, p0, d1);
                VectorSubtract(p2, p0, d2);
                CrossProduct(d2, d1, n);
                VectorNormalize(n);

                for (int k = 0; k < 3; k++) {
                    float *dstn = iqmData->normals + (first_vertex + tri[k]) * 3;
                    VectorAdd(dstn, n, dstn);
                }
            }

            first_vertex += mesh->num_verts;
            first_triangle += mesh->num_tris;
        }

        for (int i = 0; i < total_verts; i++)
            VectorNormalize(iqmData->normals + i * 3);
    }

    model->iqmData = iqmData;

    // animation is optional: without it the model still draws its bind pose
    if (MD5_LoadAnim(model, iqmData, mod_name, iqmData->jointNames, num_joints))
        MD5_LoadScale(iqmData, mod_name, iqmData->jointNames, num_joints);
    else
        Com_DPrintf("%s: no usable .md5anim, using bind pose\n", mod_name);

    MD5_FreeMeshes(meshes, parsed_meshes);
    return Q_ERR_SUCCESS;

fail:
    MD5_FreeMeshes(meshes, parsed_meshes);
    model->iqmData = NULL;
    return ret;
}
