/*
CPU FRAME PROFILER - wall-clock microseconds per section of the main loop.

Sections accumulate across Qcommon_Frame calls and are closed out once per
rendered frame by CL_Frame, so a loop iteration that skipped the refresh rolls
into the next frame. With cl_cpustats 1 the client prints a CPUSTATS line each
second (per-section averages plus the worst frame's breakdown) and a CPULONG
line for every frame longer than cl_cpustats_long milliseconds.

Added for the demo-playback frame drops, which present stats showed to be CPU
time (fence ~0, GPU well under the frame) with no existing way to say where.
*/

#pragma once

#include <stdint.h>

#define CPUPROF_LIST \
    CPUPROF_DO(NET_SLEEP,   "sleep")   \
    CPUPROF_DO(SERVER,      "server")  \
    CPUPROF_DO(REFLEX,      "reflex")  \
    CPUPROF_DO(CL_EVENTS,   "events")  \
    CPUPROF_DO(DEMO,        "demo")    \
    CPUPROF_DO(PREDICT,     "predict") \
    CPUPROF_DO(R_BEGIN,     "rbegin")  \
    CPUPROF_DO(VIEW_PREP,   "viewprep") \
    CPUPROF_DO(R_RENDER,    "rrender") \
    CPUPROF_DO(HUD,         "hud")     \
    CPUPROF_DO(R_END,       "rend")    \
    CPUPROF_DO(SOUND,       "sound")

typedef enum {
#define CPUPROF_DO(id, name) CPUPROF_##id,
    CPUPROF_LIST
#undef CPUPROF_DO
    CPUPROF_COUNT
} cpuprof_section_t;

extern uint64_t cpuprof_us[CPUPROF_COUNT];

uint64_t Sys_Microseconds(void);

/* FILES READ, for first-use hitches (the first monster shot after a map load, 11-06):
   such a hitch is nearly always something loaded from disk mid-game. FS_LoadFileEx and
   FS_OpenFile (reads) note every file here, and with cl_cpustats 1 a long frame's
   CPULONG line is followed by a CPULONG files line naming what that frame read. */
void cpuprof_note_file(const char *path, int64_t bytes, uint64_t us);
// this frame's notes as text into `buf` ("" when none), then cleared; returns the length
size_t cpuprof_take_files(char *buf, size_t size);

#define CPUPROF_BEGIN(id) const uint64_t cpuprof_t0_##id = Sys_Microseconds()
#define CPUPROF_END(id)   (cpuprof_us[CPUPROF_##id] += Sys_Microseconds() - cpuprof_t0_##id)

/* THE MAIN THREAD BLOCKED (11-14, for the GPU profiler's CPU frame time - Matt: the
   frame to frame time "mostly just matches the gpu number", since GPU-bound the CPU
   spends the difference waiting). A running total, never reset, of the time the main
   thread sat waiting rather than working: the frame limiter's sleep, Reflex's, and the
   renderers' waits on the GPU and the swapchain (the frame's fence, the swapchain's
   waitable or acquire, the present). The profiler takes the difference over a frame
   from the frame's time: what is left is the CPU's own work. Main thread only. */
