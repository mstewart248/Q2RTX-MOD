/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2026 Q2RTX contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

// g_q1vm_game.c - the game around Quake 1's progs: starting a level, the
// players (their moves, views and HUD), the frame, level changes and the
// spawn parms carried between levels. The parts of Quake's sv_main.c,
// sv_user.c, host_cmd.c and view.c that are not physics.

#include "g_q1vm.h"

static cvar_t *g_q1_progs;
static cvar_t *g_q1_flaregun_delay;    // seconds between flares (THE FLARE GUN)

// a player, as Quake's server and client knew them
typedef struct {
    // carried between levels: Quake's spawn parms (SetNewParms / SetChangeParms)
    bool    valid;
    float   parms[16];
    float   level_parms[16];    // what this level began with: "restart", setspawnparms

    bool    in_game;            // PutClientInServer has run on this level
    bool    loaded;             // a savegame's entity waits for them (Q1VM_ReadLevel)

    // the commands since the last frame
    int     buttons;            // every button pressed since the last frame
    int     buttons_now;        // as the last command had them
    int     upmove;
    short   cmd_angles[3];

    // the view (Quake's view.c)
    float   dmg_time, dmg_roll, dmg_pitch;
    float   cshift_damage;
    unsigned hits;      // svc_damages so far, for the status bar's pain face (STAT_Q1_HIT)
    vec3_t  damage_color;
    float   cshift_bonus;

    // HUD
    int     layout_sent;        // intermission kind its layout went out for
    int     layout_chars;       // how much of the finale text it showed
    bool    finale_armed;       // let go of the buttons since the text typed out
    bool    show_scores;        // Quake's +showscores: toggled by inven / score / help
    int     gun_string;         // weaponmodel the gun index is for
    int     gun_index;

    // Quake II RTX's flare gun (THE FLARE GUN): out instead of the progs'
    // weapon, when the next flare may go, the view model's frame
    bool    flare_held;
    float   flare_next;
    int     flare_frame;
} q1_client_t;

static q1_client_t  q1c[MAX_CLIENTS];
static float        q1_serverflags;

// Quake II RTX's flare gun on Quake 1's levels (THE FLARE GUN, below)
#define Q1_FLARE_CLASS  "q2rtx_flare"
#define Q1_FLARE_MODEL  "models/objects/flare/tris.md2"
#define Q1_FLARE_VIEW   "models/weapons/v_flareg/tris.md3"
static int flare_class_str, flare_model_str;    // their QC strings this level
static bool q1_flaregun_allowed(void);
static void q1_flare_weapon_impulse(int num, int impulse);

// a field the status bar reads that only some progs define (-1 when they
// don't): the mission packs' items2
static int fld_items2;

static int q1_field(const char *name)
{
    qc_def_t *d = QC_FindField(name);
    return d && (d->type & ~QC_DEF_SAVEGLOBAL) == QC_ev_float ? d->ofs : -1;
}

static float q1_field_float(int num, int ofs)
{
    return ofs >= 0 ? QC_Field(num, ofs)->_float : 0;
}

bool Q1VM_Active(void)
{
    return qvm.active;
}

void Q1VM_GameInit(void)
{
    g_q1_progs = gi.cvar("g_q1_progs", "1", 0);
    g_q1_flaregun_delay = gi.cvar("g_q1_flaregun_delay", "2", 0);
    Q1_PhysicsInit();

    // a new game: no parms, no runes
    memset(q1c, 0, sizeof(q1c));
    q1_serverflags = 0;
    QC_FreeProgs();
}

int Q1VM_Skill(void)
{
    int s = skill ? skill->integer : 1;
    clamp(s, 0, 3);
    return s;
}

bool Q1VM_ClientInGame(int num)
{
    if (num < 1 || num > game.maxclients)
        return false;
    edict_t *e = &g_edicts[num];
    return q1c[num - 1].in_game && e->inuse && e->client && e->client->pers.connected;
}

void Q1VM_GetSpawnParms(int client, float *parms)
{
    memcpy(parms, q1c[client - 1].level_parms, sizeof(q1c[0].level_parms));
}

/*
==============================================================================

STARTING A LEVEL

==============================================================================
*/

// "q1/e1m1" is id1's e1m1; "q1/hipnotic/hip1m1" is the add-on's
static void q1_parse_mapname(const char *mapname, char *episode, size_t esize, char *map, size_t msize)
{
    const char *s = mapname;

    if (!Q_strncasecmp(s, "q1/", 3))
        s += 3;

    const char *slash = strchr(s, '/');
    if (slash) {
        size_t n = slash - s;
        if (n >= esize)
            n = esize - 1;
        memcpy(episode, s, n);
        episode[n] = 0;
        Q_strlcpy(map, slash + 1, msize);
    } else {
        Q_strlcpy(episode, "id1", esize);
        Q_strlcpy(map, s, msize);
    }
}

/*
Quake's SV_SpawnServer, from loading the progs to the entities' spawn
functions. Returns false for a level the progs don't run - not a Quake 1 map,
a deathmatch (the bot match), g_q1_progs 0, or a progs.dat that won't load -
and Quake II's own SpawnEntities carries on.
*/
bool Q1VM_SpawnEntities(const char *mapname, const char *entities)
{
    char episode[16], map[MAX_QPATH];

    // the last level's machine went with its memory (TAG_LEVEL)
    QC_FreeProgs();

    if (!level.q1map || deathmatch->value || !g_q1_progs || !g_q1_progs->integer) {
        gi.cvar_forceset("sv_q1vm", "0");
        return false;
    }

    q1_parse_mapname(mapname, episode, sizeof(episode), map, sizeof(map));
    COM_StripExtension(map, map, sizeof(map));

    if (!QC_LoadProgs(episode)) {
        QC_FreeProgs();
        gi.cvar_forceset("sv_q1vm", "0");
        gi.dprintf("Quake 1 game: %s's progs didn't load; the map runs with Quake II's rules\n", episode);
        return false;
    }

    qvm.active = true;
    Q_strlcpy(qvm.q1map, map, sizeof(qvm.q1map));
    Q_strlcpy(qvm.q2map, mapname, sizeof(qvm.q2map));
    // for the engine's saves: Quake has no hubs, so a level left is not kept
    // to come back to (server/save.c); the level's own save is the progs'
    // machine (Q1VM_WriteLevel)
    gi.cvar_forceset("sv_q1vm", "1");

    qvm.time = 1.0f;
    qvm.frametime = FRAMETIME;
    qvm.skill = Q1VM_Skill();

    // the players' entities are there from the start, empty until they join
    qvm.num_edicts = game.maxclients + 1;
    if (globals.num_edicts < qvm.num_edicts)
        globals.num_edicts = qvm.num_edicts;
    for (int i = 0; i <= game.maxclients; i++)
        QC_ClearEdict(i);
    for (int i = 0; i < game.maxclients; i++)
        q1c[i].in_game = false;

    // the world
    qc_entvars_t *w = QC_EV(0);
    w->model = QC_NewString(va("maps/%s.bsp", map));
    w->modelindex = 1;
    w->solid = Q1_SOLID_BSP;
    w->movetype = Q1_MOVETYPE_PUSH;

    // and the engine's world ("world" is g_local.h's macro for it)
    world->inuse = true;
    world->s.modelindex = 1;
    world->solid = SOLID_BSP;
    world->movetype = MOVETYPE_PUSH;
    world->classname = "worldspawn";
    gi.cvar_set("sv_gravity", "800");

    if (coop->value)
        qvm.g->coop = coop->value;
    else
        qvm.g->deathmatch = 0;
    qvm.g->teamplay = 0;
    qvm.g->mapname = QC_NewString(map);
    // serverflags are for cross level information (sigils)
    qvm.g->serverflags = q1_serverflags;
    qvm.g->world_ = 0;

    fld_items2 = q1_field("items2");

    QC_LoadEntities(entities);

    // the level's configstrings: Quake II's worldspawn doesn't run
    const char *message = QC_Localize(QC_Str(w->message));
    if (*message) {
        gi.configstring(CS_NAME, message);
        Q_strlcpy(level.level_name, message, sizeof(level.level_name));
    } else {
        Q_strlcpy(level.level_name, map, sizeof(level.level_name));
    }
    gi.configstring(CS_SKY, "unit1_");
    gi.configstring(CS_SKYROTATE, "0 0");
    gi.configstring(CS_SKYAXIS, "0 0 0");
    gi.configstring(CS_CDTRACK, va("%i", (int)w->sounds));
    gi.configstring(CS_MAXCLIENTS, va("%i", (int)maxclients->value));
    gi.configstring(CS_GAME_ID, GAME_ID_STRING);
    gi.configstring(CS_Q3RULES, "00");
    // no CS_STATUSBAR: the clients draw Quake's own (STAT_Q1_*), and load its
    // effects and pictures
    gi.configstring(CS_Q1VM, qvm.episode);

    // Quake II RTX's flare gun (THE FLARE GUN), loaded with the level
    flare_class_str = flare_model_str = 0;
    if (q1_flaregun_allowed()) {
        gi.modelindex(Q1_FLARE_VIEW);
        gi.modelindex(Q1_FLARE_MODEL);
    }

    gi.dprintf("Quake 1 game: %s (%s), %i entities\n", map, episode, qvm.num_edicts);
    return true;
}

