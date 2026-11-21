/*
Copyright (C) 1997-2001 Id Software, Inc.
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

//
// cl_precache.c
//

#include "client.h"
#include "client/sound/vorbis.h"
#include "common/q3shader.h"

/*
================
CL_ParsePlayerSkin

Breaks up playerskin into name (optional), model and skin components.
If model or skin are found to be invalid, replaces them with sane defaults.
================
*/
void CL_ParsePlayerSkin(char *name, char *model, char *skin, const char *s)
{
    size_t len;
    char *t;

    // configstring parsing guarantees that playerskins can never
    // overflow, but still check the length to be entirely fool-proof
    len = strlen(s);
    if (len >= MAX_QPATH) {
        Com_Error(ERR_DROP, "%s: oversize playerskin", __func__);
    }

    // isolate the player's name
    t = strchr(s, '\\');
    if (t) {
        len = t - s;
        strcpy(model, t + 1);
    } else {
        len = 0;
        strcpy(model, s);
    }

    // copy the player's name
    if (name) {
        memcpy(name, s, len);
        name[len] = 0;
    }

    // isolate the model name
    t = strchr(model, '/');
    if (!t)
        t = strchr(model, '\\');
    if (!t)
        goto default_model;
    *t = 0;

    // isolate the skin name
    strcpy(skin, t + 1);

    // fix empty model to male
    if (t == model)
        strcpy(model, "male");

    // apply restrictions on skins
    if (cl_noskins->integer == 2 || !COM_IsPath(skin))
        goto default_skin;

    if (cl_noskins->integer || !COM_IsPath(model))
        goto default_model;

    return;

default_skin:
    if (!Q_stricmp(model, "female")) {
        strcpy(model, "female");
        strcpy(skin, "athena");
    } else {
default_model:
        strcpy(model, "male");
        strcpy(skin, "grunt");
    }
}

/*
================
CL_RegisterVWep

Vwep `i` for a body whose weapons are in players/<dir>/. A Quake III gun's
(the bot match's g_q3_weapons: "models/weapons2/<gun>/<gun>.md3") is its
own model in a Quake III body's hand, and its Quake II counterpart in a
Quake II body's, whose md2 has no hand to hold it.
================
*/
static qhandle_t CL_RegisterVWep(const char *dir, int i, bool q3body)
{
    char        filename[MAX_QPATH];
    const char  *name = cl.weaponModels[i];
    const char  *q2 = CL_Q3W_VWepFallback(name);

    if (q2 && q3body)
        return R_RegisterModel(name);
    Q_concat(filename, sizeof(filename), "players/", dir, "/", q2 ? q2 : name);
    return R_RegisterModel(filename);
}

