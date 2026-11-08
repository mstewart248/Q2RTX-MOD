/*
THE PROFILER'S ROWS (profiler 1), shared by both path tracers.

vkpt.h includes this, and so does the D3D12 renderer (d3d12/passes.h), which
includes no Vulkan header: it times the same entries with its own timestamp
queries (d3d12/gpu_timers.c) and hands the ticks to profiler.c, which keeps the
averages, draws the table and answers profiler_dump for both.
*/

#ifndef REFRESH_VKPT_PROFILER_H
#define REFRESH_VKPT_PROFILER_H

#include <stdbool.h>
#include <stdint.h>

#define PROFILER_LIST \
	PROFILER_DO(PROFILER_FRAME_TIME,                 0) \
	PROFILER_DO(PROFILER_INSTANCE_GEOMETRY,          1) \
	PROFILER_DO(PROFILER_BVH_UPDATE,                 1) \
    PROFILER_DO(PROFILER_UPLOAD_LIGHTS,              1) \
	PROFILER_DO(PROFILER_PRIMARY_RAYS,               1) \
	PROFILER_DO(PROFILER_REFLECT_REFRACT_1,          1) \
	PROFILER_DO(PROFILER_REFLECT_REFRACT_2,          1) \
	PROFILER_DO(PROFILER_ASVGF_GRADIENT_REPROJECT,   1) \
	PROFILER_DO(PROFILER_SHARC,                      1) \
	PROFILER_DO(PROFILER_DIRECT_LIGHTING,            1) \
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING,          1) \
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING_0,        2) \
	PROFILER_DO(PROFILER_INDIRECT_LIGHTING_1,        2) \
	PROFILER_DO(PROFILER_RESTIR_GI,                  1) \
	PROFILER_DO(PROFILER_ASVGF_FULL,                 1) \
	PROFILER_DO(PROFILER_ASVGF_RECONSTRUCT_GRADIENT, 2) \
	PROFILER_DO(PROFILER_ASVGF_TEMPORAL,             2) \
	PROFILER_DO(PROFILER_ASVGF_ATROUS,               2) \
	PROFILER_DO(PROFILER_MGPU_TRANSFERS,             1) \
	PROFILER_DO(PROFILER_INTERLEAVE,                 1) \
	PROFILER_DO(PROFILER_ASVGF_TAA,                  2) \
	PROFILER_DO(PROFILER_BLOOM,                      1) \
	PROFILER_DO(PROFILER_MOTION_BLUR,                1) \
	PROFILER_DO(PROFILER_CHROMATIC_ABERRATION,       1) \
	PROFILER_DO(PROFILER_TONE_MAPPING,               1) \
	PROFILER_DO(PROFILER_DLSS,                       1) \
	PROFILER_DO(PROFILER_UPDATE_ENVIRONMENT,         1) \
	PROFILER_DO(PROFILER_GOD_RAYS,                   1) \
	PROFILER_DO(PROFILER_GOD_RAYS_REFLECT_REFRACT,   1) \
	PROFILER_DO(PROFILER_GOD_RAYS_FILTER,            1) \
	/* The map-fog volumetric (cl_fog 2). Without these the fog cost is \
	   INVISIBLE - it is buried inside PROFILER_GOD_RAYS above, which also \
	   carries the sun shafts, so there is no way to tell the two apart, or \
	   to show the froxel grid is cheaper than the march it replaced. \
	   FOG_FROXEL is the whole grid; the two sub-entries split it between \
	   the per-cell lighting and the prefix sum over the slices. \
	   NB every line here carries its own backslash - line splicing happens \
	   BEFORE comment removal, so an un-continued line ends the macro. */ \
	PROFILER_DO(PROFILER_FOG_FROXEL,                 1) \
	PROFILER_DO(PROFILER_FOG_FROXEL_RESERVOIR,       2) \
	PROFILER_DO(PROFILER_FOG_FROXEL_SPATIAL,         2) \
	PROFILER_DO(PROFILER_FOG_FROXEL_SCATTER,         2) \
	PROFILER_DO(PROFILER_FOG_FROXEL_INTEGRATE,       2) \
	/* NVIDIA Flow (vkpt/flow.c): its simulation, which the library submits \
	   itself, with the two small passes after it; and the world's light on \
	   its smoke (pt_flow_env). Its march is inside the ray passes, or under \
	   pt_flow_split_pass 1 the primary one is its own pass, FLOW_MARCH. */ \
	PROFILER_DO(PROFILER_FLOW_SIMULATION,            1) \
	PROFILER_DO(PROFILER_FLOW_SMOKE_LIGHTING,        1) \
	PROFILER_DO(PROFILER_FLOW_MARCH,                 1) \
	PROFILER_DO(PROFILER_SHADOW_MAP,                 1) \
	PROFILER_DO(PROFILER_COMPOSITING,                1) \
	/* The whole frame on the graphics queue: from when the frame before it is \
	   done (or this one starts, if the GPU sat idle) to the end of the last \
	   submit before present - Flow's simulation, the passes after DLSS, the \
	   blit and the HUD included, which PROFILER_FRAME_TIME leaves out. \
	   GPU-bound, it is 1000 / fps. */ \
	PROFILER_DO(PROFILER_GPU_FRAME,                  0) \

enum {
#define PROFILER_DO(a, ...) a,
	PROFILER_LIST
#undef PROFILER_DO
	NUM_PROFILER_ENTRIES,
};

#define NUM_PROFILER_QUERIES_PER_FRAME (NUM_PROFILER_ENTRIES * 2)

typedef enum {
	PROFILER_START,
	PROFILER_STOP,
} VKPTProfilerAction;

// profiler.c, for both renderers; title (or NULL) is a line drawn above the table
void draw_profiler(int enable_asvgf, const char *title);
void vkpt_profiler_dump(void);
double vkpt_get_profiler_result(int idx);

/* The D3D12 renderer's frame of results: ticks[NUM_PROFILER_QUERIES_PER_FRAME] at
   entry * 2 + action, used[] the queries it wrote, ns_per_tick its queue's
   timestamp period. Taken as vkpt_profiler_next_frame takes the Vulkan pool's. */
void vkpt_profiler_take_results(const uint64_t *ticks, const bool *used, double ns_per_tick);
// the samples freed and every result cleared (the D3D12 renderer's shutdown)
void vkpt_profiler_release(void);

#endif // REFRESH_VKPT_PROFILER_H