/*
==============================================================================

PLAYERS

==============================================================================
*/

static void q1_copy_parms_out(int num)
{
    memcpy(q1c[num - 1].parms, qvm.g->parm, sizeof(qvm.g->parm));
}

/*
Quake's Host_Spawn_f: the player's entity made afresh, the parms it carries
handed to the progs, ClientConnect and PutClientInServer.
*/
void Q1VM_ClientBegin(edict_t *ent)
{
    int num = (int)(ent - g_edicts);
    q1_client_t *c = &q1c[num - 1];
    gclient_t *client = ent->client;
    short fov = client->ps.fov;

    // the engine's side
    ent->inuse = true;
    ent->s.number = num;
    ent->classname = "player";
    ent->svflags = 0;
    memset(&client->ps, 0, sizeof(client->ps));
    client->ps.fov = fov ? fov : 90;
    client->ps.pmove.pm_type = PM_NORMAL;
    memset(&client->old_pmove, 0, sizeof(client->old_pmove));

    // a savegame's player (Quake's Host_Spawn_f under sv.loadgame): the
    // entity is the save's, and the progs are not told
    if (c->loaded && !qvm.ed[num].free) {
        qc_entvars_t *ev = QC_EV(num);

        c->loaded = false;
        c->buttons = c->buttons_now = c->upmove = 0;
        c->dmg_time = c->cshift_damage = c->cshift_bonus = 0;
        c->layout_sent = 0;
        c->gun_string = -1;
        c->gun_index = 0;
        c->flare_held = false;
        c->flare_next = 0;

        // the view the save had: the client's own angles start at zero
        for (int i = 0; i < 3; i++) {
            client->ps.pmove.delta_angles[i] = ANGLE2SHORT(ev->v_angle[i]);
            c->cmd_angles[i] = 0;
        }
        VectorCopy(ev->v_angle, client->ps.viewangles);
        VectorCopy(ev->v_angle, client->v_angle);
        ev->fixangle = 0;

        c->in_game = true;
        Q1_LinkEdict(num, false);
        Q1VM_SyncEdict(num);
        return;
    }
    c->loaded = false;

    // a new player (or a new game) gets new parms
    if (!c->valid) {
        qvm.g->self = QC_ToProg(num);
        QC_Execute(qvm.g->SetNewParms);
        q1_copy_parms_out(num);
        c->valid = true;
    }
    memcpy(c->level_parms, c->parms, sizeof(c->parms));

    c->buttons = c->buttons_now = 0;
    c->dmg_time = c->cshift_damage = c->cshift_bonus = 0;
    c->layout_sent = 0;
    c->gun_string = -1;
    c->gun_index = 0;
    c->flare_held = false;      // the progs' weapon on every level
    c->flare_next = 0;

    // the client clears its view angles when it connects to a level, so the
    // spawn's fixangle (q1_fix_angles, before this level's first command)
    // turns from zero - not from the last command of the level before, which
    // faced every changelevel arrival off by the yaw it left with (11-21,
    // Matt: e1m1 from start faced the wall). Quake II's InitClientResp.
    for (int i = 0; i < 3; i++)
        c->cmd_angles[i] = 0;

    // set up the edict
    QC_ClearEdict(num);
    qc_entvars_t *ev = QC_EV(num);
    ev->colormap = num;
    ev->team = 1;
    ev->netname = QC_NewString(client->pers.netname);

    // copy spawn parms out of the client
    memcpy(qvm.g->parm, c->parms, sizeof(qvm.g->parm));

    // call the spawn function
    qvm.g->time = qvm.time;
    qvm.g->self = QC_ToProg(num);
    QC_Execute(qvm.g->ClientConnect);

    qvm.g->time = qvm.time;
    qvm.g->self = QC_ToProg(num);
    QC_Execute(qvm.g->PutClientInServer);

    c->in_game = true;
    Q1_LinkEdict(num, false);
    Q1VM_SyncEdict(num);
}

void Q1VM_ClientDisconnect(edict_t *ent)
{
    int num = (int)(ent - g_edicts);
    q1_client_t *c = &q1c[num - 1];

    if (c->in_game && !qvm.ed[num].free) {
        // this will set the body to a dead frame, among other things
        int save_self = qvm.g->self;
        qvm.g->time = qvm.time;
        qvm.g->self = QC_ToProg(num);
        QC_Execute(qvm.g->ClientDisconnect);
        qvm.g->self = save_self;
    }

    c->in_game = false;
    c->valid = false;       // the next to take the slot starts afresh

    gi.unlinkentity(ent);
    ent->s.modelindex = 0;
    ent->s.modelindex4 = 0;
    ent->s.sound = 0;
    ent->s.event = 0;
    ent->s.effects = 0;
    ent->s.renderfx = 0;
    ent->s.solid = 0;
    ent->solid = SOLID_NOT;
    ent->inuse = false;
    ent->classname = "disconnected";
    ent->client->pers.connected = false;
    qvm.ed[num].linked = false;
    qvm.ed[num].sent = false;
}

static edict_t *q1_pm_passent;

static trace_t q_gameabi q1_pm_trace(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end)
{
    int num = (int)(q1_pm_passent - g_edicts);
    int mask = QC_EV(num)->health > 0 ? MASK_PLAYERSOLID : MASK_DEADSOLID;
    return gi.trace(start, mins, maxs, end, q1_pm_passent, mask);
}

// the view as the progs set it (fixangle): a teleport, a spawn, intermission
static void q1_fix_angles(edict_t *ent, int num)
{
    qc_entvars_t *ev = QC_EV(num);
    q1_client_t *c = &q1c[num - 1];
    gclient_t *client = ent->client;

    if (!ev->fixangle)
        return;

    for (int i = 0; i < 3; i++)
        client->ps.pmove.delta_angles[i] = ANGLE2SHORT(ev->angles[i]) - c->cmd_angles[i];
    VectorCopy(ev->angles, ev->v_angle);
    VectorCopy(ev->angles, client->ps.viewangles);
    ev->fixangle = 0;
}

// Quake's V_CalcRoll: leaning into a strafe
static float q1_calc_roll(const vec3_t angles, const vec3_t velocity, float rollangle, float rollspeed)
{
    vec3_t forward, right, up;
    float side, sign;

    AngleVectors(angles, forward, right, up);
    side = DotProduct(velocity, right);
    sign = side < 0 ? -1 : 1;
    side = fabsf(side);

    if (side < rollspeed)
        side = side * rollangle / rollspeed;
    else
        side = rollangle;

    return side * sign;
}

