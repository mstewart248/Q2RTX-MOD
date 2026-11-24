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
#include <format/md2.h>
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

static void MD5_Matrix34Multiply(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 3; r++) {
        const float *ar = a + r * 4;
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = ar[0] * b[c] + ar[1] * b[4 + c] + ar[2] * b[8 + c] + (c == 3 ? ar[3] : 0.0f);
    }
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

    // bounds are per frame. The path tracers do not use them; GL and software
    // cull by them, in iqmData->bounds as an IQM's (mins, maxs per frame)
    if (MD5_Check(&p, "bounds")) {
        float *bounds = MOD_Malloc(sizeof(float) * 6 * num_frames);

        MD5_Expect(&p, "{");
        for (int i = 0; i < num_frames; i++) {
            vec3_t mins, maxs;
            MD5_ParseVector(&p, mins);
            MD5_ParseVector(&p, maxs);
            if (bounds) {
                VectorCopy(mins, bounds + i * 6);
                VectorCopy(maxs, bounds + i * 6 + 3);
            }
        }
        MD5_Expect(&p, "}");
        iqmData->bounds = bounds;
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
        iqmData->bounds = NULL;
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

  MD5_WeldSeamNormals

  A .md5mesh stores one vertex per (position, texcoord) pair, so every
  point a texture seam runs through appears two or more times, each copy
  with its own vertex normal.  Accumulating face normals per vertex INDEX
  therefore gives each copy only the faces on its own side of the cut, and
  the model lights with a hard crease along every UV boundary - on the
  rerelease soldier that is 46% of the vertices, up to 81 degrees apart,
  and the worst of it is on the helmet.

  Nothing downstream can undo it: tangents are supposed to split at a seam
  and the shader interpolates whatever normals it is handed.  So weld the
  accumulated (still unnormalized) normals by bind position first.  The
  duplicates are exact copies of the same weight list, so they land on
  bit-identical positions; the quantization below only exists so that an
  exporter emitting near-duplicates still welds.

=================================================================
*/

#define MD5_WELD_GRID   1024.0f     // weld tolerance is one part in this

typedef struct {
    int32_t     key[3];
    int         vert;
} md5_weld_entry_t;

static void MD5_WeldSeamNormals(iqm_model_t *iqmData, int total_verts)
{
    md5_weld_entry_t    *table;
    int                 *head;
    uint32_t            size = 16;

    while (size < (uint32_t)total_verts * 2)
        size <<= 1;

    table = Z_Mallocz(sizeof(*table) * size);
    head = Z_Mallocz(sizeof(*head) * total_verts);

    for (uint32_t i = 0; i < size; i++)
        table[i].vert = -1;

    // first pass: find the representative vertex of each point in space and
    // fold every duplicate's normal into it
    for (int i = 0; i < total_verts; i++) {
        const float *pos = iqmData->positions + i * 3;
        int32_t key[3];
        uint32_t h = 2166136261u;

        for (int k = 0; k < 3; k++) {
            key[k] = (int32_t)floorf(pos[k] * MD5_WELD_GRID + 0.5f);
            h = (h ^ (uint32_t)key[k]) * 16777619u;
        }

        for (uint32_t probe = h & (size - 1); ; probe = (probe + 1) & (size - 1)) {
            md5_weld_entry_t *e = &table[probe];

            if (e->vert < 0) {
                e->key[0] = key[0];
                e->key[1] = key[1];
                e->key[2] = key[2];
                e->vert = i;
                head[i] = i;
                break;
            }
            if (e->key[0] == key[0] && e->key[1] == key[1] && e->key[2] == key[2]) {
                float *dst = iqmData->normals + e->vert * 3;

                head[i] = e->vert;
                VectorAdd(dst, iqmData->normals + i * 3, dst);
                break;
            }
        }
    }

    // second pass: hand the welded normal back to every duplicate.  A single
    // forward walk is enough because a representative is always the lowest
    // index of its group, so it is already normalized when a duplicate reads it
    for (int i = 0; i < total_verts; i++) {
        float *dst = iqmData->normals + i * 3;

        if (head[i] == i)
            VectorNormalize(dst);
        else
            VectorCopy(iqmData->normals + head[i] * 3, dst);
    }

    Z_Free(table);
    Z_Free(head);
}

/*
=================================================================

  MOD_LoadMD5_Base

  Fills model->iqmData.  The caller owns the hunk and is responsible
  for turning iqmData into renderer meshes, exactly as it does for
  IQM.

=================================================================
*/

static vec3_t   md5_load_offset;
static bool     md5_load_offset_set;
static float    md5_load_scale = 1.0f;

void MD5_SetLoadOffset(const vec_t *offset)
{
    md5_load_offset_set = offset && (offset[0] || offset[1] || offset[2]);
    if (md5_load_offset_set)
        VectorCopy(offset, md5_load_offset);
    else
        VectorClear(md5_load_offset);
}

void MD5_SetLoadScale(float scale)
{
    md5_load_scale = scale > 0.0f ? scale : 1.0f;
}

/*
Scales a loaded model by (s) about its origin: the bind positions, and every
joint's translation in the bind and in each frame. Skinning is (pose x inverse
bind) x position, and a uniform scale commutes with the rotations, so the
whole model - at rest or animated - comes out (s) times as big. Normals keep.
Quake 1's rerelease box meshes are 2/3 of the boxes they stand in for (KEX
draws them 1.5 times up: shellssmall is 15 units across, b_shell0 22).
*/
static void MD5_ApplyScale(iqm_model_t *iqmData, int num_joints, float s)
{
    for (int j = 0; j < num_joints; j++) {
        float *m = iqmData->bindJoints + j * 12;
        m[3] *= s;
        m[7] *= s;
        m[11] *= s;
        MD5_Matrix34Invert(m, iqmData->invBindJoints + j * 12);
    }

    for (uint32_t v = 0; v < iqmData->num_vertexes * 3; v++)
        iqmData->positions[v] *= s;

    if (iqmData->poses) {
        for (uint32_t f = 0; f < iqmData->num_frames; f++)
            for (int j = 0; j < num_joints; j++)
                VectorScale(iqmData->poses[f * num_joints + j].translate, s,
                            iqmData->poses[f * num_joints + j].translate);
    }

    iqmData->bounds = NULL;
}

/*
Moves a loaded model by (o): the bind positions, the bind joints and the root
joints of every frame. Moving the vertices alone is not enough - skinning
puts each through its joint's (pose x inverse bind), and a frame that turns
the root would turn the offset with it. Moved together, pose and bind cancel
as before and the whole model is (o) away. The .md5anim's bounds were not
moved and, for the boxes this is for, are wrong anyway (a few units across);
GL and software cull by the bind pose instead (MD5_FrameBounds).
*/
static void MD5_ApplyOffset(iqm_model_t *iqmData, int num_joints, const vec3_t o)
{
    for (int j = 0; j < num_joints; j++) {
        float *m = iqmData->bindJoints + j * 12;
        m[3] += o[0];
        m[7] += o[1];
        m[11] += o[2];
        MD5_Matrix34Invert(m, iqmData->invBindJoints + j * 12);
    }

    for (uint32_t v = 0; v < iqmData->num_vertexes; v++)
        VectorAdd(iqmData->positions + v * 3, o, iqmData->positions + v * 3);

    if (iqmData->poses) {
        for (uint32_t f = 0; f < iqmData->num_frames; f++) {
            for (int j = 0; j < num_joints; j++) {
                if (iqmData->jointParents[j] < 0)
                    VectorAdd(iqmData->poses[f * num_joints + j].translate, o,
                              iqmData->poses[f * num_joints + j].translate);
            }
        }
    }

    iqmData->bounds = NULL;
}

/* A MUZZLE FLASH JOINT. The Quake rerelease's md5s carry the flash on a joint of
   its own - "flame" (v_nail2, v_shot, v_rock, soldier, enforcer), "flame_1".."_3"
   (v_nail, v_rock2), "small_flame"/"big_flame" (player) - that JUMPS: parked
   behind or inside the gun on frames with no flash, at one barrel or another on
   those with one. Blended, v_nail2's slid between its barrels and up out of the
   gun's body (Matt, 2026-11-21: "it should be right barrel left barrel and it's
   moving the muzzle flashes in between"). Such a joint takes the nearer frame's
   pose whole (R_ComputeIQMTransforms) - what Quake, which never blended, drew.

   v_light's arcs are the same kind of joint: "bolt_1".."bolt_4", one pair of
   arcs (electrode to barrel to electrode) each, parked 20-37 units behind the
   gun and out at the muzzle on firing frame 1..4 in turn (v_light.mdl did it
   with 16 vertices hidden inside the gun on its idle frame). Blended, the one
   going out and the one coming in both slid through the inside of the gun for
   most of each 10 Hz frame - firing showed the bolt and no arcs (Matt,
   2026-11-23: "there should be two small archs between the electrodes on the
   gun when firing"). No other id1/add-on md5 has a "bolt_" joint. */
static bool md5_snap_joint(const char *name)
{
    const size_t len = strlen(name);
    if (!Q_stricmp(name, "flame") || !Q_stricmpn(name, "flame_", 6))
        return true;
    if (!Q_stricmpn(name, "bolt_", 5))
        return true;
    return len > 6 && !Q_stricmp(name + len - 6, "_flame");
}

/* A MUZZLE FLASH RIDES ITS BARREL. Snapping a flash joint to the nearer frame
   keeps it off the blend between where it is parked and the muzzle, but the
   barrels it sits on still blend: v_nail2's four turn 45 degrees a frame and its
   one "flame" joint (parented to the root, not to a barrel) is put on a barrel
   of each frame, so snapped it sat up to 22.5 degrees round from that barrel -
   between two of them (Matt, 2026-11-21: "the muzzle flash on the chain gun seem
   to have the same problem as the nail gun"). v_nail's recoiling barrels left
   their flashes off the muzzles the same way.

   So for each frame a flash joint is tied to the joint it sits on there, and
   R_ComputeIQMTransforms places it by that joint's BLENDED pose at this frame's
   offset from it. "Sits on" = nearest ACROSS the model's +x, the way a Quake
   model faces, the whole distance only a tie-break: v_nail2's aligned barrel is
   0.2 across (the body 3.6, the next barrel 4.8); v_nail's right barrel 0.4
   where the nearer "middlestick" is 4.1. A flash with joints below it only
   snaps, as before.

   A PARKED FLASH. On the frames with no flash the joint is put away BEHIND the
   gun - behind the eye or under the gun, out of the view: v_nail2 24 units
   back, v_rock 35 back, v_nail's three flames 10 back. A rasterizer never
   sees it there; the path tracer does - an emissive mesh lit the gun from
   behind whenever it was not firing (Matt, 2026-11-23: nailgun, super
   nailgun, grenade launcher), and showed up in mirrors and water. So a frame
   where the flash sits more than MD5_FLASH_PARKED_BEHIND units behind its
   rider along +x is marked parked. Measured on every rerelease md5 with a flash
   joint: parked 8..44 units behind, at the muzzle 11..52 in front (v_light's
   arcs: 22..40 behind the "weapon" joint, 24 in front). The player,
   soldier and enforcer keep theirs inside the torso (within 2 units of
   Spine1 / chest along x), so they never count as parked. */
#define MD5_FLASH_PARKED_BEHIND 4.0f

static void MD5_SnapRiders(model_t *model, iqm_model_t *iqmData, int num_joints)
{
    if (!iqmData->jointSnap || !iqmData->poses || !iqmData->num_frames)
        return;

    const int num_frames = (int)iqmData->num_frames;
    short *rider = MOD_Malloc(sizeof(short) * num_frames * num_joints);
    float *offset = MOD_Malloc(sizeof(float) * 12 * num_frames * num_joints);
    byte *parked = MOD_Malloc((size_t)num_frames * num_joints);
    float *abs_m = Z_Malloc(sizeof(float) * 12 * num_joints);
    if (!rider || !offset || !parked) {
        Z_Free(abs_m);
        return;
    }
    memset(parked, 0, (size_t)num_frames * num_joints);
    bool any_parked = false;

    for (int f = 0; f < num_frames; f++) {
        const iqm_transform_t *pose = iqmData->poses + f * num_joints;

        // this frame's absolute joints, as R_ComputeIQMTransforms composes them
        for (int j = 0; j < num_joints; j++) {
            float local[12];
            MD5_JointToMatrix(pose[j].rotate, pose[j].translate, local);
            if (iqmData->jointParents[j] >= 0)
                MD5_Matrix34Multiply(abs_m + iqmData->jointParents[j] * 12, local, abs_m + j * 12);
            else
                memcpy(abs_m + j * 12, local, sizeof(local));
        }

        for (int s = 0; s < num_joints; s++) {
            short *out = rider + f * num_joints + s;
            *out = -1;
            if (!iqmData->jointSnap[s])
                continue;

            bool has_children = false;
            for (int c = 0; c < num_joints; c++)
                if (iqmData->jointParents[c] == s)
                    has_children = true;
            if (has_children)
                continue;

            const float *ps = abs_m + s * 12;
            float best = 1e30f;
            for (int r = 0; r < num_joints; r++) {
                if (iqmData->jointSnap[r])
                    continue;
                const float *pr = abs_m + r * 12;
                const float dx = ps[3] - pr[3], dy = ps[7] - pr[7], dz = ps[11] - pr[11];
                const float across = sqrtf(dy * dy + dz * dz);
                const float score = across + 0.1f * sqrtf(dx * dx + dy * dy + dz * dz);
                if (score < best) {
                    best = score;
                    *out = (short)r;
                }
            }
            if (*out < 0)
                continue;

            float inv_rider[12];
            MD5_Matrix34Invert(abs_m + *out * 12, inv_rider);
            MD5_Matrix34Multiply(inv_rider, ps, offset + ((size_t)f * num_joints + s) * 12);

            if (ps[3] - abs_m[*out * 12 + 3] < -MD5_FLASH_PARKED_BEHIND) {
                parked[(size_t)f * num_joints + s] = 1;
                any_parked = true;
            }
        }
    }

    Z_Free(abs_m);
    iqmData->jointSnapRider = rider;
    iqmData->jointSnapOffset = offset;
    iqmData->jointSnapParked = any_parked ? parked : NULL;
}

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

        if (md5_snap_joint(iqmData->jointNames + i * MAX_QPATH)) {
            if (!iqmData->jointSnap) {
                if (!(iqmData->jointSnap = MOD_Malloc(num_joints))) {
                    ret = Q_ERR(ENOMEM);
                    goto fail;
                }
                memset(iqmData->jointSnap, 0, num_joints);
            }
            iqmData->jointSnap[i] = 1;
        }
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

        // the accumulation above is per vertex INDEX, and a texture seam
        // splits one point into several of those, so this is where the
        // smoothing actually happens
        MD5_WeldSeamNormals(iqmData, total_verts);
    }

    model->iqmData = iqmData;

    // animation is optional: without it the model still draws its bind pose
    if (MD5_LoadAnim(model, iqmData, mod_name, iqmData->jointNames, num_joints))
        MD5_LoadScale(iqmData, mod_name, iqmData->jointNames, num_joints);
    else
        Com_DPrintf("%s: no usable .md5anim, using bind pose\n", mod_name);

    if (md5_load_scale != 1.0f)
        MD5_ApplyScale(iqmData, num_joints, md5_load_scale);
    if (md5_load_offset_set)
        MD5_ApplyOffset(iqmData, num_joints, md5_load_offset);

    // after the scale and offset, which move the poses
    MD5_SnapRiders(model, iqmData, num_joints);

    MD5_FreeMeshes(meshes, parsed_meshes);
    return Q_ERR_SUCCESS;

