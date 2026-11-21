/*
Copyright (C) 1996-1997 Id Software, Inc.
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
// model_q1.c -- Quake 1's model formats, for the QuakeC game (g_q1vm*.c)
//
// THE GAME NAMES EVERY QUAKE 1 MODEL "id1/<episode>/<path>" - id1/id1/progs/
// soldier.mdl, id1/hipnotic/progs/player.mdl, id1/id1/maps/b_bh25.bsp - and
// files.c reads that episode's copy first, then id1's, the way the add-on
// itself saw its data (quake -hipnotic). Everything worked out from a model's
// name here keeps the prefix, so an add-on's own skins win on its own maps.
//
// None of the three formats gets a loader of its own in any renderer. Each is
// turned into a file in a format every renderer already loads - path tracer,
// D3D12, GL and software alike - and handed to that loader:
//
//   .mdl  (IDPO 6)  -> an .md2. The two are the same idea a year apart: byte
//                      vertices under one scale and origin, the same 162
//                      normals, a frame per pose. What differs is the skin:
//                      Quake's lives inside the file, 8-bit, and a vertex on
//                      the seam between the front and back halves of the
//                      skin takes s + width/2 on a back-facing triangle.
//                      The .md2 gets a second texture coordinate for each
//                      seam vertex, and its skins are named so images.c can
//                      cut them out of the .mdl (MOD_Q1ModelSkin).
//   .spr  (IDSP 1)  -> an .sp2, frames named for MOD_Q1SpriteFrame.
//   maps/b_*.bsp    -> an .md3, one mesh per wall texture: the health and
//                      ammo boxes are little BSP files, the only brush
//                      models Quake ever used as entities.
//
// With cl_md5_models, the rerelease's skeletal replacement is used instead
// where it ships one: <dir>/<stem>.md5mesh beside the .mdl (no md5/ folder in
// Quake 1), its skins <dir>/<stem>_<NN>_00.lmp (model_md5.c), and for the
// boxes the item meshes in progs/ (q1_item_md5).
//
// IMAGE NAMES follow tools/q1matdev.py and id1/materials/quake1.mat, with the
// episode prefix in front: a rerelease skin is its .lmp's own name
// (id1/id1/progs/ogre_00_00.lmp), an .mdl skin <model>_mdl_<skin>_<frame>
// (id1/id1/progs/ogre_mdl_00_00.lmp), a sprite frame <sprite>_spr_<frame>.
// The material system looks the prefixed name up first and the bare one
// (progs/ogre_mdl_00_00) after it, so quake1.mat's entries apply to every
// episode while each episode's pixels stay its own. A box's wall textures are
// textures/q1/<name>, as bsp_q1.c spells a map's.
//

#include <shared/shared.h>
#include <common/common.h>
#include <common/files.h>
#include <format/md2.h>
#if USE_MD3
#include <format/md3.h>
#endif
#include <format/sp2.h>
#include <refresh/models.h>

#define Q1MDL_IDENT     MakeLittleLong('I','D','P','O')
#define Q1MDL_VERSION   6
#define Q1SPR_IDENT     MakeLittleLong('I','D','S','P')
#define Q1SPR_VERSION   1
#define Q1BSP_VERSION   29
#define Q1BSP2_IDENT    MakeLittleLong('B','S','P','2')

#define Q1MDL_HEADER    84
#define Q1SPR_HEADER    36

// more faces than any box has: the .bsp is a whole map (MOD_LoadQuake1)
#define Q1_MAX_BOX_FACES    1024

static int32_t q1_rd32(const byte *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (int32_t)LittleLong(v);
}

static float q1_rdf(const byte *p)
{
    float v;
    memcpy(&v, p, 4);
    return LittleFloat(v);
}

/*
=================================================================

  names

=================================================================
*/

// "id1/<episode>/<rest>": a pointer to <rest> and (*is_id1) whether the
// episode is Quake itself; (name) when there is no such prefix (then it is
// taken as id1's)
static const char *q1_rest(const char *name, bool *is_id1)
{
    *is_id1 = true;
    if (Q_strncasecmp(name, "id1/", 4))
        return name;
    const char *slash = strchr(name + 4, '/');
    if (!slash)
        return name;
    *is_id1 = (slash - (name + 4) == 3) && !Q_strncasecmp(name + 4, "id1", 3);
    return slash + 1;
}

// the base name of a path, without its directory or extension
static void q1_stem(const char *name, char *out, size_t size)
{
    COM_StripExtension(out, COM_SkipPath(name), size);
}

bool MOD_Q1IsModel(const char *name)
{
    size_t len = strlen(name);

    if (len < 5 || name[0] == '*')
        return false;
    if (!Q_stricmp(name + len - 4, ".mdl") || !Q_stricmp(name + len - 4, ".spr"))
        return true;
    // a box (maps/b_*.bsp): the game's prefixed names and b_ files. Quake II's
    // own .bsp names are maps, which the client never registers as models; a
    // whole map the game precaches by its prefixed name anyway is drawn as
    // nothing (Q1_MAX_BOX_FACES).
    if (!Q_stricmp(name + len - 4, ".bsp")) {
        const char *base = COM_SkipPath(name);
        return !Q_strncasecmp(name, "id1/", 4) || !Q_strncasecmp(base, "b_", 2);
    }
    return false;
}

// a file's bytes are the same as another's: the episode did not replace it
static bool q1_same_file(const char *a, const char *b)
{
    byte *da = NULL, *db = NULL;
    int la = FS_LoadFile(a, (void **)&da);
    int lb = FS_LoadFile(b, (void **)&db);
    bool same = da && db && la == lb && !memcmp(da, db, la);

    if (da)
        FS_FreeFile(da);
    if (db)
        FS_FreeFile(db);
    return same;
}