/*
A command from the player: Quake II's pmove moves them (so the client can
predict it), then the result is the progs' - origin, velocity, ground, water -
and what they ran into and through is touched, as Quake's SV_Physics_Client
would have.
*/
void Q1VM_ClientThink(edict_t *ent, usercmd_t *ucmd)
{
    int num = (int)(ent - g_edicts);
    q1_client_t *c = &q1c[num - 1];
    gclient_t *client = ent->client;
    qc_entvars_t *ev = QC_EV(num);
    pmove_t pm;
    int i, j;

    c->buttons |= ucmd->buttons;
    c->buttons_now = ucmd->buttons;
    c->upmove = ucmd->upmove;
    for (i = 0; i < 3; i++)
        c->cmd_angles[i] = ucmd->angles[i];

    if (!Q1VM_ClientInGame(num) || qvm.ed[num].free)
        return;

    // the impulse waits until the progs take it (ImpulseCommands)
    if (ucmd->impulse) {
        ev->impulse = ucmd->impulse;
        q1_flare_weapon_impulse(num, ucmd->impulse);
    }

    q1_fix_angles(ent, num);

    if (qvm.intermission) {
        client->ps.pmove.pm_type = PM_FREEZE;
        return;
    }

    switch ((int)ev->movetype) {
    case Q1_MOVETYPE_NOCLIP:
    case Q1_MOVETYPE_FLY:
        client->ps.pmove.pm_type = PM_SPECTATOR;
        break;
    case Q1_MOVETYPE_WALK:
        client->ps.pmove.pm_type = ev->health > 0 ? PM_NORMAL : PM_DEAD;
        break;
    default:
        // NONE, or a body the progs' physics moves
        client->ps.pmove.pm_type = PM_FREEZE;
        break;
    }

    memset(&pm, 0, sizeof(pm));
    /* sv_gravity times the player's own .gravity, as Quake's SV_AddGravity:
       hipnotic's trigger_setgravity rooms (QC_Gravity). It goes out in the
       player state, so the client's prediction falls the same way. A short on
       the wire: clamped, for a mod's extreme values. */
    {
        const float g = sv_gravity->value * QC_Gravity(num);
        client->ps.pmove.gravity = (short)(g > 32767 ? 32767 : g < -32768 ? -32768 : g);
    }
    pm.s = client->ps.pmove;

    for (i = 0; i < 3; i++) {
        pm.s.origin[i] = COORD2SHORT(ev->origin[i]);
        pm.s.velocity[i] = COORD2SHORT(ev->velocity[i]);
    }

    if (memcmp(&client->old_pmove, &pm.s, sizeof(pm.s)))
        pm.snapinitial = true;

    pm.cmd = *ucmd;
    // Quake has no crouch: down only swims (the client's prediction does the same)
    if (pm.cmd.upmove < 0 && (pm.s.pm_flags & PMF_ON_GROUND))
        pm.cmd.upmove = 0;
    pm.trace = q1_pm_trace;
    pm.pointcontents = gi.pointcontents;
    q1_pm_passent = ent;

    gi.Pmove(&pm);

    if (client->ps.pmove.pm_type != PM_FREEZE) {
        for (i = 0; i < 3; i++) {
            ev->origin[i] = SHORT2COORD(pm.s.origin[i]);
            ev->velocity[i] = SHORT2COORD(pm.s.velocity[i]);
        }
    }

    // Quake played the jump from the progs (PlayerJump); pmove jumps now
    if ((~client->ps.pmove.pm_flags & pm.s.pm_flags & PMF_JUMP_HELD) && pm.waterlevel == 0)
        Q1_StartSound(num, CHAN_BODY, "player/plyrjmp8.wav", 1, ATTN_NORM);

    client->ps.pmove = pm.s;
    client->old_pmove = pm.s;

    // the progs' view of it
    VectorCopy(pm.viewangles, ev->v_angle);
    VectorCopy(pm.viewangles, client->v_angle);
    if (ev->health > 0) {
        // show 1/3 the pitch angle and all the roll angle (Quake's SV_ClientThink)
        vec3_t v_angle;
        VectorAdd(ev->v_angle, ev->punchangle, v_angle);
        ev->angles[ROLL] = q1_calc_roll(ev->angles, ev->velocity, 2, 200) * 4;
        ev->angles[PITCH] = -v_angle[PITCH] / 3;
        ev->angles[YAW] = v_angle[YAW];
    }

    if (pm.groundentity) {
        ev->flags = (int)ev->flags | Q1_FL_ONGROUND;
        ev->groundentity = QC_ToProg((int)(pm.groundentity - g_edicts));
    } else {
        ev->flags = (int)ev->flags & ~Q1_FL_ONGROUND;
    }
    Q1_CheckWater(num);

    // touch what it is in, then what it ran into
    Q1_LinkEdict(num, true);

    for (i = 0; i < pm.numtouch && !qvm.ed[num].free; i++) {
        edict_t *other = pm.touchents[i];
        for (j = 0; j < i; j++)
            if (pm.touchents[j] == other)
                break;
        if (j != i)
            continue;   // duplicated
        int onum = (int)(other - g_edicts);
        if (onum <= 0 || onum >= qvm.num_edicts || qvm.ed[onum].free)
            continue;
        Q1_Impact(num, onum);
    }
}

/*
==============================================================================

THE FLARE GUN

Quake II RTX's flare gun on Quake 1's levels (11-24, Matt: "the quake1 levels
should have the flare gun as an option"), under sv_flaregun as in Quake II.
F ("use flare gun") takes it out in place of the progs' weapon; F again, or
a Quake weapon key, puts it away. It is free, one flare every
g_q1_flaregun_delay seconds, since Quake has no grenades to spend.

The flare is Quake II's (fire_flaregun): 800 ups, spinning, stopped dead by
what it hits (q1_physics_toss), sparking (TE_FLARE) and burning for 60
seconds. It is the progs' entity (QC_Alloc), so it saves with the level, but
the progs never run it: no think, no touch, SOLID_NOT (bmodels only, as a
Quake trigger moves). q1_flares_frame keeps it, in fields the engine doesn't
read for a bounce:
  ltime = when it goes out
  teleport_time = the next spark
  armorvalue = the next burn sound

Campaign only: the bot match on a Quake map is Quake II's game, which has
its own flare gun.

==============================================================================
*/

#define FLARE_LIFE          60.0f
#define FLARE_SPARK         0.2f    // seconds between TE_FLAREs (flare_think)
#define FLARE_SOUND_LEN     15.0f   // weapons/flare.wav
#define FLARE_SOUND_GAP     14.9f   // the longest wait between its plays

// v_flareg's frames (Weapon_FlareGun), one a server frame
#define FLARE_FRAME_UP_LAST     8
#define FLARE_FRAME_FIRE        9
#define FLARE_FRAME_FIRE_LAST   13
#define FLARE_FRAME_IDLE        14
#define FLARE_FRAME_IDLE_LAST   49
#define FLARE_FRAME_DOWN        50
#define FLARE_FRAME_DOWN_LAST   53

static bool q1_flaregun_allowed(void)
{
    return sv_flaregun && sv_flaregun->integer > 0;
}

// a QC string made once a level (the heap is saved with it)
static int q1_flare_string(int *cache, const char *s)
{
    if (!*cache || strcmp(QC_Str(*cache), s))
        *cache = QC_NewString(s);
    return *cache;
}

bool Q1VM_IsFlare(int num)
{
    return !strcmp(QC_Str(QC_EV(num)->classname), Q1_FLARE_CLASS);
}

static void q1_flare_fire(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t forward, right, eye, start;

    if (qvm.num_edicts >= qvm.max_edicts - 8)
        return;     // QC_Alloc would end the game

    // Quake II's P_ProjectSource: 8 forward, 8 right, 8 below the eye, held
    // back by a wall in between
    AngleVectors(ev->v_angle, forward, right, NULL);
    VectorAdd(ev->origin, ev->view_ofs, eye);
    VectorMA(eye, 8, forward, start);
    VectorMA(start, 8, right, start);
    start[2] -= 8;
    q1trace_t tr = Q1_Move(eye, vec3_origin, vec3_origin, start, Q1_MOVE_NOMONSTERS, num);

    int f = QC_Alloc();
    qc_entvars_t *fv = QC_EV(f);
    fv->classname = q1_flare_string(&flare_class_str, Q1_FLARE_CLASS);
    fv->model = q1_flare_string(&flare_model_str, Q1_FLARE_MODEL);
    fv->modelindex = gi.modelindex(Q1_FLARE_MODEL);
    fv->movetype = Q1_MOVETYPE_BOUNCE;
    fv->solid = Q1_SOLID_NOT;
    fv->owner = QC_ToProg(num);
    VectorCopy(tr.endpos, fv->origin);
    VectorCopy(tr.endpos, fv->oldorigin);
    VectorScale(forward, 800, fv->velocity);
    VectorSet(fv->avelocity, 300, 300, 300);
    fv->ltime = qvm.time + FLARE_LIFE;
    fv->teleport_time = qvm.time + FLARE_SPARK;
    fv->armorvalue = 0;     // the first spark starts the burn
    Q1_LinkEdict(f, false);

    gi.WriteByte(svc_muzzleflash);
    gi.WriteShort(num);
    gi.WriteByte(MZ_FLARE);
    gi.multicast(ev->origin, MULTICAST_PVS);

    ev->punchangle[0] = -1;
}

/* When the burn sound has to start again, given that it starts now:
   flare_next_sound_frame's spacing, so that the last play ends with the
   flare instead of running on after it. */
static float q1_flare_next_sound(float now, float out)
{
    float until_last = out - now - FLARE_SOUND_LEN;

    if (until_last <= 0)
        return out + 1;     // this play outlasts the flare

    int restarts = (int)ceilf(until_last / FLARE_SOUND_GAP);
    return now + until_last / restarts;
}

// flare_sparks
static void q1_flare_spark(int f)
{
    qc_entvars_t *fv = QC_EV(f);
    bool sound = qvm.time >= fv->armorvalue;

    if (sound)
        fv->armorvalue = q1_flare_next_sound(qvm.time, fv->ltime);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_FLARE);
    gi.WriteShort(f);
    gi.WriteByte(sound);
    gi.WritePosition(fv->origin);
    if (VectorLength(fv->velocity) > 0) {
        vec3_t dir, up;
        vectoangles(fv->velocity, dir);
        AngleVectors(dir, NULL, NULL, up);
        gi.WriteDir(up);
    } else {
        gi.WriteDir(vec3_origin);
    }
    gi.multicast(fv->origin, MULTICAST_PVS);
}

// the flares' sparks and ends, once a frame after the physics
static void q1_flares_frame(void)
{
    for (int i = game.maxclients + 1; i < qvm.num_edicts; i++) {
        if (qvm.ed[i].free)
            continue;
        qc_entvars_t *fv = QC_EV(i);
        if ((int)fv->movetype != Q1_MOVETYPE_BOUNCE || !Q1VM_IsFlare(i))
            continue;
        if (qvm.time >= fv->ltime) {
            QC_Free(i);
            continue;
        }
        if (qvm.time >= fv->teleport_time) {
            fv->teleport_time = qvm.time + FLARE_SPARK;
            q1_flare_spark(i);
        }
    }
}

