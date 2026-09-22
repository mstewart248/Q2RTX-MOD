/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2008 Andrey Nazarov
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

#include "shared/shared.h"
#include "shared/list.h"
#include "common/common.h"
#include "common/files.h"
#include "system/hunk.h"
#include "format/md2.h"
#if USE_MD3
#include "format/md3.h"
#endif
#include "format/sp2.h"
#include "format/iqm.h"
#include "format/md5.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "../client/client.h"
#include "gl/gl.h"

// during registration it is possible to have more models than could actually
// be referenced during gameplay, because we don't want to free anything until
// we are sure we won't need it.
#define MAX_RMODELS     (MAX_MODELS * 2)

model_t      r_models[MAX_RMODELS];
int          r_numModels;

cvar_t    *cl_md5_models;
cvar_t    *cl_classic_railgun;
cvar_t    *cl_testmodel;
cvar_t    *cl_testfps;
cvar_t    *cl_testalpha;
qhandle_t  cl_testmodel_handle = -1;
vec3_t     cl_testmodel_position;

static model_t *MOD_Alloc(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            break;
        }
    }

    if (i == r_numModels) {
        if (r_numModels == MAX_RMODELS) {
            return NULL;
        }
        r_numModels++;
    }

    return model;
}

static model_t *MOD_Find(const char *name)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        if (!FS_pathcmp(model->name, name)) {
            return model;
        }
    }

    return NULL;
}

static void MOD_List_f(void)
{
    static const char types[4] = "FASE";
    int     i, count;
    model_t *model;
    size_t  bytes;

    Com_Printf("------------------\n");
    bytes = count = 0;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        Com_Printf("%c %8zu : %s\n", types[model->type],
                   model->hunk.mapped, model->name);
        bytes += model->hunk.mapped;
        count++;
    }
    Com_Printf("Total models: %d (out of %d slots)\n", count, r_numModels);
    Com_Printf("Total resident: %zu\n", bytes);
}

void MOD_FreeUnused(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        if (model->registration_sequence == registration_sequence) {
            // make sure it is paged in
            Com_PageInMemory(model->hunk.base, model->hunk.cursize);
        } else {
            // don't need this model
            Hunk_Free(&model->hunk);
            memset(model, 0, sizeof(*model));
        }
    }
}

void MOD_FreeAll(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }

        Hunk_Free(&model->hunk);
        memset(model, 0, sizeof(*model));
    }

    r_numModels = 0;
}

