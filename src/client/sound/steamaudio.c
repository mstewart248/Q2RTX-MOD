/*
Copyright (C) 2026 Matt Stewart

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
// steamaudio.c -- HRTF and traced occlusion for the OpenAL backend, through
// Valve's Steam Audio (phonon.dll)
//
// OpenAL places each sound itself, and the only thing it can be told about the
// world is a reverb preset. Steam Audio does not play sound at all: it is a set
// of effects a program runs on its own sample buffers. So with s_steamaudio 1
// this file is the mixer for effect sounds:
//
//   Every effect channel that is not an interface sound becomes a voice here
//   instead of an OpenAL source. On OpenAL's mixer thread, a frame at a time
//   (SA_FRAME samples at the device rate), each voice is resampled from its
//   sfx, run through a Steam Audio direct effect - the EQ of whatever lies
//   between it and the listener - and a binaural effect (HRTF), and summed
//   into a stereo bus. The bus plays through an OpenAL source fed by a
//   callback buffer, with direct channels so OpenAL does not pan it a second
//   time.
//
//   The room reverb (reverb.c) is fed by a second callback source, muted on
//   its direct path: a mono mix of the voices before the HRTF, each at the
//   level OpenAL would send to the room from an ordinary source that far
//   away (SA_RoomGain). Sending the binaural bus itself made everything ring
//   - every distant sound reached the room at full level, and the HRTF's
//   near-ear gain on top.
//
//   Occlusion is traced on the game thread against the collision BSP and the
//   brush entities, so a closed door muffles exactly where it blocks and an
//   open one does not. A few points around each origin are tested as well as
//   the origin itself, so a sound half round a corner is half muffled instead
//   of switching. What a blocked path lets through depends on what it crosses:
//   sa_materials, by footstep material word (the textures/<name>.mat words
//   footsteps.c uses), so a metal wall passes more low end than rock.
//
//   A blocked sound can also come over or round what is in the way
//   (SA_FindPath): past the edge of a crate or a pillar between the two, or
//   round a corner, through a point the listener can see that can see the
//   source, or a point the source can see that the listener can, or one of
//   each. Such a path loses a little for its length and its bends, the highs
//   more than the lows, and the sound seems to come from the way it arrives.
//
//   Sound off walls (s_reflections) is Steam Audio's reflection simulation,
//   on a thread of its own (SA_SimThread): rays from the listener bounce
//   round a copy of the map - the world's faces and the brush entities, each
//   face absorbing by what its footstep word says it is made of - and give
//   each of a few slots an impulse response of everything that reaches the
//   listener off the walls from there. The loudest sources of sound have a
//   slot each and one more is at the listener, for everything else. On the
//   mixer thread each voice is convolved with its slot's response, and the
//   lot is summed as Ambisonics and decoded to stereo. It takes over
//   from the room reverb (reverb.c), which stays the fallback until the
//   first simulation of a map is in. See REFLECTIONS.
//
//   Sound that goes round (s_pathing) is Steam Audio's pathing, on the same
//   thread: probes over every floor of the map, baked once per map into the
//   shortest ways between them, give each hidden voice the way it comes round
//   - how loud, how dulled by its bends, from which way - heard as a second
//   voice from there. Where a source or the listener has no probe in reach,
//   SA_FindPath's corner paths stand in. See PATHING.
//
//   With s_diffraction, the GPU works out the same for each hidden voice
//   from the world's corners, by how sound bends round a wedge, and that
//   takes the place of the probes' way wherever one corner gets the sound
//   round. See DIFFRACTION.
//
// Interface sounds (AL_ChannelIsDry) and the music stream stay ordinary OpenAL
// sources.
//
// The transmission EQs below are still louder than real walls, from before
// pathing, when they stood in for the sound that goes round.
//
// phonon.dll is loaded at run time. Without it, or with s_steamaudio 0, the
// OpenAL path is exactly as it was, without the room reverb too: the stock
// sound. Occlusion, like the reflections and pathing, is part of the menu's
// "ray traced sound" (s_raytracing).
//

#include "sound.h"

#if USE_STEAMAUDIO

#include "qal/fixed.h"
#include <AL/alc.h>
#include <AL/alext.h>
#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <phonon.h>

#define SA_FRAME            512     // samples per Steam Audio frame: 10.7 ms at 48 kHz
#define SA_UNITS_PER_METER  32.0f   // the player is 56 units, 1.75 m

#define SA_RING             4       // frames kept for the two OpenAL sources

#define SA_TRACE_MSEC       33      // how often a voice re-traces its occlusion
#define SA_SAMPLE_RADIUS    12.0f   // points around the origin tested with it
#define SA_VOICE_DROP       8.0f    // a body's sound comes from this far under the top of its box: see SA_VoicePoint
#define SA_MAX_WALLS        3       // walls a blocked path is followed through
#define SA_BURIED_SLOP      32.0f   // see SA_Walk
#define SA_SMOOTH_SEC       0.06f   // occlusion changes ease in over about this

// paths round corners: see SA_FindPath
#define SA_RELAY_RAYS       26      // from the listener
#define SA_RELAY_REACH      2048.0f
#define SA_RELAY_STEP       128.0f  // points along a ray, about this far apart
#define SA_RELAY_PER_RAY    6
#define SA_RELAY_BACKOFF    24.0f   // kept this far short of the wall a ray hits
#define SA_RELAY_MSEC       100     // the listener's are rebuilt this often
#define SA_RELAY_MOVE       16.0f   // or when it moves this far
#define SA_SRC_RAYS         10      // from a blocked source
#define SA_SRC_REACH        1024.0f
#define SA_SRC_PER_RAY      2
#define SA_PATH_CANDIDATES  16      // shortest one-bend paths kept for two bends
#define SA_PATH_TESTS       4       // traces each kind of path may try
#define SA_EDGE_TRACES      32      // over or past an obstacle: see SA_EdgePath
#define SA_EDGE_SIDES       3       // the first of its directions: over it, and to either side
#define SA_PATH_RETRY_MSEC  100     // a voice with no way round looks again this often
#define SA_GO_ON_MSEC       250     // see SA_GoesOn
#define SA_GO_ON_DIST       32.0f
#define SA_YOUNG_MSEC       150     // a sound this new takes its first way round at once: see SA_Occlude

// sound off walls: see REFLECTIONS
#define SA_REFL_MAX_SLOTS   31      // sources with a response of their own, at most: a channel
                                    // each but the listener's, and with the room's the 32
                                    // sources the GPU tracer takes (SOUND_TRACE_MAX_SOURCES)
#define SA_REFL_GPU_BUDGET  (8192 * 9)  // rays a GPU run shares out among its sources
#define SA_REFL_MIN_RAYS    1024    // a source's at the least, shared out or not
#define SA_REFL_IDLE_FRAMES 20      // a slot rung out is still run this long: see SA_MixReflections
#define SA_REFL_ORDER       1       // Ambisonics order of the reflected sound,
#define SA_REFL_CHANNELS    4       // which makes this many channels
#define SA_REFL_MAX_RAYS    8192
#define SA_REFL_DIFFUSE     32      // directions a diffuse bounce picks from
#define SA_REFL_MIN_DIST    1.0f    // metres: a wall this near a source does not count (Steam Audio's own)
#define SA_REFL_FULL_DIST   64.0f   // see SA_ReflSend
#define SA_REFL_OWN         0.5f    // the player's own sounds, as from half a metre: see SA_Reflect
#define SA_REFL_HIDDEN      0.3f    // see SA_Reflect
#define SA_REFL_MAX_SEND    32.0f
#define SA_LIMIT_RELEASE    0.15f   // seconds: see SA_Limit
#define SA_DUCK_KEY         0.25f   // the player's weapon at this level ducks the rest fully: see SA_Duck
#define SA_DUCK_RELEASE     0.15f   // seconds: see SA_Duck
#define SA_SIM_MIN_MSEC     25      // between two runs at least: see SA_MixReflections
#define SA_SIM_MOVE         8.0f    // what moves less than this is simulated where it was
#define SA_SIM_MOVER_MSEC   2000    // a brush entity moving longer than this, a fan or a train,
#define SA_SIM_MOVER_EVERY  1000    // is put where it has got to only this often
#define SA_SLOT_READY_DIST  64.0f   // a slot's response serves a sound this near where it was simulated
#define SA_SLOT_KEEP_MSEC   5000    // a slot whose source falls quiet follows it this long
#define SA_SLOT_TAKEOVER    4.0f    // a new source this much louder takes a playing source's slot
#define SA_SIM_MAX_BMODELS  1024    // brush entities placed per run

// sound that goes round: see PATHING
#define SA_PATH_MAX_PROBES  2000    // over the floors; the bake's memory goes as the square
#define SA_PATH_MAX_SPOTS   512     // and where things stand
#define SA_PATH_HEIGHT      48.0f   // a probe's height over its floor
#define SA_PATH_STEP        8.0f    // a column is searched for floors in steps of this
#define SA_PATH_RADIUS      1.25f   // of a probe's reach, in column spacings
#define SA_PATH_OFF_GRID    0.37f   // units every probe is moved by: see SA_ProbeColumns
#define SA_PATH_VIS_SAMPLES 4       // points in each of two probes, all pairs tested
#define SA_PATH_VIS_RADIUS  0.5f    // metres round a probe they are picked from
#define SA_PATH_VIS_SHARE   0.1f    // of the rays, as a share of the points, that must get through
#define SA_PATH_VIS_RANGE   50.0f   // metres: probes farther apart are not tested
#define SA_PATH_RANGE       100.0f  // metres: no longer way is kept
#define SA_PATH_STALE_MSEC  250     // a run older than this is not used
#define SA_PATH_MIN_SPATIAL 0.3f    // a sound coming round every way at once is this placed
#define SA_PATH_FILE_VERSION 1
#define SA_SH_C0            0.28209479f // the order-0 spherical harmonic, 1 / sqrt(4 pi)
#define SA_SQRT3            1.7320508f  // a first-order harmonic over it, at its peak

// what OpenAL's sources are given (al.c), which SA_RoomGain follows
#define SA_AL_MAX_DISTANCE  8192.0f

// A sound with no direction - the player's own - comes out of the binaural
// effect at full level in BOTH ears, where OpenAL spreads it at -3 dB a side.
// Matching OpenAL keeps the player's weapons where they sat against everything
// else, so the unplaced part of a voice is scaled by this. (The weapons then
// get s_steamaudio_weapon on top: see SA_Duck.)
#define SA_CENTRE_GAIN      0.70710678f

#ifdef _WIN32
#define SA_LIBNAME          "phonon.dll"
#elif defined(__APPLE__)
#define SA_LIBNAME          "libphonon.dylib"
#else
#define SA_LIBNAME          "libphonon.so"
#endif

/*
==============================================================================

LIBRARY

==============================================================================
*/