// F: out, or away again
static void q1_flare_use(edict_t *ent, int num)
{
    q1_client_t *c = &q1c[num - 1];

    if (!q1_flaregun_allowed()) {
        gi.cprintf(ent, PRINT_HIGH, "The flare gun is off (sv_flaregun 0)\n");
        return;
    }
    if (QC_EV(num)->health <= 0 || qvm.intermission)
        return;

    if (c->flare_held && c->flare_frame < FLARE_FRAME_DOWN) {
        c->flare_frame = FLARE_FRAME_DOWN;
    } else {
        c->flare_held = true;
        c->flare_frame = 0;
    }
}

// a Quake weapon key while it's out: put it away, the progs' weapon after
static void q1_flare_weapon_impulse(int num, int impulse)
{
    q1_client_t *c = &q1c[num - 1];

    if ((impulse >= 1 && impulse <= 8) || impulse == 10 || impulse == 12)
        if (c->flare_held && c->flare_frame < FLARE_FRAME_DOWN)
            c->flare_frame = FLARE_FRAME_DOWN;
}

// Weapon_Generic, for one gun with no ammo: up, fire, idle, down
static void q1_flare_think(int num)
{
    q1_client_t *c = &q1c[num - 1];
    qc_entvars_t *ev = QC_EV(num);

    if (!c->flare_held)
        return;
    if (!q1_flaregun_allowed() || ev->health <= 0 || qvm.intermission) {
        c->flare_held = false;
        return;
    }

    int f = c->flare_frame;
    if (f >= FLARE_FRAME_DOWN) {
        if (++f > FLARE_FRAME_DOWN_LAST) {
            c->flare_held = false;
            return;
        }
    } else if (f < FLARE_FRAME_UP_LAST || (f >= FLARE_FRAME_FIRE && f < FLARE_FRAME_FIRE_LAST)) {
        f++;
    } else if ((c->buttons & BUTTON_ATTACK) && qvm.time >= c->flare_next) {
        // ready: up, fired or idling
        f = FLARE_FRAME_FIRE;
        c->flare_next = qvm.time + max(g_q1_flaregun_delay->value, 0.0f);
        q1_flare_fire(num);
    } else if (f < FLARE_FRAME_IDLE || f >= FLARE_FRAME_IDLE_LAST) {
        f = FLARE_FRAME_IDLE;
    } else {
        f++;
    }
    c->flare_frame = f;
}

/*
==============================================================================

COMMANDS

==============================================================================
*/

// Quake II's weapon names, as Quake 1's weapon keys
static int q1_weapon_impulse(const char *s)
{
    static const struct {
        const char  *name;
        int         impulse;
    } weapons[] = {
        { "blaster", 1 }, { "shotgun", 2 }, { "super shotgun", 3 },
        { "machinegun", 4 }, { "chaingun", 5 }, { "grenade launcher", 6 },
        { "grenades", 6 }, { "rocket launcher", 7 }, { "hyperblaster", 8 },
    };

    if (s[0] >= '1' && s[0] <= '8' && !s[1])
        return s[0] - '0';
    for (int i = 0; i < q_countof(weapons); i++)
        if (!Q_stricmp(s, weapons[i].name))
            return weapons[i].impulse;
    return 0;
}

// "sv q1head x y z [vx vy vz] [model]" (Q1_ThrowTestHead)
bool Q1VM_ServerCommand(const char *cmd)
{
    // sv q1gib x y z [count]: the progs' own ThrowGib, count times, from a
    // stand-in body there (health -60) - to see what becomes of its gibs
    if (!Q_stricmp(cmd, "q1gib")) {
        qc_function_t *f = qvm.active ? QC_FindFunction("ThrowGib") : NULL;
        if (!f || gi.argc() < 5) {
            gi.cprintf(NULL, PRINT_HIGH, "usage (on a Quake 1 level the progs run): sv q1gib x y z [count]\n");
            return true;
        }
        static const char *const gibs[3] = { "progs/gib1.mdl", "progs/gib2.mdl", "progs/gib3.mdl" };
        const int count = gi.argc() > 5 ? atoi(gi.argv(5)) : 3;
        const int body = QC_Alloc();
        const int save_self = qvm.g->self;
        qc_entvars_t *ev = QC_EV(body);
        for (int k = 0; k < 3; k++)
            ev->origin[k] = atof(gi.argv(2 + k));
        ev->health = -60;
        for (int i = 0; i < count; i++) {
            qvm.g->time = qvm.time;
            qvm.g->self = QC_ToProg(body);
            QC_G_INT(QC_OFS_PARM0) = QC_EngineString(gibs[i % 3]);
            QC_G_FLOAT(QC_OFS_PARM1) = -60;
            QC_Execute((int)(f - qvm.functions));
        }
        qvm.g->self = save_self;
        QC_Free(body);
        gi.cprintf(NULL, PRINT_HIGH, "q1gib: %d thrown\n", count);
        return true;
    }

    if (Q_stricmp(cmd, "q1head"))
        return false;
    if (!qvm.active || gi.argc() < 5) {
        gi.cprintf(NULL, PRINT_HIGH, "usage (on a Quake 1 level the progs run): sv q1head x y z [vx vy vz] [model]\n");
        return true;
    }
    vec3_t org, vel = { 0, 0, 0 };
    for (int k = 0; k < 3; k++) {
        org[k] = atof(gi.argv(2 + k));
        if (gi.argc() > 7)
            vel[k] = atof(gi.argv(5 + k));
    }
    const char *model = gi.argc() > 8 ? gi.argv(8) : "progs/h_ogre.mdl";
    int n = Q1_ThrowTestHead(org, vel, model);
    gi.cprintf(NULL, PRINT_HIGH, n ? "q1head: entity %d\n" : "q1head: no head (%d)\n", n);
    return true;
}

bool Q1VM_ClientCommand(edict_t *ent)
{
    int num = (int)(ent - g_edicts);
    const char *cmd = gi.argv(0);

    // talk and the player list are Quake II's
    if (!Q_stricmp(cmd, "say") || !Q_stricmp(cmd, "say_team") || !Q_stricmp(cmd, "players"))
        return false;

    if (!Q1VM_ClientInGame(num) || qvm.ed[num].free)
        return true;

    qc_entvars_t *ev = QC_EV(num);

    if (!Q_stricmp(cmd, "use")) {
        const char *s = gi.args();
        if (!Q_stricmp(s, "flare gun") || !Q_stricmp(s, "flaregun")) {
            q1_flare_use(ent, num);
        } else {
            int imp = q1_weapon_impulse(s);
            if (imp)
                ev->impulse = imp;
            q1_flare_weapon_impulse(num, imp);
        }
    } else if (!Q_stricmp(cmd, "weapnext")) {
        ev->impulse = 10;
        q1_flare_weapon_impulse(num, 10);
    } else if (!Q_stricmp(cmd, "weapprev")) {
        ev->impulse = 12;
        q1_flare_weapon_impulse(num, 12);
    } else if (!Q_stricmp(cmd, "impulse")) {
        ev->impulse = atoi(gi.argv(1));
        q1_flare_weapon_impulse(num, (int)ev->impulse);
    } else if (!Q_stricmp(cmd, "kill")) {
        // Quake's Host_Kill_f
        if (ev->health <= 0) {
            gi.cprintf(ent, PRINT_HIGH, "Can't suicide -- already dead!\n");
        } else {
            qvm.g->time = qvm.time;
            qvm.g->self = QC_ToProg(num);
            QC_Execute(qvm.g->ClientKill);
        }
    } else if (!Q_stricmp(cmd, "god")) {
        ev->flags = (int)ev->flags ^ Q1_FL_GODMODE;
        gi.cprintf(ent, PRINT_HIGH, ((int)ev->flags & Q1_FL_GODMODE) ? "godmode ON\n" : "godmode OFF\n");
    } else if (!Q_stricmp(cmd, "notarget")) {
        ev->flags = (int)ev->flags ^ Q1_FL_NOTARGET;
        gi.cprintf(ent, PRINT_HIGH, ((int)ev->flags & Q1_FL_NOTARGET) ? "notarget ON\n" : "notarget OFF\n");
    } else if (!Q_stricmp(cmd, "noclip")) {
        if ((int)ev->movetype != Q1_MOVETYPE_NOCLIP) {
            ev->movetype = Q1_MOVETYPE_NOCLIP;
            gi.cprintf(ent, PRINT_HIGH, "noclip ON\n");
        } else {
            ev->movetype = Q1_MOVETYPE_WALK;
            gi.cprintf(ent, PRINT_HIGH, "noclip OFF\n");
        }
    } else if (!Q_stricmp(cmd, "give")) {
        // the progs' own cheat: every weapon and ammo
        ev->impulse = 9;
    } else if (!Q_stricmp(cmd, "inven")) {
        // Quake held Tab for the scoreboard (+showscores): the client sends
        // this as the inventory key goes down, putaway as it comes up
        q1c[num - 1].show_scores = true;
    } else if (!Q_stricmp(cmd, "score") || !Q_stricmp(cmd, "help")) {
        q1c[num - 1].show_scores ^= true;
    } else if (!Q_stricmp(cmd, "putaway")) {
        q1c[num - 1].show_scores = false;
    }
    // the rest (drop, invuse...) are Quake II things

    return true;
}

