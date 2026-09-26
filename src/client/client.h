/*
Copyright (C) 1997-2001 Id Software, Inc.

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

// client.h -- primary header for client

#include "shared/shared.h"
#include "shared/list.h"

#include "common/bsp.h"
#include "common/cmd.h"
#include "common/cmodel.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/field.h"
#include "common/files.h"
#include "common/pmove.h"
#include "common/math.h"
#include "common/msg.h"
#include "common/net/chan.h"
#include "common/net/net.h"
#include "common/prompt.h"
#include "common/protocol.h"
#include "common/sizebuf.h"
#include "common/zone.h"
#include "common/cpuprof.h"

#include "system/system.h"
#include "refresh/refresh.h"
#include "server/server.h"

#include "client/client.h"
#include "client/input.h"
#include "client/keys.h"
#include "client/sound/sound.h"
#include "client/ui.h"
#include "client/video.h"

#if USE_ZLIB
#include <zlib.h>
#endif

//=============================================================================

#define MAX_EXPLOSIONS  32

typedef struct {
	enum {
		ex_free,
		ex_explosion,
		ex_misc,
		ex_flash,
		ex_mflash,
		ex_poly,
		ex_poly2,
		ex_light,
		ex_blaster,
		ex_hyperblaster,
		ex_flare
	} type;

	entity_t    ent;
	int         frames;
	float       light;
	vec3_t      lightcolor;
	float       start;
	int         baseframe;
	int         frametime; /* in milliseconds */

	// Impact smoke drifts back out along the surface normal instead of
	// hanging exactly where the bullet landed. Zero for every other
	// explosion type. spawn_origin is kept because ent.origin is what gets
	// advanced, so the drift has to integrate from a fixed point.
	vec3_t      vel;
	vec3_t      spawn_origin;
} explosion_t;

extern explosion_t  cl_explosions[MAX_EXPLOSIONS];

typedef struct centity_s {
    entity_state_t    current;
    entity_state_t    prev;            // will always be valid, but might just be a copy of current

    vec3_t          mins, maxs;

    int             serverframe;        // if not current, this ent isn't in the frame

    int             trailcount;         // for diminishing grenade trails
    vec3_t          lerp_origin;        // for trails (variable hz)

    // Alias model animation is interpolated over the ANIMATION interval rather
    // than the server interval whenever the two differ - see CL_AddPacketEntities.
    // The rerelease runs its game at 40 Hz but still advances monster animation
    // at 10 Hz, so without this a demo of it animates in visible steps.
    int             prev_frame;
    int             anim_start;

#if USE_FPS
    int             event_frame;
#endif

    int             fly_stoptime;

    int             id;
} centity_t;

extern centity_t    cl_entities[MAX_EDICTS];

// compass breadcrumb points the client will hold; must match POI_PATH_MAX
// in the game DLL, which is what fills the packet
#define MAX_POI_PATH    16

// How long the breadcrumb trail stays up. The objective marker outlives it -
// the marker's lifetime comes from the server in TE_POI - because the trail is
// only there to get you moving, while the marker is what you steer by.
#define POI_TRAIL_TIME      10000   // ms

// both fade out over the last stretch of their life rather than popping
#define POI_FADE_TIME       1500    // ms

// The rerelease beeps once per breadcrumb, because it drops them one at a time
// over a couple of seconds. We send the whole trail in a single packet, so the
// beeps are a client side chain instead: the server's own beep is the first of
// them, these are the rest.
#define POI_BEEP_INTERVAL   200     // ms
#define POI_BEEP_MIN        4
#define POI_BEEP_MAX        8

// The compass green. The objective marker and the breadcrumbs share it on
// purpose - that is what makes a chevron on the floor read as belonging to the
// same trail as the arrow hanging over the door.
#define POI_COLOR(a)        MakeColor(64, 255, 64, (a))

// The breadcrumb chevron is the rerelease's own models/objects/pointer: a flat
// arrowhead plate, 4 units long and 6 across, lying in its own XY plane and
// pointing down +X. That is small for a mark you are meant to read from across
// a room, so it is scaled up - see cl_poi_marker_scale.
#define POI_MARKER_MODEL    "models/objects/pointer/tris.md2"

#define MAX_CLIENTWEAPONMODELS        20        // PGM -- upped from 16 to fit the chainfist vwep

typedef struct clientinfo_s {
    char name[MAX_QPATH];
    qhandle_t skin;
    qhandle_t icon;
    char model_name[MAX_QPATH];
    char skin_name[MAX_QPATH];
    qhandle_t model;
    qhandle_t weaponmodel[MAX_CLIENTWEAPONMODELS];
} clientinfo_t;

typedef struct {
    unsigned    sent;    // time sent, for calculating pings
    unsigned    rcvd;    // time rcvd, for calculating pings
    unsigned    cmdNumber;    // current cmdNumber for this frame
} client_history_t;

typedef struct {
    bool            valid;

    int             number;
    int             delta;

    byte            areabits[MAX_MAP_AREA_BYTES];
    int             areabytes;

    player_state_t  ps;
    int             clientNum;

    int             numEntities;
    int             firstEntity;
} server_frame_t;

// locally calculated frame flags for debug display
#define FF_SERVERDROP   (1<<4)
#define FF_BADFRAME     (1<<5)
#define FF_OLDFRAME     (1<<6)
#define FF_OLDENT       (1<<7)
#define FF_NODELTA      (1<<8)

// variable server FPS
#if USE_FPS
#define CL_FRAMETIME    cl.frametime
#define CL_1_FRAMETIME  cl.frametime_inv
#define CL_FRAMEDIV     cl.framediv
#define CL_FRAMESYNC    !(cl.frame.number % cl.framediv)
#define CL_KEYPS        &cl.keyframe.ps
#define CL_OLDKEYPS     &cl.oldkeyframe.ps
#define CL_KEYLERPFRAC  cl.keylerpfrac
#else
// USE_FPS is off in this tree, so the keyframe machinery below stays compiled
// out - but the server frame INTERVAL still cannot be a constant any more.
// Rerelease demos run the game at 40 Hz, and cl.servertime is derived straight
// from the frame number (CL_DeltaFrame), so a hardcoded 100 ms would play them
// back at a quarter speed. cl.frametime is set once per connection, from the
// rate the server or demo announces, and is BASE_FRAMETIME for everything that
// is not a rerelease demo - so nothing about ordinary play changes.
#define CL_FRAMETIME    cl.frametime
#define CL_1_FRAMETIME  cl.frametime_inv
#define CL_FRAMEDIV     1
#define CL_FRAMESYNC    1
#define CL_KEYPS        &cl.frame.ps
#define CL_OLDKEYPS     &cl.oldframe.ps
#define CL_KEYLERPFRAC  cl.lerpfrac
#endif

