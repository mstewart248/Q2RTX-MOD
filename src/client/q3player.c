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
*/

/*
QUAKE III ARENA PLAYER MODELS

See q3player.h for what a Quake III player is made of. This file loads one
(animation.cfg, the tags of lower.md3 and upper.md3, the .skin files), gives
the renderer its meshes with every skin on them (Q3P_PatchMD3), and each frame
works out what the legs and the torso are doing from what a Quake II server
tells us about the player:

- the LEGS from how the body moves: idle, walk, run, run backwards, crouch,
  swim, jump - a backflip (LEGS_JUMPB) when the jump goes backwards - and the
  landing. The walk and run play at the speed the body moves, so the feet keep
  their grip on the floor, and they turn up to 45 degrees toward where the
  body goes (Quake III's movementOffsets).
- the TORSO from the Quake II frame and the muzzle flashes: attack on every
  shot, gesture for wave/salute/taunt/point/flip, drop and raise round a
  weapon change, the deaths - and otherwise standing, whatever the legs do.
  Firing never stops the legs: no skating.
- the HEAD looks where the player looks, pitch and all; the torso takes three
  quarters of the pitch and turns after the head with some lag, the legs after
  the torso (Quake III's CG_PlayerAngles and CG_SwingAngles).

The Quake II server sends a player's pitch divided by 3 (p_view.c), which is
multiplied back here.

THE GUN is Quake II's: players/male/w_*.md2 (the rerelease's md5 when there is
one), drawn at frame 0, moved from the Quake II male's hand onto Quake III's
tag_weapon. Every rerelease w_*.md5mesh has a joint "Weapon" at the male's
hand, and at frame 0 it is the same for all 21 of them, so one transform
(q3p_grip below) puts every Quake II gun in a Quake III hand the way the male
holds it. cl_q3_gun_offset / cl_q3_gun_angles / cl_q3_gun_scale tune it.
*/

#include "client.h"
#include "format/md3.h"

static cvar_t   *cl_q3_gun_offset;
static cvar_t   *cl_q3_gun_angles;
static cvar_t   *cl_q3_gun_scale;
static cvar_t   *cl_q3_swing_speed;
static cvar_t   *cl_q3_debug;

/*
=============================================================================

LOADING

=============================================================================
*/

typedef struct {
    int     first;          // frame in its .md3
    int     num;
    int     loop;           // frames that loop at the end; 0 = plays once
    float   fps;
    bool    reversed;
} q3panim_t;

typedef struct {
    vec3_t  origin;
    vec3_t  axis[3];
} q3ptag_t;

struct q3player_s {
    char        name[MAX_QPATH];
    char        sex;                // 'm', 'f', 'n'
    bool        fixedlegs, fixedtorso;
    vec3_t      headoffset;         // the HUD head's nudge (cg_draw.c CG_DrawHead)
    q3panim_t   anims[Q3A_COUNT];

    int         numskins;
    char        skins[Q3P_MAX_SKINS][MAX_QPATH];

    int         lower_frames, upper_frames;
    q3ptag_t    *tag_torso;         // lower.md3, per frame
    q3ptag_t    *tag_head;          // upper.md3, per frame
    q3ptag_t    *tag_weapon;        // upper.md3, per frame
};

#define Q3P_MAX_MODELS  64

static q3player_t   *q3p_models[Q3P_MAX_MODELS];
static int          q3p_num_models;

// models looked for and not found, so a bad name is not looked for every time
static char         q3p_missing[Q3P_MAX_MODELS][MAX_QPATH];
static int          q3p_num_missing;

static bool q3p_valid_name(const char *model)
{
    return model && *model && strlen(model) < MAX_QPATH - 32 && COM_IsPath(model)
        && !strchr(model, '/') && !strchr(model, '\\') && !strstr(model, "..");
}

/*
A Quake III model: models/players/<model>/ has animation.cfg and the three
parts - and Quake II has no model of that name, which keeps the old meaning
of any Quake II skin that happens to share one.
*/
bool Q3P_ModelExists(const char *model)
{
    char path[MAX_QPATH];
    static const char *const files[] = { "animation.cfg", "lower.md3", "upper.md3", "head.md3" };
    int i;

    if (!q3p_valid_name(model))
        return false;

    if (Q_concat(path, sizeof(path), "players/", model, "/tris.md2") >= sizeof(path) || FS_FileExists(path))
        return false;

    for (i = 0; i < q_countof(files); i++) {
        if (Q_concat(path, sizeof(path), "models/players/", model, "/", files[i]) >= sizeof(path))
            return false;
        if (!FS_FileExists(path))
            return false;
    }
    return true;
}

static int skincmp(const void *a, const void *b)
{
    return Q_stricmp((const char *)a, (const char *)b);
}

/*
The model's skins: one for each lower_<skin>.skin, "default" first and the
rest in alphabetical order. The md3 patch (Q3P_PatchMD3) and the skin lookup
both come here, so a skin's place in the list is its skinnum.
*/
int Q3P_SkinList(const char *model, char (*out)[MAX_QPATH], int max)
{
    char    path[MAX_QPATH];
    void    **list;
    int     i, j, n, count = 0;

    if (max < 1 || !q3p_valid_name(model))
        return 0;

    Q_concat(path, sizeof(path), "models/players/", model, "/lower_default.skin");
    if (FS_FileExists(path))
        Q_strlcpy(out[count++], "default", MAX_QPATH);

    Q_concat(path, sizeof(path), "models/players/", model);
    list = FS_ListFiles(path, ".skin", 0, &n);
    for (i = 0; list && i < n && count < max; i++) {
        const char *name = list[i];
        char skin[MAX_QPATH];
        size_t len;

        if (Q_strncasecmp(name, "lower_", 6))
            continue;
        Q_strlcpy(skin, name + 6, sizeof(skin));
        len = strlen(skin);
        if (len <= 5 || Q_stricmp(skin + len - 5, ".skin"))
            continue;
        skin[len - 5] = 0;
        Q_strlwr(skin);

        for (j = 0; j < count; j++)
            if (!Q_stricmp(out[j], skin))
                break;
        if (j < count)
            continue;
        Q_strlcpy(out[count++], skin, MAX_QPATH);
    }
    if (list)
        FS_FreeList(list);

    // "default" stays first
    if (count > 1) {
        int start = Q_stricmp(out[0], "default") ? 0 : 1;
        qsort(out + start, count - start, MAX_QPATH, skincmp);
    }
    return count;
}