fail:
    MD5_FreeMeshes(meshes, parsed_meshes);
    model->iqmData = NULL;
    return ret;
}

/*
=================================================================

  skins

  Which image each skinnum of a .md5mesh draws. This used to live in
  the path tracer's loader (vkpt/models.c); it is here so that the GL
  and software renderers draw the same artwork from the same rules.
  Every renderer gets the same list of names: the RTX ones hand each
  to MAT_Find and inherit the classic material's tuning from
  md5_skin_t.inherit, GL and software hand it to IMG_Find.

=================================================================
*/

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

    /* THE REMASTER MERGED THIS MODEL INTO ANOTHER ONE, so its artwork is
       filed under the other model's name.

       models/monsters/soldierh is the case, and it is the mirror image of
       MD5_AppendMergedSkins below. Xatrix shipped its three soldier variants
       as a separate model; the remaster folded them into the stock soldier and
       ships sold01..sold03_p in models/monsters/soldier/md5/ ONLY. This fork
       keeps the two models apart (see SP_monster_soldier_h), so it still asks
       for models/monsters/soldierh - and every probe above then looks under a
       soldierh/md5/ that the remaster does not ship, which is how the ripper,
       hypergun and LASERGUN soldiers render solid white for anyone whose
       rerelease artwork comes out of the pak rather than a hand-built tree.

       The stems are identical across the merge, so only the directory moves. */
    if (slash) {
        static const struct {
            const char *from;   // the model directory this fork asks for
            const char *to;     // where the remaster actually files the skins
        } merged_dirs[] = {
            { "models/monsters/soldierh", "models/monsters/soldier" },
        };
        size_t dirlen = (size_t)(slash - skin);

        for (int i = 0; i < q_countof(merged_dirs); i++) {
            if (dirlen != strlen(merged_dirs[i].from)
                || Q_stricmpn(skin, merged_dirs[i].from, dirlen))
                continue;

            Q_snprintf(candidate, sizeof(candidate), "%s/md5/%s",
                       merged_dirs[i].to, name);
            if (MD5_SkinExists(candidate)) {
                Q_snprintf(buffer, size, "%s.png", candidate);
                return;
            }
            break;
        }
    }

    // the MODEL's own md5/ directory, for a model whose .md2 names its skin
    // somewhere else entirely
    Q_snprintf(candidate, sizeof(candidate), "%s/%s", base_path, name);
    if (MD5_SkinExists(candidate)) {
        // MAT_Find truncates the extension anyway; the material system is what
        // decides which image file actually backs this name
        Q_snprintf(buffer, size, "%s.png", candidate);
        return;
    }

    /* NO md5-ERA OVERRIDE EXISTS FOR THIS SKIN, so fall back to the name the
       .md2 actually wrote - which is where the classic artwork is, normally
       inside pak0.pak.

       Every step above hunts for a re-unwrapped rerelease texture, and where
       the rerelease ships one it is unambiguously the right answer. Where it
       does not - the user mounted the geometry but not the artwork, or this
       model's md5/ directory simply holds no image under this stem - the old
       code still handed MAT_Find the md5/ path it had just failed to find, and
       a material with no base image renders PURE WHITE. That is the "white
       monsters" of the bug reports, and it is a regression against the .md2
       the MD5 replaced: the classic skin was on the search path the whole time.

       The UVs disagree, so a classic texture is not a perfect fit on a
       re-unwrapped mesh. It is still enormously better than white, it is what
       the model looked like before MD5 support existed, and it costs nothing
       when the rerelease artwork is present because this line is not reached.
       MAT_InheritScalars in the caller then inherits from this same name, which
       is a no-op rather than a conflict. */
    {
        char classic[MAX_QPATH];
        char *cdot;

        Q_strlcpy(classic, skin, sizeof(classic));
        cdot = strrchr(classic, '.');
        if (cdot && !strchr(cdot, '/'))
            *cdot = 0;

        if (MD5_SkinExists(classic)) {
            Q_strlcpy(buffer, skin, size);
            return;
        }
    }

    // nothing exists anywhere; name the md5/ path, so the material warning
    // points at the directory the artwork is supposed to live in
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
MD5_AppendMergedSkins