//
// the client_state_t structure is wiped completely at every
// server map change
//
typedef struct client_state_s {
    int         timeoutcount;

    unsigned    lastTransmitTime;
    unsigned    lastTransmitCmdNumber;
    unsigned    lastTransmitCmdNumberReal;
    bool        sendPacketNow;

    usercmd_t    cmd;
    usercmd_t    cmds[CMD_BACKUP];    // each mesage will send several old cmds
    unsigned     cmdNumber;
    short        predicted_origins[CMD_BACKUP][3];    // for debug comparing against server
    client_history_t    history[CMD_BACKUP];
    int         initialSeq;

    float       predicted_step;                // for stair up smoothing
    unsigned    predicted_step_time;
    unsigned    predicted_step_frame;

    vec3_t      predicted_origin;    // generated by CL_PredictMovement
    vec3_t      predicted_angles;
    vec3_t      predicted_velocity;
    vec3_t      prediction_error;

    // rebuilt each valid frame
    centity_t       *solidEntities[MAX_PACKET_ENTITIES];
    int             numSolidEntities;

    entity_state_t  baselines[MAX_EDICTS];

    entity_state_t  entityStates[MAX_PARSE_ENTITIES];
    int             numEntityStates;

    msgEsFlags_t    esFlags;

    server_frame_t  frames[UPDATE_BACKUP];
    unsigned        frameflags;

    server_frame_t  frame;                // received from server
    server_frame_t  oldframe;
    int             servertime;
    int             serverdelta;

#if USE_FPS
    server_frame_t  keyframe;
    server_frame_t  oldkeyframe;
    int             keyservertime;
#endif

    byte            dcs[CS_BITMAP_BYTES];

    // the client maintains its own idea of view angles, which are
    // sent to the server each frame.  It is cleared to 0 upon entering each level.
    // the server sends a delta each frame which is added to the locally
    // tracked view angles to account for standing on rotating objects,
    // and teleport direction changes
    vec3_t      viewangles;

    // interpolated movement vector used for local prediction,
    // never sent to server, rebuilt each client frame
    vec3_t      localmove;

    // accumulated mouse forward/side movement, added to both
    // localmove and pending cmd, cleared each time cmd is finalized
    vec2_t      mousemove;

#if USE_SMOOTH_DELTA_ANGLES
    short       delta_angles[3]; // interpolated
#endif

    int         time;           // this is the time value that the client
                                // is rendering at.  always <= cl.servertime
    float       lerpfrac;       // between oldframe and frame

#if USE_FPS
    int         keytime;
    float       keylerpfrac;
#endif

    refdef_t    refdef;
    float       fov_x;      // interpolated
    float       fov_y;      // derived from fov_x assuming 4/3 aspect ratio
    int         lightlevel;

    vec3_t      v_forward, v_right, v_up;    // set when refdef.angles is set

    bool        thirdPersonView;

    // predicted values, used for smooth player entity movement in thirdperson view
    vec3_t      playerEntityOrigin;
    vec3_t      playerEntityAngles;

    //
    // transient data from server
    //
    char        layout[MAX_NET_STRING];     // general 2D overlay
    int         inventory[MAX_ITEMS];

    //
    // server state information
    //
    int         serverstate;    // ss_* constants
    int         servercount;    // server identification for prespawns
    char        gamedir[MAX_QPATH];
    int         clientNum;            // never changed during gameplay, set by serverdata packet
    int         maxclients;
    pmoveParams_t pmp;

    // Server frame interval in milliseconds and its reciprocal. Always
    // BASE_FRAMETIME / BASE_1_FRAMETIME except while playing back a rerelease
    // demo, which runs the game at 40 Hz - see CL_FRAMETIME above. Set by
    // CL_SetServerFrameTime, which both CL_ClearState and CL_ParseServerData
    // call, so they are never left at the zero the state wipe leaves behind.
    int         frametime;      // variable server frame time
    float       frametime_inv;  // 1/frametime
#if USE_FPS
    int         framediv;       // BASE_FRAMETIME/frametime
#endif

    // [rerelease] Non-zero while a KEX-protocol demo is being played back;
    // holds PROTOCOL_VERSION_KEX_DEMOS or PROTOCOL_VERSION_KEX. Everything the
    // compatibility layer needs to key off lives in src/client/kexdemo.c, but
    // the entity and frame parsers in parse.c have to branch on it too.
    int         kex_protocol;

    // The view weapon animates on the same 10 Hz clock the world models do, so
    // when the server tick is faster than that it needs the same treatment -
    // see CL_AddViewWeapon. Tracked here rather than per entity because the gun
    // lives in the player state, not in the entity list.
    int         gun_prev_frame;
    int         gun_anim_start;

    // Which protocol encoding the demo being played back uses. The two halves
    // are tracked apart because this fork changed them in separate commits, so
    // a demo recorded in between has one and not the other. Not known until the
    // first configstring settles it. See the OLD CONFIGSTRING LAYOUT block in
    // parse.c.
    bool        demo_encoding_known;
    bool        demo_old_configstrings;
    bool        demo_old_indices;

    char        baseconfigstrings[MAX_CONFIGSTRINGS][MAX_QPATH];
    char        configstrings[MAX_CONFIGSTRINGS][MAX_QPATH];
    char        mapname[MAX_QPATH]; // short format - q2dm1, etc

#if USE_AUTOREPLY
    unsigned    reply_time;
    unsigned    reply_delta;
#endif

    //
    // locally derived information from server state
    //
    bsp_t        *bsp;

    qhandle_t model_draw[MAX_MODELS];
    mmodel_t *model_clip[MAX_MODELS];

    qhandle_t sound_precache[MAX_SOUNDS];
    qhandle_t image_precache[MAX_IMAGES];

    clientinfo_t    clientinfo[MAX_CLIENTS];
    clientinfo_t    baseclientinfo;

    char    weaponModels[MAX_CLIENTWEAPONMODELS][MAX_QPATH];
    int     numWeaponModels;

    // [rerelease] the compass objective marker, set by TE_POI. poi_time is
    // when it stops being drawn (cl.time based), 0 = nothing to draw.
    vec3_t      poi_origin;
    qhandle_t   poi_pic;
    int         poi_time;

    // The breadcrumb trail that goes with it. It is deliberately shorter lived
    // than the marker, so it gets its own expiry instead of sharing poi_time.
    vec3_t      poi_path[MAX_POI_PATH];
    int         poi_path_count;
    int         poi_path_time;

    // the chain of beeps that plays while the trail is being laid out
    int         poi_beep_time;
    int         poi_beeps_left;
} client_state_t;

extern    client_state_t    cl;

/*
==================================================================

the client_static_t structure is persistant through an arbitrary number
of server connections

==================================================================
*/

// resend delay for challenge/connect packets
#define CONNECT_DELAY       3000u

#define CONNECT_INSTANT     CONNECT_DELAY
#define CONNECT_FAST        (CONNECT_DELAY - 1000u)

