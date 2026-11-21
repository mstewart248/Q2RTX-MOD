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

// g_q1vm_cmds.c - the QuakeC builtins (Quake's pr_cmds.c, plus the 2021
// rerelease's ex_* as vkQuake has them), the models and sounds the progs
// precache, and the network messages they write, re-sent in Quake II's terms.

#include "g_q1vm.h"

qc_builtin_t qc_builtins[QC_MAX_BUILTINS];

static cvar_t *g_q1_aim;

/*
==============================================================================

FILES, MODELS AND SOUNDS

A Quake 1 name goes out as "id1/<episode>/<name>": that episode's copy, then
id1's (files.c). The model index the progs see is Quake II's own, so "*N" is
N + 1 in both games and the world is 1.

==============================================================================
*/

typedef struct {
    char    name[MAX_QPATH];    // as the progs named it
    int     flags;              // a .mdl's header flags (Q1_MF_*)
    bool    bounds_known;
    vec3_t  mins, maxs;         // what setmodel gives an entity
    int     frame_slots;        // into q1_anim_slots, numframes of them; -1 none
    int     skin_slots;         // into q1_anim_slots, numskins of them; -1 none
    int     numframes, numskins;
    bool    anim;               // frame_slots or skin_slots is set
    bool    sync_rand;          // ST_RAND: each entity at its own point in the cycle
} q1_model_t;

static q1_model_t q1_models[MAX_MODELS];

/*
FRAME AND SKIN GROUPS. A .mdl frame or skin may be a group (the torch flame's
six flickers, the spike's four spins, the rogue sphere's skins) that Quake
cycled by the clock. model_q1.c builds every member as a frame or skin of its
own, in order, as long as they fit an .md2 (the same limits below); here is
where each frame's and skin's members start and each one's end in the cycle.
*/
#define Q1_EXPAND_MAX_FRAMES    1024    // MD2_MAX_FRAMES
#define Q1_EXPAND_MAX_SKINS     32      // MAX_ALIAS_SKINS, MD2_MAX_SKINS
#define Q1_MAX_ANIM_SLOTS       4096
#define Q1_MAX_ANIM_TIMES       8192

typedef struct {
    short   first;          // the renderer's frame (skin) of member 0
    short   count;          // members; 1 for a plain frame
    int     times;          // into q1_anim_times: each member's end, cumulative
} q1_anim_slot_t;

static q1_anim_slot_t   q1_anim_slots[Q1_MAX_ANIM_SLOTS];
static float            q1_anim_times[Q1_MAX_ANIM_TIMES];
static int              q1_num_anim_slots, q1_num_anim_times;

static int q1_rd32(const byte *p)
{
    return LittleLong(*(const int *)p);
}

static float q1_rdf(const byte *p)
{
    return LittleFloat(*(const float *)p);
}

// a slot per frame or skin of the run at (o) when any is a group; -1 if none
// is, or the run is not all there. (header) is the bytes before a single's
// data, (group_header) a group's before its intervals, (size) one member's.
static int q1_anim_read(const byte *buf, size_t len, size_t *o, int count, size_t header,
                        size_t group_header, size_t size, int max_expanded)
{
    size_t p = *o;
    int members = 0;
    bool groups = false;

    // walk it: are there groups, and do they fit what model_q1.c expands?
    for (int i = 0; i < count; i++) {
        if (p + 8 > len)
            return -1;
        if (q1_rd32(buf + p) == 0) {
            p += header + size;
            members++;
        } else {
            int n = q1_rd32(buf + p + 4);
            if (n < 1 || n > 1024)
                return -1;
            p += group_header + 4 * (size_t)n + size * n;
            members += n;
            groups = true;
        }
        if (p > len)
            return -1;
    }
    const size_t end = p;

    if (!groups || members > max_expanded
        || q1_num_anim_slots + count > Q1_MAX_ANIM_SLOTS
        || q1_num_anim_times + members > Q1_MAX_ANIM_TIMES) {
        *o = end;
        return -1;
    }

    const int slots = q1_num_anim_slots;
    int first = 0;
    p = *o;
    for (int i = 0; i < count; i++) {
        q1_anim_slot_t *s = &q1_anim_slots[q1_num_anim_slots++];
        s->first = first;
        s->times = q1_num_anim_times;
        if (q1_rd32(buf + p) == 0) {
            s->count = 1;
            q1_anim_times[q1_num_anim_times++] = 0.1f;
            p += header + size;
        } else {
            int n = q1_rd32(buf + p + 4);
            s->count = n;
            for (int k = 0; k < n; k++)
                q1_anim_times[q1_num_anim_times++] = q1_rdf(buf + p + group_header + 4 * k);
            p += group_header + 4 * (size_t)n + size * n;
        }
        first += s->count;
    }
    *o = end;
    return slots;
}

// an .mdl's groups (model_q1.c draws an md5 instead, with no groups, when the
// model has one)
static void q1_read_anim(q1_model_t *m, const byte *buf, size_t len)
{
    char stem[MAX_QPATH], path[MAX_QPATH];
    byte *md5;

    if (len < 84)
        return;
    const int numskins = q1_rd32(buf + 48), sw = q1_rd32(buf + 52), sh = q1_rd32(buf + 56);
    const int nv = q1_rd32(buf + 60), nt = q1_rd32(buf + 64), nf = q1_rd32(buf + 68);
    if (numskins < 0 || numskins > 32 || sw < 1 || sw > 4096 || sh < 1 || sh > 4096
        || nv < 3 || nv > 4096 || nt < 1 || nf < 1 || nf > Q1_EXPAND_MAX_FRAMES)
        return;

    COM_StripExtension(stem, m->name, sizeof(stem));
    if (Q_concat(path, sizeof(path), "id1/", qvm.episode, "/", stem, ".md5mesh") < sizeof(path)
        && gi.LoadFile(path, (void **)&md5) > 0 && md5) {
        gi.FreeFile(md5);
        return;
    }

    size_t o = 84;
    m->numskins = numskins;
    m->skin_slots = q1_anim_read(buf, len, &o, numskins, 4, 8, (size_t)sw * sh, Q1_EXPAND_MAX_SKINS);
    o += 12 * (size_t)nv + 16 * (size_t)nt;
    m->numframes = nf;
    m->frame_slots = q1_anim_read(buf, len, &o, nf, 4, 16, 24 + 4 * (size_t)nv, Q1_EXPAND_MAX_FRAMES);
    m->sync_rand = q1_rd32(buf + 72) != 0;
    m->anim = m->frame_slots >= 0 || m->skin_slots >= 0;
}

// Quake's R_AliasSetupFrame: the member whose end is past (time) into the cycle
static int q1_anim_member(const q1_anim_slot_t *s, float time)
{
    const float *t = q1_anim_times + s->times;
    const float full = t[s->count - 1];
    int i;

    if (s->count < 2 || full <= 0)
        return s->first;
    float target = time - (int)(time / full) * full;
    for (i = 0; i < s->count - 1; i++)
        if (t[i] > target)
            break;
    return s->first + i;
}

/*
The renderer's frame and skin for an entity (num) showing QuakeC's (*frame)
and (*skin) of model (modelindex) at (time). Unchanged for a model with no
groups; out of range is the first, as in Quake.
*/
void Q1_AnimPick(int modelindex, int num, float time, int *frame, int *skin)
{
    if (modelindex <= 0 || modelindex >= MAX_MODELS)
        return;
    const q1_model_t *m = &q1_models[modelindex];
    if (!m->anim)
        return;

    // Quake's syncbase: 0 to 1 s, random per entity for ST_RAND
    if (m->sync_rand)
        time += (float)(((unsigned)num * 2654435761u >> 16) & 0x7fff) / 0x7fff;

    if (m->frame_slots >= 0) {
        int f = (*frame >= 0 && *frame < m->numframes) ? *frame : 0;
        *frame = q1_anim_member(&q1_anim_slots[m->frame_slots + f], time);
    }
    if (m->skin_slots >= 0) {
        int s = (*skin >= 0 && *skin < m->numskins) ? *skin : 0;
        *skin = q1_anim_member(&q1_anim_slots[m->skin_slots + s], time);
    }
}