/*
================
CL_LoadClientinfo

================
*/
void CL_LoadClientinfo(clientinfo_t *ci, const char *s)
{
    int         i;
    char        model_name[MAX_QPATH];
    char        skin_name[MAX_QPATH];
    char        model_filename[MAX_QPATH];
    char        skin_filename[MAX_QPATH];
    char        icon_filename[MAX_QPATH];

    CL_ParsePlayerSkin(ci->name, model_name, skin_name, s);

    memset(&ci->q3, 0, sizeof(ci->q3));

    // A Quake III model (baseq3's models/players/<model>/, when Quake II has
    // no model of that name): its three parts in the skin asked for, and the
    // Quake II male's weapons for its hand - see q3player.c. A skin it does
    // not have is its "default"; a model we do not have goes on below to the
    // Quake II male, as any missing model does.
    if (ci != &cl.baseclientinfo && CL_Q3PlayersEnabled() && Q3P_Register(&ci->q3, model_name, skin_name)) {
        Q_strlcpy(skin_name, Q3P_SkinName(ci->q3.q3, ci->q3.skin), sizeof(skin_name));

        ci->model = ci->q3.lower;
        ci->skin = 0;
        for (i = 0; i < cl.numWeaponModels; i++)
            ci->weaponmodel[i] = CL_RegisterVWep("male", i, true);

        Q_concat(icon_filename, sizeof(icon_filename),
                 "/models/players/", model_name, "/icon_", skin_name, ".tga");
        ci->icon = R_RegisterPic2(icon_filename);
        if (!ci->icon) {
            Q_concat(icon_filename, sizeof(icon_filename),
                     "/models/players/", model_name, "/icon_default.tga");
            ci->icon = R_RegisterPic2(icon_filename);
        }

        Q_strlcpy(ci->model_name, Q3P_ModelName(ci->q3.q3), sizeof(ci->model_name));
        Q_strlcpy(ci->skin_name, skin_name, sizeof(ci->skin_name));
        return;
    }

    // model file
    Q_concat(model_filename, sizeof(model_filename),
             "players/", model_name, "/tris.md2");
    ci->model = R_RegisterModel(model_filename);
    if (!ci->model && Q_stricmp(model_name, "male")) {
        strcpy(model_name, "male");
        strcpy(model_filename, "players/male/tris.md2");
        ci->model = R_RegisterModel(model_filename);
    }

    // skin file
    Q_concat(skin_filename, sizeof(skin_filename),
             "players/", model_name, "/", skin_name, ".pcx");
    ci->skin = R_RegisterSkin(skin_filename);

    // if we don't have the skin and the model was female,
    // see if athena skin exists
    if (!ci->skin && !Q_stricmp(model_name, "female")) {
        strcpy(skin_name, "athena");
        strcpy(skin_filename, "players/female/athena.pcx");
        ci->skin = R_RegisterSkin(skin_filename);
    }

    // if we don't have the skin and the model wasn't male,
    // see if the male has it (this is for CTF's skins)
    if (!ci->skin && Q_stricmp(model_name, "male")) {
        // change model to male
        strcpy(model_name, "male");
        strcpy(model_filename, "players/male/tris.md2");
        ci->model = R_RegisterModel(model_filename);

        // see if the skin exists for the male model
        Q_concat(skin_filename, sizeof(skin_filename),
                 "players/male/", skin_name, ".pcx");
        ci->skin = R_RegisterSkin(skin_filename);
    }

    // if we still don't have a skin, it means that the male model
    // didn't have it, so default to grunt
    if (!ci->skin) {
        // see if the skin exists for the male model
        strcpy(skin_name, "grunt");
        strcpy(skin_filename, "players/male/grunt.pcx");
        ci->skin = R_RegisterSkin(skin_filename);
    }

    // weapon file
    for (i = 0; i < cl.numWeaponModels; i++) {
        ci->weaponmodel[i] = CL_RegisterVWep(model_name, i, false);
        if (!ci->weaponmodel[i] && !Q_stricmp(model_name, "cyborg")) {
            // try male
            ci->weaponmodel[i] = CL_RegisterVWep("male", i, false);
        }
    }

    // icon file
    Q_concat(icon_filename, sizeof(icon_filename),
             "/players/", model_name, "/", skin_name, "_i.pcx");
    ci->icon = R_RegisterPic2(icon_filename);

    strcpy(ci->model_name, model_name);
    strcpy(ci->skin_name, skin_name);

    // base info should be at least partially valid
    if (ci == &cl.baseclientinfo)
        return;

    // must have loaded all data types to be valid
    if (!ci->skin || !ci->icon || !ci->model || !ci->weaponmodel[0]) {
        ci->skin = 0;
        ci->icon = 0;
        ci->model = 0;
        ci->weaponmodel[0] = 0;
        ci->model_name[0] = 0;
        ci->skin_name[0] = 0;
    }
}