typedef enum {
    ca_uninitialized,
    ca_disconnected,    // not talking to a server
    ca_challenging,     // sending getchallenge packets to the server
    ca_connecting,      // sending connect packets to the server
    ca_connected,       // netchan_t established, waiting for svc_serverdata
    ca_loading,         // loading level data
    ca_precached,       // loaded level data, waiting for svc_frame
    ca_active,          // game views should be displayed
    ca_cinematic        // running a cinematic
} connstate_t;

#define FOR_EACH_DLQ(q) \
    LIST_FOR_EACH(dlqueue_t, q, &cls.download.queue, entry)
#define FOR_EACH_DLQ_SAFE(q, n) \
    LIST_FOR_EACH_SAFE(dlqueue_t, q, n, &cls.download.queue, entry)

typedef enum {
    // generic types
    DL_OTHER,
    DL_MAP,
    DL_MODEL,
#if USE_CURL
    // special types
    DL_LIST,
    DL_PAK
#endif
} dltype_t;

typedef enum {
    DL_PENDING,
    DL_RUNNING,
    DL_DONE
} dlstate_t;

typedef struct {
    list_t      entry;
    dltype_t    type;
    dlstate_t   state;
    char        path[1];
} dlqueue_t;

typedef struct client_static_s {
    connstate_t state;
    keydest_t   key_dest;

    active_t    active;

    bool        ref_initialized;
    ref_type_t  ref_type;
    unsigned    disable_screen;

    int         userinfo_modified;
    cvar_t      *userinfo_updates[MAX_PACKET_USERINFOS];
// this is set each time a CVAR_USERINFO variable is changed
// so that the client knows to send it to the server

    int         framecount;
    unsigned    realtime;           // always increasing, no clamping, etc
    float       frametime;          // seconds since last frame

// preformance measurement
#define C_FPS   cls.measure.fps[0]
#define R_FPS   cls.measure.fps[1]
#define C_MPS   cls.measure.fps[2]
#define C_PPS   cls.measure.fps[3]
#define C_FRAMES    cls.measure.frames[0]
#define R_FRAMES    cls.measure.frames[1]
#define M_FRAMES    cls.measure.frames[2]
#define P_FRAMES    cls.measure.frames[3]
    struct {
        unsigned    time;
        int         frames[4];
        int         fps[4];
        int         ping;
    } measure;

// connection information
    netadr_t    serverAddress;
    char        servername[MAX_OSPATH]; // name of server from original connect
    unsigned    connect_time;           // for connection retransmits
    int         connect_count;
    bool        passive;

#if USE_ZLIB
    z_stream    z;
#endif

    int         quakePort;          // a 16 bit value that allows quake servers
                                    // to work around address translating routers
    netchan_t   *netchan;
    int         serverProtocol;     // in case we are doing some kind of version hack
    int         protocolVersion;    // minor version

    int         challenge;          // from the server to use for connecting

#if USE_ICMP
    bool        errorReceived;      // got an ICMP error from server
#endif

#define RECENT_ADDR 4
#define RECENT_MASK (RECENT_ADDR - 1)

    netadr_t    recent_addr[RECENT_ADDR];
    int         recent_head;

    struct {
        list_t      queue;              // queue of paths we need
        int         pending;            // number of non-finished entries in queue
        dlqueue_t   *current;           // path being downloaded
        int         percent;            // how much downloaded
        int         position;           // how much downloaded (in bytes)
        qhandle_t   file;               // UDP file transfer from server
        char        temp[MAX_QPATH + 4];// account 4 bytes for .tmp suffix
#if USE_ZLIB
        z_stream    z;                  // UDP download zlib stream
#endif
        string_entry_t  *ignores;       // list of ignored paths
    } download;

// demo recording info must be here, so it isn't cleared on level change
    struct {
        qhandle_t   playback;
        qhandle_t   recording;
        unsigned    time_start;
        unsigned    time_frames;
        int         last_server_frame;  // number of server frame the last svc_frame was written
        int         frames_written;     // number of frames written to demo file
        int         frames_dropped;     // number of svc_frames that didn't fit
        int         others_dropped;     // number of misc svc_* messages that didn't fit
        int         frames_read;        // number of frames read from demo file
        int         last_snapshot;      // number of demo frame the last snapshot was saved
        int64_t     file_size;
        int64_t     file_offset;
        int         file_percent;
        sizebuf_t   buffer;
        list_t      snapshots;
        bool        paused;
        bool        seeking;
        bool        eof;
    } demo;
    struct {
        // Number of timedemo runs to perform
        int         runs_total;
        // Current run
        int         run_current;
        // Results of timedemo runs
        unsigned    *results;
    } timedemo;

#if USE_CLIENT_GTV
    struct {
        connstate_t     state;

        netstream_t     stream;
        size_t          msglen;

        player_packed_t     ps;
        entity_packed_t     entities[MAX_EDICTS];

        sizebuf_t       message;
    } gtv;
#endif
} client_static_t;

extern client_static_t    cls;

extern cmdbuf_t    cl_cmdbuf;
extern char        cl_cmdbuf_text[MAX_STRING_CHARS];

//=============================================================================

#define NOPART_GRENADE_EXPLOSION    1
#define NOPART_GRENADE_TRAIL        2
#define NOPART_ROCKET_EXPLOSION     4
#define NOPART_ROCKET_TRAIL         8
#define NOPART_BLOOD                16

#define NOEXP_GRENADE   1
#define NOEXP_ROCKET    2

#define DLHACK_ROCKET_COLOR         1
#define DLHACK_SMALLER_EXPLOSION    2
#define DLHACK_NO_MUZZLEFLASH       4

//
// cvars
//
extern cvar_t    *cl_gunalpha;
extern cvar_t    *cl_muzzleflash_models;
extern cvar_t    *cl_muzzleflash_scale;
extern cvar_t    *cl_muzzleflash_view_size;
extern cvar_t    *cl_muzzleflash_view_brightness;
extern cvar_t    *cl_muzzleflash_time;
extern cvar_t    *cl_muzzleflash_light;
extern cvar_t    *cl_muzzleflash_brightness;
extern cvar_t    *cl_muzzleflash_offset;
extern cvar_t    *cl_predict;
extern cvar_t    *cl_muzzleflash_world_fwd;
extern cvar_t    *cl_muzzleflash_world_right;
extern cvar_t    *cl_muzzleflash_world_up;
extern cvar_t    *cl_footsteps;
// Surface-dependent footsteps (footsteps.c). 0 restores the four generic
// player/stepN.wav sounds.
extern cvar_t    *cl_footstep_materials;
qhandle_t CL_FootstepSound(const vec3_t origin);
void CL_ClearFootstepCache(void);
void CL_InitFootsteps(void);
extern cvar_t    *cl_noskins;
extern cvar_t    *cl_kickangles;
extern cvar_t    *cl_rollhack;
extern cvar_t    *cl_noglow;
extern cvar_t    *cl_nolerp;