/*
A rerelease md5 belongs to the .mdl it was made from, and only id1 and mg3
ship any. On an add-on's map "id1/rogue/progs/ogre.md5mesh" falls through to
id1's - but Dissolution of Eternity has its OWN ogre.mdl, with a second skin
(the multi-grenade ogre), and its own zombie.mdl, with ten more frames (the
zombie getting up). id1's md5 drawn for those would lose the skin and stop
the animation dead. So when the episode replaced the .mdl, the md5 must be
the episode's too: one that is byte for byte id1's is refused.
(ours) is the episode's model, (md5) the md5 beside it, each compared with
the same name in id1 - "id1/id1/<path>".
*/
static bool q1_md5_from_episode(const char *ours, const char *md5)
{
    char theirs[MAX_QPATH], theirs_md5[MAX_QPATH];
    bool is_id1;
    const char *rest = q1_rest(ours, &is_id1);

    if (is_id1)
        return true;
    if (Q_concat(theirs, sizeof(theirs), "id1/id1/", rest) >= sizeof(theirs))
        return true;
    if (!FS_FileExists(theirs) || q1_same_file(ours, theirs))
        return true;    // the episode has the model from id1, or id1 has none

    rest = q1_rest(md5, &is_id1);
    if (Q_concat(theirs_md5, sizeof(theirs_md5), "id1/id1/", rest) >= sizeof(theirs_md5))
        return true;
    return !FS_FileExists(theirs_md5) || !q1_same_file(md5, theirs_md5);
}

// how many frames <stem>.md5anim has; -1 when there is none
static int q1_md5anim_frames(const char *md5)
{
    char anim[MAX_QPATH];
    char *data = NULL;
    int frames = -1;

    COM_StripExtension(anim, md5, sizeof(anim));
    if (Q_strlcat(anim, ".md5anim", sizeof(anim)) >= sizeof(anim))
        return -1;
    if (FS_LoadFile(anim, (void **)&data) < 0 || !data)
        return -1;
    const char *p = strstr(data, "numFrames");
    if (p)
        frames = atoi(p + 9);
    FS_FreeFile(data);
    return frames;
}

// how many <stem>_<NN>_00.lmp skins the md5 has
static int q1_md5_skins(const char *md5)
{
    char stem[MAX_QPATH], path[MAX_QPATH];
    int n;

    COM_StripExtension(stem, md5, sizeof(stem));
    for (n = 0; n < MAX_ALIAS_SKINS; n++) {
        if (Q_snprintf(path, sizeof(path), "%s_%02d_00.lmp", stem, n) >= sizeof(path))
            break;
        if (!FS_FileExists(path))
            break;
    }
    return n;
}

/*
=================================================================

  .mdl

=================================================================
*/

typedef struct {
    const byte  *data;
    size_t      length;
    int         numskins, skinwidth, skinheight;
    int         numverts, numtris, numframes, flags;
    int         numposes, numskinposes;     // counting each member of a group
    vec3_t      scale, origin;
    size_t      ofs_st, ofs_tris, ofs_frames, end;
} q1mdl_t;

/*
GROUPS. A Quake frame or skin may be a group - the torch flame's six
flickers, the spike's four spins, the rogue sphere's four skins - that Quake
cycled through by the clock. The .md2 has every member as a frame or skin of
its own, in order, and the QuakeC VM sends the one Quake would have drawn
(g_q1vm_cmds.c Q1_AnimFrame). Both sides expand under the same limits.
*/
static bool q1_mdl_expand_frames(const q1mdl_t *m)
{
    return m->numposes <= MD2_MAX_FRAMES;
}

static bool q1_mdl_expand_skins(const q1mdl_t *m)
{
    return m->numskinposes <= MAX_ALIAS_SKINS && m->numskinposes <= MD2_MAX_SKINS;
}

// the header, and where the skins, texture coordinates, triangles and frames
// start; false for anything that is not a whole Quake 1 model
static bool q1_mdl_parse(const byte *data, size_t length, q1mdl_t *m)
{
    size_t o, skin;

    memset(m, 0, sizeof(*m));
    if (length < Q1MDL_HEADER)
        return false;
    if (q1_rd32(data) != (int32_t)Q1MDL_IDENT || q1_rd32(data + 4) != Q1MDL_VERSION)
        return false;

    for (int i = 0; i < 3; i++) {
        m->scale[i] = q1_rdf(data + 8 + i * 4);
        m->origin[i] = q1_rdf(data + 20 + i * 4);
    }
    m->numskins = q1_rd32(data + 48);
    m->skinwidth = q1_rd32(data + 52);
    m->skinheight = q1_rd32(data + 56);
    m->numverts = q1_rd32(data + 60);
    m->numtris = q1_rd32(data + 64);
    m->numframes = q1_rd32(data + 68);
    m->flags = q1_rd32(data + 76);

    if (m->numskins < 0 || m->numskins > MAX_ALIAS_SKINS
        || m->skinwidth < 1 || m->skinwidth > 4096 || m->skinheight < 1 || m->skinheight > 4096
        || m->numverts < 3 || m->numverts > MAX_ALIAS_VERTS
        || m->numtris < 1 || m->numtris > MD2_MAX_TRIANGLES
        || m->numframes < 1 || m->numframes > MD2_MAX_FRAMES)
        return false;

    skin = (size_t)m->skinwidth * m->skinheight;
    o = Q1MDL_HEADER;
    for (int i = 0; i < m->numskins; i++) {
        if (o + 4 > length)
            return false;
        if (q1_rd32(data + o) == 0) {
            o += 4 + skin;
        } else {
            int n;
            if (o + 8 > length)
                return false;
            n = q1_rd32(data + o + 4);
            if (n < 1 || n > 256)
                return false;
            o += 8 + 4 * (size_t)n + skin * n;
            m->numskinposes += n - 1;
        }
        m->numskinposes++;
    }
    m->ofs_st = o;
    o += 12 * (size_t)m->numverts;
    m->ofs_tris = o;
    o += 16 * (size_t)m->numtris;
    m->ofs_frames = o;
    if (o > length)
        return false;

    // walk the frames to be sure they are all there
    const size_t frame = 24 + 4 * (size_t)m->numverts;
    for (int i = 0; i < m->numframes; i++) {
        if (o + 4 > length)
            return false;
        if (q1_rd32(data + o) == 0) {
            o += 4 + frame;
            m->numposes++;
        } else {
            int n;
            if (o + 8 > length)
                return false;
            n = q1_rd32(data + o + 4);
            if (n < 1 || n > 1024)
                return false;
            o += 8 + 8 + 4 * (size_t)n + frame * n;
            m->numposes += n;
        }
        if (o > length)
            return false;
    }

    m->data = data;
    m->length = length;
    m->end = o;
    return true;
}

