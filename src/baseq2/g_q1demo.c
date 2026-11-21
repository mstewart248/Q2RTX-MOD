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

/*
==============================================================================

QUAKE 1'S DEMOS (g_q1demo.c)

The .dem files the Quake 1 episodes ship - id1's demo1-3, hipnotic's
hipdemo1-4, rogue's demo1-2, dopa's demo1-3, every one NetQuake protocol 15 -
played back on their own maps, for the attract loop. The engine's
"q1demo id1/<episode>/<name>.dem" (server/commands.c) reads which map the
demo was recorded on, starts it as a one-player game with g_q1_demo set, and
this plays the demo in place of running the level: the recorded server
messages are read as Quake's client read them (cl_demo.c, cl_parse.c), and
what they describe goes onto the edicts and the player's view - the
entities, their sounds and effects, the light styles, the status bar - the
way the Quake 1 game (g_q1vm*.c) sends its own. No progs.dat runs and nothing
thinks; the models, sounds and HUD are the Quake 1 game's (Q1_ModelIndex,
Q1VM_SoundIndex, CS_Q1VM), so a demo looks as that episode plays.

TIME. Quake's client read the next message once its clock had passed the
last one's svc_time, and drew everything lerped between the last two
(CL_LerpPoint, CL_RelinkEntities). Here the clock moves a server frame at a
time, the messages up to it are read, and the entities and the view go out
lerped to it; the client lerps the rest of the way between frames. A sound
carries its offset into the frame, so it lands with the picture. The clock
starts when the player is in (ClientBegin), not when the map loads.

ENTITY NUMBERS. The recorded player (svc_setview) is the client's own edict,
1; Quake's entity n is edict n + maxclients; the static entities and the
ambient sounds (Quake's client-side fixtures) take the edicts after those.

The demo ends at its svc_disconnect (or at the end of the file, or a message
that can't be read): the server's nextserver runs - the attract loop's next
demo - or, with none, the server is killed and the menu comes back.

==============================================================================
*/

#include "g_q1vm.h"

#define Q1D_PROTOCOL        15
#define Q1D_MAX_MODELS      256
#define Q1D_MAX_SOUNDS      256
#define Q1D_MAX_ENTS        1024    // Quake's numbers; NetQuake's MAX_EDICTS was 600
#define Q1D_MAX_STATICS     512
#define Q1D_MAX_MESSAGE     65536

// Quake's server messages
enum {
    Q1D_SVC_NOP = 1,
    Q1D_SVC_DISCONNECT,
    Q1D_SVC_UPDATESTAT,
    Q1D_SVC_VERSION,
    Q1D_SVC_SETVIEW,
    Q1D_SVC_SOUND,
    Q1D_SVC_TIME,
    Q1D_SVC_PRINT,
    Q1D_SVC_STUFFTEXT,
    Q1D_SVC_SETANGLE,
    Q1D_SVC_SERVERINFO,
    Q1D_SVC_LIGHTSTYLE,
    Q1D_SVC_UPDATENAME,
    Q1D_SVC_UPDATEFRAGS,
    Q1D_SVC_CLIENTDATA,
    Q1D_SVC_STOPSOUND,
    Q1D_SVC_UPDATECOLORS,
    Q1D_SVC_PARTICLE,
    Q1D_SVC_DAMAGE,
    Q1D_SVC_SPAWNSTATIC,
    Q1D_SVC_SPAWNBINARY,
    Q1D_SVC_SPAWNBASELINE,
    Q1D_SVC_TEMP_ENTITY,
    Q1D_SVC_SETPAUSE,
    Q1D_SVC_SIGNONNUM,
    Q1D_SVC_CENTERPRINT,
    Q1D_SVC_KILLEDMONSTER,
    Q1D_SVC_FOUNDSECRET,
    Q1D_SVC_SPAWNSTATICSOUND,
    Q1D_SVC_INTERMISSION,
    Q1D_SVC_FINALE,
    Q1D_SVC_CDTRACK,
    Q1D_SVC_SELLSCREEN,
    Q1D_SVC_CUTSCENE
};

// svc_ fast update bits (the first byte has 128 set)
#define Q1D_U_MOREBITS      (1 << 0)
#define Q1D_U_ORIGIN1       (1 << 1)
#define Q1D_U_ORIGIN2       (1 << 2)
#define Q1D_U_ORIGIN3       (1 << 3)
#define Q1D_U_ANGLE2        (1 << 4)
#define Q1D_U_NOLERP        (1 << 5)
#define Q1D_U_FRAME         (1 << 6)
#define Q1D_U_ANGLE1        (1 << 8)
#define Q1D_U_ANGLE3        (1 << 9)
#define Q1D_U_MODEL         (1 << 10)
#define Q1D_U_COLORMAP      (1 << 11)
#define Q1D_U_SKIN          (1 << 12)
#define Q1D_U_EFFECTS       (1 << 13)
#define Q1D_U_LONGENTITY    (1 << 14)

// svc_clientdata bits
#define Q1D_SU_VIEWHEIGHT   (1 << 0)
#define Q1D_SU_IDEALPITCH   (1 << 1)
#define Q1D_SU_PUNCH1       (1 << 2)
#define Q1D_SU_VELOCITY1    (1 << 5)
#define Q1D_SU_ONGROUND     (1 << 10)
#define Q1D_SU_INWATER      (1 << 11)
#define Q1D_SU_WEAPONFRAME  (1 << 12)
#define Q1D_SU_ARMOR        (1 << 13)
#define Q1D_SU_WEAPON       (1 << 14)

// svc_sound
#define Q1D_SND_VOLUME      (1 << 0)
#define Q1D_SND_ATTENUATION (1 << 1)

// Quake's cl.stats
#define Q1D_STAT_HEALTH         0
#define Q1D_STAT_FRAGS          1
#define Q1D_STAT_WEAPON         2
#define Q1D_STAT_AMMO           3
#define Q1D_STAT_ARMOR          4
#define Q1D_STAT_WEAPONFRAME    5
#define Q1D_STAT_SHELLS         6
#define Q1D_STAT_ACTIVEWEAPON   10
#define Q1D_STAT_TOTALSECRETS   11
#define Q1D_STAT_TOTALMONSTERS  12
#define Q1D_STAT_SECRETS        13
#define Q1D_STAT_MONSTERS       14
#define Q1D_MAX_STATS           32

#define Q1D_DEFAULT_VIEWHEIGHT  22

// an entity as Quake's client kept it (cl_entities[])
typedef struct {
    // svc_spawnbaseline: what a message leaves out
    int     b_model, b_frame, b_skin;
    vec3_t  b_origin, b_angles;

    float   msgtime;            // the svc_time of the last message it was in
    vec3_t  msg_origins[2];     // [0] that message's, [1] the one before
    vec3_t  msg_angles[2];
    int     model, frame, skin, effects;
    bool    forcelink;          // don't lerp from [1]
    bool    jumped;             // moved over 100 units between two messages: a teleport
    bool    flash;              // EF_MUZZLEFLASH in a message since the last frame

    // as it went out
    bool    shown;
    vec3_t  sent_origin;
    int     sent_model;         // the modelindex its box was set for
} q1d_ent_t;