#if USE_DEBUG
#define SHOWNET(level, ...) \
    if (cl_shownet->integer > level) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__)
#define SHOWCLAMP(level, ...) \
    if (cl_showclamp->integer > level) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__)
#define SHOWMISS(...) \
    if (cl_showmiss->integer) \
        Com_LPrintf(PRINT_DEVELOPER, __VA_ARGS__)
extern cvar_t    *cl_shownet;
extern cvar_t    *cl_showmiss;
extern cvar_t    *cl_showclamp;
#else
#define SHOWNET(...)
#define SHOWCLAMP(...)
#define SHOWMISS(...)
#endif

extern cvar_t    *cl_vwep;

// Overrides the auto-detected encoding of a demo being played back: -1 detects
// it, otherwise a mask of 1 (original configstring layout) and 2 (original byte
// model and gun indices), so 0 is fully extended and 3 fully original. See the
// OLD CONFIGSTRING LAYOUT block in parse.c.
extern cvar_t    *cl_demo_encoding;

extern cvar_t    *cl_disable_particles;
extern cvar_t    *cl_disable_explosions;
extern cvar_t    *cl_explosion_sprites;
extern cvar_t    *cl_explosion_frametime;
extern cvar_t    *cl_dlight_hacks;
extern cvar_t    *cl_blaster_color;
extern cvar_t    *cl_ludicrous_gibs;
extern cvar_t    *cl_spheretrans_alpha;
extern cvar_t    *cl_tracker_bubble;
extern cvar_t    *cl_tracker_bubble_scale;
extern cvar_t    *cl_blood_spheres;
extern cvar_t    *cl_blood_sphere_radius;

extern cvar_t    *cl_chat_notify;
extern cvar_t    *cl_chat_sound;
extern cvar_t    *cl_chat_filter;

extern cvar_t    *cl_disconnectcmd;
extern cvar_t    *cl_changemapcmd;
extern cvar_t    *cl_beginmapcmd;

extern cvar_t    *cl_gibs;
extern cvar_t    *cl_flares;

#define CL_PLAYER_MODEL_DISABLED     0
#define CL_PLAYER_MODEL_ONLY_GUN     1
#define CL_PLAYER_MODEL_FIRST_PERSON 2
#define CL_PLAYER_MODEL_THIRD_PERSON 3

extern cvar_t    *cl_player_model;
extern cvar_t    *cl_thirdperson_angle;
extern cvar_t    *cl_thirdperson_range;

extern cvar_t    *cl_async;

//
// userinfo
//
extern cvar_t    *info_password;
extern cvar_t    *info_spectator;
extern cvar_t    *info_name;
extern cvar_t    *info_skin;
extern cvar_t    *info_rate;
extern cvar_t    *info_fov;
extern cvar_t    *info_msg;
extern cvar_t    *info_hand;
extern cvar_t    *info_gender;
extern cvar_t    *info_uf;

//
// models.c
//
extern cvar_t    *cl_testmodel;
extern cvar_t    *cl_testfps;
extern cvar_t    *cl_testalpha;
extern qhandle_t  cl_testmodel_handle;
extern vec3_t     cl_testmodel_position;

//=============================================================================

//
// main.c
//

void CL_Init(void);
void CL_Quit_f(void);
void CL_Disconnect(error_type_t type);
void CL_UpdateRecordingSetting(void);
void CL_Begin(void);
void CL_CheckForResend(void);
void CL_ClearState(void);
void CL_RestartFilesystem(bool total);
void CL_RestartRefresh(bool total);
void CL_ClientCommand(const char *string);
void CL_SendRcon(const netadr_t *adr, const char *pass, const char *cmd);
const char *CL_Server_g(const char *partial, int argnum, int state);
void CL_CheckForPause(void);
void CL_UpdateFrameTimes(void);
bool CL_CheckForIgnore(const char *s);
void CL_WriteConfig(void);

void cl_timeout_changed(cvar_t *self);

//
// precache.c
//

typedef enum {
    LOAD_NONE,
    LOAD_MAP,
    LOAD_MODELS,
    LOAD_IMAGES,
    LOAD_CLIENTS,
    LOAD_SOUNDS
} load_state_t;

void CL_ParsePlayerSkin(char *name, char *model, char *skin, const char *s);
void CL_LoadClientinfo(clientinfo_t *ci, const char *s);
void CL_LoadState(load_state_t state);
void CL_RegisterSounds(void);
void CL_RegisterBspModels(void);
void CL_RegisterVWepModels(void);
void CL_PrepRefresh(void);
void CL_UpdateConfigstring(int index);


//
// download.c
//
int CL_QueueDownload(const char *path, dltype_t type);
bool CL_IgnoreDownload(const char *path);
void CL_FinishDownload(dlqueue_t *q);
void CL_CleanupDownloads(void);
void CL_LoadDownloadIgnores(void);
void CL_HandleDownload(byte *data, int size, int percent, int decompressed_size);
bool CL_CheckDownloadExtension(const char *ext);
void CL_StartNextDownload(void);
void CL_RequestNextDownload(void);
void CL_ResetPrecacheCheck(void);
void CL_InitDownloads(void);


//
// input.c
//
void IN_Init(void);
void IN_Shutdown(void);
void IN_Frame(void);
void IN_Activate(void);

void CL_RegisterInput(void);
void CL_UpdateCmd(int msec);
void CL_FinalizeCmd(void);
void CL_SendCmd(void);


//
// parse.c
//

typedef struct {
    int type;
    vec3_t path[MAX_POI_PATH];
    vec3_t pos1;
    vec3_t pos2;
    vec3_t offset;
    vec3_t dir;
    int count;
    int color;
    int entity1;
    int entity2;
    int time;
} tent_params_t;

typedef struct {
    int entity;
    int weapon;
    bool silenced;
    // svc_muzzleflash3 only: the direction the shot was actually fired in.
    // Without it the flash can only be oriented from the monster's BODY, which
    // is wrong for every monster whose gun arm animates separately.
    bool has_dir;
    vec3_t dir;
} mz_params_t;

typedef struct {
    int     flags;
    int     index;
    int     entity;
    int     channel;
    vec3_t  pos;
    float   volume;
    float   attenuation;
    float   timeofs;
} snd_params_t;

extern tent_params_t    te;
extern mz_params_t      mz;
extern snd_params_t     snd;

void CL_ParseServerMessage(void);
void CL_SeekDemoMessage(void);
void CL_SetServerFrameTime(int fps);

// Handlers shared with the rerelease demo compatibility layer. Its message
// dispatch is a separate loop (KEX renumbered every server command above
// svc_frame), but everything those commands do once decoded is the same work
// CL_ParseServerMessage does, so it calls straight into these rather than
// growing a second copy of each.
bool CL_ParseServerData(void);
void CL_ParseConfigstring(int index);
void CL_ParseBaseline(int index, uint64_t bits);
void CL_ParseFrame(int extrabits);
void CL_ParsePrint(void);
void CL_ParseCenterPrint(void);
void CL_ParseStuffText(void);
void CL_ParseLayout(void);
void CL_ParseInventory(void);
void CL_ParseReconnect(void);


