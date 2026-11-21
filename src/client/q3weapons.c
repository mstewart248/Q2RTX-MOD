/*
Copyright (C) 1999-2005 Id Software, Inc.

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
==============================================================================

QUAKE III ARENA'S WEAPONS, THE CLIENT'S HALF

The bot match's g_q3_weapons (the game's half is baseq2 g_q3weap.c; the rule
reaches here in CS_Q3RULES as cl.q3_weapons). What Quake III's guns look like:

  - the view weapon, put together as cg_weapons.c does: the hand model (only
    tags, it draws nothing) animated from the gun's state in ps.gunframe, the
    gun on its tag_weapon, the barrel on the gun's tag_barrel - spinning
    while the machinegun, the gauntlet or the BFG fires - and the flash on
    its tag_flash
  - the same gun in a Quake III player model's hand, and on a pickup
  - a pickup carrying both games' guns (RF_Q3_ITEM): each player sees their
    own game's
  - the plasma ball, Quake III's sprite (RF_Q3_SPRITE)
  - the Lightning Gun's bolt, from the gun to what it hits, every frame
  - the impacts: Quake III's sounds, its bullet and plasma flashes, and its
    marks - bullet holes, scorches, plasma burns (RF_DECAL)
  - spent brass, ejected by Quake III's machinegun and shotgun and, when the
    Quake III data is there, by Quake II's shotguns, machinegun and chaingun

Rockets, grenades, the rail and the BFG keep Quake II's Flow effects (tent.c
and entities.c); the sounds of firing are the server's.

Brass and marks stay until the level ends, unless it is a network game - a
server elsewhere, or this one with a player from another machine - where they
go as Quake III's do (cl_brass_time, cl_q3_marks_time). Matt, 11-18: "I want
these to never despawn unless you are playing an actual network game."

==============================================================================
*/

#include "client.h"
#include "format/md3.h"

#define Q3W_NUM     10      // the game's Q3W_*: 1 gauntlet .. 9 BFG

// the renderer tells meshes apart for motion vectors by entity id (14 bits)
// and model together: a gun's parts take their gun's id, the brass these
#define Q3W_BRASS_ID    0x2000