/*
=================
CL_RegisterSounds
=================
*/
void CL_RegisterSounds(void)
{
    int i;
    char    *s;

    S_BeginRegistration();
    CL_RegisterTEntSounds();
    CL_RegisterFootstepSounds();
    for (i = 1; i < MAX_SOUNDS; i++) {
        s = cl.configstrings[CS_SOUNDS + i];
        if (!s[0])
            break;
        cl.sound_precache[i] = S_RegisterSound(s);
    }
    S_EndRegistration();
}

/*
=================
CL_RegisterBspModels

Registers main BSP file and inline models
=================
*/
void CL_RegisterBspModels(void)
{
    int ret;
    char *name;
    int i;

    ret = BSP_Load(cl.configstrings[CS_MODELS + 1], &cl.bsp);
    if (cl.bsp == NULL) {
        Com_Error(ERR_DROP, "Couldn't load %s: %s",
                  cl.configstrings[CS_MODELS + 1], Q_ErrorString(ret));
    }

    if (cl.bsp->checksum != atoi(cl.configstrings[CS_MAPCHECKSUM])) {
        if (cls.demo.playback) {
            Com_WPrintf("Local map version differs from demo: %i != %s\n",
                        cl.bsp->checksum, cl.configstrings[CS_MAPCHECKSUM]);
        } else {
            Com_Error(ERR_DROP, "Local map version differs from server: %i != %s",
                      cl.bsp->checksum, cl.configstrings[CS_MAPCHECKSUM]);
        }
    }

    // Quake III map: Haste and Flight ride in the player's gravity - the same
    // flag SV_SpawnServer sets from its load of the map (pmove.c)
    cl.pmp.q3 = cl.bsp->q3;

    CL_SetVoidFloor();

    for (i = 1; i < MAX_MODELS; i++) {
        name = cl.configstrings[CS_MODELS + i];
        if (!name[0]) {
            if (i == MODELINDEX_PLAYER)
                continue;   // reserved and left empty - see PF_FindIndex
            break;
        }
        if (name[0] == '*')
            cl.model_clip[i] = BSP_InlineModel(cl.bsp, name);
        else
            cl.model_clip[i] = NULL;
    }
}

/*
=================
CL_RegisterVWepModels

Builds a list of visual weapon models
=================
*/
void CL_RegisterVWepModels(void)
{
    int         i;
    char        *name;

    cl.numWeaponModels = 1;
    strcpy(cl.weaponModels[0], "weapon.md2");

    // only default model when vwep is off
    if (!cl_vwep->integer) {
        return;
    }

    for (i = 2; i < MAX_MODELS; i++) {
        name = cl.configstrings[CS_MODELS + i];
        if (!name[0]) {
            if (i == MODELINDEX_PLAYER)
                continue;   // reserved and left empty - see PF_FindIndex
            break;
        }
        if (name[0] != '#') {
            continue;
        }

        // special player weapon model
        strcpy(cl.weaponModels[cl.numWeaponModels++], name + 1);

        if (cl.numWeaponModels == MAX_CLIENTWEAPONMODELS) {
            break;
        }
    }
}

/*
=================
CL_Q3SkyShader

A Quake III map has no sky name: its sky is the shader on its sky surfaces
(bsp_q3.c names a texinfo after its shader, less "textures/"). Writes
"*<shader>" for R_SetSky, which bakes it into a cube map (vkpt main.c,
q3_sky_faces).
=================
*/
static bool CL_Q3SkyShader(char *out, size_t size)
{
    char    name[MAX_QPATH];
    int     i;

    if (!cl.bsp || !cl.bsp->q3)
        return false;

    Q3Shader_Load();
    for (i = 0; i < cl.bsp->numtexinfo; i++) {
        const mtexinfo_t *ti = &cl.bsp->texinfo[i];
        const q3shader_t *sh;

        if (!(ti->c.flags & SURF_SKY))
            continue;
        if (!strncmp(ti->name, "../", 3))
            Q_strlcpy(name, ti->name + 3, sizeof(name));
        else
            Q_concat(name, sizeof(name), "textures/", ti->name);

        sh = Q3Shader_Find(name);
        if (sh && sh->sky) {
            Q_concat(out, size, "*", name);
            return true;
        }
    }
    return false;
}