// a static entity: Quake's frame and skin, as a group animates on
typedef struct {
    int     num;
    int     modelindex, frame, skin;
} q1d_static_t;

static struct {
    bool        active;         // this level plays a demo
    bool        broken;         // it couldn't be read: over at the first frame
    bool        started;        // the player is in: the clock runs
    bool        finished;       // the next thing has been asked for
    char        path[MAX_QPATH];

    byte        *data;          // the whole file (TAG_LEVEL)
    int         size, pos;      // the next block
    int         forcetrack;     // the header's CD track, -1 for the map's

    // the message being read
    const byte  *msg;
    int         msglen, msgpos;
    bool        bad;
    bool        signon;         // still in the signon: prints and sounds don't go out

    float       time;           // the demo's clock (Quake's cl.time)
    float       mtime[2];       // the last two messages' svc_time
    vec3_t      mviewangles[2]; // and the view angles recorded with them
    bool        timed;          // an svc_time has been read

    int         viewentity;
    bool        standard_quake; // activeweapon is the item (id1); hipnotic and rogue send its bit number
    bool        serverinfo;
    char        mapname[MAX_QPATH];     // the demo's map, "e1m3"
    char        level_name[MAX_QPATH];
    int         model_map[Q1D_MAX_MODELS];
    int         sound_map[Q1D_MAX_SOUNDS];
    int         num_models, num_sounds;

    q1d_ent_t   *ents;          // Q1D_MAX_ENTS of them (TAG_LEVEL)
    int         max_ent;        // the highest number used
    q1d_static_t statics[Q1D_MAX_STATICS];
    int         num_statics;
    int         next_static;    // the edict for the next static entity or sound

    // the player: Quake's cl.stats and the rest of clientdata
    int         stats[Q1D_MAX_STATS];
    int         items;
    int         viewheight;
    vec3_t      punchangle;
    vec3_t      velocity;
    bool        onground;
    int         frags;
    int         intermission;
    float       completed_time;

    // the view (Quake's view.c)
    vec3_t      view_origin;    // the recorded player, lerped, as last sent
    float       dmg_time, dmg_roll, dmg_pitch;
    float       cshift_damage, cshift_bonus;
    vec3_t      damage_color;
    unsigned    hits;

    char        print[256];     // svc_print's text until a whole line is in
} q1d;

static cvar_t *g_q1_demo;

bool Q1Demo_Active(void)
{
    return q1d.active;
}

/*
==============================================================================

READING

==============================================================================
*/

static int q1d_byte(void)
{
    if (q1d.msgpos + 1 > q1d.msglen) {
        q1d.bad = true;
        return 0;
    }
    return q1d.msg[q1d.msgpos++];
}

static int q1d_char(void)
{
    return (int8_t)q1d_byte();
}

static int q1d_short(void)
{
    if (q1d.msgpos + 2 > q1d.msglen) {
        q1d.bad = true;
        return 0;
    }
    const byte *p = q1d.msg + q1d.msgpos;
    q1d.msgpos += 2;
    return (int16_t)(p[0] | (p[1] << 8));
}