Skins the rerelease added to a model's list that the classic .md2 cannot name,
because the classic .md2 is the only one we can read.

models/monsters/soldier is the case, and it is a real defect rather than a
cosmetic one. Xatrix shipped its three soldier variants as a SEPARATE model,
models/monsters/soldierh - a pure reskin of the stock soldier, same 434
triangles - and the remaster MERGED them: its soldier/tris.md2 lists TWELVE
skins (the classic six, then sold01, sold01_p, sold02, sold02_p, sold03,
sold03_p) and its game code spawns the ripper, hypergun and lasergun soldiers
as skinnum 6, 8 and 10 on the one model.

This fork's game code goes the other way and keeps the two models apart - see
SP_monster_soldier_h in m_soldier.c - which is fine while WE are the server.
A rerelease DEMO is not: it replays the remaster's own entity states, so it
sends modelindex = soldier with skinnum 8. Nothing has six-to-eleven, so
get_mesh_material (main.c) fell back to materials[0], and every Xatrix soldier
in every shipped demo played back wearing skin_lt - the LIGHT soldier's
artwork, blue blaster and all.

The artwork is not missing. The remaster ships sold01..sold03_p right there in
the model's own md5/ directory. Only the LIST is short, because the only
models/monsters/soldier/tris.md2 on the search path is the 1997 one out of
baseq2/pak0.pak - the remaster ships no loose copy of its own twelve-skin file
for us to read.