/*
==============================================================================

THE FRAME

==============================================================================
*/

// Quake's svc_damage, read as its client did: a red flash and a kick away
static void q1_take_damage(int num)
{
    qc_entvars_t *ev = QC_EV(num);
    q1_client_t *c = &q1c[num - 1];
    float armor = ev->dmg_save, blood = ev->dmg_take;
    int other = QC_FromProg(ev->dmg_inflictor);
    vec3_t from, forward, right, up;

    float count = blood * 0.5f + armor * 0.5f;
    if (count < 10)
        count = 10;

    // V_ParseDamage's cl.faceanimtime: the face flinches for this and nothing
    // else - not the megahealth's rot, which takes health without T_Damage
    c->hits++;

    c->cshift_damage += 3 * count;
    clamp(c->cshift_damage, 0, 150);

    if (armor > blood)
        VectorSet(c->damage_color, 200, 100, 100);
    else if (armor)
        VectorSet(c->damage_color, 220, 50, 50);
    else
        VectorSet(c->damage_color, 255, 0, 0);

    // calculate view angle kicks
    qc_entvars_t *ov = QC_EV(other >= 0 && other < qvm.num_edicts ? other : 0);
    for (int i = 0; i < 3; i++)
        from[i] = ov->origin[i] + 0.5f * (ov->mins[i] + ov->maxs[i]) - ev->origin[i];
    VectorNormalize(from);

    AngleVectors(ev->angles, forward, right, up);
    c->dmg_roll = count * DotProduct(from, right) * 0.6f;
    c->dmg_pitch = count * DotProduct(from, forward) * 0.6f;
    c->dmg_time = 0.5f;

    ev->dmg_take = 0;
    ev->dmg_save = 0;
}

static void q1_add_cshift(float *blend, float r, float g, float b, float percent)
{
    float a2 = percent / 255.0f;
    if (a2 <= 0)
        return;

    blend[3] = blend[3] + a2 * (1 - blend[3]);
    a2 = a2 / blend[3];
    blend[0] = blend[0] * (1 - a2) + r / 255.0f * a2;
    blend[1] = blend[1] * (1 - a2) + g / 255.0f * a2;
    blend[2] = blend[2] * (1 - a2) + b / 255.0f * a2;
}

static void q1_send_layout(edict_t *ent, const char *layout)
{
    gi.WriteByte(svc_layout);
    gi.WriteString(layout);
    gi.unicast(ent, true);
}

// the text, a centred line each ("" lines stay as gaps). Only the first
// "chars" characters show, as Quake typed a finale out; a line still typing
// is padded with spaces so it sits where it will end up.
static void q1_layout_text(char *out, size_t size, int y, const char *text, int chars)
{
    char line[64];
    size_t n = 0, shown = 0;

    for (const char *s = text;; s++) {
        if (*s == '\n' || !*s || n >= sizeof(line) - 1) {
            line[n] = 0;
            if (shown)
                Q_strlcat(out, va("yv %d cstring \"%s\" ", y, line), size);
            y += 8;     // Quake's lines touched
            n = shown = 0;
            if (!*s)
                break;
            if (*s != '\n') {
                line[n++] = *s;
                if (chars-- > 0)
                    shown = n;
                else
                    line[n - 1] = ' ';
            }
            continue;
        }
        line[n++] = (*s == '"') ? '\'' : *s;
        if (chars-- > 0)
            shown = n;
        else
            line[n - 1] = ' ';
    }
}

static int q1_finale_total(void)
{
    int total = 0;

    for (const char *s = qvm.finale; *s; s++)
        if (*s != '\n')
            total++;
    return total;
}

// Quake's scr_printspeed: the finale's characters (not its newlines) at 8 a second
static int q1_finale_chars(void)
{
    int total = q1_finale_total();
    float typed = 8 * (qvm.time - qvm.finale_time);
    return typed >= total ? total : (int)typed;
}

/*
The rerelease's ex_finaleFinished: the end of a game (finale_5 on end.bsp,
the mission packs' finale_check) waits on it before the credits. True once
the text has typed out and a player has pressed fire, jump or use since -
a button already held when it finished has to be let go first.
*/
float Q1VM_FinaleFinished(void)
{
    if (qvm.intermission < 2 || q1_finale_chars() < q1_finale_total())
        return 0;

    for (int i = 0; i < game.maxclients; i++) {
        q1_client_t *c = &q1c[i];
        if (!c->in_game || qvm.ed[i + 1].free)
            continue;
        // what the frame latched, so a quick tap between polls still counts
        const qc_entvars_t *ev = QC_EV(i + 1);
        bool held = ev->button0 || ev->button1 || ev->button2;
        if (!held)
            c->finale_armed = true;
        else if (c->finale_armed)
            return 1;
    }
    return 0;
}

static void q1_intermission_layout(edict_t *ent, int chars)
{
    char layout[1400];

    // the intermission is Quake's own overlay, the client's (STAT_Q1_*): the
    // finale's and a cutscene's text come this way
    layout[0] = 0;
    if (qvm.intermission != 1) {
        // SCR_DrawCenterString's place on a 320x200 screen - 35% of the way
        // down, or at 48 when it has more than 4 lines - and a layout's yv
        // counts from 20 above that screen
        int lines = 1;
        for (const char *s = qvm.finale; *s; s++)
            if (*s == '\n')
                lines++;
        Q_strlcpy(layout, "xv 0 ", sizeof(layout));
        q1_layout_text(layout, sizeof(layout), 20 + (lines <= 4 ? 70 : 48), qvm.finale, chars);
    }
    q1_send_layout(ent, layout);
}

/*
What Quake's status bar read - SV_WriteClientdataToMessage's stats and items,
the sigils (or the mission packs' items2) in the items' high bits - for the
client's copy of sbar.c, which also draws the intermission from it.
*/
static void q1_hud_stats(player_state_t *ps, int num)
{
    const qc_entvars_t *ev = QC_EV(num);
    uint32_t items = (uint32_t)(int)ev->items;
    const uint32_t weapon = (uint32_t)(int)ev->weapon;

    if (fld_items2 >= 0)
        items |= (uint32_t)(int)q1_field_float(num, fld_items2) << 23;
    else
        items |= (uint32_t)(int)qvm.g->serverflags << 28;

    ps->stats[STAT_HEALTH] = (short)ev->health;
    ps->stats[STAT_ARMOR] = (short)ev->armorvalue;
    ps->stats[STAT_AMMO] = (short)ev->currentammo;
    ps->stats[STAT_FRAGS] = (short)ev->frags;

    ps->stats[STAT_Q1_ITEMS_LO] = (short)(items & 0xffff);
    ps->stats[STAT_Q1_ITEMS_HI] = (short)(items >> 16);
    ps->stats[STAT_Q1_WEAPON_LO] = (short)(weapon & 0xffff);
    ps->stats[STAT_Q1_WEAPON_HI] = (short)(weapon >> 16);
    ps->stats[STAT_Q1_SHELLS] = (short)ev->ammo_shells;
    ps->stats[STAT_Q1_NAILS] = (short)ev->ammo_nails;
    ps->stats[STAT_Q1_ROCKETS] = (short)ev->ammo_rockets;
    ps->stats[STAT_Q1_CELLS] = (short)ev->ammo_cells;

    ps->stats[STAT_Q1_MONSTERS] = (short)qvm.g->killed_monsters;
    ps->stats[STAT_Q1_TOTALMONSTERS] = (short)qvm.g->total_monsters;
    ps->stats[STAT_Q1_SECRETS] = (short)qvm.g->found_secrets;
    ps->stats[STAT_Q1_TOTALSECRETS] = (short)qvm.g->total_secrets;
    ps->stats[STAT_Q1_TIME] = (short)(qvm.intermission ? qvm.intermission_time : qvm.time - 1);
    ps->stats[STAT_Q1_INTERMISSION] = (short)qvm.intermission;
    if (num >= 1 && num <= game.maxclients && q1c[num - 1].show_scores)
        ps->stats[STAT_Q1_INTERMISSION] |= STAT_Q1_SHOWSCORES;
    if (num >= 1 && num <= game.maxclients)
        ps->stats[STAT_Q1_INTERMISSION] |= (short)((q1c[num - 1].hits * STAT_Q1_HIT) & STAT_Q1_HITS);
}

static int q1_gun_index(q1_client_t *c, qc_entvars_t *ev)
{
    if (ev->weaponmodel != c->gun_string) {
        c->gun_string = ev->weaponmodel;
        c->gun_index = Q1_ModelIndex(QC_Str(ev->weaponmodel));
    }
    return c->gun_index;
}