static bool q3p_parse_anims(q3player_t *q3, const char *path, const char *text)
{
    const char  *p = text;
    char        *tok;
    int         i, skip = 0;

    q3->sex = 'm';

    // the optional keywords, until the first number
    while (1) {
        const char *prev = p;

        tok = COM_Parse(&p);
        if (!*tok) {
            Com_WPrintf("%s: no animations\n", path);
            return false;
        }
        if (!Q_stricmp(tok, "footsteps")) {
            COM_Parse(&p);
            continue;
        }
        if (!Q_stricmp(tok, "headoffset")) {
            for (i = 0; i < 3; i++)
                q3->headoffset[i] = (float)atof(COM_Parse(&p));
            continue;
        }
        if (!Q_stricmp(tok, "sex")) {
            tok = COM_Parse(&p);
            q3->sex = Q_tolower(tok[0]) == 'f' ? 'f' : Q_tolower(tok[0]) == 'n' ? 'n' : 'm';
            continue;
        }
        if (!Q_stricmp(tok, "fixedlegs")) {
            q3->fixedlegs = true;
            continue;
        }
        if (!Q_stricmp(tok, "fixedtorso")) {
            q3->fixedtorso = true;
            continue;
        }
        if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-') {
            p = prev;
            break;
        }
        // anything else (Team Arena's keywords) is skipped
    }

    for (i = 0; i < Q3A_FILE_COUNT; i++) {
        q3panim_t *a = &q3->anims[i];

        tok = COM_Parse(&p);
        if (!*tok) {
            Com_WPrintf("%s: only %d animations\n", path, i);
            return false;
        }
        a->first = atoi(tok);

        // the legs' frames are numbered as if after the torso's, but lower.md3
        // does not have the torso-only ones (Quake III's CG_ParseAnimationFile)
        if (i == Q3A_LEGS_WALKCR)
            skip = a->first - q3->anims[Q3A_TORSO_GESTURE].first;
        if (i >= Q3A_LEGS_WALKCR)
            a->first -= skip;

        a->num = atoi(COM_Parse(&p));
        if (a->num < 0) {
            a->num = -a->num;
            a->reversed = true;
        }
        if (a->num < 1)
            a->num = 1;
        a->loop = atoi(COM_Parse(&p));
        clamp(a->loop, 0, a->num);
        a->fps = atof(COM_Parse(&p));
        if (a->fps <= 0)
            a->fps = 1;
    }

    q3->anims[Q3A_LEGS_BACKCR] = q3->anims[Q3A_LEGS_WALKCR];
    q3->anims[Q3A_LEGS_BACKCR].reversed = !q3->anims[Q3A_LEGS_WALKCR].reversed;
    q3->anims[Q3A_LEGS_BACKWALK] = q3->anims[Q3A_LEGS_WALK];
    q3->anims[Q3A_LEGS_BACKWALK].reversed = !q3->anims[Q3A_LEGS_WALK].reversed;
    return true;
}

// the named tags of an .md3, per frame; *out[i] stays NULL for one it lacks
static bool q3p_load_tags(const char *path, int *num_frames, int count,
                          const char *const *names, q3ptag_t **out)
{
    byte            *data;
    int             len, f, t, i;
    dmd3header_t    h;
    bool            ok = false;

    for (i = 0; i < count; i++)
        out[i] = NULL;

    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return false;

    if (len < (int)sizeof(h))
        goto done;
    memcpy(&h, data, sizeof(h));
    if (LittleLong(h.ident) != MD3_IDENT || LittleLong(h.version) != MD3_VERSION)
        goto done;
    h.num_frames = LittleLong(h.num_frames);
    h.num_tags = LittleLong(h.num_tags);
    h.ofs_tags = LittleLong(h.ofs_tags);
    if (h.num_frames < 1 || h.num_frames > MD3_MAX_FRAMES || h.num_tags > MD3_MAX_TAGS)
        goto done;
    if ((uint64_t)h.ofs_tags + (uint64_t)h.num_frames * h.num_tags * sizeof(dmd3tag_t) > (uint64_t)len)
        goto done;

    for (i = 0; i < count; i++) {
        const dmd3tag_t *src = (const dmd3tag_t *)(data + h.ofs_tags);

        for (t = 0; t < (int)h.num_tags; t++)
            if (!Q_strncasecmp(src[t].name, names[i], MD3_MAX_PATH))
                break;
        if (t == (int)h.num_tags)
            continue;

        out[i] = Z_Mallocz(sizeof(q3ptag_t) * h.num_frames);
        for (f = 0; f < (int)h.num_frames; f++) {
            const dmd3tag_t *tag = src + f * h.num_tags + t;
            int k;

            for (k = 0; k < 3; k++) {
                out[i][f].origin[k] = LittleFloat(tag->origin[k]);
                out[i][f].axis[0][k] = LittleFloat(tag->axis[0][k]);
                out[i][f].axis[1][k] = LittleFloat(tag->axis[1][k]);
                out[i][f].axis[2][k] = LittleFloat(tag->axis[2][k]);
            }
        }
    }

    *num_frames = h.num_frames;
    ok = true;

done:
    FS_FreeFile(data);
    return ok;
}

static q3player_t *q3p_load(const char *model)
{
    char        path[MAX_QPATH];
    char        *text;
    q3player_t  *q3;
    q3ptag_t    *tags[2];
    static const char *const lower_tags[] = { "tag_torso" };
    static const char *const upper_tags[] = { "tag_head", "tag_weapon" };
    int         i;

    if (!Q3P_ModelExists(model))
        return NULL;

    q3 = Z_Mallocz(sizeof(*q3));
    Q_strlcpy(q3->name, model, sizeof(q3->name));
    Q_strlwr(q3->name);

    Q_concat(path, sizeof(path), "models/players/", model, "/animation.cfg");
    if (FS_LoadFile(path, (void **)&text) < 0 || !text) {
        Z_Free(q3);
        return NULL;
    }
    // FS_LoadFile terminates what it reads
    i = q3p_parse_anims(q3, path, text);
    FS_FreeFile(text);
    if (!i) {
        Z_Free(q3);
        return NULL;
    }

    Q_concat(path, sizeof(path), "models/players/", model, "/lower.md3");
    if (!q3p_load_tags(path, &q3->lower_frames, 1, lower_tags, tags) || !tags[0]) {
        Com_WPrintf("%s: no tag_torso\n", path);
        Z_Free(tags[0]);
        Z_Free(q3);
        return NULL;
    }
    q3->tag_torso = tags[0];

    Q_concat(path, sizeof(path), "models/players/", model, "/upper.md3");
    if (!q3p_load_tags(path, &q3->upper_frames, 2, upper_tags, tags) || !tags[0]) {
        Com_WPrintf("%s: no tag_head\n", path);
        Z_Free(tags[0]);
        Z_Free(tags[1]);
        Z_Free(q3->tag_torso);
        Z_Free(q3);
        return NULL;
    }
    q3->tag_head = tags[0];
    q3->tag_weapon = tags[1];   // NULL: no gun drawn

    q3->numskins = Q3P_SkinList(model, q3->skins, Q3P_MAX_SKINS);
    if (!q3->numskins) {
        // no .skin at all: the md3s' own shaders
        Q_strlcpy(q3->skins[0], "default", MAX_QPATH);
        q3->numskins = 1;
    }

    // keep every animation inside its model
    for (i = 0; i < Q3A_COUNT; i++) {
        q3panim_t *a = &q3->anims[i];
        int frames = (i >= Q3A_LEGS_WALKCR || i <= Q3A_BOTH_DEAD3) ? q3->lower_frames : q3->upper_frames;

        if (i <= Q3A_BOTH_DEAD3)
            frames = min(q3->lower_frames, q3->upper_frames);
        if (a->first < 0 || a->first >= frames)
            a->first = 0;
        if (a->first + a->num > frames)
            a->num = max(1, frames - a->first);
        clamp(a->loop, 0, a->num);
    }

    if (cl_q3_debug && cl_q3_debug->integer)
        Com_Printf("Q3 player %s: sex %c, %d skins, %d/%d frames\n",
                   q3->name, q3->sex, q3->numskins, q3->lower_frames, q3->upper_frames);
    return q3;
}

q3player_t *Q3P_Get(const char *model)
{
    q3player_t *q3;
    int i;

    if (!q3p_valid_name(model))
        return NULL;

    for (i = 0; i < q3p_num_models; i++)
        if (!Q_stricmp(q3p_models[i]->name, model))
            return q3p_models[i];
    for (i = 0; i < q3p_num_missing; i++)
        if (!Q_stricmp(q3p_missing[i], model))
            return NULL;

    q3 = q3p_load(model);
    if (!q3) {
        if (q3p_num_missing < Q3P_MAX_MODELS)
            Q_strlcpy(q3p_missing[q3p_num_missing++], model, MAX_QPATH);
        return NULL;
    }
    if (q3p_num_models == Q3P_MAX_MODELS) {
        Com_WPrintf("Too many Quake III player models\n");
        return q3;  // works, just not cached (leaks one; never in practice)
    }
    q3p_models[q3p_num_models++] = q3;
    return q3;
}

const char *Q3P_ModelName(const q3player_t *q3)
{
    return q3->name;
}