const char *Q1_FilePath(const char *path, char *buffer, size_t size)
{
    Q_snprintf(buffer, size, "id1/%s/%s", qvm.episode, path);
    return buffer;
}

const char *Q1_ModelName(int modelindex)
{
    if (modelindex <= 0 || modelindex >= MAX_MODELS)
        return "";
    return q1_models[modelindex].name;
}

int Q1_ModelFlags(int modelindex)
{
    if (modelindex <= 0 || modelindex >= MAX_MODELS)
        return 0;
    return q1_models[modelindex].flags;
}

static bool q1_has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !Q_stricmp(s + a - b, suffix);
}

// what Quake's server knew of a model: an alias model's or a sprite's box is
// the fixed 32 unit cube Quake gave every one ("FIXME: do this right"); an
// outside .bsp's is its model 0's, read here
static void q1_read_model_info(q1_model_t *m)
{
    char    path[MAX_QPATH];
    byte    *buf;
    int     len;

    VectorSet(m->mins, -16, -16, -16);
    VectorSet(m->maxs, 16, 16, 16);
    m->bounds_known = true;

    m->frame_slots = m->skin_slots = -1;

    if (q1_has_suffix(m->name, ".mdl")) {
        len = gi.LoadFile(Q1_FilePath(m->name, path, sizeof(path)), (void **)&buf);
        if (len >= 84 && buf && !memcmp(buf, "IDPO", 4)) {
            m->flags = LittleLong(*(int *)(buf + 76));
            q1_read_anim(m, buf, len);
        }
        if (buf)
            gi.FreeFile(buf);
        return;
    }

    if (q1_has_suffix(m->name, ".bsp")) {
        len = gi.LoadFile(Q1_FilePath(m->name, path, sizeof(path)), (void **)&buf);
        if (len >= 4 + 15 * 8 && buf) {
            int version = LittleLong(*(int *)buf);
            // version 29 or "BSP2": the lump table is the same, models is 14
            if (version == 29 || !memcmp(buf, "BSP2", 4)) {
                int ofs = LittleLong(*(int *)(buf + 4 + 14 * 8));
                int size = LittleLong(*(int *)(buf + 4 + 14 * 8 + 4));
                if (ofs > 0 && size >= 24 && ofs + 24 <= len) {
                    const float *f = (const float *)(buf + ofs);
                    for (int i = 0; i < 3; i++) {
                        m->mins[i] = LittleFloat(f[i]);
                        m->maxs[i] = LittleFloat(f[3 + i]);
                    }
                }
            }
        }
        if (buf)
            gi.FreeFile(buf);
    }
}

int Q1_ModelIndex(const char *name)
{
    char path[MAX_QPATH];
    int index;

    if (!name || !*name)
        return 0;

    if (name[0] == '*') {
        index = gi.modelindex(name);
    } else {
        index = gi.modelindex(Q1_FilePath(name, path, sizeof(path)));
    }

    if (index > 0 && index < MAX_MODELS && !q1_models[index].name[0]) {
        q1_model_t *m = &q1_models[index];
        Q_strlcpy(m->name, name, sizeof(m->name));
        if (name[0] != '*')
            q1_read_model_info(m);
    }
    return index;
}

static char q1_sounds[MAX_SOUNDS][MAX_QPATH];

int Q1VM_SoundIndex(const char *name)
{
    char path[MAX_QPATH];
    int index;

    if (!name || !*name)
        return 0;
    Q_snprintf(path, sizeof(path), "#id1/%s/sound/%s", qvm.episode, name);
    index = gi.soundindex(path);
    if (index > 0 && index < MAX_SOUNDS && !q1_sounds[index][0])
        Q_strlcpy(q1_sounds[index], name, sizeof(q1_sounds[index]));
    return index;
}

// whether the progs precached it: Quake played nothing for a sound they had not
bool Q1_SoundPrecached(const char *name)
{
    for (int i = 1; i < MAX_SOUNDS; i++)
        if (q1_sounds[i][0] && !strcmp(q1_sounds[i], name))
            return true;
    return false;
}

// setmodel's size for the entity: an inline model's own box, else the above
void Q1_SetModelBounds(int num, int modelindex)
{
    qc_entvars_t *ev = QC_EV(num);
    vec3_t mins = { 0, 0, 0 }, maxs = { 0, 0, 0 };

    if (modelindex > 0 && modelindex < MAX_MODELS) {
        q1_model_t *m = &q1_models[modelindex];
        if (m->name[0] == '*' && num > 0) {
            edict_t *e = &g_edicts[num];
            e->inuse = true;            // (the engine links only those)
            e->s.number = num;
            gi.setmodel(e, m->name);    // links it; Q1_LinkEdict relinks below
            VectorCopy(e->mins, mins);
            VectorCopy(e->maxs, maxs);
        } else if (m->bounds_known) {
            VectorCopy(m->mins, mins);
            VectorCopy(m->maxs, maxs);
        }
    }

    VectorCopy(mins, ev->mins);
    VectorCopy(maxs, ev->maxs);
    VectorSubtract(maxs, mins, ev->size);
    Q1_LinkEdict(num, false);
}

void Q1_StartSound(int ent, int channel, const char *sample, float volume, float attenuation)
{
    qc_entvars_t *ev = QC_EV(ent);
    vec3_t org;
    int index;

    clamp(volume, 0, 1);
    clamp(attenuation, 0, 4);

    index = Q1VM_SoundIndex(sample);
    if (!index)
        return;

    // Quake plays an entity's sound from the middle of its box
    for (int i = 0; i < 3; i++)
        org[i] = ev->origin[i] + 0.5f * (ev->mins[i] + ev->maxs[i]);

    // Quake's channel 0 was "any free one" and 1-7 replaced each other, like
    // Quake II's.
    //
    // EVERY CLIENT GETS IT, as Quake sent every sound to every client and let the
    // distance fade it (SV_StartSound wrote it to sv.datagram). Culled by the PHS
    // here, a sound from inside the world's solid was heard by no one - its leaf
    // has no cluster - and Quake's lifts sink into the floor: 150 of the 178
    // func_plats in id1/hipnotic/rogue/dopa/mg1 put the middle of their box in
    // solid at the bottom, so a lift setting off from there, and landing there,
    // made no sound (Matt, 2026-11-21: "this lift isn't playing a sound").
    gi.positioned_sound(org, &g_edicts[ent], (channel & 7) | CHAN_NO_PHS_ADD, index, volume, attenuation, 0);
}

// a sound only (client) hears - the rerelease's localsound, Quake's svc_localsound
void Q1_LocalSound(int client, const char *sample)
{
    int index = Q1VM_SoundIndex(sample);
    if (!index || client < 1 || client > game.maxclients)
        return;

    // the engine has no "to one player" sound; the player's own entity with
    // no attenuation is what that sounds like to them
    edict_t *e = &g_edicts[client];
    if (!e->inuse)
        return;
    gi.sound(e, CHAN_AUTO, index, 1, ATTN_NONE, 0);
}

/*
==============================================================================

STRINGS FOR PRINTING

The rerelease's prints take a format key and arguments: "$qc_got_item" and
"$qc_shotgun" make "You got Shotgun". Without a placeholder the arguments
are joined, as Quake's PF_VarString did. (vkQuake's PF_VarString.)

==============================================================================
*/