int MOD_ValidateMD2(dmd2header_t *header, size_t length)
{
    size_t end;

    // check ident and version
    if (header->ident != MD2_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header->version != MD2_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;

    // check triangles
    if (header->num_tris < 1)
        return Q_ERR_TOO_FEW;
    if (header->num_tris > MD2_MAX_TRIANGLES)
        return Q_ERR_TOO_MANY;

    end = header->ofs_tris + sizeof(dmd2triangle_t) * header->num_tris;
    if (header->ofs_tris < sizeof(*header) || end < header->ofs_tris || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_tris % q_alignof(dmd2triangle_t))
        return Q_ERR_BAD_ALIGN;

    // check st
    if (header->num_st < 3)
        return Q_ERR_TOO_FEW;
    if (header->num_st > MAX_ALIAS_VERTS)
        return Q_ERR_TOO_MANY;

    end = header->ofs_st + sizeof(dmd2stvert_t) * header->num_st;
    if (header->ofs_st < sizeof(*header) || end < header->ofs_st || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_st % q_alignof(dmd2stvert_t))
        return Q_ERR_BAD_ALIGN;

    // check xyz and frames
    if (header->num_xyz < 3)
        return Q_ERR_TOO_FEW;
    if (header->num_xyz > MAX_ALIAS_VERTS)
        return Q_ERR_TOO_MANY;
    if (header->num_frames < 1)
        return Q_ERR_TOO_FEW;
    if (header->num_frames > MD2_MAX_FRAMES)
        return Q_ERR_TOO_MANY;

    end = sizeof(dmd2frame_t) + (header->num_xyz - 1) * sizeof(dmd2trivertx_t);
    if (header->framesize < end || header->framesize > MD2_MAX_FRAMESIZE)
        return Q_ERR_BAD_EXTENT;

    end = header->ofs_frames + (size_t)header->framesize * header->num_frames;
    if (header->ofs_frames < sizeof(*header) || end < header->ofs_frames || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_frames % q_alignof(dmd2frame_t))
        return Q_ERR_BAD_ALIGN;

    // check skins
    if (header->num_skins) {
        if (header->num_skins > MAX_ALIAS_SKINS)
            return Q_ERR_TOO_MANY;

        end = header->ofs_skins + (size_t)MD2_MAX_SKINNAME * header->num_skins;
        if (header->ofs_skins < sizeof(*header) || end < header->ofs_skins || end > length)
            return Q_ERR_BAD_EXTENT;
    }

    if (header->skinwidth < 1 || header->skinwidth > MD2_MAX_SKINWIDTH)
        return Q_ERR_INVALID_FORMAT;
    if (header->skinheight < 1 || header->skinheight > MD2_MAX_SKINHEIGHT)
        return Q_ERR_INVALID_FORMAT;

    return Q_ERR_SUCCESS;
}

static model_class_t
get_model_class(const char *name)
{
	if (!strcmp(name, "models/objects/explode/tris.md2"))
		return MCLASS_EXPLOSION;
	else if (!strcmp(name, "models/objects/r_explode/tris.md2"))
		return MCLASS_EXPLOSION;
	// A SUFFIX, not an exact name: the rerelease muzzle flashes live at
	// models/weapons/v_<gun>/flash/tris.md2 and have to land here too.
	//
	// This class routes a model to the effects TLAS, whose hit shader
	// (pt_logic_explosion) samples base_texture and does
	//     emission.a *= alpha;  emission.rgb *= emission.a;
	// i.e. SOFT PER-TEXEL alpha - a bright core tapering out at the edges.
	// That taper is the entire look of a muzzle flash and it is why the flash
	// is here rather than on MCLASS_REGULAR. The regular path can sample
	// texture_emissive, but its only transparency is texture_mask, a hard
	// binary >= 0.5 cutout that renders a flat opaque star - tolerable across
	// a room, wrong at arm's length in front of your own gun.
	//
	// The flashes therefore carry NO emissive map on purpose. They do not need
	// one: every weapon that fires already spawns its own dynamic light.
	else if (strstr(name, "/flash/tris.md2"))
		return MCLASS_FLASH;
	else if (!strcmp(name, "models/objects/smoke/tris.md2"))
		return MCLASS_SMOKE;
	else if (!strcmp(name, "models/objects/minelite/light2/tris.md2"))
        return MCLASS_STATIC_LIGHT;
    else if (!strcmp(name, "models/objects/flare/tris.md2"))
        return MCLASS_FLARE;
    else if (!strcmp(name, "models/proj/beam/tris.md2"))
        return MCLASS_PLAYER_BEAM;
	else
		return MCLASS_REGULAR;
}

static int MOD_LoadSP2(model_t *model, const void *rawdata, size_t length, const char* mod_name)
{
    dsp2header_t header;
    dsp2frame_t *src_frame;
    mspriteframe_t *dst_frame;
    char buffer[SP2_MAX_FRAMENAME];
    int i, ret;

    if (length < sizeof(header))
        return Q_ERR_FILE_TOO_SMALL;

    // byte swap the header
    LittleBlock(&header, rawdata, sizeof(header));

    if (header.ident != SP2_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.version != SP2_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.numframes < 1) {
        // empty models draw nothing
        model->type = MOD_EMPTY;
        return Q_ERR_SUCCESS;
    }
    if (header.numframes > SP2_MAX_FRAMES)
        return Q_ERR_TOO_MANY;
    if (sizeof(dsp2header_t) + sizeof(dsp2frame_t) * header.numframes > length)
        return Q_ERR_BAD_EXTENT;

    Hunk_Begin(&model->hunk, sizeof(mspriteframe_t) * header.numframes);
    model->type = MOD_SPRITE;

    CHECK(model->spriteframes = MOD_Malloc(sizeof(mspriteframe_t) * header.numframes));
    model->numframes = header.numframes;

    src_frame = (dsp2frame_t *)((byte *)rawdata + sizeof(dsp2header_t));
    dst_frame = model->spriteframes;
    for (i = 0; i < header.numframes; i++) {
        dst_frame->width = (int32_t)LittleLong(src_frame->width);
        dst_frame->height = (int32_t)LittleLong(src_frame->height);

        dst_frame->origin_x = (int32_t)LittleLong(src_frame->origin_x);
        dst_frame->origin_y = (int32_t)LittleLong(src_frame->origin_y);

        if (!Q_memccpy(buffer, src_frame->name, 0, sizeof(buffer))) {
            Com_WPrintf("%s has bad frame name\n", model->name);
            dst_frame->image = R_NOTEXTURE;
        } else {
            FS_NormalizePath(buffer);
            dst_frame->image = IMG_Find(buffer, IT_SPRITE, IF_SRGB);
        }

        src_frame++;
        dst_frame++;
    }

    Hunk_End(&model->hunk);

    return Q_ERR_SUCCESS;

fail:
    return ret;
}

#define TRY_MODEL_SRC_GAME      1
#define TRY_MODEL_SRC_BASE      0

/*
=================
MOD_BuildMD5Path

The rerelease keeps its improved skeletal model beside the classic one, in an
md5/ subdirectory: models/monsters/soldier/md5/tris.md5mesh next to
models/monsters/soldier/tris.md2. Turning one path into the other is all the
per-model fallback this needs - if the MD5 is not there, the MD2 still loads.

(md2_path) must already be normalized and end in ".md2".
=================
*/
static bool MOD_BuildMD5Path(const char *md2_path, char *buffer, size_t size)
{
    const char *slash = strrchr(md2_path, '/');
    size_t dirlen = slash ? (size_t)(slash - md2_path) + 1 : 0;
    size_t namelen = strlen(md2_path);

    // "md5/" goes in and ".md2" grows to ".md5mesh"
    if (namelen + 8 >= size)
        return false;

    memcpy(buffer, md2_path, dirlen);
    memcpy(buffer + dirlen, "md5/", 4);
    memcpy(buffer + dirlen + 4, md2_path + dirlen, namelen - dirlen - 4);
    memcpy(buffer + namelen, ".md5mesh", 9);
    return true;
}

qhandle_t R_RegisterModel(const char *name)
{
    char normalized[MAX_QPATH];
    char md5_path[MAX_QPATH];
    const char *load_name = name;
    qhandle_t index;
    size_t namelen;
    int filelen = 0;
    model_t *model;
    byte *rawdata = NULL;
    uint32_t ident;
    mod_load_t load;
    bool loaded_substitute = false;
    int ret;

    // empty names are legal, silently ignore them
    if (!*name)
        return 0;

    if (*name == '*') {
        // inline bsp model
        index = atoi(name + 1);
        return ~index;
    }

    // normalize the path
    namelen = FS_NormalizePathBuffer(normalized, name, MAX_QPATH);

    // this should never happen
    if (namelen >= MAX_QPATH)
        Com_Error(ERR_DROP, "%s: oversize name", __func__);

    // normalized to empty name?
    if (namelen == 0) {
        Com_DPrintf("%s: empty name\n", __func__);
        return 0;
    }

    // see if it's already loaded
    model = MOD_Find(normalized);
    if (model) {
        MOD_Reference(model);
        goto done;
    }

    // Always prefer models from the game dir, even if format might be 'inferior'
    for (int try_location = Q_stricmp(fs_game->string, BASEGAME) ? TRY_MODEL_SRC_GAME : TRY_MODEL_SRC_BASE;
         try_location >= TRY_MODEL_SRC_BASE;
         try_location--)
    {
        int fs_flags = 0;
        if (try_location > 0)
            fs_flags = try_location == TRY_MODEL_SRC_GAME ? FS_PATH_GAME : FS_PATH_BASE;

        char* extension = normalized + namelen - 4;
        bool is_md2 = namelen > 4 && strcmp(extension, ".md2") == 0;

        bool try_md3 = cls.ref_type == REF_TYPE_VKPT || (cls.ref_type == REF_TYPE_GL && gl_use_hd_assets->integer);

        // Q2RTX's railgun is a redesign rather than a remaster - a different
        // gun, not a higher-poly one - so it gets its own switch. Declining it
        // just means not taking the .md3, and the classic shape then comes from
        // the MD5 or the MD2 according to cl_md5_models.
        if (try_md3 && cl_classic_railgun->integer
            && (strstr(normalized, "weapons/g_rail") || strstr(normalized, "weapons/v_rail")))
            try_md3 = false;

        // MOD_LoadMD5 is NULL in the GL renderer, which has no skeletal path
        bool try_md5 = is_md2 && MOD_LoadMD5 && cl_md5_models->integer;

        if (is_md2 && try_md3)
        {
            memcpy(extension, ".md3", 4);

            filelen = FS_LoadFileFlags(normalized, (void **)&rawdata, fs_flags);

            memcpy(extension, ".md2", 4);

            if (rawdata)
                loaded_substitute = true;
        }

        if (!rawdata && try_md5
            && MOD_BuildMD5Path(normalized, md5_path, sizeof(md5_path)))
        {
            filelen = FS_LoadFileFlags(md5_path, (void **)&rawdata, fs_flags);

            if (rawdata) {
                load_name = md5_path;
                loaded_substitute = true;
            }
        }
        if (!rawdata)
        {
            filelen = FS_LoadFileFlags(normalized, (void **)&rawdata, fs_flags);
        }
        if (rawdata)
            break;
    }

	if (!rawdata)
	{
		filelen = FS_LoadFile(normalized, (void **)&rawdata);
		if (!rawdata) {
			if (filelen == Q_ERR(ENOENT)) {
				/* A MODEL THAT IS NOT ON THE SEARCH PATH IS THE MOST INVISIBLE
				   KIND OF INVISIBLE THERE IS.

				   Returning 0 is right - handle 0 draws nothing and every
				   caller copes - but doing it in complete silence is what
				   makes "the gladiator is missing" unanswerable. The usual
				   cause is a mission-pack model (models/monsters/gladb,
				   models/monsters/soldierh: Xatrix, not baseq2) on a search
				   path that does not include the pak holding it, and the
				   engine knew that the whole time and said nothing.

				   The comment this replaces was right that a per-miss warning
				   would spam: the client probes for per-player weapon models
				   that legitimately do not exist. Hence once per NAME, capped
				   - a bounded handful of lines at map load, which is what a
				   bug report needs and a play session will never notice. */
				static const char *missing[32];
				static int         num_missing;
				int i;

				for (i = 0; i < num_missing; i++)
					if (!FS_pathcmp(missing[i], normalized))
						break;

				if (i == num_missing && num_missing < q_countof(missing)) {
					missing[num_missing] = Z_CopyString(normalized);
					num_missing++;
					Com_WPrintf("%s: '%s' is not on the search path; anything using "
								"it will be invisible.\n", __func__, normalized);
				}
				return 0;
			}

			ret = filelen;
			goto fail1;
		}
	}

    if (filelen < 4) {
        ret = Q_ERR_FILE_TOO_SMALL;
        goto fail2;
    }

    // check ident
    ident = LittleLong(*(uint32_t *)rawdata);
    switch (ident) {
    case MD2_IDENT:
        load = MOD_LoadMD2;
        break;
#if USE_MD3
    case MD3_IDENT:
        load = MOD_LoadMD3;
        break;
#endif
    case SP2_IDENT:
        load = MOD_LoadSP2;
        break;
    case IQM_IDENT:
        load = MOD_LoadIQM;
        break;
    case MD5_IDENT:
        load = MOD_LoadMD5;
        break;
    default:
        ret = Q_ERR_UNKNOWN_FORMAT;
        goto fail2;
    }

    if (!load)
    {
        ret = Q_ERR_UNKNOWN_FORMAT;
        goto fail2;
    }

    model = MOD_Alloc();
    if (!model) {
        ret = Q_ERR_OUT_OF_SLOTS;
        goto fail2;
    }

    memcpy(model->name, normalized, namelen + 1);
    model->registration_sequence = registration_sequence;

    ret = load(model, rawdata, filelen, load_name);

    FS_FreeFile(rawdata);
    rawdata = NULL;

    /*
       A .md5mesh (or .md3) THAT DOES NOT LOAD MUST NOT COST US THE MODEL.

       Everything above is a substitution: the caller asked for a .md2 and we
       quietly handed the loader a higher-detail stand-in sitting beside it.
       The MD5 parser is deliberately strict - model_md5.c says so, and says
       the caller "falls back to the .md2" when it refuses a file - but nothing
       here actually did. A single unexpected token in one .md5mesh therefore
       returned handle 0, and handle 0 is not a broken model, it is NO model:
       the monster is silently, completely invisible, and the only trace is one
       Com_EPrintf that a player is never going to be looking at.

       That is a strictly worse outcome than the format we were substituting
       FOR, which is still sitting right there on the search path. So take it.
       The fallback is per model, so one bad file costs that one model its
       skeletal version and nothing else.

       Note this runs only for a substitute. A .md2 that fails to load has
       nothing left to fall back to and still reports the error as before.
    */
    if (ret && loaded_substitute) {
        // WARN, not DPrintf: this is the one line that tells a user why a
        // model looks like the 1997 one, and the bug reports it answers are
        // written by people who are not running with developer 1. At most one
        // line per model per registration.
        Com_WPrintf("%s: %s failed to load (%s); falling back to %s\n",
                    __func__, load_name, Q_ErrorString(ret), normalized);

        memset(model, 0, sizeof(*model));

        filelen = FS_LoadFile(normalized, (void **)&rawdata);
        if (rawdata) {
            if (filelen >= 4 && LittleLong(*(uint32_t *)rawdata) == MD2_IDENT && MOD_LoadMD2) {
                memcpy(model->name, normalized, namelen + 1);
                model->registration_sequence = registration_sequence;
                ret = MOD_LoadMD2(model, rawdata, filelen, normalized);
            }
            FS_FreeFile(rawdata);
            rawdata = NULL;
        }
    }

    if (ret) {
        memset(model, 0, sizeof(*model));
        goto fail1;
    }

	model->model_class = get_model_class(model->name);

done:
    index = (model - r_models) + 1;
    return index;

fail2:
    FS_FreeFile(rawdata);
fail1:
    Com_EPrintf("Couldn't load %s: %s\n", normalized, Q_ErrorString(ret));
    return 0;
}

/*
=================
MOD_ForHandle_

Call it through the MOD_ForHandle macro, which fills in the call site.

AN INLINE BSP MODEL HANDLE IS NOT AN ERROR.  R_RegisterModel returns ~N for a
"*N" configstring, so every brush entity on the map - every door, lift, plat and
train - carries a negative handle, and ~62 is the -63 that used to kill the
client on the way into mgu5m2.  There is no model_t behind one: the geometry is
part of the world, and the renderer dispatches it on `model & 0x80000000` long
before it would want a mesh.  NULL is therefore the correct answer and not a
papered-over failure - it is the same answer a slot with no loaded model gives,
and every caller already handles it, because every caller already has to.

What made this fatal was that the test was written as "is this a valid index
into r_models", which a legitimate handle of a second kind cannot pass.  The
callers that are correct are the ones that check the top bit first; the ones
that are not simply asked a question with no answer.

The ONE-TIME WARNING is deliberately not silent. A caller reaching here is
usually doing something it did not mean to - looking up the mesh of a door -
and the whole point of the file:line is that the old message could not say who.
Once per call site, so a per-frame path cannot flood the log.
=================
*/
model_t *MOD_ForHandle_(qhandle_t h, const char *file, int line)
{
    model_t *model;

    if (!h) {
        return NULL;
    }

    if (h & 0x80000000) {
        // inline bsp model ("*N") - world geometry, no model_t. Warn once per site.
        static const char *seen_file[8];
        static int         seen_line[8];
        static int         seen_num;
        int i;

        for (i = 0; i < seen_num; i++)
            if (seen_line[i] == line && seen_file[i] == file)
                break;

        if (i == seen_num) {
            Com_WPrintf("%s: inline bsp model handle %d (*%d) asked for at %s:%d; "
                        "that brush has no model_t, returning NULL\n",
                        __func__, h, ~h, file, line);
            if (seen_num < q_countof(seen_file)) {
                seen_file[seen_num] = file;
                seen_line[seen_num] = line;
                seen_num++;
            }
        }

        return NULL;
    }

    /* Say WHICH handle, what the valid range was, AND WHO ASKED. The bare
       Q_assert printed only the expression, which is the one thing you already
       know when you are staring at the log - it left no way to tell a stale
       handle from a previous map (small h, plausible) from an out-of-bounds
       array read somewhere in the caller (huge h, which is what an unclamped
       model index off the wire produces). The call site is the rest of it: this
       error names the model system and the fault is always in the caller. */
    if (!(h > 0 && h <= r_numModels))
        Com_Error(ERR_FATAL, "%s: bad model handle %d (r_numModels = %d), from %s:%d",
                  __func__, h, r_numModels, file, line);

    model = &r_models[h - 1];
    if (!model->type) {
        return NULL;
    }

    return model;
}

static void MOD_PutTest_f(void)
{
    VectorCopy(cl.refdef.vieworg, cl_testmodel_position);
    cl_testmodel_position[2] -= 46.12f; // player eye-level
}

void MOD_Init(void)
{
    Q_assert(!r_numModels);
    Cmd_AddCommand("modellist", MOD_List_f);
    Cmd_AddCommand("puttest", MOD_PutTest_f);

    // Use the rerelease's skeletal (MD5) models instead of the classic MD2s,
    // wherever one exists. Falls back to the .md2 per model. CVAR_FILES
    // re-registers everything on change, so it can be flipped in the menu
    // without reloading the map.
    cl_md5_models = Cvar_Get("cl_md5_models", "1", CVAR_ARCHIVE | CVAR_FILES);

    // Keep the classic railgun rather than Q2RTX's redesigned one. The shape
    // then comes from the MD5 or the MD2 depending on cl_md5_models.
    cl_classic_railgun = Cvar_Get("cl_classic_railgun", "0", CVAR_ARCHIVE | CVAR_FILES);

    // Path to the test model - can be an .md2, .md3 or .iqm file
    cl_testmodel = Cvar_Get("cl_testmodel", "", 0);

    // Test model animation frames per second, can be adjusted at runtime
    cl_testfps = Cvar_Get("cl_testfps", "10", 0);

    // Test model alpha, 0-1
    cl_testalpha = Cvar_Get("cl_testalpha", "1", 0);
}

void MOD_Shutdown(void)
{
    MOD_FreeAll();
    Cmd_RemoveCommand("modellist");
    Cmd_RemoveCommand("puttest");
}