extern uint64_t cpuprof_wait_us;
#define CPUPROF_WAITED(us) (cpuprof_wait_us += (uint64_t)(us))
// CPUPROF_END for a section that is waiting, not working
#define CPUPROF_END_WAIT(id) do { \
        const uint64_t cpuprof_d_##id = Sys_Microseconds() - cpuprof_t0_##id; \
        cpuprof_us[CPUPROF_##id] += cpuprof_d_##id; \
        CPUPROF_WAITED(cpuprof_d_##id); \
    } while (0)

/* WHERE INSIDE THE SECTIONS THE CPU FRAME GOES (11-14, for the GPU profiler's dip
   capture - Matt: "CPU frame time is causing the problem ... can we thread this or
   optimize it"). Finer pieces INSIDE the sections above (each comment names its
   section), so they are not added into CL_CpuStatsFrame's "other"; closed out with
   them. WAIT_BEGIN and WAIT_PRESENT are the renderers' waits inside rbegin and rend
   (also in cpuprof_wait_us), so those two sections can be shown as work alone. */
#define CPUPROF_DETAIL_LIST \
    CPUPROF_DD(ENTITIES,     "entities")      /* viewprep: CL_AddPacketEntities */ \
    CPUPROF_DD(PARTICLES,    "particles")     /* viewprep: CL_AddParticles, blood included */ \
    CPUPROF_DD(BLOOD_SIM,    "blood sim")     /* particles: the droplets stepped and traced */ \
    CPUPROF_DD(BLOOD_BODIES, "blood bodies")  /* blood sim: droplets against monster meshes */ \
    CPUPROF_DD(BLOOD_FILM,   "blood film")    /* particles: CL_BloodFilmFrame */ \
    CPUPROF_DD(SCENE,        "scene")         /* rrender: vkpt_prepare_frame */ \
    CPUPROF_DD(LIGHTS,       "lights")        /* scene: light lists + lights, Quake III glows (issue #31) */ \
    CPUPROF_DD(LIGHT_LISTS,  "light lists")   /* lights: the cluster lists copied, model lights put in */ \
    CPUPROF_DD(GLOW_SETUP,   "glow set-up")   /* lights: which glows to work out, programs set up (main thread) */ \
    CPUPROF_DD(GLOW_EVAL,    "glow eval")     /* lights: the glows worked out (workers, and the wait for them) */ \
    CPUPROF_DD(LIGHT_COPY,   "light copy")    /* lights: every light into the upload, inline glows included */ \
    CPUPROF_DD(FLOW,         "flow")          /* rrender: vkpt_flow_update */ \
    CPUPROF_DD(TRANSPARENT,  "transparent")   /* rrender: particles, beams, sprites to geometry */ \
    CPUPROF_DD(BLOOD_MESH,   "blood mesh")    /* rrender: vkpt_blood_prepare */ \
    CPUPROF_DD(FILM_MESH,    "film mesh")     /* blood mesh: write_film_geometry */ \
    CPUPROF_DD(BLAS,         "blas")          /* rrender: dynamic BLAS plan, instances */ \
    CPUPROF_DD(RECORD,       "record")        /* rrender: the frame's command list */ \
    CPUPROF_DD(BLOOD_COPY,   "blood copy")    /* record: blood regions into staging */ \
    CPUPROF_DD(WAIT_BEGIN,   "wait begin")    /* rbegin: fence, swapchain, acquire */ \
    CPUPROF_DD(WAIT_FENCE,   "wait fence")    /* wait begin: the frame slot's fence (the rest: the swapchain) */ \
    CPUPROF_DD(WAIT_PRESENT, "wait present")  /* rend: the present */

typedef enum {
#define CPUPROF_DD(id, name) CPUPROF_D_##id,
    CPUPROF_DETAIL_LIST
#undef CPUPROF_DD
    CPUPROF_DETAIL_COUNT
} cpuprof_detail_t;

// what the frame's work was made of, for reading the times: set, not added
#define CPUPROF_COUNTER_LIST \
    CPUPROF_DC(BLOOD_AIR)           /* droplets in flight */ \
    CPUPROF_DC(BLOOD_LANDED)        /* droplets stuck to something */ \
    CPUPROF_DC(BLOOD_TRACES)        /* world traces by the blood sim */ \
    CPUPROF_DC(BLOOD_BODY_TRACES)   /* monster mesh traces */ \
    CPUPROF_DC(BLOOD_MOVED)         /* meshes moved as they were */ \
    CPUPROF_DC(BLOOD_REBUILT)       /* meshes made again */ \
    CPUPROF_DC(BLOOD_STILL)         /* meshes untouched */ \
    CPUPROF_DC(BLOOD_COPIES)        /* upload regions */ \
    CPUPROF_DC(BLOOD_UP_KB)         /* bytes those regions carry, KB */ \
    CPUPROF_DC(BLOOD_GPU_MOVES)     /* moved meshes the GPU moved (pt_blood_gpu_move) */ \
    CPUPROF_DC(LIGHT_POLYS)         /* lights copied to the upload */ \
    CPUPROF_DC(LIGHT_MODEL)         /* of those, model lights */ \
    CPUPROF_DC(LIGHT_NODES)         /* light list entries */ \
    CPUPROF_DC(LIST_BUILDS)         /* frames of the last 64 that built the light lists again */ \
    CPUPROF_DC(LIST_STAGED)         /* frames of the last 64 that wrote them to the staging */ \
    CPUPROF_DC(GLOW_EVALS)          /* Quake III glows worked out on the workers */ \
    CPUPROF_DC(GLOW_SAMPLES)        /* their samples (n x n each) */ \
    CPUPROF_DC(GLOW_INLINE)         /* glows the copy had to work out itself */ \
    CPUPROF_DC(GLOW_RUNS)           /* material runs the set-up looked at */ \
    CPUPROF_DC(GLOW_IMAGES)         /* light images made (each one a box filter of a texture) */

typedef enum {
#define CPUPROF_DC(id) CPUPROF_C_##id,
    CPUPROF_COUNTER_LIST
#undef CPUPROF_DC
    CPUPROF_COUNTER_COUNT
} cpuprof_counter_t;

extern uint64_t cpuprof_detail_us[CPUPROF_DETAIL_COUNT];
extern uint32_t cpuprof_count[CPUPROF_COUNTER_COUNT];

// the last frame CL_CpuStatsFrame closed: its sections (+ "other" last), details and counters
typedef struct {
    uint64_t us[CPUPROF_COUNT + 1];
    uint64_t detail_us[CPUPROF_DETAIL_COUNT];
    uint32_t count[CPUPROF_COUNTER_COUNT];
    uint64_t wall_us;   // the frame, close to close: its sections and waits add up to it
    uint32_t serial;    // bumped each frame closed
} cpuprof_frame_t;
extern cpuprof_frame_t cpuprof_last;

/* THE LAST FRAMES CLOSED, at serial % CPUPROF_RING (11-14). The GPU's results come
   back frames late; the profiler pairs them with THEIR frame's CPU side from here
   (the renderer notes cpuprof_last.serial + 1 as a frame begins - the serial that
   frame closes as), not with whichever frame closed last - Matt's dip capture had
   the GPU's numbers from one frame and the CPU's from two others. */
#define CPUPROF_RING 16
extern cpuprof_frame_t cpuprof_ring[CPUPROF_RING];

#define CPUPROF_DETAIL_BEGIN(id) const uint64_t cpuprof_dt0_##id = Sys_Microseconds()
#define CPUPROF_DETAIL_END(id)   (cpuprof_detail_us[CPUPROF_D_##id] += Sys_Microseconds() - cpuprof_dt0_##id)
#define CPUPROF_DETAIL_ADD(id, us) (cpuprof_detail_us[CPUPROF_D_##id] += (uint64_t)(us))
#define CPUPROF_COUNT_SET(id, n) (cpuprof_count[CPUPROF_C_##id] = (uint32_t)(n))