/*
The player's view and HUD for the frame: Quake's view.c (bob, roll, kicks,
the colour shifts) and status bar, in Quake II's player_state_t.
*/
static void q1_client_end_frame(edict_t *ent)
{
    int num = (int)(ent - g_edicts);
    q1_client_t *c = &q1c[num - 1];
    gclient_t *client = ent->client;
    qc_entvars_t *ev = QC_EV(num);
    player_state_t *ps = &client->ps;
    vec3_t forward, right, up;
    int i;

    // pushed by a plat or kicked by an explosion since ClientThink: the
    // client's prediction starts from here
    for (i = 0; i < 3; i++) {
        ps->pmove.origin[i] = COORD2SHORT(ev->origin[i]);
        ps->pmove.velocity[i] = COORD2SHORT(ev->velocity[i]);
    }

    q1_fix_angles(ent, num);
    VectorCopy(ev->v_angle, ps->viewangles);

    if (ev->dmg_take || ev->dmg_save)
        q1_take_damage(num);

    // drop the punch angle (Quake's DropPunchAngle, once a frame)
    float len = VectorNormalize(ev->punchangle);
    len -= 10 * FRAMETIME;
    if (len < 0)
        len = 0;
    VectorScale(ev->punchangle, len, ev->punchangle);

    memset(ps->stats, 0, sizeof(ps->stats));
    VectorClear(ps->kick_angles);
    VectorClear(ps->gunoffset);
    VectorClear(ps->gunangles);
    Vector4Clear(ps->blend);
    ps->rdflags = 0;

    q1_hud_stats(ps, num);

    if (qvm.intermission) {
        // Quake's V_CalcIntermissionRefdef: the eye at the spot, no gun
        VectorClear(ps->viewoffset);
        ps->gunindex = 0;
        ps->pmove.pm_type = PM_FREEZE;
        int chars = qvm.intermission >= 2 ? q1_finale_chars() : 0;
        if (c->layout_sent != qvm.intermission || c->layout_chars != chars) {
            c->layout_sent = qvm.intermission;
            c->layout_chars = chars;
            q1_intermission_layout(ent, chars);
        }
        ps->stats[STAT_LAYOUTS] = 1;
        return;
    }
    if (c->layout_sent) {
        c->layout_sent = 0;
        q1_send_layout(ent, "");
    }

    // the view: the eyes, bobbing as Quake's V_CalcBob did
    float bob = 0;
    if (ev->health > 0 && ((int)ev->flags & Q1_FL_ONGROUND)) {
        const float bobcycle = 0.6f, bobup = 0.5f;
        float cycle = level.time - (int)(level.time / bobcycle) * bobcycle;
        cycle /= bobcycle;
        if (cycle < bobup)
            cycle = M_PI * cycle / bobup;
        else
            cycle = M_PI + M_PI * (cycle - bobup) / (1.0f - bobup);
        bob = sqrtf(ev->velocity[0] * ev->velocity[0] + ev->velocity[1] * ev->velocity[1]) * 0.02f;
        bob = bob * 0.3f + bob * 0.7f * sinf(cycle);
        clamp(bob, -7, 4);
    }
    VectorCopy(ev->view_ofs, ps->viewoffset);
    ps->viewoffset[2] += bob;

    if (ev->health <= 0)
        ps->viewangles[ROLL] = 80;  // dead view angle

    // kicks: the punch, damage, leaning into a strafe
    VectorCopy(ev->punchangle, ps->kick_angles);
    if (c->dmg_time > 0) {
        ps->kick_angles[ROLL] += c->dmg_time / 0.5f * c->dmg_roll;
        ps->kick_angles[PITCH] += c->dmg_time / 0.5f * c->dmg_pitch;
        c->dmg_time -= FRAMETIME;
    }
    if (ev->health > 0)
        ps->kick_angles[ROLL] += q1_calc_roll(ps->viewangles, ev->velocity, 2, 200);

    // the gun, where Quake held it, or Quake II RTX's flare gun
    if (c->flare_held && ev->health > 0) {
        ps->gunindex = gi.modelindex(Q1_FLARE_VIEW);
        ps->gunframe = c->flare_frame;
    } else {
        ps->gunindex = ev->health > 0 ? q1_gun_index(c, ev) : 0;
        ps->gunframe = (int)ev->weaponframe;
    }
    AngleVectors(ps->viewangles, forward, right, up);
    VectorScale(forward, bob * 0.4f, ps->gunoffset);
    ps->gunoffset[2] += 2;

    // the colour shifts (Quake's V_CalcBlend): contents, damage, bonus, powerups
    vec3_t eye;
    VectorAdd(ev->origin, ev->view_ofs, eye);
    switch (Q1_PointContents(eye)) {
    case Q1_CONTENTS_LAVA:
        q1_add_cshift(ps->blend, 255, 80, 0, 150);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    case Q1_CONTENTS_SLIME:
        q1_add_cshift(ps->blend, 0, 25, 5, 150);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    case Q1_CONTENTS_WATER:
        q1_add_cshift(ps->blend, 130, 80, 50, 128);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    default:
        break;
    }

    q1_add_cshift(ps->blend, c->damage_color[0], c->damage_color[1], c->damage_color[2], c->cshift_damage);
    q1_add_cshift(ps->blend, 215, 186, 69, c->cshift_bonus);

    const int items = (int)ev->items;
    if (items & Q1_IT_QUAD)
        q1_add_cshift(ps->blend, 0, 0, 255, 30);
    else if (items & Q1_IT_SUIT)
        q1_add_cshift(ps->blend, 0, 255, 0, 20);
    else if (items & Q1_IT_INVISIBILITY)
        q1_add_cshift(ps->blend, 100, 100, 100, 100);
    else if (items & Q1_IT_INVULNERABILITY)
        q1_add_cshift(ps->blend, 255, 255, 0, 30);

    for (i = 0; i < 4; i++)
        clamp(ps->blend[i], 0, 1);

    c->cshift_damage -= FRAMETIME * 150;
    if (c->cshift_damage < 0)
        c->cshift_damage = 0;
    c->cshift_bonus -= FRAMETIME * 100;
    if (c->cshift_bonus < 0)
        c->cshift_bonus = 0;
}

void Q1VM_BonusFlash(int client)
{
    if (client >= 1 && client <= game.maxclients)
        q1c[client - 1].cshift_bonus = 50;
}

bool Q1_IsAliasModel(int modelindex)
{
    const char *name = Q1_ModelName(modelindex);
    size_t len = strlen(name);
    return len > 4 && !Q_stricmp(name + len - 4, ".mdl");
}

// a model's trail (its .mdl flags), Quake II's nearest
unsigned Q1_ModelTrailEffects(int modelindex)
{
    int fl = Q1_ModelFlags(modelindex);
    unsigned effects = 0;

    if (fl & Q1_MF_ROCKET)
        effects |= EF_ROCKET;
    if (fl & Q1_MF_GRENADE)
        effects |= EF_GRENADE;
    if (fl & (Q1_MF_GIB | Q1_MF_ZOMGIB))
        effects |= EF_GIB;
    if (fl & Q1_MF_ROTATE)
        effects |= EF_ROTATE;
    if (fl & Q1_MF_TRACER)
        effects |= EF_GREENGIB;
    if (fl & Q1_MF_TRACER2)
        effects |= EF_BLASTER;
    if (fl & Q1_MF_TRACER3)
        effects |= EF_TRACKER;
    return effects;
}

