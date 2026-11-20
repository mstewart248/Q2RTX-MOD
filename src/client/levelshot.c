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
LEVELSHOTS - map thumbnails, made the way Quake III made its own

Quake III ships levelshots/<map>.jpg for its arenas, and the menus show them
(the campaign's level select, client/ui/q3sp.c, and the bot match page's map
row). Quake II ships none, so this makes them:

    levelshots [force] [map ...]

loads each map in turn as an empty deathmatch, holds the view on the map's
intermission camera - info_player_intermission, aimed at its target when it
has one, else the first deathmatch spawn at eye height - lets the picture
settle (the exposure and the denoiser need a few seconds), and writes the
middle 4:3 of the frame, shrunk to 640x480, to levelshots/<map>.jpg in the
game's write directory (IMG_GrabLevelshot, refresh/images.c). With no maps
named it does every map the bot match page lists that has no levelshot yet;
"force" redoes the ones that have. `levelshot` does the map you are on,
without changing level. `levelshots stop` gives up.

While a shot is being taken the HUD, the console's notify lines, your gun
and your own body are left out, and the field of view is Quake III's: 90
degrees across the 4:3 that is kept. An open menu or console holds the
capture back until it is closed again.

THE SAME LOOK FOR EVERY ARENA, WHATEVER THE PLAYER HAS PICKED (Matt,
2026-11-20: "for the deathmatch and CTF levels can we use the stroggos sky in
the evening position except for the space levels those should show the space
sky", and "show the quake2 weapons for the quake2 levels"):

  - a Quake II deathmatch or CTF map gets the Stroggos sky (sky_type 3) with
    the sun held at the evening preset (sun_preset 7, sun_animate 0); one whose
    worldspawn names a space skybox (rdm4, q2ctf5: "space1") gets the space
    sky over it as well (physical_sky_space 1), as boss2's own cfg does
  - the campaign's maps keep the player's own sky - their cfgs state their
    own sky and hour, see rerelease/maps/<map>.cfg - and so do Quake III's
  - every Quake II map is loaded with Quake II's guns (g_q3_weapons 0), or a
    Quake III player model under "by player model" puts Quake III pickups in
    the picture

All of it is the player's own again once the shots are done, stopped, or the
game quits - see ls_saved_cvars.

A CAMERA IN THE DARK. Some intermission cameras look at nothing - the Quake 64
arenas' mostly, a few of them pure black. A shot whose mean brightness is under
levelshot_dark (0..255) is not kept while there is somewhere else to stand: the
map's other intermission cameras, then its deathmatch spawns, then its player
start, LS_MAX_CAMS in all. If every one of them is dark, the brightest is the
one written.
*/

#include "client.h"
#include "refresh/images.h"
#include "format/bsp.h"
#include "format/bsp3.h"

#define LS_WIDTH        640
#define LS_HEIGHT       480
#define LS_QUALITY      90
#define LS_MAX_MAPS     128
#define LS_MAX_CAMS     6           // places a dark map's shot is tried from
#define LS_LOAD_TIMEOUT 120000      // ms for a map to come up before it is skipped

// physical_sky.c's values
#define LS_SKY_TYPE_STROGGOS    "3"     // SKY_TYPE_STROGGOS
#define LS_SUN_PRESET_EVENING   "7"     // SUN_PRESET_EVENING

static cvar_t   *levelshot_delay;   // seconds the picture settles before it is taken
static cvar_t   *levelshot_dark;    // mean brightness (0..255) under which another camera is tried

// the deathmatch and CTF arenas the bot match page lists (baseq2/q2rtx.menu);
// missing ones are skipped
static const char *const ls_arena_maps[] = {
    "q2dm1", "q2dm2", "q2dm3", "q2dm4", "q2dm5", "q2dm6", "q2dm7", "q2dm8",
    "mgdm1",
    "xdm1", "xdm2", "xdm3", "xdm4", "xdm5", "xdm6", "xdm7",
    "rdm1", "rdm2", "rdm3", "rdm4", "rdm5", "rdm6", "rdm7", "rdm8", "rdm9",
    "rdm10", "rdm11", "rdm12", "rdm13", "rdm14",
    "q64/dm1", "q64/dm2", "q64/dm3", "q64/dm4", "q64/dm5", "q64/dm6",
    "q64/dm7", "q64/dm8", "q64/dm9", "q64/dm10",
    "q2ctf1", "q2ctf2", "q2ctf3", "q2ctf4", "q2ctf5", "q2kctf1", "q2kctf2", "ndctf0",
};

// the campaign's, which the bot match offers too; these keep the player's sky
static const char *const ls_campaign_maps[] = {
    "base1", "base2", "base3", "train", "bunk1", "ware1", "ware2",
    "jail1", "jail2", "jail3", "jail4", "jail5", "security",
    "mintro", "mine1", "mine2", "mine3", "mine4", "fact1", "fact2", "fact3",
    "power1", "power2", "cool1", "waste1", "waste2", "waste3", "biggun",
    "hangar1", "hangar2", "lab", "command", "strike", "space",
    "city1", "city2", "city3", "boss1", "boss2",
};

// What the shots change, saved when they start and put back when they end.
// sky_type first: putting it back rewrites physical_sky and sky_use_map_skybox
// (sky_type_changed), which then get their own values back after it.
static const char *const ls_saved_cvars[] = {
    "sky_type",
    "physical_sky",
    "sky_use_map_skybox",
    "sun_preset",
    "sun_animate",
    "sun_elevation",
    "sun_azimuth",
    "physical_sky_space",
    "g_q3_weapons",
};

#define LS_NUM_SAVED    q_countof(ls_saved_cvars)
#define LS_SAVED_SKY    7           // the sky and sun ones: ls_saved_cvars[0 .. LS_SAVED_SKY - 1]
#define LS_SAVED_WEAPONS 8          // g_q3_weapons

static struct {
    bool        active;
    bool        single;             // `levelshot`: the map already loaded
    char        maps[LS_MAX_MAPS][MAX_QPATH];
    int         num, cur;
    int         saved, failed;

    unsigned    map_time;           // cls.realtime the current map was asked for
    unsigned    settle_time;        // cls.realtime the view first held still, 0 = not yet
    int         settle_frames;

    bool        arena;              // a Quake II deathmatch or CTF map: the arenas' sky
    bool        look_applied;       // ... set again since the map came up

    // where the shot can be taken from, best first, and the one in use
    char        camera_map[MAX_QPATH];
    int         num_cams, cam;
    vec3_t      cam_origin[LS_MAX_CAMS];
    vec3_t      cam_angles[LS_MAX_CAMS];

    // the brightest picture of this map so far, written if every camera is dark
    void        *best;
    size_t      best_len;
    float       best_bright;
    int         best_cam;

    // the player's own settings
    bool        have_saved;
    bool        saved_exists[LS_NUM_SAVED];
    char        saved_value[LS_NUM_SAVED][MAX_QPATH];
} ls;

/*
=============================================================================

THE CAMERA

=============================================================================
*/

typedef struct {
    char    classname[64];
    char    targetname[64];
    char    target[64];
    vec3_t  origin;
    vec3_t  angles;
    bool    has_origin;
} ls_ent_t;

// the next entity of the map's entity string, or false at its end
static bool LS_ParseEntity(const char **data, ls_ent_t *e)
{
    char    key[MAX_TOKEN_CHARS];
    char    *tok;

    memset(e, 0, sizeof(*e));

    tok = COM_Parse(data);
    if (!*data || tok[0] != '{')
        return false;

    while (1) {
        tok = COM_Parse(data);
        if (!*data)
            return false;
        if (tok[0] == '}')
            return true;
        Q_strlcpy(key, tok, sizeof(key));
        tok = COM_Parse(data);
        if (!*data)
            return false;

        if (!strcmp(key, "classname")) {
            Q_strlcpy(e->classname, tok, sizeof(e->classname));
        } else if (!strcmp(key, "targetname")) {
            Q_strlcpy(e->targetname, tok, sizeof(e->targetname));
        } else if (!strcmp(key, "target")) {
            Q_strlcpy(e->target, tok, sizeof(e->target));
        } else if (!strcmp(key, "origin")) {
            e->has_origin = sscanf(tok, "%f %f %f", &e->origin[0], &e->origin[1], &e->origin[2]) == 3;
        } else if (!strcmp(key, "angles") || !strcmp(key, "mangle")) {
            sscanf(tok, "%f %f %f", &e->angles[0], &e->angles[1], &e->angles[2]);
        } else if (!strcmp(key, "angle")) {
            e->angles[YAW] = atof(tok);
        }
    }
}

// Quake III aims its intermission camera at a target
static void LS_AimAtTarget(ls_ent_t *spot)
{
    const char  *data = cl.bsp->entitystring;
    ls_ent_t    target;
    vec3_t      dir;

    while (LS_ParseEntity(&data, &target)) {
        if (!target.has_origin || strcmp(target.targetname, spot->target))
            continue;
        VectorSubtract(target.origin, spot->origin, dir);
        vectoangles2(dir, spot->angles);
        return;
    }
}

// Where the shot can be taken from, best first: the map's intermission
// cameras, then its deathmatch spawns and its player start, at the eye height
// of someone spawned there. Only the first is used unless its picture comes
// out dark - see CL_LevelshotFrame.
static void LS_FindCameras(void)
{
    static const char *const classes[] = {
        "info_player_intermission", "info_player_deathmatch", "info_player_start"
    };
    const char  *data;
    ls_ent_t    e;
    int         pass;

    ls.num_cams = 0;
    ls.cam = 0;
    Q_strlcpy(ls.camera_map, cl.mapname, sizeof(ls.camera_map));

    if (!cl.bsp || !cl.bsp->entitystring)
        return;

    for (pass = 0; pass < (int)q_countof(classes) && ls.num_cams < LS_MAX_CAMS; pass++) {
        data = cl.bsp->entitystring;
        while (ls.num_cams < LS_MAX_CAMS && LS_ParseEntity(&data, &e)) {
            if (!e.has_origin || strcmp(e.classname, classes[pass]))
                continue;
            if (pass == 0) {
                if (e.target[0])
                    LS_AimAtTarget(&e);
            } else {
                e.origin[2] += 9 + 22;
            }
            e.angles[ROLL] = 0;
            VectorCopy(e.origin, ls.cam_origin[ls.num_cams]);
            VectorCopy(e.angles, ls.cam_angles[ls.num_cams]);
            ls.num_cams++;
        }
    }
}

/*
=============================================================================

WHAT THE REST OF THE CLIENT ASKS

=============================================================================
*/

bool CL_LevelshotActive(void)
{
    return ls.active && cls.state == ca_active;
}

// the camera for this frame's view, if a shot is being set up; fov_x and
// fov_y for a 4:3 cut of 90 degrees across, whatever the screen's shape
bool CL_LevelshotView(vec3_t origin, vec3_t angles, float width, float height,
                      float *fov_x, float *fov_y)
{
    if (!CL_LevelshotActive())
        return false;

    if (Q_stricmp(ls.camera_map, cl.mapname))
        LS_FindCameras();
    if (!ls.num_cams)
        return false;

    VectorCopy(ls.cam_origin[ls.cam], origin);
    VectorCopy(ls.cam_angles[ls.cam], angles);
    *fov_y = V_CalcFov(90, 4, 3);
    *fov_x = V_CalcFov(*fov_y, height, width);
    return true;
}

/*
=============================================================================

THE LOOK

=============================================================================
*/

static bool LS_IsCampaign(const char *map)
{
    int i;

    for (i = 0; i < (int)q_countof(ls_campaign_maps); i++)
        if (!Q_stricmp(map, ls_campaign_maps[i]))
            return true;
    return false;
}

static bool LS_MapIsQuake3(const char *map)
{
    char        path[MAX_QPATH];
    qhandle_t   f = 0;
    uint32_t    header[2] = { 0, 0 };
    int64_t     len;
    bool        q3;

    if (Q_snprintf(path, sizeof(path), "maps/%s.bsp", map) >= sizeof(path))
        return false;
    len = FS_OpenFile(path, &f, FS_MODE_READ);
    if (!f)
        return false;
    q3 = len >= (int64_t)sizeof(header) &&
         FS_Read(header, sizeof(header), f) == (int)sizeof(header) &&
         LittleLong(header[0]) == IDBSPHEADER && LittleLong(header[1]) == Q3_BSPVERSION;
    FS_CloseFile(f);
    return q3;
}

// Only cvars that exist: the sky's belong to the RTX renderers, and setting
// one under GL or the software renderer would make a stray user cvar of it.
static void LS_Set(const char *name, const char *value)
{
    if (Cvar_FindVar(name))
        Cvar_Set(name, value);
}

static void LS_SaveSettings(void)
{
    cvar_t  *var;
    int     i;

    for (i = 0; i < (int)LS_NUM_SAVED; i++) {
        var = Cvar_FindVar(ls_saved_cvars[i]);
        ls.saved_exists[i] = var != NULL;
        ls.saved_value[i][0] = 0;
        if (var)
            Q_strlcpy(ls.saved_value[i], var->string, sizeof(ls.saved_value[i]));
    }
    ls.have_saved = true;
}

static void LS_PutBack(int i)
{
    if (ls.have_saved && ls.saved_exists[i])
        LS_Set(ls_saved_cvars[i], ls.saved_value[i]);
}

static void LS_RestoreSettings(void)
{
    int i;

    for (i = 0; i < (int)LS_NUM_SAVED; i++)
        LS_PutBack(i);
    ls.have_saved = false;
}

// The arenas' sky. Set before the map loads and again once it is up, because
// a map cfg's "mapcvar" (q3dm17's sun_animate) is undone at the NEXT load and
// would put back the value it found. The space sky only once the map is up:
// maps/default.cfg resets physical_sky_space on every load.
static void LS_ArenaSky(bool loaded)
{
    LS_Set("sky_type", LS_SKY_TYPE_STROGGOS);
    LS_Set("sun_preset", LS_SUN_PRESET_EVENING);
    LS_Set("sun_animate", "0");

    if (loaded && !Q_strncasecmp(cl.configstrings[CS_SKY], "space", 5))
        LS_Set("physical_sky_space", "1");
}

// before a map loads: its sky, and Quake II's guns on a Quake II map
static void LS_SetLook(bool quake3)
{
    int i;

    if (ls.arena) {
        LS_ArenaSky(false);
    } else {
        for (i = 0; i < LS_SAVED_SKY; i++)
            LS_PutBack(i);
    }

    if (quake3)
        LS_PutBack(LS_SAVED_WEAPONS);
    else
        LS_Set("g_q3_weapons", "0");
}

/*
=============================================================================

RUNNING THROUGH THE MAPS

=============================================================================
*/

static bool LS_IsCTF(const char *map)
{
    return !Q_strncasecmp(map, "q2ctf", 5) || !Q_strncasecmp(map, "q2kctf", 6) ||
           !Q_strncasecmp(map, "ndctf", 5);
}

static void LS_FreeBest(void)
{
    Z_Free(ls.best);
    ls.best = NULL;
    ls.best_len = 0;
    ls.best_bright = 0;
    ls.best_cam = 0;
}

static void LS_StartMap(void)
{
    const char  *map = ls.maps[ls.cur];
    bool        quake3;

    ls.map_time = cls.realtime;
    ls.settle_time = 0;
    ls.settle_frames = 0;
    ls.camera_map[0] = 0;
    ls.num_cams = ls.cam = 0;
    ls.look_applied = false;
    LS_FreeBest();

    if (ls.single)
        return;

    quake3 = LS_MapIsQuake3(map);
    ls.arena = !quake3 && !LS_IsCampaign(map);
    LS_SetLook(quake3);

    Com_Printf("Levelshot %d of %d: %s\n", ls.cur + 1, ls.num, map);
    Cbuf_AddText(&cmd_buffer, va("set bot_match 0; set sp_campaign 0; set teamplay 0; "
                                 "set ctf %d; deathmatch 1; coop 0; map %s force\n",
                                 LS_IsCTF(map) ? 1 : 0, map));
}

static void LS_Finish(void)
{
    Com_Printf("Levelshots: %d written, %d failed.\n", ls.saved, ls.failed);
    ls.active = false;
    LS_FreeBest();
    LS_RestoreSettings();
    if (!ls.single)
        Cbuf_AddText(&cmd_buffer, "disconnect\n");
}

static void LS_NextMap(void)
{
    if (++ls.cur >= ls.num) {
        LS_Finish();
        return;
    }
    LS_StartMap();
}

// the brightest picture taken of this map
static void LS_WriteBest(const char *map)
{
    char    path[MAX_QPATH];
    int     ret;

    if (Q_snprintf(path, sizeof(path), "levelshots/%s.jpg", map) >= sizeof(path)) {
        ls.failed++;
        return;
    }

    ret = FS_WriteFile(path, ls.best, ls.best_len);
    if (ret < 0) {
        Com_EPrintf("Levelshot: couldn't write %s: %s\n", path, Q_ErrorString(ret));
        ls.failed++;
        return;
    }

    if (ls.best_cam)
        Com_Printf("Wrote %s (camera %d of %d, brightness %.1f)\n", path,
                   ls.best_cam + 1, ls.num_cams, ls.best_bright);
    else
        Com_Printf("Wrote %s\n", path);
    ls.saved++;
}

/*
=================
CL_LevelshotFrame

After each frame is drawn: once the map is up and the view has held still on
the camera for levelshot_delay seconds, the frame just drawn is the shot -
unless it is dark and there is another camera to try.
=================
*/
void CL_LevelshotFrame(void)
{
    const char  *map;
    void        *data;
    size_t      len;
    float       bright;
    int         ret;

    if (!ls.active)
        return;

    map = ls.maps[ls.cur];

    // still loading, or still on the last map
    if (cls.state != ca_active || cls.disable_screen || !cl.frame.valid ||
        (!ls.single && Q_stricmp(cl.mapname, map))) {
        if (cls.realtime - ls.map_time > LS_LOAD_TIMEOUT) {
            Com_WPrintf("Levelshot: %s did not load; skipped.\n", map);
            ls.failed++;
            LS_NextMap();
        }
        return;
    }

    if (!ls.look_applied) {
        ls.look_applied = true;
        if (ls.arena)
            LS_ArenaSky(true);
    }

    // something in front of the picture: wait for it to go
    if (cls.key_dest != KEY_GAME || !ls.num_cams) {
        ls.settle_time = 0;
        ls.settle_frames = 0;
        if (!ls.num_cams && !Q_stricmp(ls.camera_map, cl.mapname)) {
            Com_WPrintf("Levelshot: %s has no intermission camera or spawn point; skipped.\n", map);
            ls.failed++;
            LS_NextMap();
        }
        return;
    }

    if (!ls.settle_time)
        ls.settle_time = cls.realtime;
    ls.settle_frames++;

    if (cls.realtime - ls.settle_time < Cvar_ClampValue(levelshot_delay, 0, 60) * 1000 ||
        ls.settle_frames < 10)
        return;

    ret = IMG_GrabLevelshot(LS_WIDTH, LS_HEIGHT, LS_QUALITY, &data, &len, &bright);
    if (ret < 0) {
        Com_EPrintf("Levelshot: couldn't grab %s: %s\n", map, Q_ErrorString(ret));
        ls.failed++;
        LS_NextMap();
        return;
    }

    if (!ls.best || bright > ls.best_bright) {
        LS_FreeBest();
        ls.best = data;
        ls.best_len = len;
        ls.best_bright = bright;
        ls.best_cam = ls.cam;
    } else {
        Z_Free(data);
    }

    // too dark to see anything from here: stand somewhere else while there is
    // somewhere to stand
    if (bright < Cvar_ClampValue(levelshot_dark, 0, 255) && ls.cam + 1 < ls.num_cams) {
        Com_Printf("Levelshot: %s is dark from camera %d (brightness %.1f); trying camera %d of %d.\n",
                   map, ls.cam + 1, bright, ls.cam + 2, ls.num_cams);
        ls.cam++;
        ls.settle_time = 0;
        ls.settle_frames = 0;
        return;
    }

    LS_WriteBest(map);
    LS_NextMap();
}

static bool LS_HasShot(const char *map)
{
    return FS_FileExists(va("levelshots/%s.jpg", map)) || FS_FileExists(va("levelshots/%s.png", map)) ||
           FS_FileExists(va("levelshots/%s.tga", map));
}

static void LS_AddMap(const char *map, bool force)
{
    if (ls.num >= LS_MAX_MAPS)
        return;
    if (!FS_FileExists(va("maps/%s.bsp", map))) {
        Com_DPrintf("Levelshots: no maps/%s.bsp\n", map);
        return;
    }
    if (!force && LS_HasShot(map))
        return;
    Q_strlcpy(ls.maps[ls.num++], map, MAX_QPATH);
}

static void CL_Levelshots_f(void)
{
    bool    force = false;
    int     i, first = 1;

    if (Cmd_Argc() > 1 && !Q_stricmp(Cmd_Argv(1), "stop")) {
        if (ls.active) {
            Com_Printf("Levelshots stopped.\n");
            LS_Finish();
        }
        return;
    }
    if (ls.active) {
        Com_Printf("Levelshots are already being taken; \"levelshots stop\" to give up.\n");
        return;
    }
    if (Cmd_Argc() > 1 && !Q_stricmp(Cmd_Argv(1), "force")) {
        force = true;
        first = 2;
    }

    memset(&ls, 0, sizeof(ls));
    if (Cmd_Argc() > first) {
        for (i = first; i < Cmd_Argc(); i++)
            LS_AddMap(Cmd_Argv(i), true);
    } else {
        for (i = 0; i < (int)q_countof(ls_arena_maps); i++)
            LS_AddMap(ls_arena_maps[i], force);
        for (i = 0; i < (int)q_countof(ls_campaign_maps); i++)
            LS_AddMap(ls_campaign_maps[i], force);
    }

    if (!ls.num) {
        Com_Printf("Levelshots: nothing to do - every map has one (\"levelshots force\" redoes them).\n");
        return;
    }

    Com_Printf("Levelshots: %d map%s, %g seconds each to settle.\n", ls.num, ls.num == 1 ? "" : "s",
               levelshot_delay->value);
    LS_SaveSettings();
    ls.active = true;
    UI_OpenMenu(UIMENU_NONE);
    Con_Close(true);
    LS_StartMap();
}

static void CL_Levelshot_f(void)
{
    if (ls.active) {
        Com_Printf("Levelshots are already being taken.\n");
        return;
    }
    if (cls.state != ca_active || !cl.mapname[0]) {
        Com_Printf("Not on a map; \"levelshots <map>\" loads one.\n");
        return;
    }

    memset(&ls, 0, sizeof(ls));
    Q_strlcpy(ls.maps[0], cl.mapname, MAX_QPATH);
    ls.num = 1;
    ls.single = true;
    // the map is already loaded, so only its sky can change here, not its guns
    ls.arena = !(cl.bsp && cl.bsp->q3) && !LS_IsCampaign(cl.mapname);
    LS_SaveSettings();
    ls.active = true;
    UI_OpenMenu(UIMENU_NONE);
    Con_Close(true);
    LS_StartMap();
}

// quitting in the middle of a run: the player's settings back before the
// config is written
void CL_ShutdownLevelshots(void)
{
    if (!ls.active)
        return;
    ls.active = false;
    LS_FreeBest();
    LS_RestoreSettings();
}

static void LS_Maps_c(genctx_t *ctx, int argnum)
{
    if (argnum >= 1)
        Prompt_AddMatch(ctx, "force");
}

static const cmdreg_t c_levelshot[] = {
    { "levelshots", CL_Levelshots_f, LS_Maps_c },
    { "levelshot", CL_Levelshot_f },
    { NULL }
};

void CL_InitLevelshots(void)
{
    levelshot_delay = Cvar_Get("levelshot_delay", "5", 0);
    levelshot_dark = Cvar_Get("levelshot_dark", "3", 0);
    Cmd_Register(c_levelshot);
}