static const char *q1_var_string(int first)
{
    static char out[1024];
    const char *format;
    size_t len = 0;

    out[0] = 0;
    if (first >= qvm.argc)
        return out;

    format = QC_Localize(QC_G_STRING(QC_OFS_PARM0 + first * 3));

    if (strchr(format, '{')) {
        int next = first + 1;
        const char *s = format;
        while (*s && len < sizeof(out) - 1) {
            if (*s == '{') {
                const char *p = s + 1;
                int arg = 0;
                bool digits = false;
                while (*p >= '0' && *p <= '9') {
                    arg = arg * 10 + (*p++ - '0');
                    digits = true;
                }
                if (*p == '}') {
                    int idx = digits ? first + 1 + arg : next++;
                    const char *insert = idx < qvm.argc ? QC_Localize(QC_G_STRING(QC_OFS_PARM0 + idx * 3)) : "";
                    size_t n = strlen(insert);
                    if (n > sizeof(out) - 1 - len)
                        n = sizeof(out) - 1 - len;
                    memcpy(out + len, insert, n);
                    len += n;
                    s = p + 1;
                    continue;
                }
            }
            out[len++] = *s++;
        }
        out[len] = 0;
    } else {
        for (int i = first; i < qvm.argc; i++)
            Q_strlcat(out, QC_Localize(QC_G_STRING(QC_OFS_PARM0 + i * 3)), sizeof(out));
    }
    return out;
}

static edict_t *q1_client_edict(int num)
{
    if (num < 1 || num > game.maxclients)
        return NULL;
    edict_t *e = &g_edicts[num];
    if (!e->inuse || !e->client)
        return NULL;
    return e;
}

/*
==============================================================================

THE MESSAGES THE PROGS WRITE

WriteByte & co. append to one of Quake's buffers - the unreliable broadcast,
one client, everyone reliably, the signon. Each call is kept as a token per
buffer, and as soon as the tokens make up a whole message Quake would have
understood, it is decoded and re-sent as Quake II's equivalent.

==============================================================================
*/

enum { MT_BYTE, MT_CHAR, MT_SHORT, MT_LONG, MT_COORD, MT_ANGLE, MT_STRING, MT_ENTITY };

#define Q1_MSG_TOKENS   32

typedef struct {
    int     type;
    float   f;          // every number
    char    s[512];     // MT_STRING
} q1_token_t;

typedef struct {
    q1_token_t  tok[Q1_MSG_TOKENS];
    int         num;
    int         client;     // MSG_ONE: msg_entity when the message began
} q1_msgbuf_t;

static q1_msgbuf_t q1_msg[4];

// stuffcmd's text for each player, until a whole line is in (QuakeC often
// sends a command a piece at a time: "fog ", ftos(density), " ", ...)
static char q1_stuff[MAX_CLIENTS][256];

// and localcmd's, for the server's console (PF_localcmd)
static char q1_localcmd_buf[256];

void Q1_ResetMessages(void)
{
    memset(q1_msg, 0, sizeof(q1_msg));
    memset(q1_stuff, 0, sizeof(q1_stuff));
    q1_localcmd_buf[0] = 0;
}

static int q1_tok_int(const q1_msgbuf_t *m, int i)
{
    return (int)m->tok[i].f;
}

static void q1_tok_vec(const q1_msgbuf_t *m, int i, vec3_t v)
{
    v[0] = m->tok[i].f;
    v[1] = m->tok[i + 1].f;
    v[2] = m->tok[i + 2].f;
}

static void q1_send(int dest, const q1_msgbuf_t *m, const vec3_t pos, bool reliable)
{
    if (dest == Q1_MSG_ONE) {
        edict_t *e = q1_client_edict(m->client);
        if (e)
            gi.unicast(e, reliable);
        else
            gi.multicast(vec3_origin, MULTICAST_ALL);   // must flush the write
        return;
    }
    if (dest == Q1_MSG_BROADCAST && pos)
        gi.multicast(pos, MULTICAST_PVS);
    else
        gi.multicast(vec3_origin, reliable ? MULTICAST_ALL_R : MULTICAST_ALL);
}

// tokens a temp entity takes after [svc][kind], or -1 for an unknown kind
static int q1_te_length(int kind)
{
    switch (kind) {
    case TE_Q1_SPIKE:
    case TE_Q1_SUPERSPIKE:
    case TE_Q1_GUNSHOT:
    case TE_Q1_EXPLOSION:
    case TE_Q1_TAREXPLOSION:
    case TE_Q1_WIZSPIKE:
    case TE_Q1_KNIGHTSPIKE:
    case TE_Q1_LAVASPLASH:
    case TE_Q1_TELEPORT:
        return 3;
    case TE_Q1_LIGHTNING1:
    case TE_Q1_LIGHTNING2:
    case TE_Q1_LIGHTNING3:
    case TE_Q1_BEAM:
        return 7;
    case TE_Q1_EXPLOSION2:
        return 5;
    default:
        return -1;
    }
}

static void q1_temp_entity(int dest, const q1_msgbuf_t *m)
{
    int kind = q1_tok_int(m, 1);
    vec3_t pos, end;

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(kind);

    switch (kind) {
    case TE_Q1_LIGHTNING1:
    case TE_Q1_LIGHTNING2:
    case TE_Q1_LIGHTNING3:
    case TE_Q1_BEAM:
        q1_tok_vec(m, 3, pos);
        q1_tok_vec(m, 6, end);
        gi.WriteShort(q1_tok_int(m, 2));
        gi.WritePosition(pos);
        gi.WritePosition(end);
        break;
    case TE_Q1_EXPLOSION2:
        q1_tok_vec(m, 2, pos);
        gi.WritePosition(pos);
        gi.WriteByte(q1_tok_int(m, 5));
        gi.WriteByte(q1_tok_int(m, 6));
        break;
    default:
        q1_tok_vec(m, 2, pos);
        gi.WritePosition(pos);
        break;
    }

    q1_send(dest, m, pos, false);
}

// tokens a whole message takes, 0 for "not known yet", -1 for "can't tell"
static int q1_message_length(const q1_msgbuf_t *m)
{
    if (m->num < 1)
        return 0;

    switch (q1_tok_int(m, 0)) {
    case Q1_SVC_TEMPENTITY: {
        if (m->num < 2)
            return 0;
        int n = q1_te_length(q1_tok_int(m, 1));
        return n < 0 ? -1 : 2 + n;
    }
    case Q1_SVC_INTERMISSION:
    case Q1_SVC_SELLSCREEN:
    case Q1_SVC_KILLEDMONSTER:
    case Q1_SVC_FOUNDSECRET:
    case Q1_SVC_SPAWNEDMONSTER:
    case Q1_SVC_BACKTOLOBBY:
        return 1;
    case Q1_SVC_FINALE:
    case Q1_SVC_CUTSCENE:
    case Q1_SVC_CENTERPRINT:
    case Q1_SVC_ACHIEVEMENT:
    case 8:     // svc_print
    case 9:     // svc_stufftext
        return 2;
    case Q1_SVC_CDTRACK:
        return 3;
    case Q1_SVC_UPDATESTAT:
        return 3;
    case Q1_SVC_SETANGLE:
        return 4;
    case Q1_SVC_SPAWNSTATICSOUND:
        return 7;
    default:
        return -1;
    }
}