//
// kexdemo.c - Quake II rerelease (KEX) demo playback compatibility layer
//
bool CL_KexDemo_ParseServerData(int protocol);
void CL_KexDemo_ParseMessage(void);
void CL_KexDemo_SeekMessage(void);
int  CL_KexDemo_RemapConfigstring(int index);
int  CL_KexDemo_ParseEntityBits(uint64_t *bits);
void CL_KexDemo_ParseDeltaEntity(const entity_state_t *from, entity_state_t *to,
                                 int number, uint64_t bits);
void CL_KexDemo_ParsePlayerstate(const player_state_t *from, player_state_t *to);
void CL_KexDemo_EntityRemoved(int number);


//
// entities.c
//
void CL_DeltaFrame(void);
void CL_AddEntities(void);
// Adjust a gun origin so that the gun doesn't intersect with walls. Used for view weapons.
void CL_AdjustGunPosition(vec3_t viewangles, vec3_t *gun_origin);
bool CL_GetViewWeaponTransform(vec3_t origin, vec3_t angles);
void CL_CalcViewValues(void);

#if USE_DEBUG
void CL_CheckEntityPresent(int entnum, const char *what);
#endif

// the sound code makes callbacks to the client for entitiy position
// information, so entities can be dynamically re-spatialized
void CL_GetEntitySoundOrigin(int ent, vec3_t org);


//
// view.c
//
extern    int       gun_frame;
extern    qhandle_t gun_model;

void V_Init(void);
void V_Shutdown(void);
void V_RenderView(void);
void V_AddEntity(entity_t *ent);
void V_AddParticle(particle_t *p);
// Zero-sized trace against the world and the solid bmodel entities. predict.c.
// clip_bbox_entities adds the axial boxes of monsters, corpses and the like -
// see CL_TracePoint for why anything that DRAWS its hit point wants them out.
trace_t CL_TracePoint(const vec3_t start, const vec3_t end, int contentmask, bool clip_bbox_entities);
// Which entity a CL_TracePoint result hit, or NULL for the world.
centity_t *CL_TraceHitEntity(const trace_t *tr);
blood_sphere_t *V_AddBloodSphere(void);

// Wet impact sounds for landing blood - registered in CL_RegisterTEntSounds,
// played from CL_BloodStick.  One of these is picked at random per impact.
#define NUM_BLOOD_SFX 2
extern qhandle_t cl_sfx_blood_splat[NUM_BLOOD_SFX];

// The POOLING sound, played from CL_BloodPoolInto when a droplet is absorbed
// into blood that is already there instead of leaving a mark of its own.
//
// That event used to be SILENT, and it is the most common one there is once
// pooling is on: a merging droplet returns early and never reaches
// CL_BloodStick, so it never reached the impact sound either.
extern qhandle_t cl_sfx_blood_pool;
void V_AddLight(const vec3_t org, float intensity, float r, float g, float b);
void V_AddSphereLight(const vec3_t org, float intensity, float r, float g, float b, float radius);
// per-light volumetric scale for the light most recently added; negative puts it
// back on the class default. See V_SetLightVolumetricScale in view.c.
void V_SetLightVolumetricScale(float scale);
void V_AddSpotLight(const vec3_t org, const vec3_t dir, float intensity, float r, float g, float b, float width_angle, float falloff_angle);
void V_AddLightStyle(int style, float value);

//
// dynamiclights.c
//

// One rerelease dynamic_light entity, parsed out of the BSP entity lump.
//
// These no longer light the map on their own. They are DORMANT: a black debug
// sphere marking a position, and a set of values for the light editor to
// inherit from. `light replace` in lightedit.c creates a real light that stands
// in for one of these, at which point `replaced` goes true here and this entity
// stops emitting for good - the replacement is drawn instead.
typedef struct {
    vec3_t  origin;
    vec3_t  color;
    vec3_t  direction;
    float   intensity;      // shadowlightintensity, already folded with radius
    float   cone_angle;     // full cone angle in degrees
    int     style;
    int     switch_index;   // bit in CS_DYNAMICLIGHTS, or -1 for always-on
    bool    is_spot;
    float   vol_scale;      // LIGHT_VOLUMETRIC_SCALE_UNSET = class default

    // [light editor] a replacement light stands in for this entity. Owned by
    // lightedit.c, which is also what clears it.
    bool    replaced;
} cdynamiclight_t;

int CL_NumDynamicLights(void);
cdynamiclight_t *CL_GetDynamicLight(int index);

// Whether the game currently has this light switched on. A replacement light
// inherits its entity's switchability, so lightedit.c needs to ask.
bool CL_DynamicLightSwitchedOn(const cdynamiclight_t *dl);

void CL_InitDynamicLights(void);
void CL_InitMapFog(void);
void CL_LoadMapFog(void);
void CL_FreeMapFog(void);
bool CL_GetMapFog(mapfog_params_t *out);
void CL_LoadDynamicLights(void);
void CL_FreeDynamicLights(void);
void CL_AddDynamicLightsToScene(void);
float CL_LightStyleValue(int style);
void CL_UpdateBlendSetting(void);


//
// tent.c
//

typedef struct cl_sustain_s {
    int     id;
    int     type;
    int     endtime;
    int     nextthink;
    vec3_t  org;
    vec3_t  dir;
    int     color;
    int     count;
    int     magnitude;
    void    (*think)(struct cl_sustain_s *self);
} cl_sustain_t;

void CL_SmokeAndFlash(const vec3_t origin);
void CL_ImpactSmokeAndFlash(const vec3_t origin, const vec3_t dir);
// view_fx: 0 for a flash both views should see, or RF_FIRST_PERSON_FX /
// RF_REFLECTION_FX for one half of your own gun's split pair.
void CL_MuzzleFlashModel(const vec3_t origin, const vec3_t angles, int view_fx);
// as above, but with this weapon's own flash graphic; model 0 = the generic star
void CL_MuzzleFlashModel2(const vec3_t origin, const vec3_t angles,
                          int view_fx, qhandle_t model);
void CL_RegisterViewMuzzleFlashes(void);
void CL_SetupMuzzleFlashEntity(entity_t *ent, const vec3_t origin,
                               const vec3_t angles, float roll,
                               int view_fx, qhandle_t model);
// the generic starburst, for monsters and for weapons with no flash/ of their own
extern qhandle_t cl_mod_muzzleflash;
void CL_MuzzleOffset_f(void);
void CL_ViewMuzzleFlash(void);
void CL_PlayerModelMuzzleFlash(void);
void CL_AddPlayerModelFlash(void);

void CL_RegisterTEntSounds(void);
void CL_RegisterTEntModels(void);
void CL_ParseTEnt(void);
void CL_AddTEnts(void);
float CL_CompassFade(int endtime);
void CL_ClearTEnts(void);
void CL_InitTEnts(void);