So the extra names are written out, in the remaster's order, exactly as
MD5_HeldWeaponSkin above writes out the held-weapon skins: there is no rule
that derives them. (classic_skins) is what the 1997 list is expected to hold,
and a list of any other length is left alone - so if a twelve-skin .md2 ever
does turn up on the path, it wins and nothing is appended twice.
=================
*/
static int MD5_AppendMergedSkins(const char *base_path, md5_skin_t *skins, int numskins)
{
    static const struct {
        const char *md5_dir;        // the model's own md5/ directory
        int         classic_skins;  // how many names the 1997 .md2 carries
        const char *inherit_dir;    // where these skins' classic materials live
        const char *stems[6];
        // classic material to take tuning from, when it is not stems[j] itself
        const char *inherit_stems[6];
    } merged[] = {
        { "models/monsters/soldier/md5", 6, "models/monsters/soldierh",
          { "sold01", "sold01_p", "sold02", "sold02_p", "sold03", "sold03_p" } },
        // monster_gladb: the remaster's gladiatr.md2 lists skin2/pain2 after
        // the classic pair; the 1997 one in baseq2/pak0.pak stops at two
        { "models/monsters/gladiatr/md5", 2, "models/monsters/gladiatr",
          { "skin2", "pain2" } },
        // monster_guncmdr: skinnum 2 (cskin), 3 when hurt (cpain). The 1997
        // gunner.md2 stops at skin/pain, so the commander drew the plain
        // gunner skin. No classic cskin material exists; take the gunner's.
        { "models/monsters/gunner/md5", 2, "models/monsters/gunner",
          { "cskin", "cpain" }, { "skin", "pain" } },
    };

    for (int i = 0; i < q_countof(merged); i++) {
        if (Q_stricmp(base_path, merged[i].md5_dir))
            continue;
        if (numskins != merged[i].classic_skins)
            break;

        for (int j = 0; j < q_countof(merged[i].stems); j++) {
            char probe[MAX_QPATH];
            md5_skin_t *skin;

            if (!merged[i].stems[j] || numskins >= MAX_ALIAS_SKINS)
                break;

            Q_snprintf(probe, sizeof(probe), "%s/%s", base_path, merged[i].stems[j]);
            if (!MD5_SkinExists(probe))
                break;      // an incomplete set is not worth guessing through

            skin = &skins[numskins];
            Q_snprintf(skin->path, sizeof(skin->path), "%s.png", probe);

            // Same reason as the .md2's own skins below: take the classic
            // material's tuning, which lives under the soldierh path these
            // skins came from.
            Q_snprintf(skin->inherit, sizeof(skin->inherit), "%s/%s.pcx", merged[i].inherit_dir,
                       merged[i].inherit_stems[j] ? merged[i].inherit_stems[j] : merged[i].stems[j]);

            numskins++;
        }
        break;
    }

    return numskins;
}