/*
Pose (index): with (expand) every member of every group counts, else the
frames as QuakeC numbers them, a group's first member standing for it.
Returns the pose's 24-byte header (bounding box, name), the vertices after it.
*/
static const byte *q1_mdl_pose(const q1mdl_t *m, int index, bool expand)
{
    const size_t frame = 24 + 4 * (size_t)m->numverts;
    size_t o = m->ofs_frames;
    int pose = 0;

    for (int i = 0; i < m->numframes; i++) {
        if (q1_rd32(m->data + o) == 0) {
            if ((expand ? pose : i) == index)
                return m->data + o + 4;
            o += 4 + frame;
            pose++;
        } else {
            int n = q1_rd32(m->data + o + 4);
            size_t first = o + 8 + 8 + 4 * (size_t)n;

            if (expand ? (index >= pose && index < pose + n) : i == index)
                return m->data + first + frame * (expand ? index - pose : 0);
            o = first + frame * n;
            pose += n;
        }
    }
    return NULL;
}

/*
=================
MOD_Q1ModelSkin

Skin (skin) of the .mdl at (path), frame (frame) of it if it is a group, as
(*width) x (*height) palette indices; Z_Free it. NULL if there is no such
model or skin.
=================
*/
byte *MOD_Q1ModelSkin(const char *path, int skin, int frame, int *width, int *height)
{
    byte *data = NULL, *out = NULL;
    q1mdl_t m;
    int len;

    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return NULL;

    if (q1_mdl_parse(data, len, &m) && skin >= 0 && skin < m.numskins) {
        const size_t size = (size_t)m.skinwidth * m.skinheight;
        size_t o = Q1MDL_HEADER;

        for (int i = 0; i <= skin; i++) {
            const byte *pixels;
            int n = 1;

            if (q1_rd32(data + o) == 0) {
                pixels = data + o + 4;
                o += 4 + size;
            } else {
                n = q1_rd32(data + o + 4);
                pixels = data + o + 8 + 4 * (size_t)n + size * (frame < 0 ? 0 : frame >= n ? n - 1 : frame);
                o += 8 + 4 * (size_t)n + size * n;
            }
            if (i == skin) {
                out = Z_Malloc(size);
                memcpy(out, pixels, size);
                *width = m.skinwidth;
                *height = m.skinheight;
            }
        }
    }

    FS_FreeFile(data);
    return out;
}

