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

QUAKE 1'S OWN GAME CODE, RUN AS QUAKE RAN IT (g_q1vm*.c)

Every Quake 1 rerelease episode ships its game logic as a compiled QuakeC
program, progs.dat: id1, hipnotic, rogue, dopa, mg1, mg3 and ctf each have
their own, and all seven are plain version 6 progs. They call the 63 standard
builtins and 18 rerelease extras (ex_*, resolved by name), nothing else. So
rather than port each episode's monsters, weapons and triggers to C, the game
DLL runs the progs themselves - this is that interpreter, ported from id's GPL
Quake source (pr_exec.c, pr_edict.c, pr_cmds.c, sv_phys.c, sv_move.c,
world.c), with the rerelease's extras as vkQuake implements them.

It runs on a Quake 1 map in single player and co-op (Q1VM_Active). The bot
match keeps g_q1.c's translation to Quake II's rules. g_q1_progs 0 turns it
off, and so does a progs.dat that will not load.

Each QuakeC entity IS the Quake II edict of the same number: entity 0 is the
world, 1..maxclients the players, as in both games. The progs' fields live in
their own block (qvm.fields); after the progs have run each frame, every
entity's state is copied onto its edict (g_q1vm_game.c), which is all the
server, the network and the renderer ever see. Collision goes through the
engine's own trace, which already traces Quake 1's clip hulls (bsp_q1.c), plus
Quake's box rules for the entities (g_q1vm_phys.c).

The player moves with Quake II's pmove, so the client can predict; the result
is copied into the progs' fields before they think. Everything the progs write
with WriteByte & co. is decoded and re-sent as Quake II messages
(g_q1vm_cmds.c): temp entities become TE_Q1, intermission and finale become
the game's own.

Names: every model, sound and file the progs ask for is sent as
"id1/<episode>/<path>", which the file system resolves to that episode's copy
first and id1's after (files.c) - an add-on's own models and sounds win on its
own maps, and Quake II's files of the same name never do.

==============================================================================
*/

#pragma once

#include "g_local.h"

// ---------------------------------------------------------------------------
// progs.dat (version 6), as written by qcc
// ---------------------------------------------------------------------------

#define QC_PROG_VERSION     6
#define QC_PROGHEADER_CRC   5927    // the standard progdefs.h; every rerelease progs has it

enum {
    QC_ev_void, QC_ev_string, QC_ev_float, QC_ev_vector,
    QC_ev_entity, QC_ev_field, QC_ev_function, QC_ev_pointer
};

#define QC_DEF_SAVEGLOBAL   (1 << 15)

#define QC_OFS_NULL         0
#define QC_OFS_RETURN       1
#define QC_OFS_PARM0        4       // leave 3 ofs for each parm to hold vectors
#define QC_OFS_PARM1        7
#define QC_OFS_PARM2        10
#define QC_OFS_PARM3        13
#define QC_OFS_PARM4        16
#define QC_OFS_PARM5        19
#define QC_OFS_PARM6        22
#define QC_OFS_PARM7        25
#define QC_RESERVED_OFS     28

#define QC_MAX_PARMS        8

typedef struct {
    unsigned short  op;
    short           a, b, c;
} qc_statement_t;

typedef struct {
    unsigned short  type;       // if DEF_SAVEGLOBAL bit is set the variable needs to be saved in savegames
    unsigned short  ofs;
    int             s_name;
} qc_def_t;

typedef struct {
    int     first_statement;    // negative numbers are builtins
    int     parm_start;
    int     locals;             // total ints of parms + locals
    int     profile;            // runtime
    int     s_name;
    int     s_file;             // source file defined in
    int     numparms;
    byte    parm_size[QC_MAX_PARMS];
} qc_function_t;

typedef struct {
    int     version;
    int     crc;                // check of header file
    int     ofs_statements, numstatements;  // statement 0 is an error
    int     ofs_globaldefs, numglobaldefs;
    int     ofs_fielddefs, numfielddefs;
    int     ofs_functions, numfunctions;    // function 0 is an empty
    int     ofs_strings, numstrings;        // first string is a null string
    int     ofs_globals, numglobals;
    int     entityfields;
} qc_progs_t;

typedef union {
    int     string;
    float   _float;
    float   vector[3];
    int     function;
    int     _int;
    int     edict;
} qc_eval_t;

