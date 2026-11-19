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
game's write directory (IMG_SaveLevelshot, refresh/images.c). With no maps
named it does every map the bot match page lists that has no levelshot yet;
"force" redoes the ones that have. `levelshot` does the map you are on,
without changing level. `levelshots stop` gives up.

While a shot is being taken the HUD, the console's notify lines, your gun
and your own body are left out, and the field of view is Quake III's: 90
degrees across the 4:3 that is kept. An open menu or console holds the
capture back until it is closed again.
*/

#include "client.h"
#include "refresh/images.h"

#define LS_WIDTH        640
#define LS_HEIGHT       480
#define LS_QUALITY      90
#define LS_MAX_MAPS     128
#define LS_LOAD_TIMEOUT 120000      // ms for a map to come up before it is skipped

static cvar_t   *levelshot_delay;   // seconds the picture settles before it is taken

// every map the bot match page offers (baseq2/q2rtx.menu); missing ones are skipped
static const char *const ls_default_maps[] = {
    "q2dm1", "q2dm2", "q2dm3", "q2dm4", "q2dm5", "q2dm6", "q2dm7", "q2dm8",
    "mgdm1",
    "xdm1", "xdm2", "xdm3", "xdm4", "xdm5", "xdm6", "xdm7",
    "rdm1", "rdm2", "rdm3", "rdm4", "rdm5", "rdm6", "rdm7", "rdm8", "rdm9",
    "rdm10", "rdm11", "rdm12", "rdm13", "rdm14",
    "q64/dm1", "q64/dm2", "q64/dm3", "q64/dm4", "q64/dm5", "q64/dm6",
    "q64/dm7", "q64/dm8", "q64/dm9", "q64/dm10",
    "q2ctf1", "q2ctf2", "q2ctf3", "q2ctf4", "q2ctf5", "q2kctf1", "q2kctf2", "ndctf0",
};

static struct {
    bool        active;
    bool        single;             // `levelshot`: the map already loaded
    char        maps[LS_MAX_MAPS][MAX_QPATH];
    int         num, cur;
    int         saved, failed;

    unsigned    map_time;           // cls.realtime the current map was asked for
    unsigned    settle_time;        // cls.realtime the view first held still, 0 = not yet
    int         settle_frames;

    char        camera_map[MAX_QPATH];
    bool        have_camera;
    vec3_t      origin, angles;
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

// where the shot is taken from: the map's first intermission camera
static bool LS_FindCamera(void)
{
    ls_ent_t    e, spot, target;
    const char  *data;
    bool        have_spot = false, spot_is_camera = false;

    ls.have_camera = false;
    Q_strlcpy(ls.camera_map, cl.mapname, sizeof(ls.camera_map));

    if (!cl.bsp || !cl.bsp->entitystring)
        return false;

    data = cl.bsp->entitystring;
    while (LS_ParseEntity(&data, &e)) {
        if (!e.has_origin)
            continue;
        if (!strcmp(e.classname, "info_player_intermission")) {
            spot = e;
            have_spot = spot_is_camera = true;
            break;
        }
        if (!have_spot && (!strcmp(e.classname, "info_player_deathmatch") ||
                           !strcmp(e.classname, "info_player_start"))) {
            spot = e;
            have_spot = true;
        }
    }
    if (!have_spot)
        return false;

    VectorCopy(spot.origin, ls.origin);
    VectorCopy(spot.angles, ls.angles);

    if (!spot_is_camera) {
        // a spawn point: where the eye of someone spawned there would be
        ls.origin[2] += 9 + 22;
    } else if (spot.target[0]) {
        // Quake III aims its intermission camera at a target
        data = cl.bsp->entitystring;
        while (LS_ParseEntity(&data, &target)) {
            vec3_t dir;

            if (!target.has_origin || strcmp(target.targetname, spot.target))
                continue;
            VectorSubtract(target.origin, spot.origin, dir);
            vectoangles2(dir, ls.angles);
            break;
        }
    }

    ls.angles[ROLL] = 0;
    ls.have_camera = true;
    return true;
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
        LS_FindCamera();
    if (!ls.have_camera)
        return false;

    VectorCopy(ls.origin, origin);
    VectorCopy(ls.angles, angles);
    *fov_y = V_CalcFov(90, 4, 3);
    *fov_x = V_CalcFov(*fov_y, height, width);
    return true;
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

static void LS_StartMap(void)
{
    const char *map = ls.maps[ls.cur];

    ls.map_time = cls.realtime;
    ls.settle_time = 0;
    ls.settle_frames = 0;
    ls.camera_map[0] = 0;
    ls.have_camera = false;

    if (ls.single)
        return;

    Com_Printf("Levelshot %d of %d: %s\n", ls.cur + 1, ls.num, map);
    Cbuf_AddText(&cmd_buffer, va("set bot_match 0; set sp_campaign 0; set teamplay 0; "
                                 "set ctf %d; deathmatch 1; coop 0; map %s force\n",
                                 LS_IsCTF(map) ? 1 : 0, map));
}

static void LS_Finish(void)
{
    Com_Printf("Levelshots: %d written, %d failed.\n", ls.saved, ls.failed);
    ls.active = false;
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

/*
=================
CL_LevelshotFrame

After each frame is drawn: once the map is up and the view has held still on
the camera for levelshot_delay seconds, the frame just drawn is the shot.
=================
*/
void CL_LevelshotFrame(void)
{
    const char  *map;
    char        path[MAX_QPATH];
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

    // something in front of the picture: wait for it to go
    if (cls.key_dest != KEY_GAME || !ls.have_camera) {
        ls.settle_time = 0;
        ls.settle_frames = 0;
        if (!ls.have_camera && !Q_stricmp(ls.camera_map, cl.mapname)) {
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

    if (Q_snprintf(path, sizeof(path), "levelshots/%s.jpg", map) >= sizeof(path)) {
        ls.failed++;
    } else {
        ret = IMG_SaveLevelshot(path, LS_WIDTH, LS_HEIGHT, LS_QUALITY);
        if (ret < 0) {
            Com_EPrintf("Levelshot: couldn't write %s: %s\n", path, Q_ErrorString(ret));
            ls.failed++;
        } else {
            Com_Printf("Wrote %s\n", path);
            ls.saved++;
        }
    }

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
        for (i = 0; i < (int)q_countof(ls_default_maps); i++)
            LS_AddMap(ls_default_maps[i], force);
    }

    if (!ls.num) {
        Com_Printf("Levelshots: nothing to do - every map has one (\"levelshots force\" redoes them).\n");
        return;
    }

    Com_Printf("Levelshots: %d map%s, %g seconds each to settle.\n", ls.num, ls.num == 1 ? "" : "s",
               levelshot_delay->value);
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
    ls.active = true;
    UI_OpenMenu(UIMENU_NONE);
    Con_Close(true);
    LS_StartMap();
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
    Cmd_Register(c_levelshot);
}