/*
The .md2 made from a .mdl.

TEXTURE COORDINATES. GLQuake samples texel centres, (s + 0.5) / width; an
.md2 is s / width with whole-number s. Doubling both - 2s + 1 over 2 width -
is the same position, and keeps the half texel as long as twice the skin
fits the .md2's 640 x 480 limit. Quake's own skins are all 320 or narrower;
mg3's 480- and 512-wide ones lose the half texel, nothing else.

THE SEAM. Every vertex has one (s, t). One on the seam (onseam) is shared by
the front half of the skin and the back half, and a triangle facing away
(facesfront 0) takes it at s + width/2. So each seam vertex gets a second
texture coordinate after the first numverts, and back-facing triangles use
it. The .md2 loaders split a vertex used with two coordinates by themselves.
*/
static int q1_mdl_to_md2(const char *name, const q1mdl_t *m, void **out, size_t *out_len)
{
    char stem[MAX_QPATH];
    const int fit = (m->skinwidth * 2 <= MD2_MAX_SKINWIDTH && m->skinheight * 2 <= MD2_MAX_SKINHEIGHT) ? 2 : 1;
    const bool expand_frames = q1_mdl_expand_frames(m), expand_skins = q1_mdl_expand_skins(m);
    const int numframes = expand_frames ? m->numposes : m->numframes;
    const int numskins = expand_skins ? m->numskinposes : m->numskins;
    int num_st = m->numverts;
    int *seam = Z_Malloc(sizeof(int) * m->numverts);
    const size_t framesize = sizeof(dmd2frame_t) + sizeof(dmd2trivertx_t) * (m->numverts - 1);
    size_t size;
    byte *buf;
    dmd2header_t *h;

    if (m->skinwidth > MD2_MAX_SKINWIDTH || m->skinheight > MD2_MAX_SKINHEIGHT) {
        Z_Free(seam);
        return Q_ERR_INVALID_FORMAT;
    }

    for (int v = 0; v < m->numverts; v++) {
        seam[v] = -1;
        if (q1_rd32(m->data + m->ofs_st + v * 12))
            seam[v] = num_st++;
    }
    if (num_st > MAX_ALIAS_VERTS) {
        Z_Free(seam);
        return Q_ERR_TOO_MANY;
    }

    size = sizeof(dmd2header_t) + MD2_MAX_SKINNAME * (size_t)numskins
         + sizeof(dmd2stvert_t) * num_st + sizeof(dmd2triangle_t) * m->numtris
         + framesize * numframes;
    buf = Z_Mallocz(size);
    h = (dmd2header_t *)buf;

    h->ident = LittleLong(MD2_IDENT);
    h->version = LittleLong(MD2_VERSION);
    h->skinwidth = LittleLong(m->skinwidth * fit);
    h->skinheight = LittleLong(m->skinheight * fit);
    h->framesize = LittleLong((uint32_t)framesize);
    h->num_skins = LittleLong(numskins);
    h->num_xyz = LittleLong(m->numverts);
    h->num_st = LittleLong(num_st);
    h->num_tris = LittleLong(m->numtris);
    h->num_glcmds = 0;
    h->num_frames = LittleLong(numframes);
    h->ofs_skins = LittleLong(sizeof(dmd2header_t));
    h->ofs_st = LittleLong(sizeof(dmd2header_t) + MD2_MAX_SKINNAME * numskins);
    h->ofs_tris = LittleLong(LittleLong(h->ofs_st) + sizeof(dmd2stvert_t) * num_st);
    h->ofs_frames = LittleLong(LittleLong(h->ofs_tris) + sizeof(dmd2triangle_t) * m->numtris);
    h->ofs_glcmds = LittleLong((uint32_t)size);
    h->ofs_end = LittleLong((uint32_t)size);

    // skins: <model>_mdl_<skin>_<member>, which images.c cuts out of this
    // .mdl; every member of a skin group when they fit, else the first
    COM_StripExtension(stem, name, sizeof(stem));
    {
        const size_t skinsize = (size_t)m->skinwidth * m->skinheight;
        size_t o = Q1MDL_HEADER;
        int out_skin = 0;

        for (int i = 0; i < m->numskins; i++) {
            int n = 1;
            if (q1_rd32(m->data + o) == 0) {
                o += 4 + skinsize;
            } else {
                n = q1_rd32(m->data + o + 4);
                o += 8 + 4 * (size_t)n + skinsize * n;
            }
            for (int j = 0; j < (expand_skins ? n : 1); j++, out_skin++) {
                char *dst = (char *)buf + LittleLong(h->ofs_skins) + MD2_MAX_SKINNAME * out_skin;
                if (Q_snprintf(dst, MD2_MAX_SKINNAME, "%s_mdl_%02d_%02d.lmp", stem, i, j) >= MD2_MAX_SKINNAME) {
                    Z_Free(buf);
                    Z_Free(seam);
                    return Q_ERR_STRING_TRUNCATED;
                }
            }
        }
    }

    // texture coordinates, then the seam's second ones
    dmd2stvert_t *st = (dmd2stvert_t *)(buf + LittleLong(h->ofs_st));
    for (int v = 0; v < m->numverts; v++) {
        const byte *src = m->data + m->ofs_st + v * 12;
        int s = q1_rd32(src + 4), t = q1_rd32(src + 8);

        st[v].s = LittleShort((int16_t)(s * fit + (fit - 1)));
        st[v].t = LittleShort((int16_t)(t * fit + (fit - 1)));
        if (seam[v] >= 0) {
            st[seam[v]].s = LittleShort((int16_t)((s + m->skinwidth / 2) * fit + (fit - 1)));
            st[seam[v]].t = st[v].t;
        }
    }

    // triangles
    dmd2triangle_t *tri = (dmd2triangle_t *)(buf + LittleLong(h->ofs_tris));
    for (int i = 0; i < m->numtris; i++) {
        const byte *src = m->data + m->ofs_tris + i * 16;
        bool front = q1_rd32(src) != 0;

        for (int k = 0; k < 3; k++) {
            int v = q1_rd32(src + 4 + k * 4);
            if (v < 0 || v >= m->numverts) {
                Z_Free(buf);
                Z_Free(seam);
                return Q_ERR_BAD_INDEX;
            }
            tri[i].index_xyz[k] = LittleShort((uint16_t)v);
            tri[i].index_st[k] = LittleShort((uint16_t)((!front && seam[v] >= 0) ? seam[v] : v));
        }
    }

    // frames: the same byte vertices and normal indices, under the model's
    // one scale and origin
    for (int f = 0; f < numframes; f++) {
        const byte *src = q1_mdl_pose(m, f, expand_frames);
        dmd2frame_t *dst = (dmd2frame_t *)(buf + LittleLong(h->ofs_frames) + framesize * f);

        if (!src) {
            Z_Free(buf);
            Z_Free(seam);
            return Q_ERR_BAD_EXTENT;
        }
        for (int k = 0; k < 3; k++) {
            dst->scale[k] = LittleFloat(m->scale[k]);
            dst->translate[k] = LittleFloat(m->origin[k]);
        }
        memcpy(dst->name, src + 8, sizeof(dst->name));
        dst->name[sizeof(dst->name) - 1] = 0;
        memcpy(dst->verts, src + 24, sizeof(dmd2trivertx_t) * m->numverts);
    }

    Z_Free(seam);
    *out = buf;
    *out_len = size;
    return Q_ERR_SUCCESS;
}

/*
=================================================================

  .spr

=================================================================
*/

typedef struct {
    int type;           // SPR_VP_PARALLEL_UPRIGHT 0 ... SPR_VP_PARALLEL_ORIENTED 4
    int numframes;
} q1spr_t;

// frame (index) of a sprite: its origin (Quake's: the left edge, the top
// edge), size and pixels; a group's first member
static const byte *q1_spr_frame(const byte *data, size_t length, int index,
                                int *ox, int *oy, int *w, int *h)
{
    size_t o = Q1SPR_HEADER;
    int numframes;

    if (length < Q1SPR_HEADER || q1_rd32(data) != (int32_t)Q1SPR_IDENT || q1_rd32(data + 4) != Q1SPR_VERSION)
        return NULL;
    numframes = q1_rd32(data + 24);

    for (int i = 0; i < numframes; i++) {
        int n = 1;
        size_t first;

        if (o + 4 > length)
            return NULL;
        if (q1_rd32(data + o) == 0) {
            first = o + 4;
        } else {
            if (o + 8 > length)
                return NULL;
            n = q1_rd32(data + o + 4);
            if (n < 1 || n > 1024)
                return NULL;
            first = o + 8 + 4 * (size_t)n;
        }

        // the members, one after another
        size_t p = first;
        for (int j = 0; j < n; j++) {
            if (p + 16 > length)
                return NULL;
            int fw = q1_rd32(data + p + 8), fh = q1_rd32(data + p + 12);
            if (fw < 1 || fh < 1 || fw > 4096 || fh > 4096 || p + 16 + (size_t)fw * fh > length)
                return NULL;
            if (i == index && j == 0) {
                *ox = q1_rd32(data + p);
                *oy = q1_rd32(data + p + 4);
                *w = fw;
                *h = fh;
                return data + p + 16;
            }
            p += 16 + (size_t)fw * fh;
        }
        o = p;
    }
    return NULL;
}

/*
=================
MOD_Q1SpriteFrame

Frame (frame) of the sprite at (path) as palette indices (255 see-through),
(*width) x (*height); Z_Free it.
=================
*/
byte *MOD_Q1SpriteFrame(const char *path, int frame, int *width, int *height)
{
    byte *data = NULL, *out = NULL;
    int len, ox, oy, w, h;

    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return NULL;

    const byte *src = q1_spr_frame(data, len, frame, &ox, &oy, &w, &h);
    if (src) {
        out = Z_Malloc((size_t)w * h);
        memcpy(out, src, (size_t)w * h);
        *width = w;
        *height = h;
    }
    FS_FreeFile(data);
    return out;
}