static void q1_decode(int dest)
{
    q1_msgbuf_t *m = &q1_msg[dest];

    while (m->num) {
        int len = q1_message_length(m);
        if (len == 0)
            return;     // wait for the rest

        if (len < 0 || len > Q1_MSG_TOKENS) {
            static int warned;
            if (warned++ < 8)
                gi.dprintf("Quake 1 game: can't decode message %d (%d writes), dropped\n",
                           q1_tok_int(m, 0), m->num);
            m->num = 0;
            return;
        }
        if (m->num < len)
            return;

        int svc = q1_tok_int(m, 0);
        switch (svc) {
        case Q1_SVC_TEMPENTITY:
            q1_temp_entity(dest, m);
            break;

        case Q1_SVC_INTERMISSION:
            Q1VM_Intermission(1, NULL);
            break;

        case Q1_SVC_FINALE:
            Q1VM_Intermission(2, QC_Localize(m->tok[1].s));
            break;

        case Q1_SVC_CUTSCENE:
            Q1VM_Intermission(3, QC_Localize(m->tok[1].s));
            break;

        case Q1_SVC_SELLSCREEN:
            break;

        case Q1_SVC_CDTRACK:
            gi.configstring(CS_CDTRACK, va("%i", q1_tok_int(m, 1)));
            break;

        case Q1_SVC_CENTERPRINT: {
            const char *s = QC_Localize(m->tok[1].s);
            if (dest == Q1_MSG_ONE) {
                edict_t *e = q1_client_edict(m->client);
                if (e)
                    gi.centerprintf(e, "%s", s);
            } else {
                for (int i = 1; i <= game.maxclients; i++) {
                    edict_t *e = q1_client_edict(i);
                    if (e)
                        gi.centerprintf(e, "%s", s);
                }
            }
            break;
        }

        case 8: {   // svc_print
            const char *s = QC_Localize(m->tok[1].s);
            if (dest == Q1_MSG_ONE) {
                edict_t *e = q1_client_edict(m->client);
                if (e)
                    gi.cprintf(e, PRINT_HIGH, "%s", s);
            } else {
                gi.bprintf(PRINT_HIGH, "%s", s);
            }
            break;
        }

        case Q1_SVC_SETANGLE:
            if (dest == Q1_MSG_ONE) {
                qc_entvars_t *ev = QC_EV(m->client);
                if (m->client >= 1 && m->client <= game.maxclients) {
                    q1_tok_vec(m, 1, ev->angles);
                    ev->fixangle = 1;
                }
            }
            break;

        case Q1_SVC_SPAWNSTATICSOUND:
            // (needs Quake's sound index; ambientsound is what the progs use)
            break;

        default:
            // the client's own bookkeeping (killed monster, found secret, a
            // stat, an achievement, a chat line, stufftext): the HUD reads
            // the progs' globals instead, and the rest has no Quake II side
            break;
        }

        // take the message off the front
        memmove(m->tok, m->tok + len, sizeof(q1_token_t) * (m->num - len));
        m->num -= len;
    }
}

static void q1_write(int type, float f, const char *s)
{
    int dest = (int)QC_G_FLOAT(QC_OFS_PARM0);
    if (dest < 0 || dest > 3) {
        QC_RunError("WriteDest: bad destination %d", dest);
        return;
    }

    q1_msgbuf_t *m = &q1_msg[dest];
    if (m->num >= Q1_MSG_TOKENS) {
        gi.dprintf("Quake 1 game: message overflow, dropped\n");
        m->num = 0;
    }
    if (m->num == 0)
        m->client = QC_FromProg(qvm.g->msg_entity);

    q1_token_t *t = &m->tok[m->num++];
    t->type = type;
    t->f = f;
    t->s[0] = 0;
    if (s)
        Q_strlcpy(t->s, s, sizeof(t->s));

    q1_decode(dest);
}

// at the end of a frame anything half-written is dropped, as a broken message
void Q1_MessagesFrame(void)
{
    for (int i = 0; i < 4; i++) {
        if (q1_msg[i].num) {
            static int warned;
            if (warned++ < 8)
                gi.dprintf("Quake 1 game: incomplete message %d dropped\n", q1_tok_int(&q1_msg[i], 0));
            q1_msg[i].num = 0;
        }
    }
}

static void PF_WriteByte(void)   { q1_write(MT_BYTE,   (float)((int)QC_G_FLOAT(QC_OFS_PARM1) & 255), NULL); }
static void PF_WriteChar(void)   { q1_write(MT_CHAR,   QC_G_FLOAT(QC_OFS_PARM1), NULL); }
static void PF_WriteShort(void)  { q1_write(MT_SHORT,  QC_G_FLOAT(QC_OFS_PARM1), NULL); }
static void PF_WriteLong(void)   { q1_write(MT_LONG,   QC_G_FLOAT(QC_OFS_PARM1), NULL); }
static void PF_WriteCoord(void)  { q1_write(MT_COORD,  QC_G_FLOAT(QC_OFS_PARM1), NULL); }
static void PF_WriteAngle(void)  { q1_write(MT_ANGLE,  QC_G_FLOAT(QC_OFS_PARM1), NULL); }
static void PF_WriteString(void) { q1_write(MT_STRING, 0, QC_G_STRING(QC_OFS_PARM1)); }
static void PF_WriteEntity(void) { q1_write(MT_ENTITY, (float)QC_G_EDICTNUM(QC_OFS_PARM1), NULL); }

/*
==============================================================================

BUILTINS

==============================================================================
*/

static void PF_makevectors(void)
{
    AngleVectors(QC_G_VECTOR(QC_OFS_PARM0), qvm.g->v_forward, qvm.g->v_right, qvm.g->v_up);
}

static void PF_setorigin(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    VectorCopy(QC_G_VECTOR(QC_OFS_PARM1), QC_EV(e)->origin);
    Q1_LinkEdict(e, false);
}

static void PF_setsize(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    qc_entvars_t *ev = QC_EV(e);
    float *min = QC_G_VECTOR(QC_OFS_PARM1), *max = QC_G_VECTOR(QC_OFS_PARM2);

    for (int i = 0; i < 3; i++)
        if (min[i] > max[i])
            QC_RunError("backwards mins/maxs");

    VectorCopy(min, ev->mins);
    VectorCopy(max, ev->maxs);
    VectorSubtract(max, min, ev->size);
    Q1_LinkEdict(e, false);
}

static void PF_setmodel(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    int s = QC_G_INT(QC_OFS_PARM1);
    const char *m = QC_Str(s);
    qc_entvars_t *ev = QC_EV(e);

    // Quake demanded a precache; a late one is let through, as modern ports do
    int index = Q1_ModelIndex(m);

    ev->model = s;
    ev->modelindex = index;
    qvm.ed[e].mdl_flags = Q1_ModelFlags(index);
    Q1_SetModelBounds(e, index);
}

static void PF_break(void)
{
    gi.dprintf("Quake 1 game: break statement\n");
}

static void PF_random(void)
{
    QC_RETURN_FLOAT((rand() & 0x7fff) / ((float)0x7fff));
}

static void PF_sound(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    int channel = (int)QC_G_FLOAT(QC_OFS_PARM1);
    const char *sample = QC_G_STRING(QC_OFS_PARM2);
    float volume = QC_G_FLOAT(QC_OFS_PARM3);
    float attenuation = QC_G_FLOAT(QC_OFS_PARM4);

    Q1_StartSound(e, channel, sample, volume, attenuation);
}

static void PF_normalize(void)
{
    float *v = QC_G_VECTOR(QC_OFS_PARM0);
    vec3_t out;
    double len = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]);

    if (len == 0) {
        VectorClear(out);
    } else {
        len = 1 / len;
        out[0] = v[0] * len;
        out[1] = v[1] * len;
        out[2] = v[2] * len;
    }
    VectorCopy(out, QC_G_VECTOR(QC_OFS_RETURN));
}