/*
=================
CL_Q1Sky

A Quake 1 map's sky, for R_SetSky. A rerelease map can name a skybox in its
worldspawn ("_sky" "map1/map1_", or FitzQuake's "sky"): six .tga faces under
the episode's gfx/env/, written "id1/gfx/env/map1/map1_" so the renderer finds
them among Quake 1's files. Any other map shows its sky texture - Quake's two
scrolling layers of cloud: "%<texture>" (vkpt main.c, q1_sky_faces). The
server's CS_SKY is not asked: Quake 1 has no target_sky, and the game sends
Quake II's default.
=================
*/
static bool CL_Q1Sky(char *out, size_t size)
{
    char        key[MAX_QPATH], value[MAX_QPATH];
    const char  *data, *tok;
    int         i;

    if (!cl.bsp || !cl.bsp->q1)
        return false;

    // worldspawn, the first entity
    data = cl.bsp->entitystring;
    if (data && *(tok = COM_Parse(&data)) == '{') {
        while (data) {
            tok = COM_Parse(&data);
            if (!*tok || *tok == '}')
                break;
            Q_strlcpy(key, tok, sizeof(key));
            Q_strlcpy(value, COM_Parse(&data), sizeof(value));
            // a key the mapper wrote with a trailing space ("_skyfog ") is the same key
            for (size_t n = strlen(key); n > 0 && key[n - 1] == ' '; n--)
                key[n - 1] = 0;
            if (Q_stricmp(key, "_sky") && Q_stricmp(key, "sky"))
                continue;
            if (!value[0] || strstr(value, ".."))
                continue;
            char face[MAX_QPATH];
            if (Q_concat(face, sizeof(face), "id1/gfx/env/", value, "rt.tga") >= sizeof(face) ||
                !FS_FileExists(face)) {
                Com_DPrintf("Quake 1 skybox %s not found, using the sky texture\n", value);
                continue;
            }
            Q_concat(out, size, "id1/gfx/env/", value);
            return true;
        }
    }

    // the sky texture: bsp_q1.c names a texinfo "q1/<texture>"
    for (i = 0; i < cl.bsp->numtexinfo; i++) {
        const mtexinfo_t *ti = &cl.bsp->texinfo[i];

        if (!(ti->c.flags & SURF_SKY) || Q_strncasecmp(ti->name, "q1/", 3) || !ti->name[3])
            continue;
        Q_concat(out, size, "%", ti->name + 3);
        return true;
    }
    return false;
}

/*
=================
CL_PlayMapMusic

CS_CDTRACK: a track number, or on a Quake III map its worldspawn "music" -
one .wav, or an intro and a loop (g_spawn.c passes the key on as it is). Its
paths name Quake III's own files, so they are looked up there ("baseq3/",
files.c), and Quake III's maps write some of them with backslashes. On a
Quake 1 map the number is a track of Quake's own soundtrack, its episode's.
=================
*/
/*
A Quake 1 map's episode, for its music: files.c loads id1's maps as q1/<map>
and an add-on's as q1/<episode>/<map>, so "q1/hipnotic/hip1m1" is hipnotic's
and "q1/e1m1" id1's. id1's test/ and vault/ maps come out as "test" and
"vault", which have no music of their own, and get id1's like dopa and mg1.
*/
static void CL_Quake1Episode(char *out, size_t size)
{
    const char *name = cl.mapname, *slash;

    Q_strlcpy(out, "id1", size);
    if (Q_strncasecmp(name, "q1/", 3))
        return;
    name += 3;
    slash = strchr(name, '/');
    if (!slash || slash == name || (size_t)(slash - name) >= size)
        return;
    for (const char *c = name; c < slash; c++)
        if (!Q_isalnum(*c) && *c != '_')
            return;     // a server's name, not one files.c made
    memcpy(out, name, slash - name);
    out[slash - name] = 0;
    Q_strlwr(out);
}