//
// predict.c
//
void CL_PredictAngles(void);
void CL_PredictMovement(void);
void CL_CheckPredictionError(void);


//
// effects.c
//
#define PARTICLE_GRAVITY        120
#define BLASTER_PARTICLE_COLOR  0xe0

/* The soldier that fires the blue blaster (monster_fire_blueblaster, via
   EF_BLUEHYPERBLASTER / MZ_BLUEHYPERBLASTER) lit the room with a PURE blue
   light - literally (0, 0, 1) at both the muzzle and the bolt in flight.
   Under the path tracer a light with no red or green in it at all leaves
   every surface it touches with only its blue channel, so a grey wall goes
   navy and skin goes black, which reads as a colour bug rather than as a
   blue weapon.

   The bolt ITSELF was never that saturated: models/objects/blaser/skin.tga
   averages rgb(73, 74, 122), a soft periwinkle. Only the dynamic light was
   pure. These are the light to match it with - rgb(100, 100, 255) over 255.

   Both emitters use this, so they cannot drift apart. */
#define BLUEBLASTER_LIGHT_R     (100.0f / 255.0f)
#define BLUEBLASTER_LIGHT_G     (100.0f / 255.0f)
#define BLUEBLASTER_LIGHT_B     (255.0f / 255.0f)
#define INSTANT_PARTICLE    -10000.0f

typedef struct cparticle_s {
    struct cparticle_s    *next;

    float   time;

    vec3_t  org;
    vec3_t  vel;
    vec3_t  accel;
    int     color;      // -1 => use rgba
    float   alpha;
    float   alphavel;
    color_t rgba;
	float   brightness;
    int     particleType;

    // Draw this one as shaded sphere geometry instead of a camera-facing quad.
    //
    // A FLAG OF ITS OWN, not another particleType value: the ludicrous-gibs
    // blood trail is already PARTICLE_TYPE_SHORT_LIVED and is also blood, so the
    // two properties have to coexist. Folding "is a sphere" into the type enum
    // costs that trail either its 2-second cull or its spheres, depending which
    // assignment wins - which is exactly why gibs had no spheres at first.
    bool    is_blood_sphere;

    // Stable geometry slot for the renderer, held for this droplet's whole life
    // and released when it dies. -1 when this is not a blood sphere.
    int     blood_slot;

    // is_blood_sphere only. The droplet's motion is analytic (see
    // CL_AddParticles), so there is no integrated state to read a previous
    // position out of - and a shaded sphere needs one every frame or it ghosts
    // under DLSS-RR. Keeping last frame's emitted origin here is exact at any
    // frame rate and costs one vec3.
    vec3_t  prev_org;
    float   radius;
    float   seed;       // per-droplet phase for the animated surface ripple

    // Collision simulation (cl_blood_collision).  A blood sphere is the one
    // particle in this system that is NOT analytic: ordinary particles compute
    // their position as org + vel*t + accel*t^2 from their spawn time, which has
    // no way to express "stopped when it hit a wall".  So these integrate step by
    // step instead, and org/vel are live state rather than initial conditions.
    int     blood_state;        // BLOOD_AIRBORNE / BLOOD_STUCK
    vec3_t  blood_normal;       // surface normal while stuck
    float   blood_flatten;      // 1 = round; drops toward the splat target on impact
    vec3_t  blood_tangent;      // long axis in the surface plane
    float   blood_stretch;      // elongation along blood_tangent; 1 = round
    float   blood_cross;        // half-extent ACROSS it, same units; 1 = one droplet

    // RESHAPING WHILE IT SLIDES.  blood_tangent starts as the impact direction,
    // and for a splat that never moves again that is the whole story.  But a pool
    // that lands smeared along X and then runs downhill along Y kept its X smear
    // the whole way, because nothing touched the tangent after the landing frame.
    // A mark's direction is the direction the blood went, and blood that keeps
    // going has a new one.  See CL_BloodReshapeSlide.
    //
    // blood_slide_axis is the desired axis, turned smoothly every frame; it is
    // NOT in blood_sphere_t, so moving it costs nothing.  blood_tangent is
    // committed from it only once the two differ enough to matter, because that
    // one IS in blood_sphere_t and the renderer caches on the whole struct.
    vec3_t  blood_slide_axis;
    float   blood_stretch_base; // the elongation it landed with
    float   blood_cross_base;   // and the cross extent, in the CURRENT frame
    float   blood_cross_run;    // the cross extent the narrowing is decaying from
    float   blood_slide_dist;   // how far it has travelled since landing
    float   blood_narrow_dist;  // distance run since blood_cross_run was captured

    // SETTLING: a pool that has stopped running is not finished.
    //
    // A run draws the mark out along the direction of travel, which is right
    // while the blood is moving and wrong the moment it stops - what is left at
    // the bottom of a slope is a POOL, not a streak.  So the length the run
    // added is handed back over cl_blood_slide_relax seconds, and at the end of
    // that the splat asks ONCE whether it has come to rest against blood that is
    // already lying there.
    //
    // All three are deliberately OUTSIDE blood_sphere_t: the renderer caches
    // geometry on a memcmp of that struct, so a value that moves every frame
    // pays a full per-vertex rebuild every frame.  blood_stretch is the
    // quantized value the renderer sees; these are the continuous state it is
    // derived from - the same split blood_slide_axis needs, and for the same
    // reason.  Thresholding a smoothed value against ITSELF never fires.
    // A RUN THAT HAS REACHED THE FOOT OF A SURFACE HANDS ITS BLOOD OVER, it
    // does not teleport.
    //
    // The probe that keeps a running splat attached goes solid at the inside of
    // a wall/floor corner, so a run arriving at the bottom of a wall took the
    // "ran off the end of the surface" branch - which resets the stretch, the
    // tangent and the flatten, i.e. COLLAPSES THE WHOLE STREAK TO A SPHERE in
    // one frame, and re-lands it as a single round floor splat. Matt, playing:
    // "as soon as a part of the splat hits the floor the whole thing disappears
    // to a splat on the floor... it should be a tiny floor puddle and expand to
    // the normal size it would be if it hit the floor as the rest of the splat
    // makes it to the floor."
    //
    // So the mark stays where it is and DRAINS: its area is delivered to a pool
    // at the foot over cl_blood_drain seconds, in quanta, growing the pool as it
    // goes. The streak shrinks to match, and because the trail is anchored at
    // the leading edge it contracts toward the foot of the run - into the pool
    // it is feeding - for free.
    bool    blood_arrived;      // reached the foot; no longer slides
    float   blood_drain;        // seconds of handing-over left
    float   blood_drain_total;  // area (radius squared) it arrived with
    float   blood_drain_done;   // area delivered so far
    vec3_t  blood_feed_org;     // where the pool is forming
    vec3_t  blood_feed_normal;

    bool    blood_settling;     // armed once, on the frame the run stopped
    float   blood_settle;       // seconds of settling left
    float   blood_stretch_run;  // the elongation the run ended with
    float   blood_cross_rest;   // and the cross extent it ended with

    // The brush model this splat is riding, or -1 for the world.  A door, lift
    // or platform carries its blood with it; without this a splat is a world
    // position that the surface under it simply drives away from, which reads as
    // exactly the same "floating in mid-air" artifact that bounding boxes give.
    //
    // Only BRUSH models are ever adopted.  A rigid transform is the whole truth
    // for one of those, and the local coordinates below stay valid forever.
    // blood_ent_id is centity_t::id at the moment of contact, so a recycled
    // entity slot detaches the splat instead of teleporting it somewhere new.
    int     blood_ent;
    int     blood_ent_id;
    vec3_t  blood_local_org;    // contact point in the entity's own frame
    vec3_t  blood_local_normal; // and its surface normal, likewise

    // HOW FAR THE SURFACE ACTUALLY REACHES, in eight directions around the rim.
    //
    // A splat is a DISC and it lands wherever its CENTRE lands, so one that lands
    // near a ledge draws half of itself out over the drop.  The collision trace
    // cannot see that: it answers "did the droplet hit something", which it did.
    //
    // Eight radial probes measure the surface's extent once the droplet has
    // stopped.  The renderer clips the outline to it, and the unsupported side is
    // where drips are released from.  Eight nibbles, each the reach in TENTHS of
    // the nominal rim radius - so 10 is exactly the rim and 15 covers the outward
    // lobes pt_blood_wobble can add.  BLOOD_RIM_FULL is "supported all the way
    // round", which is the great majority of splats and takes exactly the path
    // the mesh always took.
    //
    // Measured ONCE, amortised across frames, and again only if pooling grows the
    // splat - never per frame.  See CL_BloodProbeRim.
    uint32_t blood_rim;
    bool     blood_rim_dirty;   // queued for (re)probing

    // WHICH OF THOSE EIGHT WERE STOPPED BY MATERIAL RATHER THAN BY A DROP, one
    // bit per sample.  The distinction is the whole reason it exists: both come
    // back as a short reach and the outline is clipped to either, but a pool
    // that runs into a wall must NOT be shoved through it the way an overhanging
    // one is shoved off a ledge.  CL_BloodOverhang skips these samples; a pool
    // blocked all the way round is simply a pool that has found its shape.
    //
    // Not in blood_sphere_t - the renderer only ever needs the combined reach.
    uint32_t blood_block;

    // A pool spreads ALONG a wall it has run into, once, per resting place.  The
    // flag is the bound: growing along the wall moves the outline, which re-queues
    // the probe, which would otherwise find the new blocked directions and grow it
    // again for the rest of the level.
    bool     blood_wall_spread;

    // Distance run when the rim was last measured.  A sliding splat is re-probed
    // on a DISTANCE step rather than per frame - see the probe site in
    // CL_AddParticles for why per frame is not an option.
    float    blood_rim_dist;

    // Shoves left before an overhanging pool gives up trying to leave.  One
    // shove usually carries it off the lip; a pool parked in a corner can be
    // measured as overhanging in a direction that does not actually get it
    // anywhere, and without a bound it would re-measure and re-shove itself for
    // the rest of the level.  Out of tries it simply stays, clipped to the edge,
    // which is still not floating.
    int      blood_edge_tries;

    // BLOOD_DISSOLVE only: when it reached the water. The wobble's churn runs off
    // this rather than off the fade, because a merge rewinds the fade (see
    // CL_BloodMergeWaterPools) and the outline must not jump back with it.
    float    blood_born;
} cparticle_t;