static void PF_error(void)
{
    const char *s = q1_var_string(0);
    gi.dprintf("======SERVER ERROR in %s:\n%s\n", QC_Str(qvm.xfunction->s_name), s);
    QC_PrintEdict(QC_FromProg(qvm.g->self));
    QC_RunError("error: %s", s);
}

// Quake ended the game here; the ports since only drop the entity
static void PF_objerror(void)
{
    const char *s = q1_var_string(0);
    int self = QC_FromProg(qvm.g->self);
    gi.dprintf("======OBJECT ERROR in %s:\n%s\n", QC_Str(qvm.xfunction->s_name), s);
    QC_PrintEdict(self);
    if (self)
        QC_Free(self);
}

static void PF_vlen(void)
{
    float *v = QC_G_VECTOR(QC_OFS_PARM0);
    QC_RETURN_FLOAT((float)sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]));
}

static void PF_vectoyaw(void)
{
    float *v = QC_G_VECTOR(QC_OFS_PARM0);
    float yaw;

    if (v[1] == 0 && v[0] == 0) {
        yaw = 0;
    } else {
        yaw = (int)(atan2(v[1], v[0]) * 180 / M_PI);
        if (yaw < 0)
            yaw += 360;
    }
    QC_RETURN_FLOAT(yaw);
}

static void PF_vectoangles(void)
{
    float *v = QC_G_VECTOR(QC_OFS_PARM0);
    float forward, yaw, pitch;

    if (v[1] == 0 && v[0] == 0) {
        yaw = 0;
        pitch = v[2] > 0 ? 90 : 270;
    } else {
        yaw = (int)(atan2(v[1], v[0]) * 180 / M_PI);
        if (yaw < 0)
            yaw += 360;
        forward = sqrt(v[0] * v[0] + v[1] * v[1]);
        pitch = (int)(atan2(v[2], forward) * 180 / M_PI);
        if (pitch < 0)
            pitch += 360;
    }

    QC_G_FLOAT(QC_OFS_RETURN + 0) = pitch;
    QC_G_FLOAT(QC_OFS_RETURN + 1) = yaw;
    QC_G_FLOAT(QC_OFS_RETURN + 2) = 0;
}

static void PF_spawn(void)
{
    QC_RETURN_EDICT(QC_Alloc());
}

static void PF_remove(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    if (e <= game.maxclients) {
        gi.dprintf("Quake 1 game: tried to remove entity %d\n", e);
        return;
    }
    QC_Free(e);
}

static void q1_set_trace_globals(const q1trace_t *t)
{
    qvm.g->trace_allsolid = t->allsolid;
    qvm.g->trace_startsolid = t->startsolid;
    qvm.g->trace_fraction = t->fraction;
    qvm.g->trace_inwater = t->inwater;
    qvm.g->trace_inopen = t->inopen;
    VectorCopy(t->endpos, qvm.g->trace_endpos);
    VectorCopy(t->plane.normal, qvm.g->trace_plane_normal);
    qvm.g->trace_plane_dist = t->plane.dist;
    qvm.g->trace_ent = QC_ToProg(t->ent >= 0 ? t->ent : 0);
}

static void PF_traceline(void)
{
    vec3_t v1, v2;
    VectorCopy(QC_G_VECTOR(QC_OFS_PARM0), v1);
    VectorCopy(QC_G_VECTOR(QC_OFS_PARM1), v2);
    int nomonsters = (int)QC_G_FLOAT(QC_OFS_PARM2);
    int ent = QC_G_EDICTNUM(QC_OFS_PARM3);

    q1trace_t t = Q1_Move(v1, vec3_origin, vec3_origin, v2, nomonsters, ent);
    q1_set_trace_globals(&t);
}

/*
Returns a client (or object that has a client enemy) that would be a valid
target. If there are more than one valid options, they are cycled each frame.
If (self.origin + self.viewofs) is not in the PVS of the current target, it is
not returned at all. (The engine's inPVS stands in for Quake's own PVS bytes.)
*/
static int q1_newcheckclient(int check)
{
    int i;

    // cycle to the next one
    if (check < 1)
        check = 1;
    if (check > game.maxclients)
        check = game.maxclients;

    i = (check == game.maxclients) ? 1 : check + 1;

    for (;; i++) {
        if (i == game.maxclients + 1)
            i = 1;

        if (i == check)
            break;  // didn't find anything else

        if (qvm.ed[i].free || !q1_client_edict(i))
            continue;
        qc_entvars_t *ev = QC_EV(i);
        if (ev->health <= 0)
            continue;
        if ((int)ev->flags & Q1_FL_NOTARGET)
            continue;

        // anything that is a client, or has a client as an enemy
        break;
    }
    return i;
}

static void PF_checkclient(void)
{
    // find a new check if on a new frame
    if (qvm.time - qvm.lastchecktime >= 0.1f) {
        qvm.lastcheck = q1_newcheckclient(qvm.lastcheck);
        qvm.lastchecktime = qvm.time;
    }

    // return check if it might be visible
    int check = qvm.lastcheck;
    if (check < 1 || check > game.maxclients || qvm.ed[check].free || !q1_client_edict(check) ||
        QC_EV(check)->health <= 0) {
        QC_RETURN_EDICT(0);
        return;
    }

    // if current entity can't possibly see the check entity, return 0
    qc_entvars_t *self = QC_EV(QC_FromProg(qvm.g->self));
    qc_entvars_t *ce = QC_EV(check);
    vec3_t view, cview;
    VectorAdd(self->origin, self->view_ofs, view);
    VectorAdd(ce->origin, ce->view_ofs, cview);
    if (!gi.inPVS(view, cview)) {
        QC_RETURN_EDICT(0);
        return;
    }

    // might be able to see it
    QC_RETURN_EDICT(check);
}

// one whole line the progs stuffed into a player's console
static void q1_stuff_line(edict_t *ent, int e, const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line)
        return;

    // what Quake's client did with the commands the progs send it: "bf" is
    // the item pickup flash
    if (!strcmp(line, "bf")) {
        Q1VM_BonusFlash(e);
        return;
    }
    // letting go of the fire button (a death, a weapon that must not
    // repeat): Quake II's client has the same command
    if (!strcmp(line, "-attack")) {
        gi.WriteByte(svc_stufftext);
        gi.WriteString("-attack\n");
        gi.unicast(ent, true);
        return;
    }
    // MG1's FitzQuake "fog <density> <r> <g> <b> [fade s]": the client's
    // q1fog ("fog" there is the renderer's fog volumes)
    if (!strncmp(line, "fog", 3) && (line[3] == ' ' || !line[3])) {
        gi.WriteByte(svc_stufftext);
        gi.WriteString(va("q1fog%s\n", line + 3));
        gi.unicast(ent, true);
        return;
    }

    // the rest have no meaning here
    static int warned;
    if (warned++ < 16)
        gi.dprintf("Quake 1 game: stuffcmd \"%s\" ignored\n", line);
}

static void PF_stuffcmd(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    const char *s = QC_G_STRING(QC_OFS_PARM1);
    edict_t *ent = q1_client_edict(e);

    if (!ent)
        return;

    char *buf = q1_stuff[e - 1];
    size_t len = strlen(buf);

    for (; *s; s++) {
        if (*s == '\n' || *s == ';') {
            buf[len] = 0;
            q1_stuff_line(ent, e, buf);
            len = 0;
        } else if (len < sizeof(q1_stuff[0]) - 1) {
            buf[len++] = *s;
        }
    }
    buf[len] = 0;
}