// animation.cfg's headoffset: where the HUD's 3D head sits in its box, on top
// of the framing (hud_q3.c, as cg_draw.c CG_DrawHead)
void Q3P_HeadOffset(const q3player_t *q3, vec3_t out)
{
    VectorCopy(q3->headoffset, out);
}

char Q3P_Sex(const q3player_t *q3)
{
    return q3->sex;
}

int Q3P_NumSkins(const q3player_t *q3)
{
    return q3->numskins;
}

const char *Q3P_SkinName(const q3player_t *q3, int index)
{
    if (index < 0 || index >= q3->numskins)
        return "default";
    return q3->skins[index];
}

int Q3P_SkinIndex(const q3player_t *q3, const char *skin)
{
    int i;

    for (i = 0; i < q3->numskins; i++)
        if (!Q_stricmp(q3->skins[i], skin))
            return i;
    return -1;
}

bool Q3P_Register(q3pmodel_t *m, const char *model, const char *skin)
{
    char path[MAX_QPATH];

    memset(m, 0, sizeof(*m));
    m->q3 = Q3P_Get(model);
    if (!m->q3)
        return false;

    Q_concat(path, sizeof(path), "models/players/", m->q3->name, "/lower.md3");
    m->lower = R_RegisterModel(path);
    Q_concat(path, sizeof(path), "models/players/", m->q3->name, "/upper.md3");
    m->upper = R_RegisterModel(path);
    Q_concat(path, sizeof(path), "models/players/", m->q3->name, "/head.md3");
    m->head = R_RegisterModel(path);

    if (!m->lower || !m->upper || !m->head) {
        memset(m, 0, sizeof(*m));
        return false;
    }

    m->skin = Q3P_SkinIndex(m->q3, skin);
    if (m->skin < 0)
        m->skin = 0;
    return true;
}

/*
=============================================================================

EVERY SKIN ON ONE MODEL

A Quake III part's skins are .skin files - "surface,shader" lines - not the
shader names in the .md3. Each mesh here gets a skin list with an entry for
every skin of the model, in Q3P_SkinList's order, so one loaded model draws
in any of them by entity skinnum and a server full of Sarges in different
skins loads Sarge once. The list goes on the end of each mesh, so nothing
else in the file moves.

A surface a .skin leaves out takes the default skin's shader, then the mesh's
own.

=============================================================================
*/

#define Q3P_MAX_SURFS   32

typedef struct {
    int     count;
    char    surf[Q3P_MAX_SURFS][MD3_MAX_PATH];
    char    shader[Q3P_MAX_SURFS][MD3_MAX_PATH];
} q3pskinfile_t;

static void q3p_load_skin(const char *model, const char *part, const char *skin, q3pskinfile_t *sf)
{
    char    path[MAX_QPATH];
    char    *text, *line, *next;

    sf->count = 0;
    Q_snprintf(path, sizeof(path), "models/players/%s/%s_%s.skin", model, part, skin);
    if (FS_LoadFile(path, (void **)&text) < 0 || !text)
        return;

    for (line = text; line && *line; line = next) {
        char *comma, *end;

        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        end = line + strlen(line);
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        while (*line == ' ' || *line == '\t')
            line++;

        comma = strchr(line, ',');
        if (!comma || comma == line)
            continue;
        *comma++ = 0;
        while (*comma == ' ' || *comma == '\t')
            comma++;
        if (!*comma || !Q_strncasecmp(line, "tag_", 4))
            continue;
        if (sf->count == Q3P_MAX_SURFS)
            break;
        Q_strlcpy(sf->surf[sf->count], line, MD3_MAX_PATH);
        Q_strlcpy(sf->shader[sf->count], comma, MD3_MAX_PATH);
        sf->count++;
    }
    FS_FreeFile(text);
}

static const char *q3p_skin_shader(const q3pskinfile_t *sf, const char *surf)
{
    int i;

    for (i = 0; i < sf->count; i++)
        if (!Q_stricmp(sf->surf[i], surf))
            return sf->shader[i];
    return NULL;
}

bool Q3P_PatchMD3(const char *path, byte **data, int *len)
{
    char            model[MAX_QPATH], part[16];
    char            skins[Q3P_MAX_SKINS][MAX_QPATH];
    q3pskinfile_t   *sf;
    const char      *s, *slash;
    const byte      *src = *data;
    byte            *out;
    dmd3header_t    h;
    size_t          srclen = *len, outlen, off, dst;
    int             numskins, i, k;

    // models/players/<model>/<part>.md3
    if (Q_strncasecmp(path, "models/players/", 15))
        return false;
    s = path + 15;
    slash = strchr(s, '/');
    if (!slash || slash == s || (size_t)(slash - s) >= sizeof(model))
        return false;
    memcpy(model, s, slash - s);
    model[slash - s] = 0;
    s = slash + 1;
    if (!Q_stricmp(s, "lower.md3"))
        strcpy(part, "lower");
    else if (!Q_stricmp(s, "upper.md3"))
        strcpy(part, "upper");
    else if (!Q_stricmp(s, "head.md3"))
        strcpy(part, "head");
    else
        return false;

    if (!Q3P_ModelExists(model))
        return false;
    numskins = Q3P_SkinList(model, skins, Q3P_MAX_SKINS);
    if (numskins < 1)
        return false;

    if (srclen < sizeof(h))
        return false;
    memcpy(&h, src, sizeof(h));
    if (LittleLong(h.ident) != MD3_IDENT || LittleLong(h.version) != MD3_VERSION)
        return false;
    h.num_frames = LittleLong(h.num_frames);
    h.num_tags = LittleLong(h.num_tags);
    h.num_meshes = LittleLong(h.num_meshes);
    h.ofs_frames = LittleLong(h.ofs_frames);
    h.ofs_tags = LittleLong(h.ofs_tags);
    h.ofs_meshes = LittleLong(h.ofs_meshes);
    if (h.num_meshes < 1 || h.num_meshes > MD3_MAX_MESHES || h.ofs_meshes > srclen)
        return false;

    // the meshes move, so everything else has to come before them (it does
    // in every id model; an exporter that put the frames last is left alone)
    if ((uint64_t)h.ofs_frames + (uint64_t)h.num_frames * sizeof(dmd3frame_t) > h.ofs_meshes
        || (uint64_t)h.ofs_tags + (uint64_t)h.num_frames * h.num_tags * sizeof(dmd3tag_t) > h.ofs_meshes)
        return false;

    // every .skin of this part, read once
    sf = Z_Malloc(sizeof(*sf) * numskins);
    for (k = 0; k < numskins; k++)
        q3p_load_skin(model, part, skins[k], &sf[k]);

    outlen = srclen + (size_t)h.num_meshes * numskins * sizeof(dmd3skin_t);
    out = Z_Malloc(outlen);
    memcpy(out, src, h.ofs_meshes);

    off = dst = h.ofs_meshes;
    for (i = 0; i < (int)h.num_meshes; i++) {
        dmd3mesh_t  mh;
        dmd3skin_t  *skin;
        char        surf[MD3_MAX_PATH], own[MD3_MAX_PATH];
        uint32_t    meshsize, num_skins, ofs_skins;

        if (off + sizeof(mh) > srclen)
            goto fail;
        memcpy(&mh, src + off, sizeof(mh));
        meshsize = LittleLong(mh.meshsize);
        num_skins = LittleLong(mh.num_skins);
        ofs_skins = LittleLong(mh.ofs_skins);
        if (meshsize < sizeof(mh) || off + meshsize > srclen || (meshsize & 3))
            goto fail;

        memcpy(out + dst, src + off, meshsize);

        Q_strlcpy(surf, mh.name, sizeof(surf));
        own[0] = 0;
        if (num_skins && (uint64_t)ofs_skins + sizeof(dmd3skin_t) <= meshsize)
            Q_strlcpy(own, ((const dmd3skin_t *)(src + off + ofs_skins))->name, sizeof(own));

        skin = (dmd3skin_t *)(out + dst + meshsize);
        for (k = 0; k < numskins; k++) {
            const char *shader = q3p_skin_shader(&sf[k], surf);

            if (!shader)
                shader = q3p_skin_shader(&sf[0], surf);
            if (!shader)
                shader = own;
            memset(&skin[k], 0, sizeof(skin[k]));
            Q_strlcpy(skin[k].name, shader, sizeof(skin[k].name));
        }

        mh.num_skins = LittleLong(numskins);
        mh.ofs_skins = LittleLong(meshsize);
        mh.meshsize = LittleLong(meshsize + numskins * (uint32_t)sizeof(dmd3skin_t));
        memcpy(out + dst, &mh, sizeof(mh));

        off += meshsize;
        dst += meshsize + numskins * sizeof(dmd3skin_t);
    }

    h.ofs_end = LittleLong((uint32_t)dst);
    memcpy(out + offsetof(dmd3header_t, ofs_end), &h.ofs_end, sizeof(h.ofs_end));

    Z_Free(sf);
    FS_FreeFile(*data);
    *data = out;
    *len = (int)dst;
    return true;

fail:
    Com_WPrintf("%s: bad mesh, skins left as they are\n", path);
    Z_Free(sf);
    Z_Free(out);
    return false;
}