// the progs' globals, progdefs.h as of the standard 5927 header
typedef struct {
    int     pad[28];
    int     self;
    int     other;
    int     world_;            // "world" is a macro in g_local.h
    float   time;
    float   frametime;
    float   force_retouch;
    int     mapname;
    float   deathmatch;
    float   coop;
    float   teamplay;
    float   serverflags;
    float   total_secrets;
    float   total_monsters;
    float   found_secrets;
    float   killed_monsters;
    float   parm[16];           // parm1 .. parm16
    vec3_t  v_forward;
    vec3_t  v_up;
    vec3_t  v_right;
    float   trace_allsolid;
    float   trace_startsolid;
    float   trace_fraction;
    vec3_t  trace_endpos;
    vec3_t  trace_plane_normal;
    float   trace_plane_dist;
    int     trace_ent;
    float   trace_inopen;
    float   trace_inwater;
    int     msg_entity;
    int     main;
    int     StartFrame;
    int     PlayerPreThink;
    int     PlayerPostThink;
    int     ClientKill;
    int     ClientConnect;
    int     PutClientInServer;
    int     ClientDisconnect;
    int     SetNewParms;
    int     SetChangeParms;
} qc_globals_t;

// the engine's fields of every entity, the first 105 of the progs' own
typedef struct {
    float   modelindex;
    vec3_t  absmin;
    vec3_t  absmax;
    float   ltime;
    float   movetype;
    float   solid;
    vec3_t  origin;
    vec3_t  oldorigin;
    vec3_t  velocity;
    vec3_t  angles;
    vec3_t  avelocity;
    vec3_t  punchangle;
    int     classname;
    int     model;
    float   frame;
    float   skin;
    float   effects;
    vec3_t  mins;
    vec3_t  maxs;
    vec3_t  size;
    int     touch;
    int     use;
    int     think;
    int     blocked;
    float   nextthink;
    int     groundentity;
    float   health;
    float   frags;
    float   weapon;
    int     weaponmodel;
    float   weaponframe;
    float   currentammo;
    float   ammo_shells;
    float   ammo_nails;
    float   ammo_rockets;
    float   ammo_cells;
    float   items;
    float   takedamage;
    int     chain;
    float   deadflag;
    vec3_t  view_ofs;
    float   button0;
    float   button1;
    float   button2;
    float   impulse;
    float   fixangle;
    vec3_t  v_angle;
    float   idealpitch;
    int     netname;
    int     enemy;
    float   flags;
    float   colormap;
    float   team;
    float   max_health;
    float   teleport_time;
    float   armortype;
    float   armorvalue;
    float   waterlevel;
    float   watertype;
    float   ideal_yaw;
    float   yaw_speed;
    int     aiment;
    int     goalentity;
    float   spawnflags;
    int     target;
    int     targetname;
    float   dmg_take;
    float   dmg_save;
    int     dmg_inflictor;
    int     owner;
    vec3_t  movedir;
    int     message;
    float   sounds;
    int     noise;
    int     noise1;
    int     noise2;
    int     noise3;
} qc_entvars_t;

#define QC_SYSTEM_FIELDS    105     // sizeof(qc_entvars_t) / 4

// ---------------------------------------------------------------------------
// Quake 1's constants the engine side knows (defs.qc)
// ---------------------------------------------------------------------------

#define Q1_MOVETYPE_NONE        0
#define Q1_MOVETYPE_WALK        3
#define Q1_MOVETYPE_STEP        4
#define Q1_MOVETYPE_FLY         5
#define Q1_MOVETYPE_TOSS        6
#define Q1_MOVETYPE_PUSH        7
#define Q1_MOVETYPE_NOCLIP      8
#define Q1_MOVETYPE_FLYMISSILE  9
#define Q1_MOVETYPE_BOUNCE      10
#define Q1_MOVETYPE_GIB         11      // rerelease: falls like TOSS

#define Q1_SOLID_NOT            0
#define Q1_SOLID_TRIGGER        1
#define Q1_SOLID_BBOX           2
#define Q1_SOLID_SLIDEBOX       3
#define Q1_SOLID_BSP            4
#define Q1_SOLID_CORPSE         5       // rerelease: passes players, monsters and other corpses

#define Q1_FL_FLY               1
#define Q1_FL_SWIM              2
#define Q1_FL_CONVEYOR          4
#define Q1_FL_CLIENT            8
#define Q1_FL_INWATER           16
#define Q1_FL_MONSTER           32
#define Q1_FL_GODMODE           64
#define Q1_FL_NOTARGET          128
#define Q1_FL_ITEM              256
#define Q1_FL_ONGROUND          512
#define Q1_FL_PARTIALGROUND     1024
#define Q1_FL_WATERJUMP         2048
#define Q1_FL_JUMPRELEASED      4096