static void PF_find(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    int f = QC_G_INT(QC_OFS_PARM1);
    const char *s = QC_G_STRING(QC_OFS_PARM2);

    for (e++; e < qvm.num_edicts; e++) {
        if (qvm.ed[e].free)
            continue;
        const char *t = QC_Str(QC_Field(e, f)->string);
        if (!strcmp(t, s)) {
            QC_RETURN_EDICT(e);
            return;
        }
    }
    QC_RETURN_EDICT(0);
}

static void PF_precache_file(void)
{
    // precache_file is only used to copy files with qcc, it does nothing
    QC_G_INT(QC_OFS_RETURN) = QC_G_INT(QC_OFS_PARM0);
}

static void PF_precache_sound(void)
{
    QC_G_INT(QC_OFS_RETURN) = QC_G_INT(QC_OFS_PARM0);
    Q1VM_SoundIndex(QC_G_STRING(QC_OFS_PARM0));
}

static void PF_precache_model(void)
{
    QC_G_INT(QC_OFS_RETURN) = QC_G_INT(QC_OFS_PARM0);
    Q1_ModelIndex(QC_G_STRING(QC_OFS_PARM0));
}

static void PF_findradius(void)
{
    int chain = 0;
    float *org = QC_G_VECTOR(QC_OFS_PARM0);
    float rad = QC_G_FLOAT(QC_OFS_PARM1);

    for (int i = 1; i < qvm.num_edicts; i++) {
        if (qvm.ed[i].free)
            continue;
        qc_entvars_t *ev = QC_EV(i);
        if (ev->solid == Q1_SOLID_NOT)
            continue;
        vec3_t eorg;
        for (int j = 0; j < 3; j++)
            eorg[j] = org[j] - (ev->origin[j] + (ev->mins[j] + ev->maxs[j]) * 0.5f);
        if (VectorLength(eorg) > rad)
            continue;
        ev->chain = QC_ToProg(chain);
        chain = i;
    }
    QC_RETURN_EDICT(chain);
}

static void PF_bprint(void)
{
    gi.bprintf(PRINT_HIGH, "%s", q1_var_string(0));
}

static void PF_sprint(void)
{
    edict_t *e = q1_client_edict(QC_G_EDICTNUM(QC_OFS_PARM0));
    const char *s = q1_var_string(1);
    if (e)
        gi.cprintf(e, PRINT_HIGH, "%s", s);
}

static void PF_centerprint(void)
{
    edict_t *e = q1_client_edict(QC_G_EDICTNUM(QC_OFS_PARM0));
    const char *s = q1_var_string(1);
    if (e && *s)
        gi.centerprintf(e, "%s", s);
}

static void PF_dprint(void)
{
    gi.dprintf("%s", q1_var_string(0));
}

static void PF_ftos(void)
{
    char buf[64];
    float v = QC_G_FLOAT(QC_OFS_PARM0);

    if (v == (int)v)
        Q_snprintf(buf, sizeof(buf), "%d", (int)v);
    else
        Q_snprintf(buf, sizeof(buf), "%5.1f", v);
    QC_G_INT(QC_OFS_RETURN) = QC_TempString(buf);
}

static void PF_fabs(void)
{
    QC_RETURN_FLOAT(fabsf(QC_G_FLOAT(QC_OFS_PARM0)));
}

static void PF_vtos(void)
{
    char buf[128];
    float *v = QC_G_VECTOR(QC_OFS_PARM0);
    Q_snprintf(buf, sizeof(buf), "'%5.1f %5.1f %5.1f'", v[0], v[1], v[2]);
    QC_G_INT(QC_OFS_RETURN) = QC_TempString(buf);
}

static void PF_coredump(void)
{
    for (int i = 0; i < qvm.num_edicts; i++)
        QC_PrintEdict(i);
}

static void PF_traceon(void)
{
}

static void PF_traceoff(void)
{
}

static void PF_eprint(void)
{
    QC_PrintEdict(QC_G_EDICTNUM(QC_OFS_PARM0));
}

static void PF_walkmove(void)
{
    int self = QC_FromProg(qvm.g->self);
    float yaw = QC_G_FLOAT(QC_OFS_PARM0);
    float dist = QC_G_FLOAT(QC_OFS_PARM1);

    // save program state, because the move may call other progs
    qc_function_t *oldf = qvm.xfunction;
    int oldself = qvm.g->self;

    QC_RETURN_FLOAT(Q1_WalkMove(self, yaw, dist));

    // restore program state
    qvm.xfunction = oldf;
    qvm.g->self = oldself;
}

static void PF_droptofloor(void)
{
    QC_RETURN_FLOAT(Q1_DropToFloor(QC_FromProg(qvm.g->self)));
}

static void PF_lightstyle(void)
{
    int style = (int)QC_G_FLOAT(QC_OFS_PARM0);
    const char *val = QC_G_STRING(QC_OFS_PARM1);

    if (style < 0 || style >= MAX_LIGHTSTYLES)
        return;
    gi.configstring(CS_LIGHTS + style, val);
}

static void PF_rint(void)
{
    float f = QC_G_FLOAT(QC_OFS_PARM0);
    QC_RETURN_FLOAT(f > 0 ? (int)(f + 0.5f) : (int)(f - 0.5f));
}

static void PF_floor(void)
{
    QC_RETURN_FLOAT(floorf(QC_G_FLOAT(QC_OFS_PARM0)));
}

static void PF_ceil(void)
{
    QC_RETURN_FLOAT(ceilf(QC_G_FLOAT(QC_OFS_PARM0)));
}

static void PF_checkbottom(void)
{
    QC_RETURN_FLOAT(Q1_CheckBottom(QC_G_EDICTNUM(QC_OFS_PARM0)));
}

static void PF_pointcontents(void)
{
    float *v = QC_G_VECTOR(QC_OFS_PARM0);

    // a missile's touch function asking whether it went into the sky
    if (qvm.sky_impact && Distance(v, qvm.sky_point) < 1.0f) {
        QC_RETURN_FLOAT(Q1_CONTENTS_SKY);
        return;
    }
    QC_RETURN_FLOAT(Q1_PointContents(v));
}

static void PF_nextent(void)
{
    int i = QC_G_EDICTNUM(QC_OFS_PARM0);

    while (1) {
        i++;
        if (i >= qvm.num_edicts) {
            QC_RETURN_EDICT(0);
            return;
        }
        if (!qvm.ed[i].free) {
            QC_RETURN_EDICT(i);
            return;
        }
    }
}

#define Q1_DAMAGE_AIM   2