#define SA_IMPORTS \
    SA_IMP(IPLerror, iplContextCreate, (IPLContextSettings *, IPLContext *)) \
    SA_IMP(void, iplContextRelease, (IPLContext *)) \
    SA_IMP(IPLerror, iplAudioBufferAllocate, (IPLContext, IPLint32, IPLint32, IPLAudioBuffer *)) \
    SA_IMP(void, iplAudioBufferFree, (IPLContext, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplHRTFCreate, (IPLContext, IPLAudioSettings *, IPLHRTFSettings *, IPLHRTF *)) \
    SA_IMP(void, iplHRTFRelease, (IPLHRTF *)) \
    SA_IMP(IPLerror, iplBinauralEffectCreate, (IPLContext, IPLAudioSettings *, IPLBinauralEffectSettings *, IPLBinauralEffect *)) \
    SA_IMP(void, iplBinauralEffectRelease, (IPLBinauralEffect *)) \
    SA_IMP(void, iplBinauralEffectReset, (IPLBinauralEffect)) \
    SA_IMP(IPLAudioEffectState, iplBinauralEffectApply, (IPLBinauralEffect, IPLBinauralEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplPanningEffectCreate, (IPLContext, IPLAudioSettings *, IPLPanningEffectSettings *, IPLPanningEffect *)) \
    SA_IMP(void, iplPanningEffectRelease, (IPLPanningEffect *)) \
    SA_IMP(void, iplPanningEffectReset, (IPLPanningEffect)) \
    SA_IMP(IPLAudioEffectState, iplPanningEffectApply, (IPLPanningEffect, IPLPanningEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplDirectEffectCreate, (IPLContext, IPLAudioSettings *, IPLDirectEffectSettings *, IPLDirectEffect *)) \
    SA_IMP(void, iplDirectEffectRelease, (IPLDirectEffect *)) \
    SA_IMP(void, iplDirectEffectReset, (IPLDirectEffect)) \
    SA_IMP(IPLAudioEffectState, iplDirectEffectApply, (IPLDirectEffect, IPLDirectEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplSceneCreate, (IPLContext, IPLSceneSettings *, IPLScene *)) \
    SA_IMP(void, iplSceneRelease, (IPLScene *)) \
    SA_IMP(void, iplSceneCommit, (IPLScene)) \
    SA_IMP(IPLerror, iplStaticMeshCreate, (IPLScene, IPLStaticMeshSettings *, IPLStaticMesh *)) \
    SA_IMP(void, iplStaticMeshRelease, (IPLStaticMesh *)) \
    SA_IMP(void, iplStaticMeshAdd, (IPLStaticMesh, IPLScene)) \
    SA_IMP(IPLerror, iplInstancedMeshCreate, (IPLScene, IPLInstancedMeshSettings *, IPLInstancedMesh *)) \
    SA_IMP(void, iplInstancedMeshRelease, (IPLInstancedMesh *)) \
    SA_IMP(void, iplInstancedMeshAdd, (IPLInstancedMesh, IPLScene)) \
    SA_IMP(void, iplInstancedMeshUpdateTransform, (IPLInstancedMesh, IPLScene, IPLMatrix4x4)) \
    SA_IMP(IPLerror, iplSimulatorCreate, (IPLContext, IPLSimulationSettings *, IPLSimulator *)) \
    SA_IMP(void, iplSimulatorRelease, (IPLSimulator *)) \
    SA_IMP(void, iplSimulatorSetScene, (IPLSimulator, IPLScene)) \
    SA_IMP(void, iplSimulatorCommit, (IPLSimulator)) \
    SA_IMP(void, iplSimulatorSetSharedInputs, (IPLSimulator, IPLSimulationFlags, IPLSimulationSharedInputs *)) \
    SA_IMP(void, iplSimulatorRunReflections, (IPLSimulator)) \
    SA_IMP(IPLerror, iplSourceCreate, (IPLSimulator, IPLSourceSettings *, IPLSource *)) \
    SA_IMP(void, iplSourceRelease, (IPLSource *)) \
    SA_IMP(void, iplSourceAdd, (IPLSource, IPLSimulator)) \
    SA_IMP(void, iplSourceSetInputs, (IPLSource, IPLSimulationFlags, IPLSimulationInputs *)) \
    SA_IMP(void, iplSourceGetOutputs, (IPLSource, IPLSimulationFlags, IPLSimulationOutputs *)) \
    SA_IMP(IPLerror, iplReflectionEffectCreate, (IPLContext, IPLAudioSettings *, IPLReflectionEffectSettings *, IPLReflectionEffect *)) \
    SA_IMP(void, iplReflectionEffectRelease, (IPLReflectionEffect *)) \
    SA_IMP(void, iplReflectionEffectReset, (IPLReflectionEffect)) \
    SA_IMP(IPLAudioEffectState, iplReflectionEffectApply, (IPLReflectionEffect, IPLReflectionEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *, IPLReflectionMixer)) \
    SA_IMP(IPLerror, iplReflectionMixerCreate, (IPLContext, IPLAudioSettings *, IPLReflectionEffectSettings *, IPLReflectionMixer *)) \
    SA_IMP(void, iplReflectionMixerRelease, (IPLReflectionMixer *)) \
    SA_IMP(void, iplReflectionMixerReset, (IPLReflectionMixer)) \
    SA_IMP(IPLAudioEffectState, iplReflectionMixerApply, (IPLReflectionMixer, IPLReflectionEffectParams *, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplAmbisonicsDecodeEffectCreate, (IPLContext, IPLAudioSettings *, IPLAmbisonicsDecodeEffectSettings *, IPLAmbisonicsDecodeEffect *)) \
    SA_IMP(void, iplAmbisonicsDecodeEffectRelease, (IPLAmbisonicsDecodeEffect *)) \
    SA_IMP(void, iplAmbisonicsDecodeEffectReset, (IPLAmbisonicsDecodeEffect)) \
    SA_IMP(IPLAudioEffectState, iplAmbisonicsDecodeEffectApply, (IPLAmbisonicsDecodeEffect, IPLAmbisonicsDecodeEffectParams *, IPLAudioBuffer *, IPLAudioBuffer *)) \
    SA_IMP(IPLerror, iplSerializedObjectCreate, (IPLContext, IPLSerializedObjectSettings *, IPLSerializedObject *)) \
    SA_IMP(void, iplSerializedObjectRelease, (IPLSerializedObject *)) \
    SA_IMP(IPLsize, iplSerializedObjectGetSize, (IPLSerializedObject)) \
    SA_IMP(IPLbyte *, iplSerializedObjectGetData, (IPLSerializedObject)) \
    SA_IMP(IPLerror, iplProbeBatchCreate, (IPLContext, IPLProbeBatch *)) \
    SA_IMP(void, iplProbeBatchRelease, (IPLProbeBatch *)) \
    SA_IMP(IPLerror, iplProbeBatchLoad, (IPLContext, IPLSerializedObject, IPLProbeBatch *)) \
    SA_IMP(void, iplProbeBatchSave, (IPLProbeBatch, IPLSerializedObject)) \
    SA_IMP(IPLint32, iplProbeBatchGetNumProbes, (IPLProbeBatch)) \
    SA_IMP(void, iplProbeBatchAddProbe, (IPLProbeBatch, IPLSphere)) \
    SA_IMP(void, iplProbeBatchCommit, (IPLProbeBatch)) \
    SA_IMP(IPLsize, iplProbeBatchGetDataSize, (IPLProbeBatch, IPLBakedDataIdentifier *)) \
    SA_IMP(void, iplPathBakerBake, (IPLContext, IPLPathBakeParams *, IPLProgressCallback, void *)) \
    SA_IMP(void, iplSimulatorAddProbeBatch, (IPLSimulator, IPLProbeBatch)) \
    SA_IMP(void, iplSimulatorRemoveProbeBatch, (IPLSimulator, IPLProbeBatch)) \
    SA_IMP(void, iplSimulatorRunPathing, (IPLSimulator))

#define SA_IMP(ret, name, args) static ret (IPLCALL *q##name) args;
SA_IMPORTS
#undef SA_IMP

// Not in Valve's API: the hook setup/build_phonon.py adds, which lets the GPU
// trace the reflections (see SA_TraceReflections). NULL with a phonon.dll
// without it.
typedef int (*sa_tracer_t)(int phase, int num_sources, const float *sources, const float *listener,
                           int rays, int bounces, float duration, int order, float min_dist,
                           int channels, int bins, float *const *fields, void *user);
static void (*qiplq2SetReflectionTracer)(sa_tracer_t tracer, void *user);

/*
==============================================================================

STATE

==============================================================================
*/

// What the game thread wants a voice doing. The mixer copies all of them once
// a frame, under param_lock, and writes back only the fields marked as its own.
typedef struct {
    unsigned        gen;            // bumped by every start: a new sound
    bool            playing;
    bool            looping;
    const int16_t   *data;
    int             numsamples;
    int             rate;
    double          start;          // sample to start from (loop sync)

    float           gain;           // master volume * distance falloff
    float           spatial;        // 0 = inside the head, 1 = fully placed
    IPLVector3      dir;            // listener space, Steam Audio axes
    float           occlusion;      // fraction of the source the listener sees
    float           trans[3];       // what the blocked part lets through
    float           air[3];         // air absorption (and a water surface)
    float           room;           // of the direct effect's output, what goes to the reverb
    bool            key;            // the player's own weapon, which the rest ducks under (SA_Duck)
    int             refl_slot;      // the reflection slot it is heard off the walls through, -1 = none
    float           refl;           // of the voice's samples, what goes to that slot
    float           path;           // of the voice's samples, what comes round the map's paths
    float           path_eq[3];     // what their bends and the air leave
    IPLVector3      path_dir;       // listener space, Steam Audio axes
    float           path_spatial;   // 1 = all from path_dir, less = from several ways
    uint64_t        queued;         // Sys_Microseconds when it was started, for sa_voices

    // the mixer's
    unsigned        done_gen;       // the sound started as this gen has run out
    unsigned        pos_gen;
    double          pos;            // where it has got to, in sfx samples
} sa_parm_t;

// the mixer thread's own per-voice state
typedef struct {
    unsigned            gen;
    bool                finished;
    double              pos;
    float               gain;       // where the last frame's ramp ended
    IPLBinauralEffect   binaural;
    IPLPanningEffect    panning;
    IPLDirectEffect     direct;

    // the way round: a second voice, from where the path arrives
    bool                path_live;  // was heard last frame
    IPLBinauralEffect   path_binaural;
    IPLPanningEffect    path_panning;
    IPLDirectEffect     path_direct;
} sa_voice_t;

// the game thread's per-channel occlusion state
typedef struct {
    bool        fresh;              // just started: take the first trace as is
    unsigned    started;            // cls.realtime it was fresh at
    unsigned    next_trace;
    float       target_occ, target_trans[3];
    float       occ, trans[3];

    // how much of what is heard comes round a corner, and from which way
    // (world space, from the listener)
    float       target_round, round;
    vec3_t      target_round_dir, round_dir;

    // no way round was found, or none over or past the obstacle: not looked
    // for again before these
    unsigned    next_path, next_edge;

    // points the source can see, for paths with a bend on its side
    bool        relays_built;
    int         num_relays;
    vec3_t      relay_origin;
    vec3_t      relays[SA_SRC_RAYS * SA_SRC_PER_RAY];

    // where it is and how loud, as last placed, for the reflection slots and
    // the paths; whose, for the next sound on the channel (SA_GoesOn)
    bool        placed;             // false: from the listener
    float       level;
    vec3_t      origin;
    unsigned    placed_at;          // cls.realtime
    int         entnum;
    float       dist_mult;

    // Steam Audio's way round (see PATHING): what the simulation is asked
    // about this sound as, and what it found, eased in like the occlusion -
    // the level against the straight line, the EQ, and from which way
    // (world space, from the listener; shorter as the ways disagree)
    unsigned    path_key;
    int         path_state;         // SA_PATH_*
    bool        path_edges;         // the way round is the GPU's, round edges
    float       target_path, target_path_eq[3];
    vec3_t      target_path_vec;
    float       path, path_eq[3];
    vec3_t      path_vec;

    // for sa_voices
    float       dist;
    int         bends;
    float       path_mid;
    bool        small;              // the way over or past a small thing won over Steam Audio's
} sa_chan_t;

// what the last pathing run said of a voice
enum {
    SA_PATH_NONE,       // no run for it yet, or no probe in reach: SA_FindPath stands in
    SA_PATH_DIRECT,     // in sight, or round nothing that bends it: the direct sound covers it
    SA_PATH_ROUND       // the ways round, possibly none
};

// A reflection slot, on the game thread: the source of sound it follows (see
// SA_AssignSlots).
typedef struct {
    int         key;                // see SA_SlotKey; 0 = none yet
    bool        live;               // a sound of it is playing
    bool        simulate;           // keep its response up to date
    unsigned    last_live;          // cls.realtime
    float       level;              // its loudest sound's
    vec3_t      origin;
    vec3_t      offset;             // from its entity's origin, while live
} sa_slot_t;

// A map's geometry for the simulation, built on the game thread and handed
// over: a mesh per BSP model, 0 the world, the rest brush entities in their
// own space. malloc'd, not Z_Malloc'd: the zone is not thread-safe, and the
// simulation thread frees it.
typedef struct {
    int         num_verts, num_tris;
    IPLVector3  *verts;
    IPLTriangle *tris;
    IPLint32    *mats;
    float       radius;             // of its farthest point from its origin, in units
} sa_mesh_t;

typedef struct {
    int         num_meshes;
    sa_mesh_t   *meshes;

    // the world's corners (SA_BuildEdges), R_SOUND_EDGE_FLOATS each, for
    // the GPU's ways round them
    int         num_edges;
    float       *edges;

    // the pathing probes (SA_BuildProbes), Steam Audio's space, and where
    // the bake of them is kept ("" = nowhere)
    int         num_probes;
    IPLSphere   *probes;
    float       spacing;            // of the columns, in units
    unsigned    checksum;           // the map's
    unsigned    probe_hash;         // SA_ProbeHash
    char        path_file[MAX_OSPATH];
} sa_geom_t;

// what one run of the simulation is to place, copied out under sim_lock
typedef struct {
    unsigned    map;
    vec3_t      listener, forward, right, up;
    struct {
        bool    enabled;
        vec3_t  origin;
    } sources[SA_REFL_MAX_SLOTS + 1];   // the last is the room's, at the listener
    int         num_bmodels;
    struct {
        int     model;              // in the map's models
        vec3_t  origin, angles;
    } bmodels[SA_SIM_MAX_BMODELS];
    int         rays, bounces;
    unsigned    interval;           // msec between reflection runs
    bool        gpu;                // s_reflections_gpu: trace them on the GPU when it can
    int         gpu_rays;
    bool        check;              // sa_reflcheck asked for this run

    // a way round for each voice: see PATHING
    struct {
        unsigned key;               // the voice's sa_chan_t path_key, 0 = none wanted
        vec3_t  origin;
        float   dist_mult;
    } paths[MAX_CHANNELS];
    unsigned    path_interval;      // msec between pathing runs
    bool        alternates;         // look for another way when a door shuts one
} sa_sim_input_t;

// what a pathing run found for a voice
typedef struct {
    unsigned    key;                // as asked, 0 = nothing
    unsigned    time;               // Sys_Milliseconds of the run
    float       eq[3];
    float       sh[4];              // first-order Ambisonics, world space
    bool        edges;              // the GPU's ways round edges, not the probes'
} sa_path_out_t;

// the simulation thread's copy of the current map: the world, and a scene of
// its own for each brush model, which its entity places in the world's
typedef struct {
    IPLScene            scene;          // NULL for a model with no faces
    IPLStaticMesh       mesh;
    IPLInstancedMesh    instance;
    float               radius;
    bool                placed;         // in the world's scene, at origin and angles
    vec3_t              origin, angles;
    unsigned            moving_since;   // Sys_Milliseconds, 0 = at rest
    unsigned            moved_at;       // when it was last put where it had got to
} sa_scene_model_t;

typedef struct {
    IPLScene            scene;
    IPLStaticMesh       world;
    int                 num_models;
    sa_scene_model_t    *models;        // per BSP model, 0 unused
    int                 num_tris;
} sa_scene_t;

static struct {
    bool                active;
    void                *lib;
    IPLContext          context;
    IPLHRTF             hrtf;
    IPLAudioSettings    audio;

    ALuint              source;         // the mix
    ALuint              buffer;
    ALuint              room_source;    // the reverb's feed, 0 = no reverb
    ALuint              room_buffer;

    SDL_mutex           *param_lock;    // parms and the shared settings below
    SDL_mutex           *mix_lock;      // held by the mixer through a callback
    bool                dead;           // under mix_lock: shutting down

    // shared, under param_lock
    sa_parm_t           parms[MAX_CHANNELS];
    float               pitch;
    bool                use_hrtf;
    IPLHRTFInterpolation interp;
    float               volume;
    float               ceiling;        // linear; 0 = no limiter (SA_Limit)
    float               duck;           // linear: the most SA_Duck turns the rest down to
    bool                refl_hrtf;      // the reflections decoded through the HRTF: see SA_MixReflections

    // mixer thread only
    sa_voice_t          voices[MAX_CHANNELS];
    sa_parm_t           snap[MAX_CHANNELS];
    IPLAudioBuffer      in, mid, out;
    float               bus[2][SA_FRAME];
    float               key_bus[2][SA_FRAME];   // the player's weapon (SA_Duck)
    float               room[SA_FRAME];
    float               limit_gain;     // SA_Limit's, 1 when it is not holding anything down
    float               duck_level;     // SA_Duck's: how loud the player's weapon is, 0-1
    float               duck_gain;      // and what it last turned the rest down to

    // under mix_lock: the last SA_RING frames mixed, which both sources'
    // callbacks read from at their own pace
    float               ring[SA_RING][SA_FRAME * 2];
    float               room_ring[SA_RING][SA_FRAME];
    uint64_t            mixed;          // frames mixed so far
    uint64_t            cursor[2];      // samples each source has taken

    // under mix_lock, for sa_voices: how long new sounds waited for the mixer
    uint64_t            wait_sum;
    unsigned            wait_count, wait_max;
    // and how loud the mix came out of the voices (before SA_Limit), how
    // many frames went over full scale, the least gain SA_Limit gave, and the
    // least SA_Duck did
    float               meter_peak, meter_gain, meter_duck;
    unsigned            meter_frames, meter_over;
    // and what mixing a frame took, and how many reflection slots it ran
    uint64_t            mix_usec_sum;
    unsigned            mix_usec_max, busy_sum, busy_max;

    // game thread only
    sa_chan_t           chans[MAX_CHANNELS];
    float               weapon_gain;    // s_steamaudio_weapon, linear
    bool                listener_buried;
    bool                listener_wet;

    // points the listener can see, for paths round corners
    int                 num_relays;
    vec3_t              relay_origin;
    unsigned            next_relays;
    vec3_t              relays[SA_RELAY_RAYS * SA_RELAY_PER_RAY];

    // for sa_voices: occlusion traces and their time, per second
    unsigned            traces, trace_usec;
    unsigned            stat_traces, stat_usec, stat_time;

    // The simulation - sound off walls, sound round corners or both - all set
    // up by SA_InitSimulation or none of it. The reflection slots are 0 to
    // num_slots - 1, and num_slots is the room's.
    bool                sim;            // the simulator, its thread and sources are up
    bool                refl;           // reflections are simulated and mixed
    bool                paths;          // pathing is
    bool                diffract;       // the GPU looks for ways round edges (see DIFFRACTION)
    bool                gpu_avail;      // s_gpu_trace_available when they were set up
    int                 sim_threads;    // for reflections and the bake
    int                 num_slots;
    float               refl_duration;  // seconds of response
    IPLSimulator        simulator;
    IPLSource           sources[SA_REFL_MAX_SLOTS + 1];
    IPLReflectionEffectParams refl_params[SA_REFL_MAX_SLOTS + 1];  // fixed once set up
    SDL_Thread          *sim_thread;
    SDL_mutex           *sim_lock;
    SDL_cond            *sim_wake;

    // under sim_lock: what the game thread asks of the simulation
    bool                sim_quit;
    bool                sim_kick;       // a slot changed hands: run soon
    bool                sim_path_kick;  // a new sound wants its way round: run the paths soon
    bool                sim_new_map;    // sim_geom is the next map's (NULL: none)
    sa_geom_t           *sim_geom;
    unsigned            sim_geom_map;
    sa_sim_input_t      sim_in;

    // the simulation thread's
    sa_sim_input_t      sim_run;
    sa_scene_t          sim_scene;
    unsigned            sim_map;        // the map sim_scene is of, 0 = none
    bool                sim_enabled[SA_REFL_MAX_SLOTS + 1];
    vec3_t              sim_used[SA_REFL_MAX_SLOTS + 1];   // where each was simulated last
    IPLSource           path_sources[MAX_CHANNELS];
    IPLProbeBatch       path_batch;     // the map's, baked or loaded; NULL = none

    // the simulation thread's: the reflections on the GPU (SA_TraceReflections)
    sa_geom_t           *gpu_geom;      // the map's faces, kept for the GPU's copy of them
    r_sound_mesh_t      *gpu_meshes;
    r_sound_geom_t      gpu_scene;      // of gpu_geom, keyed by the map
    r_sound_instance_t  *gpu_inst;      // one per mesh
    bool                gpu_run;        // the run under way is the GPU's to trace
    bool                gpu_traced;     // and it did
    const char          *gpu_why;       // why not, NULL = it did or was not asked
    unsigned            gpu_failed_map; // one it failed on, which the CPU traces from then on
    const char          *gpu_failed_why;
    unsigned            gpu_seed;
    float               *check_fields;  // sa_reflcheck's GPU half, while one runs
    bool                check_ok;
    const char          *check_why;
    float               check_gpu_msec;
    uint64_t            check_cpu_start;
    float               path_dist_mult[MAX_CHANNELS];      // for SA_PathFalloff
    sa_path_out_t       path_found[MAX_CHANNELS];
    unsigned            diff_failed_map;    // one the GPU could not do, which it is not asked about again
    const char          *diff_failed_why;

    // under param_lock: what pathing has done
    sa_path_out_t       path_out[MAX_CHANNELS];
    unsigned            path_map;       // the map the rest is of, 0 = none yet
    int                 path_probes;
    float               path_spacing;   // units
    bool                path_cached;    // the bake came from its file
    float               path_sec;       // baking or loading it took
    float               path_msec;      // the last run took
    unsigned            path_runs;
    const char          *path_error;    // why there are no paths, NULL = there are
    int                 diff_edges;     // the map's corners, on the GPU
    float               diff_msec;      // what the last run of them took
    unsigned            diff_runs;
    const char          *diff_why;      // why the last was not run, NULL = it was

    // under param_lock: what the simulation has done...
    unsigned            refl_map;       // the map the last run was on
    bool                refl_ran[SA_REFL_MAX_SLOTS + 1];   // simulated on it, at refl_origin
    vec3_t              refl_origin[SA_REFL_MAX_SLOTS + 1];
    float               refl_msec;      // the last run took
    int                 refl_sources;   // and simulated this many, the room's among them,
    int                 refl_rays;      // with this many rays each
    unsigned            refl_runs;
    bool                refl_gpu;       // the last run was traced on the GPU
    const char          *refl_gpu_why;  // why it was not, when it was asked to be
    char                *check_report;  // sa_reflcheck's, for the game thread to print; malloc'd
    int                 refl_tris;      // in the scene, 0 = none
    const char          *refl_error;    // why there is no scene
    // ...and what the mixer needs from the game thread
    bool                refl_on;        // voices go off the walls instead of to the room reverb
    bool                slot_follow[SA_REFL_MAX_SLOTS + 1];    // the simulation keeps its response up
    unsigned            refl_mix_map;
    IPLCoordinateSpace3 listener_space;

    // mixer thread only
    IPLReflectionEffect refl_effects[SA_REFL_MAX_SLOTS + 1];
    IPLReflectionMixer  refl_mixer;
    IPLAmbisonicsDecodeEffect refl_decode;
    IPLAudioBuffer      refl_in[SA_REFL_MAX_SLOTS + 1];    // what each slot is sent
    IPLAudioBuffer      refl_amb;
    unsigned            refl_mixed_map;
    bool                refl_follow[SA_REFL_MAX_SLOTS + 1];    // slot_follow, this frame
    bool                refl_sent[SA_REFL_MAX_SLOTS + 1];      // a voice sent it something this frame
    int                 refl_idle[SA_REFL_MAX_SLOTS + 1];      // frames since either
    int                 refl_ringout;   // frames a slot idle that long is skipped after

    // game thread only
    sa_slot_t           slots[SA_REFL_MAX_SLOTS];
    bool                slot_ready[SA_REFL_MAX_SLOTS + 1];
    float               refl_gain;
    float               refl_hold;      // units: see SA_ReflSend
    float               path_gain;
    unsigned            path_keys;      // handed out to voices, never 0
    unsigned            path_asked[MAX_CHANNELS];   // the keys the last input asked about
    unsigned            path_told;      // the map pathing was last reported on
    unsigned            map;            // bumped by every change of map, so never 0 for one
    const bsp_t         *map_bsp;
    unsigned            map_checksum;
} sa;

static cvar_t   *s_steamaudio;
static cvar_t   *s_steamaudio_hrtf;
static cvar_t   *s_steamaudio_interp;
static cvar_t   *s_steamaudio_volume;
static cvar_t   *s_steamaudio_limiter;
static cvar_t   *s_steamaudio_ceiling;
static cvar_t   *s_steamaudio_weapon;
static cvar_t   *s_steamaudio_duck;
static cvar_t   *s_occlusion;
static cvar_t   *s_global_floor;
static cvar_t   *s_raytracing;
static cvar_t   *s_reflections;
static cvar_t   *s_reflections_sources;
static cvar_t   *s_reflections_all;
static cvar_t   *s_reflections_duration;
static cvar_t   *s_reflections_threads;
static cvar_t   *s_reflections_rays;
static cvar_t   *s_reflections_bounces;
static cvar_t   *s_reflections_rate;
static cvar_t   *s_reflections_gain;
static cvar_t   *s_reflections_hold;
static cvar_t   *s_reflections_hrtf;
static cvar_t   *s_reflections_gpu;
static cvar_t   *s_reflections_gpu_rays;
static cvar_t   *s_pathing;
static cvar_t   *s_pathing_spacing;
static cvar_t   *s_pathing_rate;
static cvar_t   *s_pathing_alternates;
static cvar_t   *s_pathing_gain;
static cvar_t   *s_diffraction;

// Whether Steam Audio can run: phonon.dll beside the exe until it is tried,
// then whether it started. Outside `sa`, which SA_Init clears. The menu shows
// "spatial simulation" only when s_steamaudio_available is 1, and without it
// there is no room reverb either - the stock sound.
static bool     sa_available;

static void SA_SetAvailable(bool available)
{
    sa_available = available;
    Cvar_FullSet("s_steamaudio_available", available ? "1" : "0", CVAR_ROM, FROM_CODE);
}

// How much walls muffle a sound. Occlusion is part of the menu's "ray traced
// sound": spatial simulation alone is the HRTF and the room reverb.
static float SA_OcclusionStrength(void)
{
    return s_raytracing->integer ? Cvar_ClampValue(s_occlusion, 0, 1) : 0;
}

/*
==============================================================================

MATERIALS

What a wall lets through, low (400 Hz) / mid (2.5 kHz) / high (15 kHz), by the
footstep word of the surface a blocked path hits. These are NOT measured
transmission: Steam Audio's own table has concrete passing 1.5% / 0.2% / 0.1%,
which with only the corner paths of SA_FindPath modelling the sound that goes
round would make anything behind a wall vanish. They are what a listener
expects from "behind something made of X" - the lows survive, the highs go -
and they are the numbers to change when pathing arrives. The reflections now
bring a blocked sound's energy off the walls on top.

A wall is one wall however many brushes it is built from: see SA_Walk.

What a reflection off a surface loses (absorb, same bands) is NOT Steam
Audio's table either. That has bare steel and concrete losing 6-8% of the
mids and highs a bounce, which in a small Quake room rings for over a second
(an RT60 of 1.05 s in a 6 x 3 x 5 m metal box, the echo 5 dB over the direct
sound a metre off), and has rock - Quake's "boot" - ring longer than metal. A
Quake face stands for pipes, panels, grilles, rubble and machinery the map
does not model, which take a good deal more: so metal loses a quarter of its
mids (that room 0.47 s, a 24 m hall 1.6 s), rock a fifth - stone rings longer
than metal - and soft things most. Scattering is likewise higher than Steam
Audio's flat-mirror 0.05. Rock losing more than a third, darker than metal,
made mgu6m1's canyon, two thirds rock, ring like a metal room a few times the
size of base1's corridor: the player's own sounds there 0.49 s, now 0.74.

"boot" is the hard default the remaster gives rock, cinder block, concrete,
brick and some painted metal; "step" is mostly floors.

==============================================================================
*/

typedef struct {
    const char  *word;
    float       trans[3];           // what a wall of it lets through
    float       absorb[3];          // what a reflection off it loses
    float       scatter;            // of a reflection off it, what goes every way
} sa_material_t;

static const sa_material_t sa_generic = { NULL, { 0.30f, 0.12f, 0.05f }, { 0.25f, 0.30f, 0.35f }, 0.30f };

static const sa_material_t sa_materials[] = {
    { "clank",  { 0.40f, 0.14f, 0.05f }, { 0.20f, 0.25f, 0.30f }, 0.25f },  // metal plate and grating
    { "mech",   { 0.40f, 0.14f, 0.05f }, { 0.25f, 0.30f, 0.35f }, 0.40f },  // machinery
    { "energy", { 0.40f, 0.14f, 0.05f }, { 0.20f, 0.25f, 0.30f }, 0.25f },
    { "junk",   { 0.40f, 0.14f, 0.05f }, { 0.30f, 0.35f, 0.40f }, 0.45f },
    { "boot",   { 0.25f, 0.08f, 0.03f }, { 0.15f, 0.20f, 0.25f }, 0.40f },  // rock, block, concrete
    { "step",   { 0.25f, 0.08f, 0.03f }, { 0.20f, 0.30f, 0.35f }, 0.30f },  // floors
    { "tile",   { 0.28f, 0.10f, 0.03f }, { 0.08f, 0.12f, 0.15f }, 0.10f },
    { "wood",   { 0.45f, 0.20f, 0.07f }, { 0.20f, 0.20f, 0.25f }, 0.20f },
    { "glass",  { 0.55f, 0.32f, 0.14f }, { 0.15f, 0.10f, 0.10f }, 0.05f },
    { "carpet", { 0.25f, 0.06f, 0.02f }, { 0.30f, 0.70f, 0.75f }, 0.30f },
    { "grass",  { 0.22f, 0.06f, 0.02f }, { 0.60f, 0.75f, 0.85f }, 0.50f },  // earth
    { "snow",   { 0.22f, 0.06f, 0.02f }, { 0.60f, 0.75f, 0.85f }, 0.50f },
    { "meat",   { 0.30f, 0.08f, 0.03f }, { 0.35f, 0.70f, 0.80f }, 0.40f },
    { "flesh",  { 0.30f, 0.08f, 0.03f }, { 0.35f, 0.70f, 0.80f }, 0.40f },
};

// windows without a .mat word of their own
static const sa_material_t sa_window = { NULL, { 0.55f, 0.32f, 0.14f }, { 0.15f, 0.10f, 0.10f }, 0.05f };

// alpha-tested fences and grates are solid to traces but mostly holes, and
// left out of the reflections altogether
static const sa_material_t sa_fence = { NULL, { 0.90f, 0.75f, 0.60f }, { 0, 0, 0 }, 0 };

// crossing a water surface, either way
static const float sa_water[3] = { 0.60f, 0.25f, 0.08f };

// water, slime and lava, to reflections: nearly a mirror
static const sa_material_t sa_liquid = { NULL, { 0.60f, 0.25f, 0.08f }, { 0.05f, 0.05f, 0.10f }, 0.05f };

// Steam Audio's default air absorption, per metre
static const float sa_air_coeffs[3] = { 0.0002f, 0.0017f, 0.0182f };

static const sa_material_t *SA_Material(const csurface_t *surface)
{
    const char  *word;
    int         i;

    if (!surface)
        return &sa_generic;

    word = CL_SurfaceMaterial(surface);
    if (word) {
        for (i = 0; i < (int)q_countof(sa_materials); i++)
            if (!strcmp(sa_materials[i].word, word))
                return &sa_materials[i];
    }

    if (surface->flags & SURF_ALPHATEST)
        return &sa_fence;
    if (surface->flags & (SURF_TRANS33 | SURF_TRANS66))
        return &sa_window;

    return &sa_generic;
}

/*
==============================================================================

TRACING

==============================================================================
*/

// the world and every solid brush entity except the one the sound comes from:
// a door's sound originates inside the door
static void SA_Trace(trace_t *tr, const vec3_t start, const vec3_t end, int skip)
{
    trace_t     trace;
    centity_t   *ent;
    mmodel_t    *cmodel;
    vec3_t      lo, hi, emins, emaxs;
    float       r;
    int         i, j;

    sa.traces++;
    CM_BoxTrace(tr, start, end, vec3_origin, vec3_origin, cl.bsp->nodes, MASK_SOLID);

    for (j = 0; j < 3; j++) {
        lo[j] = min(start[j], end[j]);
        hi[j] = max(start[j], end[j]);
    }

    for (i = 0; i < cl.numSolidEntities; i++) {
        ent = cl.solidEntities[i];
        if (ent->current.solid != PACKED_BSP)
            continue;
        if (ent - cl_entities == skip)
            continue;
        cmodel = cl.model_clip[ent->current.modelindex];
        if (!cmodel)
            continue;
        if (tr->allsolid)
            return;

        // there are a lot of traces and a lot of brush entities: skip any whose
        // box the line's box misses
        if (VectorEmpty(ent->current.angles)) {
            VectorAdd(ent->current.origin, cmodel->mins, emins);
            VectorAdd(ent->current.origin, cmodel->maxs, emaxs);
        } else {
            r = RadiusFromBounds(cmodel->mins, cmodel->maxs);
            for (j = 0; j < 3; j++) {
                emins[j] = ent->current.origin[j] - r;
                emaxs[j] = ent->current.origin[j] + r;
            }
        }
        for (j = 0; j < 3; j++)
            if (lo[j] > emaxs[j] || hi[j] < emins[j])
                break;
        if (j < 3)
            continue;

        CM_TransformedBoxTrace(&trace, start, end, vec3_origin, vec3_origin,
                               cmodel->headnode, MASK_SOLID,
                               ent->current.origin, ent->current.angles);
        CM_ClipEntity(tr, &trace, (struct edict_s *)ent);
    }
}

static bool SA_Clear(const vec3_t start, const vec3_t end, int skip)
{
    trace_t tr;

    SA_Trace(&tr, start, end, skip);
    return tr.fraction == 1.0f && !tr.startsolid;
}

// SA_Clear, except that a source buried in the world is reached by getting
// as close as SA_Walk lets it be
static bool SA_Reaches(const vec3_t start, const vec3_t src, int skip, bool buried)
{
    trace_t tr;

    SA_Trace(&tr, start, src, skip);
    if (tr.startsolid)
        return false;
    return tr.fraction == 1.0f || (buried && Distance(tr.endpos, src) <= SA_BURIED_SLOP);
}

/*
=================
SA_Walk

Follows the line from the listener to a source through up to SA_MAX_WALLS
walls, multiplying in what each one lets through. Returns how many it crossed.
A trace that starts inside a brush ignores that brush, so stepping a unit past
each hit carries on to the next one.

A brush entered right where the last one ends is the same wall: two brushes
meeting at a corner, a wall and the trim along it. Counting each brush made a
corner two walls, which took a gunshot round it down by nearly 40 dB.

A sound whose origin is buried in the world - a speaker set into a wall, an
impact right on the surface - is not occluded by the surface it is buried
behind: a hit within SA_BURIED_SLOP of such an origin ends the walk.
=================
*/
static int SA_Walk(const vec3_t from, const vec3_t to, int skip, bool buried, float trans[3])
{
    trace_t tr;
    vec3_t  start, dir, back;
    float   total, left;
    int     hits, steps, b;
    const sa_material_t *mat;

    VectorSubtract(to, from, dir);
    total = VectorNormalize(dir);
    VectorCopy(from, start);
    trans[0] = trans[1] = trans[2] = 1.0f;

    for (hits = steps = 0; hits < SA_MAX_WALLS && steps < SA_MAX_WALLS * 4; steps++) {
        SA_Trace(&tr, start, to, skip);
        if (tr.fraction == 1.0f && !tr.allsolid)
            break;

        left = Distance(tr.endpos, to);
        if (buried && left <= SA_BURIED_SLOP)
            break;

        // just short of the hit is still inside the last brush: same wall
        VectorMA(tr.endpos, -2.0f, dir, back);
        if (!steps || !(CM_PointContents(back, cl.bsp->nodes) & MASK_SOLID)) {
            mat = SA_Material(tr.surface);
            for (b = 0; b < 3; b++)
                trans[b] *= mat->trans[b];
            hits++;
        }

        if (tr.allsolid || left <= 1.0f)
            break;
        VectorMA(tr.endpos, 1.0f, dir, start);
        if (Distance(from, start) >= total)
            break;
    }

    return hits;
}

static const vec3_t sa_sample_dirs[8] = {
    {  0.577f,  0.577f,  0.577f }, {  0.577f,  0.577f, -0.577f },
    {  0.577f, -0.577f,  0.577f }, {  0.577f, -0.577f, -0.577f },
    { -0.577f,  0.577f,  0.577f }, { -0.577f,  0.577f, -0.577f },
    { -0.577f, -0.577f,  0.577f }, { -0.577f, -0.577f, -0.577f },
};

/*
=================
SA_Occlusion

The fraction of the source the listener can see: the origin and the corners of
a small cube round it, each tested for a clear line to the listener. Corners
the origin itself cannot reach - inside the wall it stands against - are not
part of the source and are left out, so a monster beside a wall is not half
muffled in plain view. Steam Audio's own volumetric occlusion counts them.
=================
*/
static float SA_Occlusion(const vec3_t src, int skip, bool buried, float trans[3])
{
    vec3_t  p;
    int     i, open, total;

    open = !SA_Walk(listener_origin, src, skip, buried, trans);
    total = 1;

    if (!buried) {
        for (i = 0; i < 8; i++) {
            VectorMA(src, SA_SAMPLE_RADIUS, sa_sample_dirs[i], p);
            if (!SA_Clear(src, p, skip))
                continue;
            total++;
            if (SA_Clear(p, listener_origin, skip))
                open++;
        }
    }

    // the line through the origin is clear but a corner is not: whatever
    // hides that corner is unknown, so it gets the generic wall
    if (open && open < total)
        VectorCopy(sa_generic.trans, trans);

    return (float)open / total;
}

/*
==============================================================================

PATHS ROUND CORNERS

==============================================================================
*/

// the listener's rays: the horizontal plane closely, where nearly every
// corner is, and enough above and below for stairs, lifts and shafts
static vec3_t sa_relay_dirs[SA_RELAY_RAYS];

// a blocked source's own
static const vec3_t sa_src_dirs[SA_SRC_RAYS] = {
    {  1.0f,     0.0f,    0.0f }, {  0.7071f,  0.7071f, 0.0f },
    {  0.0f,     1.0f,    0.0f }, { -0.7071f,  0.7071f, 0.0f },
    { -1.0f,     0.0f,    0.0f }, { -0.7071f, -0.7071f, 0.0f },
    {  0.0f,    -1.0f,    0.0f }, {  0.7071f, -0.7071f, 0.0f },
    {  0.0f,     0.0f,    1.0f }, {  0.0f,     0.0f,   -1.0f },
};

// What a bend costs, in dB per 90 degrees, low/mid/high: a little off the
// lows, more off the highs, as diffraction round an edge does. A good deal
// less than a real corner with nothing else to carry the sound round, as the
// walls' reflections, which would carry most of it, are not modelled yet.
static const float sa_bend_db[3] = { 3, 6, 12 };

static void SA_InitRelayDirs(void)
{
    vec3_t  *d = sa_relay_dirs;
    float   a;
    int     i;

    for (i = 0; i < 16; i++, d++) {
        a = DEG2RAD(i * 22.5f);
        VectorSet(*d, cosf(a), sinf(a), 0);
    }
    for (i = 0; i < 4; i++) {
        a = DEG2RAD(45 + i * 90);
        VectorSet(*d, 0.8f * cosf(a), 0.8f * sinf(a), 0.6f);
        d++;
        VectorSet(*d, 0.8f * cosf(a), 0.8f * sinf(a), -0.6f);
        d++;
    }
    VectorSet(*d, 0, 0, 1);
    d++;
    VectorSet(*d, 0, 0, -1);
}

// Points along a ray that its start can see, spread over the ray and kept
// short of the wall it ends at. Returns how many, up to `max`.
static int SA_RayRelays(const vec3_t start, const vec3_t dir, float reach, int skip, int max, vec3_t *out)
{
    trace_t tr;
    vec3_t  end;
    float   len;
    int     i, n;

    VectorMA(start, reach, dir, end);
    SA_Trace(&tr, start, end, skip);
    if (tr.startsolid)
        return 0;

    // a wall right there: nothing to get round this way
    len = tr.fraction * reach - SA_RELAY_BACKOFF;
    if (len < SA_RELAY_BACKOFF)
        return 0;

    n = min((int)ceilf(len / SA_RELAY_STEP), max);
    for (i = 0; i < n; i++)
        VectorMA(start, len * (i + 1) / n, dir, out[i]);

    return n;
}

// the listener's points, once a frame
static void SA_ListenerRelays(void)
{
    int i;

    if (cls.realtime < sa.next_relays &&
        DistanceSquared(listener_origin, sa.relay_origin) < SA_RELAY_MOVE * SA_RELAY_MOVE)
        return;

    sa.num_relays = 0;
    for (i = 0; i < SA_RELAY_RAYS; i++)
        sa.num_relays += SA_RayRelays(listener_origin, sa_relay_dirs[i], SA_RELAY_REACH, -1,
                                      SA_RELAY_PER_RAY, sa.relays + sa.num_relays);

    VectorCopy(listener_origin, sa.relay_origin);
    sa.next_relays = cls.realtime + SA_RELAY_MSEC;
}

// a source's points, kept until it moves
static void SA_SourceRelays(sa_chan_t *c, const vec3_t src, int skip)
{
    int i;

    if (c->relays_built && DistanceSquared(src, c->relay_origin) < SA_RELAY_MOVE * SA_RELAY_MOVE)
        return;

    c->num_relays = 0;
    for (i = 0; i < SA_SRC_RAYS; i++)
        c->num_relays += SA_RayRelays(src, sa_src_dirs[i], SA_SRC_REACH, skip,
                                      SA_SRC_PER_RAY, c->relays + c->num_relays);

    VectorCopy(src, c->relay_origin);
    c->relays_built = true;
}

typedef struct {
    float   len;
    int     p, q;           // the listener's point and the source's, -1 = none
} sa_cand_t;

// keeps the `max` shortest, shortest first
static int SA_AddCandidate(sa_cand_t *list, int n, int max, float len, int p, int q)
{
    int i;

    if (n == max) {
        if (len >= list[n - 1].len)
            return n;
        n--;
    }

    for (i = n; i > 0 && list[i - 1].len > len; i--)
        list[i] = list[i - 1];
    list[i].len = len;
    list[i].p = p;
    list[i].q = q;

    return n + 1;
}

typedef struct {
    int     bends;
    float   angle;          // degrees, the bends together
    float   len;
    vec3_t  dir;            // the way it reaches the listener
} sa_path_t;

// how far a path through b turns, coming from a and going on to c, in degrees
static float SA_Bend(const vec3_t a, const vec3_t b, const vec3_t c)
{
    vec3_t  u, v;

    VectorSubtract(b, a, u);
    VectorSubtract(c, b, v);
    VectorNormalize(u);
    VectorNormalize(v);

    return RAD2DEG(acosf(max(-1.0f, min(DotProduct(u, v), 1.0f))));
}

// pts runs from the listener to the source
static bool SA_MakePath(sa_path_t *path, const vec3_t *pts, int n)
{
    int i;

    path->bends = n - 2;
    path->angle = path->len = 0;
    for (i = 1; i < n; i++)
        path->len += Distance(pts[i - 1], pts[i]);
    for (i = 1; i < n - 1; i++)
        path->angle += SA_Bend(pts[i - 1], pts[i], pts[i + 1]);

    VectorSubtract(pts[1], pts[0], path->dir);
    VectorNormalize(path->dir);
    return true;
}

/*
=================
SA_EdgePath

A way over or past the thing in the way when it is small: a crate, a pillar,
a low wall. The rays SA_FindPath casts pass the edge of such a thing only by
chance, and a monster tucked into the corner of two crates was found by none
of them and went silent.

The points tried stand out from the blocked stretch of the line, up, to
either side and in between, a little further each round. They stand out from
where the sight lines over its two ends cross: |LA| / (|LA| + |BS|) of the
way from where the line goes in (A) to where it comes out (B), L being the
listener and S the source. That is the near end when the listener is pressed
against it, the far end when the source is, and the middle between.

With `sides`, the sides of the thing a way goes round - over it, and to
either side - are counted too, on the ring the first way is found on and the
next one out. More than one is a way round something small, a crate or a
pillar, which sound goes round on every side (see SA_TraceVoice); a corner,
or a doorway in a wall, has one.
=================
*/
static const float sa_edge_dists[] = { 48, 96, 160, 240 };

static bool SA_EdgePath(const vec3_t src, int skip, bool buried, sa_path_t *path, int *sides)
{
    trace_t tr;
    vec3_t  pts[3], d, a, b, k, up, side, dirs[6], p;
    float   la, bs;
    int     i, j, traces, found = -1, seen = 0;

    VectorSubtract(src, listener_origin, d);
    if (VectorNormalize(d) < 1)
        return false;

    SA_Trace(&tr, listener_origin, src, skip);
    if (tr.fraction == 1.0f || tr.startsolid)
        return false;
    VectorCopy(tr.endpos, a);

    // a buried source comes out where it is
    SA_Trace(&tr, src, listener_origin, skip);
    if (tr.startsolid)
        VectorCopy(src, b);
    else
        VectorCopy(tr.endpos, b);

    la = Distance(listener_origin, a);
    bs = Distance(b, src);
    LerpVector(a, b, la / max(la + bs, 1.0f), k);

    // across the line: as near straight up as it allows, and to the sides
    VectorSet(up, 0, 0, 1);
    VectorMA(up, -d[2], d, up);
    if (VectorNormalize(up) < 0.1f) {
        VectorSet(up, 1, 0, 0);
        VectorMA(up, -d[0], d, up);
        VectorNormalize(up);
    }
    CrossProduct(d, up, side);

    VectorCopy(up, dirs[0]);
    VectorCopy(side, dirs[1]);
    VectorNegate(side, dirs[2]);
    VectorAdd(up, side, dirs[3]);
    VectorSubtract(up, side, dirs[4]);
    VectorNegate(up, dirs[5]);
    VectorNormalize(dirs[3]);
    VectorNormalize(dirs[4]);

    for (i = traces = 0; i < (int)q_countof(sa_edge_dists); i++) {
        if (found >= 0 && i > found + 1)
            break;
        for (j = 0; j < 6; j++) {
            // once there is a way, only the sides not seen yet
            if (found >= 0 && (j >= SA_EDGE_SIDES || (seen & (1 << j))))
                continue;
            VectorMA(k, sa_edge_dists[i], dirs[j], p);
            if (CM_PointContents(p, cl.bsp->nodes) & MASK_SOLID)
                continue;
            if (traces + 2 > SA_EDGE_TRACES)
                goto done;
            traces++;
            if (!SA_Clear(listener_origin, p, skip))
                continue;
            traces++;
            if (!SA_Reaches(p, src, skip, buried))
                continue;

            seen |= 1 << j;
            if (found >= 0)
                continue;
            VectorCopy(listener_origin, pts[0]);
            VectorCopy(p, pts[1]);
            VectorCopy(src, pts[2]);
            SA_MakePath(path, pts, 3);
            found = i;
            // in between two sides is not a side of its own
            if (!sides || j >= SA_EDGE_SIDES)
                goto done;
        }
    }

done:
    if (sides)
        for (j = *sides = 0; j < 6; j++)
            *sides += (seen >> j) & 1;
    return found >= 0;
}

/*
=================
SA_FindPath

A way round for a sound the listener cannot see: over or past the obstacle
(SA_EdgePath) first, the shortest way when there is one. Then round a corner:
the listener's rays leave points it can see (SA_ListenerRelays) and a blocked
source casts a few of its own; a path is one of those points that can see the
other end, or a point of each that can see one another. Only the shortest few
of each kind are tried, so it costs a handful of traces however many points
there are.

This is not Steam Audio's pathing, which bakes probes through the whole map
and finds paths of any length - only what a few traces can find: the sound
round the corner the player is at, or the one the source is at, or both.
`sides`: SA_EdgePath's, for a way over or past; 0 for any other.
=================
*/
static bool SA_FindPath(sa_chan_t *c, const vec3_t src, int skip, bool buried, sa_path_t *path, int *sides)
{
    sa_cand_t   via[SA_PATH_CANDIDATES], list[SA_PATH_TESTS];
    vec3_t      pts[4];
    const float *p, *q;
    float       len;
    int         i, j, n, num_via;

    *sides = 0;
    if (cls.realtime >= c->next_edge) {
        if (SA_EdgePath(src, skip, buried, path, sides))
            return true;
        *sides = 0;
        c->next_edge = cls.realtime + SA_PATH_RETRY_MSEC;
    }

    VectorCopy(listener_origin, pts[0]);

    // a bend the listener can see
    for (i = num_via = 0; i < sa.num_relays; i++) {
        p = sa.relays[i];
        num_via = SA_AddCandidate(via, num_via, SA_PATH_CANDIDATES,
                                   Distance(listener_origin, p) + Distance(p, src), i, -1);
    }
    for (i = 0; i < min(num_via, SA_PATH_TESTS); i++) {
        p = sa.relays[via[i].p];
        if (SA_Reaches(p, src, skip, buried)) {
            VectorCopy(p, pts[1]);
            VectorCopy(src, pts[2]);
            return SA_MakePath(path, pts, 3);
        }
    }

    // a source in a wall casts no rays of its own
    if (buried)
        return false;

    SA_SourceRelays(c, src, skip);

    // a bend the source can see
    for (j = n = 0; j < c->num_relays; j++) {
        q = c->relays[j];
        n = SA_AddCandidate(list, n, SA_PATH_TESTS,
                            Distance(src, q) + Distance(q, listener_origin), -1, j);
    }
    for (i = 0; i < n; i++) {
        q = c->relays[list[i].q];
        if (SA_Clear(q, listener_origin, skip)) {
            VectorCopy(q, pts[1]);
            VectorCopy(src, pts[2]);
            return SA_MakePath(path, pts, 3);
        }
    }

    // one of each
    for (i = n = 0; i < num_via; i++) {
        p = sa.relays[via[i].p];
        len = Distance(listener_origin, p);
        for (j = 0; j < c->num_relays; j++) {
            q = c->relays[j];
            n = SA_AddCandidate(list, n, SA_PATH_TESTS,
                                len + Distance(p, q) + Distance(q, src), via[i].p, j);
        }
    }
    for (i = 0; i < n; i++) {
        p = sa.relays[list[i].p];
        q = c->relays[list[i].q];
        if (SA_Clear(p, q, skip)) {
            VectorCopy(p, pts[1]);
            VectorCopy(q, pts[2]);
            VectorCopy(src, pts[3]);
            return SA_MakePath(path, pts, 4);
        }
    }

    return false;
}

// the falloff OpenAL is given and the DMA mixer applies: Quake II's straight
// line, or under s_falloff 1 real sound's 1/r
static float SA_Falloff(float dist, float dist_mult)
{
    return S_Falloff(dist, dist_mult);
}

// what a path passes, low/mid/high, against the straight line: its bends,
// and the falloff over the extra distance
static void SA_PathGain(const sa_path_t *path, float dist, float dist_mult, float gain[3])
{
    float   direct, ratio;
    int     b;

    direct = SA_Falloff(dist, dist_mult);
    ratio = direct > 0 ? min(SA_Falloff(path->len, dist_mult) / direct, 1.0f) : 0;

    for (b = 0; b < 3; b++)
        gain[b] = ratio * powf(10, -sa_bend_db[b] * path->angle / (90 * 20));
}

/*
==============================================================================

REFLECTIONS

Sound off walls. Steam Audio traces rays from the listener round a copy of
the map, and at every bounce looks for each simulated source; what it finds
becomes that source's impulse response - the sound of a click there as heard
here off every wall it can reach, absorbed by each surface on the way, in
first-order Ambisonics, so with the directions it arrives from.

A response is for one place, so a source of sound needs one of its own, and
they are not cheap: 2 to 3 ms of simulation each on base1 on the CPU, and a
convolution on the mixer thread. So sources have slots: the loudest have one
each (SA_AssignSlots), and one more is at the listener - the room as it
sounds from here - for every other sound and the player's own. There are
s_reflections_sources of them, or with s_reflections_all as many as there
can be sources (SA_REFL_MAX_SLOTS), so that every sound has its own. Only
the slots in use cost anything: the GPU shares SA_REFL_GPU_BUDGET rays a run
out among those it simulates, and the mixer skips one rung out
(SA_MixReflections).

The simulation runs on a thread of its own (SA_SimThread), s_reflections_rate
times a second and soon after a slot changes hands. It owns all of Steam
Audio's simulation objects: the scene, built from geometry the game thread
hands over (the map can be freed under it), the brush entities' places in
it, and the sources. A response reaches the mixer through Steam Audio's own
triple buffer, which the convolution reads as it runs; see SA_MixReflections
for the rule that brings.

Steam Audio averages its runs while nothing moves, which smooths the noise of
a thousand rays. So what has moved less than SA_SIM_MOVE is simulated where
it was, and the scene is committed only when a brush entity has moved.

It needs the phonon.dll setup/build_phonon.py builds, which patches how Steam
Audio turns a simulation into a response: Valve's makes every response one
coherent signal, so a room's tail comes out as a single noise in both ears -
metallic, inside the head, whatever the walls are - and a tail leaning one
way as a phantom that a sound's direction follows.

==============================================================================
*/

// Quake to Steam Audio's world space: (x, z, -y), a rotation, so it keeps
// handedness; `scale` 1 / SA_UNITS_PER_METER for a point
static IPLVector3 SA_WorldVector(const vec3_t v, float scale)
{
    IPLVector3 out = { v[0] * scale, v[2] * scale, -v[1] * scale };

    return out;
}

static IPLCoordinateSpace3 SA_WorldSpace(const vec3_t origin, const vec3_t forward,
                                         const vec3_t right, const vec3_t up)
{
    IPLCoordinateSpace3 space;

    space.origin = SA_WorldVector(origin, 1.0f / SA_UNITS_PER_METER);
    space.ahead = SA_WorldVector(forward, 1);
    space.right = SA_WorldVector(right, 1);
    space.up = SA_WorldVector(up, 1);
    return space;
}

// a source: which way it faces does not matter, only that it stays the same
static IPLCoordinateSpace3 SA_WorldPoint(const vec3_t origin)
{
    static const vec3_t forward = { 1, 0, 0 }, right = { 0, -1, 0 }, up = { 0, 0, 1 };

    return SA_WorldSpace(origin, forward, right, up);
}

// A brush entity's place, from its model's space to the world's, as
// CM_TransformedBoxTrace and the renderer take it: x along forward, y left,
// z up. Steam Audio's matrices are row-major, translation in the last column.
static void SA_BmodelTransform(const vec3_t origin, const vec3_t angles, IPLMatrix4x4 *t)
{
    vec3_t      forward, right, up;
    IPLVector3  x, y, z, o;
    int         i;

    AngleVectors(angles, forward, right, up);

    // where Steam Audio's axes go, which are Quake's x, z and -y - and -y,
    // left, turns to the right
    x = SA_WorldVector(forward, 1);
    y = SA_WorldVector(up, 1);
    z = SA_WorldVector(right, 1);
    o = SA_WorldVector(origin, 1.0f / SA_UNITS_PER_METER);

    t->elements[0][0] = x.x; t->elements[0][1] = y.x; t->elements[0][2] = z.x; t->elements[0][3] = o.x;
    t->elements[1][0] = x.y; t->elements[1][1] = y.y; t->elements[1][2] = z.y; t->elements[1][3] = o.y;
    t->elements[2][0] = x.z; t->elements[2][1] = y.z; t->elements[2][2] = z.z; t->elements[2][3] = o.z;
    for (i = 0; i < 3; i++)
        t->elements[3][i] = 0;
    t->elements[3][3] = 1;
}

// the scene's materials: sa_materials in order, then these
enum {
    SA_SCENE_GENERIC = q_countof(sa_materials),
    SA_SCENE_WINDOW,
    SA_SCENE_LIQUID,
    SA_SCENE_MATERIALS
};

static IPLMaterial sa_scene_materials[SA_SCENE_MATERIALS];
static float sa_gpu_materials[SA_SCENE_MATERIALS][4];   // the same, as R_SoundTrace takes them

static void SA_SetSceneMaterial(int i, const sa_material_t *mat)
{
    int b;

    for (b = 0; b < 3; b++) {
        sa_scene_materials[i].absorption[b] = mat->absorb[b];
        sa_scene_materials[i].transmission[b] = mat->trans[b];  // not used: only reflections are simulated
        sa_gpu_materials[i][b] = mat->absorb[b];
    }
    sa_scene_materials[i].scattering = mat->scatter;
    sa_gpu_materials[i][3] = mat->scatter;
}

static void SA_InitSceneMaterials(void)
{
    int i;

    for (i = 0; i < (int)q_countof(sa_materials); i++)
        SA_SetSceneMaterial(i, &sa_materials[i]);
    SA_SetSceneMaterial(SA_SCENE_GENERIC, &sa_generic);
    SA_SetSceneMaterial(SA_SCENE_WINDOW, &sa_window);
    SA_SetSceneMaterial(SA_SCENE_LIQUID, &sa_liquid);
}

// which of them a texture is, or -1 to leave its faces out: sound goes out
// through the sky, and through fences and grates
static int SA_SceneMaterial(const mtexinfo_t *texinfo)
{
    const char  *word;
    int         i, flags = texinfo->c.flags;

    if (flags & (SURF_SKY | SURF_NODRAW | SURF_ALPHATEST))
        return -1;
    if (flags & SURF_WARP)
        return SA_SCENE_LIQUID;

    word = CL_SurfaceMaterial(&texinfo->c);
    if (word) {
        for (i = 0; i < (int)q_countof(sa_materials); i++)
            if (!strcmp(sa_materials[i].word, word))
                return i;
    }

    if (flags & SURF_TRANS_MASK)
        return SA_SCENE_WINDOW;
    return SA_SCENE_GENERIC;
}

static void SA_FreeGeometry(sa_geom_t *geom)
{
    int i;

    if (!geom)
        return;
    for (i = 0; i < geom->num_meshes; i++) {
        free(geom->meshes[i].verts);
        free(geom->meshes[i].tris);
        free(geom->meshes[i].mats);
    }
    free(geom->meshes);
    free(geom->edges);
    free(geom->probes);
    free(geom);
}

static bool SA_BmodelFace(const bsp_t *bsp, const mface_t *face)
{
    const mmodel_t  *model;
    int             i;

    for (i = 1, model = bsp->models + 1; i < bsp->nummodels; i++, model++)
        if (face >= model->firstface && face < model->firstface + model->numfaces)
            return true;
    return false;
}

// a face's SA_SceneMaterial, or -1 to leave it out of model `index`'s mesh
static int SA_FaceMaterial(const bsp_t *bsp, int index, const mface_t *face, const int *texmat)
{
    int mat = texmat[face->texinfo - bsp->texinfo];

    if (mat < 0 || face->numsurfedges < 3 || (!index && SA_BmodelFace(bsp, face)))
        return -1;
    return mat;
}

// BSP model `index`'s faces as triangles, in its own space; `texmat` is
// SA_SceneMaterial of each texinfo. The loader gives the world (model 0) no
// face range: like the renderers, it is every face no brush model owns.
static bool SA_BuildMesh(const bsp_t *bsp, int index, const int *texmat, sa_mesh_t *mesh)
{
    const mmodel_t      *model = &bsp->models[index];
    const mface_t       *first = index ? model->firstface : bsp->faces;
    const mface_t       *face;
    const msurfedge_t   *edge;
    int     count = index ? model->numfaces : bsp->numfaces;
    int     i, j, mat, nv = 0, nt = 0, base;

    mesh->radius = index ? model->radius : 0;   // the world's is not set

    for (i = 0, face = first; i < count; i++, face++) {
        if (SA_FaceMaterial(bsp, index, face, texmat) >= 0) {
            nv += face->numsurfedges;
            nt += face->numsurfedges - 2;
        }
    }
    if (!nt)
        return true;

    mesh->verts = malloc(nv * sizeof(*mesh->verts));
    mesh->tris = malloc(nt * sizeof(*mesh->tris));
    mesh->mats = malloc(nt * sizeof(*mesh->mats));
    if (!mesh->verts || !mesh->tris || !mesh->mats)
        return false;

    // a fan per face. Steam Audio's tracer turns a hit's normal to face the
    // ray, so which way they wind does not matter.
    for (i = 0, face = first; i < count; i++, face++) {
        mat = SA_FaceMaterial(bsp, index, face, texmat);
        if (mat < 0)
            continue;

        base = mesh->num_verts;
        for (j = 0, edge = face->firstsurfedge; j < face->numsurfedges; j++, edge++)
            mesh->verts[mesh->num_verts++] = SA_WorldVector(edge->edge->v[edge->vert]->point,
                                                            1.0f / SA_UNITS_PER_METER);
        for (j = 1; j < face->numsurfedges - 1; j++) {
            mesh->tris[mesh->num_tris].indices[0] = base;
            mesh->tris[mesh->num_tris].indices[1] = base + j;
            mesh->tris[mesh->num_tris].indices[2] = base + j + 1;
            mesh->mats[mesh->num_tris++] = mat;
        }
    }

    return true;
}

/*
=================
SA_BuildEdges

The world's corners that sound bends round, for the GPU's ways round them
(see DIFFRACTION): every edge two of its faces share where they meet
sticking out - the corner of a wall, a door frame, the lip of a ledge. The
BSP cuts a long corner into many edges, all on the line where the same two
planes meet, and those are joined back into one. A brush entity's corners
are left out, as it moves.
=================
*/
#define SA_EDGE_FLAT        0.999f  // two faces this near the same plane make no corner
#define SA_EDGE_MIN_LEN     12.0f   // units: a shorter corner is too small to bend much round
#define SA_EDGE_JOIN        1.0f    // units: pieces of a corner this near each other are one

typedef struct {
    uint64_t        key;            // face 0's plane and side, then the other's
    float           t0, t1;         // along the line the two planes meet on, in units
    const mface_t   *face[2];
    vec3_t          point;          // on that line
} sa_corner_t;

static int SA_CornerCmp(const void *p1, const void *p2)
{
    const sa_corner_t *a = p1, *b = p2;

    if (a->key != b->key)
        return a->key < b->key ? -1 : 1;
    if (a->t0 != b->t0)
        return a->t0 < b->t0 ? -1 : 1;
    return 0;
}

// a face's normal, out of the solid
static void SA_FaceNormal(const mface_t *face, vec3_t n)
{
    if (face->drawflags & DSURF_PLANEBACK)
        VectorNegate(face->plane->normal, n);
    else
        VectorCopy(face->plane->normal, n);
}

static void SA_FaceCentre(const mface_t *face, vec3_t c)
{
    const msurfedge_t   *edge;
    int                 i;

    VectorClear(c);
    for (i = 0, edge = face->firstsurfedge; i < face->numsurfedges; i++, edge++)
        VectorAdd(c, edge->edge->v[edge->vert]->point, c);
    VectorScale(c, 1.0f / face->numsurfedges, c);
}

// the plane a face is on and which side of it, as one number
static uint32_t SA_FaceSide(const bsp_t *bsp, const mface_t *face)
{
    return (uint32_t)(face->plane - bsp->planes) * 2 + (face->drawflags & DSURF_PLANEBACK ? 1 : 0);
}

// the line two faces meet on, the way that runs with face 0 first
static void SA_CornerLine(const mface_t *face0, const mface_t *face1, vec3_t dir)
{
    vec3_t  n0, n1;

    SA_FaceNormal(face0, n0);
    SA_FaceNormal(face1, n1);
    CrossProduct(n0, n1, dir);
    VectorNormalize(dir);
}

static void SA_BuildEdges(const bsp_t *bsp, const int *texmat, sa_geom_t *geom)
{
    const mface_t       *(*owner)[2] = NULL, *face, *swap;
    const msurfedge_t   *se;
    const medge_t       *edge;
    sa_corner_t         *corners = NULL, *c;
    int                 *count = NULL;
    vec3_t              n0, n1, c0, c1, dir, along, start;
    IPLVector3          v;
    float               *out, t0, t1, d;
    int                 i, j, k, n = 0;

    if (bsp->numedges < 1)
        return;
    owner = calloc(bsp->numedges, sizeof(*owner));
    count = calloc(bsp->numedges, sizeof(*count));
    corners = malloc(bsp->numedges * sizeof(*corners));
    if (!owner || !count || !corners)
        goto done;

    // the world's faces on each edge
    for (i = 0, face = bsp->faces; i < bsp->numfaces; i++, face++) {
        if (SA_FaceMaterial(bsp, 0, face, texmat) < 0)
            continue;
        for (j = 0, se = face->firstsurfedge; j < face->numsurfedges; j++, se++) {
            k = se->edge - bsp->edges;
            if (k < 0 || k >= bsp->numedges)
                continue;
            if (count[k] < 2)
                owner[k][count[k]] = face;
            count[k]++;
        }
    }

    for (k = 0, edge = bsp->edges; k < bsp->numedges; k++, edge++) {
        if (count[k] != 2)
            continue;
        SA_FaceNormal(owner[k][0], n0);
        SA_FaceNormal(owner[k][1], n1);
        if (DotProduct(n0, n1) > SA_EDGE_FLAT)
            continue;

        // sticking out: each face behind the other's plane
        SA_FaceCentre(owner[k][0], c0);
        SA_FaceCentre(owner[k][1], c1);
        d = DotProduct(n0, edge->v[0]->point);
        if (DotProduct(n0, c1) - d > -0.1f)
            continue;
        d = DotProduct(n1, edge->v[0]->point);
        if (DotProduct(n1, c0) - d > -0.1f)
            continue;

        // face 0 the one on the lower plane, so that every piece of a
        // corner agrees which it is and which way the line runs
        c = &corners[n++];
        c->face[0] = owner[k][0];
        c->face[1] = owner[k][1];
        if (SA_FaceSide(bsp, c->face[0]) > SA_FaceSide(bsp, c->face[1])) {
            swap = c->face[0];
            c->face[0] = c->face[1];
            c->face[1] = swap;
        }
        c->key = (uint64_t)SA_FaceSide(bsp, c->face[0]) << 32 | SA_FaceSide(bsp, c->face[1]);
        SA_CornerLine(c->face[0], c->face[1], dir);
        t0 = DotProduct(edge->v[0]->point, dir);
        t1 = DotProduct(edge->v[1]->point, dir);
        c->t0 = min(t0, t1);
        c->t1 = max(t0, t1);
        VectorCopy(edge->v[0]->point, c->point);
    }

    if (!n)
        goto done;
    qsort(corners, n, sizeof(*corners), SA_CornerCmp);
    geom->edges = malloc(n * R_SOUND_EDGE_FLOATS * sizeof(float));
    if (!geom->edges)
        goto done;

    for (i = 0; i < n; i = j) {
        c = &corners[i];
        t1 = c->t1;
        for (j = i + 1; j < n && corners[j].key == c->key && corners[j].t0 <= t1 + SA_EDGE_JOIN; j++)
            t1 = max(t1, corners[j].t1);
        if (t1 - c->t0 < SA_EDGE_MIN_LEN)
            continue;

        // from where it starts, face 0 goes off along `along`, and the open
        // space round it is pi plus the angle between the normals
        SA_CornerLine(c->face[0], c->face[1], dir);
        VectorMA(c->point, c->t0 - DotProduct(c->point, dir), dir, start);
        SA_FaceNormal(c->face[0], n0);
        SA_FaceNormal(c->face[1], n1);
        CrossProduct(n0, dir, along);
        SA_FaceCentre(c->face[0], c0);
        VectorSubtract(c0, start, c1);
        if (DotProduct(along, c1) < 0)
            VectorNegate(along, along);

        out = geom->edges + geom->num_edges++ * R_SOUND_EDGE_FLOATS;
        v = SA_WorldVector(start, 1.0f / SA_UNITS_PER_METER);
        out[0] = v.x; out[1] = v.y; out[2] = v.z;
        out[3] = (t1 - c->t0) / SA_UNITS_PER_METER;
        v = SA_WorldVector(dir, 1);
        out[4] = v.x; out[5] = v.y; out[6] = v.z;
        out[7] = 1 + acosf(min(max(DotProduct(n0, n1), -1.0f), 1.0f)) / M_PI;
        v = SA_WorldVector(along, 1);
        out[8] = v.x; out[9] = v.y; out[10] = v.z;
        out[11] = 0;
        v = SA_WorldVector(n0, 1);
        out[12] = v.x; out[13] = v.y; out[14] = v.z;
        out[15] = 0;
    }

done:
    free(owner);
    free(count);
    free(corners);
}

// the game thread's: a copy of the map's faces for the simulation thread, or
// NULL and `why` (which starts NULL)
static sa_geom_t *SA_BuildGeometry(const bsp_t *bsp, const char **why)
{
    sa_geom_t   *geom;
    int         *texmat;
    int         i;
    bool        ok;

    texmat = malloc(max(bsp->numtexinfo, 1) * sizeof(*texmat));
    geom = calloc(1, sizeof(*geom));
    if (geom)
        geom->meshes = calloc(bsp->nummodels, sizeof(*geom->meshes));
    ok = texmat && geom && geom->meshes;

    if (ok) {
        // per texinfo, not per face: the material word lookup is a search
        for (i = 0; i < bsp->numtexinfo; i++)
            texmat[i] = SA_SceneMaterial(&bsp->texinfo[i]);

        geom->num_meshes = bsp->nummodels;
        for (i = 0; ok && i < bsp->nummodels; i++)
            ok = SA_BuildMesh(bsp, i, texmat, &geom->meshes[i]);
        if (ok && !geom->meshes[0].num_tris) {
            *why = "no solid faces in the world";
            ok = false;
        }
        if (ok && sa.diffract)
            SA_BuildEdges(bsp, texmat, geom);
    }
    if (!ok && !*why)
        *why = "out of memory";

    free(texmat);
    if (!ok) {
        SA_FreeGeometry(geom);
        return NULL;
    }
    return geom;
}

// the rest of this section is the simulation thread's

static void SA_SimFreeScene(sa_scene_t *sc)
{
    sa_scene_model_t    *model;
    int                 i;

    for (i = 0, model = sc->models; i < sc->num_models; i++, model++) {
        if (model->instance)
            qiplInstancedMeshRelease(&model->instance);
        if (model->mesh)
            qiplStaticMeshRelease(&model->mesh);
        if (model->scene)
            qiplSceneRelease(&model->scene);
    }
    if (sc->world)
        qiplStaticMeshRelease(&sc->world);
    if (sc->scene)
        qiplSceneRelease(&sc->scene);

    free(sc->models);
    memset(sc, 0, sizeof(*sc));
}

static IPLScene SA_SimNewScene(void)
{
    IPLSceneSettings    settings;
    IPLScene            scene = NULL;

    memset(&settings, 0, sizeof(settings));
    settings.type = IPL_SCENETYPE_DEFAULT;
    if (qiplSceneCreate(sa.context, &settings, &scene) != IPL_STATUS_SUCCESS)
        return NULL;
    return scene;
}

static IPLStaticMesh SA_SimNewMesh(IPLScene scene, const sa_mesh_t *mesh)
{
    IPLStaticMeshSettings   settings;
    IPLStaticMesh           out = NULL;

    memset(&settings, 0, sizeof(settings));
    settings.numVertices = mesh->num_verts;
    settings.numTriangles = mesh->num_tris;
    settings.numMaterials = SA_SCENE_MATERIALS;
    settings.vertices = mesh->verts;
    settings.triangles = mesh->tris;
    settings.materialIndices = mesh->mats;
    settings.materials = sa_scene_materials;
    if (qiplStaticMeshCreate(scene, &settings, &out) != IPL_STATUS_SUCCESS)
        return NULL;

    qiplStaticMeshAdd(out, scene);
    return out;
}

// A map's scene: the world, and each brush model as a scene of its own that
// its entity places (SA_SimPlaceBmodels). NULL, or what went wrong.
static const char *SA_SimBuildScene(const sa_geom_t *geom, sa_scene_t *sc)
{
    IPLInstancedMeshSettings    settings;
    sa_scene_model_t            *model;
    int     i, n = geom->num_meshes;

    sc->scene = SA_SimNewScene();
    if (!sc->scene)
        return "couldn't create a scene";
    sc->world = SA_SimNewMesh(sc->scene, &geom->meshes[0]);
    if (!sc->world)
        return "couldn't build the world's mesh";
    sc->num_tris = geom->meshes[0].num_tris;

    sc->models = calloc(n, sizeof(*sc->models));
    if (!sc->models)
        return "out of memory";
    sc->num_models = n;

    for (i = 1, model = sc->models + 1; i < n; i++, model++) {
        if (!geom->meshes[i].num_tris)
            continue;

        model->scene = SA_SimNewScene();
        if (!model->scene)
            return "couldn't create a scene";
        model->mesh = SA_SimNewMesh(model->scene, &geom->meshes[i]);
        if (!model->mesh)
            return "couldn't build a brush model's mesh";
        qiplSceneCommit(model->scene);

        memset(&settings, 0, sizeof(settings));
        settings.subScene = model->scene;
        SA_BmodelTransform(vec3_origin, vec3_origin, &settings.transform);
        if (qiplInstancedMeshCreate(sc->scene, &settings, &model->instance) != IPL_STATUS_SUCCESS) {
            model->instance = NULL;
            return "couldn't instance a brush model";
        }
        model->radius = geom->meshes[i].radius;
        sc->num_tris += geom->meshes[i].num_tris;
    }

    qiplSceneCommit(sc->scene);
    return NULL;
}

static void SA_SimFreePaths(void);
static void SA_SimSetPaths(const sa_geom_t *geom, unsigned map);

static void SA_SimFreeGpu(void)
{
    SA_FreeGeometry(sa.gpu_geom);
    free(sa.gpu_meshes);
    free(sa.gpu_inst);
    sa.gpu_geom = NULL;
    sa.gpu_meshes = NULL;
    sa.gpu_inst = NULL;
    memset(&sa.gpu_scene, 0, sizeof(sa.gpu_scene));
}

// Takes `geom`, which the GPU's copy of the scene is built from on the first
// run it traces - and again after a vid_restart, so it is kept.
static void SA_SimKeepGpu(sa_geom_t *geom, unsigned map)
{
    int i;

    sa.gpu_meshes = calloc(geom->num_meshes, sizeof(*sa.gpu_meshes));
    sa.gpu_inst = calloc(geom->num_meshes, sizeof(*sa.gpu_inst));
    if (!sa.gpu_meshes || !sa.gpu_inst) {
        SA_FreeGeometry(geom);
        SA_SimFreeGpu();
        return;
    }

    for (i = 0; i < geom->num_meshes; i++) {
        sa.gpu_meshes[i].num_verts = geom->meshes[i].num_verts;
        sa.gpu_meshes[i].num_tris = geom->meshes[i].num_tris;
        sa.gpu_meshes[i].verts = &geom->meshes[i].verts->x;
        sa.gpu_meshes[i].tris = geom->meshes[i].tris ? geom->meshes[i].tris->indices : NULL;
        sa.gpu_meshes[i].mats = geom->meshes[i].mats;
    }
    sa.gpu_geom = geom;
    sa.gpu_scene.key = map;
    sa.gpu_scene.num_meshes = geom->num_meshes;
    sa.gpu_scene.meshes = sa.gpu_meshes;
    sa.gpu_scene.num_edges = geom->num_edges;
    sa.gpu_scene.edges = geom->edges;
}

// takes `geom`; NULL for no map
static void SA_SimSetMap(sa_geom_t *geom, unsigned map)
{
    sa_scene_t  old = sa.sim_scene;
    const char  *error = "no map, or no faces in it";

    memset(&sa.sim_scene, 0, sizeof(sa.sim_scene));
    sa.sim_map = 0;
    SA_SimFreePaths();

    if (geom) {
        error = SA_SimBuildScene(geom, &sa.sim_scene);
        if (error) {
            SA_SimFreeScene(&sa.sim_scene);
        } else {
            qiplSimulatorSetScene(sa.simulator, sa.sim_scene.scene);
            qiplSimulatorCommit(sa.simulator);
            sa.sim_map = map;
        }
    }

    // the bake sees the world alone: no brush entity is placed yet
    if (sa.paths)
        SA_SimSetPaths(sa.sim_map ? geom : NULL, map);
    SA_SimFreeGpu();
    sa.gpu_failed_map = sa.diff_failed_map = 0;
    if (sa.sim_map && ((sa.refl && qiplq2SetReflectionTracer) || sa.diffract))
        SA_SimKeepGpu(geom, map);
    else
        SA_FreeGeometry(geom);

    SDL_LockMutex(sa.param_lock);
    sa.diff_edges = sa.gpu_geom ? sa.gpu_geom->num_edges : 0;
    sa.diff_runs = 0;
    sa.diff_why = NULL;
    SDL_UnlockMutex(sa.param_lock);

    // The simulator holds a reference of its own to the scene it was given.
    // With no new one it keeps the old, but is not run.
    SA_SimFreeScene(&old);
    memset(sa.sim_enabled, 0, sizeof(sa.sim_enabled));

    SDL_LockMutex(sa.param_lock);
    sa.refl_tris = sa.sim_scene.num_tris;
    sa.refl_error = error;
    SDL_UnlockMutex(sa.param_lock);
}

// how far an angle has turned, in degrees, the short way round
static float SA_AngleTurned(float a, float b)
{
    float d = fmodf(fabsf(a - b), 360);

    return min(d, 360 - d);
}

// How far the farthest point of a brush model has gone since it was placed:
// what its origin moved and, roughly, what its turning swept.
static float SA_BmodelMoved(const sa_scene_model_t *model, const vec3_t origin, const vec3_t angles)
{
    float turned = 0;
    int   i;

    for (i = 0; i < 3; i++)
        turned = max(turned, SA_AngleTurned(angles[i], model->angles[i]));
    return Distance(origin, model->origin) + model->radius * DEG2RAD(turned);
}

// Brush entities where the game thread last saw them. One that drops out of
// the frames - out of sight, as the server sends them - stays where it was
// seen last, as a closed door far off is still closed; one never seen is not
// in the scene.
//
// Each move is a commit, and a commit restarts Steam Audio's averaging for
// every source. So a brush is moved only once it has gone SA_SIM_MOVE, and
// one that never stops - a fan, a train going round - only every
// SA_SIM_MOVER_EVERY: a door's swing is heard at once, while a spinning fan
// does not keep the reflections at the noise of a single run.
static void SA_SimPlaceBmodels(const sa_sim_input_t *in)
{
    sa_scene_t          *sc = &sa.sim_scene;
    sa_scene_model_t    *model;
    IPLMatrix4x4        t;
    unsigned            now = Sys_Milliseconds();
    bool                changed = false;
    int                 i, m;

    for (i = 0; i < in->num_bmodels; i++) {
        m = in->bmodels[i].model;
        if (m <= 0 || m >= sc->num_models || !sc->models[m].instance)
            continue;
        model = &sc->models[m];

        if (model->placed) {
            if (SA_BmodelMoved(model, in->bmodels[i].origin, in->bmodels[i].angles) < SA_SIM_MOVE) {
                model->moving_since = 0;
                continue;
            }
            if (!model->moving_since)
                model->moving_since = now | 1;
            if (now - model->moving_since > SA_SIM_MOVER_MSEC && now - model->moved_at < SA_SIM_MOVER_EVERY)
                continue;
        }

        SA_BmodelTransform(in->bmodels[i].origin, in->bmodels[i].angles, &t);
        qiplInstancedMeshUpdateTransform(model->instance, sc->scene, t);
        if (!model->placed) {
            qiplInstancedMeshAdd(model->instance, sc->scene);
            model->placed = true;
        }
        VectorCopy(in->bmodels[i].origin, model->origin);
        VectorCopy(in->bmodels[i].angles, model->angles);
        model->moved_at = now;
        changed = true;
    }

    if (changed)
        qiplSceneCommit(sc->scene);
}

/*
REFLECTIONS ON THE GPU (s_reflections_gpu). Steam Audio's run of its own
reflection simulator calls SA_TraceReflections first (the hook
setup/build_phonon.py adds), and when that returns 1 the run's energy fields
are the GPU's: refresh/vkpt/sound_trace.c traces the same rays, bounces and
shadow rays on the RT cores, round its own copy of this scene with the brush
entities where they are placed here. When it returns 0 the CPU traces as
before. The averaging, the response and the convolution are Steam Audio's
either way, so the two sound alike; the GPU's runs take s_reflections_gpu_rays
rays, as many as the simulator was made for, where the CPU's
s_reflections_rays.

It needs the RTX renderer on the ray query API, and a GPU queue of its own; a
map whose scene the GPU could not build is traced on the CPU.

sa_reflcheck runs one of each from the same place and prints the two:
energy per band, when it arrives and which way it leans, for every source.
*/

// the meshes where the scene has them: the world, and the placed brush entities
static int SA_SimGpuInstances(void)
{
    const sa_scene_t        *sc = &sa.sim_scene;
    const sa_scene_model_t  *model;
    r_sound_instance_t      *inst = sa.gpu_inst;
    IPLMatrix4x4            m;
    int     i, n;

    memset(inst, 0, sizeof(*inst));
    inst->transform[0][0] = inst->transform[1][1] = inst->transform[2][2] = 1;
    n = 1;

    for (i = 1, model = sc->models + 1; i < sc->num_models && n < sa.gpu_scene.num_meshes; i++, model++) {
        if (!model->instance || !model->placed)
            continue;
        SA_BmodelTransform(model->origin, model->angles, &m);
        inst[n].mesh = i;
        memcpy(inst[n].transform, m.elements, sizeof(inst[n].transform));
        n++;
    }
    return n;
}

// What a field holds, for sa_reflcheck: the omnidirectional channel's energy
// per band, when it arrives on average (ms after the direct sound), and the
// mid band's directional channels over its omnidirectional one.
typedef struct {
    float   energy[3];
    float   delay;
    float   lean[3];
} sa_field_stats_t;

static void SA_FieldStats(const float *field, int bins, sa_field_stats_t *st)
{
    double  sum = 0, when = 0, dir[3] = { 0 };
    int     b, c, k;

    memset(st, 0, sizeof(*st));
    for (b = 0; b < 3; b++) {
        for (k = 0; k < bins; k++) {
            float v = field[b * bins + k];
            st->energy[b] += v;
            sum += v;
            when += v * (k + 0.5);
        }
    }
    for (c = 1; c < SA_REFL_CHANNELS; c++)
        for (k = 0; k < bins; k++)
            dir[c - 1] += field[(c * 3 + 1) * bins + k];

    st->delay = sum > 0 ? when / sum * 10 : 0;
    for (c = 0; c < 3; c++)
        st->lean[c] = st->energy[1] > 0 ? dir[c] / st->energy[1] : 0;
}

static float SA_Ratio(float a, float b)
{
    return b > 0 ? a / b : 0;
}

// sa_reflcheck's report, once the CPU has run as well
static void SA_FinishCheck(int num_sources, int channels, int bins, float *const *fields)
{
    sa_field_stats_t    cpu, gpu;
    size_t      size = 256 + num_sources * 320, len;
    char        *report = malloc(size);
    int         s, field = channels * 3 * bins;

    if (report) {
        len = Q_snprintf(report, size, "Reflections check, %d rays from the same place: the GPU took %.1f ms, the CPU %.1f ms\n",
                         SA_REFL_MAX_RAYS, sa.check_gpu_msec, (Sys_Microseconds() - sa.check_cpu_start) / 1000.0f);
        if (!sa.check_ok) {
            Q_snprintf(report + len, size - len, "  the GPU could not trace it: %s\n", sa.check_why);
        } else {
            for (s = 0; s < num_sources && len < size; s++) {
                SA_FieldStats(fields[s], bins, &cpu);
                SA_FieldStats(sa.check_fields + s * field, bins, &gpu);
                len += Q_snprintf(report + len, size - len,
                                  "  %2d: energy GPU/CPU %.2f %.2f %.2f (CPU %.2e %.2e %.2e), arrives %.0f/%.0f ms, "
                                  "leans %+.2f %+.2f %+.2f / %+.2f %+.2f %+.2f\n", s,
                                  SA_Ratio(gpu.energy[0], cpu.energy[0]), SA_Ratio(gpu.energy[1], cpu.energy[1]),
                                  SA_Ratio(gpu.energy[2], cpu.energy[2]), cpu.energy[0], cpu.energy[1], cpu.energy[2],
                                  gpu.delay, cpu.delay, gpu.lean[0], gpu.lean[1], gpu.lean[2],
                                  cpu.lean[0], cpu.lean[1], cpu.lean[2]);
            }
        }
    }

    free(sa.check_fields);
    sa.check_fields = NULL;

    SDL_LockMutex(sa.param_lock);
    free(sa.check_report);
    sa.check_report = report;
    SDL_UnlockMutex(sa.param_lock);
}

// Steam Audio's call, on the simulation thread, inside qiplSimulatorRunReflections
static int SA_TraceReflections(int phase, int num_sources, const float *sources, const float *listener,
                               int rays, int bounces, float duration, int order, float min_dist,
                               int channels, int bins, float *const *fields, void *user)
{
    bool            (*trace)(const r_sound_trace_t *, const char **) = R_SoundTrace;
    float           *check[SA_REFL_MAX_SLOTS + 1];
    r_sound_trace_t t;
    const char      *why = "not on this renderer";
    uint64_t        start = Sys_Microseconds();
    bool            ok;
    int             s;

    if (phase == 1) {
        if (sa.sim_run.check)
            SA_FinishCheck(num_sources, channels, bins, fields);
        return 0;
    }
    if (!sa.gpu_run && !sa.sim_run.check)
        return 0;
    if (!sa.gpu_geom || num_sources > SA_REFL_MAX_SLOTS + 1) {
        sa.gpu_why = sa.check_why = "no scene for it";
        sa.check_ok = false;
        return 0;
    }

    // a check traces into its own fields, and leaves the run to the CPU
    if (sa.sim_run.check) {
        free(sa.check_fields);
        sa.check_fields = malloc(num_sources * channels * 3 * bins * sizeof(float));
        if (!sa.check_fields) {
            sa.check_ok = false;
            sa.check_why = "out of memory";
            return 0;
        }
        for (s = 0; s < num_sources; s++)
            check[s] = sa.check_fields + s * channels * 3 * bins;
    }

    memset(&t, 0, sizeof(t));
    t.geom = &sa.gpu_scene;
    t.num_instances = SA_SimGpuInstances();
    t.instances = sa.gpu_inst;
    t.num_materials = SA_SCENE_MATERIALS;
    t.materials = sa_gpu_materials;
    VectorCopy(listener, t.listener);
    t.num_sources = num_sources;
    t.sources = sources;
    t.num_rays = rays;
    t.num_bounces = bounces;
    t.duration = duration;
    t.min_distance = min_dist;
    t.seed = ++sa.gpu_seed;
    t.num_channels = channels;
    t.num_bins = bins;
    t.fields = sa.sim_run.check ? check : fields;

    ok = trace && trace(&t, &why);

    if (sa.sim_run.check) {
        sa.check_ok = ok;
        sa.check_why = why;
        sa.check_gpu_msec = (Sys_Microseconds() - start) / 1000.0f;
        sa.check_cpu_start = Sys_Microseconds();
        return 0;
    }

    sa.gpu_traced = ok;
    if (!ok) {
        sa.gpu_why = why;
        sa.gpu_failed_map = sa.sim_map;
        sa.gpu_failed_why = why;
    }
    return ok;
}

static void SA_SimRun(const sa_sim_input_t *in)
{
    IPLSimulationSharedInputs   shared;
    IPLSimulationInputs         inputs;
    uint64_t    start = Sys_Microseconds();
    int         s, n, room = sa.num_slots;

    SA_SimPlaceBmodels(in);

    // the room's source is at the listener, so this places both
    for (s = 0; s <= room; s++) {
        if (in->sources[s].enabled && (!sa.sim_enabled[s] ||
            DistanceSquared(in->sources[s].origin, sa.sim_used[s]) > SA_SIM_MOVE * SA_SIM_MOVE))
            VectorCopy(in->sources[s].origin, sa.sim_used[s]);
        sa.sim_enabled[s] = in->sources[s].enabled;
    }

    // the GPU traces this run when asked to and able to
    sa.gpu_run = sa.gpu_traced = false;
    sa.gpu_why = NULL;
    if (in->gpu && !in->check) {
        if (!qiplq2SetReflectionTracer)
            sa.gpu_why = "this phonon.dll has no hook for it";
        else if (sa.gpu_failed_map == sa.sim_map)
            sa.gpu_why = sa.gpu_failed_why;
        else if (!sa.gpu_geom)
            sa.gpu_why = "no scene for it";
        else if (!R_SoundTrace)
            sa.gpu_why = "not on this renderer";
        else
            sa.gpu_run = R_SoundTrace(NULL, &sa.gpu_why);
    }

    // the GPU's rays shared out among the sources this run simulates, so
    // that more of them cost it no more
    for (s = n = 0; s <= room; s++)
        n += sa.sim_enabled[s];

    memset(&shared, 0, sizeof(shared));
    shared.listener = SA_WorldSpace(sa.sim_used[room], in->forward, in->right, in->up);
    if (in->check)
        shared.numRays = SA_REFL_MAX_RAYS;
    else if (sa.gpu_run)
        shared.numRays = min(in->gpu_rays, max(SA_REFL_GPU_BUDGET / max(n, 1), SA_REFL_MIN_RAYS));
    else
        shared.numRays = in->rays;
    shared.numBounces = in->bounces;
    shared.duration = sa.refl_duration;
    shared.order = SA_REFL_ORDER;
    shared.irradianceMinDistance = SA_REFL_MIN_DIST;
    qiplSimulatorSetSharedInputs(sa.simulator, IPL_SIMULATIONFLAGS_REFLECTIONS, &shared);

    for (s = 0; s <= room; s++) {
        // one without REFLECTIONS in its flags is left out of the run
        memset(&inputs, 0, sizeof(inputs));
        if (sa.sim_enabled[s]) {
            inputs.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
            inputs.source = SA_WorldPoint(sa.sim_used[s]);
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
            inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
            inputs.reverbScale[0] = inputs.reverbScale[1] = inputs.reverbScale[2] = 1;
        }
        qiplSourceSetInputs(sa.sources[s], IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);
    }

    qiplSimulatorRunReflections(sa.simulator);

    SDL_LockMutex(sa.param_lock);
    if (sa.refl_map != in->map) {
        memset(sa.refl_ran, 0, sizeof(sa.refl_ran));
        sa.refl_map = in->map;
    }
    for (s = 0; s <= room; s++) {
        if (sa.sim_enabled[s]) {
            sa.refl_ran[s] = true;
            VectorCopy(sa.sim_used[s], sa.refl_origin[s]);
        }
    }
    sa.refl_msec = (Sys_Microseconds() - start) / 1000.0f;
    sa.refl_sources = n;
    sa.refl_rays = shared.numRays;
    sa.refl_runs++;
    if (!in->check) {
        sa.refl_gpu = sa.gpu_traced;
        sa.refl_gpu_why = in->gpu ? sa.gpu_why : NULL;
    }
    SDL_UnlockMutex(sa.param_lock);
}

/*
==============================================================================

PATHING

Sound that goes round. Probes stand over every floor of the map
(SA_BuildProbes, on the game thread as a map comes in) and, on the
simulation thread, Steam Audio bakes which of them see which and the
shortest way between every two: at a map's first start, kept after that in
pathing/<map>.bin under the game directory - about 5 MB and half a second
of four threads for base1. The bake sees the world alone.

Every s_pathing_rate a run looks up, for each voice, the way from the probe
nearest the source to those round the listener, checks it against the brush
entities where they are now - so a door that shuts takes its way out, and
s_pathing_alternates looks for another, at a cost - and gives what arrives:
how loud after the falloff of the way's length, its EQ from the bends
(sa_bend_db, as SA_FindPath has them), and in first-order Ambisonics from
which way.

That is heard as a second voice (SA_MixFrame): the sound at the path's level
and EQ, through the HRTF from the way it arrives - the average of the ways,
spread as far as they disagree - as much of it as the walls hide. What the
listener sees of a source is the direct sound's. Steam Audio's own path
effect renders through its Ambisonics decode, which comes out 7-9 dB under
the HRTF and smeared across the head.

Where Steam Audio has no probe in reach of the source or the listener - a
ledge between the columns, a lift, a flyer high up - or before the bake is
in, SA_FindPath's corner paths stand in as before. That needs the phonon.dll
setup/build_phonon.py builds, which says when there was no probe instead of
leaving the last run's paths in place.

==============================================================================
*/

static const IPLBakedDataIdentifier sa_path_id = { IPL_BAKEDDATATYPE_PATHING, IPL_BAKEDDATAVARIATION_DYNAMIC };

// what a probe cannot stand in; water is open, sound goes through it
static bool SA_ProbeSolid(const bsp_t *bsp, const vec3_t p)
{
    return CM_PointContents(p, bsp->nodes) & (CONTENTS_SOLID | CONTENTS_WINDOW);
}

static void SA_AddProbe(sa_geom_t *geom, const vec3_t p, float radius)
{
    IPLSphere *s = &geom->probes[geom->num_probes++];

    s->center = SA_WorldVector(p, 1.0f / SA_UNITS_PER_METER);
    s->radius = radius / SA_UNITS_PER_METER;
}

/*
=================
SA_ProbeColumns

Probes over the floors of columns `spacing` apart across the world: walking
up each column, every change from solid to open is a floor, and a probe
stands SA_PATH_HEIGHT over it, or half way up a lower space. A gap under a
foot high is no space. False if there would be more than `max`.

Every probe is moved SA_PATH_OFF_GRID off the whole units the columns fall
on, as brush faces do too: Steam Audio's rays from a probe lying on a face
hit it, those towards the probe do not, and the bake tests each pair of
probes one way round and its runtime check the other - so a baked way
through such a probe failed every check, and its share was lost to every
sound (on base1 nearly 60% of what reached the player's start).
=================
*/
static bool SA_ProbeColumns(const bsp_t *bsp, float spacing, sa_geom_t *geom, int max)
{
    const mmodel_t  *world = &bsp->models[0];
    vec3_t  p, at;
    float   x0, y0, ground = 0, top;
    int     nx, ny, i, j;
    bool    was, now, have_ground;

    nx = (int)((world->maxs[0] - world->mins[0]) / spacing) + 1;
    ny = (int)((world->maxs[1] - world->mins[1]) / spacing) + 1;
    x0 = (world->mins[0] + world->maxs[0] - (nx - 1) * spacing) / 2 + SA_PATH_OFF_GRID;
    y0 = (world->mins[1] + world->maxs[1] - (ny - 1) * spacing) / 2 + SA_PATH_OFF_GRID;

    geom->num_probes = 0;
    for (i = 0; i < nx; i++) {
        for (j = 0; j < ny; j++) {
            p[0] = x0 + i * spacing;
            p[1] = y0 + j * spacing;
            p[2] = world->mins[2] + 1;
            was = SA_ProbeSolid(bsp, p);
            have_ground = false;

            for (; p[2] < world->maxs[2]; was = now) {
                p[2] += SA_PATH_STEP;
                now = SA_ProbeSolid(bsp, p);
                if (was && !now) {
                    ground = p[2] - SA_PATH_STEP / 2;
                    have_ground = true;
                } else if (!was && now && have_ground) {
                    top = p[2] - SA_PATH_STEP / 2;
                    have_ground = false;
                    if (top - ground < 32)
                        continue;
                    if (geom->num_probes >= max)
                        return false;
                    VectorCopy(p, at);
                    at[2] = top - ground > SA_PATH_HEIGHT + 16 ? ground + SA_PATH_HEIGHT : (ground + top) / 2;
                    at[2] += SA_PATH_OFF_GRID;
                    SA_AddProbe(geom, at, spacing * SA_PATH_RADIUS);
                }
            }
        }
    }
    return true;
}

static const vec3_t sa_off_grid = { SA_PATH_OFF_GRID, SA_PATH_OFF_GRID, SA_PATH_OFF_GRID };

static const char *const sa_probe_spots[] = {
    "info_player_", "monster_", "item_", "weapon_", "ammo_", "key_", "misc_teleporter_dest"
};

static bool SA_ProbeSpot(const char *classname)
{
    int i;

    for (i = 0; i < (int)q_countof(sa_probe_spots); i++)
        if (!strncmp(classname, sa_probe_spots[i], strlen(sa_probe_spots[i])))
            return true;
    return false;
}

/*
=================
SA_ProbeSpots

Probes where things stand - player starts, monsters, items - so that a
ledge or a platform narrower than the columns are apart has one anyway: a
little over the origin, about where a column would put it, unless a column's
probe is near already.
=================
*/
static void SA_ProbeSpots(const bsp_t *bsp, float spacing, sa_geom_t *geom)
{
    const char  *data = bsp->entitystring, *tok;
    char        classname[MAX_QPATH], key[MAX_QPATH];
    vec3_t      origin, p;
    IPLVector3  c;
    float       dx, dy, dz, apart = spacing / 2 / SA_UNITS_PER_METER;
    int         i, end = geom->num_probes + SA_PATH_MAX_SPOTS;
    bool        has_origin;

    while (data && geom->num_probes < end) {
        tok = COM_Parse(&data);
        if (tok[0] != '{')
            break;

        classname[0] = 0;
        has_origin = false;
        while (1) {
            tok = COM_Parse(&data);
            if (!data || tok[0] == '}')
                break;
            Q_strlcpy(key, tok, sizeof(key));
            tok = COM_Parse(&data);
            if (!data)
                break;
            if (!strcmp(key, "classname"))
                Q_strlcpy(classname, tok, sizeof(classname));
            else if (!strcmp(key, "origin"))
                has_origin = sscanf(tok, "%f %f %f", &origin[0], &origin[1], &origin[2]) == 3;
        }
        if (!has_origin || !SA_ProbeSpot(classname))
            continue;

        VectorAdd(origin, sa_off_grid, p);
        p[2] += 24;
        if (SA_ProbeSolid(bsp, p)) {
            VectorAdd(origin, sa_off_grid, p);
            if (SA_ProbeSolid(bsp, p))
                continue;
        }

        c = SA_WorldVector(p, 1.0f / SA_UNITS_PER_METER);
        for (i = 0; i < geom->num_probes; i++) {
            dx = geom->probes[i].center.x - c.x;
            dy = geom->probes[i].center.y - c.y;
            dz = geom->probes[i].center.z - c.z;
            if (dx * dx + dy * dy + dz * dz < apart * apart)
                break;
        }
        if (i == geom->num_probes)
            SA_AddProbe(geom, p, spacing * SA_PATH_RADIUS);
    }
}

// FNV-1a. A bake depends on its probes and on the SA_PATH_ constants:
// change those, and SA_PATH_FILE_VERSION with them.
static unsigned SA_Hash(const void *data, size_t size)
{
    const byte  *b = data;
    unsigned    h = 2166136261u;
    size_t      i;

    for (i = 0; i < size; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

/*
=================
SA_BuildProbes

The game thread's: the map's probes, s_pathing_spacing apart or as much
wider as keeps them under SA_PATH_MAX_PROBES, and where their bake is kept.
None if they could not be had.
=================
*/
static void SA_BuildProbes(const bsp_t *bsp, sa_geom_t *geom)
{
    char    name[MAX_QPATH];
    float   spacing = Cvar_ClampValue(s_pathing_spacing, 1, 16) * SA_UNITS_PER_METER;

    geom->probes = malloc((SA_PATH_MAX_PROBES + SA_PATH_MAX_SPOTS) * sizeof(*geom->probes));
    if (!geom->probes)
        return;
    while (!SA_ProbeColumns(bsp, spacing, geom, SA_PATH_MAX_PROBES))
        spacing *= 1.25f;
    SA_ProbeSpots(bsp, spacing, geom);

    geom->spacing = spacing;
    geom->checksum = bsp->checksum;
    geom->probe_hash = SA_Hash(geom->probes, geom->num_probes * sizeof(*geom->probes));

    COM_StripExtension(name, COM_SkipPath(bsp->name), sizeof(name));
    if (Q_concat(geom->path_file, sizeof(geom->path_file), fs_gamedir, "/pathing/", name, ".bin") >=
        sizeof(geom->path_file) || FS_CreatePath(geom->path_file))
        geom->path_file[0] = 0;
}

// the rest of this section is the simulation thread's

// what the bake file starts with
typedef struct {
    char        magic[8];           // "Q2SAPATH"
    uint32_t    version;            // SA_PATH_FILE_VERSION
    uint32_t    phonon;             // STEAMAUDIO_VERSION
    uint32_t    checksum;           // the map's
    uint32_t    probe_hash;         // of the probes baked
    uint32_t    size;               // of the probe batch that follows
    uint32_t    data_hash;          // of that: Steam Audio takes it on trust
} sa_path_file_t;

// a bake kept from before, if it is of these probes on this map
static IPLProbeBatch SA_SimLoadPaths(const sa_geom_t *geom)
{
    IPLSerializedObjectSettings settings;
    IPLSerializedObject         serial = NULL;
    IPLProbeBatch               batch = NULL;
    IPLBakedDataIdentifier      id = sa_path_id;
    sa_path_file_t  head;
    byte            *data = NULL;
    FILE            *f;

    if (!geom->path_file[0] || !(f = Q_fopen(geom->path_file, "rb")))
        return NULL;
    if (fread(&head, sizeof(head), 1, f) == 1 && !memcmp(head.magic, "Q2SAPATH", 8) &&
        head.version == SA_PATH_FILE_VERSION && head.phonon == STEAMAUDIO_VERSION &&
        head.checksum == geom->checksum && head.probe_hash == geom->probe_hash &&
        head.size > 0 && head.size < (1u << 30))
        data = malloc(head.size);
    if (data && (fread(data, 1, head.size, f) != head.size || SA_Hash(data, head.size) != head.data_hash)) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (!data)
        return NULL;

    settings.data = data;
    settings.size = head.size;
    if (qiplSerializedObjectCreate(sa.context, &settings, &serial) == IPL_STATUS_SUCCESS) {
        if (qiplProbeBatchLoad(sa.context, serial, &batch) != IPL_STATUS_SUCCESS)
            batch = NULL;
        qiplSerializedObjectRelease(&serial);
    }
    free(data);

    if (batch && (qiplProbeBatchGetNumProbes(batch) != geom->num_probes || !qiplProbeBatchGetDataSize(batch, &id))) {
        qiplProbeBatchRelease(&batch);
        batch = NULL;
    }
    if (batch)
        qiplProbeBatchCommit(batch);
    return batch;
}

// written aside and renamed over, so a game killed half way leaves the old
// file or none
static void SA_SimSavePaths(const sa_geom_t *geom, IPLProbeBatch batch)
{
    IPLSerializedObjectSettings settings;
    IPLSerializedObject         serial = NULL;
    sa_path_file_t  head;
    const byte      *data;
    char            temp[MAX_OSPATH + 4];
    FILE            *f;
    bool            ok = false;

    memset(&settings, 0, sizeof(settings));
    if (!geom->path_file[0] || qiplSerializedObjectCreate(sa.context, &settings, &serial) != IPL_STATUS_SUCCESS)
        return;
    qiplProbeBatchSave(batch, serial);
    data = qiplSerializedObjectGetData(serial);

    memset(&head, 0, sizeof(head));
    memcpy(head.magic, "Q2SAPATH", 8);
    head.version = SA_PATH_FILE_VERSION;
    head.phonon = STEAMAUDIO_VERSION;
    head.checksum = geom->checksum;
    head.probe_hash = geom->probe_hash;
    head.size = (uint32_t)qiplSerializedObjectGetSize(serial);
    head.data_hash = SA_Hash(data, head.size);

    Q_snprintf(temp, sizeof(temp), "%s.tmp", geom->path_file);
    if (data && head.size && (f = Q_fopen(temp, "wb"))) {
        ok = fwrite(&head, sizeof(head), 1, f) == 1 && fwrite(data, 1, head.size, f) == head.size;
        ok = !fclose(f) && ok;
    }
    if (ok) {
        remove(geom->path_file);
        ok = !rename(temp, geom->path_file);
    }
    if (!ok)
        remove(temp);
    qiplSerializedObjectRelease(&serial);
}

// Steam Audio calls this whether one is given or not
static void IPLCALL SA_BakeProgress(IPLfloat32 progress, void *user)
{
}

static IPLProbeBatch SA_SimBakePaths(const sa_geom_t *geom)
{
    IPLPathBakeParams   params;
    IPLProbeBatch       batch = NULL;
    int                 i;

    if (qiplProbeBatchCreate(sa.context, &batch) != IPL_STATUS_SUCCESS)
        return NULL;
    for (i = 0; i < geom->num_probes; i++)
        qiplProbeBatchAddProbe(batch, geom->probes[i]);
    qiplProbeBatchCommit(batch);

    memset(&params, 0, sizeof(params));
    params.scene = sa.sim_scene.scene;
    params.probeBatch = batch;
    params.identifier = sa_path_id;
    params.numSamples = SA_PATH_VIS_SAMPLES;
    params.radius = SA_PATH_VIS_RADIUS;
    params.threshold = SA_PATH_VIS_SHARE;
    params.visRange = SA_PATH_VIS_RANGE;
    params.pathRange = SA_PATH_RANGE;
    params.numThreads = sa.sim_threads;
    qiplPathBakerBake(sa.context, &params, SA_BakeProgress, NULL);

    if (!qiplProbeBatchGetDataSize(batch, &params.identifier)) {
        qiplProbeBatchRelease(&batch);
        batch = NULL;
    }
    return batch;
}

static void SA_SimFreePaths(void)
{
    IPLSimulationInputs inputs;
    int                 i;

    if (!sa.path_batch)
        return;

    // nothing may point at the batch once it is out of the simulator
    memset(&inputs, 0, sizeof(inputs));
    for (i = 0; i < MAX_CHANNELS; i++)
        if (sa.path_sources[i])
            qiplSourceSetInputs(sa.path_sources[i], IPL_SIMULATIONFLAGS_PATHING, &inputs);
    qiplSimulatorRemoveProbeBatch(sa.simulator, sa.path_batch);
    qiplSimulatorCommit(sa.simulator);
    qiplProbeBatchRelease(&sa.path_batch);
    sa.path_batch = NULL;
}

// a new map's probes, baked or from their file; NULL for none
static void SA_SimSetPaths(const sa_geom_t *geom, unsigned map)
{
    IPLProbeBatch   batch = NULL;
    uint64_t        start = Sys_Microseconds();
    const char      *error = NULL;
    bool            cached = false;

    if (!geom) {
        error = "no map";
    } else if (!geom->num_probes) {
        error = "no probes";
    } else {
        batch = SA_SimLoadPaths(geom);
        cached = batch != NULL;
        if (!batch) {
            batch = SA_SimBakePaths(geom);
            if (batch)
                SA_SimSavePaths(geom, batch);
            else
                error = "the bake failed";
        }
    }

    if (batch) {
        qiplSimulatorAddProbeBatch(sa.simulator, batch);
        qiplSimulatorCommit(sa.simulator);
        sa.path_batch = batch;
    }

    SDL_LockMutex(sa.param_lock);
    memset(sa.path_out, 0, sizeof(sa.path_out));
    sa.path_map = map;
    sa.path_probes = geom ? geom->num_probes : 0;
    sa.path_spacing = geom ? geom->spacing : 0;
    sa.path_cached = cached;
    sa.path_sec = (Sys_Microseconds() - start) * 1e-6f;
    sa.path_error = error;
    SDL_UnlockMutex(sa.param_lock);
}

static float IPLCALL SA_PathFalloff(IPLfloat32 dist, void *user)
{
    return SA_Falloff(dist * SA_UNITS_PER_METER, *(const float *)user);
}

// what a way loses at its bends, as SA_FindPath's paths do
static float IPLCALL SA_PathBend(IPLfloat32 angle, IPLint32 band, void *user)
{
    return powf(10, -sa_bend_db[band] * (float)RAD2DEG(angle) / (90 * 20));
}

/*
DIFFRACTION (s_diffraction). Sound bends round corners: its wavelengths, 2 cm
to 17 m, are the size of doors and walls. The probes' ways round give a level
from the length of the way and a loss per bend that is a guess (sa_bend_db),
and go from probe to probe, not round the corner itself. So each pathing run
also asks the GPU (refresh/vkpt/sound_diffract.comp) what comes round the
world's corners - the edges SA_BuildEdges finds where two faces meet
sticking out - to each voice the listener cannot see: as much as the theory
of diffraction off a hard wedge says, per band, by the angle the sound turns
and how far it goes on each side. A corner it only just goes round passes
half (-6 dB); one it turns hard round, far less, the highs far less than the
lows.

What it finds takes the place of the probes' answer: in sight, or round one
or more edges - the level against the straight line from the shortest way's
length, the EQ from the edges, and where it arrives from, towards the
edges. Round two corners at once, a doorway and then along a corridor, no
single edge gets it there; there the probes' answer stands, or with no
probes (s_pathing 0) SA_FindPath's corners. Brush entities block the ways
round, but their own corners are not among the edges.
*/

static void SA_SimDiffract(const sa_sim_input_t *in)
{
    float       queries[MAX_CHANNELS * 3], out[MAX_CHANNELS * R_SOUND_DIFFRACTION_FLOATS];
    int         which[MAX_CHANNELS];
    const float *r;
    sa_path_out_t *found;
    r_sound_trace_t t;
    IPLVector3  v;
    vec3_t      dir;
    const char  *why = NULL;
    uint64_t    start = Sys_Microseconds();
    float       fall;
    int         i, j, b, n = 0;

    if (sa.diff_failed_map && sa.diff_failed_map == sa.sim_map)
        why = sa.diff_failed_why;
    else if (!sa.gpu_geom)
        why = "no scene for it";
    else if (!R_SoundTrace)
        why = "not on this renderer";
    if (why)
        goto done;

    for (i = 0; i < MAX_CHANNELS; i++) {
        if (!in->paths[i].key)
            continue;
        v = SA_WorldVector(in->paths[i].origin, 1.0f / SA_UNITS_PER_METER);
        queries[n * 3 + 0] = v.x;
        queries[n * 3 + 1] = v.y;
        queries[n * 3 + 2] = v.z;
        which[n++] = i;
    }
    if (!n)
        return;

    memset(&t, 0, sizeof(t));
    t.geom = &sa.gpu_scene;
    t.num_instances = SA_SimGpuInstances();
    t.instances = sa.gpu_inst;
    v = SA_WorldVector(in->listener, 1.0f / SA_UNITS_PER_METER);
    t.listener[0] = v.x;
    t.listener[1] = v.y;
    t.listener[2] = v.z;
    t.num_queries = n;
    t.queries = queries;
    t.diffraction = out;
    if (!R_SoundTrace(&t, &why)) {
        sa.diff_failed_map = sa.sim_map;
        sa.diff_failed_why = why ? why : "it failed";
        why = sa.diff_failed_why;
        goto done;
    }

    for (j = 0; j < n; j++) {
        r = out + j * R_SOUND_DIFFRACTION_FLOATS;
        found = &sa.path_found[which[j]];
        if (r[7] > 0) {
            // in sight: the direct sound covers it
            for (b = 0; b < 3; b++)
                found->eq[b] = 1;
            found->sh[0] = 1;
            found->sh[1] = found->sh[2] = found->sh[3] = 0;
        } else if (r[3] > 0) {
            // Round edges. All three bands near 1 would read as in sight
            // (SA_PathResult); more than one way round could add up to it.
            for (b = 0; b < 3; b++)
                found->eq[b] = min(sqrtf(r[b]), 0.99f);
            // back to Quake's axes from Steam Audio's (x, z, -y)
            VectorSet(dir, r[4], -r[6], r[5]);
            fall = SA_Falloff(r[3] * SA_UNITS_PER_METER, in->paths[which[j]].dist_mult);
            found->sh[0] = SA_SH_C0 * fall;
            found->sh[1] = -dir[0] * SA_SQRT3 * found->sh[0];
            found->sh[2] = dir[2] * SA_SQRT3 * found->sh[0];
            found->sh[3] = dir[1] * SA_SQRT3 * found->sh[0];
        } else {
            continue;   // no single edge: the probes' answer stands
        }
        found->edges = true;
    }

done:
    SDL_LockMutex(sa.param_lock);
    sa.diff_why = why;
    if (!why) {
        sa.diff_msec = (Sys_Microseconds() - start) / 1000.0f;
        sa.diff_runs++;
    }
    SDL_UnlockMutex(sa.param_lock);
}

static void SA_SimRunPaths(const sa_sim_input_t *in)
{
    IPLSimulationSharedInputs   shared;
    IPLSimulationInputs         inputs;
    IPLSimulationOutputs        outputs;
    IPLDeviationModel           bend;
    sa_path_out_t               *found;
    uint64_t    start = Sys_Microseconds();
    unsigned    now;
    int         i, n = 0;

    if (!sa.path_batch && !sa.diffract)
        return;

    SA_SimPlaceBmodels(in);

    // the GPU's ways round edges alone
    if (!sa.path_batch) {
        for (i = 0; i < MAX_CHANNELS; i++)
            n += in->paths[i].key != 0;
        if (!n)
            return;
        now = Sys_Milliseconds();
        for (i = 0, found = sa.path_found; i < MAX_CHANNELS; i++, found++) {
            memset(found, 0, sizeof(*found));
            found->key = in->paths[i].key;
            found->time = now | 1;
        }
        goto edges;
    }

    memset(&shared, 0, sizeof(shared));
    shared.listener = SA_WorldSpace(in->listener, in->forward, in->right, in->up);
    qiplSimulatorSetSharedInputs(sa.simulator, IPL_SIMULATIONFLAGS_PATHING, &shared);

    memset(&bend, 0, sizeof(bend));
    bend.type = IPL_DEVIATIONTYPE_CALLBACK;
    bend.callback = SA_PathBend;

    // one without PATHING in its flags is left out of the run
    for (i = 0; i < MAX_CHANNELS; i++) {
        memset(&inputs, 0, sizeof(inputs));
        if (in->paths[i].key) {
            sa.path_dist_mult[i] = in->paths[i].dist_mult;
            inputs.flags = IPL_SIMULATIONFLAGS_PATHING;
            inputs.source = SA_WorldPoint(in->paths[i].origin);
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
            inputs.distanceAttenuationModel.callback = SA_PathFalloff;
            inputs.distanceAttenuationModel.userData = &sa.path_dist_mult[i];
            inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
            inputs.pathingProbes = sa.path_batch;
            inputs.visRadius = SA_PATH_VIS_RADIUS;
            inputs.visThreshold = SA_PATH_VIS_SHARE;
            inputs.visRange = SA_PATH_VIS_RANGE;
            inputs.pathingOrder = 1;
            inputs.enableValidation = IPL_TRUE;
            inputs.findAlternatePaths = in->alternates ? IPL_TRUE : IPL_FALSE;
            inputs.deviationModel = &bend;
            n++;
        }
        qiplSourceSetInputs(sa.path_sources[i], IPL_SIMULATIONFLAGS_PATHING, &inputs);
    }
    if (!n)
        return;

    qiplSimulatorRunPathing(sa.simulator);

    now = Sys_Milliseconds();
    for (i = 0, found = sa.path_found; i < MAX_CHANNELS; i++, found++) {
        found->key = in->paths[i].key;
        if (!found->key)
            continue;
        memset(&outputs, 0, sizeof(outputs));
        qiplSourceGetOutputs(sa.path_sources[i], IPL_SIMULATIONFLAGS_PATHING, &outputs);
        found->time = now | 1;
        found->edges = false;
        memcpy(found->eq, outputs.pathing.eqCoeffs, sizeof(found->eq));
        if (outputs.pathing.shCoeffs)
            memcpy(found->sh, outputs.pathing.shCoeffs, sizeof(found->sh));
        else
            memset(found->sh, 0, sizeof(found->sh));
    }

edges:
    if (sa.diffract)
        SA_SimDiffract(in);

    SDL_LockMutex(sa.param_lock);
    for (i = 0; i < MAX_CHANNELS; i++)
        if (sa.path_found[i].key)
            sa.path_out[i] = sa.path_found[i];
    sa.path_msec = (Sys_Microseconds() - start) / 1000.0f;
    sa.path_runs++;
    SDL_UnlockMutex(sa.param_lock);
}

/*
=================
SA_SimThread

Takes a new map as soon as it arrives. Runs the reflections every
sim_in.interval, or sooner when a slot has changed hands, but never sooner
than SA_SIM_MIN_MSEC after the last reflection run ended; and the paths every
sim_in.path_interval, or sooner when a new sound wants them.
=================
*/
static int SDLCALL SA_SimThread(void *arg)
{
    sa_geom_t   *geom;
    unsigned    now, wait, refl_wait, path_wait, geom_map;
    unsigned    last_refl = 0, last_refl_end = 0, last_path = 0;
    bool        new_map;

    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);

    SDL_LockMutex(sa.sim_lock);
    while (!sa.sim_quit) {
        now = Sys_Milliseconds();
        refl_wait = path_wait = 1000;
        if (sa.refl) {
            refl_wait = sa.sim_kick ? 0 : sa.sim_in.interval - min(now - last_refl, sa.sim_in.interval);
            refl_wait = max(refl_wait, SA_SIM_MIN_MSEC - min(now - last_refl_end, SA_SIM_MIN_MSEC));
        }
        if (sa.paths || sa.diffract)
            path_wait = sa.sim_path_kick ? 0 : sa.sim_in.path_interval - min(now - last_path, sa.sim_in.path_interval);
        wait = sa.sim_new_map ? 0 : min(refl_wait, path_wait);
        if (wait) {
            SDL_CondWaitTimeout(sa.sim_wake, sa.sim_lock, wait);
            continue;
        }

        new_map = sa.sim_new_map;
        geom = sa.sim_geom;
        geom_map = sa.sim_geom_map;
        sa.sim_new_map = false;
        sa.sim_geom = NULL;
        if (!refl_wait)
            sa.sim_kick = false;
        if (!path_wait)
            sa.sim_path_kick = false;
        sa.sim_run = sa.sim_in;
        sa.sim_run.check &= !refl_wait;
        if (!refl_wait)
            sa.sim_in.check = false;
        SDL_UnlockMutex(sa.sim_lock);

        // a bake can take a second: what was due before it waits for the
        // next time round
        if (new_map) {
            SA_SimSetMap(geom, geom_map);
        } else if (sa.sim_map && sa.sim_map == sa.sim_run.map) {
            if (!path_wait) {
                last_path = now;
                SA_SimRunPaths(&sa.sim_run);
            }
            if (!refl_wait) {
                last_refl = now;
                SA_SimRun(&sa.sim_run);
                last_refl_end = Sys_Milliseconds();
            }
        } else {
            // nothing to run them on (no scene, or the game thread is still
            // on the last map): what was due counts as done, or this loop
            // spins on sim_lock and the game thread queues behind it
            if (!path_wait)
                last_path = now;
            if (!refl_wait)
                last_refl = last_refl_end = now;
        }

        SDL_LockMutex(sa.sim_lock);
    }
    SDL_UnlockMutex(sa.sim_lock);

    return 0;
}

/*
==============================================================================

MIXER (OpenAL's mixer thread)

==============================================================================
*/

static inline float SA_Sample(const int16_t *data, int n, int i, bool loop)
{
    if (i < 0)
        return loop ? data[(i % n + n) % n] : data[0];
    if (i >= n)
        return loop ? data[i % n] : 0;
    return data[i];
}

/*
=================
SA_Resample

One frame of a voice at the device rate, four-point (Catmull-Rom)
interpolated, with the gain ramped from last frame's to this one's. Returns
true when a one-shot runs out.
=================
*/
static bool SA_Resample(sa_voice_t *v, const sa_parm_t *p, float pitch, float *out)
{
    const int16_t *d = p->data;
    int     n = p->numsamples;
    double  pos = v->pos;
    double  step = (double)p->rate / sa.audio.samplingRate * pitch;
    float   g = v->gain * (1.0f / 32768);
    float   dg = (p->gain - v->gain) * (1.0f / 32768) / SA_FRAME;
    float   s0, s1, s2, s3, f;
    int     i, k;

    v->gain = p->gain;

    for (i = 0; i < SA_FRAME; i++) {
        if (pos >= n) {
            if (!p->looping) {
                memset(out + i, 0, (SA_FRAME - i) * sizeof(*out));
                v->pos = pos;
                return true;
            }
            pos = fmod(pos, n);
        }

        k = (int)pos;
        f = (float)(pos - k);
        s0 = SA_Sample(d, n, k - 1, p->looping);
        s1 = d[k];
        s2 = SA_Sample(d, n, k + 1, p->looping);
        s3 = SA_Sample(d, n, k + 2, p->looping);

        g += dg;
        out[i] = g * (s1 + 0.5f * f * (s2 - s0 + f * (2.0f * s0 - 5.0f * s1 + 4.0f * s2 - s3 +
                      f * (3.0f * (s1 - s2) + s3 - s0))));
        pos += step;
    }

    v->pos = pos;
    return false;
}

/*
=================
SA_MixReflections

The sound off the walls, into the bus: each slot's sends convolved with its
response, summed as Ambisonics and decoded for the listener's head.

Decoded as plain stereo, not through the HRTF, unless s_reflections_hrtf.
Steam Audio's binaural decode of first-order Ambisonics, of sound coming
from every way at once, takes 8 to 12 dB out of 1.5-3 kHz and 5 to 9 out of
the highs against the lows - where the HRTF itself, averaged over every
way, is flat within a dB - and leaves each ear peaks and dips of its own.
Every echo came out dull, boomy and, ringing at a few pitches, metallic.
Under a near sound that hid; a far voice is heard mostly as its echo, and
mgu6m1's soldiers called as a muffled wash (2026-10-21). The plain decode is
flat, and as loud within a dB (A-weighted, for a gun and a voice); the echo
keeps left from right, and loses only above from below and in front from
behind.

Every slot is run every frame, sounding or not. A new response reaches a
slot's effect through a triple buffer that takes the next only once the
last has been read - one left unread holds back the one after it, stale.
Run every frame, each is read within a frame of its arrival, as long as runs
of the simulation are two frames apart (SA_SIM_MIN_MSEC). It also lets the
tail of a sound that has ended ring out.

Except one idle: following no source, so with no response on its way, and
sent nothing for as long as a response lasts and SA_REFL_IDLE_FRAMES more,
past any run that was under way. Its convolution holds only silence by then,
and the next response for it comes after it is run again. So only the slots
in use cost anything. Returns how many were run.
=================
*/
static int SA_MixReflections(unsigned map, const IPLCoordinateSpace3 *listener, bool use_hrtf)
{
    IPLAmbisonicsDecodeEffectParams dp;
    float   *l, *r;
    int     s, j, busy = 0;

    // a new map: nothing of the last one rings on
    if (map != sa.refl_mixed_map) {
        for (s = 0; s <= sa.num_slots; s++)
            qiplReflectionEffectReset(sa.refl_effects[s]);
        qiplReflectionMixerReset(sa.refl_mixer);
        qiplAmbisonicsDecodeEffectReset(sa.refl_decode);
        sa.refl_mixed_map = map;
    }

    for (s = 0; s <= sa.num_slots; s++) {
        if (s == sa.num_slots || sa.refl_follow[s] || sa.refl_sent[s])
            sa.refl_idle[s] = 0;
        else if (sa.refl_idle[s] < sa.refl_ringout)
            sa.refl_idle[s]++;
        if (sa.refl_idle[s] >= sa.refl_ringout)
            continue;
        qiplReflectionEffectApply(sa.refl_effects[s], &sa.refl_params[s], &sa.refl_in[s],
                                  &sa.refl_amb, sa.refl_mixer);
        busy++;
    }
    qiplReflectionMixerApply(sa.refl_mixer, &sa.refl_params[0], &sa.refl_amb);

    dp.order = SA_REFL_ORDER;
    dp.hrtf = sa.hrtf;
    dp.orientation = *listener;
    dp.binaural = use_hrtf;     // and s_reflections_hrtf: see above
    qiplAmbisonicsDecodeEffectApply(sa.refl_decode, &dp, &sa.refl_amb, &sa.out);

    l = sa.out.data[0];
    r = sa.out.data[1];
    for (j = 0; j < SA_FRAME; j++) {
        sa.bus[0][j] += l[j];
        sa.bus[1][j] += r[j];
    }
    return busy;
}

static void SA_StartVoice(sa_voice_t *v, const sa_parm_t *p)
{
    v->gen = p->gen;
    v->finished = false;
    v->pos = p->start;
    v->gain = p->gain;  // no ramp up from silence: it would soften every attack
    qiplDirectEffectReset(v->direct);
    qiplBinauralEffectReset(v->binaural);
    qiplPanningEffectReset(v->panning);
    v->path_live = false;
}

// sa.mid into `bus` from `dir`, `spatial` of it placed, the rest in the
// middle of the head
static void SA_MixPlaced(float (*bus)[SA_FRAME], IPLBinauralEffect binaural, IPLPanningEffect panning,
                         const IPLVector3 *dir, float spatial, bool use_hrtf, IPLHRTFInterpolation interp)
{
    IPLBinauralEffectParams bp;
    IPLPanningEffectParams  pp;
    float       c, *l, *r, *m;
    int         j;

    if (use_hrtf) {
        bp.direction = *dir;
        bp.interpolation = interp;
        bp.spatialBlend = spatial;
        bp.hrtf = sa.hrtf;
        bp.peakDelays = NULL;
        qiplBinauralEffectApply(binaural, &bp, &sa.mid, &sa.out);
    } else {
        // plain stereo panning, for speakers. Blended to the centre by hand,
        // as the binaural effect does it itself.
        pp.direction = *dir;
        qiplPanningEffectApply(panning, &pp, &sa.mid, &sa.out);
        c = 1.0f - spatial;
        l = sa.out.data[0];
        r = sa.out.data[1];
        m = sa.mid.data[0];
        for (j = 0; j < SA_FRAME; j++) {
            l[j] = l[j] * spatial + m[j] * c;
            r[j] = r[j] * spatial + m[j] * c;
        }
    }

    l = sa.out.data[0];
    r = sa.out.data[1];
    for (j = 0; j < SA_FRAME; j++) {
        bus[0][j] += l[j];
        bus[1][j] += r[j];
    }
}

/*
=================
SA_Duck

Everything but the player's own weapon is turned down while it sounds, by
s_steamaudio_duck at the most, and the weapon is mixed back in on top. A
monster yelling within a few metres came out as loud as the gun at the
ears, or louder - its full level through the HRTF against the gun's
centred -3 dB - and the two together went over the ceiling, so SA_Limit took
the punch out of every shot. Quake II's own mixer had a monster in front at
half the level of the player's own sounds, a side.

The rest goes down with the gun's level, fully from SA_DUCK_KEY up, and
comes back over SA_DUCK_RELEASE as its shots die away; in dB, so a silenced
gun ducks it less. Only the direct sound: the echoes, the gun's own among
them, are mixed in after.
=================
*/
static void SA_Duck(float depth)
{
    float   peak = 0, level, target, gain, step;
    int     j;

    for (j = 0; j < SA_FRAME; j++)
        peak = max(peak, max(fabsf(sa.key_bus[0][j]), fabsf(sa.key_bus[1][j])));

    // up at once, down over SA_DUCK_RELEASE
    level = min(peak / SA_DUCK_KEY, 1.0f);
    if (level < sa.duck_level)
        level += (sa.duck_level - level) * expf(-SA_FRAME / (SA_DUCK_RELEASE * sa.audio.samplingRate));
    sa.duck_level = level;

    target = powf(depth, level);
    gain = sa.duck_gain;
    step = (target - gain) / SA_FRAME;
    for (j = 0; j < SA_FRAME; j++) {
        gain += step;
        sa.bus[0][j] = sa.bus[0][j] * gain + sa.key_bus[0][j];
        sa.bus[1][j] = sa.bus[1][j] * gain + sa.key_bus[1][j];
    }
    sa.duck_gain = target;
    sa.meter_duck = min(sa.meter_duck, target);
}

/*
=================
SA_Limit

The mix's ceiling (s_steamaudio_ceiling, dB under full scale). OpenAL sums
this source with the music and then limits the total, so a mix over full
scale turns the music down with it - for as long as the echoes of a fight
keep it there. Held under the ceiling here, only the effects are turned down.
The gain drops at once on a sample that would go over, and comes back up over
SA_LIMIT_RELEASE. Also meters the mix for sa_voices.
=================
*/
static void SA_Limit(float *out, float ceiling)
{
    float   gain = sa.limit_gain, least = 1, peak = 0, k, x;
    int     j;

    k = 1 - expf(-1.0f / (SA_LIMIT_RELEASE * sa.audio.samplingRate));
    for (j = 0; j < SA_FRAME; j++) {
        x = max(fabsf(out[j * 2]), fabsf(out[j * 2 + 1]));
        peak = max(peak, x);
        if (ceiling <= 0)
            continue;
        if (x * gain > ceiling)
            gain = ceiling / x;
        least = min(least, gain);
        out[j * 2] *= gain;
        out[j * 2 + 1] *= gain;
        gain += (1 - gain) * k;
    }
    sa.limit_gain = ceiling > 0 ? gain : 1;

    sa.meter_peak = max(sa.meter_peak, peak);
    sa.meter_gain = min(sa.meter_gain, least);
    sa.meter_frames++;
    if (peak > 1)
        sa.meter_over++;
}

// one frame of both outputs: the mix, stereo interleaved, and the room feed
static void SA_MixFrame(float *out, float *room)
{
    IPLDirectEffectParams   dp, path_dp;
    sa_voice_t  *v;
    sa_parm_t   *p;
    float       pitch, volume, ceiling, duck, *m;
    const float *in;
    bool        use_hrtf, refl_hrtf, refl_on;
    IPLHRTFInterpolation interp;
    IPLCoordinateSpace3 listener;
    unsigned    wait, refl_map, usec;
    uint64_t    start = Sys_Microseconds();
    int         i, j, busy = 0;

    SDL_LockMutex(sa.param_lock);
    memcpy(sa.snap, sa.parms, sizeof(sa.snap));
    pitch = sa.pitch;
    use_hrtf = sa.use_hrtf;
    refl_hrtf = sa.refl_hrtf;
    interp = sa.interp;
    volume = sa.volume;
    ceiling = sa.ceiling;
    duck = sa.duck;
    refl_on = sa.refl_on;
    refl_map = sa.refl_mix_map;
    listener = sa.listener_space;
    memcpy(sa.refl_follow, sa.slot_follow, sizeof(sa.refl_follow));
    SDL_UnlockMutex(sa.param_lock);

    memset(sa.bus, 0, sizeof(sa.bus));
    memset(sa.key_bus, 0, sizeof(sa.key_bus));
    memset(sa.room, 0, sizeof(sa.room));
    if (sa.refl) {
        for (i = 0; i <= sa.num_slots; i++)
            memset(sa.refl_in[i].data[0], 0, SA_FRAME * sizeof(float));
        memset(sa.refl_sent, 0, sizeof(sa.refl_sent));
    }

    memset(&dp, 0, sizeof(dp));
    dp.flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
               IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION |
               IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
    dp.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;

    // the way round: its level as the distance attenuation, its bends and
    // the air as the air absorption
    memset(&path_dp, 0, sizeof(path_dp));
    path_dp.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION |
                    IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;

    for (i = 0; i < MAX_CHANNELS; i++) {
        v = &sa.voices[i];
        p = &sa.snap[i];

        if (!p->playing || !p->data)
            continue;
        if (v->gen != p->gen) {
            SA_StartVoice(v, p);
            wait = (unsigned)(Sys_Microseconds() - p->queued);
            sa.wait_sum += wait;
            sa.wait_count++;
            sa.wait_max = max(sa.wait_max, wait);
        }
        if (v->finished)
            continue;

        v->finished = SA_Resample(v, p, pitch, sa.in.data[0]);

        // off the walls: the sound as it leaves the source, before what
        // lies on the direct path
        if (refl_on && p->refl_slot >= 0 && p->refl > 0) {
            m = sa.refl_in[p->refl_slot].data[0];
            in = sa.in.data[0];
            for (j = 0; j < SA_FRAME; j++)
                m[j] += in[j] * p->refl;
            sa.refl_sent[p->refl_slot] = true;
        }

        dp.occlusion = p->occlusion;
        for (j = 0; j < 3; j++) {
            dp.transmission[j] = p->trans[j];
            dp.airAbsorption[j] = p->air[j];
        }
        qiplDirectEffectApply(v->direct, &dp, &sa.in, &sa.mid);

        // the room hears it before the head does - unless the walls do
        if (!refl_on && p->room > 0) {
            m = sa.mid.data[0];
            for (j = 0; j < SA_FRAME; j++)
                sa.room[j] += m[j] * p->room;
        }

        SA_MixPlaced(p->key ? sa.key_bus : sa.bus, v->binaural, v->panning, &p->dir, p->spatial, use_hrtf, interp);

        // what comes round, as a second voice from where it arrives: to
        // the room too, as what came round went before as part of the
        // direct sound
        if (p->path > 0) {
            if (!v->path_live) {
                qiplDirectEffectReset(v->path_direct);
                qiplBinauralEffectReset(v->path_binaural);
                qiplPanningEffectReset(v->path_panning);
                v->path_live = true;
            }
            path_dp.distanceAttenuation = p->path;
            for (j = 0; j < 3; j++)
                path_dp.airAbsorption[j] = p->path_eq[j];
            qiplDirectEffectApply(v->path_direct, &path_dp, &sa.in, &sa.mid);

            if (!refl_on && p->room > 0) {
                m = sa.mid.data[0];
                for (j = 0; j < SA_FRAME; j++)
                    sa.room[j] += m[j] * p->room;
            }

            SA_MixPlaced(p->key ? sa.key_bus : sa.bus, v->path_binaural, v->path_panning,
                         &p->path_dir, p->path_spatial, use_hrtf, interp);
        } else {
            v->path_live = false;
        }
    }

    SA_Duck(duck);

    if (sa.refl)
        busy = SA_MixReflections(refl_map, &listener, use_hrtf && refl_hrtf);

    SDL_LockMutex(sa.param_lock);
    for (i = 0; i < MAX_CHANNELS; i++) {
        v = &sa.voices[i];
        p = &sa.parms[i];
        if (v->gen != p->gen)
            continue;   // restarted while this frame was mixing
        p->pos = v->pos;
        p->pos_gen = v->gen;
        if (v->finished)
            p->done_gen = v->gen;
    }
    SDL_UnlockMutex(sa.param_lock);

    for (j = 0; j < SA_FRAME; j++) {
        out[j * 2 + 0] = sa.bus[0][j] * volume;
        out[j * 2 + 1] = sa.bus[1][j] * volume;
        room[j] = sa.room[j] * volume;
    }
    SA_Limit(out, ceiling);

    usec = (unsigned)(Sys_Microseconds() - start);
    sa.mix_usec_sum += usec;
    sa.mix_usec_max = max(sa.mix_usec_max, usec);
    sa.busy_sum += busy;
    sa.busy_max = max(sa.busy_max, (unsigned)busy);
}

// which source a callback is for
enum { SA_OUT_MIX, SA_OUT_ROOM };

/*
=================
SA_Callback

Both sources' callback. OpenAL asks each for samples separately, in step, as
they are started together at the same rate: whichever asks first for samples
not mixed yet has the next frame mixed, for both, and the other finds it in
the ring. One that has somehow fallen further behind than the ring holds
skips ahead rather than stalling the other.
=================
*/
static ALsizei AL_APIENTRY SA_Callback(ALvoid *userptr, ALvoid *sampledata, ALsizei numbytes)
{
    int         which = (int)(intptr_t)userptr;
    int         width = which == SA_OUT_MIX ? 2 : 1;
    float       *out = sampledata;
    int         frames = numbytes / (int)(width * sizeof(float));
    uint64_t    *cursor = &sa.cursor[which];
    const float *src;
    int         slot, offset, n;

    SDL_LockMutex(sa.mix_lock);

    while (frames > 0) {
        if (*cursor >= sa.mixed * SA_FRAME) {
            slot = sa.mixed % SA_RING;
            if (sa.dead) {
                memset(sa.ring[slot], 0, sizeof(sa.ring[slot]));
                memset(sa.room_ring[slot], 0, sizeof(sa.room_ring[slot]));
            } else {
                SA_MixFrame(sa.ring[slot], sa.room_ring[slot]);
            }
            sa.mixed++;
        }

        if (sa.mixed > SA_RING && *cursor < (sa.mixed - SA_RING) * SA_FRAME)
            *cursor = (sa.mixed - SA_RING) * SA_FRAME;

        slot = (*cursor / SA_FRAME) % SA_RING;
        offset = *cursor % SA_FRAME;
        n = min(frames, SA_FRAME - offset);
        if (which == SA_OUT_MIX)
            src = sa.ring[slot] + offset * 2;
        else
            src = sa.room_ring[slot] + offset;
        memcpy(out, src, n * width * sizeof(float));

        out += n * width;
        frames -= n;
        *cursor += n;
    }

    SDL_UnlockMutex(sa.mix_lock);

    return numbytes;
}

/*
==============================================================================

GAME THREAD

==============================================================================
*/

// Quake (x forward, y left, z up) to Steam Audio's listener space (x right,
// y up, -z ahead)
static void SA_ListenerDir(const vec3_t d, IPLVector3 *out)
{
    out->x = DotProduct(d, listener_right);
    out->y = DotProduct(d, listener_up);
    out->z = -DotProduct(d, listener_forward);
}

/*
=================
SA_RoomGain

What OpenAL sends to the room reverb from one of its own sources this far
away, with the distance settings al.c gives them (AL_SetDistance: linear from
SOUND_FULLVOLUME, rolled off by dist_mult over SA_AL_MAX_DISTANCE, or 1/r from
the reference distance at a rolloff of 1). OpenAL Soft does not attenuate the
send the way it does the direct path: it takes an inverse curve over the
rolled-off distance and blends in the reverb's own decay over it, so only
sounds near the listener reach the room at any level - -20 dB and more by 200
units on Quake II's line. That is what the reverb was set up against with
s_steamaudio 0, so the Steam Audio mix sends the same.
=================
*/
static float SA_RoomGain(float dist, float dist_mult)
{
    float   ref, base, attn, decay;

    if (dist_mult < 0) {
        ref = -dist_mult;
        base = dist - ref;
    } else {
        ref = SOUND_FULLVOLUME;
        base = (dist - ref) * dist_mult * (SA_AL_MAX_DISTANCE - SOUND_FULLVOLUME);
    }

    if (dist <= ref)
        return 1;

    attn = ref / (ref + base);

    // the distance the reverb takes to die away by 60 dB, at 343.3 m/s
    decay = AL_ReverbDecayTime() * 343.3f;
    if (decay <= 0)
        return attn;

    return powf(0.001f, base / decay) * (1 - attn) + attn;
}

/*
=================
SA_ReflSend

What of a sound this far away goes off the walls, against what is heard of
it directly. Steam Audio's responses are in proportion to a direct sound that
falls off as real sound does, as 1/r from a metre out; Quake's direct sound
falls off far more gently. Scaled up by the difference, the reflections keep
the balance of direct and reflected sound that the simulation gives - out to
SA_REFL_FULL_DIST, two metres, and held there beyond.
Quake's falloff keeps a distant sound clear on purpose, and the real balance
would bury it: what comes off the walls outweighs the direct sound from a
metre on in a small metal room, from three in a 24 m hall, and keeps growing
with distance. Held at two metres, a sound across a small room comes with
its echo a few dB over it, one across a hall a few dB under.
Under s_falloff 1 the direct sound falls off as real sound does, and for a
while the balance was kept at every distance, so that a far sound came more
and more as what comes off the walls. It buried far voices: a soldier 25 m
off in mgu6m1's rock gorge called with his echo 13 dB over his voice. So a
voice with a slot of its own keeps the real balance out to
s_reflections_hold metres and is held there beyond - 3 by default, which
puts that soldier's echo about 5 dB under him; 1000 keeps the real balance
at every distance. Where the listener's room's response stands in for a
voice's own (`held`) it is held at SA_REFL_FULL_DIST: that is right only
for a sound in the listener's room, and the far ones mostly are not. So is
a loop sound: a map's hums are many and laid out for Quake II's falloff,
and each at its echo's full level adds up to a roar.
=================
*/
static float SA_ReflSend(float dist, float dist_mult, bool held)
{
    float r = max(dist, SA_UNITS_PER_METER);

    if (held || dist_mult >= 0)
        r = min(r, SA_REFL_FULL_DIST);
    else
        r = min(r, sa.refl_hold);

    return SA_Falloff(dist, dist_mult) * r / SA_UNITS_PER_METER;
}

// What a reflection slot follows: the entity a sound comes from, or for a
// world sound, with none, its channel. Never 0.
static int SA_SlotKey(const channel_t *ch)
{
    return ch->entnum > 0 ? ch->entnum : -1 - (int)(ch - channels);
}

static int SA_SlotIndex(int key)
{
    int s;

    for (s = 0; s < sa.num_slots; s++)
        if (sa.slots[s].key == key)
            return s;
    return -1;
}

// The slot for a new source of sound this loud: one never used, else the one
// quiet longest, else that of a source far quieter. -1 for none.
static int SA_FreeSlot(float level)
{
    int s, best = -1;

    for (s = 0; s < sa.num_slots; s++)
        if (!sa.slots[s].key)
            return s;

    for (s = 0; s < sa.num_slots; s++) {
        if (sa.slots[s].live)
            continue;
        if (best < 0 || sa.slots[s].last_live < sa.slots[best].last_live)
            best = s;
    }
    if (best >= 0)
        return best;

    for (s = 0; s < sa.num_slots; s++)
        if (best < 0 || sa.slots[s].level < sa.slots[best].level)
            best = s;
    if (best >= 0 && level > sa.slots[best].level * SA_SLOT_TAKEOVER)
        return best;
    return -1;
}

/*
=================
SA_AssignSlots

Hands the reflection slots to the loudest sources of sound. A source keeps
its slot while it plays, and for SA_SLOT_KEEP_MSEC after its entity falls
quiet the slot follows it about, so its response is ready when it sounds
again - a monster's next shot. A new source takes a slot never used, the one
quiet longest, or that of a source SA_SLOT_TAKEOVER times quieter. Until
its response is in (SA_UpdateSimulation) a sound is heard off the walls
through the room's.

Returns true when a slot changed hands, for the simulation to run soon.
=================
*/
static bool SA_AssignSlots(void)
{
    const centity_t *ent;
    sa_slot_t   *slot;
    sa_chan_t   *c;
    channel_t   *ch;
    int         order[MAX_CHANNELS];
    int         i, j, n, s;
    bool        kick = false;

    for (s = 0; s < sa.num_slots; s++) {
        sa.slots[s].live = false;
        sa.slots[s].level = 0;
    }

    // the sources that have a slot; the rest wait, loudest first
    for (i = n = 0, ch = channels; i < s_numchannels; i++, ch++) {
        c = &sa.chans[i];
        if (!ch->sfx || !ch->steam || !c->placed || c->level <= 0)
            continue;
        s = SA_SlotIndex(SA_SlotKey(ch));
        if (s < 0) {
            for (j = n++; j > 0 && sa.chans[order[j - 1]].level < c->level; j--)
                order[j] = order[j - 1];
            order[j] = i;
            continue;
        }
        slot = &sa.slots[s];
        if (c->level > slot->level) {
            slot->level = c->level;
            VectorCopy(c->origin, slot->origin);
        }
        slot->live = true;
    }

    for (i = 0; i < n; i++) {
        ch = &channels[order[i]];
        c = &sa.chans[order[i]];
        s = SA_SlotIndex(SA_SlotKey(ch));   // a louder sound of the same source took one
        if (s < 0) {
            s = SA_FreeSlot(c->level);
            if (s < 0)
                continue;
            memset(&sa.slots[s], 0, sizeof(sa.slots[s]));
            sa.slots[s].key = SA_SlotKey(ch);
            kick = true;
        }
        slot = &sa.slots[s];
        if (c->level > slot->level) {
            slot->level = c->level;
            VectorCopy(c->origin, slot->origin);
        }
        slot->live = true;
    }

    for (s = 0; s < sa.num_slots; s++) {
        slot = &sa.slots[s];
        slot->simulate = false;
        if (!slot->key)
            continue;
        ent = slot->key > 0 ? &cl_entities[slot->key] : NULL;
        if (slot->live) {
            slot->last_live = cls.realtime;
            slot->simulate = true;
            if (ent)
                VectorSubtract(slot->origin, ent->current.origin, slot->offset);
        } else if (ent && ent->serverframe == cl.frame.number &&
                   cls.realtime - slot->last_live < SA_SLOT_KEEP_MSEC) {
            VectorAdd(ent->current.origin, slot->offset, slot->origin);
            slot->simulate = true;
        }
    }

    return kick;
}

/*
=================
SA_UpdateSimulation

Once a frame: a new map's geometry and probes to the simulation, the slots to
the sources of sound, what the simulation has done to the voices, and what
it is to place and look for next.
=================
*/
static void SA_UpdateSimulation(void)
{
    const bsp_t     *bsp = cls.state == ca_active ? cl.bsp : NULL;
    sa_sim_input_t  *in = &sa.sim_in;
    const centity_t *ent;
    const mmodel_t  *cmodel;
    const channel_t *ch;
    const sa_chan_t *c;
    sa_geom_t       *geom;
    const char      *error, *why;
    char            *report;
    unsigned        key;
    float           spacing, sec;
    int             i, s, m, probes;
    bool            kick = false, path_kick = false, ask, tell, cached;

    if (!sa.sim)
        return;

    sa.refl_gain = Cvar_ClampValue(s_reflections_gain, 0, 4);
    sa.refl_hold = Cvar_ClampValue(s_reflections_hold, 1, 1000) * SA_UNITS_PER_METER;
    sa.path_gain = Cvar_ClampValue(s_pathing_gain, 0, 4);

    if (bsp != sa.map_bsp || (bsp && bsp->checksum != sa.map_checksum)) {
        sa.map_bsp = bsp;
        sa.map_checksum = bsp ? bsp->checksum : 0;
        sa.map++;
        memset(sa.slots, 0, sizeof(sa.slots));
        why = NULL;
        geom = bsp ? SA_BuildGeometry(bsp, &why) : NULL;
        if (bsp && !geom)
            Com_WPrintf("Steam Audio: no scene for %s (%s).\n", bsp->name, why);
        if (geom && sa.paths)
            SA_BuildProbes(bsp, geom);

        SDL_LockMutex(sa.sim_lock);
        SA_FreeGeometry(sa.sim_geom);   // one the simulation never took
        sa.sim_geom = geom;
        sa.sim_geom_map = sa.map;
        sa.sim_new_map = true;
        SDL_UnlockMutex(sa.sim_lock);
        SDL_CondSignal(sa.sim_wake);
    }

    if (bsp && sa.refl)
        kick = SA_AssignSlots();

    // A slot's response serves its source once simulated on this map, near
    // where the source is now. Until the room's is in, the room reverb
    // stands in for all of it.
    SDL_LockMutex(sa.param_lock);
    if (sa.refl) {
        for (s = 0; s <= sa.num_slots; s++) {
            sa.slot_ready[s] = sa.refl_map == sa.map && sa.refl_ran[s];
            if (s < sa.num_slots && sa.slot_ready[s])
                sa.slot_ready[s] = sa.slots[s].key && DistanceSquared(sa.refl_origin[s], sa.slots[s].origin) <
                                   SA_SLOT_READY_DIST * SA_SLOT_READY_DIST;
            sa.slot_follow[s] = s == sa.num_slots || sa.slots[s].simulate;
        }
        sa.refl_on = bsp && sa.slot_ready[sa.num_slots];
        sa.refl_mix_map = sa.map;
        sa.listener_space = SA_WorldSpace(listener_origin, listener_forward, listener_right, listener_up);
    }
    report = sa.check_report;
    sa.check_report = NULL;
    tell = sa.paths && bsp && sa.path_map == sa.map && sa.path_told != sa.map;
    probes = sa.path_probes;
    spacing = sa.path_spacing;
    cached = sa.path_cached;
    sec = sa.path_sec;
    error = sa.path_error;
    SDL_UnlockMutex(sa.param_lock);

    if (report) {
        Com_Printf("%s", report);
        free(report);
    }

    // once a map, when its paths are in
    if (tell) {
        sa.path_told = sa.map;
        if (error)
            Com_Printf("Steam Audio: no paths on this map, %s.\n", error);
        else if (cached)
            Com_DPrintf("Steam Audio: paths for %d probes %.1f m apart, loaded in %.2f s.\n",
                        probes, spacing / SA_UNITS_PER_METER, sec);
        else
            Com_Printf("Steam Audio: paths baked for %d probes %.1f m apart in %.1f s.\n",
                       probes, spacing / SA_UNITS_PER_METER, sec);
    }

    if (!bsp)
        return;

    SDL_LockMutex(sa.sim_lock);
    in->map = sa.map;
    VectorCopy(listener_origin, in->listener);
    VectorCopy(listener_forward, in->forward);
    VectorCopy(listener_right, in->right);
    VectorCopy(listener_up, in->up);

    if (sa.refl) {
        for (s = 0; s < sa.num_slots; s++) {
            in->sources[s].enabled = sa.slots[s].simulate;
            VectorCopy(sa.slots[s].origin, in->sources[s].origin);
        }
        in->sources[s].enabled = true;
        VectorCopy(listener_origin, in->sources[s].origin);

        in->rays = Cvar_ClampInteger(s_reflections_rays, 64, SA_REFL_MAX_RAYS);
        in->bounces = Cvar_ClampInteger(s_reflections_bounces, 1, 256);
        in->interval = 1000 / Cvar_ClampInteger(s_reflections_rate, 1, 40);
        in->gpu = s_reflections_gpu->integer;
        in->gpu_rays = Cvar_ClampInteger(s_reflections_gpu_rays, 64, SA_REFL_MAX_RAYS);
    }

    in->num_bmodels = 0;
    for (i = 0; i < cl.numSolidEntities && in->num_bmodels < SA_SIM_MAX_BMODELS; i++) {
        ent = cl.solidEntities[i];
        if (ent->current.solid != PACKED_BSP)
            continue;
        cmodel = cl.model_clip[ent->current.modelindex];
        if (!cmodel)
            continue;
        m = cmodel - bsp->models;
        if (m <= 0 || m >= bsp->nummodels)
            continue;
        in->bmodels[in->num_bmodels].model = m;
        VectorCopy(ent->current.origin, in->bmodels[in->num_bmodels].origin);
        VectorCopy(ent->current.angles, in->bmodels[in->num_bmodels].angles);
        in->num_bmodels++;
    }

    // A way round for every voice placed in the world and heard, while the
    // walls are heard at all. A new one runs the paths soon, as till then
    // SA_FindPath stands in.
    ask = (sa.paths || sa.diffract) && SA_OcclusionStrength() > 0 && !sa.listener_buried;
    for (i = 0, ch = channels, c = sa.chans; i < MAX_CHANNELS; i++, ch++, c++) {
        key = 0;
        if (ask && i < s_numchannels && ch->sfx && ch->steam && c->placed && c->level > 0)
            key = c->path_key;
        in->paths[i].key = key;
        if (key) {
            VectorCopy(c->origin, in->paths[i].origin);
            in->paths[i].dist_mult = ch->dist_mult;
            if (key != sa.path_asked[i])
                path_kick = true;
        }
        sa.path_asked[i] = key;
    }
    in->path_interval = 1000 / Cvar_ClampInteger(s_pathing_rate, 1, 100);
    in->alternates = s_pathing_alternates->integer;

    if (kick)
        sa.sim_kick = true;
    if (path_kick)
        sa.sim_path_kick = true;
    SDL_UnlockMutex(sa.sim_lock);

    if (kick || path_kick)
        SDL_CondSignal(sa.sim_wake);
}

// which slot a voice is heard off the walls through, and how much of it
static void SA_Reflect(const channel_t *ch, float dist, sa_parm_t *p)
{
    const sa_chan_t *c = &sa.chans[ch - channels];
    float   send, vis;
    int     s = sa.num_slots;

    p->refl_slot = -1;
    p->refl = 0;
    // a radio voice comes over the radio, not off the walls
    if (!sa.refl_on || p->gain <= 0 || AL_ChannelIsRadio(ch))
        return;

    if (!c->placed) {
        // The player's own. The room's response is for a source about a
        // metre off; a gun or a voice is half that from the ears, so is
        // heard twice as loud over its echo.
        send = ch->master_vol * SA_REFL_OWN;
        if (p->key)
            send *= sa.weapon_gain;
    } else {
        s = SA_SlotIndex(SA_SlotKey(ch));
        if (s < 0 || !sa.slot_ready[s]) {
            // The room's response stands in. It is right for a sound in the
            // listener's own room, which one in sight more likely is, so one
            // behind a wall sends less. A whole-level one is more likely
            // outside it altogether - a ship overhead - and sends only what
            // gets through.
            send = ch->master_vol * SA_ReflSend(dist, ch->dist_mult, true);
            // one behind something it goes round on either side of - a
            // pillar, a crate - is in the listener's room
            vis = c->small ? 1 : p->occlusion + (1 - p->occlusion) * p->trans[1];
            send *= ch->dist_mult ? SA_REFL_HIDDEN + (1 - SA_REFL_HIDDEN) * vis : vis;
            s = sa.num_slots;
        } else {
            send = ch->master_vol * SA_ReflSend(dist, ch->dist_mult, ch->autosound);
        }
    }

    p->refl_slot = s;
    p->refl = min(send * sa.refl_gain / p->gain, SA_REFL_MAX_SEND);
}

/*
=================
SA_PathResult

What the last pathing run found for a voice (see PATHING and DIFFRACTION).
Returns its SA_PATH_ state; with SA_PATH_ROUND, the targets of the way round
are set: its level against the straight line's falloff, its EQ, and the way
it comes from, as long as the ways agree.
=================
*/
static int SA_PathResult(sa_chan_t *c, float dist, float dist_mult)
{
    sa_path_out_t   out;
    float           direct;
    int             b;

    if (!(sa.paths || sa.diffract) || !c->path_key)
        return SA_PATH_NONE;

    SDL_LockMutex(sa.param_lock);
    out = sa.path_out[c - sa.chans];
    SDL_UnlockMutex(sa.param_lock);

    if (out.key != c->path_key || Sys_Milliseconds() - out.time > SA_PATH_STALE_MSEC)
        return SA_PATH_NONE;
    c->path_edges = out.edges;

    // no probe in reach of the source or the listener: an EQ no way has
    if (out.eq[0] <= 0 && out.eq[1] <= 0 && out.eq[2] <= 0)
        return SA_PATH_NONE;

    // no way round at all: what goes through the walls is all there is
    c->target_path = 0;
    if (out.sh[0] <= 0)
        return SA_PATH_ROUND;

    // No bend on the way: Steam Audio sees the source, or its probes see
    // each other past whatever is in the way. Our traces may say it is
    // hidden - a grate, a sky brush, a crate's corner - and SA_FindPath's
    // corners take it.
    if (out.eq[0] > 0.999f && out.eq[1] > 0.999f && out.eq[2] > 0.999f)
        return SA_PATH_DIRECT;

    // W is the falloff over the ways' lengths; first order, which way
    direct = SA_Falloff(dist, dist_mult);
    if (direct > 0)
        c->target_path = min(out.sh[0] / SA_SH_C0 / direct, 1.0f);
    for (b = 0; b < 3; b++)
        c->target_path_eq[b] = min(out.eq[b], 1.0f);
    VectorSet(c->target_path_vec, -out.sh[1], out.sh[3], out.sh[2]);
    VectorScale(c->target_path_vec, 1 / (SA_SQRT3 * out.sh[0]), c->target_path_vec);
    return SA_PATH_ROUND;
}

/*
=================
SA_TraceVoice

Sets the targets a voice's occlusion eases towards: how much of the source the
listener sees, what reaches it through walls and round corners, and how much
of what is heard comes round a corner, from which way.

The way round is Steam Audio's when it has one: the probes', or the GPU's
round an edge. SA_FindPath's stands in while it has none - no run yet, in its
sight, or no way it knows of. And over or past a small thing in the way, a
crate or a pillar, SA_EdgePath's takes over when it is the louder: sound goes
round such a thing on every side and over it, where the GPU counts only the
one edge both ends can see (-14 dB of the mids round a crate's corner, -22
of the highs) and the probes go the long way round. An infantry firing from
behind a crate was not heard at all.
=================
*/
static void SA_TraceVoice(sa_chan_t *c, const vec3_t origin, float dist, float dist_mult, int skip)
{
    sa_path_t   path;
    float       trans[3], bent[3], occ, direct, round;
    bool        buried, found;
    int         b, sides = 0;

    buried = CM_PointContents(origin, cl.bsp->nodes) & MASK_SOLID;
    occ = SA_Occlusion(origin, skip, buried, trans);

    c->target_round = 0;
    c->bends = 0;
    c->path_mid = 0;
    c->small = false;

    // Steam Audio's way round, if it has one to say...
    c->path_state = SA_PathResult(c, dist, dist_mult);
    if (c->path_state != SA_PATH_ROUND)
        c->target_path = 0;

    // ...else, mostly hidden: look for one - though while there has been
    // none, not on every trace, as finding none costs the most
    found = false;
    if (occ < 0.5f && c->target_path <= 0) {
        if (cls.realtime >= c->next_path) {
            found = SA_FindPath(c, origin, skip, buried, &path, &sides);
            if (!found)
                c->next_path = cls.realtime + SA_PATH_RETRY_MSEC;
        }
    } else if (occ < 0.5f && cls.realtime >= c->next_edge) {
        found = SA_EdgePath(origin, skip, buried, &path, &sides) && sides > 1;
        if (found) {
            SA_PathGain(&path, dist, dist_mult, bent);
            found = bent[1] * sqrtf(sides) > c->target_path * c->target_path_eq[1];
        }
        if (found)
            c->target_path = 0;
        else
            c->next_edge = cls.realtime + SA_PATH_RETRY_MSEC;
    }

    if (found) {
        SA_PathGain(&path, dist, dist_mult, bent);

        // Round something it can go round on more than one side of - a
        // pillar, a crate - each way carries its share, and they add up in
        // power; the one found stands for the others.
        if (sides > 1) {
            c->small = true;
            for (b = 0; b < 3; b++)
                bent[b] = min(bent[b] * sqrtf(sides), 1.0f);
        }

        // how loud each way is, for where it seems to come from
        direct = occ + (1 - occ) * trans[1];
        round = (1 - occ) * bent[1];
        c->target_round = round * round / max(direct * direct + round * round, 1e-6f);
        VectorCopy(path.dir, c->target_round_dir);

        // what goes through the wall and what goes round add up as power
        for (b = 0; b < 3; b++)
            trans[b] = min(sqrtf(trans[b] * trans[b] + bent[b] * bent[b]), 1.0f);

        c->bends = path.bends;
        c->path_mid = bent[1];
    }

    c->target_occ = occ;
    VectorCopy(trans, c->target_trans);
}

/*
=================
SA_VoicePoint

Where what lies between a sound and the listener is looked for from. For a
monster or a player, its upper body, SA_VOICE_DROP under the top of its box,
where it calls and fires from - not its origin, the middle of the box. In
mgu6m1's canyon the lip of a ledge hid a soldier's origin from the floor below
in every view that had his head and shoulders in plain sight, and each call
of his came through the rock; from the upper body, about one view in eight.
=================
*/
static void SA_VoicePoint(const channel_t *ch, const vec3_t origin, vec3_t point)
{
    const centity_t *ent;
    trace_t     tr;
    vec3_t      top;

    VectorCopy(origin, point);
    if (ch->entnum <= 0 || ch->entnum >= MAX_EDICTS)
        return;

    // in the frame and solid, with a box of its own - not lying dead
    ent = &cl_entities[ch->entnum];
    if (ent->serverframe != cl.frame.number || !ent->current.solid || ent->current.solid == PACKED_BSP)
        return;
    if (ent->maxs[2] <= SA_VOICE_DROP)
        return;

    // up from the origin, short of a low ceiling
    VectorCopy(origin, top);
    top[2] += ent->maxs[2] - SA_VOICE_DROP;
    SA_Trace(&tr, origin, top, ch->entnum);
    if (!tr.startsolid)
        VectorCopy(tr.endpos, point);
}

// occlusion and the way round, eased in: fills p's occlusion and turns `dir`
// (world space, from the listener) towards the way the sound comes round
static void SA_Occlude(channel_t *ch, const vec3_t origin, float dist, vec3_t dir, sa_parm_t *p)
{
    sa_chan_t   *c = &sa.chans[ch - channels];
    vec3_t      round_dir, path_dir, point;
    float       strength, a, w, spread;
    uint64_t    start;
    int         b;

    strength = SA_OcclusionStrength();
    if (!strength || sa.listener_buried)
        return;

    if (c->fresh || cls.realtime >= c->next_trace) {
        start = Sys_Microseconds();
        SA_VoicePoint(ch, origin, point);
        SA_TraceVoice(c, point, dist, ch->dist_mult, ch->entnum > 0 ? ch->entnum : -1);
        sa.trace_usec += (unsigned)(Sys_Microseconds() - start);
        c->next_trace = cls.realtime + SA_TRACE_MSEC;
    }

    if (c->fresh) {
        c->occ = c->target_occ;
        VectorCopy(c->target_trans, c->trans);
        c->round = c->target_round;
        VectorCopy(c->target_round_dir, c->round_dir);
        c->path = c->target_path;
        VectorCopy(c->target_path_eq, c->path_eq);
        VectorCopy(c->target_path_vec, c->path_vec);
        c->fresh = false;
    } else {
        a = 1.0f - expf(-cls.frametime / SA_SMOOTH_SEC);
        c->occ += (c->target_occ - c->occ) * a;
        for (b = 0; b < 3; b++)
            c->trans[b] += (c->target_trans[b] - c->trans[b]) * a;
        c->round += (c->target_round - c->round) * a;
        // with no way round, the last one fades out where it was
        if (c->target_round > 0)
            for (b = 0; b < 3; b++)
                c->round_dir[b] += (c->target_round_dir[b] - c->round_dir[b]) * a;

        // The same for Steam Audio's, which starts as it is from silence -
        // at its level too, while the sound is new: a run takes 30-100 ms to
        // answer, and a shot is over in about a tenth of a second.
        if (c->target_path > 0 && c->path < 0.001f) {
            VectorCopy(c->target_path_eq, c->path_eq);
            VectorCopy(c->target_path_vec, c->path_vec);
            if (cls.realtime - c->started < SA_YOUNG_MSEC)
                c->path = c->target_path;
        } else if (c->target_path > 0) {
            for (b = 0; b < 3; b++) {
                c->path_eq[b] += (c->target_path_eq[b] - c->path_eq[b]) * a;
                c->path_vec[b] += (c->target_path_vec[b] - c->path_vec[b]) * a;
            }
        }
        c->path += (c->target_path - c->path) * a;
    }

    p->occlusion = 1.0f - strength * (1.0f - c->occ);
    VectorCopy(c->trans, p->trans);

    // what of the hidden part comes round Steam Audio's ways, as a voice of
    // its own from where it arrives; one spread over ways that disagree is
    // that much less placed
    w = (1.0f - p->occlusion) * c->path * sa.path_gain;
    if (w > 0) {
        p->path = w;
        for (b = 0; b < 3; b++)
            p->path_eq[b] = c->path_eq[b] * p->air[b];
        spread = VectorNormalize2(c->path_vec, path_dir);
        if (spread > 0)
            SA_ListenerDir(path_dir, &p->path_dir);
        p->path_spatial = min(max(spread, SA_PATH_MIN_SPATIAL), 1.0f);
    }

    w = c->round * strength;
    if (w > 0 && VectorNormalize2(c->round_dir, round_dir) > 0) {
        for (b = 0; b < 3; b++)
            round_dir[b] = dir[b] * (1 - w) + round_dir[b] * w;
        if (VectorNormalize(round_dir) > 0.01f)
            VectorCopy(round_dir, dir);
    }
}

/*
=================
SA_GlobalFloor

A whole-level sound that comes from somewhere (AL_GlobalPlaced) is still
meant to be heard all over the level. What lies in the way takes it down to
s_global_floor at most, the lows less than the highs, as a building lets the
rumble of a ship overhead through and not its whine; in the open it is at
full level, from where it is. A flyby is nearly all below 300 Hz, so the
floor's low band is what it is heard at indoors.
=================
*/
static const float sa_global_tilt[3] = { 1.0f, 0.5f, 0.2f };

static void SA_GlobalFloor(sa_parm_t *p)
{
    float   least = Cvar_ClampValue(s_global_floor, 0, 1);
    int     b;

    for (b = 0; b < 3; b++)
        p->trans[b] = max(p->trans[b], least * sa_global_tilt[b]);
}

/*
=================
SA_Place

Everything about a channel that follows from where it is: gain, direction,
what lies between it and the listener, and what it sends to the room. Fills
the placement fields of `p`.
=================
*/
static void SA_Place(channel_t *ch, sa_parm_t *p)
{
    sa_chan_t   *c = &sa.chans[ch - channels];
    vec3_t      origin, d;
    float       dist;
    int         b;
    bool        wet;

    p->occlusion = 1;
    for (b = 0; b < 3; b++)
        p->trans[b] = p->air[b] = p->path_eq[b] = 1;
    p->path = 0;
    p->path_dir.x = p->path_dir.y = 0;
    p->path_dir.z = -1;
    p->path_spatial = 1;
    p->key = false;

    if (!AL_ChannelOrigin(ch, origin)) {
        // from the listener: no falloff, no direction, and OpenAL sends such
        // a sound to the room in full - a radio voice not at all
        p->gain = ch->master_vol * SA_CENTRE_GAIN;
        // the player's own weapon - every muzzle flash's sound - stands over
        // the rest (SA_Duck)
        if (ch->entnum == listener_entnum && (ch->entchannel & 7) == CHAN_WEAPON) {
            p->key = true;
            p->gain *= sa.weapon_gain;
        }
        p->room = AL_ChannelIsRadio(ch) ? 0 : 1.0f / SA_CENTRE_GAIN;
        p->spatial = 0;
        p->dir.x = p->dir.y = 0;
        p->dir.z = -1;
        c->dist = 0;
        c->placed = false;
        SA_Reflect(ch, 0, p);
        return;
    }

    VectorSubtract(origin, listener_origin, d);
    dist = VectorNormalize(d);
    c->dist = dist;

    p->gain = ch->master_vol * SA_Falloff(dist, ch->dist_mult);

    c->placed = true;
    c->placed_at = cls.realtime;
    c->level = p->gain;
    VectorCopy(origin, c->origin);

    // a head-sized region where the direction means nothing: blend to the
    // centre over the first metre rather than flip as a source passes through
    p->spatial = min(dist / SA_UNITS_PER_METER, 1.0f);
    p->gain *= SA_CENTRE_GAIN + (1.0f - SA_CENTRE_GAIN) * p->spatial;

    // the room send is taken from the voice after its gain; a radio voice
    // has none
    p->room = 0;
    if (p->gain > 0 && !AL_ChannelIsRadio(ch))
        p->room = min(ch->master_vol * SA_RoomGain(dist, ch->dist_mult) / p->gain, 4.0f);

    for (b = 0; b < 3; b++)
        p->air[b] = expf(-sa_air_coeffs[b] * dist / SA_UNITS_PER_METER);

    if (cls.state == ca_active && cl.bsp) {
        wet = CM_PointContents(origin, cl.bsp->nodes) & MASK_WATER;
        if (wet != sa.listener_wet) {
            for (b = 0; b < 3; b++)
                p->air[b] *= sa_water[b];
        }

        SA_Occlude(ch, origin, dist, d, p);

        if (!ch->dist_mult)
            SA_GlobalFloor(p);
    }

    if (dist < 1) {
        p->dir.x = p->dir.y = 0;
        p->dir.z = -1;
    } else {
        SA_ListenerDir(d, &p->dir);
    }

    SA_Reflect(ch, dist, p);
}

static void SA_CopyPlacement(sa_parm_t *dst, const sa_parm_t *src)
{
    dst->gain = src->gain;
    dst->spatial = src->spatial;
    dst->dir = src->dir;
    dst->occlusion = src->occlusion;
    memcpy(dst->trans, src->trans, sizeof(dst->trans));
    memcpy(dst->air, src->air, sizeof(dst->air));
    dst->room = src->room;
    dst->key = src->key;
    dst->refl_slot = src->refl_slot;
    dst->refl = src->refl;
    dst->path = src->path;
    memcpy(dst->path_eq, src->path_eq, sizeof(dst->path_eq));
    dst->path_dir = src->path_dir;
    dst->path_spatial = src->path_spatial;
}

/*
=================
SA_GoesOn

Whether a channel's new sound goes on with what was found for its last: the
same entity's, from about where that one was a moment ago - a gun's next
shot. The way round comes from a pathing run, 30-100 ms after a sound is
new, and the infantry's shot lasts 92 ms: started afresh, every shot of a
burst was over before its way round was in.
=================
*/
static bool SA_GoesOn(const sa_chan_t *c, const channel_t *ch)
{
    vec3_t  origin;

    return ch->entnum > 0 && c->placed && c->entnum == ch->entnum && c->dist_mult == ch->dist_mult &&
           cls.realtime - c->placed_at < SA_GO_ON_MSEC && AL_ChannelOrigin(ch, origin) &&
           DistanceSquared(origin, c->origin) < SA_GO_ON_DIST * SA_GO_ON_DIST;
}

/*
=================
SA_PlayChannel

Takes the channel if it is one of ours: an effect sound with samples kept for
mixing. `sync` is a looping channel of the same sfx to start in phase with.
=================
*/
bool SA_PlayChannel(channel_t *ch, const channel_t *sync)
{
    sfxcache_t  *sc = ch->sfx->cache;
    sa_chan_t   *c;
    sa_parm_t   place, *p;
    int         i = ch - channels;

    if (!sa.active || !sc || !sc->pcm || AL_ChannelIsDry(ch))
        return false;

    c = &sa.chans[i];
    if (!SA_GoesOn(c, ch)) {
        memset(c, 0, sizeof(*c));
        c->fresh = true;
        c->started = cls.realtime;
        if (!++sa.path_keys)
            sa.path_keys++;
        c->path_key = sa.path_keys;
    }
    c->entnum = ch->entnum;
    c->dist_mult = ch->dist_mult;
    SA_Place(ch, &place);

    SDL_LockMutex(sa.param_lock);
    p = &sa.parms[i];
    p->gen++;
    p->playing = true;
    p->looping = ch->autosound;
    p->data = (const int16_t *)sc->data;
    p->numsamples = sc->samples;
    p->rate = sc->rate;
    p->start = 0;
    p->queued = Sys_Microseconds();
    if (sync && sync->steam) {
        const sa_parm_t *q = &sa.parms[sync - channels];
        if (q->pos_gen == q->gen && q->data == p->data)
            p->start = q->pos;
    }
    SA_CopyPlacement(p, &place);
    SDL_UnlockMutex(sa.param_lock);

    return true;
}

void SA_StopChannel(channel_t *ch)
{
    sa_parm_t *p = &sa.parms[ch - channels];

    SDL_LockMutex(sa.param_lock);
    p->playing = false;
    p->data = NULL;
    SDL_UnlockMutex(sa.param_lock);
}

void SA_Spatialize(channel_t *ch)
{
    sa_parm_t place;

    SA_Place(ch, &place);

    SDL_LockMutex(sa.param_lock);
    SA_CopyPlacement(&sa.parms[ch - channels], &place);
    SDL_UnlockMutex(sa.param_lock);
}

bool SA_ChannelDone(const channel_t *ch)
{
    const sa_parm_t *p = &sa.parms[ch - channels];
    bool done;

    SDL_LockMutex(sa.param_lock);
    done = p->done_gen == p->gen;
    SDL_UnlockMutex(sa.param_lock);

    return done;
}

void SA_SetPitch(float pitch)
{
    if (!sa.active)
        return;

    SDL_LockMutex(sa.param_lock);
    sa.pitch = pitch;
    SDL_UnlockMutex(sa.param_lock);
}

static void SA_ReadSettings(void)
{
    bool    use_hrtf, refl_hrtf;
    IPLHRTFInterpolation interp;
    float   volume, ceiling = 0, duck;

    use_hrtf = s_steamaudio_hrtf->integer;
    refl_hrtf = s_reflections_hrtf->integer;
    interp = s_steamaudio_interp->integer ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
    volume = Cvar_ClampValue(s_steamaudio_volume, 0, 4);
    if (s_steamaudio_limiter->integer)
        ceiling = powf(10, Cvar_ClampValue(s_steamaudio_ceiling, -24, 0) / 20);
    duck = powf(10, -Cvar_ClampValue(s_steamaudio_duck, 0, 24) / 20);
    sa.weapon_gain = powf(10, Cvar_ClampValue(s_steamaudio_weapon, -12, 12) / 20);

    SDL_LockMutex(sa.param_lock);
    sa.use_hrtf = use_hrtf;
    sa.refl_hrtf = refl_hrtf;
    sa.interp = interp;
    sa.volume = volume;
    sa.ceiling = ceiling;
    sa.duck = duck;
    SDL_UnlockMutex(sa.param_lock);
}

// once a frame, before the channels are placed
void SA_Update(void)
{
    uint64_t    start;
    int         contents;

    if (!sa.active)
        return;

    // a vid_restart gave or took the GPU's sound tracing: set up again for
    // what it can do now (SA_InitSimulation), as the menu now offers
    if (s_raytracing->integer && s_reflections_gpu->integer &&
        Cvar_VariableInteger("s_gpu_trace_available") != sa.gpu_avail) {
        sa.gpu_avail = !sa.gpu_avail;   // ask once
        Cbuf_AddText(&cmd_buffer, "snd_restart\n");
    }

    sa.listener_buried = sa.listener_wet = false;
    if (cls.state == ca_active && cl.bsp) {
        contents = CM_PointContents(listener_origin, cl.bsp->nodes);
        sa.listener_buried = contents & MASK_SOLID;     // noclip
        sa.listener_wet = contents & MASK_WATER;
    }

    if (cls.state == ca_active && cl.bsp && !sa.listener_buried && SA_OcclusionStrength() > 0) {
        start = Sys_Microseconds();
        SA_ListenerRelays();
        sa.trace_usec += (unsigned)(Sys_Microseconds() - start);
    } else {
        // nothing left from the last map
        sa.num_relays = 0;
        sa.next_relays = 0;
    }

    if (cls.realtime - sa.stat_time >= 1000) {
        sa.stat_traces = sa.traces;
        sa.stat_usec = sa.trace_usec;
        sa.traces = sa.trace_usec = 0;
        sa.stat_time = cls.realtime;
    }

    SA_UpdateSimulation();
    SA_ReadSettings();
}

bool SA_Active(void)
{
    return sa.active;
}

// Whether Steam Audio can run here, for the menu's "spatial simulation" and
// the room reverb that goes with it (reverb.c). See SA_SetAvailable.
bool SA_Available(void)
{
    return sa_available;
}

// keep a copy of the samples to mix from (16 bit, the sfx's own rate)
void SA_StoreSamples(sfxcache_t *sc, const wavinfo_t *info)
{
    int16_t *out = (int16_t *)sc->data;
    int     i;

    if (info->width == 1) {
        for (i = 0; i < info->samples; i++)
            out[i] = (int16_t)((info->data[i] - 128) << 8);
    } else {
        memcpy(out, info->data, info->samples * sizeof(*out));
    }

    sc->samples = info->samples;
    sc->rate = info->rate;
    sc->pcm = true;
}

// before an sfx's samples are freed: nothing may be mixing from them
void SA_DeleteSfx(sfxcache_t *sc)
{
    int i;

    if (!sa.active || !sc->pcm)
        return;

    SDL_LockMutex(sa.mix_lock);
    SDL_LockMutex(sa.param_lock);
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (sa.parms[i].data == (const int16_t *)sc->data) {
            sa.parms[i].playing = false;
            sa.parms[i].data = NULL;
        }
    }
    SDL_UnlockMutex(sa.param_lock);
    SDL_UnlockMutex(sa.mix_lock);
}

// How far behind the mix the output is, for every sound alike, in ms: what
// OpenAL knows of, which leaves out anything after Windows - a Bluetooth
// headset's own delay among it. -1 if OpenAL cannot say.
static float SA_DeviceLatency(void)
{
    ALCdevice   *device = alcGetContextsDevice(alcGetCurrentContext());
    LPALCGETINTEGER64VSOFT  qalcGetInteger64vSOFT;
    ALCint64SOFT            latency = 0;

    if (!device || !alcIsExtensionPresent(device, "ALC_SOFT_device_clock"))
        return -1;

    qalcGetInteger64vSOFT = (LPALCGETINTEGER64VSOFT)alcGetProcAddress(device, "alcGetInteger64vSOFT");
    if (!qalcGetInteger64vSOFT)
        return -1;

    qalcGetInteger64vSOFT(device, ALC_DEVICE_LATENCY_SOFT, 1, &latency);
    return latency / 1e6f;
}

static void SA_Voices_f(void)
{
    const sa_chan_t *c;
    const sa_slot_t *slot;
    sa_parm_t   p;
    channel_t   *ch;
    char        round[16], refl[16];
    uint64_t    wait_sum;
    unsigned    wait_count, wait_max, runs, path_map, meter_frames, meter_over;
    float       latency, msec, spacing, sec, meter_peak, meter_gain, meter_duck;
    const char  *error, *gpu_why;
    uint64_t    mix_usec_sum;
    unsigned    mix_usec_max, busy_sum, busy_max;
    int         i, s, n = 0, tris, probes, states[3] = { 0 }, edge_states[3] = { 0 }, run_sources, run_rays;
    int         edges;
    bool        refl_on, cached, gpu;

    if (!sa.active) {
        Com_Printf("Steam Audio is not active.\n");
        return;
    }

    // occl: how much of it the listener sees. trans: what reaches it of the
    // rest, through walls and round corners. round: what comes round a
    // corner (mid band) against the straight line, and over how many bends
    // ("s": over or past something small, louder than Steam Audio's way) -
    // or "p", by Steam Audio's paths, or "e", round the GPU's edges
    // (s_diffraction). room: what of it goes to the reverb.
    // refl: the reflection slot it is heard off the walls through, and what
    // it sends there.
    Com_Printf("ch dist gain  occl  trans (lo/mid/hi)  round  room  refl      sfx\n");
    for (i = 0, ch = channels; i < s_numchannels; i++, ch++) {
        if (!ch->sfx || !ch->steam)
            continue;
        SDL_LockMutex(sa.param_lock);
        p = sa.parms[i];
        SDL_UnlockMutex(sa.param_lock);
        c = &sa.chans[i];
        if (c->bends)
            Q_snprintf(round, sizeof(round), "%.2f/%d%s", c->path_mid, c->bends, c->small ? "s" : "");
        else if (c->path_state == SA_PATH_ROUND)
            Q_snprintf(round, sizeof(round), "%.2f %c", c->path * c->path_eq[1], c->path_edges ? 'e' : 'p');
        else
            Q_strlcpy(round, "  -   ", sizeof(round));
        states[c->path_state]++;
        if (c->path_state != SA_PATH_NONE && c->path_edges)
            edge_states[c->path_state]++;
        if (p.refl_slot < 0)
            Q_strlcpy(refl, "   -    ", sizeof(refl));
        else if (p.refl_slot == sa.num_slots)
            Q_snprintf(refl, sizeof(refl), "rm %5.2f", p.refl * p.gain);
        else
            Q_snprintf(refl, sizeof(refl), "%2d %5.2f", p.refl_slot, p.refl * p.gain);
        Com_Printf("%2d %4.0f %.2f  %.2f  %.2f %.2f %.2f   %s %.2f  %s  %s%s\n", i, c->dist, p.gain,
                   p.occlusion, p.trans[0], p.trans[1], p.trans[2], round,
                   p.room * p.gain, refl, ch->sfx->name, ch->autosound ? " (loop)" : "");
        n++;
    }
    Com_Printf("%d voices; occlusion took %u traces, %.1f ms in the last second; %d corner points\n",
               n, sa.stat_traces, sa.stat_usec / 1000.0f, sa.num_relays);
    if (!sa.room_source)
        Com_Printf("No room reverb feed.\n");

    if (!sa.refl) {
        Com_Printf("No reflections (s_raytracing or s_reflections 0, or they could not be set up).\n");
    } else {
        SDL_LockMutex(sa.param_lock);
        refl_on = sa.refl_on;
        msec = sa.refl_msec;
        runs = sa.refl_runs;
        tris = sa.refl_tris;
        error = sa.refl_error;
        gpu = sa.refl_gpu;
        gpu_why = sa.refl_gpu_why;
        run_sources = sa.refl_sources;
        run_rays = sa.refl_rays;
        SDL_UnlockMutex(sa.param_lock);

        if (refl_on) {
            Com_Printf("Reflections: %d triangles, %u runs, the last %.1f ms on the %s for %d sources at %d rays;"
                       " %d slots and the room\n", tris, runs, msec, gpu ? "GPU" : "CPU", run_sources, run_rays,
                       sa.num_slots);
            if (gpu_why)
                Com_Printf("  not on the GPU: %s\n", gpu_why);
        } else {
            Com_Printf("Reflections waiting (%s); the room reverb meanwhile\n",
                       error ? error : "for the first run");
        }
        for (s = 0, slot = sa.slots; s < sa.num_slots; s++, slot++) {
            if (!slot->key)
                continue;
            Com_Printf("  slot %2d: %s %4d, %4.0f away, %s, %s\n", s,
                       slot->key > 0 ? "entity " : "channel", slot->key > 0 ? slot->key : -1 - slot->key,
                       Distance(slot->origin, listener_origin),
                       slot->live ? "playing" : slot->simulate ? "following" : "idle",
                       sa.slot_ready[s] ? "ready" : "not ready");
        }
    }

    if (!sa.paths) {
        Com_Printf("No pathing (s_raytracing or s_pathing 0, or it could not be set up).\n");
    } else {
        SDL_LockMutex(sa.param_lock);
        path_map = sa.path_map;
        probes = sa.path_probes;
        spacing = sa.path_spacing;
        cached = sa.path_cached;
        sec = sa.path_sec;
        msec = sa.path_msec;
        runs = sa.path_runs;
        error = sa.path_error;
        SDL_UnlockMutex(sa.param_lock);

        if (!sa.map_bsp || path_map != sa.map)
            Com_Printf("Paths waiting for the map's bake; corner paths meanwhile\n");
        else if (error)
            Com_Printf("No paths on this map (%s); corner paths instead\n", error);
        else
            Com_Printf("Paths: %d probes %.1f m apart, %s in %.1f s; %u runs, the last %.2f ms\n",
                       probes, spacing / SA_UNITS_PER_METER, cached ? "loaded" : "baked", sec, runs, msec);
        Com_Printf("  voices: %d round Steam Audio's ways, %d in its sight, %d with no probe near or no run yet\n",
                   states[SA_PATH_ROUND] - edge_states[SA_PATH_ROUND],
                   states[SA_PATH_DIRECT] - edge_states[SA_PATH_DIRECT], states[SA_PATH_NONE]);
    }

    if (!sa.diffract) {
        Com_Printf("No ways round edges (s_raytracing, s_reflections_gpu or s_diffraction 0, or the simulation could not be set up).\n");
    } else {
        SDL_LockMutex(sa.param_lock);
        edges = sa.diff_edges;
        msec = sa.diff_msec;
        runs = sa.diff_runs;
        error = sa.diff_why;
        SDL_UnlockMutex(sa.param_lock);

        if (error)
            Com_Printf("No ways round edges: %s\n", error);
        else
            Com_Printf("Edges: %d corners on the GPU, %u runs, the last %.2f ms\n", edges, runs, msec);
        Com_Printf("  voices: %d round edges, %d in sight\n",
                   edge_states[SA_PATH_ROUND], edge_states[SA_PATH_DIRECT]);
    }

    // from the game starting a sound to the mixer starting it, since the last
    // sa_voices; the output latency comes on top, as it does without Steam Audio
    SDL_LockMutex(sa.mix_lock);
    wait_sum = sa.wait_sum;
    wait_count = sa.wait_count;
    wait_max = sa.wait_max;
    sa.wait_sum = sa.wait_count = sa.wait_max = 0;
    meter_peak = sa.meter_peak;
    meter_gain = sa.meter_gain;
    meter_duck = sa.meter_duck;
    meter_frames = sa.meter_frames;
    meter_over = sa.meter_over;
    sa.meter_peak = 0;
    sa.meter_gain = sa.meter_duck = 1;
    sa.meter_frames = sa.meter_over = 0;
    mix_usec_sum = sa.mix_usec_sum;
    mix_usec_max = sa.mix_usec_max;
    busy_sum = sa.busy_sum;
    busy_max = sa.busy_max;
    sa.mix_usec_sum = 0;
    sa.mix_usec_max = sa.busy_sum = sa.busy_max = 0;
    SDL_UnlockMutex(sa.mix_lock);

    if (wait_count)
        Com_Printf("%u sounds started: waited for the mixer %.1f ms on average, %.1f ms at worst\n",
                   wait_count, wait_sum / 1000.0f / wait_count, wait_max / 1000.0f);
    // what OpenAL's own limiter, on the mix with the music, would have had
    // to take off without SA_Limit
    if (meter_frames) {
        Com_Printf("Mix peak %+.1f dBFS, over full scale %.1f%% of the time; the limiter took off up to %.1f dB\n",
                   20 * log10f(max(meter_peak, 1e-6f)), meter_over * 100.0f / meter_frames,
                   -20 * log10f(max(meter_gain, 1e-6f)));
        Com_Printf("The player's weapon ducked the rest by up to %.1f dB\n",
                   -20 * log10f(max(meter_duck, 1e-6f)));
        // the mixer has a frame's time for one, or the sound breaks up
        Com_Printf("Mixing took %.2f ms a frame on average, %.2f at worst, of %.1f; %.1f reflection slots run on average, %u at most\n",
                   mix_usec_sum / 1000.0f / meter_frames, mix_usec_max / 1000.0f,
                   SA_FRAME * 1000.0f / sa.audio.samplingRate, (float)busy_sum / meter_frames, busy_max);
    }
    latency = SA_DeviceLatency();
    if (latency >= 0)
        Com_Printf("OpenAL output latency: %.1f ms\n", latency);
}

// one run on the GPU and the CPU from the same place, compared (SA_FinishCheck)
static void SA_ReflCheck_f(void)
{
    if (!sa.refl) {
        Com_Printf("No reflections (s_raytracing or s_reflections 0, or they could not be set up).\n");
        return;
    }
    if (!qiplq2SetReflectionTracer) {
        Com_Printf("This phonon.dll has no hook for the GPU: build it with setup/build_phonon.py.\n");
        return;
    }

    SDL_LockMutex(sa.sim_lock);
    sa.sim_in.check = true;
    sa.sim_kick = true;
    SDL_UnlockMutex(sa.sim_lock);
    SDL_CondSignal(sa.sim_wake);
    Com_Printf("Tracing the next run on the GPU and on the CPU...\n");
}

void SA_SoundInfo(void)
{
    float   latency = SA_DeviceLatency();

    if (latency >= 0)
        Com_Printf("OpenAL output latency: %.1f ms\n", latency);

    if (!sa.active) {
        Com_Printf("Steam Audio: off\n");
        return;
    }
    // on top of that: a frame mixed ahead at most, and the HRTF's own lead-in
    // of about 3 ms
    Com_Printf("Steam Audio: %d Hz, %d-sample frames (%.1f ms), %s\n", sa.audio.samplingRate,
               sa.audio.frameSize, sa.audio.frameSize * 1000.0f / sa.audio.samplingRate,
               sa.use_hrtf ? "HRTF" : "stereo panning");
    if (sa.refl)
        Com_Printf("Steam Audio reflections: %d sources and the room, %.1f s responses, traced on the %s\n",
                   sa.num_slots, sa.refl_duration, s_reflections_gpu->integer ? "GPU when it can" : "CPU");
    if (sa.paths)
        Com_Printf("Steam Audio pathing: probes %g m apart, %d runs a second, alternate ways %s\n",
                   Cvar_ClampValue(s_pathing_spacing, 1, 16), Cvar_ClampInteger(s_pathing_rate, 1, 100),
                   s_pathing_alternates->integer ? "on" : "off");
    if (sa.diffract)
        Com_Printf("Steam Audio: ways round corners on the GPU when it can, %d runs a second\n",
                   Cvar_ClampInteger(s_pathing_rate, 1, 100));
}

/*
==============================================================================

INIT / SHUTDOWN

==============================================================================
*/

// after the mixer is dead: it uses the effects
static void SA_FreeSimulation(void)
{
    int i, s;

    if (sa.sim_thread) {
        SDL_LockMutex(sa.sim_lock);
        sa.sim_quit = true;
        SDL_UnlockMutex(sa.sim_lock);
        SDL_CondSignal(sa.sim_wake);
        SDL_WaitThread(sa.sim_thread, NULL);
        sa.sim_thread = NULL;
    }
    if (qiplq2SetReflectionTracer)
        qiplq2SetReflectionTracer(NULL, NULL);
    SA_SimFreeGpu();
    free(sa.check_fields);
    free(sa.check_report);
    sa.check_fields = NULL;
    sa.check_report = NULL;

    SA_SimFreePaths();
    for (i = 0; i < MAX_CHANNELS; i++)
        if (sa.path_sources[i])
            qiplSourceRelease(&sa.path_sources[i]);
    SA_SimFreeScene(&sa.sim_scene);
    SA_FreeGeometry(sa.sim_geom);
    sa.sim_geom = NULL;

    for (s = 0; s <= SA_REFL_MAX_SLOTS; s++) {
        if (sa.refl_effects[s])
            qiplReflectionEffectRelease(&sa.refl_effects[s]);
        if (sa.refl_in[s].data)
            qiplAudioBufferFree(sa.context, &sa.refl_in[s]);
        if (sa.sources[s])
            qiplSourceRelease(&sa.sources[s]);
    }
    if (sa.refl_mixer)
        qiplReflectionMixerRelease(&sa.refl_mixer);
    if (sa.refl_decode)
        qiplAmbisonicsDecodeEffectRelease(&sa.refl_decode);
    if (sa.refl_amb.data)
        qiplAudioBufferFree(sa.context, &sa.refl_amb);
    if (sa.simulator)
        qiplSimulatorRelease(&sa.simulator);

    if (sa.sim_wake)
        SDL_DestroyCond(sa.sim_wake);
    if (sa.sim_lock)
        SDL_DestroyMutex(sa.sim_lock);
    sa.sim_wake = NULL;
    sa.sim_lock = NULL;
    sa.sim = sa.refl = sa.paths = sa.diffract = false;
}

// the reflections' effects, mixer and decoder: false and why if they could
// not be had
static bool SA_InitReflectionEffects(const char **error)
{
    IPLSimulationOutputs                outputs;
    IPLReflectionEffectSettings         effect_settings;
    IPLAmbisonicsDecodeEffectSettings   decode_settings;
    int         s;

    // A source's response lives in it from the start, empty, and the mixer
    // reads it in place: what it needs to know of it never changes.
    *error = "a source has no response";
    for (s = 0; s <= sa.num_slots; s++) {
        memset(&outputs, 0, sizeof(outputs));
        qiplSourceGetOutputs(sa.sources[s], IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
        if (!outputs.reflections.ir)
            return false;
        sa.refl_params[s] = outputs.reflections;
        sa.refl_params[s].type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        sa.refl_params[s].numChannels = SA_REFL_CHANNELS;
        sa.refl_params[s].irSize = (int)ceilf(sa.refl_duration * sa.audio.samplingRate);
    }

    effect_settings.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    effect_settings.irSize = (int)ceilf(sa.refl_duration * sa.audio.samplingRate);
    effect_settings.numChannels = SA_REFL_CHANNELS;
    sa.refl_ringout = (effect_settings.irSize + SA_FRAME - 1) / SA_FRAME + SA_REFL_IDLE_FRAMES;
    *error = "couldn't create the effects";
    for (s = 0; s <= sa.num_slots; s++) {
        if (qiplReflectionEffectCreate(sa.context, &sa.audio, &effect_settings, &sa.refl_effects[s]) != IPL_STATUS_SUCCESS ||
            qiplAudioBufferAllocate(sa.context, 1, SA_FRAME, &sa.refl_in[s]) != IPL_STATUS_SUCCESS)
            return false;
    }
    if (qiplReflectionMixerCreate(sa.context, &sa.audio, &effect_settings, &sa.refl_mixer) != IPL_STATUS_SUCCESS ||
        qiplAudioBufferAllocate(sa.context, SA_REFL_CHANNELS, SA_FRAME, &sa.refl_amb) != IPL_STATUS_SUCCESS)
        return false;

    memset(&decode_settings, 0, sizeof(decode_settings));
    decode_settings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    decode_settings.hrtf = sa.hrtf;
    decode_settings.maxOrder = SA_REFL_ORDER;
    if (qiplAmbisonicsDecodeEffectCreate(sa.context, &sa.audio, &decode_settings, &sa.refl_decode) != IPL_STATUS_SUCCESS)
        return false;

    return true;
}

/*
=================
SA_InitSimulation

The simulator, its sources and the thread, for reflections, pathing or both;
and for reflections an effect per slot. Failing leaves the mix as it is
without them.

s_raytracing 0 turns all of it off. On the CPU - s_reflections_gpu 0, or a
renderer that cannot trace sound (s_gpu_trace_available, published by
vkpt/sound_trace.c) - there is no diffraction, which only the GPU works out,
and s_reflections_all is ignored: each slot costs the CPU 2 to 3 ms a run, so
only the loudest s_reflections_sources get one. SA_Update sets it all up again
when a vid_restart changes what the GPU can do.
=================
*/
static void SA_InitSimulation(void)
{
    IPLSimulationSettings   sim_settings;
    IPLSourceSettings       source_settings;
    const char  *error;
    int         i, s, threads;
    bool        avail = Cvar_VariableInteger("s_gpu_trace_available");
    bool        rt = s_raytracing->integer, gpu = s_reflections_gpu->integer && avail;
    bool        refl = rt && s_reflections->integer, paths = rt && s_pathing->integer;
    bool        diffract = rt && gpu && s_diffraction->integer;

    sa.gpu_avail = avail;
    if (!refl && !paths && !diffract)
        return;

    if (!refl)
        sa.num_slots = 0;
    else if (s_reflections_all->integer && gpu)
        sa.num_slots = SA_REFL_MAX_SLOTS;
    else
        sa.num_slots = Cvar_ClampInteger(s_reflections_sources, 0, SA_REFL_MAX_SLOTS);
    sa.refl_duration = Cvar_ClampValue(s_reflections_duration, 0.25f, 3);
    threads = Cvar_ClampInteger(s_reflections_threads, 1, 16);
    sa.sim_threads = threads;
    SA_InitSceneMaterials();

    memset(&sim_settings, 0, sizeof(sim_settings));
    if (refl)
        sim_settings.flags |= IPL_SIMULATIONFLAGS_REFLECTIONS;
    if (paths)
        sim_settings.flags |= IPL_SIMULATIONFLAGS_PATHING;
    sim_settings.sceneType = IPL_SCENETYPE_DEFAULT;
    sim_settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    sim_settings.maxNumRays = SA_REFL_MAX_RAYS;
    sim_settings.numDiffuseSamples = SA_REFL_DIFFUSE;
    sim_settings.maxDuration = sa.refl_duration;
    sim_settings.maxOrder = SA_REFL_ORDER;     // pathing's too: at 0 it gives no direction
    sim_settings.maxNumSources = (refl ? sa.num_slots + 1 : 0) + (paths ? MAX_CHANNELS : 0);
    sim_settings.numThreads = threads;
    sim_settings.numVisSamples = SA_PATH_VIS_SAMPLES;
    sim_settings.samplingRate = sa.audio.samplingRate;
    sim_settings.frameSize = SA_FRAME;
    error = "couldn't create the simulator";
    if (qiplSimulatorCreate(sa.context, &sim_settings, &sa.simulator) != IPL_STATUS_SUCCESS)
        goto fail;

    error = "couldn't create a source";
    if (refl) {
        source_settings.flags = IPL_SIMULATIONFLAGS_REFLECTIONS;
        for (s = 0; s <= sa.num_slots; s++) {
            if (qiplSourceCreate(sa.simulator, &source_settings, &sa.sources[s]) != IPL_STATUS_SUCCESS)
                goto fail;
            qiplSourceAdd(sa.sources[s], sa.simulator);
        }
    }
    // one per channel, each asked about its sound's way round
    if (paths) {
        source_settings.flags = IPL_SIMULATIONFLAGS_PATHING;
        for (i = 0; i < MAX_CHANNELS; i++) {
            if (qiplSourceCreate(sa.simulator, &source_settings, &sa.path_sources[i]) != IPL_STATUS_SUCCESS)
                goto fail;
            qiplSourceAdd(sa.path_sources[i], sa.simulator);
        }
    }
    qiplSimulatorCommit(sa.simulator);

    if (refl && !SA_InitReflectionEffects(&error))
        goto fail;

    error = "out of memory";
    sa.sim_lock = SDL_CreateMutex();
    sa.sim_wake = SDL_CreateCond();
    if (!sa.sim_lock || !sa.sim_wake)
        goto fail;
    sa.sim_in.interval = 100;
    sa.sim_in.path_interval = 33;

    // before the thread, which goes by them
    sa.sim = true;
    sa.refl = refl;
    sa.paths = paths;
    sa.diffract = diffract;
    if (refl && qiplq2SetReflectionTracer)
        qiplq2SetReflectionTracer(SA_TraceReflections, NULL);

    error = "couldn't start the simulation thread";
    sa.sim_thread = SDL_CreateThread(SA_SimThread, "steam-audio-sim", NULL);
    if (!sa.sim_thread)
        goto fail;

    if (refl)
        Com_Printf("Steam Audio reflections: %d sources and the room, %d threads, %.1f s responses.\n",
                   sa.num_slots, threads, sa.refl_duration);
    if (paths)
        Com_Printf("Steam Audio pathing: on.\n");
    if (diffract)
        Com_Printf("Steam Audio: ways round corners on the GPU, when it can.\n");
    return;

fail:
    SA_FreeSimulation();
    Com_Printf("Steam Audio: no reflections or pathing, %s.\n", error);
}

static void SA_Free(void)
{
    sa_voice_t  *v;
    int         i;

    if (sa.source)
        qalSourceStop(sa.source);
    if (sa.room_source)
        qalSourceStop(sa.room_source);
    if (sa.mix_lock) {
        // the mixer may be inside a callback right now
        SDL_LockMutex(sa.mix_lock);
        sa.dead = true;
        SDL_UnlockMutex(sa.mix_lock);
    }
    if (sa.source) {
        qalSourcei(sa.source, AL_BUFFER, 0);
        qalDeleteSources(1, &sa.source);
    }
    if (sa.room_source) {
        qalSourcei(sa.room_source, AL_BUFFER, 0);
        qalDeleteSources(1, &sa.room_source);
    }
    if (sa.buffer)
        qalDeleteBuffers(1, &sa.buffer);
    if (sa.room_buffer)
        qalDeleteBuffers(1, &sa.room_buffer);

    if (sa.context) {
        SA_FreeSimulation();
        for (i = 0, v = sa.voices; i < MAX_CHANNELS; i++, v++) {
            if (v->binaural)
                qiplBinauralEffectRelease(&v->binaural);
            if (v->panning)
                qiplPanningEffectRelease(&v->panning);
            if (v->direct)
                qiplDirectEffectRelease(&v->direct);
            if (v->path_binaural)
                qiplBinauralEffectRelease(&v->path_binaural);
            if (v->path_panning)
                qiplPanningEffectRelease(&v->path_panning);
            if (v->path_direct)
                qiplDirectEffectRelease(&v->path_direct);
        }
        if (sa.in.data)
            qiplAudioBufferFree(sa.context, &sa.in);
        if (sa.mid.data)
            qiplAudioBufferFree(sa.context, &sa.mid);
        if (sa.out.data)
            qiplAudioBufferFree(sa.context, &sa.out);
        if (sa.hrtf)
            qiplHRTFRelease(&sa.hrtf);
        qiplContextRelease(&sa.context);
    }

    if (sa.param_lock)
        SDL_DestroyMutex(sa.param_lock);
    if (sa.mix_lock)
        SDL_DestroyMutex(sa.mix_lock);
    if (sa.lib)
        Sys_FreeLibrary(sa.lib);

    memset(&sa, 0, sizeof(sa));
}

static bool SA_LoadLibrary(void)
{
    char    path[MAX_OSPATH];

    Q_concat(path, sizeof(path), sys_libdir->string, PATH_SEP_STRING, SA_LIBNAME);
    Sys_LoadLibrary(path, NULL, &sa.lib);
    if (!sa.lib)
        Sys_LoadLibrary(SA_LIBNAME, NULL, &sa.lib);
    if (!sa.lib) {
        Com_Printf("Steam Audio: %s not found.\n", SA_LIBNAME);
        return false;
    }

#define SA_IMP(ret, name, args) \
    if (!(*(void **)&q##name = Sys_GetProcAddress(sa.lib, #name))) { \
        Com_Printf("Steam Audio: %s has no %s.\n", SA_LIBNAME, #name); \
        return false; \
    }
    SA_IMPORTS
#undef SA_IMP

    *(void **)&qiplq2SetReflectionTracer = Sys_GetProcAddress(sa.lib, "iplq2SetReflectionTracer");
    return true;
}

bool SA_Init(void)
{
    IPLContextSettings          context_settings;
    IPLHRTFSettings             hrtf_settings;
    IPLBinauralEffectSettings   binaural_settings;
    IPLPanningEffectSettings    panning_settings;
    IPLDirectEffectSettings     direct_settings;
    LPALBUFFERCALLBACKSOFT      qalBufferCallbackSOFT;
    ALCint      rate = 0;
    sa_voice_t  *v;
    int         i;
    char        path[MAX_OSPATH];

    // off unless the player turns it on - the menu's "spatial simulation";
    // must match reverb.c's registration
    s_steamaudio = Cvar_Get("s_steamaudio", "0", CVAR_ARCHIVE | CVAR_SOUND);
    s_steamaudio_hrtf = Cvar_Get("s_steamaudio_hrtf", "1", CVAR_ARCHIVE);
    s_steamaudio_interp = Cvar_Get("s_steamaudio_interp", "1", 0);
    s_steamaudio_volume = Cvar_Get("s_steamaudio_volume", "1", 0);
    s_steamaudio_limiter = Cvar_Get("s_steamaudio_limiter", "1", 0);
    s_steamaudio_ceiling = Cvar_Get("s_steamaudio_ceiling", "-1", 0);
    // dB: the player's own weapons over their other sounds, and the most
    // everything else is turned down while one sounds (SA_Duck)
    s_steamaudio_weapon = Cvar_Get("s_steamaudio_weapon", "2", 0);
    s_steamaudio_duck = Cvar_Get("s_steamaudio_duck", "6", 0);
    s_occlusion =Cvar_Get("s_occlusion", "1", CVAR_ARCHIVE);
    s_global_floor = Cvar_Get("s_global_floor", "0.25", 0);
    // the menu's "ray traced sound" and "hardware": over s_reflections,
    // s_pathing and s_diffraction, which stay for the console (SA_InitSimulation)
    s_raytracing = Cvar_Get("s_raytracing", "1", CVAR_ARCHIVE | CVAR_SOUND);
    s_reflections = Cvar_Get("s_reflections", "1", CVAR_ARCHIVE | CVAR_SOUND);
    s_reflections_sources = Cvar_Get("s_reflections_sources", "8", CVAR_SOUND);
    s_reflections_all = Cvar_Get("s_reflections_all", "1", CVAR_ARCHIVE | CVAR_SOUND);
    s_reflections_duration = Cvar_Get("s_reflections_duration", "1.5", CVAR_SOUND);
    s_reflections_threads = Cvar_Get("s_reflections_threads", "4", CVAR_SOUND);
    s_reflections_rays = Cvar_Get("s_reflections_rays", "1024", 0);
    s_reflections_bounces = Cvar_Get("s_reflections_bounces", "64", 0);
    s_reflections_rate = Cvar_Get("s_reflections_rate", "10", 0);
    s_reflections_gain = Cvar_Get("s_reflections_gain", "1", CVAR_ARCHIVE);
    // a far voice's echo against it: SA_ReflSend; and how it is decoded:
    // SA_MixReflections
    s_reflections_hold = Cvar_Get("s_reflections_hold", "3", 0);
    s_reflections_hrtf = Cvar_Get("s_reflections_hrtf", "0", 0);
    s_reflections_gpu = Cvar_Get("s_reflections_gpu", "1", CVAR_ARCHIVE | CVAR_SOUND);
    s_reflections_gpu_rays = Cvar_Get("s_reflections_gpu_rays", "8192", 0);
    s_pathing = Cvar_Get("s_pathing", "1", CVAR_ARCHIVE | CVAR_SOUND);
    s_pathing_spacing = Cvar_Get("s_pathing_spacing", "3", CVAR_SOUND);
    s_pathing_rate = Cvar_Get("s_pathing_rate", "30", 0);
    s_pathing_alternates = Cvar_Get("s_pathing_alternates", "0", 0);
    s_pathing_gain = Cvar_Get("s_pathing_gain", "1", CVAR_ARCHIVE);
    s_diffraction = Cvar_Get("s_diffraction", "1", CVAR_ARCHIVE | CVAR_SOUND);

    Cmd_AddCommand("sa_voices", SA_Voices_f);
    Cmd_AddCommand("sa_reflcheck", SA_ReflCheck_f);

    memset(&sa, 0, sizeof(sa));

    // until it is tried, whether there is a phonon.dll to try (SA_LoadLibrary
    // looks beside the exe first); from then on, whether it started
    Q_concat(path, sizeof(path), sys_libdir->string, PATH_SEP_STRING, SA_LIBNAME);
    SA_SetAvailable(!os_access(path, F_OK));

    if (!s_steamaudio->integer)
        return false;

    // what the bus needs from OpenAL - check before loading the library
    if (!qalIsExtensionPresent("AL_SOFT_callback_buffer") ||
        !qalIsExtensionPresent("AL_SOFT_direct_channels") ||
        !qalIsExtensionPresent("AL_EXT_FLOAT32")) {
        Com_Printf("Steam Audio: OpenAL lacks callback buffers, direct channels or float samples.\n");
        SA_SetAvailable(false);
        return false;
    }
    qalBufferCallbackSOFT = (LPALBUFFERCALLBACKSOFT)qalGetProcAddress("alBufferCallbackSOFT");
    alcGetIntegerv(alcGetContextsDevice(alcGetCurrentContext()), ALC_FREQUENCY, 1, &rate);
    if (!qalBufferCallbackSOFT || rate < 8000) {
        Com_Printf("Steam Audio: no usable OpenAL output (rate %d).\n", rate);
        SA_SetAvailable(false);
        return false;
    }

    if (!SA_LoadLibrary())
        goto fail;

    memset(&context_settings, 0, sizeof(context_settings));
    context_settings.version = STEAMAUDIO_VERSION;
    // zero would be SSE2 only. AVX512 is left out on purpose: Steam Audio's own
    // advice, as it can throttle the clock.
    context_settings.simdLevel = IPL_SIMDLEVEL_AVX2;
    if (qiplContextCreate(&context_settings, &sa.context) != IPL_STATUS_SUCCESS) {
        Com_Printf("Steam Audio: couldn't create a context (%s too old or too new?).\n", SA_LIBNAME);
        goto fail;
    }

    sa.audio.samplingRate = rate;
    sa.audio.frameSize = SA_FRAME;
    sa.limit_gain = sa.meter_gain = 1;
    sa.duck_gain = sa.meter_duck = 1;
    sa.duck_level = 0;

    memset(&hrtf_settings, 0, sizeof(hrtf_settings));
    hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
    hrtf_settings.volume = 1.0f;    // zero is silence, not "unchanged"
    hrtf_settings.normType = IPL_HRTFNORMTYPE_NONE;
    if (qiplHRTFCreate(sa.context, &sa.audio, &hrtf_settings, &sa.hrtf) != IPL_STATUS_SUCCESS) {
        Com_Printf("Steam Audio: couldn't load the HRTF.\n");
        goto fail;
    }

    binaural_settings.hrtf = sa.hrtf;
    memset(&panning_settings, 0, sizeof(panning_settings));
    panning_settings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    direct_settings.numChannels = 1;

    for (i = 0, v = sa.voices; i < MAX_CHANNELS; i++, v++) {
        if (qiplBinauralEffectCreate(sa.context, &sa.audio, &binaural_settings, &v->binaural) != IPL_STATUS_SUCCESS ||
            qiplPanningEffectCreate(sa.context, &sa.audio, &panning_settings, &v->panning) != IPL_STATUS_SUCCESS ||
            qiplDirectEffectCreate(sa.context, &sa.audio, &direct_settings, &v->direct) != IPL_STATUS_SUCCESS ||
            qiplBinauralEffectCreate(sa.context, &sa.audio, &binaural_settings, &v->path_binaural) != IPL_STATUS_SUCCESS ||
            qiplPanningEffectCreate(sa.context, &sa.audio, &panning_settings, &v->path_panning) != IPL_STATUS_SUCCESS ||
            qiplDirectEffectCreate(sa.context, &sa.audio, &direct_settings, &v->path_direct) != IPL_STATUS_SUCCESS) {
            Com_Printf("Steam Audio: couldn't create the voice effects.\n");
            goto fail;
        }
    }

    if (qiplAudioBufferAllocate(sa.context, 1, SA_FRAME, &sa.in) != IPL_STATUS_SUCCESS ||
        qiplAudioBufferAllocate(sa.context, 1, SA_FRAME, &sa.mid) != IPL_STATUS_SUCCESS ||
        qiplAudioBufferAllocate(sa.context, 2, SA_FRAME, &sa.out) != IPL_STATUS_SUCCESS) {
        Com_Printf("Steam Audio: out of memory.\n");
        goto fail;
    }

    sa.param_lock = SDL_CreateMutex();
    sa.mix_lock = SDL_CreateMutex();
    if (!sa.param_lock || !sa.mix_lock)
        goto fail;

    sa.pitch = 1;
    SA_ReadSettings();  // before the mixer can run
    SA_InitRelayDirs();
    SA_InitSimulation();

    // the bus: one stereo source that pulls frames from SA_Callback
    qalGetError();
    qalGenBuffers(1, &sa.buffer);
    qalBufferCallbackSOFT(sa.buffer, AL_FORMAT_STEREO_FLOAT32, rate, SA_Callback, (ALvoid *)(intptr_t)SA_OUT_MIX);
    qalGenSources(1, &sa.source);
    qalSourcei(sa.source, AL_SOURCE_RELATIVE, AL_TRUE);
    qalSource3f(sa.source, AL_POSITION, 0, 0, 0);
    qalSourcef(sa.source, AL_ROLLOFF_FACTOR, 0);
    qalSourcei(sa.source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
    qalSourcei(sa.source, AL_BUFFER, sa.buffer);
    if (qalGetError() != AL_NO_ERROR) {
        Com_Printf("Steam Audio: couldn't set up the OpenAL output.\n");
        goto fail;
    }

    // the room feed: mono, at the listener, heard only through the reverb.
    // Without one the mix is simply dry.
    qalGenBuffers(1, &sa.room_buffer);
    qalBufferCallbackSOFT(sa.room_buffer, AL_FORMAT_MONO_FLOAT32, rate, SA_Callback, (ALvoid *)(intptr_t)SA_OUT_ROOM);
    qalGenSources(1, &sa.room_source);
    qalSourcei(sa.room_source, AL_SOURCE_RELATIVE, AL_TRUE);
    qalSource3f(sa.room_source, AL_POSITION, 0, 0, 0);
    qalSourcef(sa.room_source, AL_ROLLOFF_FACTOR, 0);
    qalSourcei(sa.room_source, AL_BUFFER, sa.room_buffer);
    if (qalGetError() != AL_NO_ERROR || !AL_ReverbFeed(sa.room_source)) {
        qalGetError();
        if (sa.room_source) {
            qalSourcei(sa.room_source, AL_BUFFER, 0);
            qalDeleteSources(1, &sa.room_source);
        }
        if (sa.room_buffer)
            qalDeleteBuffers(1, &sa.room_buffer);
        sa.room_source = sa.room_buffer = 0;
        qalGetError();
    }

    sa.active = true;   // before Play: SA_Update and friends look at this

    // together, so the two callbacks run in step
    if (sa.room_source) {
        ALuint sources[2] = { sa.source, sa.room_source };
        qalSourcePlayv(2, sources);
    } else {
        qalSourcePlay(sa.source);
    }
    if (qalGetError() != AL_NO_ERROR) {
        Com_Printf("Steam Audio: couldn't start the OpenAL output.\n");
        goto fail;
    }

    Com_Printf("Steam Audio initialized: %d Hz, HRTF %s.\n", rate,
               sa.use_hrtf ? "on" : "off (stereo panning)");
    SA_SetAvailable(true);
    return true;

fail:
    SA_Free();
    SA_SetAvailable(false);
    Com_Printf("Steam Audio disabled, OpenAL places sounds.\n");
    return false;
}

void SA_Shutdown(void)
{
    Cmd_RemoveCommand("sa_voices");
    Cmd_RemoveCommand("sa_reflcheck");
    SA_Free();
}

#else // USE_STEAMAUDIO

bool SA_Init(void) { return false; }
void SA_Shutdown(void) { }
bool SA_Active(void) { return false; }
bool SA_Available(void) { return false; }
void SA_Update(void) { }
void SA_SoundInfo(void) { }
bool SA_PlayChannel(channel_t *ch, const channel_t *sync) { return false; }
void SA_StopChannel(channel_t *ch) { }
void SA_Spatialize(channel_t *ch) { }
bool SA_ChannelDone(const channel_t *ch) { return true; }
void SA_SetPitch(float pitch) { }
void SA_StoreSamples(sfxcache_t *sc, const wavinfo_t *info) { }
void SA_DeleteSfx(sfxcache_t *sc) { }

#endif // !USE_STEAMAUDIO