/*
=================
MD5_LoadSkinsFromMD2

Reads the classic model's skin list so that skinnum keeps selecting the same
skin it always did. Returns 0 when the .md2 is unreadable, in which case the
caller uses a single default skin.
=================
*/
static int MD5_LoadSkinsFromMD2(const char *mod_name, const char *base_path, md5_skin_t *skins)
{
    char md2_path[MAX_QPATH];
    char skinname[MAX_QPATH];
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
        MD5_SkinPath(base_path, skinname, mod_name, skins[numskins].path, sizeof(skins[numskins].path));

        // The re-unwrapped MD5 skins cannot use Q2RTX's hand-authored normal
        // and emissive maps, but the classic material's tuning is not tied to
        // a UV layout - without this the model renders at base_factor 1.0
        // against the 1.5-2.5 the classic skin asks for, i.e. darker than the
        // .md2 it replaces.
        Q_strlcpy(skins[numskins].inherit, skinname, sizeof(skins[numskins].inherit));

        numskins++;
    }

    // ...and the ones the remaster added to this model that the 1997 list
    // cannot name. Only reached when the .md2 above actually read.
    numskins = MD5_AppendMergedSkins(base_path, skins, numskins);

done:
    FS_FreeFile(rawdata);
    return numskins;
}

/*
=================
MD5_ResolveSkins

The skin list of the .md5mesh (mod_name): (skins) must hold MAX_ALIAS_SKINS.
Always at least one entry - a model with no readable .md2 to take the list
from gets a single default skin.
=================
*/
int MD5_ResolveSkins(const char *mod_name, md5_skin_t *skins)
{
    char base_path[MAX_QPATH];
    char default_skin[MAX_QPATH];
    int numskins;

    memset(skins, 0, sizeof(*skins) * MAX_ALIAS_SKINS);

    /* QUAKE 1's rerelease md5s sit beside the .mdl, in progs/, with no .md2
       to take a skin list from: skin N is <model>_<NN>_00.lmp, the first
       frame of the .mdl's skin N (model_q1.c chose the md5 knowing it has as
       many as the .mdl). The name keeps the game's "id1/<episode>/", so an
       add-on's own skin wins on its maps; images.c reads the .lmp. */
    if (!strstr(mod_name, "/md5/")
        && (!Q_strncasecmp(mod_name, "id1/", 4) || !Q_strncasecmp(mod_name, "progs/", 6))) {
        char stem[MAX_QPATH];

        COM_StripExtension(stem, mod_name, sizeof(stem));
        for (numskins = 0; numskins < MAX_ALIAS_SKINS; numskins++) {
            if (Q_snprintf(skins[numskins].path, sizeof(skins[numskins].path), "%s_%02d_00.lmp",
                           stem, numskins) >= sizeof(skins[numskins].path))
                break;
            if (!FS_FileExists(skins[numskins].path))
                break;
        }
        if (numskins < MAX_ALIAS_SKINS)
            skins[numskins].path[0] = 0;
        if (!numskins) {
            Q_snprintf(skins[0].path, sizeof(skins[0].path), "%s_00_00.lmp", stem);
            numskins = 1;
        }
        return numskins;
    }

    COM_FilePath(mod_name, base_path, sizeof(base_path));

    numskins = MD5_LoadSkinsFromMD2(mod_name, base_path, skins);
    if (numskins)
        return numskins;

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

    /* (base_path) is "<dir>/md5"; (classic_path) is the "<dir>" beside it,
       which is where a .pcx out of pak0.pak lives. Same rule as
       MD5_SkinPath: an md5-era override wins, and when there is none the
       classic artwork is used rather than a name that resolves to nothing
       and renders white. */
    char classic_path[MAX_QPATH];
    char *md5_slash;

    Q_strlcpy(classic_path, base_path, sizeof(classic_path));
    md5_slash = strrchr(classic_path, '/');
    if (md5_slash && !Q_stricmp(md5_slash, "/md5"))
        *md5_slash = 0;
    else
        classic_path[0] = 0;

    if (MD5_SkinExists(probe_path))
        Q_snprintf(default_skin, sizeof(default_skin), "%s.png", probe_path);
    else if (!MD5_HeldWeaponSkin(mod_name, default_skin, sizeof(default_skin))
             && !MD5_LoneSkinInDir(base_path, default_skin, sizeof(default_skin)))
    {
        char classic_probe[MAX_QPATH];

        // the classic model's own stem, then the "skin" convention, then
        // a directory that holds exactly one image - all beside the .md2
        if (classic_path[0]
            && (Q_snprintf(classic_probe, sizeof(classic_probe), "%s/%s", classic_path, name),
                MD5_SkinExists(classic_probe)))
            Q_snprintf(default_skin, sizeof(default_skin), "%s.pcx", classic_probe);
        else if (classic_path[0]
                 && (Q_snprintf(classic_probe, sizeof(classic_probe), "%s/skin", classic_path),
                     MD5_SkinExists(classic_probe)))
            Q_snprintf(default_skin, sizeof(default_skin), "%s.pcx", classic_probe);
        else if (!classic_path[0]
                 || !MD5_LoneSkinInDir(classic_path, default_skin, sizeof(default_skin)))
            Q_snprintf(default_skin, sizeof(default_skin), "%s/skin.png", base_path);
    }

    Q_strlcpy(skins[0].path, default_skin, sizeof(skins[0].path));
    return 1;
}