#define Q1_EF_BRIGHTFIELD       1
#define Q1_EF_MUZZLEFLASH       2
#define Q1_EF_BRIGHTLIGHT       4
#define Q1_EF_DIMLIGHT          8
#define Q1_EF_QUADLIGHT         16      // rerelease
#define Q1_EF_PENTALIGHT        32      // rerelease
#define Q1_EF_CANDLELIGHT       64      // rerelease

// a .mdl's header flags
#define Q1_MF_ROCKET            1
#define Q1_MF_GRENADE           2
#define Q1_MF_GIB               4
#define Q1_MF_ROTATE            8
#define Q1_MF_TRACER            16
#define Q1_MF_ZOMGIB            32
#define Q1_MF_TRACER2           64
#define Q1_MF_TRACER3           128

#define Q1_CONTENTS_EMPTY       -1
#define Q1_CONTENTS_SOLID       -2
#define Q1_CONTENTS_WATER       -3
#define Q1_CONTENTS_SLIME       -4
#define Q1_CONTENTS_LAVA        -5
#define Q1_CONTENTS_SKY         -6

#define Q1_MOVE_NORMAL          0
#define Q1_MOVE_NOMONSTERS      1
#define Q1_MOVE_MISSILE         2

#define Q1_SPAWNFLAG_NOT_EASY       256
#define Q1_SPAWNFLAG_NOT_MEDIUM     512
#define Q1_SPAWNFLAG_NOT_HARD       1024
#define Q1_SPAWNFLAG_NOT_DEATHMATCH 2048

#define Q1_IT_SHOTGUN           1
#define Q1_IT_SUPER_SHOTGUN     2
#define Q1_IT_NAILGUN           4
#define Q1_IT_SUPER_NAILGUN     8
#define Q1_IT_GRENADE_LAUNCHER  16
#define Q1_IT_ROCKET_LAUNCHER   32
#define Q1_IT_LIGHTNING         64
#define Q1_IT_SHELLS            256
#define Q1_IT_NAILS             512
#define Q1_IT_ROCKETS           1024
#define Q1_IT_CELLS             2048
#define Q1_IT_AXE               4096
#define Q1_IT_ARMOR1            8192
#define Q1_IT_ARMOR2            16384
#define Q1_IT_ARMOR3            32768
#define Q1_IT_SUPERHEALTH       65536
#define Q1_IT_KEY1              131072
#define Q1_IT_KEY2              262144
#define Q1_IT_INVISIBILITY      524288
#define Q1_IT_INVULNERABILITY   1048576
#define Q1_IT_SUIT              2097152
#define Q1_IT_QUAD              4194304

// the progs' message bytes (defs.qc)
#define Q1_MSG_BROADCAST        0
#define Q1_MSG_ONE              1
#define Q1_MSG_ALL              2
#define Q1_MSG_INIT             3

#define Q1_SVC_UPDATESTAT       3
#define Q1_SVC_SETANGLE         10
#define Q1_SVC_TEMPENTITY       23
#define Q1_SVC_CENTERPRINT      26
#define Q1_SVC_KILLEDMONSTER    27
#define Q1_SVC_FOUNDSECRET      28
#define Q1_SVC_SPAWNSTATICSOUND 29
#define Q1_SVC_INTERMISSION     30
#define Q1_SVC_FINALE           31
#define Q1_SVC_CDTRACK          32
#define Q1_SVC_SELLSCREEN       33
#define Q1_SVC_CUTSCENE         34
#define Q1_SVC_SPAWNEDMONSTER   39
#define Q1_SVC_ACHIEVEMENT      52
#define Q1_SVC_CHAT             53
#define Q1_SVC_LEVELCOMPLETED   54
#define Q1_SVC_BACKTOLOBBY      55
#define Q1_SVC_LOCALSOUND       56
#define Q1_SVC_PROMPT           57

// ---------------------------------------------------------------------------
// the machine
// ---------------------------------------------------------------------------

#define QC_MAX_STACK_DEPTH      64
#define QC_LOCALSTACK_SIZE      4096
#define QC_MAX_BUILTINS         128

#define QC_TEMPSTRINGS          32      // ring of results (ftos, vtos...)
#define QC_TEMPSTRING_SIZE      256
#define QC_STRING_HEAP          (2 * 1024 * 1024)

typedef struct {
    bool    free;
    float   freetime;           // qvm.time when it was freed
    bool    linked;             // in the engine's area grid (gi.linkentity)
    int     mdl_flags;          // the model's .mdl header flags (Q1_MF_*)
    bool    isstatic;           // makestatic / ambientsound: drawn, never the progs' again
    int     qc_frame, qc_skin;  // the progs' frame and skin when last sent (a static's group animates on)
    bool    sent;               // sent_origin is good (Q1VM_SyncEdict)
    vec3_t  sent_origin;        // where the clients last saw it
} qc_edict_t;