static void CL_PlayMapMusic(const char *s)
{
    char    intro[MAX_QPATH], loop[MAX_QPATH];
    char    *tok;
    const char *p = s;

    while (*p == ' ')
        p++;
    if (!*p || Q_isdigit(*p) || *p == '-') {
        // a Quake 1 map's "sounds" is a track of Quake's soundtrack, its
        // episode's own (ogg.c, OGG_PlayQuake1Track)
        if (cl.bsp && cl.bsp->q1) {
            char episode[MAX_QPATH];
            CL_Quake1Episode(episode, sizeof(episode));
            OGG_PlayQuake1Track(episode, atoi(s));
            return;
        }
        OGG_PlayTrack(atoi(s));
        return;
    }

    intro[0] = loop[0] = 0;
    tok = COM_Parse(&p);
    if (*tok)
        Q_concat(intro, sizeof(intro), cl.bsp && cl.bsp->q3 ? "baseq3/" : "", tok);
    tok = COM_Parse(&p);
    if (*tok)
        Q_concat(loop, sizeof(loop), cl.bsp && cl.bsp->q3 ? "baseq3/" : "", tok);

    FS_NormalizePath(intro);
    FS_NormalizePath(loop);
    OGG_PlayMusicFiles(intro, loop);
}

/*
=================
SKY OVERRIDE

sky_override <name>: a sky in place of the map's own, for a map's cfg - with
"mapcvar sky_override space1" baseq3/maps/q3tourney1.cfg gives that Quake III
map the space sky of Quake II's boss2 (Matt, 2026-11-16). sky_override_rotate
and sky_override_axis are a worldspawn's skyrotate (degrees a second) and
skyaxis; boss2's turn is "3" about "1 1 0". Typed in the console, they change
the sky at once. "" = the map's own.
=================
*/
static cvar_t   *sky_override;
static cvar_t   *sky_override_rotate;
static cvar_t   *sky_override_axis;
static bool     prep_refresh_running;

static void sky_override_changed(cvar_t *self)
{
    // a map load sets the sky itself once its cfgs have run (CL_PrepRefresh)
    if (cls.state >= ca_precached && cls.ref_initialized && cl.mapname[0] && !prep_refresh_running)
        CL_SetSky();
}

void CL_InitSkyOverride(void)
{
    sky_override = Cvar_Get("sky_override", "", 0);
    sky_override_rotate = Cvar_Get("sky_override_rotate", "0", 0);
    sky_override_axis = Cvar_Get("sky_override_axis", "0 0 1", 0);
    sky_override->changed = sky_override_changed;
    sky_override_rotate->changed = sky_override_changed;
    sky_override_axis->changed = sky_override_changed;
}

/*
=================
CL_SetSky

=================
*/
void CL_SetSky(void)
{
    float       rotate = 0;
    int         autorotate = 1;
    vec3_t      axis;
    const char  *sky = cl.configstrings[CS_SKY];
    char        q3sky[MAX_QPATH];

    sscanf(cl.configstrings[CS_SKYROTATE], "%f %d", &rotate, &autorotate);
    if (sscanf(cl.configstrings[CS_SKYAXIS], "%f %f %f",
               &axis[0], &axis[1], &axis[2]) != 3) {
        Com_DPrintf("Couldn't parse CS_SKYAXIS\n");
        VectorClear(axis);
    }

    // a Quake III map with no "sky" key of its own (g_spawn.c leaves the
    // name empty): its sky shader
    if (!sky[0] && CL_Q3SkyShader(q3sky, sizeof(q3sky)))
        sky = q3sky;

    // a Quake 1 map's own sky, whatever the server named
    if (CL_Q1Sky(q3sky, sizeof(q3sky))) {
        sky = q3sky;
        rotate = 0;
        VectorSet(axis, 0, 0, 1);
    }

    // a map cfg's sky in place of all of that (SKY OVERRIDE)
    if (sky_override && sky_override->string[0]) {
        sky = sky_override->string;
        rotate = sky_override_rotate->value;
        autorotate = 1;
        if (sscanf(sky_override_axis->string, "%f %f %f", &axis[0], &axis[1], &axis[2]) != 3)
            VectorSet(axis, 0, 0, 1);
    }

    R_SetSky(sky, rotate, autorotate, axis);
}