/*
Pick a vector for the player to shoot along. Quake's autoaim; g_q1_aim 1 (the
modern default, as QuakeSpasm's sv_aim) turns it off, 0.93 is Quake's.
*/
static void PF_aim(void)
{
    int ent = QC_G_EDICTNUM(QC_OFS_PARM0);
    qc_entvars_t *ev = QC_EV(ent);
    vec3_t start, dir, end, bestdir;
    float dist, bestdist;
    int bestent;
    q1trace_t tr;

    VectorCopy(ev->origin, start);
    start[2] += 20;

    // try sending a trace straight
    VectorCopy(qvm.g->v_forward, dir);
    VectorMA(start, 2048, dir, end);
    tr = Q1_Move(start, vec3_origin, vec3_origin, end, Q1_MOVE_NORMAL, ent);
    bool teams = teamplay && teamplay->value;

    if (tr.ent > 0 && QC_EV(tr.ent)->takedamage == Q1_DAMAGE_AIM &&
        (!teams || ev->team <= 0 || ev->team != QC_EV(tr.ent)->team)) {
        VectorCopy(qvm.g->v_forward, QC_G_VECTOR(QC_OFS_RETURN));
        return;
    }

    // try all possible entities
    VectorCopy(dir, bestdir);
    bestdist = g_q1_aim->value;
    bestent = 0;

    for (int i = 1; i < qvm.num_edicts; i++) {
        if (qvm.ed[i].free)
            continue;
        qc_entvars_t *check = QC_EV(i);
        if (check->takedamage != Q1_DAMAGE_AIM)
            continue;
        if (i == ent)
            continue;
        if (teams && ev->team > 0 && ev->team == check->team)
            continue;   // don't aim at teammate
        for (int j = 0; j < 3; j++)
            end[j] = check->origin[j] + 0.5f * (check->mins[j] + check->maxs[j]);
        VectorSubtract(end, start, dir);
        VectorNormalize(dir);
        dist = DotProduct(dir, qvm.g->v_forward);
        if (dist < bestdist)
            continue;   // to far to turn
        tr = Q1_Move(start, vec3_origin, vec3_origin, end, Q1_MOVE_NORMAL, ent);
        if (tr.ent == i) {
            // can shoot at this one
            bestdist = dist;
            bestent = i;
        }
    }

    if (bestent) {
        qc_entvars_t *b = QC_EV(bestent);
        VectorSubtract(b->origin, ev->origin, dir);
        dist = DotProduct(dir, qvm.g->v_forward);
        VectorScale(qvm.g->v_forward, dist, end);
        end[2] = dir[2];
        VectorNormalize(end);
        VectorCopy(end, QC_G_VECTOR(QC_OFS_RETURN));
    } else {
        VectorCopy(bestdir, QC_G_VECTOR(QC_OFS_RETURN));
    }
}

static void PF_changeyaw(void)
{
    Q1_ChangeYaw(QC_FromProg(qvm.g->self));
}

/*
The cvars the progs read. Quake's own names where Quake II has them; the
ones it does not (registered, the shareware check) answer as the full game.
*/
static void PF_cvar(void)
{
    const char *name = QC_G_STRING(QC_OFS_PARM0);

    if (!Q_stricmp(name, "registered")) {
        QC_RETURN_FLOAT(1);
        return;
    }
    if (!Q_stricmp(name, "skill")) {
        QC_RETURN_FLOAT(Q1VM_Skill());
        return;
    }

    cvar_t *var = gi.cvar(name, "", 0);
    QC_RETURN_FLOAT(var ? var->value : 0);
}

static void PF_cvar_set(void)
{
    const char *name = QC_G_STRING(QC_OFS_PARM0);
    const char *value = QC_G_STRING(QC_OFS_PARM1);

    // skill is latched in Quake II; the skill halls set it for the next map
    if (!Q_stricmp(name, "skill"))
        gi.cvar_forceset(name, value);
    else
        gi.cvar_set(name, value);
}

// one whole line of the server's console the progs wrote
static void q1_localcmd_line(const char *cmd)
{
    char arg[MAX_QPATH];

    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (!*cmd)
        return;

    // A player who died in single player: Quake restarted the level - the
    // rerelease with "changelevel <this map>", after SetChangeParms gave the
    // dead player new parms. Like PF_changelevel, the first in a level wins:
    // the engine runs it a frame later, and a held button sends one a frame.
    if (!strcmp(cmd, "restart") || !strncmp(cmd, "restart ", 8)) {
        if (!qvm.changelevel_issued) {
            qvm.changelevel_issued = true;
            Q1VM_ChangeLevel(NULL);
        }
        return;
    }
    if (!strncmp(cmd, "changelevel ", 12) || !strncmp(cmd, "map ", 4)) {
        const char *s = cmd + (cmd[0] == 'c' ? 12 : 4);
        while (*s == ' ')
            s++;
        Q_strlcpy(arg, s, sizeof(arg));
        for (char *p = arg; *p; p++) {
            if (*p == ' ' || *p == '\t') {
                *p = 0;
                break;
            }
        }
        if (arg[0] && !qvm.changelevel_issued) {
            qvm.changelevel_issued = true;
            Q1VM_ChangeLevel(arg);
        }
        return;
    }
    if (!strcmp(cmd, "disconnect") || !strcmp(cmd, "killserver")) {
        // the client falls back to the main menu
        gi.AddCommandString("killserver\n");
        return;
    }
    // the rerelease's menus (menu_credits after the last finale): none here
    if (!strncmp(cmd, "menu_", 5))
        return;
    if (!strncmp(cmd, "echo ", 5)) {
        gi.dprintf("%s\n", cmd + 5);
        return;
    }

    static int warned;
    if (warned++ < 16)
        gi.dprintf("Quake 1 game: localcmd \"%s\" ignored\n", cmd);
}

/*
The server's console, which QuakeC often writes a piece at a time
("changelevel ", mapname, "\n"): the text waits for its newline, as it did in
Quake's command buffer.
*/
static void PF_localcmd(void)
{
    const char *s = QC_G_STRING(QC_OFS_PARM0);
    size_t len = strlen(q1_localcmd_buf);

    for (; *s; s++) {
        if (*s == '\n' || *s == ';') {
            q1_localcmd_buf[len] = 0;
            q1_localcmd_line(q1_localcmd_buf);
            len = 0;
        } else if (len < sizeof(q1_localcmd_buf) - 1) {
            q1_localcmd_buf[len++] = *s;
        }
    }
    q1_localcmd_buf[len] = 0;
}

static void PF_particle(void)
{
    float *org = QC_G_VECTOR(QC_OFS_PARM0);
    float *dir = QC_G_VECTOR(QC_OFS_PARM1);
    int color = (int)QC_G_FLOAT(QC_OFS_PARM2);
    int count = (int)QC_G_FLOAT(QC_OFS_PARM3);

    gi.WriteByte(svc_temp_entity);
    gi.WriteByte(TE_Q1);
    gi.WriteByte(TE_Q1_PARTICLE);
    gi.WritePosition(org);
    for (int i = 0; i < 3; i++) {
        int v = (int)(dir[i] * 16);
        clamp(v, -128, 127);
        gi.WriteChar(v);
    }
    clamp(count, 0, 255);
    gi.WriteByte(count);
    gi.WriteByte(color & 255);
    gi.multicast(org, MULTICAST_PVS);
}

/*
A looping sound at a point, for the rest of the level: an entity of its own
the progs never see (a "static" one), its s.sound the loop.
*/
static void PF_ambientsound(void)
{
    float *pos = QC_G_VECTOR(QC_OFS_PARM0);
    const char *samp = QC_G_STRING(QC_OFS_PARM1);
    int index = Q1VM_SoundIndex(samp);

    if (!index)
        return;

    int n = QC_Alloc();
    qvm.ed[n].free = true;      // not the progs' to see...
    qvm.ed[n].isstatic = true;  // ...nor for QC_Alloc to hand out again

    edict_t *e = &g_edicts[n];
    memset(e, 0, sizeof(*e));
    e->s.number = n;
    e->inuse = true;
    VectorCopy(pos, e->s.origin);
    VectorCopy(pos, e->s.old_origin);   // or the sound comes from 0,0,0 for a frame
    e->s.sound = index;
    e->solid = SOLID_NOT;
    gi.linkentity(e);
}

static void PF_makestatic(void)
{
    int n = QC_G_EDICTNUM(QC_OFS_PARM0);

    // Quake handed it to the clients as a fixture and freed it. Here it stays
    // an entity - drawn, never solid, never thinking - that the progs no
    // longer see and QC_Alloc never reuses.
    if (n <= game.maxclients)
        return;

    qc_entvars_t *ev = QC_EV(n);
    Q1_UnlinkEdict(n);
    ev->solid = Q1_SOLID_NOT;
    ev->nextthink = 0;
    ev->movetype = Q1_MOVETYPE_NONE;
    Q1VM_SyncEdict(n);
    gi.linkentity(&g_edicts[n]);    // for its PVS clusters

    qvm.ed[n].free = true;
    qvm.ed[n].isstatic = true;
}