typedef struct {
    int     s;
    qc_function_t *f;
} qc_stackframe_t;

typedef void (*qc_builtin_t)(void);

typedef struct {
    bool            active;     // the progs run this level (Q1VM_Active)
    char            episode[16];    // "id1", "hipnotic", ... - the progs' and the files' episode
    char            q1map[MAX_QPATH];   // the map's own name, "e1m1"
    char            q2map[MAX_QPATH];   // the server's, "q1/e1m1" or "q1/hipnotic/hip1m1"

    // the program
    void            *file;      // progs.dat as loaded (gi.LoadFile)
    qc_progs_t      *progs;
    qc_function_t   *functions;
    qc_statement_t  *statements;
    qc_def_t        *globaldefs;
    qc_def_t        *fielddefs;
    float           *globals;
    qc_globals_t    *g;
    int             numbuiltins;

    // strings: the progs' own, then the heap (QC_NewString) and the temp ring
    char            *strings;
    int             strings_size;   // all of it
    int             progs_strings;  // the progs' part
    int             heap_used;      // from progs_strings + the ring
    int             temp_next;

    // entities
    int             edict_size;     // bytes of fields per entity
    byte            *fields;        // max_edicts * edict_size
    qc_edict_t      *ed;
    int             max_edicts;
    int             num_edicts;

    // optional fields some progs add, or -1 (word offsets)
    int             fld_alpha, fld_scale;

    // the interpreter
    qc_stackframe_t stack[QC_MAX_STACK_DEPTH];
    int             depth;
    int             localstack[QC_LOCALSTACK_SIZE];
    int             localstack_used;
    qc_function_t   *xfunction;
    int             xstatement;
    int             argc;
    bool            loading;        // entities being spawned: world may be written

    // time, as the progs see it: starts at 1, as Quake's sv.time did
    float           time;
    float           frametime;

    // the level
    bool            changelevel_issued;
    int             intermission;   // 0, 1 intermission, 2 finale, 3 cutscene
    float           intermission_time;
    char            finale[1024];
    float           finale_time;    // when the text began typing out
    int             skill;

    // checkclient's rotation (PF_checkclient)
    int             lastcheck;
    float           lastchecktime;

    // a missile that just reached the sky (Q1_SkyImpact): pointcontents of this
    // point is CONTENTS_SKY until its touch function returns
    bool            sky_impact;
    vec3_t          sky_point;
} qc_vm_t;

extern qc_vm_t qvm;

// ---------------------------------------------------------------------------
// access
// ---------------------------------------------------------------------------

#define QC_G_FLOAT(o)       (qvm.globals[o])
#define QC_G_INT(o)         (((int *)qvm.globals)[o])
#define QC_G_VECTOR(o)      (&qvm.globals[o])
#define QC_G_STRING(o)      QC_Str(QC_G_INT(o))
#define QC_G_EDICTNUM(o)    QC_FromProg(QC_G_INT(o))
#define QC_RETURN_FLOAT(v)  (QC_G_FLOAT(QC_OFS_RETURN) = (v))
#define QC_RETURN_EDICT(n)  (QC_G_INT(QC_OFS_RETURN) = QC_ToProg(n))

static inline qc_entvars_t *QC_EV(int num)
{
    return (qc_entvars_t *)(qvm.fields + (size_t)num * qvm.edict_size);
}

static inline int QC_ToProg(int num)
{
    return num * qvm.edict_size;
}

static inline int QC_FromProg(int e)
{
    return e / qvm.edict_size;
}

// a word of entity (num)'s fields by offset
static inline qc_eval_t *QC_Field(int num, int ofs)
{
    return (qc_eval_t *)(qvm.fields + (size_t)num * qvm.edict_size + ofs * 4);
}

// g_q1vm.c: the machine
const char *QC_Str(int s);
int         QC_NewString(const char *s);
int         QC_TempString(const char *s);
int         QC_EngineString(const char *s);     // a pointer into qvm.strings, else a copy
void        QC_Execute(int fnum);
void        QC_RunError(const char *fmt, ...) q_printf(1, 2);
void        QC_PrintStack(void);
qc_function_t *QC_FindFunction(const char *name);
qc_def_t    *QC_FindField(const char *name);
qc_def_t    *QC_FindGlobal(const char *name);
const char  *QC_Localize(const char *s);
int         QC_Alloc(void);
void        QC_Free(int num);
void        QC_ClearEdict(int num);
void        QC_PrintEdict(int num);
bool        QC_LoadProgs(const char *episode);
void        QC_FreeProgs(void);
void        QC_LoadEntities(const char *entities);
bool        QC_ParseEpair(int num, qc_def_t *key, const char *value);
void        QC_LoadLocalization(const char *episode);