/*
=================
MD5_FrameBounds

From the .md5anim's own per-frame bounds. Without them, the bind pose padded
by half its size on every side, which a walk or a death fall stays inside.
=================
*/
void MD5_FrameBounds(const iqm_model_t *model, int frame, vec3_t mins, vec3_t maxs)
{
    if (model->bounds && frame >= 0 && frame < (int)model->num_frames) {
        VectorCopy(model->bounds + frame * 6, mins);
        VectorCopy(model->bounds + frame * 6 + 3, maxs);
        return;
    }

    ClearBounds(mins, maxs);
    for (uint32_t v = 0; v < model->num_vertexes; v++)
        AddPointToBounds(model->positions + v * 3, mins, maxs);

    for (int i = 0; i < 3; i++) {
        float pad = (maxs[i] - mins[i]) * 0.5f;
        mins[i] -= pad;
        maxs[i] += pad;
    }
}

/*
=================
MD5_SkinVertices

CPU skinning, for the renderers with no skinning shader (GL and software): the
same blend instance_geometry.comp does, from the bone matrices
R_ComputeIQMTransforms makes (3x4, row major). A model with no animation, or
a vertex with no weights, keeps its bind pose.
=================
*/
void MD5_SkinVertices(const iqm_model_t *model, const float *pose_matrices,
                      uint32_t first_vertex, uint32_t num_vertexes,
                      float *positions, float *normals)
{
    const float *src_pos = model->positions + first_vertex * 3;
    const float *src_nrm = model->normals + first_vertex * 3;
    const byte  *src_idx = model->blend_indices + first_vertex * 4;
    const byte  *src_wgt = model->blend_weights + first_vertex * 4;

    for (uint32_t v = 0; v < num_vertexes; v++) {
        const float *p = src_pos + v * 3;
        const float *n = src_nrm + v * 3;
        float *out_p = positions + v * 3;
        float m[12] = { 0 };
        float sum = 0;

        if (pose_matrices) {
            for (int k = 0; k < 4; k++) {
                const byte *idx = src_idx + v * 4;
                const byte *wgt = src_wgt + v * 4;
                const float *bone;
                float w;

                if (!wgt[k] || idx[k] >= model->num_poses)
                    continue;
                w = wgt[k];
                bone = pose_matrices + idx[k] * 12;
                for (int j = 0; j < 12; j++)
                    m[j] += bone[j] * w;
                sum += w;
            }
        }

        if (sum <= 0) {
            VectorCopy(p, out_p);
            if (normals)
                VectorCopy(n, normals + v * 3);
            continue;
        }

        sum = 1.0f / sum;
        for (int r = 0; r < 3; r++)
            out_p[r] = (m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]) * sum;

        if (normals) {
            float *out_n = normals + v * 3;

            for (int r = 0; r < 3; r++)
                out_n[r] = m[r * 4 + 0] * n[0] + m[r * 4 + 1] * n[1] + m[r * 4 + 2] * n[2];
            // a joint scaled to nothing (.md5scale) leaves no direction at all
            if (!VectorNormalize(out_n))
                VectorCopy(n, out_n);
        }
    }
}