/*
The .sp2. Quake gives a frame's left and top edges from the sprite's origin;
a Quake II frame gives the distance from its left edge and its bottom edge to
the origin, so origin_x = -left and origin_y = height - top. The frames are
named <sprite>_spr_<frame>.lmp for MOD_Q1SpriteFrame.
*/
static int q1_spr_to_sp2(const char *name, const byte *data, size_t length, void **out, size_t *out_len,
                         int *type)
{
    char stem[MAX_QPATH];
    int numframes;

    if (length < Q1SPR_HEADER || q1_rd32(data) != (int32_t)Q1SPR_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (q1_rd32(data + 4) != Q1SPR_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;
    *type = q1_rd32(data + 8);
    numframes = q1_rd32(data + 24);
    if (numframes < 1 || numframes > SP2_MAX_FRAMES)
        return Q_ERR_INVALID_FORMAT;

    COM_StripExtension(stem, name, sizeof(stem));

    size_t size = sizeof(dsp2header_t) + sizeof(dsp2frame_t) * numframes;
    byte *buf = Z_Mallocz(size);
    dsp2header_t *h = (dsp2header_t *)buf;
    dsp2frame_t *f = (dsp2frame_t *)(buf + sizeof(dsp2header_t));

    h->ident = LittleLong(SP2_IDENT);
    h->version = LittleLong(SP2_VERSION);
    h->numframes = LittleLong(numframes);

    for (int i = 0; i < numframes; i++) {
        int ox, oy, w, hh;
        if (!q1_spr_frame(data, length, i, &ox, &oy, &w, &hh)) {
            Z_Free(buf);
            return Q_ERR_BAD_EXTENT;
        }
        f[i].width = LittleLong(w);
        f[i].height = LittleLong(hh);
        f[i].origin_x = LittleLong(-ox);
        f[i].origin_y = LittleLong(hh - oy);
        if (Q_snprintf(f[i].name, sizeof(f[i].name), "%s_spr_%02d.lmp", stem, i) >= sizeof(f[i].name)) {
            Z_Free(buf);
            return Q_ERR_STRING_TRUNCATED;
        }
    }

    *out = buf;
    *out_len = size;
    return Q_ERR_SUCCESS;
}

/*
=================================================================

  maps/b_*.bsp: the health and ammo boxes

=================================================================
*/

/*
THE BOXES' WALL TEXTURES are named textures/q1/<name> like a map's, so the
materials quake1.mat has for them apply. images.c finds a map's in the map
(BSP_Q1FindMiptex); a box's are not in the map, so it asks here after that.
Kept for the session: a few dozen 32 x 32 textures at most.
*/
typedef struct {
    char    name[32];
    int     width, height;
    byte    *pixels;
} q1_item_tex_t;

static q1_item_tex_t    q1_item_tex[64];
static int              q1_num_item_tex;

const byte *MOD_Q1ItemMiptex(const char *name, int *width, int *height)
{
    for (int i = 0; i < q1_num_item_tex; i++) {
        if (!Q_stricmp(q1_item_tex[i].name, name)) {
            *width = q1_item_tex[i].width;
            *height = q1_item_tex[i].height;
            return q1_item_tex[i].pixels;
        }
    }
    return NULL;
}

static void q1_item_tex_add(const char *name, int w, int h, const byte *pixels)
{
    q1_item_tex_t *t = NULL;

    for (int i = 0; i < q1_num_item_tex; i++) {
        if (!Q_stricmp(q1_item_tex[i].name, name)) {
            t = &q1_item_tex[i];
            break;
        }
    }
    if (!t) {
        if (q1_num_item_tex == q_countof(q1_item_tex))
            return;
        t = &q1_item_tex[q1_num_item_tex++];
        Q_strlcpy(t->name, name, sizeof(t->name));
    } else {
        Z_Free(t->pixels);
    }
    t->width = w;
    t->height = h;
    t->pixels = Z_Malloc((size_t)w * h);
    memcpy(t->pixels, pixels, (size_t)w * h);
}

// "*water1" -> "#water1", lower case: bsp_q1.c's spelling
static void q1_texture_name(const char *miptex, char *out, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size && i < 16 && miptex[i]; i++) {
        char c = Q_tolower(miptex[i]);
        out[i] = (c == '*') ? '#' : c;
    }
    out[i] = 0;
}

/*
The rerelease's replacements for the boxes. Each is centred on its origin,
standing on it; a box's origin is its CORNER (QuakeC gives every item mins
'0 0 0', maxs '32 32 56', and the .bsp sits in 1..31), so the mesh moves
16 16 0 to stand where the box stood. Paired by size: the 0 boxes are the
small ones, the 1 boxes the large; b_bh10 is the 15-health rotten box.
*/
#define Q1_BOX_MD5_SCALE    1.5f

static const struct {
    const char *box;
    const char *md5;
} q1_item_md5[] = {
    { "b_bh10",   "health15"     },
    { "b_bh25",   "health25"     },
    { "b_bh100",  "health100"    },
    { "b_shell0", "shellssmall"  },
    { "b_shell1", "shellslarge"  },
    { "b_nail0",  "nailssmall"   },
    { "b_nail1",  "nailslarge"   },
    { "b_rock0",  "rocketssmall" },
    { "b_rock1",  "rocketslarge" },
    { "b_batt0",  "cellssmall"   },
    { "b_batt1",  "cellslarge"   },
};

// the md5 that stands in for box (name), "" if none: <prefix>progs/<md5>.md5mesh
static void q1_item_md5_path(const char *name, char *out, size_t size)
{
    char stem[MAX_QPATH];

    out[0] = 0;
    q1_stem(name, stem, sizeof(stem));
    for (int i = 0; i < q_countof(q1_item_md5); i++) {
        if (Q_stricmp(stem, q1_item_md5[i].box))
            continue;
        // the prefix is everything before "maps/"
        const char *maps = Q_stristr(name, "maps/");
        size_t prefix = maps ? (size_t)(maps - name) : 0;
        if (prefix + 6 + strlen(q1_item_md5[i].md5) + 8 >= size)
            return;
        memcpy(out, name, prefix);
        out[prefix] = 0;
        Q_strlcat(out, "progs/", size);
        Q_strlcat(out, q1_item_md5[i].md5, size);
        Q_strlcat(out, ".md5mesh", size);
        return;
    }
}

#if USE_MD3

typedef struct {
    int     miptex;
    int     numverts, numtris;
} q1_bsp_group_t;

// a Quake 1 BSP's lumps
enum { Q1L_PLANES = 1, Q1L_TEXTURES = 2, Q1L_VERTEXES = 3, Q1L_TEXINFO = 6, Q1L_FACES = 7,
       Q1L_EDGES = 12, Q1L_SURFEDGES = 13, Q1L_MODELS = 14, Q1L_COUNT = 15 };

static void q1_md3_normal(const vec3_t n, byte out[2])
{
    // Quake III's latitude and longitude, 256 steps to a turn - the steps of
    // the sine table the loaders decode them with
    const float steps = 256.0f / (2.0f * (float)M_PI);
    float z = n[2] < -1.0f ? -1.0f : n[2] > 1.0f ? 1.0f : n[2];
    float lat = acosf(z) * steps;
    float lng = atan2f(n[1], n[0]) * steps;
    out[0] = (byte)(Q_rint(lat) & 255);
    out[1] = (byte)(Q_rint(lng) & 255);
}

/*
The .md3. Model 0's faces, grouped by wall texture, each a fan in Quake's
own winding - the same convention as an alias model's triangles, which the
loaders turn round alike. Every face keeps its own corners, so each has its
plane's normal and its texinfo's texture coordinates, flat.
*/
static int q1_bsp_to_md3(const char *name, const byte *data, size_t length, void **out, size_t *out_len)
{
    int32_t lofs[Q1L_COUNT], llen[Q1L_COUNT];
    bool bsp2;
    size_t edge_size, face_size;

    if (length < 4 + 8 * Q1L_COUNT)
        return Q_ERR_FILE_TOO_SMALL;
    bsp2 = q1_rd32(data) == (int32_t)Q1BSP2_IDENT;
    if (!bsp2 && q1_rd32(data) != Q1BSP_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;
    for (int i = 0; i < Q1L_COUNT; i++) {
        lofs[i] = q1_rd32(data + 4 + i * 8);
        llen[i] = q1_rd32(data + 8 + i * 8);
        if (lofs[i] < 0 || llen[i] < 0 || (size_t)lofs[i] + llen[i] > length)
            return Q_ERR_BAD_EXTENT;
    }
    edge_size = bsp2 ? 8 : 4;
    face_size = bsp2 ? 28 : 20;

    const byte *planes = data + lofs[Q1L_PLANES];
    const byte *verts = data + lofs[Q1L_VERTEXES];
    const byte *texinfo = data + lofs[Q1L_TEXINFO];
    const byte *faces = data + lofs[Q1L_FACES];
    const byte *edges = data + lofs[Q1L_EDGES];
    const byte *surfedges = data + lofs[Q1L_SURFEDGES];
    const byte *mt = data + lofs[Q1L_TEXTURES];
    const int numplanes = llen[Q1L_PLANES] / 20, numverts = llen[Q1L_VERTEXES] / 12;
    const int numtexinfo = llen[Q1L_TEXINFO] / 40, numfaces = (int)(llen[Q1L_FACES] / face_size);
    const int numedges = (int)(llen[Q1L_EDGES] / edge_size), numsurfedges = llen[Q1L_SURFEDGES] / 4;
    const int nummiptex = llen[Q1L_TEXTURES] >= 4 ? q1_rd32(mt) : 0;

    if (llen[Q1L_MODELS] < 64 || nummiptex < 1 || nummiptex > 256 || llen[Q1L_TEXTURES] < 4 + 4 * nummiptex)
        return Q_ERR_INVALID_FORMAT;

    const byte *model = data + lofs[Q1L_MODELS];
    const int firstface = q1_rd32(model + 56), facecount = q1_rd32(model + 60);
    if (firstface < 0 || facecount < 1 || firstface + facecount > numfaces)
        return Q_ERR_INVALID_FORMAT;
    // a box has a few dozen faces; this many is a map (MOD_LoadQuake1)
    if (facecount > Q1_MAX_BOX_FACES)
        return Q_ERR_TOO_MANY;

    // which mesh each face goes in: one per texture
    q1_bsp_group_t groups[MD3_MAX_MESHES];
    int numgroups = 0;
    int *face_group = Z_Malloc(sizeof(int) * facecount);

    for (int i = 0; i < facecount; i++) {
        const byte *f = faces + (size_t)(firstface + i) * face_size;
        const int ti = bsp2 ? q1_rd32(f + 16) : (int16_t)LittleShort(*(const int16_t *)(f + 10));
        const int ne = bsp2 ? q1_rd32(f + 12) : (int16_t)LittleShort(*(const int16_t *)(f + 8));
        int g, miptex;

        face_group[i] = -1;
        if (ti < 0 || ti >= numtexinfo || ne < 3 || ne > 64)
            continue;
        miptex = q1_rd32(texinfo + ti * 40 + 32);
        if (miptex < 0 || miptex >= nummiptex || q1_rd32(mt + 4 + 4 * miptex) < 0)
            continue;
        for (g = 0; g < numgroups; g++)
            if (groups[g].miptex == miptex)
                break;
        if (g == numgroups) {
            if (numgroups == MD3_MAX_MESHES)
                continue;
            groups[numgroups].miptex = miptex;
            groups[numgroups].numverts = groups[numgroups].numtris = 0;
            numgroups++;
        }
        groups[g].numverts += ne;
        groups[g].numtris += ne - 2;
        face_group[i] = g;
    }

    if (!numgroups) {
        Z_Free(face_group);
        return Q_ERR_TOO_FEW;
    }

    // the size of it all
    size_t size = sizeof(dmd3header_t) + sizeof(dmd3frame_t);
    for (int g = 0; g < numgroups; g++)
        size += sizeof(dmd3mesh_t) + sizeof(dmd3skin_t) + 12 * (size_t)groups[g].numtris
              + sizeof(dmd3coord_t) * groups[g].numverts + sizeof(dmd3vertex_t) * groups[g].numverts;

    byte *buf = Z_Mallocz(size);
    dmd3header_t *h = (dmd3header_t *)buf;
    dmd3frame_t *frame = (dmd3frame_t *)(buf + sizeof(dmd3header_t));

    h->ident = LittleLong(MD3_IDENT);
    h->version = LittleLong(MD3_VERSION);
    Q_strlcpy(h->filename, COM_SkipPath(name), sizeof(h->filename));
    h->num_frames = LittleLong(1);
    h->num_meshes = LittleLong(numgroups);
    h->ofs_frames = LittleLong(sizeof(dmd3header_t));
    h->ofs_tags = LittleLong(sizeof(dmd3header_t) + sizeof(dmd3frame_t));
    h->ofs_meshes = h->ofs_tags;
    h->ofs_end = LittleLong((uint32_t)size);

    // the box the corners span: the model lump's is a unit in from them
    vec3_t mins, maxs;
    ClearBounds(mins, maxs);
    Q_strlcpy(frame->creator, "model_q1.c", sizeof(frame->creator));

    byte *mesh_at = buf + sizeof(dmd3header_t) + sizeof(dmd3frame_t);
    int ret = Q_ERR_SUCCESS;

    for (int g = 0; g < numgroups && !ret; g++) {
        dmd3mesh_t *mesh = (dmd3mesh_t *)mesh_at;
        const byte *mip = mt + q1_rd32(mt + 4 + 4 * groups[g].miptex);
        char texname[32];
        const int mw = q1_rd32(mip + 16), mh = q1_rd32(mip + 20);
        const int mip0 = q1_rd32(mip + 24);

        q1_texture_name((const char *)mip, texname, sizeof(texname));
        if (mw > 0 && mh > 0 && mw <= 1024 && mh <= 1024 && mip0 > 0
            && (size_t)(mip - data) + mip0 + (size_t)mw * mh <= length)
            q1_item_tex_add(texname, mw, mh, mip + mip0);

        const size_t ofs_skins = sizeof(dmd3mesh_t);
        const size_t ofs_tris = ofs_skins + sizeof(dmd3skin_t);
        const size_t ofs_tcs = ofs_tris + 12 * (size_t)groups[g].numtris;
        const size_t ofs_verts = ofs_tcs + sizeof(dmd3coord_t) * groups[g].numverts;
        const size_t meshsize = ofs_verts + sizeof(dmd3vertex_t) * groups[g].numverts;

        mesh->ident = LittleLong(MD3_IDENT);
        Q_strlcpy(mesh->name, texname, sizeof(mesh->name));
        mesh->num_frames = LittleLong(1);
        mesh->num_skins = LittleLong(1);
        mesh->num_verts = LittleLong(groups[g].numverts);
        mesh->num_tris = LittleLong(groups[g].numtris);
        mesh->ofs_indexes = LittleLong((uint32_t)ofs_tris);
        mesh->ofs_skins = LittleLong((uint32_t)ofs_skins);
        mesh->ofs_tcs = LittleLong((uint32_t)ofs_tcs);
        mesh->ofs_verts = LittleLong((uint32_t)ofs_verts);
        mesh->meshsize = LittleLong((uint32_t)meshsize);

        dmd3skin_t *skin = (dmd3skin_t *)(mesh_at + ofs_skins);
        Q_snprintf(skin->name, sizeof(skin->name), "textures/q1/%s.wal", texname);

        uint32_t *idx = (uint32_t *)(mesh_at + ofs_tris);
        dmd3coord_t *tc = (dmd3coord_t *)(mesh_at + ofs_tcs);
        dmd3vertex_t *vx = (dmd3vertex_t *)(mesh_at + ofs_verts);
        int nv = 0;

        for (int i = 0; i < facecount; i++) {
            if (face_group[i] != g)
                continue;

            const byte *f = faces + (size_t)(firstface + i) * face_size;
            const int pn = bsp2 ? q1_rd32(f) : (int16_t)LittleShort(*(const int16_t *)f);
            const int side = bsp2 ? q1_rd32(f + 4) : (int16_t)LittleShort(*(const int16_t *)(f + 2));
            const int fe = bsp2 ? q1_rd32(f + 8) : q1_rd32(f + 4);
            const int ne = bsp2 ? q1_rd32(f + 12) : (int16_t)LittleShort(*(const int16_t *)(f + 8));
            const int ti = bsp2 ? q1_rd32(f + 16) : (int16_t)LittleShort(*(const int16_t *)(f + 10));
            const byte *tx = texinfo + ti * 40;
            vec3_t normal = { 0, 0, 1 };
            byte enc[2];

            if (pn >= 0 && pn < numplanes) {
                for (int k = 0; k < 3; k++)
                    normal[k] = q1_rdf(planes + pn * 20 + k * 4);
                if (side)
                    VectorNegate(normal, normal);
            }
            q1_md3_normal(normal, enc);

            const int first = nv;
            for (int e = 0; e < ne; e++) {
                int se, v;
                if (fe + e < 0 || fe + e >= numsurfedges) {
                    ret = Q_ERR_BAD_INDEX;
                    break;
                }
                se = q1_rd32(surfedges + (fe + e) * 4);
                if (abs(se) >= numedges) {
                    ret = Q_ERR_BAD_INDEX;
                    break;
                }
                const byte *edge = edges + (size_t)abs(se) * edge_size;
                if (bsp2)
                    v = q1_rd32(edge + (se < 0 ? 4 : 0));
                else
                    v = (uint16_t)LittleShort(*(const uint16_t *)(edge + (se < 0 ? 2 : 0)));
                if (v < 0 || v >= numverts) {
                    ret = Q_ERR_BAD_INDEX;
                    break;
                }

                vec3_t p;
                for (int k = 0; k < 3; k++)
                    p[k] = q1_rdf(verts + v * 12 + k * 4);
                AddPointToBounds(p, mins, maxs);
                for (int k = 0; k < 3; k++)
                    vx[nv].point[k] = LittleShort((int16_t)Q_rint(p[k] / MD3_XYZ_SCALE));
                vx[nv].norm[0] = enc[0];
                vx[nv].norm[1] = enc[1];

                float s = DotProduct(p, (const float *)tx) + q1_rdf(tx + 12);
                float t = DotProduct(p, (const float *)(tx + 16)) + q1_rdf(tx + 28);
                tc[nv].st[0] = LittleFloat(mw > 0 ? s / mw : s);
                tc[nv].st[1] = LittleFloat(mh > 0 ? t / mh : t);
                nv++;
            }
            if (ret)
                break;

            // a fan from the face's first corner
            for (int k = 2; k < ne; k++) {
                *idx++ = LittleLong(first);
                *idx++ = LittleLong(first + k - 1);
                *idx++ = LittleLong(first + k);
            }
        }

        mesh_at += meshsize;
    }

    Z_Free(face_group);
    if (ret) {
        Z_Free(buf);
        return ret;
    }

    for (int k = 0; k < 3; k++) {
        frame->mins[k] = LittleFloat(mins[k]);
        frame->maxs[k] = LittleFloat(maxs[k]);
    }
    frame->radius = LittleFloat(RadiusFromBounds(mins, maxs));

    *out = buf;
    *out_len = size;
    return Q_ERR_SUCCESS;
}

#endif // USE_MD3

/*
=================================================================

  MOD_Q1Build

=================================================================
*/

// the md5 to draw instead of .mdl (name), "" for none
static void q1_mdl_md5(const char *name, const q1mdl_t *m, char *md5, size_t size)
{
    char stem[MAX_QPATH];

    md5[0] = 0;
    COM_StripExtension(stem, name, sizeof(stem));
    if (Q_concat(md5, size, stem, ".md5mesh") >= size || !FS_FileExists(md5)) {
        md5[0] = 0;
        return;
    }

    // an add-on's own .mdl takes no md5 of id1's (q1_md5_from_episode)
    if (!q1_md5_from_episode(name, md5)) {
        Com_DPrintf("%s: the episode's own model, not id1's md5\n", name);
        md5[0] = 0;
        return;
    }

    // the md5 must animate every frame QuakeC can ask for, and have every skin
    int frames = q1_md5anim_frames(md5);
    if ((m->numframes > 1 && frames < m->numframes) || q1_md5_skins(md5) < m->numskins) {
        Com_DPrintf("%s: md5 has %d frames and %d skins for %d and %d; using the .mdl\n",
                    name, frames, q1_md5_skins(md5), m->numframes, m->numskins);
        md5[0] = 0;
    }
}

/*
=================
MOD_Q1Build

The file to hand a renderer's loader for Quake 1 model (name): a .md5mesh as
read (md5 true and one fits), or an .md2, .sp2 or .md3 made from the Quake
one. (*data) is Z_Free'd by the caller; (load_name) is the name to load it
under (the md5's own path, so its .md5anim and skins are found from it);
(*md5_scale) sizes an md5 and (md5_offset) then moves its vertices,
(*sprite_type) is a sprite's Quake orientation type, -1 otherwise.
=================
*/
int MOD_Q1Build(const char *name, bool md5, void **data, size_t *length,
                char *load_name, size_t size, vec3_t md5_offset, float *md5_scale, int *sprite_type)
{
    byte *raw = NULL;
    int len, ret;

    *data = NULL;
    *length = 0;
    *sprite_type = -1;
    VectorClear(md5_offset);
    *md5_scale = 1.0f;
    Q_strlcpy(load_name, name, size);

    const char *ext = COM_FileExtension(name);

    // the boxes: the rerelease's mesh, if this episode has the box from id1
    if (!Q_stricmp(ext, ".bsp") && md5) {
        char md5_path[MAX_QPATH], ours_id1[MAX_QPATH];
        bool is_id1;
        const char *rest = q1_rest(name, &is_id1);

        q1_item_md5_path(name, md5_path, sizeof(md5_path));
        if (md5_path[0] && FS_FileExists(md5_path)
            && (is_id1 || Q_concat(ours_id1, sizeof(ours_id1), "id1/id1/", rest) >= sizeof(ours_id1)
                || q1_same_file(name, ours_id1))) {
            len = FS_LoadFile(md5_path, (void **)&raw);
            if (raw) {
                *data = raw;
                *length = len;
                Q_strlcpy(load_name, md5_path, size);
                // KEX draws them half again as big: every one of them
                // then fills its box (shellssmall 15 -> b_shell0's 22,
                // health25 20 x 10 -> b_bh25's 30 x 14; Matt 11-21: "too small")
                *md5_scale = Q1_BOX_MD5_SCALE;
                VectorSet(md5_offset, 16, 16, 0);
                return Q_ERR_SUCCESS;
            }
        }
    }

    len = FS_LoadFile(name, (void **)&raw);
    if (!raw)
        return len;

    if (!Q_stricmp(ext, ".mdl")) {
        q1mdl_t m;
        if (!q1_mdl_parse(raw, len, &m)) {
            ret = Q_ERR_INVALID_FORMAT;
        } else {
            char md5_path[MAX_QPATH];
            byte *md5_raw = NULL;
            int md5_len = 0;

            if (md5) {
                q1_mdl_md5(name, &m, md5_path, sizeof(md5_path));
                if (md5_path[0])
                    md5_len = FS_LoadFile(md5_path, (void **)&md5_raw);
            }
            if (md5_raw) {
                *data = md5_raw;
                *length = md5_len;
                Q_strlcpy(load_name, md5_path, size);
                ret = Q_ERR_SUCCESS;
            } else {
                ret = q1_mdl_to_md2(name, &m, data, length);
            }
        }
    } else if (!Q_stricmp(ext, ".spr")) {
        ret = q1_spr_to_sp2(name, raw, len, data, length, sprite_type);
    } else if (!Q_stricmp(ext, ".bsp")) {
#if USE_MD3
        ret = q1_bsp_to_md3(name, raw, len, data, length);
#else
        ret = Q_ERR_UNKNOWN_FORMAT;
#endif
    } else {
        ret = Q_ERR_UNKNOWN_FORMAT;
    }

    FS_FreeFile(raw);
    return ret;
}