// g_q1vm_cmds.c: the builtins and the messages they write
extern qc_builtin_t qc_builtins[QC_MAX_BUILTINS];
extern const char *const qc_ex_builtin_names[];
int         QC_ExBuiltinNumber(const char *name);
void        Q1_InitBuiltins(void);
const char *Q1_FilePath(const char *path, char *buffer, size_t size);  // "id1/<episode>/<path>"
int         Q1_ModelIndex(const char *name);
int         Q1VM_SoundIndex(const char *name);
bool        Q1_SoundPrecached(const char *name);
int         Q1_ModelFlags(int modelindex);
void        Q1_MessagesFrame(void);
void        Q1_ResetMessages(void);
void        Q1_StartSound(int ent, int channel, const char *sample, float volume, float attenuation);
void        Q1_LocalSound(int client, const char *sample);
void        Q1_SetModelBounds(int num, int modelindex);
const char *Q1_ModelName(int modelindex);
void        Q1_AnimPick(int modelindex, int num, float time, int *frame, int *skin);

// a ThrowHead head's mesh (progs/h_*.mdl), for g_q1vm_phys.c's rolling heads:
// frame 0's vertices about the middle of their box, in the model's own axes
#define Q1_HEAD_MAX_VERTS   128
typedef struct {
    int     numverts;
    vec3_t  centre;         // the middle of frame 0's box, from the model's origin
    float   reach;          // the farthest vertex from the centre
    vec3_t  verts[Q1_HEAD_MAX_VERTS];   // relative to the centre
} q1_head_mesh_t;
const q1_head_mesh_t *Q1_HeadMesh(int modelindex);     // NULL: not a head's model

// g_q1vm_phys.c: Quake's physics and world
typedef struct {
    bool        allsolid;
    bool        startsolid;
    bool        inopen, inwater;
    float       fraction;
    vec3_t      endpos;
    cplane_t    plane;
    int         ent;            // entity number hit, -1 for none
    bool        sky;            // the world surface hit was sky
} q1trace_t;

q1trace_t   Q1_Move(const vec3_t start, const vec3_t mins, const vec3_t maxs, const vec3_t end, int type, int passedict);
int         Q1_PointContents(const vec3_t p);
void        Q1_LinkEdict(int num, bool touch_triggers);
void        Q1_SyncLink(int num);
void        Q1_UnlinkEdict(int num);
int         Q1_TestEntityPosition(int num);
void        Q1_Physics(void);
bool        Q1_CheckBottom(int num);
bool        Q1_MoveStep(int num, const vec3_t move, bool relink);
void        Q1_MoveToGoal(int num, float dist);
bool        Q1_WalkMove(int num, float yaw, float dist);
void        Q1_Impact(int e1, int e2);
void        Q1_ChangeYaw(int num);
float       Q1_DropToFloor(int num);
bool        Q1_RunThink(int num);
void        Q1_CheckWater(int num);
void        Q1_PhysicsInit(void);           // its cvars (Q1VM_GameInit)
int         Q1_OldestKeptGib(void);         // a g_no_janitor gib to recycle, 0 for none
int         Q1_ThrowTestHead(const vec3_t origin, const vec3_t velocity, const char *model);

// g_q1vm_game.c: the game around it
bool        Q1VM_Active(void);
void        Q1VM_GameInit(void);
bool        Q1VM_SpawnEntities(const char *mapname, const char *entities);
void        Q1VM_RunFrame(void);
void        Q1VM_ClientBegin(edict_t *ent);
void        Q1VM_ClientDisconnect(edict_t *ent);
void        Q1VM_ClientThink(edict_t *ent, usercmd_t *cmd);
bool        Q1VM_ClientCommand(edict_t *ent);
bool        Q1VM_ServerCommand(const char *cmd);    // "sv q1head", true when it was one
void        Q1VM_ChangeLevel(const char *map);
void        Q1VM_Intermission(int kind, const char *text);
float       Q1VM_FinaleFinished(void);
void        Q1VM_BonusFlash(int client);
void        Q1VM_SyncEdict(int num);
bool        Q1VM_IsFlare(int num);      // Quake II RTX's flare (g_q1vm_game.c THE FLARE GUN)
bool        Q1_IsAliasModel(int modelindex);
unsigned    Q1_ModelTrailEffects(int modelindex);
int         Q1VM_Skill(void);
void        Q1VM_GetSpawnParms(int client, float *parms);
bool        Q1VM_ClientInGame(int num);