#define BLOOD_AIRBORNE  0
#define BLOOD_STUCK     1
#define BLOOD_DISSOLVE  2   // hit water: spreading out and fading - CL_BloodDissolve

// BLOOD_RIM_SAMPLES / BLOOD_RIM_FULL / BLOOD_RIM_SCALE live in refresh.h, beside
// blood_sphere_t - the client measures the reach and the renderer consumes it, so
// the encoding has to be one definition seen by both.

// cparticle_t::particleType
#define PARTICLE_TYPE_NORMAL        0
#define PARTICLE_TYPE_SHORT_LIVED   1   // culled after 2 seconds regardless of alpha

// Promote a freshly allocated particle to a blood droplet, if the feature is on.
// `scale` sizes it against cl_blood_sphere_radius: 1.0 for wound spray, smaller
// for the drips off a flying gib.  Call it AFTER setting org and vel.
void CL_MakeBloodSphere(cparticle_t *p, float scale);

typedef struct cdlight_s {
    int     key;        // so entities can reuse same entry
    vec3_t  color;
    vec3_t  origin;
    float   radius;
    float   die;        // stop lighting after this time
    float   decay;      // drop this each second
	vec3_t  velosity;     // move this far each second
} cdlight_t;

void CL_BigTeleportParticles(const vec3_t org);
void CL_RocketTrail(const vec3_t start, const vec3_t end, centity_t *old);
void CL_DiminishingTrail(const vec3_t start, const vec3_t end, centity_t *old, int flags);
void CL_FlyEffect(centity_t *ent, const vec3_t origin);
void CL_BfgParticles(entity_t *ent);
void CL_ItemRespawnParticles(const vec3_t org);
void CL_InitEffects(void);
void CL_ClearEffects(void);
void CL_BlasterParticles(const vec3_t org, const vec3_t dir);
void CL_ExplosionParticles(const vec3_t org);
void CL_BFGExplosionParticles(const vec3_t org);
void CL_BlasterTrail(const vec3_t start, const vec3_t end);
// "Hyoer" is the spelling in effects.c; declared as-is rather than renamed.
void CL_HyoerBlasterTrail(vec3_t start, vec3_t end);
void CL_NonDiminishingTrail(vec3_t start, vec3_t end, centity_t *old, int flags);
void CL_OldRailTrail(void);
void CL_BubbleTrail(const vec3_t start, const vec3_t end);
void CL_FlagTrail(const vec3_t start, const vec3_t end, int color);
void CL_MuzzleFlash(void);
void CL_MuzzleFlash2(void);
void CL_TeleporterParticles(const vec3_t org);
void CL_TeleportParticles(const vec3_t org);
void CL_ParticleEffect(const vec3_t org, const vec3_t dir, int color, int count);
void CL_ParticleEffectWaterSplash(const vec3_t org, const vec3_t dir, int color, int count);
void CL_BloodParticleEffect(const vec3_t org, const vec3_t dir, int color, int count);
void CL_ParticleEffect2(const vec3_t org, const vec3_t dir, int color, int count);
cparticle_t *CL_AllocParticle(void);
void CL_RunParticles(void);
void CL_AddParticles(void);
cdlight_t *CL_AllocDlight(int key);
void CL_AddDLights(void);
void CL_SetLightStyle(int index, const char *s);
void CL_AddLightStyles(void);