/*
=================
CL_PrepRefresh

Call before entering a new level, or after changing dlls
=================
*/
void CL_PrepRefresh(void)
{
    int         i;
    char        *name;

    if (!cls.ref_initialized)
        return;
    if (!cl.mapname[0])
        return;     // no map loaded

    // its cfgs' mapcvars must not set the sky half way through (SKY OVERRIDE)
    prep_refresh_running = true;

    // register models, pics, and skins
    Com_LoadMark("precache");
    R_BeginRegistration(cl.mapname);
    // the RTX renderers mark their parts (BSP, world textures, mesh, GPU); this
    // is what follows them: blood's memory, the sky's settings
    Com_LoadMark("world settings");

    CL_LoadState(LOAD_MODELS);

    CL_RegisterTEntModels();

	if (cl_testmodel->string && cl_testmodel->string[0])
	{
		cl_testmodel_handle = R_RegisterModel(cl_testmodel->string);
		if (cl_testmodel_handle)
			Com_Printf("Loaded the test model: %s\n", cl_testmodel->string);
		else
			Com_WPrintf("Failed to load the test model from %s\n", cl_testmodel->string);
	}
	else
		cl_testmodel_handle = -1;

    for (i = 2; i < MAX_MODELS; i++) {
        name = cl.configstrings[CS_MODELS + i];
        if (!name[0]) {
            if (i == MODELINDEX_PLAYER)
                continue;   // reserved and left empty - see PF_FindIndex
            break;
        }
        if (name[0] == '#') {
            continue;
        }
        cl.model_draw[i] = R_RegisterModel(name);
    }
    Com_LoadMark("models");

    CL_LoadState(LOAD_IMAGES);
    for (i = 1; i < MAX_IMAGES; i++) {
        name = cl.configstrings[CS_IMAGES + i];
        if (!name[0]) {
            break;
        }
        cl.image_precache[i] = R_RegisterPic2(name);
    }
    // Quake 1's status bar (sbar_q1.c), when its own game runs the level
    SCR_Q1RegisterMedia();
    Com_LoadMark("pics");

    CL_LoadState(LOAD_CLIENTS);
    for (i = 0; i < MAX_CLIENTS; i++) {
        name = cl.configstrings[CS_PLAYERSKINS + i];
        if (!name[0]) {
            continue;
        }
        CL_LoadClientinfo(&cl.clientinfo[i], name);
    }

    CL_LoadClientinfo(&cl.baseclientinfo, "unnamed\\male/grunt");
    Com_LoadMark("player models");

    // set sky textures and speed
    // Rerelease maps used to be forced onto a dummy "base" sky here so that the
    // procedural sky was always used; the renderer now decides that itself from
    // the map's own skybox and the sky_use_map_skybox cvar.
    prep_refresh_running = false;
    CL_SetSky();

    // the renderer can now free unneeded stuff
    R_EndRegistration();
    Com_LoadMark("sky and end registration");

    // clear any lines of console text
    Con_ClearNotify_f();

    SCR_UpdateScreen();

    CL_PlayMapMusic(cl.configstrings[CS_CDTRACK]);
}

