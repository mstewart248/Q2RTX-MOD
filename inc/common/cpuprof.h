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

#define CPUPROF_BEGIN(id) const uint64_t cpuprof_t0_##id = Sys_Microseconds()
#define CPUPROF_END(id)   (cpuprof_us[CPUPROF_##id] += Sys_Microseconds() - cpuprof_t0_##id)