/*
=============================================================================

ANIMATION

=============================================================================
*/

// the Quake II player frames (src/baseq2/m_player.h) as what they show
typedef enum {
    Q2F_STAND,
    Q2F_RUN,
    Q2F_ATTACK,
    Q2F_PAIN,
    Q2F_JUMP,
    Q2F_GESTURE,    // flip, salute, taunt, wave, point
    Q2F_CRSTAND,
    Q2F_CRWALK,
    Q2F_CRATTACK,
    Q2F_CRPAIN,
    Q2F_DEATH,
} q2frameclass_t;

static q2frameclass_t q2_frame_class(int f, int *death, bool *last)
{
    *death = 0;
    *last = false;
    if (f < 40)  return Q2F_STAND;
    if (f < 46)  return Q2F_RUN;
    if (f < 54)  return Q2F_ATTACK;
    if (f < 66)  return Q2F_PAIN;
    if (f < 72)  return Q2F_JUMP;
    if (f < 135) return Q2F_GESTURE;
    if (f < 154) return Q2F_CRSTAND;
    if (f < 160) return Q2F_CRWALK;
    if (f < 169) return Q2F_CRATTACK;
    if (f < 173) return Q2F_CRPAIN;
    if (f < 178) { *death = 1; *last = f == 177; return Q2F_DEATH; }    // crdeath
    if (f < 184) { *death = 1; *last = f == 183; return Q2F_DEATH; }
    if (f < 190) { *death = 2; *last = f == 189; return Q2F_DEATH; }
    if (f < 198) { *death = 3; *last = f == 197; return Q2F_DEATH; }
    return Q2F_STAND;
}

static bool q2_crouched(q2frameclass_t c)
{
    return c == Q2F_CRSTAND || c == Q2F_CRWALK || c == Q2F_CRATTACK || c == Q2F_CRPAIN;
}

static bool anim_is_oneshot(int anim)
{
    return anim == Q3A_TORSO_ATTACK || anim == Q3A_TORSO_ATTACK2 || anim == Q3A_TORSO_GESTURE
        || anim == Q3A_TORSO_DROP || anim == Q3A_TORSO_RAISE;
}

static int anim_frame(const q3panim_t *a, int i)
{
    if (i >= a->num) {
        if (a->loop > 0)
            i = a->num - a->loop + (i - a->num) % a->loop;
        else
            i = a->num - 1;
    }
    if (i < 0)
        i = 0;
    return a->reversed ? a->first + a->num - 1 - i : a->first + i;
}

// start an animation; the change blends over one of its frames (Quake III's
// initialLerp). restart begins it again when it is already playing.
static void lerp_set(q3plerp_t *lf, const q3player_t *q3, int anim, bool restart)
{
    const q3panim_t *a = &q3->anims[anim];

    if (lf->anim == anim && !restart)
        return;

    if (lf->anim >= 0) {
        lf->from_frame = lf->backlerp < 0.5f ? lf->frame : lf->oldframe;
        lf->blend = 0;
        lf->blend_rate = a->fps * 0.001f;
    } else {
        lf->from_frame = -1;
    }
    lf->anim = anim;
    lf->phase = 0;
    lf->done = false;
}

// on through the current animation; speed scales its rate
static void lerp_run(q3plerp_t *lf, const q3player_t *q3, float dt, float speed)
{
    const q3panim_t *a = &q3->anims[lf->anim];
    float   frac;
    int     i;

    lf->phase += dt * 0.001f * a->fps * speed;
    if (a->loop <= 0) {
        if (lf->phase >= a->num - 1) {
            lf->phase = a->num - 1;
            lf->done = true;
        }
    } else if (lf->phase >= a->num) {
        lf->phase = a->num - a->loop + fmodf(lf->phase - a->num, (float)a->loop);
    }

    i = (int)lf->phase;
    frac = lf->phase - i;

    if (lf->from_frame >= 0) {
        lf->blend += dt * lf->blend_rate;
        if (lf->blend < 1.0f) {
            lf->oldframe = lf->from_frame;
            lf->frame = anim_frame(a, i);
            lf->backlerp = 1.0f - lf->blend;
            return;
        }
        lf->from_frame = -1;
    }

    lf->oldframe = anim_frame(a, i);
    lf->frame = anim_frame(a, i + 1);
    lf->backlerp = 1.0f - frac;
    if (lf->done) {
        lf->frame = lf->oldframe;
        lf->backlerp = 0;
    }
}

// a - b, in -180..180
static float angle_sub(float a, float b)
{
    float d = fmodf(a - b, 360.0f);

    if (d > 180)
        d -= 360;
    else if (d < -180)
        d += 360;
    return d;
}

static float angle_norm(float a)
{
    a = fmodf(a, 360.0f);
    if (a < 0)
        a += 360;
    return a;
}

// Quake III's CG_SwingAngles: turn toward dest once off by more than
// tolerance, faster the further off, never more than clamp_tol behind
static void swing_angle(float dest, float tolerance, float clamp_tol, float speed, float dt,
                        float *angle, bool *swinging)
{
    float swing, move, scale;

    if (!*swinging) {
        swing = angle_sub(*angle, dest);
        if (swing > tolerance || swing < -tolerance)
            *swinging = true;
    }

    if (*swinging) {
        swing = angle_sub(dest, *angle);
        scale = fabsf(swing);
        if (scale < tolerance * 0.5f)
            scale = 0.5f;
        else if (scale < tolerance)
            scale = 1.0f;
        else
            scale = 2.0f;

        move = dt * scale * speed;
        if (swing >= 0) {
            if (move >= swing) {
                move = swing;
                *swinging = false;
            }
        } else {
            move = -move;
            if (move <= swing) {
                move = swing;
                *swinging = false;
            }
        }
        *angle = angle_norm(*angle + move);
    }

    swing = angle_sub(dest, *angle);
    if (swing > clamp_tol)
        *angle = angle_norm(dest - (clamp_tol - 1));
    else if (swing < -clamp_tol)
        *angle = angle_norm(dest + (clamp_tol - 1));
}

// the way it moves against the way it faces, as Quake III's movementDir:
// 0 forward, 1 forward-left, 2 left, 3 back-left, 4 back, 5 back-right,
// 6 right, 7 forward-right
static int movement_dir(const vec3_t velocity, float yaw)
{
    float rel = angle_sub(RAD2DEG(atan2f(velocity[1], velocity[0])), yaw);
    int dir = (int)floorf((rel + 22.5f) / 45.0f);

    return dir & 7;
}

// Quake III's movementOffsets: the legs turn this far toward the movement
static const float movement_offsets[8] = { 0, 22, 45, -22, 0, 22, -45, -22 };