/*
An entity's state after the progs have run, onto its edict: what the clients
are sent.
*/
void Q1VM_SyncEdict(int num)
{
    if (num <= 0 || num >= qvm.max_edicts)
        return;     // the world's edict was made at spawn
    if (num <= game.maxclients && !Q1VM_ClientInGame(num))
        return;     // the engine's until the player joins

    edict_t *e = &g_edicts[num];
    qc_edict_t *ed = &qvm.ed[num];

    if (ed->free) {
        if (ed->isstatic) {
            // drawn as it was made, but a torch's flame group flickers on
            if (e->inuse && e->s.modelindex) {
                int frame = ed->qc_frame, skin = ed->qc_skin;
                Q1_AnimPick(e->s.modelindex, num, qvm.time, &frame, &skin);
                e->s.frame = frame;
                e->s.skinnum = skin;
            }
            return;
        }
        if (e->inuse && num > game.maxclients) {
            gi.unlinkentity(e);
            memset(&e->s, 0, sizeof(e->s));
            e->s.number = num;
            e->inuse = false;
            e->solid = SOLID_NOT;
            e->owner = NULL;
            e->linkcount = 0;
        }
        ed->linked = false;
        ed->sent = false;
        return;
    }

    qc_entvars_t *ev = QC_EV(num);
    int mi = (int)ev->modelindex;
    if (mi < 0 || mi >= MAX_MODELS)
        mi = 0;

    e->inuse = true;
    e->s.number = num;
    e->s.modelindex = mi;

    /* Quake sent only an entity with a model NAME (SV_WriteEntitiesToClient:
       "ignore ents without visible models", !pr_strings[ent->v.model]). A
       picked-up item is hidden that way - the progs set its model to "" and go
       SOLID_NOT, its modelindex left as it was (the regen puts the name back).
       Sent by its modelindex, every box stayed drawn after it was taken (11-21,
       Matt: "shotgun shells ... didn't pickup" - they had). Not sent at all,
       its modelindex kept for a brush model's clipping, as Quake's was. */
    if (mi && !*QC_Str(ev->model))
        e->svflags |= SVF_NOCLIENT;
    else
        e->svflags &= ~SVF_NOCLIENT;

    // a frame or skin group: the member Quake would draw now
    int frame = (int)ev->frame, skin = (int)ev->skin;
    ed->qc_frame = frame;
    ed->qc_skin = skin;
    Q1_AnimPick(mi, num, qvm.time, &frame, &skin);
    e->s.frame = frame;
    e->s.skinnum = skin;
    VectorCopy(ev->angles, e->s.angles);
    if (Q1_IsAliasModel(mi)) {
        // Quake drew its alias models pitched (and rolled) the other way
        e->s.angles[PITCH] = -e->s.angles[PITCH];
        e->s.angles[ROLL] = -e->s.angles[ROLL];
    }

    // the model's trail, Quake II's nearest
    e->s.effects = mi ? Q1_ModelTrailEffects(mi) : 0;
    e->s.renderfx = 0;

    // the entity's lights ride in modelindex4 (CS_Q1VM)
    int ef = (int)ev->effects;
    e->s.modelindex4 = ef & (Q1_EF_BRIGHTFIELD | Q1_EF_BRIGHTLIGHT | Q1_EF_DIMLIGHT |
                             Q1_EF_QUADLIGHT | Q1_EF_PENTALIGHT | Q1_EF_CANDLELIGHT);

    // a muzzle flash is for one frame
    if (ef & Q1_EF_MUZZLEFLASH) {
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_Q1);
        gi.WriteByte(TE_Q1_MUZZLEFLASH);
        gi.WriteShort(num);
        gi.multicast(ev->origin, MULTICAST_PVS);
        ev->effects = ef & ~Q1_EF_MUZZLEFLASH;
    }

    if (qvm.fld_alpha >= 0) {
        float a = QC_Field(num, qvm.fld_alpha)->_float;
        e->s.alpha = (a > 0 && a < 1) ? a : 0;
    }
    if (qvm.fld_scale >= 0) {
        float s = QC_Field(num, qvm.fld_scale)->_float;
        e->s.scale = s > 0 ? s : 0;
    }

    Q1_SyncLink(num);

    // Where it was when last synced, which a client lerps a NEW entity from
    // (Quake II's G_RunFrame keeps s.old_origin so); never synced, where it
    // is. Never set, a freed slot's state had been zeroed and the next entity
    // in it - a lava ball, say - came in sliding from the world's origin over
    // its first frame: models "flashing in in random places" and, with the
    // rocket's trail, fire puffing up out of nothing (Matt, 11-21).
    VectorCopy(ed->sent ? ed->sent_origin : e->s.origin, e->s.old_origin);

    /* A jump no velocity explains is a teleport: no sliding across the map.
       Quake's own clients called it one past 100 units on any axis in one
       update (FitzQuake CL_RelinkEntities: "assume a teleportation, not a
       motion"). This was 64 units in all, and a dog runs in 64-unit strides
       (dog_run5/run11, ai_run(64)) with no velocity - a walking monster moves
       by steps - so every stride over a stair or a slope, and some diagonals
       by rounding alone, went out as a teleport. The client drops a
       teleport's lerp, so position, angles and frame snapped twice a run
       cycle (Matt 11-24: the dog "jumps around", "warping"). Velocity keeps
       its own allowance, for missiles and falls. */
    if (ed->sent) {
        const float allow = 100 + VectorLength(ev->velocity) * 0.2f;
        for (int k = 0; k < 3; k++) {
            if (fabsf(ev->origin[k] - ed->sent_origin[k]) > allow) {
                e->s.event = EV_OTHER_TELEPORT;
                break;
            }
        }
    }
    VectorCopy(ev->origin, ed->sent_origin);
    ed->sent = true;
}

void Q1VM_RunFrame(void)
{
    int i;

    // the players' buttons since the last frame (Quake's SV_RunClients)
    for (i = 1; i <= game.maxclients; i++) {
        if (!Q1VM_ClientInGame(i) || qvm.ed[i].free)
            continue;
        q1_client_t *c = &q1c[i - 1];
        qc_entvars_t *ev = QC_EV(i);
        int b = c->buttons;

        // the flare gun, when it's out, has the attack button
        q1_flare_think(i);
        ev->button0 = ((b & BUTTON_ATTACK) && !c->flare_held) ? 1 : 0;
        ev->button1 = (b & BUTTON_USE) ? 1 : 0;
        // jumping is pmove's; jump still swims up, respawns and leaves
        // the intermission, which the progs do
        ev->button2 = (c->upmove > 0 && (ev->waterlevel >= 2 || ev->health <= 0 || qvm.intermission)) ? 1 : 0;

        c->buttons = c->buttons_now;
    }

    qvm.frametime = FRAMETIME;
    qvm.g->frametime = qvm.frametime;

    Q1_Physics();
    q1_flares_frame();
    Q1_MessagesFrame();

    for (i = 1; i < qvm.num_edicts; i++)
        Q1VM_SyncEdict(i);

    for (i = 1; i <= game.maxclients; i++) {
        if (Q1VM_ClientInGame(i) && !qvm.ed[i].free)
            q1_client_end_frame(&g_edicts[i]);
    }
}

/*
==============================================================================

LEVEL CHANGES

==============================================================================
*/

/*
Quake's changelevel, with SV_SaveSpawnparms first: every player's parms as
the progs want them carried (SetChangeParms), and the runes. A NULL map is
Quake's "restart": the same level with the parms it began with.
*/
void Q1VM_ChangeLevel(const char *map)
{
    char path[MAX_QPATH];

    if (map) {
        q1_serverflags = qvm.g->serverflags;

        for (int i = 1; i <= game.maxclients; i++) {
            if (!Q1VM_ClientInGame(i) || qvm.ed[i].free)
                continue;
            // call the progs to get the spawn parms to carry
            qvm.g->self = QC_ToProg(i);
            QC_Execute(qvm.g->SetChangeParms);
            q1_copy_parms_out(i);
            q1c[i - 1].valid = true;
        }

        // the same folder: "q1/" or "q1/<episode>/"
        const char *slash = strrchr(qvm.q2map, '/');
        int folder = slash ? (int)(slash - qvm.q2map + 1) : 0;
        Q_snprintf(path, sizeof(path), "%.*s%s", folder, qvm.q2map, map);
    } else {
        for (int i = 0; i < game.maxclients; i++)
            memcpy(q1c[i].parms, q1c[i].level_parms, sizeof(q1c[i].parms));
        Q_strlcpy(path, qvm.q2map, sizeof(path));
    }

    gi.AddCommandString(va("gamemap \"%s\"\n", path));
}

/*
Quake's svc_intermission (1), svc_finale (2) and svc_cutscene (3): the
players' screens. The progs have already put them at the camera.
*/
void Q1VM_Intermission(int kind, const char *text)
{
    if (kind == 1 && !qvm.intermission)
        qvm.intermission_time = qvm.time - 1;  // Quake's clock started at 1
    qvm.intermission = kind;
    if (text) {
        Q_strlcpy(qvm.finale, text, sizeof(qvm.finale));
        qvm.finale_time = qvm.time;
    }

    // the layouts go out again
    for (int i = 0; i < game.maxclients; i++) {
        q1c[i].layout_sent = 0;
        if (text)
            q1c[i].finale_armed = false;
    }
}

/*
==============================================================================

SAVEGAMES

Quake's Host_Savegame_f and Host_Loadgame_f, in Quake II's save flow. A load
spawns the map first (SpawnEntities: the progs' spawn functions precache the
same models and sounds in the same order and make the same static entities),
then ReadLevel lays the saved machine over it - the globals, every entity's
fields, the strings, the players' parms - as Quake did. Quake wrote fields by
name as text; the progs here are the same file, checked, so the bytes do.
The players are not put in the server again (ClientBegin): their entities
are the save's. Quake has no hubs, so a level left is never kept for a
return (server/save.c).

==============================================================================
*/

#define Q1VM_SAVE_MAGIC     MakeLittleLong('Q','1','V','M')
#define Q1VM_SAVE_VERSION   1

typedef struct {
    int     magic, version;

    // which progs, which level
    char    episode[16];
    char    q2map[MAX_QPATH];
    int     progs_crc, numglobals, numfielddefs, numfunctions, edict_size;
    int     strings_size, progs_strings, maxclients;

    // the machine
    int     heap_used, temp_next;
    int     num_edicts;
    float   time;
    int     intermission;
    float   intermission_time, finale_time;
    char    finale[1024];
    int     skill;
    int     lastcheck;
    float   lastchecktime;
    float   serverflags;
    int     num_statics;
} q1_save_header_t;

typedef struct {
    int     valid;
    float   parms[16], level_parms[16];
} q1_save_client_t;