static int q1d_long(void)
{
    if (q1d.msgpos + 4 > q1d.msglen) {
        q1d.bad = true;
        return 0;
    }
    const byte *p = q1d.msg + q1d.msgpos;
    q1d.msgpos += 4;
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static float q1d_float(void)
{
    union { int32_t i; float f; } u;
    u.i = q1d_long();
    return u.f;
}

static float q1d_coord(void)
{
    return q1d_short() * (1.0f / 8);
}

static float q1d_angle(void)
{
    return q1d_char() * (360.0f / 256);
}

static const char *q1d_string(void)
{
    static char buf[2048];
    size_t n = 0;

    while (1) {
        if (q1d.msgpos >= q1d.msglen) {
            q1d.bad = true;
            break;
        }
        int c = q1d.msg[q1d.msgpos++];
        if (!c)
            break;
        if (n < sizeof(buf) - 1)
            buf[n++] = c;
    }
    buf[n] = 0;
    return buf;
}

// the next block of the file: a message and the view angles Quake recorded
// with it (CL_GetMessage)
static bool q1d_next_block(void)
{
    if (q1d.pos + 16 > q1d.size)
        return false;

    const byte *p = q1d.data + q1d.pos;
    int len = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    if (len < 0 || len > Q1D_MAX_MESSAGE || q1d.pos + 16 + len > q1d.size)
        return false;

    VectorCopy(q1d.mviewangles[0], q1d.mviewangles[1]);
    for (int i = 0; i < 3; i++) {
        float f;
        memcpy(&f, p + 4 + 4 * i, 4);
        q1d.mviewangles[0][i] = LittleFloat(f);
    }

    q1d.msg = p + 16;
    q1d.msglen = len;
    q1d.msgpos = 0;
    q1d.bad = false;
    q1d.pos += 16 + len;
    return true;
}

// Quake's entity number as an edict's
static int q1d_edict_num(int n)
{
    if (n <= 0)
        return 0;
    if (n == q1d.viewentity)
        return 1;
    return n + game.maxclients;
}

static int q1d_model(int index)
{
    return (index > 0 && index < q1d.num_models) ? q1d.model_map[index] : 0;
}

/*
==============================================================================

THE MESSAGES

==============================================================================
*/

static void q1d_parse_serverinfo(void)
{
    int protocol = q1d_long();
    q1d_byte();     // maxclients
    q1d_byte();     // gametype
    Q_strlcpy(q1d.level_name, q1d_string(), sizeof(q1d.level_name));

    if (protocol != Q1D_PROTOCOL) {
        gi.dprintf("Quake 1 demo %s: protocol %d, not %d\n", q1d.path, protocol, Q1D_PROTOCOL);
        q1d.bad = true;
        return;
    }

    // a second level in the demo: it can't go on (the map is this one)
    if (q1d.serverinfo) {
        q1d.bad = true;
        return;
    }
    q1d.serverinfo = true;

    // the models: Quake's index 1 is the world, then its inline models, then
    // the rest - each as the Quake 1 game names it
    q1d.num_models = 1;
    while (1) {
        const char *s = q1d_string();
        if (!*s || q1d.bad)
            break;
        if (q1d.num_models >= Q1D_MAX_MODELS) {
            q1d.bad = true;
            return;
        }
        int i = q1d.num_models++;
        if (i == 1) {
            // "maps/e1m3.bsp": the server already has it
            char map[MAX_QPATH];
            COM_StripExtension(map, COM_SkipPath(s), sizeof(map));
            Q_strlcpy(q1d.mapname, map, sizeof(q1d.mapname));
            q1d.model_map[i] = 1;
        } else {
            q1d.model_map[i] = Q1_ModelIndex(s);
        }
    }

    q1d.num_sounds = 1;
    while (1) {
        const char *s = q1d_string();
        if (!*s || q1d.bad)
            break;
        if (q1d.num_sounds >= Q1D_MAX_SOUNDS) {
            q1d.bad = true;
            return;
        }
        int i = q1d.num_sounds++;
        q1d.sound_map[i] = Q1VM_SoundIndex(s);
    }
}

// svc_spawnbaseline's and svc_spawnstatic's fields
static void q1d_parse_baseline(int *model, int *frame, int *skin, vec3_t origin, vec3_t angles)
{
    *model = q1d_byte();
    *frame = q1d_byte();
    q1d_byte();     // colormap
    *skin = q1d_byte();
    for (int i = 0; i < 3; i++) {
        origin[i] = q1d_coord();
        angles[i] = q1d_angle();
    }
}

static void q1d_note_entity(int n)
{
    if (n > q1d.max_ent) {
        q1d.max_ent = n;
        int num = q1d_edict_num(n);
        if (globals.num_edicts <= num)
            globals.num_edicts = num + 1;
    }
}

// an edict for a static entity or sound, after the entities'
static edict_t *q1d_new_static(void)
{
    if (q1d.next_static >= game.maxentities) {
        q1d.bad = true;
        return NULL;
    }
    int num = q1d.next_static++;
    edict_t *e = &g_edicts[num];
    memset(e, 0, sizeof(*e));
    e->s.number = num;
    e->inuse = true;
    e->solid = SOLID_NOT;
    e->classname = "q1demo_static";
    if (globals.num_edicts <= num)
        globals.num_edicts = num + 1;
    return e;
}

// an entity's box, for the server's PVS: an inline model's own, else
// Quake's 32 unit cube
static void q1d_set_box(edict_t *e, int modelindex)
{
    const char *name = Q1_ModelName(modelindex);

    if (name[0] == '*') {
        gi.setmodel(e, (char *)name);
    } else {
        VectorSet(e->mins, -16, -16, -16);
        VectorSet(e->maxs, 16, 16, 16);
        e->s.modelindex = modelindex;
    }
}

static void q1d_parse_static(void)
{
    int model, frame, skin;
    vec3_t origin, angles;

    q1d_parse_baseline(&model, &frame, &skin, origin, angles);
    if (q1d.bad)
        return;

    int mi = q1d_model(model);
    if (!mi || q1d.num_statics >= Q1D_MAX_STATICS)
        return;

    edict_t *e = q1d_new_static();
    if (!e)
        return;

    q1d_static_t *st = &q1d.statics[q1d.num_statics++];
    st->num = e->s.number;
    st->modelindex = mi;
    st->frame = frame;
    st->skin = skin;

    q1d_set_box(e, mi);
    VectorCopy(origin, e->s.origin);
    VectorCopy(origin, e->s.old_origin);
    VectorCopy(angles, e->s.angles);
    if (Q1_IsAliasModel(mi)) {
        e->s.angles[PITCH] = -e->s.angles[PITCH];
        e->s.angles[ROLL] = -e->s.angles[ROLL];
    }
    Q1_AnimPick(mi, st->num, q1d.time, &frame, &skin);
    e->s.frame = frame;
    e->s.skinnum = skin;
    e->s.effects = Q1_ModelTrailEffects(mi);
    gi.linkentity(e);
}

static void q1d_parse_staticsound(void)
{
    vec3_t origin;

    for (int i = 0; i < 3; i++)
        origin[i] = q1d_coord();
    int sound = q1d_byte();
    q1d_byte();     // volume
    q1d_byte();     // attenuation: a loop is Quake II's ATTN_STATIC
    if (q1d.bad || sound <= 0 || sound >= q1d.num_sounds || !q1d.sound_map[sound])
        return;

    // as the Quake 1 game's ambientsound: an entity of its own, its s.sound the loop
    edict_t *e = q1d_new_static();
    if (!e)
        return;
    VectorCopy(origin, e->s.origin);
    VectorCopy(origin, e->s.old_origin);
    e->s.sound = q1d.sound_map[sound];
    gi.linkentity(e);
}

// CL_ParseUpdate
static void q1d_parse_update(int bits)
{
    if (bits & Q1D_U_MOREBITS)
        bits |= q1d_byte() << 8;

    int n = (bits & Q1D_U_LONGENTITY) ? q1d_short() : q1d_byte();
    if (n <= 0 || n >= Q1D_MAX_ENTS) {
        q1d.bad = true;
        return;
    }
    q1d_note_entity(n);

    q1d_ent_t *e = &q1d.ents[n];
    bool forcelink = e->msgtime != q1d.mtime[1];   // no previous frame to lerp from
    e->msgtime = q1d.mtime[0];

    int model = (bits & Q1D_U_MODEL) ? q1d_byte() : e->b_model;
    if (model != e->model) {
        e->model = model;
        if (!model)
            forcelink = true;
    }
    e->frame = (bits & Q1D_U_FRAME) ? q1d_byte() : e->b_frame;
    if (bits & Q1D_U_COLORMAP)
        q1d_byte();
    e->skin = (bits & Q1D_U_SKIN) ? q1d_byte() : e->b_skin;
    e->effects = (bits & Q1D_U_EFFECTS) ? q1d_byte() : 0;
    if (e->effects & Q1_EF_MUZZLEFLASH)
        e->flash = true;

    VectorCopy(e->msg_origins[0], e->msg_origins[1]);
    VectorCopy(e->msg_angles[0], e->msg_angles[1]);

    e->msg_origins[0][0] = (bits & Q1D_U_ORIGIN1) ? q1d_coord() : e->b_origin[0];
    e->msg_angles[0][0] = (bits & Q1D_U_ANGLE1) ? q1d_angle() : e->b_angles[0];
    e->msg_origins[0][1] = (bits & Q1D_U_ORIGIN2) ? q1d_coord() : e->b_origin[1];
    e->msg_angles[0][1] = (bits & Q1D_U_ANGLE2) ? q1d_angle() : e->b_angles[1];
    e->msg_origins[0][2] = (bits & Q1D_U_ORIGIN3) ? q1d_coord() : e->b_origin[2];
    e->msg_angles[0][2] = (bits & Q1D_U_ANGLE3) ? q1d_angle() : e->b_angles[2];

    // Quake didn't lerp a monster's steps (U_NOLERP); Quake II's client lerps
    // them between frames as its own monsters' - only a real jump snaps
    if (bits & Q1D_U_NOLERP)
        e->forcelink = true;

    if (forcelink) {
        VectorCopy(e->msg_origins[0], e->msg_origins[1]);
        VectorCopy(e->msg_angles[0], e->msg_angles[1]);
        e->forcelink = true;
    } else {
        for (int i = 0; i < 3; i++) {
            float d = e->msg_origins[0][i] - e->msg_origins[1][i];
            if (d > 100 || d < -100)
                e->jumped = true;
        }
    }
}

// CL_ParseClientdata
static void q1d_parse_clientdata(void)
{
    int bits = q1d_short() & 0xffff;
    int i;

    q1d.viewheight = (bits & Q1D_SU_VIEWHEIGHT) ? q1d_char() : Q1D_DEFAULT_VIEWHEIGHT;
    if (bits & Q1D_SU_IDEALPITCH)
        q1d_char();

    for (i = 0; i < 3; i++) {
        q1d.punchangle[i] = (bits & (Q1D_SU_PUNCH1 << i)) ? q1d_char() : 0;
        q1d.velocity[i] = (bits & (Q1D_SU_VELOCITY1 << i)) ? q1d_char() * 16 : 0;
    }

    q1d.items = q1d_long();     // always sent
    q1d.onground = (bits & Q1D_SU_ONGROUND) != 0;

    q1d.stats[Q1D_STAT_WEAPONFRAME] = (bits & Q1D_SU_WEAPONFRAME) ? q1d_byte() : 0;
    q1d.stats[Q1D_STAT_ARMOR] = (bits & Q1D_SU_ARMOR) ? q1d_byte() : 0;
    q1d.stats[Q1D_STAT_WEAPON] = (bits & Q1D_SU_WEAPON) ? q1d_byte() : 0;
    q1d.stats[Q1D_STAT_HEALTH] = q1d_short();
    q1d.stats[Q1D_STAT_AMMO] = q1d_byte();
    for (i = 0; i < 4; i++)
        q1d.stats[Q1D_STAT_SHELLS + i] = q1d_byte();

    i = q1d_byte();
    q1d.stats[Q1D_STAT_ACTIVEWEAPON] = q1d.standard_quake ? i : (i < 32 ? 1 << i : 0);
}

static float q1d_sound_offset(void)
{
    // into the frame the client is lerping toward (see TIME above)
    float ofs = q1d.mtime[0] - q1d.time + FRAMETIME;
    clamp(ofs, 0, 0.255f);
    return ofs;
}

static void q1d_parse_sound(void)
{
    int mask = q1d_byte();
    float volume = (mask & Q1D_SND_VOLUME) ? q1d_byte() / 255.0f : 1.0f;
    float atten = (mask & Q1D_SND_ATTENUATION) ? q1d_byte() / 64.0f : 1.0f;
    int channel = q1d_short() & 0xffff;
    int sound = q1d_byte();
    vec3_t pos;

    for (int i = 0; i < 3; i++)
        pos[i] = q1d_coord();

    if (q1d.bad || q1d.signon || sound <= 0 || sound >= q1d.num_sounds || !q1d.sound_map[sound])
        return;

    int ent = channel >> 3;
    channel &= 7;
    clamp(atten, 0, 4);

    int num = q1d_edict_num(ent);
    if (num >= game.maxentities)
        num = 0;

    // the player's own come from the listener, as Quake played them; the rest
    // from where Quake started them, heard everywhere as the Quake 1 game's
    // are (Q1_StartSound)
    if (num == 1)
        gi.sound(&g_edicts[1], channel | CHAN_NO_PHS_ADD, q1d.sound_map[sound], volume, atten, q1d_sound_offset());
    else
        gi.positioned_sound(pos, &g_edicts[num], channel | CHAN_NO_PHS_ADD, q1d.sound_map[sound], volume, atten, q1d_sound_offset());
}

static void q1d_parse_temp_entity(void)
{
    int kind = q1d_byte();
    int ent = 0;
    vec3_t pos, end;
    int color_start = 0, color_length = 0;

    switch (kind) {
    case TE_Q1_LIGHTNING1:
    case TE_Q1_LIGHTNING2:
    case TE_Q1_LIGHTNING3:
    case TE_Q1_BEAM:
        ent = q1d_short();
        for (int i = 0; i < 3; i++)
            pos[i] = q1d_coord();
        for (int i = 0; i < 3; i++)
            end[i] = q1d_coord();
        break;
    case TE_Q1_EXPLOSION2:
        for (int i = 0; i < 3; i++)
            pos[i] = q1d_coord();
        color_start = q1d_byte();
        color_length = q1d_byte();
        break;
    case TE_Q1_SPIKE:
    case TE_Q1_SUPERSPIKE:
    case TE_Q1_GUNSHOT:
    case TE_Q1_EXPLOSION:
    case TE_Q1_TAREXPLOSION:
    case TE_Q1_WIZSPIKE:
    case TE_Q1_KNIGHTSPIKE:
    case TE_Q1_LAVASPLASH:
    case TE_Q1_TELEPORT:
        for (int i = 0; i < 3; i++)
            pos[i] = q1d_coord();
        break;
    default:
        q1d.bad = true;
        return;
    }
    if (q1d.bad || q1d.signon)
        return;

    // as the Quake 1 game writes them (q1_temp_entity), to everyone as Quake did
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(kind);
    switch (kind) {
    case TE_Q1_LIGHTNING1:
    case TE_Q1_LIGHTNING2:
    case TE_Q1_LIGHTNING3:
    case TE_Q1_BEAM:
        gi.WriteShort(q1d_edict_num(ent));
        gi.WritePosition(pos);
        gi.WritePosition(end);
        break;
    case TE_Q1_EXPLOSION2:
        gi.WritePosition(pos);
        gi.WriteByte(color_start);
        gi.WriteByte(color_length);
        break;
    default:
        gi.WritePosition(pos);
        break;
    }
    gi.multicast(pos, MULTICAST_ALL);
}

static void q1d_parse_particle(void)
{
    vec3_t pos;
    int dir[3];

    for (int i = 0; i < 3; i++)
        pos[i] = q1d_coord();
    for (int i = 0; i < 3; i++)
        dir[i] = q1d_char();
    int count = q1d_byte();
    int color = q1d_byte();
    if (q1d.bad || q1d.signon)
        return;

    // as the Quake 1 game's particle(): the direction as Quake sent it, x16
    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(TE_Q1_PARTICLE);
    gi.WritePosition(pos);
    for (int i = 0; i < 3; i++)
        gi.WriteChar(dir[i]);
    gi.WriteByte(count);
    gi.WriteByte(color);
    gi.multicast(pos, MULTICAST_ALL);
}

// V_ParseDamage: a red flash and a kick away
static void q1d_parse_damage(void)
{
    int armor = q1d_byte();
    int blood = q1d_byte();
    vec3_t from, forward, right, up, angles;

    for (int i = 0; i < 3; i++)
        from[i] = q1d_coord();
    if (q1d.bad || q1d.signon)
        return;

    float count = blood * 0.5f + armor * 0.5f;
    if (count < 10)
        count = 10;

    q1d.hits++;     // the status bar's pain face

    q1d.cshift_damage += 3 * count;
    clamp(q1d.cshift_damage, 0, 150);

    if (armor > blood)
        VectorSet(q1d.damage_color, 200, 100, 100);
    else if (armor)
        VectorSet(q1d.damage_color, 220, 50, 50);
    else
        VectorSet(q1d.damage_color, 255, 0, 0);

    // calculate view angle kicks
    if (q1d.viewentity > 0 && q1d.viewentity < Q1D_MAX_ENTS)
        VectorSubtract(from, q1d.ents[q1d.viewentity].msg_origins[0], from);
    VectorNormalize(from);

    VectorSet(angles, 0, q1d.mviewangles[0][YAW], 0);
    AngleVectors(angles, forward, right, up);
    q1d.dmg_roll = count * DotProduct(from, right) * 0.6f;
    q1d.dmg_pitch = count * DotProduct(from, forward) * 0.6f;
    q1d.dmg_time = 0.5f;
}

// svc_print's pieces, a whole line to the console at a time
static void q1d_print(const char *s)
{
    if (q1d.signon)
        return;     // the server's version banner

    for (; *s; s++) {
        int c = *s & 255;
        if (c == 1 || c == 2)
            continue;   // Quake's talk-sound and colour markers
        size_t n = strlen(q1d.print);
        if (n < sizeof(q1d.print) - 1) {
            q1d.print[n] = c;
            q1d.print[n + 1] = 0;
        }
        if (c == '\n') {
            if (q1d.started)
                gi.cprintf(&g_edicts[1], PRINT_HIGH, "%s", q1d.print);
            q1d.print[0] = 0;
        }
    }
}

static void q1d_centerprint(const char *s)
{
    if (q1d.started && !q1d.signon && *s)
        gi.centerprintf(&g_edicts[1], "%s", s);
}

static void q1d_cdtrack(int track)
{
    if (q1d.forcetrack != -1)
        track = q1d.forcetrack;
    gi.configstring(CS_CDTRACK, va("%i", track));
}

// CL_ParseServerMessage; false for the end of the demo (or a message that
// can't be read)
static bool q1d_parse_message(void)
{
    while (q1d.msgpos < q1d.msglen && !q1d.bad) {
        int cmd = q1d_byte();

        if (cmd & 128) {
            q1d_parse_update(cmd & 127);
            continue;
        }

        switch (cmd) {
        case Q1D_SVC_NOP:
            break;

        case Q1D_SVC_DISCONNECT:
            return false;

        case Q1D_SVC_UPDATESTAT: {
            int i = q1d_byte();
            int v = q1d_long();
            if (i >= 0 && i < Q1D_MAX_STATS)
                q1d.stats[i] = v;
            break;
        }

        case Q1D_SVC_VERSION:
            q1d_long();
            break;

        case Q1D_SVC_SETVIEW:
            q1d.viewentity = q1d_short();
            break;

        case Q1D_SVC_SOUND:
            q1d_parse_sound();
            break;

        case Q1D_SVC_TIME:
            q1d.mtime[1] = q1d.mtime[0];
            q1d.mtime[0] = q1d_float();
            q1d.timed = true;
            break;

        case Q1D_SVC_PRINT:
            q1d_print(q1d_string());
            break;

        case Q1D_SVC_STUFFTEXT: {
            // "bf": the bonus flash (an item picked up); nothing else applies
            const char *s = q1d_string();
            if (!strncmp(s, "bf", 2) && (s[2] == '\n' || !s[2]))
                q1d.cshift_bonus = 50;
            break;
        }

        case Q1D_SVC_SETANGLE:
            // the recorded view angles have it already
            q1d_angle();
            q1d_angle();
            q1d_angle();
            break;

        case Q1D_SVC_SERVERINFO:
            q1d_parse_serverinfo();
            break;

        case Q1D_SVC_LIGHTSTYLE: {
            int i = q1d_byte();
            const char *s = q1d_string();
            if (i >= 0 && i < MAX_LIGHTSTYLES)
                gi.configstring(CS_LIGHTS + i, s);
            break;
        }

        case Q1D_SVC_UPDATENAME:
            q1d_byte();
            q1d_string();
            break;

        case Q1D_SVC_UPDATEFRAGS: {
            int i = q1d_byte();
            int v = q1d_short();
            if (i + 1 == q1d.viewentity)
                q1d.frags = v;
            break;
        }

        case Q1D_SVC_CLIENTDATA:
            q1d_parse_clientdata();
            break;

        case Q1D_SVC_STOPSOUND:
            q1d_short();
            break;

        case Q1D_SVC_UPDATECOLORS:
            q1d_byte();
            q1d_byte();
            break;

        case Q1D_SVC_PARTICLE:
            q1d_parse_particle();
            break;

        case Q1D_SVC_DAMAGE:
            q1d_parse_damage();
            break;

        case Q1D_SVC_SPAWNSTATIC:
            q1d_parse_static();
            break;

        case Q1D_SVC_SPAWNBASELINE: {
            int n = q1d_short();
            if (n < 0 || n >= Q1D_MAX_ENTS) {
                q1d.bad = true;
                break;
            }
            q1d_ent_t *e = &q1d.ents[n];
            q1d_parse_baseline(&e->b_model, &e->b_frame, &e->b_skin, e->b_origin, e->b_angles);
            if (n > 0)
                q1d_note_entity(n);
            break;
        }

        case Q1D_SVC_TEMP_ENTITY:
            q1d_parse_temp_entity();
            break;

        case Q1D_SVC_SETPAUSE:
        case Q1D_SVC_SIGNONNUM:
            q1d_byte();
            break;

        case Q1D_SVC_CENTERPRINT:
            q1d_centerprint(q1d_string());
            break;

        case Q1D_SVC_KILLEDMONSTER:
            q1d.stats[Q1D_STAT_MONSTERS]++;
            break;

        case Q1D_SVC_FOUNDSECRET:
            q1d.stats[Q1D_STAT_SECRETS]++;
            break;

        case Q1D_SVC_SPAWNSTATICSOUND:
            q1d_parse_staticsound();
            break;

        case Q1D_SVC_INTERMISSION:
            q1d.intermission = 1;
            q1d.completed_time = q1d.time;
            break;

        case Q1D_SVC_FINALE:
        case Q1D_SVC_CUTSCENE:
            q1d.intermission = cmd == Q1D_SVC_FINALE ? 2 : 3;
            q1d.completed_time = q1d.time;
            q1d_centerprint(q1d_string());
            break;

        case Q1D_SVC_CDTRACK: {
            int track = q1d_byte();
            q1d_byte();     // the loop track
            q1d_cdtrack(track);
            break;
        }

        case Q1D_SVC_SELLSCREEN:
            break;

        default:
            q1d.bad = true;
            break;
        }
    }

    if (q1d.bad) {
        gi.dprintf("Quake 1 demo %s: a message at %d couldn't be read; the demo ends there\n",
                   q1d.path, q1d.pos);
        return false;
    }
    return true;
}

/*
==============================================================================

STARTING

==============================================================================
*/

// "id1/<episode>/<name>.dem"
static bool q1d_episode(const char *path, char *episode, size_t size)
{
    if (Q_strncasecmp(path, "id1/", 4))
        return false;
    const char *s = path + 4;
    const char *slash = strchr(s, '/');
    if (!slash || slash == s || (size_t)(slash - s) >= size)
        return false;
    memcpy(episode, s, slash - s);
    episode[slash - s] = 0;
    return true;
}

static bool q1d_load(const char *mapname)
{
    char episode[16];
    void *buf;
    int len, i;

    if (!q1d_episode(q1d.path, episode, sizeof(episode))) {
        gi.dprintf("Quake 1 demo %s: not an id1/<episode>/ path\n", q1d.path);
        return false;
    }

    len = gi.LoadFile(q1d.path, &buf);
    if (len <= 0 || !buf) {
        gi.dprintf("Quake 1 demo %s: not found\n", q1d.path);
        return false;
    }
    q1d.data = gi.TagMalloc(len, TAG_LEVEL);
    memcpy(q1d.data, buf, len);
    gi.FreeFile(buf);
    q1d.size = len;

    // the header: the CD track the demo forces, as text, to the first newline
    for (i = 0; i < len && i < 16 && q1d.data[i] != '\n'; i++)
        ;
    if (i >= len || q1d.data[i] != '\n') {
        gi.dprintf("Quake 1 demo %s: no header\n", q1d.path);
        return false;
    }
    char track[17];
    memcpy(track, q1d.data, i);
    track[i] = 0;
    q1d.forcetrack = atoi(track);
    q1d.pos = i + 1;

    // the Quake 1 game's names and tables, with no progs.dat behind them
    QC_FreeProgs();
    Q_strlcpy(qvm.episode, episode, sizeof(qvm.episode));
    Q1_InitBuiltins();

    q1d.ents = gi.TagMalloc(sizeof(q1d_ent_t) * Q1D_MAX_ENTS, TAG_LEVEL);
    memset(q1d.ents, 0, sizeof(q1d_ent_t) * Q1D_MAX_ENTS);
    q1d.standard_quake = Q_stricmp(episode, "hipnotic") && Q_stricmp(episode, "rogue");
    q1d.viewentity = 1;
    q1d.next_static = game.maxclients + Q1D_MAX_ENTS;
    q1d.viewheight = Q1D_DEFAULT_VIEWHEIGHT;

    // the signon - the server info, the baselines, the statics, the light
    // styles - up to the message that starts the clock
    q1d.signon = true;
    while (!q1d.timed) {
        if (!q1d_next_block() || !q1d_parse_message()) {
            gi.dprintf("Quake 1 demo %s: ends before it begins\n", q1d.path);
            return false;
        }
    }
    q1d.signon = false;
    q1d.time = q1d.mtime[0];

    if (!q1d.serverinfo) {
        gi.dprintf("Quake 1 demo %s: no server info\n", q1d.path);
        return false;
    }
    if (Q_stricmp(q1d.mapname, COM_SkipPath(mapname))) {
        gi.dprintf("Quake 1 demo %s: recorded on %s, not %s\n", q1d.path, q1d.mapname, mapname);
        return false;
    }
    return true;
}

/*
A Quake 1 map started for a demo (g_q1_demo): the demo's signon read and the
level set up for it, in place of the map's own entities. Returns false for
any other level.
*/
bool Q1Demo_SpawnEntities(const char *mapname)
{
    memset(&q1d, 0, sizeof(q1d));
    gi.cvar_forceset("sv_q1demo", "0");

    if (!g_q1_demo)
        g_q1_demo = gi.cvar("g_q1_demo", "", 0);
    if (!level.q1map || !g_q1_demo->string[0])
        return false;

    // one level's worth: the next map is a game again
    Q_strlcpy(q1d.path, g_q1_demo->string, sizeof(q1d.path));
    gi.cvar_forceset("g_q1_demo", "");

    // the Quake 1 game doesn't run (g_q1vm_game.c): the last level's machine
    // went with its memory (TAG_LEVEL)
    QC_FreeProgs();
    gi.cvar_forceset("sv_q1vm", "0");
    gi.cvar_forceset("sv_q1demo", "1");     // no saves (server/save.c)

    // the world
    world->inuse = true;
    world->s.modelindex = 1;
    world->solid = SOLID_BSP;
    world->movetype = MOVETYPE_PUSH;
    world->classname = "worldspawn";
    if (globals.num_edicts < game.maxclients + 1)
        globals.num_edicts = game.maxclients + 1;

    q1d.active = true;
    if (!q1d_load(mapname))
        q1d.broken = true;  // over at the first frame: the attract loop goes on

    // the level's configstrings, as the Quake 1 game's (Q1VM_SpawnEntities)
    const char *name = q1d.level_name[0] ? q1d.level_name : COM_SkipPath(mapname);
    gi.configstring(CS_NAME, name);
    Q_strlcpy(level.level_name, name, sizeof(level.level_name));
    gi.configstring(CS_SKY, "unit1_");
    gi.configstring(CS_SKYROTATE, "0 0");
    gi.configstring(CS_SKYAXIS, "0 0 0");
    gi.configstring(CS_MAXCLIENTS, va("%i", (int)maxclients->value));
    gi.configstring(CS_GAME_ID, GAME_ID_STRING);
    gi.configstring(CS_Q3RULES, "00");
    gi.configstring(CS_Q1VM, qvm.episode[0] ? qvm.episode : "id1");

    gi.dprintf("Quake 1 demo: %s on %s (%s), %d models, %d sounds, %d statics\n",
               q1d.path, mapname, q1d.level_name, q1d.num_models - 1, q1d.num_sounds - 1, q1d.num_statics);
    return true;
}

/*
==============================================================================

THE PLAYER

==============================================================================
*/

void Q1Demo_ClientBegin(edict_t *ent)
{
    int num = (int)(ent - g_edicts);
    gclient_t *client = ent->client;
    short fov = client->ps.fov;

    ent->inuse = true;
    ent->s.number = num;
    ent->classname = "player";
    ent->svflags = 0;
    ent->solid = SOLID_NOT;
    ent->movetype = MOVETYPE_NONE;
    memset(&client->ps, 0, sizeof(client->ps));
    client->ps.fov = fov ? fov : 90;
    client->ps.pmove.pm_type = PM_FREEZE;
    client->ps.pmove.pm_flags = PMF_NO_PREDICTION;
    memset(&client->old_pmove, 0, sizeof(client->old_pmove));

    // the recorded player is edict 1; anyone else just watches it
    if (num == 1)
        q1d.started = true;
}

void Q1Demo_ClientDisconnect(edict_t *ent)
{
    int num = (int)(ent - g_edicts);

    if (num == 1)
        q1d.started = false;

    gi.unlinkentity(ent);
    ent->s.modelindex = 0;
    ent->s.modelindex4 = 0;
    ent->s.sound = 0;
    ent->s.event = 0;
    ent->s.effects = 0;
    ent->solid = SOLID_NOT;
    ent->inuse = false;
    ent->classname = "disconnected";
    ent->client->pers.connected = false;
}

static void q1d_add_cshift(float *blend, float r, float g, float b, float percent)
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

// V_CalcRoll: leaning into a strafe
static float q1d_calc_roll(const vec3_t angles, const vec3_t velocity)
{
    const float rollangle = 2, rollspeed = 200;
    vec3_t forward, right, up;

    AngleVectors(angles, forward, right, up);
    float side = DotProduct(velocity, right);
    float sign = side < 0 ? -1.0f : 1.0f;
    side = fabsf(side);
    side = side < rollspeed ? side * rollangle / rollspeed : rollangle;
    return side * sign;
}

/*
The player's view and HUD: Quake's view.c (V_CalcRefdef, V_CalcBlend) and the
status bar's stats from the recorded clientdata, as the Quake 1 game's
q1_client_end_frame puts them in Quake II's player_state_t.
*/
static void q1d_player_state(float frac)
{
    edict_t *ent = &g_edicts[1];
    if (!ent->inuse || !ent->client)
        return;

    gclient_t *client = ent->client;
    player_state_t *ps = &client->ps;
    vec3_t forward, right, up;
    int i;

    ps->pmove.pm_type = PM_FREEZE;
    ps->pmove.pm_flags = PMF_NO_PREDICTION;     // the view lerps between frames
    for (i = 0; i < 3; i++) {
        ps->pmove.origin[i] = COORD2SHORT(q1d.view_origin[i]);
        ps->pmove.velocity[i] = 0;
        ps->pmove.delta_angles[i] = 0;
    }

    // the recorded view, lerped as Quake's demo playback did
    for (i = 0; i < 3; i++) {
        float d = q1d.mviewangles[0][i] - q1d.mviewangles[1][i];
        if (d > 180)
            d -= 360;
        else if (d < -180)
            d += 360;
        ps->viewangles[i] = q1d.mviewangles[1][i] + frac * d;
    }

    memset(ps->stats, 0, sizeof(ps->stats));
    VectorClear(ps->kick_angles);
    VectorClear(ps->gunoffset);
    VectorClear(ps->gunangles);
    Vector4Clear(ps->blend);
    ps->rdflags = 0;
    ps->gunindex = 0;
    ps->gunframe = 0;

    // the status bar (q1_hud_stats): the items already carry the sigils or
    // the mission packs' items2, as Quake's server sent them
    const int health = q1d.stats[Q1D_STAT_HEALTH];
    const uint32_t items = (uint32_t)q1d.items;
    const uint32_t weapon = (uint32_t)q1d.stats[Q1D_STAT_ACTIVEWEAPON];
    ps->stats[STAT_HEALTH] = (short)health;
    ps->stats[STAT_ARMOR] = (short)q1d.stats[Q1D_STAT_ARMOR];
    ps->stats[STAT_AMMO] = (short)q1d.stats[Q1D_STAT_AMMO];
    ps->stats[STAT_FRAGS] = (short)q1d.frags;
    ps->stats[STAT_Q1_ITEMS_LO] = (short)(items & 0xffff);
    ps->stats[STAT_Q1_ITEMS_HI] = (short)(items >> 16);
    ps->stats[STAT_Q1_WEAPON_LO] = (short)(weapon & 0xffff);
    ps->stats[STAT_Q1_WEAPON_HI] = (short)(weapon >> 16);
    for (i = 0; i < 4; i++)
        ps->stats[STAT_Q1_SHELLS + i] = (short)q1d.stats[Q1D_STAT_SHELLS + i];
    ps->stats[STAT_Q1_MONSTERS] = (short)q1d.stats[Q1D_STAT_MONSTERS];
    ps->stats[STAT_Q1_TOTALMONSTERS] = (short)q1d.stats[Q1D_STAT_TOTALMONSTERS];
    ps->stats[STAT_Q1_SECRETS] = (short)q1d.stats[Q1D_STAT_SECRETS];
    ps->stats[STAT_Q1_TOTALSECRETS] = (short)q1d.stats[Q1D_STAT_TOTALSECRETS];
    float t = (q1d.intermission ? q1d.completed_time : q1d.time) - 1;
    ps->stats[STAT_Q1_TIME] = (short)(t > 0 ? t : 0);
    ps->stats[STAT_Q1_INTERMISSION] = (short)(q1d.intermission | ((q1d.hits * STAT_Q1_HIT) & STAT_Q1_HITS));

    if (q1d.intermission) {
        // V_CalcIntermissionRefdef: the eye at the spot, no gun
        VectorClear(ps->viewoffset);
        return;
    }

    // the eyes, bobbing as V_CalcBob did
    float bob = 0;
    if (health > 0 && q1d.onground) {
        const float bobcycle = 0.6f, bobup = 0.5f;
        float cycle = q1d.time - (int)(q1d.time / bobcycle) * bobcycle;
        cycle /= bobcycle;
        if (cycle < bobup)
            cycle = M_PI * cycle / bobup;
        else
            cycle = M_PI + M_PI * (cycle - bobup) / (1.0f - bobup);
        bob = sqrtf(q1d.velocity[0] * q1d.velocity[0] + q1d.velocity[1] * q1d.velocity[1]) * 0.02f;
        bob = bob * 0.3f + bob * 0.7f * sinf(cycle);
        clamp(bob, -7, 4);
    }
    VectorSet(ps->viewoffset, 0, 0, q1d.viewheight + bob);

    if (health <= 0)
        ps->viewangles[ROLL] = 80;  // dead view angle

    // kicks: the punch, damage, leaning into a strafe
    VectorCopy(q1d.punchangle, ps->kick_angles);
    if (q1d.dmg_time > 0) {
        ps->kick_angles[ROLL] += q1d.dmg_time / 0.5f * q1d.dmg_roll;
        ps->kick_angles[PITCH] += q1d.dmg_time / 0.5f * q1d.dmg_pitch;
        q1d.dmg_time -= FRAMETIME;
    }
    if (health > 0)
        ps->kick_angles[ROLL] += q1d_calc_roll(ps->viewangles, q1d.velocity);

    // the gun, where Quake held it (none while invisible or dead)
    if (health > 0 && !(items & Q1_IT_INVISIBILITY)) {
        ps->gunindex = q1d_model(q1d.stats[Q1D_STAT_WEAPON]);
        ps->gunframe = q1d.stats[Q1D_STAT_WEAPONFRAME];
    }
    AngleVectors(ps->viewangles, forward, right, up);
    VectorScale(forward, bob * 0.4f, ps->gunoffset);
    ps->gunoffset[2] += 2;

    // the colour shifts (V_CalcBlend): contents, damage, bonus, powerups
    vec3_t eye;
    VectorAdd(q1d.view_origin, ps->viewoffset, eye);
    switch (Q1_PointContents(eye)) {
    case Q1_CONTENTS_LAVA:
        q1d_add_cshift(ps->blend, 255, 80, 0, 150);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    case Q1_CONTENTS_SLIME:
        q1d_add_cshift(ps->blend, 0, 25, 5, 150);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    case Q1_CONTENTS_WATER:
        q1d_add_cshift(ps->blend, 130, 80, 50, 128);
        ps->rdflags |= RDF_UNDERWATER;
        break;
    default:
        break;
    }

    q1d_add_cshift(ps->blend, q1d.damage_color[0], q1d.damage_color[1], q1d.damage_color[2], q1d.cshift_damage);
    q1d_add_cshift(ps->blend, 215, 186, 69, q1d.cshift_bonus);

    if (items & Q1_IT_QUAD)
        q1d_add_cshift(ps->blend, 0, 0, 255, 30);
    else if (items & Q1_IT_SUIT)
        q1d_add_cshift(ps->blend, 0, 255, 0, 20);
    else if (items & Q1_IT_INVISIBILITY)
        q1d_add_cshift(ps->blend, 100, 100, 100, 100);
    else if (items & Q1_IT_INVULNERABILITY)
        q1d_add_cshift(ps->blend, 255, 255, 0, 30);

    for (i = 0; i < 4; i++)
        clamp(ps->blend[i], 0, 1);

    q1d.cshift_damage -= FRAMETIME * 150;
    if (q1d.cshift_damage < 0)
        q1d.cshift_damage = 0;
    q1d.cshift_bonus -= FRAMETIME * 100;
    if (q1d.cshift_bonus < 0)
        q1d.cshift_bonus = 0;
}

/*
==============================================================================

THE FRAME

==============================================================================
*/

// CL_LerpPoint: how far the clock is from the message before the last to the last
static float q1d_lerp_point(void)
{
    float f = q1d.mtime[0] - q1d.mtime[1];

    if (f <= 0) {
        q1d.time = q1d.mtime[0];
        return 1;
    }
    if (f > 0.1f) {
        // dropped packet, or start of demo
        q1d.mtime[1] = q1d.mtime[0] - 0.1f;
        f = 0.1f;
    }

    float frac = (q1d.time - q1d.mtime[1]) / f;
    if (frac < 0) {
        if (frac < -0.01f)
            q1d.time = q1d.mtime[1];
        frac = 0;
    } else if (frac > 1) {
        if (frac > 1.01f)
            q1d.time = q1d.mtime[0];
        frac = 1;
    }
    return frac;
}

// CL_RelinkEntities for one entity, onto its edict
static void q1d_sync_entity(int n, float frac)
{
    q1d_ent_t *qe = &q1d.ents[n];
    int num = q1d_edict_num(n);
    if (num <= 0 || num >= game.maxentities)
        return;
    edict_t *e = &g_edicts[num];
    int mi = q1d_model(qe->model);

    // only what the last message had is drawn
    if (qe->msgtime != q1d.mtime[0] || !mi) {
        if (qe->shown) {
            e->svflags |= SVF_NOCLIENT;
            e->s.event = 0;
            qe->shown = false;
        }
        qe->flash = false;
        return;
    }

    vec3_t origin, angles;
    if (qe->forcelink) {
        VectorCopy(qe->msg_origins[0], origin);
        VectorCopy(qe->msg_angles[0], angles);
    } else {
        vec3_t delta;
        float f = frac;
        VectorSubtract(qe->msg_origins[0], qe->msg_origins[1], delta);
        for (int i = 0; i < 3; i++)
            if (delta[i] > 100 || delta[i] < -100)
                f = 1;  // assume a teleportation, not a motion
        for (int i = 0; i < 3; i++) {
            origin[i] = qe->msg_origins[1][i] + f * delta[i];
            float d = qe->msg_angles[0][i] - qe->msg_angles[1][i];
            if (d > 180)
                d -= 360;
            else if (d < -180)
                d += 360;
            angles[i] = qe->msg_angles[1][i] + f * d;
        }
    }

    e->inuse = true;
    e->s.number = num;
    e->svflags &= ~SVF_NOCLIENT;
    e->solid = SOLID_NOT;
    if (num > game.maxclients)
        e->classname = "q1demo_entity";

    if (mi != qe->sent_model || !e->s.modelindex) {
        q1d_set_box(e, mi);
        qe->sent_model = mi;
    }
    e->s.modelindex = mi;

    int frame = qe->frame, skin = qe->skin;
    Q1_AnimPick(mi, num, q1d.time, &frame, &skin);
    e->s.frame = frame;
    e->s.skinnum = skin;

    VectorCopy(angles, e->s.angles);
    if (Q1_IsAliasModel(mi)) {
        // Quake drew its alias models pitched (and rolled) the other way
        e->s.angles[PITCH] = -e->s.angles[PITCH];
        e->s.angles[ROLL] = -e->s.angles[ROLL];
    }

    e->s.effects = Q1_ModelTrailEffects(mi);
    e->s.renderfx = 0;
    // the entity's lights ride in modelindex4 (CS_Q1VM)
    e->s.modelindex4 = qe->effects & (Q1_EF_BRIGHTFIELD | Q1_EF_BRIGHTLIGHT | Q1_EF_DIMLIGHT |
                                      Q1_EF_QUADLIGHT | Q1_EF_PENTALIGHT | Q1_EF_CANDLELIGHT);

    // a muzzle flash is for one frame
    if (qe->flash) {
        gi.WriteByte(svc_temp_entity);
        gi.WriteByte(TE_Q1);
        gi.WriteByte(TE_Q1_MUZZLEFLASH);
        gi.WriteShort(num);
        gi.multicast(origin, MULTICAST_PVS);
        qe->flash = false;
    }

    VectorCopy(origin, e->s.origin);
    // a new one starts where it is; a jump is a teleport, not a slide
    VectorCopy(qe->shown ? qe->sent_origin : origin, e->s.old_origin);
    e->s.event = 0;
    if (qe->shown && (qe->jumped || (qe->forcelink && Distance(qe->sent_origin, origin) > 100)))
        e->s.event = EV_OTHER_TELEPORT;
    gi.linkentity(e);

    VectorCopy(origin, qe->sent_origin);
    qe->shown = true;
    qe->forcelink = false;
    qe->jumped = false;

    if (n == q1d.viewentity)
        VectorCopy(origin, q1d.view_origin);
}

// the end of the demo: the attract loop's next, or back to the menu
static void q1d_finish(void)
{
    if (q1d.finished)
        return;
    q1d.finished = true;

    cvar_t *next = gi.cvar("nextserver", "", 0);
    if (next->string[0]) {
        char cmd[MAX_QPATH * 2];
        Q_snprintf(cmd, sizeof(cmd), "%s\n", next->string);
        gi.cvar_set("nextserver", "");
        gi.AddCommandString(cmd);
    } else {
        gi.AddCommandString("killserver\n");
    }
}

void Q1Demo_RunFrame(void)
{
    if (!q1d.started || q1d.finished)
        return;

    if (q1d.broken) {
        q1d_finish();   // it couldn't be read
        return;
    }

    // read until the last message is at or past the clock (CL_ReadFromServer)
    q1d.time += FRAMETIME;
    bool more = true;
    while (q1d.time > q1d.mtime[0]) {
        if (!q1d_next_block() || !q1d_parse_message()) {
            more = false;
            break;
        }
    }

    float frac = q1d_lerp_point();

    for (int n = 1; n <= q1d.max_ent && n < Q1D_MAX_ENTS; n++)
        q1d_sync_entity(n, frac);

    // the torches' flames and the other frame groups go on
    for (int i = 0; i < q1d.num_statics; i++) {
        q1d_static_t *st = &q1d.statics[i];
        int frame = st->frame, skin = st->skin;
        Q1_AnimPick(st->modelindex, st->num, q1d.time, &frame, &skin);
        g_edicts[st->num].s.frame = frame;
        g_edicts[st->num].s.skinnum = skin;
    }

    q1d_player_state(frac);

    if (!more)
        q1d_finish();
}