/*
=================
CL_UpdateConfigstring

A configstring update has been parsed.
=================
*/
void CL_UpdateConfigstring(int index)
{
    const char *s = cl.configstrings[index];

    if (index == CS_MAXCLIENTS) {
        cl.maxclients = atoi(s);
        return;
    }

    if (index == CS_AIRACCEL) {
        cl.pmp.airaccelerate = cl.pmp.qwmode || atoi(s);
        return;
    }

    // the bot match's weapon set and player physics (SV_UpdateQ3Rules does the
    // same with the physics for the server's pmove)
    if (index == CS_Q3RULES) {
        cl.q3_weapons = Q3RULES_WEAPONS(s);
        cl.q1_weapons = Q3RULES_Q1WEAPONS(s);
        cl.pmp.q3move = Q3RULES_PHYSICS(s);
        return;
    }

    // a Quake 1 level run by its own progs (baseq2 g_q1vm*.c)
    if (index == CS_Q1VM) {
        Q_strlcpy(cl.q1vm, s, sizeof(cl.q1vm));
        return;
    }

    if (index == CS_MODELS + 1) {
        size_t len = strlen(s);

        if (len <= 9) {
            Com_Error(ERR_DROP, "%s: bad world model: %s", __func__, s);
        }
        memcpy(cl.mapname, s + 5, len - 9);   // skip "maps/"
        cl.mapname[len - 9] = 0; // cut off ".bsp"
        // Quake II 64 player physics - the same rule SV_SpawnServer applies,
        // or prediction and the server disagree about crouching
        cl.pmp.n64 = cl.pmp.rerelease && !Q_strncasecmp(cl.mapname, "q64/", 4);
        return;
    }

    if (index >= CS_LIGHTS && index < CS_LIGHTS + MAX_LIGHTSTYLES) {
        CL_SetLightStyle(index - CS_LIGHTS, s);
        return;
    }

    if (cls.state < ca_precached) {
        return;
    }

    // A mid-level sky change - target_sky, which every MGU drop-pod opener
    // fires on landing to stop the descent spin and swap the environment.
    // Without this the change is dropped and the renderer keeps whatever the
    // one CL_SetSky() at precache time happened to see, so whether the sky
    // stops rotating is a race against level load time.
    if (index == CS_SKY || index == CS_SKYAXIS || index == CS_SKYROTATE) {
        CL_SetSky();
        return;
    }

    // A mid-level music change - target_music (mgu5m3 switches between tracks 9,
    // 19 and 64 as you go), a finished N64 target_goal, target_camera. Without
    // this the change was dropped: the new track sat in the configstring until
    // the next CL_PrepRefresh - a save load, vid_restart or `mat reload` - played
    // it, so those seemed to change the music by themselves. Upstream Q2RTX has
    // the same case; this fork's music code predates it. OGG_PlayTrack leaves a
    // track that is already playing alone.
    if (index == CS_CDTRACK) {
        CL_PlayMapMusic(s);
        return;
    }

    if (index >= CS_MODELS + 2 && index < CS_MODELS + MAX_MODELS) {
        int i = index - CS_MODELS;

        cl.model_draw[i] = R_RegisterModel(s);
        if (*s == '*')
            cl.model_clip[i] = BSP_InlineModel(cl.bsp, s);
        else
            cl.model_clip[i] = NULL;
        return;
    }

    if (index >= CS_SOUNDS && index < CS_SOUNDS + MAX_SOUNDS) {
        cl.sound_precache[index - CS_SOUNDS] = S_RegisterSound(s);
        return;
    }

    if (index >= CS_IMAGES && index < CS_IMAGES + MAX_IMAGES) {
        cl.image_precache[index - CS_IMAGES] = R_RegisterPic2(s);
        return;
    }

    if (index >= CS_PLAYERSKINS && index < CS_PLAYERSKINS + MAX_CLIENTS) {
        CL_LoadClientinfo(&cl.clientinfo[index - CS_PLAYERSKINS], s);
        return;
    }
}