#define Q3P_PAIN_TWITCH_TIME    200
#define Q3P_LAND_TIME           130     // Quake III's TIMER_LAND
#define Q3P_FIRE_HOLD           300     // ms a muzzle flash speaks for the frames

static void q3p_reset(q3pstate_t *st, const q3pmodel_t *m, const q3pinput_t *in)
{
    memset(st, 0, sizeof(*st));
    st->valid = true;
    st->q3 = m->q3;
    st->time = in->time;
    st->legs.anim = st->torso.anim = -1;
    st->legs.from_frame = st->torso.from_frame = -1;
    st->torso_yaw = st->legs_yaw = angle_norm(in->view[YAW]);
    st->onground = true;
    st->q2frame = -1;
    st->fire_time = -100000;
    st->weapon_shown = in->weapon;
}

// the drawn parts' axes multiply as Quake III's: rows are the basis vectors,
// child in parent = local x tag x parent
static void axis_mul(const vec3_t a[3], const vec3_t b[3], vec3_t out[3])
{
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

// back to angles for entity_t, which the renderers turn into this same axis
// (AnglesToAxis: forward, left, up)
static void axis_to_angles(const vec3_t axis[3], vec3_t angles)
{
    float len = sqrtf(axis[0][0] * axis[0][0] + axis[0][1] * axis[0][1]);

    if (len > 1e-6f) {
        angles[YAW] = RAD2DEG(atan2f(axis[0][1], axis[0][0]));
        angles[PITCH] = RAD2DEG(atan2f(-axis[0][2], len));
        angles[ROLL] = RAD2DEG(atan2f(axis[1][2], axis[2][2]));
    } else {
        angles[PITCH] = axis[0][2] > 0 ? -90.0f : 90.0f;
        angles[YAW] = RAD2DEG(atan2f(-axis[1][0], axis[1][1]));
        angles[ROLL] = 0;
    }
}

static void lerp_tag(const q3ptag_t *tags, int frames, int oldframe, int frame, float backlerp, q3ptag_t *out)
{
    const q3ptag_t *a, *b;
    float front = 1.0f - backlerp;
    int i;

    clamp(oldframe, 0, frames - 1);
    clamp(frame, 0, frames - 1);
    a = &tags[oldframe];
    b = &tags[frame];

    for (i = 0; i < 3; i++) {
        out->origin[i] = a->origin[i] * backlerp + b->origin[i] * front;
        out->axis[0][i] = a->axis[0][i] * backlerp + b->axis[0][i] * front;
        out->axis[1][i] = a->axis[1][i] * backlerp + b->axis[1][i] * front;
        out->axis[2][i] = a->axis[2][i] * backlerp + b->axis[2][i] * front;
    }
    VectorNormalize(out->axis[0]);
    VectorNormalize(out->axis[1]);
    VectorNormalize(out->axis[2]);
}

// place a child on a parent's tag: origin and axis in the world
static void on_tag(const vec3_t parent_origin, const vec3_t parent_axis[3], const q3ptag_t *tag,
                   const vec3_t local[3], vec3_t origin, vec3_t axis[3])
{
    vec3_t temp[3];
    int i;

    VectorCopy(parent_origin, origin);
    for (i = 0; i < 3; i++)
        VectorMA(origin, tag->origin[i], parent_axis[i], origin);

    axis_mul(local, tag->axis, temp);
    axis_mul(temp, parent_axis, axis);
}

/*
THE GRIP: the Quake II male's hand at frame 0 - the rerelease w_*.md5mesh
joint "Weapon", the same in all 21 of them - in Quake III's tag_weapon frame,
whose x is along the barrel and z up. The joint has its y along the barrel
and its x down, so tag = (y, -z, -x) of the joint. Worked out from the joint's
quaternion (-0.4904857576 -0.4952283204 0.5859329700) at
(-7.6532745361 -7.4095697403 9.4494590759); see the 11-17 session notes.
Rows: the md2's x, y and z axes in tag space. The guns land 0-33 units ahead
of the hand along x, as Quake III's own do.
*/
static const vec3_t q3p_grip_axis[3] = {
    {  0.970191f,  0.165382f,  0.177137f },
    { -0.167787f,  0.985822f, -0.001418f },
    { -0.174860f, -0.028345f,  0.984185f },
};
static const vec3_t q3p_grip_origin = { 7.834243f, 8.838079f, -7.954849f };

static void q3p_gun_transform(vec3_t axis[3], vec3_t origin, float *scale)
{
    vec3_t  tune_angles, tune_axis[3], grip[3];
    float   s = cl_q3_gun_scale->value;
    int     i;

    if (s <= 0)
        s = 1;
    *scale = s;

    // the tuning: a turn and an offset in the hand
    if (sscanf(cl_q3_gun_angles->string, "%f %f %f", &tune_angles[0], &tune_angles[1], &tune_angles[2]) != 3)
        VectorClear(tune_angles);
    AnglesToAxis(tune_angles, tune_axis);
    if (sscanf(cl_q3_gun_offset->string, "%f %f %f", &origin[0], &origin[1], &origin[2]) != 3)
        VectorClear(origin);

    // gun = tune x grip: the md2's axes, then its origin, in tag space
    axis_mul(q3p_grip_axis, tune_axis, grip);
    for (i = 0; i < 3; i++)
        VectorCopy(grip[i], axis[i]);
    for (i = 0; i < 3; i++)
        VectorMA(origin, q3p_grip_origin[i] * s, tune_axis[i], origin);
}

static void q3p_set_entity(entity_t *e, const entity_t *base, qhandle_t model, int skin,
                           const vec3_t origin, const vec3_t axis[3], const q3plerp_t *lf)
{
    *e = *base;
    e->model = model;
    e->skin = 0;            // the mesh's skin list, by skinnum
    e->skinnum = skin;
    e->scale = 0;
    e->blood_key = 0;
    VectorCopy(origin, e->origin);
    VectorCopy(origin, e->oldorigin);
    axis_to_angles(axis, e->angles);
    if (lf) {
        e->frame = lf->frame;
        e->oldframe = lf->oldframe;
        e->backlerp = lf->backlerp;
    } else {
        e->frame = e->oldframe = 0;
        e->backlerp = 0;
    }
}

int Q3P_Animate(q3pstate_t *st, const q3pmodel_t *m, const q3pinput_t *in,
                const entity_t *base, entity_t *out)
{
    const q3player_t    *q3 = m->q3;
    q2frameclass_t      fc, prevfc = Q2F_STAND;
    int                 death, prevdeath = 0, dir, n;
    bool                last, prevlast, dead, crouched, back;
    float               dt, speed, legs_speed = 1.0f;
    vec3_t              flatvel, head_angles, torso_angles, legs_angles;
    vec3_t              legs_axis[3], torso_rel[3], head_rel[3], torso_axis[3], head_axis[3];
    vec3_t              torso_origin, head_origin;
    q3ptag_t            tag;

    if (!q3 || !m->lower)
        return 0;

    if (!st->valid || st->q3 != q3 || in->time < st->time || in->time - st->time > 1000)
        q3p_reset(st, m, in);

    dt = (float)(in->time - st->time);
    st->time = in->time;

    // the body's motion, smoothed over a few frames: a Quake II server sends
    // positions 10 (or 40) times a second
    if (dt > 0) {
        float k = 1.0f - expf(-dt / 50.0f);

        st->velocity[0] += (in->velocity[0] - st->velocity[0]) * k;
        st->velocity[1] += (in->velocity[1] - st->velocity[1]) * k;
        st->velocity[2] = in->velocity[2];
    }
    VectorSet(flatvel, st->velocity[0], st->velocity[1], 0);
    speed = VectorLength(flatvel);

    fc = q2_frame_class(in->q2frame, &death, &last);
    if (st->q2frame >= 0)
        prevfc = q2_frame_class(st->q2frame, &prevdeath, &prevlast);
    dead = fc == Q2F_DEATH;
    crouched = q2_crouched(fc);

    dir = speed > 10 ? movement_dir(flatvel, in->view[YAW]) : 0;
    back = speed > 10 && dir >= 3 && dir <= 5;

    // ---- events from the Quake II frames ----

    if (st->q2frame >= 0 && !dead) {
        bool attack = fc == Q2F_ATTACK || fc == Q2F_CRATTACK;
        bool was_attack = prevfc == Q2F_ATTACK || prevfc == Q2F_CRATTACK;

        // an attack with no muzzle flash (a hand grenade, the chainfist)
        if (attack && (!was_attack || in->q2frame < st->q2frame)
            && in->time - st->fire_time > Q3P_FIRE_HOLD)
            st->fire_pending = true;

        if ((fc == Q2F_PAIN || fc == Q2F_CRPAIN) && prevfc != fc) {
            st->pain_time = in->time;
            st->pain_dir = !st->pain_dir;
        }

        if (fc == Q2F_GESTURE && prevfc != Q2F_GESTURE)
            st->gesture_pending = true;
    }
    if (in->fired && !dead) {
        st->fire_pending = true;
        st->fire_time = in->time;
    }
    if (in->gesture)
        st->gesture_pending = true;

    // ---- torso ----

    if (dead) {
        int anim = death == 2 ? Q3A_BOTH_DEATH2 : death == 3 ? Q3A_BOTH_DEATH3 : Q3A_BOTH_DEATH1;

        if (st->torso.anim != anim) {
            lerp_set(&st->torso, q3, anim, false);
            lerp_set(&st->legs, q3, anim, false);
            // a body seen for the first time already down (the body queue,
            // or coming into view) lies there; it does not die again
            if (st->q2frame < 0 || (last && st->q2frame == in->q2frame)) {
                st->torso.phase = st->legs.phase = q3->anims[anim].num - 1;
                st->torso.from_frame = st->legs.from_frame = -1;
            }
        }
        st->weapon_shown = st->weapon_pending = 0;
        st->fire_pending = st->gesture_pending = false;
    } else {
        int stand = in->melee ? Q3A_TORSO_STAND2 : Q3A_TORSO_STAND;
        int ta = st->torso.anim;

        // up from the dead (respawn): straight into the stand
        if (ta <= Q3A_BOTH_DEAD3) {
            st->weapon_shown = in->weapon;
            st->weapon_pending = 0;
            lerp_set(&st->torso, q3, stand, false);
            ta = stand;
        }

        // a weapon change: put the old one down, then the new one up
        if (in->weapon != st->weapon_shown && ta != Q3A_TORSO_DROP) {
            if (ta == Q3A_TORSO_RAISE || !st->weapon_shown || !in->weapon) {
                st->weapon_shown = in->weapon;      // nothing to put down
            } else {
                st->weapon_pending = in->weapon;
                lerp_set(&st->torso, q3, Q3A_TORSO_DROP, false);
                ta = Q3A_TORSO_DROP;
            }
        } else if (ta == Q3A_TORSO_DROP) {
            st->weapon_pending = in->weapon;    // may have changed again
        }

        if (st->fire_pending && ta != Q3A_TORSO_DROP && ta != Q3A_TORSO_RAISE) {
            lerp_set(&st->torso, q3, in->melee ? Q3A_TORSO_ATTACK2 : Q3A_TORSO_ATTACK, true);
            ta = st->torso.anim;
            st->fire_pending = false;
            st->gesture_pending = false;
        }
        if (st->gesture_pending && !anim_is_oneshot(ta)) {
            lerp_set(&st->torso, q3, Q3A_TORSO_GESTURE, false);
            ta = Q3A_TORSO_GESTURE;
        }
        st->gesture_pending = false;
        if (ta == Q3A_TORSO_DROP || ta == Q3A_TORSO_RAISE)
            st->fire_pending = false;   // no shooting while changing

        if (anim_is_oneshot(ta) && st->torso.done) {
            if (ta == Q3A_TORSO_DROP) {
                st->weapon_shown = st->weapon_pending;
                st->weapon_pending = 0;
                lerp_set(&st->torso, q3, Q3A_TORSO_RAISE, false);
            } else {
                lerp_set(&st->torso, q3, stand, false);
            }
        } else if (!anim_is_oneshot(ta) && ta != stand) {
            lerp_set(&st->torso, q3, stand, false);
        }
    }

    // ---- legs ----

    if (!dead) {
        bool was_onground = st->onground;

        st->onground = in->onground;

        if (!in->onground) {
            if (was_onground)
                st->air_since = in->time;

            if (in->swimming) {
                st->jump_anim = 0;
                lerp_set(&st->legs, q3, Q3A_LEGS_SWIM, false);
            } else if (st->jump_anim) {
                lerp_set(&st->legs, q3, st->jump_anim, false);
            } else if (in->velocity[2] > 100 || in->time - st->air_since > 200) {
                // going up (a jump, a jump pad, a rocket) or falling long
                // enough to show - Quake III waits for 64 units of drop, so
                // a step or a stair down never gets here
                st->jump_anim = back ? Q3A_LEGS_JUMPB : Q3A_LEGS_JUMP;
                lerp_set(&st->legs, q3, st->jump_anim, true);
            }
        } else {
            if (st->jump_anim) {
                st->land_anim = st->jump_anim == Q3A_LEGS_JUMPB ? Q3A_LEGS_LANDB : Q3A_LEGS_LAND;
                // standing still the whole landing shows; moving, Quake III's
                // 130 ms before the run takes over
                st->land_until = in->time + (speed > 10 ? Q3P_LAND_TIME
                    : (int)(1000.0f * q3->anims[st->land_anim].num / q3->anims[st->land_anim].fps));
                st->jump_anim = 0;
                lerp_set(&st->legs, q3, st->land_anim, true);
            }

            if (in->time < st->land_until) {
                lerp_set(&st->legs, q3, st->land_anim, false);
            } else if (crouched) {
                if (speed < 10)
                    lerp_set(&st->legs, q3, Q3A_LEGS_IDLECR, false);
                else {
                    // Quake III crouch-walks at 80 (320 x pm_duckScale)
                    lerp_set(&st->legs, q3, back ? Q3A_LEGS_BACKCR : Q3A_LEGS_WALKCR, false);
                    legs_speed = speed / 80.0f;
                }
            } else if (speed < 10) {
                lerp_set(&st->legs, q3, st->legs_yawing ? Q3A_LEGS_TURN : Q3A_LEGS_IDLE, false);
            } else if (speed > 220) {
                // the run is drawn for Quake III's 320
                lerp_set(&st->legs, q3, back ? Q3A_LEGS_BACK : Q3A_LEGS_RUN, false);
                legs_speed = speed / 320.0f;
            } else {
                // and the walk for its 160
                lerp_set(&st->legs, q3, back ? Q3A_LEGS_BACKWALK : Q3A_LEGS_WALK, false);
                legs_speed = speed / 160.0f;
            }
        }
        clamp(legs_speed, 0.5f, 2.0f);
    }

    st->q2frame = in->q2frame;

    lerp_run(&st->legs, q3, dt, legs_speed);
    lerp_run(&st->torso, q3, dt, 1.0f);

    // ---- angles (Quake III's CG_PlayerAngles) ----

    VectorSet(head_angles, in->view[PITCH], angle_norm(in->view[YAW]), 0);
    clamp(head_angles[PITCH], -90.0f, 90.0f);

    if (st->legs.anim != Q3A_LEGS_IDLE
        || (st->torso.anim != Q3A_TORSO_STAND && st->torso.anim != Q3A_TORSO_STAND2)) {
        // moving or busy: everything lines up
        st->torso_yawing = st->torso_pitching = st->legs_yawing = true;
    }

    if (dead)
        dir = 0;
    {
        float swing_speed = cl_q3_swing_speed->value;
        float legs_dest = head_angles[YAW] + movement_offsets[dir];
        float torso_dest = head_angles[YAW] + 0.25f * movement_offsets[dir];
        float pitch_dest = head_angles[PITCH] * 0.75f;
        float swing_dt = min(dt, 100.0f);

        swing_angle(torso_dest, 25, 90, swing_speed, swing_dt, &st->torso_yaw, &st->torso_yawing);
        swing_angle(legs_dest, 40, 90, swing_speed, swing_dt, &st->legs_yaw, &st->legs_yawing);
        swing_angle(pitch_dest, 15, 30, 0.1f, swing_dt, &st->torso_pitch, &st->torso_pitching);
    }

    VectorSet(torso_angles, st->torso_pitch, st->torso_yaw, 0);
    VectorSet(legs_angles, 0, st->legs_yaw, 0);

    if (dead) {
        // a body lies as it fell
        torso_angles[PITCH] = 0;
        head_angles[PITCH] = 0;
        head_angles[YAW] = st->torso_yaw;
    } else if (speed > 1 && in->onground) {
        // lean into the run
        vec3_t axis[3], v;
        float side, s = speed * 0.05f;

        VectorScale(flatvel, 1.0f / speed, v);
        AnglesToAxis(legs_angles, axis);
        side = s * DotProduct(v, axis[1]);
        legs_angles[ROLL] -= side;
        side = s * DotProduct(v, axis[0]);
        legs_angles[PITCH] += side;
    }

    if (q3->fixedlegs) {
        legs_angles[YAW] = torso_angles[YAW];
        legs_angles[PITCH] = legs_angles[ROLL] = 0;
    }
    if (q3->fixedtorso)
        torso_angles[PITCH] = 0;

    // a hit twitches the torso
    if (!dead && in->time - st->pain_time < Q3P_PAIN_TWITCH_TIME) {
        float f = 1.0f - (float)(in->time - st->pain_time) / Q3P_PAIN_TWITCH_TIME;

        torso_angles[ROLL] += st->pain_dir ? 20 * f : -20 * f;
    }

    // each part relative to the one it hangs on
    {
        vec3_t rel;

        rel[0] = angle_sub(head_angles[0], torso_angles[0]);
        rel[1] = angle_sub(head_angles[1], torso_angles[1]);
        rel[2] = angle_sub(head_angles[2], torso_angles[2]);
        AnglesToAxis(rel, head_rel);

        rel[0] = angle_sub(torso_angles[0], legs_angles[0]);
        rel[1] = angle_sub(torso_angles[1], legs_angles[1]);
        rel[2] = angle_sub(torso_angles[2], legs_angles[2]);
        AnglesToAxis(rel, torso_rel);
    }
    AnglesToAxis(legs_angles, legs_axis);

    // ---- the parts ----

    n = 0;
    q3p_set_entity(&out[n++], base, m->lower, m->skin, base->origin, legs_axis, &st->legs);

    lerp_tag(q3->tag_torso, q3->lower_frames, st->legs.oldframe, st->legs.frame, st->legs.backlerp, &tag);
    on_tag(base->origin, legs_axis, &tag, torso_rel, torso_origin, torso_axis);
    q3p_set_entity(&out[n++], base, m->upper, m->skin, torso_origin, torso_axis, &st->torso);

    lerp_tag(q3->tag_head, q3->upper_frames, st->torso.oldframe, st->torso.frame, st->torso.backlerp, &tag);
    on_tag(torso_origin, torso_axis, &tag, head_rel, head_origin, head_axis);
    q3p_set_entity(&out[n++], base, m->head, m->skin, head_origin, head_axis, NULL);

    if (q3->tag_weapon && st->weapon_shown) {
        vec3_t  gun_axis[3], gun_origin, local[3], local_origin, tag_axis[3];
        float   scale;
        int     i;

        lerp_tag(q3->tag_weapon, q3->upper_frames, st->torso.oldframe, st->torso.frame, st->torso.backlerp, &tag);

        // the tag in the world
        axis_mul(tag.axis, torso_axis, tag_axis);
        VectorCopy(torso_origin, gun_origin);
        for (i = 0; i < 3; i++)
            VectorMA(gun_origin, tag.origin[i], torso_axis[i], gun_origin);

        // and the gun on it: Quake III's own (the bot match's g_q3_weapons)
        // sit on the tag as they are, Quake II's need the grip
        if (CL_Q3W_IsGun(st->weapon_shown)) {
            for (i = 0; i < 3; i++)
                VectorCopy(tag_axis[i], gun_axis[i]);
            scale = 1.0f;
        } else {
            q3p_gun_transform(local, local_origin, &scale);
            for (i = 0; i < 3; i++)
                VectorMA(gun_origin, local_origin[i], tag_axis[i], gun_origin);
            axis_mul(local, tag_axis, gun_axis);
        }

        q3p_set_entity(&out[n], base, st->weapon_shown, 0, gun_origin, gun_axis, NULL);
        out[n].scale = scale == 1.0f ? 0 : scale;
        n++;
    }

    if (cl_q3_debug->integer > 1)
        Com_Printf("q3 %s: legs %d (%d) torso %d (%d) speed %.0f dir %d ground %d\n",
                   q3->name, st->legs.anim, st->legs.frame, st->torso.anim, st->torso.frame,
                   speed, dir, in->onground);
    return n;
}

/*
=============================================================================

SOUNDS

Quake II's "*" sounds are the player model's: players/<model>/<name>.
A Quake III model's are sound/player/<model>/, under Quake III's names; a
model without its own (the id staff ones) uses Sarge's or Major's.

=============================================================================
*/

static const struct {
    const char *q2, *q3;
} q3p_sounds[] = {
    { "death1.wav",     "death1.wav" },
    { "death2.wav",     "death2.wav" },
    { "death3.wav",     "death3.wav" },
    { "death4.wav",     "death1.wav" },
    { "drown1.wav",     "drown.wav" },
    { "fall1.wav",      "fall1.wav" },
    { "fall2.wav",      "fall1.wav" },
    { "gurp1.wav",      "/sound/player/gurp1.wav" },    // Quake III's are not per model
    { "gurp2.wav",      "/sound/player/gurp2.wav" },
    { "jump1.wav",      "jump1.wav" },
    { "pain25_1.wav",   "pain25_1.wav" },
    { "pain25_2.wav",   "pain25_1.wav" },
    { "pain50_1.wav",   "pain50_1.wav" },
    { "pain50_2.wav",   "pain50_1.wav" },
    { "pain75_1.wav",   "pain75_1.wav" },
    { "pain75_2.wav",   "pain75_1.wav" },
    { "pain100_1.wav",  "pain100_1.wav" },
    { "pain100_2.wav",  "pain100_1.wav" },
    { "taunt.wav",      "taunt.wav" },      // the taunt key (game g_cmds.c Cmd_Taunt_f)
};

bool Q3P_SexedSound(const char *model, const char *base, char *out, size_t size)
{
    q3player_t  *q3;
    const char  *name = base + (*base == '*');
    int         i;

    if (!model || !*model || !(q3 = Q3P_Get(model)))
        return false;

    for (i = 0; i < q_countof(q3p_sounds); i++)
        if (!Q_stricmp(name, q3p_sounds[i].q2))
            break;
    if (i == q_countof(q3p_sounds))
        return false;

    if (q3p_sounds[i].q3[0] == '/') {
        Q_strlcpy(out, q3p_sounds[i].q3 + 1, size);
        return FS_FileExists(out);
    }

    if (Q_snprintf(out, size, "sound/player/%s/%s", q3->name, q3p_sounds[i].q3) < size
        && FS_FileExists(out))
        return true;

    Q_snprintf(out, size, "sound/player/%s/%s", q3->sex == 'f' ? "major" : "sarge", q3p_sounds[i].q3);
    return FS_FileExists(out);
}

/*
=============================================================================

IN THE GAME

What CL_AddPacketEntities needs: each drawn Quake III player's state, kept
by entity number, and its input from what the server sent.

=============================================================================
*/

static cvar_t   *cl_q3_players;

#define CL_Q3_SLOTS     96      // players and the bodies they leave

typedef struct {
    int         entnum;         // 0 = free
    int         last_used;      // cls.framecount
    bool        fired;          // a muzzle flash not yet seen by the animation
    bool        have_origin;
    vec3_t      last_origin;
    int         last_time;
    vec3_t      velocity;
    q3pstate_t  st;
} cl_q3slot_t;

static cl_q3slot_t  cl_q3_slots[CL_Q3_SLOTS];

// cl_q3_players 0 draws a Quake III skin as any missing model: the Quake II male
bool CL_Q3PlayersEnabled(void)
{
    return cl_q3_players && cl_q3_players->integer;
}

static cl_q3slot_t *cl_q3_slot(int entnum, bool create)
{
    cl_q3slot_t *slot, *free_slot = NULL, *oldest = NULL;
    int i;

    for (i = 0, slot = cl_q3_slots; i < CL_Q3_SLOTS; i++, slot++) {
        if (slot->entnum == entnum)
            return slot;
        if (!slot->entnum) {
            if (!free_slot)
                free_slot = slot;
        } else if (!oldest || slot->last_used < oldest->last_used) {
            oldest = slot;
        }
    }
    if (!create)
        return NULL;

    slot = free_slot ? free_slot : oldest;
    memset(slot, 0, sizeof(*slot));
    slot->entnum = entnum;
    return slot;
}

// from CL_MuzzleFlash: the player fired - the torso's attack
void CL_Q3PlayerFired(int entnum)
{
    cl_q3slot_t *slot = cl_q3_slot(entnum, false);

    if (slot)
        slot->fired = true;
}

/*
A Quake III player's parts for a packet entity, base being the entity as the
Quake II model would have been drawn (flags, alpha, id, origin - for your own
body the one slid back for first person). Returns how many went into out;
*gun is the index of the gun in it, or -1.
*/
int CL_AddQ3Player(const centity_t *cent, const clientinfo_t *ci, const entity_t *base,
                   bool self, entity_t *out, int *gun)
{
    const entity_state_t    *s1 = &cent->current;
    cl_q3slot_t             *slot;
    q3pinput_t              in;
    vec3_t                  origin, angles;
    int                     n, i;

    *gun = -1;
    if (!ci->q3.q3)
        return 0;

    slot = cl_q3_slot(s1->number, true);
    slot->last_used = cls.framecount;

    memset(&in, 0, sizeof(in));
    in.time = cl.time;
    in.q2frame = s1->frame;

    // where the body is, not where it is drawn: yours is slid back for
    // first person by an amount that turns with you
    VectorCopy(self ? cl.playerEntityOrigin : base->origin, origin);
    VectorCopy(origin, in.origin);

    // the view, with the pitch the server divides by 3 multiplied back
    if (self) {
        VectorCopy(cl.playerEntityAngles, angles);
    } else {
        LerpAngles(cent->prev.angles, cent->current.angles, cl.lerpfrac, angles);
        angles[PITCH] = angle_sub(angles[PITCH], 0);
    }
    in.view[PITCH] = angles[PITCH] * 3;
    in.view[YAW] = angles[YAW];
    in.view[ROLL] = 0;

    // the motion, from where it was drawn last: the same for every player and
    // in demos. A jump of more than 64 units in a frame is a teleport.
    if (slot->have_origin && cl.time > slot->last_time) {
        vec3_t d;
        float dt = (cl.time - slot->last_time) * 0.001f;

        VectorSubtract(origin, slot->last_origin, d);
        if (VectorLength(d) < 64)
            VectorScale(d, 1.0f / dt, slot->velocity);
    } else if (!slot->have_origin) {
        VectorClear(slot->velocity);
    }
    if (cl.time != slot->last_time || !slot->have_origin) {
        VectorCopy(origin, slot->last_origin);
        slot->last_time = cl.time;
        slot->have_origin = true;
    }
    VectorCopy(slot->velocity, in.velocity);

    // on the ground: yours the movement code says; anyone else's is looked
    // for under the feet - far enough to keep a stair down (and the
    // interpolation across it) on the ground; the trace takes in lifts and
    // doors, so a rising lift is ground, not a jump. Quake II's jump1/jump2
    // frames are the server saying off the ground (G_SetClientFrame), unless
    // a shot or a pain frame outranks them.
    if (self && !cls.demo.playback) {
        in.onground = cl.frame.ps.pmove.pm_flags & PMF_ON_GROUND;
    } else {
        static const vec3_t mins = { -15, -15, -24 }, maxs = { 15, 15, 32 };
        vec3_t start, end;
        trace_t tr;

        VectorCopy(origin, start);
        start[2] += 4;
        VectorCopy(origin, end);
        end[2] -= 20;
        tr = CL_TraceBox(start, mins, maxs, end, MASK_PLAYERSOLID, false);
        in.onground = tr.startsolid || tr.fraction < 1.0f;
    }
    if (s1->frame == 66 || s1->frame == 67)
        in.onground = false;

    // swimming: in water to the waist and off the floor
    if (!in.onground && (CL_PointContents(origin) & MASK_WATER))
        in.swimming = true;

    // the gun: the Quake II male's, as the server has it
    if (s1->modelindex2 == 255) {
        i = (s1->skinnum >> 8) & 0xff;
        if (i >= cl.numWeaponModels)
            i = 0;
        in.weapon = ci->weaponmodel[i];
        if (!in.weapon)
            in.weapon = ci->weaponmodel[0] ? ci->weaponmodel[0] : cl.baseclientinfo.weaponmodel[0];
        in.melee = cl.numWeaponModels > i && !Q_stricmp(cl.weaponModels[i], "w_chainfist.md2");
    } else if (s1->modelindex2) {
        in.weapon = cl.model_draw[s1->modelindex2];
    }

    in.fired = slot->fired;
    slot->fired = false;

    n = Q3P_Animate(&slot->st, &ci->q3, &in, base, out);
    if (n == Q3P_NUM_PARTS)
        *gun = Q3P_PART_WEAPON;
    return n;
}

// a filesystem restart: every Quake III model is looked for again
void CL_Q3Flush(void)
{
    int i;

    memset(cl_q3_slots, 0, sizeof(cl_q3_slots));
    for (i = 0; i < MAX_CLIENTS; i++)
        memset(&cl.clientinfo[i].q3, 0, sizeof(cl.clientinfo[i].q3));
    memset(&cl.baseclientinfo.q3, 0, sizeof(cl.baseclientinfo.q3));
    Q3P_Flush();
}

// after a filesystem restart (game change, baseq3 added): look again
void Q3P_Flush(void)
{
    int i;

    for (i = 0; i < q3p_num_models; i++) {
        q3player_t *q3 = q3p_models[i];

        Z_Free(q3->tag_torso);
        Z_Free(q3->tag_head);
        Z_Free(q3->tag_weapon);
        Z_Free(q3);
    }
    q3p_num_models = 0;
    q3p_num_missing = 0;
}

void Q3P_Init(void)
{
    cl_q3_gun_offset = Cvar_Get("cl_q3_gun_offset", "0 0 0", 0);
    cl_q3_gun_angles = Cvar_Get("cl_q3_gun_angles", "0 0 0", 0);
    cl_q3_gun_scale = Cvar_Get("cl_q3_gun_scale", "1", 0);
    cl_q3_swing_speed = Cvar_Get("cl_q3_swing_speed", "0.3", 0);
    cl_q3_debug = Cvar_Get("cl_q3_debug", "0", 0);
    cl_q3_players = Cvar_Get("cl_q3_players", "1", CVAR_ARCHIVE);
}