static void PF_setspawnparms(void)
{
    int e = QC_G_EDICTNUM(QC_OFS_PARM0);
    extern void Q1VM_GetSpawnParms(int client, float *parms);

    if (e < 1 || e > game.maxclients)
        QC_RunError("entity is not a client");
    Q1VM_GetSpawnParms(e, qvm.g->parm);
}

static void PF_changelevel(void)
{
    // make sure we don't issue two changelevels
    if (qvm.changelevel_issued)
        return;
    qvm.changelevel_issued = true;

    Q1VM_ChangeLevel(QC_G_STRING(QC_OFS_PARM0));
}

static void PF_movetogoal(void)
{
    int self = QC_FromProg(qvm.g->self);
    float dist = QC_G_FLOAT(QC_OFS_PARM0);

    qc_function_t *oldf = qvm.xfunction;
    int oldself = qvm.g->self;

    Q1_MoveToGoal(self, dist);

    qvm.xfunction = oldf;
    qvm.g->self = oldself;
}

static void PF_checkextension(void)
{
    QC_RETURN_FLOAT(0);
}

static void PF_Fixme(void)
{
    QC_RunError("unimplemented builtin");
}

// the rerelease's (vkQuake's answers)

static void PF_ex_finaleFinished(void)
{
    QC_RETURN_FLOAT(Q1VM_FinaleFinished());
}

static void PF_ex_CheckPlayerEXFlags(void)
{
    QC_RETURN_FLOAT(0);
}

// PATH_ERROR: the progs fall back on movetogoal
static void PF_ex_walkpathtogoal(void)
{
    QC_RETURN_FLOAT(0);
}

static void PF_ex_localsound(void)
{
    Q1_LocalSound(QC_G_EDICTNUM(QC_OFS_PARM0), QC_G_STRING(QC_OFS_PARM1));
}

static void PF_ex_nothing(void)
{
}

static void PF_ex_zero(void)
{
    QC_RETURN_FLOAT(0);
}

/*
==============================================================================

THE TABLE

==============================================================================
*/

#define QC_EX_BASE  100

const char *const qc_ex_builtin_names[] = {
    "ex_centerprint", "ex_bprint", "ex_sprint", "ex_finaleFinished",
    "ex_CheckPlayerEXFlags", "ex_walkpathtogoal", "ex_localsound",
    "ex_draw_point", "ex_draw_line", "ex_draw_arrow", "ex_draw_ray", "ex_draw_circle",
    "ex_draw_bounds", "ex_draw_worldtext", "ex_draw_sphere", "ex_draw_cylinder",
    "ex_bot_movetopoint", "ex_bot_followentity",
    "ex_prompt", "ex_promptchoice", "ex_clearprompt",
    NULL
};

static const qc_builtin_t qc_ex_builtin_funcs[] = {
    PF_centerprint, PF_bprint, PF_sprint, PF_ex_finaleFinished,
    PF_ex_CheckPlayerEXFlags, PF_ex_walkpathtogoal, PF_ex_localsound,
    PF_ex_nothing, PF_ex_nothing, PF_ex_nothing, PF_ex_nothing, PF_ex_nothing,
    PF_ex_nothing, PF_ex_nothing, PF_ex_nothing, PF_ex_nothing,
    PF_ex_zero, PF_ex_zero,
    PF_ex_nothing, PF_ex_zero, PF_ex_nothing,
};

int QC_ExBuiltinNumber(const char *name)
{
    for (int i = 0; qc_ex_builtin_names[i]; i++)
        if (!strcmp(qc_ex_builtin_names[i], name))
            return QC_EX_BASE + i;
    return 0;
}

void Q1_InitBuiltins(void)
{
    static const qc_builtin_t classic[] = {
        PF_Fixme,
        PF_makevectors,     // void(entity e) makevectors = #1
        PF_setorigin,       // void(entity e, vector o) setorigin = #2
        PF_setmodel,        // void(entity e, string m) setmodel = #3
        PF_setsize,         // void(entity e, vector min, vector max) setsize = #4
        PF_Fixme,           // void(entity e, vector min, vector max) setabssize = #5
        PF_break,           // void() break = #6
        PF_random,          // float() random = #7
        PF_sound,           // void(entity e, float chan, string samp) sound = #8
        PF_normalize,       // vector(vector v) normalize = #9
        PF_error,           // void(string e) error = #10
        PF_objerror,        // void(string e) objerror = #11
        PF_vlen,            // float(vector v) vlen = #12
        PF_vectoyaw,        // float(vector v) vectoyaw = #13
        PF_spawn,           // entity() spawn = #14
        PF_remove,          // void(entity e) remove = #15
        PF_traceline,       // float(vector v1, vector v2, float tryents) traceline = #16
        PF_checkclient,     // entity() clientlist = #17
        PF_find,            // entity(entity start, .string fld, string match) find = #18
        PF_precache_sound,  // void(string s) precache_sound = #19
        PF_precache_model,  // void(string s) precache_model = #20
        PF_stuffcmd,        // void(entity client, string s)stuffcmd = #21
        PF_findradius,      // entity(vector org, float rad) findradius = #22
        PF_bprint,          // void(string s) bprint = #23
        PF_sprint,          // void(entity client, string s) sprint = #24
        PF_dprint,          // void(string s) dprint = #25
        PF_ftos,            // void(string s) ftos = #26
        PF_vtos,            // void(string s) vtos = #27
        PF_coredump,
        PF_traceon,
        PF_traceoff,
        PF_eprint,          // void(entity e) debug print an entire entity
        PF_walkmove,        // float(float yaw, float dist) walkmove
        PF_Fixme,           // float(float yaw, float dist) walkmove
        PF_droptofloor,
        PF_lightstyle,
        PF_rint,
        PF_floor,
        PF_ceil,
        PF_Fixme,
        PF_checkbottom,
        PF_pointcontents,
        PF_Fixme,
        PF_fabs,
        PF_aim,
        PF_cvar,
        PF_localcmd,
        PF_nextent,
        PF_particle,
        PF_changeyaw,
        PF_Fixme,
        PF_vectoangles,

        PF_WriteByte,
        PF_WriteChar,
        PF_WriteShort,
        PF_WriteLong,
        PF_WriteCoord,
        PF_WriteAngle,
        PF_WriteString,
        PF_WriteEntity,

        PF_Fixme,
        PF_Fixme,
        PF_Fixme,
        PF_Fixme,
        PF_Fixme,
        PF_Fixme,
        PF_Fixme,

        PF_movetogoal,
        PF_precache_file,
        PF_makestatic,

        PF_changelevel,
        PF_Fixme,

        PF_cvar_set,
        PF_centerprint,

        PF_ambientsound,

        PF_precache_model,
        PF_precache_sound,  // precache_sound2 is different only for qcc
        PF_precache_file,

        PF_setspawnparms,
    };

    memset(qc_builtins, 0, sizeof(qc_builtins));
    for (int i = 0; i < q_countof(classic); i++)
        qc_builtins[i] = classic[i];
    qc_builtins[99] = PF_checkextension;
    for (int i = 0; qc_ex_builtin_names[i]; i++)
        qc_builtins[QC_EX_BASE + i] = qc_ex_builtin_funcs[i];

    memset(q1_models, 0, sizeof(q1_models));
    q1_num_anim_slots = q1_num_anim_times = 0;
    memset(q1_sounds, 0, sizeof(q1_sounds));
    Q1_ResetMessages();

    g_q1_aim = gi.cvar("g_q1_aim", "1", 0);
}