//
// newfx.c
//

void CL_BlasterParticles2(const vec3_t org, const vec3_t dir, unsigned int color);
void CL_BlasterTrail2(const vec3_t start, const vec3_t end);
void CL_DebugTrail(const vec3_t start, const vec3_t end);
void CL_Flashlight(int ent, const vec3_t pos);
void CL_ForceWall(const vec3_t start, const vec3_t end, int color);
void CL_BubbleTrail2(const vec3_t start, const vec3_t end, int dist);
void CL_Heatbeam(const vec3_t start, const vec3_t end);
void CL_ParticleSteamEffect(const vec3_t org, const vec3_t dir, int color, int count, int magnitude);
void CL_TrackerTrail(const vec3_t start, const vec3_t end, int particleColor);
void CL_TagTrail(const vec3_t start, const vec3_t end, int color);
void CL_ColorFlash(const vec3_t pos, int ent, int intensity, float r, float g, float b);
void CL_Tracker_Shell(const vec3_t centre, float radius);
void CL_MonsterPlasma_Shell(const vec3_t origin);
void CL_ColorExplosionParticles(const vec3_t org, int color, int run);
void CL_ParticleSmokeEffect(const vec3_t org, const vec3_t dir, int color, int count, int magnitude);
void CL_Widowbeamout(cl_sustain_t *self);
void CL_Nukeblast(cl_sustain_t *self);
void CL_WidowSplash(void);
void CL_IonripperTrail(const vec3_t start, const vec3_t end);
void CL_TrapParticles(centity_t *ent, const vec3_t origin);
void CL_BarrelBurnEffect(centity_t *ent, const vec3_t origin);
void CL_ParticleEffect3(const vec3_t org, const vec3_t dir, int color, int count);
void CL_ParticleSteamEffect2(cl_sustain_t *self);


//
// demo.c
//
void CL_InitDemos(void);
void CL_CleanupDemos(void);
void CL_DemoFrame(int msec);
bool CL_WriteDemoMessage(sizebuf_t *buf);
void CL_EmitDemoFrame(void);
void CL_EmitDemoSnapshot(void);
void CL_FirstDemoFrame(void);
void CL_Stop_f(void);
demoInfo_t *CL_GetDemoInfo(const char *path, demoInfo_t *info);


//
// lightedit.c
//
void LE_Init(void);
void LE_LoadLights(void);
void LE_FreeLights(void);
void LE_AddLightsToScene(void);


//
// locs.c
//
void LOC_Init(void);
void LOC_LoadLocations(void);
void LOC_FreeLocations(void);
void LOC_UpdateCvars(void);
void LOC_AddLocationsToScene(void);


//
// console.c
//
void Con_Init(void);
void Con_PostInit(void);
void Con_Shutdown(void);
void Con_DrawConsole(void);
void Con_RunConsole(void);
void Con_Print(const char *txt);
void Con_ClearNotify_f(void);
void Con_ToggleConsole_f(void);
void Con_ClearTyping(void);
void Con_Close(bool force);
void Con_Popup(bool force);
void Con_SkipNotify(bool skip);
void Con_RegisterMedia(void);
void Con_CheckResize(void);

void Key_Console(int key);
void Key_Message(int key);
void Char_Console(int key);
void Char_Message(int key);


//
// refresh.c
//
void    CL_InitRefresh(void);
void    CL_ShutdownRefresh(void);
void    CL_RunRefresh(void);


//
// screen.c
//
extern vrect_t      scr_vrect;        // position of render window

void    SCR_Init(void);
void    SCR_Shutdown(void);
void    SCR_UpdateScreen(void);
void    SCR_SizeUp(void);
void    SCR_SizeDown(void);
void    SCR_CenterPrint(const char *str);
void    SCR_FinishCinematic(void);
void    SCR_StopCinematic(void);
void    SCR_PlayCinematic(const char *name);
void    SCR_RunCinematic(void);
void    SCR_BeginLoadingPlaque(void);
void    SCR_EndLoadingPlaque(void);
void    SCR_TouchPics(void);
void    SCR_RegisterMedia(void);
void    SCR_ModeChanged(void);
void    SCR_LagSample(void);
void    SCR_LagClear(void);
void    SCR_SetCrosshairColor(void);

// the rerelease item wheel - see the ITEM WHEEL block in screen.c
bool    SCR_ItemWheelKey(bool down, bool autorepeat);
bool    SCR_ItemWheelMouse(float dx, float dy);
void    SCR_ItemWheelAbort(void);
float   SCR_ItemWheelPhase(void);
float   SCR_ItemWheelBlur(void);
qhandle_t SCR_GetFont(void);
void    SCR_SetHudAlpha(float alpha);

float   SCR_FadeAlpha(unsigned startTime, unsigned visTime, unsigned fadeTime);
int     SCR_DrawStringEx(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font);
void    SCR_DrawStringMulti(int x, int y, int flags, size_t maxlen, const char *s, qhandle_t font);

void    SCR_ClearChatHUD_f(void);
void    SCR_AddToChatHUD(const char *text);


//
// ascii.c
//
void CL_InitAscii(void);


//
// http.c
//
#if USE_CURL
void HTTP_Init(void);
void HTTP_Shutdown(void);
void HTTP_SetServer(const char *url);
int HTTP_QueueDownload(const char *path, dltype_t type);
void HTTP_RunDownloads(void);
void HTTP_CleanupDownloads(void);
#else
#define HTTP_Init()                     (void)0
#define HTTP_Shutdown()                 (void)0
#define HTTP_SetServer(url)             (void)0
#define HTTP_QueueDownload(path, type)  Q_ERR(ENOSYS)
#define HTTP_RunDownloads()             (void)0
#define HTTP_CleanupDownloads()         (void)0
#endif

//
// gtv.c
//

#if USE_CLIENT_GTV
void CL_GTV_EmitFrame(void);
void CL_GTV_WriteMessage(byte *data, size_t len);
void CL_GTV_Resume(void);
void CL_GTV_Suspend(void);
void CL_GTV_Transmit(void);
void CL_GTV_Run(void);
void CL_GTV_Init(void);
void CL_GTV_Shutdown(void);
#else
#define CL_GTV_EmitFrame()              (void)0
#define CL_GTV_WriteMessage(data, len)  (void)0
#define CL_GTV_Resume()                 (void)0
#define CL_GTV_Suspend()                (void)0
#define CL_GTV_Transmit()               (void)0
#define CL_GTV_Run()                    (void)0
#define CL_GTV_Init()                   (void)0
#define CL_GTV_Shutdown()               (void)0
#endif

//
// crc.c
//
byte COM_BlockSequenceCRCByte(byte *base, size_t length, int sequence);

//
// effects.c
//
void FX_Init(void);

// RTX development feature that loads and spawns a set of material sample balls
#define CL_RTX_SHADERBALLS 1