static float q3w_clip01(float f)
{
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

typedef struct {
    vec3_t  origin;
    vec3_t  axis[3];
} q3wtag_t;

typedef struct {
    const char  *dir;           // models/weapons2/<dir>/<dir>.md3
    vec3_t      light;          // the flash's light (cg_weapons.c)
    const char  *q2_vwep;       // what a Quake II body holds instead
    bool        has_barrel;
    bool        continuous;     // the flash stays while firing (LG, gauntlet)
    float       flash_scale;    // its flash model's size (0 = half, q3w_flash_scale)

    qhandle_t   world, barrel, flash;
    q3wtag_t    *hand_tags;     // tag_weapon on each frame of the hand
    int         hand_frames;
    q3wtag_t    barrel_tag, flash_tag;
    bool        have_barrel_tag, have_flash_tag;
} q3wgun_t;

static q3wgun_t q3w_guns[Q3W_NUM] = {
    [1] = { "gauntlet",   { 0.6f, 0.6f, 1.0f  }, "w_blaster.md2",     true,  true  },
    // the machinegun's flash at half its size: as made it fills the view
    // (Matt, 11-18: "too large... cut the size in half")
    [2] = { "machinegun", { 1.0f, 1.0f, 0.0f  }, "w_machinegun.md2",  true,  false, 0.5f },
    [3] = { "shotgun",    { 1.0f, 1.0f, 0.0f  }, "w_sshotgun.md2",    false, false },
    [4] = { "grenadel",   { 1.0f, 0.7f, 0.0f  }, "w_glauncher.md2",   false, false },
    [5] = { "rocketl",    { 1.0f, 0.75f, 0.0f }, "w_rlauncher.md2",   false, false },
    [6] = { "lightning",  { 0.6f, 0.6f, 1.0f  }, "w_chaingun.md2",    false, true  },
    [7] = { "railgun",    { 1.0f, 0.5f, 0.0f  }, "w_railgun.md2",     false, false },
    [8] = { "plasma",     { 0.6f, 0.6f, 1.0f  }, "w_hyperblaster.md2", false, false },
    [9] = { "bfg",        { 1.0f, 0.7f, 1.0f  }, "w_bfg.md2",         true,  false },
};

enum { Q3W_GAUNTLET = 1, Q3W_MACHINEGUN, Q3W_SHOTGUN, Q3W_GRENADE, Q3W_ROCKET,
       Q3W_LIGHTNING, Q3W_RAILGUN, Q3W_PLASMA, Q3W_BFG };

// a spent case as a cylinder along the model's Z: see "HOW A SHELL FALLS"
typedef struct {
    float       radius, zmin, zmax;
} q3wshell_t;

static struct {
    bool        guns;               // the guns are registered (cl.q3_weapons)
    qhandle_t   brass[2];           // machinegun, shotgun
    q3wshell_t  shell[2];
    qhandle_t   mark_bullet, mark_burn, mark_hole, mark_energy;
    qhandle_t   plasma_sprite, plasma_boom, electric, bullet_flash[3];
    qhandle_t   lightning;
    qhandle_t   sfx_ric[3], sfx_lghit[3], sfx_plasmaexp, sfx_rockexp;
    // Q2RTX's railgun in Quake III's place, view and pickup (0 = Quake III's
    // own): see q3w_want_rtx_rail
    qhandle_t   rtx_rail_view, rtx_rail_world;
    // Quake III's gibs and blood (GIBS, below): the pieces and the box each
    // collides as, its gib sounds, and the blood pictures shown when the
    // blood simulation is off - the trail's puff, the stain, the spurt
    bool        gibs_ready;
    qhandle_t   gib_model[10];
    vec3_t      gib_mins[10], gib_maxs[10];
    qhandle_t   sfx_gib, sfx_gibimp[3];
    qhandle_t   blood_trail, mark_blood, blood_spurt[3];
} q3w;

static cvar_t   *cl_brass_time;
static cvar_t   *cl_q3_flash_scale;
static cvar_t   *cl_brass_max;
static cvar_t   *cl_q3_marks_time;
static cvar_t   *cl_q3_marks_max;
static cvar_t   *cl_q3_debug;
static cvar_t   *cl_q3_plasma_from_gun;
static cvar_t   *cl_q3_gibs_time;
static cvar_t   *cl_q3_gibs_max;
static cvar_t   *cl_q3_gib_drops;

static void q3p_register(void);         // PREDICTED SHOTS

// cl_q3_debug 1: what the plasma ball, the marks and the impact flashes did,
// once a second (q3w_debug_print) - Matt saw none of them on 11-18; the
// renderer's own line (transparency.c) says whether they reached it
enum { Q3W_REFUSED_NO_IMAGE, Q3W_REFUSED_RENDERER, Q3W_REFUSED_OFF, Q3W_REFUSED_NO_SURFACE,
       Q3W_REFUSED_NO_FIT, Q3W_NUM_REFUSED };
static struct {
    int         balls, balls_drawn, marks, refused[Q3W_NUM_REFUSED], marks_drawn, fx_drawn;
    unsigned    last;
} q3w_dbg;

/*
==============================================================================

WHOSE GUNS, AND WHOSE GAME

==============================================================================
*/

/*
=================
CL_Q3W_Self

Whether our own player has Quake III's guns: everyone does with g_q3_weapons
1, and with 2 whoever plays a Quake III model - the game's test (g_q3weap.c
Q3W_ModelIsQ3), on the model our own skin names.
=================
*/
bool CL_Q3W_Self(void)
{
    static int      framecount = -1;
    static bool     self;
    const char      *s, *slash;
    char            model[MAX_QPATH];
    size_t          len;

    if (cl.q3_weapons == 1)
        return true;
    if (cl.q3_weapons != 2)
        return false;
    if (framecount == cls.framecount)
        return self;
    framecount = cls.framecount;

    if (cl.clientinfo[cl.clientNum].q3.q3)
        return self = true;

    // "name\model/skin"
    s = cl.configstrings[CS_PLAYERSKINS + cl.clientNum];
    s = strchr(s, '\\') ? strchr(s, '\\') + 1 : s;
    slash = strchr(s, '/');
    len = slash ? (size_t)(slash - s) : 0;
    if (!len || len >= sizeof(model))
        return self = false;
    memcpy(model, s, len);
    model[len] = 0;
    return self = Q3P_ModelExists(model);
}

/*
=================
q3w_network_game

Brass and marks fade in a network game only: a server on another machine,
or ours with a player from one. Bots are not clients.
=================
*/
static bool q3w_network_game(void)
{
    if (cls.demo.playback)
        return false;
    if (!NET_IsLocalAddress(&cls.serverAddress))
        return true;
    return SV_HasRemoteClients();
}

// how long a piece of brass or a mark lasts: 0 for ever
static int q3w_life(cvar_t *var, int network_ms)
{
    if (var->integer > 0)
        return var->integer;
    return q3w_network_game() ? network_ms : 0;
}

/*
==============================================================================

MEDIA

==============================================================================
*/

// one tag of an .md3, per frame; NULL if it lacks it
static q3wtag_t *q3w_load_tags(const char *path, const char *tagname, int *num_frames)
{
    byte                *data;
    dmd3header_t        h;
    const dmd3tag_t     *src;
    q3wtag_t            *out = NULL;
    int                 len, frames, tags, ofs, f, t, k;

    *num_frames = 0;
    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return NULL;

    if (len < (int)sizeof(h))
        goto done;
    memcpy(&h, data, sizeof(h));
    if (LittleLong(h.ident) != MD3_IDENT || LittleLong(h.version) != MD3_VERSION)
        goto done;
    frames = LittleLong(h.num_frames);
    tags = LittleLong(h.num_tags);
    ofs = LittleLong(h.ofs_tags);
    if (frames < 1 || frames > MD3_MAX_FRAMES || tags < 1 || tags > MD3_MAX_TAGS || ofs < 0)
        goto done;
    if ((uint64_t)ofs + (uint64_t)frames * tags * sizeof(dmd3tag_t) > (uint64_t)len)
        goto done;

    src = (const dmd3tag_t *)(data + ofs);
    for (t = 0; t < tags; t++)
        if (!Q_strncasecmp(src[t].name, tagname, MD3_MAX_PATH))
            break;
    if (t == tags)
        goto done;

    out = Z_Mallocz(sizeof(*out) * frames);
    for (f = 0; f < frames; f++) {
        const dmd3tag_t *tag = src + f * tags + t;

        for (k = 0; k < 3; k++) {
            out[f].origin[k] = LittleFloat(tag->origin[k]);
            out[f].axis[0][k] = LittleFloat(tag->axis[0][k]);
            out[f].axis[1][k] = LittleFloat(tag->axis[1][k]);
            out[f].axis[2][k] = LittleFloat(tag->axis[2][k]);
        }
    }
    *num_frames = frames;

done:
    FS_FreeFile(data);
    return out;
}

static bool q3w_load_tag0(const char *path, const char *tagname, q3wtag_t *out)
{
    int         frames;
    q3wtag_t    *tags = q3w_load_tags(path, tagname, &frames);

    if (!tags)
        return false;
    *out = tags[0];
    Z_Free(tags);
    return true;
}

// a shell's size, from its first frame's bounds; (r, zmin, zmax) without them
static void q3w_load_shell(const char *path, q3wshell_t *out, float r, float zmin, float zmax)
{
    byte            *data;
    dmd3header_t    h;
    dmd3frame_t     f;
    int             len, ofs;

    out->radius = r;
    out->zmin = zmin;
    out->zmax = zmax;

    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return;
    if (len >= (int)sizeof(h)) {
        memcpy(&h, data, sizeof(h));
        ofs = LittleLong(h.ofs_frames);
        if (LittleLong(h.ident) == MD3_IDENT && LittleLong(h.num_frames) >= 1 &&
            ofs >= 0 && (uint64_t)ofs + sizeof(f) <= (uint64_t)len) {
            memcpy(&f, data + ofs, sizeof(f));
            float rx = (LittleFloat(f.maxs[0]) - LittleFloat(f.mins[0])) * 0.5f;
            float ry = (LittleFloat(f.maxs[1]) - LittleFloat(f.mins[1])) * 0.5f;
            if (rx > 0 && ry > 0 && LittleFloat(f.maxs[2]) > LittleFloat(f.mins[2])) {
                out->radius = (rx + ry) * 0.5f;
                out->zmin = LittleFloat(f.mins[2]);
                out->zmax = LittleFloat(f.maxs[2]);
            }
        }
    }
    FS_FreeFile(data);
}

static void q3w_free_guns(void)
{
    int i;

    for (i = 1; i < Q3W_NUM; i++) {
        Z_Free(q3w_guns[i].hand_tags);
        q3w_guns[i].hand_tags = NULL;
        q3w_guns[i].hand_frames = 0;
        q3w_guns[i].world = q3w_guns[i].barrel = q3w_guns[i].flash = 0;
        q3w_guns[i].have_barrel_tag = q3w_guns[i].have_flash_tag = false;
    }
    q3w.guns = false;
}

// a mark's or sprite's picture: baseq3/materialDev's copy when there is one,
// as quake3.mat's entries name the models' (edit the copy, the game changes),
// or the pak's own.
//
// The leading slash matters: R_RegisterImage puts "pics/" in front of every
// name but a skin's that does not start with one, and a picture that is not
// there fails without a word. Without it every one of these was
// pics/sprites/plasmaa.jpg and so on, all came back 0, and the plasma ball,
// the marks and the impact flashes drew nothing (Matt, 11-18 - the ball's
// light, added before the picture is looked at, was all there was).
static qhandle_t q3w_image(const char *name)
{
    char        path[MAX_QPATH];
    qhandle_t   image = 0;

    if (Q_concat(path, sizeof(path), "/materialDev/", name) < sizeof(path) && FS_FileExists(path + 1))
        image = R_RegisterImage(path, IT_SPRITE, IF_SRGB, NULL);
    if (!image && Q_concat(path, sizeof(path), "/", name) < sizeof(path))
        image = R_RegisterImage(path, IT_SPRITE, IF_SRGB, NULL);
    if (!image)
        Com_WPrintf("Quake III weapons: no picture %s\n", name);
    return image;
}

static qhandle_t q3w_sound(const char *name)
{
    return S_RegisterSound(va("#baseq3/sound/%s", name));
}

/*
=================
q3w_want_rtx_rail

Whether Q2RTX's redesigned railgun stands in for Quake III's, in the view
and on the ground (Matt, 11-19: "When use rtx rail gun is true can we show
that weapon instead of the quake 3 rail gun. If use classic rail gun is true
we can show the quake 3 version"). cl_classic_railgun 0 does it, and only
where the renderer takes the redesign's .md3s (refresh/models.c's try_md3):
without them v_rail is the 1997 gun, and Quake III's own is the better one
to keep. A Quake III body still holds Quake III's: Quake II's view and
pickup models have no grip on its tag_weapon (q3player.c).
=================
*/
static bool q3w_want_rtx_rail(void)
{
    if (Cvar_VariableInteger("cl_classic_railgun"))
        return false;
    if (!CL_PathTraced() && !(cls.ref_type == REF_TYPE_GL && Cvar_VariableInteger("gl_use_hd_assets")))
        return false;
    return FS_FileExists("models/weapons/v_rail/tris.md3") && FS_FileExists("models/weapons/g_rail/tris.md3");
}

/*
=================
q3w_q3_game

Whether Quake III's game is being played: a Quake III arena, or Quake III's
guns or movement on any map (CS_Q3RULES). The game throws Quake III's gibs
under the same rules (g_q3.c Q3_GibStyle), so the gibs' media is loaded
then, and Quake III's blood shown.
=================
*/
static bool q3w_q3_game(void)
{
    return (cl.bsp && cl.bsp->q3) || cl.q3_weapons || cl.pmp.q3move;
}

// Quake III's own blood pictures rather than the blood simulation's spheres:
// the path tracers draw them (as the marks), and only with the spheres off.
// Without Quake III's data, Quake II's blood.
static bool q3w_blood_pictures(void)
{
    return CL_PathTraced() && !CL_BloodSpheresActive() && q3w.blood_trail;
}

// the box a gib collides as: its .md3's bounds, narrowed to what lies under it
// whichever way it is turned (it turns about Z only), so it rests on the
// floor rather than half in it, as Quake III's point did
static void q3w_gib_box(const char *path, vec3_t mins, vec3_t maxs)
{
    byte            *data;
    dmd3header_t    h;
    dmd3frame_t     f;
    int             len, ofs;

    VectorSet(mins, -3, -3, -3);
    VectorSet(maxs, 3, 3, 3);

    len = FS_LoadFile(path, (void **)&data);
    if (!data)
        return;
    if (len >= (int)sizeof(h)) {
        memcpy(&h, data, sizeof(h));
        ofs = LittleLong(h.ofs_frames);
        if (LittleLong(h.ident) == MD3_IDENT && LittleLong(h.num_frames) >= 1 &&
            ofs >= 0 && (uint64_t)ofs + sizeof(f) <= (uint64_t)len) {
            memcpy(&f, data + ofs, sizeof(f));
            float rx = (LittleFloat(f.maxs[0]) - LittleFloat(f.mins[0])) * 0.5f;
            float ry = (LittleFloat(f.maxs[1]) - LittleFloat(f.mins[1])) * 0.5f;
            float r = min(min(rx, ry), 6.0f);
            float zmin = LittleFloat(f.mins[2]), zmax = LittleFloat(f.maxs[2]);

            if (r > 0.5f && zmax > zmin) {
                VectorSet(mins, -r, -r, max(zmin, -8.0f));
                VectorSet(maxs, r, r, min(zmax, 8.0f));
            }
        }
    }
    FS_FreeFile(data);
}

/*
=================
q3w_register_gibs

Quake III's gibs (cg_main.c): the ten pieces (models/gibs), their sounds, and
its blood pictures. A piece this install lacks is Quake II's chunk of meat.
=================
*/
static const char *const q3w_gib_names[10] = {
    "skull", "brain", "abdomen", "arm", "chest", "fist", "foot", "forearm", "intestine", "leg"
};

static void q3w_register_gibs(void)
{
    char    path[MAX_QPATH];
    int     i;

    for (i = 0; i < 10; i++) {
        Q_snprintf(path, sizeof(path), "models/gibs/%s.md3", q3w_gib_names[i]);
        if (!FS_FileExists(path))
            Q_strlcpy(path, "models/objects/gibs/sm_meat/tris.md2", sizeof(path));
        q3w.gib_model[i] = R_RegisterModel(path);
        q3w_gib_box(path, q3w.gib_mins[i], q3w.gib_maxs[i]);
    }
    // without Quake III's data: Quake II's gib sound, and no splats
    if (!Cvar_VariableInteger("fs_quake3")) {
        q3w.sfx_gib = S_RegisterSound("misc/udeath.wav");
        q3w.gibs_ready = true;
        return;
    }

    q3w.sfx_gib = q3w_sound("player/gibsplt1.wav");
    for (i = 0; i < 3; i++)
        q3w.sfx_gibimp[i] = q3w_sound(va("player/gibimp%d.wav", i + 1));

    {
        q3w.blood_trail = q3w_image("gfx/damage/blood_spurt.tga");
        q3w.mark_blood = q3w_image("gfx/damage/blood_stain.tga");
        // its bloodExplosion's animmap runs at 5 a second for half a second:
        // the first three of its five pictures
        for (i = 0; i < 3; i++)
            q3w.blood_spurt[i] = q3w_image(va("models/weaphits/blood20%d.tga", i + 1));
    }
    q3w.gibs_ready = true;
}

/*
=================
CL_Q3W_RegisterMedia

CL_RegisterTEntModels, at each level's start: the brass whenever the Quake
III data is there (Quake II's guns throw it too), the gibs and the blood in
Quake III's game (q3w_q3_game), and the rest in a game with Quake III's guns.
=================
*/
void CL_Q3W_RegisterMedia(void)
{
    char    path[MAX_QPATH];
    int     i;

    q3w_free_guns();
    memset(&q3w, 0, sizeof(q3w));

    if (!Cvar_VariableInteger("fs_quake3"))
        return;

    q3w.brass[0] = R_RegisterModel("models/weapons2/shells/m_shell.md3");
    q3w.brass[1] = R_RegisterModel("models/weapons2/shells/s_shell.md3");
    q3w_load_shell("models/weapons2/shells/m_shell.md3", &q3w.shell[0], 0.42f, -1.69f, 1.86f);
    q3w_load_shell("models/weapons2/shells/s_shell.md3", &q3w.shell[1], 0.92f, -3.56f, 2.05f);

    if (q3w_q3_game())
        q3w_register_gibs();

    if (!cl.q3_weapons)
        return;

    for (i = 1; i < Q3W_NUM; i++) {
        q3wgun_t *g = &q3w_guns[i];

        Q_snprintf(path, sizeof(path), "models/weapons2/%s/%s.md3", g->dir, g->dir);
        g->world = R_RegisterModel(path);
        g->have_barrel_tag = q3w_load_tag0(path, "tag_barrel", &g->barrel_tag);
        g->have_flash_tag = q3w_load_tag0(path, "tag_flash", &g->flash_tag);

        Q_snprintf(path, sizeof(path), "models/weapons2/%s/%s_hand.md3", g->dir, g->dir);
        // only its tags: it has no surfaces, and the model loaders want some
        g->hand_tags = q3w_load_tags(path, "tag_weapon", &g->hand_frames);

        if (g->has_barrel) {
            Q_snprintf(path, sizeof(path), "models/weapons2/%s/%s_barrel.md3", g->dir, g->dir);
            g->barrel = R_RegisterModel(path);
        }
        Q_snprintf(path, sizeof(path), "models/weapons2/%s/%s_flash.md3", g->dir, g->dir);
        g->flash = R_RegisterModel(path);
    }
    q3w.guns = true;

    // (cl_classic_railgun is CVAR_FILES: flipping it re-registers, and lands here)
    if (q3w_want_rtx_rail()) {
        q3w.rtx_rail_view = R_RegisterModel("models/weapons/v_rail/tris.md2");
        q3w.rtx_rail_world = R_RegisterModel("models/weapons/g_rail/tris.md2");
    }

    q3w.mark_bullet = q3w_image("gfx/damage/bullet_mrk.jpg");
    q3w.mark_burn = q3w_image("gfx/damage/burn_med_mrk.jpg");
    q3w.mark_hole = q3w_image("gfx/damage/hole_lg_mrk.jpg");
    q3w.mark_energy = q3w_image("gfx/damage/plasma_mrk.tga");
    q3w.plasma_sprite = q3w_image("sprites/plasmaa.jpg");
    q3w.plasma_boom = q3w_image("models/weaphits/plasmaboom.jpg");
    q3w.electric = q3w_image("models/weaphits/electric.jpg");
    for (i = 0; i < 3; i++)
        q3w.bullet_flash[i] = q3w_image(va("models/weaphits/bullet%d.jpg", i + 1));
    if (cl_q3_debug->integer)
        Com_Printf("q3 weapons images (0 = not found): marks bullet %d, burn %d, hole %d, energy %d; "
                   "plasma %d, plasma boom %d, electric %d, bullet flashes %d %d %d\n",
                   q3w.mark_bullet, q3w.mark_burn, q3w.mark_hole, q3w.mark_energy, q3w.plasma_sprite,
                   q3w.plasma_boom, q3w.electric, q3w.bullet_flash[0], q3w.bullet_flash[1], q3w.bullet_flash[2]);

    q3w.lightning = R_RegisterModel("models/proj/lightning/tris.md2");

    for (i = 0; i < 3; i++)
        q3w.sfx_ric[i] = q3w_sound(va("weapons/machinegun/ric%d.wav", i + 1));
    q3w.sfx_lghit[0] = q3w_sound("weapons/lightning/lg_hit.wav");
    q3w.sfx_lghit[1] = q3w_sound("weapons/lightning/lg_hit2.wav");
    q3w.sfx_lghit[2] = q3w_sound("weapons/lightning/lg_hit3.wav");
    q3w.sfx_plasmaexp = q3w_sound("weapons/plasma/plasmx1a.wav");
    q3w.sfx_rockexp = q3w_sound("weapons/rocket/rocklx1a.wav");

    q3p_register();
}

static q3wgun_t *q3w_gun_for_model(qhandle_t model)
{
    int i;

    if (!q3w.guns || !model)
        return NULL;
    for (i = 1; i < Q3W_NUM; i++)
        if (q3w_guns[i].world == model)
            return &q3w_guns[i];
    return NULL;
}

// how big a gun's flash is drawn, on a gun drawn at `scale`: its own size
// (flash_scale) times cl_q3_flash_scale, the knob for all of them. Half the
// size as made unless the gun says otherwise: the machinegun's halved flash
// was right, and the plasma gun's as made was "too large" too (Matt, 11-18) -
// every Quake III flash model is about 36 units across, made for its 90-degree
// view with no path-traced glow round it.
static float q3w_flash_scale(const q3wgun_t *g, float scale)
{
    float s = g->flash_scale > 0 ? g->flash_scale : 0.5f;

    if (cl_q3_flash_scale->value > 0)
        s *= cl_q3_flash_scale->value;
    return s * (scale > 0 ? scale : 1.0f);
}

// a Quake III gun's model, held or on the ground (q3player.c: no grip for it)
bool CL_Q3W_IsGun(qhandle_t model)
{
    return q3w_gun_for_model(model) != NULL;
}

/*
=================
CL_Q3W_ItemModel

entities.c, a pickup's model once chosen: Quake III's railgun becomes Q2RTX's
redesign when that stands in for it (q3w_want_rtx_rail), at Quake II's size -
the 1.5 Quake III draws its guns at on the ground goes with it.
=================
*/
void CL_Q3W_ItemModel(entity_t *ent)
{
    if (q3w.rtx_rail_world && ent->model && ent->model == q3w_guns[Q3W_RAILGUN].world) {
        ent->model = q3w.rtx_rail_world;
        ent->scale = 0;
    }
}

/*
=================
CL_Q3W_VWepFallback

CL_LoadClientinfo: what a Quake II body holds for a Quake III gun's vwep
("models/weapons2/<dir>/<dir>.md3") - Quake II's own counterpart, since its
models have no hand to bolt Quake III's to. NULL for any other name.
=================
*/
const char *CL_Q3W_VWepFallback(const char *vwep)
{
    int i;

    if (Q_strncasecmp(vwep, "models/weapons2/", 16))
        return NULL;
    for (i = 1; i < Q3W_NUM; i++) {
        size_t len = strlen(q3w_guns[i].dir);

        if (!Q_strncasecmp(vwep + 16, q3w_guns[i].dir, len) && vwep[16 + len] == '/')
            return q3w_guns[i].q2_vwep;
    }
    return "w_machinegun.md2";
}

/*
==============================================================================

TAGS

==============================================================================
*/

static void q3w_axis_mul(const vec3_t a[3], const vec3_t b[3], vec3_t out[3])
{
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

// back to angles for entity_t, which the renderers turn into this axis
// again (AnglesToAxis: forward, left, up) - q3player.c's axis_to_angles
static void q3w_axis_to_angles(const vec3_t axis[3], vec3_t angles)
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

static void q3w_lerp_tag(const q3wtag_t *tags, int frames, int oldframe, int frame,
                         float backlerp, q3wtag_t *out)
{
    const q3wtag_t  *a, *b;
    float           front = 1.0f - backlerp;
    int             i;

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

// a child on its parent's tag, turned by `local` (cg_ents.c
// CG_PositionRotatedEntityOnTag); `scale` is the parent's
static void q3w_on_tag(const vec3_t parent_origin, const vec3_t parent_axis[3], float scale,
                       const q3wtag_t *tag, const vec3_t local[3], vec3_t origin, vec3_t axis[3])
{
    vec3_t  temp[3];
    int     i;

    VectorCopy(parent_origin, origin);
    for (i = 0; i < 3; i++)
        VectorMA(origin, tag->origin[i] * scale, parent_axis[i], origin);

    q3w_axis_mul(local, tag->axis, temp);
    q3w_axis_mul(temp, parent_axis, axis);
}

static void q3w_roll_axis(float roll, vec3_t axis[3])
{
    vec3_t angles = { 0, 0, roll };

    AnglesToAxis(angles, axis);
}

static void q3w_part(entity_t *e, const entity_t *base, qhandle_t model, int id,
                     const vec3_t origin, const vec3_t axis[3])
{
    *e = *base;
    e->model = model;
    e->id = id;
    e->skin = 0;
    e->skinnum = 0;
    e->frame = e->oldframe = 0;
    e->backlerp = 0;
    e->blood_key = 0;
    VectorCopy(origin, e->origin);
    VectorCopy(origin, e->oldorigin);
    q3w_axis_to_angles(axis, e->angles);
}

/*
==============================================================================

THE GUN BEING FIRED

==============================================================================
*/

// the barrel's spin (cg_weapons.c CG_MachinegunSpinAngle)
typedef struct {
    float   angle;
    int     time;
    bool    spinning;
} q3wspin_t;

typedef struct {
    int         fire_time;      // its last muzzle flash
    int         weapon;         // Q3W_*, of that flash
    q3wspin_t   spin;
    vec3_t      muzzle;         // its gun's tag_flash in the world...
    int         muzzle_frame;   // ...as drawn on this cls.framecount
} q3wshooter_t;

static q3wshooter_t q3w_shooters[MAX_CLIENTS + 1];

#define Q3W_SPIN_SPEED  0.9f
#define Q3W_COAST_TIME  1000

static float q3w_spin_angle(q3wspin_t *s, bool firing)
{
    int     delta = cl.time - s->time;
    float   angle, speed;

    if (delta < 0) {
        s->time = cl.time;
        delta = 0;
    }
    if (s->spinning) {
        angle = s->angle + delta * Q3W_SPIN_SPEED;
    } else {
        if (delta > Q3W_COAST_TIME)
            delta = Q3W_COAST_TIME;
        speed = 0.5f * (Q3W_SPIN_SPEED + (float)(Q3W_COAST_TIME - delta) / Q3W_COAST_TIME);
        angle = s->angle + delta * speed;
    }

    if (s->spinning != firing) {
        s->time = cl.time;
        s->angle = anglemod(angle);
        s->spinning = firing;
    }
    return angle;
}

static q3wshooter_t *q3w_shooter(int entnum)
{
    if (entnum < 1 || entnum > MAX_CLIENTS)
        return NULL;
    return &q3w_shooters[entnum];
}

static bool q3w_firing(const q3wshooter_t *sh, int window)
{
    return sh && sh->fire_time && cl.time >= sh->fire_time && cl.time - sh->fire_time < window;
}

/*
==============================================================================

THE VIEW WEAPON

==============================================================================
*/

static struct {
    qhandle_t   model;
    int         state, state_time;
    vec3_t      muzzle;         // tag_flash in the world, as last drawn
    int         muzzle_time;    // cl.time it was
} q3v;

static int q3p_view_state(int state);   // PREDICTED SHOTS

// the hand's frame (cg_weapons.c CG_MapTorsoToWeaponFrame): 0 still, 1-6 the
// torso's attack, 6-10 its drop and 11-14 its raise, which follows the drop
static float q3w_hand_frame(const q3wgun_t *g, int state, const q3wshooter_t *sh)
{
    int     t = cl.time - q3v.state_time;
    float   pos = 0;

    if (state == Q3_GUNFRAME_RAISING) {
        pos = 11 + 3 * min(t, 250) / 250.0f;
    } else if (state == Q3_GUNFRAME_DROPPING) {
        pos = 6 + 4 * min(t, 200) / 200.0f;
    } else if (sh && sh->fire_time && cl.time >= sh->fire_time) {
        // the attack runs six frames at 15 a second
        float a = 1 + (cl.time - sh->fire_time) * 0.015f;

        if (a < 6)
            pos = a;
    }
    if (pos > g->hand_frames - 1)
        pos = max(0, g->hand_frames - 1);
    return pos;
}

/*
=================
q3w_add_rtx_rail

The view weapon when it is Quake III's railgun and Q2RTX's redesign stands in
for it (q3w_want_rtx_rail). Quake II's view model goes where Quake II draws
one - `gun` as placed, no hand tag - and plays Weapon_Railgun's frames
(p_weapon.c: 0-3 raise, 4-18 the shot, 19-56 idle, 57-61 drop) from Quake
III's state, on Quake III's clock: a 250 ms raise, a 200 ms drop, and 1500
ms a shot, which is what Quake II's fifteen fire frames take anyway. The
model has no flash of its own; the shot's light is CL_Q3W_MuzzleFlash's.
=================
*/
#define RTX_RAIL_RAISE_LAST 3
#define RTX_RAIL_FIRE       4
#define RTX_RAIL_IDLE       19
#define RTX_RAIL_IDLE_LAST  56
#define RTX_RAIL_DROP       57
#define RTX_RAIL_LAST       61
#define RTX_RAIL_SHOT_MS    1500    // frames 4 to 19 at Quake II's 10 a second

static void q3w_add_rtx_rail(const entity_t *gun, int state, const q3wshooter_t *sh)
{
    entity_t    e;
    vec3_t      forward, right, up;
    int         t = cl.time - q3v.state_time, idle_start, a, b;
    bool        railed = sh && sh->weapon == Q3W_RAILGUN && sh->fire_time && cl.time >= sh->fire_time;
    float       pos, frac;

    if (state == Q3_GUNFRAME_RAISING) {
        // 0 to 3, then on into the idle's first frame as Quake II does
        pos = (RTX_RAIL_RAISE_LAST + 1) * min(t, 250) / 250.0f;
    } else if (state == Q3_GUNFRAME_DROPPING) {
        pos = RTX_RAIL_DROP + (RTX_RAIL_LAST - RTX_RAIL_DROP) * min(t, 200) / 200.0f;
    } else if (railed && cl.time - sh->fire_time < RTX_RAIL_SHOT_MS) {
        pos = RTX_RAIL_FIRE + (cl.time - sh->fire_time) * 0.01f;
    } else {
        // the idle loop, 19 to 56 and round, from when the gun came up or the
        // last shot's frames ran out, whichever was later
        idle_start = q3v.state_time;
        if (railed && sh->fire_time + RTX_RAIL_SHOT_MS > idle_start)
            idle_start = sh->fire_time + RTX_RAIL_SHOT_MS;
        pos = RTX_RAIL_IDLE + fmodf(max(cl.time - idle_start, 0) * 0.01f,
                                    RTX_RAIL_IDLE_LAST + 1 - RTX_RAIL_IDLE);
    }

    a = (int)pos;
    frac = pos - a;
    b = a + 1;
    if (state == Q3_GUNFRAME_RAISING && a >= RTX_RAIL_RAISE_LAST) {
        a = RTX_RAIL_RAISE_LAST;
        b = RTX_RAIL_IDLE;
        frac = min(pos - RTX_RAIL_RAISE_LAST, 1.0f);
    } else if (a == RTX_RAIL_IDLE_LAST) {
        b = RTX_RAIL_IDLE;      // the idle loop closes
    } else if (a >= RTX_RAIL_LAST) {
        a = b = RTX_RAIL_LAST;
        frac = 0;
    }

    e = *gun;
    e.model = q3w.rtx_rail_view;
    e.oldframe = a;
    e.frame = b;
    e.backlerp = 1.0f - frac;
    V_AddEntity(&e);

    // where the rail starts (CL_Q3W_RailStart): Quake II's own for this gun,
    // weapon_railgun_fire's 7 right and 8 down from the eye, so the beam
    // leaves the redesign as it does in Quake II - on the gun's axes, so it
    // rides the bob, kick and pullback the model does
    AngleVectors(gun->angles, forward, right, up);
    VectorMA(gun->origin, (gun->flags & RF_LEFTHAND) ? -7 : 7, right, q3v.muzzle);
    VectorMA(q3v.muzzle, -8, up, q3v.muzzle);
    q3v.muzzle_time = cl.time;
}

/*
=================
CL_Q3W_AddViewWeapon

CL_AddViewWeapon, with `gun` placed (bob, kick, pullback and all) and its
model a Quake III gun's: draws it, false if it is not one. ps.gunframe is the
gun's state and shots (Q3_GUNFRAME_*, from g_q3weap.c).
=================
*/
bool CL_Q3W_AddViewWeapon(const entity_t *gun, int gunframe)
{
    q3wgun_t        *g = q3w_gun_for_model(gun->model);
    q3wshooter_t    *sh = q3w_shooter(cl.frame.clientNum + 1);
    vec3_t          hand_axis[3], gun_origin, gun_axis[3], part_origin, part_axis[3], local[3];
    q3wtag_t        tag;
    entity_t        e;
    int             state = q3p_view_state(gunframe & Q3_GUNFRAME_STATE);
    bool            firing;

    if (!g)
        return false;

    if (state != q3v.state || gun->model != q3v.model) {
        q3v.state = state;
        q3v.model = gun->model;
        q3v.state_time = cl.time;
    }
    firing = state == Q3_GUNFRAME_FIRING;

    // Q2RTX's railgun in its place (cl_classic_railgun 0)
    if (g == &q3w_guns[Q3W_RAILGUN] && q3w.rtx_rail_view) {
        q3w_add_rtx_rail(gun, state, sh);
        return true;
    }

    // the gun on the hand's tag_weapon (the hand itself draws nothing)
    AnglesToAxis(gun->angles, hand_axis);
    if (g->hand_tags) {
        float   pos = q3w_hand_frame(g, state, sh);
        int     old = (int)pos;

        q3w_lerp_tag(g->hand_tags, g->hand_frames, old, old + 1, 1.0f - (pos - old), &tag);
        q3w_roll_axis(0, local);
        q3w_on_tag(gun->origin, hand_axis, 1, &tag, local, gun_origin, gun_axis);
    } else {
        VectorCopy(gun->origin, gun_origin);
        memcpy(gun_axis, hand_axis, sizeof(gun_axis));
    }
    q3w_part(&e, gun, g->world, gun->id, gun_origin, gun_axis);
    V_AddEntity(&e);

    // the barrel spins while the trigger is down
    if (g->barrel && g->have_barrel_tag) {
        q3w_roll_axis(sh ? q3w_spin_angle(&sh->spin, firing) : 0, local);
        q3w_on_tag(gun_origin, gun_axis, 1, &g->barrel_tag, local, part_origin, part_axis);
        q3w_part(&e, gun, g->barrel, gun->id, part_origin, part_axis);
        V_AddEntity(&e);
    }

    // the flash: for each shot, or all the while for the LG and gauntlet
    if (g->have_flash_tag) {
        q3w_roll_axis(crand() * 10, local);
        q3w_on_tag(gun_origin, gun_axis, 1, &g->flash_tag, local, part_origin, part_axis);
        VectorCopy(part_origin, q3v.muzzle);
        q3v.muzzle_time = cl.time;

        // (Quake III's EF_FIRING: the gauntlet flashes whether it hits or not)
        if (g->flash && (g->continuous ? firing : q3w_firing(sh, 40))) {
            q3w_part(&e, gun, g->flash, gun->id, part_origin, part_axis);
            e.scale = q3w_flash_scale(g, gun->scale);
            e.flags |= RF_FULLBRIGHT;
            V_AddEntity(&e);
        }
    }
    return true;
}

// the view gun's muzzle, as drawn last frame
bool CL_Q3W_ViewMuzzle(vec3_t out)
{
    if (!q3v.muzzle_time || cl.time - q3v.muzzle_time > 200 || cl.thirdPersonView)
        return false;
    VectorCopy(q3v.muzzle, out);
    return true;
}

/*
=================
CL_Q3W_AddGunParts

After a Quake III gun is drawn anywhere but the view: in a Quake III
player's hand (`entnum` the player) or on the ground (0). Its barrel, and on
a player its flash, and where its muzzle is for the Lightning Gun's bolt.
=================
*/
void CL_Q3W_AddGunParts(const entity_t *gun, int entnum)
{
    q3wgun_t        *g = q3w_gun_for_model(gun->model);
    q3wshooter_t    *sh = q3w_shooter(entnum);
    vec3_t          gun_axis[3], local[3], origin, axis[3];
    entity_t        e;
    float           scale;
    bool            firing;

    if (!g)
        return;

    scale = gun->scale > 0 ? gun->scale : 1;
    AnglesToAxis(gun->angles, gun_axis);
    firing = sh && sh->weapon == g - q3w_guns && q3w_firing(sh, 150);

    if (g->barrel && g->have_barrel_tag) {
        q3w_roll_axis(sh ? q3w_spin_angle(&sh->spin, firing) : 0, local);
        q3w_on_tag(gun->origin, gun_axis, scale, &g->barrel_tag, local, origin, axis);
        q3w_part(&e, gun, g->barrel, gun->id, origin, axis);
        e.scale = gun->scale;
        V_AddEntity(&e);
    }

    if (sh && g->have_flash_tag) {
        q3w_roll_axis(crand() * 10, local);
        q3w_on_tag(gun->origin, gun_axis, scale, &g->flash_tag, local, origin, axis);
        VectorCopy(origin, sh->muzzle);
        sh->muzzle_frame = cls.framecount;

        // Not on your own body in first person (RF_VIEWERMODEL: there only
        // for mirrors). The renderer puts all of that body's meshes in the
        // viewer-model BLAS as masked geometry, and the flash, an additive
        // effect with no mask, is a solid ball there: the mirror took its
        // depth and motion at the end of the gun, and the lightning gun's
        // stays up the whole time it fires (Matt, 11-19: "it looks like a
        // ball at the end of the weapon and that's when I am seeing the
        // glitching"). Bots' flashes take the transparent pass and are fine.
        if (g->flash && !(gun->flags & RF_VIEWERMODEL) &&
            (g->continuous ? firing : q3w_firing(sh, 40))) {
            q3w_part(&e, gun, g->flash, gun->id, origin, axis);
            e.scale = q3w_flash_scale(g, gun->scale);
            e.flags |= RF_FULLBRIGHT;
            V_AddEntity(&e);
        }
    }
}

/*
==============================================================================

SPENT BRASS (cg_weapons.c CG_MachineGunEjectBrass, CG_ShotgunEjectBrass;
cg_localents.c CG_AddFragment)

==============================================================================
*/

#define Q3W_MAX_BRASS   8192

/* HOW A SHELL FALLS. Quake III's brass barely turns in the air (1 degree a
   second) and stops where the point in its middle meets the floor: a shotgun
   shell, 5.6 units long and modelled standing up, landed upright with half of
   it under the floor (Matt, 2026-11-18: "they land upward and sink into the
   ground ... they should fall over or react more physically correct").

   Now: it tumbles in the air, and it collides as the box round the cylinder it
   is, turned as it is - so it bounces off the floor with its lowest point, not
   its middle. Once it has stopped bouncing it tips over about the floor until
   its length lies along the surface (on a slope too), touching it all the way
   down, then rolls across its length with what speed it had left, slowing,
   stopped by walls, falling again off an edge. A shell is a cylinder along the
   model's Z axis (q3wshell_t, from the .md3's bounds). */
enum { Q3WB_FLY, Q3WB_TIP, Q3WB_ROLL, Q3WB_REST };

typedef struct {
    int         kind;           // 0 machinegun, 1 shotgun; -1 free
    int         state;          // Q3WB_*
    bool        on_entity;      // came to rest on a door, a lift...
    vec3_t      origin, velocity;
    vec3_t      axis[3];        // its turn: axis[2] is its length
    vec3_t      omega;          // spin in the air, radians a second (world)
    float       bounce;
    int         start, end;     // end 0: for ever
    int         next_check;

    // falling over and rolling
    vec3_t      normal;         // what it lies on
    vec3_t      foot;           // the point of that under its middle
    vec3_t      tip_from[3];    // its turn when it touched down
    vec3_t      tip_about;      // the line it tips over about
    float       tip_angle, tip_time, tip_t;
    vec3_t      roll_dir;
    float       roll_speed;
} q3wbrass_t;

static q3wbrass_t   q3w_brass[Q3W_MAX_BRASS];
static int          q3w_brass_next;
static int          q3w_brass_time;     // cl.time of the last physics step

static int q3w_brass_cap(void)
{
    return Cvar_ClampInteger(cl_brass_max, 16, Q3W_MAX_BRASS);
}

// (v) turned about the unit vector (k) by (angle) radians (Rodrigues)
static void q3w_rotate(vec3_t v, const vec3_t k, float angle)
{
    vec3_t  kxv, out;
    float   c = cosf(angle), s = sinf(angle), kv = DotProduct(k, v);

    CrossProduct(k, v, kxv);
    for (int i = 0; i < 3; i++)
        out[i] = v[i] * c + kxv[i] * s + k[i] * kv * (1 - c);
    VectorCopy(out, v);
}

static void q3w_rotate_axis(vec3_t axis[3], const vec3_t k, float angle)
{
    for (int i = 0; i < 3; i++)
        q3w_rotate(axis[i], k, angle);
    // keep it a rotation
    VectorNormalize(axis[2]);
    CrossProduct(axis[2], axis[0], axis[1]);
    VectorNormalize(axis[1]);
    CrossProduct(axis[1], axis[2], axis[0]);
}

// the box round the shell turned as (axis) is - a cylinder's, exactly
static void q3w_shell_box(const q3wshell_t *s, const vec3_t axis[3], vec3_t mins, vec3_t maxs)
{
    for (int i = 0; i < 3; i++) {
        float a = axis[2][i];
        float disk = s->radius * sqrtf(max(0.0f, 1 - a * a));
        float lo = min(s->zmin * a, s->zmax * a), hi = max(s->zmin * a, s->zmax * a);

        mins[i] = lo - disk;
        maxs[i] = hi + disk;
    }
}

// how far the shell reaches from its middle against (n): where it touches
static float q3w_shell_depth(const q3wshell_t *s, const vec3_t axis[3], const vec3_t n)
{
    float c = DotProduct(axis[2], n);

    return max(-s->zmin * c, -s->zmax * c) + s->radius * sqrtf(max(0.0f, 1 - c * c));
}

static void q3w_spawn_brass(int kind, const vec3_t origin, const vec3_t axis[3],
                            const vec3_t velocity, float bounce, int life)
{
    q3wbrass_t  *b;
    vec3_t      angles, k;
    float       water = 1.0f;
    int         i;

    b = &q3w_brass[q3w_brass_next];
    q3w_brass_next = (q3w_brass_next + 1) % q3w_brass_cap();

    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->state = Q3WB_FLY;
    VectorCopy(origin, b->origin);
    if (CL_PointContents(origin) & MASK_WATER)
        water = 0.10f;
    for (i = 0; i < 3; i++)
        b->velocity[i] = (velocity[0] * axis[0][i] + velocity[1] * axis[1][i] + velocity[2] * axis[2][i]) * water;
    b->bounce = bounce * water;

    // any way round, turning over 1-2.5 times a second about a line across it
    VectorSet(angles, frand() * 360, frand() * 360, frand() * 360);
    AnglesToAxis(angles, b->axis);
    VectorSet(k, crand(), crand(), crand());
    VectorMA(k, -DotProduct(k, b->axis[2]) * 0.8f, b->axis[2], k);
    if (VectorNormalize(k) < 0.01f)
        VectorCopy(b->axis[0], k);
    VectorScale(k, (6 + 10 * frand()) * water, b->omega);

    b->start = cl.time - (Q_rand() & 15);
    b->end = life ? cl.time + life : 0;
}

/*
=================
CL_Q3W_EjectBrass

A shot from a gun that throws brass, by a player: Quake III's machinegun (a
cartridge) and shotgun (two shells), and Quake II's machinegun and chaingun
(a cartridge a bullet) and shotguns (a shell a barrel).
=================
*/
void CL_Q3W_EjectBrass(int entnum, int mz)
{
    vec3_t      origin, angles, axis[3], offset, velocity;
    float       up = 20;
    int         kind, count, life, i, j;

    switch (mz) {
    case MZ_MACHINEGUN:
    case MZ_CHAINGUN1:
    case MZ_Q3_MACHINEGUN:
        kind = 0; count = 1; break;
    case MZ_CHAINGUN2:
        kind = 0; count = 2; break;
    case MZ_CHAINGUN3:
        kind = 0; count = 3; break;
    case MZ_SHOTGUN:
        kind = 1; count = 1; break;
    case MZ_SSHOTGUN:
    case MZ_Q3_SHOTGUN:
        kind = 1; count = 2; break;
    default:
        return;
    }

    if (!q3w.brass[kind] || cl_brass_time->integer == 0)
        return;
    if (entnum < 1 || entnum > MAX_CLIENTS)
        return;     // players only: monsters' guns are theirs

    // where it comes out: Quake III's 8 forward, 4 right and 24 up from the
    // player's middle, which is 2 under its eye; ours is 22 over the middle
    if (entnum == cl.frame.clientNum + 1) {
        if (!cl.thirdPersonView) {
            VectorCopy(cl.refdef.vieworg, origin);
            VectorCopy(cl.refdef.viewangles, angles);
            up = -2;
        } else {
            VectorCopy(cl.playerEntityOrigin, origin);
            VectorCopy(cl.playerEntityAngles, angles);
        }
    } else {
        const centity_t *cent = &cl_entities[entnum];

        VectorCopy(cent->lerp_origin, origin);
        VectorCopy(cent->current.angles, angles);
        // a player's pitch is sent divided by 3 (p_view.c)
        if (angles[PITCH] > 180)
            angles[PITCH] -= 360;
        angles[PITCH] *= 3;
    }
    AnglesToAxis(angles, axis);

    VectorSet(offset, 8, -4, up);
    for (j = 0; j < 3; j++)
        origin[j] += offset[0] * axis[0][j] + offset[1] * axis[1][j] + offset[2] * axis[2][j];

    for (i = 0; i < count; i++) {
        if (kind == 0) {
            VectorSet(velocity, 0, -50 + 40 * crand(), 100 + 50 * crand());
            life = q3w_life(cl_brass_time, 2500 + (int)(625 * frand()));
            q3w_spawn_brass(0, origin, axis, velocity, 0.4f, life);
        } else {
            VectorSet(velocity, 60 + 60 * crand(), (i & 1 ? -40 : 40) + 10 * crand(), 100 + 50 * crand());
            life = q3w_life(cl_brass_time, 2500);
            if (life && cl_brass_time->integer <= 0)
                life = 7500 + (int)(2500 * frand());    // Quake III's shells last 3-4 times as long
            q3w_spawn_brass(1, origin, axis, velocity, 0.3f, life);
        }
    }
}

// it has stopped bouncing on (n): start falling over (see "HOW A SHELL FALLS")
static void q3w_brass_land(q3wbrass_t *b, const vec3_t n, bool on_entity)
{
    const q3wshell_t *s = &q3w.shell[b->kind];
    vec3_t  down, slide, along;
    trace_t tr;
    float   c;

    VectorCopy(n, b->normal);
    b->on_entity = on_entity;

    // the floor under its middle
    VectorMA(b->origin, -(s->zmax - s->zmin + 8), n, down);
    tr = CL_TracePoint(b->origin, down, MASK_SOLID, false);
    if (tr.fraction < 1 && !tr.startsolid)
        VectorCopy(tr.endpos, b->foot);
    else
        VectorMA(b->origin, -q3w_shell_depth(s, (const vec3_t *)b->axis, n), n, b->foot);

    // lying, its length is its length now laid along the floor
    VectorCopy(b->axis[2], along);
    VectorMA(along, -DotProduct(along, n), n, along);
    if (VectorNormalize(along) < 0.05f) {
        // standing straight up: over whichever way it faces
        VectorCopy(b->axis[0], along);
        VectorMA(along, -DotProduct(along, n), n, along);
        VectorNormalize(along);
    }
    c = DotProduct(b->axis[2], along);
    clamp(c, -1.0f, 1.0f);
    CrossProduct(b->axis[2], along, b->tip_about);
    if (VectorNormalize(b->tip_about) < 0.001f) {
        b->tip_angle = 0;
    } else {
        b->tip_angle = acosf(c);
    }
    memcpy(b->tip_from, b->axis, sizeof(b->tip_from));
    // a quarter turn takes about a fifth of a second, as a thing that size falls
    b->tip_time = 0.05f + 0.15f * b->tip_angle / (float)(M_PI / 2);
    b->tip_t = 0;

    // what it still had along the floor rolls it, across its length
    VectorMA(b->velocity, -DotProduct(b->velocity, n), n, slide);
    VectorMA(slide, -DotProduct(slide, along), along, slide);
    b->roll_speed = VectorNormalize2(slide, b->roll_dir) * 0.6f;

    VectorClear(b->velocity);
    VectorClear(b->omega);
    b->state = Q3WB_TIP;
}

// one step in the air (CG_AddFragment, CG_ReflectVelocity)
static void q3w_brass_fly(q3wbrass_t *b, float dt)
{
    const q3wshell_t *s = &q3w.shell[b->kind];
    vec3_t  next, mins, maxs, k;
    trace_t tr;
    float   dot, spin;

    b->velocity[2] -= 800 * dt;
    VectorMA(b->origin, dt, b->velocity, next);

    // fallen out of the map (CL_SetVoidFloor): gone, as in Quake III's NODROP
    if (CL_BelowVoidFloor(next)) {
        b->kind = -1;
        return;
    }

    q3w_shell_box(s, (const vec3_t *)b->axis, mins, maxs);
    tr = CL_TraceBox(b->origin, mins, maxs, next, MASK_SOLID, false);
    if (tr.startsolid) {
        // turned into the floor it is skipping over: lifted out of it first
        vec3_t  above;
        trace_t out;

        VectorCopy(b->origin, above);
        above[2] += s->zmax - s->zmin;
        out = CL_TraceBox(above, mins, maxs, b->origin, MASK_SOLID, false);
        if (!out.startsolid) {
            VectorCopy(out.endpos, b->origin);
            VectorMA(b->origin, dt, b->velocity, next);
            tr = CL_TraceBox(b->origin, mins, maxs, next, MASK_SOLID, false);
        }
    }
    if (tr.startsolid) {
        // wedged: its middle goes on as a point would
        tr = CL_TracePoint(b->origin, next, MASK_SOLID, false);
        if (tr.allsolid) {
            vec3_t up = { 0, 0, 1 };
            q3w_brass_land(b, up, false);
            return;
        }
    }

    spin = VectorLength(b->omega);
    if (tr.fraction == 1.0f) {
        VectorCopy(next, b->origin);
        if (spin > 0) {
            VectorScale(b->omega, 1 / spin, k);
            q3w_rotate_axis(b->axis, k, spin * dt);
        }
        return;
    }

    // into lava or slime, or down on the void (sky, nodraw - the floor under
    // a floating map): gone (Quake III's CONTENTS_NODROP)
    if ((CL_PointContents(tr.endpos) & (CONTENTS_LAVA | CONTENTS_SLIME)) ||
        (tr.plane.normal[2] > 0.7f && CL_BloodVoidSurface(tr.surface))) {
        b->kind = -1;
        return;
    }

    VectorCopy(tr.endpos, b->origin);
    dot = DotProduct(b->velocity, tr.plane.normal);
    VectorMA(b->velocity, -2 * dot, tr.plane.normal, b->velocity);
    VectorScale(b->velocity, b->bounce, b->velocity);
    VectorMA(b->origin, 0.1f, tr.plane.normal, b->origin);

    // a knock sets it turning some other way
    VectorSet(k, crand(), crand(), crand());
    VectorScale(b->omega, 0.5f, b->omega);
    VectorMA(b->omega, min(fabsf(dot) * 0.04f, 12.0f), k, b->omega);

    if (tr.plane.normal[2] > 0.5f && (DotProduct(b->velocity, tr.plane.normal) < 40 || VectorLength(b->velocity) < 10))
        q3w_brass_land(b, tr.plane.normal, CL_TraceHitEntity(&tr) != NULL);
}

// falling over: turned about the line on the floor until it lies along it,
// its lowest point on the floor all the way
static void q3w_brass_tip(q3wbrass_t *b, float dt)
{
    const q3wshell_t *s = &q3w.shell[b->kind];
    float   f;

    b->tip_t += dt;
    f = b->tip_time > 0 ? min(b->tip_t / b->tip_time, 1.0f) : 1.0f;

    memcpy(b->axis, b->tip_from, sizeof(b->axis));
    if (b->tip_angle > 0)
        q3w_rotate_axis(b->axis, b->tip_about, b->tip_angle * f * f);   // faster as it goes
    VectorMA(b->foot, q3w_shell_depth(s, (const vec3_t *)b->axis, b->normal) + 0.05f, b->normal, b->origin);

    if (f < 1)
        return;
    b->state = b->roll_speed > 6 ? Q3WB_ROLL : Q3WB_REST;
    b->next_check = cl.time + 250 + (Q_rand() & 255);
}

// rolling across its length, slowing; a wall stops it, an edge drops it
static void q3w_brass_roll(q3wbrass_t *b, float dt)
{
    const q3wshell_t *s = &q3w.shell[b->kind];
    vec3_t  next, below, k;
    trace_t tr;
    float   dist = b->roll_speed * dt;

    b->roll_speed -= 90 * dt;
    if (dist <= 0 || b->roll_speed <= 0) {
        b->state = Q3WB_REST;
        return;
    }

    VectorMA(b->origin, dist, b->roll_dir, next);
    tr = CL_TracePoint(b->origin, next, MASK_SOLID, false);
    if (tr.fraction < 1) {
        b->state = Q3WB_REST;
        return;
    }

    // still something under it?
    VectorMA(next, -(s->radius + 4), b->normal, below);
    tr = CL_TracePoint(next, below, MASK_SOLID, false);
    if (tr.fraction == 1.0f || tr.startsolid) {
        // over the edge
        VectorCopy(next, b->origin);
        VectorScale(b->roll_dir, b->roll_speed, b->velocity);
        CrossProduct(b->normal, b->roll_dir, k);
        VectorScale(k, b->roll_speed / max(s->radius, 0.1f), b->omega);
        b->state = Q3WB_FLY;
        return;
    }
    if (tr.plane.normal[2] > 0.5f)
        VectorCopy(tr.plane.normal, b->normal);
    VectorCopy(tr.endpos, b->foot);
    VectorMA(b->foot, s->radius + 0.05f, b->normal, b->origin);

    // turned as far as the ground went by under it
    CrossProduct(b->normal, b->roll_dir, k);
    if (VectorNormalize(k) > 0.001f)
        q3w_rotate_axis(b->axis, k, dist / max(s->radius, 0.1f));
}

// a piece at rest on something that moves: still on it?
static void q3w_brass_support(q3wbrass_t *b)
{
    const q3wshell_t *s = &q3w.shell[b->kind];
    vec3_t  start, end;
    trace_t tr;
    float   depth = q3w_shell_depth(s, (const vec3_t *)b->axis, b->normal);

    b->next_check = cl.time + 250 + (Q_rand() & 127);

    VectorCopy(b->origin, start);
    VectorCopy(b->origin, end);
    end[2] -= depth + 3;
    tr = CL_TracePoint(start, end, MASK_SOLID, false);
    if (tr.startsolid) {
        // it came up through the piece: on top of it
        start[2] = b->origin[2] + 48;
        tr = CL_TracePoint(start, b->origin, MASK_SOLID, false);
        if (!tr.startsolid) {
            VectorCopy(tr.endpos, b->foot);
            VectorCopy(tr.endpos, b->origin);
            b->origin[2] += depth;
        }
        return;
    }
    if (tr.fraction == 1.0f) {
        b->state = Q3WB_FLY;    // it went away underneath
        b->on_entity = false;
    } else if (b->origin[2] - tr.endpos[2] > depth + 0.5f) {
        // it went down a little (a lift): go with it
        VectorCopy(tr.endpos, b->foot);
        VectorCopy(tr.endpos, b->origin);
        b->origin[2] += depth;
    }
}

static void q3w_add_brass(void)
{
    q3wbrass_t  *b;
    entity_t    e;
    float       dt, dist2;
    int         i, cap = q3w_brass_cap();

    dt = (cl.time - q3w_brass_time) * 0.001f;
    q3w_brass_time = cl.time;
    if (dt < 0 || dt > 0.1f)
        dt = dt < 0 ? 0 : 0.1f;

    memset(&e, 0, sizeof(e));
    for (i = 0, b = q3w_brass; i < cap; i++, b++) {
        if (b->kind < 0 || !q3w.brass[b->kind])
            continue;
        if (b->end && cl.time >= b->end) {
            b->kind = -1;
            continue;
        }

        if (dt > 0) {
            switch (b->state) {
            case Q3WB_FLY:
                q3w_brass_fly(b, dt);
                break;
            case Q3WB_TIP:
                q3w_brass_tip(b, dt);
                break;
            case Q3WB_ROLL:
                q3w_brass_roll(b, dt);
                break;
            default:
                if (b->on_entity && cl.time >= b->next_check)
                    q3w_brass_support(b);
                break;
            }
            if (b->kind < 0)
                continue;
        }

        dist2 = DistanceSquared(b->origin, cl.refdef.vieworg);
        if (dist2 > 2048 * 2048)
            continue;

        e.model = q3w.brass[b->kind];
        e.id = Q3W_BRASS_ID + i;
        VectorCopy(b->origin, e.origin);
        // sink into the ground over its last second (SINK_TIME)
        if (b->end && b->state == Q3WB_REST && b->end - cl.time < 1000)
            e.origin[2] -= 16 * (1.0f - (b->end - cl.time) / 1000.0f);
        VectorCopy(e.origin, e.oldorigin);
        q3w_axis_to_angles((const vec3_t *)b->axis, e.angles);
        V_AddEntity(&e);
    }
}

/*
==============================================================================

MARKS (cg_marks.c): bullet holes, scorches and plasma burns, flat on the wall

==============================================================================
*/

#define Q3W_MAX_MARKS   4096

typedef struct {
    qhandle_t   image;
    vec3_t      origin;
    vec3_t      angles;         // forward the surface normal, roll its turn
    float       radius;
    int         mode;           // DECAL_*
    bool        energy;         // fades from glowing to burnt (CG_AddMarks)
    vec3_t      color;
    int         start, end;
} q3wmark_t;

static q3wmark_t    q3w_marks[Q3W_MAX_MARKS];
static int          q3w_mark_next;

static int q3w_mark_cap(void)
{
    return Cvar_ClampInteger(cl_q3_marks_max, 16, Q3W_MAX_MARKS);
}

// where a mark at `origin` on the world goes; false for none (not the world,
// the sky, or a brush entity that may move)
static bool q3w_mark_surface(const vec3_t origin, const vec3_t dir, vec3_t surface, vec3_t normal)
{
    vec3_t  start, end;
    trace_t tr;

    if (!cl.bsp || !cl.bsp->nodes)
        return false;
    VectorMA(origin, 4, dir, start);
    VectorMA(origin, -8, dir, end);
    CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);
    if (tr.startsolid || tr.fraction >= 1.0f)
        return false;
    if (tr.surface && (tr.surface->flags & SURF_SKY))
        return false;
    VectorCopy(tr.endpos, surface);
    VectorCopy(tr.plane.normal, normal);
    return true;
}

// whether every corner of the mark lies on the surface - Quake III clips its
// marks to the wall; a quad that would hang off an edge is made smaller
static bool q3w_mark_fits(const vec3_t origin, const vec3_t normal, const vec3_t right,
                          const vec3_t up, float radius)
{
    static const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    vec3_t  p, start, end;
    trace_t tr;
    int     i;

    for (i = 0; i < 4; i++) {
        VectorMA(origin, corners[i][0] * radius, right, p);
        VectorMA(p, corners[i][1] * radius, up, p);
        VectorMA(p, 2, normal, start);
        VectorMA(p, -4, normal, end);
        CM_BoxTrace(&tr, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);
        if (tr.startsolid || tr.fraction >= 1.0f || DotProduct(tr.plane.normal, normal) < 0.9f)
            return false;
        if (fabsf(tr.fraction * 6 - 2) > 1.5f)
            return false;
    }
    return true;
}

static void q3w_mark(qhandle_t image, const vec3_t origin, const vec3_t dir, float radius,
                     int mode, bool energy, const vec3_t color)
{
    q3wmark_t   *m;
    vec3_t      surface, normal, angles, forward, right, up;
    int         tries;

    if (!image || !CL_PathTraced() || cl_q3_marks_time->integer == 0) {
        q3w_dbg.refused[!image ? Q3W_REFUSED_NO_IMAGE : !CL_PathTraced() ? Q3W_REFUSED_RENDERER : Q3W_REFUSED_OFF]++;
        return;
    }
    if (!q3w_mark_surface(origin, dir, surface, normal)) {
        q3w_dbg.refused[Q3W_REFUSED_NO_SURFACE]++;
        if (cl_q3_debug->integer > 1)
            Com_Printf("q3 mark: no surface under %.1f %.1f %.1f along %.2f %.2f %.2f\n",
                       origin[0], origin[1], origin[2], dir[0], dir[1], dir[2]);
        return;
    }

    vectoangles2(normal, angles);
    angles[ROLL] = frand() * 360;
    AngleVectors(angles, forward, right, up);

    for (tries = 0; tries < 3; tries++, radius *= 0.5f)
        if (q3w_mark_fits(surface, normal, right, up, radius))
            break;
    if (tries == 3) {
        q3w_dbg.refused[Q3W_REFUSED_NO_FIT]++;
        return;
    }
    q3w_dbg.marks++;

    m = &q3w_marks[q3w_mark_next];
    q3w_mark_next = (q3w_mark_next + 1) % q3w_mark_cap();

    m->image = image;
    VectorMA(surface, 0.5f, normal, m->origin);
    VectorCopy(angles, m->angles);
    m->radius = radius;
    m->mode = mode;
    m->energy = energy;
    VectorCopy(color, m->color);
    m->start = cl.time;
    m->end = q3w_life(cl_q3_marks_time, 10000);
    if (m->end)
        m->end += cl.time;
}

static uint32_t q3w_pack_rgb(const vec3_t c)
{
    int r = (int)(q3w_clip01(c[0]) * 255);
    int g = (int)(q3w_clip01(c[1]) * 255);
    int b = (int)(q3w_clip01(c[2]) * 255);

    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xffu;
}

static void q3w_add_marks(void)
{
    q3wmark_t   *m;
    entity_t    e;
    vec3_t      color;
    int         i, cap = q3w_mark_cap();

    if (!CL_PathTraced())
        return;

    memset(&e, 0, sizeof(e));
    for (i = 0, m = q3w_marks; i < cap; i++, m++) {
        float alpha = 1.0f;

        if (!m->image)
            continue;
        if (m->end && cl.time >= m->end) {
            m->image = 0;
            continue;
        }
        if (cl.time < m->start) {
            m->image = 0;       // a stale one from before the clock restarted
            continue;
        }
        if (DistanceSquared(m->origin, cl.refdef.vieworg) > 3072 * 3072)
            continue;

        VectorCopy(m->color, color);
        // the plasma's burn glows, then goes dark over 3 seconds
        if (m->energy) {
            float fade = 450 - 450 * ((cl.time - m->start) / 3000.0f);

            fade = q3w_clip01(fade / 255.0f);
            VectorScale(color, fade, color);
        }
        // the last second of one that does not stay
        if (m->end && m->end - cl.time < 1000)
            alpha = (m->end - cl.time) / 1000.0f;

        e.flags = RF_FLARE | RF_DECAL;
        e.frame = m->image;
        e.skinnum = m->mode;
        e.scale = m->radius;
        e.alpha = alpha;
        e.rgba.u32 = q3w_pack_rgb(color);
        VectorCopy(m->origin, e.origin);
        VectorCopy(m->origin, e.oldorigin);
        VectorCopy(m->angles, e.angles);
        V_AddEntity(&e);
        q3w_dbg.marks_drawn++;
    }
}

/*
==============================================================================

FLASHES OF IMPACT: short-lived sprites

==============================================================================
*/

// (the gibs' blood trails are most of them: a puff every 150 ms from each of
// eleven pieces, lasting 2 s)
#define Q3W_MAX_FX  1024

typedef struct {
    qhandle_t   image[3];
    int         images;
    vec3_t      origin, angles;
    bool        flat;           // on the surface (angles), or facing the camera
    float       size0, size1;
    vec3_t      color;
    int         start, life;
    // Quake III's blood (GIBS): blended over what is behind rather than added
    // to it, facing the camera, turned by roll degrees; sinking drop units
    // over its life; hold: no fade (LE_EXPLOSION); near: gone when the view
    // is inside it (CG_AddFallScaleFade)
    bool        blend, hold, near;
    float       roll, drop;
} q3wfx_t;

static q3wfx_t  q3w_fx[Q3W_MAX_FX];
static int      q3w_fx_next;

static q3wfx_t *q3w_alloc_fx(void)
{
    q3wfx_t *fx = &q3w_fx[q3w_fx_next];

    q3w_fx_next = (q3w_fx_next + 1) % Q3W_MAX_FX;
    memset(fx, 0, sizeof(*fx));
    fx->start = cl.time;
    VectorSet(fx->color, 1, 1, 1);
    return fx;
}

static void q3w_add_fx(void)
{
    q3wfx_t     *fx;
    entity_t    e;
    int         i;

    if (!CL_PathTraced())
        return;

    memset(&e, 0, sizeof(e));
    for (i = 0, fx = q3w_fx; i < Q3W_MAX_FX; i++, fx++) {
        float frac;

        if (!fx->life)
            continue;
        if (cl.time < fx->start || cl.time >= fx->start + fx->life) {
            fx->life = 0;
            continue;
        }
        frac = (cl.time - fx->start) / (float)fx->life;

        e.frame = fx->image[min((int)(frac * fx->images), fx->images - 1)];
        if (!e.frame)
            continue;
        e.alpha = fx->hold ? 1.0f : 1.0f - frac;
        e.rgba.u32 = q3w_pack_rgb(fx->color);
        VectorCopy(fx->origin, e.origin);
        e.origin[2] -= fx->drop * frac;
        VectorCopy(e.origin, e.oldorigin);
        if (fx->blend) {
            // a sprite Quake III blends (blendFunc blend): a decal turned to
            // face the camera, as the renderer's camera-facing sprites only add
            vec3_t  to_view;
            float   size = fx->size0 + (fx->size1 - fx->size0) * frac;

            VectorSubtract(cl.refdef.vieworg, e.origin, to_view);
            if (fx->near && VectorLength(to_view) < size)
                continue;
            if (VectorNormalize(to_view) < 0.001f)
                continue;
            e.flags = RF_FLARE | RF_DECAL;
            e.skinnum = DECAL_BLEND;
            e.scale = size;
            vectoangles2(to_view, e.angles);
            e.angles[ROLL] = fx->roll;
        } else if (fx->flat) {
            e.flags = RF_FLARE | RF_DECAL;
            e.skinnum = DECAL_ADD;
            e.scale = fx->size0 + (fx->size1 - fx->size0) * frac;
            VectorCopy(fx->angles, e.angles);
        } else {
            e.flags = RF_FLARE;
            e.skinnum = 0;
            e.scale = (fx->size0 + (fx->size1 - fx->size0) * frac) / FLARE_BASE_SIZE;
            VectorClear(e.angles);
        }
        V_AddEntity(&e);
        q3w_dbg.fx_drawn++;
    }
}

/*
==============================================================================

IMPACTS (cg_weapons.c CG_MissileHitWall)

==============================================================================
*/

static const vec3_t q3w_white = { 1, 1, 1 };

static void q3w_sound_at(const vec3_t origin, qhandle_t sfx)
{
    if (sfx)
        S_StartSound(origin, 0, 0, sfx, 1, ATTN_NORM, 0);
}

/*
=================
CL_Q3W_Impact

tent.c, for TE_Q3_BULLET, TE_Q3_SHOTGUN and TE_Q3_PLASMA_EXPLOSION.
=================
*/
void CL_Q3W_Impact(int type, const vec3_t origin, const vec3_t dir)
{
    q3wfx_t     *fx;
    cdlight_t   *dl;

    switch (type) {
    case TE_Q3_BULLET:
    case TE_Q3_SHOTGUN:
        // the flash on the wall (bulletExplosion: three frames of
        // models/weaphits/bullet*), Quake II's puff of dust with it
        fx = q3w_alloc_fx();
        fx->images = 3;
        memcpy(fx->image, q3w.bullet_flash, sizeof(fx->image));
        VectorMA(origin, 0.75f, dir, fx->origin);
        vectoangles2(dir, fx->angles);
        fx->angles[ROLL] = frand() * 360;
        fx->flat = true;
        fx->size0 = fx->size1 = type == TE_Q3_BULLET ? 6 : 4;
        fx->life = 200;
        CL_ParticleEffect(origin, dir, 0, type == TE_Q3_BULLET ? 8 : 4);
        if (!CL_PathTraced())
            CL_ImpactSmokeAndFlash(origin, dir);

        q3w_mark(q3w.mark_bullet, origin, dir, type == TE_Q3_BULLET ? 8 : 4, DECAL_DARKEN, false, q3w_white);
        if (type == TE_Q3_BULLET) {
            // CG_MissileHitWall: ric1 half the time, ric2 and ric3 a quarter
            int r = Q_rand() & 3;

            q3w_sound_at(origin, q3w.sfx_ric[r < 2 ? 0 : r - 1]);
        }
        break;

    case TE_Q3_PLASMA_EXPLOSION:
        // plasmaExplosion: a ring that swells and fades
        fx = q3w_alloc_fx();
        fx->images = 1;
        fx->image[0] = q3w.plasma_boom;
        VectorMA(origin, 2, dir, fx->origin);
        fx->size0 = 8;
        fx->size1 = 22;
        fx->life = 400;
        VectorSet(fx->color, 0.7f, 0.7f, 1.0f);

        dl = CL_AllocDlight(0);
        VectorMA(origin, 4, dir, dl->origin);
        VectorSet(dl->color, 0.6f, 0.6f, 1.0f);
        dl->radius = 150;
        dl->die = cl.time + 150;

        q3w_mark(q3w.mark_energy, origin, dir, 16, DECAL_BLEND, true, q3w_white);
        q3w_sound_at(origin, q3w.sfx_plasmaexp);
        break;
    }
}

/*
=================
CL_Q3W_Explosion

tent.c, after the Quake II and Flow fire of a Quake III rocket, grenade or
BFG ball: its sound and its scorch.
=================
*/
void CL_Q3W_Explosion(int type, const vec3_t origin)
{
    static const vec3_t dirs[6] = {
        { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }
    };
    trace_t best;
    int     i;

    q3w_sound_at(origin, q3w.sfx_rockexp);

    // the message carries no surface: the nearest one round it, as the blast
    // is drawn a little back from where it hit (g_q3weap.c: 1/50 of a
    // second of flight, 18 units for a rocket)
    if (!cl.bsp || !cl.bsp->nodes)
        return;
    memset(&best, 0, sizeof(best));
    best.fraction = 1.0f;
    for (i = 0; i < 6; i++) {
        vec3_t  end;
        trace_t tr;

        VectorMA(origin, -32, dirs[i], end);
        CM_BoxTrace(&tr, origin, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);
        if (!tr.startsolid && tr.fraction < best.fraction)
            best = tr;
    }
    if (best.fraction >= 1.0f)
        return;     // in the air: no scorch
    if (best.surface && (best.surface->flags & SURF_SKY))
        return;

    q3w_mark(q3w.mark_burn, best.endpos, best.plane.normal, type == TE_Q3_BFG_EXPLOSION ? 32 : 64,
             DECAL_DARKEN, false, q3w_white);
}

/*
=================
CL_Q3W_RailImpact

The rail's end on a wall: plasmx1a, and a burn in its colour.
=================
*/
void CL_Q3W_RailImpact(const vec3_t start, const vec3_t end, const vec3_t color)
{
    vec3_t  dir, before, past;
    trace_t tr;

    if (!cl.bsp || !cl.bsp->nodes)
        return;
    VectorSubtract(end, start, dir);
    if (VectorNormalize(dir) < 1)
        return;
    VectorMA(end, -4, dir, before);
    VectorMA(end, 4, dir, past);
    CM_BoxTrace(&tr, before, past, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);
    if (tr.startsolid || tr.fraction >= 1.0f || (tr.surface && (tr.surface->flags & SURF_SKY)))
        return;     // it ended in someone, or the sky

    q3w_sound_at(tr.endpos, q3w.sfx_plasmaexp);
    q3w_mark(q3w.mark_energy, tr.endpos, tr.plane.normal, 24, DECAL_BLEND, true, color);
}

// the server's start of a rail fired in first person: from the view gun's
// muzzle instead, as Quake III draws it
void CL_Q3W_RailStart(vec3_t start)
{
    vec3_t muzzle, eye;

    if (!CL_Q3W_ViewMuzzle(muzzle))
        return;
    VectorMA(cl.refdef.vieworg, 14, cl.v_forward, eye);
    if (Distance(eye, start) > 24)
        return;     // not ours
    VectorCopy(muzzle, start);
}

/*
==============================================================================

THE LIGHTNING GUN'S BOLT (cg_weapons.c CG_LightningBolt)

==============================================================================
*/

typedef struct {
    int     endtime;
    vec3_t  start, end;
    bool    hit;
    vec3_t  last_mark;      // where its last hole went
} q3wbolt_t;

static q3wbolt_t    q3w_bolts[MAX_CLIENTS + 1];

/*
=================
CL_Q3W_Lightning

TE_Q3_LIGHTNING, once a server frame while the gun fires: keep the bolt up,
and its hit's sound and mark.
=================
*/
void CL_Q3W_Lightning(int shooter, bool hit, const vec3_t start, const vec3_t end)
{
    q3wbolt_t   *b;
    vec3_t      dir, normal, surface;

    if (shooter < 1 || shooter > MAX_CLIENTS)
        return;
    b = &q3w_bolts[shooter];
    b->endtime = cl.time + 150;
    VectorCopy(start, b->start);
    VectorCopy(end, b->end);
    b->hit = hit;

    if (!hit)
        return;
    q3w_sound_at(end, q3w.sfx_lghit[Q_rand() % 3]);

    // a hole where it burns, but not one on top of the last: held on a
    // wall it would fill the marks with copies of itself
    VectorSubtract(start, end, dir);
    if (VectorNormalize(dir) > 0 && q3w_mark_surface(end, dir, surface, normal) &&
        DistanceSquared(surface, b->last_mark) > 8 * 8) {
        VectorCopy(surface, b->last_mark);
        q3w_mark(q3w.mark_hole, surface, normal, 12, DECAL_DARKEN, false, q3w_white);
    }
}

// `flags`: RF_WEAPONMODEL for our own bolt from the view gun, RF_VIEWERMODEL
// for its copy from the gun our body holds (q3w_add_bolts)
static void q3w_draw_bolt(int index, const vec3_t start, const vec3_t end, int flags)
{
    vec3_t      dist, org, angles;
    entity_t    e;
    float       d, len, steps;
    const float model_length = 35.0f;
    int         seg = 0, j;

    if (!q3w.lightning)
        return;

    VectorSubtract(end, start, dist);
    vectoangles2(dist, angles);
    d = VectorNormalize(dist);
    if (d < 1)
        return;
    steps = ceilf(d / model_length);
    len = steps > 1 ? (d - model_length) / (steps - 1) : 0;

    memset(&e, 0, sizeof(e));
    e.model = q3w.lightning;
    e.flags = RF_FULLBRIGHT | flags;
    VectorCopy(start, org);

    // as CL_AddBeams lays Quake II's lightning: the model points back
    while (d > 0) {
        VectorCopy(org, e.origin);
        VectorCopy(org, e.oldorigin);
        e.angles[0] = -angles[0];
        e.angles[1] = angles[1] + 180.0f;
        e.angles[2] = CL_FrameRand((index << 10) + seg++) % 360;
        V_AddEntity(&e);

        for (j = 0; j < 3; j++)
            org[j] += dist[j] * len;
        d -= model_length;
    }
}

static void q3w_add_bolts(void)
{
    int i;

    for (i = 1; i <= MAX_CLIENTS; i++) {
        q3wbolt_t       *b = &q3w_bolts[i];
        q3wshooter_t    *sh = &q3w_shooters[i];
        vec3_t          start, end, dir;
        bool            hit = b->hit;
        bool            view = false;

        if (!b->endtime || cl.time >= b->endtime || cl.time < b->endtime - 1000) {
            b->endtime = 0;
            continue;
        }

        if (i == cl.frame.clientNum + 1 && CL_Q3W_ViewMuzzle(start)) {
            // ours in first person: from the gun to where we look now, as far
            // as the server's shot reached (it may have stopped in a player)
            vec3_t  eye, far_end;
            trace_t tr;
            float   range = min(Distance(b->start, b->end), 768.0f);

            VectorCopy(cl.refdef.vieworg, eye);
            VectorMA(eye, 14, cl.v_forward, eye);
            VectorMA(eye, 768, cl.v_forward, far_end);
            tr = CL_TracePoint(eye, far_end, MASK_SHOT, false);
            if (tr.fraction * 768 < range || !b->hit)
                range = tr.fraction * 768;
            hit = b->hit || tr.fraction < 1.0f;
            VectorMA(eye, range, cl.v_forward, end);
            view = true;
        } else {
            if (sh->muzzle_frame == cls.framecount)
                VectorCopy(sh->muzzle, start);
            else
                VectorCopy(b->start, start);
            VectorCopy(b->end, end);
        }

        if (view) {
            vec3_t body_muzzle;

            // Two bolts, as Quake II's beams (tent.c CL_AddPlayerBeams): this
            // one from the view gun for the eye only, which a mirror leaves
            // out as it does the view gun - it came out of your face there
            // (Matt, 11-19: "doubling up or ghosting in the mirror") - and,
            // for mirrors only, one from the gun your body holds there, to
            // the same end.
            q3w_draw_bolt(i, start, end, RF_WEAPONMODEL);
            if (sh->muzzle_frame == cls.framecount)
                q3w_draw_bolt(i, sh->muzzle, end, RF_VIEWERMODEL);
            else if (CL_PlayerGunMuzzlePoint(body_muzzle, 0))
                q3w_draw_bolt(i, body_muzzle, end, RF_VIEWERMODEL);
        } else {
            q3w_draw_bolt(i, start, end, 0);
        }

        // the crackle where it hits
        if (hit && q3w.electric && CL_PathTraced()) {
            entity_t e;

            memset(&e, 0, sizeof(e));
            VectorSubtract(start, end, dir);
            VectorNormalize(dir);
            VectorMA(end, 2, dir, e.origin);
            VectorCopy(e.origin, e.oldorigin);
            e.flags = RF_FLARE;
            e.frame = q3w.electric;
            e.scale = (10 + 4 * frand()) / FLARE_BASE_SIZE;
            e.alpha = 1;
            e.rgba.u32 = 0xffffffffu;
            V_AddEntity(&e);
        }
        V_AddLight(end, 120, 0.6f, 0.6f, 1.0f);
    }
}

/*
==============================================================================

THE PLASMA BALL, AND THE MUZZLE FLASH EVENT

==============================================================================
*/

/*
A new ball first shows where the server has it: 100 units out (fire_plasma's
MISSILE_PRESTEP_TIME, 50 ms at 2000 u/s) and up to a frame's flight more,
well clear of the gun (Matt, 11-19: "spawning too far away from the
camera"). So it is drawn from its shooter's muzzle instead (s.skinnum, from
g_q3weap.c), the offset to there shrinking to nothing over Q3W_BALL_BLEND ms.
Where it really is, and what it hits, stay the server's.
*/
#define Q3W_BALL_BLEND  100     // ms: one 10 Hz server frame
#define Q3W_BALL_LEAD   8       // units out of the barrel it starts

typedef struct {
    int     id;         // centity_t.id of the ball in this slot
    int     time;       // cl.time it was first drawn
    vec3_t  offset;     // from where the server had it then, to the gun
} q3wball_t;

static q3wball_t q3w_balls[MAX_EDICTS];

// the muzzle of the gun that has just fired `shooter`'s ball: the view gun's
// if it is ours in first person. The flash comes in the same packet as the ball.
static bool q3w_ball_muzzle(int shooter, vec3_t out)
{
    q3wshooter_t *sh = q3w_shooter(shooter);

    if (!sh || sh->weapon != Q3W_PLASMA || !q3w_firing(sh, 200))
        return false;
    if (shooter == cl.frame.clientNum + 1 && CL_Q3W_ViewMuzzle(out))
        return true;
    if (sh->muzzle_frame == cls.framecount) {   // its body was drawn this frame
        VectorCopy(sh->muzzle, out);
        return true;
    }
    return false;
}

static void q3w_ball_origin(const entity_state_t *s1, const vec3_t origin, vec3_t out)
{
    const centity_t *cent = &cl_entities[s1->number];
    q3wball_t       *b = &q3w_balls[s1->number];
    vec3_t          muzzle, dir;
    float           frac;

    VectorCopy(origin, out);

    if (b->id != cent->id) {
        b->id = cent->id;
        b->time = cl.time;
        VectorClear(b->offset);
        if (cl_q3_plasma_from_gun->integer && q3w_ball_muzzle(s1->skinnum, muzzle)) {
            AngleVectors(s1->angles, dir, NULL, NULL);
            VectorMA(muzzle, Q3W_BALL_LEAD, dir, muzzle);
            VectorSubtract(muzzle, origin, b->offset);
            // one that came into view in flight, far from the gun
            if (VectorLength(b->offset) > 512)
                VectorClear(b->offset);
        }
    }

    frac = 1.0f - (float)(cl.time - b->time) / Q3W_BALL_BLEND;
    if (frac > 0)
        VectorMA(out, min(frac, 1.0f), b->offset, out);
}

// entities.c: an RF_Q3_SPRITE entity (no model) at `origin`
void CL_Q3W_AddSprite(const entity_state_t *s1, const vec3_t origin_in)
{
    entity_t e;
    vec3_t   origin;

    q3w_ball_origin(s1, origin_in, origin);

    // cg_weapons.c: the plasma's missileDlight, 200 in its blue
    V_AddLight(origin, 200, 0.6f, 0.6f, 1.0f);

    q3w_dbg.balls++;
    if (s1->frame != Q3_SPRITE_PLASMA || !q3w.plasma_sprite || !CL_PathTraced())
        return;
    q3w_dbg.balls_drawn++;

    memset(&e, 0, sizeof(e));
    VectorCopy(origin, e.origin);
    VectorCopy(origin, e.oldorigin);
    e.flags = RF_FLARE;
    e.frame = q3w.plasma_sprite;
    e.scale = 1;            // FLARE_BASE_SIZE, Quake III's radius 16
    e.alpha = 1;
    e.rgba.u32 = 0xffffffffu;
    // spinning: sprites/plasma1's "tcMod rotate 931", 931 degrees a second off
    // the shader clock, so every ball turns together as in Quake III (Matt,
    // 11-18: "looks too static"). The sprite pass turns the quad by angles[ROLL]
    // (transparency.c); 360 s is a whole number of turns, so the wrap is seamless.
    e.angles[ROLL] = -(float)(cl.time % 360000) * 0.931f;
    V_AddEntity(&e);
}

/*
=================
CL_Q3W_MuzzleFlash

CL_MuzzleFlash, for MZ_Q3_*: the light in the gun's colour, the time the
view and third-person guns flash and spin by, and the brass.
=================
*/
void CL_Q3W_MuzzleFlash(int entnum, int mz, cdlight_t *dl)
{
    int             w = mz - MZ_Q3_GAUNTLET + 1;
    q3wshooter_t    *sh = q3w_shooter(entnum);

    if (w < 1 || w >= Q3W_NUM)
        return;

    VectorCopy(q3w_guns[w].light, dl->color);
    dl->radius = 300 + (Q_rand() & 31);

    if (sh) {
        sh->fire_time = cl.time;
        sh->weapon = w;
    }
    CL_Q3W_EjectBrass(entnum, mz);
}

/*
==============================================================================

PREDICTED SHOTS (cl_q3_predict_shots)

OUR OWN GUN FIRES ON OUR OWN MACHINE, AS QUAKE III'S DOES. A player wrote in
(2026-11-21): the Quake III mode "still feels too Quake 2 in terms of
responsiveness ... the precise timing and responsiveness of weapon firing is
not as good as Quake 3", and beat Crash on Nightmare at once in Quake3e.

The gun itself already runs as Quake III's does - g_q3weap.c Q3W_Think, on
every usercmd, with Quake III's millisecond weaponTime. What did not was
everything we SEE and HEAR of a shot: its sound, flash, light, brass and the
gun's recoil all came from the server, which sends them with its next 10 Hz
frame - up to 100 ms after the click. Quake III's client runs PM_Weapon on its
own commands (cg_predict.c) and plays EV_FIRE_WEAPON the moment a predicted
command fires. Now so does ours: CL_Q3W_PredictCmd runs the same machine on
each command as it is made, and a shot is shown then. The shot itself - the
hit, the missile - is still the server's.

KEEPING IN STEP. Every stat is in use, so the server's weaponTime cannot be
sent; but the two machines run the same commands from the same start, and
both drop back to time 0 the moment the trigger is up. ps.gunframe's six-bit
shot count says what the server fired up to the command a frame acknowledges
(CL_Q3W_FrameParsed): its flashes and fire sounds of ours in that packet that
we predicted are dropped, the rest (a shot we did not foresee - a gauntlet
hit, a gun just raised, an out-of-step burst) still play, and our count is
lined up with the server's from there.

Not foreseen, and left to the server: the gauntlet (it fires on a hit), a
change of gun begun this frame, the last shot of the ammo. cl_q3_predict_shots
0 is the old behaviour.

==============================================================================
*/

#define Q3P_SHOTS   Q3_GUNFRAME_SHOTS   // the shot count wraps at 64

// g_q3weap.c q3w_info's fire_time and flash_sound
static const struct {
    int         fire_time;
    const char  *sound;     // NULL: the machinegun picks one of four
} q3p_info[Q3W_NUM] = {
    [Q3W_GAUNTLET]   = {  400, NULL },
    [Q3W_MACHINEGUN] = {  100, NULL },
    [Q3W_SHOTGUN]    = { 1000, "weapons/shotgun/sshotf1b.wav" },
    [Q3W_GRENADE]    = {  800, "weapons/grenade/grenlf1a.wav" },
    [Q3W_ROCKET]     = {  800, "weapons/rocket/rocklf1a.wav" },
    [Q3W_LIGHTNING]  = {   50, "weapons/lightning/lg_fire.wav" },
    [Q3W_RAILGUN]    = { 1500, "weapons/railgun/railgf1a.wav" },
    [Q3W_PLASMA]     = {  100, "weapons/plasma/hyprbf1a.wav" },
    [Q3W_BFG]        = {  200, "weapons/bfg/bfg_fire.wav" },
};

static cvar_t   *cl_q3_predict_shots;

static struct {
    bool        active;         // the last command was predicted
    bool        firing;         // WEAPON_FIRING
    int         weapon;         // the Q3W_* it was predicted with
    int         time;           // client->q3_weapon_time
    int         shots;          // client->q3_shots & Q3P_SHOTS
    byte        shots_at[CMD_BACKUP];   // the count after each command
    int         ack;            // the command the last frame acknowledged
    int         server_shots;   // and the server's count at it
    int         skip_flashes;   // the server's copies of ours in this packet
    int         skip_sounds;
    qhandle_t   sfx[Q3W_NUM], sfx_mg[4];
} q3p;

static void q3p_register(void)
{
    int i;

    memset(&q3p, 0, sizeof(q3p));
    for (i = 1; i < Q3W_NUM; i++)
        if (q3p_info[i].sound)
            q3p.sfx[i] = q3w_sound(q3p_info[i].sound);
    for (i = 0; i < 4; i++)
        q3p.sfx_mg[i] = q3w_sound(va("weapons/machinegun/machgf%db.wav", i + 1));
}

// the Quake III gun the server has up in our hand, ready or firing; 0 when
// there is nothing to predict
static int q3p_weapon(void)
{
    const player_state_t    *ps = &cl.frame.ps;
    const int               state = ps->gunframe & Q3_GUNFRAME_STATE;
    q3wgun_t                *g;

    if (!cl_q3_predict_shots->integer || !cl_predict->integer || cls.demo.playback || !cl.frame.valid)
        return 0;
    // dead, a spectator, frozen at the intermission
    if (ps->pmove.pm_type != PM_NORMAL || (ps->pmove.pm_flags & PMF_NO_PREDICTION))
        return 0;
    if (ps->stats[STAT_HEALTH] <= 0)
        return 0;
    // raising or lowering: the server's
    if (state != Q3_GUNFRAME_READY && state != Q3_GUNFRAME_FIRING)
        return 0;
    if (ps->gunindex <= 0 || ps->gunindex >= MAX_MODELS)
        return 0;
    g = q3w_gun_for_model(cl.model_draw[ps->gunindex]);
    return g ? (int)(g - q3w_guns) : 0;
}

// Q3W_Think's time between shots: haste (g_q3.c Q3_PmoveGravity puts it in
// the gravity's low bit for prediction) 1.3 times as fast, the CTF Time Accel
// (stat 31 its icon, g_ctf.c) twice
static int q3p_fire_time(int w)
{
    int t = q3p_info[w].fire_time;
    int tech = cl.frame.ps.stats[31];

    if (cl.frame.ps.pmove.gravity & 1)
        t = (int)(t / 1.3f);
    if (tech > 0 && tech < MAX_IMAGES && strstr(cl.configstrings[CS_IMAGES + tech], "tech3"))
        t /= 2;
    return t;
}

// a predicted shot: what the server's svc_muzzleflash and fire sound would
// have done (CL_MuzzleFlash: the light, the view and model flashes, the brass)
static void q3p_shot(int w, bool was_firing)
{
    const int       self = cl.frame.clientNum + 1;
    mz_params_t     saved = mz;
    qhandle_t       sfx;

    mz.entity = self;
    mz.weapon = MZ_Q3_GAUNTLET + w - 1;
    mz.silenced = false;
    mz.has_dir = false;
    CL_MuzzleFlash();
    mz = saved;

    // the lightning gun's fire sound only as the trigger goes down; its hum
    // loops after (the server's weapon sound)
    if (w == Q3W_MACHINEGUN)
        sfx = q3p.sfx_mg[Q_rand() & 3];
    else if (w == Q3W_LIGHTNING && was_firing)
        sfx = 0;
    else
        sfx = q3p.sfx[w];
    if (sfx)
        S_StartSound(NULL, self, CHAN_WEAPON, sfx, 1, ATTN_NORM, 0);
}

/*
=================
CL_Q3W_PredictCmd

CL_FinalizeCmd, as command (number) is stored: Q3W_Think on it, once.
=================
*/
void CL_Q3W_PredictCmd(int number)
{
    const usercmd_t *cmd = &cl.cmds[number & CMD_MASK];
    const int       w = q3p_weapon();
    int             pending;
    bool            was_firing;

    if (!w) {
        q3p.active = false;
        q3p.firing = false;
        q3p.time = 0;
        goto done;
    }
    if (!q3p.active || w != q3p.weapon) {
        // taking over from the server, with its gun up: from rest
        q3p.weapon = w;
        q3p.time = 0;
        q3p.firing = false;
    }
    q3p.active = true;

    if (q3p.time > 0)
        q3p.time -= cmd->msec;
    if (q3p.time > 0)
        goto done;

    if (!(cmd->buttons & BUTTON_ATTACK)) {
        q3p.time = 0;
        q3p.firing = false;
        goto done;
    }

    // the gauntlet fires only on a hit, which is the server's to find
    if (w == Q3W_GAUNTLET) {
        q3p.time = 0;
        q3p.firing = true;
        goto done;
    }

    // out of ammo, counting the shots the server has not seen yet: the
    // server clicks and changes gun
    pending = (q3p.shots - q3p.shots_at[q3p.ack & CMD_MASK]) & Q3P_SHOTS;
    if (cl.frame.ps.stats[STAT_AMMO] - pending <= 0) {
        q3p.firing = false;
        q3p.time += 500;
        goto done;
    }

    was_firing = q3p.firing;
    q3p.shots = (q3p.shots + 1) & Q3P_SHOTS;
    q3p.firing = true;
    q3p.time += q3p_fire_time(w);
    q3p_shot(w, was_firing);

done:
    q3p.shots_at[number & CMD_MASK] = q3p.shots;
}

/*
=================
CL_Q3W_FrameParsed

CL_DeltaFrame: the server's shot count at the command this frame
acknowledges. The flashes and fire sounds of ours that follow in the packet
are for the shots it fired since the last frame; as many of those as we
predicted are dropped (CL_Q3W_SkipServerFlash/Sound).
=================
*/
void CL_Q3W_FrameParsed(void)
{
    int ack, server, covered, fresh, off, n;

    q3p.skip_flashes = q3p.skip_sounds = 0;
    if (!cls.netchan || cls.demo.playback)
        return;

    ack = cl.history[cls.netchan->incoming_acknowledged & CMD_MASK].cmdNumber;
    if (cl.cmdNumber - ack >= CMD_BACKUP || ack - q3p.ack < 0) {
        q3p.ack = ack;
        q3p.server_shots = cl.frame.ps.gunframe & Q3P_SHOTS;
        return;
    }
    server = cl.frame.ps.gunframe & Q3P_SHOTS;

    if (q3p.ack && cl.cmdNumber - q3p.ack < CMD_BACKUP) {
        covered = (q3p.shots_at[ack & CMD_MASK] - q3p.shots_at[q3p.ack & CMD_MASK]) & Q3P_SHOTS;
        fresh = (server - q3p.server_shots) & Q3P_SHOTS;
        q3p.skip_flashes = q3p.skip_sounds = min(covered, fresh);
    }

    // our count lined up with the server's, from the acknowledged command on
    off = (server - q3p.shots_at[ack & CMD_MASK]) & Q3P_SHOTS;
    if (off) {
        for (n = ack; n - cl.cmdNumber <= 0; n++)
            q3p.shots_at[n & CMD_MASK] = (q3p.shots_at[n & CMD_MASK] + off) & Q3P_SHOTS;
        q3p.shots = (q3p.shots + off) & Q3P_SHOTS;
    }
    q3p.ack = ack;
    q3p.server_shots = server;
}

// parse.c: the server's muzzle flash of a shot of ours already shown
bool CL_Q3W_SkipServerFlash(int entity, int weapon)
{
    if (q3p.skip_flashes <= 0 || entity != cl.frame.clientNum + 1 ||
        weapon < MZ_Q3_GAUNTLET || weapon > MZ_Q3_BFG)
        return false;
    q3p.skip_flashes--;
    return true;
}

// parse.c: the server's fire sound of a shot of ours already heard
bool CL_Q3W_SkipServerSound(int entity, int channel)
{
    if (q3p.skip_sounds <= 0 || entity != cl.frame.clientNum + 1 || channel != CHAN_WEAPON)
        return false;
    q3p.skip_sounds--;
    return true;
}

// the view gun's state: ours while we predict, the server's while it raises
// or lowers the gun
static int q3p_view_state(int state)
{
    if (q3p.active && (state == Q3_GUNFRAME_READY || state == Q3_GUNFRAME_FIRING))
        return q3p.firing ? Q3_GUNFRAME_FIRING : Q3_GUNFRAME_READY;
    return state;
}

/*
==============================================================================

GIBS (cg_effects.c CG_GibPlayer, cg_localents.c CG_AddFragment)

Where Quake III's game is played (q3w_q3_game; the game's g_q3.c
Q3_GibStyle), a gibbed player's body goes and the game sends TE_Q3_GIBS from
where it was, as Quake III's EV_GIB_PLAYER. Each client throws the pieces
itself, as Quake III's did: a skull or a brain, and unless cl_gibs is 0 an
abdomen, an arm, a chest, a fist, a foot, a forearm, the intestines and two
legs - eleven, each 250 units a second any way across and 250 +- 250 up,
under gravity 800, bouncing back 0.6 of the way, and at rest on the first
floor it meets moving up slower than 40. Like Quake III's they do not turn
in the air (each starts facing a random way).

Their blood (Matt, 11-19: "the same style blood streams just using our
sphereical blood ... If someone has the blood simulation turned off maybe
they should just get the quake 3 blood graphics and effects. The blood also
sometimes leaves decals"):
  - with the blood simulation on, each piece trails the sphere drip Quake
    II's gibs do, cl_q3_gib_drops times as thick (CL_DiminishingTrailDrops;
    3 by default - Matt, 11-19), and its first landing throws a
    splash of spheres that stain where they fall
  - with it off, Quake III's own: a puff of its bloodTrail every 150 ms in
    the air (20 units, growing as it falls 40 over 2 s), and its bloodMark on
    the first landing (16-47 units); its bloodExplosion spurts where shots
    hit (CL_Q3W_Bleed). Under GL and software, Quake II's particles.
The first landing also makes a splat half the time (gibimp1-3), and the
gibbing Quake III's gibsplt1.

Pieces stay until the level ends in a local game, as the brass does; in a
network game they go as Quake III's do, 5-8 s, sinking into the floor over
the last second (cl_q3_gibs_time, as cl_brass_time).

==============================================================================
*/

#define Q3W_MAX_GIBS    1024
#define Q3W_GIB_ID      0x3000      // ids for motion vectors, past the brass's

enum { Q3W_GIB_SKULL, Q3W_GIB_BRAIN, Q3W_GIB_ABDOMEN, Q3W_GIB_ARM, Q3W_GIB_CHEST, Q3W_GIB_FIST,
       Q3W_GIB_FOOT, Q3W_GIB_FOREARM, Q3W_GIB_INTESTINE, Q3W_GIB_LEG };

#define Q3W_GIB_VELOCITY    250     // GIB_VELOCITY
#define Q3W_GIB_JUMP        250     // GIB_JUMP
#define Q3W_GIB_BOUNCE      0.6f    // CG_LaunchGib's bounceFactor
#define Q3W_BLOOD_STEP      150     // CG_BloodTrail: a puff every 150 ms

typedef struct {
    int         model;          // Q3W_GIB_*; -1 free
    bool        resting;
    bool        landed;         // its first landing made its mark and sound
    vec3_t      origin, velocity;
    float       yaw;
    int         trailcount;     // the sphere drip's (CL_DiminishingTrailCount)
    int         next_puff;      // the next bloodTrail puff, on the 150 ms grid
    int         end;            // 0: for ever
} q3wgib_t;

static q3wgib_t q3w_gibs[Q3W_MAX_GIBS];
static int      q3w_gib_next;
static int      q3w_gib_time;   // cl.time of the last physics step

static int q3w_gib_cap(void)
{
    return Cvar_ClampInteger(cl_q3_gibs_max, 16, Q3W_MAX_GIBS);
}

static void q3w_launch_gib(int model, const vec3_t origin)
{
    q3wgib_t    *g = &q3w_gibs[q3w_gib_next];
    int         life;

    q3w_gib_next = (q3w_gib_next + 1) % q3w_gib_cap();

    memset(g, 0, sizeof(*g));
    g->model = model;
    VectorCopy(origin, g->origin);
    g->velocity[0] = crand() * Q3W_GIB_VELOCITY;
    g->velocity[1] = crand() * Q3W_GIB_VELOCITY;
    g->velocity[2] = Q3W_GIB_JUMP + crand() * Q3W_GIB_VELOCITY;
    g->yaw = frand() * 360;
    g->trailcount = 1024;
    g->next_puff = Q3W_BLOOD_STEP * (cl.time / Q3W_BLOOD_STEP + 1);

    life = q3w_life(cl_q3_gibs_time, 5000 + (Q_rand() % 3000));
    g->end = life ? cl.time + life : 0;
}

// Quake III's blood puff: CG_BloodTrail's CG_SmokePuff, LE_FALL_SCALE_FADE
static void q3w_blood_puff(const vec3_t origin, int start)
{
    q3wfx_t *fx;

    if (!q3w.blood_trail)
        return;
    fx = q3w_alloc_fx();
    fx->image[0] = q3w.blood_trail;
    fx->images = 1;
    VectorCopy(origin, fx->origin);
    fx->start = start;
    fx->life = 2000;
    fx->size0 = 16;             // its radius grows from 16 to 20 + 16
    fx->size1 = 36;
    fx->drop = 40;
    fx->roll = frand() * 360;
    fx->blend = fx->near = true;
}

// the blood of a piece flying from `from` to `to` in the step of `ms` that
// ends at `now`
static void q3w_gib_trail(q3wgib_t *g, const vec3_t from, const vec3_t to, int ms, int now)
{
    if (cl_disable_particles->integer & NOPART_BLOOD)
        return;

    if (!q3w_blood_pictures()) {
        CL_DiminishingTrailDrops(from, to, &g->trailcount, EF_GIB,
                                 Cvar_ClampInteger(cl_q3_gib_drops, 1, 8));
        return;
    }

    // a puff on each 150 ms of the step, where the piece was then
    while (g->next_puff <= now) {
        float   f = ms > 0 ? 1.0f - (now - g->next_puff) / (float)ms : 1.0f;
        vec3_t  at;

        LerpVector(from, to, q3w_clip01(f), at);
        q3w_blood_puff(at, g->next_puff);
        g->next_puff += Q3W_BLOOD_STEP;
    }
}

// the first time a piece lands: its mark (CG_FragmentBounceMark) and, half
// the time, a splat (CG_FragmentBounceSound)
static void q3w_gib_land(q3wgib_t *g, const vec3_t point, const vec3_t normal)
{
    g->landed = true;

    if (Q_rand() & 1) {
        int r = Q_rand() & 3;

        q3w_sound_at(point, q3w.sfx_gibimp[r == 0 ? 0 : r == 1 ? 1 : 2]);
    }

    if (cl_disable_particles->integer & NOPART_BLOOD)
        return;
    if (q3w_blood_pictures()) {
        q3w_mark(q3w.mark_blood, point, normal, 16 + (Q_rand() & 31), DECAL_BLEND, false, q3w_white);
        return;
    }
    // a splash of the simulation's spheres, which stain where they fall
    CL_BloodParticleEffect(point, normal, 0xe8, 8 + (Q_rand() & 15));
}

// one step in the air, of dt seconds ending at `now`
static void q3w_gib_fly(q3wgib_t *g, float dt, int now)
{
    vec3_t  from, next, v;
    trace_t tr;
    float   dot;
    int     ms = (int)(dt * 1000);

    VectorCopy(g->origin, from);
    VectorMA(g->origin, dt, g->velocity, next);
    next[2] -= 400 * dt * dt;

    // fallen out of the map (CL_SetVoidFloor): gone, and no more trail
    if (CL_BelowVoidFloor(next)) {
        g->model = -1;
        return;
    }

    tr = CL_TraceBox(g->origin, q3w.gib_mins[g->model], q3w.gib_maxs[g->model], next, MASK_SOLID, false);
    if (tr.allsolid) {
        // started inside a wall: Quake III's stop
        g->resting = true;
        return;
    }
    if (tr.fraction == 1.0f) {
        VectorCopy(next, g->origin);
        g->velocity[2] -= 800 * dt;
        q3w_gib_trail(g, from, next, ms, now);
        return;
    }

    // into lava or slime, or down on the void: gone, as the brass (Quake
    // III's CONTENTS_NODROP) - no splat, no mark
    if ((CL_PointContents(tr.endpos) & (CONTENTS_LAVA | CONTENTS_SLIME)) ||
        (tr.plane.normal[2] > 0.7f && CL_BloodVoidSurface(tr.surface))) {
        g->model = -1;
        return;
    }

    VectorCopy(tr.endpos, g->origin);
    q3w_gib_trail(g, from, g->origin, ms, now);
    if (!g->landed)
        q3w_gib_land(g, tr.endpos, tr.plane.normal);

    // CG_ReflectVelocity: the speed when it hit, reflected, times 0.6
    VectorCopy(g->velocity, v);
    v[2] -= 800 * dt * tr.fraction;
    dot = DotProduct(v, tr.plane.normal);
    VectorMA(v, -2 * dot, tr.plane.normal, g->velocity);
    VectorScale(g->velocity, Q3W_GIB_BOUNCE, g->velocity);
    VectorMA(g->origin, 0.1f, tr.plane.normal, g->origin);

    if (tr.plane.normal[2] > 0 && g->velocity[2] < 40) {
        g->resting = true;
        VectorClear(g->velocity);
    }
}

static void q3w_add_gibs(void)
{
    q3wgib_t    *g;
    entity_t    e;
    float       dt;
    int         i, cap = q3w_gib_cap();

    dt = (cl.time - q3w_gib_time) * 0.001f;
    q3w_gib_time = cl.time;
    if (dt < 0 || dt > 0.1f)
        dt = dt < 0 ? 0 : 0.1f;

    memset(&e, 0, sizeof(e));
    for (i = 0, g = q3w_gibs; i < cap; i++, g++) {
        if (g->model < 0 || !q3w.gib_model[g->model])
            continue;
        if (g->end && cl.time >= g->end) {
            g->model = -1;
            continue;
        }

        if (dt > 0 && !g->resting) {
            // two steps when the frame is long, so a fast piece is not missing walls
            if (dt > 0.05f) {
                q3w_gib_fly(g, dt * 0.5f, cl.time - (int)(dt * 500));
                if (g->model >= 0 && !g->resting)
                    q3w_gib_fly(g, dt * 0.5f, cl.time);
            } else {
                q3w_gib_fly(g, dt, cl.time);
            }
            if (g->model < 0)
                continue;
        }

        if (DistanceSquared(g->origin, cl.refdef.vieworg) > 3072 * 3072)
            continue;

        e.model = q3w.gib_model[g->model];
        e.id = Q3W_GIB_ID + i;
        VectorCopy(g->origin, e.origin);
        // sink into the ground over its last second (SINK_TIME)
        if (g->end && g->resting && g->end - cl.time < 1000)
            e.origin[2] -= 16 * (1.0f - (g->end - cl.time) / 1000.0f);
        VectorCopy(e.origin, e.oldorigin);
        VectorSet(e.angles, 0, g->yaw, 0);
        V_AddEntity(&e);
    }
}

/*
=================
CL_Q3W_Gibs

TE_Q3_GIBS (tent.c): a player gibbed where `origin` is.
=================
*/
void CL_Q3W_Gibs(const vec3_t origin)
{
    int i;

    // a level whose start did not expect Quake III's game: load them now
    if (!q3w.gibs_ready)
        q3w_register_gibs();

    q3w_sound_at(origin, q3w.sfx_gib);

    q3w_launch_gib((Q_rand() & 1) ? Q3W_GIB_SKULL : Q3W_GIB_BRAIN, origin);

    // allow gibs to be turned off for speed: the head alone
    if (!cl_gibs->integer)
        return;

    for (i = Q3W_GIB_ABDOMEN; i <= Q3W_GIB_LEG; i++)
        q3w_launch_gib(i, origin);
    q3w_launch_gib(Q3W_GIB_LEG, origin);
}

/*
=================
CL_Q3W_Bleed

TE_BLOOD (tent.c): where Quake III's game is played and its blood pictures
are shown (the blood simulation off, a path tracer), Quake III's spurt of
blood where the shot hit (CG_Bleed: its bloodExplosion, 24 units, for half a
second) - and true, so Quake II's spray is not drawn too.
=================
*/
bool CL_Q3W_Bleed(const vec3_t origin)
{
    q3wfx_t *fx;
    int     i;

    if (!q3w_q3_game() || !q3w_blood_pictures() || !q3w.blood_spurt[0])
        return false;
    if (cl_disable_particles->integer & NOPART_BLOOD)
        return true;

    fx = q3w_alloc_fx();
    for (i = 0; i < 3; i++)
        fx->image[i] = q3w.blood_spurt[i];
    fx->images = 3;
    VectorCopy(origin, fx->origin);
    fx->life = 500;
    fx->size0 = fx->size1 = 24;
    fx->roll = (float)(Q_rand() % 360);
    fx->blend = fx->hold = true;
    return true;
}

/*
==============================================================================

EACH FRAME, AND THE REST

==============================================================================
*/

// cl_q3_debug: the last second's counts (frames' worth of the drawn ones)
static void q3w_debug_print(void)
{
    unsigned now = Sys_Milliseconds();

    if (!cl_q3_debug->integer) {
        q3w_dbg.last = now;
        return;
    }
    if (now - q3w_dbg.last < 1000)
        return;
    Com_Printf("q3 weapons (cl.q3_weapons %d, path traced %d): plasma balls %d (%d drawn, sprite image %d); "
               "marks made %d, refused: no image %d, renderer %d, cl_q3_marks_time 0 %d, no surface %d, "
               "no fit %d; drawn (entity-frames): marks %d, impact flashes %d\n",
               cl.q3_weapons, CL_PathTraced(), q3w_dbg.balls, q3w_dbg.balls_drawn, q3w.plasma_sprite,
               q3w_dbg.marks, q3w_dbg.refused[Q3W_REFUSED_NO_IMAGE], q3w_dbg.refused[Q3W_REFUSED_RENDERER],
               q3w_dbg.refused[Q3W_REFUSED_OFF], q3w_dbg.refused[Q3W_REFUSED_NO_SURFACE],
               q3w_dbg.refused[Q3W_REFUSED_NO_FIT], q3w_dbg.marks_drawn, q3w_dbg.fx_drawn);
    memset(&q3w_dbg, 0, sizeof(q3w_dbg));
    q3w_dbg.last = now;
}

// CL_AddTEnts
void CL_Q3W_AddEntities(void)
{
    q3w_add_brass();
    q3w_add_gibs();
    q3w_add_marks();
    q3w_add_fx();
    q3w_add_bolts();
    q3w_debug_print();
}

// CL_ClearTEnts: a new level, a disconnect
void CL_Q3W_Clear(void)
{
    int i;

    for (i = 0; i < Q3W_MAX_BRASS; i++)
        q3w_brass[i].kind = -1;
    q3w_brass_next = 0;
    q3w_brass_time = 0;
    for (i = 0; i < Q3W_MAX_GIBS; i++)
        q3w_gibs[i].model = -1;
    q3w_gib_next = 0;
    q3w_gib_time = 0;
    memset(q3w_marks, 0, sizeof(q3w_marks));
    q3w_mark_next = 0;
    memset(q3w_fx, 0, sizeof(q3w_fx));
    memset(q3w_bolts, 0, sizeof(q3w_bolts));
    memset(q3w_balls, 0, sizeof(q3w_balls));
    memset(q3w_shooters, 0, sizeof(q3w_shooters));
    memset(&q3v, 0, sizeof(q3v));
    // a new server's commands start again from 0; the sounds stay registered
    q3p.active = q3p.firing = false;
    q3p.time = q3p.shots = q3p.ack = q3p.server_shots = 0;
    q3p.skip_flashes = q3p.skip_sounds = 0;
    memset(q3p.shots_at, 0, sizeof(q3p.shots_at));
}

void CL_Q3W_Init(void)
{
    // ms each piece of brass and each mark lasts: -1 (the default) for ever,
    // except in a network game, where Quake III's (2.5 s for a cartridge,
    // 7.5-10 for a shell; 10 s for a mark); 0 none at all
    cl_brass_time = Cvar_Get("cl_brass_time", "-1", CVAR_ARCHIVE);
    cl_q3_marks_time = Cvar_Get("cl_q3_marks_time", "-1", CVAR_ARCHIVE);
    // how many there can be; past that the oldest go
    cl_brass_max = Cvar_Get("cl_brass_max", "4096", CVAR_ARCHIVE);
    cl_q3_marks_max = Cvar_Get("cl_q3_marks_max", "1024", CVAR_ARCHIVE);
    // every Quake III gun's flash, on top of its own size (the machinegun's
    // is half): a tuning knob, not archived
    cl_q3_flash_scale = Cvar_Get("cl_q3_flash_scale", "1", 0);
    // 1: once a second, what the plasma ball, the marks and the impact flashes
    // did (here) and what reached the renderer (transparency.c); 2: each mark
    // that found no wall too
    cl_q3_debug = Cvar_Get("cl_q3_debug", "0", 0);
    // 1: a plasma ball starts at its gun's muzzle and catches up with the
    // server's ball in 0.1 s; 0: first seen where Quake III has it, 100+ out
    cl_q3_plasma_from_gun = Cvar_Get("cl_q3_plasma_from_gun", "1", 0);
    // Quake III's gibs (GIBS): ms each piece lasts, as cl_brass_time - -1 for
    // ever except in a network game, where Quake III's 5-8 s - and how many
    // pieces there can be (eleven a gibbing); past that the oldest go
    cl_q3_gibs_time = Cvar_Get("cl_q3_gibs_time", "-1", CVAR_ARCHIVE);
    cl_q3_gibs_max = Cvar_Get("cl_q3_gibs_max", "512", CVAR_ARCHIVE);
    // droplets for each drip of a gib's blood trail with the simulation on
    // (Matt, 11-19: "3x the amount of blood in the trails"); Blood menu
    cl_q3_gib_drops = Cvar_Get("cl_q3_gib_drops", "3", CVAR_ARCHIVE);
    // 1: our own Quake III gun's shots are shown and heard as the command
    // fires them, as Quake III's are, not when the server's 10 Hz frame says
    // so (PREDICTED SHOTS); 0: the server's
    cl_q3_predict_shots = Cvar_Get("cl_q3_predict_shots", "1", CVAR_ARCHIVE);

    CL_Q3W_Clear();
}