static FILE *q1_save_file;

static void q1_save_write(const void *p, size_t n)
{
    if (n && fwrite(p, 1, n, q1_save_file) != n) {
        fclose(q1_save_file);
        gi.error("Quake 1 game: couldn't write the savegame");
    }
}

static void q1_save_read(void *p, size_t n)
{
    if (n && fread(p, 1, n, q1_save_file) != n) {
        fclose(q1_save_file);
        gi.error("Quake 1 game: the savegame is cut short");
    }
}

static void q1_save_header(q1_save_header_t *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = Q1VM_SAVE_MAGIC;
    h->version = Q1VM_SAVE_VERSION;
    Q_strlcpy(h->episode, qvm.episode, sizeof(h->episode));
    Q_strlcpy(h->q2map, qvm.q2map, sizeof(h->q2map));
    h->progs_crc = qvm.progs->crc;
    h->numglobals = qvm.progs->numglobals;
    h->numfielddefs = qvm.progs->numfielddefs;
    h->numfunctions = qvm.progs->numfunctions;
    h->edict_size = qvm.edict_size;
    h->strings_size = qvm.strings_size;
    h->progs_strings = qvm.progs_strings;
    h->maxclients = game.maxclients;
}

void Q1VM_WriteLevel(const char *filename)
{
    q1_save_header_t h;

    q1_save_header(&h);
    h.heap_used = qvm.heap_used;
    h.temp_next = qvm.temp_next;
    h.num_edicts = qvm.num_edicts;
    h.time = qvm.time;
    h.intermission = qvm.intermission;
    h.intermission_time = qvm.intermission_time;
    h.finale_time = qvm.finale_time;
    Q_strlcpy(h.finale, qvm.finale, sizeof(h.finale));
    h.skill = qvm.skill;
    h.lastcheck = qvm.lastcheck;
    h.lastchecktime = qvm.lastchecktime;
    h.serverflags = q1_serverflags;
    for (int i = game.maxclients + 1; i < qvm.num_edicts; i++)
        if (qvm.ed[i].free && qvm.ed[i].isstatic)
            h.num_statics++;

    q1_save_file = fopen(filename, "wb");
    if (!q1_save_file)
        gi.error("Couldn't open %s", filename);

    q1_save_write(&h, sizeof(h));
    q1_save_write(qvm.globals, sizeof(float) * h.numglobals);
    // the temporary ring and the heap: whatever a field or global points at
    q1_save_write(qvm.strings + qvm.progs_strings, qvm.heap_used - qvm.progs_strings);
    q1_save_write(qvm.ed, sizeof(qvm.ed[0]) * qvm.num_edicts);
    q1_save_write(qvm.fields, (size_t)qvm.edict_size * qvm.num_edicts);

    for (int i = 0; i < game.maxclients; i++) {
        q1_save_client_t c;
        memset(&c, 0, sizeof(c));
        c.valid = q1c[i].valid;
        memcpy(c.parms, q1c[i].parms, sizeof(c.parms));
        memcpy(c.level_parms, q1c[i].level_parms, sizeof(c.level_parms));
        q1_save_write(&c, sizeof(c));
    }

    // the static entities are drawn from their edicts alone
    for (int i = game.maxclients + 1; i < qvm.num_edicts; i++) {
        if (!(qvm.ed[i].free && qvm.ed[i].isstatic))
            continue;
        q1_save_write(&i, sizeof(i));
        q1_save_write(&g_edicts[i].s, sizeof(g_edicts[i].s));
    }

    if (fclose(q1_save_file))
        gi.error("Couldn't write %s", filename);
    q1_save_file = NULL;
}

void Q1VM_ReadLevel(const char *filename)
{
    q1_save_header_t h, want;
    int i;

    q1_save_file = fopen(filename, "rb");
    if (!q1_save_file)
        gi.error("Couldn't open %s", filename);

    q1_save_read(&h, sizeof(h));
    q1_save_header(&want);

    if (h.magic != Q1VM_SAVE_MAGIC || h.version != Q1VM_SAVE_VERSION) {
        fclose(q1_save_file);
        gi.error("Not a Quake 1 savegame (saved with g_q1_progs 0?)");
    }
    if (Q_stricmp(h.episode, want.episode) || Q_stricmp(h.q2map, want.q2map) ||
        h.progs_crc != want.progs_crc || h.numglobals != want.numglobals ||
        h.numfielddefs != want.numfielddefs || h.numfunctions != want.numfunctions ||
        h.edict_size != want.edict_size || h.strings_size != want.strings_size ||
        h.progs_strings != want.progs_strings || h.maxclients != want.maxclients) {
        fclose(q1_save_file);
        gi.error("The savegame is from another progs.dat or level");
    }
    if (h.heap_used < h.progs_strings || h.heap_used > h.strings_size ||
        h.temp_next < 0 || h.temp_next >= QC_TEMPSTRINGS ||
        h.num_edicts <= game.maxclients || h.num_edicts > qvm.max_edicts ||
        h.num_statics < 0 || h.num_statics > h.num_edicts) {
        fclose(q1_save_file);
        gi.error("The Quake 1 savegame is damaged");
    }

    // the machine as it was
    q1_save_read(qvm.globals, sizeof(float) * h.numglobals);
    q1_save_read(qvm.strings + qvm.progs_strings, h.heap_used - h.progs_strings);
    qvm.heap_used = h.heap_used;
    qvm.temp_next = h.temp_next;

    const int old_num = qvm.num_edicts;
    q1_save_read(qvm.ed, sizeof(qvm.ed[0]) * h.num_edicts);
    q1_save_read(qvm.fields, (size_t)qvm.edict_size * h.num_edicts);
    if (old_num > h.num_edicts) {
        memset(qvm.ed + h.num_edicts, 0, sizeof(qvm.ed[0]) * (old_num - h.num_edicts));
        memset(qvm.fields + (size_t)qvm.edict_size * h.num_edicts, 0,
               (size_t)qvm.edict_size * (old_num - h.num_edicts));
    }
    qvm.num_edicts = h.num_edicts;

    qvm.time = h.time;
    qvm.intermission = h.intermission;
    qvm.intermission_time = h.intermission_time;
    qvm.finale_time = h.finale_time;
    Q_strlcpy(qvm.finale, h.finale, sizeof(qvm.finale));
    qvm.skill = h.skill;
    qvm.lastcheck = h.lastcheck;
    qvm.lastchecktime = h.lastchecktime;
    qvm.changelevel_issued = false;
    qvm.sky_impact = false;
    q1_serverflags = h.serverflags;
    flare_class_str = flare_model_str = 0;     // the save's heap, looked up again

    for (i = 0; i < game.maxclients; i++) {
        q1_save_client_t c;
        q1_save_read(&c, sizeof(c));
        q1c[i].valid = c.valid != 0;
        memcpy(q1c[i].parms, c.parms, sizeof(c.parms));
        memcpy(q1c[i].level_parms, c.level_parms, sizeof(c.level_parms));
        // the player's entity is waiting for them (ClientBegin)
        q1c[i].in_game = false;
        q1c[i].loaded = !qvm.ed[i + 1].free;
        q1c[i].layout_sent = 0;
        q1c[i].show_scores = false;
    }

    // the engine's entities, made again: the server has unlinked them all
    // (SV_ClearWorld). The players' wait for them to join.
    const int top = max(old_num, max(h.num_edicts, globals.num_edicts));
    for (i = 1; i < top && i < game.maxentities; i++) {
        edict_t *e = &g_edicts[i];
        if (i <= game.maxclients) {
            e->inuse = false;
            e->s.modelindex = e->s.modelindex4 = 0;
            e->s.sound = e->s.event = 0;
            e->s.effects = 0;
            e->solid = SOLID_NOT;
            continue;
        }
        memset(e, 0, sizeof(*e));
        e->s.number = i;
    }
    globals.num_edicts = max(qvm.num_edicts, game.maxclients + 1);

    for (int n = 0; n < h.num_statics; n++) {
        int num;
        entity_state_t s;
        q1_save_read(&num, sizeof(num));
        q1_save_read(&s, sizeof(s));
        if (num <= game.maxclients || num >= qvm.num_edicts) {
            fclose(q1_save_file);
            gi.error("The Quake 1 savegame is damaged");
        }
        edict_t *e = &g_edicts[num];
        e->s = s;
        e->s.number = num;
        e->inuse = true;
        e->solid = SOLID_NOT;
        gi.linkentity(e);
    }

    fclose(q1_save_file);
    q1_save_file = NULL;

    for (i = 1; i < qvm.num_edicts; i++) {
        qvm.ed[i].linked = false;
        qvm.ed[i].sent = false;
    }
    for (i = game.maxclients + 1; i < qvm.num_edicts; i++)
        if (!qvm.ed[i].free)
            Q1VM_SyncEdict(i);

    qvm.g->time = qvm.time;
    gi.dprintf("Quake 1 game: %s loaded, %i entities\n", qvm.q1map, qvm.num_edicts);
}
